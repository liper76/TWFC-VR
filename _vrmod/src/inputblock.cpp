// While the headset is on, the game gets no mouse or keyboard input (DirectInput): a bumped mouse or desk keyboard
// would otherwise turn the view or press buttons. (Button hints are a separate matter: hints.cpp.)
#include "common.h"
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include "MinHook.h"

namespace {

typedef HRESULT(STDMETHODCALLTYPE* GetDeviceStateFn)(void* dev, DWORD size, void* data);
typedef HRESULT(STDMETHODCALLTYPE* GetDeviceDataFn)(void* dev, DWORD size, DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags);
GetDeviceStateFn g_origState[2];
GetDeviceDataFn g_origData[2];

bool Blocking() { return g_cfg.blockMouseKeyboard && VRIsRunning(); }

template <int K>
HRESULT STDMETHODCALLTYPE HookState(void* dev, DWORD size, void* data)
{
    HRESULT r = g_origState[K](dev, size, data);
    if (SUCCEEDED(r) && data && Blocking()) memset(data, 0, size);
    return r;
}

template <int K>
HRESULT STDMETHODCALLTYPE HookData(void* dev, DWORD size, DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags)
{
    HRESULT r = g_origData[K](dev, size, data, count, flags);
    if (SUCCEEDED(r) && count && Blocking() && !(flags & DIGDD_PEEK)) *count = 0;   // events read and dropped
    return r;
}

template <int K>
void HookDeviceVtable(void** vt)
{
    if (!g_origState[K] && MH_CreateHook(vt[9], (void*)HookState<K>, (void**)&g_origState[K]) == MH_OK) MH_EnableHook(vt[9]);
    if (!g_origData[K] && MH_CreateHook(vt[10], (void*)HookData<K>, (void**)&g_origData[K]) == MH_OK) MH_EnableHook(vt[10]);
}

}   // namespace

// The device method tables are shared by all devices of a kind (ANSI / wide): a keyboard device of each is made
// once to find them.
void InputBlockInstall()
{
    HMODULE di = LoadLibraryW(L"dinput8.dll");
    typedef HRESULT(WINAPI* CreateFn)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    CreateFn create = di ? (CreateFn)GetProcAddress(di, "DirectInput8Create") : nullptr;
    if (!create) return;
    IDirectInput8W* w = nullptr;
    if (SUCCEEDED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, (void**)&w, nullptr)) && w) {
        IDirectInputDevice8W* dev = nullptr;
        if (SUCCEEDED(w->CreateDevice(GUID_SysKeyboard, &dev, nullptr)) && dev) { HookDeviceVtable<0>(*(void***)dev); dev->Release(); }
        w->Release();
    }
    IDirectInput8A* a = nullptr;
    if (SUCCEEDED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, (void**)&a, nullptr)) && a) {
        IDirectInputDevice8A* dev = nullptr;
        if (SUCCEEDED(a->CreateDevice(GUID_SysKeyboard, &dev, nullptr)) && dev) {
            HookDeviceVtable<1>(*(void***)dev);   // (fails harmlessly when it is the same code as the wide one)
            dev->Release();
        }
        a->Release();
    }
    Log("input: DirectInput mouse/keyboard %s while the headset is on", g_origState[0] || g_origState[1] ? "blocked" : "hooks FAILED");
}
