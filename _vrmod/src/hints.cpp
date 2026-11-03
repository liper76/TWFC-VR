// Button hints. The PC game only ever writes keyboard keys into its hints: the localized templates hold gamepad
// tokens ("Press {a} to Jump"), the script turns them into keys ("Press <font color='#FF9333'>Space</font> to Jump").
// Every engine string handed to Scaleform becomes a Scaleform string in one place (GASStringManager::CreateStringW,
// reached from GFxValue -> GASValue for SetVariable, Invoke, SetMember...): there the keys are replaced by the VR
// controller buttons that do the same thing.
#include "common.h"
#include "MinHook.h"
#include <string>
#include <set>
#include <mutex>
#include <regex>

namespace {

// GASStringManager::CreateStringNode(const char* utf8, size_t len): thiscall, ret 8; copies the text. Every string
// Scaleform makes from engine text ends here (CreateString 0x110B9600, CreateStringW 0x110B9630 from
// Value2ASValue 0x1103A2E0, ...).
const uintptr_t kCreateStringNode = 0x110B9520;
typedef void*(__fastcall* CreateStringNodeFn)(void* mgr, void* edx, const char* s, size_t len);
CreateStringNodeFn g_origCreateStringNode;
struct Key { const wchar_t* key; const wchar_t* button; const wchar_t* buttonGripLayout; };
// keyboard key (as the game prints it) -> VR controller button; the grip layout (GripAbilities=1) differs for the
// abilities and the weapon switch
const Key kKeys[] = {
    { L"Space", L"A", nullptr },           { L"SpaceBar", L"A", nullptr },
    { L"E", L"X", nullptr },               { L"R", L"X", nullptr },
    { L"F", L"L Stick", nullptr },
    { L"Shift", L"Tap B", L"R Grip" },     { L"LeftShift", L"Tap B", L"R Grip" },   { L"Left Shift", L"Tap B", L"R Grip" },
    { L"Ctrl", L"R Stick", L"L Grip" },    { L"Control", L"R Stick", L"L Grip" },
    { L"LeftControl", L"R Stick", L"L Grip" }, { L"Left Ctrl", L"R Stick", L"L Grip" },
    { L"G", L"Y", L"B" },
    { L"C", L"A", nullptr },               { L"V", L"Y", L"B" },
    { L"Q", L"Swing", nullptr },           { L"Middle Mouse", L"Swing", nullptr }, { L"MiddleMouseButton", L"Swing", nullptr },
    { L"Left Mouse", L"Trigger", nullptr },  { L"LeftMouseButton", L"Trigger", nullptr }, { L"Left Mouse Button", L"Trigger", nullptr },
    { L"LMB", L"Trigger", nullptr },
    { L"Right Mouse", L"L Trigger", nullptr }, { L"RightMouseButton", L"L Trigger", nullptr },
    { L"Right Mouse Button", L"L Trigger", nullptr }, { L"RMB", L"L Trigger", nullptr },
    { L"Mouse Wheel", L"Hold B", L"Y" },   { L"PageUp", L"Hold B", L"Y" },        { L"Page Up", L"Hold B", L"Y" },
    { L"Escape", L"Menu", nullptr },       { L"Esc", L"Menu", nullptr },
    { L"W", L"L Stick", nullptr },         { L"A", L"L Stick", nullptr },
    { L"S", L"L Stick", nullptr },         { L"D", L"L Stick", nullptr },
    // as the tutorials print them
    { L"W, A, S, D", L"L Stick", nullptr }, { L"W, S, A, D", L"L Stick", nullptr }, { L"Arrow Keys", L"L Stick", nullptr },
    { L"Mouse", L"R Controller", nullptr }, { L"Middle Mouse Button", L"Swing", nullptr },
    { L"Page Up/Page Down", L"Hold B", L"Y" },
};

const wchar_t* ButtonFor(const std::wstring& key)
{
    if (g_cfg.gripUse && !_wcsicmp(key.c_str(), L"E")) return L"Grip";   // the use key (R is the reload: X)
    for (const Key& k : kKeys)
        if (!_wcsicmp(k.key, key.c_str())) return g_cfg.gripAbilities && k.buttonGripLayout ? k.buttonGripLayout : k.button;
    return nullptr;
}

std::mutex g_logMutex;
std::set<std::wstring> g_unknown;
long g_rewrites;

void LogUnknown(const std::wstring& key)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_unknown.size() < 64 && g_unknown.insert(key).second) Log("hints: no VR button for key '%ls'", key.c_str());
}

// "<font color='...'>KEY</font>" (the highlighted key) and "Press KEY " / "Hold KEY " (plain hints)
bool Rewrite(const wchar_t* s, int len, std::wstring& out)
{
    out.clear();
    bool changed = false;
    int i = 0;
    while (i < len) {
        if (!wcsncmp(s + i, L"<font ", 6)) {
            const wchar_t* gt = wcschr(s + i, L'>');
            const wchar_t* close = gt ? wcsstr(gt, L"</font>") : nullptr;
            if (gt && close && close - gt - 1 <= 24) {
                std::wstring key(gt + 1, close);
                if (const wchar_t* b = ButtonFor(key)) {
                    out.append(s + i, gt + 1);
                    out += b;
                    i = (int)(close - s);
                    changed = true;
                    continue;
                }
                if (!key.empty()) LogUnknown(key);
            }
        }
        for (const wchar_t* verb : { L"Press ", L"Hold ", L"press ", L"hold " }) {
            size_t vl = wcslen(verb);
            if (wcsncmp(s + i, verb, vl) || (i > 0 && iswalpha(s[i - 1]))) continue;
            int k = i + (int)vl, e = k;
            while (e < len && s[e] != L' ' && s[e] != L'<' && e - k < 12) e++;
            if (e < len && s[e] == L' ' && e > k) {
                if (const wchar_t* b = ButtonFor(std::wstring(s + k, s + e))) {
                    out.append(s + i, s + k);
                    out += b;
                    i = e;
                    changed = true;
                    goto next;
                }
            }
        }
        out += s[i++];
    next:;
    }
    if (changed && out.find(L" or ") != std::wstring::npos) {
        // two keys that became one button: "R Grip or the R Grip", "L Stick (or L Stick or L Stick)"
        static const std::wregex orSame(L"(<font[^>]*>([^<]+)</font>)( or (?:use )?(?:the )?<font[^>]*>\\2</font>)+");
        static const std::wregex orSameParen(L"(<font[^>]*>([^<]+)</font>) \\(or <font[^>]*>\\2</font>(?: or <font[^>]*>\\2</font>)*\\)");
        out = std::regex_replace(std::regex_replace(out, orSameParen, L"$1"), orSame, L"$1");
    }
    return changed;
}

// a hint at all? (this runs for every string Scaleform makes)
bool MaybeHint(const char* s, size_t len)
{
    if (len < 6 || len > 8192) return false;
    __try {
        for (size_t i = 0; i + 6 <= len; i++) {
            char c = s[i];
            if (c == '<' && i + 7 <= len && !memcmp(s + i, "</font>", 7)) return true;
            if ((c == 'P' || c == 'p') && !memcmp(s + i + 1, "ress ", 5)) return true;
            if ((c == 'H' || c == 'h') && !memcmp(s + i + 1, "old ", 4)) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

void* __fastcall HookCreateStringNode(void* mgr, void* edx, const char* s, size_t len)
{
    if (s && g_cfg.vrControllers && g_cfg.buttonHints && MaybeHint(s, len)) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s, (int)len, nullptr, 0);
        if (n > 0) {
            std::wstring w(n, L'\0'), r;
            MultiByteToWideChar(CP_UTF8, 0, s, (int)len, &w[0], n);
            if (Rewrite(w.c_str(), n, r)) {
                if (g_rewrites++ < 20 && g_cfg.logFps) Log("hints: '%ls'", r.c_str());
                int m = WideCharToMultiByte(CP_UTF8, 0, r.c_str(), (int)r.size(), nullptr, 0, nullptr, nullptr);
                std::string u(m, '\0');
                WideCharToMultiByte(CP_UTF8, 0, r.c_str(), (int)r.size(), &u[0], m, nullptr, nullptr);
                return g_origCreateStringNode(mgr, edx, u.c_str(), u.size());
            }
        }
    }
    return g_origCreateStringNode(mgr, edx, s, len);
}

}   // namespace

void HintsInstall()
{
    bool ok = MH_CreateHook((void*)kCreateStringNode, (void*)HookCreateStringNode, (void**)&g_origCreateStringNode) == MH_OK &&
              MH_EnableHook((void*)kCreateStringNode) == MH_OK;
    Log("hints: Scaleform string hook %s", ok ? "installed" : "FAILED");
}