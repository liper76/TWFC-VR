// D3D11-level work on dgVoodoo's immediate context:
// - research trace of one frame ("d3dtrace N" debug command);
// - HUD split: the game draws its 3D scene into dgVoodoo's D3D9-backbuffer texture with one final
//   full-screen pass and then the 2D HUD on top. While the VR gameplay view is active, every draw into that
//   texture after the first one is redirected into our own transparent textures, one per HUD layer (the layer is
//   set by gfx.cpp around the HUD movie's drawing; the rest - the game's Canvas - goes to kHudEyes). The eyes then
//   get the scene without HUD, and vr.cpp puts the layers where they belong. Hurt (its post-process chain 1), the
//   game draws the movie into a texture of its own instead, runs scan lines over it and lays that over the screen
//   with one draw: the movie's draws go to their layers wherever the game sends them (else that one draw put the
//   whole HUD - the crosshair in the middle - into the eye images, and the panel and the reticle were empty).
#include "common.h"
#include <d3d11.h>
#include <d3d10.h>
#include "MinHook.h"
#include <vector>
#include <algorithm>
#include <math.h>

typedef void(STDMETHODCALLTYPE* PFN_OMSetRT)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
typedef void(STDMETHODCALLTYPE* PFN_Draw)(ID3D11DeviceContext*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexed)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInst)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawInst)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_ClearRTV)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
typedef void(STDMETHODCALLTYPE* PFN_CopyRes)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
typedef void(STDMETHODCALLTYPE* PFN_CopySub)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
typedef void(STDMETHODCALLTYPE* PFN_ResolveSub)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);
static const int kD3DVars = 6;
static PFN_OMSetRT g_oSetRT[kD3DVars];
static PFN_Draw g_oDraw[kD3DVars];
static PFN_DrawIndexed g_oDrawIdx[kD3DVars];
static PFN_DrawIndexedInst g_oDrawIdxInst[kD3DVars];
static PFN_DrawInst g_oDrawInst[kD3DVars];
static PFN_ClearRTV g_oClear[kD3DVars];
static PFN_CopyRes g_oCopy[kD3DVars];
static PFN_CopySub g_oCopySub[kD3DVars];
static PFN_ResolveSub g_oResolveSub[kD3DVars];
static void* g_slotTargets[9][kD3DVars];
static int g_slotCount[9];
static const int kSlotVtableIdx[9] = { 33, 13, 12, 20, 21, 50, 47, 46, 57 };
static thread_local int t_hookDepth;
static ID3D11DeviceContext* g_imm;    // dgVoodoo's immediate context (our own deferred contexts are ignored)

static int ActiveSlotVar(int slot)
{
    if (g_imm) {
        void* cur = (*(void***)g_imm)[kSlotVtableIdx[slot]];
        for (int v = 0; v < g_slotCount[slot]; v++)
            if (g_slotTargets[slot][v] == cur) return v;
    }
    return g_slotCount[slot] > 0 ? g_slotCount[slot] - 1 : 0;
}

static void OrigSetRT(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv)
{
    PFN_OMSetRT fn = g_oSetRT[ActiveSlotVar(0)];
    if (!fn) return;
    t_hookDepth++;
    fn(c, n, rtv, dsv);
    t_hookDepth--;
}

static void OrigClear(ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv, const FLOAT col[4])
{
    PFN_ClearRTV fn = g_oClear[ActiveSlotVar(5)];
    if (!fn) return;
    t_hookDepth++;
    fn(c, rtv, col);
    t_hookDepth--;
}

static void OrigCopy(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src)
{
    PFN_CopyRes fn = g_oCopy[ActiveSlotVar(6)];
    if (!fn) return;
    t_hookDepth++;
    fn(c, dst, src);
    t_hookDepth--;
}

static void OrigCopySub(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT ss, const D3D11_BOX* box)
{
    PFN_CopySub fn = g_oCopySub[ActiveSlotVar(7)];
    if (!fn) return;
    t_hookDepth++;
    fn(c, dst, ds, x, y, z, src, ss, box);
    t_hookDepth--;
}

volatile long g_traceFrames;          // frames left to trace (set by debug command)
static ID3D11Device* g_dev;
static ID3D11Resource* g_curRT;       // resource of the bound render target 0 (not AddRef'd)
static ID3D11DepthStencilView* g_curDSV;
static ID3D11RenderTargetView* g_curRTVs[8];   // ... the bound targets as the game bound them
static UINT g_curRTVn;
static int g_drawsOnRT;
static long g_traceDraws;             // draws of the traced frame

// ---- HUD split state ----
static volatile bool g_hudWanted;     // set by vr.cpp for the coming frame
static bool g_hudActive;              // redirect is active in this frame
static ID3D11Resource* g_bbTex;       // dgVoodoo's D3D9 backbuffer texture (source of its Present copy)
static bool g_copySeen;
static bool g_copyExact;              // saw a copy of a drawn RT matching RenderWidth x RenderHeight this frame
static bool g_bbDrawnThisFrame;       // g_bbTex was drawn into during this frame
static ID3D11Resource* g_drawnRTs[256];   // textures drawn into this frame (only these can be the backbuffer texture)
static int g_drawnN;
static int g_drawsOnBB;
static int g_hudLayer;                // where the HUD's draws go now
static bool g_hudMovie;               // ... and the HUD movie draws now (HudSetLayer)
static int g_boundLayer = -1;         // the layer whose target is bound instead of the backbuffer texture's
struct HudLayer {
    ID3D11Texture2D* tex;
    ID3D11RenderTargetView* rtv;
    ID3D11ShaderResourceView* srv;
    bool cleared;
    long draws;                       // this frame
};
static HudLayer g_layers[kHudLayers];
static UINT g_hudW, g_hudH;
static ID3D11Resource* g_movieRT;    // where the HUD movie's first draw of this frame went (for the log)
static long g_movieDraws;            // ... and how many it made
static HRESULT g_layersFailed;       // the layers could not be made (logged once)

static bool TexInfo(ID3D11Resource* r, UINT& w, UINT& h, DXGI_FORMAT& fmt)
{
    ID3D11Texture2D* t = nullptr;
    if (!r || FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) return false;
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    t->Release();
    w = d.Width;
    h = d.Height;
    fmt = d.Format;
    return true;
}

static bool TexSize(ID3D11Resource* r, UINT& w, UINT& h)
{
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    return TexInfo(r, w, h, fmt);
}

static bool IsDisplayColorFormat(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return true;
    default:
        return false;
    }
}

static void Desc(ID3D11Resource* r, char* out, size_t n)
{
    ID3D11Texture2D* t = nullptr;
    if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) {
        D3D11_TEXTURE2D_DESC d;
        t->GetDesc(&d);
        sprintf_s(out, n, "%p %ux%u f%d", r, d.Width, d.Height, d.Format);
        t->Release();
    } else {
        sprintf_s(out, n, "%p", r);
    }
}

static bool EnsureLayers(UINT w, UINT h)
{
    if (g_layers[0].tex && g_hudW == w && g_hudH == h) return true;
    for (HudLayer& l : g_layers) {
        if (l.srv) l.srv->Release();
        if (l.rtv) l.rtv->Release();
        if (l.tex) l.tex->Release();
        l = HudLayer();
    }
    g_hudW = g_hudH = 0;
    D3D11_TEXTURE2D_DESC d = { w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    for (HudLayer& l : g_layers) {
        HRESULT hr = g_dev->CreateTexture2D(&d, nullptr, &l.tex);
        if (SUCCEEDED(hr)) hr = g_dev->CreateRenderTargetView(l.tex, nullptr, &l.rtv);
        if (SUCCEEDED(hr)) hr = g_dev->CreateShaderResourceView(l.tex, nullptr, &l.srv);
        if (FAILED(hr) || !l.rtv || !l.srv) {
            if (!g_layersFailed) Log("hud: layer textures %ux%u FAILED (%08lX)", w, h, (unsigned long)hr);
            g_layersFailed = FAILED(hr) ? hr : E_FAIL;
            return false;
        }
    }
    g_hudW = w;
    g_hudH = h;
    Log("hud: layer textures %ux%u", w, h);
    return true;
}

volatile bool g_d3dOwnCalls;   // set by vr.cpp while it issues its own copies/draws/OpenXR calls on the immediate context

// Before a draw into the backbuffer texture: after the scene's final pass, send the draw to the current layer. While
// the HUD movie draws, also a draw into a texture of the game's as large (hurt: the movie drawn there first).
static void BeforeDraw(ID3D11DeviceContext* c)
{
    if (g_d3dOwnCalls || c != g_imm) return;
    if (g_hudActive && g_hudMovie && !g_movieDraws++) g_movieRT = g_curRT;
    if (g_boundLayer >= 0 && g_curRT != g_bbTex && !g_hudMovie) {   // (the movie done: the game's texture back)
        OrigSetRT(c, g_curRTVn, g_curRTVs, g_curDSV);
        g_boundLayer = -1;
    }
    if (!g_hudActive || !g_curRT || !g_bbTex || g_drawsOnBB < 1 || g_boundLayer == g_hudLayer) return;
    UINT w, h, rw, rh;
    if (!TexSize(g_bbTex, w, h)) return;
    if (g_curRT != g_bbTex && (!g_hudMovie || !TexSize(g_curRT, rw, rh) || rw != w || rh != h)) return;
    if (!EnsureLayers(w, h)) return;
    HudLayer& l = g_layers[g_hudLayer];
    if (!l.cleared) {
        const FLOAT zero[4] = { 0, 0, 0, 0 };
        OrigClear(c, l.rtv, zero);
        l.cleared = true;
    }
    OrigSetRT(c, 1, &l.rtv, g_curDSV);
    g_boundLayer = g_hudLayer;
}

static void AfterDraw(ID3D11DeviceContext* c)
{
    if (g_d3dOwnCalls) return;   // (the eye images' draws and OpenXR runtime calls: not the game's textures)
    if (c == g_imm && g_curRT && (!g_drawnN || g_drawnRTs[g_drawnN - 1] != g_curRT) && g_drawnN < 256 &&
        std::find(g_drawnRTs, g_drawnRTs + g_drawnN, g_curRT) == g_drawnRTs + g_drawnN)
        g_drawnRTs[g_drawnN++] = g_curRT;
    if (c == g_imm && g_traceFrames > 0) g_traceDraws++;
    if (c == g_imm && g_curRT && g_curRT == g_bbTex) {
        g_drawsOnBB++;
        g_bbDrawnThisFrame = true;
    }
    if (c == g_imm && g_boundLayer >= 0) g_layers[g_boundLayer].draws++;
}

bool HudSplitActive() { return g_hudActive; }
void HudSetLayer(int layer, bool movie)
{
    g_hudLayer = layer >= 0 && layer < kHudLayers ? layer : kHudEyes;
    g_hudMovie = movie;
    if (g_traceFrames > 0) Log("trace: HUD layer %d", g_hudLayer);
}

template <int V>
static void STDMETHODCALLTYPE HSetRT(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv)
{
    if (t_hookDepth > 0 || g_d3dOwnCalls || c != g_imm) { g_oSetRT[V](c, n, rtv, dsv); return; }
    t_hookDepth++;
    ID3D11Resource* r = nullptr;
    if (n && rtv && rtv[0]) { rtv[0]->GetResource(&r); if (r) r->Release(); }
    if (g_traceFrames > 0 && r != g_curRT) {
        if (g_curRT) Log("trace:   (%d draws)", g_drawsOnRT);
        char d[96]; Desc(r, d, sizeof d);
        Log("trace: RT %s dsv %p", d, dsv);
        g_drawsOnRT = 0;
    }
    g_curRT = r;
    g_curDSV = dsv;
    g_curRTVn = rtv ? std::min(n, 8u) : 0;
    for (UINT i = 0; i < g_curRTVn; i++) g_curRTVs[i] = rtv[i];
    g_boundLayer = -1;
    g_oSetRT[V](c, n, rtv, dsv);
    t_hookDepth--;
}

template <int V>
static void STDMETHODCALLTYPE HDraw(ID3D11DeviceContext* c, UINT vc, UINT sv)
{
    if (t_hookDepth > 0 || g_d3dOwnCalls || c != g_imm) { g_oDraw[V](c, vc, sv); return; }
    t_hookDepth++;
    BeforeDraw(c);
    if (g_traceFrames > 0 && (++g_drawsOnRT <= 3 || g_boundLayer >= 0)) Log("trace:   Draw %u -> layer %d", vc, g_boundLayer);
    g_oDraw[V](c, vc, sv);
    AfterDraw(c);
    t_hookDepth--;
}

template <int V>
static void STDMETHODCALLTYPE HDrawIdx(ID3D11DeviceContext* c, UINT ic, UINT si, INT bv)
{
    if (t_hookDepth > 0 || g_d3dOwnCalls || c != g_imm) { g_oDrawIdx[V](c, ic, si, bv); return; }
    t_hookDepth++;
    BeforeDraw(c);
    if (g_traceFrames > 0 && (++g_drawsOnRT <= 3 || g_boundLayer >= 0)) Log("trace:   DrawIndexed %u -> layer %d", ic, g_boundLayer);
    g_oDrawIdx[V](c, ic, si, bv);
    AfterDraw(c);
    t_hookDepth--;
}

template <int V>
static void STDMETHODCALLTYPE HDrawIdxInst(ID3D11DeviceContext* c, UINT ic, UINT inst, UINT si, INT bv, UINT sinst)
{
    if (t_hookDepth > 0 || g_d3dOwnCalls || c != g_imm) { g_oDrawIdxInst[V](c, ic, inst, si, bv, sinst); return; }
    t_hookDepth++;
    BeforeDraw(c);
    g_oDrawIdxInst[V](c, ic, inst, si, bv, sinst);
    AfterDraw(c);
    t_hookDepth--;
}

template <int V>
static void STDMETHODCALLTYPE HDrawInst(ID3D11DeviceContext* c, UINT vc, UINT inst, UINT sv, UINT sinst)
{
    if (t_hookDepth > 0 || g_d3dOwnCalls || c != g_imm) { g_oDrawInst[V](c, vc, inst, sv, sinst); return; }
    t_hookDepth++;
    BeforeDraw(c);
    g_oDrawInst[V](c, vc, inst, sv, sinst);
    AfterDraw(c);
    t_hookDepth--;
}

template <int V>
static void STDMETHODCALLTYPE HClear(ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv, const FLOAT col[4])
{
    if (t_hookDepth > 0 || g_d3dOwnCalls || c != g_imm) { g_oClear[V](c, rtv, col); return; }
    t_hookDepth++;
    if (g_traceFrames > 0 && rtv) {
        ID3D11Resource* r = nullptr;
        rtv->GetResource(&r);
        char d[96]; Desc(r, d, sizeof d);
        Log("trace: Clear %s (%.2f %.2f %.2f %.2f)", d, col[0], col[1], col[2], col[3]);
        if (r) r->Release();
    }
    g_oClear[V](c, rtv, col);
    t_hookDepth--;
}

// dgVoodoo's present path copies the D3D9 backbuffer texture: with CopyResource when the game's window is as large as
// its picture, with CopySubresourceRegion and a scaling draw when Windows made the window smaller (e.g. 1080p/1440p
// monitor: 1800x1040 or 2560x1520), or ResolveSubresource when MSAA is on. Prefer the latest drawn display-color RT
// matching RenderWidth x RenderHeight so mid-frame HDR/smaller StretchRect copies never hijack g_bbTex.
static void NoteCopy(ID3D11Resource* src)
{
    if (!src || std::find(g_drawnRTs, g_drawnRTs + g_drawnN, src) == g_drawnRTs + g_drawnN) return;
    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    if (!TexInfo(src, w, h, fmt) || !IsDisplayColorFormat(fmt)) return;
    if (g_cfg.renderWidth > 0 && g_cfg.renderHeight > 0) {
        bool exact = w == (UINT)g_cfg.renderWidth && h == (UINT)g_cfg.renderHeight;
        if (!exact) {
            if (g_copyExact || g_copySeen) return;
            // Don't overwrite an already-known exact-size backbuffer with a smaller mid-frame RT.
            UINT bw = 0, bh = 0;
            if (g_bbTex && TexSize(g_bbTex, bw, bh) && bw == (UINT)g_cfg.renderWidth && bh == (UINT)g_cfg.renderHeight) return;
        } else {
            g_copyExact = true;
        }
    } else if (g_copySeen) {
        return;
    }
    g_copySeen = true;
    if (src != g_bbTex) {
        g_bbTex = src;
        char b[96]; Desc(src, b, sizeof b);
        Log("hud: backbuffer texture %s", b);
    }
}

template <int V>
static void STDMETHODCALLTYPE HCopy(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src)
{
    if (t_hookDepth > 0 || g_d3dOwnCalls || c != g_imm) { g_oCopy[V](c, dst, src); return; }
    t_hookDepth++;
    if (g_traceFrames > 0) {
        char a[96], b[96]; Desc(dst, a, sizeof a); Desc(src, b, sizeof b);
        Log("trace: CopyResource %s <- %s", a, b);
    }
    NoteCopy(src);
    g_oCopy[V](c, dst, src);
    t_hookDepth--;
}

template <int V>
static void STDMETHODCALLTYPE HCopySub(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT ss, const D3D11_BOX* box)
{
    if (t_hookDepth > 0 || g_d3dOwnCalls || c != g_imm) { g_oCopySub[V](c, dst, ds, x, y, z, src, ss, box); return; }
    t_hookDepth++;
    if (g_traceFrames > 0) {
        char a[96], b[96]; Desc(dst, a, sizeof a); Desc(src, b, sizeof b);
        if (box) Log("trace: CopySubresourceRegion %s <- %s box %u,%u-%u,%u", a, b, box->left, box->top, box->right, box->bottom);
        else Log("trace: CopySubresourceRegion %s <- %s", a, b);
    }
    NoteCopy(src);
    g_oCopySub[V](c, dst, ds, x, y, z, src, ss, box);
    t_hookDepth--;
}

template <int V>
static void STDMETHODCALLTYPE HResolveSub(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, ID3D11Resource* src, UINT ss, DXGI_FORMAT fmt)
{
    if (t_hookDepth > 0 || g_d3dOwnCalls || c != g_imm) { g_oResolveSub[V](c, dst, ds, src, ss, fmt); return; }
    t_hookDepth++;
    if (g_traceFrames > 0) {
        char a[96], b[96]; Desc(dst, a, sizeof a); Desc(src, b, sizeof b);
        Log("trace: ResolveSubresource %s <- %s fmt %d", a, b, (int)fmt);
    }
    NoteCopy(src);
    g_oResolveSub[V](c, dst, ds, src, ss, fmt);
    t_hookDepth--;
}

static void* DetourFor(int slot, int v)
{
#define CASE_VAR(V) case V: switch (slot) { \
    case 0: return (void*)HSetRT<V>; case 1: return (void*)HDraw<V>; case 2: return (void*)HDrawIdx<V>; \
    case 3: return (void*)HDrawIdxInst<V>; case 4: return (void*)HDrawInst<V>; case 5: return (void*)HClear<V>; \
    case 6: return (void*)HCopy<V>; case 7: return (void*)HCopySub<V>; case 8: return (void*)HResolveSub<V>; } break;
    switch (v) { CASE_VAR(0) CASE_VAR(1) CASE_VAR(2) CASE_VAR(3) CASE_VAR(4) CASE_VAR(5) }
#undef CASE_VAR
    return nullptr;
}

static void** OrigFor(int slot, int v)
{
    switch (slot) {
    case 0: return (void**)&g_oSetRT[v];
    case 1: return (void**)&g_oDraw[v];
    case 2: return (void**)&g_oDrawIdx[v];
    case 3: return (void**)&g_oDrawIdxInst[v];
    case 4: return (void**)&g_oDrawInst[v];
    case 5: return (void**)&g_oClear[v];
    case 6: return (void**)&g_oCopy[v];
    case 7: return (void**)&g_oCopySub[v];
    case 8: return (void**)&g_oResolveSub[v];
    }
    return nullptr;
}

// Windows d3d11.dll overwrites the function pointers inside CContext's embedded vtable in-place across multiple
// states (initial T0 -> lazy multithread T1 -> active locked T2 -> toggled-off T3). Check all 9 slots and hook
// any new function address we haven't seen yet so SteamVR/Oculus/mod SetMultithreadProtected transitions never
// bypass backbuffer capture or HUD splitting.
static int HookNewContextSlots(void** vt)
{
    int added = 0;
    for (int i = 0; i < 9; i++) {
        void* fn = vt[kSlotVtableIdx[i]];
        if (!fn) continue;
        bool seen = false;
        for (int v = 0; v < g_slotCount[i]; v++)
            if (g_slotTargets[i][v] == fn) { seen = true; break; }
        if (seen) continue;
        if (g_slotCount[i] >= kD3DVars) continue;
        int v = g_slotCount[i];
        MH_STATUS st = MH_CreateHook(fn, DetourFor(i, v), OrigFor(i, v));
        if (st == MH_OK) {
            g_slotTargets[i][v] = fn;
            g_slotCount[i]++;
            MH_EnableHook(fn);
            added++;
        } else {
            Log("d3d: hook slot %d var %d (%p) failed: %d", i, v, fn, (int)st);
        }
    }
    return added;
}

void TraceInstall(ID3D11Device* dev)
{
    if (!g_imm) {
        g_dev = dev;
        dev->AddRef();
        dev->GetImmediateContext(&g_imm);   // keep the reference: the pointer identifies dgVoodoo's context
        int n0 = HookNewContextSlots(*(void***)g_imm);

        BOOL mtPrev = FALSE, mtNow = FALSE;
        ID3D10Multithread* mt = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt)) && mt) {
            mtPrev = mt->GetMultithreadProtected();
            mt->SetMultithreadProtected(TRUE);
            mtNow = mt->GetMultithreadProtected();
            mt->Release();
        }
        ID3D10Multithread* cmt = nullptr;
        if (SUCCEEDED(g_imm->QueryInterface(__uuidof(ID3D10Multithread), (void**)&cmt)) && cmt) {
            cmt->SetMultithreadProtected(TRUE);
            cmt->Release();
        }
        int n1 = HookNewContextSlots(*(void***)g_imm);
        // Trigger d3d11's lazy T1 -> T2 transition right away if Flush promotes it, then hook any promoted slots.
        g_imm->Flush();
        int n2 = HookNewContextSlots(*(void***)g_imm);
        Log("d3d: context hooks installed (+%d/+%d/+%d, multithread %d -> %d)", n0, n1, n2, mtPrev ? 1 : 0, mtNow ? 1 : 0);
        return;
    }
    int added = HookNewContextSlots(*(void***)g_imm);
    if (added > 0) Log("d3d: context vtable updated in-place, hooked +%d slots", added);
}

// debug "eyebright N": for N drawings, the mean brightness of the middle of the scene (x 0.35-0.65,
// y 0.40-0.70 of the D3D9 backbuffer texture) is logged with its pass, to compare both eyes of one tick.
volatile long g_eyeBrightFrames;
static ID3D11Texture2D* g_brightStage;

static void LogEyeBright()
{
    ID3D11Texture2D* t = nullptr;
    if (!g_bbTex || FAILED(g_bbTex->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) return;
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    t->Release();
    if (d.SampleDesc.Count != 1 || (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
                                     d.Format != DXGI_FORMAT_B8G8R8X8_UNORM)) {
        Log("eyebright: backbuffer texture format %d samples %u not read", d.Format, d.SampleDesc.Count);
        g_eyeBrightFrames = 0;
        return;
    }
    if (g_brightStage) {
        D3D11_TEXTURE2D_DESC s;
        g_brightStage->GetDesc(&s);
        if (s.Width != d.Width || s.Height != d.Height || s.Format != d.Format) { g_brightStage->Release(); g_brightStage = nullptr; }
    }
    if (!g_brightStage) {
        D3D11_TEXTURE2D_DESC s = d;
        s.Usage = D3D11_USAGE_STAGING;
        s.BindFlags = 0;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        s.MiscFlags = 0;
        s.MipLevels = 1;
        if (FAILED(g_dev->CreateTexture2D(&s, nullptr, &g_brightStage))) return;
    }
    OrigCopySub(g_imm, g_brightStage, 0, 0, 0, 0, g_bbTex, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_imm->Map(g_brightStage, 0, D3D11_MAP_READ, 0, &m))) return;
    double sum = 0;
    long n = 0;
    for (UINT y = d.Height * 40 / 100; y < d.Height * 70 / 100; y += 2) {
        const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
        for (UINT x = d.Width * 35 / 100; x < d.Width * 65 / 100; x += 2) {
            sum += row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2];
            n++;
        }
    }
    g_imm->Unmap(g_brightStage, 0);
    bool GameDrawingRightEye();
    extern long GameFrameCount();
    Log("eyebright: frame %ld pass %d %.2f", GameFrameCount(), GameDrawingRightEye() ? 2 : 1, n ? sum / (3.0 * n) : 0.0);
}

// debug "hudsave": the HUD layers of the next split frame are saved as hud_eyes/panel/reticle.bmp in the mod's
// folder, drawn over grey (their draw counts in the log)
volatile long g_hudSave;

static void SaveLayer(HudLayer& l, const wchar_t* name)
{
    D3D11_TEXTURE2D_DESC d;
    l.tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) return;
    OrigCopy(g_imm, st, l.tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_imm->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        UINT row = d.Width * 3;
        std::vector<uint8_t> px((size_t)row * d.Height);
        for (UINT y = 0; y < d.Height; y++) {
            const uint8_t* s = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
            uint8_t* o = &px[(size_t)(d.Height - 1 - y) * row];   // (bottom-up)
            for (UINT x = 0; x < d.Width; x++, s += 4, o += 3) {
                // premultiplied over grey; coverage as the panel's shader takes it
                float r = s[0] / 255.0f, g = s[1] / 255.0f, b = s[2] / 255.0f;
                float a = std::min(1.0f, std::max(sqrtf(s[3] / 255.0f), std::max(r, std::max(g, b))));
                float bg = 0.3f * (1.0f - a);
                o[0] = (uint8_t)(std::min(1.0f, b + bg) * 255);
                o[1] = (uint8_t)(std::min(1.0f, g + bg) * 255);
                o[2] = (uint8_t)(std::min(1.0f, r + bg) * 255);
            }
        }
        g_imm->Unmap(st, 0);
        BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + px.size()), 0, 0,
                                sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) };
        BITMAPINFOHEADER ih = { sizeof(BITMAPINFOHEADER), (LONG)d.Width, (LONG)d.Height, 1, 24, BI_RGB };
        FILE* f = nullptr;
        if (!_wfopen_s(&f, (ModDir() + name).c_str(), L"wb") && f) {
            fwrite(&fh, sizeof fh, 1, f);
            fwrite(&ih, sizeof ih, 1, f);
            fwrite(px.data(), 1, px.size(), f);
            fclose(f);
        }
    }
    st->Release();
}

// Called at Present: closes the frame; gives the layers this frame's HUD was split into (null: nothing drawn there
// or no split) and the game's picture of it, the D3D9 backbuffer texture (null: not seen in this frame).
void TraceEndFrame(ID3D11ShaderResourceView* layers[kHudLayers], ID3D11Resource** gameBB)
{
    if (g_eyeBrightFrames > 0) {
        g_eyeBrightFrames--;
        LogEyeBright();
    }
    if (g_traceFrames > 0) {
        bool GameDrawingRightEye();
        Log("trace:   (%d draws)", g_drawsOnRT);
        Log("trace: ===== Present (pass %d, %ld draws in all) =====", GameDrawingRightEye() ? 2 : 1, g_traceDraws);
        g_drawsOnRT = 0;
        g_traceDraws = 0;
        g_traceFrames--;
    }
    for (int i = 0; i < kHudLayers; i++) layers[i] = g_hudActive && g_layers[i].draws ? g_layers[i].srv : nullptr;
    // The HUD movie drew but nothing went to the layers (its draws stayed in the eye images): why, now and then.
    if (g_hudActive && g_movieDraws && !g_layers[kHudEyes].draws && !g_layers[kHudPanel].draws && !g_layers[kHudReticle].draws) {
        static DWORD at;
        static int times;
        if (times < 8 && (!times || GetTickCount() - at > 20000)) {
            at = GetTickCount();
            times++;
            char m[96], b[96];
            Desc(g_movieRT, m, sizeof m);
            Desc(g_bbTex, b, sizeof b);
            Log("hud: the HUD movie drew %ld times into %s, kept in the eyes: the backbuffer texture %s, %d draws on it%s",
                g_movieDraws, m, b, g_drawsOnBB, g_layersFailed ? ", the layers could not be made" : "");
        }
    }
    g_movieRT = nullptr;
    g_movieDraws = 0;
    if (g_hudSave && g_hudActive && g_layers[0].tex) {
        g_hudSave = 0;
        bool GameDrawingRightEye();
        Log("hud: saved (pass %d): eyes %ld draws, panel %ld, reticle %ld", GameDrawingRightEye() ? 2 : 1, g_layers[kHudEyes].draws,
            g_layers[kHudPanel].draws, g_layers[kHudReticle].draws);
        SaveLayer(g_layers[kHudEyes], L"hud_eyes.bmp");
        SaveLayer(g_layers[kHudPanel], L"hud_panel.bmp");
        SaveLayer(g_layers[kHudReticle], L"hud_reticle.bmp");
    }
    *gameBB = (g_copySeen || g_bbDrawnThisFrame) ? g_bbTex : nullptr;
    g_hudActive = g_hudWanted;
    g_copySeen = false;
    g_copyExact = false;
    g_bbDrawnThisFrame = false;
    g_drawnN = 0;
    g_drawsOnBB = 0;
    g_boundLayer = -1;
    g_hudLayer = kHudEyes;
    g_hudMovie = false;
    for (HudLayer& l : g_layers) { l.cleared = false; l.draws = 0; }
    g_curRT = nullptr;
}

void HudSetWanted(bool on) { g_hudWanted = on; }
