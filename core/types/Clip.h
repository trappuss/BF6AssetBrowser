#pragma once
#include "core/math/RigMath.h"
#include <QString>
#include <QVector>

// ── The engine-neutral animation clip ───────────────────────────────────────────────────────────
// The three decoders are unrelated (geometry-export.md §3), but all three end up as per-joint TRS
// keys at the glTF boundary, so that is what core carries:
//   D4   AnimParser::DecodedAnim — DENSE, one key per frame per bone, bound by NAME HASH
//   POE2 AstSkeleton::Clip       — SPARSE per channel, times in FRAME UNITS, bound by bone INDEX
//   FOX  glb::GlbAnimation       — no clip type at all: a pose CALLBACK the exporter bakes
//
// Core is the sparse form in SECONDS bound by joint index: POE2 fits natively (÷ fps once at
// ingest — its keys are in frame units, which is the detail that made this worth stating), D4 fits
// as dense (times = i/fps, hash→index resolved at ingest), FOX fits as baked-dense.
namespace core {

struct Track {
    int joint = -1;
    QVector<float> tTimes;  QVector<std::array<float,3>> t;    // seconds
    QVector<float> qTimes;  QVector<rm::Quat>            q;    // xyzw
    QVector<float> sTimes;  QVector<std::array<float,3>> s;    // FOX never authors scale

    bool isEmpty() const { return t.isEmpty() && q.isEmpty() && s.isEmpty(); }

    // A CHANNEL equal to the joint's rest value within eps carries no information; the exporter
    // drops it and the node's rest TRS supplies the same pose. FOX does this and it is the single
    // biggest saving on a long clip (a walk leaves the fingers and the whole face rig exactly where
    // the bind pose put them: 3 MB of constants → 400 KB). Per channel, not per track, because a
    // bone that rotates but never translates is the common case, not the exception.
    bool tIsConstant(const float restT[3], float eps = 1e-6f) const;
    bool qIsConstant(const rm::Quat& restQ, float eps = 1e-6f) const;   // sign-agnostic: -q == q
    bool sIsConstant(const float restS[3], float eps = 1e-6f) const;
    bool isConstant(const float restT[3], const rm::Quat& restQ, const float restS[3], float eps = 1e-6f) const
    { return tIsConstant(restT, eps) && qIsConstant(restQ, eps) && sIsConstant(restS, eps); }

    // A channel whose time and value arrays disagree is a corrupt decode, not a short clip; every
    // consumer must skip it rather than read past the shorter array.
    bool wellFormed() const
    { return tTimes.size() == t.size() && qTimes.size() == q.size() && sTimes.size() == s.size(); }
};

struct Clip {
    QString name;
    float   fps = 30;
    float   duration = 0;        // seconds; derived at ingest, never re-derived downstream
    QVector<Track> tracks;
    bool    baked = false;       // [FOX] sampled from a solver, so dense by construction
    // [FOX] the tracks are not decoded yet: name/fps are right, duration may be 0, tracks empty.
    // IGameStore::bakeClip fills them on demand (model::ensureClip) — a Fox model binds tens of
    // thousands of clips and baking them all at load measured 99 s for one model.
    bool    lazy = false;
    // A lazy clip's identity in its STORE's own terms, opaque to core (FOX: archive file index and
    // clip number). bakeClip reads it from the stub it is handed, so a clip list that changed since
    // (FOX's Games filter re-lists a model's clips) can never bake a different clip under the same
    // row number. -1 = the store identifies clips by index alone.
    qint64  storeRef = -1;

    bool isEmpty() const { return tracks.isEmpty(); }
    // Samples at i/fps for i in [0, round(duration·fps)] — INCLUSIVE of both ends, so a 1-second
    // 30 fps clip has 31 frames and its last frame index is exactly duration·fps. D4's decoder
    // counts the same way; the first draft here counted 30 and the viewport parity test caught the
    // last frame sampling at 29/30 s.
    int  frameCount() const { return fps > 0 && duration > 0 ? int(duration * fps + 0.5f) + 1 : (tracks.isEmpty() ? 0 : 1); }
    void recomputeDuration()
    {
        duration = 0;
        for (const Track& tr : tracks)
            for (const QVector<float>* a : { &tr.tTimes, &tr.qTimes, &tr.sTimes })
                for (float v : *a) if (v > duration) duration = v;
    }
};

}  // namespace core
