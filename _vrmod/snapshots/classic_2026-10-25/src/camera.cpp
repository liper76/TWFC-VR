// VR camera: applies the headset eye pose to the engine's final point of view (game thread).
// Stereo is alternate-eye (AFR): every game frame renders one eye; vr.cpp pairs the frames up.
#include "common.h"
#include "vrmath.h"

static DWORD g_lastCameraTick;
static int g_snapYaw;            // accumulated snap-turn, UE rotator units

bool CameraInGameplay()
{
    return GetTickCount() - g_lastCameraTick < 250;
}

void CameraSnapTurn(int units)
{
    g_snapYaw = (g_snapYaw + units) & 0xFFFF;
}

void CameraOnUpdate(TPOV* pov, void* viewTarget, float dt)
{
    g_lastCameraTick = GetTickCount();
    if (g_cfg.logCameraSeq > 0) {
        g_cfg.logCameraSeq--;
        Log("seq: --- UpdateViewTarget done (pov %p) ---", pov);
    }
    if (g_cfg.logFps) {
        static DWORD t0 = GetTickCount();
        static long calls;
        calls++;
        if (g_lastCameraTick - t0 >= 5000) {
            static long f0;
            long f = GameFrameCount();
            Log("camera: %.1f view updates/s, %.1f frame waits/s (dt %.4f)", calls * 1000.0f / (g_lastCameraTick - t0),
                (f - f0) * 1000.0f / (g_lastCameraTick - t0), dt);
            f0 = f;
            t0 = g_lastCameraTick;
            calls = 0;
        }
    }
    VREyeView eye;
    if (!VRBeginGameFrame(eye)) return;

    // Body frame: game yaw (from stick / character) plus snap turn; pitch from the stick is dropped
    // unless configured, the head supplies pitch and roll.
    int basePitch = g_cfg.keepStickPitch ? pov->pitch : 0;
    Mat3 body = RotatorToMat(basePitch, (pov->yaw + g_snapYaw) & 0xFFFF, 0);

    Mat3 head = XrRotToUE(QuatToMat(eye.qx, eye.qy, eye.qz, eye.qw));
    Mat3 view = Mul(body, head);
    MatToRotator(view, pov->pitch, pov->yaw, pov->roll);

    Vec3 off = XrToUE({ eye.px, eye.py, eye.pz });
    float s = g_cfg.worldScale;
    off = { off.x * s, off.y * s, off.z * s };
    Vec3 w = Mul(body, off);

    // First person: put the view at the robot's eyes instead of the over-the-shoulder camera.
    Vec3f pawn;
    if (g_cfg.cameraMode == 1 && ReadActorLocation(viewTarget, pawn)) {
        Vec3 fwd = Mul(body, Vec3{ g_cfg.eyeForward, 0.0f, 0.0f });
        pov->x = pawn.x + fwd.x;
        pov->y = pawn.y + fwd.y;
        pov->z = pawn.z + g_cfg.eyeHeight;
    }
    pov->x += w.x;
    pov->y += w.y;
    pov->z += w.z;
    pov->fov = eye.renderFovDeg;
}
