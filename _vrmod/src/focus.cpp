// Game window handling:
// - keep the game believing its window is focused (in VR the desktop focus is often elsewhere and UE3
//   ignores pad input / pauses without focus);
// - deadlock fix for cross-thread window calls (see below);
// - VR render size: the window client area is forced to RenderWidth x RenderHeight (it may be larger than
//   the monitor). dgVoodoo sizes its swapchain from the client area, so the frame we hand to the headset
//   then has exactly the game's render resolution instead of a stretched desktop-sized copy.
#include "common.h"
#include "MinHook.h"

static HWND g_gameWnd;
static WNDPROC g_origWndProc;
typedef HWND(WINAPI* PFN_GetWnd)();
static PFN_GetWnd g_origGetForeground, g_origGetActive, g_origGetFocus;

static bool ForceSize() { VRRenderSizeWait(); return g_cfg.renderWidth > 0 && g_cfg.renderHeight > 0; }

static bool IsGameClass(const wchar_t* cls)
{
    return cls && !IS_INTRESOURCE(cls) && !wcscmp(cls, L"LaunchUnrealUWindowsClient");
}

static bool OwnThreadOwnsGame()
{
    return g_gameWnd && GetWindowThreadProcessId(g_gameWnd, nullptr) == GetCurrentThreadId();
}

static HWND WINAPI HookGetForegroundWindow() { return g_gameWnd ? g_gameWnd : g_origGetForeground(); }
static HWND WINAPI HookGetActiveWindow() { return OwnThreadOwnsGame() ? g_gameWnd : g_origGetActive(); }
static HWND WINAPI HookGetFocus() { return OwnThreadOwnsGame() ? g_gameWnd : g_origGetFocus(); }

// Window size for a client area of RenderWidth x RenderHeight with the window's current styles.
static SIZE WindowSizeFor(HWND h)
{
    RECT r = { 0, 0, g_cfg.renderWidth, g_cfg.renderHeight };
    AdjustWindowRectEx(&r, (DWORD)GetWindowLongW(h, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(h, GWL_EXSTYLE));
    return { r.right - r.left, r.bottom - r.top };
}

static LRESULT CALLBACK GameWndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_ACTIVATEAPP:
        if (g_cfg.alwaysFocused && !w) return 0;
        break;
    case WM_ACTIVATE:
        if (g_cfg.alwaysFocused && LOWORD(w) == WA_INACTIVE) return 0;
        break;
    case WM_KILLFOCUS:
        if (g_cfg.alwaysFocused) return 0;
        break;
    case WM_GETMINMAXINFO:
        if (ForceSize()) {
            LRESULT r = CallWindowProcW(g_origWndProc, h, msg, w, l);
            auto* mmi = (MINMAXINFO*)l;
            SIZE s = WindowSizeFor(h);
            mmi->ptMaxTrackSize = { std::max<LONG>(mmi->ptMaxTrackSize.x, s.cx), std::max<LONG>(mmi->ptMaxTrackSize.y, s.cy) };
            mmi->ptMaxSize = { std::max<LONG>(mmi->ptMaxSize.x, s.cx), std::max<LONG>(mmi->ptMaxSize.y, s.cy) };
            return r;
        }
        break;
    case WM_WINDOWPOSCHANGING:
        if (ForceSize()) {
            auto* wp = (WINDOWPOS*)l;
            if (!(wp->flags & SWP_NOSIZE)) {
                SIZE s = WindowSizeFor(h);
                wp->cx = s.cx;
                wp->cy = s.cy;
            }
        }
        break;
    }
    return CallWindowProcW(g_origWndProc, h, msg, w, l);
}

static void AttachToGameWindow(HWND h)
{
    if (g_gameWnd) return;
    g_gameWnd = h;
    g_origWndProc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)GameWndProc);
    if (ForceSize()) {
        SIZE s = WindowSizeFor(h);
        SetWindowPos(h, nullptr, 0, 0, s.cx, s.cy, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
        RECT c;
        GetClientRect(h, &c);
        Log("window: client forced to %ldx%ld (wanted %dx%d)", c.right, c.bottom, g_cfg.renderWidth, g_cfg.renderHeight);
    }
}

typedef HWND(WINAPI* PFN_CreateWindowExW)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static PFN_CreateWindowExW g_origCreateWindowExW;

static HWND WINAPI HookCreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h,
                                       HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    bool game = IsGameClass(cls);
    if (game && ForceSize()) {
        RECT r = { 0, 0, g_cfg.renderWidth, g_cfg.renderHeight };
        AdjustWindowRectEx(&r, style, FALSE, ex);
        w = r.right - r.left;
        h = r.bottom - r.top;
    }
    HWND wnd = g_origCreateWindowExW(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (game && wnd) AttachToGameWindow(wnd);
    return wnd;
}

static BOOL CALLBACK FindGameWnd(HWND h, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    wchar_t cls[64];
    if (pid == GetCurrentProcessId() && GetClassNameW(h, cls, 64) && IsGameClass(cls)) {
        AttachToGameWindow(h);
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI FocusThread(LPVOID)
{
    while (!g_gameWnd) {
        EnumWindows(FindGameWnd, 0);   // fallback if the window was created before our hook
        Sleep(250);
    }
    if (!g_cfg.alwaysFocused) return 0;
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

// The VR render size only works in windowed mode (exclusive fullscreen would switch the monitor to it),
// so "-windowed" is appended to the command line the game's CRT parses after our DllMain.
typedef LPWSTR(WINAPI* PFN_GetCmdW)();
typedef LPSTR(WINAPI* PFN_GetCmdA)();
static PFN_GetCmdW g_origGetCmdW;
static PFN_GetCmdA g_origGetCmdA;

static LPWSTR WINAPI HookGetCmdW()
{
    static std::wstring s = std::wstring(g_origGetCmdW()) + L" -windowed";
    return (LPWSTR)s.c_str();
}

static LPSTR WINAPI HookGetCmdA()
{
    static std::string s = std::string(g_origGetCmdA()) + " -windowed";
    return (LPSTR)s.c_str();
}

// Called from DllMain, before the game creates its window.
void WindowEarlyInit()
{
    if (VRRenderSizeOn()) {   // (DllMain: not waiting for the headset's size)
        MH_Initialize();
        MH_CreateHookApi(L"kernel32", "GetCommandLineW", (void*)HookGetCmdW, (void**)&g_origGetCmdW);
        MH_CreateHookApi(L"kernel32", "GetCommandLineA", (void*)HookGetCmdA, (void**)&g_origGetCmdA);
    }
    // Without DPI awareness Windows scales the window (125% etc.) and dgVoodoo's swapchain would not match
    // the render size pixel for pixel.
    if (VRRenderSizeOn()) SetProcessDPIAware();
    MH_Initialize();
    MH_CreateHookApi(L"user32", "CreateWindowExW", (void*)HookCreateWindowExW, (void**)&g_origCreateWindowExW);
    MH_EnableHook(MH_ALL_HOOKS);
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
