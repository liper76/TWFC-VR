#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#include <stdint.h>
#include <string>
#include <mutex>
#include <algorithm>

// ---- logging / config (main.cpp) ----
void Log(const char* fmt, ...);
std::wstring ModDir();

struct Config {
    float fpsLimit = 120.0f;        // 0 = leave game default
    float vrFpsLimit = 300.0f;      // cap while the headset session runs (the headset paces the game)
    int   vrEnabled = 1;
    int   vrMode = 1;               // 0 = theater (virtual screen) always, 1 = stereo 3D in gameplay
    float worldScale = 100.0f;       // Unreal units per real metre for head movement
    float fovScale = 1.0f;          // >1 renders a wider frustum than the headset needs
    int   cameraMode = 0;           // 0 = third person (game camera), 1 = first person (at the pawn's eyes)
    float eyeHeight = 180.0f;      // first person: eye height above the pawn origin (Unreal units)
    float eyeForward = 30.0f;       // first person: eye offset forward of the pawn origin
    int   keepStickPitch = 0;       // 1 = keep the game camera pitch under the head pitch
    int   snapTurnDeg = 30;
    float screenDistance = 3.0f;    // metres
    float screenWidth = 4.0f;       // metres
    float screenHeight = 1.6f;      // metres above floor/eye origin offset
    int   vrControllers = 1;        // feed VR controllers into XInput
    float stickDeadzone = 0.15f;
    int   alwaysFocused = 1;        // game keeps input/rendering when its window is not in front
    int   debugAutomation = 0;      // twfcvr_cmd.txt drives a virtual pad
    int   logFps = 0;
    int   probeCamera = 0;
    int   fakeHmd = 0;              // synthetic head pose for testing without a headset
    int   logCameraSeq = 0;         // debug: mark N camera updates in the log
};
extern Config g_cfg;

// ---- game.cpp ----
bool GameIsSupportedBuild();
void GameStartPatchThread();

struct Vec3f { float x, y, z; };

// UE3 camera point of view as laid out in this build (FTViewTarget+8 / CameraCache.POV).
#pragma pack(push, 4)
struct TPOV {
    float x, y, z;              // Location (Unreal units, +X forward, +Y right, +Z up)
    int pitch, yaw, roll;       // Rotation (65536 = 360 degrees)
    float fov;                  // horizontal FOV in degrees
};
#pragma pack(pop)

// ---- camera.cpp ----
void CameraOnUpdate(TPOV* pov, void* viewTarget, float dt);   // game thread, after the engine computed the view
bool ReadActorLocation(void* actor, Vec3f& out);               // game.cpp, SEH-guarded
void GameSetAim(bool valid, int pitch, int yaw);               // game.cpp: weapon aim applied at next frame start
long GameFrameCount();
bool CameraInGameplay();                    // camera updated recently (not in menus / movies)
void CameraSnapTurn(int units);

// ---- vr.cpp: stereo frame pairing ----
struct VREyeView {
    int eye;                     // 0 = left, 1 = right
    float px, py, pz;            // eye position in LOCAL space (OpenXR, metres)
    float qx, qy, qz, qw;        // eye orientation (OpenXR)
    float renderFovDeg;          // horizontal FOV the game must render with
};
bool VRBeginGameFrame(VREyeView& out);      // game thread: picks the eye for this frame
bool VRIsRunning();

// ---- focus.cpp ----
void FocusStart();

// ---- debug.cpp ----
void DebugStart();
bool DebugGetPad(XINPUT_GAMEPAD& out);

// ---- vr.cpp ----
void VRStart();   // installs present hooks from a worker thread

// Latest VR controller state mapped to an Xbox pad (written by the VR thread, read by XInput proxy).
struct VRPadState {
    bool active = false;
    XINPUT_GAMEPAD pad = {};
};
void VRGetPad(VRPadState& out);

