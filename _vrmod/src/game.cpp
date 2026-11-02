// Engine patches for the retail TWFC.exe build (PE timestamp 1274231782, base 0x10900000).
#include "common.h"
#include "MinHook.h"
#include <stdio.h>
#include <intrin.h>

namespace addr {
    const uintptr_t kImageBase = 0x10900000;
    const uint32_t kTimestamp = 1274231782;
    const uintptr_t kFpsLimit = 0x12A03F30;   // float, engine frame rate cap (default 30)
}

static uintptr_t ExeBase() { return (uintptr_t)GetModuleHandleW(nullptr); }

bool GameIsSupportedBuild()
{
    uintptr_t base = ExeBase();
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    Log("game: exe base %08X timestamp %u", (unsigned)base, (unsigned)nt->FileHeader.TimeDateStamp);
    return base == addr::kImageBase && nt->FileHeader.TimeDateStamp == addr::kTimestamp;
}

// Near clipping plane of the scene view. The engine builds the projection from two constants (near 100,
// far 100000 -> m22 0.999, m32 -99.9); 100 units is half a metre at VR scale, so hands near the face vanished.
// The two loads are redirected to our own values (the 0.999 constant itself is shared with other code).
namespace addr {
    const uintptr_t kProjM22Operand = 0x11451929;   // movss xmm1, [0x129D2678]
    const uintptr_t kProjM32Operand = 0x1145194C;   // movss xmm1, [0x129D2A18]
}
static float g_projM22 = 0.999f, g_projM32 = -99.9f;
static float g_gameNear = 100.0f, g_wantNear = 100.0f;

static void UpdateNearClip()
{
    const float farZ = 100000.0f;
    float n = g_cfg.nearClip > 0.0f ? g_cfg.nearClip : 100.0f;
    g_projM22 = farZ / (farZ - n);
    g_projM32 = -n * farZ / (farZ - n);
    g_wantNear = n;
}

// FPerspectiveMatrix(halfFov, width, height) with MinZ in xmm0 builds the scene views' projections; the views
// pass the engine's near plane (100). Only that value is replaced, so other projections keep theirs.
namespace addr {
    const uintptr_t kPerspectiveMatrix = 0x10DD5530;
}
static void* g_origPerspective;

__declspec(naked) static void HookPerspective()
{
    __asm {
        ucomiss xmm0, dword ptr [g_gameNear]
        jne keep
        movss xmm0, dword ptr [g_wantNear]
    keep:
        jmp dword ptr [g_origPerspective]
    }
}

// Canvas Project (the UCanvas in edi, the world point in edx, the result in esi, returned in eax): script's
// Canvas.Project and the native HUD's markers. While the HUD is split off, the points are spread about the
// canvas centre as for the eyes' frustum without the head's roll (VRHudProjectScale, vr.cpp).
namespace addr {
    const uintptr_t kCanvasProject = 0x115D54E0;
}
static void* g_origCanvasProject;

static void __cdecl SpreadProjected(float* out, const unsigned char* canvas)
{
    float s = VRHudProjectScale();
    if (s == 1.0f || !HudSplitActive()) return;
    float cx = *(const float*)(canvas + 0x34) * 0.5f, cy = *(const float*)(canvas + 0x38) * 0.5f;   // ClipX, ClipY
    out[0] = cx + (out[0] - cx) * s;
    out[1] = cy + (out[1] - cy) * s;
}

__declspec(naked) static void HookCanvasProject()
{
    __asm {
        call dword ptr [g_origCanvasProject]
        push eax
        sub esp, 48                  // the original leaves xmm5-7 alone
        movups [esp], xmm5
        movups [esp + 16], xmm6
        movups [esp + 32], xmm7
        push edi
        push esi
        call SpreadProjected
        add esp, 8
        movups xmm5, [esp]
        movups xmm6, [esp + 16]
        movups xmm7, [esp + 32]
        add esp, 48
        pop eax
        ret
    }
}

static void InstallNearClip()
{
    if (MH_CreateHook((void*)addr::kCanvasProject, (void*)HookCanvasProject, &g_origCanvasProject) == MH_OK &&
        MH_EnableHook((void*)addr::kCanvasProject) == MH_OK)
        Log("game: canvas project hook installed");
    UpdateNearClip();
    if (MH_CreateHook((void*)addr::kPerspectiveMatrix, (void*)HookPerspective, &g_origPerspective) == MH_OK &&
        MH_EnableHook((void*)addr::kPerspectiveMatrix) == MH_OK)
        Log("game: projection hook installed");
    uintptr_t ops[2] = { addr::kProjM22Operand, addr::kProjM32Operand };
    float* vals[2] = { &g_projM22, &g_projM32 };
    for (int i = 0; i < 2; i++) {
        DWORD old;
        if (*(uint32_t*)ops[i] != (i ? 0x129D2A18u : 0x129D2678u)) { Log("game: near clip patch: unexpected code"); return; }
        VirtualProtect((void*)ops[i], 4, PAGE_EXECUTE_READWRITE, &old);
        *(float**)ops[i] = vals[i];
        VirtualProtect((void*)ops[i], 4, old, &old);
    }
    Log("game: near clip plane %.1f", g_cfg.nearClip);
}

// ---------------------------------------------------------------------------------------------
// OpenXR tick pacing + legacy AFR eye-pair time sync.
// In StereoPerFrame=1 + SingleThreadRender=1 (default), VRBeginTick() runs right before
// appUpdateTimeAndHandleMaxTickRate (0x110029B0) measures QueryPerformanceCounter: xrWaitFrame paces the tick,
// xrBeginFrame + UpdateInput run once at tick start, GDeltaTime is the true natural frame time (never frozen),
// and both eyes render from the exact same ticked world state at the end of UGameEngine::Tick.
// In StereoPerFrame=0 (AFR fallback), SyncEyeTime holds GDeltaTime on right-eye ticks.
namespace addr {
    const uintptr_t kUpdateTime = 0x110029B0;        // appUpdateTimeAndHandleMaxTickRate
    const uintptr_t kAfterUpdateTime = 0x1100956C;   // main loop, right after appUpdateTimeAndHandleMaxTickRate
    const uintptr_t kGDeltaTime = 0x12A05540;        // double
}
static void* g_origUpdateTime;
static void* g_origAfterUpdateTime;
static double g_heldDelta;
static volatile bool g_timeFrozen;
static volatile DWORD g_gameThread;

DWORD GameThreadId() { return g_gameThread; }

static void __stdcall OnBeforeUpdateTime()
{
    if (!g_gameThread) g_gameThread = GetCurrentThreadId();
    VRBeginTick();
}

__declspec(naked) static void HookUpdateTime()
{
    __asm {
        pushad
        pushfd
        call OnBeforeUpdateTime
        popfd
        popad
        jmp dword ptr [g_origUpdateTime]
    }
}

static void __stdcall OnFrameDelta()
{
    if (!g_gameThread) g_gameThread = GetCurrentThreadId();
    GfxTick();   // the mod's requests to the Scaleform movies (game thread)
    double* dt = (double*)addr::kGDeltaTime;
    if (g_cfg.logCameraSeq > 0) Log("seq: frame dt %.2f ms (held %.2f)", *dt * 1000.0, g_heldDelta * 1000.0);
    int eye;
    bool dualEye = g_cfg.stereoPerFrame && GameSingleThreadRender();
    // Held frames (AFR fallback only) still advance 1 ms: with 0.01 ms vehicle physics divided by tiny dt.
    bool stereo = !dualEye && VRNextStereoEye(eye) && g_cfg.syncEyeTime && (g_cfg.syncEyeTimeVehicle || !IKPawnIsVehicle());
    g_timeFrozen = stereo;
    if (stereo && eye == 1) {
        const double tiny = g_cfg.heldFrameTime;   // not zero: some code divides by the frame time
        if (*dt > tiny) { g_heldDelta += *dt - tiny; *dt = tiny; }
    } else if (g_heldDelta > 0.0) {
        *dt += g_heldDelta;
        g_heldDelta = 0.0;
    }
    if (g_heldDelta > 0.25) g_heldDelta = 0.25;   // never bank more than a hitch

    // stats for hunting hitches: frames, frames held for the right eye, the longest frame time
    static long frames, held;
    static double maxDt;
    static DWORD t0;
    frames++;
    if (stereo && eye == 1) held++;
    if (*dt > maxDt) maxDt = *dt;
    if (g_cfg.logFps && GetTickCount() - t0 > 5000) {
        extern volatile long g_vrViewFailures;
        if (dualEye)
            Log("timing: %ld stereo ticks in 5 s (both eyes/tick), longest step %.1f ms, view failures %ld",
                frames, maxDt * 1000.0, g_vrViewFailures);
        else if (stereo || held)
            Log("timing: %ld frames, %ld right-eye (time held), longest step %.1f ms, view failures %ld",
                frames, held, maxDt * 1000.0, g_vrViewFailures);
        frames = held = 0;
        maxDt = 0;
        g_vrViewFailures = 0;
        t0 = GetTickCount();
    }
}

bool GameTimeFrozenForRightEye() { return g_timeFrozen; }

__declspec(naked) static void HookAfterUpdateTime()
{
    __asm {
        pushad
        pushfd
        call OnFrameDelta
        popfd
        popad
        jmp dword ptr [g_origAfterUpdateTime]
    }
}

static DWORD WINAPI PatchThread(LPVOID)
{
    volatile float* fps = (volatile float*)addr::kFpsLimit;
    float last = -1.0f;
    for (;;) {
        UpdateNearClip();
        // The engine (re)initialises the cap while loading config/maps, so keep enforcing it.
        // With a headset session the runtime paces frames (xrWaitFrame), so the cap is raised.
        float want = VRIsRunning() ? g_cfg.vrFpsLimit : g_cfg.fpsLimit;
        if (want > 0.0f && *fps != want) {
            if (*fps != last) Log("game: fps cap %.1f -> %.1f", *fps, want);
            *fps = want;
            last = want;
        }
        ConfigReloadIfChanged();
        Sleep(500);
    }
}

// ---------------------------------------------------------------------------------------------
// Camera probe (research): hooks the camera FOV-blend function and logs its POV structs.
#include "MinHook.h"

namespace addr {
    const uintptr_t kCameraFovBlend = 0x10A96500;   // __thiscall(this, POV* out, float, POV* in, float), ret 0x10
}

int g_probeYaw;   // set by the debug script ("probeyaw <units>")

typedef void*(__fastcall* PFN_FovBlend)(void* self, void* edx, void* out, float a2, void* in, float a4);
static PFN_FovBlend g_origFovBlend;

static void DumpPov(const char* tag, const void* p)
{
    const float* f = (const float*)p;
    const int* i = (const int*)p;
    char buf[1024];
    int n = 0;
    for (int k = 0; k < 0x5c / 4; k++)
        n += sprintf_s(buf + n, sizeof(buf) - n, "%02x:%.2f/%d ", k * 4, f[k], i[k]);
    Log("probe %s %s", tag, buf);
}

static void* __fastcall HookFovBlend(void* self, void* edx, void* out, float a2, void* in, float a4)
{
    void* r = g_origFovBlend(self, edx, out, a2, in, a4);
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last > 2000) {
        last = now;
        Log("probe this=%p out=%p in=%p a2=%.3f a4=%.3f", self, out, in, a2, a4);
        DumpPov("in ", in);
        DumpPov("out", out);
    }
    return r;
}

// AActor::Location in this build (verified on the player pawn: +0xD8 matches the pawn position).
static const uintptr_t kActorLocation = 0xD8;

bool ReadActorLocation(void* actor, Vec3f& out)
{
    if (!actor) return false;
    __try {
        out = *(Vec3f*)((uint8_t*)actor + kActorLocation);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadActorRotation(void* actor, int out[3])
{
    if (!actor) return false;
    __try {
        memcpy(out, (uint8_t*)actor + kActorLocation + 12, 12);   // Rotation (pitch, yaw, roll) after Location
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ACamera::UpdateViewTarget(FTViewTarget& OutVT, FLOAT DeltaTime) - __thiscall, ret 8. OutVT+8 = TPOV.
namespace addr {
    const uintptr_t kUpdateViewTarget = 0x113A8390;
    const uintptr_t kFrameWait = 0x11593600;
}
typedef void(__fastcall* PFN_UpdateViewTarget)(void* self, void* edx, uint8_t* outVT, float dt);
static PFN_UpdateViewTarget g_origUpdateViewTarget;

// Split test (research): gameplay sees a POV turned 90 degrees, the suspected view-building path gets the
// real (head) POV restored on entry. Tells which reader of the camera cache builds the rendered view.
int g_splitTest;
static TPOV* g_splitPov;
static TPOV g_splitHead;
static void* g_origSplitFn;

static void __stdcall OnSplitFn()
{
    if (!g_splitTest || !g_splitPov) return;
    *g_splitPov = g_splitHead;                                      // OutVT.POV
    *(TPOV*)((uint8_t*)g_splitPov - 0x24) = g_splitHead;             // camera cache copy (cam+0x300)
    uint8_t* cam = (uint8_t*)g_splitPov - 0x324;
    if (g_splitTest >= 2) memcpy(cam + kActorLocation, &g_splitHead, 24);       // camera actor Location+Rotation
    uint8_t* pc = *(uint8_t**)(cam + 0x1C8);
    if (g_splitTest >= 3 && pc) memcpy(pc + kActorLocation, &g_splitHead, 24);  // PlayerController Location+Rotation
}

__declspec(naked) static void HookSplitFn()
{
    __asm {
        pushad
        pushfd
        call OnSplitFn
        popfd
        popad
        jmp dword ptr [g_origSplitFn]
    }
}

// Frame-start hook: 0x11593600 is the engine's max-tick-rate wait, called once per frame between the
// previous frame's view drawing and this frame's actor tick (weapons fire during the actor tick).
static void* g_origFrameWait;
static uint8_t* g_framePC;
static TPOV* g_frameView;                    // OutVT.POV of the last camera update
static int g_framePitch, g_frameYaw;
static float g_frameAimLoc[3];
static volatile bool g_frameAimValid;
static long g_frameCount;

int g_splitWait = 1;                 // split test 5: restore the head POV at this frame wait after the view update
static int g_waitsSinceView;
static TPOV* g_split5Pov;
static TPOV g_split5Head;

static void __stdcall OnFrameStart()
{
    g_frameCount++;
    g_waitsSinceView++;
    if (g_cfg.logCameraSeq > 0) Log("seq: frame wait");
    if ((g_splitTest == 8 || g_splitTest == 9) && g_split5Pov && g_waitsSinceView == g_splitWait) {
        // Reverse split: the view was drawn with the head POV; now hand the gameplay tick the aim POV.
        TPOV aim = g_split5Head;
        aim.yaw = (aim.yaw + 16384) & 0xFFFF;
        uint8_t* cam = (uint8_t*)g_split5Pov - 0x324;
        *g_split5Pov = aim;
        *(TPOV*)(cam + 0x300) = aim;
        memcpy(cam + kActorLocation, &aim, 24);
        uint8_t* pc = *(uint8_t**)(cam + 0x1C8);
        if (g_splitTest == 9 && pc) memcpy(pc + kActorLocation + 12, &aim.pitch, 12);   // PlayerController.Rotation only
        return;
    }
    if (g_splitTest >= 5 && g_split5Pov && g_waitsSinceView == g_splitWait) {
        uint8_t* cam = (uint8_t*)g_split5Pov - 0x324;
        *g_split5Pov = g_split5Head;                            // OutVT.POV
        *(TPOV*)(cam + 0x300) = g_split5Head;                   // CameraCache.POV
        memcpy(cam + kActorLocation, &g_split5Head, 24);        // camera actor Location + Rotation
        uint8_t* pc = *(uint8_t**)(cam + 0x1C8);
        if (g_splitTest >= 6 && pc) memcpy(pc + kActorLocation, &g_split5Head, 24);   // PlayerController Location + Rotation
        if (g_splitTest == 7) {
            static DWORD last;
            if (GetTickCount() - last > 2000) {
                last = GetTickCount();
                Log("split7: head x %.3f (bits %08X), marker x %.3f (bits %08X)", g_split5Head.x, *(unsigned*)&g_split5Head.x,
                    g_split5Head.x + 1000.0f, [](float f) { return *(unsigned*)&f; }(g_split5Head.x + 1000.0f));
            }
        }
    }
    // Controller aim. The engine builds the rendered view right after UpdateViewTarget (head pose), so on the
    // first frame wait after it the view is already drawn: from here until the next camera update only gameplay
    // (weapons fire in the actor tick) reads the camera/controller rotation. Verified with split test 9:
    // view unchanged, pawn turns and shoots along the injected rotation, no camera drift.
    if (g_waitsSinceView != 1 || !g_frameAimValid || !g_framePC || !g_frameView) return;
    __try {
        uint8_t* cam = (uint8_t*)g_frameView - 0x324;
        struct { float x, y, z; int pitch, yaw, roll; } aim = { g_frameAimLoc[0], g_frameAimLoc[1], g_frameAimLoc[2],
                                                                g_framePitch, g_frameYaw, 0 };
        memcpy(g_frameView, &aim, 24);                           // OutVT.POV location + rotation
        memcpy(cam + 0x300, &aim, 24);                           // CameraCache.POV
        memcpy(cam + kActorLocation, &aim, 24);                  // camera actor Location + Rotation
        memcpy(g_framePC + kActorLocation, &aim, 24);            // PlayerController Location + Rotation (weapon aim)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_framePC = nullptr;
    }
}

__declspec(naked) static void HookFrameWait()
{
    __asm {
        pushad
        pushfd
        call OnFrameStart
        popfd
        popad
        jmp dword ptr [g_origFrameWait]
    }
}

// Called on the game thread after the view is final: the aim the weapons should use next frame.
void GameSetAim(bool valid, int pitch, int yaw, float x, float y, float z)
{
    g_framePitch = pitch;
    g_frameYaw = yaw;
    g_frameAimLoc[0] = x;
    g_frameAimLoc[1] = y;
    g_frameAimLoc[2] = z;
    g_frameAimValid = valid;
}

long GameFrameCount() { return g_frameCount; }

// ---------------------------------------------------------------------------------------------
// Both eyes per game frame. At the end of UGameEngine::Tick, UGameEngine::RedrawViewports (0x11698DA0) calls
// DrawViewport(GameViewport->Viewport) at 0x11698A90 (__stdcall, ret 4). (0x1184FA20 was UAudioDevice::Update.)
// With a stereo pair, DrawViewport runs twice from the exact same game state: first with the left-eye view,
// then with the right-eye view and its own FSceneViewState (so motion blur and occlusion queries stay per-eye).
// Everything that moves (vehicles, robots, IK arms, particles, lifts) is at the identical instant in both eyes.
namespace addr {
    const uintptr_t kDrawViewport = 0x11698A90;       // DrawViewport(FViewport*): __stdcall, ret 4
    const uintptr_t kAllocateViewState = 0x11533290;  // AllocateViewState(): __cdecl -> FSceneViewStateInterface*
    const uintptr_t kGEngine = 0x12B10968;            // UGameEngine*
}
typedef void(__stdcall* PFN_DrawViewport)(void* viewport);
typedef void*(__cdecl* PFN_AllocateViewState)();
static PFN_DrawViewport g_origDrawViewport;
static void* g_rightViewState;
static TPOV g_rightPov;
static volatile long g_rightPovFrame = -1;
float g_debugRightYaw;   // debug "rightyaw <deg>": turns the second drawing, to see which view it uses
int g_debugSkipDraw;     // debug "skipdraw <1|2>": skip loading / end-of-tick viewport draw
int g_debugPass2Frame;   // debug "pass2frame <0|1>"
float g_debugTestView;   // debug "testview <deg>"
int g_debugWindowEye;    // debug "windoweye <0|1|2>": the game window shows only the left (1) / right (2) drawing
int g_debugRightVS = 1;  // debug "rightvs <0|1>": the second drawing uses its own FSceneViewState
int g_debugEyeSwap;      // debug "eyeswap <0|1>": the first drawing gets the right-eye view, the second the left
static volatile bool g_drawingRightEye;
static volatile bool g_inStereoPair;   // both drawings of a stereo pair are under way
static long g_occTests[2], g_occHidden[2];   // shared occlusion test per eye in 5 s: calls, "not visible" results

void GameSetRightEyeView(const TPOV& pov)
{
    g_rightPov = pov;
    g_rightPovFrame = g_frameCount;
}

// The view the renderer takes from the player camera: its view target POV, its cached POV, its location,
// and PlayerController's Location+Rotation (ATransPlayerController::GetPlayerViewPoint at 0x10B197C0 reads
// pc+0xD8..0xEC directly when controlling an ATransPawn, bypassing ACamera::GetCameraViewPoint).
static void SetCameraView(const TPOV& p)
{
    uint8_t* cam = (uint8_t*)g_frameView - 0x324;
    *g_frameView = p;                        // OutVT.POV
    *(TPOV*)(cam + 0x300) = p;               // CameraCache.POV (including .fov at cam+0x318)
    memcpy(cam + kActorLocation, &p, 24);    // camera actor Location + Rotation
    uint8_t* pc = *(uint8_t**)(cam + 0x1C8);
    if (pc) memcpy(pc + kActorLocation, &p, 24);   // PlayerController Location + Rotation
}

static uint8_t* GetLocalPlayer0()
{
    __try {
        uint8_t* eng = *(uint8_t**)addr::kGEngine;
        if (!eng || *(int*)(eng + 0x354) <= 0) return nullptr;
        uint8_t** players = *(uint8_t***)(eng + 0x350);
        return players ? players[0] : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static void __stdcall HookDrawViewport(void* viewport)
{
    bool twice = g_cfg.stereoPerFrame && GameSingleThreadRender() && g_rightPovFrame == g_frameCount &&
                 g_frameView && CameraInGameplay();
    if (twice) g_rightPovFrame = -1;   // draw the stereo pair once per tick
    if (g_cfg.logCameraSeq > 0 && g_frameView)
        Log("seq: DrawViewport %p from %p (twice %d) camera view %.3f %.3f %.3f yaw %d fov %.2f", viewport, _ReturnAddress(),
            twice, g_frameView->x, g_frameView->y, g_frameView->z, g_frameView->yaw, g_frameView->fov);
    if (g_debugTestView && g_frameView) {
        // debug "testview <deg>": the drawing frame is drawn with the camera fields turned
        TPOV saved = *g_frameView, t = saved;
        t.yaw = (t.yaw + (int)(g_debugTestView * 65536.0f / 360.0f)) & 0xFFFF;
        __try { SetCameraView(t); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_origDrawViewport(viewport);
        __try { SetCameraView(saved); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }
    if (g_debugSkipDraw == 1 && (uintptr_t)_ReturnAddress() == 0x115CB494) return;
    if (g_debugSkipDraw == 2 && (uintptr_t)_ReturnAddress() == 0x11698DE2) return;
    static long draws, doubles, logged;
    static DWORD t0;
    draws++;
    if (g_cfg.logFps && logged < 12 && g_frameView) {
        logged++;
        Log("game: DrawViewport %p (twice %d) frame %ld from %p", viewport, twice, g_frameCount, _ReturnAddress());
    }
    if (twice) doubles++;
    if (g_cfg.logFps && GetTickCount() - t0 > 5000) {
        Log("game: %ld viewport draws in 5 s, %ld drawn for both eyes (frame %ld); occlusion tests L %ld/%ld hidden, R %ld/%ld hidden",
            draws, doubles, g_frameCount, g_occHidden[0], g_occTests[0], g_occHidden[1], g_occTests[1]);
        draws = doubles = 0;
        g_occTests[0] = g_occTests[1] = g_occHidden[0] = g_occHidden[1] = 0;
        t0 = GetTickCount();
    }
    if (!twice) { g_origDrawViewport(viewport); return; }

    TPOV left = *g_frameView;
    TPOV right = g_rightPov;
    if (g_debugRightYaw) right.yaw = (right.yaw + (int)(g_debugRightYaw * 65536.0f / 360.0f)) & 0xFFFF;
    if (g_debugEyeSwap) std::swap(left, right);

    uint8_t* cam = (uint8_t*)g_frameView - 0x324;
    uint32_t savedCamFlags = 0;
    bool haveCamFlags = false;
    __try {
        savedCamFlags = *(uint32_t*)(cam + 0x1D4);
        *(uint32_t*)(cam + 0x1D4) = savedCamFlags & ~3u;   // clear bLockedFOV | bConstrainAspectRatio for VR frustum
        haveCamFlags = true;
        SetCameraView(left);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_origDrawViewport(viewport);
        return;
    }

    // Pass 1: Left Eye
    g_inStereoPair = true;
    g_origDrawViewport(viewport);

    // Pass 2: Right Eye (same world state, separate FSceneViewState for per-eye motion blur & occlusion queries)
    __try { SetCameraView(right); } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_inStereoPair = false;
        if (haveCamFlags) __try { *(uint32_t*)(cam + 0x1D4) = savedCamFlags; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }
    uint8_t* lp = GetLocalPlayer0();
    void* savedVS = nullptr;
    float savedLastLoc[3] = {};
    __try {
        if (g_debugRightVS && lp && *(void**)(lp + 0x74)) {
            if (!g_rightViewState) {
                g_rightViewState = ((PFN_AllocateViewState)addr::kAllocateViewState)();
                Log("game: allocated right-eye FSceneViewState %p (left %p)", g_rightViewState, *(void**)(lp + 0x74));
            }
            if (g_rightViewState) {
                savedVS = *(void**)(lp + 0x74);
                memcpy(savedLastLoc, lp + 0x80, 12);
                *(void**)(lp + 0x74) = g_rightViewState;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        savedVS = nullptr;
    }

    g_drawingRightEye = true;
    if (g_debugPass2Frame) {   // debug "pass2frame 1": the second drawing gets its own frame number
        ++*(volatile long*)0x12A2E3DC;   // GFrameNumber
        ++*(volatile long*)0x12A363F0;   // GFrameNumberRenderThread
    }
    g_origDrawViewport(viewport);
    g_drawingRightEye = false;
    g_inStereoPair = false;

    __try {
        if (savedVS && lp) {
            *(void**)(lp + 0x74) = savedVS;
            memcpy(lp + 0x80, savedLastLoc, 12);
        }
        SetCameraView(left);
        if (haveCamFlags) *(uint32_t*)(cam + 0x1D4) = savedCamFlags;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool GameDrawingRightEye() { return g_drawingRightEye; }

// Occlusion test shared by both eyes (0x113C75A0, __stdcall, ret 0x14; called from the scene renderer, e.g.
// 0x113CE0D6). Its per-object records live in a container that both drawings share (24-byte entries indexed by
// object+0x48: {frame, result} for the two frame parities of 0x12B109EC, +0x14 = query index), while the query
// itself is issued into the view's FSceneViewState pool (+0x200). With two drawings per tick the right eye read the
// left eye's query index from its own pool: wrong results (things vanish or flicker in one eye) or, with the fresh
// right-eye pool, a crash at 0x1151BFFB. During a stereo pair the test is skipped: "visible", no query - the
// same as the engine's own early exit for objects with flag 0x40 at +0x30.
namespace addr {
    const uintptr_t kSharedOcclusionTest = 0x113C75A0;
}
typedef void(__stdcall* PFN_SharedOcclusionTest)(void*, void*, void*, int*, int*);
static PFN_SharedOcclusionTest g_origSharedOcclusionTest;
int g_debugPairOcclusion;   // debug "pairocc <0|1>": 1 = keep the shared occlusion test in stereo pairs (old bug)

static void __stdcall HookSharedOcclusionTest(void* records, void* obj, void* viewState, int* visible, int* issueQuery)
{
    int eye = g_drawingRightEye ? 1 : 0;
    g_occTests[eye]++;
    if (g_inStereoPair && !g_debugPairOcclusion) {
        *visible = 1;
        *issueQuery = 0;
        return;
    }
    g_origSharedOcclusionTest(records, obj, viewState, visible, issueQuery);
    if (!*visible) g_occHidden[eye]++;
}

// debug: the player camera actor of the last camera update
void* GameCameraActor() { return g_frameView ? (uint8_t*)g_frameView - 0x324 : nullptr; }

static void __fastcall HookUpdateViewTarget(void* self, void* edx, uint8_t* outVT, float dt)
{
    g_origUpdateViewTarget(self, edx, outVT, dt);
    TPOV* pov = (TPOV*)(outVT + 8);
    if (g_probeYaw) pov->yaw += g_probeYaw;
    uint8_t* cam = (uint8_t*)pov - 0x324;
    g_framePC = *(uint8_t**)(cam + 0x1C8);   // Camera.PCOwner
    g_frameView = pov;
    g_frameAimValid = false;                  // re-armed by the VR camera every frame
    g_waitsSinceView = 0;
    CameraOnUpdate(pov, *(void**)outVT, g_framePC, dt);
    g_waitsSinceView = 0;
    if (g_splitTest == 8 || g_splitTest == 9) {
        g_split5Pov = pov;
        g_split5Head = *pov;
        return;
    }
    if (g_splitTest == 5 || g_splitTest == 6 || g_splitTest == 7) {
        g_split5Pov = pov;
        g_split5Head = *pov;
        if (g_splitTest == 7) pov->x += 1000.0f;   // location marker instead of rotation
        else pov->yaw = (pov->yaw + 16384) & 0xFFFF;
        return;
    }
    if (g_splitTest == 4) {
        GameSetAim(true, pov->pitch, (pov->yaw + 16384) & 0xFFFF, pov->x, pov->y, pov->z);   // aim 90 degrees right of the view
        return;
    }
    if (g_splitTest) {
        g_splitPov = pov;
        g_splitHead = *pov;
        pov->yaw = (pov->yaw + 16384) & 0xFFFF;
    }
}

void GameInstallSplitHook(uintptr_t fn)
{
    if (MH_CreateHook((void*)fn, (void*)HookSplitFn, &g_origSplitFn) == MH_OK && MH_EnableHook((void*)fn) == MH_OK)
        Log("split: hook at %08X", (unsigned)fn);
    else
        Log("split: hook at %08X FAILED", (unsigned)fn);
}

// Bink movies (intros, story videos, some loading screens): while one decodes frames the headset shows the virtual
// screen - the game keeps ticking its camera under them, so they counted as gameplay and sat in front of the eyes.
typedef int(__stdcall* BinkDoFrameFn)(void* bink);
static BinkDoFrameFn g_origBinkDoFrame;
static volatile DWORD g_movieTick;

static int __stdcall HookBinkDoFrame(void* bink)
{
    g_movieTick = GetTickCount();
    return g_origBinkDoFrame(bink);
}

void GameInstallMovieHook()
{
    HMODULE bink = LoadLibraryW(L"binkw32.dll");
    void* fn = bink ? (void*)GetProcAddress(bink, "_BinkDoFrame@4") : nullptr;
    bool ok = fn && MH_CreateHook(fn, (void*)HookBinkDoFrame, (void**)&g_origBinkDoFrame) == MH_OK && MH_EnableHook(fn) == MH_OK;
    Log("game: movie detection %s", ok ? "installed" : "FAILED");
}

bool GameMoviePlaying() { return GetTickCount() - g_movieTick < 300; }

// Rendering on the game thread (the engine's own "-ONETHREAD" mode). UE3 renders on a thread of its own, a frame
// or two behind the game, so a presented image could not be matched to the game frame (eye and pose) it was drawn
// for - the eye frames went through a queue that guessed the depth and fell out of step after loads, menus and
// movies. On one thread every frame is drawn and presented inside the game tick that set its view. The engine's
// start-up check (0x11002B60: not -ONETHREAD, more than one CPU -> GUseThreadedRendering = 1, start the thread) ends
// in "mov [GUseThreadedRendering], eax; jmp StartRenderingThread", replaced by a ret. Called from DllMain.
namespace addr {
    const uintptr_t kStartRenderThreadTail = 0x11002BB2;
}

void GameEarlyPatches()
{
    if (!g_cfg.singleThreadRender || !GameIsSupportedBuild()) return;
    uint8_t* p = (uint8_t*)addr::kStartRenderThreadTail;
    static const uint8_t expect[] = { 0xA3, 0x44, 0x0C, 0xB1, 0x12, 0xE9 };
    if (memcmp(p, expect, sizeof(expect)) != 0) { Log("game: render thread patch: unexpected code"); return; }
    DWORD old;
    VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old);
    *p = 0xC3;
    VirtualProtect(p, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 1);
    Log("game: rendering on the game thread (no render thread)");
}

bool GameSingleThreadRender() { return g_cfg.singleThreadRender && *(volatile int*)0x12B10C44 == 0; }

void GameStartPatchThread()
{
    MH_Initialize();
    InstallNearClip();
    if (MH_CreateHook((void*)addr::kUpdateTime, (void*)HookUpdateTime, &g_origUpdateTime) == MH_OK &&
        MH_EnableHook((void*)addr::kUpdateTime) == MH_OK)
        Log("game: OpenXR tick-start hook installed");
    if (MH_CreateHook((void*)addr::kAfterUpdateTime, (void*)HookAfterUpdateTime, &g_origAfterUpdateTime) == MH_OK &&
        MH_EnableHook((void*)addr::kAfterUpdateTime) == MH_OK)
        Log("game: eye-pair time sync installed");
    CloseHandle(CreateThread(nullptr, 0, PatchThread, nullptr, 0, nullptr));
    if (MH_CreateHook((void*)addr::kUpdateViewTarget, (void*)HookUpdateViewTarget, (void**)&g_origUpdateViewTarget) == MH_OK &&
        MH_EnableHook((void*)addr::kUpdateViewTarget) == MH_OK)
        Log("game: camera hook installed");
    if (MH_CreateHook((void*)addr::kFrameWait, (void*)HookFrameWait, &g_origFrameWait) == MH_OK &&
        MH_EnableHook((void*)addr::kFrameWait) == MH_OK)
        Log("game: frame-start hook installed");
    if (MH_CreateHook((void*)addr::kDrawViewport, (void*)HookDrawViewport, (void**)&g_origDrawViewport) == MH_OK &&
        MH_EnableHook((void*)addr::kDrawViewport) == MH_OK)
        Log("game: DrawViewport hook installed (both eyes per frame)");
    if (MH_CreateHook((void*)addr::kSharedOcclusionTest, (void*)HookSharedOcclusionTest, (void**)&g_origSharedOcclusionTest) == MH_OK &&
        MH_EnableHook((void*)addr::kSharedOcclusionTest) == MH_OK)
        Log("game: shared occlusion test hook installed (skipped in stereo pairs)");
    if (g_cfg.probeCamera) {
        if (MH_CreateHook((void*)addr::kCameraFovBlend, (void*)HookFovBlend, (void**)&g_origFovBlend) == MH_OK &&
            MH_EnableHook((void*)addr::kCameraFovBlend) == MH_OK)
            Log("probe: camera FOV hook installed");
    }
}
