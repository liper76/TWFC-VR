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
static thread_local int t_inGameWndProc;
typedef HWND(WINAPI* PFN_GetWnd)();
static PFN_GetWnd g_origGetForeground, g_origGetActive, g_origGetFocus;
typedef LONG(WINAPI* PFN_SetWindowLongW)(HWND, int, LONG);
typedef LONG(WINAPI* PFN_SetWindowLongA)(HWND, int, LONG);
static PFN_SetWindowLongW g_origSetWindowLongW;
static PFN_SetWindowLongA g_origSetWindowLongA;

static bool ForceSize() { return g_cfg.renderWidth > 0 && g_cfg.renderHeight > 0; }

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

bool FocusIsRealForeground()
{
    HWND fg = g_origGetForeground ? g_origGetForeground() : GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Window size for a client area of RenderWidth x RenderHeight with the window's current styles.
static SIZE WindowSizeFor(HWND h)
{
    RECT r = { 0, 0, g_cfg.renderWidth, g_cfg.renderHeight };
    AdjustWindowRectEx(&r, (DWORD)GetWindowLongW(h, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(h, GWL_EXSTYLE));
    return { r.right - r.left, r.bottom - r.top };
}

static LRESULT CALLBACK GameWndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (t_inGameWndProc > 8) return g_origWndProc ? CallWindowProcW(g_origWndProc, h, msg, w, l) : DefWindowProcW(h, msg, w, l);
    t_inGameWndProc++;
    LRESULT res = 0;
    bool handled = false;
    switch (msg) {
    case WM_ACTIVATEAPP:
        if (g_cfg.alwaysFocused && !w) { handled = true; res = 0; }
        break;
    case WM_ACTIVATE:
        if (g_cfg.alwaysFocused && LOWORD(w) == WA_INACTIVE) { handled = true; res = 0; }
        break;
    case WM_KILLFOCUS:
        if (g_cfg.alwaysFocused) { handled = true; res = 0; }
        break;
    case WM_GETMINMAXINFO:
        if (ForceSize()) {
            res = CallWindowProcW(g_origWndProc, h, msg, w, l);
            auto* mmi = (MINMAXINFO*)l;
            SIZE s = WindowSizeFor(h);
            mmi->ptMaxTrackSize = { std::max<LONG>(mmi->ptMaxTrackSize.x, s.cx + 64), std::max<LONG>(mmi->ptMaxTrackSize.y, s.cy + 64) };
            mmi->ptMaxSize = { std::max<LONG>(mmi->ptMaxSize.x, s.cx + 64), std::max<LONG>(mmi->ptMaxSize.y, s.cy + 64) };
            handled = true;
        }
        break;
    case WM_WINDOWPOSCHANGING:
        if (ForceSize() && !IsIconic(h)) {
            auto* wp = (WINDOWPOS*)l;
            if (!(wp->flags & SWP_NOSIZE)) {
                SIZE s = WindowSizeFor(h);
                wp->cx = s.cx;
                wp->cy = s.cy;
            }
        }
        break;
    }
    if (!handled) res = CallWindowProcW(g_origWndProc, h, msg, w, l);
    t_inGameWndProc--;
    return res;
}

static bool AddrInExe(void* p)
{
    HMODULE mod = nullptr;
    if (!p || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)p, &mod))
        return false;
    return mod == GetModuleHandleW(nullptr);
}

// Keep GameWndProc installed when TWFC.exe (0x116BA764, or Steam equivalent) resets GWL_WNDPROC back to
// its own WndProc (0x117FC7D0). Without this, Windows clamps the window height to the monitor work area
// (e.g. ~1040 px on 1080p, 1520 px on 1600p), UE3 shrinks its viewport via WM_SIZE/GetClientRect, and the
// bottom half of the square VR backbuffer renders solid black (issue #3). Non-exe subclasses (dgVoodoo/DXGI)
// pass through normally on top of GameWndProc so no subclass cycle can ever form.
static LONG WINAPI HookSetWindowLongW(HWND h, int idx, LONG val)
{
    if (h && h == g_gameWnd && idx == GWLP_WNDPROC && val && (WNDPROC)val != GameWndProc && AddrInExe((void*)val)) {
        g_origWndProc = (WNDPROC)val;
        Log("window: retained GameWndProc over game WndProc %p", (void*)val);
        return g_origSetWindowLongW(h, idx, (LONG)GameWndProc);
    }
    return g_origSetWindowLongW(h, idx, val);
}

static LONG WINAPI HookSetWindowLongA(HWND h, int idx, LONG val)
{
    if (h && h == g_gameWnd && idx == GWLP_WNDPROC && val && (WNDPROC)val != GameWndProc && AddrInExe((void*)val)) {
        g_origWndProc = (WNDPROC)val;
        Log("window: retained GameWndProc over game WndProc %p", (void*)val);
        return g_origSetWindowLongA(h, idx, (LONG)GameWndProc);
    }
    return g_origSetWindowLongA(h, idx, val);
}

static void AttachToGameWindow(HWND h)
{
    if (g_gameWnd) return;
    g_gameWnd = h;
    g_origWndProc = g_origSetWindowLongW
        ? (WNDPROC)g_origSetWindowLongW(h, GWLP_WNDPROC, (LONG)GameWndProc)
        : (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)GameWndProc);
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
    if (h && h == g_gameWnd && ForceSize() && !(flags & SWP_NOSIZE)) {
        SIZE s = WindowSizeFor(h);
        cx = s.cx;
        cy = s.cy;
    }
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
    if (h && h == g_gameWnd && ForceSize()) {
        SIZE s = WindowSizeFor(h);
        w = s.cx;
        hgt = s.cy;
    }
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

// Both WFC and FoC use High Moon's shared "UnrealEngine3_11" single-instance mutex;
// scope WFC VR's mutex so it still prevents duplicate WFC launches without colliding with FoC.
typedef HANDLE(WINAPI* PFN_CreateMutexW)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
static PFN_CreateMutexW g_origCreateMutexW;

static HANDLE WINAPI HookCreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCWSTR name)
{
    if (name && !wcscmp(name, L"UnrealEngine3_11"))
        name = L"UnrealEngine3_11_TWFC";
    return g_origCreateMutexW(sa, own, name);
}

// Called from DllMain, before the game creates its window.
void WindowEarlyInit()
{
    if (ForceSize()) {
        MH_Initialize();
        MH_CreateHookApi(L"kernel32", "GetCommandLineW", (void*)HookGetCmdW, (void**)&g_origGetCmdW);
        MH_CreateHookApi(L"kernel32", "GetCommandLineA", (void*)HookGetCmdA, (void**)&g_origGetCmdA);
    }
    // Without DPI awareness Windows scales the window (125% etc.) and dgVoodoo's swapchain would not match
    // the render size pixel for pixel.
    if (ForceSize()) SetProcessDPIAware();
    MH_Initialize();
    if (MH_CreateHookApi(L"kernelbase", "CreateMutexW", (void*)HookCreateMutexW, (void**)&g_origCreateMutexW) != MH_OK)
        MH_CreateHookApi(L"kernel32", "CreateMutexW", (void*)HookCreateMutexW, (void**)&g_origCreateMutexW);
    MH_CreateHookApi(L"user32", "CreateWindowExW", (void*)HookCreateWindowExW, (void**)&g_origCreateWindowExW);
    MH_CreateHookApi(L"user32", "SetWindowLongW", (void*)HookSetWindowLongW, (void**)&g_origSetWindowLongW);
    MH_CreateHookApi(L"user32", "SetWindowLongA", (void*)HookSetWindowLongA, (void**)&g_origSetWindowLongA);
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
