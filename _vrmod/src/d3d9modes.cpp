// The game only accepts a resolution that is in the adapter's display mode list. Append the VR render
// resolution (e.g. 1800x1800) to the list D3D9 (dgVoodoo) reports, so the registry override is accepted.
#include "common.h"
#include <d3d9.h>
#include "MinHook.h"

typedef IDirect3D9*(WINAPI* PFN_Create9)(UINT);
typedef UINT(STDMETHODCALLTYPE* PFN_ModeCount)(IDirect3D9*, UINT, D3DFORMAT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_EnumModes)(IDirect3D9*, UINT, D3DFORMAT, UINT, D3DDISPLAYMODE*);
static PFN_Create9 g_origCreate9;
static PFN_ModeCount g_origModeCount;
static PFN_EnumModes g_origEnumModes;

static bool Enabled() { return g_cfg.renderWidth > 0 && g_cfg.renderHeight > 0; }

static UINT STDMETHODCALLTYPE HookModeCount(IDirect3D9* d, UINT adapter, D3DFORMAT fmt)
{
    UINT n = g_origModeCount(d, adapter, fmt);
    return n ? n + 1 : n;
}

static HRESULT STDMETHODCALLTYPE HookEnumModes(IDirect3D9* d, UINT adapter, D3DFORMAT fmt, UINT mode, D3DDISPLAYMODE* out)
{
    UINT n = g_origModeCount(d, adapter, fmt);
    if (n && mode == n && out) {
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

static IDirect3D9* WINAPI HookCreate9(UINT sdk)
{
    IDirect3D9* d = g_origCreate9(sdk);
    if (d && !g_origModeCount) {
        void** vt = *(void***)d;
        MH_CreateHook(vt[6], (void*)HookModeCount, (void**)&g_origModeCount);
        MH_CreateHook(vt[7], (void*)HookEnumModes, (void**)&g_origEnumModes);
        MH_EnableHook(vt[6]);
        MH_EnableHook(vt[7]);
        Log("d3d9: mode list hooks installed");
    }
    return d;
}

// DllMain: D3D9.dll (dgVoodoo) is a static import of the game, so it is already loaded.
void D3D9ModesInstall()
{
    if (!Enabled()) return;
    MH_Initialize();
    if (MH_CreateHookApi(L"d3d9", "Direct3DCreate9", (void*)HookCreate9, (void**)&g_origCreate9) == MH_OK)
        MH_EnableHook(MH_ALL_HOOKS);
}
