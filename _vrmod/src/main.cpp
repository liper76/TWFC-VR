// TWFC VR mod - entry point. Loaded by the game as xinput1_3.dll (proxy).
#include "common.h"
#include "default_ini.h"
#include <stdio.h>
#include <stdarg.h>
#include <share.h>

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
            fputs(kDefaultIni, f);
            fclose(f);
        }
    }
    g_cfg.fpsLimit = IniFloat(ini, L"General", L"FpsLimit", g_cfg.fpsLimit);
    g_cfg.vrFpsLimit = IniFloat(ini, L"General", L"VRFpsLimit", g_cfg.vrFpsLimit);
    g_cfg.texturePoolMB = IniFloat(ini, L"General", L"TexturePoolMB", g_cfg.texturePoolMB);
    g_cfg.vrEnabled = (int)IniFloat(ini, L"VR", L"Enabled", (float)g_cfg.vrEnabled);
    g_cfg.vrMode = (int)IniFloat(ini, L"VR", L"Mode", (float)g_cfg.vrMode);
    g_cfg.worldScale = IniFloat(ini, L"VR", L"WorldScale", g_cfg.worldScale);
    g_cfg.fovScale = IniFloat(ini, L"VR", L"FovScale", g_cfg.fovScale);
    g_cfg.levelView = (int)IniFloat(ini, L"VR", L"LevelView", (float)g_cfg.levelView);
    g_cfg.renderWidth = (int)IniFloat(ini, L"VR", L"RenderWidth", (float)g_cfg.renderWidth);
    g_cfg.renderHeight = (int)IniFloat(ini, L"VR", L"RenderHeight", (float)g_cfg.renderHeight);
    g_cfg.renderScale = IniFloat(ini, L"VR", L"RenderScale", g_cfg.renderScale);
    g_cfg.maxRenderSize = (int)IniFloat(ini, L"VR", L"MaxRenderSize", (float)g_cfg.maxRenderSize);
    g_cfg.cameraMode = (int)IniFloat(ini, L"VR", L"CameraMode", (float)g_cfg.cameraMode);
    g_cfg.aimMode = (int)IniFloat(ini, L"VR", L"AimMode", (float)g_cfg.aimMode);
    g_cfg.vrHud = (int)IniFloat(ini, L"HUD", L"FloatingHud", (float)g_cfg.vrHud);
    g_cfg.hudDistance = IniFloat(ini, L"HUD", L"Distance", g_cfg.hudDistance);
    g_cfg.hudWidth = IniFloat(ini, L"HUD", L"Width", g_cfg.hudWidth);
    g_cfg.hudFollowSpeed = IniFloat(ini, L"HUD", L"FollowSpeed", g_cfg.hudFollowSpeed);
    g_cfg.hudMinimal = (int)IniFloat(ini, L"HUD", L"Minimal", (float)g_cfg.hudMinimal);
    g_cfg.hudPrompts = (int)IniFloat(ini, L"HUD", L"Prompts", (float)g_cfg.hudPrompts);
    g_cfg.reticle = (int)IniFloat(ini, L"VR", L"Reticle", (float)g_cfg.reticle);
    g_cfg.reticleDistance = IniFloat(ini, L"VR", L"ReticleDistance", g_cfg.reticleDistance);
    g_cfg.syncEyeTime = (int)IniFloat(ini, L"VR", L"SyncEyeTime", (float)g_cfg.syncEyeTime);
    g_cfg.stereoPerFrame = (int)IniFloat(ini, L"VR", L"StereoPerFrame", (float)g_cfg.stereoPerFrame);
    g_cfg.singleThreadRender = (int)IniFloat(ini, L"VR", L"SingleThreadRender", (float)g_cfg.singleThreadRender);
    g_cfg.syncEyeTimeVehicle = (int)IniFloat(ini, L"VR", L"SyncEyeTimeVehicle", (float)g_cfg.syncEyeTimeVehicle);
    g_cfg.autoScale = 0;   // the T-pose scale is off (the user asked: it misjudged); WorldScale is set by hand
    g_cfg.userArmSpan = IniFloat(ini, L"VR", L"UserArmSpan", g_cfg.userArmSpan);
    g_cfg.userEyeHeight = IniFloat(ini, L"VR", L"UserEyeHeight", g_cfg.userEyeHeight);
    g_cfg.heldFrameTime =IniFloat(ini, L"VR", L"HeldFrameTime", (float)g_cfg.heldFrameTime);
    g_cfg.handIK = (int)IniFloat(ini, L"VR", L"HandIK", (float)g_cfg.handIK);
    g_cfg.meleeGesture = (int)IniFloat(ini, L"Input", L"MeleeGesture", (float)g_cfg.meleeGesture);
    g_cfg.meleeSpeed = IniFloat(ini, L"Input", L"MeleeSpeed", g_cfg.meleeSpeed);
    g_cfg.meleeContact = (int)IniFloat(ini, L"Input", L"MeleeContact", (float)g_cfg.meleeContact);
    g_cfg.meleeReach = IniFloat(ini, L"Input", L"MeleeReach", g_cfg.meleeReach);
    g_cfg.meleeHitRadius = IniFloat(ini, L"Input", L"MeleeHitRadius", g_cfg.meleeHitRadius);
    g_cfg.throwGesture = (int)IniFloat(ini, L"Input", L"ThrowGesture", (float)g_cfg.throwGesture);
    g_cfg.throwSpeed = IniFloat(ini, L"Input", L"ThrowSpeed", g_cfg.throwSpeed);
    g_cfg.grenadeGrab = (int)IniFloat(ini, L"Input", L"GrenadeGrab", (float)g_cfg.grenadeGrab);
    g_cfg.grenadeThrowScale = IniFloat(ini, L"Input", L"GrenadeThrowScale", g_cfg.grenadeThrowScale);
    g_cfg.grenadeGravity = IniFloat(ini, L"Input", L"GrenadeGravity", g_cfg.grenadeGravity);
    g_cfg.grenadeBelt = (int)IniFloat(ini, L"Input", L"GrenadeBelt", (float)g_cfg.grenadeBelt);
    g_cfg.grenadeScale = IniFloat(ini, L"Input", L"GrenadeScale", g_cfg.grenadeScale);
    g_cfg.cutscenes3D = (int)IniFloat(ini, L"VR", L"Cutscenes3D", (float)g_cfg.cutscenes3D);
    g_cfg.reticleSize = IniFloat(ini, L"VR", L"ReticleSize", g_cfg.reticleSize);
    g_cfg.reticleStyle = (int)IniFloat(ini, L"VR", L"ReticleStyle", (float)g_cfg.reticleStyle);
    g_cfg.hudMarkersInEyes = (int)IniFloat(ini, L"HUD", L"MarkersInEyes", (float)g_cfg.hudMarkersInEyes);
    {
        wchar_t b[96];
        GetPrivateProfileStringW(L"Input", L"BeltOffset", L"", b, 96, ini.c_str());
        float o[3];
        if (swscanf_s(b, L"%f,%f,%f", &o[0], &o[1], &o[2]) == 3) memcpy(g_cfg.beltOffset, o, sizeof(o));
    }
    g_cfg.fingerPoses = (int)IniFloat(ini, L"VR", L"FingerPoses", (float)g_cfg.fingerPoses);
    g_cfg.fingerPalmSign = (int)IniFloat(ini, L"VR", L"FingerPalmSign", (float)g_cfg.fingerPalmSign);
    g_cfg.pull = (int)IniFloat(ini, L"Input", L"Pull", (float)g_cfg.pull);
    g_cfg.pullRange = IniFloat(ini, L"Input", L"PullRange", g_cfg.pullRange);
    g_cfg.pullConeDeg = IniFloat(ini, L"Input", L"PullCone", g_cfg.pullConeDeg);
    g_cfg.pullSpeed = IniFloat(ini, L"Input", L"PullSpeed", g_cfg.pullSpeed);
    g_cfg.pullAutoTake = (int)IniFloat(ini, L"Input", L"PullAutoTake", (float)g_cfg.pullAutoTake);
    g_cfg.pullTrace = (int)IniFloat(ini, L"Input", L"PullTrace", (float)g_cfg.pullTrace);
    g_cfg.pullMark = (int)IniFloat(ini, L"Input", L"PullMark", (float)g_cfg.pullMark);
    g_cfg.gripUse = (int)IniFloat(ini, L"Input", L"GripUse", (float)g_cfg.gripUse);
    g_cfg.twoHanded = (int)IniFloat(ini, L"Input", L"TwoHanded", (float)g_cfg.twoHanded);
    g_cfg.twoHandReach = IniFloat(ini, L"Input", L"TwoHandReach", g_cfg.twoHandReach);
    g_cfg.twoHandRadius = IniFloat(ini, L"Input", L"TwoHandRadius", g_cfg.twoHandRadius);
    g_cfg.gunstock = (int)IniFloat(ini, L"Input", L"Gunstock", (float)g_cfg.gunstock);
    g_cfg.weaponSmoothing = IniFloat(ini, L"Input", L"WeaponSmoothing", g_cfg.weaponSmoothing);
    g_cfg.elbowInGun[0] = IniFloat(ini, L"Input", L"ElbowInGunForward", g_cfg.elbowInGun[0]);   // (temporary)
    g_cfg.elbowInGun[1] = IniFloat(ini, L"Input", L"ElbowInGunUp", g_cfg.elbowInGun[1]);
    g_cfg.elbowInGun[2] = IniFloat(ini, L"Input", L"ElbowInGunOut", g_cfg.elbowInGun[2]);
    {   // (optional: StockShoulder, StockButt, StockSight - x,y,z metres)
        const wchar_t* keys[3] = { L"StockShoulder", L"StockButt", L"StockSight" };
        float* dst[3] = { g_cfg.stockShoulder, g_cfg.stockButt, g_cfg.stockSight };
        for (int k = 0; k < 3; k++) {
            wchar_t b[96];
            GetPrivateProfileStringW(L"Input", keys[k], L"", b, 96, ini.c_str());
            float o[3];
            if (swscanf_s(b, L"%f,%f,%f", &o[0], &o[1], &o[2]) == 3) memcpy(dst[k], o, sizeof(o));
        }
    }
    g_cfg.physicalAim = (int)IniFloat(ini, L"Input", L"PhysicalAim", (float)g_cfg.physicalAim);
    g_cfg.gripAbilities = (int)IniFloat(ini, L"Input", L"GripAbilities", (float)g_cfg.gripAbilities);
    g_cfg.wheelSize = IniFloat(ini, L"Input", L"WheelSize", g_cfg.wheelSize);
    g_cfg.handOffset[0] = IniFloat(ini, L"VR", L"HandOffsetForward", 0.0f);
    g_cfg.handOffset[1] = IniFloat(ini, L"VR", L"HandOffsetRight", 0.0f);
    g_cfg.handOffset[2] = IniFloat(ini, L"VR", L"HandOffsetUp", 0.0f);
    {
        // GunGrip / Foregrip: set in the headset (hold the menu button), comma separated numbers
        wchar_t buf[200] = {};
        float v[7];
        GetPrivateProfileStringW(L"VR", L"GunGrip", L"", buf, 200, ini.c_str());
        if (swscanf_s(buf, L"%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) == 7) memcpy(g_cfg.gunGrip, v, sizeof(v));
        GetPrivateProfileStringW(L"VR", L"EmptyGripR", L"", buf, 200, ini.c_str());   // the empty right hand (setup)
        if (swscanf_s(buf, L"%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) == 7) memcpy(g_cfg.emptyGripR, v, sizeof(v));
        GetPrivateProfileStringW(L"VR", L"Foregrip", L"", buf, 200, ini.c_str());
        g_cfg.foregripSet = swscanf_s(buf, L"%f,%f,%f", &v[0], &v[1], &v[2]) == 3 &&
                            v[0] * v[0] + v[1] * v[1] + v[2] * v[2] > 0.05f * 0.05f;   // not on the right hand itself
        if (g_cfg.foregripSet) memcpy(g_cfg.foregrip, v, 3 * sizeof(float));
    }
    g_cfg.scope = (int)IniFloat(ini, L"Input", L"Scope", (float)g_cfg.scope);
    g_cfg.scopeFov = IniFloat(ini, L"Input", L"ScopeFov", g_cfg.scopeFov);
    g_cfg.scopeSize = IniFloat(ini, L"Input", L"ScopeSize", g_cfg.scopeSize);
    g_cfg.scopeForward = IniFloat(ini, L"Input", L"ScopeForward", g_cfg.scopeForward);
    g_cfg.scopeUp = IniFloat(ini, L"Input", L"ScopeUp", g_cfg.scopeUp);
    {
        wchar_t def[128];
        wcscpy_s(def, g_cfg.scopeWeapons);
        GetPrivateProfileStringW(L"Input", L"ScopeWeapons", def, g_cfg.scopeWeapons, 128, ini.c_str());
    }
    g_cfg.abilityYield = IniFloat(ini, L"VR", L"AbilityAnimation", g_cfg.abilityYield);
    g_cfg.noNearFade = IniFloat(ini, L"VR", L"NoNearFade", g_cfg.noNearFade);
    g_cfg.nearClip = IniFloat(ini, L"VR", L"NearClip", g_cfg.nearClip);
    g_cfg.showBody = (int)IniFloat(ini, L"VR", L"ShowBody", (float)g_cfg.showBody);
    g_cfg.limbThickness = IniFloat(ini, L"VR", L"LimbThickness", g_cfg.limbThickness);
    g_cfg.legThickness = IniFloat(ini, L"VR", L"LegThickness", g_cfg.legThickness);
    g_cfg.armSize = IniFloat(ini, L"VR", L"ArmSize", g_cfg.armSize);
    g_cfg.handScale = IniFloat(ini, L"VR", L"HandScale", g_cfg.handScale);
    g_cfg.weaponScale = IniFloat(ini, L"VR", L"WeaponScale", g_cfg.weaponScale);
    {
        // the game sizes weapons per character, so the size is the character's own once set (the grips stay shared)
        std::wstring who = IKCharacter();
        if (!who.empty()) g_cfg.weaponScale = IniFloat(ini, (L"Character." + who).c_str(), L"WeaponScale", g_cfg.weaponScale);
    }
    g_cfg.muzzleFlashScale = IniFloat(ini, L"VR", L"MuzzleFlashScale", g_cfg.muzzleFlashScale);
    {
        // the mod's own text (VR menu, weapon wheel): en / ru, or auto = Russian for a Russian, Ukrainian or
        // Belarusian Windows, English otherwise
        wchar_t b[16] = {};
        GetPrivateProfileStringW(L"General", L"Language", L"auto", b, 16, ini.c_str());
        LANGID ui = PRIMARYLANGID(GetUserDefaultUILanguage());
        bool ru = !_wcsicmp(b, L"ru") || (_wcsicmp(b, L"en") && (ui == LANG_RUSSIAN || ui == LANG_UKRAINIAN || ui == LANG_BELARUSIAN));
        g_cfg.english = ru ? 0 : 1;
    }
    g_cfg.bodyFollowHead = (int)IniFloat(ini, L"VR", L"BodyFollowHead", (float)g_cfg.bodyFollowHead);
    g_cfg.vehicleCamera = (int)IniFloat(ini, L"VR", L"VehicleCamera", (float)g_cfg.vehicleCamera);
    g_cfg.chaseDistance = IniFloat(ini, L"VR", L"ChaseDistance", g_cfg.chaseDistance);
    g_cfg.chaseHeight = IniFloat(ini, L"VR", L"ChaseHeight", g_cfg.chaseHeight);
    g_cfg.vehicleHeadAim = (int)IniFloat(ini, L"VR", L"VehicleHeadAim", (float)g_cfg.vehicleHeadAim);
    g_cfg.vehicleEyeHeight = IniFloat(ini, L"VR", L"VehicleEyeHeight", g_cfg.vehicleEyeHeight);
    g_cfg.vehicleEyeForward = IniFloat(ini, L"VR", L"VehicleEyeForward", g_cfg.vehicleEyeForward);
    g_cfg.moveByHead = (int)IniFloat(ini, L"Input", L"MoveByHead", (float)g_cfg.moveByHead);
    g_cfg.eyeHeight = IniFloat(ini, L"VR", L"FirstPersonEyeHeight", g_cfg.eyeHeight);
    g_cfg.eyeForward = IniFloat(ini, L"VR", L"FirstPersonEyeForward", g_cfg.eyeForward);
    g_cfg.turretEyeUp = IniFloat(ini, L"VR", L"TurretEyeUp", g_cfg.turretEyeUp);
    g_cfg.keepStickPitch = (int)IniFloat(ini, L"VR", L"KeepStickPitch", (float)g_cfg.keepStickPitch);
    g_cfg.snapTurnDeg = (int)IniFloat(ini, L"Input", L"SnapTurnDegrees", (float)g_cfg.snapTurnDeg);
    g_cfg.smoothTurnDegPerSec = IniFloat(ini, L"Input", L"SmoothTurnSpeed", g_cfg.smoothTurnDegPerSec);
    g_cfg.ownTurning = (int)IniFloat(ini, L"Input", L"VRTurning", (float)g_cfg.ownTurning);
    g_cfg.screenDistance = IniFloat(ini, L"VR", L"ScreenDistance", g_cfg.screenDistance);
    g_cfg.screenWidth = IniFloat(ini, L"VR", L"ScreenWidth", g_cfg.screenWidth);
    g_cfg.screenHeight = IniFloat(ini, L"VR", L"ScreenHeight", 0.0f);
    g_cfg.vrControllers = (int)IniFloat(ini, L"Input", L"VRControllers", (float)g_cfg.vrControllers);
    g_cfg.stickDeadzone = IniFloat(ini, L"Input", L"StickDeadzone", g_cfg.stickDeadzone);
    g_cfg.alwaysFocused = (int)IniFloat(ini, L"Input", L"AlwaysFocused", (float)g_cfg.alwaysFocused);
    g_cfg.blockMouseKeyboard = (int)IniFloat(ini, L"Input", L"BlockMouseKeyboard", (float)g_cfg.blockMouseKeyboard);
    g_cfg.buttonHints = (int)IniFloat(ini, L"HUD", L"ButtonHints", (float)g_cfg.buttonHints);
    g_cfg.debugAutomation = (int)IniFloat(ini, L"Debug", L"Automation", 0.0f);
    g_cfg.logFps = (int)IniFloat(ini, L"Debug", L"LogFps", 0.0f);
    g_cfg.probeCamera = (int)IniFloat(ini, L"Debug", L"ProbeCamera", 0.0f);
    g_cfg.fakeHmd = (int)IniFloat(ini, L"Debug", L"FakeHmd", 0.0f);
}

void ConfigReloadIfChanged()
{
    static FILETIME last;
    static std::wstring lastWho;   // another character: its own weapon size
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW((ModDir() + L"twfcvr.ini").c_str(), GetFileExInfoStandard, &a)) return;
    std::wstring who = IKCharacter();
    if (!last.dwLowDateTime && !last.dwHighDateTime) { last = a.ftLastWriteTime; lastWho = who; return; }
    if (CompareFileTime(&a.ftLastWriteTime, &last) == 0 && who == lastWho) return;
    last = a.ftLastWriteTime;
    lastWho = who;
    // Startup-only values (render size) keep their original effect; everything else applies live.
    int rw = g_cfg.renderWidth, rh = g_cfg.renderHeight;
    LoadConfig();
    g_cfg.renderWidth = rw;
    g_cfg.renderHeight = rh;
    Log("config reloaded: worldScale %.0f eyeHeight %.0f camera %d aim %d snap %d, weapon size %.2f (%ls)", g_cfg.worldScale,
        g_cfg.eyeHeight, g_cfg.cameraMode, g_cfg.aimMode, g_cfg.snapTurnDeg, g_cfg.weaponScale, who.empty() ? L"shared" : who.c_str());
}

static DWORD WINAPI InitThread(LPVOID)
{
    Log("config: fps=%.1f vr=%d mode=%d stereoPerFrame=%d singleThread=%d dist=%.2f width=%.2f controllers=%d",
        g_cfg.fpsLimit, g_cfg.vrEnabled, g_cfg.vrMode, g_cfg.stereoPerFrame, g_cfg.singleThreadRender,
        g_cfg.screenDistance, g_cfg.screenWidth, g_cfg.vrControllers);
    if (!GameIsSupportedBuild()) {
        Log("unsupported TWFC.exe build - game patches disabled");
    } else {
        GameStartPatchThread();
        IKInstall();
        WeaponsInstall();
        InputBlockInstall();
        HintsInstall();
        GfxInstall();
        GameInstallMovieHook();
    }
    FocusStart();
    if (g_cfg.debugAutomation) DebugStart();
    if (g_cfg.vrEnabled) VRStart();
    return 0;
}

static void StartGamePatches()
{
    GameEarlyPatches();   // (before the engine starts its render thread)
    CloseHandle(CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr));
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        // Deny writers so a second game instance cannot clobber this log; it gets its own file instead.
        g_log = _wfsopen((ModDir() + L"twfcvr.log").c_str(), L"w", _SH_DENYWR);
        if (!g_log) g_log = _wfsopen((ModDir() + L"twfcvr_2.log").c_str(), L"w", _SH_DENYWR);
        Log("TWFC VR mod loaded (build %s %s)", __DATE__, __TIME__);
        CrashTraceStart();
        SteamCopyStart();   // (before the Steam DRM wrapper runs)
        // The resolution override must be in place before the game reads its settings at startup.
        LoadConfig();
        VRRenderSizeStart();  // (before the game reads the size)
        RegistryInstallHooks();
        D3D9ModesInstall();
        WindowEarlyInit();
        // The game's code: patched now, or (the Steam version's DRM wrapper) once the wrapper has decrypted it.
        if (GameCodeEncrypted()) GameStartWhenDecrypted(StartGamePatches);
        else StartGamePatches();
    }
    return TRUE;
}

