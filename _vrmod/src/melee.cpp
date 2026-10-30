// Contact melee: a swing of the melee weapon strikes only when the weapon really reaches another robot. The game's
// own melee attack (its damage and hit reaction) is started at that moment and aimed at the robot that was hit; a
// swing through the air does nothing - no attack animation, no lunge, the player keeps walking. (The script-side
// TakeDamage is an empty stub in this build - the game applies damage its own way - so the attack is the game's.)
#include "common.h"
#include <math.h>

namespace {

const uintptr_t kGWorld = 0x12B10A9C;
const uintptr_t kGNames = 0x12B2F0F4;
const int kWorldLevels = 0x34, kLevelActors = 0x28;
const int kActorLocation = 0xD8;
const int kPawnHealth = 0x35C;      // float (player 550, AI 100 on Ventilation Shafts)

uint8_t* g_pawns[128];
int g_pawnCount;
DWORD g_listAt;
float g_prevHead[3];
bool g_prevValid;
uint8_t* g_target;
DWORD g_targetAt;
volatile long g_debug;          // 1 = log the robots, 2 = bring g_debugActor in front of the hand
uint8_t* g_debugActor;

const uintptr_t kFarMoveActor = 0x11689630;   // UWorld::FarMoveActor(actor, &dest, bTest, bNoCheck, bAttachedMove)
typedef int(__thiscall* FarMoveFn)(void* world, void* actor, const Vec3f* dest, int test, int noCheck, int attached);

const wchar_t* NameOf(unsigned fname)
{
    uint8_t** names = *(uint8_t***)kGNames;
    int count = *(int*)(kGNames + 4);
    int i = (int)(fname >> 14);
    if (!names || i <= 0 || i >= count || !names[i]) return nullptr;
    return (const wchar_t*)(names[i] + 8);
}

// the other robots of the world's levels (refreshed twice a second)
void ListPawns(void* player)
{
    g_pawnCount = 0;
    __try {
        uint8_t* world = *(uint8_t**)kGWorld;
        uint8_t** levels = world ? *(uint8_t***)(world + kWorldLevels) : nullptr;
        int levelCount = world ? *(int*)(world + kWorldLevels + 4) : 0;
        for (int l = 0; levels && l < levelCount && l < 64; l++) {
            uint8_t* level = levels[l];
            if (!level) continue;
            uint8_t** actors = *(uint8_t***)(level + kLevelActors);
            int count = *(int*)(level + kLevelActors + 4);
            for (int i = 0; actors && i < count && i < 20000 && g_pawnCount < 128; i++) {
                uint8_t* a = actors[i];
                if (!a || a == player) continue;
                const wchar_t* name = NameOf(*(unsigned*)(a + 4));
                if (name && !wcsncmp(name, L"Tn", 2) && wcsstr(name, L"Pawn") && !wcsstr(name, L"Player")) g_pawns[g_pawnCount++] = a;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_pawnCount = 0;
    }
}

// distance from point p to the robot's body: a vertical segment (its height) - horizontal distance, and how far
// outside the height p is
bool Touches(const float* p, const Vec3f& at, float radius, float halfHeight)
{
    float dx = p[0] - at.x, dy = p[1] - at.y, dz = p[2] - at.z;
    return dx * dx + dy * dy < radius * radius && fabsf(dz) < halfHeight;
}

}   // namespace

// Game thread, every frame while a melee weapon is held. hand: the hand holding it; swinging: that hand moves faster
// than MeleeSpeed. True when the weapon hit a robot this frame (the attack starts now).
bool MeleeContact(int hand, bool swinging, DWORD now)
{
    void* player = IKPawn();
    float pos[3], fwd[3], rel[3];
    if (!player || !IKHandWorld(hand, pos, fwd, rel)) { g_prevValid = false; return false; }
    float reach = g_cfg.meleeReach * g_cfg.worldScale;
    float head[3] = { pos[0] + fwd[0] * reach, pos[1] + fwd[1] * reach, pos[2] + fwd[2] * reach };
    float prev[3] = { head[0], head[1], head[2] };
    if (g_prevValid) memcpy(prev, g_prevHead, sizeof(prev));
    memcpy(g_prevHead, head, sizeof(head));
    g_prevValid = true;
    long dbg = InterlockedExchange(&g_debug, 0);
    if (dbg == 1) {
        ListPawns(player);
        g_listAt = now;
        for (int i = 0; i < g_pawnCount; i++) {
            Vec3f at;
            const wchar_t* nm = NameOf(*(unsigned*)(g_pawns[i] + 4));
            if (!ReadActorLocation(g_pawns[i], at)) continue;
            float dx = at.x - pos[0], dy = at.y - pos[1], dz = at.z - pos[2];
            Log("melee debug: %p %ls health %.0f dist %.0f", g_pawns[i], nm ? nm : L"?", *(float*)(g_pawns[i] + kPawnHealth),
                sqrtf(dx * dx + dy * dy + dz * dz));
        }
    } else if (dbg == 2 && g_debugActor) {
        __try {
            float h = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1]);
            Vec3f dest = { pos[0], pos[1], pos[2] };
            if (h > 0.01f) { dest.x += fwd[0] / h * 200.0f; dest.y += fwd[1] / h * 200.0f; }
            int ok = ((FarMoveFn)kFarMoveActor)(*(void**)kGWorld, g_debugActor, &dest, 0, 1, 0);
            Log("melee debug: moved %p -> %.0f %.0f %.0f ok %d", g_debugActor, dest.x, dest.y, dest.z, ok);
        } __except (EXCEPTION_EXECUTE_HANDLER) { Log("melee debug: move failed"); }
    }
    if (!swinging) return false;
    if (now - g_listAt > 500) { ListPawns(player); g_listAt = now; }

    // the weapon's path this frame (head from the last frame to now) and its shaft (hand to head)
    const int kSteps = 8;
    float radius = g_cfg.meleeHitRadius, halfHeight = g_cfg.meleeHitRadius * 2.0f;
    for (int i = 0; i < g_pawnCount; i++) {
        uint8_t* a = g_pawns[i];
        Vec3f at;
        float health = 0;
        __try { health = *(float*)(a + kPawnHealth); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (health <= 0 || !ReadActorLocation(a, at)) continue;
        // the weapon's reach from the hand counts whichever way the weapon points in the hand (its forward is not
        // always the controller's): a swing with the robot within the weapon's length of the hand hits it
        bool nearHand = Touches(pos, at, radius + reach, halfHeight + reach * 0.5f);
        for (int s = 0; s <= kSteps; s++) {
            float t = (float)s / kSteps, p[3], q[3];
            for (int k = 0; k < 3; k++) { p[k] = prev[k] + (head[k] - prev[k]) * t; q[k] = pos[k] + (head[k] - pos[k]) * t; }
            if (nearHand || Touches(p, at, radius, halfHeight) || Touches(q, at, radius, halfHeight)) {
                if (g_target != a || now - g_targetAt > 600) Log("melee: weapon reached %p (health %.0f)", a, health);
                g_target = a;
                g_targetAt = now;
                return true;
            }
        }
    }
    return false;
}

// For a moment after a hit: the world yaw (Unreal units) from the player to the robot that was hit - the game's
// attack is aimed there.
bool MeleeTargetYaw(int& yaw)
{
    if (!g_target || GetTickCount() - g_targetAt > 400) return false;
    Vec3f me, it;
    if (!ReadActorLocation(IKPawn(), me) || !ReadActorLocation(g_target, it)) return false;
    float dx = it.x - me.x, dy = it.y - me.y;
    if (dx * dx + dy * dy < 1.0f) return false;
    yaw = (int)(atan2f(dy, dx) * 32768.0f / 3.14159265f) & 0xFFFF;
    return true;
}

// debug (applied on the next frame a melee weapon is held): 1 = log the robots, 2 = bring that robot in front of the hand
void MeleeDebug(int what, void* actor)
{
    g_debugActor = (uint8_t*)actor;
    g_debug = what;
}

// debug: the robot last hit and its health
void MeleeDebugTarget(void** target, float* health)
{
    *target = g_target;
    *health = 0;
    if (g_target) __try { *health = *(float*)((uint8_t*)g_target + kPawnHealth); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
