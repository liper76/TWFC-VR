// The game only accepts a resolution that is in the adapter's display mode list. Append the VR render
// resolution (e.g. 1800x1800) to the list D3D9 (dgVoodoo) reports, so the registry override is accepted.
// Also keeps the game's D3D9 device: D3D9Flush() makes the draws issued so far reach D3D11 now (see gfx.cpp).
#include "common.h"
#include <d3d9.h>
#include "MinHook.h"

extern volatile long g_traceFrames;   // d3dtrace.cpp

typedef IDirect3D9*(WINAPI* PFN_Create9)(UINT);
typedef UINT(STDMETHODCALLTYPE* PFN_ModeCount)(IDirect3D9*, UINT, D3DFORMAT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_EnumModes)(IDirect3D9*, UINT, D3DFORMAT, UINT, D3DDISPLAYMODE*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDevice)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                     IDirect3DDevice9**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DrawPrim)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DrawIdxPrim)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DrawPrimUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DrawIdxPrimUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT,
                                                      const void*, UINT);
static PFN_Create9 g_origCreate9;
static PFN_ModeCount g_origModeCount;
static PFN_EnumModes g_origEnumModes;
static PFN_CreateDevice g_origCreateDevice;
static PFN_DrawPrim g_origDrawPrim;
static PFN_DrawIdxPrim g_origDrawIdxPrim;
static PFN_DrawPrimUP g_origDrawPrimUP;
static PFN_DrawIdxPrimUP g_origDrawIdxPrimUP;
static IDirect3DDevice9* g_dev;
static IDirect3DQuery9* g_flushQuery;
int g_hudFlush = 1;   // debug "hudflush": 0 off, 1 an event query, 2 the query and a flush of the command buffer

static bool Enabled() { VRRenderSizeWait(); return g_cfg.renderWidth > 0 && g_cfg.renderHeight > 0; }

static UINT STDMETHODCALLTYPE HookModeCount(IDirect3D9* d, UINT adapter, D3DFORMAT fmt)
{
    UINT n = g_origModeCount(d, adapter, fmt);
    return n && Enabled() ? n + 1 : n;
}

static HRESULT STDMETHODCALLTYPE HookEnumModes(IDirect3D9* d, UINT adapter, D3DFORMAT fmt, UINT mode, D3DDISPLAYMODE* out)
{
    UINT n = g_origModeCount(d, adapter, fmt);
    if (n && mode == n && out && Enabled()) {
        D3DDISPLAYMODE last = {};
        g_origEnumModes(d, adapter, fmt, n - 1, &last);
        out->Width = g_cfg.renderWidth;
        out->Height = g_cfg.renderHeight;
        out->RefreshRate = last.RefreshRate;
        out->Format = last.Format;
        static bool logged;
        if (!logged) { Log("d3d9: added display mode %ux%u@%u", out->Width, out->Height, out->RefreshRate); logged = true; }
        return D3D_OK;
    }
    return g_origEnumModes(d, adapter, fmt, mode, out);
}

static HRESULT STDMETHODCALLTYPE HookDrawPrim(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT start, UINT n)
{
    if (g_traceFrames > 0) Log("trace: d3d9 DrawPrimitive %u", n);
    return g_origDrawPrim(d, t, start, n);
}

static HRESULT STDMETHODCALLTYPE HookDrawIdxPrim(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT base, UINT minIdx, UINT verts,
                                                 UINT start, UINT n)
{
    if (g_traceFrames > 0) Log("trace: d3d9 DrawIndexedPrimitive %u", n);
    return g_origDrawIdxPrim(d, t, base, minIdx, verts, start, n);
}

static HRESULT STDMETHODCALLTYPE HookDrawPrimUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT n, const void* v, UINT stride)
{
    if (g_traceFrames > 0) Log("trace: d3d9 DrawPrimitiveUP %u", n);
    return g_origDrawPrimUP(d, t, n, v, stride);
}

static HRESULT STDMETHODCALLTYPE HookDrawIdxPrimUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT minIdx, UINT verts, UINT n,
                                                   const void* idx, D3DFORMAT fmt, const void* v, UINT stride)
{
    if (g_traceFrames > 0) Log("trace: d3d9 DrawIndexedPrimitiveUP %u", n);
    return g_origDrawIdxPrimUP(d, t, minIdx, verts, n, idx, fmt, v, stride);
}

static HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags,
                                                  D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    HRESULT hr = g_origCreateDevice(d, adapter, type, wnd, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        g_dev = *out;
        if (g_flushQuery) g_flushQuery->Release();
        g_flushQuery = nullptr;
        g_dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_flushQuery);
        if (!g_origDrawPrim) {
            void** vt = *(void***)g_dev;
            MH_CreateHook(vt[81], (void*)HookDrawPrim, (void**)&g_origDrawPrim);
            MH_CreateHook(vt[82], (void*)HookDrawIdxPrim, (void**)&g_origDrawIdxPrim);
            MH_CreateHook(vt[83], (void*)HookDrawPrimUP, (void**)&g_origDrawPrimUP);
            MH_CreateHook(vt[84], (void*)HookDrawIdxPrimUP, (void**)&g_origDrawIdxPrimUP);
            for (int i = 81; i <= 84; i++) MH_EnableHook(vt[i]);
        }
        Log("d3d9: device %p, flush query %p", g_dev, g_flushQuery);
    }
    return hr;
}

static IDirect3D9* WINAPI HookCreate9(UINT sdk)
{
    IDirect3D9* d = g_origCreate9(sdk);
    if (d && !g_origCreateDevice) {
        void** vt = *(void***)d;
        if (VRRenderSizeOn()) {
            MH_CreateHook(vt[6], (void*)HookModeCount, (void**)&g_origModeCount);
            MH_CreateHook(vt[7], (void*)HookEnumModes, (void**)&g_origEnumModes);
            MH_EnableHook(vt[6]);
            MH_EnableHook(vt[7]);
            Log("d3d9: mode list hooks installed");
        }
        MH_CreateHook(vt[16], (void*)HookCreateDevice, (void**)&g_origCreateDevice);
        MH_EnableHook(vt[16]);
    }
    return d;
}

void D3D9Flush()
{
    if (!g_flushQuery || !g_hudFlush) return;
    g_flushQuery->Issue(D3DISSUE_END);
    if (g_hudFlush >= 2) g_flushQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH);
}

// DllMain: D3D9.dll (dgVoodoo) is a static import of the game, so it is already loaded.
void D3D9ModesInstall()
{
    MH_Initialize();
    if (MH_CreateHookApi(L"d3d9", "Direct3DCreate9", (void*)HookCreate9, (void**)&g_origCreate9) == MH_OK)
        MH_EnableHook(MH_ALL_HOOKS);
}
