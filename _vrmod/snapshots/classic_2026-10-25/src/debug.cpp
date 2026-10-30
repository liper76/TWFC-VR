// Debug automation: commands written to twfcvr_cmd.txt (next to the DLL) drive a virtual gamepad.
// Lines: "press <A|B|X|Y|START|BACK|LB|RB|LS|RS|UP|DOWN|LEFT|RIGHT|LT|RT> <ms>", "stick <LX|LY|RX|RY> <-1..1> <ms>", "wait <ms>"
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <vector>

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
        char stack[256];
        int k = 0, len = 0;
        for (int i = 0; i < 64 && k < 8; i++) {
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

static void RunLine(const char* line)
{
    char cmd[32] = {}, a[32] = {};
    float v = 0, ms = 0;
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
    } else if (sscanf_s(line, "watchseq %31s %f", a, (unsigned)sizeof(a), &v) == 2) {
        g_watchSeq = (long)v;
        SetWatch((uintptr_t)strtoul(a, nullptr, 16), true);
    } else if (sscanf_s(line, "watchrw %31s", a, (unsigned)sizeof(a)) == 1) {
        SetWatch((uintptr_t)strtoul(a, nullptr, 16), true);
    } else if (sscanf_s(line, "watch %31s", a, (unsigned)sizeof(a)) == 1) {
        SetWatch((uintptr_t)strtoul(a, nullptr, 16));
    } else if (!strncmp(line, "unwatch", 7)) {
        SetWatch(0);
    } else if (sscanf_s(line, "fakehead %f %f", &v, &ms) == 2) {
        extern float g_fakeHeadYaw, g_fakeHeadPitch;
        g_fakeHeadYaw = v;
        g_fakeHeadPitch = ms;
    } else if (sscanf_s(line, "splithook %31s", a, (unsigned)sizeof(a)) == 1) {
        void GameInstallSplitHook(uintptr_t fn);
        GameInstallSplitHook((uintptr_t)strtoul(a, nullptr, 16));
    } else if (sscanf_s(line, "splittest %f", &v) == 1) {
        extern int g_splitTest;
        g_splitTest = (int)v;
    } else if (sscanf_s(line, "markcam %f", &v) == 1) {
        g_cfg.logCameraSeq = (int)v;
    } else if (sscanf_s(line, "cammode %f", &v) == 1) {
        g_cfg.cameraMode = (int)v;
    } else if (sscanf_s(line, "eyeheight %f", &v) == 1) {
        g_cfg.eyeHeight = v;
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

void DebugStart()
{
    CloseHandle(CreateThread(nullptr, 0, DebugThread, nullptr, 0, nullptr));
}
