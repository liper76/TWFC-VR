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
//                 a free hand (the weapon hangs off its weapon bone).
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
        const float* q = (const float*)(b + 8);
        const float* t = (const float*)(b + 0x18);
        float x = q[0], y = q[1], z = q[2], w = q[3];
        k.ref[i] = { { { 1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y) },
                       { 2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x) },
                       { 2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y) } },
                     { t[0], t[1], t[2] } };
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
// The held gun: a skeletal mesh attached to the right arm (Transformers carry their guns on the forearm). Read
// from the pawn mesh's attachments: which bone it hangs on and its exact placement relative to that bone.
// The game's attachment is kept as it made it (restored whenever we let go); every frame the gun is placed anew
// from it - nothing is measured from the animation (a weapon taken mid-animation, with the hand shrunk away,
// once left the gun ten times its size for good) and nothing builds up.
struct Gun {
    uint8_t* comp = nullptr;
    uint8_t* mesh = nullptr;         // the pawn mesh it is attached to
    uint8_t* entry = nullptr;        // its attachment entry (this frame)
    unsigned origBone = 0;           // the attachment as the game made it
    float origLoc[3], origScale[3];
    int origRot[3];
    int origSide = 1;                // the arm the game put it on
    bool placed = false;             // the entry holds our placement:
    unsigned wroteBone = 0;          //   on this bone, at this location
    float wroteLoc[3];
    Pose inHand;                     // gun relative to the hand, the wrist straight (unit axes; component units)
    float size = 1.0f;               // gun units -> component units (before the component's own Scale)
    float handSize = 1.0f;           // the hand's size as the game draws it under the gun
    Pose pose;                       // the gun as posed this frame (unit axes; component space)
    bool posed = false;
    uint8_t* skelMesh = nullptr;     // to notice weapon changes
    bool scoped = false;             // a sniper rifle (ini ScopeWeapons)
    bool hasGrip = false;
    Vec3 grip;                       // front grip in gun space (gun units, unscaled)
} g_gun;
int g_gunSide = 1;              // the arm holding the gun (1 right; 0 left - VRGunHand)

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

// The game's attachment back in the entry (the gun as the game's animation holds it).
void RestoreGunEntry(uint8_t* e)
{
    if (!e || !g_gun.placed) return;
    *(unsigned*)(e + 4) = g_gun.origBone;
    memcpy(e + 8, g_gun.origLoc, 12);
    memcpy(e + 0x14, g_gun.origRot, 12);
    memcpy(e + 0x20, g_gun.origScale, 12);
    g_gun.placed = false;
}

bool IsOurPlacement(const uint8_t* e)
{
    return g_gun.placed && *(const unsigned*)(e + 4) == g_gun.wroteBone && !memcmp(e + 8, g_gun.wroteLoc, 12);
}

// The gun relative to the hand with the wrist straight (the bind pose from its bone to the hand), from the game's
// attachment: a gun on the forearm is an extension of it, the hand inside.
void CaptureGun(uint8_t* comp, const uint8_t* e, int b)
{
    g_gun.comp = comp;
    g_gun.origBone = *(const unsigned*)(e + 4);
    memcpy(g_gun.origLoc, e + 8, 12);
    memcpy(g_gun.origRot, e + 0x14, 12);
    memcpy(g_gun.origScale, e + 0x20, 12);
    g_gun.placed = false;
    g_gun.origSide = ArmOfBone(b);
    const Arm& a = g_sk.arm[g_gun.origSide];
    Pose rel = RotatorPose(g_gun.origRot);
    rel.pos = { g_gun.origLoc[0], g_gun.origLoc[1], g_gun.origLoc[2] };
    Pose in = Compose(rel, Compose(RefComponent(b), Inverse(RefComponent(a.hand))));
    Vec3 ax[3];
    UnitAxes(in, ax);
    g_gun.inHand = { { ax[0], ax[1], ax[2] }, in.pos };
    g_gun.size = fabsf(g_gun.origScale[0]) > 1e-3f ? fabsf(g_gun.origScale[0]) : 1.0f;
    Log("ik: gun %p on bone %d (%s, %s arm): %.1f %.1f %.1f from the hand, size %.2f", comp, b,
        b == a.elbow ? "forearm" : b == a.hand ? "hand" : "weapon bone", g_gun.origSide ? "right" : "left",
        in.pos.x, in.pos.y, in.pos.z, g_gun.size);
}

// The posed gun into its attachment entry, on the gun hand's bone (uniform: on the slimmed forearm it was sheared).
void PlaceGunEntry(const Pose* pose)
{
    uint8_t* e = g_gun.entry;
    int hb = g_sk.arm[g_gunSide].hand;
    if (!e || hb < 0) return;
    Pose g = g_gun.pose;
    for (auto& a : g.ax) a = Scale(a, g_gun.size);
    Pose rel = Compose(g, Inverse(pose[hb]));
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
    g_gun.wroteBone = *(unsigned*)(e + 4) = BoneName(hb);
    memcpy(g_gun.wroteLoc, loc, 12);
    g_gun.placed = true;
}

// ---------------------------------------------------------------------------------------------
// The folded gun. Put away (a switch to another weapon, a grenade throw), the game folds a forearm gun into a shell
// around the forearm - an animation of the gun's own bones; its bind pose is the gun deployed. The most compact pose
// each gun mesh is seen in (component space; kept in twfcvr_folded.dat across runs) is held while the gun is not in
// use, so the forearm wears the folded gun as in the game instead of a bare thin arm.
const int kFoldBones = 64;
struct Folded { wchar_t name[64]; int n; float extent; float m[kFoldBones * 16]; };
const int kMaxFolded = 24;
Folded g_folded[kMaxFolded];
int g_foldedCount = -1;          // -1: the file not read yet
float g_foldMax[kMaxFolded];     // the gun's largest extent seen this run (deployed)
bool g_foldDirty;
DWORD g_foldChanged;
bool g_holdFolded;               // the gun not in use (set by UpdateGun): drawn folded
int g_foldLog;                   // debug (armdump): the gun's extent logged for this many updates
uint8_t* g_recentGuns[4];        // the guns held lately: the game folds a gun on after it is let go of

std::wstring FoldedPath() { return ModDir() + L"twfcvr_folded.dat"; }

void LoadFolded()
{
    g_foldedCount = 0;
    FILE* f = nullptr;
    if (_wfopen_s(&f, FoldedPath().c_str(), L"rb") || !f) return;
    int n = (int)fread(g_folded, sizeof(Folded), kMaxFolded, f);
    fclose(f);
    for (int i = 0; i < n; i++) if (g_folded[i].n <= 0 || g_folded[i].n > kFoldBones) { n = i; break; }
    g_foldedCount = n;
    Log("ik: %d folded gun poses read", n);
}

void SaveFolded()
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, FoldedPath().c_str(), L"wb") || !f) return;
    fwrite(g_folded, sizeof(Folded), g_foldedCount, f);
    fclose(f);
    g_foldDirty = false;
}

// The entry for a gun mesh (by its name); `add`: a new one when there is none.
int FoldedIndex(uint8_t* skel, bool add)
{
    if (g_foldedCount < 0) LoadFolded();
    const wchar_t* nm = skel ? NameOf(*(unsigned*)(skel + 4)) : nullptr;
    if (!nm) return -1;
    for (int i = 0; i < g_foldedCount; i++) if (!wcscmp(g_folded[i].name, nm)) return i;
    if (!add || g_foldedCount >= kMaxFolded) return -1;
    Folded& f = g_folded[g_foldedCount];
    memset(&f, 0, sizeof(f));
    wcsncpy_s(f.name, nm, _TRUNCATE);
    return g_foldedCount++;
}

// The gun's own skeleton update: its most compact pose remembered; while the gun is put away, that pose is drawn.
void OnGunPose(uint8_t* comp)
{
    float* bases = *(float**)(comp + kCompSpaceBases);
    int n = *(int*)(comp + kCompSpaceBases + 4);
    uint8_t* skel = *(uint8_t**)(comp + kCompSkelMesh);
    if (!bases || n <= 1 || n > kFoldBones || !skel) return;
    int k = FoldedIndex(skel, true);
    if (k < 0) return;
    Folded& f = g_folded[k];
    if (g_holdFolded && comp == g_gun.comp && f.n == n) {
        memcpy(bases, f.m, n * 16 * sizeof(float));
        return;
    }
    // how far the bones reach from the root (a folded gun is compact; parts tucked away are scaled to nothing);
    // not a pose collapsed to a point
    float extent = 0.0f;
    for (int i = 1; i < n; i++) {
        const float* m = bases + i * 16;
        float s = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        if (s > 2.0f) return;
        if (s < 0.05f) continue;
        extent += sqrtf((m[12] - bases[12]) * (m[12] - bases[12]) + (m[13] - bases[13]) * (m[13] - bases[13]) + (m[14] - bases[14]) * (m[14] - bases[14]));
    }
    if (g_foldLog > 0) { g_foldLog--; Log("foldlog %p %ls extent %.0f", comp, f.name, extent); }
    g_foldMax[k] = fmaxf(g_foldMax[k], extent);
    if (extent < 0.25f * g_foldMax[k]) return;
    if (f.n != n || extent < f.extent * 0.995f) {
        if (f.n == n && extent < f.extent * 0.9f) Log("ik: folded gun '%ls': extent %.0f (deployed %.0f)", f.name, extent, g_foldMax[k]);
        f.n = n;
        f.extent = extent;
        memcpy(f.m, bases, n * 16 * sizeof(float));
        g_foldDirty = true;
        g_foldChanged = GetTickCount();
    }
    if (g_foldDirty && GetTickCount() - g_foldChanged > 1000) SaveFolded();
}

// Folded far enough from its deployed pose to be drawn as the put-away shell.
bool HasFolded(uint8_t* comp)
{
    uint8_t* skel = *(uint8_t**)(comp + kCompSkelMesh);
    int k = FoldedIndex(skel, false);
    if (k < 0 || g_folded[k].n != *(int*)(comp + kCompSpaceBases + 4)) return false;
    float deployed = g_foldMax[k] > 0.0f ? g_foldMax[k] : g_folded[k].extent;
    return g_folded[k].extent < 0.85f * deployed;
}

// Finds the gun among the mesh's attachments, sets its size (folded on the forearm while the hand is empty or
// holds the melee weapon; hidden when its folded pose is not known yet) and the size of its muzzle flashes.

void UpdateGun(uint8_t* mesh, const Pose* anim)
{
    // the gun hand: the right one, or the left when the gun was taken there
    g_gunSide = VRGunHand() == 0 ? 0 : 1;
    g_gun.entry = nullptr;
    g_gun.posed = false;
    g_holdFolded = false;
    uint8_t* att = *(uint8_t**)(mesh + kCompAttachments);
    int count = *(int*)(mesh + kCompAttachments + 4);
    // the weapon in hand (Pawn.Weapon) and its mesh components
    uint8_t* weapon = *(uint8_t**)((uint8_t*)g_pawn + kPawnWeapon);
    uint8_t* mine[2] = { weapon ? *(uint8_t**)(weapon + kWeaponMesh) : nullptr, weapon ? *(uint8_t**)(weapon + kWeaponMesh2) : nullptr };
    uint8_t* found = nullptr;
    uint8_t* entry = nullptr;
    uint8_t* oldEntry = nullptr;     // the gun we hold, still at our placement (whatever Pawn.Weapon is now)
    bool ours = false;
    for (int i = 0; att && i < count && i < 64; i++) {
        uint8_t* e = att + i * 0x30;
        uint8_t* c = *(uint8_t**)e;
        if (!c) continue;
        if (c == g_gun.comp && IsOurPlacement(e)) oldEntry = e;   // (the array may have moved)
        if ((c != mine[0] && c != mine[1]) || WeaponsIsMeleeComp(c) || (found && !(c == g_gun.comp && oldEntry == e))) continue;
        if (c == g_gun.comp && oldEntry == e) { found = c; entry = e; ours = true; }
        else if (!found && ArmOfBone(BoneIndex(*(const unsigned*)(e + 4))) >= 0) { found = c; entry = e; }
    }
    // Letting go of a gun (Pawn.Weapon switched, even for a moment - a grenade grabbed off the belt): the game's
    // attachment goes back into its entry first. Dropped at our placement it stayed on the hand bone ten times its
    // size (sized for the shrunk hand), and the next capture took that for the game's - the stretched gun for good.
    if (g_gun.comp && found != g_gun.comp) {
        if (oldEntry) RestoreGunEntry(oldEntry);
        g_gun.placed = false;
        RestoreScale(g_gun.comp);   // the previous gun gets its size back
    } else if (g_gun.comp && !oldEntry) g_gun.placed = false;   // (the game re-made the attachment: captured anew)
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
    if (!found) { g_gun.comp = nullptr; return; }
    if (!ours) CaptureGun(found, entry, BoneIndex(*(const unsigned*)(entry + 4)));   // new, or attached anew by the game
    if (std::find(g_recentGuns, g_recentGuns + 4, found) == g_recentGuns + 4) {
        memmove(g_recentGuns + 1, g_recentGuns, 3 * sizeof(uint8_t*));
        g_recentGuns[0] = found;
    }
    g_gun.mesh = mesh;
    g_gun.entry = entry;
    // the hand under the gun as small as the game draws it (the gun takes its place)
    int animHand = g_sk.arm[g_gun.origSide].hand;
    g_gun.handSize = fminf(fmaxf(Len(anim[animHand].ax[0]), 0.05f), 1.0f);

    bool hidden = WeaponsMeleeHeld() == g_gunSide || VRGunHand() < 0;   // not in use (no muzzle flashes)
    g_holdFolded = hidden && HasFolded(found);
    *(float*)(found + kCompScale) = g_holdFolded ? 1.0f : hidden ? 0.001f : g_cfg.weaponScale;
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
        sc[0] = sc[1] = sc[2] = hidden ? 0.001f : g_cfg.muzzleFlashScale;
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
    if (!g_gun.hasGrip && !hidden) {
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
// shrugs a little when the hand is raised, the wrist goes to `wrist` (out of reach: the arm points at it,
// straight), the elbow bends down, out and back from the hand, the forearm takes half of the wrist's roll and the
// hand gets `handAx` at `handSize`. Upper arm and forearm are slimmed by LimbThickness, the armour is not.
void SolveArm(int side, Pose* pose, Vec3 wrist, const Vec3 ctrl[3], const Vec3 handAx[3], float handSize, bool rigidWrist = false)
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
    Vec3 H = Add(S, Scale(dir, dist));
    Vec3 hinge = Norm(Cross(perp, dir));
    Vec3 up1[3], fore1[3];
    BendFrame(Sub(E, S), hinge, up1);
    BendFrame(Sub(H, E), hinge, fore1);

    Pose sh = rs, el = re;
    float stretch1 = stretch, stretch2 = stretch;
    bool rigid = false;
    if (rigidWrist) {
        // the gun arm: the forearm is part of the gun (the game hangs the gun on it) - fixed to the hand, the
        // wrist straight as in the bind pose, always: the gun's back end stays on the forearm. The elbow goes
        // where that puts it; the upper arm gives (0.5..1.2 of its length) and beyond that the gun with the
        // forearm moves along it, still pointing where the controller does.
        float L1 = l1 / stretch, L2 = l2 / stretch;
        Vec3 rhAx[3];
        UnitAxes(rh, rhAx);
        Vec3 d = Norm(FromFrame(InFrame(Norm(Sub(rh.pos, re.pos)), rhAx), handAx));   // the forearm, turned with the hand
        Vec3 Er = Sub(wrist, Scale(d, L2));
        Vec3 v = Sub(Er, S);
        float len = Len(v);
        Vec3 ve = len > 1e-3f ? Scale(v, 1.0f / len) : perp;
        float k = fminf(fmaxf(len, 0.5f * L1), 1.2f * L1);
        Vec3 E2 = Add(S, Scale(ve, k));
        H = Add(E2, Scale(d, L2));
        rigid = true;
        Vec3 dirN = Norm(Sub(H, S)), eN = Sub(E2, S);
        Vec3 perpN = Sub(eN, Scale(dirN, Dot(eN, dirN)));
        Vec3 hingeN = Len(perpN) > 1e-3f ? Norm(Cross(perpN, dirN)) : hinge;
        BendFrame(eN, hingeN, up1);
        RotatePose(sh, FrameToFrame(up0, up1));
        RotatePose(el, FrameToFrame(rhAx, handAx));
        el.pos = E2;
        stretch1 = k / fmaxf(L1, 1e-3f);
        stretch2 = 1.0f;
        E = E2;
    }
    if (!rigid) {
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
    g_padFrame[side][0] = pad;
    g_padFrame[side][1] = el;
    g_padValid[side] = true;

    StretchThin(sh, Sub(E, S), stretch1, g_cfg.limbThickness);
    StretchThin(el, Sub(H, E), stretch2, g_cfg.limbThickness);
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

// The arm carrying the gun: the gun aims as the controller does (barrel = forward, its up = up) and is held where
// the real hand is - an extension of the arm, the hand inside it turned with it (wrist straight as in the bind
// pose), the arm reaching that wrist like the other arm. Nothing is stretched.
void SolveGunArm(Pose* pose, Vec3 target, const Vec3 ctrl[3])
{
    const Arm& a = g_sk.arm[g_gunSide];
    bool mirrored = g_gunSide != g_gun.origSide;   // (taken into the other hand)
    Pose handInGun = Inverse(g_gun.inHand);
    // the gun is drawn smaller (WeaponScale) about its origin: the hand stays on its grip
    Vec3 hp = Scale(handInGun.pos, g_cfg.weaponScale);
    if (mirrored) hp.y = -hp.y;
    Vec3 handAx[3];
    if (mirrored) {
        HandAxesFor(g_gunSide, pose, ctrl, handAx);
    } else {
        for (int j = 0; j < 3; j++) handAx[j] = FromFrame(handInGun.ax[j], ctrl);
        Orthonormalize(handAx);
    }
    SolveArm(g_gunSide, pose, target, ctrl, handAx, g_gun.handSize, true);
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
        if (std::find(g_recentGuns, g_recentGuns + 4, comp) != g_recentGuns + 4) { OnGunPose(comp); return; }
        if (*(uint8_t**)((uint8_t*)g_pawn + kPawnMesh) != comp) return;
        uint8_t* skel = *(uint8_t**)(comp + kCompSkelMesh);
        if (!skel) return;
        if (skel != g_sk.mesh && !ResolveSkeleton(skel)) return;
        float* bases = *(float**)(comp + kCompSpaceBases);
        int n = *(int*)(comp + kCompSpaceBases + 4);
        if (!bases || n != g_sk.n) return;
        g_poseTick = GetTickCount();
        if (!g_sk.armsOk) return;   // vehicle form: drawn as the game poses it

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

        UpdateGun(comp, anim);
        int melee = WeaponsMeleeHeld();
        bool gunArm = g_gun.comp && melee != g_gunSide && VRGunHand() >= 0;   // (g_gunSide: the arm the gun is on)
        for (int s = 0; s < 2; s++) for (int b : { g_sk.arm[s].shoulder, g_sk.arm[s].elbow, g_sk.arm[s].hand }) driven[b] = true;
        g_padValid[0] = g_padValid[1] = false;
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
                if (s == g_gunSide && gunArm) {
                    SolveGunArm(pose, target, ctrl);
                    if (eye == 0) {
                        g_gripWorld = h.pos;
                        for (int k = 0; k < 3; k++) g_ctrlWorld[k] = Norm(h.ax[k]);
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
            if (g_gun.entry && gunArm && g_gun.posed) PlaceGunEntry(pose);
            if (ikWeight < 1.0f)
                for (int s = 0; s < 2; s++)
                    for (int b : { g_sk.arm[s].shoulder, g_sk.arm[s].elbow, g_sk.arm[s].hand }) {
                        for (int k = 0; k < 3; k++) pose[b].ax[k] = Add(Scale(anim[b].ax[k], 1 - ikWeight), Scale(pose[b].ax[k], ikWeight));
                        pose[b].pos = Add(Scale(anim[b].pos, 1 - ikWeight), Scale(pose[b].pos, ikWeight));
                    }
        }
        if (g_gun.entry && !(gunArm && g_gun.posed)) RestoreGunEntry(g_gun.entry);   // as the game holds it
        // a hand the player moves freely is the whole hand, as in the bind pose (the game's weapon animations
        // shrink the gun hand away and fold its parts: an empty right hand was a bare bone)
        bool freeHand[2];
        for (int s = 0; s < 2; s++)
            freeHand[s] = g_cfg.handIK && ikWeight >= 1.0f && g_hand[s].valid && melee != s && !(s == g_gunSide && gunArm);

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
    }
}

// __thiscall (this = comp+0x204, args: comp, ?); the caller then updates the transform and attachments
typedef int(__fastcall* SkelUpdateFn)(void* self, void* edx, uint8_t* comp, void* arg);
SkelUpdateFn g_origSkelUpdate;

int __fastcall HookSkelUpdate(void* self, void* edx, uint8_t* comp, void* arg)
{
    int r = g_origSkelUpdate(self, edx, comp, arg);
    OnSkelPose(comp);
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
    if (!pawn && g_gun.comp) {   // leaving first person (cutscene, menu): the weapon as the game had it
        __try {
            uint8_t* att = g_gun.placed && g_gun.mesh ? *(uint8_t**)(g_gun.mesh + kCompAttachments) : nullptr;
            int count = att ? *(int*)(g_gun.mesh + kCompAttachments + 4) : 0;
            for (int i = 0; i < count && i < 64; i++) {
                uint8_t* e = att + i * 0x30;
                if (*(uint8_t**)e == g_gun.comp && IsOurPlacement(e)) RestoreGunEntry(e);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_gun.placed = false;
        RestoreScale(g_gun.comp);
        g_gun.comp = nullptr;
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

// The other side's bone of the player's skeleton ("R_Weapon01_Wpn_XW" <-> "L_Weapon01_Wpn_XW"), 0 if none.
void IKArmDump() { g_armDump = 1; g_foldLog = 400; }

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
