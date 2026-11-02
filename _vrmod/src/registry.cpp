// Render resolution for VR: the game reads its resolution from
// HKCU\Software\Activision\TransformersWFC\ScreenResolution = "Width:W Height:H Fullscreen:F".
// We answer that query with the VR resolution and drop writes of it, so the user's normal (flat)
// game keeps its own setting.
#include "common.h"
#include "MinHook.h"
#include <stdio.h>

typedef LSTATUS(WINAPI* PFN_QueryW)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LSTATUS(WINAPI* PFN_QueryA)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LSTATUS(WINAPI* PFN_SetW)(HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD);
typedef LSTATUS(WINAPI* PFN_SetA)(HKEY, LPCSTR, DWORD, DWORD, const BYTE*, DWORD);
static PFN_QueryW g_origQueryW;
static PFN_QueryA g_origQueryA;
static PFN_SetW g_origSetW;
static PFN_SetA g_origSetA;

static bool Enabled() { VRRenderSizeWait(); return g_cfg.renderWidth > 0 && g_cfg.renderHeight > 0; }

static LSTATUS Answer(LPDWORD type, LPBYTE data, LPDWORD cb, const void* str, DWORD bytes)
{
    if (type) *type = REG_SZ;
    if (!cb) return data ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
    if (!data) { *cb = bytes; return ERROR_SUCCESS; }
    if (*cb < bytes) { *cb = bytes; return ERROR_MORE_DATA; }
    memcpy(data, str, bytes);
    *cb = bytes;
    return ERROR_SUCCESS;
}

static LSTATUS WINAPI HookQueryW(HKEY k, LPCWSTR name, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD cb)
{
    if (Enabled() && name && !_wcsicmp(name, L"ScreenResolution")) {
        wchar_t s[96];
        swprintf_s(s, L"Width:%d Height:%d Fullscreen:0", g_cfg.renderWidth, g_cfg.renderHeight);
        static bool logged;
        if (!logged) { Log("registry: ScreenResolution -> %dx%d", g_cfg.renderWidth, g_cfg.renderHeight); logged = true; }
        return Answer(type, data, cb, s, (DWORD)((wcslen(s) + 1) * sizeof(wchar_t)));
    }
    return g_origQueryW(k, name, res, type, data, cb);
}

static LSTATUS WINAPI HookQueryA(HKEY k, LPCSTR name, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD cb)
{
    if (Enabled() && name && !_stricmp(name, "ScreenResolution")) {
        char s[96];
        sprintf_s(s, "Width:%d Height:%d Fullscreen:0", g_cfg.renderWidth, g_cfg.renderHeight);
        static bool logged;
        if (!logged) { Log("registry: ScreenResolution (A) -> %dx%d", g_cfg.renderWidth, g_cfg.renderHeight); logged = true; }
        return Answer(type, data, cb, s, (DWORD)(strlen(s) + 1));
    }
    return g_origQueryA(k, name, res, type, data, cb);
}

static LSTATUS WINAPI HookSetW(HKEY k, LPCWSTR name, DWORD r, DWORD type, const BYTE* data, DWORD cb)
{
    if (Enabled() && name && !_wcsicmp(name, L"ScreenResolution")) return ERROR_SUCCESS;
    return g_origSetW(k, name, r, type, data, cb);
}

static LSTATUS WINAPI HookSetA(HKEY k, LPCSTR name, DWORD r, DWORD type, const BYTE* data, DWORD cb)
{
    if (Enabled() && name && !_stricmp(name, "ScreenResolution")) return ERROR_SUCCESS;
    return g_origSetA(k, name, r, type, data, cb);
}

// Called from DllMain: the game reads the value during startup, before our worker thread would run.
void RegistryInstallHooks()
{
    if (!VRRenderSizeOn()) return;
    MH_Initialize();
    MH_CreateHookApi(L"advapi32", "RegQueryValueExW", (void*)HookQueryW, (void**)&g_origQueryW);
    MH_CreateHookApi(L"advapi32", "RegQueryValueExA", (void*)HookQueryA, (void**)&g_origQueryA);
    MH_CreateHookApi(L"advapi32", "RegSetValueExW", (void*)HookSetW, (void**)&g_origSetW);
    MH_CreateHookApi(L"advapi32", "RegSetValueExA", (void*)HookSetA, (void**)&g_origSetA);
    MH_EnableHook(MH_ALL_HOOKS);
}
