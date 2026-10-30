// D3D11-level work on dgVoodoo's immediate context:
// - research trace of one frame ("d3dtrace N" debug command);
// - HUD split: the game draws its 3D scene into dgVoodoo's D3D9-backbuffer texture with one final
//   full-screen pass and then the 2D HUD on top. While the VR gameplay view is active, every draw into that
//   texture after the first one is redirected into our own transparent texture. The eyes then get the scene
//   without HUD, and vr.cpp shows the HUD texture as a floating panel.
#include "common.h"
#include <d3d11.h>
#include "MinHook.h"

typedef void(STDMETHODCALLTYPE* PFN_OMSetRT)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
typedef void(STDMETHODCALLTYPE* PFN_Draw)(ID3D11DeviceContext*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexed)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInst)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawInst)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_ClearRTV)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
typedef void(STDMETHODCALLTYPE* PFN_CopyRes)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
typedef void(STDMETHODCALLTYPE* PFN_CopySub)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
static PFN_OMSetRT g_oSetRT;
static PFN_Draw g_oDraw;
static PFN_DrawIndexed g_oDrawIdx;
static PFN_DrawIndexedInst g_oDrawIdxInst;
static PFN_DrawInst g_oDrawInst;
static PFN_ClearRTV g_oClear;
static PFN_CopyRes g_oCopy;
static PFN_CopySub g_oCopySub;

volatile long g_traceFrames;          // frames left to trace (set by debug command)
static ID3D11DeviceContext* g_imm;    // dgVoodoo's immediate context (our own deferred contexts are ignored)
static ID3D11Device* g_dev;
static ID3D11Resource* g_curRT;       // resource of the bound render target 0 (not AddRef'd)
static ID3D11DepthStencilView* g_curDSV;
static int g_drawsOnRT;
static long g_traceDraws;             // draws of the traced frame

// ---- HUD split state ----
static volatile bool g_hudWanted;     // set by vr.cpp for the coming frame
static bool g_hudActive;              // redirect is active in this frame
static ID3D11Resource* g_bbTex;       // dgVoodoo's D3D9 backbuffer texture (source of the first copy each frame)
static bool g_copySeen;
static int g_drawsOnBB;
static bool g_hudBound, g_hudCleared, g_hudHadDraws;
static ID3D11Texture2D* g_hudTex;
static ID3D11RenderTargetView* g_hudRTV;
static ID3D11ShaderResourceView* g_hudSRV;
static UINT g_hudW, g_hudH;

static bool TexSize(ID3D11Resource* r, UINT& w, UINT& h)
{
    ID3D11Texture2D* t = nullptr;
    if (!r || FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) return false;
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    t->Release();
    w = d.Width;
    h = d.Height;
    return true;
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

static bool EnsureHudTex(UINT w, UINT h)
{
    if (g_hudTex && g_hudW == w && g_hudH == h) return true;
    if (g_hudSRV) { g_hudSRV->Release(); g_hudSRV = nullptr; }
    if (g_hudRTV) { g_hudRTV->Release(); g_hudRTV = nullptr; }
    if (g_hudTex) { g_hudTex->Release(); g_hudTex = nullptr; }
    D3D11_TEXTURE2D_DESC d = { w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_hudTex))) return false;
    g_dev->CreateRenderTargetView(g_hudTex, nullptr, &g_hudRTV);
    g_dev->CreateShaderResourceView(g_hudTex, nullptr, &g_hudSRV);
    g_hudW = w;
    g_hudH = h;
    Log("hud: texture %ux%u", w, h);
    return g_hudRTV && g_hudSRV;
}

// Before a draw into the backbuffer texture: after the scene's final pass, send the draw to the HUD texture.
static void BeforeDraw(ID3D11DeviceContext* c)
{
    if (c != g_imm || !g_hudActive || g_curRT != g_bbTex || !g_bbTex || g_drawsOnBB < 1 || g_hudBound) return;
    UINT w, h;
    if (!TexSize(g_bbTex, w, h) || !EnsureHudTex(w, h)) return;
    if (!g_hudCleared) {
        const FLOAT zero[4] = { 0, 0, 0, 0 };
        g_oClear(c, g_hudRTV, zero);
        g_hudCleared = true;
    }
    g_oSetRT(c, 1, &g_hudRTV, g_curDSV);
    g_hudBound = true;
    g_hudHadDraws = true;
}

static void AfterDraw(ID3D11DeviceContext* c)
{
    if (c == g_imm && g_traceFrames > 0) g_traceDraws++;
    if (c == g_imm && g_curRT && g_curRT == g_bbTex) g_drawsOnBB++;
}

static void STDMETHODCALLTYPE HSetRT(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv)
{
    if (c == g_imm) {
        ID3D11Resource* r = nullptr;
        if (n && rtv && rtv[0]) { rtv[0]->GetResource(&r); r->Release(); }
        if (g_traceFrames > 0 && r != g_curRT) {
            if (g_curRT) Log("trace:   (%d draws)", g_drawsOnRT);
            char d[96]; Desc(r, d, sizeof d);
            Log("trace: RT %s dsv %p", d, dsv);
            g_drawsOnRT = 0;
        }
        g_curRT = r;
        g_curDSV = dsv;
        g_hudBound = false;
    }
    g_oSetRT(c, n, rtv, dsv);
}

static void STDMETHODCALLTYPE HDraw(ID3D11DeviceContext* c, UINT vc, UINT sv)
{
    if (c == g_imm && g_traceFrames > 0 && ++g_drawsOnRT <= 3) Log("trace:   Draw %u", vc);
    BeforeDraw(c);
    g_oDraw(c, vc, sv);
    AfterDraw(c);
}

static void STDMETHODCALLTYPE HDrawIdx(ID3D11DeviceContext* c, UINT ic, UINT si, INT bv)
{
    if (c == g_imm && g_traceFrames > 0 && ++g_drawsOnRT <= 3) Log("trace:   DrawIndexed %u", ic);
    BeforeDraw(c);
    g_oDrawIdx(c, ic, si, bv);
    AfterDraw(c);
}

static void STDMETHODCALLTYPE HDrawIdxInst(ID3D11DeviceContext* c, UINT ic, UINT inst, UINT si, INT bv, UINT sinst)
{
    BeforeDraw(c);
    g_oDrawIdxInst(c, ic, inst, si, bv, sinst);
    AfterDraw(c);
}

static void STDMETHODCALLTYPE HDrawInst(ID3D11DeviceContext* c, UINT vc, UINT inst, UINT sv, UINT sinst)
{
    BeforeDraw(c);
    g_oDrawInst(c, vc, inst, sv, sinst);
    AfterDraw(c);
}

static void STDMETHODCALLTYPE HClear(ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv, const FLOAT col[4])
{
    if (c == g_imm && g_traceFrames > 0) {
        ID3D11Resource* r = nullptr;
        rtv->GetResource(&r);
        char d[96]; Desc(r, d, sizeof d);
        Log("trace: Clear %s (%.2f %.2f %.2f %.2f)", d, col[0], col[1], col[2], col[3]);
        if (r) r->Release();
    }
    g_oClear(c, rtv, col);
}

volatile bool g_d3dOwnCalls;   // set by vr.cpp while it issues its own copies on the immediate context

static void STDMETHODCALLTYPE HCopy(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src)
{
    if (c == g_imm && !g_d3dOwnCalls) {
        if (g_traceFrames > 0) {
            char a[96], b[96]; Desc(dst, a, sizeof a); Desc(src, b, sizeof b);
            Log("trace: CopyResource %s <- %s", a, b);
        }
        // dgVoodoo's present path starts by copying the D3D9 backbuffer texture.
        if (!g_copySeen) {
            g_copySeen = true;
            if (src != g_bbTex) { g_bbTex = src; Log("hud: backbuffer texture %p", src); }
        }
    }
    g_oCopy(c, dst, src);
}

static void STDMETHODCALLTYPE HCopySub(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT ss, const D3D11_BOX* box)
{
    if (c == g_imm && g_traceFrames > 0) {
        char a[96], b[96]; Desc(dst, a, sizeof a); Desc(src, b, sizeof b);
        Log("trace: CopySubresourceRegion %s <- %s", a, b);
    }
    g_oCopySub(c, dst, ds, x, y, z, src, ss, box);
}

void TraceInstall(ID3D11Device* dev)
{
    static bool done;
    if (done) return;
    done = true;
    g_dev = dev;
    dev->AddRef();
    dev->GetImmediateContext(&g_imm);   // keep the reference: the pointer identifies dgVoodoo's context
    void** vt = *(void***)g_imm;
    MH_CreateHook(vt[33], (void*)HSetRT, (void**)&g_oSetRT);              // OMSetRenderTargets
    MH_CreateHook(vt[13], (void*)HDraw, (void**)&g_oDraw);                // Draw
    MH_CreateHook(vt[12], (void*)HDrawIdx, (void**)&g_oDrawIdx);          // DrawIndexed
    MH_CreateHook(vt[20], (void*)HDrawIdxInst, (void**)&g_oDrawIdxInst);  // DrawIndexedInstanced
    MH_CreateHook(vt[21], (void*)HDrawInst, (void**)&g_oDrawInst);        // DrawInstanced
    MH_CreateHook(vt[50], (void*)HClear, (void**)&g_oClear);              // ClearRenderTargetView
    MH_CreateHook(vt[47], (void*)HCopy, (void**)&g_oCopy);                // CopyResource
    MH_CreateHook(vt[46], (void*)HCopySub, (void**)&g_oCopySub);          // CopySubresourceRegion
    MH_EnableHook(MH_ALL_HOOKS);
    Log("d3d: context hooks installed");
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
    g_oCopySub(g_imm, g_brightStage, 0, 0, 0, 0, g_bbTex, 0, nullptr);
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

// Called at Present: closes the frame; returns the HUD texture if this frame's HUD was redirected.
ID3D11ShaderResourceView* TraceEndFrame()
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
    ID3D11ShaderResourceView* hud = (g_hudActive && g_hudHadDraws) ? g_hudSRV : nullptr;
    g_hudActive = g_hudWanted;
    g_copySeen = false;
    g_drawsOnBB = 0;
    g_hudBound = false;
    g_hudCleared = false;
    g_hudHadDraws = false;
    g_curRT = nullptr;
    return hud;
}

void HudSetWanted(bool on) { g_hudWanted = on; }
