// Debug automation: commands written to twfcvr_cmd.txt (next to the DLL) drive a virtual gamepad.
// Lines: "press <A|B|X|Y|START|BACK|LB|RB|LS|RS|UP|DOWN|LEFT|RIGHT|LT|RT> <ms>", "stick <LX|LY|RX|RY> <-1..1> <ms>", "wait <ms>",
// "pad <LX> <LY> <RX> <RY> <LT 0..1> <RT 0..1> <ms>"
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <vector>
#include "MinHook.h"

static std::mutex g_dbgMutex;
static XINPUT_GAMEPAD g_dbgPad;
static bool g_dbgActive;

bool DebugGetPad(XINPUT_GAMEPAD& out)
{
    std::lock_guard<std::mutex> lock(g_dbgMutex);
    out = g_dbgPad;
    return g_dbgActive;
}

static WORD ButtonBit(const char* n)
{
    struct { const char* n; WORD b; } map[] = {
        { "A", XINPUT_GAMEPAD_A }, { "B", XINPUT_GAMEPAD_B }, { "X", XINPUT_GAMEPAD_X }, { "Y", XINPUT_GAMEPAD_Y },
        { "START", XINPUT_GAMEPAD_START }, { "BACK", XINPUT_GAMEPAD_BACK }, { "LB", XINPUT_GAMEPAD_LEFT_SHOULDER },
        { "RB", XINPUT_GAMEPAD_RIGHT_SHOULDER }, { "LS", XINPUT_GAMEPAD_LEFT_THUMB }, { "RS", XINPUT_GAMEPAD_RIGHT_THUMB },
        { "UP", XINPUT_GAMEPAD_DPAD_UP }, { "DOWN", XINPUT_GAMEPAD_DPAD_DOWN }, { "LEFT", XINPUT_GAMEPAD_DPAD_LEFT },
        { "RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT },
    };
    for (auto& m : map) if (!_stricmp(m.n, n)) return m.b;
    return 0;
}

static void SetPad(const XINPUT_GAMEPAD& p, bool active)
{
    std::lock_guard<std::mutex> lock(g_dbgMutex);
    g_dbgPad = p;
    g_dbgActive = active;
}

// ---------------------------------------------------------------------------------------------
// Hardware data breakpoints ("watch <hexaddr>", "unwatch"): logs the code that writes an address.
#include <tlhelp32.h>
static uintptr_t g_watchAddr;
static volatile long g_watchHits;
static uintptr_t g_watchSeen[32];
static volatile long g_watchSeq;

static LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 0xF))
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = e->ContextRecord;
    c->Dr6 = 0;
    uintptr_t eip = c->Eip;
    if (g_watchSeq > 0) {
        // Sequence mode: log every access in order (with the caller chain) until the budget runs out.
        g_watchSeq--;
        uintptr_t* sp = (uintptr_t*)c->Esp;
        char stack[256];
        int k = 0, len = 0;
        for (int i = 0; i < 96 && k < 6; i++) {
            uintptr_t v = sp[i];
            if (v >= 0x10901000 && v < 0x11948000) { len += sprintf_s(stack + len, sizeof(stack) - len, " %08X", (unsigned)v); k++; }
        }
        Log("seq: EIP %08X thread %lu stack:%s", (unsigned)eip, GetCurrentThreadId(), stack);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    long n = g_watchHits;
    for (long i = 0; i < n && i < 32; i++) if (g_watchSeen[i] == eip) return EXCEPTION_CONTINUE_EXECUTION;
    if (n < 32) {
        g_watchSeen[n] = eip;
        g_watchHits = n + 1;
        uintptr_t* sp = (uintptr_t*)c->Esp;
        char stack[512];
        int k = 0, len = 0;
        for (int i = 0; i < 400 && k < 40; i++) {
            uintptr_t v = sp[i];
            if (v >= 0x10901000 && v < 0x11948000) { len += sprintf_s(stack + len, sizeof(stack) - len, " %08X", (unsigned)v); k++; }
        }
        Log("watch: access by EIP %08X ecx=%08X esi=%08X edi=%08X ebx=%08X stack:%s", (unsigned)eip, c->Ecx, c->Esi, c->Edi, c->Ebx, stack);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void SetWatch(uintptr_t addr, bool readWrite = false)
{
    static PVOID veh;
    if (!veh) veh = AddVectoredExceptionHandler(1, WatchHandler);
    g_watchAddr = addr;
    g_watchHits = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof(te) };
    int count = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        SuspendThread(t);
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c)) {
            c.Dr0 = addr;
            c.Dr7 = addr ? (1 | ((readWrite ? 3 : 1) << 16) | (3 << 18)) : 0;   // L0, write or read/write, 4 bytes
            c.Dr6 = 0;
            if (SetThreadContext(t, &c)) count++;
        }
        ResumeThread(t);
        CloseHandle(t);
    }
    CloseHandle(snap);
    Log("watch: %08X set on %d threads", (unsigned)addr, count);
}

// "countexec <hexaddr>": counts how often a code address runs (MinHook detour there), "countlog" prints it
static void* g_countTramp;
static volatile long g_countExec;
__declspec(naked) static void CountStub()
{
    __asm {
        lock inc dword ptr [g_countExec]
        jmp dword ptr [g_countTramp]
    }
}

static void RunLine(const char* line)
{
    char cmd[32] = {}, a[32] = {};
    float v = 0, ms = 0, s[6] = {};
    XINPUT_GAMEPAD p = {};
    if (sscanf_s(line, "press %31s %f", a, (unsigned)sizeof(a), &ms) == 2) {
        if (!_stricmp(a, "LT")) p.bLeftTrigger = 255;
        else if (!_stricmp(a, "RT")) p.bRightTrigger = 255;
        else p.wButtons = ButtonBit(a);
        SetPad(p, true);
        Sleep((DWORD)ms);
        SetPad({}, true);
        Sleep(100);
    } else if (sscanf_s(line, "stick %31s %f %f", a, (unsigned)sizeof(a), &v, &ms) == 3) {
        SHORT s = (SHORT)(v * 32767.0f);
        if (!_stricmp(a, "LX")) p.sThumbLX = s;
        else if (!_stricmp(a, "LY")) p.sThumbLY = s;
        else if (!_stricmp(a, "RX")) p.sThumbRX = s;
        else if (!_stricmp(a, "RY")) p.sThumbRY = s;
        SetPad(p, true);
        Sleep((DWORD)ms);
        SetPad({}, true);
    } else if (sscanf_s(line, "pad %f %f %f %f %f %f %f", &s[0], &s[1], &s[2], &s[3], &s[4], &s[5], &ms) == 7) {
        // pad <LX> <LY> <RX> <RY> <LT> <RT> <ms>: sticks and triggers at once (e.g. steering while boosting)
        p.sThumbLX = (SHORT)(s[0] * 32767.0f); p.sThumbLY = (SHORT)(s[1] * 32767.0f);
        p.sThumbRX = (SHORT)(s[2] * 32767.0f); p.sThumbRY = (SHORT)(s[3] * 32767.0f);
        p.bLeftTrigger = (BYTE)(s[4] * 255.0f); p.bRightTrigger = (BYTE)(s[5] * 255.0f);
        SetPad(p, true);
        Sleep((DWORD)ms);
        SetPad({}, true);
    } else if (sscanf_s(line, "watchcam %f", &v) == 1) {   // reads/writes of the camera cache POV, with stacks
        void* GameCameraActor();
        uint8_t* cam = (uint8_t*)GameCameraActor();
        if (cam) { g_watchSeq = 0; SetWatch((uintptr_t)(cam + 0x300 + (int)v), true); }   // v = byte offset in the POV
    } else if (sscanf_s(line, "watchseqw %31s %f", a, (unsigned)sizeof(a), &v) == 2) {   // before "watchseq": same prefix
        g_watchSeq = (long)v;
        SetWatch((uintptr_t)strtoul(a, nullptr, 16), false);
    } else if (sscanf_s(line, "watchseq %31s %f", a, (unsigned)sizeof(a), &v) == 2) {
        g_watchSeq = (long)v;
        SetWatch((uintptr_t)strtoul(a, nullptr, 16), true);
    } else if (sscanf_s(line, "dump %31s %f", a, (unsigned)sizeof(a), &v) == 2) {   // dump <hex addr> <bytes>
        const uint8_t* base = (const uint8_t*)strtoul(a, nullptr, 16);
        __try {
            for (int o = 0; o < (int)v; o += 16) {
                const uint32_t* d = (const uint32_t*)(base + o);
                Log("dump %p+%03X: %08X %08X %08X %08X | %g %g %g %g", base, o, d[0], d[1], d[2], d[3],
                    *(const float*)&d[0], *(const float*)&d[1], *(const float*)&d[2], *(const float*)&d[3]);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { Log("dump: bad address"); }
    } else if (sscanf_s(line, "pokef %31s %f", a, (unsigned)sizeof(a), &v) == 2) {   // pokef <hex addr> <float>
        __try { *(float*)strtoul(a, nullptr, 16) = v; Log("debug: pokef %s = %g", a, v); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("debug: pokef: bad address"); }
    } else if (sscanf_s(line, "objscan %31s %f", a, (unsigned)sizeof(a), &v) == 2) {   // objscan <hex addr> <bytes>
        // the fields of an object that point at other objects (names from GNames, the FName at +4)
        const uint8_t* base = (const uint8_t*)strtoul(a, nullptr, 16);
        uint8_t** names = *(uint8_t***)0x12B2F0F4;
        int nameCount = *(int*)(0x12B2F0F4 + 4);
        for (int o = 0; o < (int)v; o += 4) {
            __try {
                const uint8_t* p = *(const uint8_t* const*)(base + o);
                if ((uintptr_t)p < 0x10000) continue;
                unsigned fn = *(const unsigned*)(p + 4);
                int i = (int)(fn >> 14);
                if (!names || i <= 0 || i >= nameCount || !names[i]) continue;
                Log("objscan %p+%03X -> %p %ls", base, o, p, (const wchar_t*)(names[i] + 8));
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    } else if (sscanf_s(line, "watchrw %31s", a, (unsigned)sizeof(a)) == 1) {
        SetWatch((uintptr_t)strtoul(a, nullptr, 16), true);
    } else if (sscanf_s(line, "watch %31s", a, (unsigned)sizeof(a)) == 1) {
        SetWatch((uintptr_t)strtoul(a, nullptr, 16));
    } else if (!strncmp(line, "unwatch", 7)) {
        SetWatch(0);
    } else if (sscanf_s(line, "countexec %31s", a, (unsigned)sizeof(a)) == 1) {
        void* target = (void*)strtoul(a, nullptr, 16);
        g_countExec = 0;
        bool ok = MH_CreateHook(target, (void*)CountStub, &g_countTramp) == MH_OK && MH_EnableHook(target) == MH_OK;
        Log("debug: countexec %p %s", target, ok ? "installed" : "FAILED");
    } else if (!strncmp(line, "countlog", 8)) {
        Log("debug: countexec hits %ld", g_countExec);
    } else if (sscanf_s(line, "wheel %f %f", &v, &ms) == 2) {
        void VRDebugWheel(int hand, int hot);
        VRDebugWheel((int)v, (int)ms);
    } else if (sscanf_s(line, "calibok %f", &v) == 1) {
        void VRDebugCalib(int, int);
        VRDebugCalib(1, (int)v);
    } else if (!strncmp(line, "calib", 5)) {
        void VRDebugCalib(int, int);
        VRDebugCalib(0, 0);
    } else if (sscanf_s(line, "testview %f", &v) == 1) {
        extern float g_debugTestView;
        g_debugTestView = v;
    } else if (sscanf_s(line, "rightvs %f", &v) == 1) {
        extern int g_debugRightVS;
        g_debugRightVS = (int)v;
    } else if (sscanf_s(line, "eyeswap %f", &v) == 1) {
        extern int g_debugEyeSwap;
        g_debugEyeSwap = (int)v;
    } else if (sscanf_s(line, "windoweye %f", &v) == 1) {
        extern int g_debugWindowEye;
        g_debugWindowEye = (int)v;
    } else if (sscanf_s(line, "pass2frame %f", &v) == 1) {
        extern int g_debugPass2Frame;
        g_debugPass2Frame = (int)v;
    } else if (sscanf_s(line, "pairocc %f", &v) == 1) {
        extern int g_debugPairOcclusion;
        g_debugPairOcclusion = (int)v;
    } else if (sscanf_s(line, "eyebright %f", &v) == 1) {
        extern volatile long g_eyeBrightFrames;
        g_eyeBrightFrames = (long)v;
    } else if (sscanf_s(line, "skipdraw %f", &v) == 1) {
        extern int g_debugSkipDraw;
        g_debugSkipDraw = (int)v;
    } else if (sscanf_s(line, "rightyaw %f", &v) == 1) {
        extern float g_debugRightYaw;
        g_debugRightYaw = v;
    } else if (!strncmp(line, "pawns", 5)) {   // the other robots (needs a melee weapon in hand)
        MeleeDebug(1, nullptr);
    } else if (!strncmp(line, "bringpawn", 9)) {   // bringpawn <actor hex>: in front of the hand
        MeleeDebug(2, (void*)strtoul(line + 9, nullptr, 16));
    } else if (!strncmp(line, "meleetarget", 11)) {
        void* t;
        float h;
        MeleeDebugTarget(&t, &h);
        Log("debug: melee target %p health %.1f", t, h);
    } else if (sscanf_s(line, "melee %f", &v) == 1) {
        WeaponsSetMeleeHand((int)v);
    } else if (sscanf_s(line, "weapon %f", &v) == 1) {
        WeaponsSelect((int)v);
    } else if (!strncmp(line, "foregrip", 8)) {
        float f[3];
        if (IKForegrip(f)) Log("debug: foregrip right %.3f up %.3f forward %.3f m", f[0], f[1], -f[2]);
        else Log("debug: no foregrip");
    } else if (!strncmp(line, "pickups", 7)) {
        PullDebug(1);
    } else if (!strncmp(line, "pullaim", 7)) {
        PullDebug(4);
    } else if (!strncmp(line, "pullflick", 9)) {
        PullDebug(5);
    } else if (!strncmp(line, "pullnear2", 9)) {   // ... and re-place the pawn when it lands
        PullDebug(3);
    } else if (!strncmp(line, "grenade ", 8)) {   // grenade grab | arm | release [vx vy vz (m/s, world)]
        void GrenadeDebugVelocity(const float v[3]);
        float gv[3];
        if (!strncmp(line + 8, "grab", 4)) GrenadeGrab(0);
        else if (!strncmp(line + 8, "arm", 3)) GrenadeArm();
        else if (!strncmp(line + 8, "release", 7)) {
            if (sscanf_s(line + 15, "%f %f %f", &gv[0], &gv[1], &gv[2]) == 3) GrenadeDebugVelocity(gv);
            GrenadeRelease();
        }
    } else if (!strncmp(line, "pullnear", 8)) {   // pullnear [actor hex]: the nearest pickup, or that one
        PullDebug(2, (void*)strtoul(line + 8, nullptr, 16));
    } else if (!strncmp(line, "weapons", 7)) {
        WeaponList wl;
        WeaponsGetList(wl);
        for (int i = 0; i < wl.count; i++) Log("debug: weapon %d %ls%s", i, wl.name[i], i == wl.current ? " (in hand)" : "");
        Log("debug: melee hand %d held %d", WeaponsMeleeHand(), WeaponsMeleeHeld());
    } else if (!strncmp(line, "turretdbg", 9)) {   // turretdbg 1|2|0: on a turret, log its aim once a second (2: its traces)
        extern int g_debugTurret;
        g_debugTurret = atoi(line + 9);
    } else if (!strncmp(line, "gfxget ", 7) || !strncmp(line, "gfxset ", 7) || !strncmp(line, "gfxcall ", 8) ||
               !strncmp(line, "gfxkids ", 8)) {   // HUD movie variables, methods and the clips in a clip
        GfxQueue(line);
    } else if (!strncmp(line, "hudsave", 7)) {   // the HUD layers of the next frame as hud_*.bmp
        extern volatile long g_hudSave;
        g_hudSave = 1;
    } else if (!strncmp(line, "gfxprobe", 8)) {   // which GFxMovieView methods the game calls (2 s)
        void GfxProbe();
        GfxProbe();
    } else if (!strncmp(line, "turretfx", 8)) {
        void WeaponsDebugTurretFx();
        WeaponsDebugTurretFx();
    } else if (!strncmp(line, "armdump", 7)) {   // the right arm's bones as the game animates them (once)
        void IKArmDump();
        IKArmDump();
    } else if (!strncmp(line, "armbind", 7)) {   // armbind 1|0: the empty hand's arm model in the bind pose or as the game poses it
        void IKArmBindPose(bool on);
        IKArmBindPose(atoi(line + 7) != 0);
    } else if (!strncmp(line, "putaway", 7)) {   // putaway 1|0: the right hand empty (the gun put away) or back
        void VRDebugPutAway(int on);
        VRDebugPutAway(atoi(line + 7));
    } else if (!strncmp(line, "ability", 7)) {
        void VRDebugAbility();
        VRDebugAbility();
    } else if (!strncmp(line, "viewoffset", 10)) {
        extern float g_debugViewOffset[4];
        float o[4] = {};
        sscanf_s(line + 10, "%f %f %f %f", &o[0], &o[1], &o[2], &o[3]);
        memcpy(g_debugViewOffset, o, sizeof(o));
    } else if (sscanf_s(line, "fakehead %f %f", &v, &ms) == 2) {
        extern float g_fakeHeadYaw, g_fakeHeadPitch;
        g_fakeHeadYaw = v;
        g_fakeHeadPitch = ms;
    } else if (sscanf_s(line, "splithook %31s", a, (unsigned)sizeof(a)) == 1) {
        void GameInstallSplitHook(uintptr_t fn);
        GameInstallSplitHook((uintptr_t)strtoul(a, nullptr, 16));
    } else if (sscanf_s(line, "splitwait %f", &v) == 1) {
        extern int g_splitWait;
        g_splitWait = (int)v;
    } else if (sscanf_s(line, "splittest %f", &v) == 1) {
        extern int g_splitTest;
        g_splitTest = (int)v;
    } else if (sscanf_s(line, "markcam %f", &v) == 1) {
        g_cfg.logCameraSeq = (int)v;
    } else if (sscanf_s(line, "cammode %f", &v) == 1) {
        g_cfg.cameraMode = (int)v;
    } else if (sscanf_s(line, "eyeheight %f", &v) == 1) {
        g_cfg.eyeHeight = v;
    } else if (sscanf_s(line, "d3dtrace %f", &v) == 1) {
        extern volatile long g_traceFrames;
        g_traceFrames = (long)v;
    } else if (sscanf_s(line, "hudflush %f", &v) == 1) {
        extern int g_hudFlush;
        g_hudFlush = (int)v;
    } else if (sscanf_s(line, "fakehand %f", &v) == 1) {
        extern float g_fakeHandYaw, g_fakeHandPitch;
        g_fakeHandYaw = v;
        g_fakeHandPitch = 0;
        sscanf_s(line, "fakehand %f %f", &v, &g_fakeHandPitch);
    } else if (sscanf_s(line, "probeyaw %f", &v) == 1) {
        extern int g_probeYaw;
        g_probeYaw = (int)v;
    } else if (sscanf_s(line, "wait %f", &ms) == 1) {
        Sleep((DWORD)ms);
    } else if (sscanf_s(line, "%31s", cmd, (unsigned)sizeof(cmd)) == 1 && !_stricmp(cmd, "release")) {
        SetPad({}, false);
    }
    Log("debug: %s", line);
}

static DWORD WINAPI DebugThread(LPVOID)
{
    std::wstring path = ModDir() + L"twfcvr_cmd.txt";
    for (;;) {
        Sleep(200);
        FILE* f = _wfopen(path.c_str(), L"r");
        if (!f) continue;
        std::vector<std::string> lines;
        char buf[256];
        while (fgets(buf, sizeof(buf), f)) {
            buf[strcspn(buf, "\r\n")] = 0;
            if (buf[0]) lines.push_back(buf);
        }
        fclose(f);
        _wremove(path.c_str());
        for (auto& l : lines) RunLine(l.c_str());
        SetPad({}, false);
        Log("debug: script done");
    }
}

// Without a headset (FakeHmd) nobody presses the buttons weapons.cpp asks for: press them here.
static DWORD WINAPI PressRelayThread(LPVOID)
{
    for (;;) {
        Sleep(20);
        if (VRIsRunning()) continue;
        for (int melee = 0; melee < 2; melee++) {
            if (!WeaponsWantPress(melee != 0)) continue;
            XINPUT_GAMEPAD p = {};
            p.wButtons = melee ? XINPUT_GAMEPAD_RIGHT_THUMB : XINPUT_GAMEPAD_Y;
            Log("debug: relay press %s", melee ? "RS" : "Y");
            if (melee) WeaponsStrike(1);
            SetPad(p, true);
            Sleep(100);
            SetPad({}, false);
        }
    }
}

// A crash while testing: where it happened (the game itself just closes).
static LONG WINAPI CrashLogger(EXCEPTION_POINTERS* e)
{
    CONTEXT* c = e->ContextRecord;
    uintptr_t* sp = (uintptr_t*)c->Esp;
    char stack[512];
    int len = 0, k = 0;
    __try {
        for (int i = 0; i < 400 && k < 24; i++)
            if (sp[i] >= 0x10901000 && sp[i] < 0x11948000) { len += sprintf_s(stack + len, sizeof(stack) - len, " %08X", (unsigned)sp[i]); k++; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    stack[len] = 0;
    Log("CRASH: code %08X at EIP %08X (addr %08X) eax=%08X ecx=%08X edx=%08X esi=%08X edi=%08X stack:%s",
        (unsigned)e->ExceptionRecord->ExceptionCode, (unsigned)c->Eip,
        (unsigned)(e->ExceptionRecord->NumberParameters > 1 ? e->ExceptionRecord->ExceptionInformation[1] : 0),
        c->Eax, c->Ecx, c->Edx, c->Esi, c->Edi, stack);
    return EXCEPTION_CONTINUE_SEARCH;
}

// First-chance access violations (the engine catches crashes itself and just quits): the first few are logged.
static LONG CALLBACK FirstChanceLogger(EXCEPTION_POINTERS* e)
{
    static volatile long logged;
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && InterlockedIncrement(&logged) <= 6)
        CrashLogger(e);
    return EXCEPTION_CONTINUE_SEARCH;
}

void DebugStart()
{
    SetUnhandledExceptionFilter(CrashLogger);
    AddVectoredExceptionHandler(0, FirstChanceLogger);
    CloseHandle(CreateThread(nullptr, 0, DebugThread, nullptr, 0, nullptr));
    CloseHandle(CreateThread(nullptr, 0, PressRelayThread, nullptr, 0, nullptr));
}
