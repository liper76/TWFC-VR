// The game's Scaleform movies driven from the mod: ActionScript variables read and written by path, e.g.
// "_root.hud_mc.topLeft_mc._visible" in the HUD movie (Hud_GFX). The game advances its movies on the game thread, so
// the requests are queued and run there each frame (GfxTick).
#include "common.h"
#include <string>
#include <vector>
#include <mutex>
#include <stdlib.h>

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
