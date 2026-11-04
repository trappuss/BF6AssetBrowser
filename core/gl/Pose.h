#pragma once
#include "core/math/RigMath.h"
#include "core/types/Clip.h"
#include "core/types/Skeleton.h"
#include <QVector>

// ── Pose evaluation over the core types ─────────────────────────────────────────────────────────
// The three viewports each had their own (D4 in the widget, POE2 in AstSkeleton::skinMatrices,
// FOX in a solver callback). This is the one the core widget, the thumbnail renderer and any
// tool use, so "what pose is frame 12 of clip X" has exactly one answer.
namespace core::gl {

// Local (parent-relative) TRS matrix of every joint at time `t` seconds: the clip's track where
// it has one, the rest pose otherwise. Sparse tracks interpolate (nlerp for rotation, lerp for
// translation/scale) and clamp at both ends; a null clip is the rest pose.
QVector<rm::Mat4> localPose(const Skeleton& sk, const Clip* clip, float t);

// World matrices from locals. Does NOT assume parents precede children (core guarantees it, D4's
// own widget learned not to rely on it): unresolved joints are settled in later passes, and a
// parent cycle is broken by treating the offenders as roots rather than looping forever.
QVector<rm::Mat4> worldPose(const Skeleton& sk, const QVector<rm::Mat4>& local);

// Skin palette = inverseBind · world, per joint (row-vector: "into bone space, then to world").
QVector<rm::Mat4> skinPalette(const Skeleton& sk, const QVector<rm::Mat4>& world);

// Sample one track at time t. Exposed for the tests and the skeleton overlay.
void sampleTrack(const Track& tr, float t, const Joint& rest, float outT[3], rm::Quat& outQ, float outS[3]);

const char* selfTest();   // "" on success

}  // namespace core::gl
