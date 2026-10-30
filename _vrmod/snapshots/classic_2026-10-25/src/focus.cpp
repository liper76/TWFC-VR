// Keep the game believing its window is focused. In VR the desktop focus is often elsewhere (Virtual Desktop,
// overlays), and UE3 ignores pad input and pauses when it loses focus.
#include "common.h"
#include "MinHook.h"

static HWND g_gameWnd;
static WNDPROC g_origWndProc;
typedef HWND(WINAPI* PFN_GetWnd)();
static PFN_GetWnd g_origGetForeground, g_origGetActive, g_origGetFocus;

static bool OwnThreadOwnsGame()
{
    return g_gameWnd && GetWindowThreadProcessId(g_gameWnd, nullptr) == GetCurrentThreadId();
}

static HWND WINAPI HookGetForegroundWindow() { return g_gameWnd ? g_gameWnd : g_origGetForeground(); }
static HWND WINAPI HookGetActiveWindow() { return OwnThreadOwnsGame() ? g_gameWnd : g_origGetActive(); }
static HWND WINAPI HookGetFocus() { return OwnThreadOwnsGame() ? g_gameWnd : g_origGetFocus(); }

static LRESULT CALLBACK GameWndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_ACTIVATEAPP:
        if (!w) return 0;
        break;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) return 0;
        break;
    case WM_KILLFOCUS:
        return 0;
    }
    return CallWindowProcW(g_origWndProc, h, msg, w, l);
}

static BOOL CALLBACK FindGameWnd(HWND h, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    wchar_t cls[64];
    if (pid == GetCurrentProcessId() && GetClassNameW(h, cls, 64) && !wcscmp(cls, L"LaunchUnrealUWindowsClient")) {
        g_gameWnd = h;
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI FocusThread(LPVOID)
{
    while (!g_gameWnd) {
        EnumWindows(FindGameWnd, 0);
        Sleep(250);
    }
    if (!g_cfg.alwaysFocused) return 0;
    g_origWndProc = (WNDPROC)SetWindowLongPtrW(g_gameWnd, GWLP_WNDPROC, (LONG_PTR)GameWndProc);
    MH_CreateHookApi(L"user32", "GetForegroundWindow", (void*)HookGetForegroundWindow, (void**)&g_origGetForeground);
    MH_CreateHookApi(L"user32", "GetActiveWindow", (void*)HookGetActiveWindow, (void**)&g_origGetActive);
    MH_CreateHookApi(L"user32", "GetFocus", (void*)HookGetFocus, (void**)&g_origGetFocus);
    MH_EnableHook(MH_ALL_HOOKS);
    Log("focus: game window %p kept focused", g_gameWnd);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Deadlock fix: dgVoodoo resizes/shows the game window from the render thread while the UE3 game thread
// is busy-waiting for that render thread and not pumping messages. Synchronous cross-thread window
// calls then block forever. Make them asynchronous when they come from a thread that does not own the window.
typedef BOOL(WINAPI* PFN_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
typedef BOOL(WINAPI* PFN_ShowWindow)(HWND, int);
typedef BOOL(WINAPI* PFN_MoveWindow)(HWND, int, int, int, int, BOOL);
static PFN_SetWindowPos g_origSetWindowPos;
static PFN_ShowWindow g_origShowWindow;
static PFN_MoveWindow g_origMoveWindow;

static bool ForeignThread(HWND h)
{
    return h && GetWindowThreadProcessId(h, nullptr) != GetCurrentThreadId();
}

static BOOL WINAPI HookSetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    if (ForeignThread(h)) flags |= SWP_ASYNCWINDOWPOS;
    return g_origSetWindowPos(h, after, x, y, cx, cy, flags);
}

static BOOL WINAPI HookShowWindow(HWND h, int cmd)
{
    if (ForeignThread(h)) return ShowWindowAsync(h, cmd);
    return g_origShowWindow(h, cmd);
}

static BOOL WINAPI HookMoveWindow(HWND h, int x, int y, int w, int hgt, BOOL repaint)
{
    if (ForeignThread(h))
        return g_origSetWindowPos(h, nullptr, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS | (repaint ? 0 : SWP_NOREDRAW));
    return g_origMoveWindow(h, x, y, w, hgt, repaint);
}

void FocusStart()
{
    MH_Initialize();
    MH_CreateHookApi(L"user32", "SetWindowPos", (void*)HookSetWindowPos, (void**)&g_origSetWindowPos);
    MH_CreateHookApi(L"user32", "ShowWindow", (void*)HookShowWindow, (void**)&g_origShowWindow);
    MH_CreateHookApi(L"user32", "MoveWindow", (void*)HookMoveWindow, (void**)&g_origMoveWindow);
    MH_EnableHook(MH_ALL_HOOKS);
    Log("window: async cross-thread window calls enabled");
    CloseHandle(CreateThread(nullptr, 0, FocusThread, nullptr, 0, nullptr));
}
