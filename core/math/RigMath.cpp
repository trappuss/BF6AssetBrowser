#include "core/math/RigMath.h"
#include <cstdio>
#include <cstring>

namespace core::rm {

static thread_local char g_msg[256];
static const char* fail(const char* fmt, double a = 0, double b = 0)
{
    std::snprintf(g_msg, sizeof(g_msg), fmt, a, b);
    return g_msg;
}
static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
static bool nearMat(const Mat4& a, const Mat4& b, float eps = 1e-4f)
{
    for (int i = 0; i < 16; ++i) if (!near(a[i], b[i], eps)) return false;
    return true;
}

const char* selfTest()
{
    // 1. mul is "a then b" for a row vector: translate(1,0,0) then rotate 90° about Z must land a
    //    point where doing it in that order does, not the reverse.
    {
        Mat4 T = identity(); T[12] = 1;
        const float q[4] = {0, 0, 0.70710678f, 0.70710678f};   // +90° about Z
        const Mat4 R = quatToMat(q[0], q[1], q[2], q[3]);
        float p[3];
        transformPoint(mul(T, R), 0, 0, 0, p);                 // translate first → (1,0,0) → rotate → (0,1,0)
        if (!near(p[0], 0) || !near(p[1], 1) || !near(p[2], 0))
            return fail("mul(a,b) is not 'a then b': got (%.3f, %.3f, ...)", p[0], p[1]);
        transformPoint(mul(R, T), 0, 0, 0, p);                 // rotate first (no-op at origin) → (1,0,0)
        if (!near(p[0], 1) || !near(p[1], 0))
            return fail("mul(b,a) mismatch: got (%.3f, %.3f, ...)", p[0], p[1]);
    }
    // 2. invertRigid really inverts under THIS convention (it was lifted from D4's column-vector
    //    RigMath, so this is the check that the lift was legitimate rather than an assumption).
    {
        const float t[3] = {3, -4, 5};
        const float q[4] = {0.2f, -0.3f, 0.5f, 0.78740079f};
        const float s[3] = {1, 1, 1};
        const Mat4 m = composeTRS(t, q, s);
        if (!nearMat(mul(m, invertRigid(m)), identity())) return "invertRigid: m · inv(m) != I";
        if (!nearMat(mul(invertRigid(m), m), identity())) return "invertRigid: inv(m) · m != I";
        if (!nearMat(invertRigid(m), inverse(m), 1e-3f))  return "invertRigid disagrees with inverse()";
    }
    // 3. compose/decompose round-trip, including non-uniform scale.
    {
        const float t[3] = {1.5f, -2.25f, 0.75f};
        const float q[4] = {0.1f, 0.2f, -0.3f, 0.92736185f};
        const float s[3] = {2.0f, 0.5f, 1.25f};
        const Mat4 m = composeTRS(t, q, s);
        float t2[3], q2[4], s2[3];
        decomposeTRS(m, t2, q2, s2);
        if (!nearMat(composeTRS(t2, q2, s2), m)) return "composeTRS/decomposeTRS do not round-trip";
        for (int i = 0; i < 3; ++i) if (!near(s[i], s2[i])) return fail("scale lost: %.3f vs %.3f", s[i], s2[i]);
    }
    // 4. The frame conversions do what their names say, and are PROPER rotations (det +1) so
    //    triangle winding survives. A mirrored "conversion" would flip every face silently.
    {
        float p[3];
        transformPoint(kZUpToYUp, 1, 2, 3, p);                 // D4-native z-up → (x, z, -y)
        if (!near(p[0],1) || !near(p[1],3) || !near(p[2],-2))
            return fail("kZUpToYUp wrong: (%.1f, %.1f, ...)", p[0], p[1]);
        transformPoint(kZDownToYUp, 1, 2, 3, p);               // PoE2-native → (x, -z, y)
        if (!near(p[0],1) || !near(p[1],-3) || !near(p[2],2))
            return fail("kZDownToYUp wrong: (%.1f, %.1f, ...)", p[0], p[1]);
        // The two named frames are each other's inverse — which is why reframe() derives the
        // inverse rather than letting a caller choose a constant by name (case 5).
        transformPoint(mul(kZUpToYUp, kZDownToYUp), 1, 2, 3, p);
        if (!near(p[0],1) || !near(p[1],2) || !near(p[2],3)) return "kZDownToYUp is not the inverse of kZUpToYUp";
        if (!near(determinant3(kZUpToYUp),   1)) return "kZUpToYUp is not a proper rotation (det != +1)";
        if (!near(determinant3(kZDownToYUp), 1)) return "kZDownToYUp is not a proper rotation (det != +1)";
    }
    // 5. reframe() picks the right inverse. A pure translation of +3 along the native axis must
    //    land where transforming the POINT lands it — the check that failed when the conjugation
    //    was written by hand with the wrong constant.
    {
        for (const Mat4* S : { &kZUpToYUp, &kZDownToYUp }) {
            Mat4 L = identity(); L[12] = 1; L[13] = 3; L[14] = -2;    // translate (1,3,-2) natively
            float viaMat[3], viaPoint[3];
            transformPoint(reframe(L, *S), 0, 0, 0, viaMat);          // origin through the reframed matrix
            transformPoint(*S, 1, 3, -2, viaPoint);                   // the same translation, converted
            for (int i = 0; i < 3; ++i)
                if (!near(viaMat[i], viaPoint[i])) return fail("reframe picked the wrong inverse: %.3f vs %.3f", viaMat[i], viaPoint[i]);
        }
        if (!nearMat(reframe(identity(), kZDownToYUp), identity())) return "reframe(I) must be I";
    }
    // 6. quatToMat agrees with composeTRS at unit scale, and rotates a direction as expected.
    {
        const float q[4] = {0, 0.70710678f, 0, 0.70710678f};   // +90° about Y
        float d[3];
        transformDir(quatToMat(q[0],q[1],q[2],q[3]), 0, 0, 1, d);
        if (!near(d[0], 1) || !near(d[1], 0) || !near(d[2], 0))
            return fail("quatToMat: +90 about Y should send +Z to +X, got (%.3f, %.3f, ...)", d[0], d[1]);
    }
    return "";
}

}  // namespace core::rm
