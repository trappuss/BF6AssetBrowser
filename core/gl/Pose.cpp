#include "core/gl/Pose.h"
#include <cmath>

namespace core::gl {
namespace {

// Index of the last key at or before t, or -1 when t precedes the first key.
int keyBefore(const QVector<float>& times, float t)
{
    int lo = 0, hi = times.size() - 1, best = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (times[mid] <= t) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return best;
}

template <typename T, typename Lerp>
void sampleChannel(const QVector<float>& times, const QVector<T>& vals, float t, T& out, const Lerp& lerp)
{
    if (vals.isEmpty() || times.size() != vals.size()) return;   // absent or corrupt → keep rest
    if (vals.size() == 1 || t <= times.first()) { out = vals.first(); return; }
    if (t >= times.last()) { out = vals.last(); return; }
    const int i = keyBefore(times, t);
    if (i < 0) { out = vals.first(); return; }
    if (i + 1 >= vals.size()) { out = vals.last(); return; }
    const float span = times[i + 1] - times[i];
    // An exact key hit returns the key untouched, so a frame-rate-aligned sample reproduces a
    // dense clip bit-for-bit (this is what makes D4's per-frame skinning comparable).
    if (span <= 0.0f || t == times[i]) { out = vals[i]; return; }
    const float u = (t - times[i]) / span;
    out = lerp(vals[i], vals[i + 1], u);
}

}  // namespace

void sampleTrack(const Track& tr, float t, const Joint& rest, float outT[3], rm::Quat& outQ, float outS[3])
{
    std::array<float, 3> T{rest.restT[0], rest.restT[1], rest.restT[2]};
    std::array<float, 3> S{rest.restS[0], rest.restS[1], rest.restS[2]};
    rm::Quat Q = rest.restQ;
    const auto lerp3 = [](const std::array<float, 3>& a, const std::array<float, 3>& b, float u) {
        return std::array<float, 3>{a[0] + (b[0] - a[0]) * u, a[1] + (b[1] - a[1]) * u, a[2] + (b[2] - a[2]) * u};
    };
    const auto nlerp = [](const rm::Quat& a, const rm::Quat& b, float u) {
        rm::Quat o; rm::quatNlerp(a.data(), b.data(), u, o.data()); return o;
    };
    sampleChannel(tr.tTimes, tr.t, t, T, lerp3);
    sampleChannel(tr.qTimes, tr.q, t, Q, nlerp);
    sampleChannel(tr.sTimes, tr.s, t, S, lerp3);
    outT[0] = T[0]; outT[1] = T[1]; outT[2] = T[2];
    outS[0] = S[0]; outS[1] = S[1]; outS[2] = S[2];
    outQ = Q;
}

QVector<rm::Mat4> localPose(const Skeleton& sk, const Clip* clip, float t)
{
    const int nb = sk.size();
    QVector<rm::Mat4> local(nb);
    // Rest pose for everything first; a clip then overrides the joints it has tracks for. A track
    // pointing at a joint the skeleton does not have is ignored — the rig is the authority.
    for (int j = 0; j < nb; ++j) {
        const Joint& jt = sk.joints[j];
        local[j] = rm::composeTRS(jt.restT, jt.restQ.data(), jt.restS);
    }
    if (!clip) return local;
    for (const Track& tr : clip->tracks) {
        if (tr.joint < 0 || tr.joint >= nb || !tr.wellFormed()) continue;
        float T[3], S[3]; rm::Quat Q;
        sampleTrack(tr, t, sk.joints[tr.joint], T, Q, S);
        local[tr.joint] = rm::composeTRS(T, Q.data(), S);
    }
    return local;
}

QVector<rm::Mat4> worldPose(const Skeleton& sk, const QVector<rm::Mat4>& local)
{
    const int nb = sk.size();
    QVector<rm::Mat4> world(nb);
    if (local.size() != nb) return world;
    QVector<quint8> done(nb, 0);
    int remaining = 0;
    for (int j = 0; j < nb; ++j) {
        const int p = sk.joints[j].parent;
        if (p < 0 || p >= nb)   { world[j] = local[j]; done[j] = 1; }
        else if (p < j)         { world[j] = rm::mul(local[j], world[p]); done[j] = 1; }   // child then parent
        else                    { ++remaining; }
    }
    while (remaining > 0) {
        int progressed = 0;
        for (int j = 0; j < nb; ++j) {
            if (done[j]) continue;
            const int p = sk.joints[j].parent;
            if (p >= 0 && p < nb && done[p]) { world[j] = rm::mul(local[j], world[p]); done[j] = 1; --remaining; ++progressed; }
        }
        if (!progressed) {   // a parent cycle: break it rather than spin forever
            for (int j = 0; j < nb; ++j) if (!done[j]) { world[j] = local[j]; done[j] = 1; }
            remaining = 0;
        }
    }
    return world;
}

QVector<rm::Mat4> skinPalette(const Skeleton& sk, const QVector<rm::Mat4>& world)
{
    const int nb = sk.size();
    QVector<rm::Mat4> pal(nb);
    if (world.size() != nb) return pal;
    for (int j = 0; j < nb; ++j) pal[j] = rm::mul(sk.joints[j].inverseBind, world[j]);
    return pal;
}

const char* selfTest()
{
    // Two bones, the child 2 up the Y axis, a clip rotating the child 90 degrees about Z at t=1.
    // A point on the child at (1,2,0): bind-local (1,0,0); at t=1 it must land at (0,3,0) — the
    // same analytic case the export gate uses, so the viewport and the exporter agree by
    // construction on what the pose means.
    Skeleton sk;
    Joint root; root.name = QStringLiteral("root");
    Joint child; child.name = QStringLiteral("child"); child.parent = 0; child.restT[1] = 2;
    child.inverseBind = rm::identity(); child.inverseBind[13] = -2;
    sk.joints = {root, child};
    Clip clip; Track tr; tr.joint = 1; tr.qTimes = {0.0f, 1.0f};
    tr.q = {{{0, 0, 0, 1}}, {{0, 0, 0.70710678f, 0.70710678f}}};
    clip.tracks = {tr}; clip.recomputeDuration();

    auto skinAt = [&](float t, float out[3]) {
        const auto pal = skinPalette(sk, worldPose(sk, localPose(sk, &clip, t)));
        rm::transformPoint(pal[1], 1, 2, 0, out);
    };
    float p[3];
    skinAt(0.0f, p);
    if (std::fabs(p[0] - 1) > 1e-5f || std::fabs(p[1] - 2) > 1e-5f || std::fabs(p[2]) > 1e-5f)
        return "Pose: bind pose must leave the vertex where it was authored";
    skinAt(1.0f, p);
    if (std::fabs(p[0]) > 1e-5f || std::fabs(p[1] - 3) > 1e-5f || std::fabs(p[2]) > 1e-5f)
        return "Pose: 90-degree child rotation should carry (1,2,0) to (0,3,0)";
    skinAt(0.5f, p);   // halfway: nlerp of a 90-degree turn is 45 degrees exactly (symmetric)
    const float c = 0.70710678f;
    if (std::fabs(p[0] - c) > 1e-4f || std::fabs(p[1] - (2 + c)) > 1e-4f)
        return "Pose: midway sample should be the 45-degree pose";
    skinAt(5.0f, p);   // past the end clamps to the last key
    if (std::fabs(p[0]) > 1e-5f || std::fabs(p[1] - 3) > 1e-5f)
        return "Pose: a time past the last key must clamp, not extrapolate";

    // Parents after children: the same rig with the array reversed must give the same world pose.
    Skeleton rev; rev.joints = {child, root}; rev.joints[0].parent = 1; rev.joints[1].parent = -1;
    const auto w = worldPose(rev, localPose(rev, nullptr, 0));
    if (std::fabs(w[0][13] - 2) > 1e-6f) return "Pose: a child listed before its parent was not resolved";

    // A cycle terminates (both joints claim the other as parent).
    Skeleton cyc; cyc.joints = {root, child}; cyc.joints[0].parent = 1; cyc.joints[1].parent = 0;
    (void)worldPose(cyc, localPose(cyc, nullptr, 0));
    return "";
}

}  // namespace core::gl
