// Pulling pickups from a distance (like the gravity gloves of Half-Life: Alyx / Boneworks): point the free left hand
// at a pickup (a tick in the controller), then jerk the hand back towards you - the pickup flies to the robot and the
// game picks it up as if you had walked over it.
//
// Pickups: actors of the world's levels (GWorld -> Levels -> Actors) whose name has "Pickup" in it (health, ammo,
// weapon factories, dropped weapons). They are moved with UWorld::FarMoveActor - what the script's SetLocation uses -
// so their collision moves with them and the pawn touches them. A pickup factory goes back home afterwards (it
// respawns there); a dropped weapon stays where it landed.
#include "common.h"
#include <math.h>

bool IKHandWorld(int hand, float pos[3], float fwd[3], float rel[3]);   // ik.cpp

namespace {

const uintptr_t kGWorld = 0x12B10A9C;
const uintptr_t kGNames = 0x12B2F0F4;
const uintptr_t kFarMoveActor = 0x11689630;   // UWorld::FarMoveActor(actor, &dest, bTest, bNoCheck, bAttachedMove)
const int kWorldLevels = 0x34;                // TArray<ULevel*>
const int kLevelActors = 0x28;                // TArray<AActor*>
const int kActorLocation = 0xD8;
const int kActorFlags1 = 0x78;                // bit 0: bStatic
const int kActorFlags2 = 0x7C;                // bit 0x2000: movable (FarMoveActor refuses others during play)

typedef int(__thiscall* FarMoveFn)(void* world, void* actor, const Vec3f* dest, int test, int noCheck, int attached);

struct Pickup { uint8_t* actor; Vec3f loc, aim; bool factory, weapon; };   // aim: the drawn item (a factory's floats above it)
volatile long g_grabNow;    // vr.cpp: the pointing hand's grip was squeezed - pull now
volatile long g_grabHand = -1;   // the hand whose grip that was
volatile bool g_points[2];  // each free hand points at a pickup now
DWORD g_interactUntil;      // a pulled weapon arrived: the interact button is held until then (the game takes it so)
Pickup g_list[64];
int g_count;
DWORD g_scanAt;

uint8_t* g_target;          // pickup the pointing hand (g_hand) points at
DWORD g_targetAt;           // last time it was pointed at (the flick may turn the hand away)
float g_prevRel[3];
bool g_prevValid;
int g_hand = -1;            // the hand that points (0 left, 1 right; -1: none is free)
LARGE_INTEGER g_prevT;
float g_vel[3];             // hand velocity relative to the body (world axes, Unreal units/s), smoothed

bool g_debugAim, g_debugFlick;

struct Flight { uint8_t* actor; Vec3f from, home, landing, drawn; bool factory, weapon; int hand; float t, duration; DWORD landedAt, retryAt; bool landed, checked, movePawn; } g_fly;

const wchar_t* NameOf(unsigned fname)
{
    uint8_t** names = *(uint8_t***)kGNames;
    int count = *(int*)(kGNames + 4);
    int i = (int)(fname >> 14);
    if (!names || i <= 0 || i >= count || !names[i]) return nullptr;
    return (const wchar_t*)(names[i] + 8);
}

bool MoveActor(uint8_t* actor, const Vec3f& dest, bool noCheck)
{
    uint8_t* world = *(uint8_t**)kGWorld;
    if (!world || !actor) return false;
    // pickup factories are placed in the level; let them move for this call
    uint32_t f1 = *(uint32_t*)(actor + kActorFlags1), f2 = *(uint32_t*)(actor + kActorFlags2);
    *(uint32_t*)(actor + kActorFlags1) = f1 & ~1u;
    *(uint32_t*)(actor + kActorFlags2) = f2 | 0x2000;
    int ok = ((FarMoveFn)kFarMoveActor)(world, actor, &dest, 0, noCheck ? 1 : 0, 0);
    *(uint32_t*)(actor + kActorFlags1) = (*(uint32_t*)(actor + kActorFlags1) & ~1u) | (f1 & 1u);
    *(uint32_t*)(actor + kActorFlags2) = (*(uint32_t*)(actor + kActorFlags2) & ~0x2000u) | (f2 & 0x2000u);
    return ok != 0;
}

// A factory whose item was taken hides it: available only while one of its meshes or effects is drawn (not
// HiddenGame - flags +0xF0 bit 2 - and in the scene, +0x40). The collision cylinder is always hidden.
// Where the item is drawn, relative to the actor: a factory's mesh floats above its base (the actor's place), so
// the actor goes to the hand minus this to put the item itself in the hand. Centre of the drawn meshes' bounds
// (radius: of those bounds, Unreal units).
Vec3f DrawnOffset(uint8_t* a, float* radius = nullptr)
{
    if (radius) *radius = 30.0f;
    uint8_t** comps = *(uint8_t***)(a + 0x50);
    int n = *(int*)(a + 0x54);
    const Vec3f& loc = *(Vec3f*)(a + kActorLocation);
    for (int i = 0; comps && i < n && i < 32; i++) {
        uint8_t* c = comps[i];
        const wchar_t* nm = c ? NameOf(*(unsigned*)(c + 4)) : nullptr;
        if (!nm || !wcsstr(nm, L"MeshComponent") || (*(uint32_t*)(c + 0xF0) & 0x4) || !*(void**)(c + 0x40)) continue;
        const float* b = (const float*)(c + 0xBC);   // FBoxSphereBounds (world): origin, extent, radius
        Vec3f d = { b[0] - loc.x, b[1] - loc.y, b[2] - loc.z };
        if (b[6] > 1.0f && d.x * d.x + d.y * d.y + d.z * d.z < 300.0f * 300.0f) {
            if (radius) *radius = fminf(b[6], 150.0f);
            return d;
        }
    }
    return { 0, 0, 0 };
}

bool PickupShown(uint8_t* a)
{
    uint8_t** comps = *(uint8_t***)(a + 0x50);
    int n = *(int*)(a + 0x54);
    for (int i = 0; comps && i < n && i < 32; i++) {
        uint8_t* c = comps[i];
        const wchar_t* nm = c ? NameOf(*(unsigned*)(c + 4)) : nullptr;
        if (!nm || (!wcsstr(nm, L"MeshComponent") && !wcsstr(nm, L"ParticleSystemComponent"))) continue;
        if (!(*(uint32_t*)(c + 0xF0) & 0x4) && *(void**)(c + 0x40)) return true;
    }
    return false;
}

void Scan(bool log)
{
    int n = 0;
    uint8_t* world = *(uint8_t**)kGWorld;
    uint8_t** levels = world ? *(uint8_t***)(world + kWorldLevels) : nullptr;
    int levelCount = world ? *(int*)(world + kWorldLevels + 4) : 0;
    for (int l = 0; levels && l < levelCount && l < 64; l++) {
        uint8_t* level = levels[l];
        if (!level) continue;
        uint8_t** actors = *(uint8_t***)(level + kLevelActors);
        int count = *(int*)(level + kLevelActors + 4);
        for (int i = 0; actors && i < count && i < 20000 && n < 64; i++) {
            uint8_t* a = actors[i];
            if (!a) continue;
            const wchar_t* name = NameOf(*(unsigned*)(a + 4));
            // pickups: factories (ammo, health, weapons) and dropped items (weapons, grenades of beaten enemies) - any
            // "...Pickup..." / "...Dropped..." class (not only the Tn ones)
            if (!name || (!wcsstr(name, L"Pickup") && !wcsstr(name, L"Dropped"))) continue;
            bool factory = wcsstr(name, L"Factory") != nullptr;
            if (factory && !PickupShown(a) && !(g_fly.actor == a)) continue;   // taken (empty)
            Pickup& p = g_list[n++];
            p.actor = a;
            p.loc = *(Vec3f*)(a + kActorLocation);
            Vec3f off = DrawnOffset(a);
            p.aim = { p.loc.x + off.x, p.loc.y + off.y, p.loc.z + off.z };
            p.factory = wcsstr(name, L"Factory") != nullptr;
            p.weapon = wcsstr(name, L"Weapon") != nullptr;
            if (log) Log("pull: %ls %p at %.0f %.0f %.0f", name, a, p.loc.x, p.loc.y, p.loc.z);
        }
    }
    g_count = n;
    if (log) Log("pull: %d pickups in %d levels", n, levelCount);
}

bool Listed(uint8_t* a)
{
    for (int i = 0; i < g_count; i++) if (g_list[i].actor == a) return true;
    return false;
}

void StartFlight(int i)
{
    const Pickup& p = g_list[i];
    g_fly = {};
    g_fly.actor = p.actor;
    g_fly.from = *(Vec3f*)(p.actor + kActorLocation);
    g_fly.home = g_fly.from;
    g_fly.factory = p.factory;
    g_fly.weapon = p.weapon;
    g_fly.hand = g_hand;
    g_fly.drawn = DrawnOffset(p.actor);
    g_target = nullptr;
    VRHaptic(g_hand < 0 ? 0 : g_hand, 0.6f, 0.08f);
    Log("pull: pulling %p (%s) to hand %d, drawn %.0f %.0f %.0f from its base", p.actor, p.factory ? "factory" : "dropped",
        g_hand, g_fly.drawn.x, g_fly.drawn.y, g_fly.drawn.z);
}

// debug: offsets of the arrays (TArray: data, count, max) in obj that hold other (the actors' Touching lists)
void LogArraysHolding(const char* what, uint8_t* obj, uint8_t* other)
{
    for (int o = 0x40; o < 0x800; o += 4) {
        __try {
            uint8_t** data = *(uint8_t***)(obj + o);
            int n = *(int*)(obj + o + 4), max = *(int*)(obj + o + 8);
            if (!data || n <= 0 || n > 64 || max < n || max > 256) continue;
            for (int i = 0; i < n; i++)
                if (data[i] == other) { Log("pull: %s +0x%X holds it (%d of %d)", what, o, i, n); break; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

void Fly(uint8_t* pawn, float dt, DWORD now)
{
    if (!g_fly.actor) return;
    // the actor may be gone (a dropped weapon picked up and destroyed)
    if (!Listed(g_fly.actor) && now - g_scanAt < 100) { g_fly.actor = nullptr; return; }
    Vec3f to;
    if (!ReadActorLocation(pawn, to)) return;
    if (!g_fly.landed) {
        // like Boneworks: it flies smoothly into the hand that pulled it (following the hand), then is taken (the last
        // step puts it on the pawn, which touches it)
        float hp[3], hf[3], hr[3];
        Vec3f aim = to;
        // (the drawn item - not the actor's base - ends in the palm)
        if (g_fly.hand >= 0 && IKHandWorld(g_fly.hand, hp, hf, hr))
            aim = { hp[0] + hf[0] * 10.0f - g_fly.drawn.x, hp[1] + hf[1] * 10.0f - g_fly.drawn.y, hp[2] + hf[2] * 10.0f - g_fly.drawn.z };
        float dist = sqrtf((aim.x - g_fly.from.x) * (aim.x - g_fly.from.x) + (aim.y - g_fly.from.y) * (aim.y - g_fly.from.y) +
                           (aim.z - g_fly.from.z) * (aim.z - g_fly.from.z));
        if (g_fly.t == 0.0f) g_fly.duration = 0.25f + fminf(dist / 6000.0f, 0.3f);   // quick: it is taken on arrival
        // the interact button is held from the last part of the flight on: the game takes a weapon the moment it
        // touches (a swap for a full slot counts the hold from here, not from the landing)
        if (g_fly.weapon && g_cfg.pullAutoTake && g_fly.t > 0.6f) g_interactUntil = now + 1500;
        g_fly.t = fminf(g_fly.t + dt / g_fly.duration, 1.0f);
        float t = g_fly.t, s = t * t * t * (t * (t * 6 - 15) + 10);   // smootherstep: eases out of its place and into the hand
        // a low arc that is gone well before the hand (a high one came in over the head)
        float arc = t < 0.7f ? sinf(t / 0.7f * 3.14159265f) : 0.0f;
        Vec3f p = { g_fly.from.x + (aim.x - g_fly.from.x) * s, g_fly.from.y + (aim.y - g_fly.from.y) * s,
                    g_fly.from.z + (aim.z - g_fly.from.z) * s + arc * fminf(dist * 0.05f, 35.0f) };
        if (t < 1.0f) MoveActor(g_fly.actor, p, true);
        else {
            // into the pawn without the checks (with them the engine pushed it out - up, above the head), then the
            // pawn's own move (with the checks, to where it is) finds the touch: the game takes it
            MoveActor(g_fly.actor, to, true);
            MoveActor(pawn, to, false);
            g_fly.landing = to;
            g_fly.landed = true;
            g_fly.landedAt = now;
            VRHaptic(g_hand < 0 ? 0 : g_hand, 0.4f, 0.05f);
            if (g_fly.weapon && g_cfg.pullAutoTake) g_interactUntil = now + 1500;   // ("hold X to take")
        }
    } else {
        // a factory goes back to its place (it respawns there) once you walk away from it or after a while: some are
        // taken by touching, weapons with the interact button
        if (g_cfg.logFps && !g_fly.checked && now - g_fly.landedAt > 500) {
            g_fly.checked = true;
            LogArraysHolding("pawn", pawn, g_fly.actor);
            LogArraysHolding("pickup", g_fly.actor, pawn);
            Log("pull: touch check done (pawn %p)", pawn);
        }
        // not taken (full health / ammo, a weapon not swapped): back to its place right away, not left floating
        bool taken = !g_fly.factory ? !Listed(g_fly.actor) : !PickupShown(g_fly.actor);
        DWORD wait = g_fly.weapon ? 2000 : 800;
        if (!taken && now - g_fly.landedAt < wait) {
            // a crate or health not taken (yet): the touch is tried again, and meanwhile it stays in the hand - not
            // inside the body. (Weapons stay on the pawn: the held interact button takes what it touches.)
            float hp[3], hf[3], hr[3];
            if (g_fly.factory && !g_fly.weapon && g_fly.hand >= 0 && IKHandWorld(g_fly.hand, hp, hf, hr)) {
                if (now - g_fly.retryAt > 150) {
                    g_fly.retryAt = now;
                    MoveActor(g_fly.actor, to, true);
                    MoveActor(pawn, to, false);
                }
                if (PickupShown(g_fly.actor))
                    MoveActor(g_fly.actor, { hp[0] + hf[0] * 10.0f - g_fly.drawn.x, hp[1] + hf[1] * 10.0f - g_fly.drawn.y,
                                             hp[2] + hf[2] * 10.0f - g_fly.drawn.z }, true);
            }
            float dx = g_fly.landing.x - to.x, dy = g_fly.landing.y - to.y;
            if (dx * dx + dy * dy < 450.0f * 450.0f) return;
        }
        if (taken && g_fly.factory && now - g_fly.landedAt < 300) return;
        if (g_fly.factory) MoveActor(g_fly.actor, g_fly.home, true);
        Log("pull: landed%s", g_fly.factory ? ", factory moved back" : "");
        g_fly.actor = nullptr;
    }
}

// The world's line check (what AActor::FastTrace calls, 0x1167C9B0): __thiscall(world, hit, source actor, ?, end,
// start, trace flags, extent, ?, ?); returns non-zero when nothing is hit. Flags as FastTrace (world geometry).
typedef int(__fastcall* LineCheckFn)(void* world, void* edx, void* hit, void* actor, void* unused, const Vec3f* end,
                                     const Vec3f* start, unsigned flags, const Vec3f* extent, void* a, void* b);
const uintptr_t kLineCheck = 0x1167C9B0;

bool LineClear(uint8_t* source, const Vec3f& from, const Vec3f& to)
{
    void* world = *(void**)kGWorld;
    if (!world || !g_cfg.pullTrace) return true;
    Vec3f extent = { 0, 0, 0 };
    return ((LineCheckFn)kLineCheck)(world, nullptr, nullptr, source, nullptr, &to, &from, 0x2286, &extent, nullptr, nullptr) != 0;
}

// the pickup closest to a hand's pointing ray, not through walls (-1: none)
int PointedPickup(uint8_t* pawn, const float pos[3], const float fwd[3])
{
    float range = g_cfg.pullRange * g_cfg.worldScale, cone = cosf(g_cfg.pullConeDeg * 0.0174533f);
    int best = -1;
    float bestCos = cone;
    // (at the item as drawn: a factory's floats above its base - pointing at the base missed it)
    for (int i = 0; i < g_count; i++) {
        Vec3f l = g_list[i].aim;
        float d[3] = { l.x - pos[0], l.y - pos[1], l.z - pos[2] };
        float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len < 80.0f || len > range) continue;
        float c = (d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2]) / len;
        if (c > bestCos) { bestCos = c; best = i; }
    }
    // the line from the hand to (just short of) the pickup must be free
    if (best >= 0) {
        Vec3f l = g_list[best].aim;
        Vec3f from = { pos[0], pos[1], pos[2] };
        float d[3] = { l.x - pos[0], l.y - pos[1], l.z - pos[2] };
        float len = fmaxf(sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), 1.0f);
        float k = fmaxf(len - 70.0f, 0.0f) / len;   // the pickup's own collision is not a wall
        Vec3f to = { pos[0] + d[0] * k, pos[1] + d[1] * k, pos[2] + d[2] * k + 20.0f };
        if (!LineClear(pawn, from, to)) best = -1;
    }
    return best;
}

void Aim(uint8_t* pawn, float dt, DWORD now)
{
    // every free hand points; the one in use keeps it while it has a pickup, else a free hand that points at one
    bool free[2] = {
        !VRTwoHanded() && WeaponsMeleeHeld() != 0 && GrenadeHand() != 0 && VRGunHand() != 0,
        !VRTwoHanded() && VRRightEmpty() && WeaponsMeleeHeld() != 1 && GrenadeHand() != 1 };
    float hp[2][3], hf[2][3], hr[2][3];
    int found[2] = { -1, -1 };
    for (int h = 0; h < 2; h++) {
        if (free[h] && !IKHandWorld(h, hp[h], hf[h], hr[h])) free[h] = false;
        if (free[h]) found[h] = PointedPickup(pawn, hp[h], hf[h]);
        g_points[h] = found[h] >= 0;
    }
    int want = InterlockedExchange(&g_grabHand, -1);   // a grip was squeezed (PullGrab)
    bool kept = g_hand >= 0 && free[g_hand] && (found[g_hand] >= 0 || (g_target && now - g_targetAt <= 600));
    int hand = kept ? g_hand : free[0] && found[0] >= 0 ? 0 : free[1] && found[1] >= 0 ? 1
             : g_hand >= 0 && free[g_hand] ? g_hand : free[0] ? 0 : free[1] ? 1 : -1;
    if (want >= 0 && want != hand && free[want] && found[want] >= 0) hand = want;
    if (want >= 0 && want != hand) g_grabNow = 0;   // that hand points at nothing
    if (hand != g_hand) { g_prevValid = false; g_target = nullptr; g_hand = hand; }
    if (hand < 0) { g_prevValid = false; g_target = nullptr; return; }
    float* pos = hp[hand], * fwd = hf[hand], * rel = hr[hand];

    // hand velocity relative to the body (walking and turning do not count)
    if (g_prevValid && dt > 0) {
        float d[3], len2 = 0;
        for (int k = 0; k < 3; k++) { d[k] = (rel[k] - g_prevRel[k]) / dt; len2 += (rel[k] - g_prevRel[k]) * (rel[k] - g_prevRel[k]); }
        if (len2 < 100.0f * 100.0f) {   // (a snap turn jumps)
            float a = fminf(dt / 0.04f, 1.0f);
            for (int k = 0; k < 3; k++) g_vel[k] += (d[k] - g_vel[k]) * a;
        }
    }
    memcpy(g_prevRel, rel, sizeof(g_prevRel));
    g_prevValid = true;

    int best = found[hand];
    if (g_debugAim) {
        g_debugAim = false;
        Log("pull: hand %d at %.0f %.0f %.0f points %.2f %.2f %.2f", hand, pos[0], pos[1], pos[2], fwd[0], fwd[1], fwd[2]);
        for (int i = 0; i < g_count; i++) {
            Vec3f l = g_list[i].loc;
            float d[3] = { l.x - pos[0], l.y - pos[1], l.z - pos[2] };
            float yaw = (atan2f(d[1], d[0]) - atan2f(fwd[1], fwd[0])) * 57.29578f;
            yaw = remainderf(yaw, 360.0f);
            Vec3f from = { pos[0], pos[1], pos[2] };
            Log("pull:   %p %.1f m, %.0f deg to the right, line %s", g_list[i].actor, sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / g_cfg.worldScale,
                yaw, LineClear(pawn, from, l) ? "clear" : "blocked");
        }
    }
    if (best >= 0) {
        if (g_list[best].actor != g_target) {
            VRHaptic(g_hand, 0.25f, 0.03f);
            if (g_cfg.logFps) Log("pull: pointing at %p", g_list[best].actor);
        }
        g_target = g_list[best].actor;
        g_targetAt = now;
    } else if (g_target && now - g_targetAt > 600) {
        g_target = nullptr;
    }
    if (!g_target || g_fly.actor) { g_grabNow = 0; return; }

    // the flick (or a squeeze of the grip: PullGrab): the hand jerks back, away from the pickup
    for (int i = 0; i < g_count; i++) {
        if (g_list[i].actor != g_target) continue;
        Vec3f l = g_list[i].aim;
        float d[3] = { l.x - pos[0], l.y - pos[1], l.z - pos[2] };
        float len = fmaxf(sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), 1.0f);
        float away = -(g_vel[0] * d[0] + g_vel[1] * d[1] + g_vel[2] * d[2]) / len / g_cfg.worldScale;
        if ((g_cfg.pullSpeed > 0 && away > g_cfg.pullSpeed) || g_debugFlick || InterlockedExchange(&g_grabNow, 0)) StartFlight(i);
        g_debugFlick = false;
        break;
    }
}

volatile long g_debugPull;
void* volatile g_debugPullActor;

}   // namespace

bool WorldLineClear(void* source, const float from[3], const float to[3], unsigned flags)
{
    void* world = *(void**)kGWorld;
    if (!world) return true;
    Vec3f a = { from[0], from[1], from[2] }, b = { to[0], to[1], to[2] }, extent = { 0, 0, 0 };
    __try {
        return ((LineCheckFn)kLineCheck)(world, nullptr, nullptr, source, nullptr, &b, &a, flags, &extent, nullptr, nullptr) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

bool MoveActorTo(void* actor, const float pos[3], bool noCheck)
{
    __try {
        return MoveActor((uint8_t*)actor, { pos[0], pos[1], pos[2] }, noCheck);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// game thread, once per frame; pawn = first-person player pawn (null = none)
void PullUpdate(void* pawnPtr)
{
    uint8_t* pawn = (uint8_t*)pawnPtr;
    DWORD now = GetTickCount();
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    float dt = g_prevT.QuadPart ? fminf((float)(t.QuadPart - g_prevT.QuadPart) / f.QuadPart, 0.1f) : 0.0f;
    g_prevT = t;
    long dbg = InterlockedExchange(&g_debugPull, 0);
    if (!g_cfg.pull || !pawn) { g_target = nullptr; g_fly.actor = nullptr; g_prevValid = false; return; }
    __try {
        if (now - g_scanAt > 250 || dbg == 1) { Scan(dbg == 1); g_scanAt = now; }
        if (dbg == 4) g_debugAim = true;
        if (dbg == 5) g_debugFlick = true;
        if (IKPawnIsVehicle()) { g_target = nullptr; g_prevValid = false; }
        else Aim(pawn, dt, now);
        if ((dbg == 2 || dbg == 3) && !g_fly.actor) {   // debug: pull the nearest one
            Vec3f p;
            int best = -1;
            float bestD = 1e30f;
            if (ReadActorLocation(pawn, p))
                for (int i = 0; i < g_count; i++) {
                    float dx = g_list[i].loc.x - p.x, dy = g_list[i].loc.y - p.y, dz = g_list[i].loc.z - p.z;
                    float d = dx * dx + dy * dy + dz * dz;
                    if (g_debugPullActor ? g_list[i].actor == g_debugPullActor : d > 100.0f * 100.0f && d < bestD) { bestD = d; best = i; }
                }
            if (best >= 0) StartFlight(best);
            else Log("pull: nothing to pull");
            g_fly.movePawn = dbg == 3;
        }
        Fly(pawn, dt, now);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("pull: exception");
        g_count = 0;
        g_target = nullptr;
        g_fly.actor = nullptr;
    }
}

// VR input: hand h's grip was just squeezed - true (and the pickup flies) when that hand points at one
bool PullGrab(int h)
{
    if (h < 0 || h > 1 || g_fly.actor || !(g_points[h] || (h == g_hand && g_target))) return false;
    g_grabHand = h;
    g_grabNow = 1;
    return true;
}

bool PullWantInteract() { return GetTickCount() < g_interactUntil; }

// The pickup a squeeze of the pointing hand's grip would pull (game thread, after PullUpdate): the centre of what is
// drawn and its radius, Unreal units - vr.cpp rings it (a tick alone was easy to miss: the hand grabbed at air).
bool PullMarkWorld(float centre[3], float& radius)
{
    if (!g_cfg.pull || !g_cfg.pullMark || !g_target || g_fly.actor || g_hand < 0) return false;
    __try {
        const Vec3f& loc = *(Vec3f*)(g_target + kActorLocation);
        Vec3f off = DrawnOffset(g_target, &radius);
        centre[0] = loc.x + off.x; centre[1] = loc.y + off.y; centre[2] = loc.z + off.z;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 1 = log the pickups, 2 = pull the nearest (3: and re-place the pawn), 4 = log where the left hand points,
// 5 = a flick now (pulls what the hand points at)
void PullDebug(int what, void* actor)
{
    g_debugPullActor = actor;
    g_debugPull = what;
}
