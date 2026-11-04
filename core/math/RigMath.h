#pragma once
#include <array>
#include <cmath>

// ── The ONE matrix convention for the whole family ──────────────────────────────────────────────
//
// Mat4 is 16 floats, ROW-MAJOR, row-vector (v·M), translation in row 3 ([12],[13],[14]).
// mul(a,b) means "a then b".
//
// This is POE2's and FOX's convention (2026-09-17 decision, plan §8.1). D4's RigMath is the
// column-vector spelling of the SAME BYTES: D4's mat4mul(a,b) == mul(b,a), and every one of the
// three projects already places translation at [12..14] and the r10 rotation element at index 1
// (verified: d4 RigMath.h:25-28, poe2 RigMath.h:87,102, fox AnimMath.h:53-56,68). So a D4 matrix
// can be handed to these helpers unchanged; only the argument order of a multiply differs.
//
// Bodies below are POE2's RigMath verbatim (it is the most complete and is self-tested by
// AstSkeleton), plus D4's invertRigid, which — checked index by index — is convention-agnostic:
// a transpose is o[r*4+c] = m[c*4+r] in either storage, and -t·Rᵀ expands to the same expression.
// selfTest() proves that rather than asserting it.
namespace core::rm {

using Mat4 = std::array<float, 16>;   // row-major, v·M
using Quat = std::array<float, 4>;    // x, y, z, w  (all three projects agree)

inline Mat4 identity() { return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; }

// (a*b)[r][c] = sum_k a[r][k]*b[k][c]   — "apply a, then b" to a row vector.
inline Mat4 mul(const Mat4& a, const Mat4& b)
{
    Mat4 o{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[r*4+k] * b[k*4+c];
            o[r*4+c] = s;
        }
    return o;
}

// General inverse (Gauss-Jordan). Identity on a singular matrix — fail closed, never garbage.
inline Mat4 inverse(const Mat4& m)
{
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) { a[r][c] = m[r*4+c]; a[r][c+4] = (r == c) ? 1.0 : 0.0; }
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        for (int r = col+1; r < 4; ++r) if (std::fabs(a[r][col]) > std::fabs(a[piv][col])) piv = r;
        if (std::fabs(a[piv][col]) < 1e-12) return identity();
        if (piv != col) for (int c = 0; c < 8; ++c) std::swap(a[col][c], a[piv][c]);
        const double d = a[col][col];
        for (int c = 0; c < 8; ++c) a[col][c] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            const double f = a[r][col];
            for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
        }
    }
    Mat4 o{};
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) o[r*4+c] = float(a[r][c+4]);
    return o;
}

// Fast inverse for a rotation+translation matrix (no scale, no shear). From D4.
inline Mat4 invertRigid(const Mat4& m)
{
    Mat4 o{};
    o[0] = m[0]; o[1] = m[4]; o[2]  = m[8];   o[3]  = 0;     // Rᵀ
    o[4] = m[1]; o[5] = m[5]; o[6]  = m[9];   o[7]  = 0;
    o[8] = m[2]; o[9] = m[6]; o[10] = m[10];  o[11] = 0;
    const float tx = m[12], ty = m[13], tz = m[14];
    o[12] = -(m[0] * tx + m[1] * ty + m[2]  * tz);           // -t·Rᵀ
    o[13] = -(m[4] * tx + m[5] * ty + m[6]  * tz);
    o[14] = -(m[8] * tx + m[9] * ty + m[10] * tz);
    o[15] = 1;
    return o;
}

inline void transformPoint(const Mat4& m, float x, float y, float z, float out[3])
{
    out[0] = x*m[0] + y*m[4] + z*m[8]  + m[12];
    out[1] = x*m[1] + y*m[5] + z*m[9]  + m[13];
    out[2] = x*m[2] + y*m[6] + z*m[10] + m[14];
}

// Directions (normals, tangents): upper 3x3 only. Not normalised — the caller does that after a
// weighted blend.
inline void transformDir(const Mat4& m, float x, float y, float z, float out[3])
{
    out[0] = x*m[0] + y*m[4] + z*m[8];
    out[1] = x*m[1] + y*m[5] + z*m[9];
    out[2] = x*m[2] + y*m[6] + z*m[10];
}

inline void translationOf(const Mat4& m, float out[3]) { out[0]=m[12]; out[1]=m[13]; out[2]=m[14]; }

inline float determinant3(const Mat4& m)
{
    return m[0]*(m[5]*m[10] - m[6]*m[9])
         - m[1]*(m[4]*m[10] - m[6]*m[8])
         + m[2]*(m[4]*m[9]  - m[5]*m[8]);
}

// Rotation from a unit quaternion (x,y,z,w) as a row-major v·M matrix.
inline Mat4 quatToMat(float x, float y, float z, float w)
{
    const float n = std::sqrt(x*x + y*y + z*z + w*w);
    if (n < 1e-12f) return identity();
    x /= n; y /= n; z /= n; w /= n;
    const float xx=x*x, yy=y*y, zz=z*z, xy=x*y, xz=x*z, yz=y*z, wx=w*x, wy=w*y, wz=w*z;
    return {
        1-2*(yy+zz),  2*(xy+wz),    2*(xz-wy),    0,
        2*(xy-wz),    1-2*(xx+zz),  2*(yz+wx),    0,
        2*(xz+wy),    2*(yz-wx),    1-2*(xx+yy),  0,
        0,            0,            0,            1
    };
}

// v' = v · (S · R) + T.
inline Mat4 composeTRS(const float t[3], const float q[4], const float s[3])
{
    const Mat4 R = quatToMat(q[0], q[1], q[2], q[3]);
    Mat4 m{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) m[r*4+c] = s[r] * R[r*4+c];
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2]; m[15] = 1;
    return m;
}

// Inverse of composeTRS (no shear / no negative determinant).
inline void decomposeTRS(const Mat4& m, float t[3], float q[4], float s[3])
{
    t[0] = m[12]; t[1] = m[13]; t[2] = m[14];
    float rows[3][3];
    for (int r = 0; r < 3; ++r) {
        const float len = std::sqrt(m[r*4+0]*m[r*4+0] + m[r*4+1]*m[r*4+1] + m[r*4+2]*m[r*4+2]);
        s[r] = len;
        const float inv = len > 1e-12f ? 1.0f/len : 0.0f;
        for (int c = 0; c < 3; ++c) rows[r][c] = m[r*4+c] * inv;
    }
    const float m00=rows[0][0], m01=rows[0][1], m02=rows[0][2];
    const float m10=rows[1][0], m11=rows[1][1], m12=rows[1][2];
    const float m20=rows[2][0], m21=rows[2][1], m22=rows[2][2];
    const float tr = m00 + m11 + m22;
    float x,y,z,w;
    if (tr > 0) {
        float ss = std::sqrt(tr + 1.0f) * 2.0f;
        w = 0.25f*ss; x = (m12 - m21)/ss; y = (m20 - m02)/ss; z = (m01 - m10)/ss;
    } else if (m00 > m11 && m00 > m22) {
        float ss = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        w = (m12 - m21)/ss; x = 0.25f*ss; y = (m01 + m10)/ss; z = (m20 + m02)/ss;
    } else if (m11 > m22) {
        float ss = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        w = (m20 - m02)/ss; x = (m01 + m10)/ss; y = 0.25f*ss; z = (m12 + m21)/ss;
    } else {
        float ss = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        w = (m01 - m10)/ss; x = (m20 + m02)/ss; y = (m12 + m21)/ss; z = 0.25f*ss;
    }
    const float qn = std::sqrt(x*x+y*y+z*z+w*w);
    if (qn > 1e-12f) { x/=qn; y/=qn; z/=qn; w/=qn; }
    q[0]=x; q[1]=y; q[2]=z; q[3]=w;
}

inline void quatNlerp(const float a[4], const float b[4], float u, float out[4])
{
    float dot = a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3];
    const float sgn = dot < 0 ? -1.0f : 1.0f;
    for (int i = 0; i < 4; ++i) out[i] = a[i]*(1-u) + sgn*b[i]*u;
    const float n = std::sqrt(out[0]*out[0]+out[1]*out[1]+out[2]*out[2]+out[3]*out[3]);
    if (n > 1e-12f) for (int i = 0; i < 4; ++i) out[i] /= n; else { out[0]=out[1]=out[2]=0; out[3]=1; }
}

// ── The two source-frame conversions, named ─────────────────────────────────────────────────────
// core::Geometry is ALWAYS canonical Y-up right-handed. An adapter applies one of these ONCE at
// ingest and nothing downstream rotates again (geometry-export.md §1.4: D4's mixed-space contract —
// Y-up vertices with Z-up rest TRS and hardpoints — is exactly the bug this rule prevents).
// Both are proper rotations, determinant +1, so winding survives; selfTest() checks that.
inline const Mat4 kZUpToYUp  {{1,0,0,0,  0,0,-1,0, 0,1,0,0,  0,0,0,1}};  // (x,y,z)→(x, z,-y)  D4-native
inline const Mat4 kZDownToYUp{{1,0,0,0,  0,0,1,0,  0,-1,0,0, 0,0,0,1}};  // (x,y,z)→(x,-z, y)  PoE2-native
inline const Mat4 kIdentityFrame = identity();                            // Fox-native (already Y-up)

// WARNING, and the reason reframe() exists: kZUpToYUp and kZDownToYUp are each other's INVERSE,
// so the two constants above are numerically the transpose of one another. Hand-picking "the
// inverse one" by name is therefore a coin flip that compiles, runs, and puts a rig's bind pose in
// the wrong place — which is exactly what happened on the first draft of the PoE2 adapter and what
// the adapter test caught. Never write the conjugation by hand; call this.
//
// A MODEL-SPACE matrix authored in a native frame, expressed in the canonical frame:
//   p_canon · M_canon  ==  (p_native · M_native) · S,  with p_canon = p_native · S
//   ⇒ M_canon = S⁻¹ · M_native · S
// S is always a pure rotation here, so invertRigid is exact.
inline Mat4 reframe(const Mat4& m, const Mat4& toCanonical)
{
    if (toCanonical == kIdentityFrame) return m;
    return mul(mul(invertRigid(toCanonical), m), toCanonical);
}

const char* selfTest();   // "" on success, else the first failing case (core/math/RigMath.cpp)

}  // namespace core::rm
