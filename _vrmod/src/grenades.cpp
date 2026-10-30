// Grenades in the hand (like Half-Life: Alyx). The free left hand squeezes at the body (the "pouch") - the game's
// grenade button is pressed for it and the grenade the game throws (TnProjectileGrenade..., ~0.5 s later) is caught
// and kept in the hand. The left trigger arms it; letting go throws it with the hand's own motion. An unarmed grenade
// let go goes back into the pouch (the count is given back).
//
// Found (MockXR/FakeHmd): the flak grenade does not explode on a timer from the throw - held still in the air it
// waits (tested 5 s); it explodes a while after it lands. So holding it is safe; Actor.Velocity +0xF0.
#include "common.h"
#include <math.h>
#include <algorithm>

bool IKHandWorld(int hand, float pos[3], float fwd[3], float rel[3]);   // ik.cpp

namespace {

const uintptr_t kGWorld = 0x12B10A9C;
const uintptr_t kGNames = 0x12B2F0F4;
const int kWorldLevels = 0x34, kLevelActors = 0x28;
const int kActorLocation = 0xD8, kActorVelocity = 0xF0;
const int kPawnInvManager = 0x424, kInvFirst = 0x1C8, kInvNext = 0x1C8;
const int kWeaponClip = 0x2A0, kWeaponReserve = 0x2A4;   // a grenade throw takes one from the reserve

struct Grenades { uint8_t* a[64]; int n; bool Has(uint8_t* x) const { return std::find(a, a + n, x) != a + n; } };

enum State { Idle, Waiting, Holding };
State g_state;
DWORD g_stateAt;
uint8_t* g_grenade;
Grenades g_before;                     // grenades already flying when this one was asked for
DWORD g_aliveAt;
bool g_armed;
float g_debugVel[3];                   // debug: the next let-go uses this hand velocity (m/s, world axes)
volatile long g_debugVelSet;
volatile long g_reqGrab, g_reqArm, g_reqRelease, g_wantPress;
volatile int g_hand;                   // the hand that took it (0 left, 1 right)

// The thrown grenade: the game's own fly almost straight (their physics keeps the velocity; Acceleration makes them
// steer like rockets), so the mod flies it along the arc itself - in game time, checking the way with the world's
// line check - and hands it back to the game's physics at the first thing in the way (it bounces and explodes there).
struct Flight { uint8_t* actor; float pos[3], vel[3]; float time; DWORD since; } g_fly;
// an unarmed grenade let go is parked out of the world, still, and is the next one taken (no new throw then)
uint8_t* g_spare;
float g_sparePos[3];
const int kActorWorldInfo = 0x98, kWorldInfoTimeSeconds = 0x338;
const unsigned kTraceWorld = 0x2286, kTraceWorldAndPawns = 0x2286 | 1;
const int kActorInstigator = 0x94;
uint8_t* g_seen[16];                   // grenades of the player already looked at (thrown with the button: they get weight too)

float g_prevPos[3];
bool g_prevValid;
float g_vel[3];                        // hand velocity, world (Unreal units/s), smoothed
LARGE_INTEGER g_prevT;

const wchar_t* NameOf(unsigned fname)
{
    uint8_t** names = *(uint8_t***)kGNames;
    int count = *(int*)(kGNames + 4);
    int i = (int)(fname >> 14);
    if (!names || i <= 0 || i >= count || !names[i]) return nullptr;
    return (const wchar_t*)(names[i] + 8);
}

// thrown things: TnProjectileGrenade, ...KamikazeMine(Placeable), ...EmpGrenade - not the grenade launcher's shells,
// the space mines' spikes, nor the ones dying robots drop (Martyrdom) or the killstreak's
bool GrenadeProjectileClass(const wchar_t* n)
{
    if (wcsncmp(n, L"TnProjectile", 12) || (!wcsstr(n, L"Grenade") && !wcsstr(n, L"Mine"))) return false;
    return !wcsstr(n, L"Launcher") && !wcsstr(n, L"Spike") && !wcsstr(n, L"Martyr") && !wcsstr(n, L"Killstreak") && !wcsstr(n, L"Data");
}

// the flying grenades of the world's levels
void FindGrenades(Grenades& out)
{
    out.n = 0;
    __try {
        uint8_t* world = *(uint8_t**)kGWorld;
        uint8_t** levels = world ? *(uint8_t***)(world + kWorldLevels) : nullptr;
        int levelCount = world ? *(int*)(world + kWorldLevels + 4) : 0;
        for (int l = 0; levels && l < levelCount && l < 64; l++) {
            uint8_t* level = levels[l];
            if (!level) continue;
            uint8_t** actors = *(uint8_t***)(level + kLevelActors);
            int count = *(int*)(level + kLevelActors + 4);
            for (int i = 0; actors && i < count && i < 20000; i++) {
                uint8_t* a = actors[i];
                const wchar_t* name = a ? NameOf(*(unsigned*)(a + 4)) : nullptr;
                if (name && GrenadeProjectileClass(name) && out.n < 64) out.a[out.n++] = a;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool Alive(uint8_t* a)
{
    Grenades all;
    FindGrenades(all);
    return all.Has(a);
}

bool Contains(uint8_t* const* arr, uint8_t* a) { for (int i = 0; i < 16; i++) if (arr[i] == a) return true; return false; }
void Remember(uint8_t* a)
{
    static int next;
    g_seen[next] = a;
    next = (next + 1) % 16;
}

// the hand grenades in the inventory (TnWeapon...Grenades): null when there are none
uint8_t* GrenadeWeapon(uint8_t* pawn, int* count)
{
    __try {
        uint8_t* inv = *(uint8_t**)(pawn + kPawnInvManager);
        uint8_t* it = inv ? *(uint8_t**)(inv + kInvFirst) : nullptr;
        for (int n = 0; it && n < 32; n++, it = *(uint8_t**)(it + kInvNext)) {
            const wchar_t* name = NameOf(*(unsigned*)(it + 4));
            if (!name || !GrenadeWeaponClass(name)) continue;
            *count = *(int*)(it + kWeaponReserve);
            return it;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

// In the hand the grenade neither spins (its RotationRate; it turns with the hand) nor sounds (its audio
// components, muted) until it is armed.
const int kActorRotation = 0xE4, kActorRotationRate = 0x168, kActorComponents = 0x50, kAudioVolume = 0x110;
int g_savedRate[3];
float g_savedVolume[4];

void ForEachAudio(uint8_t* a, void (*fn)(uint8_t* comp, int i))
{
    __try {
        uint8_t** comps = *(uint8_t***)(a + kActorComponents);
        int n = *(int*)(a + kActorComponents + 4), k = 0;
        for (int i = 0; comps && i < n && i < 32 && k < 4; i++) {
            const wchar_t* name = comps[i] ? NameOf(*(unsigned*)(comps[i] + 4)) : nullptr;
            if (name && wcsstr(name, L"AudioComponent")) fn(comps[i], k++);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void Hush(uint8_t* a, bool hush)
{
    __try {
        int* rate = (int*)(a + kActorRotationRate);
        if (hush) { memcpy(g_savedRate, rate, sizeof(g_savedRate)); memset(rate, 0, sizeof(g_savedRate)); }
        else memcpy(rate, g_savedRate, sizeof(g_savedRate));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (hush) ForEachAudio(a, [](uint8_t* c, int i) { float* v = (float*)(c + kAudioVolume); g_savedVolume[i] = *v; *v = 0.0f; });
    else ForEachAudio(a, [](uint8_t* c, int i) { *(float*)(c + kAudioVolume) = g_savedVolume[i] > 0.0f ? g_savedVolume[i] : 1.0f; });
}

void TurnWithHand(uint8_t* a, const float fwd[3])
{
    __try {
        int* rot = (int*)(a + kActorRotation);
        rot[0] = (int)(asinf(fmaxf(-1.0f, fminf(1.0f, fwd[2]))) * 65536.0f / 6.2831853f);
        rot[1] = (int)(atan2f(fwd[1], fwd[0]) * 65536.0f / 6.2831853f);
        rot[2] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void SetVelocity(uint8_t* a, const float v[3])
{
    __try {
        float* f = (float*)(a + kActorVelocity);
        f[0] = v[0]; f[1] = v[1]; f[2] = v[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

float GameTime(uint8_t* actor)
{
    __try {
        uint8_t* wi = *(uint8_t**)(actor + kActorWorldInfo);
        return wi ? *(float*)(wi + kWorldInfoTimeSeconds) : -1.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1.0f;
    }
}

void FlyStep(uint8_t* pawn, DWORD now)
{
    Flight& f = g_fly;
    float ts = GameTime(f.actor);
    if (ts < 0.0f || now - f.since > 5000) { f.actor = nullptr; return; }
    // (the second eye's frame repeats a moment: game time does not move then, nor does the grenade)
    if (f.time < 0.0f || ts <= f.time) { if (f.time < 0.0f) f.time = ts; return; }
    float dt = fminf(ts - f.time, 0.05f);
    f.time = ts;
    float next[3], vel[3] = { f.vel[0], f.vel[1], f.vel[2] - 9.8f * g_cfg.worldScale * g_cfg.grenadeGravity * dt };
    for (int k = 0; k < 3; k++) next[k] = f.pos[k] + (f.vel[k] + vel[k]) * 0.5f * dt;
    memcpy(f.vel, vel, sizeof(vel));
    // (just out of the hand, the robot's own gun and body are around it: walls only at first)
    if (!WorldLineClear(pawn, f.pos, next, now - f.since < 150 ? kTraceWorld : kTraceWorldAndPawns)) {
        SetVelocity(f.actor, f.vel);   // something in the way: the game's physics takes it from here
        if (g_cfg.logFps) Log("grenades: hit something after %.2f s", (now - f.since) / 1000.0f);
        f.actor = nullptr;
        return;
    }
    MoveActorTo(f.actor, next, true);
    SetVelocity(f.actor, f.vel);
    memcpy(f.pos, next, sizeof(next));
}

void LetGo(uint8_t* pawn, const float hand[3])
{
    if (g_armed) {
        float v[3];
        bool dbg = InterlockedExchange(&g_debugVelSet, 0) != 0;
        for (int k = 0; k < 3; k++) v[k] = (dbg ? g_debugVel[k] * g_cfg.worldScale : g_vel[k]) * g_cfg.grenadeThrowScale;
        MoveActorTo(g_grenade, hand, true);
        SetVelocity(g_grenade, v);
        g_fly = { g_grenade, { hand[0], hand[1], hand[2] }, { v[0], v[1], v[2] }, -1.0f, GetTickCount() };
        __try { memcpy(g_grenade + kActorRotationRate, g_savedRate, sizeof(g_savedRate)); } __except (EXCEPTION_EXECUTE_HANDLER) {}   // spins in flight
        VRHaptic(g_hand,0.5f, 0.04f);
        Log("grenades: thrown at %.0f units/s", sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
    } else {
        // back into the pouch: the count is given back; the grenade waits out of the world for the next grab
        int count = 0;
        if (uint8_t* w = GrenadeWeapon(pawn, &count)) *(int*)(w + kWeaponReserve) = count + 1;
        g_spare = g_grenade;
        g_sparePos[0] = hand[0]; g_sparePos[1] = hand[1]; g_sparePos[2] = hand[2] - 60000.0f;
        VRHaptic(g_hand,0.2f, 0.03f);
        Log("grenades: put back");
    }
    g_grenade = nullptr;
    g_state = Idle;
}

// ---- the grenade on the belt: the grenade weapon's own model (a StaticMeshComponent the game never attaches -
// its throw shows another one) hangs on the robot's hips while there are grenades; ik.cpp places it each frame
const uintptr_t kMeshAttach = 0x1163EF00;   // USkeletalMeshComponent::AttachComponent(comp, bone, &loc, &rot, &scale)
const uintptr_t kMeshDetach = 0x1163B730;   // ... DetachComponent: eax = mesh, edi = component
const int kPawnMesh = 0x3F4, kWeaponModel = 0x208, kMeshAttachments = 0x260, kCompFlags = 0xF0, kCompOwner = 0x34;
const int kCompSkelMesh = 0x1BC, kMeshRefSkeleton = 0x68, kMeshBoneSize = 0x40;
const int kCompTranslation = 0x160;   // PrimitiveComponent.Translation: this model has (0, 0, 150) - zero on the belt
float g_beltTranslation[3];
const uint32_t kHiddenGame = 0x4;
typedef void(__fastcall* AttachFn)(uint8_t* mesh, uint8_t* comp, unsigned bone, const float* loc, const int* rot, const float* scale);
uint8_t* g_belt;            // the model on the belt
uint8_t* g_beltMesh;        // ... attached to this skeletal mesh

bool Attached(uint8_t* mesh, uint8_t* comp)
{
    uint8_t* att = *(uint8_t**)(mesh + kMeshAttachments);
    int n = *(int*)(mesh + kMeshAttachments + 4);
    for (int i = 0; att && i < n && i < 64; i++) if (*(uint8_t**)(att + i * 0x30) == comp) return true;
    return false;
}

void Detach(uint8_t* mesh, uint8_t* comp)
{
    void* fn = (void*)kMeshDetach;
    __asm {
        push edi
        mov eax, mesh
        mov edi, comp
        call fn
        pop edi
    }
}

unsigned HipsBone(uint8_t* mesh)
{
    uint8_t* skel = *(uint8_t**)(mesh + kCompSkelMesh);
    uint8_t* bones = skel ? *(uint8_t**)(skel + kMeshRefSkeleton) : nullptr;
    int n = skel ? *(int*)(skel + kMeshRefSkeleton + 4) : 0;
    for (int i = 0; bones && i < n && i < 256; i++) {
        unsigned name = *(unsigned*)(bones + i * kMeshBoneSize);
        const wchar_t* s = NameOf(name);
        if (s && !wcsncmp(s, L"C_Spine00_Hips", 14)) return name;
    }
    return 0;
}

void UpdateBelt(uint8_t* pawn, bool show)
{
    __try {
        uint8_t* mesh = pawn ? *(uint8_t**)(pawn + kPawnMesh) : nullptr;
        int count = 0;
        uint8_t* w = pawn && show ? GrenadeWeapon(pawn, &count) : nullptr;
        uint8_t* model = w && (count > 0 || g_spare) ? *(uint8_t**)(w + kWeaponModel) : nullptr;
        if (g_belt && (g_belt != model || g_beltMesh != mesh)) {   // none left, another body (transformed), level change
            if (g_beltMesh && Attached(g_beltMesh, g_belt)) Detach(g_beltMesh, g_belt);
            memcpy(g_belt + kCompTranslation, g_beltTranslation, sizeof(g_beltTranslation));
            g_belt = g_beltMesh = nullptr;
            IKSetBelt(nullptr);
        }
        if (!model || !mesh) return;
        if (!g_belt) {
            unsigned hips = HipsBone(mesh);
            if (!hips || Attached(mesh, model)) return;
            float loc[3] = {}, scale[3] = { 1, 1, 1 };
            int rot[3] = {};
            memcpy(g_beltTranslation, model + kCompTranslation, sizeof(g_beltTranslation));
            memset(model + kCompTranslation, 0, sizeof(g_beltTranslation));
            ((AttachFn)kMeshAttach)(mesh, model, hips, loc, rot, scale);
            g_belt = model;
            g_beltMesh = mesh;
            Log("grenades: grenade model %p on the belt", model);
        }
        *(uint8_t**)(g_belt + kCompOwner) = pawn;   // (the grenade weapon itself is hidden: drawn as part of the robot)
        *(uint32_t*)(g_belt + kCompFlags) &= ~kHiddenGame;
        IKSetBelt(g_belt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_belt = g_beltMesh = nullptr;
        IKSetBelt(nullptr);
    }
}

}   // namespace

void GrenadeDebugVelocity(const float v[3])
{
    memcpy(g_debugVel, v, sizeof(g_debugVel));
    g_debugVelSet = 1;
}

void GrenadeGrab(int hand) { if (g_state == Idle) g_hand = hand; g_reqGrab = 1; }
void GrenadeArm() { g_reqArm = 1; }
void GrenadeRelease() { g_reqRelease = 1; }
bool GrenadeHeld() { return g_state != Idle; }
int GrenadeHand() { return g_state != Idle ? g_hand : -1; }
bool GrenadeWantPress() { return InterlockedExchange(&g_wantPress, 0) != 0; }
bool GrenadeWeaponClass(const wchar_t* cls)
{
    return !wcsncmp(cls, L"TnWeapon", 8) && (wcsstr(cls, L"Grenades") || wcsstr(cls, L"Mines")) && !wcsstr(cls, L"Launcher");
}

// game thread, once per frame; pawn = first-person player pawn (null = none)
void GrenadesUpdate(void* pawnPtr)
{
    uint8_t* pawn = (uint8_t*)pawnPtr;
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    float dt = g_prevT.QuadPart ? fminf((float)(t.QuadPart - g_prevT.QuadPart) / f.QuadPart, 0.1f) : 0.0f;
    g_prevT = t;
    bool grab = InterlockedExchange(&g_reqGrab, 0) != 0, arm = InterlockedExchange(&g_reqArm, 0) != 0,
         release = InterlockedExchange(&g_reqRelease, 0) != 0;
    if (!pawn) {   // cutscene, menu, level change: the belt model is left as it is (the pawn may be gone)
        g_belt = g_beltMesh = nullptr;
        IKSetBelt(nullptr);
    }
    bool vehicle = pawn && IKPawnIsVehicle();
    if (pawn) UpdateBelt(pawn, g_cfg.grenadeGrab && g_cfg.grenadeBelt && !vehicle);
    if (!pawn || vehicle || !g_cfg.grenadeGrab) { g_state = Idle; g_grenade = g_spare = g_fly.actor = nullptr; g_prevValid = false; return; }

    float pos[3], fwd[3], rel[3];
    bool hand = IKHandWorld(g_hand, pos, fwd, rel);
    if (hand && g_prevValid && dt > 0) {
        // the hand's own motion in the world (walking adds to a throw, as it would)
        float a = fminf(dt / 0.03f, 1.0f);
        for (int k = 0; k < 3; k++) g_vel[k] += ((pos[k] - g_prevPos[k]) / dt - g_vel[k]) * a;
    }
    if (hand) memcpy(g_prevPos, pos, sizeof(pos));
    g_prevValid = hand;
    DWORD now = GetTickCount();
    // the grenade sits in the palm, a little in front of the wrist
    float held[3] = { pos[0] + fwd[0] * 10.0f, pos[1] + fwd[1] * 10.0f, pos[2] + fwd[2] * 10.0f };

    if (grab && g_state == Idle) {
        int count = 0;
        if (!GrenadeWeapon(pawn, &count) || count <= 0) {
            VRHaptic(g_hand,0.15f, 0.02f);
            Log("grenades: none left");
        } else if (g_spare && Alive(g_spare)) {
            // the one put back
            if (uint8_t* w = GrenadeWeapon(pawn, &count)) *(int*)(w + kWeaponReserve) = count - 1;
            g_grenade = g_spare;
            g_spare = nullptr;
            g_state = Holding;
            g_armed = false;
            VRHaptic(g_hand,0.5f, 0.05f);
        } else {
            g_spare = nullptr;
            FindGrenades(g_before);
            g_wantPress = 1;   // the game's grenade button (vr.cpp)
            g_state = Waiting;
            g_stateAt = now;
            g_armed = false;
        }
    }
    if (arm && g_state != Idle && !g_armed) {
        g_armed = true;
        Log("grenades: armed");
        if (g_grenade) ForEachAudio(g_grenade, [](uint8_t* c, int i) { *(float*)(c + kAudioVolume) = g_savedVolume[i] > 0.0f ? g_savedVolume[i] : 1.0f; });
    }

    if (g_state == Waiting) {
        Grenades now_;
        FindGrenades(now_);
        for (int i = 0; i < now_.n && !g_grenade; i++)
            if (!g_before.Has(now_.a[i])) g_grenade = now_.a[i];
        if (g_grenade) {
            g_state = Holding;
            Hush(g_grenade, true);
            VRHaptic(g_hand,0.5f, 0.05f);
            Log("grenades: in the hand (%p)", g_grenade);
        } else if (now - g_stateAt > 1500) {
            Log("grenades: the game threw none");
            g_state = Idle;
        }
    }
    static DWORD spareCheck;
    if (g_spare && now - spareCheck > 500) {   // (gone with a level change?)
        spareCheck = now;
        if (!Alive(g_spare)) g_spare = nullptr;
    }
    if (g_spare) {
        float zero[3] = {};
        MoveActorTo(g_spare, g_sparePos, true);
        SetVelocity(g_spare, zero);
    }
    // a grenade thrown with the button (the game's own throw flies almost straight): it gets the same weight
    static DWORD scanAt;
    if (!g_fly.actor && g_state == Idle && g_cfg.grenadeGravity > 0.0f && now - scanAt > 50) {
        scanAt = now;
        Grenades all;
        FindGrenades(all);
        for (int i = 0; i < all.n; i++) {
            uint8_t* a = all.a[i];
            if (a == g_spare || Contains(g_seen, a)) continue;
            Remember(a);
            __try {
                if (*(uint8_t**)(a + kActorInstigator) != pawn) continue;
                const float* l = (const float*)(a + kActorLocation);
                const float* v = (const float*)(a + kActorVelocity);
                g_fly = { a, { l[0], l[1], l[2] }, { v[0], v[1], v[2] }, -1.0f, now };
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            break;
        }
    }
    if (g_fly.actor) {
        if (now - g_aliveAt > 100) {
            g_aliveAt = now;
            if (!Alive(g_fly.actor)) g_fly.actor = nullptr;
        }
        if (g_fly.actor) FlyStep(pawn, now);
    }
    if (g_state == Holding) {
        if (now - g_aliveAt > 100) {   // (exploded, or the level changed)
            g_aliveAt = now;
            if (!Alive(g_grenade)) { g_state = Idle; g_grenade = nullptr; return; }
        }
        if (release || !hand) { LetGo(pawn, held); return; }
        float zero[3] = {};
        MoveActorTo(g_grenade, held, true);
        TurnWithHand(g_grenade, fwd);
        SetVelocity(g_grenade, zero);
    } else if (release && g_state == Waiting) {
        // let go before the grenade came: it is caught and handled on arrival
        g_reqRelease = 1;
    }
}
