// First-person VR body: the robot's arms follow the controllers.
//
// The engine poses the player's skeleton (component-space bone matrices, SpaceBases) and then uses that pose for
// attachments (the weapons) and skinning. Right after the pose (hook 0x116350E0) the pose is rebuilt, once per
// stereo pair (the left-eye frame); the right-eye frame replays it:
//
//   1. Frame    - where the mesh will be drawn this frame (its LocalToWorld is still last frame's here).
//   2. Body     - spine and clavicles in the bind pose (no idle/walk sway at the roots of the arms), the mesh
//                 turned to where the head looks and kept under the head when you walk around the room.
//   3. Arms     - rigid, from the bind pose (both arms alike): each wrist goes where the real hand holds the
//                 controller (out of reach: the arm points at it), the shoulder pads turn half as far as the
//                 upper arm. The right arm holding the forearm-mounted gun is posed from
//                 the gun: the gun takes the controller's aim, the wrist sits in the real hand. The left hand
//                 holding the gun's front grip (two-handed) sits on the grip. A hand holding the melee weapon is
//                 a free hand (the weapon hangs off its weapon bone). The gun is the one of Pawn.Weapon among the
//                 mesh's attachments, taken from the game's own attachment of it (records of the game's values:
//                 the mod's placement is never mistaken for the game's); guns held before stay hidden where the
//                 game has them. The empty right hand wears the robot's arm model (the arm shell the game shows
//                 on a grenade throw) on the forearm as posed.
//   4. Look     - head hidden (the eyes are inside), bulky torso hidden, slimmer limbs, hand and weapon sizes.
//
// Abilities (spins, special attacks) are arm animations: for a moment after an ability button the arms blend to
// the game's animation and back. Bones are found by name, so other playable robots work too.
#include "common.h"
#include "vrmath.h"
#include "MinHook.h"

namespace {

// ---------------------------------------------------------------------------------------------
// Engine
namespace addr {
const uintptr_t kSkelUpdate = 0x116350E0;     // poses a SkeletalMeshComponent (local atoms -> SpaceBases, skel controls)
const uintptr_t kSetSemiTransparentOpacity = 0x11500FB0;   // AActor native: __stdcall(actor, opacity)
const uintptr_t kSetOpacityOverride = 0x11500F20;          // __stdcall(actor, opacity, flag): the camera's near fade
const uintptr_t kGNames = 0x12B2F0F4;         // TArray<FNameEntry*>, wide name at entry+8
}
const int kCompLocalToWorld = 0x50;  // FMatrix
const int kCompBounds = 0xBC;        // FBoxSphereBounds (world): origin, box extent, radius
const int kCompScale = 0x178;        // PrimitiveComponent.Scale
const int kCompSkelMesh = 0x1BC;     // SkeletalMeshComponent.SkeletalMesh
const int kCompSpaceBases = 0x1EC;   // TArray<FMatrix> component-space bone transforms
const int kCompAttachments = 0x260;  // TArray<FAttachment>, 0x30 each: component, bone FName, rel loc, rel rot, rel scale
const int kMeshRefSkeleton = 0x68;   // TArray<FMeshBone>, 0x40 each: +0 FName, +8 quat, +0x18 pos, +0x34 parent
const int kMeshBoneSize = 0x40;
const int kPawnMesh = 0x3F4;         // Pawn.Mesh
const int kPawnWeapon = 0x428;       // Pawn.Weapon
const int kWeaponMesh = 0x20C;       // the weapon's mesh components (first person / third person)
const int kWeaponMesh2 = 0x208;
const int kActorRotation = 0xE4;     // Actor.Rotation (pitch, yaw, roll)
const int kMaxBones = 400;

const wchar_t* NameOf(unsigned fname)
{
    unsigned idx = fname >> 14;
    uintptr_t arr = *(uintptr_t*)addr::kGNames;
    int num = *(int*)(addr::kGNames + 4);
    if (!arr || idx >= (unsigned)num) return nullptr;
    uintptr_t e = ((uintptr_t*)arr)[idx];
    return e ? (const wchar_t*)(e + 8) : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Math. A Pose is an FMatrix: rows = axes (with scale), then the origin; row vectors, point_parent = point * M.
struct Pose { Vec3 ax[3]; Vec3 pos; };

Vec3 Add(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec3 Sub(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Vec3 Scale(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
float Len(Vec3 a) { return sqrtf(Dot(a, a)); }
Vec3 Norm(Vec3 a) { float l = Len(a); return l > 1e-6f ? Scale(a, 1.0f / l) : Vec3{ 1, 0, 0 }; }
float Comp(const Vec3& v, int j) { return (&v.x)[j]; }
Vec3 InFrame(Vec3 v, const Vec3 f[3]) { return { Dot(v, f[0]), Dot(v, f[1]), Dot(v, f[2]) }; }
Vec3 FromFrame(Vec3 c, const Vec3 f[3]) { return Add(Add(Scale(f[0], c.x), Scale(f[1], c.y)), Scale(f[2], c.z)); }
Vec3 YawRotate(Vec3 v, float yaw) { float c = cosf(yaw), s = sinf(yaw); return { v.x * c - v.y * s, v.x * s + v.y * c, v.z }; }

void Orthonormalize(Vec3 a[3])
{
    a[0] = Norm(a[0]);
    a[2] = Norm(Cross(a[0], a[1]));
    a[1] = Cross(a[2], a[0]);
}

// Rotation (column-vector map) taking direction a onto direction b.
Mat3 RotFromTo(Vec3 a, Vec3 b)
{
    a = Norm(a); b = Norm(b);
    Vec3 v = Cross(a, b);
    float c = Dot(a, b);
    if (c < -0.9999f) {
        Vec3 p = Norm(fabsf(a.x) < 0.9f ? Cross(a, { 1, 0, 0 }) : Cross(a, { 0, 1, 0 }));
        Mat3 r;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) r.m[i][j] = 2.0f * Comp(p, i) * Comp(p, j) - (i == j ? 1.0f : 0.0f);
        return r;
    }
    float k = 1.0f / (1.0f + c);
    return { { { v.x * v.x * k + c, v.x * v.y * k - v.z, v.x * v.z * k + v.y },
               { v.y * v.x * k + v.z, v.y * v.y * k + c, v.y * v.z * k - v.x },
               { v.z * v.x * k - v.y, v.z * v.y * k + v.x, v.z * v.z * k + c } } };
}

// Rotation taking the orthonormal frame `from` (axes) onto the frame `to`.
Mat3 FrameToFrame(const Vec3 from[3], const Vec3 to[3])
{
    Mat3 r = {};
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) r.m[i][j] += Comp(to[k], i) * Comp(from[k], j);
    return r;
}

void RotatePose(Pose& p, const Mat3& r) { for (auto& a : p.ax) a = Mul(r, a); }

// Rotation by `angle` (radians) about the unit axis.
Mat3 AxisAngle(Vec3 a, float angle)
{
    float c = cosf(angle), s = sinf(angle), t = 1.0f - c;
    return { { { c + t * a.x * a.x, t * a.x * a.y - s * a.z, t * a.x * a.z + s * a.y },
               { t * a.y * a.x + s * a.z, c + t * a.y * a.y, t * a.y * a.z - s * a.x },
               { t * a.z * a.x - s * a.y, t * a.z * a.y + s * a.x, c + t * a.z * a.z } } };
}

// Scale a bone by `along` in direction dir and by `across` perpendicular to it.
void StretchThin(Pose& p, Vec3 dir, float along, float across)
{
    dir = Norm(dir);
    for (auto& a : p.ax) {
        Vec3 l = Scale(dir, Dot(a, dir));
        a = Add(Scale(l, along), Scale(Sub(a, l), across));
    }
}

void SetAxes(Pose& p, const Vec3 axes[3], float scale) { for (int k = 0; k < 3; k++) p.ax[k] = Scale(Norm(axes[k]), scale); }
void UnitAxes(const Pose& p, Vec3 out[3]) { for (int k = 0; k < 3; k++) out[k] = p.ax[k]; Orthonormalize(out); }

Pose Load(const float* m) { return { { { m[0], m[1], m[2] }, { m[4], m[5], m[6] }, { m[8], m[9], m[10] } }, { m[12], m[13], m[14] } }; }

void Store(const Pose& p, float* m)
{
    for (int k = 0; k < 3; k++) { m[k * 4] = p.ax[k].x; m[k * 4 + 1] = p.ax[k].y; m[k * 4 + 2] = p.ax[k].z; m[k * 4 + 3] = 0; }
    m[12] = p.pos.x; m[13] = p.pos.y; m[14] = p.pos.z; m[15] = 1;
}

Pose Compose(const Pose& a, const Pose& b)   // a, then b
{
    auto xf = [&](Vec3 v) { return Add(Add(Scale(b.ax[0], v.x), Scale(b.ax[1], v.y)), Scale(b.ax[2], v.z)); };
    return { { xf(a.ax[0]), xf(a.ax[1]), xf(a.ax[2]) }, Add(xf(a.pos), b.pos) };
}

Pose Inverse(const Pose& p)
{
    Vec3 c0 = Cross(p.ax[1], p.ax[2]), c1 = Cross(p.ax[2], p.ax[0]), c2 = Cross(p.ax[0], p.ax[1]);
    float det = Dot(p.ax[0], c0);
    if (fabsf(det) < 1e-12f) det = 1e-12f;
    float id = 1.0f / det;
    Pose r;
    r.ax[0] = { c0.x * id, c1.x * id, c2.x * id };
    r.ax[1] = { c0.y * id, c1.y * id, c2.y * id };
    r.ax[2] = { c0.z * id, c1.z * id, c2.z * id };
    r.pos = Scale(FromFrame(p.pos, r.ax), -1.0f);
    return r;
}

Vec3 PointTo(const Pose& m, Vec3 p) { return Add(FromFrame(p, m.ax), m.pos); }     // point through m
Vec3 DirTo(const Pose& m, Vec3 d) { return FromFrame(d, m.ax); }                   // direction through m

// FRotator (pitch, yaw, roll) as a Pose's axes.
Pose RotatorPose(const int* rot)
{
    Mat3 r = RotatorToMat(rot[0], rot[1], rot[2]);
    return { { r.col(0), r.col(1), r.col(2) }, { 0, 0, 0 } };
}

// ---------------------------------------------------------------------------------------------
// Input from the camera (game thread): hands in the body frame relative to the eye base, the body/head yaw, the
// head's room offset and the eye of the coming view.
struct Hand { bool valid; Vec3 localPos, localAx[3]; Vec3 pos, ax[3]; };   // pos/ax: world, placed per pose
Hand g_hand[2];
float g_bodyYaw, g_headYaw;
Vec3 g_headOffset;
int g_frameEye;
void* g_pawn;
DWORD g_inputTick;
Vec3 g_viewPos;
DWORD g_poseTick;    // last time the player's skeleton was posed

// ---------------------------------------------------------------------------------------------
// Skeleton (per SkeletalMesh)
struct Arm { int shoulder, elbow, hand, weapon, finger, thumb; };
struct Skeleton {
    void* mesh = nullptr;
    int n = 0;
    int parent[kMaxBones];
    Pose ref[kMaxBones];                 // bind pose relative to the parent
    bool bindPose[kMaxBones];            // spine/clavicles: kept in the bind pose
    bool armour[kMaxBones];              // armour plates on the upper arm and forearm (*Robo*): kept in the bind pose
    uint8_t group[kMaxBones];            // 1 = torso (hidden unless ShowBody=2), 2 = waist/legs (hidden with ShowBody=0)
    Arm arm[2];
    int fingers[2][4][2];                // per hand: index, middle, ring, thumb - base and tip bone (-1: none)
    bool armsOk = false;                 // false: no arms (vehicle form)
    int head = -1, hips = -1, chest = -1;   // chest: the lowest spine bone above the hips (the torso twists there)
    int thigh[2] = { -1, -1 }, knee[2] = { -1, -1 }, ankle[2] = { -1, -1 };
} g_sk;

bool Starts(const wchar_t* s, const wchar_t* p) { return !wcsncmp(s, p, wcslen(p)); }

// A RefSkeleton bone's bind pose relative to its parent.
Pose RefLocal(const uint8_t* b)
{
    const float* q = (const float*)(b + 8);
    const float* t = (const float*)(b + 0x18);
    float x = q[0], y = q[1], z = q[2], w = q[3];
    return { { { 1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y) },
               { 2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x) },
               { 2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y) } },
             { t[0], t[1], t[2] } };
}

bool ResolveSkeleton(uint8_t* mesh)
{
    Skeleton& k = g_sk;
    k.mesh = mesh;
    k.armsOk = false;
    uint8_t* bones = *(uint8_t**)(mesh + kMeshRefSkeleton);
    int n = *(int*)(mesh + kMeshRefSkeleton + 4);
    if (!bones || n <= 0 || n > kMaxBones) return false;
    k.n = n;
    k.head = k.hips = k.chest = -1;
    for (int s = 0; s < 2; s++) { k.arm[s] = { -1, -1, -1, -1, -1, -1 }; k.thigh[s] = k.knee[s] = k.ankle[s] = -1; }
    memset(k.fingers, 0xFF, sizeof(k.fingers));
    static const wchar_t* fingerNames[4] = { L"Finger02_Index0", L"Finger03_Middle0", L"Finger04_Ring0", L"Finger01_Thumb0" };
    static const wchar_t* side[2] = { L"L_", L"R_" };
    // (the shoulders - clavicles, shoulder armour - stay: without them the arms grew out of nothing)
    static const wchar_t* torso[] = { L"C_Spine02", L"C_Spine03", L"C_Spine04", L"Lumbar02Robo" };
    static const wchar_t* waist[] = { L"C_Spine01", L"Leg01_Thigh", L"HipsRobo", L"Lumbar01Robo" };
    static const wchar_t* bind[] = { L"C_Spine01", L"C_Spine02", L"C_Spine03", L"C_Spine04" };
    for (int i = 0; i < n; i++) {
        const uint8_t* b = bones + i * kMeshBoneSize;
        k.parent[i] = *(const int*)(b + 0x34);
        if (k.parent[i] < 0 || k.parent[i] >= i) k.parent[i] = -1;
        k.ref[i] = RefLocal(b);
        k.group[i] = 0;
        k.bindPose[i] = false;
        const wchar_t* nm = NameOf(*(const unsigned*)b);
        if (!nm) continue;
        if (Starts(nm, L"C_Spine00_Hips")) k.hips = i;
        if (Starts(nm, L"C_Spine04_Head_XB")) k.head = i;
        if (Starts(nm, L"C_Spine01") && k.chest < 0) k.chest = i;
        for (auto s : bind) if (Starts(nm, s)) k.bindPose[i] = true;
        if (wcsstr(nm, L"_Arm01_Clav_XB")) k.bindPose[i] = true;
        for (auto s : torso) if (wcsstr(nm, s)) k.group[i] = 1;
        for (auto s : waist) if (wcsstr(nm, s)) k.group[i] = 2;
        for (int s = 0; s < 2; s++) {
            if (!Starts(nm, side[s])) continue;
            const wchar_t* r = nm + 2;
            Arm& a = k.arm[s];
            if (Starts(r, L"Arm02_Shoulder_XB")) a.shoulder = i;
            if (Starts(r, L"Arm03_Elbow_XB")) a.elbow = i;
            if (Starts(r, L"Arm04_Hand_XB")) a.hand = i;
            if (Starts(r, L"Weapon01_Wpn")) a.weapon = i;
            if (Starts(r, L"Finger03_Middle01")) a.finger = i;
            if (Starts(r, L"Finger01_Thumb01")) a.thumb = i;
            for (int f = 0; f < 4; f++)
                if (Starts(r, fingerNames[f]) && (r[wcslen(fingerNames[f])] == L'1' || r[wcslen(fingerNames[f])] == L'2'))
                    k.fingers[s][f][r[wcslen(fingerNames[f])] - L'1'] = i;
            if (Starts(r, L"Leg01_Thigh_XB")) k.thigh[s] = i;
            if (Starts(r, L"Leg02_Knee_XB")) k.knee[s] = i;
            if (Starts(r, L"Leg03_Ankle_XB")) k.ankle[s] = i;
        }
    }
    // the arm armour: the game's weapon animations fold it away (the forearm looked bare with an empty hand or
    // some guns, until an attack unfolded it)
    for (int i = 0; i < n; i++) {
        k.armour[i] = false;
        int p = k.parent[i];
        const wchar_t* nm = NameOf(*(const unsigned*)(bones + i * kMeshBoneSize));
        if (p < 0 || !nm || !wcsstr(nm, L"Robo")) continue;
        for (int s = 0; s < 2; s++) if (p == k.arm[s].shoulder || p == k.arm[s].elbow) k.armour[i] = true;
    }
    k.armsOk = true;
    for (int s = 0; s < 2; s++) {
        const Arm& a = k.arm[s];
        if (a.shoulder < 0 || a.elbow < 0 || a.hand < 0 || k.parent[a.elbow] != a.shoulder || k.parent[a.hand] != a.elbow)
            k.armsOk = false;
    }
    Log("ik: skeleton %p, %d bones, arms %s (L %d/%d/%d R %d/%d/%d weapon %d) head %d", mesh, n, k.armsOk ? "found" : "NOT found",
        k.arm[0].shoulder, k.arm[0].elbow, k.arm[0].hand, k.arm[1].shoulder, k.arm[1].elbow, k.arm[1].hand, k.arm[1].weapon, k.head);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Attachments. Every skeletal mesh attached to the player's mesh (guns, the arm model, melee parts) has a record of
// its attachment exactly as the game made it - taken when the game attaches it (the attach hook in weapons.cpp) or
// when it is first seen, never re-read from the entry (that may hold our placement). Whatever the mod writes into
// an entry or a component's Scale goes back from the record when the mod lets go of it: when the gun is put away,
// switched, picked up or the arm model is no longer needed, and when leaving first person.
const int kCompOwner = 0x34;         // ActorComponent.Owner
const int kCompFlags = 0xF0;         // PrimitiveComponent bit flags
const uint32_t kHiddenGame = 0x4;
struct Att {
    uint8_t* comp;
    unsigned bone;                   // as the game attached it: bone, location, rotation, scale
    float loc[3], scale[3];
    int rot[3];
    unsigned serial;                 // a new one for every attach (a new capture of the gun)
    bool moved;                      // the entry holds our placement
    bool sized;                      // the component's Scale is ours
};
const int kMaxAtt = 32;
Att g_att[kMaxAtt];
int g_attCount;
uint8_t* g_attMesh;                  // the mesh the records are of
unsigned g_attSerial;
uint8_t* g_knownGuns[8];             // guns held lately (most recent first): hidden while on an arm but not in hand

// ---------------------------------------------------------------------------------------------
// The held gun: a skeletal mesh attached to the right arm (Transformers carry their guns on the forearm) - the mesh
// of the weapon in hand (Pawn.Weapon). Every frame it is placed anew from its record: nothing is measured from the
// animation (a weapon taken mid-animation, with the hand shrunk away, once left the gun ten times its size) and
// nothing builds up.
struct Gun {
    uint8_t* comp = nullptr;
    uint8_t* entry = nullptr;        // its attachment entry (this frame)
    unsigned serial = 0;             // the attachment it was captured from
    DWORD lastSeen = 0;
    int origSide = 1;                // the arm the game put it on
    Pose inHand;                     // gun relative to the hand, the wrist straight (unit axes; component units)
    float size = 1.0f;               // gun units -> component units (before the component's own Scale)
    Pose pose;                       // the gun as posed this frame (unit axes; component space)
    bool posed = false;
    bool held = false;               // in use: the gun arm is posed from it (also for a moment while switching)
    uint8_t* skelMesh = nullptr;     // to notice weapon changes
    bool scoped = false;             // a sniper rifle (ini ScopeWeapons)
    bool hasGrip = false;
    Vec3 grip;                       // front grip in gun space (gun units, unscaled)
} g_gun;
int g_gunSide = 1;              // the arm holding the gun (1 right; 0 left - VRGunHand)

// The arm model: the robot's whole forearm (CP_<robot>Arm_SKEL), which the game attaches to the right forearm while
// the gun is folded away (a grenade throw, a pickup, a melee attack). An empty right hand wears it, so it is a
// full arm and not the thin forearm the gun animations leave.
uint8_t* g_armComp;                  // found for this pawn (null: not yet)
uint8_t* g_armPawn;
uint8_t* g_armMesh;
bool g_armSearched;                  // looked for in the pawn once
bool g_armOurs;                      // the mod keeps it attached (the game's detach is refused)
bool g_armAttachedByUs;              // ... and detaches it itself when done
bool g_armWasHidden;
uint8_t* g_armEntry;                 // its attachment entry (this frame)
volatile bool g_armWanted;           // the right hand is empty (skeleton update, eye 0)
volatile DWORD g_armWantedTick;
unsigned g_armGameBone;              // where the game attaches it (for attaching it ourselves)
float g_armGameLoc[3], g_armGameScale[3];
int g_armGameRot[3];
bool g_armGameSaved;
bool g_armBindOff;                   // debug: armbind 0 - the plates as the game poses them

bool MatchesScopeList(const std::wstring& names)
{
    std::wstring hay = names, list = g_cfg.scopeWeapons;
    for (auto& ch : hay) ch = towlower(ch);
    for (auto& ch : list) ch = towlower(ch);
    size_t p = 0;
    while (p <= list.size()) {
        size_t q = list.find(L',', p);
        if (q == std::wstring::npos) q = list.size();
        std::wstring item = list.substr(p, q - p);
        item.erase(0, item.find_first_not_of(L' '));
        item.erase(item.find_last_not_of(L' ') + 1);
        if (item == L"*" || (!item.empty() && hay.find(item) != std::wstring::npos)) return true;
        p = q + 1;
    }
    return false;
}

void RestoreScale(uint8_t* comp)
{
    __try { *(float*)(comp + kCompScale) = 1.0f; } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

int BoneIndex(unsigned fname)
{
    uint8_t* bones = *(uint8_t**)((uint8_t*)g_sk.mesh + kMeshRefSkeleton);
    for (int i = 0; i < g_sk.n; i++) if ((*(unsigned*)(bones + i * kMeshBoneSize) >> 14) == (fname >> 14)) return i;
    return -1;
}

unsigned BoneName(int i) { return *(unsigned*)(*(uint8_t**)((uint8_t*)g_sk.mesh + kMeshRefSkeleton) + i * kMeshBoneSize); }

// The arm (0 left, 1 right) whose forearm, hand or weapon bone `b` is; -1: none.
int ArmOfBone(int b)
{
    for (int s = 0; s < 2; s++) if (b >= 0 && (b == g_sk.arm[s].elbow || b == g_sk.arm[s].hand || b == g_sk.arm[s].weapon)) return s;
    return -1;
}

// A bone's bind pose in component space.
Pose RefComponent(int b)
{
    Pose p = g_sk.ref[b];
    for (int i = g_sk.parent[b]; i >= 0; i = g_sk.parent[i]) p = Compose(p, g_sk.ref[i]);
    return p;
}

// ---- attachment records ----
uint8_t* EntryOf(uint8_t* mesh, uint8_t* comp)
{
    uint8_t* att = *(uint8_t**)(mesh + kCompAttachments);
    int count = *(int*)(mesh + kCompAttachments + 4);
    for (int i = 0; att && i < count && i < 64; i++) if (*(uint8_t**)(att + i * 0x30) == comp) return att + i * 0x30;
    return nullptr;
}

Att* FindAtt(const uint8_t* comp)
{
    for (int i = 0; comp && i < g_attCount; i++) if (g_att[i].comp == comp) return &g_att[i];
    return nullptr;
}

// The entry's values as the game's (only for an entry the mod has not written to).
void ReadAtt(Att* r, const uint8_t* e)
{
    r->bone = *(const unsigned*)(e + 4);
    memcpy(r->loc, e + 8, 12);
    memcpy(r->rot, e + 0x14, 12);
    memcpy(r->scale, e + 0x20, 12);
    r->serial = ++g_attSerial;
    r->moved = false;
}

Att* RecordAtt(uint8_t* comp, const uint8_t* e)
{
    if (g_attCount >= kMaxAtt) return nullptr;
    Att* r = &g_att[g_attCount++];
    r->comp = comp;
    r->sized = false;
    ReadAtt(r, e);
    return r;
}

void DropAtt(Att* r)
{
    for (auto& g : g_knownGuns) if (g == r->comp) g = nullptr;
    *r = g_att[--g_attCount];
}

void RestoreEntry(Att* r, uint8_t* e)
{
    if (!r->moved) return;
    if (e) {
        *(unsigned*)(e + 4) = r->bone;
        memcpy(e + 8, r->loc, 12);
        memcpy(e + 0x14, r->rot, 12);
        memcpy(e + 0x20, r->scale, 12);
    }
    r->moved = false;
}

void SetScale(Att* r, float s)
{
    *(float*)(r->comp + kCompScale) = s;
    r->sized = s != 1.0f;
}

// Everything the mod changed back as the game had it; `forget`: the records too (leaving first person).
void LetGoAll(bool forget)
{
    __try {
        for (int i = 0; i < g_attCount; i++) {
            Att& r = g_att[i];
            if (r.moved) RestoreEntry(&r, g_attMesh ? EntryOf(g_attMesh, r.comp) : nullptr);
            if (r.sized) { RestoreScale(r.comp); r.sized = false; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (forget) {
        g_attCount = 0;
        g_attMesh = nullptr;
        memset(g_knownGuns, 0, sizeof(g_knownGuns));
    }
}

// `want` (component space) into the entry, on bone b as posed: uniform axes there (on the slimmed forearm the
// attachment was sheared).
void WriteEntry(Att* r, uint8_t* e, const Pose& want, int b, const Pose* pose)
{
    Pose rel = Compose(want, Inverse(pose[b]));
    Vec3 ax[3];
    UnitAxes(rel, ax);
    Mat3 m;
    for (int i = 0; i < 3; i++) { m.m[i][0] = Comp(ax[0], i); m.m[i][1] = Comp(ax[1], i); m.m[i][2] = Comp(ax[2], i); }
    int* rot = (int*)(e + 0x14);
    MatToRotator(m, rot[0], rot[1], rot[2]);
    float* loc = (float*)(e + 8);
    float* scale = (float*)(e + 0x20);
    loc[0] = rel.pos.x; loc[1] = rel.pos.y; loc[2] = rel.pos.z;
    for (int k = 0; k < 3; k++) scale[k] = Len(rel.ax[k]);
    *(unsigned*)(e + 4) = BoneName(b);
    r->moved = true;
}

bool IsKnownGun(const uint8_t* c) { return c && std::find(g_knownGuns, g_knownGuns + 8, c) != g_knownGuns + 8; }

void AddKnownGun(uint8_t* c)
{
    int i = 0;
    while (i < 7 && g_knownGuns[i] != c) i++;
    memmove(g_knownGuns + 1, g_knownGuns, i * sizeof(uint8_t*));
    g_knownGuns[0] = c;
}

// The gun relative to the hand with the wrist straight (the bind pose from its bone to the hand), from the game's
// attachment: a gun on the forearm is an extension of it, the hand inside.
void CaptureGun(const Att* r)
{
    int b = BoneIndex(r->bone);
    g_gun.serial = r->serial;
    g_gun.origSide = ArmOfBone(b);
    const Arm& a = g_sk.arm[g_gun.origSide];
    Pose rel = RotatorPose(r->rot);
    rel.pos = { r->loc[0], r->loc[1], r->loc[2] };
    Pose in = Compose(rel, Compose(RefComponent(b), Inverse(RefComponent(a.hand))));
    Vec3 ax[3];
    UnitAxes(in, ax);
    g_gun.inHand = { { ax[0], ax[1], ax[2] }, in.pos };
    g_gun.size = fabsf(r->scale[0]) > 1e-3f ? fabsf(r->scale[0]) : 1.0f;
    Log("ik: gun %p on bone %d (%s, %s arm): %.1f %.1f %.1f from the hand, size %.2f", r->comp, b,
        b == a.elbow ? "forearm" : b == a.hand ? "hand" : "weapon bone", g_gun.origSide ? "right" : "left",
        in.pos.x, in.pos.y, in.pos.z, g_gun.size);
}

// The posed gun into its attachment entry, on the gun hand's bone.
bool PlaceGunEntry(const Pose* pose)
{
    Att* r = FindAtt(g_gun.comp);
    int hb = g_sk.arm[g_gunSide].hand;
    if (!r || !g_gun.entry || hb < 0) return false;
    Pose g = g_gun.pose;
    for (auto& a : g.ax) a = Scale(a, g_gun.size);
    WriteEntry(r, g_gun.entry, g, hb, pose);
    return true;
}

// Still a skeletal mesh component (as `mesh`): not freed or reused.
bool SameKind(const uint8_t* c, const uint8_t* mesh)
{
    __try { return c && *(const uintptr_t*)c == *(const uintptr_t*)mesh; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

const wchar_t* SkelName(const uint8_t* c)
{
    const uint8_t* skel = c ? *(uint8_t* const*)(c + kCompSkelMesh) : nullptr;
    const wchar_t* nm = skel ? NameOf(*(const unsigned*)(skel + 4)) : nullptr;
    return nm ? nm : L"?";
}

// ---- the arm model ----
// (memory of a pawn read while looking for it: only what is mapped and readable)
uintptr_t g_readLo, g_readHi;
bool Readable(const void* p, size_t n)
{
    uintptr_t a = (uintptr_t)p;
    if (a < 0x10000) return false;
    if (a >= g_readLo && a + n <= g_readHi) return true;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return false;
    g_readLo = (uintptr_t)mbi.BaseAddress;
    g_readHi = g_readLo + mbi.RegionSize;
    return a + n <= g_readHi;
}

// A skeletal mesh component of this pawn whose model is an arm (CP_SoundwaveArm_SKEL; not a weapon: WEP_...).
bool IsArmModel(uint8_t* c, uint8_t* pawn, uint8_t* mesh)
{
    if (!c || c == mesh || *(uintptr_t*)c != *(uintptr_t*)mesh) return false;
    uint8_t* owner = *(uint8_t**)(c + kCompOwner);
    if (owner && owner != pawn) return false;
    uint8_t* skel = *(uint8_t**)(c + kCompSkelMesh);
    const wchar_t* nm = skel ? NameOf(*(unsigned*)(skel + 4)) : nullptr;
    return nm && wcsstr(nm, L"Arm") && !Starts(nm, L"WEP");
}

bool ArmModelAt(uint8_t* c, uint8_t* pawn, uint8_t* mesh)
{
    if (!Readable(c, kCompSkelMesh + 4)) return false;
    uint8_t* skel = *(uint8_t**)(c + kCompSkelMesh);
    if (*(uintptr_t*)c != *(uintptr_t*)mesh || !Readable(skel, 8)) return false;
    return IsArmModel(c, pawn, mesh);
}

// The arm model among an object's properties (a component, or in an array of them); `at`: the offset found.
uint8_t* FindArmModelIn(uint8_t* obj, int size, uint8_t* pawn, uint8_t* mesh, int& at)
{
    g_readLo = g_readHi = 0;
    __try {
        for (int o = 0x40; o < size; o += 4) {
            if (!Readable(obj + o, 12)) break;
            at = o;
            uint8_t* p = *(uint8_t**)(obj + o);
            if (ArmModelAt(p, pawn, mesh)) return p;
            int n = *(int*)(obj + o + 4), max = *(int*)(obj + o + 8);
            if (n < 1 || n > 64 || max < n || max > 1024 || !Readable(p, n * 4)) continue;
            for (int i = 0; i < n; i++) {
                uint8_t* c = ((uint8_t**)p)[i];
                if (ArmModelAt(c, pawn, mesh)) return c;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

// The arm model before the game ever attached it: on the pawn, or on one of its inventory items (weapons).
uint8_t* FindArmModel(uint8_t* pawn, uint8_t* mesh)
{
    const int kPawnInvManager = 0x424, kInvChain = 0x1C8;
    int at = 0;
    if (uint8_t* c = FindArmModelIn(pawn, 0x1800, pawn, mesh, at)) {
        Log("ik: arm model %p (%ls) on the pawn +0x%X", c, SkelName(c), at);
        return c;
    }
    __try {
        uint8_t* inv = *(uint8_t**)(pawn + kPawnInvManager);
        uint8_t* item = inv ? *(uint8_t**)(inv + kInvChain) : nullptr;
        for (int k = 0; item && k < 32; k++, item = *(uint8_t**)(item + kInvChain)) {
            if (uint8_t* c = FindArmModelIn(item, 0x800, pawn, mesh, at)) {
                const wchar_t* in = NameOf(*(unsigned*)(item + 4));
                Log("ik: arm model %p (%ls) on %ls +0x%X", c, SkelName(c), in ? in : L"?", at);
                return c;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

bool ArmWanted() { return g_armWanted && GetTickCount() - g_armWantedTick < 300 && !IKPawnIsVehicle(); }

void ReleaseArmModel()
{
    g_armOurs = false;
    uint8_t* comp = g_armComp, *mesh = g_armMesh;
    if (!comp || !mesh) { g_armAttachedByUs = false; return; }
    uint8_t* e = EntryOf(mesh, comp);
    Att* r = mesh == g_attMesh ? FindAtt(comp) : nullptr;
    if (r) {
        RestoreEntry(r, e);
        if (r->sized) { RestoreScale(comp); r->sized = false; }
    }
    if (g_armAttachedByUs) {
        if (r) DropAtt(r);
        if (e) WeaponsDetachOriginal(mesh, comp);
        Log("ik: arm model %p taken off", comp);
    }
    if (g_armWasHidden) *(uint32_t*)(comp + kCompFlags) |= kHiddenGame;
    g_armAttachedByUs = false;
}

// The gun in hand among the mesh's attachments (the mesh of Pawn.Weapon, on an arm), the arm model's entry, and the
// sizes: the gun at WeaponScale while in use, hidden while the hand is empty or holds the melee weapon; guns held
// before and still on an arm (the game lets go of a gun only after its put-away animation, and a pickup swaps
// guns) back where the game has them, hidden.
void UpdateGun(uint8_t* mesh)
{
    DWORD now = GetTickCount();
    if (mesh != g_attMesh) {   // another mesh: its attachments are new
        g_attCount = 0;
        g_attMesh = mesh;
        memset(g_knownGuns, 0, sizeof(g_knownGuns));
        g_gun.comp = nullptr;
        g_gun.serial = 0;
    }
    // the gun hand: the right one, or the left when the gun was taken there
    g_gunSide = VRGunHand() == 0 ? 0 : 1;
    g_gun.entry = nullptr;
    g_gun.posed = false;
    g_armEntry = nullptr;
    bool use = WeaponsMeleeHeld() != g_gunSide && VRGunHand() >= 0;
    uint8_t* att = *(uint8_t**)(mesh + kCompAttachments);
    int count = *(int*)(mesh + kCompAttachments + 4);
    // the weapon in hand (Pawn.Weapon) and its mesh components
    uint8_t* weapon = *(uint8_t**)((uint8_t*)g_pawn + kPawnWeapon);
    uint8_t* mine[2] = { weapon ? *(uint8_t**)(weapon + kWeaponMesh) : nullptr, weapon ? *(uint8_t**)(weapon + kWeaponMesh2) : nullptr };
    // (a weapon picked up may be drawn with a component of an earlier one of its kind - the weapon still names its
    // own, unattached: the gun is then the latest attached of its model)
    uint8_t* model = mine[0] ? *(uint8_t**)(mine[0] + kCompSkelMesh) : mine[1] ? *(uint8_t**)(mine[1] + kCompSkelMesh) : nullptr;
    uint8_t* found = nullptr, *same = nullptr, *sameEntry = nullptr;
    unsigned sameSerial = 0;
    bool live[kMaxAtt] = {};
    for (int i = 0; att && i < count && i < 64; i++) {
        uint8_t* e = att + i * 0x30;
        uint8_t* c = *(uint8_t**)e;
        if (!c || *(uintptr_t*)c != *(uintptr_t*)mesh) continue;   // skeletal meshes only
        Att* r = FindAtt(c);
        if (!r) r = RecordAtt(c, e);
        if (!r) continue;
        live[r - g_att] = true;
        if (WeaponsIsMeleeComp(c)) continue;   // (the melee weapon in a hand: weapons.cpp)
        if (!g_armComp && g_armPawn == g_pawn && IsArmModel(c, (uint8_t*)g_pawn, mesh)) {
            g_armComp = c;
            Log("ik: arm model %p (%ls), attached by the game", c, SkelName(c));
        }
        if (c == g_armComp) { g_armEntry = e; continue; }
        if (ArmOfBone(BoneIndex(r->bone)) < 0) continue;
        if ((c == mine[0] || c == mine[1]) && !found) { found = c; g_gun.entry = e; continue; }
        if (model && *(uint8_t**)(c + kCompSkelMesh) == model && r->serial > sameSerial) { same = c; sameEntry = e; sameSerial = r->serial; }
        if (IsKnownGun(c)) { RestoreEntry(r, e); SetScale(r, 0.001f); }
    }
    if (!found && same) { found = same; g_gun.entry = sameEntry; }
    // records of what is no longer attached (taken off without the detach hook: its Scale back if still a component)
    for (int i = g_attCount - 1; i >= 0; i--) {
        if (live[i]) continue;
        if (g_att[i].sized && SameKind(g_att[i].comp, mesh)) RestoreScale(g_att[i].comp);
        DropAtt(&g_att[i]);
    }
    static uint8_t* loggedWeapon;   // a weapon in hand whose gun is not on an arm: what the mesh has (once per weapon)
    if (!found && weapon && weapon != loggedWeapon) {
        loggedWeapon = weapon;
        const wchar_t* wn = NameOf(*(unsigned*)(weapon + 4));
        Log("ik: weapon %p (%ls) - its gun not found on an arm: meshes %p %p, %d attachments", weapon, wn ? wn : L"?", mine[0], mine[1], count);
        for (int i = 0; att && i < count && i < 64; i++) {
            uint8_t* e = att + i * 0x30;
            uint8_t* c = *(uint8_t**)e;
            int b = c ? BoneIndex(*(const unsigned*)(e + 4)) : -1;
            const wchar_t* cn = c ? NameOf(*(unsigned*)(c + 4)) : nullptr;
            const wchar_t* bn = b >= 0 ? NameOf(BoneName(b)) : nullptr;
            Log("ik:   %d: %p %ls on bone %d %ls (arm %d)%s", i, c, cn ? cn : L"?", b, bn ? bn : L"?", ArmOfBone(b),
                c == mine[0] || c == mine[1] ? " <- the weapon's" : "");
        }
    }
    Att* r = found ? FindAtt(found) : nullptr;
    if (!r) {
        g_gun.comp = nullptr;
        g_gun.entry = nullptr;
    } else {
        AddKnownGun(found);
        // new, or attached anew by the game (switched back, picked up): captured from the game's attachment
        if (found != g_gun.comp || r->serial != g_gun.serial) {
            g_gun.comp = found;
            CaptureGun(r);
        }
        g_gun.lastSeen = now;
        SetScale(r, use ? g_cfg.weaponScale : 0.001f);
        // muzzle flashes: particle systems attached to the gun, right at the eyes; also gives the weapon's names
        uint8_t* fx = *(uint8_t**)(found + kCompAttachments);
        int fxCount = *(int*)(found + kCompAttachments + 4);
        uint8_t* skel = *(uint8_t**)(found + kCompSkelMesh);
        bool identify = skel != g_gun.skelMesh;
        std::wstring names;
        if (identify && skel) if (const wchar_t* nm = NameOf(*(unsigned*)(skel + 4))) names = nm;
        for (int i = 0; fx && i < fxCount && i < 16; i++) {
            uint8_t* e = fx + i * 0x30;
            uint8_t* c = *(uint8_t**)e;
            const wchar_t* nm = c ? NameOf(*(unsigned*)(c + 4)) : nullptr;
            if (!nm || !wcsstr(nm, L"ParticleSystemComponent")) continue;
            float* sc = (float*)(e + 0x20);
            sc[0] = sc[1] = sc[2] = use ? g_cfg.muzzleFlashScale : 0.001f;
            uint8_t* tmpl = *(uint8_t**)(c + 0x1B0);   // Template (MuzzleFlash_<weapon>_FX)
            const wchar_t* tn = identify && tmpl ? NameOf(*(unsigned*)(tmpl + 4)) : nullptr;
            if (tn) { names += L' '; names += tn; }
        }
        if (identify) {
            g_gun.skelMesh = skel;
            g_gun.scoped = MatchesScopeList(names);
            g_gun.hasGrip = false;
            Log("ik: weapon '%ls'%s", names.c_str(), g_gun.scoped ? " - sniper scope" : "");
        }
        if (!g_gun.hasGrip && use) {
            // front grip from the gun's world bounds: on the barrel line through the middle of the gun, a third of
            // the way from the middle to the muzzle
            Pose w = Load((float*)(found + kCompLocalToWorld));
            const float* b = (const float*)(found + kCompBounds);
            float s = Len(w.ax[0]);
            if (s > 1e-3f && b[3] > 1.0f) {
                Pose inv = Inverse(w);
                Vec3 center = PointTo(inv, { b[0], b[1], b[2] });
                Vec3 x = Norm(w.ax[0]);
                float half = fabsf(b[3] * x.x) + fabsf(b[4] * x.y) + fabsf(b[5] * x.z);   // box support along the barrel
                g_gun.grip = { center.x + half / s * 0.35f, center.y, center.z };
                g_gun.hasGrip = true;
            }
        }
    }
    // the gun arm is posed from the gun while it is in use - also for the moment between one gun and the next (no
    // free hand flashing up while switching)
    g_gun.held = use && (r || (g_gun.serial && now - g_gun.lastSeen < 150));
}

// ---------------------------------------------------------------------------------------------
// Arms

// Robot hand axes for a controller: fingers along the controller's forward axis, thumb up and a little inward
// (mirrored for the right hand), from the hand's own finger and thumb bones.
void HandAxesFor(int side, const Pose* pose, const Vec3 ctrl[3], Vec3 out[3])
{
    const Arm& a = g_sk.arm[side];
    Vec3 cur[3];
    UnitAxes(pose[a.hand], cur);
    if (a.finger < 0 || a.thumb < 0) { for (int k = 0; k < 3; k++) out[k] = ctrl[k]; return; }
    Vec3 inward = Scale(ctrl[1], side ? -0.3f : 0.3f);   // ctrl[1] = right
    Vec3 fW = ctrl[0], tW = Norm(Add(ctrl[2], inward));
    Vec3 fl = InFrame(Norm(Sub(pose[a.finger].pos, pose[a.hand].pos)), cur);
    Vec3 tl = InFrame(Norm(Sub(pose[a.thumb].pos, pose[a.hand].pos)), cur);
    Vec3 a1 = Norm(fl), a2 = Norm(Sub(tl, Scale(a1, Dot(tl, a1)))), a3 = Cross(a1, a2);
    Vec3 b1 = Norm(fW), b2 = Norm(Sub(tW, Scale(b1, Dot(tW, b1)))), b3 = Cross(b1, b2);
    for (int j = 0; j < 3; j++) out[j] = Add(Add(Scale(b1, Comp(a1, j)), Scale(b2, Comp(a2, j))), Scale(b3, Comp(a3, j)));
    Orthonormalize(out);
}

// The armour on the arm bones (ShoulderRobo*, ElbowRobo*) is placed from these frames, not from the slimmed bones:
// the shoulder pads turn half as far as the upper arm (they stay over the shoulder, as a deltoid does), the elbow
// plates go with the forearm, both at their full size.
Pose g_padFrame[2][2];      // per side: shoulder pads, elbow plates (component space)
bool g_padValid[2];

// The bend of an arm: the direction of its bone and the elbow's hinge axis (the same for both bones of a
// two-bone limb) as a frame.
void BendFrame(Vec3 along, Vec3 hinge, Vec3 out[3])
{
    out[0] = Norm(along);
    out[1] = Norm(Sub(hinge, Scale(out[0], Dot(hinge, out[0]))));
    out[2] = Cross(out[0], out[1]);
}

// Twist (radians) about the unit axis d taking frame a onto frame b (their axis 1 compared, across d).
float TwistAbout(Vec3 d, const Vec3 a[3], const Vec3 b[3])
{
    int k = fabsf(Dot(a[1], d)) < 0.9f ? 1 : 2;
    Vec3 p = Norm(Sub(a[k], Scale(d, Dot(a[k], d)))), q = Norm(Sub(b[k], Scale(d, Dot(b[k], d))));
    return atan2f(Dot(Cross(p, q), d), Dot(p, q));
}

// An arm, rigid, from the bind pose (both arms are mirror images there, so they look the same): the clavicle
// shrugs a little when the hand is raised, the wrist goes to `wrist` - always, out of reach too (the hand and the
// gun stay on the controller: the arm points at it straight, stretches a little and then its forearm up to
// kMaxReach; past that the hand leaves the arm behind, not the controller), the elbow bends down, out and back from
// the hand, the forearm takes half of the wrist's roll and the hand gets `handAx` at `handSize`. Both bones and the
// armour on them are ArmSize thick (the upper arm also LimbThickness). With `holdAx` the forearm is part of the gun
// instead (see SolveGunArm).
const float kMaxReach = 1.4f;
void SolveArm(int side, Pose* pose, Vec3 wrist, const Vec3 ctrl[3], const Vec3 handAx[3], float handSize,
              const Vec3* holdAx = nullptr)
{
    const Arm& a = g_sk.arm[side];
    int clav = g_sk.parent[a.shoulder];
    Vec3 up = { 0, 0, 1 };
    Vec3 right = clav >= 0 && g_sk.arm[1 - side].shoulder >= 0 ? Norm(Sub(pose[g_sk.arm[1].shoulder].pos, pose[g_sk.arm[0].shoulder].pos)) : ctrl[1];
    Vec3 out = Scale(right, side ? 1.0f : -1.0f);

    // the bind pose of the arm on the clavicle as the body has it
    Pose rs = Compose(g_sk.ref[a.shoulder], clav >= 0 ? pose[clav] : pose[a.shoulder]);
    Pose re = Compose(g_sk.ref[a.elbow], rs), rh = Compose(g_sk.ref[a.hand], re);
    float l1 = Len(Sub(re.pos, rs.pos)), l2 = Len(Sub(rh.pos, re.pos));

    // shrug: the hand above the shoulder lifts the clavicle (up to 20 degrees)
    if (clav >= 0) {
        Vec3 C = pose[clav].pos, c = Sub(rs.pos, C);
        float lift = fminf(fmaxf(Dot(Sub(wrist, rs.pos), up) / fmaxf(l1 + l2, 1.0f), 0.0f), 1.0f);
        Vec3 axis = Cross(c, up);
        if (lift > 0.0f && Len(axis) > 1e-3f) {
            Mat3 r = AxisAngle(Norm(axis), lift * 20.0f * 0.0174533f);
            RotatePose(pose[clav], r);
            for (Pose* p : { &rs, &re, &rh }) { RotatePose(*p, r); p->pos = Add(C, Mul(r, Sub(p->pos, C))); }
        }
        // reach: a hand near the end of the arm's reach draws the shoulder after it (forward, across, up - up to 25
        // degrees about the clavicle's root), so the whole shoulder follows the hand
        Vec3 toW = Sub(wrist, C);
        c = Sub(rs.pos, C);
        float reach = Len(Sub(wrist, rs.pos)) / fmaxf(l1 + l2, 1.0f);
        Vec3 axis2 = Cross(c, toW);
        if (reach > 0.8f && Len(axis2) > 1e-3f && Len(toW) > 1e-3f) {
            float want = acosf(fminf(fmaxf(Dot(Norm(c), Norm(toW)), -1.0f), 1.0f));
            float ang = fminf(want, fminf((reach - 0.8f) / 0.3f, 1.0f) * 25.0f * 0.0174533f);
            Mat3 r = AxisAngle(Norm(axis2), ang);
            RotatePose(pose[clav], r);
            for (Pose* p : { &rs, &re, &rh }) { RotatePose(*p, r); p->pos = Add(C, Mul(r, Sub(p->pos, C))); }
        }
    }
    Vec3 S = rs.pos;

    // the bind pose's bend: the elbow's side of the shoulder-wrist line, and the hinge
    Vec3 dir0 = Norm(Sub(rh.pos, S)), e0 = Sub(re.pos, S);
    Vec3 perp0 = Sub(e0, Scale(dir0, Dot(e0, dir0)));
    Vec3 hinge0 = Len(perp0) > 1e-3f ? Norm(Cross(perp0, dir0)) : Norm(Cross(out, dir0));
    Vec3 up0[3], fore0[3];
    BendFrame(e0, hinge0, up0);
    BendFrame(Sub(rh.pos, re.pos), hinge0, fore0);

    // the new bend
    Vec3 toT = Sub(wrist, S);
    Vec3 dir = Norm(toT);
    // still out of reach: the arm stretches a little (up to 10 %), so the hand gets there
    float stretch = fminf(fmaxf(Len(toT) / ((l1 + l2) * 0.999f), 1.0f), 1.1f);
    l1 *= stretch;
    l2 *= stretch;
    float dist = fminf(fmaxf(Len(toT), fabsf(l1 - l2) + 0.5f), (l1 + l2) * 0.999f);
    // pole: like a human elbow - behind the hand along the forearm, down, and out to the side
    Vec3 pole = Norm(Add(Add(Scale(ctrl[0], -0.6f), Scale(up, -0.8f)), Scale(out, 0.5f)));
    Vec3 perp = Sub(pole, Scale(dir, Dot(pole, dir)));
    if (Len(perp) < 1e-3f) perp = Sub(out, Scale(dir, Dot(out, dir)));
    perp = Norm(perp);
    float cosA = fminf(fmaxf((l1 * l1 + dist * dist - l2 * l2) / (2.0f * l1 * dist), -1.0f), 1.0f);
    float sinA = sqrtf(1.0f - cosA * cosA);
    Vec3 E = Add(S, Scale(Add(Scale(dir, cosA), Scale(perp, sinA)), l1));
    Vec3 H = wrist;   // (within reach S + dir * dist; out of reach the forearm takes the rest, below)
    Vec3 hinge = Norm(Cross(perp, dir));
    Vec3 up1[3], fore1[3];
    BendFrame(Sub(E, S), hinge, up1);
    BendFrame(Sub(H, E), hinge, fore1);

    Pose sh = rs, el = re;
    float L1 = l1 / stretch, L2 = l2 / stretch;   // (the bind pose's lengths)
    float stretch1 = stretch, stretch2 = fminf(fmaxf(Len(Sub(H, E)) / fmaxf(L2, 1e-3f), 0.6f), kMaxReach);
    Vec3 foreDir = Sub(H, E);
    if (holdAx) {
        // the gun arm: the forearm turns into the gun from the elbow (the game hangs the gun on it, its back end
        // over the forearm) - fixed to it as the game has it, the wrist straight, the wrist at `wrist`. The elbow
        // goes where that puts it; the upper arm gives (0.6..kMaxReach of its length) and beyond that it falls
        // short of the gun, which stays on the controller.
        Vec3 rhAx[3];
        UnitAxes(rh, rhAx);
        Vec3 d = Norm(FromFrame(InFrame(Norm(Sub(rh.pos, re.pos)), rhAx), holdAx));   // the forearm, turned with the gun
        Vec3 v = Sub(Sub(wrist, Scale(d, L2)), S);
        float len = Len(v);
        float k = fminf(fmaxf(len, 0.6f * L1), kMaxReach * L1);
        E = Add(S, Scale(len > 1e-3f ? Scale(v, 1.0f / len) : perp, k));
        foreDir = d;
        Vec3 dirN = Norm(Sub(H, S)), eN = Sub(E, S);
        Vec3 perpN = Sub(eN, Scale(dirN, Dot(eN, dirN)));
        BendFrame(eN, Len(perpN) > 1e-3f ? Norm(Cross(perpN, dirN)) : hinge, up1);
        RotatePose(sh, FrameToFrame(up0, up1));
        RotatePose(el, FrameToFrame(rhAx, holdAx));
        el.pos = E;
        stretch1 = k / fmaxf(L1, 1e-3f);
        stretch2 = 1.0f;
    } else {
        RotatePose(sh, FrameToFrame(up0, up1));
        RotatePose(el, FrameToFrame(fore0, fore1));
        el.pos = E;
        // the forearm rolls half of the way the hand rolls past it
        Pose rigidHand = rh;
        RotatePose(rigidHand, FrameToFrame(fore0, fore1));
        Vec3 hr[3];
        UnitAxes(rigidHand, hr);
        float roll = TwistAbout(fore1[0], hr, handAx);
        RotatePose(el, AxisAngle(fore1[0], roll * 0.5f));
    }

    // the armour's frames: elbow plates on the forearm; shoulder pads halfway between the bind and the upper arm
    Vec3 mid[3] = { Add(up0[0], up1[0]), Add(up0[1], up1[1]), {} };
    Pose pad = rs;
    if (Len(mid[0]) > 0.2f && Len(mid[1]) > 0.2f) {
        BendFrame(mid[0], mid[1], mid);
        RotatePose(pad, FrameToFrame(up0, mid));
    } else {
        pad = sh;
    }
    // (the armour and the arm model on the empty hand at ArmSize across the bone, at their full length)
    float thick = fminf(fmaxf(g_cfg.armSize, 0.1f), 2.0f);
    g_padFrame[side][0] = pad;
    g_padFrame[side][1] = el;
    StretchThin(g_padFrame[side][0], Sub(E, S), 1.0f, thick);
    StretchThin(g_padFrame[side][1], foreDir, 1.0f, thick);
    g_padValid[side] = true;

    StretchThin(sh, Sub(E, S), stretch1, g_cfg.limbThickness * thick);
    StretchThin(el, foreDir, stretch2, thick);
    if (holdAx) {
        // the gun replaces the forearm and the hand (in the game nothing of them is left past the elbow): shrunk to
        // a point at the elbow and inside the gun, not to stick out of it
        const float kGone = 0.01f;
        for (auto& ax : el.ax) ax = Scale(ax, kGone);
        handSize = kGone;
    }
    pose[a.shoulder] = sh;
    pose[a.elbow] = el;
    pose[a.hand].pos = H;
    SetAxes(pose[a.hand], handAx, handSize);
}

// A free hand (empty, holding the gun's front grip, or holding the melee weapon): the hand takes the
// controller's orientation and the wrist goes where the real hand is.
void SolveFreeArm(int side, Pose* pose, Vec3 target, const Vec3 ctrl[3])
{
    Vec3 want[3];
    HandAxesFor(side, pose, ctrl, want);
    SolveArm(side, pose, target, ctrl, want, g_cfg.handScale);
}

// A leg of a crouching body: the hip went down, the ankle stays at `ankle` (its orientation too: the foot stays
// flat) and the knee bends forward.
void CrouchLeg(int side, Pose* pose, Vec3 ankle, Vec3 fwd, bool* driven)
{
    int t = g_sk.thigh[side], k = g_sk.knee[side], a = g_sk.ankle[side];
    if (g_sk.parent[k] != t || g_sk.parent[a] != k) return;
    Vec3 T = pose[t].pos, K = pose[k].pos, A = pose[a].pos;
    float l1 = Len(Sub(K, T)), l2 = Len(Sub(A, K));
    Vec3 dir = Norm(Sub(ankle, T));
    float dist = fminf(fmaxf(Len(Sub(ankle, T)), fabsf(l1 - l2) + 0.5f), l1 + l2 - 0.05f);
    Vec3 perp = Sub(fwd, Scale(dir, Dot(fwd, dir)));
    if (Len(perp) < 1e-3f) return;
    perp = Norm(perp);
    float cosA = fminf(fmaxf((l1 * l1 + dist * dist - l2 * l2) / (2.0f * l1 * dist), -1.0f), 1.0f);
    Vec3 K2 = Add(T, Scale(Add(Scale(dir, cosA), Scale(perp, sqrtf(1.0f - cosA * cosA))), l1));
    Vec3 A2 = Add(T, Scale(dir, dist));
    Mat3 r1 = RotFromTo(Sub(K, T), Sub(K2, T));
    RotatePose(pose[t], r1);
    RotatePose(pose[k], Mul(RotFromTo(Mul(r1, Sub(A, K)), Sub(A2, K2)), r1));
    pose[k].pos = K2;
    pose[a].pos = A2;
    driven[t] = driven[k] = driven[a] = true;
}

// Fingers of a hand the player controls (not the gun hand, which the game animates around the grip): from the
// bind pose (open hand) bent towards the palm by curl[] (0..1: index, middle, ring, thumb). The palm side is
// where the thumb sits. Marks the bones as posed.
void PoseFingers(int side, Pose* pose, const float curl[4], bool* driven)
{
    const auto& fb = g_sk.fingers[side];
    int hand = g_sk.arm[side].hand;
    if (hand < 0 || fb[0][0] < 0 || fb[1][0] < 0 || fb[2][0] < 0 || fb[3][0] < 0) return;
    for (int f = 0; f < 4; f++) if (g_sk.parent[fb[f][0]] != hand) return;
    // in the hand's own frame: along the fingers, across the knuckles, and the palm normal
    Vec3 along = Norm(g_sk.ref[fb[1][0]].pos);
    Vec3 across = Norm(Sub(g_sk.ref[fb[0][0]].pos, g_sk.ref[fb[2][0]].pos));
    Vec3 palm = Norm(Cross(along, across));
    if (Dot(palm, g_sk.ref[fb[3][0]].pos) < 0.0f) palm = Scale(palm, -1.0f);
    palm = Scale(palm, g_cfg.fingerPalmSign < 0 ? -1.0f : 1.0f);
    const float kBase = 75.0f * 0.0174533f, kTip = 85.0f * 0.0174533f, kThumbBase = 35.0f * 0.0174533f, kThumbTip = 50.0f * 0.0174533f;
    // straight fingers: the bind pose has them spread and drooping (a pointing finger looked crooked), so each
    // finger is first laid flat along the hand (wrist -> middle knuckle), its tip in line with its base
    auto boneDir = [&](int f) { return fb[f][1] >= 0 ? Norm(DirTo(g_sk.ref[fb[f][0]], g_sk.ref[fb[f][1]].pos)) : along; };
    Vec3 straight = Norm(Sub(along, Scale(palm, Dot(along, palm))));
    for (int f = 0; f < 4; f++) {
        int b1 = fb[f][0], b2 = fb[f][1];
        float c = fminf(fmaxf(curl[f], 0.0f), 1.0f);
        Vec3 dir = f == 3 ? boneDir(3) : straight;
        Vec3 axis = Norm(Cross(dir, palm));   // turning dir about it moves it towards the palm
        Pose l1 = g_sk.ref[b1];
        if (f != 3) RotatePose(l1, RotFromTo(boneDir(f), straight));
        RotatePose(l1, AxisAngle(axis, c * (f == 3 ? kThumbBase : kBase)));
        pose[b1] = Compose(l1, pose[hand]);
        driven[b1] = true;
        if (b2 < 0 || g_sk.parent[b2] != b1) continue;
        Vec3 u[3];
        UnitAxes(l1, u);
        Pose l2 = g_sk.ref[b2];
        if (f != 3) for (int k = 0; k < 3; k++) l2.ax[k] = Scale(Vec3{ k == 0 ? 1.0f : 0, k == 1 ? 1.0f : 0, k == 2 ? 1.0f : 0 }, Len(l2.ax[k]));
        RotatePose(l2, AxisAngle(Norm(InFrame(axis, u)), c * (f == 3 ? kThumbTip : kTip)));
        pose[b2] = Compose(l2, pose[b1]);
        driven[b2] = true;
    }
}

// The arm carrying the gun, as the game has it: the forearm turns into the gun from the elbow (the gun hangs on the
// forearm, its back end over it) - the gun aimed as the controller is (barrel = forward, its up = up) and held where
// the real hand is, the forearm fixed to it (wrist straight), the upper arm reaching that elbow. The hand is a fist
// on the gun's grip, at its full size.
void SolveGunArm(Pose* pose, Vec3 target, const Vec3 ctrl[3])
{
    const Arm& a = g_sk.arm[g_gunSide];
    bool mirrored = g_gunSide != g_gun.origSide;   // (taken into the other hand)
    Pose handInGun = Inverse(g_gun.inHand);
    // the gun is drawn at WeaponScale about its origin: the hand stays on its grip
    Vec3 hp = Scale(handInGun.pos, g_cfg.weaponScale);
    if (mirrored) hp.y = -hp.y;
    Vec3 holdAx[3], handAx[3];
    HandAxesFor(g_gunSide, pose, ctrl, handAx);
    if (mirrored) {
        for (int j = 0; j < 3; j++) holdAx[j] = handAx[j];
    } else {
        for (int j = 0; j < 3; j++) holdAx[j] = FromFrame(handInGun.ax[j], ctrl);
        Orthonormalize(holdAx);
    }
    SolveArm(g_gunSide, pose, target, ctrl, handAx, g_cfg.handScale, holdAx);
    g_gun.pose = { { ctrl[0], ctrl[1], ctrl[2] }, Sub(pose[a.hand].pos, FromFrame(hp, ctrl)) };
    g_gun.posed = true;
}

// The gun in component space as posed, at the size it is drawn (component units per gun unit).
Pose GunInComponent()
{
    Pose g = g_gun.pose;
    for (auto& a : g.ax) a = Scale(a, g_gun.size * g_cfg.weaponScale);
    return g;
}

// The arm model on the empty right hand: where the game attaches it on the forearm, relative to the forearm as
// posed at its full thickness (the elbow plates' frame; the bone itself is slimmed), written onto the hand bone
// (not sheared). The plates in the bind pose (the mod's empty hand) run along the model's +x from its root, and the
// game attaches the root to the elbow bone, whose +x points back at the shoulder: the shell turned half about z lies
// on the forearm, its striped face up like the left forearm's.
bool PlaceArmModel(const Pose* pose)
{
    Att* r = FindAtt(g_armComp);
    int hb = g_sk.arm[1].hand;
    if (!r || !g_armEntry || hb < 0) return false;
    if (BoneIndex(r->bone) != g_sk.arm[1].elbow) { RestoreEntry(r, g_armEntry); return false; }
    Pose rel = RotatorPose(r->rot);
    for (int k = 0; k < 3; k++) rel.ax[k] = Scale(rel.ax[k], r->scale[k]);
    rel.pos = { r->loc[0], r->loc[1], r->loc[2] };
    if (g_armOurs && !g_armBindOff) {
        Pose t = { { { -1, 0, 0 }, { 0, -1, 0 }, { 0, 0, 1 } }, { 0, 0, 0 } };
        rel = Compose(t, rel);
    }
    WriteEntry(r, g_armEntry, Compose(rel, g_padFrame[1][1]), hb, pose);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Frame state shared with the right-eye replay and with vr.cpp
Pose g_frozen[kMaxBones];
int g_frozenCount;
float g_robotSpan;              // the robot's arm span (world units), 0 until measured
float g_robotEye;               // the robot's eye height above its feet (world units)
Pose g_frozenL2W;
Vec3 g_frozenPawn;
Pose g_gunWorld;               // the gun in the world as last posed (front grip for the two-handed hold)
bool g_gunWorldValid;
volatile long g_armDump;        // debug "armdump": the right arm's bones as animated, once
Vec3 g_gripWorld;               // the right hand target (world) when it was posed
Vec3 g_ctrlWorld[3];            // the right controller's axes then

// LocalToWorld the engine is about to give the component: the pawn's current location and yaw, with the mesh's
// offset from the pawn measured from the previous frame (LocalToWorld is still last frame's at this point).
Pose PredictComponentToWorld(uint8_t* comp)
{
    static Vec3 prevPawn;
    static bool prevValid;
    Pose m = Load((float*)(comp + kCompLocalToWorld));
    Vec3f pl;
    if (!ReadActorLocation(g_pawn, pl)) return m;
    Vec3 now = { pl.x, pl.y, pl.z };
    Vec3 offset = prevValid ? Sub(m.pos, prevPawn) : Vec3{ 0, 0, m.pos.z - now.z };
    if (Len(offset) > 400.0f) offset = { 0, 0, m.pos.z - now.z };   // teleport / respawn
    float yaw = *(int*)((uint8_t*)g_pawn + kActorRotation + 4) * kUURotToRad;
    float s = Len(m.ax[0]);
    m.ax[0] = { cosf(yaw) * s, sinf(yaw) * s, 0 };
    m.ax[1] = { -sinf(yaw) * s, cosf(yaw) * s, 0 };
    m.ax[2] = { 0, 0, s };
    m.pos = Add(now, offset);
    prevPawn = now;
    prevValid = true;
    return m;
}

void* volatile g_beltComp;
volatile float g_beltSize;   // its size in the world (0 = like the weapons in hand)

// The belt model (grenades.cpp) hangs on the hips bone: its attachment offset is set so it sits at the left front
// of the belt where the hand takes grenades - under the eyes, turned with the body - whatever the hips do.
// The middle of what is drawn goes there, not the model's origin (the pickup model's is ~90 units off to a side).
void PlaceBelt(uint8_t* comp, const Pose* pose, const Pose& compToWorld)
{
    uint8_t* belt = (uint8_t*)g_beltComp;
    if (!belt || g_sk.hips < 0) return;
    static uint8_t* measured;
    static int frames;
    static Vec3 centre;   // the middle of its bounds in its own frame (model units)
    if (measured != belt) { measured = belt; frames = 0; centre = {}; }
    if (frames >= 0 && ++frames > 3) {   // (once it has been drawn where it is placed)
        Pose m = Load((const float*)(belt + kCompLocalToWorld));
        const float* b = (const float*)(belt + kCompBounds);
        float ms = Len(m.ax[0]);
        Vec3 d = Sub({ b[0], b[1], b[2] }, m.pos);
        if (b[6] > 1.0f && ms > 1e-3f && Len(d) < b[6] * 2.0f + 50.0f) {
            Vec3 u[3];
            UnitAxes(m, u);
            centre = Scale(InFrame(d, u), 1.0f / ms);
            frames = -1;
            Log("ik: belt model %p drawn %.1f %.1f %.1f from its origin (radius %.0f), centred", belt, centre.x, centre.y, centre.z, b[6] / ms);
        } else if (frames > 200) frames = -1;   // no bounds: its origin then
    }
    float size = g_beltSize > 0.0f ? g_beltSize : g_cfg.weaponScale;
    uint8_t* att = *(uint8_t**)(comp + kCompAttachments);
    int count = *(int*)(comp + kCompAttachments + 4);
    for (int i = 0; att && i < count && i < 64; i++) {
        uint8_t* e = att + i * 0x30;
        if (*(uint8_t**)e != belt) continue;
        float ws = g_cfg.worldScale;
        const float* b = g_cfg.beltOffset;   // right, up, forward (m) -> Unreal x forward, y right, z up
        Vec3 at = Add(g_viewPos, YawRotate({ b[2] * ws, b[0] * ws, b[1] * ws }, g_bodyYaw + g_headYaw));
        Pose hips = Compose(pose[g_sk.hips], compToWorld);
        Vec3 u[3];
        UnitAxes(hips, u);
        float s = fmaxf(Len(hips.ax[0]), 1e-3f);
        // (attached unturned: its axes are the hips')
        Vec3 rel = Sub(Scale(InFrame(Sub(at, hips.pos), u), 1.0f / s), Scale(centre, size / s));
        float* loc = (float*)(e + 8);
        loc[0] = rel.x; loc[1] = rel.y; loc[2] = rel.z;
        // its size the same whatever the hips' scale
        *(float*)(belt + kCompScale) = size / s;
        static DWORD logAt;
        if (g_cfg.logFps && GetTickCount() - logAt > 5000) {
            logAt = GetTickCount();
            const float* l2w = (const float*)(belt + kCompLocalToWorld);
            Log("ik: belt want %.0f %.0f %.0f (eye %.0f %.0f %.0f), hips %.0f %.0f %.0f scale %.2f, rel %.1f %.1f %.1f, drawn at %.0f %.0f %.0f size %.2f",
                at.x, at.y, at.z, g_viewPos.x, g_viewPos.y, g_viewPos.z, hips.pos.x, hips.pos.y, hips.pos.z, s, rel.x, rel.y, rel.z,
                l2w[12], l2w[13], l2w[14], sqrtf(l2w[0] * l2w[0] + l2w[1] * l2w[1] + l2w[2] * l2w[2]));
        }
        return;
    }
}

void OnSkelPose(uint8_t* comp)
{
    static long calls, posed;
    static DWORD t0;
    calls++;
    if (g_cfg.logFps && GetTickCount() - t0 > 5000) {
        Log("ik: %ld skeleton updates, %ld arm poses in 5 s (pawn %p)", calls, posed, g_pawn);
        calls = posed = 0;
        t0 = GetTickCount();
    }
    if (!g_pawn || GetTickCount() - g_inputTick > 250) return;
    __try {
        if (*(uint8_t**)((uint8_t*)g_pawn + kPawnMesh) != comp) return;
        uint8_t* skel = *(uint8_t**)(comp + kCompSkelMesh);
        if (!skel) return;
        if (skel != g_sk.mesh && !ResolveSkeleton(skel)) return;
        float* bases = *(float**)(comp + kCompSpaceBases);
        int n = *(int*)(comp + kCompSpaceBases + 4);
        if (!bases || n != g_sk.n) return;
        g_poseTick = GetTickCount();
        if (!g_sk.armsOk) { LetGoAll(false); return; }   // vehicle form: drawn as the game poses it

        // ---- 1. frame ----
        Pose compToWorld = PredictComponentToWorld(comp);
        Pose worldToComp = Inverse(compToWorld);
        Vec3f pl;
        if (!ReadActorLocation(g_pawn, pl)) return;
        Vec3 pawnNow = { pl.x, pl.y, pl.z };
        // In AFR (StereoPerFrame=0), the right-eye frame replays the left-eye pose (same moment of the game), moved
        // with the pawn in case something carries it between the two frames (lifts). In StereoPerFrame=1, both eyes
        // draw from this single tick, so every tick computes a fresh pose and updates belt/gun world transforms.
        int eye = (g_cfg.stereoPerFrame && GameSingleThreadRender()) ? 0 : (g_frameEye ^ 1);
        if (eye == 1 && g_frozenCount == n) {
            Pose l2w = g_frozenL2W;
            l2w.pos = Add(l2w.pos, Sub(pawnNow, g_frozenPawn));
            Pose toComp = Compose(l2w, worldToComp);
            for (int i = 0; i < n; i++) Store(Compose(g_frozen[i], toComp), bases + i * 16);
            g_frozenCount = 0;
            posed++;
            return;
        }

        static Pose anim[kMaxBones], pose[kMaxBones];
        static bool moved[kMaxBones], driven[kMaxBones];
        for (int i = 0; i < n; i++) { anim[i] = pose[i] = Load(bases + i * 16); moved[i] = driven[i] = false; }

        // hand targets in the world, from the pawn's position now and the latched yaw (as the camera does)
        float baseFwd, baseUp;
        CameraBaseOffset(baseFwd, baseUp);   // the eye base as the camera has it (glides when transforming)
        Vec3 base = Add({ pawnNow.x, pawnNow.y, pawnNow.z + baseUp }, YawRotate({ baseFwd, 0, 0 }, g_bodyYaw));
        for (int s = 0; s < 2; s++) {
            Hand& h = g_hand[s];
            h.pos = Add(base, YawRotate(h.localPos, g_bodyYaw));
            for (int k = 0; k < 3; k++) h.ax[k] = YawRotate(h.localAx[k], g_bodyYaw);
        }

        // ---- 2. body ----
        // spine and clavicles in the bind pose, standing on the animated hips' position
        if (g_sk.hips >= 0) {
            Pose hips = Compose(g_sk.ref[g_sk.hips], g_sk.ref[0]);
            hips.pos = anim[g_sk.hips].pos;
            for (int i = 0; i < n; i++) {
                int p = g_sk.parent[i];
                if (p < 0) continue;
                if (g_sk.bindPose[i]) pose[i] = Compose(g_sk.ref[i], p == g_sk.hips ? hips : pose[p]);
                else if (moved[p]) pose[i] = Compose(Compose(anim[i], Inverse(anim[p])), pose[p]);
                else continue;
                moved[i] = true;
            }
        }
        // the body turns in two parts, as VR body IK usually does (VRIK, VRChat), and stays under the head when you
        // walk around the room:
        //  - the pelvis and the feet stay where they stand while the head looks around; once the head is more than
        //    50 degrees away (75 with both hands held out in front - aiming) the pelvis steps round to the head, and
        //    the head never gets more than 90 degrees away from it; walking (stick or room) lines it up at once
        //  - the chest twists over the pelvis toward where the hands are (between them, in front); without hands, a
        //    little with the head (12% of its angle up to 30 degrees, 45% beyond); at most 60 degrees
        float crouch = 0.0f;   // how far the body went down (component units)
        static float pelvisYaw, chestYaw;   // relative to g_bodyYaw, as g_headYaw
        if (eye == 0) {
            const float deg = 0.0174533f, pi2 = 6.2831853f;
            static DWORD torsoAt;
            static bool stepping;
            static float walkSpeed;
            static Vec3 lastHead;
            DWORD nowMs = GetTickCount();
            float dt = torsoAt ? fminf((nowMs - torsoAt) / 1000.0f, 0.1f) : 1.0f;
            torsoAt = nowMs;
            // walking: the head moving over the ground (the pawn and the player in the room both count)
            Vec3 headW = Add(pawnNow, YawRotate({ g_headOffset.x, g_headOffset.y, 0.0f }, g_bodyYaw));
            float speed = dt > 1e-3f ? hypotf(headW.x - lastHead.x, headW.y - lastHead.y) / dt : 0.0f;
            lastHead = headW;
            walkSpeed += (speed - walkSpeed) * (1.0f - expf(-dt * 10.0f));
            bool walking = walkSpeed > 0.6f * g_cfg.worldScale;
            Vec3 sum = {};
            int count = 0, ahead = 0;
            for (int s = 0; s < 2; s++) {
                if (!g_hand[s].valid) continue;
                Vec3 d = Sub(g_hand[s].localPos, { g_headOffset.x, g_headOffset.y, g_hand[s].localPos.z });
                sum = Add(sum, d);
                count++;
                if (d.x * cosf(g_headYaw) + d.y * sinf(g_headYaw) > 0.3f * g_cfg.worldScale) ahead++;
            }
            float off = remainderf(g_headYaw - pelvisYaw, pi2);
            if (walking || fabsf(off) > (ahead == 2 ? 75.0f : 50.0f) * deg) stepping = true;
            if (stepping) {
                pelvisYaw += off * (1.0f - expf(-dt * (walking ? 10.0f : 6.0f)));
                off = remainderf(g_headYaw - pelvisYaw, pi2);
                if (!walking && fabsf(off) < 3.0f * deg) stepping = false;
            }
            off = fminf(fmaxf(off, -90.0f * deg), 90.0f * deg);
            pelvisYaw = g_headYaw - off;
            float a = fabsf(off);
            float target = pelvisYaw + copysignf(a < 30.0f * deg ? 0.12f * a : 0.12f * 30.0f * deg + 0.45f * (a - 30.0f * deg), off);
            if (count && sum.x * sum.x + sum.y * sum.y > 1e-6f) {
                float handYaw = atan2f(sum.y, sum.x);
                // hands far behind (reloading over the shoulder...) do not twist the body round
                if (fabsf(remainderf(handYaw - g_headYaw, pi2)) < 1.6f) target = handYaw;
            }
            const float maxTwist = 60.0f * deg;
            float t = remainderf(target - pelvisYaw, pi2);
            target = pelvisYaw + fminf(fmaxf(t, -maxTwist), maxTwist);
            chestYaw += remainderf(target - chestYaw, pi2) * (1.0f - expf(-dt * 8.0f));
            // (always within the twist limit, also when the pelvis steps fast)
            t = remainderf(chestYaw - pelvisYaw, pi2);
            chestYaw = pelvisYaw + fminf(fmaxf(t, -maxTwist), maxTwist);
        }
        {
            float meshYaw = atan2f(compToWorld.ax[0].y, compToWorld.ax[0].x);
            float turn = remainderf(g_bodyYaw + pelvisYaw - meshYaw, 6.2831853f);
            Vec3 shift = {};
            if (g_cfg.bodyFollowHead) {
                Vec3 w = YawRotate({ g_headOffset.x, g_headOffset.y, 0.0f }, g_bodyYaw);
                shift = DirTo(worldToComp, w);
            }
            // on a mounted turret the eyes are raised (camera.cpp): the body goes up with them
            if (float lift = CameraTurretLift())
                shift = Add(shift, Scale(Norm(DirTo(worldToComp, { 0, 0, 1 })), lift / fmaxf(Len(compToWorld.ax[0]), 1e-3f)));
            // crouching (the head lower than at the start): the body goes down with the eyes - the feet stay on
            // the floor and the knees bend (below)
            float drop = -g_headOffset.z - 0.05f * g_cfg.worldScale;
            if (drop > 0.0f && g_sk.ankle[0] >= 0 && g_sk.ankle[1] >= 0 && g_sk.knee[0] >= 0 && g_sk.knee[1] >= 0 && g_sk.thigh[0] >= 0 && g_sk.thigh[1] >= 0) {
                const int t = g_sk.thigh[0], k = g_sk.knee[0], a = g_sk.ankle[0];
                float leg = Len(Sub(pose[k].pos, pose[t].pos)) + Len(Sub(pose[a].pos, pose[k].pos));
                crouch = fminf(drop / fmaxf(Len(compToWorld.ax[0]), 1e-3f), leg * 0.6f);
                shift = Add(shift, Scale(Norm(DirTo(worldToComp, { 0, 0, 1 })), -crouch));
            }
            for (int i = 0; i < n; i++) {
                pose[i].pos = Add(YawRotate(pose[i].pos, turn), shift);
                for (auto& a : pose[i].ax) a = YawRotate(a, turn);
                moved[i] = true;
            }
            // the chest's twist over the pelvis: the spine from the waist up, round its root (the legs stay)
            float twist = remainderf(chestYaw - pelvisYaw, 6.2831853f);
            if (g_sk.chest >= 0 && fabsf(twist) > 1e-4f) {
                bool upper[kMaxBones] = {};
                Vec3 c = pose[g_sk.chest].pos;
                for (int i = 0; i < n; i++) {
                    int p = g_sk.parent[i];
                    upper[i] = i == g_sk.chest || (p >= 0 && upper[p] && i != g_sk.thigh[0] && i != g_sk.thigh[1]);
                    if (!upper[i]) continue;
                    pose[i].pos = Add(c, YawRotate(Sub(pose[i].pos, c), twist));
                    for (auto& a : pose[i].ax) a = YawRotate(a, twist);
                }
            }
        }
        for (int i = 0; i < n; i++) anim[i] = pose[i];   // from here on: the body as the arms start from
        if (crouch > 0.0f) {
            Vec3 up = Norm(DirTo(worldToComp, { 0, 0, 1 }));
            float yaw = g_bodyYaw + pelvisYaw;
            Vec3 fwd = Norm(DirTo(worldToComp, { cosf(yaw), sinf(yaw), 0 }));
            for (int s = 0; s < 2; s++) CrouchLeg(s, pose, Add(pose[g_sk.ankle[s]].pos, Scale(up, crouch)), fwd, driven);
        }

        // the robot's arm span (wrist to wrist, arms out: shoulders apart + both arms' bone lengths - rigid, so any
        // pose gives it), in world units; with the player's own span (T-pose in the setup) it sets the world's scale:
        // a bigger robot makes the world around smaller, and the arms reach as far as the real ones
        if (eye == 0 && g_sk.armsOk) {
            const Arm& L = g_sk.arm[0];
            const Arm& R = g_sk.arm[1];
            float span = Len(Sub(anim[L.shoulder].pos, anim[R.shoulder].pos));
            for (const Arm* a : { &L, &R })
                span += Len(Sub(anim[a->elbow].pos, anim[a->shoulder].pos)) + Len(Sub(anim[a->hand].pos, anim[a->elbow].pos));
            g_robotSpan = span * Len(compToWorld.ax[0]);
            // the robot's eye height: the head above its lowest bone (the feet), in the world - while standing (a
            // crouch or a jump gives less: the highest seen recently counts)
            if (g_sk.head >= 0) {
                float lo = 1e30f, headZ = Comp(PointTo(compToWorld, anim[g_sk.head].pos), 2);
                for (int i = 0; i < n; i++) lo = fminf(lo, Comp(PointTo(compToWorld, anim[i].pos), 2));
                float h = headZ - lo;
                static void* measuredFor;
                if (measuredFor != g_sk.mesh) { measuredFor = g_sk.mesh; g_robotEye = h; }
                else if (h > g_robotEye * 0.9f) g_robotEye += (h - g_robotEye) * (h > g_robotEye ? 0.2f : 0.01f);
            }
            // you fit into the robot (VRChat-like): the world is scaled so your eye height is the robot's - a big
            // robot sees a world that is small around you, and its arms reach as yours do
            float userEye = g_cfg.userEyeHeight;
            if (g_cfg.autoScale && userEye > 0.8f && g_robotEye > 10.0f) {
                float ws = fminf(fmaxf(g_robotEye / userEye, 60.0f), 900.0f);
                static float logged;
                if (fabsf(ws - logged) > logged * 0.03f) {
                    Log("ik: robot eye height %.0f units, player %.2f m -> world scale %.0f (arm span %.0f)", g_robotEye, userEye, ws, g_robotSpan);
                    logged = ws;
                }
                g_cfg.worldScale = ws;
            }
        }

        // ---- 3. arms ----
        static float ikWeight = 1.0f;
        static DWORD lastTick;
        DWORD nowTick = GetTickCount();
        float step = fminf((nowTick - lastTick) / 150.0f, 1.0f);
        lastTick = nowTick;
        bool yield = g_cfg.abilityYield > 0.0f && nowTick - VRAbilityPressedAt() < (DWORD)(g_cfg.abilityYield * 1000.0f);
        ikWeight = yield ? fmaxf(ikWeight - step, 0.0f) : fminf(ikWeight + step, 1.0f);

        UpdateGun(comp);
        int melee = WeaponsMeleeHeld();
        bool gunArm = g_gun.held;   // the gun arm (g_gunSide) posed from the gun
        for (int s = 0; s < 2; s++) for (int b : { g_sk.arm[s].shoulder, g_sk.arm[s].elbow, g_sk.arm[s].hand }) driven[b] = true;
        g_padValid[0] = g_padValid[1] = false;
        bool solved[2] = {}, gunPlaced = false, armPlaced = false;
        if (g_cfg.handIK && ikWeight > 0.0f) {
            Vec3 gunTarget = {}, gunCtrl[3] = {};
            bool gunValid = false;
            int support = 1 - g_gunSide;   // the hand that may hold the gun's front grip (two-handed)
            for (int i = 0; i < 2; i++) {   // the gun arm first: the other hand may hold the gun's front grip
                int s = i == 0 ? g_gunSide : support;
                const Hand& h = g_hand[s];
                if (!h.valid) continue;
                Vec3 target = DirTo(worldToComp, Sub(h.pos, compToWorld.pos));
                Vec3 ctrl[3];
                for (int k = 0; k < 3; k++) ctrl[k] = Norm(DirTo(worldToComp, h.ax[k]));
                if (s == g_gunSide) {
                    gunTarget = target;
                    for (int k = 0; k < 3; k++) gunCtrl[k] = ctrl[k];
                    gunValid = true;
                }
                solved[s] = true;
                if (s == g_gunSide && gunArm) {
                    SolveGunArm(pose, target, ctrl);
                    if (eye == 0) {
                        g_gripWorld = h.pos;
                        for (int k = 0; k < 3; k++) g_ctrlWorld[k] = Norm(h.ax[k]);
                    }
                    if (g_cfg.fingerPoses) {   // a fist around the gun, the index finger as on the trigger
                        float curl[4];
                        VRFingerCurls(s, curl);
                        curl[0] = 0.6f + 0.3f * fminf(fmaxf(curl[0], 0.0f), 1.0f);
                        curl[1] = curl[2] = 0.95f;
                        curl[3] = 0.85f;
                        PoseFingers(s, pose, curl, driven);
                    }
                    if (eye == 0 && g_armDump) {   // the fist and the gun in the aim frame (forward, right, up from the wrist)
                        Vec3 W = pose[g_sk.arm[s].hand].pos;
                        auto inAim = [&](Vec3 p) { return InFrame(Sub(p, W), ctrl); };
                        Vec3 go = inAim(g_gun.pose.pos), el = inAim(pose[g_sk.arm[s].elbow].pos);
                        Log("armdump aim: fwd %.2f %.2f %.2f right %.2f %.2f %.2f up %.2f %.2f %.2f | gun origin %.1f %.1f %.1f (k %.2f), elbow %.1f %.1f %.1f",
                            ctrl[0].x, ctrl[0].y, ctrl[0].z, ctrl[1].x, ctrl[1].y, ctrl[1].z, ctrl[2].x, ctrl[2].y, ctrl[2].z,
                            go.x, go.y, go.z, g_gun.size * g_cfg.weaponScale, el.x, el.y, el.z);
                        for (int f = 0; f < 4; f++)
                            for (int j = 0; j < 2; j++) {
                                int b = g_sk.fingers[s][f][j];
                                if (b < 0) continue;
                                Vec3 q = inAim(pose[b].pos);
                                Log("armdump aim: finger %d.%d at %.1f %.1f %.1f", f, j, q.x, q.y, q.z);
                            }
                        if (Att* gr = FindAtt(g_gun.comp)) {   // the gun as the game's animation holds it (component axes)
                            int gb = BoneIndex(gr->bone);
                            if (gb >= 0) {
                                Pose rel = RotatorPose(gr->rot);
                                rel.pos = { gr->loc[0], gr->loc[1], gr->loc[2] };
                                Pose nat = Compose(rel, anim[gb]);
                                Vec3 nx[3];
                                UnitAxes(nat, nx);
                                Vec3 fa = Norm(Sub(anim[g_sk.arm[s].hand].pos, anim[g_sk.arm[s].elbow].pos));
                                Log("armdump aim: the game's gun x %.2f %.2f %.2f y %.2f %.2f %.2f z %.2f %.2f %.2f | its forearm %.2f %.2f %.2f, rot %d %d %d",
                                    nx[0].x, nx[0].y, nx[0].z, nx[1].x, nx[1].y, nx[1].z, nx[2].x, nx[2].y, nx[2].z, fa.x, fa.y, fa.z,
                                    gr->rot[0], gr->rot[1], gr->rot[2]);
                            }
                        }
                    }
                } else {
                    if (s == support && VRTwoHanded() && gunArm && g_cfg.foregripSet && gunValid) {
                        // other hand where it was set in the headset: relative to the gun grip, in the aim axes
                        // (set for the right hand: mirrored across for a gun in the left)
                        float f[3];
                        IKForegrip(f);
                        float k = g_cfg.worldScale / fmaxf(Len(compToWorld.ax[0]), 1e-3f);
                        target = Add(gunTarget, Scale(Add(Add(Scale(gunCtrl[1], f[0]), Scale(gunCtrl[2], f[1])), Scale(gunCtrl[0], -f[2])), k));
                    } else if (s == support && VRTwoHanded() && gunArm && g_gun.hasGrip) {   // other hand on the gun's front grip
                        target = PointTo(GunInComponent(), g_gun.grip);   // (posed with the gun arm)
                    }
                    SolveFreeArm(s, pose, target, ctrl);
                    if (g_cfg.fingerPoses) {
                        // what the hand holds shapes it; empty, it follows the fingers on the controller
                        float curl[4];
                        VRFingerCurls(s, curl);
                        if (s == support && VRTwoHanded() && gunArm) { curl[0] = 0.55f; curl[1] = curl[2] = 0.75f; curl[3] = 0.8f; }
                        else if (GrenadeHand() == s || melee == s) { curl[0] = 0.85f; curl[1] = curl[2] = 0.95f; curl[3] = 0.9f; }
                        PoseFingers(s, pose, curl, driven);
                    }
                }
            }
            // the gun into its attachment, relative to the hand as posed (a blend below carries it along)
            if (g_gun.entry && gunArm && g_gun.posed) gunPlaced = PlaceGunEntry(pose);
            // the arm model on the empty right forearm (not while the right hand holds the melee weapon)
            if (g_armEntry && solved[1] && !(g_gunSide == 1 && gunArm) && melee != 1 && g_padValid[1] && !WeaponsIsMeleeComp(g_armComp) && !WeaponsIsFlipped(g_armComp))
                armPlaced = PlaceArmModel(pose);
            if (ikWeight < 1.0f)
                for (int s = 0; s < 2; s++)
                    for (int b : { g_sk.arm[s].shoulder, g_sk.arm[s].elbow, g_sk.arm[s].hand }) {
                        for (int k = 0; k < 3; k++) pose[b].ax[k] = Add(Scale(anim[b].ax[k], 1 - ikWeight), Scale(pose[b].ax[k], ikWeight));
                        pose[b].pos = Add(Scale(anim[b].pos, 1 - ikWeight), Scale(pose[b].pos, ikWeight));
                    }
        }
        // not placed this frame: where the game has it
        if (g_gun.entry && !gunPlaced)
            if (Att* r = FindAtt(g_gun.comp)) RestoreEntry(r, g_gun.entry);
        if (Att* ar = g_armComp ? FindAtt(g_armComp) : nullptr) {
            if (g_armEntry && !armPlaced) RestoreEntry(ar, g_armEntry);
            // the game's arm model (a grenade throw) under the gun in the right hand: hidden, the gun is the arm
            if (gunArm && g_gunSide == 1 && melee != 1 && !WeaponsIsMeleeComp(g_armComp)) SetScale(ar, 0.001f);
            else if (ar->sized) { RestoreScale(g_armComp); ar->sized = false; }
        }
        // the right hand empty for a moment (not between two guns): the arm model goes on (weapons.cpp attaches
        // it, IKArmModelUpdate)
        if (eye == 0) {
            bool rightFree = g_cfg.handIK && g_hand[1].valid && melee != 1 && !(g_gunSide == 1 && gunArm);
            static DWORD freeSince;
            if (!rightFree) freeSince = 0;
            else if (!freeSince) freeSince = nowTick;
            g_armWanted = rightFree && nowTick - freeSince >= 200;
            g_armWantedTick = nowTick;
        }
        // a hand the player moves is the whole hand, as in the bind pose (the game's weapon animations shrink the
        // gun hand away and fold its parts: an empty right hand was a bare bone); with the gun it is a fist
        bool freeHand[2];
        for (int s = 0; s < 2; s++)
            freeHand[s] = g_cfg.handIK && ikWeight >= 1.0f && g_hand[s].valid && melee != s;

        // ---- 4. look ----
        for (int s = 0; s < 2; s++)   // slimmer legs (the rest of the leg inherits it)
            if (g_sk.thigh[s] >= 0 && g_sk.knee[s] >= 0 && g_cfg.legThickness < 0.999f && g_cfg.showBody > 0) {
                StretchThin(pose[g_sk.thigh[s]], Sub(pose[g_sk.knee[s]].pos, pose[g_sk.thigh[s]].pos), 1.0f, g_cfg.legThickness);
                driven[g_sk.thigh[s]] = true;
            }
        // children keep their local transforms relative to the moved bones; hidden parts collapse to a point: the
        // head always (the eyes are inside it), the torso unless ShowBody=2, the legs too with ShowBody=0
        static bool changed[kMaxBones];   // pose differs from `anim`: the children follow it
        static int8_t inHand[kMaxBones];  // the hand (side + 1) the bone is part of; -1: the weapon bone's own
        static int8_t inArm[kMaxBones];   // the arm (side + 1) whose upper arm or forearm the bone hangs on
        bool padsOn = ikWeight >= 1.0f;   // (blending to an ability's animation: the armour on the blended bones)
        for (int i = 0; i < n; i++) {
            int p = g_sk.parent[i];
            inHand[i] = inArm[i] = 0;
            if (p >= 0) {
                if (i == g_sk.arm[0].weapon || i == g_sk.arm[1].weapon) inHand[i] = -1;
                else if (p == g_sk.arm[0].hand) inHand[i] = 1;
                else if (p == g_sk.arm[1].hand) inHand[i] = 2;
                else inHand[i] = inHand[p];
                for (int s = 0; s < 2; s++) if (p == g_sk.arm[s].shoulder || p == g_sk.arm[s].elbow) inArm[i] = (int8_t)(s + 1);
                if (!inArm[i]) inArm[i] = inArm[p];
            }
            if (g_armDump && eye == 0 && (inArm[i] == 2 || inHand[i] == 2 || inHand[i] == -1 || i == g_sk.arm[1].elbow) && p >= 0) {
                Pose l = Compose(anim[i], Inverse(anim[p]));
                const wchar_t* nm = NameOf(BoneName(i));
                Log("armdump %d %ls p%d arm%d hand%d armour%d: anim scale %.2f pos %.1f %.1f %.1f | ref pos %.1f %.1f %.1f",
                    i, nm ? nm : L"?", p, inArm[i], inHand[i], g_sk.armour[i] ? 1 : 0, Len(l.ax[0]), l.pos.x, l.pos.y, l.pos.z,
                    g_sk.ref[i].pos.x, g_sk.ref[i].pos.y, g_sk.ref[i].pos.z);
            }
            // the clavicles keep their bind pose even though the torso they hang on is hidden (collapsed with it,
            // the shoulders and the top of the upper arms were gone)
            changed[i] = driven[i] || (g_sk.bindPose[i] && g_sk.group[i] == 0);
            if (changed[i]) continue;
            if (g_sk.armour[i] && p >= 0) {
                const Pose* frame = &pose[p];
                for (int s = 0; s < 2; s++)
                    if (padsOn && g_padValid[s]) {
                        if (p == g_sk.arm[s].shoulder) frame = &g_padFrame[s][0];
                        if (p == g_sk.arm[s].elbow) frame = &g_padFrame[s][1];
                    }
                pose[i] = Compose(g_sk.ref[i], *frame);
                changed[i] = true;
                continue;
            }
            if (inHand[i] > 0 && freeHand[inHand[i] - 1]) { pose[i] = Compose(g_sk.ref[i], pose[p]); changed[i] = true; continue; }
            // the parts on a free arm too: the gun animation folds the forearm into the gun (the empty right arm
            // was thin and pointed; normal only for the moment a grenade's animation played)
            if (inHand[i] == 0 && inArm[i] > 0 && freeHand[inArm[i] - 1]) { pose[i] = Compose(g_sk.ref[i], pose[p]); changed[i] = true; continue; }
            if (p >= 0 && changed[p]) { pose[i] = Compose(Compose(anim[i], Inverse(anim[p])), pose[p]); changed[i] = true; }
            bool hide = i == g_sk.head || (g_sk.group[i] == 1 && g_cfg.showBody < 2) || (g_sk.group[i] == 2 && g_cfg.showBody == 0);
            if (hide) { for (auto& a : pose[i].ax) a = Scale(a, 0.01f); changed[i] = true; }
        }
        if (eye == 0 && g_armDump && g_gun.comp) {   // ... and the gun's own bones
            uint8_t* gc = g_gun.comp;
            float* gb = *(float**)(gc + kCompSpaceBases);
            int gn = *(int*)(gc + kCompSpaceBases + 4);
            uint8_t* gs = *(uint8_t**)(gc + kCompSkelMesh);
            uint8_t* gbones = gs ? *(uint8_t**)(gs + kMeshRefSkeleton) : nullptr;
            int gref = gs ? *(int*)(gs + kMeshRefSkeleton + 4) : 0;
            Log("armdump gun %p: %d bones, scale %.3f", gc, gn, *(float*)(gc + kCompScale));
            Vec3 refPos[40] = {};
            for (int i = 0; gb && i < gn && i < 40; i++) {
                const float* m = gb + i * 16;
                const wchar_t* bn = gbones && i < gref ? NameOf(*(unsigned*)(gbones + i * kMeshBoneSize)) : nullptr;
                if (gbones && i < gref) {   // the bind pose's position (component space; rotations ignored past the parent's)
                    const uint8_t* b = gbones + i * kMeshBoneSize;
                    int par = *(const int*)(b + 0x34);
                    const float* q = (const float*)(b + 8);
                    const float* t = (const float*)(b + 0x18);
                    (void)q;
                    refPos[i] = { t[0], t[1], t[2] };
                    if (par >= 0 && par < i) refPos[i] = Add(refPos[i], refPos[par]);
                }
                Log("armdump gun %d %ls: scale %.2f pos %.1f %.1f %.1f | ref ~%.1f %.1f %.1f", i, bn ? bn : L"?",
                    sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), m[12], m[13], m[14], refPos[i].x, refPos[i].y, refPos[i].z);
            }
        }
        if (eye == 0 && g_armDump) {   // ... and what the mod keeps of the mesh's attachments
            Log("armdump gun %p held %d side %d, arm model %p entry %p ours %d wanted %d", g_gun.comp, g_gun.held, g_gunSide,
                g_armComp, g_armEntry, g_armOurs, g_armWanted);
            if (g_armComp) {   // where it is drawn, and the right arm (world)
                const float* m = (const float*)(g_armComp + kCompLocalToWorld);
                const float* b = (const float*)(g_armComp + kCompBounds);
                Vec3 el = PointTo(compToWorld, pose[g_sk.arm[1].elbow].pos), ha = PointTo(compToWorld, pose[g_sk.arm[1].hand].pos);
                int nb = *(int*)(g_armComp + kCompSpaceBases + 4);
                Log("armdump arm model at %.0f %.0f %.0f size %.2f, bounds %.0f %.0f %.0f ext %.0f %.0f %.0f, flags %X, %d bones | elbow %.0f %.0f %.0f hand %.0f %.0f %.0f",
                    m[12], m[13], m[14], sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), b[0], b[1], b[2], b[3], b[4], b[5],
                    *(uint32_t*)(g_armComp + kCompFlags), nb, el.x, el.y, el.z, ha.x, ha.y, ha.z);
                // its bones (world) and the body's bones of the same names
                const float* ab = *(const float**)(g_armComp + kCompSpaceBases);
                uint8_t* as = *(uint8_t**)(g_armComp + kCompSkelMesh);
                uint8_t* abones = as ? *(uint8_t**)(as + kMeshRefSkeleton) : nullptr;
                int aref = as ? *(int*)(as + kMeshRefSkeleton + 4) : 0;
                Pose aw = Load(m);
                Vec3 aRefPos[40] = {};
                for (int i = 0; ab && i < nb && i < 40; i++) {
                    const float* q = ab + i * 16;
                    Vec3 w = PointTo(aw, { q[12], q[13], q[14] });
                    unsigned fn = abones && i < aref ? *(unsigned*)(abones + i * kMeshBoneSize) : 0;
                    int par = abones && i < aref ? *(const int*)(abones + i * kMeshBoneSize + 0x34) : -1;
                    const float* rq = abones && i < aref ? (const float*)(abones + i * kMeshBoneSize + 8) : nullptr;
                    if (abones && i < aref) {   // the bind pose's position (component space; rotations ignored past the parent's)
                        const float* t = (const float*)(abones + i * kMeshBoneSize + 0x18);
                        aRefPos[i] = { t[0], t[1], t[2] };
                        if (par >= 0 && par < i) aRefPos[i] = Add(aRefPos[i], aRefPos[par]);
                    }
                    const wchar_t* bn = fn ? NameOf(fn) : nullptr;
                    int pb = fn ? BoneIndex(fn) : -1;
                    Vec3 pw = pb >= 0 ? PointTo(compToWorld, pose[pb].pos) : Vec3{};
                    Log("armdump arm model %d %ls p%d: %.0f %.0f %.0f (in it %.1f %.1f %.1f, scale %.2f, x %.2f %.2f %.2f) | ref ~%.1f %.1f %.1f q %.2f %.2f %.2f %.2f | body bone %d: %.0f %.0f %.0f",
                        i, bn ? bn : L"?", par, w.x, w.y, w.z, q[12], q[13], q[14], sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]),
                        q[0], q[1], q[2], aRefPos[i].x, aRefPos[i].y, aRefPos[i].z, rq ? rq[0] : 0.f, rq ? rq[1] : 0.f,
                        rq ? rq[2] : 0.f, rq ? rq[3] : 0.f, pb, pw.x, pw.y, pw.z);
                }
                Log("armdump arm model axes %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f",
                    m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
            }
            for (int i = 0; i < g_attCount; i++) {
                const Att& r = g_att[i];
                Log("armdump att %p (%ls) bone %d loc %.1f %.1f %.1f scale %.2f moved %d sized %d comp scale %.3f", r.comp,
                    SkelName(r.comp), BoneIndex(r.bone), r.loc[0], r.loc[1], r.loc[2], r.scale[0], r.moved, r.sized,
                    *(float*)(r.comp + kCompScale));
            }
        }
        if (eye == 0) g_armDump = 0;
        for (int i = 0; i < n; i++) Store(pose[i], bases + i * 16);

        // ---- remember for the right eye and for the two-handed hold ----
        if (eye == 0) {
            PlaceBelt(comp, pose, compToWorld);
            memcpy(g_frozen, pose, n * sizeof(Pose));
            g_frozenCount = n;
            g_frozenL2W = compToWorld;
            g_frozenPawn = pawnNow;
            if (g_gun.comp && g_gun.posed) {
                g_gunWorld = Compose(GunInComponent(), compToWorld);
                g_gunWorldValid = gunArm;
            } else {
                g_gunWorldValid = false;
            }
        }
        posed++;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_sk.mesh = nullptr;
        g_gun.entry = nullptr;   // (the game's attachment is kept: the gun is still let go of properly)
        g_gun.posed = false;
        g_armEntry = nullptr;
    }
}

// __thiscall (this = comp+0x204, args: comp, ?); the caller then updates the transform and attachments
typedef int(__fastcall* SkelUpdateFn)(void* self, void* edx, uint8_t* comp, void* arg);
SkelUpdateFn g_origSkelUpdate;

// The arm model the mod holds on the empty right hand: its plates in the bind pose - the assembled shell, as the game
// shows it on its own empty-handed level (TnWeaponEmpty, Kaon Prison Break); elsewhere its animation folds them into
// a thin sleeve (the throw's animation assembles them only for a moment).
void ArmModelBindPose(uint8_t* comp)
{
    __try {
        float* bases = *(float**)(comp + kCompSpaceBases);
        int n = *(int*)(comp + kCompSpaceBases + 4);
        uint8_t* skel = *(uint8_t**)(comp + kCompSkelMesh);
        uint8_t* bones = skel ? *(uint8_t**)(skel + kMeshRefSkeleton) : nullptr;
        if (!bases || !bones || n <= 0 || n > kMaxBones || n != *(int*)(skel + kMeshRefSkeleton + 4)) return;
        static Pose bind[kMaxBones];
        for (int i = 0; i < n; i++) {
            const uint8_t* b = bones + i * kMeshBoneSize;
            int p = *(const int*)(b + 0x34);
            bind[i] = p >= 0 && p < i ? Compose(RefLocal(b), bind[p]) : RefLocal(b);
            Store(bind[i], bases + i * 16);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

int __fastcall HookSkelUpdate(void* self, void* edx, uint8_t* comp, void* arg)
{
    int r = g_origSkelUpdate(self, edx, comp, arg);
    OnSkelPose(comp);
    if (comp && comp == g_armComp && g_armOurs && !g_armBindOff) ArmModelBindPose(comp);
    return r;
}

// The game fades robots out when the camera comes close (meant for the third-person camera). In first-person VR
// the camera is always close: keep the own body and robots near the head solid. Far actors keep their fades
// (cloaking and other effects).
bool KeepSolid(void* actor)
{
    if (!g_pawn || GetTickCount() - g_inputTick > 250 || !g_cfg.noNearFade) return false;
    if (actor == g_pawn) return true;
    Vec3f loc;
    if (!ReadActorLocation(actor, loc)) return false;
    float dx = loc.x - g_viewPos.x, dy = loc.y - g_viewPos.y, dz = loc.z - g_viewPos.z;
    return dx * dx + dy * dy + dz * dz < g_cfg.noNearFade * g_cfg.noNearFade;
}

typedef void(__stdcall* SetOpacityFn)(void* actor, float opacity);
SetOpacityFn g_origSetOpacity;
void __stdcall HookSetOpacity(void* actor, float opacity)
{
    if (opacity < 1.0f && KeepSolid(actor)) opacity = 1.0f;
    g_origSetOpacity(actor, opacity);
}

typedef void(__stdcall* SetOpacityOverrideFn)(void* actor, float opacity, int flag);
SetOpacityOverrideFn g_origSetOpacityOverride;
void __stdcall HookSetOpacityOverride(void* actor, float opacity, int flag)
{
    if (opacity < 1.0f && KeepSolid(actor)) opacity = 1.0f;
    g_origSetOpacityOverride(actor, opacity, flag);
}

}   // namespace

// ---------------------------------------------------------------------------------------------
void IKSetPawn(void* pawn, const float viewPos[3])
{
    if (!pawn) {   // leaving first person (cutscene, menu): the weapons and the arm model as the game has them
        if (g_attCount) LetGoAll(true);
        g_gun.comp = nullptr;
        g_gun.serial = 0;
    }
    g_pawn = pawn;
    g_inputTick = GetTickCount();
    if (viewPos) g_viewPos = { viewPos[0], viewPos[1], viewPos[2] };
}

void IKSetFrame(int eye, const float headOffset[3], float bodyYaw, float headYaw)
{
    g_frameEye = eye;
    g_headOffset = { headOffset[0], headOffset[1], headOffset[2] };
    g_bodyYaw = bodyYaw;
    g_headYaw = headYaw;
}

void IKSetHand(int hand, bool valid, const float pos[3], const float axes[3][3])
{
    Hand& h = g_hand[hand];
    h.valid = valid;
    if (!valid) return;
    h.localPos = { pos[0], pos[1], pos[2] };
    for (int k = 0; k < 3; k++) h.localAx[k] = { axes[k][0], axes[k][1], axes[k][2] };
}

bool IKHandWorld(int hand, float pos[3], float fwd[3], float rel[3])
{
    const Hand& h = g_hand[hand];
    Vec3f pl;
    if (!g_pawn || !h.valid || GetTickCount() - g_inputTick > 250 || !ReadActorLocation(g_pawn, pl)) return false;
    float baseFwd, baseUp;
    CameraBaseOffset(baseFwd, baseUp);
    Vec3 base = Add({ pl.x, pl.y, pl.z + baseUp }, YawRotate({ baseFwd, 0, 0 }, g_bodyYaw));
    Vec3 r = YawRotate(h.localPos, g_bodyYaw), p = Add(base, r), f = Norm(YawRotate(h.localAx[0], g_bodyYaw));
    for (int k = 0; k < 3; k++) { pos[k] = Comp(p, k); fwd[k] = Comp(f, k); rel[k] = Comp(r, k); }
    return true;
}

void IKSetBelt(void* comp, float size) { g_beltSize = size; g_beltComp = comp; }

float IKRobotArmSpan() { return g_robotSpan; }
float IKRobotEyeHeight() { return g_robotEye; }

void* IKPawn() { return g_pawn && GetTickCount() - g_inputTick < 250 ? g_pawn : nullptr; }

bool IKWeaponHasScope()
{
    return g_pawn && GetTickCount() - g_poseTick < 500 && g_gun.comp && g_gun.scoped;
}

bool IKPawnIsVehicle()
{
    return g_pawn && GetTickCount() - g_poseTick < 500 && g_sk.mesh && !g_sk.armsOk;
}

// Held gun's front grip relative to where the right hand holds the controller, in the controller's aim axes
// (OpenXR: x right, y up, -z forward; metres). Measured while not held two-handed (then the aim is turned).
bool IKForegrip(float out[3])
{
    static float last[3];
    static bool have;
    if (g_cfg.foregripSet && g_pawn && g_gun.comp) {   // set in the headset (for the right hand; mirrored for the left)
        for (int k = 0; k < 3; k++) out[k] = g_cfg.foregrip[k];
        if (g_gunSide == 0) out[0] = -out[0];
        return true;
    }
    if (!g_pawn || GetTickCount() - g_poseTick > 500 || !g_gun.comp || !g_gun.hasGrip) { have = false; return false; }
    if (g_gunWorldValid && !VRTwoHanded()) {
        Vec3 p = PointTo(g_gunWorld, g_gun.grip);
        Vec3 d = Scale(Sub(p, g_gripWorld), 1.0f / g_cfg.worldScale);
        float fwd = Dot(d, g_ctrlWorld[0]), right = Dot(d, g_ctrlWorld[1]), up = Dot(d, g_ctrlWorld[2]);
        last[0] = right; last[1] = up; last[2] = -fwd;
        have = true;
    }
    if (!have) return false;
    for (int k = 0; k < 3; k++) out[k] = last[k];
    return true;
}

void IKArmDump() { g_armDump = 1; }

// The game attached a component to a mesh (weapons.cpp, right after): the values of the game's attachment - also
// for a component attached anew (a gun switched back to or picked up: captured again).
void IKOnAttach(void* mesh, void* comp)
{
    uint8_t* m = (uint8_t*)mesh, *c = (uint8_t*)comp;
    if (!g_pawn || !m || m != g_attMesh || !c || *(uintptr_t*)c != *(uintptr_t*)m) return;
    uint8_t* e = EntryOf(m, c);
    if (!e) return;
    if (Att* r = FindAtt(c)) ReadAtt(r, e);   // (its size stays: the component's Scale is the mod's)
    else RecordAtt(c, e);
    if (!g_armComp && g_armPawn == g_pawn && IsArmModel(c, (uint8_t*)g_pawn, m)) {
        g_armComp = c;
        Log("ik: arm model %p (%ls), attached by the game", c, SkelName(c));
    }
    if (c == g_armComp) {   // where the game wants it: the mod attaches it there too
        g_armGameBone = *(const unsigned*)(e + 4);
        memcpy(g_armGameLoc, e + 8, 12);
        memcpy(g_armGameRot, e + 0x14, 12);
        memcpy(g_armGameScale, e + 0x20, 12);
        g_armGameSaved = true;
    }
}

// The game is about to take a component off a mesh (weapons.cpp): false keeps it on - the arm model while the
// mod shows it on the empty right hand (the game takes it off after a grenade throw).
bool IKOnDetach(void* mesh, void* comp)
{
    uint8_t* m = (uint8_t*)mesh, *c = (uint8_t*)comp;
    if (!c || !m || (m != g_attMesh && m != g_armMesh)) return true;
    if (c == g_armComp && m == g_armMesh && g_armOurs) {
        if (ArmWanted()) {
            g_armAttachedByUs = true;   // (the mod takes it off when the hand is no longer empty)
            return false;
        }
        g_armOurs = false;
        g_armAttachedByUs = false;
        if (g_armWasHidden) *(uint32_t*)(c + kCompFlags) |= kHiddenGame;
    }
    if (Att* r = m == g_attMesh ? FindAtt(c) : nullptr) {
        if (r->sized) RestoreScale(c);
        DropAtt(r);
    }
    return true;
}

// The game attaching the arm model the mod already holds attached (a grenade throw): skipped (weapons.cpp).
bool IKArmModelHeld(void* mesh, void* comp) { return comp && comp == g_armComp && mesh == g_armMesh && g_armOurs; }

// debug: the empty hand's arm model in the bind pose (default) or as the game poses it
void IKArmBindPose(bool on) { g_armBindOff = !on; Log("debug: arm model bind pose %s", on ? "on" : "off"); }

// The arm model on the empty right hand (weapons.cpp, each frame, game thread): the robot's own arm shell (the
// model the game shows on a grenade throw), attached where the game attaches it while the hand is empty and
// shown; let go of (taken off if the mod attached it) once the hand holds something again. `allowed`: not while the
// right hand is about to take the melee weapon (its attack attaches meshes of its own).
void IKArmModelUpdate(void* pawnArg, bool allowed)
{
    uint8_t* pawn = (uint8_t*)pawnArg;
    __try {
        uint8_t* mesh = pawn ? *(uint8_t**)(pawn + kPawnMesh) : nullptr;
        static uint8_t* compPawn;   // the pawn g_armComp belongs to (kept while out of first person for a moment)
        if (pawn != g_armPawn || mesh != g_armMesh) {
            if (g_armOurs && (!pawn || pawn == g_armPawn)) ReleaseArmModel();
            g_armOurs = false;
            g_armAttachedByUs = false;
            if (pawn && pawn != compPawn) { g_armComp = nullptr; g_armSearched = false; g_armGameSaved = false; compPawn = pawn; }
            else if (pawn && g_armComp && !ArmModelAt(g_armComp, pawn, mesh)) { g_armComp = nullptr; g_armSearched = false; }
            g_armPawn = pawn;
            g_armMesh = mesh;
        }
        if (!pawn || !mesh) return;
        bool want = allowed && ArmWanted() && !WeaponsIsMeleeComp(g_armComp) && !WeaponsIsFlipped(g_armComp);
        if (want && !g_armComp && !g_armSearched && pawn == g_pawn && g_sk.mesh) {
            g_armSearched = true;
            g_armComp = FindArmModel(pawn, mesh);
            if (!g_armComp) Log("ik: no arm model on pawn %p or its inventory", pawn);
        }
        if (want && g_armComp) {
            if (!WeaponsIsAttached(mesh, g_armComp)) {
                unsigned bone = g_armGameBone;
                float loc[3] = {}, scale[3] = { 1.0f, 1.0f, 1.0f };
                int rot[3] = {};
                if (g_armGameSaved) {
                    memcpy(loc, g_armGameLoc, 12);
                    memcpy(rot, g_armGameRot, 12);
                    memcpy(scale, g_armGameScale, 12);
                } else {
                    bone = g_sk.mesh && g_sk.arm[1].elbow >= 0 ? BoneName(g_sk.arm[1].elbow) : 0;
                }
                if (bone) WeaponsAttachOriginal(mesh, g_armComp, bone, loc, rot, scale);
                if (!bone || !WeaponsIsAttached(mesh, g_armComp)) {
                    Log("ik: arm model %p could not be attached", g_armComp);
                    g_armComp = nullptr;
                    return;
                }
                g_armAttachedByUs = true;
                Log("ik: arm model %p on the empty right hand (bone %d)", g_armComp, BoneIndex(bone));
            }
            if (!g_armOurs) {
                g_armOurs = true;
                g_armWasHidden = (*(uint32_t*)(g_armComp + kCompFlags) & kHiddenGame) != 0;
            }
            *(uint32_t*)(g_armComp + kCompFlags) &= ~kHiddenGame;   // (the game shows it only for its animations)
            if (!*(uint8_t**)(g_armComp + kCompOwner)) *(uint8_t**)(g_armComp + kCompOwner) = pawn;
        } else if (g_armOurs) {
            ReleaseArmModel();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_armOurs = false;
        g_armAttachedByUs = false;
        g_armComp = nullptr;
    }
}

// The other side's bone of the player's skeleton ("R_Weapon01_Wpn_XW" <-> "L_Weapon01_Wpn_XW"), 0 if none.
unsigned IKMirrorBone(unsigned boneName)
{
    __try {
        uint8_t* mesh = (uint8_t*)g_sk.mesh;
        const wchar_t* nm = mesh ? NameOf(boneName) : nullptr;
        if (!nm || wcslen(nm) < 3 || nm[1] != L'_' || (nm[0] != L'R' && nm[0] != L'L')) return 0;
        wchar_t want[80];
        wcsncpy_s(want, nm, _TRUNCATE);
        want[0] = nm[0] == L'R' ? L'L' : L'R';
        uint8_t* bones = *(uint8_t**)(mesh + kMeshRefSkeleton);
        for (int i = 0; bones && i < g_sk.n; i++) {
            unsigned f = *(unsigned*)(bones + i * kMeshBoneSize);
            const wchar_t* b = NameOf(f);
            if (b && !wcscmp(b, want)) return f;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

void IKInstall()
{
    MH_Initialize();
    if (MH_CreateHook((void*)addr::kSkelUpdate, (void*)HookSkelUpdate, (void**)&g_origSkelUpdate) == MH_OK &&
        MH_EnableHook((void*)addr::kSkelUpdate) == MH_OK)
        Log("ik: skeleton hook installed");
    else
        Log("ik: skeleton hook FAILED");
    if (MH_CreateHook((void*)addr::kSetSemiTransparentOpacity, (void*)HookSetOpacity, (void**)&g_origSetOpacity) == MH_OK &&
        MH_EnableHook((void*)addr::kSetSemiTransparentOpacity) == MH_OK &&
        MH_CreateHook((void*)addr::kSetOpacityOverride, (void*)HookSetOpacityOverride, (void**)&g_origSetOpacityOverride) == MH_OK &&
        MH_EnableHook((void*)addr::kSetOpacityOverride) == MH_OK)
        Log("ik: near-camera fade hooks installed");
}
