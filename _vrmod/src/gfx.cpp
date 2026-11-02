// The game's Scaleform movies driven from the mod: ActionScript variables read and written by path, e.g.
// "_root.hud_mc.topLeft_mc._visible" in the HUD movie (Hud_GFX). The game advances its movies on the game thread, so
// the requests are queued and run there each frame (GfxTick).
#include "common.h"
#include "MinHook.h"
#include <string>
#include <vector>
#include <mutex>
#include <stdlib.h>
#include <algorithm>

extern volatile long g_traceFrames;   // d3dtrace.cpp

namespace {

const uintptr_t kGNames = 0x12B2F0F4;
const uintptr_t kGFxEngine = 0x12B000B4;   // GGFxEngine: +0x40 TArray<FGFxMovie*> (the movies playing)
const int kEngineMovies = 0x40;
const int kMovieView = 0x34;               // FGFxMovie: GFxMovieView*
const int kMovieObject = 0x50;             // FGFxMovie: its UObject (name +4: Hud_GFX, GFxMovie, ...)
const int kVtSetVariable = 0x2C;           // GFxMovieView vtable: SetVariable(path, const GFxValue&, setType) ret 0xC
const int kVtGetVariable = 0x38;           // GetVariable(GFxValue* out, path) ret 8
const int kVtInvoke = 0x60;                // Invoke(method path, GFxValue* result, const GFxValue* args, n) ret 0x10

// GFxValue: type at +0 (0 undefined, 1 null, 2 bool, 3 number, 4 string, ...), the value at +8
struct GFxValue {
    unsigned type;
    unsigned pad;
    union { double n; bool b; const char* s; const wchar_t* w; void* p; };
    uint8_t rest[0x30];
};
typedef bool(__fastcall* SetVariableFn)(void* view, void* edx, const char* path, const GFxValue* v, int setType);
typedef bool(__fastcall* GetVariableFn)(void* view, void* edx, GFxValue* out, const char* path);
// an Invoke argument: the array stride is 0x20
struct GFxArg {
    unsigned type;
    unsigned pad;
    union { double n; bool b; const char* s; };
    uint8_t rest[0x10];
};
typedef bool(__fastcall* InvokeFn)(void* view, void* edx, const char* method, GFxValue* result, const GFxArg* args, unsigned n);

std::mutex g_lock;
std::vector<std::string> g_queue;

const wchar_t* NameOf(unsigned fname)
{
    uint8_t** names = *(uint8_t***)kGNames;
    int count = *(int*)(kGNames + 4);
    int i = (int)(fname >> 14);
    if (!names || i <= 0 || i >= count || !names[i]) return nullptr;
    return (const wchar_t*)(names[i] + 8);
}

// The view of the movie whose object is named so (no number suffix), or null.
void* FindView(const wchar_t* name)
{
    uint8_t* eng = *(uint8_t**)kGFxEngine;
    if (!eng) return nullptr;
    uint8_t** movies = *(uint8_t***)(eng + kEngineMovies);
    int n = *(int*)(eng + kEngineMovies + 4);
    for (int i = 0; movies && i < n && i < 64; i++) {
        uint8_t* obj = movies[i] ? *(uint8_t**)(movies[i] + kMovieObject) : nullptr;
        const wchar_t* on = obj ? NameOf(*(unsigned*)(obj + 4)) : nullptr;
        if (on && !_wcsicmp(on, name)) return *(void**)(movies[i] + kMovieView);
    }
    return nullptr;
}

// The name of the movie with this view (debug)
const wchar_t* ViewName(void* view)
{
    uint8_t* eng = *(uint8_t**)kGFxEngine;
    if (!eng) return L"?";
    uint8_t** movies = *(uint8_t***)(eng + kEngineMovies);
    int n = *(int*)(eng + kEngineMovies + 4);
    for (int i = 0; movies && i < n && i < 64; i++) {
        if (!movies[i] || *(void**)(movies[i] + kMovieView) != view) continue;
        uint8_t* obj = *(uint8_t**)(movies[i] + kMovieObject);
        const wchar_t* on = obj ? NameOf(*(unsigned*)(obj + 4)) : nullptr;
        return on ? on : L"?";
    }
    return L"(not listed)";
}

bool CallGet(void* view, const char* path, GFxValue* out)
{
    __try {
        auto fn = (GetVariableFn)(*(uintptr_t**)view)[kVtGetVariable / 4];
        return fn(view, nullptr, out, path);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallSet(void* view, const char* path, const GFxValue* v)
{
    __try {
        auto fn = (SetVariableFn)(*(uintptr_t**)view)[kVtSetVariable / 4];
        return fn(view, nullptr, path, v, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallInvoke(void* view, const char* method, GFxValue* result, const GFxArg* args, unsigned n)
{
    __try {
        auto fn = (InvokeFn)(*(uintptr_t**)view)[kVtInvoke / 4];
        return fn(view, nullptr, method, result, args, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// true/false -> bool, a number -> number, else text
void ParseValue(const char* text, unsigned* type, double* n, bool* b, const char** s)
{
    char* end = nullptr;
    double d = strtod(text, &end);
    if (!_stricmp(text, "true") || !_stricmp(text, "false")) { *type = 2; *b = !_stricmp(text, "true"); }
    else if (*text && end && !*end) { *type = 3; *n = d; }
    else { *type = 4; *s = text; }
}

void Describe(const GFxValue& v, char* out, size_t n)
{
    __try {
        switch (v.type & 0xF) {
        case 0: _snprintf_s(out, n, _TRUNCATE, "undefined"); break;
        case 1: _snprintf_s(out, n, _TRUNCATE, "null"); break;
        case 2: _snprintf_s(out, n, _TRUNCATE, "bool %s", v.b ? "true" : "false"); break;
        case 3: _snprintf_s(out, n, _TRUNCATE, "number %g", v.n); break;
        case 4: _snprintf_s(out, n, _TRUNCATE, "string \"%.200s\"", v.s ? v.s : ""); break;
        case 5: _snprintf_s(out, n, _TRUNCATE, "wstring \"%.200ls\"", v.w ? v.w : L""); break;
        default: _snprintf_s(out, n, _TRUNCATE, "type %u (%p)", v.type, v.p); break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        _snprintf_s(out, n, _TRUNCATE, "type %u (unreadable)", v.type);
    }
}

// The clip at this depth of a clip (its path, "_root." for "_level0.") or "".
std::string ClipAtDepth(void* view, const std::string& parent, int depth)
{
    GFxArg a = {};
    a.type = 3;
    a.n = depth;
    GFxValue r = {};
    if (!CallInvoke(view, (parent + ".getInstanceAtDepth").c_str(), &r, &a, 1) || (r.type & 0xF) != 4 || !r.s) return "";
    std::string p = r.s;
    if (!strncmp(p.c_str(), "_level0.", 8)) p = "_root." + p.substr(8);
    return p;
}

// Where a clip puts the clips it attaches: from 0 up to getNextHighestDepth (-1: unknown).
int NextHighestDepth(void* view, const std::string& clip)
{
    GFxValue r = {};
    if (!CallInvoke(view, (clip + ".getNextHighestDepth").c_str(), &r, nullptr, 0) || (r.type & 0xF) != 3) return -1;
    return (int)r.n;
}

// debug "gfxkids [movie:]clip [n]": the clips in a clip - at its timeline's depths 1..n (default 64) and at the ones
// attached at run time - with _visible, _x, _y, _alpha
void LogKids(void* view, const char* path, int timeline)
{
    int next = NextHighestDepth(view, path);
    Log("gfx: clips in %s (next highest depth %d):", path, next);
    auto one = [&](int depth, int shown) {
        std::string p = ClipAtDepth(view, path, depth);
        if (p.empty()) return;
        char vis[64], x[64], y[64], alpha[64];
        GFxValue v = {};
        CallGet(view, (p + "._visible").c_str(), &v); Describe(v, vis, sizeof(vis)); v = {};
        CallGet(view, (p + "._x").c_str(), &v); Describe(v, x, sizeof(x)); v = {};
        CallGet(view, (p + "._y").c_str(), &v); Describe(v, y, sizeof(y)); v = {};
        CallGet(view, (p + "._alpha").c_str(), &v); Describe(v, alpha, sizeof(alpha));
        Log("gfx:   depth %d: %s  visible %s, x %s, y %s, alpha %s", shown, p.c_str(), vis, x, y, alpha);
    };
    for (int d = 1; d <= timeline; d++) one(d - 16384, d);
    for (int d = 0; d < next && d < 2000; d++) one(d, d);
    for (int d = std::max(2000, next - 200); d < next; d++) one(d, d);
}

// "gfxget [movie:]path" / "gfxset [movie:]path value" (value: true/false, a number or text) / "gfxcall [movie:]method
// [args...]" (space separated); the movie defaults to the HUD
void Run(const std::string& line)
{
    char cmd[16] = {}, target[256] = {};
    int used = 0;
    if (sscanf_s(line.c_str(), "%15s %255s %n", cmd, (unsigned)sizeof(cmd), target, (unsigned)sizeof(target), &used) < 2) return;
    const char* value = line.c_str() + used;
    wchar_t movie[64] = L"Hud_GFX";
    const char* path = target;
    if (const char* c = strchr(target, ':')) {
        swprintf_s(movie, L"%.*hs", (int)(c - target), target);
        path = c + 1;
    }
    void* view = FindView(movie);
    if (!view) { Log("gfx: no movie %ls", movie); return; }
    GFxValue v = {};
    char desc[300];
    if (!_stricmp(cmd, "gfxget")) {
        bool ok = CallGet(view, path, &v);
        Describe(v, desc, sizeof(desc));
        Log("gfx: %ls %s = %s%s", movie, path, desc, ok ? "" : " (failed)");
    } else if (!_stricmp(cmd, "gfxset")) {
        ParseValue(value, &v.type, &v.n, &v.b, &v.s);
        bool ok = CallSet(view, path, &v);
        Describe(v, desc, sizeof(desc));
        Log("gfx: %ls %s := %s%s", movie, path, desc, ok ? "" : " (failed)");
    } else if (!_stricmp(cmd, "gfxcall")) {
        std::vector<std::string> words;
        char buf[256];
        for (const char* p = value; sscanf_s(p, "%255s %n", buf, (unsigned)sizeof(buf), &used) == 1; p += used)
            words.push_back(buf);
        GFxArg args[8] = {};
        unsigned n = 0;
        for (; n < words.size() && n < 8; n++) ParseValue(words[n].c_str(), &args[n].type, &args[n].n, &args[n].b, &args[n].s);
        bool ok = CallInvoke(view, path, &v, args, n);
        Describe(v, desc, sizeof(desc));
        Log("gfx: %ls %s(%u args) -> %s%s", movie, path, n, desc, ok ? "" : " (failed)");
    } else if (!_stricmp(cmd, "gfxkids")) {
        LogKids(view, path, atoi(value) > 0 ? atoi(value) : 64);
    }
}

// debug "gfxprobe": for 2 s every GFxMovieView method call is counted (per vtable entry: calls, the last view and
// thread), to find which ones the game calls each frame (Advance, Display)
const int kProbeEntries = 48;
volatile long g_probeCount[kProbeEntries];
void* volatile g_probeThis[kProbeEntries];
volatile DWORD g_probeTid[kProbeEntries];
uintptr_t g_probeOrig[kProbeEntries];

DWORD WINAPI ProbeThread(LPVOID)
{
    void* view = FindView(L"Hud_GFX");
    if (!view) { Log("gfx: probe: no HUD movie"); return 0; }
    uintptr_t* vt = *(uintptr_t**)view;
    uint8_t* code = (uint8_t*)VirtualAlloc(nullptr, kProbeEntries * 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code) return 0;
    for (int i = 0; i < kProbeEntries; i++) {
        g_probeOrig[i] = vt[i];
        g_probeCount[i] = 0;
        uint8_t* c = code + i * 32;
        auto put32 = [&](const void* p) { uint32_t v = (uint32_t)(uintptr_t)p; memcpy(c, &v, 4); c += 4; };
        *c++ = 0x50;                                              // push eax
        *c++ = 0x64; *c++ = 0xA1; put32((void*)0x24);              // mov eax, fs:[0x24] (thread id)
        *c++ = 0xA3; put32((void*)&g_probeTid[i]);                // mov [tid], eax
        *c++ = 0x58;                                              // pop eax
        *c++ = 0x89; *c++ = 0x0D; put32((void*)&g_probeThis[i]);  // mov [this], ecx
        *c++ = 0xF0; *c++ = 0xFF; *c++ = 0x05; put32((void*)&g_probeCount[i]);   // lock inc [count]
        *c++ = 0xFF; *c++ = 0x25; put32((void*)&g_probeOrig[i]);  // jmp [orig]
    }
    DWORD old;
    VirtualProtect(vt, kProbeEntries * 4, PAGE_READWRITE, &old);
    for (int i = 0; i < kProbeEntries; i++) vt[i] = (uintptr_t)(code + i * 32);
    Sleep(2000);
    for (int i = 0; i < kProbeEntries; i++) vt[i] = g_probeOrig[i];
    VirtualProtect(vt, kProbeEntries * 4, old, &old);
    Log("gfx: probe of view %p (vtable %p), game thread %lu:", view, vt, GameThreadId());
    for (int i = 0; i < kProbeEntries; i++)
        if (g_probeCount[i])
            Log("gfx:   vt+%02x %08x: %ld calls in 2 s, last view %p thread %lu", i * 4, (unsigned)g_probeOrig[i],
                g_probeCount[i], g_probeThis[i], g_probeTid[i]);
    return 0;   // the thunks stay: a call may still be passing through one
}

// ---- the HUD movie drawn in layers ----
// While the HUD is split off (d3dtrace.cpp) the HUD movie is drawn up to three times, each time with only the parts
// of that layer visible (their _visible set before and given back after; nothing of the game runs in between):
// - kHudEyes: the markers (waypoints, names, lock-on marks) - drawn by the game for each eye's view, so they sit at
//   their place in depth in the eye images;
// - kHudPanel: the rest but the crosshair - the floating panel; Minimal: only the top corners (health, abilities,
//   weapon, ammo, grenades) and, with Prompts, the button hints below the crosshair;
// - kHudReticle: the crosshair and its hit marks - the VR reticle (ReticleStyle=1).
// The parts are found by name or, unnamed (instanceN is numbered at run time), by their depth in the timeline.
// The clips the crosshair's group attaches at run time (hit marks "causeDamageN_mc", the turret's and the range
// finder's reticles) are the crosshair's: hidden from the panel.
// Other movies (the mouse cursor) go on the panel. Off the game thread (threaded rendering) the HUD movie goes on
// the panel whole.
const uintptr_t kDisplay = 0x110432C0;   // GFxMovieView::Display (vtable +0x94), shared by the movies' views
typedef void(__fastcall* DisplayFn)(void* view, void* edx);
DisplayFn g_origDisplay;

enum {
    cMarkers, cIncap, cHud,
    cZoom, cTopLeft, cBottomLeft, cTopRight, cBottomRight, cMidCenter, cTopCenter, cMidLeft, cMidRight, cBottomCenter,
    cCrossGroup, cMidCenter2, cMidCenter3,
    cIncDamage, cHint1, cHint2, cHint3, cFeedback, cAmmoWarn, cLeftOfCross, cCross, cAboveCross,
    kClips
};
const struct { int parent; const char* name; int depth; } kClipDef[kClips] = {
    { -1, nullptr, 1 },                     // the marker manager
    { -1, "incapacitated_mc", 0 },
    { -1, "hud_mc", 0 },
    { cHud, "zoomScopeAttachPoint_mc", 0 },  // the scope overlay of zoomed aiming
    { cHud, "topLeft_mc", 0 },               // health, ability
    { cHud, "bottomLeft_mc", 0 },
    { cHud, "topRight_mc", 0 },              // weapon, ammo, ability, grenades
    { cHud, "bottomRight_mc", 0 },
    { cHud, "midCenter_mc", 0 },
    { cHud, "topCenter_mc", 0 },             // objective time, announcements
    { cHud, "midLeft_mc", 0 },
    { cHud, "midRight_mc", 0 },
    { cHud, "bottomCenter_mc", 0 },          // notifications
    { cMidCenter, nullptr, 1 },              // the crosshair's group: the crosshair and the anchors around it
    { cMidCenter, nullptr, 19 },
    { cMidCenter, nullptr, 21 },
    { cCrossGroup, "increaseDamageAnchor_mc", 0 },
    { cCrossGroup, nullptr, 3 },             // anchors below the crosshair (y 140, 150, 100): button hints
    { cCrossGroup, nullptr, 5 },
    { cCrossGroup, nullptr, 7 },
    { cCrossGroup, "weaponFeedback_mc", 0 }, // hit marks
    { cCrossGroup, nullptr, 11 },            // right of the crosshair: "LOW AMMO", "X RELOAD"
    { cCrossGroup, nullptr, 13 },            // left of it
    { cCrossGroup, "crosshairAnchor_mc", 0 },
    { cCrossGroup, nullptr, 17 },            // above it
};
std::string g_visPath[kClips];   // "<clip>._visible"
std::string g_crossGroup;        // the crosshair's group
void* g_clipsView;               // the HUD view they were found in
DWORD g_clipsFailedAt;

bool FindClips(void* view)
{
    if (view == g_clipsView) return true;
    if (g_clipsFailedAt && GetTickCount() - g_clipsFailedAt < 2000) return false;
    std::string path[kClips];
    for (int i = 0; i < kClips; i++) {
        const std::string parent = kClipDef[i].parent < 0 ? "_root" : path[kClipDef[i].parent];
        if (kClipDef[i].name) {
            path[i] = parent + "." + kClipDef[i].name;
        } else {
            GFxArg a = {};
            a.type = 3;
            a.n = kClipDef[i].depth - 16384;   // (AS2 depth of a timeline object)
            GFxValue r = {};
            if (CallInvoke(view, (parent + ".getInstanceAtDepth").c_str(), &r, &a, 1) && (r.type & 0xF) == 4 && r.s)
                path[i] = r.s;
            if (!strncmp(path[i].c_str(), "_level0.", 8)) path[i] = "_root." + path[i].substr(8);
        }
        GFxValue v = {};
        if (path[i].empty() || !CallGet(view, (path[i] + "._visible").c_str(), &v) || (v.type & 0xF) != 2) {
            if (!g_clipsFailedAt) Log("gfx: HUD part %d (%s) not found: no layers", i, path[i].c_str());
            g_clipsFailedAt = GetTickCount() | 1;
            return false;
        }
    }
    for (int i = 0; i < kClips; i++) g_visPath[i] = path[i] + "._visible";
    g_crossGroup = path[cCrossGroup];
    g_clipsView = view;
    g_clipsFailedAt = 0;
    Log("gfx: HUD layers: markers %s, crosshair %s", path[cMarkers].c_str(), path[cCross].c_str());
    return true;
}

void __fastcall HookDisplay(void* view, void* edx)
{
    if (!HudSplitActive()) { g_origDisplay(view, edx); return; }
    void* hud = FindView(L"Hud_GFX");
    if (view != hud || GetCurrentThreadId() != GameThreadId() || !FindClips(view)) {
        if (g_traceFrames > 0)
            Log("trace: Display of %ls view %p (HUD %p), thread %lu", ViewName(view), view, hud, GetCurrentThreadId());
        HudSetLayer(kHudPanel, true);
        g_origDisplay(view, edx);
        D3D9Flush();
        HudSetLayer(kHudEyes);
        return;
    }
    bool orig[kClips], cur[kClips];
    for (int i = 0; i < kClips; i++) {
        GFxValue v = {};
        if (!CallGet(view, g_visPath[i].c_str(), &v) || (v.type & 0xF) != 2) {   // (the movie changed)
            g_clipsView = nullptr;
            HudSetLayer(kHudPanel, true);
            g_origDisplay(view, edx);
            D3D9Flush();
            HudSetLayer(kHudEyes);
            return;
        }
        orig[i] = cur[i] = v.b;
    }
    std::vector<std::string> attached;   // "<clip>._visible" of the crosshair group's visible clips attached at run time
    int next = NextHighestDepth(view, g_crossGroup);
    for (int d = std::max(0, next - 32); d < next; d++) {   // (they remove themselves: a few at most)
        std::string p = ClipAtDepth(view, g_crossGroup, d);
        GFxValue v = {};
        if (!p.empty() && CallGet(view, (p + "._visible").c_str(), &v) && (v.type & 0xF) == 2 && v.b)
            attached.push_back(p + "._visible");
    }
    auto showAttached = [&](bool on) {
        for (auto& p : attached) {
            GFxValue v = {};
            v.type = 2;
            v.b = on;
            CallSet(view, p.c_str(), &v);
        }
    };
    auto setVisible = [&](int i, bool on) {
        if (cur[i] == on) return;
        GFxValue v = {};
        v.type = 2;
        v.b = on;
        CallSet(view, g_visPath[i].c_str(), &v);
        cur[i] = on;
    };
    // draws the movie into a layer with these parts hidden (the others as the game has them)
    auto pass = [&](int layer, const std::vector<int>& hidden) {
        bool hide[kClips] = {};
        for (int i : hidden) hide[i] = true;
        for (int i = 0; i < kClips; i++) setVisible(i, orig[i] && !hide[i]);
        HudSetLayer(layer, true);
        g_origDisplay(view, edx);
        D3D9Flush();   // (dgVoodoo holds the last draw back until the next one: let it land in this layer)
    };
    bool player = CameraPlayerView();
    bool minimal = g_cfg.hudMinimal && player;
    bool eyes = g_cfg.hudMarkersInEyes && player;
    if (eyes) pass(kHudEyes, { cIncap, cHud });
    std::vector<int> panelHidden = { cZoom, cIncDamage, cFeedback, cCross };
    if (eyes || minimal) panelHidden.push_back(cMarkers);
    if (minimal)
        for (int i : { cIncap, cBottomLeft, cBottomRight, cTopCenter, cMidLeft, cMidRight, cBottomCenter, cMidCenter2, cMidCenter3,
                       cAmmoWarn, cLeftOfCross, cAboveCross })
            panelHidden.push_back(i);
    if (minimal && !g_cfg.hudPrompts)
        for (int i : { cHint1, cHint2, cHint3 }) panelHidden.push_back(i);
    showAttached(false);
    pass(kHudPanel, panelHidden);
    showAttached(true);
    if (g_cfg.reticleStyle == 1)
        pass(kHudReticle, { cMarkers, cIncap, cZoom, cTopLeft, cBottomLeft, cTopRight, cBottomRight, cTopCenter, cMidLeft, cMidRight,
                            cBottomCenter, cMidCenter2, cMidCenter3, cHint1, cHint2, cHint3, cAmmoWarn, cLeftOfCross, cAboveCross });
    for (int i = 0; i < kClips; i++) setVisible(i, orig[i]);
    HudSetLayer(kHudEyes);
}

}  // namespace

void GfxProbe() { CloseHandle(CreateThread(nullptr, 0, ProbeThread, nullptr, 0, nullptr)); }

void GfxQueue(const char* line)
{
    std::lock_guard<std::mutex> g(g_lock);
    g_queue.push_back(line);
}

void GfxTick()
{
    std::vector<std::string> q;
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (g_queue.empty()) return;
        q.swap(g_queue);
    }
    for (auto& l : q) Run(l);
}

void GfxInstall()
{
    bool ok = MH_CreateHook((void*)kDisplay, (void*)HookDisplay, (void**)&g_origDisplay) == MH_OK &&
              MH_EnableHook((void*)kDisplay) == MH_OK;
    Log("gfx: HUD movie layers %s", ok ? "installed" : "FAILED");
}
