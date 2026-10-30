// xinput1_3.dll proxy: forwards to the system XInput and merges in VR controller input.
#include "common.h"
#include <math.h>

typedef DWORD(WINAPI* PFN_GetState)(DWORD, XINPUT_STATE*);
typedef DWORD(WINAPI* PFN_SetState)(DWORD, XINPUT_VIBRATION*);
typedef DWORD(WINAPI* PFN_GetCaps)(DWORD, DWORD, XINPUT_CAPABILITIES*);
typedef void(WINAPI* PFN_Enable)(BOOL);
typedef DWORD(WINAPI* PFN_GetGuids)(DWORD, GUID*, GUID*);
typedef DWORD(WINAPI* PFN_GetBattery)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*);
typedef DWORD(WINAPI* PFN_GetKeystroke)(DWORD, DWORD, PXINPUT_KEYSTROKE);

static struct {
    HMODULE dll;
    PFN_GetState getState, getStateEx;
    PFN_SetState setState;
    PFN_GetCaps getCaps;
    PFN_Enable enable;
    PFN_GetGuids getGuids;
    PFN_GetBattery getBattery;
    PFN_GetKeystroke getKeystroke;
} real;

static void LoadReal()
{
    static std::once_flag once;
    std::call_once(once, [] {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        std::wstring base(sys);
        real.dll = LoadLibraryW((base + L"\\xinput1_3.dll").c_str());
        if (!real.dll) real.dll = LoadLibraryW((base + L"\\xinput1_4.dll").c_str());
        if (!real.dll) { Log("xinput: no system XInput found"); return; }
        real.getState = (PFN_GetState)GetProcAddress(real.dll, "XInputGetState");
        real.getStateEx = (PFN_GetState)GetProcAddress(real.dll, (LPCSTR)100);
        real.setState = (PFN_SetState)GetProcAddress(real.dll, "XInputSetState");
        real.getCaps = (PFN_GetCaps)GetProcAddress(real.dll, "XInputGetCapabilities");
        real.enable = (PFN_Enable)GetProcAddress(real.dll, "XInputEnable");
        real.getGuids = (PFN_GetGuids)GetProcAddress(real.dll, "XInputGetDSoundAudioDeviceGuids");
        real.getBattery = (PFN_GetBattery)GetProcAddress(real.dll, "XInputGetBatteryInformation");
        real.getKeystroke = (PFN_GetKeystroke)GetProcAddress(real.dll, "XInputGetKeystroke");
        Log("xinput: forwarding to system XInput");
    });
}

static SHORT PickAxis(SHORT a, SHORT b) { return abs(a) >= abs(b) ? a : b; }

static void MergePad(XINPUT_GAMEPAD& p, const XINPUT_GAMEPAD& v)
{
    p.wButtons |= v.wButtons;
    p.bLeftTrigger = std::max(p.bLeftTrigger, v.bLeftTrigger);
    p.bRightTrigger = std::max(p.bRightTrigger, v.bRightTrigger);
    p.sThumbLX = PickAxis(p.sThumbLX, v.sThumbLX);
    p.sThumbLY = PickAxis(p.sThumbLY, v.sThumbLY);
    p.sThumbRX = PickAxis(p.sThumbRX, v.sThumbRX);
    p.sThumbRY = PickAxis(p.sThumbRY, v.sThumbRY);
}

// Merge VR (and debug automation) pads into player 0. Returns true if a virtual pad is active.
static bool MergeVR(DWORD user, XINPUT_STATE* st, bool realOk)
{
    if (user != 0) return false;
    VRPadState vr;
    VRGetPad(vr);
    bool vrOn = vr.active && g_cfg.vrControllers;
    XINPUT_GAMEPAD dbg;
    bool dbgOn = DebugGetPad(dbg);
    if (!vrOn && !dbgOn && !g_cfg.vrControllers) return false;
    static DWORD packet;
    XINPUT_GAMEPAD& p = st->Gamepad;
    if (!realOk) p = {};
    if (vrOn) MergePad(p, vr.pad);
    if (dbgOn) MergePad(p, dbg);
    st->dwPacketNumber = realOk ? st->dwPacketNumber + (++packet) : ++packet;
    return true;
}

// The virtual pad reports "connected" from the very start: the game picks gamepad vs keyboard UI at boot,
// long before the headset session (and VR controller input) is up.
static bool VirtualPadActive()
{
    XINPUT_GAMEPAD dbg;
    return g_cfg.vrControllers || DebugGetPad(dbg);
}

static DWORD GetStateImpl(PFN_GetState fn, DWORD user, XINPUT_STATE* st)
{
    LoadReal();
    DWORD r = fn ? fn(user, st) : ERROR_DEVICE_NOT_CONNECTED;
    if (MergeVR(user, st, r == ERROR_SUCCESS)) return ERROR_SUCCESS;
    return r;
}

extern "C" DWORD WINAPI Proxy_XInputGetState(DWORD user, XINPUT_STATE* st) { LoadReal(); return GetStateImpl(real.getState, user, st); }
extern "C" DWORD WINAPI Proxy_XInputGetStateEx(DWORD user, XINPUT_STATE* st)
{
    LoadReal();
    return GetStateImpl(real.getStateEx ? real.getStateEx : real.getState, user, st);
}

extern "C" DWORD WINAPI Proxy_XInputSetState(DWORD user, XINPUT_VIBRATION* v)
{
    LoadReal();
    DWORD r = real.setState ? real.setState(user, v) : ERROR_DEVICE_NOT_CONNECTED;
    if (r != ERROR_SUCCESS && user == 0 && VirtualPadActive()) return ERROR_SUCCESS;
    return r;
}

extern "C" DWORD WINAPI Proxy_XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps)
{
    LoadReal();
    DWORD r = real.getCaps ? real.getCaps(user, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
    if (r != ERROR_SUCCESS && user == 0) {
        if (VirtualPadActive()) {
            *caps = {};
            caps->Type = XINPUT_DEVTYPE_GAMEPAD;
            caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
            caps->Gamepad.wButtons = 0xF3FF;
            caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 0xFF;
            caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = (SHORT)0xFFC0;
            return ERROR_SUCCESS;
        }
    }
    return r;
}

extern "C" void WINAPI Proxy_XInputEnable(BOOL e) { LoadReal(); if (real.enable) real.enable(e); }

extern "C" DWORD WINAPI Proxy_XInputGetDSoundAudioDeviceGuids(DWORD user, GUID* a, GUID* b)
{
    LoadReal();
    return real.getGuids ? real.getGuids(user, a, b) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI Proxy_XInputGetBatteryInformation(DWORD user, BYTE type, XINPUT_BATTERY_INFORMATION* info)
{
    LoadReal();
    return real.getBattery ? real.getBattery(user, type, info) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI Proxy_XInputGetKeystroke(DWORD user, DWORD reserved, PXINPUT_KEYSTROKE ks)
{
    LoadReal();
    return real.getKeystroke ? real.getKeystroke(user, reserved, ks) : ERROR_EMPTY;
}
