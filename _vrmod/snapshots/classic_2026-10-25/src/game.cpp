// Engine patches for the retail TWFC.exe build (PE timestamp 1274231782, base 0x10900000).
#include "common.h"
#include <stdio.h>

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

static DWORD WINAPI PatchThread(LPVOID)
{
    volatile float* fps = (volatile float*)addr::kFpsLimit;
    float last = -1.0f;
    for (;;) {
        // The engine (re)initialises the cap while loading config/maps, so keep enforcing it.
        // With a headset session the runtime paces frames (xrWaitFrame), so the cap is raised.
        float want = VRIsRunning() ? g_cfg.vrFpsLimit : g_cfg.fpsLimit;
        if (want > 0.0f && *fps != want) {
            if (*fps != last) Log("game: fps cap %.1f -> %.1f", *fps, want);
            *fps = want;
            last = want;
        }
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
static int g_framePitch, g_frameYaw;
static volatile bool g_frameAimValid;
static long g_frameCount;

static void __stdcall OnFrameStart()
{
    g_frameCount++;
    if (!g_frameAimValid || !g_framePC) return;
    __try {
        int* rot = (int*)(g_framePC + kActorLocation + 12);   // PlayerController.Rotation
        rot[0] = g_framePitch;
        rot[1] = g_frameYaw;
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
void GameSetAim(bool valid, int pitch, int yaw)
{
    g_framePitch = pitch;
    g_frameYaw = yaw;
    g_frameAimValid = valid;
}

long GameFrameCount() { return g_frameCount; }

static void __fastcall HookUpdateViewTarget(void* self, void* edx, uint8_t* outVT, float dt)
{
    g_origUpdateViewTarget(self, edx, outVT, dt);
    TPOV* pov = (TPOV*)(outVT + 8);
    if (g_probeYaw) pov->yaw += g_probeYaw;
    uint8_t* cam = (uint8_t*)pov - 0x324;
    g_framePC = *(uint8_t**)(cam + 0x1C8);   // Camera.PCOwner
    g_frameAimValid = false;                  // re-armed below (or by the VR camera) every frame
    CameraOnUpdate(pov, *(void**)outVT, dt);
    if (g_splitTest == 4) {
        GameSetAim(true, pov->pitch, (pov->yaw + 16384) & 0xFFFF);   // aim 90 degrees right of the view
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

void GameStartPatchThread()
{
    CloseHandle(CreateThread(nullptr, 0, PatchThread, nullptr, 0, nullptr));
    MH_Initialize();
    if (MH_CreateHook((void*)addr::kUpdateViewTarget, (void*)HookUpdateViewTarget, (void**)&g_origUpdateViewTarget) == MH_OK &&
        MH_EnableHook((void*)addr::kUpdateViewTarget) == MH_OK)
        Log("game: camera hook installed");
    if (MH_CreateHook((void*)addr::kFrameWait, (void*)HookFrameWait, &g_origFrameWait) == MH_OK &&
        MH_EnableHook((void*)addr::kFrameWait) == MH_OK)
        Log("game: frame-start hook installed");
    if (g_cfg.probeCamera) {
        if (MH_CreateHook((void*)addr::kCameraFovBlend, (void*)HookFovBlend, (void**)&g_origFovBlend) == MH_OK &&
            MH_EnableHook((void*)addr::kCameraFovBlend) == MH_OK)
            Log("probe: camera FOV hook installed");
    }
}
