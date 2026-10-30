// VR output (OpenXR, D3D11) and VR controller input.
// The game renders D3D9 -> dgVoodoo2 -> D3D11; we hook the final IDXGISwapChain::Present, copy the
// back buffer into an OpenXR swapchain and show it as a virtual screen (theater mode).
#include "common.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <vector>
#include <atomic>
#include "MinHook.h"

#define XR_CHECK(call) do { XrResult _r = (call); if (XR_FAILED(_r)) { Log("vr: %s failed: %d", #call, (int)_r); return false; } } while (0)

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------------------------------------
// Shared pad state
static std::mutex g_padMutex;
static VRPadState g_pad;

void VRGetPad(VRPadState& out)
{
    std::lock_guard<std::mutex> lock(g_padMutex);
    out = g_pad;
}

// ---------------------------------------------------------------------------------------------
// Blit: back buffer -> OpenXR swapchain image (handles format / size / gamma differences)
static const char* kBlitHLSL = R"(
Texture2D src : register(t0);
SamplerState smp : register(s0);
cbuffer C : register(b0) { float linearize; float3 pad; };
void VS(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 c = src.Sample(smp, uv).rgb;
    if (linearize > 0.5) c = pow(abs(c), 2.2);
    return float4(c, 1);
}
)";

struct Blitter {
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11SamplerState* smp = nullptr;
    ID3D11Buffer* cb[2] = {};   // [0] = passthrough, [1] = linearize

    bool Init(ID3D11Device* dev)
    {
        HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
        if (!compile) { Log("vr: d3dcompiler_47.dll not available"); return false; }
        ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
        if (FAILED(compile(kBlitHLSL, strlen(kBlitHLSL), "blit", nullptr, nullptr, "VS", "vs_4_0", 0, 0, &vsb, &err)) ||
            FAILED(compile(kBlitHLSL, strlen(kBlitHLSL), "blit", nullptr, nullptr, "PS", "ps_4_0", 0, 0, &psb, &err))) {
            Log("vr: shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
            SafeRelease(err); SafeRelease(vsb); SafeRelease(psb);
            return false;
        }
        dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
        dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
        vsb->Release(); psb->Release();
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &smp);
        for (int i = 0; i < 2; i++) {
            float data[4] = { (float)i, 0, 0, 0 };
            D3D11_BUFFER_DESC bd = { 16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
            D3D11_SUBRESOURCE_DATA init = { data, 0, 0 };
            dev->CreateBuffer(&bd, &init, &cb[i]);
        }
        return vs && ps && smp && cb[0] && cb[1];
    }

    // Draws on a deferred context so the game's immediate-context state is untouched.
    void Draw(ID3D11Device* dev, ID3D11DeviceContext* imm, ID3D11ShaderResourceView* srv, ID3D11RenderTargetView* rtv,
              UINT w, UINT h, bool linearize)
    {
        ID3D11DeviceContext* dc = nullptr;
        if (FAILED(dev->CreateDeferredContext(0, &dc))) return;
        D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
        dc->RSSetViewports(1, &vp);
        dc->OMSetRenderTargets(1, &rtv, nullptr);
        dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        dc->VSSetShader(vs, nullptr, 0);
        dc->PSSetShader(ps, nullptr, 0);
        dc->PSSetShaderResources(0, 1, &srv);
        dc->PSSetSamplers(0, 1, &smp);
        dc->PSSetConstantBuffers(0, 1, &cb[linearize ? 1 : 0]);
        dc->Draw(3, 0);
        ID3D11CommandList* cl = nullptr;
        if (SUCCEEDED(dc->FinishCommandList(FALSE, &cl))) {
            imm->ExecuteCommandList(cl, TRUE);
            cl->Release();
        }
        dc->Release();
    }
};

// ---------------------------------------------------------------------------------------------
// OpenXR session
enum Btn { kA, kB, kX, kY, kLClick, kRClick, kMenu, kBtnCount };

struct XR {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    int64_t format = 0;
    bool linearize = false;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    bool failed = false;

    ID3D11Device* device = nullptr;
    Blitter blit;

    // input
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction stick[2] = {}, trigger[2] = {}, grip[2] = {}, btn[kBtnCount] = {};
    XrPath hand[2] = {};
    DWORD menuDownAt = 0;
    bool menuWasDown = false;
    int backPulse = 0, startPulse = 0;
};
static XR xr;

static XrPath Path(const char* s)
{
    XrPath p = XR_NULL_PATH;
    xrStringToPath(xr.instance, s, &p);
    return p;
}

static bool CreateAction(XrActionType type, const char* name, XrAction* out, bool perHand)
{
    XrActionCreateInfo ci = { XR_TYPE_ACTION_CREATE_INFO };
    ci.actionType = type;
    strcpy_s(ci.actionName, name);
    strcpy_s(ci.localizedActionName, name);
    if (perHand) { ci.countSubactionPaths = 2; ci.subactionPaths = xr.hand; }
    XR_CHECK(xrCreateAction(xr.actionSet, &ci, out));
    return true;
}

static void SuggestBindings(const char* profile, const std::vector<std::pair<XrAction, const char*>>& b)
{
    std::vector<XrActionSuggestedBinding> sb;
    for (auto& e : b) sb.push_back({ e.first, Path(e.second) });
    XrInteractionProfileSuggestedBinding s = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    s.interactionProfile = Path(profile);
    s.suggestedBindings = sb.data();
    s.countSuggestedBindings = (uint32_t)sb.size();
    XrResult r = xrSuggestInteractionProfileBindings(xr.instance, &s);
    Log("vr: bindings for %s: %d", profile, (int)r);
}

static bool InitInput()
{
    XrActionSetCreateInfo ai = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy_s(ai.actionSetName, "gameplay");
    strcpy_s(ai.localizedActionSetName, "Gameplay");
    XR_CHECK(xrCreateActionSet(xr.instance, &ai, &xr.actionSet));
    xr.hand[0] = Path("/user/hand/left");
    xr.hand[1] = Path("/user/hand/right");
    const char* names[2] = { "left", "right" };
    for (int h = 0; h < 2; h++) {
        char n[64];
        sprintf_s(n, "%s_stick", names[h]);   if (!CreateAction(XR_ACTION_TYPE_VECTOR2F_INPUT, n, &xr.stick[h], false)) return false;
        sprintf_s(n, "%s_trigger", names[h]); if (!CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, n, &xr.trigger[h], false)) return false;
        sprintf_s(n, "%s_grip", names[h]);    if (!CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, n, &xr.grip[h], false)) return false;
    }
    const char* bn[kBtnCount] = { "button_a", "button_b", "button_x", "button_y", "left_stick_click", "right_stick_click", "menu" };
    for (int i = 0; i < kBtnCount; i++)
        if (!CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, bn[i], &xr.btn[i], false)) return false;

    SuggestBindings("/interaction_profiles/oculus/touch_controller", {
        { xr.stick[0], "/user/hand/left/input/thumbstick" }, { xr.stick[1], "/user/hand/right/input/thumbstick" },
        { xr.trigger[0], "/user/hand/left/input/trigger/value" }, { xr.trigger[1], "/user/hand/right/input/trigger/value" },
        { xr.grip[0], "/user/hand/left/input/squeeze/value" }, { xr.grip[1], "/user/hand/right/input/squeeze/value" },
        { xr.btn[kA], "/user/hand/right/input/a/click" }, { xr.btn[kB], "/user/hand/right/input/b/click" },
        { xr.btn[kX], "/user/hand/left/input/x/click" }, { xr.btn[kY], "/user/hand/left/input/y/click" },
        { xr.btn[kLClick], "/user/hand/left/input/thumbstick/click" }, { xr.btn[kRClick], "/user/hand/right/input/thumbstick/click" },
        { xr.btn[kMenu], "/user/hand/left/input/menu/click" },
    });
    SuggestBindings("/interaction_profiles/valve/index_controller", {
        { xr.stick[0], "/user/hand/left/input/thumbstick" }, { xr.stick[1], "/user/hand/right/input/thumbstick" },
        { xr.trigger[0], "/user/hand/left/input/trigger/value" }, { xr.trigger[1], "/user/hand/right/input/trigger/value" },
        { xr.grip[0], "/user/hand/left/input/squeeze/value" }, { xr.grip[1], "/user/hand/right/input/squeeze/value" },
        { xr.btn[kA], "/user/hand/right/input/a/click" }, { xr.btn[kB], "/user/hand/right/input/b/click" },
        { xr.btn[kX], "/user/hand/left/input/a/click" }, { xr.btn[kY], "/user/hand/left/input/b/click" },
        { xr.btn[kLClick], "/user/hand/left/input/thumbstick/click" }, { xr.btn[kRClick], "/user/hand/right/input/thumbstick/click" },
        { xr.btn[kMenu], "/user/hand/left/input/trackpad/force" },
    });

    XrSessionActionSetsAttachInfo at = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    at.countActionSets = 1;
    at.actionSets = &xr.actionSet;
    XR_CHECK(xrAttachSessionActionSets(xr.session, &at));
    return true;
}

static float GetFloat(XrAction a)
{
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateFloat s = { XR_TYPE_ACTION_STATE_FLOAT };
    xrGetActionStateFloat(xr.session, &gi, &s);
    return s.isActive ? s.currentState : 0.0f;
}

static bool GetBool(XrAction a)
{
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateBoolean s = { XR_TYPE_ACTION_STATE_BOOLEAN };
    xrGetActionStateBoolean(xr.session, &gi, &s);
    return s.isActive && s.currentState;
}

static XrVector2f GetStick(XrAction a)
{
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateVector2f s = { XR_TYPE_ACTION_STATE_VECTOR2F };
    xrGetActionStateVector2f(xr.session, &gi, &s);
    return s.isActive ? s.currentState : XrVector2f{ 0, 0 };
}

static SHORT AxisToShort(float v)
{
    float dz = g_cfg.stickDeadzone;
    float a = fabsf(v);
    if (a < dz) return 0;
    a = (a - dz) / (1.0f - dz);
    if (a > 1.0f) a = 1.0f;
    return (SHORT)((v < 0 ? -a : a) * 32767.0f);
}

static void UpdateInput()
{
    if (!xr.running || !xr.actionSet) return;
    XrActiveActionSet as = { xr.actionSet, XR_NULL_PATH };
    XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
    si.countActiveActionSets = 1;
    si.activeActionSets = &as;
    if (XR_FAILED(xrSyncActions(xr.session, &si))) return;

    XINPUT_GAMEPAD p = {};
    XrVector2f ls = GetStick(xr.stick[0]), rs = GetStick(xr.stick[1]);
    p.sThumbLX = AxisToShort(ls.x); p.sThumbLY = AxisToShort(ls.y);
    p.sThumbRX = AxisToShort(rs.x); p.sThumbRY = AxisToShort(rs.y);
    p.bLeftTrigger = (BYTE)(GetFloat(xr.trigger[0]) * 255.0f);
    p.bRightTrigger = (BYTE)(GetFloat(xr.trigger[1]) * 255.0f);
    if (GetFloat(xr.grip[0]) > 0.6f) p.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (GetFloat(xr.grip[1]) > 0.6f) p.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (GetBool(xr.btn[kA])) p.wButtons |= XINPUT_GAMEPAD_A;
    if (GetBool(xr.btn[kB])) p.wButtons |= XINPUT_GAMEPAD_B;
    if (GetBool(xr.btn[kX])) p.wButtons |= XINPUT_GAMEPAD_X;
    if (GetBool(xr.btn[kY])) p.wButtons |= XINPUT_GAMEPAD_Y;
    if (GetBool(xr.btn[kLClick])) p.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (GetBool(xr.btn[kRClick])) p.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;

    // Menu button: short press = Start, hold 0.6 s = Back.
    bool menu = GetBool(xr.btn[kMenu]);
    DWORD now = GetTickCount();
    if (menu && !xr.menuWasDown) xr.menuDownAt = now;
    if (menu && now - xr.menuDownAt > 600 && xr.menuDownAt) { xr.backPulse = 8; xr.menuDownAt = 0; }
    if (!menu && xr.menuWasDown && xr.menuDownAt) xr.startPulse = 8;
    xr.menuWasDown = menu;
    if (xr.startPulse > 0) { p.wButtons |= XINPUT_GAMEPAD_START; xr.startPulse--; }
    if (xr.backPulse > 0) { p.wButtons |= XINPUT_GAMEPAD_BACK; xr.backPulse--; }

    std::lock_guard<std::mutex> lock(g_padMutex);
    g_pad.active = true;
    g_pad.pad = p;
}

static bool g_retryLater;   // headset not available yet: try again in a few seconds

static bool InitXR(ID3D11Device* dev)
{
    g_retryLater = false;
    if (!xr.instance) {
        const char* exts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
        XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
        strcpy_s(ici.applicationInfo.applicationName, "TWFC VR");
        strcpy_s(ici.applicationInfo.engineName, "UnrealEngine3");
        ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        ici.enabledExtensionCount = 1;
        ici.enabledExtensionNames = exts;
        XrResult r = xrCreateInstance(&ici, &xr.instance);
        if (XR_FAILED(r)) {
            // No runtime running (e.g. Virtual Desktop streamer closed) - keep polling.
            Log("vr: xrCreateInstance failed: %d (will retry)", (int)r);
            xr.instance = XR_NULL_HANDLE;
            g_retryLater = true;
            return false;
        }
        XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
        xrGetInstanceProperties(xr.instance, &ip);
        Log("vr: runtime %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
            XR_VERSION_PATCH(ip.runtimeVersion));
    }

    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult sr = xrGetSystem(xr.instance, &sgi, &xr.system);
    if (sr == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        static bool logged;
        if (!logged) Log("vr: headset not connected yet - the game runs flat until it is");
        logged = true;
        g_retryLater = true;
        return false;
    }
    XR_CHECK(sr);
    Log("vr: headset found");

    PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq));
    XrGraphicsRequirementsD3D11KHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    XR_CHECK(getReq(xr.instance, xr.system, &req));

    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC ad = {};
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&adapter)))
        adapter->GetDesc(&ad);
    SafeRelease(adapter); SafeRelease(dxgiDev);
    if (memcmp(&ad.AdapterLuid, &req.adapterLuid, sizeof(LUID)) != 0)
        Log("vr: WARNING game GPU (%ls) is not the headset GPU", ad.Description);

    XrGraphicsBindingD3D11KHR gb = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    gb.device = dev;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &gb;
    sci.systemId = xr.system;
    XR_CHECK(xrCreateSession(xr.instance, &sci, &xr.session));

    XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    XR_CHECK(xrCreateReferenceSpace(xr.session, &rs, &xr.localSpace));

    xr.device = dev;
    dev->AddRef();
    if (!xr.blit.Init(dev)) return false;
    if (!InitInput()) Log("vr: input init failed - controllers disabled");
    Log("vr: session created");
    return true;
}

// One OpenXR swapchain plus render target views (theater screen, left eye, right eye).
struct Chain {
    XrSwapchain sc = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ID3D11RenderTargetView*> rtvs;
    uint32_t width = 0, height = 0;
};
static Chain g_theater, g_eyes[2];

static bool PickFormat()
{
    if (xr.format) return true;
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(xr.session, 0, &n, nullptr);
    std::vector<int64_t> fmts(n);
    xrEnumerateSwapchainFormats(xr.session, n, &n, fmts.data());
    // Prefer sRGB formats: the game's back buffer holds gamma-encoded colour, we write it through a UNORM view.
    const int64_t prefer[] = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM,
                               DXGI_FORMAT_B8G8R8A8_UNORM };
    for (int64_t p : prefer) {
        for (int64_t f : fmts) if (f == p) { xr.format = f; break; }
        if (xr.format) break;
    }
    if (!xr.format) Log("vr: no usable swapchain format");
    return xr.format != 0;
}

static void DestroyChain(Chain& c)
{
    for (auto* r : c.rtvs) SafeRelease(r);
    c.rtvs.clear();
    c.images.clear();
    if (c.sc) { xrDestroySwapchain(c.sc); c.sc = XR_NULL_HANDLE; }
    c.width = c.height = 0;
}

static bool CreateChain(Chain& c, UINT w, UINT h)
{
    DestroyChain(c);
    if (!PickFormat()) return false;
    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = xr.format;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(xr.session, &ci, &c.sc));
    uint32_t n = 0;
    xrEnumerateSwapchainImages(c.sc, 0, &n, nullptr);
    c.images.assign(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    XR_CHECK(xrEnumerateSwapchainImages(c.sc, n, &n, (XrSwapchainImageBaseHeader*)c.images.data()));

    DXGI_FORMAT unorm = xr.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM
                      : xr.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? DXGI_FORMAT_B8G8R8A8_UNORM : (DXGI_FORMAT)xr.format;
    xr.linearize = false;
    for (auto& img : c.images) {
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = unorm;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11RenderTargetView* rtv = nullptr;
        if (FAILED(xr.device->CreateRenderTargetView(img.texture, &rd, &rtv))) {
            // Texture is not typeless: write through the sRGB view and linearise in the shader instead.
            rd.Format = (DXGI_FORMAT)xr.format;
            xr.linearize = true;
            xr.device->CreateRenderTargetView(img.texture, &rd, &rtv);
        }
        c.rtvs.push_back(rtv);
    }
    c.width = w;
    c.height = h;
    Log("vr: swapchain %ux%u format %d images %u linearize %d", w, h, (int)xr.format, n, xr.linearize);
    return true;
}

// Copies the game's back buffer into the next image of a swapchain.
static bool CopyToChain(Chain& c, ID3D11Texture2D* bb)
{
    D3D11_TEXTURE2D_DESC bd;
    bb->GetDesc(&bd);
    if (!c.sc || c.width != bd.Width || c.height != bd.Height)
        if (!CreateChain(c, bd.Width, bd.Height)) return false;

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrAcquireSwapchainImage(c.sc, &ai, &idx))) return false;
    if (XR_FAILED(xrWaitSwapchainImage(c.sc, &wi))) return false;

    ID3D11DeviceContext* imm = nullptr;
    xr.device->GetImmediateContext(&imm);
    ID3D11Texture2D* src = bb;
    ID3D11Texture2D* resolved = nullptr;
    if (bd.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC rd = bd;
        rd.SampleDesc = { 1, 0 };
        rd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        rd.MiscFlags = 0;
        if (SUCCEEDED(xr.device->CreateTexture2D(&rd, nullptr, &resolved))) {
            imm->ResolveSubresource(resolved, 0, bb, 0, bd.Format);
            src = resolved;
        }
    }
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11Texture2D* copy = nullptr;
    if (FAILED(xr.device->CreateShaderResourceView(src, nullptr, &srv))) {
        // Back buffer not bindable as SRV: copy it into a sampleable texture first.
        D3D11_TEXTURE2D_DESC cd = bd;
        cd.SampleDesc = { 1, 0 };
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        cd.MiscFlags = 0;
        cd.Usage = D3D11_USAGE_DEFAULT;
        if (SUCCEEDED(xr.device->CreateTexture2D(&cd, nullptr, &copy))) {
            imm->CopyResource(copy, src);
            xr.device->CreateShaderResourceView(copy, nullptr, &srv);
        }
    }
    if (srv) xr.blit.Draw(xr.device, imm, srv, c.rtvs[idx], c.width, c.height, xr.linearize);
    SafeRelease(srv); SafeRelease(copy); SafeRelease(resolved); SafeRelease(imm);
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(c.sc, &ri);
    return true;
}


static void PollEvents()
{
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(xr.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* s = (XrEventDataSessionStateChanged*)&ev;
            xr.state = s->state;
            Log("vr: session state %d", (int)s->state);
            if (s->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                xr.running = XR_SUCCEEDED(xrBeginSession(xr.session, &bi));
            } else if (s->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(xr.session);
                xr.running = false;
                std::lock_guard<std::mutex> lock(g_padMutex);
                g_pad.active = false;
            } else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
                xr.running = false;
                xr.failed = true;   // keep the game running flat
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            xr.running = false;
            xr.failed = true;
        }
        ev = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

// ---------------------------------------------------------------------------------------------
// Stereo (alternate-eye rendering). The game thread picks an eye and pose per game frame
// (VRBeginGameFrame); the present thread receives the frames in the same order and submits a
// projection layer once both eyes have a fresh image.
#include <deque>

struct EyeFrame {
    int eye;
    XrPosef pose;
    XrFovf fov;
};
static std::mutex g_stereoMutex;
static std::deque<EyeFrame> g_pending;
static std::atomic<int64_t> g_lastDisplayTime;
static std::atomic<float> g_aspect{ 16.0f / 9.0f };
static int g_nextEye;
static EyeFrame g_eyeDone[2];
static bool g_eyeHave[2];

bool VRIsRunning() { return xr.running; }

// Debug: synthetic head pose (no headset needed) to verify the camera path. Yaw/pitch come from the
// debug script ("fakehead <yawDeg> <pitchDeg>").
float g_fakeHeadYaw, g_fakeHeadPitch;

static bool FakeHeadView(VREyeView& out)
{
    int eye = g_nextEye;
    g_nextEye ^= 1;
    // OpenXR yaw is about +Y; positive yaw turns left. Pitch about +X; positive pitch looks up.
    float hy = -g_fakeHeadYaw * 0.0174533f * 0.5f, hp = g_fakeHeadPitch * 0.0174533f * 0.5f;
    // q = qYaw * qPitch
    float cy = cosf(hy), sy = sinf(hy), cp = cosf(hp), sp = sinf(hp);
    out.qx = cy * sp; out.qy = sy * cp; out.qz = -sy * sp; out.qw = cy * cp;
    out.eye = eye;
    out.px = eye ? 0.032f : -0.032f; out.py = 0.0f; out.pz = 0.0f;
    out.renderFovDeg = 100.0f;
    return true;
}

bool VRBeginGameFrame(VREyeView& out)
{
    if (g_cfg.fakeHmd) return FakeHeadView(out);
    if (g_cfg.vrMode != 1 || !xr.running || !g_lastDisplayTime.load()) return false;
    XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO };
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = g_lastDisplayTime.load();
    li.space = xr.localSpace;
    XrViewState vs = { XR_TYPE_VIEW_STATE };
    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    uint32_t n = 0;
    if (XR_FAILED(xrLocateViews(xr.session, &li, &vs, 2, &n, views)) || n != 2 ||
        !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        return false;

    // Symmetric frustum covering both eyes' FOV at the back buffer's aspect ratio.
    float aspect = g_aspect.load();
    float tanH = 0.0f, tanV = 0.0f;
    for (auto& v : views) {
        tanH = std::max(tanH, std::max(tanf(-v.fov.angleLeft), tanf(v.fov.angleRight)));
        tanV = std::max(tanV, std::max(tanf(v.fov.angleUp), tanf(-v.fov.angleDown)));
    }
    tanH = std::max(tanH, tanV * aspect) * g_cfg.fovScale;
    float half = atanf(tanH);
    if (half > 1.39f) half = 1.39f;   // ~160 degrees total
    tanH = tanf(half);

    int eye = g_nextEye;
    g_nextEye ^= 1;
    EyeFrame f;
    f.eye = eye;
    f.pose = views[eye].pose;
    f.fov.angleLeft = -half;
    f.fov.angleRight = half;
    f.fov.angleUp = atanf(tanH / aspect);
    f.fov.angleDown = -f.fov.angleUp;
    {
        std::lock_guard<std::mutex> lock(g_stereoMutex);
        g_pending.push_back(f);
        while (g_pending.size() > 4) g_pending.pop_front();   // presents fell behind (loading etc.)
    }
    out.eye = eye;
    out.px = f.pose.position.x; out.py = f.pose.position.y; out.pz = f.pose.position.z;
    out.qx = f.pose.orientation.x; out.qy = f.pose.orientation.y; out.qz = f.pose.orientation.z; out.qw = f.pose.orientation.w;
    out.renderFovDeg = half * 2.0f * 57.2957795f;
    return true;
}

static void ResetStereo()
{
    std::lock_guard<std::mutex> lock(g_stereoMutex);
    g_pending.clear();
    g_eyeHave[0] = g_eyeHave[1] = false;
}

static bool WaitAndBegin(XrFrameState& fs)
{
    XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
    fs = { XR_TYPE_FRAME_STATE };
    if (XR_FAILED(xrWaitFrame(xr.session, &wi, &fs))) return false;
    g_lastDisplayTime = fs.predictedDisplayTime;
    XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
    xrBeginFrame(xr.session, &bi);
    return true;
}

static void EndFrame(const XrFrameState& fs, const XrCompositionLayerBaseHeader* layer)
{
    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layer ? 1 : 0;
    ei.layers = layer ? &layer : nullptr;
    xrEndFrame(xr.session, &ei);
}

static void PresentTheater(ID3D11Texture2D* bb)
{
    XrFrameState fs;
    if (!WaitAndBegin(fs)) return;
    XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    bool ok = fs.shouldRender && CopyToChain(g_theater, bb);
    if (ok) {
        float w = g_cfg.screenWidth;
        float h = w * (float)g_theater.height / (float)g_theater.width;
        quad.space = xr.localSpace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = g_theater.sc;
        quad.subImage.imageRect = { { 0, 0 }, { (int32_t)g_theater.width, (int32_t)g_theater.height } };
        quad.pose.orientation.w = 1.0f;
        quad.pose.position = { 0.0f, g_cfg.screenHeight, -g_cfg.screenDistance };
        quad.size = { w, h };
    }
    EndFrame(fs, ok ? (XrCompositionLayerBaseHeader*)&quad : nullptr);
}

static void PresentStereo(ID3D11Texture2D* bb, const EyeFrame& f)
{
    if (!CopyToChain(g_eyes[f.eye], bb)) return;
    g_eyeDone[f.eye] = f;
    g_eyeHave[f.eye] = true;
    if (f.eye != 1 || !g_eyeHave[0]) return;   // submit after each left+right pair
    g_eyeHave[0] = g_eyeHave[1] = false;

    XrFrameState fs;
    if (!WaitAndBegin(fs)) return;
    XrCompositionLayerProjectionView pv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    for (int i = 0; i < 2; i++) {
        pv[i].pose = g_eyeDone[i].pose;
        pv[i].fov = g_eyeDone[i].fov;
        pv[i].subImage.swapchain = g_eyes[i].sc;
        pv[i].subImage.imageRect = { { 0, 0 }, { (int32_t)g_eyes[i].width, (int32_t)g_eyes[i].height } };
    }
    XrCompositionLayerProjection proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    proj.space = xr.localSpace;
    proj.viewCount = 2;
    proj.views = pv;
    EndFrame(fs, fs.shouldRender ? (XrCompositionLayerBaseHeader*)&proj : nullptr);
}

// Called on the game's render/present thread for every presented frame.
static void VRFrame(IDXGISwapChain* sc)
{
    if (xr.failed) return;
    if (!xr.session) {
        static DWORD nextTry;
        if (GetTickCount() < nextTry) return;
        ID3D11Device* dev = nullptr;
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev))) { xr.failed = true; Log("vr: swapchain has no D3D11 device"); return; }
        bool ok = InitXR(dev);
        dev->Release();
        if (!ok) {
            if (g_retryLater) nextTry = GetTickCount() + 3000;
            else { xr.failed = true; Log("vr: init failed, running flat"); }
            return;
        }
    }
    PollEvents();
    if (!xr.running) { ResetStereo(); return; }
    UpdateInput();

    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return;
    D3D11_TEXTURE2D_DESC bd;
    bb->GetDesc(&bd);
    g_aspect = (float)bd.Width / (float)bd.Height;

    EyeFrame f;
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(g_stereoMutex);
        if (!g_pending.empty()) { f = g_pending.front(); g_pending.pop_front(); have = true; }
    }
    static bool wasStereo;
    bool stereo = g_cfg.vrMode == 1 && CameraInGameplay();
    if (stereo != wasStereo) {
        Log("vr: %s", stereo ? "stereo gameplay view" : "theater screen");
        wasStereo = stereo;
        if (!stereo) ResetStereo();
    }
    if (stereo) {
        static long pops, misses;
        if (have) { PresentStereo(bb, f); pops++; } else misses++;
        if (g_cfg.logFps && (pops + misses) % 600 == 0) Log("vr: stereo frames %ld, frames without eye data %ld", pops, misses);
    } else {
        PresentTheater(bb);
    }
    bb->Release();
}


// ---------------------------------------------------------------------------------------------
// Present hooks
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static PFN_Present g_origPresent;
static PFN_Present1 g_origPresent1;
static std::atomic<long> g_frames;

static void OnPresent(IDXGISwapChain* sc, UINT flags)
{
    if (flags & DXGI_PRESENT_TEST) return;
    long n = ++g_frames;
    if (n == 1) Log("vr: first Present on swapchain %p", sc);
    if (g_cfg.logFps) {
        static DWORD t0 = GetTickCount();
        static long n0 = 0;
        DWORD now = GetTickCount();
        if (now - t0 >= 5000) {
            Log("fps: %.1f", (n - n0) * 1000.0f / (now - t0));
            t0 = now;
            n0 = n;
        }
    }
    __try {
        VRFrame(sc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("vr: exception %08X in VRFrame - VR disabled", GetExceptionCode());
        xr.failed = true;
    }
}

static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    OnPresent(sc, flags);
    return g_origPresent(sc, xr.running ? 0 : sync, flags);   // headset paces the game, not the monitor
}

static HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    OnPresent(sc, flags);
    return g_origPresent1(sc, xr.running ? 0 : sync, flags, pp);
}

static LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static bool InstallPresentHooks()
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DummyProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TWFCVRDummy";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.Width = sd.BufferDesc.Height = 64;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* sc = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd,
                                               &sc, &dev, nullptr, &ctx);
    if (FAILED(hr)) { Log("vr: dummy swapchain failed %08X", hr); DestroyWindow(hwnd); return false; }
    void** vt = *(void***)sc;
    void* present = vt[8];
    void* present1 = nullptr;
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1))) {
        present1 = (*(void***)sc1)[22];
        sc1->Release();
    }
    sc->Release(); ctx->Release(); dev->Release();
    DestroyWindow(hwnd);

    MH_Initialize();
    bool ok = MH_CreateHook(present, (void*)HookPresent, (void**)&g_origPresent) == MH_OK;
    if (present1 && MH_CreateHook(present1, (void*)HookPresent1, (void**)&g_origPresent1) != MH_OK) present1 = nullptr;
    MH_EnableHook(MH_ALL_HOOKS);
    Log("vr: Present hook %s (%p), Present1 hook %s (%p)", ok ? "ok" : "FAILED", present, present1 ? "ok" : "none", present1);
    return ok;
}

static DWORD WINAPI VRThread(LPVOID)
{
    // dgVoodoo loads d3d11 when the game creates its D3D9 device; hooking earlier is fine too, but the
    // dummy device must not be created under the loader lock, hence this thread.
    InstallPresentHooks();
    return 0;
}

void VRStart()
{
    CloseHandle(CreateThread(nullptr, 0, VRThread, nullptr, 0, nullptr));
}