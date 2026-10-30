#pragma once
// Minimal 3x3 rotation math for converting between OpenXR and Unreal Engine 3 conventions.
// OpenXR: right-handed, +X right, +Y up, -Z forward, metres.
// UE3:    left-handed,  +X forward, +Y right, +Z up, rotator units 65536 = 360 deg.
#include <math.h>

struct Vec3 { float x, y, z; };

struct Mat3 {
    float m[3][3];   // m[row][col]; columns are the basis axes (X forward, Y right, Z up for UE)
    Vec3 col(int c) const { return { m[0][c], m[1][c], m[2][c] }; }
};

static inline Mat3 Mul(const Mat3& a, const Mat3& b)
{
    Mat3 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}

static inline Vec3 Mul(const Mat3& a, const Vec3& v)
{
    return { a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z,
             a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
             a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z };
}

static const float kUURotToRad = 3.14159265f * 2.0f / 65536.0f;

// UE3 FRotationMatrix, written with the axes as columns.
static inline Mat3 RotatorToMat(int pitch, int yaw, int roll)
{
    float sp = sinf(pitch * kUURotToRad), cp = cosf(pitch * kUURotToRad);
    float sy = sinf(yaw * kUURotToRad), cy = cosf(yaw * kUURotToRad);
    float sr = sinf(roll * kUURotToRad), cr = cosf(roll * kUURotToRad);
    Vec3 X = { cp * cy, cp * sy, sp };
    Vec3 Y = { sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp };
    Vec3 Z = { -(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp };
    Mat3 r;
    r.m[0][0] = X.x; r.m[1][0] = X.y; r.m[2][0] = X.z;
    r.m[0][1] = Y.x; r.m[1][1] = Y.y; r.m[2][1] = Y.z;
    r.m[0][2] = Z.x; r.m[1][2] = Z.y; r.m[2][2] = Z.z;
    return r;
}

static inline void MatToRotator(const Mat3& r, int& pitch, int& yaw, int& roll)
{
    Vec3 X = r.col(0), Y = r.col(1), Z = r.col(2);
    float p = atan2f(X.z, sqrtf(X.x * X.x + X.y * X.y));
    float y = atan2f(X.y, X.x);
    float ro = atan2f(-Y.z, Z.z);
    pitch = (int)lroundf(p / kUURotToRad) & 0xFFFF;
    yaw = (int)lroundf(y / kUURotToRad) & 0xFFFF;
    roll = (int)lroundf(ro / kUURotToRad) & 0xFFFF;
}

// OpenXR quaternion (x, y, z, w) -> rotation matrix in OpenXR space.
static inline Mat3 QuatToMat(float x, float y, float z, float w)
{
    Mat3 r;
    r.m[0][0] = 1 - 2 * (y * y + z * z); r.m[0][1] = 2 * (x * y - z * w);     r.m[0][2] = 2 * (x * z + y * w);
    r.m[1][0] = 2 * (x * y + z * w);     r.m[1][1] = 1 - 2 * (x * x + z * z); r.m[1][2] = 2 * (y * z - x * w);
    r.m[2][0] = 2 * (x * z - y * w);     r.m[2][1] = 2 * (y * z + x * w);     r.m[2][2] = 1 - 2 * (x * x + y * y);
    return r;
}

// Change of basis OpenXR -> UE: ue = C * xr with ue.x = -xr.z, ue.y = xr.x, ue.z = xr.y.
static inline Vec3 XrToUE(const Vec3& v) { return { -v.z, v.x, v.y }; }

static inline Mat3 XrRotToUE(const Mat3& xr)
{
    static const Mat3 C = { { { 0, 0, -1 }, { 1, 0, 0 }, { 0, 1, 0 } } };
    static const Mat3 Ct = { { { 0, 1, 0 }, { 0, 0, 1 }, { -1, 0, 0 } } };
    return Mul(Mul(C, xr), Ct);
}
