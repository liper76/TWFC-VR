// The player's weapons for the VR weapon wheel, and the melee weapon kept in a hand.
//
// Weapons: Pawn.InvManager's inventory chain. The wheel lists the ones the game's "next weapon" button cycles
// through; choosing one presses that button until it is the weapon in hand (works whatever order the game uses).
//
// Melee weapon: the game has no "hold the melee weapon" state - a melee attack attaches the robot's melee meshes
// (hammer, blade... and arm parts; they differ per robot) to the skeleton and detaches them when the attack ends.
// To keep one in the hand, the mod starts a melee attack, notes which skeletal meshes got attached for it, and
// refuses their detach (USkeletalMeshComponent::DetachComponent) until the weapon is put away; then it detaches
// them itself. Each swing is a normal melee attack of the game.
#include "common.h"
#include "MinHook.h"

namespace {

const uintptr_t kMeshDetach = 0x1163B730;   // USkeletalMeshComponent::DetachComponent: eax = mesh, edi = component
const uintptr_t kMeshAttach = 0x1163EF00;   // USkeletalMeshComponent::AttachComponent: __fastcall(mesh, comp, 4 more)
const uintptr_t kGNames = 0x12B2F0F4;
const int kPawnMesh = 0x3F4;
const int kPawnInvManager = 0x424;
const int kPawnWeapon = 0x428;
const int kInvChain = 0x1C8;          // InventoryManager.InventoryChain / Inventory.Inventory (next)
const int kWeaponGroup = 0x2A0;       // seen: 0 / 3 hand weapons, 1 vehicle weapon, 50 melee weapon (Megatron)
const int kMeshAttachments = 0x260;   // TArray, 0x30 per entry: +0 component, +4 bone FName
const int kCompScale = 0x178;
const int kCompFlags = 0xF0;          // PrimitiveComponent bit flags
const uint32_t kHiddenGame = 0x4;

const wchar_t* NameOf(unsigned fname)
{
    uint8_t** names = *(uint8_t***)kGNames;
    int count = *(int*)(kGNames + 4);
    int i = (int)(fname >> 14);
    if (!names || i <= 0 || i >= count || !names[i]) return nullptr;
    return (const wchar_t*)(names[i] + 8);
}

// "TnWeaponAssaultRifle" -> "Assault Rifle"
void DisplayName(const wchar_t* cls, wchar_t* out, size_t n)
{
    const wchar_t* s = cls;
    if (!wcsncmp(s, L"TnWeapon", 8)) s += 8;
    size_t k = 0;
    for (size_t i = 0; s[i] && k + 2 < n; i++) {
        if (i && iswupper(s[i]) && !iswupper(s[i - 1])) out[k++] = L' ';
        out[k++] = s[i];
    }
    out[k] = 0;
}

std::mutex g_listMutex;
WeaponList g_list;
void* g_listWeapon[8];               // game objects behind g_list (game thread)
void* g_handWeapons[16];             // weapons seen in hand in robot form (the "next weapon" cycle)
void* g_vehicleWeapons[16];          // weapons seen in hand in vehicle form
void* g_unreachable[16];             // weapons the "next weapon" button did not reach
void* g_switchStart;                 // weapon in hand when the switch began
void* g_pawn;
uint8_t* g_mesh;

// weapon switching
void* volatile g_want;               // weapon to switch to (game thread)
int g_tries;
DWORD g_pressAt;
void* g_pressFrom;
volatile long g_pressNext, g_pressMelee;   // button presses wanted by the game thread (read by VR input)

// melee weapon in a hand
volatile long g_meleeHand = -1;      // asked for
volatile long g_meleeHeld = -1;      // actually held
enum { kIdle, kEquipping, kHolding } g_meleeState;
DWORD g_meleeStart, g_meleeFirst;
int g_meleeTry;
uint8_t* g_kept[8];                  // the melee meshes kept attached
unsigned g_keptBone[8];              // their original bones
int g_keptCount;
struct Seen { uint8_t* comp; DWORD since; } g_seen[32];
int g_seenCount;
volatile bool g_keepFlag;            // HookDetach scratch (game thread only)
void* g_origDetach;

bool g_keptAnyWeaponBone;            // one of them hangs on a weapon bone (that one is the weapon)

bool IsWeaponBone(unsigned bone)
{
    const wchar_t* n = NameOf(bone);
    return n && (wcsstr(n, L"Wpn") || wcsstr(n, L"Weapon"));
}

bool Contains(void* const* arr, int n, void* v) { for (int i = 0; i < n; i++) if (arr[i] == v) return true; return false; }
void Remember(void** arr, void* v)
{
    if (Contains(arr, 16, v)) return;
    for (int i = 0; i < 16; i++) if (!arr[i]) { arr[i] = v; return; }
}

// Keeps only the weapons still in the inventory (or in hand). Before, the memories only grew: weapons of past lives,
// turrets and dropped guns filled them, a new weapon was no longer noted as held in hand and fell out of the wheel
// (and a freed weapon's address could come back as a new one with the old one's verdict).
void Prune(void** arr, void* const* live, int n)
{
    int k = 0;
    for (int i = 0; i < 16; i++)
        if (arr[i] && Contains(live, n, arr[i])) arr[k++] = arr[i];
    for (; k < 16; k++) arr[k] = nullptr;
}

// While held, the game may not detach the kept meshes (it does so from several places when an attack ends); the mod
// detaches them itself through the original function.
bool __stdcall KeepAttached(uint8_t* mesh, uint8_t* comp, uintptr_t ret)
{
    if (g_meleeState == kHolding && mesh == g_mesh && Contains((void* const*)g_kept, g_keptCount, comp)) {
        if (g_cfg.logFps) Log("weapons: detach of %p from %08X refused", comp, (unsigned)ret);
        return true;
    }
    // ik.cpp: the arm model on the empty right hand stays; the records of what is attached
    __try { return !IKOnDetach(mesh, comp); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(naked) void HookDetach()
{
    __asm {
        pushad
        push dword ptr [esp + 32]   // return address of this call
        push edi
        push eax
        call KeepAttached
        mov g_keepFlag, al
        popad
        cmp g_keepFlag, 0
        jne keep
        jmp g_origDetach
    keep:
        ret
    }
}

void CallDetach(uint8_t* mesh, uint8_t* comp)
{
    void* fn = g_origDetach;
    __asm {
        mov eax, mesh
        mov edi, comp
        call fn
    }
}

bool IsAttached(uint8_t* mesh, uint8_t* comp)
{
    uint8_t* att = *(uint8_t**)(mesh + kMeshAttachments);
    int n = *(int*)(mesh + kMeshAttachments + 4);
    for (int i = 0; att && i < n && i < 64; i++) if (*(uint8_t**)(att + i * 0x30) == comp) return true;
    return false;
}

// The next swing attaches the meshes again: they are still attached (kept), so that is skipped - attaching twice
// breaks the game.
typedef void(__fastcall* AttachFn)(uint8_t* mesh, uint8_t* comp, void* bone, void* loc, void* rot, void* scale);
AttachFn g_origAttach;

void __fastcall HookAttach(uint8_t* mesh, uint8_t* comp, void* bone, void* loc, void* rot, void* scale)
{
    // (the same for the arm model ik.cpp keeps on the empty right hand: a grenade throw attaches it)
    if ((g_meleeState == kHolding && mesh == g_mesh && Contains((void* const*)g_kept, g_keptCount, comp)) || IKArmModelHeld(mesh, comp)) {
        bool attached = false;
        __try { attached = IsAttached(mesh, comp); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (attached) return;
    }
    g_origAttach(mesh, comp, bone, loc, rot, scale);
    __try { IKOnAttach(mesh, comp); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void ReleaseMelee(bool detach)
{
    if (g_meleeState == kHolding && detach && g_mesh) {
        for (int k = 0; k < g_keptCount; k++) {
            __try {
                *(float*)(g_kept[k] + kCompScale) = 1.0f;
                for (int i = 0; i < 4 && IsAttached(g_mesh, g_kept[k]); i++) CallDetach(g_mesh, g_kept[k]);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        Log("weapons: melee weapon put away (%d meshes)", g_keptCount);
    }
    g_meleeState = kIdle;
    g_keptCount = 0;
    g_meleeHeld = -1;
}

// Skeletal meshes attached to the player's mesh, with how long each has been attached.
void TrackAttachments(uint8_t* mesh, DWORD now)
{
    Seen cur[32];
    int n = 0;
    uint8_t* att = *(uint8_t**)(mesh + kMeshAttachments);
    int count = *(int*)(mesh + kMeshAttachments + 4);
    for (int i = 0; att && i < count && i < 64 && n < 32; i++) {
        uint8_t* c = *(uint8_t**)(att + i * 0x30);
        if (!c || *(uintptr_t*)c != *(uintptr_t*)mesh) continue;   // skeletal meshes only (not particles, lights)
        DWORD since = now;
        for (int j = 0; j < g_seenCount; j++) if (g_seen[j].comp == c) since = g_seen[j].since;
        cur[n++] = { c, since };
    }
    memcpy(g_seen, cur, sizeof(cur));
    g_seenCount = n;
}

void UpdateMelee(uint8_t* mesh, bool vehicle, DWORD now)
{
    if (vehicle) g_meleeHand = -1;   // transforming puts it away (and does not take it out again by itself)
    int want = g_meleeHand;
    if (want < 0) {
        if (g_meleeState != kIdle) ReleaseMelee(true);
        return;
    }
    if (g_meleeState == kIdle) {
        // start a melee attack: its meshes get attached
        g_meleeState = kEquipping;
        g_meleeStart = now;
        g_meleeFirst = 0;
        g_meleeTry = 0;
        g_keptCount = 0;
        g_keptAnyWeaponBone = false;
        g_pressMelee = 1;
        return;
    }
    if (g_meleeState == kEquipping) {
        for (int i = 0; i < g_seenCount && g_keptCount < 8; i++) {
            const Seen& s = g_seen[i];
            if (s.since + 30 < g_meleeStart || Contains((void* const*)g_kept, g_keptCount, s.comp) || IKArmModelHeld(mesh, s.comp)) continue;
            g_kept[g_keptCount] = s.comp;
            g_keptBone[g_keptCount++] = 0;
            if (!g_meleeFirst) g_meleeFirst = now;
        }
        if (g_meleeFirst && now - g_meleeFirst > 150) {
            for (int k = 0; k < g_keptCount; k++) {
                uint8_t* m = *(uint8_t**)(g_kept[k] + 0x1BC);
                const wchar_t* nm = m ? NameOf(*(unsigned*)(m + 4)) : nullptr;
                Log("weapons: keeping melee mesh %p %ls", g_kept[k], nm ? nm : L"?");
            }
            g_meleeState = kHolding;
        } else if (!g_meleeFirst && now - g_meleeStart > 1500) {
            if (++g_meleeTry >= 2) {
                Log("weapons: no melee weapon appeared - giving up");
                g_meleeHand = -1;
                g_meleeState = kIdle;
                return;
            }
            g_meleeStart = now;
            g_pressMelee = 1;
        }
        return;
    }
    // holding: the meshes go to the chosen hand's side of the skeleton and take the weapon size
    uint8_t* att = *(uint8_t**)(mesh + kMeshAttachments);
    int count = *(int*)(mesh + kMeshAttachments + 4);
    for (int i = 0; att && i < count && i < 64; i++) {
        uint8_t* e = att + i * 0x30;
        uint8_t* c = *(uint8_t**)e;
        for (int k = 0; k < g_keptCount; k++) {
            if (c != g_kept[k]) continue;
            unsigned& bone = *(unsigned*)(e + 4);
            if (!g_keptBone[k]) {
                g_keptBone[k] = bone;
                if (IsWeaponBone(bone)) g_keptAnyWeaponBone = true;
            }
            unsigned target = g_keptBone[k];
            if (want == 0) { unsigned m = IKMirrorBone(target); if (m) target = m; }
            bone = target;
            // only the weapon itself is shown (the mesh on a weapon bone); the rest of the attack set - arm parts
            // that would cover the view right in front of the eyes - is hidden
            bool weapon = IsWeaponBone(g_keptBone[k]) || !g_keptAnyWeaponBone;
            // world size = WeaponScale, whatever the scale of the bone it hangs on (the hand may be resized)
            float* scale = (float*)(c + kCompScale);
            const float* l2w = (const float*)(c + 0x50);
            float world = sqrtf(l2w[0] * l2w[0] + l2w[1] * l2w[1] + l2w[2] * l2w[2]);
            float parent = *scale > 1e-4f ? world / *scale : 1.0f;
            *scale = weapon ? g_cfg.weaponScale / std::min(std::max(parent, 0.05f), 20.0f) : 0.001f;
            // after each attack the game hides the weapon (PrimitiveComponent HiddenGame, flags +0xF0 bit 2) and the
            // engine drops it from the scene: kept visible while held (the engine re-adds it by itself)
            if (weapon) *(uint32_t*)(c + kCompFlags) &= ~kHiddenGame;
        }
    }
    g_meleeHeld = want;
}

// A punch with the empty left hand is a normal melee attack of the game, which attaches its meshes to the right
// side of the skeleton: while that attack lasts, the meshes it attached go to the mirrored (left) bones.
volatile DWORD g_strikeAt;
volatile long g_strikeHand = -1;
uint8_t* g_flipped[8];
unsigned g_flippedBone[8];   // their original (right side) bones
int g_flipCount;

void MirrorStrike(uint8_t* mesh, DWORD now)
{
    if (g_meleeState != kIdle || g_strikeHand != 0 || now - g_strikeAt > 2000) { g_flipCount = 0; return; }
    uint8_t* att = *(uint8_t**)(mesh + kMeshAttachments);
    int count = *(int*)(mesh + kMeshAttachments + 4);
    for (int i = 0; att && i < count && i < 64; i++) {
        uint8_t* e = att + i * 0x30;
        uint8_t* c = *(uint8_t**)e;
        int k = 0;
        while (k < g_flipCount && g_flipped[k] != c) k++;
        if (k == g_flipCount) {
            // a skeletal mesh attached since the strike (the attack's melee set)
            bool fresh = false;
            for (int j = 0; j < g_seenCount; j++) if (g_seen[j].comp == c && g_seen[j].since + 30 >= g_strikeAt) fresh = true;
            if (!fresh || g_flipCount >= 8) continue;
            g_flipped[g_flipCount] = c;
            g_flippedBone[g_flipCount++] = *(unsigned*)(e + 4);
        }
        unsigned m = IKMirrorBone(g_flippedBone[k]);
        if (m) *(unsigned*)(e + 4) = m;
    }
}

// A melee attack makes the robot lunge at the target; in VR you swing where you stand. During the attack the
// robot moves no faster than walking (horizontally), so the lunge is gone but you can still walk.
bool g_prevValid;
float g_prevPos[2];
LARGE_INTEGER g_prevT;

void HoldInPlace(uint8_t* pawn, DWORD now)
{
    float* loc = (float*)(pawn + 0xD8);   // Actor.Location
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    if (g_strikeAt && now - g_strikeAt < 750 && g_prevValid) {
        float dt = fmaxf((float)(t.QuadPart - g_prevT.QuadPart) / f.QuadPart, 0.001f);
        float dx = loc[0] - g_prevPos[0], dy = loc[1] - g_prevPos[1];
        float d = sqrtf(dx * dx + dy * dy), maxD = 650.0f * dt;   // ~ running speed (Unreal units per second)
        if (d > maxD && d < 2000.0f) {   // (a jump of more than that is a teleport: leave it)
            loc[0] = g_prevPos[0] + dx * maxD / d;
            loc[1] = g_prevPos[1] + dy * maxD / d;
        }
    }
    g_prevPos[0] = loc[0];
    g_prevPos[1] = loc[1];
    g_prevT = t;
    g_prevValid = true;
}

void UpdateList(uint8_t* pawn, bool vehicle, DWORD now)
{
    void* cur = *(void**)(pawn + kPawnWeapon);
    uint8_t* inv = *(uint8_t**)(pawn + kPawnInvManager);
    void* live[17] = {};
    int liveCount = 0;
    for (uint8_t* w = inv ? *(uint8_t**)(inv + kInvChain) : nullptr; w && liveCount < 16; w = *(uint8_t**)(w + kInvChain))
        live[liveCount++] = w;
    int chainCount = liveCount;
    if (cur) live[liveCount++] = cur;
    if (chainCount) {   // (not while the inventory is away)
        Prune(g_handWeapons, live, liveCount);
        Prune(g_vehicleWeapons, live, liveCount);
        Prune(g_unreachable, live, liveCount);
    }
    if (cur) Remember(vehicle ? g_vehicleWeapons : g_handWeapons, cur);
    WeaponList l;
    void* ptrs[8] = {};
    if (cur && !vehicle) {   // (the setup of the gun in hand follows it even when the wheel leaves it out)
        const wchar_t* cls = NameOf(*(unsigned*)((uint8_t*)cur + 4));
        if (cls && !GrenadeWeaponClass(cls) && *(int*)((uint8_t*)cur + kWeaponGroup) != 50) DisplayName(cls, l.held, 40);
    }
    wchar_t left[256] = {};   // (for the log: the ones left out)
    for (int i = 0; i < chainCount && l.count < 8; i++) {
        uint8_t* w = (uint8_t*)live[i];
        bool hand = Contains(g_handWeapons, 16, w);
        int group = *(int*)(w + kWeaponGroup);
        const wchar_t* cls = NameOf(*(unsigned*)(w + 4));
        if (!cls) continue;
        bool guessVehicle = group == 1 || wcsstr(cls, L"Vehicle") || wcsstr(cls, L"Plane");
        // grenades are thrown with their own button, the "next weapon" button never reaches them
        bool grenade = GrenadeWeaponClass(cls);
        bool out = grenade || Contains(g_unreachable, 16, w) ||
                   (!hand && (Contains(g_vehicleWeapons, 16, w) || guessVehicle || group == 50));
        if (out) {
            if (!grenade) {
                wchar_t s[128];
                _snwprintf_s(s, _TRUNCATE, L"%ls%ls (%d%ls)", left[0] ? L", " : L"", cls, group,
                             Contains(g_unreachable, 16, w) ? L" unreachable" : Contains(g_vehicleWeapons, 16, w) ? L" vehicle" : L"");
                wcsncat_s(left, s, _TRUNCATE);
            }
            continue;
        }
        if (w == cur) l.current = l.count;
        DisplayName(cls, l.name[l.count], 40);
        ptrs[l.count++] = w;
    }
    memcpy(g_listWeapon, ptrs, sizeof(ptrs));
    // the list in the log when it changes
    static wchar_t shown[640];
    wchar_t text[640] = {};
    for (int i = 0; i < l.count; i++) {
        wcsncat_s(text, i ? L", " : L"", _TRUNCATE);
        wcsncat_s(text, l.name[i], _TRUNCATE);
    }
    if (left[0]) { wcsncat_s(text, L"; left out: ", _TRUNCATE); wcsncat_s(text, left, _TRUNCATE); }
    if (wcscmp(text, shown)) {
        wcscpy_s(shown, text);
        Log("weapons: the wheel lists %ls", text[0] ? text : L"nothing");
    }
    std::lock_guard<std::mutex> lock(g_listMutex);
    g_list = l;
}

void UpdateSwitch(uint8_t* pawn, DWORD now)
{
    void* want = g_want;
    if (!want) return;
    void* cur = *(void**)(pawn + kPawnWeapon);
    if (cur == want) { g_want = nullptr; return; }
    // back where we started (a whole round of "next weapon") or out of tries: that weapon cannot be reached this
    // way - stop, and leave it out of the wheel from now on
    if (g_tries <= 0 || (g_pressAt && cur == g_switchStart && cur != g_pressFrom)) {
        Remember(g_unreachable, want);
        Log("weapons: cannot switch to that weapon - left out of the wheel");
        g_want = nullptr;
        return;
    }
    if (!g_pressAt) g_switchStart = cur;
    // one press at a time: wait until the weapon has changed (or a second has passed)
    if (g_pressAt && cur == g_pressFrom && now - g_pressAt < 1200) return;
    if (g_pressAt && now - g_pressAt < 250) return;
    g_pressNext = 1;
    g_pressAt = now;
    g_pressFrom = cur;
    g_tries--;
}

// Turrets (mounted ones and the ones ripped off and carried: TnMountableTurret..., TnWeapon...Turret,
// TnWeaponMounted...): their firing effects are made for a camera far behind - in VR they go off right at the eyes.
// The particle systems of the turrets near the player are drawn at MuzzleFlashScale, like the hand guns' muzzle
// flashes (ik.cpp). The turrets are looked up twice a second; the sizes are set every frame (a flash attached anew
// for a shot comes at full size). Never made bigger (a hidden gun's flashes stay hidden).
const uintptr_t kGWorld = 0x12B10A9C;
const int kWorldLevels = 0x34, kLevelActors = 0x28, kActorLocation = 0xD8, kActorComponents = 0x50, kActorInstigator = 0x94;
const int kActorRotation = 0xE4;
const int kMountableLimits = 0x2B0;   // TnMountable: yaw min, max (from its own), pitch min, max - degrees
const int kWeaponMesh = 0x20C, kWeaponMesh2 = 0x208;
const float kTurretRange = 800.0f;
uint8_t* g_turrets[8];
int g_turretCount;
volatile bool g_onTurret;

bool IsTurret(const wchar_t* n)
{
    if (!wcsncmp(n, L"TnMountable", 11)) return true;
    return !wcsncmp(n, L"TnWeapon", 8) && (wcsstr(n, L"Turret") || !wcsncmp(n, L"TnWeaponMounted", 15));
}

int ShrinkAttachedFx(uint8_t* mesh, float s, int depth)
{
    const wchar_t* mn = mesh ? NameOf(*(unsigned*)(mesh + 4)) : nullptr;
    if (!mn || !wcsstr(mn, L"SkeletalMeshComponent")) return 0;
    uint8_t* att = *(uint8_t**)(mesh + kMeshAttachments);
    int n = *(int*)(mesh + kMeshAttachments + 4), k = 0;
    for (int i = 0; att && i < n && i < 32; i++) {
        uint8_t* e = att + i * 0x30;
        uint8_t* c = *(uint8_t**)e;
        const wchar_t* nm = c ? NameOf(*(unsigned*)(c + 4)) : nullptr;
        if (!nm) continue;
        if (wcsstr(nm, L"ParticleSystemComponent")) {
            float* sc = (float*)(e + 0x20);
            for (int a = 0; a < 3; a++) sc[a] = fminf(sc[a], s);
            k++;
        } else if (depth > 0) {
            k += ShrinkAttachedFx(c, s, depth - 1);
        }
    }
    return k;
}

int ShrinkTurretFx(uint8_t* a, float s)
{
    int k = 0;
    const wchar_t* n = NameOf(*(unsigned*)(a + 4));
    if (n && !wcsncmp(n, L"TnWeapon", 8)) {
        uint8_t* m1 = *(uint8_t**)(a + kWeaponMesh);
        uint8_t* m2 = *(uint8_t**)(a + kWeaponMesh2);
        k += ShrinkAttachedFx(m1, s, 1);
        if (m2 != m1) k += ShrinkAttachedFx(m2, s, 1);
    }
    uint8_t** comps = *(uint8_t***)(a + kActorComponents);
    int cn = *(int*)(a + kActorComponents + 4);
    for (int i = 0; comps && i < cn && i < 32; i++) {
        uint8_t* c = comps[i];
        const wchar_t* nm = c ? NameOf(*(unsigned*)(c + 4)) : nullptr;
        if (!nm) continue;
        if (wcsstr(nm, L"ParticleSystemComponent")) {
            float* sc = (float*)(c + kCompScale);
            *sc = fminf(*sc, s);
            k++;
        } else {
            k += ShrinkAttachedFx(c, s, 1);
        }
    }
    return k;
}

void UpdateTurrets(uint8_t* pawn, DWORD now)
{
    static DWORD findAt;
    if (now - findAt > 500) {
        findAt = now;
        const float* p = (const float*)(pawn + kActorLocation);
        uint8_t* found[8];
        int n = 0;
        uint8_t* world = *(uint8_t**)kGWorld;
        uint8_t** levels = world ? *(uint8_t***)(world + kWorldLevels) : nullptr;
        int levelCount = world ? *(int*)(world + kWorldLevels + 4) : 0;
        for (int l = 0; levels && l < levelCount && l < 64; l++) {
            uint8_t* level = levels[l];
            uint8_t** actors = level ? *(uint8_t***)(level + kLevelActors) : nullptr;
            int count = level ? *(int*)(level + kLevelActors + 4) : 0;
            for (int i = 0; actors && i < count && i < 20000 && n < 8; i++) {
                uint8_t* a = actors[i];
                const wchar_t* name = a ? NameOf(*(unsigned*)(a + 4)) : nullptr;
                if (!name || !IsTurret(name)) continue;
                if (!wcsncmp(name, L"TnWeapon", 8)) {   // the player's or a mounted turret's, not the squad's
                    uint8_t* by = *(uint8_t**)(a + kActorInstigator);
                    const wchar_t* bn = by ? NameOf(*(unsigned*)(by + 4)) : nullptr;
                    if (by != pawn && !(bn && !wcsncmp(bn, L"TnMountable", 11))) continue;
                }
                const float* q = (const float*)(a + kActorLocation);
                float dx = q[0] - p[0], dy = q[1] - p[1], dz = q[2] - p[2];
                if (dx * dx + dy * dy + dz * dz < kTurretRange * kTurretRange) found[n++] = a;
            }
        }
        bool changed = n != g_turretCount || memcmp(found, g_turrets, n * sizeof(found[0]));
        memcpy(g_turrets, found, n * sizeof(found[0]));
        g_turretCount = n;
        if (changed) {
            for (int i = 0; i < n; i++)
                Log("weapons: turret %ls nearby: %d particle systems at %.2f", NameOf(*(unsigned*)(found[i] + 4)),
                    ShrinkTurretFx(found[i], g_cfg.muzzleFlashScale), g_cfg.muzzleFlashScale);
            if (!n) Log("weapons: no turret nearby");
        }
    }
    for (int i = 0; i < g_turretCount; i++) ShrinkTurretFx(g_turrets[i], g_cfg.muzzleFlashScale);

    // on a mounted turret: the weapon in hand is a turret's and a mountable turret stands right where the player is
    // (a turret gun torn off and carried away is not on one)
    // (it flickered on and off every frame at the edge: on within 250 units, off only past 400 or with another
    // weapon, and only after 0.3 s of that)
    bool close = false;
    uint8_t* w = *(uint8_t**)(pawn + kPawnWeapon);
    const wchar_t* wn = w ? NameOf(*(unsigned*)(w + 4)) : nullptr;
    if (wn && IsTurret(wn)) {
        const float* p = (const float*)(pawn + kActorLocation);
        float r = g_onTurret ? 400.0f : 250.0f;
        for (int i = 0; i < g_turretCount && !close; i++) {
            const wchar_t* tn = NameOf(*(unsigned*)(g_turrets[i] + 4));
            if (!tn || wcsncmp(tn, L"TnMountable", 11)) continue;
            const float* q = (const float*)(g_turrets[i] + kActorLocation);
            float dx = q[0] - p[0], dy = q[1] - p[1];
            close = dx * dx + dy * dy < r * r;
        }
    }
    static DWORD awayAt;
    bool on = close;
    if (g_onTurret && !close) {
        if (!awayAt) awayAt = now;
        on = now - awayAt < 300;
    }
    if (close) awayAt = 0;
    if (on != g_onTurret) Log("weapons: %s a turret (%ls)", on ? "on" : "off", wn ? wn : L"-");
    g_onTurret = on;
}

}   // namespace

// For ik.cpp's arm model: attach/detach through the original functions (not the hooks), and what is attached.
void WeaponsAttachOriginal(void* mesh, void* comp, unsigned bone, const float* loc, const int* rot, const float* scale)
{
    AttachFn fn = g_origAttach ? g_origAttach : (AttachFn)kMeshAttach;
    fn((uint8_t*)mesh, (uint8_t*)comp, (void*)(uintptr_t)bone, (void*)loc, (void*)rot, (void*)scale);
}

void WeaponsDetachOriginal(void* mesh, void* comp)
{
    if (g_origDetach) CallDetach((uint8_t*)mesh, (uint8_t*)comp);
}

bool WeaponsIsAttached(void* mesh, void* comp)
{
    __try { return mesh && comp && IsAttached((uint8_t*)mesh, (uint8_t*)comp); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// A mesh a punch with the empty left hand moved to the left side (MirrorStrike).
bool WeaponsIsFlipped(void* comp) { return comp && g_flipCount && Contains((void* const*)g_flipped, g_flipCount, comp); }

void WeaponsInstall()
{
    bool ok = MH_CreateHook((void*)kMeshAttach, (void*)HookAttach, (void**)&g_origAttach) == MH_OK &&
              MH_EnableHook((void*)kMeshAttach) == MH_OK;
    // without the attach guard, keeping the meshes would break the next swing: no melee holding then
    if (ok && MH_CreateHook((void*)kMeshDetach, (void*)HookDetach, &g_origDetach) == MH_OK && MH_EnableHook((void*)kMeshDetach) == MH_OK)
        Log("weapons: attach/detach hooks installed");
    else
        Log("weapons: attach/detach hooks FAILED");
    bool InstallShotHook();
    Log(InstallShotHook() ? "weapons: turret shot hook installed" : "weapons: turret shot hook FAILED");
}

void WeaponsUpdate(void* pawnPtr)
{
    uint8_t* pawn = (uint8_t*)pawnPtr;
    DWORD now = GetTickCount();
    __try {
        uint8_t* mesh = pawn ? *(uint8_t**)(pawn + kPawnMesh) : nullptr;
        if (pawn != g_pawn || mesh != g_mesh) {
            // out of first person (cutscene, menu): put the melee weapon away; a new pawn or mesh has new meshes
            // (the old ones may be gone), so nothing to detach then
            ReleaseMelee(!pawn);
            g_pawn = pawn;
            g_mesh = mesh;
            g_seenCount = 0;
            if (!pawn) { g_meleeHand = -1; g_want = nullptr; }
        }
        // the arm model on the empty right hand (ik.cpp; not while the right hand takes the melee weapon)
        IKArmModelUpdate(mesh && g_origDetach ? pawn : nullptr, g_meleeHand != 1);
        if (!pawn || !mesh || !g_origDetach) { g_onTurret = false; return; }
        UpdateTurrets(pawn, now);
        bool vehicle = IKPawnIsVehicle();
        TrackAttachments(mesh, now);
        UpdateMelee(mesh, vehicle, now);
        MirrorStrike(mesh, now);
        if (!vehicle) HoldInPlace(pawn, now);
        static DWORD listAt;
        if (now - listAt > 200) { listAt = now; UpdateList(pawn, vehicle, now); }
        UpdateSwitch(pawn, now);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_pawn = nullptr;
        g_mesh = nullptr;
        g_meleeState = kIdle;
        g_keptCount = 0;
        g_meleeHeld = -1;
    }
}

// debug: the turret effects code run on the weapon in hand (how many particle systems it reaches)
void WeaponsDebugTurretFx()
{
    __try {
        uint8_t* w = g_pawn ? *(uint8_t**)((uint8_t*)g_pawn + kPawnWeapon) : nullptr;
        if (w) Log("debug: %ls: %d particle systems at %.2f", NameOf(*(unsigned*)(w + 4)), ShrinkTurretFx(w, g_cfg.muzzleFlashScale), g_cfg.muzzleFlashScale);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("debug: turretfx failed");
    }
}

// debug "turretdbg 1": on a turret, once a second, what lies along the aim ray (o, unit dir): the gun's meshes and
// particle systems, the mountable turret's components (along: distance down the ray, off: distance from it).
static void LogAlong(const char* what, const wchar_t* name, const float* p, const float* ext, const float o[3], const float dir[3])
{
    float v[3] = { p[0] - o[0], p[1] - o[1], p[2] - o[2] };
    float along = v[0] * dir[0] + v[1] * dir[1] + v[2] * dir[2];
    float off2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] - along * along;
    if (ext) Log("turretdbg:   %s %ls along %.0f off %.0f extent %.0f %.0f %.0f", what, name, along, sqrtf(fmaxf(off2, 0.0f)), ext[0], ext[1], ext[2]);
    else Log("turretdbg:   %s %ls along %.0f off %.0f", what, name, along, sqrtf(fmaxf(off2, 0.0f)));
}

static float GunTipAlong(uint8_t* w, const float o[3], const float dir[3], int* boneOut = nullptr);

void WeaponsDebugTurretAim(const float o[3], const float dir[3])
{
    __try {
        uint8_t* w = g_pawn ? *(uint8_t**)((uint8_t*)g_pawn + kPawnWeapon) : nullptr;
        if (!w) return;
        Log("turretdbg: weapon %ls, on %d", NameOf(*(unsigned*)(w + 4)), (int)g_onTurret);
        int tipBone = -1;
        float tip = GunTipAlong(w, o, dir, &tipBone);
        Log("turretdbg:   barrels' end (bone %d) along %.0f", tipBone, tip);
        uint8_t* meshes[2] = { *(uint8_t**)(w + kWeaponMesh), *(uint8_t**)(w + kWeaponMesh2) };
        for (int m = 0; m < 2; m++) {
            if (!meshes[m] || (m == 1 && meshes[1] == meshes[0])) continue;
            const float* b = (const float*)(meshes[m] + 0xBC);
            LogAlong("mesh", NameOf(*(unsigned*)(meshes[m] + 4)), b, b + 3, o, dir);
            uint8_t* att = *(uint8_t**)(meshes[m] + kMeshAttachments);
            int count = *(int*)(meshes[m] + kMeshAttachments + 4);
            for (int i = 0; att && i < count && i < 32; i++) {
                uint8_t* c = *(uint8_t**)(att + i * 0x30);
                const wchar_t* nm = c ? NameOf(*(unsigned*)(c + 4)) : nullptr;
                if (!nm) continue;
                const wchar_t* bone = NameOf(*(unsigned*)(att + i * 0x30 + 4));
                Log("turretdbg:   attachment %ls at bone %ls", nm, bone ? bone : L"-");
                LogAlong("  at", nm, (const float*)(c + 0x50 + 0x30), nullptr, o, dir);
            }
        }
        for (int i = 0; i < g_turretCount; i++) {
            uint8_t* a = g_turrets[i];
            const wchar_t* tn = NameOf(*(unsigned*)(a + 4));
            LogAlong("actor", tn ? tn : L"?", (const float*)(a + kActorLocation), nullptr, o, dir);
            uint8_t** comps = *(uint8_t***)(a + kActorComponents);
            int cn = *(int*)(a + kActorComponents + 4);
            for (int k = 0; comps && k < cn && k < 32; k++) {
                const wchar_t* cnm = comps[k] ? NameOf(*(unsigned*)(comps[k] + 4)) : nullptr;
                if (cnm) LogAlong("  comp", cnm, (const float*)(comps[k] + 0xBC), (const float*)(comps[k] + 0xBC + 12), o, dir);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("turretdbg: failed");
    }
}

// The turret's shots: the game traces them by the pawn from its eyes along the aim - behind the turret, so tilted, they
// hit its own gun a step away (and burst in its muzzle). The aim's start past the turret (camera.cpp: past its bounds
// and barrels) takes their place in the world's line check (0x1167C9B0, what Actor.Trace calls): the player's long
// traces from near the pawn along the aim start there instead. The aim ring's trace already starts there.
typedef int(__fastcall* PFN_WorldLineCheck)(void* world, void* edx, uint8_t* hit, uint8_t* source, void* unused,
                                             const float* end, const float* start, unsigned flags, const float* extent,
                                             void* a, void* b);
static PFN_WorldLineCheck g_origWorldLineCheck;
static float g_shotStart[3], g_shotDir[3];
static volatile DWORD g_shotTick;

void WeaponsTurretShotStart(const float start[3], const float dir[3])
{
    memcpy(g_shotStart, start, sizeof g_shotStart);
    memcpy(g_shotDir, dir, sizeof g_shotDir);
    g_shotTick = GetTickCount();
}

static int __fastcall HookWorldLineCheck(void* world, void* edx, uint8_t* hit, uint8_t* source, void* unused,
                                         const float* end, const float* start, unsigned flags, const float* extent,
                                         void* a, void* b)
{
    if (!g_onTurret || !g_pawn || source != (uint8_t*)g_pawn || GetCurrentThreadId() != GameThreadId())
        return g_origWorldLineCheck(world, edx, hit, source, unused, end, start, flags, extent, a, b);
    float s2[3], e2[3];
    bool moved = false;
    __try {
        const float* p = (const float*)((uint8_t*)g_pawn + kActorLocation);
        float d[3] = { start[0] - p[0], start[1] - p[1], start[2] - p[2] };
        float l[3] = { end[0] - start[0], end[1] - start[1], end[2] - start[2] };
        float len = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
        float along = len > 0.0f ? (l[0] * g_shotDir[0] + l[1] * g_shotDir[1] + l[2] * g_shotDir[2]) / len : 0.0f;
        float o[3] = { start[0] - g_shotStart[0], start[1] - g_shotStart[1], start[2] - g_shotStart[2] };
        if (GetTickCount() - g_shotTick < 250 && len > 5000.0f && along > 0.995f &&
            d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 400.0f * 400.0f && o[0] * o[0] + o[1] * o[1] + o[2] * o[2] > 1.0f) {
            for (int k = 0; k < 3; k++) { s2[k] = g_shotStart[k]; e2[k] = g_shotStart[k] + l[k]; }
            moved = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { moved = false; }
    if (moved) { start = s2; end = e2; }
    int r = g_origWorldLineCheck(world, edx, hit, source, unused, end, start, flags, extent, a, b);
    // debug "turretdbg 2": the player's long traces (where its shots go and what they hit; FCheckResult: actor +4,
    // location +8)
    extern int g_debugTurret;
    if (g_debugTurret != 2) return r;
    __try {
        float l[3] = { end[0] - start[0], end[1] - start[1], end[2] - start[2] };
        float len = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
        if (len < 1000.0f) return r;
        static DWORD at;
        static int n;
        DWORD now = GetTickCount();
        if (now - at > 1000) { at = now; n = 0; }
        if (++n > 12) return r;
        if (!r && hit) {
            uint8_t* ha = *(uint8_t**)(hit + 4);
            const float* hl = (const float*)(hit + 8);
            const wchar_t* hn = ha ? NameOf(*(unsigned*)(ha + 4)) : nullptr;
            float hd[3] = { hl[0] - start[0], hl[1] - start[1], hl[2] - start[2] };
            Log("turretdbg: trace flags %x%s from %.0f %.0f %.0f dir %.2f %.2f %.2f: hit %ls at %.0f (%.0f %.0f %.0f)",
                flags, moved ? " (moved)" : "", start[0], start[1], start[2], l[0] / len, l[1] / len, l[2] / len,
                hn ? hn : L"-", sqrtf(hd[0] * hd[0] + hd[1] * hd[1] + hd[2] * hd[2]), hl[0], hl[1], hl[2]);
        } else {
            Log("turretdbg: trace flags %x%s from %.0f %.0f %.0f dir %.2f %.2f %.2f: %s", flags, moved ? " (moved)" : "",
                start[0], start[1], start[2], l[0] / len, l[1] / len, l[2] / len, r ? "clear" : "hit");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}

bool InstallShotHook()
{
    const uintptr_t kWorldLineCheck = 0x1167C9B0;
    return MH_CreateHook((void*)kWorldLineCheck, (void*)HookWorldLineCheck, (void**)&g_origWorldLineCheck) == MH_OK &&
           MH_EnableHook((void*)kWorldLineCheck) == MH_OK;
}

void WeaponsGetList(WeaponList& out)
{
    std::lock_guard<std::mutex> lock(g_listMutex);
    out = g_list;
}

void WeaponsSelect(int index)
{
    if (index < 0 || index >= 8 || !g_listWeapon[index]) return;
    g_tries = 5;
    g_pressAt = 0;
    g_want = g_listWeapon[index];
}

void WeaponsSetMeleeHand(int hand)
{
    if (hand != g_meleeHand) Log("weapons: melee weapon %s", hand < 0 ? "put away" : hand ? "to the right hand" : "to the left hand");
    g_meleeHand = hand;
}

int WeaponsMeleeHand() { return g_meleeHand; }
void WeaponsStrike(int hand) { g_strikeHand = hand; g_strikeAt = GetTickCount(); }
int WeaponsMeleeHeld() { return g_meleeHeld; }

bool WeaponsOnTurret() { return g_onTurret; }

// The mountable turret the player is on: the nearest TnMountable (null: none)
static uint8_t* MountedTurret()
{
    const float* p = (const float*)((uint8_t*)g_pawn + kActorLocation);
    uint8_t* best = nullptr;
    float bd = 400.0f * 400.0f;
    for (int i = 0; i < g_turretCount; i++) {
        const wchar_t* tn = NameOf(*(unsigned*)(g_turrets[i] + 4));
        if (!tn || wcsncmp(tn, L"TnMountable", 11)) continue;
        const float* q = (const float*)(g_turrets[i] + kActorLocation);
        float dx = q[0] - p[0], dy = q[1] - p[1];
        if (dx * dx + dy * dy < bd) { bd = dx * dx + dy * dy; best = g_turrets[i]; }
    }
    return best;
}

// The turret gun's farthest bone down the ray from o along unit dir - its barrels' ends, as the gun is turned now - in
// world units (0: none). Bones off the ray by more than the gun's size are not its barrels.
const int kCompSpaceBases = 0x1EC;   // SkeletalMeshComponent: TArray<FMatrix> component-space bone transforms
static float GunTipAlong(uint8_t* w, const float o[3], const float dir[3], int* boneOut)
{
    float best = 0.0f;
    uint8_t* meshes[2] = { *(uint8_t**)(w + kWeaponMesh), *(uint8_t**)(w + kWeaponMesh2) };
    for (int m = 0; m < 2; m++) {
        if (!meshes[m] || (m == 1 && meshes[1] == meshes[0])) continue;
        const float* l2w = (const float*)(meshes[m] + 0x50);   // LocalToWorld: rows X, Y, Z, translation
        const float* bases = *(const float**)(meshes[m] + kCompSpaceBases);
        int n = *(int*)(meshes[m] + kCompSpaceBases + 4);
        for (int b = 0; bases && b < n && b < 256; b++) {
            const float* t = bases + b * 16 + 12;
            float v[3];
            for (int k = 0; k < 3; k++) v[k] = t[0] * l2w[k] + t[1] * l2w[4 + k] + t[2] * l2w[8 + k] + l2w[12 + k] - o[k];
            float along = v[0] * dir[0] + v[1] * dir[1] + v[2] * dir[2];
            float off2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] - along * along;
            if (off2 < 400.0f * 400.0f && along < 2000.0f && along > best) {
                best = along;
                if (boneOut) *boneOut = b;
            }
        }
    }
    return best;
}

// On a mounted turret: how far down the ray from o along unit dir the shots must start to clear the turret - past its
// bounds (its gun's meshes and the mountable's own: base, shield, barrels; not its touch cylinder) and past its
// barrels' ends - in world units (0: not on one). The shots are traced from the aim's start, and from the handles (or
// from the muzzle - its flash's place lags the gun as it turns; or past the bounds alone - tilted, the barrels reach
// past them) the trace hit the turret itself: the shots burst in its barrel and the ring sat on it.
const float kTurretTipMargin = 60.0f;
float WeaponsTurretClearance(const float o[3], const float dir[3])
{
    if (!g_onTurret || !g_pawn) return 0.0f;
    float best = 0.0f;
    __try {
        uint8_t* w = *(uint8_t**)((uint8_t*)g_pawn + kPawnWeapon);
        if (!w) return 0.0f;
        uint8_t* meshes[2 + 32] = { *(uint8_t**)(w + kWeaponMesh), *(uint8_t**)(w + kWeaponMesh2) };
        int count = 2;
        if (uint8_t* t = MountedTurret()) {
            uint8_t** comps = *(uint8_t***)(t + kActorComponents);
            int cn = *(int*)(t + kActorComponents + 4);
            for (int k = 0; comps && k < cn && k < 32; k++) {
                const wchar_t* cnm = comps[k] ? NameOf(*(unsigned*)(comps[k] + 4)) : nullptr;
                if (cnm && wcsstr(cnm, L"MeshComponent")) meshes[count++] = comps[k];
            }
        }
        for (int i = 0; i < count; i++) {
            uint8_t* m = meshes[i];
            if (!m) continue;
            const float* b = (const float*)(m + 0xBC);   // world bounds: origin, box extent
            float tNear = -1e9f, tFar = 1e9f;
            for (int k = 0; k < 3; k++) {
                float lo = b[k] - b[3 + k] - o[k], hi = b[k] + b[3 + k] - o[k];
                if (fabsf(dir[k]) < 1e-6f) { if (lo > 0.0f || hi < 0.0f) tFar = -1.0f; continue; }
                float t1 = lo / dir[k], t2 = hi / dir[k];
                tNear = fmaxf(tNear, fminf(t1, t2));
                tFar = fminf(tFar, fmaxf(t1, t2));
            }
            if (tFar >= fmaxf(tNear, 0.0f) && tFar < 1000.0f) best = fmaxf(best, tFar + 5.0f);
        }
        float tip = GunTipAlong(w, o, dir);
        if (tip > 0.0f) best = fmaxf(best, tip + kTurretTipMargin);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; }
    return best;
}

// On a mounted turret: how far its gun turns - yaw from the turret's own and pitch, min and max, in degrees - and the
// turret's own rotation (pitch, yaw; Unreal units). The gun stops at them (the ion turret: yaw -90..90, pitch
// -30..25); an aim past them traced away from the gun and the shots burst in its muzzle.
bool WeaponsTurretLimits(float lim[4], int& basePitch, int& baseYaw)
{
    if (!g_onTurret || !g_pawn) return false;
    __try {
        uint8_t* t = MountedTurret();
        if (!t) return false;
        const float* l = (const float*)(t + kMountableLimits);
        if (!(l[0] >= -180.0f && l[0] < l[1] && l[1] <= 180.0f && l[2] >= -90.0f && l[2] < l[3] && l[3] <= 90.0f))
            return false;
        for (int k = 0; k < 4; k++) lim[k] = l[k];
        basePitch = *(int*)(t + kActorRotation);
        baseYaw = *(int*)(t + kActorRotation + 4);
        static uint8_t* logged;
        if (logged != t) {
            logged = t;
            Log("weapons: turret %ls turns yaw %.0f..%.0f, pitch %.0f..%.0f", NameOf(*(unsigned*)(t + 4)), l[0], l[1], l[2], l[3]);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool WeaponsWantPress(bool melee)
{
    return InterlockedExchange(melee ? &g_pressMelee : &g_pressNext, 0) != 0;
}

bool WeaponsIsMeleeComp(void* comp)
{
    return g_meleeState != kIdle && Contains((void* const*)g_kept, g_keptCount, comp);
}
