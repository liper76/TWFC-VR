// TWFC VR mod - entry point. Loaded by the game as xinput1_3.dll (proxy).
#include "common.h"
#include <stdio.h>
#include <stdarg.h>

Config g_cfg;
static HMODULE g_self;
static std::mutex g_logMutex;
static FILE* g_log;

std::wstring ModDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_self, path, MAX_PATH);
    std::wstring s(path);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

void Log(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d [%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static float IniFloat(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, float def)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(sec, key, L"", buf, 64, ini.c_str());
    return buf[0] ? (float)_wtof(buf) : def;
}

static void LoadConfig()
{
    std::wstring ini = ModDir() + L"twfcvr.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = _wfopen(ini.c_str(), L"w");
        if (f) {
            fputs("; TWFC VR mod settings\n"
                  "[General]\n"
                  "; Frame rate cap written into the engine (game default 30). 0 = do not touch.\n"
                  "FpsLimit=120\n"
                  "; Cap while the headset is connected. In stereo every game frame renders one eye,\n"
                  "; so the game needs twice the headset refresh rate; the headset paces it.\n"
                  "VRFpsLimit=300\n"
                  "\n[VR]\n"
                  "Enabled=1\n"
                  "; 0 = theater: the game is always shown on a big virtual screen\n"
                  "; 1 = stereo 3D with head tracking in gameplay, virtual screen in menus and movies\n"
                  "Mode=1\n"
                  "; 0 = third person (game camera + your head), 1 = first person (from the robot's eyes)\n"
                  "CameraMode=0\n"
                  "FirstPersonEyeHeight=180\n"
                  "FirstPersonEyeForward=30\n"
                  "; Unreal units per real metre for head movement\n"
                  "WorldScale=100\n"
                  "; Render a wider view than the headset needs (1.0 = exact)\n"
                  "FovScale=1.0\n"
                  "; 1 = keep the stick-controlled camera pitch (0 = head controls pitch)\n"
                  "KeepStickPitch=0\n"
                  "; Virtual screen for menus and movies (metres)\n"
                  "ScreenDistance=3.0\n"
                  "ScreenWidth=4.0\n"
                  "ScreenHeight=0.0\n"
                  "\n[Input]\n"
                  "; Feed VR controllers to the game as an Xbox gamepad\n"
                  "VRControllers=1\n"
                  "StickDeadzone=0.15\n"
                  "; Keep the game active when its window is not in front (needed in VR)\n"
                  "AlwaysFocused=1\n", f);
            fclose(f);
        }
    }
    g_cfg.fpsLimit = IniFloat(ini, L"General", L"FpsLimit", g_cfg.fpsLimit);
    g_cfg.vrFpsLimit = IniFloat(ini, L"General", L"VRFpsLimit", g_cfg.vrFpsLimit);
    g_cfg.vrEnabled = (int)IniFloat(ini, L"VR", L"Enabled", (float)g_cfg.vrEnabled);
    g_cfg.vrMode = (int)IniFloat(ini, L"VR", L"Mode", (float)g_cfg.vrMode);
    g_cfg.worldScale = IniFloat(ini, L"VR", L"WorldScale", g_cfg.worldScale);
    g_cfg.fovScale = IniFloat(ini, L"VR", L"FovScale", g_cfg.fovScale);
    g_cfg.cameraMode = (int)IniFloat(ini, L"VR", L"CameraMode", (float)g_cfg.cameraMode);
    g_cfg.eyeHeight = IniFloat(ini, L"VR", L"FirstPersonEyeHeight", g_cfg.eyeHeight);
    g_cfg.eyeForward = IniFloat(ini, L"VR", L"FirstPersonEyeForward", g_cfg.eyeForward);
    g_cfg.keepStickPitch = (int)IniFloat(ini, L"VR", L"KeepStickPitch", (float)g_cfg.keepStickPitch);
    g_cfg.snapTurnDeg = (int)IniFloat(ini, L"Input", L"SnapTurnDegrees", (float)g_cfg.snapTurnDeg);
    g_cfg.screenDistance = IniFloat(ini, L"VR", L"ScreenDistance", g_cfg.screenDistance);
    g_cfg.screenWidth = IniFloat(ini, L"VR", L"ScreenWidth", g_cfg.screenWidth);
    g_cfg.screenHeight = IniFloat(ini, L"VR", L"ScreenHeight", 0.0f);
    g_cfg.vrControllers = (int)IniFloat(ini, L"Input", L"VRControllers", (float)g_cfg.vrControllers);
    g_cfg.stickDeadzone = IniFloat(ini, L"Input", L"StickDeadzone", g_cfg.stickDeadzone);
    g_cfg.alwaysFocused = (int)IniFloat(ini, L"Input", L"AlwaysFocused", (float)g_cfg.alwaysFocused);
    g_cfg.debugAutomation = (int)IniFloat(ini, L"Debug", L"Automation", 0.0f);
    g_cfg.logFps = (int)IniFloat(ini, L"Debug", L"LogFps", 0.0f);
    g_cfg.probeCamera = (int)IniFloat(ini, L"Debug", L"ProbeCamera", 0.0f);
    g_cfg.fakeHmd = (int)IniFloat(ini, L"Debug", L"FakeHmd", 0.0f);
}

static DWORD WINAPI InitThread(LPVOID)
{
    LoadConfig();
    Log("config: fps=%.1f vr=%d mode=%d dist=%.2f width=%.2f controllers=%d", g_cfg.fpsLimit, g_cfg.vrEnabled,
        g_cfg.vrMode, g_cfg.screenDistance, g_cfg.screenWidth, g_cfg.vrControllers);
    if (!GameIsSupportedBuild()) {
        Log("unsupported TWFC.exe build - game patches disabled");
    } else {
        GameStartPatchThread();
    }
    FocusStart();
    if (g_cfg.debugAutomation) DebugStart();
    if (g_cfg.vrEnabled) VRStart();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        g_log = _wfopen((ModDir() + L"twfcvr.log").c_str(), L"w");
        Log("TWFC VR mod loaded (build %s %s)", __DATE__, __TIME__);
        CloseHandle(CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr));
    }
    return TRUE;
}

