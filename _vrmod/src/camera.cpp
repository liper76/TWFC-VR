// VR camera: applies the headset eye pose to the engine's final point of view (game thread).
// Stereo is alternate-eye (AFR): every game frame renders one eye; vr.cpp pairs the frames up.
//
// Eye-pair sync: the body position and yaw are latched on the left-eye frame and reused for the right-eye
// frame, so both eyes of a stereo pair see the world from the same body pose (otherwise moving or turning
// between the two frames makes the eyes disagree, which is nauseating).
#include "common.h"
#include "vrmath.h"

static DWORD g_lastCameraTick;
static float g_bodyYaw;          // own body yaw in UE rotator units (main mode): right stick turns it
static bool g_bodyInit;
static bool g_snapLatched;
static float g_latchYaw;         // body yaw latched for the current eye pair
static Vec3f g_latchBase;        // body/eye base position latched for the current eye pair
static Vec3f g_latchPawn;        // pawn position when the eye pair was latched (AFR chase delta fallback)
static bool g_latchValid;
static volatile float g_turnInput;   // right stick X (-1..1) seen by the XInput proxy
float g_debugViewOffset[4];          // debug.cpp "viewoffset"
static Vec3f g_latchHead;            // head position in the room (body frame; z: height vs the start) latched for the eye pair
static float g_latchHeadYaw;         // head yaw relative to the body (radians) latched for the eye pair
static float g_moveYawDelta;         // head yaw - aim yaw (radians), for the left stick
static float g_vehicleSteer;         // vehicle form: right stick that turns the vehicle towards the aim (-1..1)
static volatile DWORD g_vehicleSteerTick;
static bool CameraVehicleAimFresh() { return GetTickCount() - g_vehicleSteerTick < 250; }
static DWORD g_moveTick;
static bool g_chaseValid;            // the latched base is a chase camera position (smoothed)
static float g_baseFwd, g_baseUp;    // eye base relative to the pawn (forward, up), gliding when transforming
static bool g_baseInit;

void CameraBaseOffset(float& forward, float& up)
{
    forward = g_baseInit ? g_baseFwd : g_cfg.eyeForward;
    up = g_baseInit ? g_baseUp : g_cfg.eyeHeight;
}
static void UpdateAimHit(void* pc, float ox, float oy, float oz);
static volatile DWORD g_vehicleAimTick;   // last camera update in vehicle form with gaze aiming

bool CameraInGameplay()
{
    return GetTickCount() - g_lastCameraTick < 250 && !GameMoviePlaying();
}

static volatile DWORD g_playerViewTick;
bool CameraPlayerView() { return GetTickCount() - g_playerViewTick < 250; }

static float g_gameFov, g_normalFov;
static volatile bool g_zoomed;

bool CameraGameZoom(float& fov)
{
    fov = g_gameFov;
    return g_zoomed && CameraPlayerView();
}

// Main mode owns turning (the game's own turning would also follow aim assist and the hand-aim rotation).
bool CameraOwnsTurning()
{
    return g_cfg.ownTurning && g_cfg.aimMode == 1 && CameraInGameplay() && VRIsRunningOrFake();
}

void CameraSetTurnInput(float x) { g_turnInput = x; }

void CameraSnapTurn(int units)
{
    g_bodyYaw = fmodf(g_bodyYaw + units + 65536.0f, 65536.0f);
}

// Pawn.Controller == Camera.PCOwner: the camera looks at the pawn the local player controls.
static bool IsPlayerPawn(void* viewTarget, void* pc)
{
    if (!viewTarget || !pc) return false;
    __try {
        return *(void**)((uint8_t*)viewTarget + 0x1D4) == pc;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The controller has a pawn (PlayerController.Pawn): in a level, not in the main menu.
static bool ControllerHasPawn(void* pc)
{
    if (!pc) return false;
    __try {
        return *(void**)((uint8_t*)pc + 0x1C8) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static unsigned VTableOf(void* obj)
{
    if (!obj) return 0;
    __try {
        return *(unsigned*)obj;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void LogStats(TPOV* pov, void* viewTarget, float dt)
{
    if (g_cfg.logCameraSeq > 0) {
        g_cfg.logCameraSeq--;
        Vec3f t = {};
        ReadActorLocation(viewTarget, t);
        Log("seq: --- UpdateViewTarget done (pov %.1f %.1f %.1f, target %p at %.1f %.1f %.1f) ---", pov->x, pov->y, pov->z,
            viewTarget, t.x, t.y, t.z);
    }
    static void* lastTarget;
    if (viewTarget != lastTarget) {
        Log("camera: view target %p vt %08X", viewTarget, VTableOf(viewTarget));
        lastTarget = viewTarget;
    }
    if (!g_cfg.logFps) return;
    static DWORD t0 = GetTickCount();
    static long calls;
    calls++;
    DWORD now = GetTickCount();
    if (now - t0 >= 5000) {
        static long f0;
        long f = GameFrameCount();
        Log("camera: %.1f view updates/s, %.1f frame waits/s (dt %.4f) target %p vt %08X", calls * 1000.0f / (now - t0),
            (f - f0) * 1000.0f / (now - t0), dt, viewTarget, VTableOf(viewTarget));
        f0 = f;
        t0 = now;
        calls = 0;
    }
}

// Orientation of a hand's aim in OpenXR space. Held with both hands, the weapon is turned about the right hand so
// that its front grip lies in the left hand.
static Mat3 HandAimXR(const VREyeView& eye, int h)
{
    const auto& a = eye.aim[h];
    Mat3 m = QuatToMat(a.qx, a.qy, a.qz, a.qw);
    float q[4];
    if (h != VRGunHand() || !VRTwoHandTurn(q)) return m;   // (the hand holding the gun: right or left)
    return Mul(QuatToMat(q[0], q[1], q[2], q[3]), m);
}

// Cutscenes and other non-player views: like the classic mode (game camera yaw + head), no aim.
static void ApplyHeadOnly(TPOV* pov, const VREyeView& eye)
{
    Mat3 body = RotatorToMat(g_cfg.keepStickPitch ? pov->pitch : 0, pov->yaw, 0);
    Mat3 view = Mul(body, XrRotToUE(QuatToMat(eye.qx, eye.qy, eye.qz, eye.qw)));
    MatToRotator(view, pov->pitch, pov->yaw, pov->roll);
    Vec3 off = XrToUE({ eye.px, eye.py, eye.pz });
    float s = g_cfg.worldScale;
    Vec3 w = Mul(body, Vec3{ off.x * s, off.y * s, off.z * s });
    pov->x += w.x; pov->y += w.y; pov->z += w.z;
    pov->fov = eye.renderFovDeg;
}

void CameraOnUpdate(TPOV* pov, void* viewTarget, void* pc, float dt)
{
    bool player = IsPlayerPawn(viewTarget, pc);
    // the main menu's backdrop camera stays on the virtual screen; cutscenes use the same camera class, but there
    // the controller has a pawn: they are shown in 3D (head tracked, like the classic mode)
    // (cutscenes on the screen too unless Cutscenes3D: their staging depends on the game's camera)
    bool menu = !player && (!g_cfg.cutscenes3D || (VTableOf(viewTarget) == g_cfg.menuCameraVTable && !ControllerHasPawn(pc)));
    LogStats(pov, viewTarget, dt);
    WeaponsUpdate(player && g_cfg.cameraMode == 1 ? viewTarget : nullptr);
    PullUpdate(player && g_cfg.cameraMode == 1 ? viewTarget : nullptr);
    GrenadesUpdate(player && g_cfg.cameraMode == 1 ? viewTarget : nullptr);
    if (menu) return;                      // main menu backdrop: stays on the virtual screen
    g_lastCameraTick = GetTickCount();

    // both eyes per game frame (the frame is drawn twice: game.cpp) or one eye per frame, alternating
    VREyeView eye, eye2;
    bool pair = g_cfg.stereoPerFrame != 0;
    if (pair ? !VRBeginStereoPair(eye, eye2) : !VRBeginGameFrame(eye)) return;
    if (!player) {
        IKSetPawn(nullptr, nullptr);
        g_latchValid = false;
        TPOV right = *pov;
        ApplyHeadOnly(pov, eye);
        if (pair) { ApplyHeadOnly(&right, eye2); GameSetRightEyeView(right); }
        return;
    }

    g_playerViewTick = GetTickCount();
    // the game zooms (fine aim with a scoped weapon) by narrowing its FOV - VR keeps the headset's, so the zoom is
    // noticed here and shown in the scope lens instead
    g_gameFov = pov->fov;
    g_normalFov = fmaxf(g_gameFov, g_normalFov - dt * 2.0f);
    g_zoomed = g_gameFov < g_normalFov * 0.85f;

    // ---- body pose for this eye pair (latched on the left eye) ----
    bool ownTurn = g_cfg.ownTurning && g_cfg.aimMode == 1;
    if (eye.eye == 0 || !g_latchValid) {
        if (ownTurn) {
            if (!g_bodyInit) { g_bodyYaw = (float)pov->yaw; g_bodyInit = true; }
            float x = g_turnInput;
            if (g_cfg.snapTurnDeg > 0) {
                if (fabsf(x) > 0.7f && !g_snapLatched) {
                    CameraSnapTurn((int)(g_cfg.snapTurnDeg * 65536.0f / 360.0f) * (x > 0 ? 1 : -1));
                    g_snapLatched = true;
                } else if (fabsf(x) < 0.3f) {
                    g_snapLatched = false;
                }
            } else {
                // smooth turn; dt covers one eye frame, the pair spans two (unless the left-eye frame already
                // carries the whole pair's time, see GameTimeFrozenForRightEye)
                float pairDt = pair || GameTimeFrozenForRightEye() ? dt : dt * 2.0f;
                g_bodyYaw = fmodf(g_bodyYaw + x * g_cfg.smoothTurnDegPerSec * 65536.0f / 360.0f * pairDt + 65536.0f, 65536.0f);
            }
            g_latchYaw = g_bodyYaw;
        } else {
            g_bodyInit = false;
            g_latchYaw = (float)pov->yaw;
        }
        Vec3f pawn;
        Mat3 yawOnly = RotatorToMat(0, (int)g_latchYaw & 0xFFFF, 0);
        if (g_cfg.cameraMode == 1 && ReadActorLocation(viewTarget, pawn)) {
            bool vehicle = IKPawnIsVehicle();
            bool chase = vehicle && g_cfg.vehicleCamera == 1;
            float height = chase ? g_cfg.chaseHeight : vehicle ? g_cfg.vehicleEyeHeight : g_cfg.eyeHeight;
            float forward = chase ? -g_cfg.chaseDistance : vehicle ? g_cfg.vehicleEyeForward : g_cfg.eyeForward;
            float pdt = pair || GameTimeFrozenForRightEye() ? dt : dt * 2.0f;
            // transforming moves the eyes (robot head <-> vehicle seat / chase camera): glide there instead of
            // jumping (about half a second)
            if (!g_baseInit) { g_baseFwd = forward; g_baseUp = height; g_baseInit = true; }
            bool adjusting = VRCameraAdjusting();   // (placed by hand in the menu: follows the hand, no glide)
            float kb = adjusting ? 1.0f : 1.0f - expf(-pdt / 0.12f);
            g_baseFwd += (forward - g_baseFwd) * kb;
            g_baseUp += (height - g_baseUp) * kb;
            if (fabsf(forward - g_baseFwd) < 0.5f) g_baseFwd = forward;
            if (fabsf(height - g_baseUp) < 0.5f) g_baseUp = height;
            Vec3 fwd = Mul(yawOnly, Vec3{ g_baseFwd, 0.0f, 0.0f });
            Vec3f want = { pawn.x + fwd.x, pawn.y + fwd.y, pawn.z + g_baseUp };
            if (chase && g_chaseValid) {
                // chase camera (vehicle form): behind and above, catching up smoothly; the horizon stays level and
                // it never turns with the vehicle (the vehicle steers towards the aim)
                float k = adjusting ? 1.0f : 1.0f - expf(-pdt / 0.15f);
                g_latchBase = { g_latchBase.x + (want.x - g_latchBase.x) * k, g_latchBase.y + (want.y - g_latchBase.y) * k,
                                g_latchBase.z + (want.z - g_latchBase.z) * k };
            } else {
                g_latchBase = want;
            }
            g_latchPawn = pawn;
            g_chaseValid = chase;
        } else {
            g_latchBase = { pov->x, pov->y, pov->z };
        }
        // where the head is in the room (horizontal, body frame) and which way it looks: the drawn body is kept
        // under the head and turned with it
        Vec3 head = XrToUE({ eye.px, eye.py, eye.pz });
        g_latchHead = { head.x * g_cfg.worldScale, head.y * g_cfg.worldScale, head.z * g_cfg.worldScale };
        Vec3 look = XrRotToUE(QuatToMat(eye.qx, eye.qy, eye.qz, eye.qw)).col(0);
        g_latchHeadYaw = atan2f(look.y, look.x);
        g_latchValid = true;
    } else if (g_cfg.cameraMode == 1) {
        // right eye (AFR fallback): yaw and head stay those of the pair, but the base follows any pawn motion
        // (lifts, platforms, or a PhysX step in vehicle chase view) so the vehicle/body never jumps between eyes
        Vec3f pawn;
        if (ReadActorLocation(viewTarget, pawn)) {
            if (g_chaseValid) {
                g_latchBase.x += pawn.x - g_latchPawn.x;
                g_latchBase.y += pawn.y - g_latchPawn.y;
                g_latchBase.z += pawn.z - g_latchPawn.z;
            } else {
                Vec3 fwd = Mul(RotatorToMat(0, (int)g_latchYaw & 0xFFFF, 0), Vec3{ g_baseFwd, 0.0f, 0.0f });
                g_latchBase = { pawn.x + fwd.x, pawn.y + fwd.y, pawn.z + g_baseUp };
            }
            g_latchPawn = pawn;
        }
    }
    float headOffset[3] = { g_latchHead.x, g_latchHead.y, g_latchHead.z };   // (z: crouching bends the legs)
    // the skeleton is posed before the next view: with both eyes per frame that is always a new pair (eye 1 ->
    // the next pose is a left-eye pose)
    IKSetFrame(pair ? 1 : eye.eye, headOffset, g_latchYaw * kUURotToRad, g_latchHeadYaw);

    int basePitch = (!ownTurn && g_cfg.keepStickPitch) ? pov->pitch : 0;
    Mat3 body = RotatorToMat(basePitch, (int)g_latchYaw & 0xFFFF, 0);
    Mat3 view = Mul(body, XrRotToUE(QuatToMat(eye.qx, eye.qy, eye.qz, eye.qw)));
    MatToRotator(view, pov->pitch, pov->yaw, pov->roll);

    float s = g_cfg.worldScale;
    Vec3 off = XrToUE({ eye.px, eye.py, eye.pz });
    Vec3 w = Mul(body, Vec3{ off.x * s, off.y * s, off.z * s });
    pov->x = g_latchBase.x + w.x;
    pov->y = g_latchBase.y + w.y;
    pov->z = g_latchBase.z + w.z;
    pov->fov = eye.renderFovDeg;
    TPOV rightPov = *pov;
    if (pair) {
        // the right eye: same body, its own eye pose; drawn by the second drawing of this frame
        MatToRotator(Mul(body, XrRotToUE(QuatToMat(eye2.qx, eye2.qy, eye2.qz, eye2.qw))), rightPov.pitch, rightPov.yaw, rightPov.roll);
        Vec3 o2 = XrToUE({ eye2.px, eye2.py, eye2.pz });
        Vec3 w2 = Mul(body, Vec3{ o2.x * s, o2.y * s, o2.z * s });
        rightPov.x = g_latchBase.x + w2.x;
        rightPov.y = g_latchBase.y + w2.y;
        rightPov.z = g_latchBase.z + w2.z;
        rightPov.fov = eye2.renderFovDeg;
    }

    // ---- hands for the arm IK (ik.cpp, applied when the engine next poses the skeleton) ----
    float viewPos[3] = { pair ? (pov->x + rightPov.x) * 0.5f : pov->x,
                         pair ? (pov->y + rightPov.y) * 0.5f : pov->y,
                         pair ? (pov->z + rightPov.z) * 0.5f : pov->z };
    IKSetPawn(g_cfg.cameraMode == 1 ? viewTarget : nullptr, viewPos);
    for (int h = 0; h < 2; h++) {
        // the robot's hand sits where the real hand holds the controller; the weapon points along the aim ray
        const auto& a = eye.aim[h];
        const auto& g = eye.grip[h].valid ? eye.grip[h] : a;
        // body frame, relative to the eye base: ik.cpp places it at the pawn's position of the frame it poses,
        // so the hands do not lag behind the body (and shake) while walking
        float pos[3] = {}, axes[3][3] = {};
        if (a.valid) {
            Mat3 r = Mul(RotatorToMat(basePitch, 0, 0), XrRotToUE(HandAimXR(eye, h)));
            Vec3 hp = XrToUE({ g.px, g.py, g.pz });
            // HandOffset (cm, body frame) lets the player line the robot's hands up with the real ones
            pos[0] = hp.x * s + g_cfg.handOffset[0] * 0.01f * s;
            pos[1] = hp.y * s + g_cfg.handOffset[1] * 0.01f * s;
            pos[2] = hp.z * s + g_cfg.handOffset[2] * 0.01f * s;
            for (int k = 0; k < 3; k++) { Vec3 c = r.col(k); axes[k][0] = c.x; axes[k][1] = c.y; axes[k][2] = c.z; }
        }
        IKSetHand(h, a.valid, pos, axes);
    }

    int headYaw = pov->yaw, headPitch = pov->pitch;

    // debug: look at the body from elsewhere without moving the hands ("viewoffset <fwd> <right> <up> <yawDeg>")
    if (g_debugViewOffset[0] || g_debugViewOffset[1] || g_debugViewOffset[2] || g_debugViewOffset[3]) {
        Vec3 o = Mul(body, Vec3{ g_debugViewOffset[0], g_debugViewOffset[1], g_debugViewOffset[2] });
        int dy = (int)(g_debugViewOffset[3] * 65536.0f / 360.0f);
        pov->x += o.x; pov->y += o.y; pov->z += o.z;
        pov->yaw = (pov->yaw + dy) & 0xFFFF;
        rightPov.x += o.x; rightPov.y += o.y; rightPov.z += o.z;
        rightPov.yaw = (rightPov.yaw + dy) & 0xFFFF;
    }
    if (pair) GameSetRightEyeView(rightPov);

    // ---- weapon aim (applied by game.cpp after the view has been drawn) ----
    // vehicle form: the vehicle drives towards its aim. Aim with the right controller (or the gaze, VehicleHeadAim=1).
    // The aim ray starts at the hand (or the eyes) in the view; with the chase camera that is behind the vehicle,
    // so the shots start at the vehicle's top and go to the point the ray looks at.
    bool vehicleForm = g_cfg.aimMode == 1 && IKPawnIsVehicle();
    if (vehicleForm) {
        bool gaze = g_cfg.vehicleHeadAim != 0 || !eye.aim[1].valid;
        if (gaze) g_vehicleAimTick = GetTickCount();
        Vec3 rayO, dir;
        if (gaze) {
            rayO = { viewPos[0], viewPos[1], viewPos[2] };
            dir = RotatorToMat(headPitch, headYaw, 0).col(0);
        } else {
            const auto& a = eye.aim[1];
            Vec3 h = XrToUE({ a.px, a.py, a.pz });
            Vec3 hw = Mul(body, Vec3{ h.x * s, h.y * s, h.z * s });
            rayO = { g_latchBase.x + hw.x, g_latchBase.y + hw.y, g_latchBase.z + hw.z };
            dir = Mul(body, XrRotToUE(QuatToMat(a.qx, a.qy, a.qz, a.qw))).col(0);
        }
        Vec3 o = rayO;
        Vec3f pawn;
        if (g_chaseValid && ReadActorLocation(viewTarget, pawn)) o = { pawn.x, pawn.y, pawn.z + g_cfg.vehicleEyeHeight };
        float d = CameraAimHitMeters() > 0.0f ? CameraAimHitMeters() * g_cfg.worldScale : 5000.0f;
        Vec3 t = { rayO.x + dir.x * d - o.x, rayO.y + dir.y * d - o.y, rayO.z + dir.z * d - o.z };
        float pitch = atan2f(t.z, sqrtf(t.x * t.x + t.y * t.y));
        // aiming steeply up or down threw the vehicle off the map: the vehicle aims within -20..+35 degrees
        pitch = fminf(fmaxf(pitch, -20.0f * 0.0174533f), 35.0f * 0.0174533f);
        int ap = (int)(pitch / kUURotToRad) & 0xFFFF;
        int ay = (int)(atan2f(t.y, t.x) / kUURotToRad) & 0xFFFF;
        GameSetAim(true, ap, ay, o.x, o.y, o.z);
        UpdateAimHit(pc, rayO.x, rayO.y, rayO.z);   // distance along the VR ray: the ring
        // boosting cars ignore the controller rotation and steer with the right stick only: steer towards the aim
        __try {
            int err = (short)(uint16_t)((ay - *(int*)((uint8_t*)viewTarget + 0xE8)) & 0xFFFF);
            g_vehicleSteer = fminf(fmaxf(err * kUURotToRad / (25.0f * 0.0174533f), -1.0f), 1.0f);
            g_vehicleSteerTick = GetTickCount();
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_vehicleSteer = 0.0f; }
        g_moveYawDelta = 0.0f;
        g_moveTick = GetTickCount();
    } else if (g_cfg.aimMode == 1 && eye.aim[VRGunHand() == 0 ? 0 : 1].valid) {
        int gh = VRGunHand() == 0 ? 0 : 1;   // the gun's hand (the left one when the gun was taken there)
        const auto& a = eye.aim[gh];
        int ap, ay, ar;
        MatToRotator(Mul(body, XrRotToUE(HandAimXR(eye, gh))), ap, ay, ar);
        float sx, sz;
        int hitYaw;
        if (MeleeTargetYaw(hitYaw)) {   // the weapon hit a robot: the game's attack goes at it
            ay = hitYaw;
            ap = 0;
        } else if (VRMeleeSwing(sx, sz)) {
            // melee swing: strike where the hand swung (OpenXR -z forward, x right -> UE yaw in the body frame)
            ay = ((int)g_latchYaw + (int)(atan2f(sx, -sz) / kUURotToRad)) & 0xFFFF;
            ap = 0;
        }
        // shots start at the hand, so they travel along the same ray as the VR crosshair
        Vec3 h = XrToUE({ a.px, a.py, a.pz });
        Vec3 hw = Mul(body, Vec3{ h.x * s, h.y * s, h.z * s });
        GameSetAim(true, ap, ay, g_latchBase.x + hw.x, g_latchBase.y + hw.y, g_latchBase.z + hw.z);
        UpdateAimHit(pc, g_latchBase.x + hw.x, g_latchBase.y + hw.y, g_latchBase.z + hw.z);
        // the game moves the pawn relative to the aim: turn the stick by (head - aim) so "forward" is where you look
        int d = (short)(uint16_t)((headYaw - ay) & 0xFFFF);
        g_moveYawDelta = g_cfg.moveByHead ? d * kUURotToRad : 0.0f;
        g_moveTick = GetTickCount();
    }
}

// Where the game's aim trace hits (TnPlayerTargeting = PlayerController+0x568, hit location at +0x28), as a distance
// from the aim origin in metres: the VR ring is drawn there, so it sits exactly where shots land.
static volatile float g_aimHitMeters;
static volatile DWORD g_aimHitTick;

static Vec3 g_aimHitWorld;

static void UpdateAimHit(void* pc, float ox, float oy, float oz)
{
    __try {
        uint8_t* targeting = *(uint8_t**)((uint8_t*)pc + 0x568);
        if (!targeting) return;
        const float* hit = (const float*)(targeting + 0x28);
        float dx = hit[0] - ox, dy = hit[1] - oy, dz = hit[2] - oz;
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > 1.0f && d < 1e6f) {
            g_aimHitMeters = d / g_cfg.worldScale;
            g_aimHitWorld = { hit[0], hit[1], hit[2] };
            g_aimHitTick = GetTickCount();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

float CameraAimHitMeters()
{
    return GetTickCount() - g_aimHitTick < 250 ? g_aimHitMeters : 0.0f;
}

bool CameraVehicleAim()
{
    return GetTickCount() - g_vehicleAimTick < 250;
}

float CameraVehicleSteer()
{
    return CameraVehicleAimFresh() ? g_vehicleSteer : 0.0f;
}

float CameraMoveYawDelta()
{
    return GetTickCount() - g_moveTick < 250 ? g_moveYawDelta : 0.0f;
}
