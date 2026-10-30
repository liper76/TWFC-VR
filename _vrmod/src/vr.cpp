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
cbuffer C : register(b0) { float linearize; float hud; float maskRadius; float scope; float4 uvRect; };
void VS(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    if (scope > 1.5) {   // a part of the HUD: the game's crosshair for the VR reticle (round: no names around it)
        if (length(uv - 0.5) > 0.49) return float4(0, 0, 0, 0);
        uv = uvRect.xy + uv * uvRect.zw;
    }
    else if (scope > 0.5) {
        // sniper scope: a round lens showing part of the eye image (uvRect = offset, size), thin crosshair
        float r = length(uv - 0.5);
        if (r > 0.5) return float4(0, 0, 0, 0);
        float3 c = src.Sample(smp, uvRect.xy + uv * uvRect.zw).rgb;
        float2 d = abs(uv - 0.5);
        if ((min(d.x, d.y) < 0.004 && r > 0.03 && r < 0.42) || r > 0.47) c = 0;
        if (linearize > 0.5) c = pow(abs(c), 2.2);
        return float4(c, 1);
    }
    float4 s = src.Sample(smp, uv);
    float3 c = s.rgb;
    float a = 1;
    // markers' colours: blue/cyan (waypoints, teammates' names and distances) and red (enemies, lock-on)
    float cmax = max(c.r, max(c.g, c.b));
    bool blueMark = c.b > 0.2 && c.b >= 0.75 * cmax && c.b > 1.6 * c.r;
    bool redMark = c.r > 0.2 && c.r > 1.8 * max(c.g, c.b);
    // the crosshair cut: only the crosshair (white/grey, red over an enemy); markers and names - also their dim
    // anti-aliased edges - go
    float lo0 = min(c.r, min(c.g, c.b));
    bool greyPx = cmax - lo0 < 0.3 * cmax + 0.02;
    bool redPx = c.r >= cmax - 0.01 && abs(c.g - c.b) < 0.2 * c.r + 0.03;
    if (scope > 1.5 && (blueMark || (!greyPx && !redPx))) return float4(0, 0, 0, 0);
    // the panel shows only health and ammo: markers and names that pass over its corners go (their colours, also
    // dim, except in the health bar - it is cyan too)
    bool hpBar = uv.x < 0.4 && uv.y > 0.07 && uv.y < 0.16;
    bool markTint = (c.b > 0.03 && c.b >= 0.75 * cmax && c.b > 1.3 * c.r) || (c.r > 0.03 && c.r > 1.8 * max(c.g, c.b));
    if (hud > 1.5 && hud < 3.5 && markTint && !hpBar) return float4(0, 0, 0, 0);
    // "LOW AMMO" (red) and "X RELOAD" below it, right of the crosshair: not shown at all in gameplay (only blue
    // markers are kept there)
    if (hud > 1.5 && (scope < 1.5 || length(uv - 0.5) > 0.06) && !blueMark && uv.x > 0.555 && uv.x < 0.73 && uv.y > 0.44 && uv.y < 0.535) return float4(0, 0, 0, 0);
    if (hud > 3.5 && !blueMark && !redMark) return float4(0, 0, 0, 0);   // into the eyes: only the markers' colours
    // the panel's parts: the top corners - health and ability (left), weapon, ammo and ability (right) - and
    // (scope < 0: HudPrompts) the middle band of texts: button hints, "reload", "no ammo"
    bool prompts = scope < -0.5 && uv.y > 0.55 && uv.y < 0.76 && uv.x > 0.2 && uv.x < 0.8;
    bool panelPart = (uv.y < 0.26 && (uv.x < 0.4 || uv.x > 0.6)) || prompts;
    // into the eye: the rest, but only what is drawn solid (markers, names, lock-on marks) - the see-through
    // full-screen effects of abilities and aiming stay out
    if (hud > 3.5 && (panelPart || max(sqrt(saturate(s.a)), max(s.r, max(s.g, s.b))) < 0.5)) return float4(0, 0, 0, 0);
    // minimal HUD (panel): only the health bar (top left) and the ammo bar with its count (top right) - not the
    // ability icons and grenades below them
    bool hpAmmo = (uv.y < 0.145 && (uv.x < 0.37 || (uv.x > 0.58 && uv.x < 0.96))) ||
                  (uv.y < 0.205 && uv.x > 0.58 && uv.x < 0.892);   // the weapon picture and the count, not the grenades
    if (hud > 1.5 && hud < 3.5 && !(hpAmmo || prompts)) return float4(0, 0, 0, 0);
    if (hud > 2.5 && hud < 3.5 && s.a < 0.85) return float4(0, 0, 0, 0);   // zoomed: the scope overlay's tint goes
    if (hud > 0.5) {
        // HUD drawn onto transparent black with SRCALPHA/INVSRCALPHA: rgb is premultiplied and alpha ends up
        // as a*a; additive elements may leave alpha at 0, so their brightness counts as coverage.
        a = saturate(max(sqrt(saturate(s.a)), max(c.r, max(c.g, c.b))));
        // the game's own crosshair: white/grey marks around the centre, fading to red over an enemy (white..pink..red
        // = red channel highest, green and blue equal); other coloured messages there stay visible
        float hi = max(c.r, max(c.g, c.b)), lo = min(c.r, min(c.g, c.b));
        bool grey = hi - lo < 0.3 * hi + 0.02;
        bool red = c.r > 0.05 && c.r >= hi - 0.01 && abs(c.g - c.b) < 0.2 * c.r + 0.03;
        float r = length(uv - 0.5);
        if (scope < 1.5 && ((grey && r < maskRadius) || (red && r < 0.065))) { c = 0; a = 0; }
        // full-screen tints (damage / ability flashes) are drawn with the HUD: on the floating panel they become a
        // coloured rectangle. They are flat and translucent - text and icons have edges - so a pixel whose
        // neighbours a few pixels away have the same translucent alpha is dropped.
        float w, h;
        src.GetDimensions(w, h);
        float2 o = float2(6.0 / w, 6.0 / h);
        float a0 = s.a;
        if (a0 > 0.01 && a0 < 0.75) {
            float d = abs(src.Sample(smp, uv + float2(o.x, 0)).a - a0) + abs(src.Sample(smp, uv - float2(o.x, 0)).a - a0) +
                      abs(src.Sample(smp, uv + float2(0, o.y)).a - a0) + abs(src.Sample(smp, uv - float2(0, o.y)).a - a0);
            if (d < 0.04) { c = 0; a = 0; }
        }
    }
    if (linearize > 0.5) c = pow(abs(c), 2.2);
    return float4(c, a);
}
)";

// HUD drawing mode for the shader: 1 = whole HUD, 2 = minimal (gameplay), 3 = minimal and zoomed (the game's scope
// overlay - a translucent tint over the whole screen - is dropped: only solid HUD pixels stay)
static float HudMode()
{
    if (!g_cfg.hudMinimal || !CameraPlayerView()) return 1.0f;
    float fov;
    return CameraGameZoom(fov) ? 3.0f : 2.0f;
}

struct Blitter {
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11SamplerState* smp = nullptr;
    ID3D11Buffer* cb = nullptr;   // {linearize, hud, maskRadius, scope, uvRect}
    ID3D11BlendState* over = nullptr;   // premultiplied "over" (the HUD drawn onto an eye image)

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
        D3D11_BUFFER_DESC bd = { 32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
        dev->CreateBuffer(&bd, nullptr, &cb);
        D3D11_BLEND_DESC bl = {};
        bl.RenderTarget[0] = { TRUE, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_BLEND_ZERO, D3D11_BLEND_ONE,
                               D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL };
        dev->CreateBlendState(&bl, &over);
        return vs && ps && smp && cb;
    }

    // Draws on a deferred context so the game's immediate-context state is untouched.
    void Draw(ID3D11Device* dev, ID3D11DeviceContext* imm, ID3D11ShaderResourceView* srv, ID3D11RenderTargetView* rtv,
              UINT w, UINT h, bool linearize, bool hud = false, float maskRadius = 0.0f, const float* scopeRect = nullptr,
              float hudMode = 0.0f,   // hudMode 4: the HUD over an eye image (blended), without the panel's parts
              bool crop = false)      // scopeRect is a part of the HUD to show as it is (no lens)
    {
        ID3D11DeviceContext* dc = nullptr;
        if (FAILED(dev->CreateDeferredContext(0, &dc))) return;
        float consts[8] = { linearize ? 1.0f : 0.0f, hudMode > 0.0f ? hudMode : hud ? HudMode() : 0.0f, maskRadius,
                            crop ? 2.0f : scopeRect ? 1.0f : (hud && g_cfg.hudPrompts ? -1.0f : 0.0f), 0, 0, 1, 1 };
        if (scopeRect) for (int i = 0; i < 4; i++) consts[4 + i] = scopeRect[i];
        dc->UpdateSubresource(cb, 0, nullptr, consts, 0, 0);
        D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
        dc->RSSetViewports(1, &vp);
        dc->OMSetRenderTargets(1, &rtv, nullptr);
        if (hudMode > 3.5f) dc->OMSetBlendState(over, nullptr, 0xFFFFFFFF);
        dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        dc->VSSetShader(vs, nullptr, 0);
        dc->PSSetShader(ps, nullptr, 0);
        dc->PSSetShaderResources(0, 1, &srv);
        dc->PSSetSamplers(0, 1, &smp);
        dc->PSSetConstantBuffers(0, 1, &cb);
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
    XrAction aimPose[2] = {}, gripPose[2] = {};
    XrAction triggerTouch[2] = {}, thumbTouch[2] = {};   // finger curls of the empty hands
    XrSpace aimSpace[2] = {}, gripSpace[2] = {};
    XrSpace viewSpace = {};   // the head: vehicle aiming
    XrSpace stageSpace = {};  // the play area, its origin on the floor (may be missing)
    XrPath hand[2] = {};
    XrAction haptic = XR_NULL_HANDLE;   // per hand (subaction paths)
    DWORD menuDownAt = 0;
    bool menuWasDown = false;
    int backPulse = 0, startPulse = 0;
    // gestures and two-handed weapons
    int meleePulse = 0;
    DWORD meleeAt = 0;
    bool twoHand = false;
    bool raised = false;      // weapon in the ready position: in front of the face, along the gaze
    bool leftGripHeld = false;
    bool gripHeld[2] = {};    // a squeeze that already did something (a new one is needed to grab)
    bool pullSqueeze[2] = {}; // this squeeze pulled a pickup (no weapon wheel for it)
    WORD prevButtons = 0;
    DWORD gripAt[2] = {};     // grip squeezed since
    DWORD clickAt[2] = {};    // thumbstick click held since (weapon wheel)
    bool clickHeld[2] = {};   // ... long enough: the wheel (no tap)
    int clickPulse[2] = {};   // a tap of the click: its button for a few updates
    DWORD throwAt = 0;        // grenade throw gesture
    int throwPulse = 0;
    bool grenade = false;     // a grip holds a grenade from the pouch (grenades.cpp)
    int grenadeHand = 0;
    bool grenadeArmed = false;
    bool fireWas = false;
};
static XR xr;
static std::atomic<int64_t> g_lastDisplayTime;   // predicted display time of the last frame (hand/eye poses)

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
    for (auto& e : b) if (e.first) sb.push_back({ e.first, Path(e.second) });
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
        sprintf_s(n, "%s_aim_pose", names[h]);  if (!CreateAction(XR_ACTION_TYPE_POSE_INPUT, n, &xr.aimPose[h], false)) return false;
        sprintf_s(n, "%s_grip_pose", names[h]); if (!CreateAction(XR_ACTION_TYPE_POSE_INPUT, n, &xr.gripPose[h], false)) return false;
    }
    for (int h = 0; h < 2; h++) {
        char n[64];
        sprintf_s(n, "%s_trigger_touch", names[h]); if (!CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, n, &xr.triggerTouch[h], false)) xr.triggerTouch[h] = XR_NULL_HANDLE;
        sprintf_s(n, "%s_thumb_touch", names[h]);   if (!CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, n, &xr.thumbTouch[h], false)) xr.thumbTouch[h] = XR_NULL_HANDLE;
    }
    const char* bn[kBtnCount] = { "button_a", "button_b", "button_x", "button_y", "left_stick_click", "right_stick_click", "menu" };
    for (int i = 0; i < kBtnCount; i++)
        if (!CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, bn[i], &xr.btn[i], false)) return false;
    if (!CreateAction(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", &xr.haptic, true)) xr.haptic = XR_NULL_HANDLE;

    SuggestBindings("/interaction_profiles/oculus/touch_controller", {
        { xr.stick[0], "/user/hand/left/input/thumbstick" }, { xr.stick[1], "/user/hand/right/input/thumbstick" },
        { xr.trigger[0], "/user/hand/left/input/trigger/value" }, { xr.trigger[1], "/user/hand/right/input/trigger/value" },
        { xr.grip[0], "/user/hand/left/input/squeeze/value" }, { xr.grip[1], "/user/hand/right/input/squeeze/value" },
        { xr.btn[kA], "/user/hand/right/input/a/click" }, { xr.btn[kB], "/user/hand/right/input/b/click" },
        { xr.btn[kX], "/user/hand/left/input/x/click" }, { xr.btn[kY], "/user/hand/left/input/y/click" },
        { xr.btn[kLClick], "/user/hand/left/input/thumbstick/click" }, { xr.btn[kRClick], "/user/hand/right/input/thumbstick/click" },
        { xr.btn[kMenu], "/user/hand/left/input/menu/click" },
        { xr.triggerTouch[0], "/user/hand/left/input/trigger/touch" }, { xr.triggerTouch[1], "/user/hand/right/input/trigger/touch" },
        { xr.thumbTouch[0], "/user/hand/left/input/thumbstick/touch" }, { xr.thumbTouch[0], "/user/hand/left/input/thumbrest/touch" },
        { xr.thumbTouch[0], "/user/hand/left/input/x/touch" }, { xr.thumbTouch[0], "/user/hand/left/input/y/touch" },
        { xr.thumbTouch[1], "/user/hand/right/input/thumbstick/touch" }, { xr.thumbTouch[1], "/user/hand/right/input/thumbrest/touch" },
        { xr.thumbTouch[1], "/user/hand/right/input/a/touch" }, { xr.thumbTouch[1], "/user/hand/right/input/b/touch" },
        { xr.aimPose[0], "/user/hand/left/input/aim/pose" }, { xr.aimPose[1], "/user/hand/right/input/aim/pose" },
        { xr.gripPose[0], "/user/hand/left/input/grip/pose" }, { xr.gripPose[1], "/user/hand/right/input/grip/pose" },
        { xr.haptic, "/user/hand/left/output/haptic" }, { xr.haptic, "/user/hand/right/output/haptic" },
    });
    SuggestBindings("/interaction_profiles/valve/index_controller", {
        { xr.stick[0], "/user/hand/left/input/thumbstick" }, { xr.stick[1], "/user/hand/right/input/thumbstick" },
        { xr.trigger[0], "/user/hand/left/input/trigger/value" }, { xr.trigger[1], "/user/hand/right/input/trigger/value" },
        { xr.grip[0], "/user/hand/left/input/squeeze/value" }, { xr.grip[1], "/user/hand/right/input/squeeze/value" },
        { xr.btn[kA], "/user/hand/right/input/a/click" }, { xr.btn[kB], "/user/hand/right/input/b/click" },
        { xr.btn[kX], "/user/hand/left/input/a/click" }, { xr.btn[kY], "/user/hand/left/input/b/click" },
        { xr.btn[kLClick], "/user/hand/left/input/thumbstick/click" }, { xr.btn[kRClick], "/user/hand/right/input/thumbstick/click" },
        { xr.btn[kMenu], "/user/hand/left/input/trackpad/force" },
        { xr.triggerTouch[0], "/user/hand/left/input/trigger/touch" }, { xr.triggerTouch[1], "/user/hand/right/input/trigger/touch" },
        { xr.thumbTouch[0], "/user/hand/left/input/thumbstick/touch" }, { xr.thumbTouch[0], "/user/hand/left/input/trackpad/touch" },
        { xr.thumbTouch[0], "/user/hand/left/input/a/touch" }, { xr.thumbTouch[0], "/user/hand/left/input/b/touch" },
        { xr.thumbTouch[1], "/user/hand/right/input/thumbstick/touch" }, { xr.thumbTouch[1], "/user/hand/right/input/trackpad/touch" },
        { xr.thumbTouch[1], "/user/hand/right/input/a/touch" }, { xr.thumbTouch[1], "/user/hand/right/input/b/touch" },
        { xr.aimPose[0], "/user/hand/left/input/aim/pose" }, { xr.aimPose[1], "/user/hand/right/input/aim/pose" },
        { xr.gripPose[0], "/user/hand/left/input/grip/pose" }, { xr.gripPose[1], "/user/hand/right/input/grip/pose" },
        { xr.haptic, "/user/hand/left/output/haptic" }, { xr.haptic, "/user/hand/right/output/haptic" },
    });

    XrSessionActionSetsAttachInfo at = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    at.countActionSets = 1;
    at.actionSets = &xr.actionSet;
    XR_CHECK(xrAttachSessionActionSets(xr.session, &at));

    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo sci = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        sci.poseInActionSpace.orientation.w = 1.0f;
        sci.action = xr.aimPose[h];
        if (XR_FAILED(xrCreateActionSpace(xr.session, &sci, &xr.aimSpace[h]))) xr.aimSpace[h] = XR_NULL_HANDLE;
        sci.action = xr.gripPose[h];
        if (XR_FAILED(xrCreateActionSpace(xr.session, &sci, &xr.gripSpace[h]))) xr.gripSpace[h] = XR_NULL_HANDLE;
    }
    Log("vr: hand pose spaces %s", xr.aimSpace[1] ? "ok" : "unavailable");
    XrReferenceSpaceCreateInfo vci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    vci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vci.poseInReferenceSpace.orientation.w = 1.0f;
    if (XR_FAILED(xrCreateReferenceSpace(xr.session, &vci, &xr.viewSpace))) xr.viewSpace = XR_NULL_HANDLE;
    vci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;   // the floor: the player's height (T-pose calibration)
    if (XR_FAILED(xrCreateReferenceSpace(xr.session, &vci, &xr.stageSpace))) xr.stageSpace = XR_NULL_HANDLE;
    return true;
}

// Pose of a hand space in LOCAL space at the given time.
static bool LocateHand(XrSpace s, XrTime t, XrPosef& out)
{
    if (!s) return false;
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(s, xr.localSpace, t, &loc))) return false;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((loc.locationFlags & need) != need) return false;
    out = loc.pose;
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

// a touch sensor: -1 = the controller has none (the action is not bound)
static int GetTouch(XrAction a)
{
    if (!a) return -1;
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateBoolean s = { XR_TYPE_ACTION_STATE_BOOLEAN };
    if (XR_FAILED(xrGetActionStateBoolean(xr.session, &gi, &s)) || !s.isActive) return -1;
    return s.currentState ? 1 : 0;
}

// How bent the fingers of an empty hand are (0 = straight, 1 = closed): index, middle, ring, thumb. The trigger
// bends the index (resting on it: a little; lifted off: pointing), the grip the middle and ring fingers, and the
// thumb lies down while it touches the stick, the buttons or the thumb rest (lifted: thumbs up).
static float g_fingers[2][4] = { { 0.3f, 0.2f, 0.2f, 1.0f }, { 0.3f, 0.2f, 0.2f, 1.0f } };

static void UpdateFingers()
{
    for (int h = 0; h < 2; h++) {
        float trig = GetFloat(xr.trigger[h]), grip = GetFloat(xr.grip[h]);
        int tt = GetTouch(xr.triggerTouch[h]), th = GetTouch(xr.thumbTouch[h]);
        float index = trig > 0.02f ? 0.35f + 0.65f * trig : (tt == 0 ? 0.0f : 0.3f);
        float rest = 0.15f + 0.85f * grip;
        float thumb = th == 0 ? 0.0f : 1.0f;
        float want[4] = { index, rest, rest, thumb };
        for (int k = 0; k < 4; k++) g_fingers[h][k] += (want[k] - g_fingers[h][k]) * 0.5f;   // (a little smoothing)
    }
}

void VRFingerCurls(int hand, float out[4])
{
    for (int k = 0; k < 4; k++) out[k] = g_fingers[hand & 1][k];
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

void VRHaptic(int hand, float amplitude, float seconds)
{
    if (!xr.running || !xr.haptic) return;
    XrHapticVibration v = { XR_TYPE_HAPTIC_VIBRATION };
    v.amplitude = amplitude;
    v.duration = (XrDuration)(seconds * 1e9);
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO };
    hi.action = xr.haptic;
    hi.subactionPath = xr.hand[hand];
    xrApplyHapticFeedback(xr.session, &hi, (const XrHapticBaseHeader*)&v);
}

static Vec3f g_gripPos[2], g_aimPos[2], g_aimFwd[2];   // LOCAL space, refreshed by UpdateGestures
static volatile DWORD g_abilityAt;                     // last ability button press (IK yields the arms)

static float g_meleeDir[2];
static float g_handSpeed[2];   // each hand's speed (m/s), last input update

bool VRTwoHanded() { return xr.twoHand; }

// "Empty hand" chosen on the right hand's wheel: the gun is put away (hidden, does not fire), the hand is free like the
// left one (grenades, pulling things, fingers). Choosing a weapon or the melee weapon there ends it.
static bool g_rightEmpty;
// The gun can be taken into the left hand from its wheel - mirrored: it leaves the right hand, which is then free.
static int g_gunHand = 1;
int VRGunHand() { return IKPawnIsVehicle() || g_gunHand == 1 ? (g_rightEmpty && !IKPawnIsVehicle() ? -1 : 1) : 0; }   // -1: put away
bool VRRightEmpty() { return VRGunHand() != 1; }
static int GunIdx() { return g_gunHand == 0 && !IKPawnIsVehicle() ? 0 : 1; }   // the controller that aims the gun

// Horizontal direction of a melee swing in the last few tenths of a second (OpenXR x/z), false if none.
bool VRMeleeSwing(float& x, float& z)
{
    if (!xr.meleeAt || GetTickCount() - xr.meleeAt > 350) return false;
    x = g_meleeDir[0];
    z = g_meleeDir[1];
    return x * x + z * z > 0.01f;
}
void VRDebugAbility() { g_abilityAt = GetTickCount(); }
DWORD VRAbilityPressedAt() { return g_abilityAt; }

// Physical controls in gameplay (render thread, once per presented frame):
// - a fast swing of either hand = melee (the game's melee button, right stick click), with a buzz;
// - left grip near the weapon's foregrip = two-handed hold (the left grip then is not the ability button);
// - weapon held two-handed or brought up to the eye = aiming (the game's zoom button: accuracy, no zoom in VR);
// - ability buttons (grips -> bumpers) let the arms play the ability's animation for a moment.
static bool WheelIsOpen();
static XrVector3f Rotate(const XrQuaternionf& q, XrVector3f v);
static Vec3f g_forePos;                     // the held weapon's front grip (LOCAL space)
static XrQuaternionf g_twoHandTurn = { 0, 0, 0, 1 };   // held two-handed: turn of the right hand's aim (LOCAL space)

static XrQuaternionf QuatMul(const XrQuaternionf& a, const XrQuaternionf& b)
{
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}

// Rotation taking direction a onto direction b.
static XrQuaternionf QuatFromTo(Vec3f a, Vec3f b)
{
    float la = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z), lb = sqrtf(b.x * b.x + b.y * b.y + b.z * b.z);
    if (la < 1e-4f || lb < 1e-4f) return { 0, 0, 0, 1 };
    a = { a.x / la, a.y / la, a.z / la };
    b = { b.x / lb, b.y / lb, b.z / lb };
    float d = a.x * b.x + a.y * b.y + a.z * b.z;
    if (d < -0.999f) return { 0, 0, 0, 1 };   // opposite: not a hold anyone does
    XrQuaternionf q = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x, 1.0f + d };
    float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return { q.x / l, q.y / l, q.z / l, q.w / l };
}

// Held two-handed, the weapon turns about the right hand so that its front grip lies in the left hand.
bool VRTwoHandTurn(float q[4])
{
    if (!xr.twoHand) return false;
    q[0] = g_twoHandTurn.x; q[1] = g_twoHandTurn.y; q[2] = g_twoHandTurn.z; q[3] = g_twoHandTurn.w;
    return true;
}

// ---------------------------------------------------------------------------------------------
// How the gun sits in the right hand (ini GunGrip: a turn and a shift in the controller's aim axes), and its
// adjustment in the headset: hold the menu button - the gun stays where it is - put the hand where it should hold
// the gun (and the left hand on its front, grip squeezed, to set the two-handed grip too), pull the trigger.
static struct Calib {
    bool active = false;
    XrPosef aim = {};         // the right hand's (adjusted) aim pose when it started: the gun stays there
    XrVector3f grip = {};
    bool triggerWas = false;
} g_calib;

static XrQuaternionf QuatConj(const XrQuaternionf& q) { return { -q.x, -q.y, -q.z, q.w }; }

// ---- Setup mode (hold the menu button 1.5 s in gameplay; again to save and leave). Per weapon (ini sections
// [Weapon.<name>]): where the gun sits in the right hand (GunGrip - by default only its place, B turns the
// rotation on too), the two-handed zone on its front (Foregrip) and the aiming lens above the barrel (Scope: metres
// right, up, forward of the aim ray, and its size); for all: the grenade pouch. The zones are shown as green
// squares; the left grip takes one and puts it elsewhere. (The game's crosshair on the VR reticle always follows the
// barrel; only its size is a setting - ReticleSize on the settings page.)
struct WeaponSetup {
    std::wstring name;
    bool grip = false, fore = false, scopeSet = false, gripL = false;   // set for this weapon (else the global ones)
    float gunGrip[7] = { 0, 0, 0, 1, 0, 0, 0 }, foregrip[3] = {}, scope[4] = {};
    float gunGripL[7] = { 0, 0, 0, 1, 0, 0, 0 };   // the gun in the left hand (GunGripL)
};
static WeaponSetup g_ws;                  // the weapon in hand
static struct Setup {
    bool active = false, rotate = false;
    int grab = -1;                        // the cube in the left hand (0 = the gun's front, 1 = the pouch)
    XrVector3f grabOffset = {}, cube[2] = {};
    bool rightWas = false, leftWas = false, aWas = false, bWas = false;
    DWORD at = 0;
    XrPosef panel = {};                   // the instructions panel
    bool dirty = true;
} g_setup;
static void SetupToggle();
static void UpdateSetup(XINPUT_GAMEPAD& p, const float grip[2], DWORD now);
static void UpdateSettingsPage(XrTime t, const XrPosef& head, DWORD now);
static const UINT kPanelW = 640, kPanelH = 520;   // the setup panel (pixels; 1 px = 1 mm)
static int g_page = 1;   // setup panel: 1 = the menu (first), 0 = the weapon page (grip, zones, lens)
static struct Pointer { bool valid = false; float u = 0, v = 0; XrVector3f from = {}, to = {}; } g_pointer;   // the menu's ray
static DWORD g_tposeAt;      // T-pose calibration: the countdown ends then (0 = none)
static int g_tposeResult;    // 0 none, 1 measured, -1 not recognised
static DWORD g_setupEnteredAt;
static void TrackWeaponSetup();

// The grenade pouch (BeltOffset: right, up, forward from the eyes, turned with the head's heading).
static const float kZoneRadius = 0.15f;   // grab zones (pouch, the gun's front): a hand this close to the point
static XrVector3f BeltPoint(const XrPosef& head)
{
    const XrQuaternionf& o = head.orientation;
    float fx = -2 * (o.x * o.z + o.y * o.w), fz = -(1 - 2 * (o.x * o.x + o.y * o.y));   // forward, flattened
    float l = sqrtf(fx * fx + fz * fz);
    if (l < 1e-3f) { fx = 0; fz = -1; } else { fx /= l; fz /= l; }
    const float* b = g_cfg.beltOffset;   // right = (-fz, fx) in x/z
    return { head.position.x + b[0] * -fz + b[2] * fx, head.position.y + b[1], head.position.z + b[0] * fx + b[2] * fz };
}

// How the gun sits in hand h: the right hand's GunGrip; the left hand's own per weapon (GunGripL, set in the setup),
// or - not set yet - the right one mirrored.
static void GunGripFor(int h, float g[7])
{
    if (h == 1 && VRGunHand() < 0) { memcpy(g, g_cfg.emptyGripR, 7 * sizeof(float)); return; }   // the empty right hand: its own
    if (h == 0 && g_ws.gripL) { memcpy(g, g_ws.gunGripL, 7 * sizeof(float)); return; }
    memcpy(g, g_cfg.gunGrip, 7 * sizeof(float));
    if (h == 0) { g[1] = -g[1]; g[2] = -g[2]; g[4] = -g[4]; }
}

// The gun hand's aim pose (and grip position) with its grip placement applied - or the frozen one while adjusting.
static bool LocateRightAim(XrTime t, XrPosef& aim, XrVector3f* grip)
{
    if (g_calib.active) {
        aim = g_calib.aim;
        if (grip) *grip = g_calib.grip;
        return true;
    }
    int h = GunIdx();
    if (!LocateHand(xr.aimSpace[h], t, aim)) return false;
    XrPosef gp;
    if (grip) *grip = LocateHand(xr.gripSpace[h], t, gp) ? gp.position : aim.position;
    float g[7];   // qx qy qz qw tx ty tz
    GunGripFor(h, g);
    XrVector3f d = Rotate(aim.orientation, { g[4], g[5], g[6] });
    aim.position = { aim.position.x + d.x, aim.position.y + d.y, aim.position.z + d.z };
    if (grip) *grip = { grip->x + d.x, grip->y + d.y, grip->z + d.z };
    aim.orientation = QuatMul(aim.orientation, { g[0], g[1], g[2], g[3] });
    return true;
}

static void GripCalibrationToggle()
{
    XrTime t = g_lastDisplayTime.load();
    if (g_calib.active) { g_calib.active = false; Log("vr: grip adjustment cancelled"); return; }
    XrPosef aim;
    XrVector3f grip;
    if (!t || !LocateRightAim(t, aim, &grip)) return;
    g_calib.aim = aim;
    g_calib.grip = grip;
    g_calib.triggerWas = true;   // a trigger already held does not confirm
    g_calib.active = true;
    VRHaptic(0, 0.5f, 0.1f);
    VRHaptic(1, 0.5f, 0.1f);
    Log("vr: grip adjustment - move the hand to the gun's grip and pull the trigger");
}

static void GripCalibrationConfirm(bool leftOnGun)
{
    XrTime t = g_lastDisplayTime.load();
    XrPosef aim, gp, lp;
    if (!t || !LocateHand(xr.aimSpace[1], t, aim) || !LocateHand(xr.gripSpace[1], t, gp)) return;
    // new GunGrip: the raw hand now, adjusted, gives the frozen gun pose
    XrQuaternionf inv = QuatConj(aim.orientation);
    XrVector3f dt = Rotate(inv, { g_calib.grip.x - gp.position.x, g_calib.grip.y - gp.position.y, g_calib.grip.z - gp.position.z });
    XrQuaternionf r = QuatMul(inv, g_calib.aim.orientation);
    float* g = g_cfg.gunGrip;
    g[0] = r.x; g[1] = r.y; g[2] = r.z; g[3] = r.w; g[4] = dt.x; g[5] = dt.y; g[6] = dt.z;
    wchar_t buf[200];
    swprintf_s(buf, L"%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f", g[0], g[1], g[2], g[3], g[4], g[5], g[6]);
    std::wstring ini = ModDir() + L"twfcvr.ini";
    WritePrivateProfileStringW(L"VR", L"GunGrip", buf, ini.c_str());
    if (leftOnGun && LocateHand(xr.gripSpace[0], t, lp)) {
        // the left hand's place on the gun, relative to the right hand's grip in the gun's aim axes
        XrVector3f f = Rotate(QuatConj(g_calib.aim.orientation),
                              { lp.position.x - g_calib.grip.x, lp.position.y - g_calib.grip.y, lp.position.z - g_calib.grip.z });
        g_cfg.foregrip[0] = f.x; g_cfg.foregrip[1] = f.y; g_cfg.foregrip[2] = f.z;
        g_cfg.foregripSet = 1;
        swprintf_s(buf, L"%.4f,%.4f,%.4f", f.x, f.y, f.z);
        WritePrivateProfileStringW(L"VR", L"Foregrip", buf, ini.c_str());
    }
    g_calib.active = false;
    VRHaptic(1, 0.8f, 0.15f);
    Log("vr: grip saved (GunGrip %ls)%s", buf, leftOnGun ? " with the front grip" : "");
}

// debug: "calib" = hold the menu button, "calibok <left>" = pull the trigger
void VRDebugCalib(int confirm, int left)
{
    if (confirm) { if (g_calib.active) GripCalibrationConfirm(left != 0); }
    else GripCalibrationToggle();
}

static void UpdateGestures(XINPUT_GAMEPAD& p, DWORD now)
{
    XrTime t = g_lastDisplayTime.load();
    if (!t) return;
    float speed[2] = {};
    XrVector3f velocity[2] = {};
    for (int h = 0; h < 2; h++) {
        XrSpaceVelocity vel = { XR_TYPE_SPACE_VELOCITY };
        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION, &vel };
        if (xr.gripSpace[h] && XR_SUCCEEDED(xrLocateSpace(xr.gripSpace[h], xr.localSpace, t, &loc))) {
            g_gripPos[h] = { loc.pose.position.x, loc.pose.position.y, loc.pose.position.z };
            if (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT)
                velocity[h] = vel.linearVelocity,
                speed[h] =sqrtf(vel.linearVelocity.x * vel.linearVelocity.x + vel.linearVelocity.y * vel.linearVelocity.y +
                                 vel.linearVelocity.z * vel.linearVelocity.z);
            g_handSpeed[h] = speed[h];
        }
        XrPosef a;
        XrVector3f rg;
        if (h == GunIdx() ? LocateRightAim(t, a, &rg) : LocateHand(xr.aimSpace[h], t, a)) {
            const XrQuaternionf& o = a.orientation;
            g_aimPos[h] = { a.position.x, a.position.y, a.position.z };
            g_aimFwd[h] = { -2 * (o.x * o.z + o.y * o.w), -2 * (o.y * o.z - o.x * o.w), -(1 - 2 * (o.x * o.x + o.y * o.y)) };
            if (h == GunIdx()) g_gripPos[h] = { rg.x, rg.y, rg.z };
        }
    }
    auto dist = [](Vec3f a, Vec3f b) { return sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); };

    // two-handed hold: the other hand's grip squeezed at the held weapon's front grip (from its model), kept until
    // released. The hand holding the gun (G) leads - right or left; the other one (S) only points the barrel.
    const int G = GunIdx(), S = 1 - G;
    float leftSqueeze = GetFloat(xr.grip[0]);
    float squeeze[2] = { leftSqueeze, GetFloat(xr.grip[1]) };
    Vec3f fore = { g_aimPos[G].x + g_aimFwd[G].x * g_cfg.twoHandReach, g_aimPos[G].y + g_aimFwd[G].y * g_cfg.twoHandReach,
                   g_aimPos[G].z + g_aimFwd[G].z * g_cfg.twoHandReach };
    float fg[3];
    XrPosef gunAim;
    if (IKForegrip(fg) && LocateRightAim(t, gunAim, nullptr)) {
        // relative to where the gun hand holds the controller, in the aim axes
        XrVector3f w = Rotate(gunAim.orientation, { fg[0], fg[1], fg[2] });
        fore = { g_gripPos[G].x + w.x, g_gripPos[G].y + w.y, g_gripPos[G].z + w.z };
    }
    g_forePos = fore;
    if (g_setup.active) { xr.twoHand = false; return; }   // (the grips move the setup's cubes now)
    if (!g_cfg.twoHanded || WeaponsMeleeHeld() >= 0 || VRGunHand() < 0) xr.twoHand = false;   // (both hands free for it)
    else if (xr.twoHand) xr.twoHand = squeeze[S] > 0.35f;
    else if (squeeze[S] > 0.6f && !xr.gripHeld[S] && dist(g_gripPos[S], fore) < g_cfg.twoHandRadius) {
        xr.twoHand = true;
        VRHaptic(S, 0.4f, 0.05f);
        Log("vr: two-handed hold (gun in the %s hand)", G ? "right" : "left");
    }
    // grenade (like Half-Life: Alyx): a new squeeze of a free hand at the body - below the head, in front of the chest or
    // belt (the "pouch") - takes one out; that hand's trigger arms it; letting go throws it with the hand's own motion.
    // Free: the left hand unless it holds the gun's front or the melee weapon, the right one when chosen empty.
    for (int h = 0; h < 2 && g_cfg.grenadeGrab && !xr.grenade && !IKPawnIsVehicle(); h++) {
        bool free = h == 0 ? !xr.twoHand && WeaponsMeleeHeld() != 0 && VRGunHand() != 0 : !xr.twoHand && VRRightEmpty() && WeaponsMeleeHeld() != 1;
        if (!free || squeeze[h] <= 0.6f || xr.gripHeld[h]) continue;
        XrPosef head;
        if (xr.viewSpace && LocateHand(xr.viewSpace, t, head)) {
            XrVector3f b = BeltPoint(head);
            float dx = g_gripPos[h].x - b.x, dy = g_gripPos[h].y - b.y, dz = g_gripPos[h].z - b.z;
            if (dx * dx + dy * dy + dz * dz < kZoneRadius * kZoneRadius) {
                xr.grenade = true;
                xr.grenadeHand = h;
                xr.grenadeArmed = false;
                GrenadeGrab(h);
            }
        }
    }
    if (xr.grenade) {
        int h = xr.grenadeHand;
        if (!xr.grenadeArmed && GetFloat(xr.trigger[h]) > 0.6f) {
            xr.grenadeArmed = true;
            GrenadeArm();
            VRHaptic(h, 0.8f, 0.1f);
        }
        if (squeeze[h] < 0.35f) {
            xr.grenade = false;
            GrenadeRelease();
        }
        if (h == GunIdx()) p.bRightTrigger = 0;   // (the trigger arms the grenade: not a shot or the game's fine aim now)
        else p.bLeftTrigger = 0;
    }
    // pointing at a pickup with a free hand: a new squeeze pulls it (like gravity gloves; the flick works too)
    for (int h = 0; h < 2; h++) {
        if (squeeze[h] > 0.6f && !xr.gripHeld[h] && !(xr.grenade && xr.grenadeHand == h) && PullGrab(h)) xr.pullSqueeze[h] = true;
        if (squeeze[h] < 0.35f) xr.pullSqueeze[h] = false;
    }
    if (PullWantInteract()) p.wButtons |= XINPUT_GAMEPAD_X;   // a pulled weapon: "hold X" takes it
    for (int h = 0; h < 2; h++) xr.gripHeld[h] = squeeze[h] > 0.6f || (xr.gripHeld[h] && squeeze[h] > 0.35f);   // grab only on a new squeeze
    xr.leftGripHeld = xr.gripHeld[0];
    if (VRGunHand() < 0) p.bRightTrigger = 0;   // the gun put away: nothing to fire
    if (xr.twoHand) {   // the gun turns about the gun hand's grip so that its front grip lies in the other hand
        Vec3f nominal = { fore.x - g_gripPos[G].x, fore.y - g_gripPos[G].y, fore.z - g_gripPos[G].z };
        Vec3f actual = { g_gripPos[S].x - g_gripPos[G].x, g_gripPos[S].y - g_gripPos[G].y, g_gripPos[S].z - g_gripPos[G].z };
        XrQuaternionf swing = QuatFromTo(nominal, actual);
        // the gun's "down" stays the main (gun) hand's down: a twist about the line to the other hand brings the turned
        // gun's up back to the gun controller's up (the other hand only points the barrel)
        float al = sqrtf(actual.x * actual.x + actual.y * actual.y + actual.z * actual.z);
        XrPosef ra;
        if (al > 0.05f && LocateRightAim(t, ra, nullptr)) {
            XrVector3f ax = { actual.x / al, actual.y / al, actual.z / al };
            XrVector3f u0 = Rotate(ra.orientation, { 0, 1, 0 }), u1 = Rotate(QuatMul(swing, ra.orientation), { 0, 1, 0 });
            auto flat = [&](XrVector3f u) { float k = u.x * ax.x + u.y * ax.y + u.z * ax.z; return XrVector3f{ u.x - ax.x * k, u.y - ax.y * k, u.z - ax.z * k }; };
            XrVector3f p0 = flat(u0), p1 = flat(u1);
            XrVector3f c = { p1.y * p0.z - p1.z * p0.y, p1.z * p0.x - p1.x * p0.z, p1.x * p0.y - p1.y * p0.x };   // p1 x p0
            float sinA = c.x * ax.x + c.y * ax.y + c.z * ax.z, cosA = p1.x * p0.x + p1.y * p0.y + p1.z * p0.z;
            if (sinA * sinA + cosA * cosA > 1e-6f) {
                float h = atan2f(sinA, cosA) * 0.5f, s = sinf(h);
                swing = QuatMul({ ax.x * s, ax.y * s, ax.z * s, cosf(h) }, swing);
            }
        }
        g_twoHandTurn = swing;
    }
    if (xr.twoHand && g_cfg.gripAbilities) p.wButtons &= ~(S == 0 ? XINPUT_GAMEPAD_LEFT_SHOULDER : XINPUT_GAMEPAD_RIGHT_SHOULDER);

    // ability buttons (bumpers): pressed now
    WORD bumpers = p.wButtons & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER);
    if (bumpers & ~xr.prevButtons) g_abilityAt = now;
    xr.prevButtons = p.wButtons;

    // aiming without the zoom button: weapon held with both hands and raised to the ready position (in front of the
    // face, pointing where you look); the raised weapon also shows a sniper scope
    xr.raised = false;
    XrPosef head;
    if (xr.viewSpace && LocateHand(xr.viewSpace, t, head)) {
        const XrQuaternionf& o = head.orientation;
        Vec3f hf = { -2 * (o.x * o.z + o.y * o.w), -2 * (o.y * o.z - o.x * o.w), -(1 - 2 * (o.x * o.x + o.y * o.y)) };
        Vec3f hp = { head.position.x, head.position.y, head.position.z };
        Vec3f wf = g_aimFwd[G];
        if (xr.twoHand) {   // the barrel runs from the gun hand to the other one
            Vec3f d = { g_gripPos[S].x - g_gripPos[G].x, g_gripPos[S].y - g_gripPos[G].y, g_gripPos[S].z - g_gripPos[G].z };
            float l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
            if (l > 0.05f) wf = { d.x / l, d.y / l, d.z / l };
        }
        float along = wf.x * hf.x + wf.y * hf.y + wf.z * hf.z;
        float below = hp.y - g_aimPos[G].y;
        xr.raised = dist(g_aimPos[G], hp) < 0.45f && below < 0.30f && along > 0.8f;

        // grenade: a throw with the free left hand (fast, forward where you look) = the game's grenade button (B)
        const XrVector3f& lv = velocity[0];
        if (g_cfg.throwGesture && !g_cfg.grenadeGrab && !xr.twoHand && WeaponsMeleeHeld() != 0 && !WheelIsOpen() && now - xr.throwAt > 1000 &&
            speed[0] > g_cfg.throwSpeed && (lv.x * hf.x + lv.y * hf.y + lv.z * hf.z) / speed[0] > 0.6f) {
            xr.throwAt = now;
            xr.throwPulse = 10;
            g_meleeDir[0] = lv.x;   // thrown where the hand moved (as a melee swing aims)
            g_meleeDir[1] = lv.z;
            xr.meleeAt = now;
            VRHaptic(0, 0.7f, 0.06f);
            Log("vr: grenade throw (%.1f m/s)", speed[0]);
        }
    }
    // (the left trigger is also some weapons' second action, so this only adds to it)
    if (g_cfg.physicalAim && !IKPawnIsVehicle() && xr.twoHand && xr.raised) p.bLeftTrigger = 255;

    // melee swing: only the hand holding the melee weapon strikes
    // (MeleeContact: only when the weapon reaches another robot - melee.cpp; it follows the weapon every frame)
    // a punch forward (where you look) with a hand that holds no gun - the left one (melee weapon or empty), the
    // right one when it is empty - is the game's melee attack right away (no weapon needed, no contact needed)
    int meleeHand = -1;
    static int strikeHand = 1;   // hand of the last strike (a left one shows the attack's weapon in the left hand)
    XrPosef mh;
    if (g_cfg.meleeGesture && !WheelIsOpen() && !IKPawnIsVehicle() && xr.viewSpace && LocateHand(xr.viewSpace, t, mh)) {
        XrVector3f hf = Rotate(mh.orientation, { 0, 0, -1 });
        for (int h = 0; h < 2 && meleeHand < 0; h++) {
            bool free = h == 0 ? !xr.twoHand && VRGunHand() != 0 : VRGunHand() != 1 || WeaponsMeleeHeld() == 1;
            if (!free || (xr.grenade && xr.grenadeHand == h) || speed[h] <= g_cfg.meleeSpeed) continue;
            const XrVector3f& v = velocity[h];
            if ((v.x * hf.x + v.y * hf.y + v.z * hf.z) / speed[h] > 0.5f) meleeHand = h;
        }
    }
    if (meleeHand < 0 && g_cfg.meleeContact) meleeHand = WeaponsMeleeHeld();   // (MeleeContact=1: a held weapon's swing that reaches a robot)
    if (g_cfg.meleeGesture && meleeHand >= 0 && !WheelIsOpen()) {
        int h = meleeHand;
        bool swinging = speed[h] > g_cfg.meleeSpeed;
        bool strike = g_cfg.meleeContact ? MeleeContact(h, swinging, now) : swinging;
        if (strike && now - xr.meleeAt > 600) {
            xr.meleeAt = now;
            xr.meleePulse = 10;
            strikeHand = h;
            g_meleeDir[0] = velocity[h].x;   // horizontal swing direction (LOCAL space): the attack goes that way
            g_meleeDir[1] = velocity[h].z;
            VRHaptic(h, 1.0f, g_cfg.meleeContact ? 0.12f : 0.08f);
            Log("vr: melee %s (%s hand, %.1f m/s)", g_cfg.meleeContact ? "hit" : "swing", h ? "right" : "left", speed[h]);
        }
    }
    // melee weapon in the right hand: no gun to fire, the trigger strikes too
    if (WeaponsWantPress(true)) xr.meleePulse = 10;
    if (WeaponsMeleeHand() == 1 && GunIdx() == 1) {   // (a gun in the left hand fires with RT)
        bool fire = p.bRightTrigger > 150;
        if (fire && !xr.fireWas && !g_cfg.meleeContact && now - xr.meleeAt > 400) {   // (contact melee: swings only)
            xr.meleePulse = 10;
            xr.meleeAt = now;
            g_meleeDir[0] = g_meleeDir[1] = 0.0f;   // strikes where the hand aims
        }
        xr.fireWas = fire;
        p.bRightTrigger = 0;
    }
    if (GrenadeWantPress()) xr.throwPulse = 10;
    if (xr.throwPulse > 0) { p.wButtons |= XINPUT_GAMEPAD_B; xr.throwPulse--; }
    if (xr.meleePulse == 10) WeaponsStrike(WeaponsMeleeHeld() >= 0 ? WeaponsMeleeHeld() : strikeHand);
    if (xr.meleePulse > 0) { p.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB; xr.meleePulse--; }
}

static void UpdateWeaponWheel(XINPUT_GAMEPAD& p, const float grip[2], DWORD now);

static void UpdateInput()
{
    if (!xr.running || !xr.actionSet) return;
    XrActiveActionSet as = { xr.actionSet, XR_NULL_PATH };
    XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
    si.countActiveActionSets = 1;
    si.activeActionSets = &as;
    if (XR_FAILED(xrSyncActions(xr.session, &si))) return;

    UpdateFingers();
    XINPUT_GAMEPAD p = {};
    XrVector2f ls = GetStick(xr.stick[0]), rs = GetStick(xr.stick[1]);
    p.sThumbLX = AxisToShort(ls.x); p.sThumbLY = AxisToShort(ls.y);
    p.sThumbRX = AxisToShort(rs.x); p.sThumbRY = AxisToShort(rs.y);
    // the gun hand's trigger fires (RT), the other one is the game's fine aim / second action (LT)
    p.bLeftTrigger = (BYTE)(GetFloat(xr.trigger[GunIdx() ^ 1]) * 255.0f);
    p.bRightTrigger = (BYTE)(GetFloat(xr.trigger[GunIdx()]) * 255.0f);
    if (GetBool(xr.btn[kA])) p.wButtons |= XINPUT_GAMEPAD_A;
    bool gameplay = CameraInGameplay();
    if (GetBool(xr.btn[kB]) && (!gameplay || g_cfg.gripAbilities)) p.wButtons |= XINPUT_GAMEPAD_B;   // (VR layout: UpdateWeaponWheel)
    if (GetBool(xr.btn[kX])) p.wButtons |= XINPUT_GAMEPAD_X;
    float grip[2] = { GetFloat(xr.grip[0]), GetFloat(xr.grip[1]) };
    DWORD now = GetTickCount();
    if (!gameplay || g_cfg.gripAbilities) {
        // menus (tabs) and the old layout: grips = bumpers, Y = next weapon
        if (grip[0] > 0.6f) p.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        if (grip[1] > 0.6f) p.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if (GetBool(xr.btn[kY])) p.wButtons |= XINPUT_GAMEPAD_Y;
        if (GetBool(xr.btn[kLClick])) p.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
        if (GetBool(xr.btn[kRClick])) p.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    } else {
        // VR layout: the grips hold things (the weapon's front, grenades, pickups); the left trigger stays the game's
        // (fine aim / the weapon's second action, e.g. detonating grenades; vehicle boost). Nothing important is on the
        // stick clicks (pressing them wears the sticks): the left click is only the game's transform (LS, one press),
        // the right one does nothing. Y tapped = main ability (RB), held = second ability (LB); B tapped = the game's
        // B (grenade), held = the weapon wheel (UpdateWeaponWheel). Melee: a punch forward with a hand.
        if (GetBool(xr.btn[kLClick])) p.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
        UpdateWeaponWheel(p, grip, now);
        if (xr.clickPulse[1] > 0) { p.wButtons |= XINPUT_GAMEPAD_B; xr.clickPulse[1]--; }
        if (xr.clickPulse[0] > 0) { p.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER; xr.clickPulse[0]--; }
        // Y held 0.35 s: the second ability, pressed for as long as Y stays down
        if (xr.clickHeld[0] && GetBool(xr.btn[kY])) p.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    }

    // Menu button: short press = Start (pause). In gameplay, held 1.5 s = adjust how the gun sits in the hand;
    // elsewhere held 0.6 s = Back.
    bool menu = GetBool(xr.btn[kMenu]);
    if (menu && !xr.menuWasDown) xr.menuDownAt = now;
    if (menu != xr.menuWasDown && g_cfg.logFps) Log("vr: menu button %s", menu ? "down" : "up");
    // the setup: menu button held + A (hard to hit by accident; also held 1.5 s alone); any menu press leaves it
    if (g_setup.active && menu && !xr.menuWasDown) { SetupToggle(); xr.menuDownAt = 0; }
    static bool comboA;
    bool aNow = GetBool(xr.btn[kA]);
    if (menu && gameplay) {
        if (aNow && !comboA && xr.menuDownAt) { SetupToggle(); xr.menuDownAt = 0; }
        p.wButtons &= ~XINPUT_GAMEPAD_A;   // (A is not a jump meanwhile)
    }
    comboA = aNow;
    if (menu && xr.menuDownAt && gameplay && now - xr.menuDownAt > 1500) { SetupToggle(); xr.menuDownAt = 0; }
    if (menu && xr.menuDownAt && !gameplay && now - xr.menuDownAt > 600) { xr.backPulse = 8; xr.menuDownAt = 0; }
    if (!menu && xr.menuWasDown && xr.menuDownAt) xr.startPulse = 8;
    xr.menuWasDown = menu;
    if (xr.startPulse > 0) { p.wButtons |= XINPUT_GAMEPAD_START; xr.startPulse--; }
    if (xr.backPulse > 0) { p.wButtons |= XINPUT_GAMEPAD_BACK; xr.backPulse--; }

    if (gameplay) TrackWeaponSetup();
    if (g_setup.active) {
        if (gameplay) UpdateSetup(p, grip, now);
        else SetupToggle();   // (a cutscene or the pause menu ends it, saved)
    } else if (g_calib.active) {
        // adjusting the grip (debug "calib"): the trigger confirms (does not shoot)
        bool fire = p.bRightTrigger > 150;
        if (fire && !g_calib.triggerWas) GripCalibrationConfirm(grip[0] > 0.6f);
        g_calib.triggerWas = fire;
        p.bRightTrigger = 0;
    }

    if (gameplay) UpdateGestures(p, now);

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

static void DrawSetupCubes(ID3D11DeviceContext* imm, ID3D11RenderTargetView* rtv, UINT w, UINT h, const XrPosef& eye, const XrFovf& fov);
static const XrPosef* g_overlayPose;   // set while an eye image is copied: things drawn into it (setup cubes)
static XrFovf g_overlayFov;
static ID3D11ShaderResourceView* g_overlayHud;   // this frame's HUD (minimal HUD in gameplay): its markers go in too

// Copies the game's back buffer into the next image of a swapchain (or, with scopeRect, a part of it into a
// size x size round scope image).
static bool CopyToChain(Chain& c, ID3D11Texture2D* bb, const float* scopeRect = nullptr, UINT size = 0)
{
    D3D11_TEXTURE2D_DESC bd;
    bb->GetDesc(&bd);
    UINT cw = size ? size : bd.Width, ch = size ? size : bd.Height;
    if (!c.sc || c.width != cw || c.height != ch)
        if (!CreateChain(c, cw, ch)) return false;

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
    if (srv) xr.blit.Draw(xr.device, imm, srv, c.rtvs[idx], c.width, c.height, xr.linearize, false, 0.0f, scopeRect);
    if (g_overlayPose && !scopeRect) {
        // the HUD's moving parts (waypoints, names over teammates, lock-on marks): drawn by the game for this eye's
        // view, so they sit at their place in depth - into the eye image, not onto the panel
        if (g_overlayHud) xr.blit.Draw(xr.device, imm, g_overlayHud, c.rtvs[idx], c.width, c.height, xr.linearize, true, g_cfg.hudCrosshairMask, nullptr, 4.0f);
        // (the setup's zone marks are compositor quads now - BuildZoneMark: drawing them into the eye image here
        // stopped the frames on the real runtime, black screen)
    }
    SafeRelease(srv); SafeRelease(copy); SafeRelease(resolved); SafeRelease(imm);
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(c.sc, &ri);
    return true;
}


// Rotation with the given axes (columns x, y, z; orthonormal) as a quaternion.
static XrQuaternionf BasisToQuat(XrVector3f x, XrVector3f y, XrVector3f z)
{
    float tr = x.x + y.y + z.z;
    if (tr > 0) { float s = sqrtf(tr + 1.0f) * 2; return { (y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s }; }
    if (x.x > y.y && x.x > z.z) { float s = sqrtf(1.0f + x.x - y.y - z.z) * 2; return { 0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s }; }
    if (y.y > z.z) { float s = sqrtf(1.0f + y.y - x.x - z.z) * 2; return { (y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s }; }
    float s = sqrtf(1.0f + z.z - x.x - y.y) * 2;
    return { (z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s };
}

// ---------------------------------------------------------------------------------------------
// Weapon wheel (like Half-Life: Alyx): hold a grip and a ring of choices appears around that hand; move the hand
// onto one and let go to take it. Right hand: the weapons, and the melee weapon (it then stays in the hand and you
// fight by swinging it); a short squeeze just takes the next weapon. Left hand (grip away from the weapon's
// front): melee weapon or empty hand - a gun in the right hand and a melee weapon in the left.
struct Wheel {
    bool open = false;
    int hand = 1;
    XrVector3f center = {}, right = {}, up = {};
    XrQuaternionf orient = { 0, 0, 0, 1 };
    int count = 0, hot = -1, current = -1;
    int item[10] = {};                       // weapon list index per slot, or one of the kItem values
    wchar_t label[10][40] = {};
    bool dirty = true;
    int presses = 0, pressPhase = 0;         // queued "next weapon" button presses
};
enum { kItemMelee = -1, kItemNext = -2, kItemEmpty = -3 };
static Wheel g_wheel;
static Chain g_wheelChain;
static bool WheelIsOpen() { return g_wheel.open; }
static const UINT kWheelPx = 384;
static const wchar_t kLabelNext[] = L"\x0421\x043B\x0435\x0434. \x043E\x0440\x0443\x0436\x0438\x0435";     // "След. оружие"
static const wchar_t kLabelEmpty[] = L"\x041F\x0443\x0441\x0442\x0430\x044F \x0440\x0443\x043A\x0430";    // "Пустая рука"

static void WheelAdd(Wheel& w, int item, const wchar_t* label)
{
    if (w.count >= 10) return;
    w.item[w.count] = item;
    wcscpy_s(w.label[w.count++], label);
}

static void WheelOpen(XrTime t, int handIndex)
{
    XrPosef hand, head;
    if (!LocateHand(xr.gripSpace[handIndex], t, hand) || !xr.viewSpace || !LocateHand(xr.viewSpace, t, head)) return;
    Wheel& w = g_wheel;
    w.count = 0;
    w.current = -1;
    int melee = WeaponsMeleeHand();
    if (handIndex == 1) {
        WeaponList wl;
        WeaponsGetList(wl);
        for (int i = 0; i < wl.count && w.count < 8; i++) {
            if (i == wl.current && melee != 1 && VRGunHand() == 1) w.current = w.count;
            WheelAdd(w, i, wl.name[i]);
        }
        if (!wl.count) WheelAdd(w, kItemNext, kLabelNext);   // inventory unknown: still offer "next weapon"
        if (VRGunHand() != 1) w.current = w.count;
        WheelAdd(w, kItemEmpty, kLabelEmpty);   // (the right hand: guns or empty - the melee weapon is the left's)
    } else {
        // the left hand: empty (no melee weapon here - a punch with the empty left hand shows the attack's weapon in it)
        if (VRGunHand() != 0) w.current = w.count;
        WheelAdd(w, kItemEmpty, kLabelEmpty);
    }
    // the ring faces the head, upright
    w.center = hand.position;
    XrVector3f f = { head.position.x - hand.position.x, head.position.y - hand.position.y, head.position.z - hand.position.z };
    float l = sqrtf(f.x * f.x + f.y * f.y + f.z * f.z);
    f = l > 0.05f ? XrVector3f{ f.x / l, f.y / l, f.z / l } : XrVector3f{ 0, 0, 1 };
    XrVector3f r = { f.z, 0.0f, -f.x };   // up(0,1,0) x f
    float lr = sqrtf(r.x * r.x + r.z * r.z);
    if (lr < 1e-3f) return;
    r = { r.x / lr, 0.0f, r.z / lr };
    XrVector3f u = { f.y * r.z - f.z * r.y, f.z * r.x - f.x * r.z, f.x * r.y - f.y * r.x };
    w.right = r; w.up = u;
    w.orient = BasisToQuat(r, u, f);
    w.hand = handIndex;
    w.hot = -1;
    w.dirty = true;
    w.open = true;
    VRHaptic(handIndex, 0.3f, 0.03f);
}

static void WheelChoose()
{
    Wheel& w = g_wheel;
    if (w.hot < 0 || w.hot >= w.count) return;
    int item = w.item[w.hot];
    int melee = WeaponsMeleeHand();
    // the gun is in one hand at a time: taken into the other, it leaves the first (mirrored); a left hand that gives it
    // up (melee weapon, empty) hands it back to the right
    if (item == kItemMelee) {
        WeaponsSetMeleeHand(w.hand);
        if (w.hand == 1) g_rightEmpty = false;
        else if (g_gunHand == 0) g_gunHand = 1;
    } else if (item == kItemEmpty) {
        if (melee == w.hand) WeaponsSetMeleeHand(-1);
        if (w.hand == 1) { if (g_gunHand == 1) g_rightEmpty = true; }
        else if (g_gunHand == 0) g_gunHand = 1;
    } else {
        g_rightEmpty = false;
        g_gunHand = w.hand;
        if (melee == w.hand) WeaponsSetMeleeHand(-1);
        if (item == kItemNext) w.presses = 1;
        else WeaponsSelect(item);
    }
    xr.twoHand = false;   // (whatever was chosen: the hands take it up anew)
    Log("vr: weapon wheel (%s hand) -> slot %d (item %d)", w.hand ? "right" : "left", w.hot, item);
    VRHaptic(w.hand, 0.6f, 0.05f);
}

static volatile int g_dbgWheelHand = -2, g_dbgWheelHot = -1;   // debug "wheel <hand> <slot>" (hand -1 = choose)
void VRDebugWheel(int hand, int hot) { g_dbgWheelHot = hot; g_dbgWheelHand = hand; }

// grip[h]: squeeze of each hand; a hand whose grip holds the weapon's front (two-handed) does not open a wheel.
static void UpdateWeaponWheel(XINPUT_GAMEPAD& p, const float grip[2], DWORD now)
{
    Wheel& w = g_wheel;
    XrTime t = g_lastDisplayTime.load();
    if (g_setup.active) {   // the grips set up the gun there
        w.open = false;
        xr.gripAt[0] = xr.gripAt[1] = 0;
        return;
    }
    bool debug = g_dbgWheelHand != -2;
    if (g_dbgWheelHand >= 0) {
        if (!w.open && t) WheelOpen(t, g_dbgWheelHand);
        if (w.open && w.hot != g_dbgWheelHot) { w.hot = g_dbgWheelHot; w.dirty = true; }
    } else if (g_dbgWheelHand == -1) {
        if (w.open) WheelChoose();
        w.open = false;
        g_dbgWheelHand = -2;
    }
    // The wheel is on B held (not the grips: grabbing things kept opening it; not the stick clicks: they wear the
    // sticks): B held 0.35 s opens the right hand's wheel, let go to choose; a short B is the game's B (grenade), sent
    // when it is let go. In vehicle form B held switches the vehicle's weapon (the game's "next weapon").
    // Y the same way: tapped = main ability (sent when let go), held = second ability (VRInput).
    bool vehicle = IKPawnIsVehicle();
    for (int h = 0; h < 2 && !debug; h++) {   // h: 1 = B (right hand), 0 = Y
        DWORD& at = xr.clickAt[h];
        bool click = GetBool(xr.btn[h ? kB : kY]);
        if (click) {
            if (!at) at = now;
            if (now - at > 350 && !xr.clickHeld[h]) {
                xr.clickHeld[h] = true;
                if (h == 1) {
                    if (vehicle) w.presses = 1;
                    else if (!w.open && t) WheelOpen(t, 1);
                }
            }
        } else {
            if (at && w.open && w.hand == h) WheelChoose();
            else if (at && !xr.clickHeld[h]) xr.clickPulse[h] = 8;   // a tap
            if (w.open && w.hand == h) w.open = false;
            at = 0;
            xr.clickHeld[h] = false;
        }
    }
    (void)grip;
    if (w.open && t && !debug) {
        XrPosef hand;
        if (LocateHand(xr.gripSpace[w.hand], t, hand)) {
            XrVector3f d = { hand.position.x - w.center.x, hand.position.y - w.center.y, hand.position.z - w.center.z };
            float x = d.x * w.right.x + d.y * w.right.y + d.z * w.right.z, y = d.x * w.up.x + d.y * w.up.y + d.z * w.up.z;
            int hot = -1;
            if (sqrtf(x * x + y * y) > g_cfg.wheelSize * 0.12f && w.count) {
                // slot 0 at the top, then clockwise
                float a = atan2f(x, y);
                if (a < 0) a += 6.2831853f;
                hot = (int)floorf(a / (6.2831853f / w.count) + 0.5f) % w.count;
            }
            if (hot != w.hot) { w.hot = hot; w.dirty = true; if (hot >= 0) VRHaptic(w.hand, 0.15f, 0.02f); }
        }
    }
    // weapon switches: the game's "next weapon" button, pressed and released
    if (WeaponsWantPress(false)) w.presses++;
    if (w.presses > 0) {
        if (w.pressPhase < 6) p.wButtons |= XINPUT_GAMEPAD_Y;
        if (++w.pressPhase >= 14) { w.pressPhase = 0; w.presses--; }
    } else {
        w.pressPhase = 0;
    }
}

// Draws the ring in the look of the game's own interface: dark see-through panels with thin cyan edges, the
// choice under the hand lit in the game's orange (its menus' highlight, #FF9333), the weapon in hand in cyan,
// names in capitals in a square condensed face. GDI into a bitmap twice the size (no anti-aliasing of its own),
// then halved into the swapchain image: smooth edges in the headset.
static bool DrawWheel()
{
    Wheel& w = g_wheel;
    const UINT S = kWheelPx, K = 2, B = S * K;
    if (!g_wheelChain.sc && !CreateChain(g_wheelChain, S, S)) return false;
    static std::vector<uint32_t> px;
    if (w.dirty || px.empty()) {
        BITMAPINFO bi = {};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), (LONG)B, -(LONG)B, 1, 32, BI_RGB };
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp) { DeleteDC(dc); return false; }
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        const COLORREF kKey = RGB(255, 0, 255);   // background: left transparent
        const COLORREF kPanel = RGB(10, 22, 28), kCurrent = RGB(18, 62, 76), kHot = RGB(255, 147, 51);
        const COLORREF kEdge = RGB(64, 186, 222), kHotEdge = RGB(255, 214, 150), kText = RGB(150, 222, 245), kHotText = RGB(26, 16, 6);
        {
            HBRUSH key = CreateSolidBrush(kKey);
            RECT all = { 0, 0, (LONG)B, (LONG)B };
            FillRect(dc, &all, key);
            DeleteObject(key);
        }
        float c = B / 2.0f, R = B / 2.0f - 2.0f * K;
        float rIn = R * 0.36f, rOut = R - 1.0f * K;
        auto pt = [&](float a, float r) { return POINT{ (LONG)lroundf(c + sinf(a) * r), (LONG)lroundf(c - cosf(a) * r) }; };
        int n = std::max(w.count, 1);
        float step = 6.2831853f / n, gap = std::min(0.05f, step * 0.08f);
        HPEN edge = CreatePen(PS_SOLID, 2 * K, kEdge), hotEdge = CreatePen(PS_SOLID, 3 * K, kHotEdge);
        HBRUSH panel = CreateSolidBrush(kPanel), current = CreateSolidBrush(kCurrent), hot = CreateSolidBrush(kHot);
        HGDIOBJ oldPen = SelectObject(dc, edge), oldBrush = SelectObject(dc, panel);
        // the slots: ring segments with gaps between them
        for (int i = 0; i < w.count; i++) {
            float a0 = i * step - step / 2 + gap, a1 = i * step + step / 2 - gap;
            POINT p[48];
            int m = 0;
            const int arc = 20;
            for (int k = 0; k <= arc; k++) p[m++] = pt(a0 + (a1 - a0) * k / arc, rOut);
            for (int k = arc; k >= 0; k--) p[m++] = pt(a0 + (a1 - a0) * k / arc, rIn);
            SelectObject(dc, i == w.hot ? hot : i == w.current ? current : panel);
            SelectObject(dc, i == w.hot ? hotEdge : edge);
            Polygon(dc, p, m);
        }
        // the middle: an octagon with the name of the choice under the hand (or of the weapon in hand)
        {
            POINT p[8];
            float r0 = R * 0.29f;
            for (int k = 0; k < 8; k++) p[k] = pt(6.2831853f * (k + 0.5f) / 8, r0);
            SelectObject(dc, panel);
            SelectObject(dc, edge);
            Polygon(dc, p, 8);
        }
        HFONT font = CreateFontW(-(int)(B / 15), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0,
                                 L"Bahnschrift SemiBold Condensed");
        HFONT smallFont = CreateFontW(-(int)(B / 19), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0,
                                  L"Bahnschrift SemiBold Condensed");
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        auto upper = [](const wchar_t* s, wchar_t* out, size_t n) { wcsncpy_s(out, n, s, _TRUNCATE); CharUpperW(out); };
        auto text = [&](const wchar_t* s, POINT q, int hw, COLORREF col) {
            wchar_t u[48];
            upper(s, u, 48);
            RECT m = { q.x - hw, 0, q.x + hw, 0 };
            DrawTextW(dc, u, -1, &m, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
            int th = m.bottom - m.top;
            RECT rc = { q.x - hw, q.y - th / 2, q.x + hw, q.y - th / 2 + th };
            SetTextColor(dc, col);
            DrawTextW(dc, u, -1, &rc, DT_CENTER | DT_WORDBREAK);
        };
        for (int i = 0; i < w.count; i++) {
            float mid = (rIn + rOut) / 2;
            int hw = (int)std::min(R * 0.30f, mid * 2.0f * sinf(step / 2 - gap) * 0.45f + R * 0.05f);
            text(w.label[i], pt(i * step, mid), hw, i == w.hot ? kHotText : kText);
        }
        int centerItem = w.hot >= 0 ? w.hot : w.current;
        if (centerItem >= 0 && centerItem < w.count) {
            SelectObject(dc, smallFont);
            text(w.label[centerItem], POINT{ (LONG)c, (LONG)c }, (int)(R * 0.24f), w.hot >= 0 ? kHot : kText);
        }
        GdiFlush();
        // halve: covered sub-pixels give the alpha (panels slightly see-through), their colours the colour
        const uint32_t* src = (const uint32_t*)bits;
        bool rgba = xr.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || xr.format == DXGI_FORMAT_R8G8B8A8_UNORM;
        const uint32_t key = 0xFF00FF;
        px.assign(S * S, 0);
        for (UINT y = 0; y < S; y++)
            for (UINT x = 0; x < S; x++) {
                uint32_t r = 0, g = 0, b = 0, cov = 0;
                for (UINT sy = 0; sy < K; sy++)
                    for (UINT sx = 0; sx < K; sx++) {
                        uint32_t v = src[(y * K + sy) * B + x * K + sx] & 0xFFFFFF;
                        if (v == key) continue;
                        b += v & 0xFF; g += (v >> 8) & 0xFF; r += (v >> 16) & 0xFF; cov++;
                    }
                if (!cov) continue;
                float a = 0.9f * cov / (K * K);
                r = (uint32_t)(r / cov * a); g = (uint32_t)(g / cov * a); b = (uint32_t)(b / cov * a);   // premultiplied
                px[y * S + x] = (rgba ? (r | (g << 8) | (b << 16)) : (b | (g << 8) | (r << 16))) | ((uint32_t)(255 * a) << 24);
            }
        SelectObject(dc, oldFont); DeleteObject(font); DeleteObject(smallFont);
        SelectObject(dc, oldPen); SelectObject(dc, oldBrush);
        DeleteObject(edge); DeleteObject(hotEdge); DeleteObject(panel); DeleteObject(current); DeleteObject(hot);
        SelectObject(dc, oldBmp); DeleteObject(bmp); DeleteDC(dc);
        w.dirty = false;
    }    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrAcquireSwapchainImage(g_wheelChain.sc, &ai, &idx))) return false;
    xrWaitSwapchainImage(g_wheelChain.sc, &wi);
    ID3D11DeviceContext* imm = nullptr;
    xr.device->GetImmediateContext(&imm);
    imm->UpdateSubresource(g_wheelChain.images[idx].texture, 0, nullptr, px.data(), S * 4, 0);
    imm->Release();
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(g_wheelChain.sc, &ri);
    return true;
}

static bool BuildWheel(XrCompositionLayerQuad& q)
{
    if (!g_wheel.open || !DrawWheel()) return false;
    q = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = xr.localSpace;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = g_wheelChain.sc;
    q.subImage.imageRect = { { 0, 0 }, { (int32_t)kWheelPx, (int32_t)kWheelPx } };
    q.pose.orientation = g_wheel.orient;
    q.pose.position = g_wheel.center;
    q.size = { g_cfg.wheelSize, g_cfg.wheelSize };
    return true;
}

// ---- Setup mode (see g_setup) ---------------------------------------------------------------------------------
static std::wstring IniPath() { return ModDir() + L"twfcvr.ini"; }

static int IniFloats(const wchar_t* sec, const wchar_t* key, float* out, int n)
{
    wchar_t b[256];
    GetPrivateProfileStringW(sec, key, L"", b, 256, IniPath().c_str());
    int k = 0;
    for (wchar_t *s = b, *end; k < n && *s; s = *end ? end + 1 : end, k++) {
        out[k] = wcstof(s, &end);
        if (end == s) break;
    }
    return k;
}

static void IniWriteFloats(const wchar_t* sec, const wchar_t* key, const float* v, int n)
{
    std::wstring s;
    wchar_t b[32];
    for (int i = 0; i < n; i++) { swprintf_s(b, i ? L",%.4f" : L"%.4f", v[i]); s += b; }
    WritePrivateProfileStringW(sec, key, s.c_str(), IniPath().c_str());
}

// The weapon in hand changed: its own settings (or none: the global ones stay).
static void TrackWeaponSetup()
{
    WeaponList wl;
    WeaponsGetList(wl);
    if (wl.current < 0 || wl.current >= wl.count || g_ws.name == wl.name[wl.current]) {
        if (g_ws.grip) memcpy(g_cfg.gunGrip, g_ws.gunGrip, sizeof(g_ws.gunGrip));   // (an ini reload resets g_cfg)
        if (g_ws.fore) { memcpy(g_cfg.foregrip, g_ws.foregrip, sizeof(g_ws.foregrip)); g_cfg.foregripSet = 1; }
        return;
    }
    // back to the global ones, then this weapon's
    static float globalGrip[7], globalFore[3];
    static int globalForeSet = -1;
    if (globalForeSet < 0) {
        memcpy(globalGrip, g_cfg.gunGrip, sizeof(globalGrip));
        memcpy(globalFore, g_cfg.foregrip, sizeof(globalFore));
        globalForeSet = g_cfg.foregripSet;
    }
    if (g_ws.grip) memcpy(g_cfg.gunGrip, globalGrip, sizeof(globalGrip));
    if (g_ws.fore) { memcpy(g_cfg.foregrip, globalFore, sizeof(globalFore)); g_cfg.foregripSet = globalForeSet; }
    g_ws = WeaponSetup();
    g_ws.name = wl.name[wl.current];
    std::wstring sec = L"Weapon." + g_ws.name;
    g_ws.grip = IniFloats(sec.c_str(), L"GunGrip", g_ws.gunGrip, 7) == 7;
    g_ws.fore = IniFloats(sec.c_str(), L"Foregrip", g_ws.foregrip, 3) == 3;
    g_ws.scopeSet = IniFloats(sec.c_str(), L"Scope", g_ws.scope, 4) == 4;
    g_ws.gripL = IniFloats(sec.c_str(), L"GunGripL", g_ws.gunGripL, 7) == 7;
    g_setup.dirty = true;
    Log("vr: weapon setup for %ls: grip %d, front %d, lens %d", g_ws.name.c_str(), g_ws.grip, g_ws.fore, g_ws.scopeSet);
}

// The aiming lens: right, up, forward (metres from the aim ray's origin) and size - this weapon's or the global ones.
static void ScopePlace(float s[4])
{
    if (g_ws.scopeSet) { memcpy(s, g_ws.scope, 4 * sizeof(float)); return; }
    s[0] = 0; s[1] = g_cfg.scopeUp; s[2] = g_cfg.scopeForward; s[3] = g_cfg.scopeSize;
}

static void SaveSetup()
{
    if (!g_ws.name.empty()) {
        std::wstring sec = L"Weapon." + g_ws.name;
        if (g_ws.grip) IniWriteFloats(sec.c_str(), L"GunGrip", g_ws.gunGrip, 7);
        if (g_ws.fore) IniWriteFloats(sec.c_str(), L"Foregrip", g_ws.foregrip, 3);
        if (g_ws.scopeSet) IniWriteFloats(sec.c_str(), L"Scope", g_ws.scope, 4);
        if (g_ws.gripL) IniWriteFloats(sec.c_str(), L"GunGripL", g_ws.gunGripL, 7);
        WritePrivateProfileStringW(sec.c_str(), L"Reticle", nullptr, IniPath().c_str());   // (the old reticle offset)
    }
    IniWriteFloats(L"Input", L"BeltOffset", g_cfg.beltOffset, 3);
    Log("vr: setup saved (%ls)", g_ws.name.c_str());
}

static void SetupToggle()
{
    if (g_setup.active) {
        g_setup.active = false;
        g_page = 1;   // (opens on the menu next time)
        g_calib.active = false;
        g_setup.grab = -1;
        SaveSetup();
        VRHaptic(0, 0.6f, 0.12f);
        VRHaptic(1, 0.6f, 0.12f);
        return;
    }
    XrTime t = g_lastDisplayTime.load();
    XrPosef head;
    if (!t || !xr.viewSpace || !LocateHand(xr.viewSpace, t, head)) return;
    // the panel: in front, a little low, facing the head
    XrVector3f f = Rotate(head.orientation, { 0, 0, -1 });
    float l = sqrtf(f.x * f.x + f.z * f.z);
    f = l > 1e-3f ? XrVector3f{ f.x / l, 0, f.z / l } : XrVector3f{ 0, 0, -1 };
    g_setup.panel.position = { head.position.x + f.x * 0.9f, head.position.y - 0.12f, head.position.z + f.z * 0.9f };
    XrVector3f back = { -f.x, 0, -f.z }, right = { back.z, 0, -back.x }, up = { 0, 1, 0 };   // (right = up x back)
    g_setup.panel.orientation = BasisToQuat(right, up, back);
    g_setup.active = true;
    g_setupEnteredAt = GetTickCount();
    g_setup.rotate = false;
    g_setup.grab = -1;
    g_setup.rightWas = g_setup.leftWas = g_setup.aWas = g_setup.bWas = true;   // (held buttons do nothing yet)
    g_setup.at = GetTickCount();
    g_setup.dirty = true;
    VRHaptic(0, 0.5f, 0.1f);
    VRHaptic(1, 0.5f, 0.1f);
    Log("vr: setup mode (%ls)", g_ws.name.c_str());
}

static void UpdateSetup(XINPUT_GAMEPAD& p, const float grip[2], DWORD now)
{
    Setup& s = g_setup;
    if (now - g_setupEnteredAt > 180000) { SetupToggle(); return; }   // (left on: back to the game after 3 min)
    XrTime t = g_lastDisplayTime.load();
    float dt = std::min((now - s.at) / 1000.0f, 0.1f);
    s.at = now;
    XrPosef head;
    if (t && xr.viewSpace && LocateHand(xr.viewSpace, t, head)) s.cube[1] = BeltPoint(head);
    else return;

    // the menu (page 1, shown first) and the weapon page (0): Y switches; on the menu the hands only point and press
    static bool yWas = true;
    bool y = GetBool(xr.btn[kY]);
    if (y && !yWas) { g_page ^= 1; s.dirty = true; g_calib.active = false; s.grab = -1; VRHaptic(0, 0.3f, 0.03f); }
    yWas = y;
    if (g_page == 1) {
        UpdateSettingsPage(t, head, now);
        s.rightWas = s.leftWas = s.aWas = s.bWas = true;   // (held into the weapon page: nothing happens yet)
        p = {};
        return;
    }
    if (s.grab != 0) s.cube[0] = { g_forePos.x, g_forePos.y, g_forePos.z };
    // the gun's hand (G: the right one, or the left when the gun is there) sets the grip, the other (S) moves the zones
    const int G = GunIdx(), S = G ^ 1;
    XrPosef rawAim, rawGrip, lp;
    bool haveRight = LocateHand(xr.aimSpace[G], t, rawAim) && LocateHand(xr.gripSpace[G], t, rawGrip);
    bool haveLeft = LocateHand(xr.gripSpace[S], t, lp);

    // the gun hand's grip held: the gun stays where it is, the hand moves to where it should hold it
    bool rg = grip[G] > (s.rightWas ? 0.35f : 0.6f);
    if (rg && !s.rightWas) {
        XrPosef aim;
        XrVector3f gp;
        if (LocateRightAim(t, aim, &gp)) { g_calib.aim = aim; g_calib.grip = gp; g_calib.active = true; VRHaptic(G, 0.4f, 0.05f); }
    } else if (!rg && s.rightWas && g_calib.active && haveRight) {
        XrQuaternionf inv = QuatConj(rawAim.orientation);
        XrVector3f d = Rotate(inv, { g_calib.grip.x - rawGrip.position.x, g_calib.grip.y - rawGrip.position.y, g_calib.grip.z - rawGrip.position.z });
        // the left hand keeps a placement of its own per weapon (GunGripL), not the right one mirrored
        float cur[7];
        GunGripFor(G, cur);
        XrQuaternionf r = s.rotate ? QuatMul(inv, g_calib.aim.orientation) : XrQuaternionf{ cur[0], cur[1], cur[2], cur[3] };
        float g[7] = { r.x, r.y, r.z, r.w, d.x, d.y, d.z };
        if (G == 1 && VRGunHand() < 0) {   // the empty right hand: a placement of its own (not a weapon's)
            memcpy(g_cfg.emptyGripR, g, sizeof(g));
            IniWriteFloats(L"VR", L"EmptyGripR", g, 7);
        } else if (G == 1) {
            memcpy(g_ws.gunGrip, g, sizeof(g));
            memcpy(g_cfg.gunGrip, g, sizeof(g));
            g_ws.grip = true;
        } else {
            memcpy(g_ws.gunGripL, g, sizeof(g));
            g_ws.gripL = true;
        }
        g_calib.active = false;
        s.dirty = true;
        VRHaptic(G, 0.8f, 0.1f);
    }
    s.rightWas = rg;

    // the other hand's grip at a green cube: carry it
    bool lg = grip[S] > (s.leftWas ? 0.35f : 0.6f);
    XrVector3f hand = haveLeft ? lp.position : XrVector3f{};
    if (lg && !s.leftWas && haveLeft) {
        float best = 0.14f;
        for (int i = 0; i < 2; i++) {
            float dx = s.cube[i].x - hand.x, dy = s.cube[i].y - hand.y, dz = s.cube[i].z - hand.z, d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d < best) { best = d; s.grab = i; s.grabOffset = { dx, dy, dz }; }
        }
        if (s.grab >= 0) VRHaptic(S, 0.4f, 0.05f);
    }
    if (s.grab >= 0 && haveLeft) s.cube[s.grab] = { hand.x + s.grabOffset.x, hand.y + s.grabOffset.y, hand.z + s.grabOffset.z };
    if (!lg && s.leftWas && s.grab >= 0) {
        const XrVector3f& c = s.cube[s.grab];
        if (s.grab == 0) {
            XrPosef aim;
            XrVector3f gp;
            if (LocateRightAim(t, aim, &gp)) {   // relative to the gun hand's grip, in the gun's aim axes
                XrVector3f f = Rotate(QuatConj(aim.orientation), { c.x - gp.x, c.y - gp.y, c.z - gp.z });
                if (G == 0) f.x = -f.x;   // (kept as for the right hand: the left one mirrors it)
                g_ws.foregrip[0] = f.x; g_ws.foregrip[1] = f.y; g_ws.foregrip[2] = f.z;
                memcpy(g_cfg.foregrip, g_ws.foregrip, sizeof(g_ws.foregrip));
                g_cfg.foregripSet = 1;
                g_ws.fore = true;
            }
        } else {   // the pouch, from the eyes: right, up, forward
            XrVector3f fw = Rotate(head.orientation, { 0, 0, -1 });
            float l = sqrtf(fw.x * fw.x + fw.z * fw.z);
            fw = l > 1e-3f ? XrVector3f{ fw.x / l, 0, fw.z / l } : XrVector3f{ 0, 0, -1 };
            XrVector3f d = { c.x - head.position.x, c.y - head.position.y, c.z - head.position.z };
            g_cfg.beltOffset[0] = d.x * -fw.z + d.z * fw.x;
            g_cfg.beltOffset[1] = d.y;
            g_cfg.beltOffset[2] = d.x * fw.x + d.z * fw.z;
        }
        s.grab = -1;
        s.dirty = true;
        VRHaptic(0, 0.8f, 0.1f);
    }
    s.leftWas = lg;

    // B: the rotation is set too; A: this weapon back to the defaults
    bool b = GetBool(xr.btn[kB]), a = GetBool(xr.btn[kA]);
    if (b && !s.bWas) { s.rotate = !s.rotate; s.dirty = true; VRHaptic(1, 0.3f, 0.03f); }
    if (a && !s.aWas && !g_ws.name.empty()) {
        std::wstring sec = L"Weapon." + g_ws.name;
        WritePrivateProfileStringW(sec.c_str(), nullptr, nullptr, IniPath().c_str());   // (the whole section)
        std::wstring keep = g_ws.name;
        g_ws.name.clear();   // reload: the global ones
        TrackWeaponSetup();
        Log("vr: setup of %ls reset", keep.c_str());
        VRHaptic(1, 0.8f, 0.2f);
    }
    s.aWas = a; s.bWas = b;

    (void)dt;
    p = {};   // the game gets nothing meanwhile (no shots, no steps)
}

// The menu (setup page 1, shown first): the mod's most important settings, changed in place and written to the ini
// (applied within a second), and actions - the weapon page, the T-pose calibration, leave. Pointed at with a ray from
// the right hand and pressed with its trigger (on a value: its left half lowers, its right half raises it), or the left
// stick (up/down picks, left/right changes) and A; the right stick scrolls.
struct SettingItem { const wchar_t* sec; const wchar_t* key; const wchar_t* label; float def, step, lo, hi; };   // sec null: an action
static const SettingItem kSettings[] = {
    { nullptr, L"@weapon", L"\x041D\x0430\x0441\x0442\x0440\x043E\x0439\x043A\x0430 \x043E\x0440\x0443\x0436\x0438\x044F: \x0445\x0432\x0430\x0442, \x0446\x0435\x0432\x044C\x0451, \x043B\x0438\x043D\x0437\x0430, \x043F\x043E\x0434\x0441\x0443\x043C\x043E\x043A", 0, 0, 0, 0 },   // ��������� ������: ����, �����, �����, ��������
    { nullptr, L"@camera", L"\x041F\x043E\x043B\x043E\x0436\x0435\x043D\x0438\x0435 \x043A\x0430\x043C\x0435\x0440\x044B \x043C\x0430\x0448\x0438\x043D\x044B (\x0442\x044F\x043D\x0443\x0442\x044C \x043C\x0438\x0440 \x0433\x0440\x0438\x043F\x043E\x043C)", 0, 0, 0, 0 },   // ����������: �-���� (������� ���� ��� ������)
    { L"VR", L"WorldScale", L"\x041C\x0430\x0441\x0448\x0442\x0430\x0431 \x043C\x0438\x0440\x0430", 200, 10, 100, 400 },   // ������� ���� (��� ����������)
    { L"VR", L"FirstPersonEyeHeight", L"\x0412\x044B\x0441\x043E\x0442\x0430 \x0433\x043B\x0430\x0437", 150, 5, 50, 300 },   // ������ ����
    { L"VR", L"SyncEyeTime", L"\x041E\x0431\x0430 \x0433\x043B\x0430\x0437\x0430 \x2014 \x043E\x0434\x0438\x043D \x043C\x043E\x043C\x0435\x043D\x0442 \x0438\x0433\x0440\x044B", 1, 1, 0, 1 },   // ��� ����� � ���� ������ ����
    { L"VR", L"HandScale", L"\x0420\x0430\x0437\x043C\x0435\x0440 \x043A\x0438\x0441\x0442\x0435\x0439", 0.6f, 0.05f, 0.2f, 1.5f },   // ������ ������
    { L"VR", L"WeaponScale", L"\x0420\x0430\x0437\x043C\x0435\x0440 \x043E\x0440\x0443\x0436\x0438\x044F", 0.6f, 0.05f, 0.2f, 1.5f },   // ������ ������
    { L"VR", L"LimbThickness", L"\x0422\x043E\x043B\x0449\x0438\x043D\x0430 \x0440\x0443\x043A", 0.5f, 0.05f, 0.2f, 1 },   // ������� ���
    { L"VR", L"LegThickness", L"\x0422\x043E\x043B\x0449\x0438\x043D\x0430 \x043D\x043E\x0433", 0.75f, 0.05f, 0.2f, 1 },   // ������� ���
    { L"VR", L"ShowBody", L"\x0422\x0435\x043B\x043E: 0 \x0440\x0443\x043A\x0438, 1 + \x043D\x043E\x0433\x0438, 2 \x0432\x0441\x0451", 1, 1, 0, 2 },   // ����: 0 ����, 1 + ����, 2 ��
    { L"VR", L"FingerPoses", L"\x041F\x0430\x043B\x044C\x0446\x044B \x043F\x043E \x043A\x043E\x043D\x0442\x0440\x043E\x043B\x043B\x0435\x0440\x0443", 1, 1, 0, 1 },   // ������ �� �����������
    { L"VR", L"Reticle", L"\x041A\x043E\x043B\x044C\x0446\x043E \x043F\x0440\x0438\x0446\x0435\x043B\x0430", 1, 1, 0, 1 },   // ������ �������
    { L"VR", L"ReticleSize", L"\x0420\x0430\x0437\x043C\x0435\x0440 \x043F\x0440\x0438\x0446\x0435\x043B\x0430 \x0438\x0433\x0440\x044B", 0.6f, 0.1f, 0.2f, 3 },   // ������ ������� ����
    { L"Input", L"SnapTurnDegrees", L"\x041F\x043E\x0432\x043E\x0440\x043E\x0442 \x0440\x044B\x0432\x043A\x043E\x043C (0 = \x043F\x043B\x0430\x0432\x043D\x043E)", 0, 15, 0, 90 },   // ������� ������ (0 = ������)
    { L"Input", L"SmoothTurnSpeed", L"\x0421\x043A\x043E\x0440\x043E\x0441\x0442\x044C \x043F\x043E\x0432\x043E\x0440\x043E\x0442\x0430", 120, 15, 30, 360 },   // �������� ��������
    { L"Input", L"MoveByHead", L"\x0418\x0434\x0442\x0438 \x043A\x0443\x0434\x0430 \x0441\x043C\x043E\x0442\x0440\x044E", 1, 1, 0, 1 },   // ���� ���� ������
    { L"Input", L"TwoHanded", L"\x0414\x0432\x0443\x0440\x0443\x0447\x043D\x044B\x0439 \x0445\x0432\x0430\x0442", 1, 1, 0, 1 },   // ��������� ����
    { L"Input", L"GrenadeGrab", L"\x0413\x0440\x0430\x043D\x0430\x0442\x044B \x0440\x0443\x043A\x043E\x0439 \x0441 \x043F\x043E\x044F\x0441\x0430", 1, 1, 0, 1 },   // ������� ����� � �����
    { L"Input", L"GrenadeThrowScale", L"\x0421\x0438\x043B\x0430 \x0431\x0440\x043E\x0441\x043A\x0430 \x0433\x0440\x0430\x043D\x0430\x0442\x044B", 3, 0.25f, 0.5f, 8 },   // ���� ������ �������
    { L"Input", L"GrenadeGravity", L"\x0412\x0435\x0441 \x0433\x0440\x0430\x043D\x0430\x0442\x044B", 1, 0.25f, 0, 3 },   // ��� �������
    { L"Input", L"MeleeContact", L"\x0423\x0434\x0430\x0440 \x0442\x043E\x043B\x044C\x043A\x043E \x043F\x0440\x0438 \x043A\x0430\x0441\x0430\x043D\x0438\x0438", 0, 1, 0, 1 },   // ���� ������ ��� �������
    { L"Input", L"MeleeSpeed", L"\x0423\x0434\x0430\x0440: \x0441\x043A\x043E\x0440\x043E\x0441\x0442\x044C \x0432\x0437\x043C\x0430\x0445\x0430 (\x043C/\x0441)", 3, 0.25f, 1, 8 },   // ����: �������� ������ (�/�)
    { L"Input", L"MeleeReach", L"\x0414\x043B\x0438\x043D\x0430 \x0431\x043B\x0438\x0436\x043D\x0435\x0433\x043E \x043E\x0440\x0443\x0436\x0438\x044F (\x043C)", 0.55f, 0.05f, 0.2f, 1.2f },   // ����� �������� ������ (�)
    { L"VR", L"VehicleCamera", L"\x041C\x0430\x0448\x0438\x043D\x0430: \x0432\x0438\x0434 \x043E\x0442 3-\x0433\x043E \x043B\x0438\x0446\x0430 (\x0441\x0437\x0430\x0434\x0438)", 1, 1, 0, 1 },   // ������: ������ �����
    { L"VR", L"VehicleHeadAim", L"\x041C\x0430\x0448\x0438\x043D\x0430: \x043F\x0440\x0438\x0446\x0435\x043B \x0432\x0437\x0433\x043B\x044F\x0434\x043E\x043C", 0, 1, 0, 1 },   // ������: ������ ��������
    { L"VR", L"Cutscenes3D", L"\x0417\x0430\x0441\x0442\x0430\x0432\x043A\x0438 \x0432 3D", 0, 1, 0, 1 },   // �������� � 3D
    { L"VR", L"NearClip", L"\x0411\x043B\x0438\x0436\x043D\x044F\x044F \x0433\x0440\x0430\x043D\x0438\x0446\x0430 \x0432\x0438\x0434\x0438\x043C\x043E\x0441\x0442\x0438", 10, 5, 1, 100 },   // ������� ������� ���������
    { L"VR", L"MuzzleFlashScale", L"\x0420\x0430\x0437\x043C\x0435\x0440 \x0432\x0441\x043F\x044B\x0448\x0435\x043A \x0432\x044B\x0441\x0442\x0440\x0435\x043B\x0430", 0.35f, 0.05f, 0, 1 },   // ������ ������� ��������
    { L"HUD", L"Minimal", L"\x041A\x043E\x0440\x043E\x0442\x043A\x0438\x0439 HUD", 1, 1, 0, 1 },   // �������� HUD
    { L"HUD", L"MarkersInEyes", L"\x041C\x0435\x0442\x043A\x0438 \x0432 \x0433\x043B\x0430\x0437\x0430", 1, 1, 0, 1 },   // ����� � �����
    { L"HUD", L"ButtonHints", L"\x041F\x043E\x0434\x0441\x043A\x0430\x0437\x043A\x0438 \x043A\x043D\x043E\x043F\x043A\x0430\x043C\x0438 VR", 1, 1, 0, 1 },   // ��������� �������� VR
    { L"HUD", L"Distance", L"\x041F\x0430\x043D\x0435\x043B\x044C HUD: \x0440\x0430\x0441\x0441\x0442\x043E\x044F\x043D\x0438\x0435", 1.6f, 0.1f, 0.5f, 4 },   // ������ HUD: ����������
    { nullptr, L"@exit", L"\x0421\x043E\x0445\x0440\x0430\x043D\x0438\x0442\x044C \x0438 \x0432\x044B\x0439\x0442\x0438", 0, 0, 0, 0 },   // ��������� � �����
};
static const int kSettingCount = sizeof(kSettings) / sizeof(kSettings[0]);
static const int kMenuRows = 12;
static float g_settingValue[kSettingCount];
static int g_settingSel, g_settingFirst, g_settingHover = -1;
static bool g_settingsLoaded;

static void LoadSettings()
{
    for (int i = 0; i < kSettingCount; i++) {
        float v;
        if (!kSettings[i].sec) continue;
        g_settingValue[i] = IniFloats(kSettings[i].sec, kSettings[i].key, &v, 1) == 1 ? v : kSettings[i].def;
    }
    g_settingsLoaded = true;
}

// the menu panel's layout (panel pixels): rows from kMenuTop, kMenuRowH high; the value column at the right
static const int kMenuTop = 62, kMenuRowH = 29, kMenuValueX = 470;

// Camera placement (menu item): like Space Mover - with a grip held, moving the hand drags the world, so the camera
// moves the other way (forward/back, up/down; the head's heading is "forward"). Applied live; the right trigger
// keeps it (ini), B puts it back. Robot form: FirstPersonEyeForward/Height; vehicle: the driver's seat
// (VehicleEyeForward/Height) or the chase camera (ChaseDistance/Height), whichever is used.
static struct CamAdjust {
    bool active = false;
    int kind = 0;                 // 0 robot, 1 vehicle seat, 2 chase camera
    float orig[2] = {}, grabVal[2] = {};
    XrVector3f grabHand = {};
    int grabbing = -1;
    bool trigWas = true, bWas = true;
} g_cam;

static float* CamParams(int kind, int k)   // k 0: forward, 1: up (the chase camera's "forward" is its distance back)
{
    if (kind == 0) return k ? &g_cfg.eyeHeight : &g_cfg.eyeForward;
    if (kind == 1) return k ? &g_cfg.vehicleEyeHeight : &g_cfg.vehicleEyeForward;
    return k ? &g_cfg.chaseHeight : &g_cfg.chaseDistance;
}
static const wchar_t* kCamKeys[3][2] = { { L"FirstPersonEyeForward", L"FirstPersonEyeHeight" }, { L"VehicleEyeForward", L"VehicleEyeHeight" },
                                         { L"ChaseDistance", L"ChaseHeight" } };

bool VRCameraAdjusting() { return g_cam.active && g_setup.active; }
static DWORD g_camOnlyVehicleAt;   // the item was picked in robot form: says so for a moment

static void CameraAdjustStart()
{
    if (!IKPawnIsVehicle()) { g_camOnlyVehicleAt = GetTickCount(); return; }   // (the vehicle's camera only)
    g_cam = CamAdjust();
    g_cam.active = true;
    g_cam.kind = g_cfg.vehicleCamera ? 2 : 1;
    for (int k = 0; k < 2; k++) g_cam.orig[k] = *CamParams(g_cam.kind, k);
}

static void CameraAdjustUpdate(XrTime t, const XrPosef& head)
{
    CamAdjust& c = g_cam;
    float* p[2] = { CamParams(c.kind, 0), CamParams(c.kind, 1) };
    float sq[2] = { GetFloat(xr.grip[0]), GetFloat(xr.grip[1]) };
    if (c.grabbing >= 0 && sq[c.grabbing] < 0.35f) c.grabbing = -1;
    for (int h = 0; h < 2 && c.grabbing < 0; h++) {
        XrPosef hp;
        if (sq[h] > 0.6f && LocateHand(xr.gripSpace[h], t, hp)) {
            c.grabbing = h;
            c.grabHand = hp.position;
            c.grabVal[0] = *p[0];
            c.grabVal[1] = *p[1];
            VRHaptic(h, 0.4f, 0.04f);
        }
    }
    XrPosef hp;
    if (c.grabbing >= 0 && LocateHand(xr.gripSpace[c.grabbing], t, hp)) {
        XrVector3f f = Rotate(head.orientation, { 0, 0, -1 });
        float l = sqrtf(f.x * f.x + f.z * f.z);
        f = l > 1e-3f ? XrVector3f{ f.x / l, 0, f.z / l } : XrVector3f{ 0, 0, -1 };
        XrVector3f d = { hp.position.x - c.grabHand.x, hp.position.y - c.grabHand.y, hp.position.z - c.grabHand.z };
        float fwd = d.x * f.x + d.z * f.z, up = d.y, s = g_cfg.worldScale;
        // the hand pulls the world towards you = the camera goes forward (the chase camera: closer)
        *p[0] = c.grabVal[0] + (c.kind == 2 ? fwd : -fwd) * s;
        *p[1] = c.grabVal[1] - up * s;
        g_setup.dirty = true;
    }
    bool trig = GetFloat(xr.trigger[1]) > 0.7f, b = GetBool(xr.btn[kB]);
    if (trig && !c.trigWas) {
        for (int k = 0; k < 2; k++) IniWriteFloats(L"VR", kCamKeys[c.kind][k], p[k], 1);
        Log("vr: camera placement saved: %ls %.0f, %ls %.0f", kCamKeys[c.kind][0], *p[0], kCamKeys[c.kind][1], *p[1]);
        c.active = false;
        g_settingsLoaded = false;
        VRHaptic(1, 0.8f, 0.12f);
    } else if (b && !c.bWas) {
        for (int k = 0; k < 2; k++) *p[k] = c.orig[k];
        c.active = false;
        VRHaptic(1, 0.3f, 0.05f);
    }
    c.trigWas = trig;
    c.bWas = b;
    g_setup.dirty = true;
}

static void SettingChange(int i, int dir)
{
    const SettingItem& it = kSettings[i];
    Log("vr: menu: %ls (%d)", it.key, dir);
    if (!it.sec) {
        if (!wcscmp(it.key, L"@weapon")) { g_page = 0; g_setup.rightWas = g_setup.leftWas = g_setup.aWas = g_setup.bWas = true; }
        else if (!wcscmp(it.key, L"@tpose")) { g_tposeAt = GetTickCount() + 3000; g_tposeResult = 0; }
        else if (!wcscmp(it.key, L"@camera")) CameraAdjustStart();
        else if (!wcscmp(it.key, L"@exit")) { SetupToggle(); return; }
        VRHaptic(1, 0.4f, 0.04f);
        g_setup.dirty = true;
        return;
    }
    float& v = g_settingValue[i];
    if (it.lo == 0 && it.hi == 1 && it.step == 1) v = v > 0.5f ? 0.0f : 1.0f;   // a switch: any press flips it
    else v = std::min(std::max(roundf((v + dir * it.step) / it.step) * it.step, it.lo), it.hi);
    wchar_t b[32];
    swprintf_s(b, it.step >= 1.0f ? L"%.0f" : L"%.2f", v);
    WritePrivateProfileStringW(it.sec, it.key, b, IniPath().c_str());
    VRHaptic(1, 0.2f, 0.02f);
    g_setup.dirty = true;
}

// T-pose: arms straight out to the sides, about shoulder height - the span between the controllers (palms) less the
// palms to the wrists gives the player's arm span, wrist to wrist
static void MeasureTPose(XrTime t, const XrPosef& head)
{
    XrPosef l, r;
    g_tposeResult = -1;
    if (LocateHand(xr.gripSpace[0], t, l) && LocateHand(xr.gripSpace[1], t, r)) {
        float dx = l.position.x - r.position.x, dy = l.position.y - r.position.y, dz = l.position.z - r.position.z;
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        float below0 = head.position.y - l.position.y, below1 = head.position.y - r.position.y;
        bool level = below0 > -0.15f && below0 < 0.6f && below1 > -0.15f && below1 < 0.6f && fabsf(dy) < 0.25f;
        Log("vr: T-pose: hands %.2f m apart, %.2f / %.2f m below the eyes", d, below0, below1);
        if (d > 1.0f && d < 2.6f && level) {
            g_cfg.userArmSpan = d - 0.12f;
            // eye height: the head above the floor (stage space), or from the arm span (fingertips ~ height, eyes ~ 0.93)
            float eye = 0.0f;
            XrSpaceLocation sl = { XR_TYPE_SPACE_LOCATION };
            if (xr.stageSpace && xr.viewSpace && XR_SUCCEEDED(xrLocateSpace(xr.viewSpace, xr.stageSpace, t, &sl)) &&
                (sl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
                eye = sl.pose.position.y;
            Log("vr: T-pose: eyes %.2f m above the floor (stage)", eye);
            if (eye < 1.0f || eye > 2.2f) eye = (g_cfg.userArmSpan + 0.30f) * 0.93f;
            g_cfg.userEyeHeight = eye;
            IniWriteFloats(L"VR", L"UserEyeHeight", &g_cfg.userEyeHeight, 1);
            g_cfg.autoScale = 1;
            IniWriteFloats(L"VR", L"UserArmSpan", &g_cfg.userArmSpan, 1);
            WritePrivateProfileStringW(L"VR", L"AutoScale", L"1", IniPath().c_str());
            g_settingsLoaded = false;
            g_tposeResult = 1;
        }
    }
    VRHaptic(0, g_tposeResult > 0 ? 0.9f : 0.2f, g_tposeResult > 0 ? 0.25f : 0.05f);
    VRHaptic(1, g_tposeResult > 0 ? 0.9f : 0.2f, g_tposeResult > 0 ? 0.25f : 0.05f);
}

static void UpdateSettingsPage(XrTime t, const XrPosef& head, DWORD now)
{
    if (!g_settingsLoaded) LoadSettings();
    static DWORD repeatAt, tposeShown;
    static int heldDir;
    static bool triggerWas = true, aWas = true;

    if (g_cam.active) { g_pointer.valid = false; CameraAdjustUpdate(t, head); triggerWas = true; return; }
    // the T-pose countdown (also: both triggers held at once, arms out)
    if (g_tposeAt) {
        if ((DWORD)(now / 1000) != tposeShown) { tposeShown = now / 1000; g_setup.dirty = true; }
        if ((int)(g_tposeAt - now) <= 0) { g_tposeAt = 0; MeasureTPose(t, head); g_setup.dirty = true; }
        return;
    }

    // the ray from the right hand onto the panel (its front, +Z, faces the player; 0.64 x kPanelH/1000 m)
    XrPosef aim;
    bool was = g_pointer.valid;
    g_pointer.valid = false;
    if (LocateHand(xr.aimSpace[1], t, aim)) {
        XrVector3f d = Rotate(aim.orientation, { 0, 0, -1 }), n = Rotate(g_setup.panel.orientation, { 0, 0, 1 });
        float den = d.x * n.x + d.y * n.y + d.z * n.z;
        const XrVector3f& c = g_setup.panel.position;
        if (den < -1e-3f) {
            float k = ((c.x - aim.position.x) * n.x + (c.y - aim.position.y) * n.y + (c.z - aim.position.z) * n.z) / den;
            XrVector3f hit = { aim.position.x + d.x * k, aim.position.y + d.y * k, aim.position.z + d.z * k };
            XrVector3f lp = Rotate(QuatConj(g_setup.panel.orientation), { hit.x - c.x, hit.y - c.y, hit.z - c.z });
            float u = lp.x / 0.64f + 0.5f, v = 0.5f - lp.y / (kPanelH / 1000.0f);
            if (k > 0 && k < 5 && u > -0.05f && u < 1.05f && v > -0.05f && v < 1.05f) {
                g_pointer = { true, u, v, aim.position, hit };
            }
        }
    }
    int hover = -1;
    if (g_pointer.valid) {
        int row = (int)floorf((g_pointer.v * kPanelH - kMenuTop) / kMenuRowH);
        if (row >= 0 && row < kMenuRows && g_settingFirst + row < kSettingCount) hover = g_settingFirst + row;
    }
    if (hover != g_settingHover || g_pointer.valid || was) { g_settingHover = hover; g_setup.dirty = true; }
    bool trig = GetFloat(xr.trigger[1]) > 0.7f;
    if (trig && !triggerWas && hover >= 0) {   // on a number's label: only picked; on its value: lower / raise
        g_settingSel = hover;
        const SettingItem& it = kSettings[hover];
        int x = (int)(g_pointer.u * kPanelW);
        bool sw = it.lo == 0 && it.hi == 1 && it.step == 1;
        if (!it.sec || sw || x >= kMenuValueX) SettingChange(hover, x < kMenuValueX + 70 ? -1 : 1);
        else g_setup.dirty = true;
    }
    triggerWas = trig;
    if (!g_setup.active) return;   // (left)

    // buttons: A presses the picked item; both triggers with the arms out measure the T-pose straight away
    bool a = GetBool(xr.btn[kA]);
    if (a && !aWas) SettingChange(g_settingSel, 1);
    aWas = a;
    if (!g_setup.active) return;
    if (GetFloat(xr.trigger[0]) > 0.8f && trig && hover < 0) { MeasureTPose(t, head); g_setup.dirty = true; triggerWas = true; }

    // sticks: left - pick and change (held: repeats), right - scroll
    XrVector2f ls = GetStick(xr.stick[0]), rs = GetStick(xr.stick[1]);
    int dir = ls.y > 0.6f ? 1 : ls.y < -0.6f ? 2 : ls.x > 0.6f ? 3 : ls.x < -0.6f ? 4 : rs.y > 0.6f ? 5 : rs.y < -0.6f ? 6 : 0;
    if (!dir) { heldDir = 0; return; }
    if (dir == heldDir && now < repeatAt) return;
    repeatAt = now + (dir == heldDir ? 120 : 400);
    heldDir = dir;
    if (dir == 1) g_settingSel = (g_settingSel + kSettingCount - 1) % kSettingCount;
    else if (dir == 2) g_settingSel = (g_settingSel + 1) % kSettingCount;
    else if (dir == 5) g_settingFirst = std::max(g_settingFirst - 1, 0);
    else if (dir == 6) g_settingFirst = std::min(g_settingFirst + 1, std::max(kSettingCount - kMenuRows, 0));
    else if (kSettings[g_settingSel].sec) SettingChange(g_settingSel, dir == 3 ? 1 : -1);
    if (dir <= 2) {   // the picked one stays in view
        if (g_settingSel < g_settingFirst) g_settingFirst = g_settingSel;
        if (g_settingSel >= g_settingFirst + kMenuRows) g_settingFirst = g_settingSel - kMenuRows + 1;
    }
    g_setup.dirty = true;
}

// The instructions panel.
static Chain g_panelChain;

static bool DrawPanel()
{
    if (!g_panelChain.sc && !CreateChain(g_panelChain, kPanelW, kPanelH)) return false;
    static std::vector<uint32_t> px;
    if (g_setup.dirty || px.empty()) {
        BITMAPINFO bi = {};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), (LONG)kPanelW, -(LONG)kPanelH, 1, 32, BI_RGB };
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp) { DeleteDC(dc); return false; }
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        RECT all = { 0, 0, (LONG)kPanelW, (LONG)kPanelH };
        HBRUSH bg = CreateSolidBrush(RGB(10, 22, 28));
        FillRect(dc, &all, bg);
        DeleteObject(bg);
        HBRUSH edge = CreateSolidBrush(RGB(64, 186, 222));
        FrameRect(dc, &all, edge);
        DeleteObject(edge);
        HFONT title = CreateFontW(-30, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Bahnschrift SemiBold Condensed");
        HFONT body = CreateFontW(-22, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Bahnschrift Condensed");
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ oldFont = SelectObject(dc, title);
        wchar_t line[200];
        swprintf_s(line, L"\x041D\x0410\x0421\x0422\x0420\x041E\x0419\x041A\x0410: %ls", g_ws.name.empty() ? L"-" : g_ws.name.c_str());   // НАСТРОЙКА
        CharUpperW(line);
        SetTextColor(dc, RGB(255, 147, 51));
        RECT r = { 18, 12, (LONG)kPanelW - 18, 50 };
        DrawTextW(dc, line, -1, &r, DT_LEFT | DT_SINGLELINE);
        SelectObject(dc, body);
        SetTextColor(dc, RGB(150, 222, 245));
        const wchar_t* help[] = {
            L"\x041F\x0440\x0430\x0432\x044B\x0439 \x0433\x0440\x0438\x043F: \x0434\x0435\x0440\x0436\x0438\x0442\x0435 \x2014 \x043F\x0443\x0448\x043A\x0430 \x0441\x0442\x043E\x0438\x0442, \x043F\x0435\x0440\x0435\x0434\x0432\x0438\x043D\x044C\x0442\x0435 \x0440\x0443\x043A\x0443, \x043E\x0442\x043F\x0443\x0441\x0442\x0438\x0442\x0435",
            L"\x041B\x0435\x0432\x044B\x0439 \x0433\x0440\x0438\x043F \x0443 \x0437\x0435\x043B\x0451\x043D\x043E\x0433\x043E \x043A\x0443\x0431\x0430 \x2014 \x043F\x0435\x0440\x0435\x043D\x0435\x0441\x0442\x0438 \x0437\x043E\x043D\x0443 (\x0446\x0435\x0432\x044C\x0451, \x043F\x043E\x0434\x0441\x0443\x043C\x043E\x043A)",
            L"\x041F\x0440\x0430\x0432\x044B\x0439 \x0433\x0440\x0438\x043F: \x043E\x0440\x0430\x043D\x0436\x0435\x0432\x044B\x0439 \x043A\x0432\x0430\x0434\x0440\x0430\x0442 \x2014 \x0433\x0434\x0435 \x0432\x0430\x0448\x0430 \x0440\x0443\x043A\x0430 \x0441\x0435\x0439\x0447\x0430\x0441",
            L"\x041F\x0443\x0448\x043A\x0430 \x0432 \x043B\x0435\x0432\x043E\x0439 \x0440\x0443\x043A\x0435: \x0440\x0443\x043A\x0438 \x043C\x0435\x043D\x044F\x044E\x0442\x0441\x044F \x0440\x043E\x043B\x044F\x043C\x0438, \x0445\x0432\x0430\x0442 \x043B\x0435\x0432\x043E\x0439 \x2014 \x0441\x0432\x043E\x0439 \x0434\x043B\x044F \x043A\x0430\x0436\x0434\x043E\x0433\x043E \x043E\x0440\x0443\x0436\x0438\x044F",
            L"B \x2014 \x043F\x043E\x0432\x043E\x0440\x043E\x0442 \x0442\x043E\x0436\x0435, A \x2014 \x0441\x0431\x0440\x043E\x0441 \x043E\x0440\x0443\x0436\x0438\x044F, \x041C\x0435\x043D\x044E \x2014 \x0441\x043E\x0445\x0440\x0430\x043D\x0438\x0442\x044C \x0438 \x0432\x044B\x0439\x0442\x0438",
        };
        int y = 62;
        if (g_page == 1) {
            // НАСТРОЙКИ VR (Y — оружие)
            SelectObject(dc, title);
            SetTextColor(dc, RGB(255, 147, 51));
            RECT rt = { 18, 12, (LONG)kPanelW - 18, 50 };
            HBRUSH bg2 = CreateSolidBrush(RGB(10, 22, 28));
            FillRect(dc, &rt, bg2);
            DeleteObject(bg2);
            DrawTextW(dc, L"\x041D\x0410\x0421\x0422\x0420\x041E\x0419\x041A\x0418 VR", -1, &rt, DT_LEFT | DT_SINGLELINE);
            SelectObject(dc, body);
            if (!g_settingsLoaded) LoadSettings();
            y = kMenuTop;
            for (int i = g_settingFirst; i < g_settingFirst + kMenuRows && i < kSettingCount; i++) {
                const SettingItem& it = kSettings[i];
                RECT rr = { 12, y - 2, (LONG)kPanelW - 12, y + kMenuRowH - 3 };
                bool sel = i == g_settingSel, hot = i == g_settingHover;
                if (sel || hot) {
                    HBRUSH hb = CreateSolidBrush(sel ? RGB(255, 147, 51) : RGB(30, 70, 86));
                    FillRect(dc, &rr, hb);
                    DeleteObject(hb);
                }
                SetTextColor(dc, sel ? RGB(26, 16, 6) : !it.sec ? RGB(255, 214, 150) : RGB(150, 222, 245));
                RECT rl = { 22, y, kMenuValueX, y + 26 };
                DrawTextW(dc, it.label, -1, &rl, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                wchar_t v[32] = L"\x25B8";   // an action
                if (it.sec && it.lo == 0 && it.hi == 1 && it.step == 1) wcscpy_s(v, g_settingValue[i] > 0.5f ? L"\x0412\x041A\x041B" : L"\x0412\x042B\x041A\x041B");
                else if (it.sec) swprintf_s(v, it.step >= 1.0f ? L"\x25C2  %.0f  \x25B8" : L"\x25C2  %.2f  \x25B8", g_settingValue[i]);
                RECT rv2 = { kMenuValueX, y, (LONG)kPanelW - 22, y + 26 };
                DrawTextW(dc, v, -1, &rv2, DT_CENTER | DT_SINGLELINE);
                y += kMenuRowH;
            }
            if (kSettingCount > kMenuRows) {   // scroll bar
                int h = kMenuRows * kMenuRowH, top = kMenuTop + h * g_settingFirst / kSettingCount, len = h * kMenuRows / kSettingCount;
                RECT sb = { (LONG)kPanelW - 8, top, (LONG)kPanelW - 4, top + len };
                HBRUSH hb = CreateSolidBrush(RGB(64, 186, 222));
                FillRect(dc, &sb, hb);
                DeleteObject(hb);
            }
            // the T-pose, its result, how to use the menu
            y = kMenuTop + kMenuRows * kMenuRowH + 10;
            RECT rs = { 18, y, (LONG)kPanelW - 18, y + 28 };
            if (g_cam.active) {
                SetTextColor(dc, RGB(255, 147, 51));
                static const wchar_t* who[3] = { L"\x0440\x043E\x0431\x043E\x0442\x0430", L"\x043C\x0430\x0448\x0438\x043D\x044B (\x043A\x0430\x0431\x0438\x043D\x0430)",
                                                 L"\x043C\x0430\x0448\x0438\x043D\x044B (\x0441\x0437\x0430\x0434\x0438)" };
                swprintf_s(line, L"\x041A\x0430\x043C\x0435\x0440\x0430 %ls: \x0432\x043F\x0435\x0440\x0451\x0434 %.0f, \x0432\x044B\x0441\x043E\x0442\x0430 %.0f",
                           who[g_cam.kind], *CamParams(g_cam.kind, 0), *CamParams(g_cam.kind, 1));
                DrawTextW(dc, line, -1, &rs, DT_LEFT | DT_SINGLELINE);
                wcscpy_s(line, L"\x0413\x0440\x0438\x043F + \x0434\x0432\x0438\x0436\x0435\x043D\x0438\x0435 \x0440\x0443\x043A\x0438 \x2014 \x0434\x0432\x0438\x0433\x0430\x0442\x044C \x043C\x0438\x0440; \x043A\x0443\x0440\x043E\x043A \x2014 \x0441\x043E\x0445\x0440\x0430\x043D\x0438\x0442\x044C, B \x2014 \x043E\x0442\x043C\x0435\x043D\x0430");
                rs.top += 32; rs.bottom += 32;
            } else if (GetTickCount() - g_camOnlyVehicleAt < 3000) {
                SetTextColor(dc, RGB(255, 120, 100));
                wcscpy_s(line, L"\x041F\x043E\x043B\x043E\x0436\x0435\x043D\x0438\x0435 \x043A\x0430\x043C\x0435\x0440\x044B \x2014 \x0442\x043E\x043B\x044C\x043A\x043E \x0432 \x043C\x0430\x0448\x0438\x043D\x0435 (\x043F\x0440\x0435\x0432\x0440\x0430\x0442\x0438\x0442\x0435\x0441\x044C)");
                g_setup.dirty = true;
            } else if (g_tposeAt) {
                SetTextColor(dc, RGB(255, 147, 51));
                swprintf_s(line, L"\x0422-\x043F\x043E\x0437\x0430: \x0432\x0441\x0442\x0430\x043D\x044C\x0442\x0435 \x043F\x0440\x044F\x043C\x043E, \x0440\x0443\x043A\x0438 \x0432 \x0441\x0442\x043E\x0440\x043E\x043D\x044B (%d)",
                           (int)((g_tposeAt - GetTickCount()) / 1000) + 1);
            } else if (g_tposeResult < 0) {
                SetTextColor(dc, RGB(255, 120, 100));
                wcscpy_s(line, L"\x0422-\x043F\x043E\x0437\x0430 \x043D\x0435 \x0440\x0430\x0441\x043F\x043E\x0437\x043D\x0430\x043D\x0430 \x2014 \x0440\x0443\x043A\x0438 \x0448\x0438\x0440\x0435 \x0438 \x043D\x0430 \x0443\x0440\x043E\x0432\x043D\x0435 \x043F\x043B\x0435\x0447");
            } else {
                SetTextColor(dc, RGB(255, 214, 150));
                swprintf_s(line, L"\x0420\x043E\x0441\x0442: %.2f \x043C (\x0433\x043B\x0430\x0437\x0430)   \x0420\x043E\x0431\x043E\x0442: %.0f   \x041C\x0430\x0441\x0448\x0442\x0430\x0431: %.0f%ls",
                           g_cfg.userEyeHeight, IKRobotEyeHeight(), g_cfg.worldScale, g_cfg.autoScale && g_cfg.userEyeHeight > 0.8f ? L" (\x0430\x0432\x0442\x043E)" : L"");
            }
            DrawTextW(dc, line, -1, &rs, DT_LEFT | DT_SINGLELINE);
            SetTextColor(dc, RGB(110, 170, 190));
            RECT rh = { 18, y + 32, (LONG)kPanelW - 18, y + 60 };
            if (!g_cam.active) DrawTextW(dc,L"\x041B\x0443\x0447 \x043F\x0440\x0430\x0432\x043E\x0439 \x0440\x0443\x043A\x0438 + \x043A\x0443\x0440\x043E\x043A, \x0438\x043B\x0438 \x0441\x0442\x0438\x043A \x0438 A; \x043F\x0440\x0430\x0432\x044B\x0439 \x0441\x0442\x0438\x043A \x2014 \x043F\x0440\x043E\x043A\x0440\x0443\x0442\x043A\x0430",
                      -1, &rh, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (g_pointer.valid) {   // the ray's dot
                int cx = (int)(g_pointer.u * kPanelW), cy = (int)(g_pointer.v * kPanelH);
                HBRUSH hb = CreateSolidBrush(RGB(255, 255, 255));
                HGDIOBJ ob = SelectObject(dc, hb);
                HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
                Ellipse(dc, cx - 6, cy - 6, cx + 7, cy + 7);
                SelectObject(dc, op);
                SelectObject(dc, ob);
                DeleteObject(hb);
            }
        } else {
        for (const wchar_t* h : help) { RECT rr = { 18, y, (LONG)kPanelW - 18, y + 30 }; DrawTextW(dc, h, -1, &rr, DT_LEFT | DT_SINGLELINE); y += 32; }
        // Поворот: вкл/выкл
        swprintf_s(line, L"\x041F\x043E\x0432\x043E\x0440\x043E\x0442: %ls", g_setup.rotate ? L"\x0412\x041A\x041B" : L"\x0412\x042B\x041A\x041B");
        SetTextColor(dc, RGB(255, 214, 150));
        RECT rv = { 18, y + 14, (LONG)kPanelW - 18, y + 48 };
        DrawTextW(dc, line, -1, &rv, DT_LEFT | DT_SINGLELINE);
        {   // Y — меню
            SetTextColor(dc, RGB(150, 222, 245));
            RECT ry = { 18, y + 54, (LONG)kPanelW - 18, y + 84 };
            DrawTextW(dc, L"Y \x2014 \x043C\x0435\x043D\x044E", -1, &ry, DT_LEFT | DT_SINGLELINE);
        }
        }
        GdiFlush();
        const uint32_t* src = (const uint32_t*)bits;
        bool rgba = xr.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || xr.format == DXGI_FORMAT_R8G8B8A8_UNORM;
        px.assign(kPanelW * kPanelH, 0);
        const float a = 0.9f;
        for (UINT i = 0; i < kPanelW * kPanelH; i++) {
            uint32_t v = src[i], b = (uint32_t)((v & 0xFF) * a), g = (uint32_t)(((v >> 8) & 0xFF) * a), r2 = (uint32_t)(((v >> 16) & 0xFF) * a);
            px[i] = (rgba ? (r2 | (g << 8) | (b << 16)) : (b | (g << 8) | (r2 << 16))) | ((uint32_t)(255 * a) << 24);
        }
        SelectObject(dc, oldFont); DeleteObject(title); DeleteObject(body);
        SelectObject(dc, oldBmp); DeleteObject(bmp); DeleteDC(dc);
        g_setup.dirty = false;
    }
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrAcquireSwapchainImage(g_panelChain.sc, &ai, &idx))) return false;
    xrWaitSwapchainImage(g_panelChain.sc, &wi);
    ID3D11DeviceContext* imm = nullptr;
    xr.device->GetImmediateContext(&imm);
    imm->UpdateSubresource(g_panelChain.images[idx].texture, 0, nullptr, px.data(), kPanelW * 4, 0);
    imm->Release();
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(g_panelChain.sc, &ri);
    return true;
}

// The setup's marks: squares facing the head (compositor quads) - green for the zones, orange for the real right
// hand while the gun is held still (right grip).
static Chain g_markChain[2];

static bool MakeSquareChain(Chain& c, float r, float g, float b)
{
    const UINT S = 32;
    if (!CreateChain(c, S, S)) return false;
    std::vector<uint32_t> px(S * S);
    bool rgba = xr.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || xr.format == DXGI_FORMAT_R8G8B8A8_UNORM;
    for (UINT y = 0; y < S; y++)
        for (UINT x = 0; x < S; x++) {
            bool edge = x < 3 || y < 3 || x >= S - 3 || y >= S - 3;
            float a = edge ? 0.95f : 0.45f;   // premultiplied
            uint32_t R = (uint32_t)(255 * r * a), G = (uint32_t)(255 * g * a), B = (uint32_t)(255 * b * a);
            px[y * S + x] = (rgba ? (R | (G << 8) | (B << 16)) : (B | (G << 8) | (R << 16))) | ((uint32_t)(255 * a) << 24);
        }
    ID3D11DeviceContext* imm = nullptr;
    xr.device->GetImmediateContext(&imm);
    for (auto& img : c.images) imm->UpdateSubresource(img.texture, 0, nullptr, px.data(), S * 4, 0);
    imm->Release();
    for (size_t k = 0; k < c.images.size(); k++) {   // (each image released once: content to show)
        uint32_t idx;
        XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        wi.timeout = XR_INFINITE_DURATION;
        XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(c.sc, &ai, &idx))) { xrWaitSwapchainImage(c.sc, &wi); xrReleaseSwapchainImage(c.sc, &ri); }
    }
    return true;
}

// The menu's ray: a thin orange strip from the right hand to the point on the panel, turned to face the eyes.
static bool BuildRay(const XrPosef& head, XrCompositionLayerQuad& q)
{
    if (!g_setup.active || g_page != 1 || !g_pointer.valid) return false;
    Chain& c = g_markChain[1];
    if (!c.sc && !MakeSquareChain(c, 1.0f, 0.58f, 0.2f)) return false;
    const XrVector3f& a = g_pointer.from;
    const XrVector3f& b = g_pointer.to;
    XrVector3f d = { b.x - a.x, b.y - a.y, b.z - a.z };
    float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len < 0.02f) return false;
    XrVector3f up = { d.x / len, d.y / len, d.z / len }, mid = { (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f };
    XrVector3f v = { head.position.x - mid.x, head.position.y - mid.y, head.position.z - mid.z };
    float k = v.x * up.x + v.y * up.y + v.z * up.z;
    XrVector3f back = { v.x - up.x * k, v.y - up.y * k, v.z - up.z * k };
    float bl = sqrtf(back.x * back.x + back.y * back.y + back.z * back.z);
    if (bl < 1e-4f) return false;
    back = { back.x / bl, back.y / bl, back.z / bl };
    XrVector3f right = { up.y * back.z - up.z * back.y, up.z * back.x - up.x * back.z, up.x * back.y - up.y * back.x };   // up x back
    q = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = xr.localSpace;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = c.sc;
    q.subImage.imageRect = { { 8, 8 }, { 16, 16 } };   // (the square's solid middle)
    q.pose.orientation = BasisToQuat(right, up, back);
    q.pose.position = mid;
    q.size = { 0.005f, len };
    return true;
}

// i: 0 = the gun's front zone, 1 = the pouch, 2 = the real right hand (the weapon page only)
static bool BuildZoneMark(int i, XrTime t, const XrPosef& head, XrCompositionLayerQuad& q)
{
    if (!g_setup.active || g_page != 0) return false;
    XrVector3f at;
    if (i < 2) at = g_setup.cube[i];
    else {
        XrPosef gp;
        if (!g_calib.active || !LocateHand(xr.gripSpace[GunIdx()], t, gp)) return false;
        at = gp.position;
    }
    Chain& c = g_markChain[i == 2];
    if (!c.sc && !(i == 2 ? MakeSquareChain(c, 1.0f, 0.58f, 0.2f) : MakeSquareChain(c, 0.16f, 1.0f, 0.31f))) return false;
    q = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = xr.localSpace;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = c.sc;
    q.subImage.imageRect = { { 0, 0 }, { 32, 32 } };
    q.pose.orientation = head.orientation;
    q.pose.position = at;
    float s = i == 2 ? 0.05f : g_setup.grab == i ? 0.1f : 0.08f;
    q.size = { s, s };
    return true;
}

static bool BuildPanel(XrCompositionLayerQuad& q)
{
    if (!g_setup.active || !DrawPanel()) return false;
    q = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = xr.localSpace;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = g_panelChain.sc;
    q.subImage.imageRect = { { 0, 0 }, { (int32_t)kPanelW, (int32_t)kPanelH } };
    q.pose = g_setup.panel;
    q.size = { kPanelW / 1000.0f, kPanelH / 1000.0f };
    return true;
}

// Green cubes for the zones, drawn into each eye's image (projected on the CPU, one coloured triangle list).
static const char* kCubeHLSL = R"(
void VS(float4 p : POSITION, float4 c : COLOR, out float4 op : SV_Position, out float4 oc : COLOR) { op = p; oc = c; }
float4 PS(float4 p : SV_Position, float4 c : COLOR) : SV_Target { return c; }
)";

static struct CubeDrawer {
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* vb = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    bool tried = false;

    bool Init(ID3D11Device* dev)
    {
        if (tried) return vs != nullptr;
        tried = true;
        HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
        ID3DBlob *vsb = nullptr, *psb = nullptr;
        if (!compile || FAILED(compile(kCubeHLSL, strlen(kCubeHLSL), "cube", nullptr, nullptr, "VS", "vs_4_0", 0, 0, &vsb, nullptr)) ||
            FAILED(compile(kCubeHLSL, strlen(kCubeHLSL), "cube", nullptr, nullptr, "PS", "ps_4_0", 0, 0, &psb, nullptr))) {
            SafeRelease(vsb);
            Log("vr: cube shader failed");
            return false;
        }
        dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
        dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
        D3D11_INPUT_ELEMENT_DESC el[] = { { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                                          { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
        dev->CreateInputLayout(el, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout);
        vsb->Release(); psb->Release();
        D3D11_BUFFER_DESC bd = { 32 * 36 * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
        dev->CreateBuffer(&bd, nullptr, &vb);
        D3D11_BLEND_DESC bl = {};
        bl.RenderTarget[0] = { TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_BLEND_ZERO, D3D11_BLEND_ONE,
                               D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL };
        dev->CreateBlendState(&bl, &blend);
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE;
        dev->CreateRasterizerState(&rd, &raster);
        return vs && ps && layout && vb && blend && raster;
    }
} g_cubes;

// called by CopyToChain for an eye image while the setup is on
static void DrawSetupCubes(ID3D11DeviceContext* imm, ID3D11RenderTargetView* rtv, UINT w, UINT h, const XrPosef& eye, const XrFovf& fov)
{
    if (!g_setup.active || !g_cubes.Init(xr.device)) return;
    struct V { float p[4], c[4]; };
    static V verts[4 * 36];
    int n = 0;
    float tl = tanf(fov.angleLeft), tr = tanf(fov.angleRight), tu = tanf(fov.angleUp), td = tanf(fov.angleDown);
    XrQuaternionf inv = QuatConj(eye.orientation);
    static const int faces[6][4] = { { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 5, 7, 3 } };
    for (int i = 0; i < 2; i++) {
        float hs = (g_setup.grab == i ? 0.05f : 0.04f);
        float col[4] = { 0.15f, 1.0f, 0.3f, g_setup.grab == i ? 0.65f : 0.4f };
        float clip[8][4];
        bool ok = true;
        for (int k = 0; k < 8 && ok; k++) {
            XrVector3f c = g_setup.cube[i];
            XrVector3f v = { c.x + (k & 1 ? hs : -hs), c.y + (k & 2 ? hs : -hs), c.z + (k & 4 ? hs : -hs) };
            XrVector3f e = Rotate(inv, { v.x - eye.position.x, v.y - eye.position.y, v.z - eye.position.z });
            if (e.z > -0.02f) { ok = false; break; }   // behind the eye
            float x = e.x / -e.z, y = e.y / -e.z, z = -e.z;
            clip[k][0] = (2 * x - (tr + tl)) / (tr - tl) * z;
            clip[k][1] = (2 * y - (tu + td)) / (tu - td) * z;
            clip[k][2] = 0.5f * z;
            clip[k][3] = z;
        }
        if (!ok) continue;
        for (auto& f : faces) {
            const int tri[6] = { f[0], f[1], f[2], f[0], f[2], f[3] };
            for (int k : tri) { memcpy(verts[n].p, clip[k], sizeof(clip[k])); memcpy(verts[n].c, col, sizeof(col)); n++; }
        }
    }
    if (!n) return;
    ID3D11DeviceContext* dc = nullptr;
    if (FAILED(xr.device->CreateDeferredContext(0, &dc))) return;
    dc->UpdateSubresource(g_cubes.vb, 0, nullptr, verts, 0, 0);
    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    dc->RSSetViewports(1, &vp);
    dc->RSSetState(g_cubes.raster);
    dc->OMSetRenderTargets(1, &rtv, nullptr);
    dc->OMSetBlendState(g_cubes.blend, nullptr, 0xFFFFFFFF);
    UINT stride = sizeof(V), offset = 0;
    dc->IASetInputLayout(g_cubes.layout);
    dc->IASetVertexBuffers(0, 1, &g_cubes.vb, &stride, &offset);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dc->VSSetShader(g_cubes.vs, nullptr, 0);
    dc->PSSetShader(g_cubes.ps, nullptr, 0);
    dc->Draw(n, 0);
    ID3D11CommandList* cl = nullptr;
    if (SUCCEEDED(dc->FinishCommandList(FALSE, &cl))) { imm->ExecuteCommandList(cl, TRUE); cl->Release(); }
    dc->Release();
}

static XrFrameState g_xrFrameState = { XR_TYPE_FRAME_STATE };
static bool g_xrFrameBegun;
static bool g_haveLastProj, g_haveLastTheater, g_wasStereo;
static XrCompositionLayerProjectionView g_lastProjViews[2];
static XrCompositionLayerQuad g_lastTheaterQuad;

static void ResetFrameLifecycle()
{
    g_xrFrameBegun = false;
    g_haveLastProj = g_haveLastTheater = false;
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
                ResetFrameLifecycle();
                std::lock_guard<std::mutex> lock(g_padMutex);
                g_pad.active = false;
            } else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
                xr.running = false;
                xr.failed = true;   // keep the game running flat
                ResetFrameLifecycle();
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            xr.running = false;
            xr.failed = true;
            ResetFrameLifecycle();
        }
        ev = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

// ---------------------------------------------------------------------------------------------
// Stereo rendering. In StereoPerFrame=1 (default), each game tick locates both eyes once at the
// current OpenXR frame's predictedDisplayTime and renders Left + Right from the identical world state;
// in StereoPerFrame=0 (AFR fallback), the game thread alternates one eye per tick.
#include <deque>

struct EyeFrame {
    int eye;
    XrPosef pose;
    XrFovf fov;
    XrTime time;   // display time the pose (and the hands the game aimed with) was predicted for
};
static std::mutex g_stereoMutex;
static std::deque<EyeFrame> g_pending;          // render thread: eye frames in the order their images will come
static EyeFrame g_tickEyes[2];                  // single-thread rendering: eye(s) located in this game tick
static int g_tickEyeCount, g_tickEyeNext;
static long g_tickEyeFrame = -1;                // ... and that tick (GameFrameCount), -1 once consumed
static std::atomic<float> g_aspect{ 16.0f / 9.0f };
static int g_nextEye;
static EyeFrame g_eyeDone[2];
static bool g_eyeHave[2];

bool VRIsRunning() { return xr.running; }
bool VRIsRunningOrFake() { return xr.running || g_cfg.fakeHmd; }

// Debug: synthetic head pose (no headset needed) to verify the camera path. Yaw/pitch come from the
// debug script ("fakehead <yawDeg> <pitchDeg>").
float g_fakeHeadYaw, g_fakeHeadPitch, g_fakeHandYaw, g_fakeHandPitch;

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
    // Fake right hand: aims g_fakeHandYaw degrees right of the head, held low and to the right.
    out.aim[0] = { true, -0.2f, -0.3f, -0.35f, 0.0f, 0.0f, 0.0f, 1.0f };   // left hand: low, left, forward
    float ay = -g_fakeHandYaw * 0.0174533f * 0.5f, ap = g_fakeHandPitch * 0.0174533f * 0.5f;
    float hcy = cosf(ay), hsy = sinf(ay), hcp = cosf(ap), hsp = sinf(ap);
    out.aim[1] = { true, 0.2f, -0.3f, -0.3f, hcy * hsp, hsy * hcp, -hsy * hsp, hcy * hcp };
    // grips: a little behind the aim origins
    out.grip[0] = out.aim[0]; out.grip[0].pz += 0.08f;
    out.grip[1] = out.aim[1]; out.grip[1].pz += 0.08f;
    return true;
}

static volatile DWORD g_lastStereoTick;
volatile long g_vrViewFailures;   // game frames that wanted an eye view but got none (logged by game.cpp)

// The eye the next stereo game frame will render, if stereo views are being produced right now.
bool VRNextStereoEye(int& eye)
{
    eye = g_cfg.stereoPerFrame ? 0 : g_nextEye;   // both eyes per frame: every frame is a whole pair
    return GetTickCount() - g_lastStereoTick < 250;
}

// Locates both eyes for the coming frame(s); fills `out` for each requested eye (in order) and queues the eye
// frames for the present thread. Hands are located once and shared.
static bool LocateEyes(const int* eyes, int count, VREyeView* out)
{
    if (g_cfg.fakeHmd) {
        g_lastStereoTick = GetTickCount();
        for (int i = 0; i < count; i++) { g_nextEye = eyes[i]; FakeHeadView(out[i]); }
        g_nextEye = eyes[count - 1] ^ 1;
        return true;
    }
    if (g_cfg.vrMode != 1 || !xr.running || !g_lastDisplayTime.load()) return false;
    XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO };
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = g_lastDisplayTime.load();
    li.space = xr.localSpace;
    XrViewState vs = { XR_TYPE_VIEW_STATE };
    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    uint32_t n = 0;
    if (XR_FAILED(xrLocateViews(xr.session, &li, &vs, 2, &n, views)) || n != 2 ||
        !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        g_vrViewFailures++;
        return false;
    }

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

    g_nextEye = eyes[count - 1] ^ 1;
    g_lastStereoTick = GetTickCount();
    VREyeView::Hand aim[2], grip[2];
    for (int h = 0; h < 2; h++) {
        XrPosef p;
        aim[h].valid = LocateHand(xr.aimSpace[h], li.displayTime, p);
        if (aim[h].valid) aim[h] = { true, p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w };
        grip[h].valid = LocateHand(xr.gripSpace[h], li.displayTime, p);
        if (grip[h].valid) grip[h] = { true, p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w };
    }
    // the gun hand holds the gun as set with GunGrip (or frozen while adjusting it)
    XrPosef ra;
    XrVector3f rg;
    int gh = GunIdx();
    if (aim[gh].valid && LocateRightAim(li.displayTime, ra, &rg)) {
        aim[gh] = { true, ra.position.x, ra.position.y, ra.position.z, ra.orientation.x, ra.orientation.y, ra.orientation.z, ra.orientation.w };
        grip[gh].px = rg.x; grip[gh].py = rg.y; grip[gh].pz = rg.z;
    }
    bool singleThread = GameSingleThreadRender();
    if (singleThread) {
        std::lock_guard<std::mutex> lock(g_stereoMutex);
        g_tickEyeCount = 0;
        g_tickEyeNext = 0;
        g_tickEyeFrame = GameFrameCount();
    }
    for (int i = 0; i < count; i++) {
        int eye = eyes[i];
        EyeFrame f;
        f.eye = eye;
        f.pose = views[eye].pose;
        f.time = li.displayTime;
        f.fov.angleLeft = -half;
        f.fov.angleRight = half;
        f.fov.angleUp = atanf(tanH / aspect);
        f.fov.angleDown = -f.fov.angleUp;
        if (singleThread) {
            // drawn and presented later in this same game tick (left eye on pass 1, right eye on pass 2)
            std::lock_guard<std::mutex> lock(g_stereoMutex);
            if (g_tickEyeCount < 2) g_tickEyes[g_tickEyeCount++] = f;
        } else {
            // Render thread: presents come a frame or two later, matched by order only. (Queueing only drawn frames,
            // trimming the queue or clearing it when the 3D view starts all broke the eye pairs on the real runtime.)
            std::lock_guard<std::mutex> lock(g_stereoMutex);
            g_pending.push_back(f);
            while (g_pending.size() > 4) g_pending.pop_front();   // presents fell behind (loading etc.)
        }
        if (g_cfg.logCameraSeq > 0) Log("seq: eye %d located (game frame %ld)", eye, GameFrameCount());
        VREyeView& o = out[i];
        o.eye = eye;
        o.px = f.pose.position.x; o.py = f.pose.position.y; o.pz = f.pose.position.z;
        o.qx = f.pose.orientation.x; o.qy = f.pose.orientation.y; o.qz = f.pose.orientation.z; o.qw = f.pose.orientation.w;
        o.renderFovDeg = half * 2.0f * 57.2957795f;
        for (int h = 0; h < 2; h++) { o.aim[h] = aim[h]; o.grip[h] = grip[h]; }
    }
    return true;
}

// Alternate-eye mode: one eye per game frame.
bool VRBeginGameFrame(VREyeView& out)
{
    int eye = g_nextEye;
    return LocateEyes(&eye, 1, &out);
}

// Both eyes from one game frame (the frame is drawn twice): left, then right.
bool VRBeginStereoPair(VREyeView& left, VREyeView& right)
{
    static const int eyes[2] = { 0, 1 };
    VREyeView v[2];
    if (!LocateEyes(eyes, 2, v)) return false;
    left = v[0];
    right = v[1];
    return true;
}

static void ResetStereo()
{
    std::lock_guard<std::mutex> lock(g_stereoMutex);
    g_pending.clear();
    g_tickEyeCount = g_tickEyeNext = 0;
    g_tickEyeFrame = -1;
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

static bool ConsumeOrWaitFrame(XrFrameState& fs)
{
    DWORD tid = GameThreadId();
    if (g_xrFrameBegun && (!tid || GetCurrentThreadId() == tid)) {
        fs = g_xrFrameState;
        g_xrFrameBegun = false;
        return true;
    }
    return WaitAndBegin(fs);
}

// Overlays (null entries skipped) are drawn over the base layer in order.
static void EndFrame(const XrFrameState& fs, const XrCompositionLayerBaseHeader* layer,
                     std::initializer_list<const XrCompositionLayerBaseHeader*> overlays = {})
{
    const XrCompositionLayerBaseHeader* layers[12];   // (OpenXR runtimes take at least 16)
    uint32_t n = 0;
    if (layer) layers[n++] = layer;
    for (auto* o : overlays) {
        if (!layer || !o || n >= 12) continue;
        if (o->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {   // a bad pose makes the runtime refuse the whole frame (black)
            XrPosef& p = ((XrCompositionLayerQuad*)o)->pose;
            XrQuaternionf& q = p.orientation;
            float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            if (!_finite(l) || l < 0.5f || l > 1.5f || !_finite(p.position.x + p.position.y + p.position.z)) {
                static DWORD logged;
                if (GetTickCount() - logged > 2000) { logged = GetTickCount(); Log("vr: overlay %u dropped: bad pose", n); }
                continue;
            }
            q = { q.x / l, q.y / l, q.z / l, q.w / l };
        }
        layers[n++] = o;
    }
    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = n;
    ei.layers = n ? layers : nullptr;
    XrResult r = xrEndFrame(xr.session, &ei);
    static DWORD logged;
    if (XR_FAILED(r) && GetTickCount() - logged > 2000) { logged = GetTickCount(); Log("vr: xrEndFrame failed %d (%u layers)", (int)r, n); }
}

// Closes an OpenXR frame begun at the start of a tick when that tick produced no fresh submission (loading / pause transition),
// repeating the most recently released swapchain image so the headset never flashes black.
static void FinishUnsubmittedFrame(const XrFrameState& fs)
{
    if (!fs.shouldRender) { EndFrame(fs, nullptr); return; }
    if (g_wasStereo && g_haveLastProj && g_eyes[0].sc && g_eyes[1].sc) {
        XrCompositionLayerProjection proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        proj.space = xr.localSpace;
        proj.viewCount = 2;
        proj.views = g_lastProjViews;
        EndFrame(fs, (XrCompositionLayerBaseHeader*)&proj);
    } else if (g_haveLastTheater && g_theater.sc) {
        EndFrame(fs, (XrCompositionLayerBaseHeader*)&g_lastTheaterQuad);
    } else {
        EndFrame(fs, nullptr);
    }
}

// Standard OpenXR frame lifecycle: wait + begin + sync input at the very start of the engine tick (before UE3
// measures GDeltaTime), so the HMD paces the tick, input has zero tick lag, and xrLocateViews + both eye renders
// use THIS frame's predictedDisplayTime rather than the previous frame's.
void VRBeginTick()
{
    if (!g_cfg.vrEnabled || !g_cfg.stereoPerFrame || !GameSingleThreadRender()) return;
    DWORD tid = GameThreadId();
    if (!tid || GetCurrentThreadId() != tid) return;
    if (xr.failed || !xr.session) return;
    if (g_xrFrameBegun) {
        if (xr.running) FinishUnsubmittedFrame(g_xrFrameState);
        g_xrFrameBegun = false;
    }
    PollEvents();
    if (!xr.running) { ResetStereo(); return; }
    if (WaitAndBegin(g_xrFrameState)) {
        g_xrFrameBegun = true;
        UpdateInput();
    }
}

// ---------------------------------------------------------------------------------------------
// Floating HUD: the game's HUD (split off by d3dtrace.cpp) on a panel in front of the player that follows
// the head with some lag, like a helmet display that settles where you look.
static ID3D11ShaderResourceView* g_frameHud;   // HUD texture of the frame being presented (or null)
static Chain g_hudChain;
static bool g_hudHasImage;
static float g_hudYaw, g_hudPitch;             // smoothed panel direction (radians, OpenXR LOCAL)
static bool g_hudPoseInit;
static LARGE_INTEGER g_hudLastT;

static bool CopyHud(ID3D11ShaderResourceView* srv)
{
    ID3D11Resource* r = nullptr;
    srv->GetResource(&r);
    UINT w = 0, h = 0;
    ID3D11Texture2D* t = nullptr;
    if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) {
        D3D11_TEXTURE2D_DESC d;
        t->GetDesc(&d);
        w = d.Width;
        h = d.Height;
        t->Release();
    }
    SafeRelease(r);
    if (!w) return false;
    if (!g_hudChain.sc || g_hudChain.width != w || g_hudChain.height != h)
        if (!CreateChain(g_hudChain, w, h)) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrAcquireSwapchainImage(g_hudChain.sc, &ai, &idx))) return false;
    xrWaitSwapchainImage(g_hudChain.sc, &wi);
    ID3D11DeviceContext* imm = nullptr;
    xr.device->GetImmediateContext(&imm);
    xr.blit.Draw(xr.device, imm, srv, g_hudChain.rtvs[idx], w, h, xr.linearize, true, g_cfg.hudCrosshairMask);
    imm->Release();
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(g_hudChain.sc, &ri);
    return true;
}

static bool BuildHud(const XrPosef& headPose, XrCompositionLayerQuad& q)
{
    if (!g_hudHasImage) return false;
    // head yaw / pitch from the orientation (forward = -Z)
    const XrQuaternionf& o = headPose.orientation;
    float fx = -2 * (o.x * o.z + o.y * o.w), fy = -2 * (o.y * o.z - o.x * o.w), fz = -(1 - 2 * (o.x * o.x + o.y * o.y));
    float yaw = atan2f(-fx, -fz), pitch = asinf(std::max(-1.0f, std::min(1.0f, fy)));
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = g_hudPoseInit ? (float)(now.QuadPart - g_hudLastT.QuadPart) / freq.QuadPart : 0.0f;
    g_hudLastT = now;
    if (!g_hudPoseInit) { g_hudYaw = yaw; g_hudPitch = pitch; g_hudPoseInit = true; }
    float k = 1.0f - expf(-dt * g_cfg.hudFollowSpeed);
    float dy = yaw - g_hudYaw;
    while (dy > 3.14159265f) dy -= 6.2831853f;
    while (dy < -3.14159265f) dy += 6.2831853f;
    g_hudYaw += dy * k;
    g_hudPitch += (pitch - g_hudPitch) * k;

    float cy = cosf(g_hudYaw), sy = sinf(g_hudYaw), cp = cosf(g_hudPitch), sp = sinf(g_hudPitch);
    float d = g_cfg.hudDistance;
    // direction for yaw (about +Y, 0 = -Z) and pitch (positive = up)
    float dx = -sy * cp, dyv = sp, dz = -cy * cp;
    q = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = xr.localSpace;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = g_hudChain.sc;
    q.subImage.imageRect = { { 0, 0 }, { (int32_t)g_hudChain.width, (int32_t)g_hudChain.height } };
    q.pose.position = { headPose.position.x + dx * d, headPose.position.y + dyv * d, headPose.position.z + dz * d };
    // orientation: yaw about Y, then pitch about X (quad faces +Z, i.e. back towards the player)
    float hy = g_hudYaw * 0.5f, hp = g_hudPitch * 0.5f;
    float qy0 = sinf(hy), qw0 = cosf(hy), qx1 = sinf(hp), qw1 = cosf(hp);
    q.pose.orientation = { qw0 * qx1, qy0 * qw1, -qy0 * qx1, qw0 * qw1 };
    float w = g_cfg.hudWidth;
    q.size = { w, w * (float)g_hudChain.height / (float)g_hudChain.width };
    return true;
}

// ---------------------------------------------------------------------------------------------
// Aim reticle: a small ring drawn by the compositor along the right controller's aim ray. The game's own
// crosshair sits at the centre of the view, which is wrong once the weapon follows the hand.
static Chain g_reticle;

static bool CreateReticle()
{
    const UINT S = 64;
    if (!CreateChain(g_reticle, S, S)) return false;
    std::vector<uint32_t> px(S * S);
    for (UINT y = 0; y < S; y++)
        for (UINT x = 0; x < S; x++) {
            float dx = x + 0.5f - S / 2.0f, dy = y + 0.5f - S / 2.0f, r = sqrtf(dx * dx + dy * dy);
            // white ring (r 18..24) with a dark outline, plus a centre dot; premultiplied alpha
            float white = (r > 18.5f && r < 23.5f) || r < 3.0f ? 1.0f : 0.0f;
            float dark = !white && ((r > 16.5f && r < 25.5f) || r < 4.5f) ? 0.8f : 0.0f;
            uint32_t a = (uint32_t)(255 * (white ? 1.0f : dark));
            uint32_t c = (uint32_t)(255 * white);
            px[y * S + x] = c | (c << 8) | (c << 16) | (a << 24);
        }
    ID3D11DeviceContext* imm = nullptr;
    xr.device->GetImmediateContext(&imm);
    for (auto& img : g_reticle.images) imm->UpdateSubresource(img.texture, 0, nullptr, px.data(), S * 4, 0);
    imm->Release();
    // Cycle every image once so the runtime has released content to show.
    for (size_t i = 0; i < g_reticle.images.size(); i++) {
        uint32_t idx;
        XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        wi.timeout = XR_INFINITE_DURATION;
        XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_reticle.sc, &ai, &idx))) {
            xrWaitSwapchainImage(g_reticle.sc, &wi);
            xrReleaseSwapchainImage(g_reticle.sc, &ri);
        }
    }
    return true;
}

// The game's own crosshair (it differs per weapon and turns red over an enemy) cut from the centre of the frame's
// HUD - drawn there for the view centre - and shown on the VR reticle instead of the ring (ReticleStyle=1).
static Chain g_reticleGame;
static DWORD g_reticleGameAt;

static const float kReticleCut = 0.2f;   // the game's crosshair: this part of the HUD's width around the centre

static void UpdateGameReticle(ID3D11ShaderResourceView* hud)
{
    if (!hud || g_cfg.reticleStyle != 1) return;
    const UINT S = 256;
    if (!g_reticleGame.sc && !CreateChain(g_reticleGame, S, S)) return;
    ID3D11Resource* r = nullptr;
    hud->GetResource(&r);
    D3D11_TEXTURE2D_DESC d = {};
    if (r) { ((ID3D11Texture2D*)r)->GetDesc(&d); r->Release(); }
    if (!d.Width || !d.Height) return;
    float half = kReticleCut * 0.5f, rect[4] = { 0.5f - half, 0.5f - half * d.Width / d.Height, 2 * half, 2 * half * d.Width / d.Height };
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrAcquireSwapchainImage(g_reticleGame.sc, &ai, &idx))) return;
    xrWaitSwapchainImage(g_reticleGame.sc, &wi);
    ID3D11DeviceContext* imm = nullptr;
    xr.device->GetImmediateContext(&imm);
    float clear[4] = {};
    imm->ClearRenderTargetView(g_reticleGame.rtvs[idx], clear);
    xr.blit.Draw(xr.device, imm, hud, g_reticleGame.rtvs[idx], S, S, xr.linearize, true, 0.0f, rect, 1.0f, true);
    imm->Release();
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(g_reticleGame.sc, &ri);
    g_reticleGameAt = GetTickCount();
}

// The ray shots travel along (LOCAL space, forward = -Z of the pose): the right controller, the line from the
// right hand to the left one when held two-handed, or the gaze in vehicles.
static bool AimRay(XrTime t, XrPosef& hand)
{
    // vehicles aim with the head (their gun is below the eyes; a gaze ray keeps the ring on the hit point)
    if (CameraVehicleAim() && xr.viewSpace) { if (!LocateHand(xr.viewSpace, t, hand)) return false; }
    else if (!LocateRightAim(t, hand, nullptr)) return false;
    // held with both hands: turned so the weapon's front grip lies in the left hand
    if (xr.twoHand && !CameraVehicleAim()) hand.orientation = QuatMul(g_twoHandTurn, hand.orientation);
    return true;
}

static bool BuildReticle(XrTime t, XrCompositionLayerQuad& q)
{
    if (!g_cfg.reticle || g_cfg.aimMode != 1 || VRGunHand() < 0 || WeaponsMeleeHeld() == VRGunHand()) return false;   // no gun in hand
    if (!g_reticle.sc && !CreateReticle()) { g_cfg.reticle = 0; return false; }
    XrPosef hand;
    if (!AimRay(t, hand)) return false;
    // forward = -Z of the aim pose: always along the barrel
    const XrQuaternionf& o = hand.orientation;
    float fx = -2 * (o.x * o.z + o.y * o.w), fy = -2 * (o.y * o.z - o.x * o.w), fz = -(1 - 2 * (o.x * o.x + o.y * o.y));
    // on the game's own hit point when known (shots land exactly on the ring), else at ReticleDistance
    float d = g_cfg.reticleDistance;
    float hit = CameraAimHitMeters();
    if (hit > 0.0f) d = std::min(std::max(hit, 0.4f), 60.0f);
    q = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = xr.localSpace;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    bool game = g_cfg.reticleStyle == 1 && g_reticleGame.sc && GetTickCount() - g_reticleGameAt < 300;
    const Chain& rc = game ? g_reticleGame : g_reticle;
    // the game's crosshair at its own angular size: the cut is 0.12 of the view's width
    q.subImage.swapchain = rc.sc;
    q.subImage.imageRect = { { 0, 0 }, { (int32_t)rc.width, (int32_t)rc.height } };
    q.pose.orientation = o;   // quad front (+Z) faces back along the ray, towards the player
    q.pose.position = { hand.position.x + fx * d, hand.position.y + fy * d, hand.position.z + fz * d };
    float size = d * 0.05f * g_cfg.reticleSize;   // ~3 degrees x ReticleSize
    q.size = { size, size };
    if (game) q.size.width = q.size.height = d * kReticleCut * 2.0f * tanf(g_eyeDone[0].fov.angleRight) * g_cfg.reticleSize;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Sniper scope: with a sniper rifle brought up to the eye (or held two-handed) a round lens above the barrel
// shows the part of the right-eye image around where the shot lands, magnified. It is cut from the normal eye
// render (no extra rendering), so it is only as sharp as that region of the eye image.
static Chain g_scope;
static bool g_scopeHasImage;

static XrVector3f Rotate(const XrQuaternionf& q, XrVector3f v)   // q * v * conj(q)
{
    XrVector3f t = { 2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x) };
    return { v.x + q.w * t.x + (q.y * t.z - q.z * t.y), v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
             v.z + q.w * t.z + (q.x * t.y - q.y * t.x) };
}

static bool ScopeWanted()
{
    // (off by default: the player keeps only the main crosshair - Scope=1 brings the lens back)
    if (!g_cfg.scope || g_cfg.aimMode != 1 || IKPawnIsVehicle() || VRGunHand() < 0 || g_setup.active) return false;
    float fov;
    if (CameraGameZoom(fov)) return true;   // the game zoomed (scoped fine aim): show its zoom in the lens
    return IKWeaponHasScope() && (xr.twoHand || xr.raised);
}

// The lens' field of view: the game's zoom while it zooms, else ScopeFov.
static float ScopeFovDeg()
{
    float fov;
    if (CameraGameZoom(fov)) return std::min(std::max(fov, 3.0f), 40.0f);
    return g_cfg.scopeFov;
}

// Right-eye frame: cut the region around the hit point out of the eye image.
static void UpdateScope(ID3D11Texture2D* bb, const EyeFrame& f)
{
    g_scopeHasImage = false;
    XrPosef ray;
    if (!ScopeWanted() || !AimRay(f.time, ray)) return;
    XrVector3f fwd = Rotate(ray.orientation, { 0, 0, -1 });
    float hit = CameraAimHitMeters();
    float d = hit > 0.0f ? std::min(std::max(hit, 0.4f), 300.0f) : 50.0f;
    XrVector3f dir = { ray.position.x + fwd.x * d - f.pose.position.x, ray.position.y + fwd.y * d - f.pose.position.y,
                       ray.position.z + fwd.z * d - f.pose.position.z };
    const XrQuaternionf& o = f.pose.orientation;
    XrVector3f e = Rotate({ -o.x, -o.y, -o.z, o.w }, dir);   // into eye space (forward = -Z)
    if (e.z > -0.1f * sqrtf(e.x * e.x + e.y * e.y + e.z * e.z)) return;
    float tanR = tanf(f.fov.angleRight), tanU = tanf(f.fov.angleUp);
    float u = 0.5f + 0.5f * (e.x / -e.z) / tanR, v = 0.5f - 0.5f * (e.y / -e.z) / tanU;
    float s = tanf(ScopeFovDeg() * 0.5f * 0.0174533f);
    float rect[4] = { 0, 0, s / tanR, s / tanU };
    rect[0] = u - rect[2] * 0.5f;
    rect[1] = v - rect[3] * 0.5f;
    g_scopeHasImage = CopyToChain(g_scope, bb, rect, 512);
}

static bool BuildScope(XrTime t, XrCompositionLayerQuad& q)
{
    XrPosef ray;
    if (!g_scopeHasImage || !ScopeWanted() || !AimRay(t, ray)) return false;
    XrVector3f fwd = Rotate(ray.orientation, { 0, 0, -1 }), up = Rotate(ray.orientation, { 0, 1, 0 }),
               right = Rotate(ray.orientation, { 1, 0, 0 });
    float sc[4];
    ScopePlace(sc);
    float a = sc[2], b = sc[1], c = sc[0];
    q = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = xr.localSpace;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = g_scope.sc;
    q.subImage.imageRect = { { 0, 0 }, { (int32_t)g_scope.width, (int32_t)g_scope.height } };
    q.pose.orientation = ray.orientation;   // lens faces back along the barrel
    q.pose.position = { ray.position.x + fwd.x * a + up.x * b + right.x * c, ray.position.y + fwd.y * a + up.y * b + right.y * c,
                        ray.position.z + fwd.z * a + up.z * b + right.z * c };
    q.size = { sc[3], sc[3] };
    return true;
}

static void PresentTheater(ID3D11Texture2D* bb)
{
    XrFrameState fs;
    if (!ConsumeOrWaitFrame(fs)) return;
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
        g_lastTheaterQuad = quad;
        g_haveLastTheater = true;
    }
    EndFrame(fs, ok ? (XrCompositionLayerBaseHeader*)&quad : nullptr);
}

static void PresentStereo(ID3D11Texture2D* bb, const EyeFrame& f)
{
    g_overlayPose = &f.pose;
    g_overlayFov = f.fov;
    g_overlayHud = g_frameHud && g_cfg.hudMinimal && g_cfg.hudMarkersInEyes && CameraPlayerView() ? g_frameHud : nullptr;
    bool copied = CopyToChain(g_eyes[f.eye], bb);
    g_overlayPose = nullptr;
    if (!copied) return;
    if (f.eye == 1) UpdateScope(bb, f);
    if (g_frameHud && f.eye == 1) { g_hudHasImage = CopyHud(g_frameHud); UpdateGameReticle(g_frameHud); }
    else if (!g_cfg.vrHud) g_hudHasImage = false;
    g_eyeDone[f.eye] = f;
    g_eyeHave[f.eye] = true;
    if (f.eye != 1 || !g_eyeHave[0]) return;   // submit after each left+right pair
    g_eyeHave[0] = g_eyeHave[1] = false;

    XrFrameState fs;
    if (!ConsumeOrWaitFrame(fs)) return;
    XrCompositionLayerProjectionView pv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    for (int i = 0; i < 2; i++) {
        pv[i].pose = g_eyeDone[i].pose;
        pv[i].fov = g_eyeDone[i].fov;
        pv[i].subImage.swapchain = g_eyes[i].sc;
        pv[i].subImage.imageRect = { { 0, 0 }, { (int32_t)g_eyes[i].width, (int32_t)g_eyes[i].height } };
        g_lastProjViews[i] = pv[i];
    }
    g_haveLastProj = true;
    XrCompositionLayerProjection proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    proj.space = xr.localSpace;
    proj.viewCount = 2;
    proj.views = pv;
    XrCompositionLayerQuad reticle, hud;
    bool haveReticle = fs.shouldRender && BuildReticle(fs.predictedDisplayTime, reticle);
    // head pose for the HUD panel: between the two eyes, oriented like the left eye
    XrPosef head = g_eyeDone[0].pose;
    head.position = { (g_eyeDone[0].pose.position.x + g_eyeDone[1].pose.position.x) * 0.5f,
                      (g_eyeDone[0].pose.position.y + g_eyeDone[1].pose.position.y) * 0.5f,
                      (g_eyeDone[0].pose.position.z + g_eyeDone[1].pose.position.z) * 0.5f };
    bool haveHud = fs.shouldRender && g_cfg.vrHud && BuildHud(head, hud);
    XrCompositionLayerQuad scope, wheel;
    bool haveScope = fs.shouldRender && BuildScope(fs.predictedDisplayTime, scope);
    static bool scopeWas;
    if (haveScope != scopeWas) { Log("vr: scope %s", haveScope ? "shown" : "hidden"); scopeWas = haveScope; }
    bool haveWheel = fs.shouldRender && BuildWheel(wheel);
    XrCompositionLayerQuad panel, mark0, mark1, mark2;
    bool havePanel = fs.shouldRender && BuildPanel(panel);
    XrTime mt = fs.predictedDisplayTime;
    bool haveMark0 = havePanel && BuildZoneMark(0, mt, head, mark0), haveMark1 = havePanel && BuildZoneMark(1, mt, head, mark1);
    bool haveMark2 = havePanel && BuildZoneMark(2, mt, head, mark2);
    XrCompositionLayerQuad ray;
    bool haveRay = havePanel && BuildRay(head, ray);
    EndFrame(fs, fs.shouldRender ? (XrCompositionLayerBaseHeader*)&proj : nullptr,
             { haveHud ? (XrCompositionLayerBaseHeader*)&hud : nullptr,
               haveScope ? (XrCompositionLayerBaseHeader*)&scope : nullptr,
               haveReticle && !haveWheel ? (XrCompositionLayerBaseHeader*)&reticle : nullptr,
               haveWheel ? (XrCompositionLayerBaseHeader*)&wheel : nullptr,
               havePanel ? (XrCompositionLayerBaseHeader*)&panel : nullptr,
               haveMark0 ? (XrCompositionLayerBaseHeader*)&mark0 : nullptr,
               haveMark1 ? (XrCompositionLayerBaseHeader*)&mark1 : nullptr,
               haveMark2 ? (XrCompositionLayerBaseHeader*)&mark2 : nullptr,
               haveRay ? (XrCompositionLayerBaseHeader*)&ray : nullptr });
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
    DWORD tid = GameThreadId();
    bool begunOnGameThread = g_xrFrameBegun && (!tid || GetCurrentThreadId() == tid);
    if (!begunOnGameThread) {
        PollEvents();
        if (!xr.running) { ResetStereo(); return; }
        UpdateInput();
    } else if (!xr.running) {
        g_xrFrameBegun = false;
        ResetStereo();
        return;
    }

    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return;
    D3D11_TEXTURE2D_DESC bd;
    bb->GetDesc(&bd);
    g_aspect = (float)bd.Width / (float)bd.Height;

    EyeFrame f;
    bool have = false;
    size_t depth = 0;
    {
        std::lock_guard<std::mutex> lock(g_stereoMutex);
        depth = g_pending.size();
        if (GameSingleThreadRender()) {
            // this tick's eye(s), in order (pass 1 pops left eye, pass 2 pops right eye)
            g_pending.clear();
            if (g_tickEyeFrame == GameFrameCount() && g_tickEyeNext < g_tickEyeCount) {
                depth = (size_t)(g_tickEyeCount - g_tickEyeNext);
                f = g_tickEyes[g_tickEyeNext++];
                have = true;
                if (g_tickEyeNext >= g_tickEyeCount) g_tickEyeFrame = -1;
            } else {
                depth = 0;
            }
        } else if (!g_pending.empty()) { f = g_pending.front(); g_pending.pop_front(); have = true; }
    }
    if (g_cfg.logCameraSeq > 0) Log("seq: present (game frame %ld, %u eye frames waited)", GameFrameCount(), (unsigned)depth);
    {   // debug: how many eye frames wait when a frame is shown (a stale extra one = every frame in the wrong eye)
        static long hist[6];
        static DWORD at;
        hist[std::min<size_t>(depth, 5)]++;
        if (g_cfg.logFps && GetTickCount() - at > 5000) {
            at = GetTickCount();
            Log("vr: eye queue at present: 0:%ld 1:%ld 2:%ld 3:%ld 4:%ld", hist[0], hist[1], hist[2], hist[3], hist[4]);
            memset(hist, 0, sizeof(hist));
        }
    }
    bool stereo = g_cfg.vrMode == 1 && CameraInGameplay();
    if (stereo != g_wasStereo) {
        Log("vr: %s", stereo ? "stereo gameplay view" : "theater screen");
        g_wasStereo = stereo;
        if (!stereo) ResetStereo();   // (not when the 3D view starts: the frame in flight would be dropped and every
                                      // later frame would go to the wrong eye)
    }
    if (stereo) {
        static long pops, misses;
        if (have) {
            PresentStereo(bb, f);
            pops++;
        } else {
            misses++;
            if (begunOnGameThread && g_xrFrameBegun) {
                FinishUnsubmittedFrame(g_xrFrameState);
                g_xrFrameBegun = false;
            }
        }
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

void TraceInstall(ID3D11Device* dev);
ID3D11ShaderResourceView* TraceEndFrame();
void HudSetWanted(bool on);

static void OnPresent(IDXGISwapChain* sc, UINT flags)
{
    if (flags & DXGI_PRESENT_TEST) return;
    if (g_cfg.debugAutomation || g_cfg.vrHud) {
        ID3D11Device* dev = nullptr;
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev))) { TraceInstall(dev); dev->Release(); }
        g_frameHud = TraceEndFrame();
        // Split the HUD off the next frame while the 3D view is shown.
        HudSetWanted(g_cfg.vrHud && g_cfg.vrMode == 1 && xr.running && CameraInGameplay());
    }
    long n = ++g_frames;
    if (n == 1) {
        DXGI_SWAP_CHAIN_DESC d = {};
        sc->GetDesc(&d);
        wchar_t cls[64] = {};
        GetClassNameW(d.OutputWindow, cls, 64);
        RECT rc = {};
        GetClientRect(d.OutputWindow, &rc);
        Log("vr: first Present on swapchain %p: %ux%u windowed %d swapeffect %d window %p '%ls' client %ldx%ld", sc,
            d.BufferDesc.Width, d.BufferDesc.Height, d.Windowed, d.SwapEffect, d.OutputWindow, cls, rc.right, rc.bottom);
    }
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
    extern volatile bool g_d3dOwnCalls;
    g_d3dOwnCalls = true;
    static void* s_faultAt;
    __try {
        VRFrame(sc);
    } __except (s_faultAt = (GetExceptionInformation())->ExceptionRecord->ExceptionAddress, EXCEPTION_EXECUTE_HANDLER) {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&OnPresent, &self);
        Log("vr: exception %08X at %p (mod+%X) in VRFrame - VR disabled", GetExceptionCode(), s_faultAt,
            (unsigned)((uintptr_t)s_faultAt - (uintptr_t)self));
        xr.failed = true;
    }
    g_d3dOwnCalls = false;
}

static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    OnPresent(sc, flags);
    extern int g_debugWindowEye;
    bool GameDrawingRightEye();
    if (g_debugWindowEye && xr.running && CameraInGameplay() && (GameDrawingRightEye() ? 2 : 1) != g_debugWindowEye)
        return S_OK;   // debug: this eye's drawing is not shown in the window
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
