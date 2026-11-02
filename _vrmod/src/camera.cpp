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
int g_debugTurret;                   // debug.cpp "turretdbg <0|1|2>": log the turret aim once a second (2: its traces)
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
static float g_turretLift;           // on a mounted turret: eyes raised by (Unreal units, gliding)
static float g_seat;                 // on a mounted turret: how far the eyes are seated on the gun (0..1, gliding)
static float g_seatPitch, g_seatYaw; // the pawn's rotation there (radians)
static bool g_seatValid;             // ... read on this turret

void CameraBaseOffset(float& forward, float& up)
{
    forward = g_baseInit ? g_baseFwd : g_cfg.eyeForward;
    up = (g_baseInit ? g_baseUp : g_cfg.eyeHeight) + g_turretLift;
}

float CameraTurretLift() { return g_turretLift; }

// The eye base for a pawn position (Unreal units): forward and up from the pawn in the body's yaw. On a mounted turret
// the game seats the robot on the gun's back end - it swings round the gun's pivot and pitches with the gun (pawn
// Rotation = the gun's) - and the eyes go with it as if seated: their offset turns with the gun's yaw and pitch, so
// the gun stays where it was in view (the head still looks where it looks, the room keeps the body's yaw).
Vec3f CameraEyeBase(const Vec3f& pawn, float bodyYaw)
{
    float fwd, up;
    CameraBaseOffset(fwd, up);
    float yaw = bodyYaw + remainderf(g_seatYaw - bodyYaw, 6.2831853f) * g_seat;
    Vec3 o = { cosf(yaw) * fwd, sinf(yaw) * fwd, up };
    if (g_seat > 0.0f) {
        float p = g_seatPitch * g_seat, cy = cosf(g_seatYaw), sy = sinf(g_seatYaw);
        float f = o.x * cy + o.y * sy, r = o.y * cy - o.x * sy, u = o.z;
        float f2 = f * cosf(p) - u * sinf(p);
        u = f * sinf(p) + u * cosf(p);
        o = { f2 * cy - r * sy, f2 * sy + r * cy, u };
    }
    return { pawn.x + o.x, pawn.y + o.y, pawn.z + o.z };
}

// On a mounted turret: the gun as it is, relative to the room (LOCAL: yaw + = left, pitch + = up; radians) and how far
// the eyes are seated on it (0..1) - its handrails (vr.cpp) are drawn turned by this times seated, as the eyes are.
bool CameraTurretSeat(float& yaw, float& pitch, float& seated)
{
    if (!g_seatValid) return false;
    yaw = -remainderf(g_seatYaw - g_latchYaw * kUURotToRad, 6.2831853f);
    pitch = g_seatPitch;
    seated = g_seat;
    return true;
}

// An aim relative to the room (as above) kept within how far the turret's gun turns.
void CameraTurretClamp(float& yaw, float& pitch)
{
    float lim[4];
    int bp, by;
    if (!WeaponsTurretLimits(lim, bp, by)) return;
    const float d = 0.0174533f, body = g_latchYaw * kUURotToRad, baseYaw = by * kUURotToRad;
    float basePitch = (short)(uint16_t)bp * kUURotToRad;
    float ry = remainderf(body - yaw - baseYaw, 6.2831853f), rp = pitch - basePitch;   // (the world's yaw: + = right)
    ry = fminf(fmaxf(ry, lim[0] * d), lim[1] * d);
    rp = fminf(fmaxf(rp, lim[2] * d), lim[3] * d);
    yaw = remainderf(body - baseYaw - ry, 6.2831853f);
    pitch = basePitch + rp;
}
static void UpdateAimHit(void* pc, float ox, float oy, float oz);
static volatile float g_aimHitMeters;   // UpdateAimHit
static Vec3 g_aimHitWorld;
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

// Orientation of a hand's aim in OpenXR space. (The gun hand's is already as the gun is held: turned onto the front
// hand with both hands, a virtual stock, smoothed - vr.cpp's GunPose.)
static Mat3 HandAimXR(const VREyeView& eye, int h)
{
    const auto& a = eye.aim[h];
    return QuatToMat(a.qx, a.qy, a.qz, a.qw);
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
            // on a mounted turret the robot sits low behind it: eyes (and body, ik.cpp) go up to see over it, and
            // are seated on the gun (CameraEyeBase)
            bool onTurret = !vehicle && WeaponsOnTurret();
            float lift = onTurret ? g_cfg.turretEyeUp * g_cfg.worldScale : 0.0f;
            float kt = 1.0f - expf(-pdt / 0.25f);
            g_turretLift += (lift - g_turretLift) * kt;
            if (fabsf(lift - g_turretLift) < 0.5f) g_turretLift = lift;
            g_seat += ((onTurret ? 1.0f : 0.0f) - g_seat) * kt;
            if (fabsf((onTurret ? 1.0f : 0.0f) - g_seat) < 0.002f) g_seat = onTurret ? 1.0f : 0.0f;
            int rot[3];
            g_seatValid = onTurret && ReadActorRotation(viewTarget, rot);
            if (g_seatValid) {
                g_seatPitch = (short)(uint16_t)rot[0] * kUURotToRad;
                g_seatYaw = rot[1] * kUURotToRad;
            }
            Vec3f want = CameraEyeBase(pawn, g_latchYaw * kUURotToRad);
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
                g_latchBase = CameraEyeBase(pawn, g_latchYaw * kUURotToRad);
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

    // ---- the pickup a squeeze would pull (pull.cpp): where it is in the room, for the ring drawn there (vr.cpp) ----
    float pickup[3], pickupR;
    if (g_cfg.cameraMode == 1 && PullMarkWorld(pickup, pickupR)) {
        Vec3 d = { pickup[0] - g_latchBase.x, pickup[1] - g_latchBase.y, pickup[2] - g_latchBase.z };
        Vec3 b = { body.m[0][0] * d.x + body.m[1][0] * d.y + body.m[2][0] * d.z,   // body^T: world -> body frame
                   body.m[0][1] * d.x + body.m[1][1] * d.y + body.m[2][1] * d.z,
                   body.m[0][2] * d.x + body.m[1][2] * d.y + body.m[2][2] * d.z };
        float local[3] = { b.y / s, b.z / s, -b.x / s };   // (XrToUE inverted)
        VRSetPullMark(true, local, pickupR / s);
    } else {
        VRSetPullMark(false, nullptr, 0.0f);
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
        float pitch = atan2f(t.z, sqrtf(t.x * t.x + t.y * t.y));   // (the game limits it per vehicle: jets climb)
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
        auto a = eye.aim[gh];
        int ap, ay, ar;
        Vec3 turretDir = {};
        float tq[4], tp[3];
        bool turret = WeaponsOnTurret() && VRTurretAim(tq, tp);
        if (turret) {
            // a mounted turret is aimed with its handrails, held with one hand or both (VRTurretAim)
            a.qx = tq[0]; a.qy = tq[1]; a.qz = tq[2]; a.qw = tq[3];
            a.px = tp[0]; a.py = tp[1]; a.pz = tp[2];
            Mat3 m = Mul(body, XrRotToUE(QuatToMat(a.qx, a.qy, a.qz, a.qw)));
            MatToRotator(m, ap, ay, ar);
            turretDir = m.col(0);
            // the gun turns only so far: the aim stops where it does, so the shots go where the barrel points
            float lim[4];
            int bp, by;
            if (WeaponsTurretLimits(lim, bp, by)) {
                const float u = 65536.0f / 360.0f;
                int ry = (short)(uint16_t)((ay - by) & 0xFFFF), rp = (short)(uint16_t)((ap - bp) & 0xFFFF);
                int cy = ry < (int)(lim[0] * u) ? (int)(lim[0] * u) : ry > (int)(lim[1] * u) ? (int)(lim[1] * u) : ry;
                int cp = rp < (int)(lim[2] * u) ? (int)(lim[2] * u) : rp > (int)(lim[3] * u) ? (int)(lim[3] * u) : rp;
                if (cy != ry || cp != rp) {
                    ay = (by + cy) & 0xFFFF;
                    ap = (bp + cp) & 0xFFFF;
                    float yr = ay * kUURotToRad, pr = (short)(uint16_t)ap * kUURotToRad;
                    turretDir = { cosf(pr) * cosf(yr), cosf(pr) * sinf(yr), sinf(pr) };
                }
            }
        } else {
            MatToRotator(Mul(body, XrRotToUE(HandAimXR(eye, gh))), ap, ay, ar);
        }
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
        Vec3 o = { g_latchBase.x + hw.x, g_latchBase.y + hw.y, g_latchBase.z + hw.z };
        // on a turret the aim trace and the shots (instant hits; the game traces them from the eyes, weapons.cpp moves
        // them here) start past the turret - its gun, base, shield and barrels - or they hit it and burst in its
        // muzzle (the ring still measured from the hands)
        Vec3 start = o;
        if (turret) {
            float of[3] = { o.x, o.y, o.z }, df[3] = { turretDir.x, turretDir.y, turretDir.z };
            float clear = WeaponsTurretClearance(of, df);
            start = { o.x + turretDir.x * clear, o.y + turretDir.y * clear, o.z + turretDir.z * clear };
            float sf[3] = { start.x, start.y, start.z };
            WeaponsTurretShotStart(sf, df);
        }
        GameSetAim(true, ap, ay, start.x, start.y, start.z);
        UpdateAimHit(pc, o.x, o.y, o.z);
        if (turret && g_debugTurret) {
            static DWORD at;
            if (GetTickCount() - at > 1000) {
                at = GetTickCount();
                float of[3] = { o.x, o.y, o.z }, df[3] = { turretDir.x, turretDir.y, turretDir.z };
                Vec3 sv = { start.x - o.x, start.y - o.y, start.z - o.z };
                Log("turretdbg: hands %.0f %.0f %.0f dir %.2f %.2f %.2f start along %.0f, hit at %.0f (%.0f %.0f %.0f)",
                    o.x, o.y, o.z, df[0], df[1], df[2], sv.x * df[0] + sv.y * df[1] + sv.z * df[2],
                    g_aimHitMeters * g_cfg.worldScale, g_aimHitWorld.x, g_aimHitWorld.y, g_aimHitWorld.z);
                void WeaponsDebugTurretAim(const float o[3], const float dir[3]);
                WeaponsDebugTurretAim(of, df);
            }
        }
        // the game moves the pawn relative to the aim: turn the stick by (head - aim) so "forward" is where you look
        int d = (short)(uint16_t)((headYaw - ay) & 0xFFFF);
        g_moveYawDelta = g_cfg.moveByHead ? d * kUURotToRad : 0.0f;
        g_moveTick = GetTickCount();
    }
}

// Where the game's aim trace hits (TnPlayerTargeting = PlayerController+0x568, hit location at +0x28), as a distance
// from the aim origin in metres: the VR ring is drawn there, so it sits exactly where shots land.
static volatile DWORD g_aimHitTick;

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
