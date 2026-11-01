// Minimal mock OpenXR runtime (D3D11, stereo HMD) for testing the mod without a headset.
// Enable per process: set XR_RUNTIME_JSON=<path>\mockxr.json before starting the game.
// Writes mockxr.log and dumps submitted eye images (mockxr_<frame>_<layer>_<view>.bmp) next to the DLL.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <mutex>
#include <math.h>

static FILE* g_log;
static std::string g_dir;
static std::mutex g_mx;

static void L(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_log) g_log = fopen((g_dir + "mockxr.log").c_str(), "w");
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

struct Swapchain {
    std::vector<ID3D11Texture2D*> tex;
    uint32_t w, h, next = 0;
    int64_t fmt;
};
static ID3D11Device* g_dev;
static std::deque<XrEventDataBuffer> g_events;
static std::map<std::string, XrPath> g_paths;
static int64_t g_frame;
static XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;

// ---- scripted poses and inputs: mockxr_pose.txt next to the DLL (re-read when it changes) --------------------
// Lines "<name> <values...>":  head x y z yaw pitch [roll]   |   <pose action> x y z yaw pitch roll   (degrees; yaw + =
// left, pitch + = up; OpenXR local space: x right, y up, -z forward; metres)   |   <float/bool action> value   |
// <vector action> x y   |   vel_left / vel_right x y z (m/s, hand velocity).  Pose actions not listed: the other
// pose of the same hand (aim <-> grip); none at all: the old fixed pose. Without "head": the slow head sweep.
struct PoseEntry { float v[6]; int n; };
static std::map<std::string, PoseEntry> g_pose;
static FILETIME g_poseTime;
static std::map<uint64_t, std::string> g_actionNames;   // action handle -> name
static std::map<uint64_t, std::string> g_spaceActions;  // action space handle -> action name
static std::map<uint64_t, int> g_refSpaceTypes;         // reference space handle -> type

static void LoadPose()
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    std::string path = g_dir + "mockxr_pose.txt";
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a)) {
        if (!g_pose.empty()) { std::lock_guard<std::mutex> lk(g_mx); g_pose.clear(); }
        return;
    }
    if (CompareFileTime(&a.ftLastWriteTime, &g_poseTime) == 0) return;
    g_poseTime = a.ftLastWriteTime;
    std::map<std::string, PoseEntry> m;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char name[64] = {};
        PoseEntry e = {};
        int n = sscanf_s(line, "%63s %f %f %f %f %f %f", name, (unsigned)sizeof(name), &e.v[0], &e.v[1], &e.v[2], &e.v[3], &e.v[4], &e.v[5]);
        if (n >= 2 && name[0] != '#') { e.n = n - 1; m[name] = e; }
    }
    fclose(f);
    std::lock_guard<std::mutex> lk(g_mx);
    g_pose = m;
}

static bool PoseValue(const std::string& name, PoseEntry& e)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_pose.find(name);
    if (it == g_pose.end()) return false;
    e = it->second;
    return true;
}

static XrQuaternionf Euler(float yawDeg, float pitchDeg, float rollDeg)
{
    const float d = 3.14159265f / 180.0f;
    float y = yawDeg * d * 0.5f, p = pitchDeg * d * 0.5f, r = rollDeg * d * 0.5f;
    XrQuaternionf qy = { 0, sinf(y), 0, cosf(y) }, qp = { sinf(p), 0, 0, cosf(p) }, qr = { 0, 0, -sinf(r), cosf(r) };
    auto mul = [](XrQuaternionf a, XrQuaternionf b) {
        return XrQuaternionf{ a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                              a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
    };
    return mul(mul(qy, qp), qr);
}

static XrTime Now()
{
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (XrTime)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}

static void PushState(XrSessionState s)
{
    XrEventDataBuffer b = { XR_TYPE_EVENT_DATA_BUFFER };
    auto* e = (XrEventDataSessionStateChanged*)&b;
    e->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    e->session = (XrSession)1;
    e->state = s;
    e->time = Now();
    g_events.push_back(b);
    g_state = s;
}

static void SaveBMP(ID3D11Texture2D* t, const std::string& name)
{
    D3D11_TEXTURE2D_DESC d; t->GetDesc(&d);
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&s, nullptr, &st))) return;
    ID3D11DeviceContext* c; g_dev->GetImmediateContext(&c);
    c->CopyResource(st, t);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(c->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        uint32_t w = d.Width / 2, h = d.Height / 2;   // half size is enough to look at
        FILE* f = fopen((g_dir + name).c_str(), "wb");
        if (f) {
            uint32_t rowBytes = w * 3, pad = (4 - rowBytes % 4) % 4, size = 54 + (rowBytes + pad) * h;
            uint8_t hdr[54] = { 'B', 'M' };
            *(uint32_t*)(hdr + 2) = size; *(uint32_t*)(hdr + 10) = 54; *(uint32_t*)(hdr + 14) = 40;
            *(int32_t*)(hdr + 18) = w; *(int32_t*)(hdr + 22) = h; *(uint16_t*)(hdr + 26) = 1; *(uint16_t*)(hdr + 28) = 24;
            fwrite(hdr, 1, 54, f);
            bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
            for (int y = h - 1; y >= 0; y--) {
                const uint8_t* row = (const uint8_t*)m.pData + (size_t)(y * 2) * m.RowPitch;
                for (uint32_t x = 0; x < w; x++) {
                    const uint8_t* p = row + x * 2 * 4;
                    uint8_t px[3] = { bgra ? p[0] : p[2], p[1], bgra ? p[2] : p[0] };
                    fwrite(px, 1, 3, f);
                }
                uint8_t z[3] = {}; fwrite(z, 1, pad, f);
            }
            fclose(f);
        }
        c->Unmap(st, 0);
    }
    st->Release(); c->Release();
}

// ---------------------------------------------------------------------------------------------
static XrResult XRAPI_CALL EnumInstExt(const char*, uint32_t cap, uint32_t* cnt, XrExtensionProperties* p)
{
    *cnt = 1;
    if (cap >= 1 && p) { strcpy_s(p[0].extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME); p[0].extensionVersion = 9; }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL CreateInstance(const XrInstanceCreateInfo* ci, XrInstance* inst) { L("xrCreateInstance app=%s", ci->applicationInfo.applicationName); *inst = (XrInstance)1; return XR_SUCCESS; }
static XrResult XRAPI_CALL DestroyInstance(XrInstance) { return XR_SUCCESS; }
static XrResult XRAPI_CALL GetInstanceProperties(XrInstance, XrInstanceProperties* p) { strcpy_s(p->runtimeName, "MockXR"); p->runtimeVersion = XR_MAKE_VERSION(0, 1, 0); return XR_SUCCESS; }
static XrResult XRAPI_CALL GetSystem(XrInstance, const XrSystemGetInfo*, XrSystemId* s) { *s = 1; return XR_SUCCESS; }
static XrResult XRAPI_CALL GetD3D11Req(XrInstance, XrSystemId, XrGraphicsRequirementsD3D11KHR* r)
{
    IDXGIFactory1* f = nullptr; IDXGIAdapter1* a = nullptr; DXGI_ADAPTER_DESC1 d = {};
    typedef HRESULT(WINAPI* PFN)(REFIID, void**);
    auto cf = (PFN)GetProcAddress(LoadLibraryA("dxgi.dll"), "CreateDXGIFactory1");
    if (cf && SUCCEEDED(cf(__uuidof(IDXGIFactory1), (void**)&f)) && SUCCEEDED(f->EnumAdapters1(0, &a))) a->GetDesc1(&d);
    if (a) a->Release(); if (f) f->Release();
    r->adapterLuid = d.AdapterLuid; r->minFeatureLevel = D3D_FEATURE_LEVEL_10_0;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL PollEvent(XrInstance, XrEventDataBuffer* b)
{
    if (g_events.empty()) return XR_EVENT_UNAVAILABLE;
    *b = g_events.front(); g_events.pop_front();
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL StringToPath(XrInstance, const char* s, XrPath* p)
{
    auto it = g_paths.find(s);
    if (it == g_paths.end()) it = g_paths.emplace(s, (XrPath)(g_paths.size() + 1)).first;
    *p = it->second; return XR_SUCCESS;
}
static XrResult XRAPI_CALL CreateSession(XrInstance, const XrSessionCreateInfo* ci, XrSession* s)
{
    auto* gb = (const XrGraphicsBindingD3D11KHR*)ci->next;
    if (!gb || gb->type != XR_TYPE_GRAPHICS_BINDING_D3D11_KHR || !gb->device) { L("xrCreateSession: no D3D11 binding"); return XR_ERROR_GRAPHICS_DEVICE_INVALID; }
    g_dev = gb->device; g_dev->AddRef();
    L("xrCreateSession device=%p", g_dev);
    *s = (XrSession)1;
    PushState(XR_SESSION_STATE_IDLE);
    PushState(XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL DestroySession(XrSession) { return XR_SUCCESS; }
static XrResult XRAPI_CALL BeginSession(XrSession, const XrSessionBeginInfo*) { L("xrBeginSession"); PushState(XR_SESSION_STATE_SYNCHRONIZED); PushState(XR_SESSION_STATE_VISIBLE); PushState(XR_SESSION_STATE_FOCUSED); return XR_SUCCESS; }
static XrResult XRAPI_CALL EndSession(XrSession) { return XR_SUCCESS; }
static XrResult XRAPI_CALL CreateRefSpace(XrSession, const XrReferenceSpaceCreateInfo* ci, XrSpace* s)
{
    static uint64_t n = 1;
    *s = (XrSpace)n++;
    std::lock_guard<std::mutex> lk(g_mx);
    g_refSpaceTypes[(uint64_t)*s] = (int)ci->referenceSpaceType;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL DestroySpace(XrSpace) { return XR_SUCCESS; }
static XrResult XRAPI_CALL EnumFormats(XrSession, uint32_t cap, uint32_t* cnt, int64_t* f)
{
    static const int64_t fmts[] = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM };
    *cnt = 3; if (cap >= 3 && f) memcpy(f, fmts, sizeof(fmts)); return XR_SUCCESS;
}
static XrResult XRAPI_CALL CreateSwapchain(XrSession, const XrSwapchainCreateInfo* ci, XrSwapchain* out)
{
    auto* sc = new Swapchain{ {}, ci->width, ci->height, 0, ci->format };
    DXGI_FORMAT typeless = ci->format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_TYPELESS
                         : ci->format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? DXGI_FORMAT_B8G8R8A8_TYPELESS : (DXGI_FORMAT)ci->format;
    for (int i = 0; i < 3; i++) {
        D3D11_TEXTURE2D_DESC d = { ci->width, ci->height, 1, 1, typeless, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        ID3D11Texture2D* t = nullptr;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &t))) { L("swapchain texture create failed"); return XR_ERROR_RUNTIME_FAILURE; }
        sc->tex.push_back(t);
    }
    L("xrCreateSwapchain %ux%u fmt %d", ci->width, ci->height, (int)ci->format);
    *out = (XrSwapchain)sc; return XR_SUCCESS;
}
static XrResult XRAPI_CALL DestroySwapchain(XrSwapchain s) { auto* sc = (Swapchain*)s; for (auto* t : sc->tex) t->Release(); delete sc; return XR_SUCCESS; }
static XrResult XRAPI_CALL EnumImages(XrSwapchain s, uint32_t cap, uint32_t* cnt, XrSwapchainImageBaseHeader* imgs)
{
    auto* sc = (Swapchain*)s; *cnt = (uint32_t)sc->tex.size();
    if (cap >= *cnt && imgs) for (uint32_t i = 0; i < *cnt; i++) ((XrSwapchainImageD3D11KHR*)imgs)[i].texture = sc->tex[i];
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL AcquireImage(XrSwapchain s, const XrSwapchainImageAcquireInfo*, uint32_t* i) { auto* sc = (Swapchain*)s; *i = sc->next; sc->next = (sc->next + 1) % sc->tex.size(); return XR_SUCCESS; }
static XrResult XRAPI_CALL WaitImage(XrSwapchain, const XrSwapchainImageWaitInfo*) { return XR_SUCCESS; }
static XrResult XRAPI_CALL ReleaseImage(XrSwapchain, const XrSwapchainImageReleaseInfo*) { return XR_SUCCESS; }

static XrTime g_nextDisplay;
static XrResult XRAPI_CALL WaitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState* fs)
{
    const XrTime period = 11111111;   // 90 Hz
    XrTime now = Now();
    if (!g_nextDisplay || g_nextDisplay < now) g_nextDisplay = now + period;
    while (Now() < g_nextDisplay - period) Sleep(1);
    fs->predictedDisplayTime = g_nextDisplay;
    fs->predictedDisplayPeriod = period;
    fs->shouldRender = XR_TRUE;
    LoadPose();
    g_nextDisplay += period;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL BeginFrame(XrSession, const XrFrameBeginInfo*) { return XR_SUCCESS; }
static XrResult XRAPI_CALL EndFrame(XrSession, const XrFrameEndInfo* ei)
{
    g_frame++;
    // like real runtimes: a quad with a non-unit orientation or a non-finite pose fails the whole frame
    for (uint32_t i = 0; i < ei->layerCount; i++)
        if (ei->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            const XrPosef& p = ((const XrCompositionLayerQuad*)ei->layers[i])->pose;
            float l = sqrtf(p.orientation.x * p.orientation.x + p.orientation.y * p.orientation.y + p.orientation.z * p.orientation.z +
                            p.orientation.w * p.orientation.w);
            if (!(fabsf(l - 1.0f) < 0.01f) || !_finite(p.position.x + p.position.y + p.position.z)) {
                static int64_t logged = -1000;
                if (g_frame - logged > 200) { logged = g_frame; L("frame %lld: layer %u POSE INVALID (|q| %.3f) -> XR_ERROR_POSE_INVALID", g_frame, i, l); }
                return XR_ERROR_POSE_INVALID;
            }
        }
    bool dump = (g_frame % 450) == 0;
    {   // on request: mockxr_dump.txt next to the DLL -> dump this frame (the file is deleted)
        std::string req = g_dir + "mockxr_dump.txt";
        if (GetFileAttributesA(req.c_str()) != INVALID_FILE_ATTRIBUTES && DeleteFileA(req.c_str())) dump = true;
    }
    static int64_t lastTypes[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    if (dump) L("frame %lld: %u layers", g_frame, ei->layerCount);
    for (uint32_t i = 1; dump && i < ei->layerCount; i++)
        if (ei->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            auto* q = (const XrCompositionLayerQuad*)ei->layers[i];
            L("  overlay quad pos(%.2f %.2f %.2f) size %.2f flags %llu", q->pose.position.x, q->pose.position.y, q->pose.position.z, q->size.width,
              (unsigned long long)q->layerFlags);
            auto* sc = (Swapchain*)q->subImage.swapchain;
            char n[64]; sprintf_s(n, "mockxr_%lld_overlay%u.bmp", g_frame, i);
            SaveBMP(sc->tex[0], n);
        }
    for (uint32_t i = 0; i < ei->layerCount; i++) {
        const XrCompositionLayerBaseHeader* l = ei->layers[i];
        int64_t& lastType = lastTypes[i < 8 ? i : 7];
        if (l->type != lastType) { L("frame %lld: layer %u type %d (quad=%d proj=%d)", g_frame, i, (int)l->type, XR_TYPE_COMPOSITION_LAYER_QUAD, XR_TYPE_COMPOSITION_LAYER_PROJECTION); lastType = l->type; }
        if (!dump) continue;
        if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            auto* q = (const XrCompositionLayerQuad*)l;
            auto* sc = (Swapchain*)q->subImage.swapchain;
            char n[64]; sprintf_s(n, "mockxr_%lld_quad.bmp", g_frame);
            SaveBMP(sc->tex[(sc->next + sc->tex.size() - 1) % sc->tex.size()], n);
        } else if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            auto* p = (const XrCompositionLayerProjection*)l;
            for (uint32_t v = 0; v < p->viewCount; v++) {
                auto* sc = (Swapchain*)p->views[v].subImage.swapchain;
                auto& pv = p->views[v];
                L("  view %u pos(%.3f %.3f %.3f) fov L%.2f R%.2f U%.2f D%.2f rect %dx%d", v, pv.pose.position.x, pv.pose.position.y, pv.pose.position.z,
                  pv.fov.angleLeft, pv.fov.angleRight, pv.fov.angleUp, pv.fov.angleDown, pv.subImage.imageRect.extent.width, pv.subImage.imageRect.extent.height);
                char n[64]; sprintf_s(n, "mockxr_%lld_eye%u.bmp", g_frame, v);
                SaveBMP(sc->tex[(sc->next + sc->tex.size() - 1) % sc->tex.size()], n);
            }
        }
    }
    if (ei->layerCount == 0 && (g_frame % 450) == 0) L("frame %lld: no layers", g_frame);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL LocateViews(XrSession, const XrViewLocateInfo* li, XrViewState* vs, uint32_t cap, uint32_t* cnt, XrView* views)
{
    *cnt = 2;
    if (cap < 2 || !views) return XR_SUCCESS;
    vs->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    // Slow head yaw sweep (+-25 deg, 8 s period) so dumps show head tracking.
    double t = (double)li->displayTime * 1e-9;
    float yaw = (float)(sin(t * 6.2831853 / 8.0) * 25.0 * 3.14159265 / 180.0), pitch = 0, roll = 0, hx = 0, hy = 0, hz = 0;
    PoseEntry h;
    if (PoseValue("head", h)) { hx = h.v[0]; hy = h.v[1]; hz = h.v[2]; yaw = h.v[3] * 3.14159265f / 180.0f; pitch = h.v[4]; roll = h.n > 5 ? h.v[5] : 0; }
    for (int e = 0; e < 2; e++) {
        XrQuaternionf q = Euler(yaw * 180.0f / 3.14159265f, pitch, roll);
        views[e].pose.orientation = q;
        // eye offset along the head's right axis (turns and tilts with the head)
        float ex = e ? 0.032f : -0.032f;
        float rx = 1 - 2 * (q.y * q.y + q.z * q.z), ry = 2 * (q.x * q.y + q.z * q.w), rz = 2 * (q.x * q.z - q.y * q.w);
        views[e].pose.position = { hx + ex * rx, hy + ex * ry, hz + ex * rz };
        views[e].fov = { e ? -0.79f : -0.87f, e ? 0.87f : 0.79f, 0.84f, -0.91f };
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL CreateActionSet(XrInstance, const XrActionSetCreateInfo*, XrActionSet* s) { static uint64_t n = 1; *s = (XrActionSet)n++; return XR_SUCCESS; }
static XrResult XRAPI_CALL CreateAction(XrActionSet, const XrActionCreateInfo* ci, XrAction* a)
{
    static uint64_t n = 1;
    *a = (XrAction)n++;
    std::lock_guard<std::mutex> lk(g_mx);
    g_actionNames[(uint64_t)*a] = ci->actionName;
    return XR_SUCCESS;
}
static std::string ActionName(XrAction a)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_actionNames.find((uint64_t)a);
    return it == g_actionNames.end() ? std::string() : it->second;
}
static XrResult XRAPI_CALL Suggest(XrInstance, const XrInteractionProfileSuggestedBinding*) { return XR_SUCCESS; }
static XrResult XRAPI_CALL Attach(XrSession, const XrSessionActionSetsAttachInfo*) { return XR_SUCCESS; }
static XrResult XRAPI_CALL Sync(XrSession, const XrActionsSyncInfo*) { return XR_SUCCESS; }
static XrResult XRAPI_CALL GetFloat(XrSession, const XrActionStateGetInfo* gi, XrActionStateFloat* s)
{
    PoseEntry e;
    bool on = PoseValue(ActionName(gi->action), e);
    s->isActive = on ? XR_TRUE : XR_FALSE;
    s->currentState = on ? e.v[0] : 0.0f;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL GetBool(XrSession, const XrActionStateGetInfo* gi, XrActionStateBoolean* s)
{
    PoseEntry e;
    bool on = PoseValue(ActionName(gi->action), e);
    s->isActive = on ? XR_TRUE : XR_FALSE;
    s->currentState = on && e.v[0] > 0.5f ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL Haptic(XrSession, const XrHapticActionInfo*, const XrHapticBaseHeader*) { return XR_SUCCESS; }
static XrResult XRAPI_CALL GetVec2(XrSession, const XrActionStateGetInfo* gi, XrActionStateVector2f* s)
{
    PoseEntry e;
    bool on = PoseValue(ActionName(gi->action), e);
    s->isActive = on ? XR_TRUE : XR_FALSE;
    s->currentState = on ? XrVector2f{ e.v[0], e.n > 1 ? e.v[1] : 0.0f } : XrVector2f{ 0, 0 };
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL CreateActionSpace(XrSession, const XrActionSpaceCreateInfo* ci, XrSpace* s)
{
    static uint64_t n = 100;
    *s = (XrSpace)n++;
    std::string name = ActionName(ci->action);
    std::lock_guard<std::mutex> lk(g_mx);
    g_spaceActions[(uint64_t)*s] = name;
    return XR_SUCCESS;
}
// Hand spaces: from mockxr_pose.txt; otherwise the right hand aims 20 degrees to the right, held low and to the right.
// The view reference space (the head) follows "head".
static XrResult XRAPI_CALL LocateSpace(XrSpace space, XrSpace, XrTime, XrSpaceLocation* loc)
{
    loc->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                         XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    std::string action;
    int refType = 0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_spaceActions.find((uint64_t)space);
        if (it != g_spaceActions.end()) action = it->second;
        auto r = g_refSpaceTypes.find((uint64_t)space);
        if (r != g_refSpaceTypes.end()) refType = r->second;
    }
    PoseEntry e;
    bool found = false;
    if (refType == XR_REFERENCE_SPACE_TYPE_VIEW) {
        found = PoseValue("head", e);
        if (found && e.n < 6) { e.v[5] = 0; e.n = 6; }
    } else if (!action.empty()) {
        found = PoseValue(action, e);
        if (!found) {   // the hand's other pose
            std::string other = action;
            size_t p = other.find("_aim_pose");
            if (p != std::string::npos) other.replace(p, 9, "_grip_pose");
            else if ((p = other.find("_grip_pose")) != std::string::npos) other.replace(p, 10, "_aim_pose");
            found = PoseValue(other, e);
        }
    }
    if (found) {
        loc->pose.orientation = Euler(e.v[3], e.v[4], e.v[5]);
        loc->pose.position = { e.v[0], e.v[1], e.v[2] };
    } else {
        float yaw = -20.0f * 3.14159265f / 180.0f;
        loc->pose.orientation = { 0.0f, sinf(yaw / 2), 0.0f, cosf(yaw / 2) };
        loc->pose.position = { 0.2f, -0.3f, -0.3f };
    }
    if (auto* v = (XrSpaceVelocity*)loc->next; v && v->type == XR_TYPE_SPACE_VELOCITY) {
        PoseEntry ve;
        bool left = action.rfind("left", 0) == 0;
        if (!action.empty() && PoseValue(left ? "vel_left" : "vel_right", ve)) {
            v->linearVelocity = { ve.v[0], ve.v[1], ve.v[2] };
            v->velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
        } else {
            v->linearVelocity = { 0, 0, 0 };
            v->velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
        }
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL EnumApiLayers(uint32_t, uint32_t* c, XrApiLayerProperties*) { *c = 0; return XR_SUCCESS; }

static XrResult XRAPI_CALL GetProc(XrInstance, const char* name, PFN_xrVoidFunction* fn)
{
    static const struct { const char* n; PFN_xrVoidFunction f; } table[] = {
#define E(n, f) { n, (PFN_xrVoidFunction)f }
        E("xrGetInstanceProcAddr", GetProc), E("xrEnumerateInstanceExtensionProperties", EnumInstExt), E("xrEnumerateApiLayerProperties", EnumApiLayers),
        E("xrCreateInstance", CreateInstance), E("xrDestroyInstance", DestroyInstance), E("xrGetInstanceProperties", GetInstanceProperties),
        E("xrGetSystem", GetSystem), E("xrGetD3D11GraphicsRequirementsKHR", GetD3D11Req), E("xrPollEvent", PollEvent), E("xrStringToPath", StringToPath),
        E("xrCreateSession", CreateSession), E("xrDestroySession", DestroySession), E("xrBeginSession", BeginSession), E("xrEndSession", EndSession),
        E("xrCreateReferenceSpace", CreateRefSpace), E("xrDestroySpace", DestroySpace), E("xrEnumerateSwapchainFormats", EnumFormats),
        E("xrCreateSwapchain", CreateSwapchain), E("xrDestroySwapchain", DestroySwapchain), E("xrEnumerateSwapchainImages", EnumImages),
        E("xrAcquireSwapchainImage", AcquireImage), E("xrWaitSwapchainImage", WaitImage), E("xrReleaseSwapchainImage", ReleaseImage),
        E("xrWaitFrame", WaitFrame), E("xrBeginFrame", BeginFrame), E("xrEndFrame", EndFrame), E("xrLocateViews", LocateViews),
        E("xrCreateActionSet", CreateActionSet), E("xrCreateAction", CreateAction), E("xrSuggestInteractionProfileBindings", Suggest),
        E("xrAttachSessionActionSets", Attach), E("xrSyncActions", Sync), E("xrGetActionStateFloat", GetFloat),
        E("xrGetActionStateBoolean", GetBool), E("xrGetActionStateVector2f", GetVec2), E("xrApplyHapticFeedback", Haptic),
        E("xrCreateActionSpace", CreateActionSpace), E("xrLocateSpace", LocateSpace),
#undef E
    };
    for (auto& e : table) if (!strcmp(e.n, name)) { *fn = e.f; return XR_SUCCESS; }
    L("unsupported: %s", name);
    *fn = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

extern "C" XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo*, XrNegotiateRuntimeRequest* req)
{
    req->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    req->runtimeApiVersion = XR_CURRENT_API_VERSION;
    req->getInstanceProcAddr = GetProc;
    return XR_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID)
{
    if (r == DLL_PROCESS_ATTACH) {
        char p[MAX_PATH]; GetModuleFileNameA(h, p, MAX_PATH);
        g_dir = p; g_dir = g_dir.substr(0, g_dir.find_last_of('\\') + 1);
    }
    return TRUE;
}

