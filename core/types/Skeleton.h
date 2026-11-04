#pragma once
#include "core/math/RigMath.h"
#include <QString>
#include <QVector>
#include <cstdint>

// ── The engine-neutral rig ──────────────────────────────────────────────────────────────────────
// From the three real rigs (geometry-export.md §2):
//   D4  ModelJoint        — name + DJB2 hash, parent, IBM + localMatrix (col-major), rest TRS (Z-up)
//   POE2 AstSkeleton::Bone— name, parent (resolved from sibling/child links), model bind + inverse
//   FOX  FmdlBone         — name + StrCode32, parent, TRANSLATION-ONLY bind (localPos/worldPos)
//
// Bind is carried BOTH ways on purpose: restT/Q/S is what an exporter writes as a node TRS (glTF
// forbids a matrix on an animated node), and inverseBind is the payload the skin needs. Deriving
// one from the other at export time is where two of the three projects lost accuracy.
namespace core {

struct Joint {
    QString  name;
    uint32_t nameHash = 0;      // [D4 DJB2] [FOX StrCode32] — 0 for POE2, which binds clips by index
    int      parent = -1;       // core GUARANTEES parents precede children (see Skeleton::sorted)

    // Bind-LOCAL TRS, canonical frame. [D4 restQ/T/S swapped from Z-up at ingest]
    // [FOX q=identity, t=localPos, s=1] [POE2 decomposeTRS(bind · inverse(parentBind))]
    rm::Quat restQ = {0,0,0,1};
    float    restT[3] = {0,0,0};
    float    restS[3] = {1,1,1};

    rm::Mat4 inverseBind = rm::identity();   // model-space inverse bind, canonical frame

    // D4 physics markers. They drive Retarget::collapseClothChains and the D4 cloth extension;
    // every other engine leaves them false. Kept in core rather than a side-car because they are
    // two bits and the alternative is a parallel array keyed by joint index.
    bool cloth = false;
    bool chain = false;
};

struct Skeleton {
    QVector<Joint> joints;
    int     nBaseBones = 0;     // [D4] joints past this are the game's simulated set; 0 = unknown
    QString note;               // [POE2] "bones only (version 9 clips not decoded)" and the like

    bool isEmpty() const { return joints.isEmpty(); }
    int  size() const { return joints.size(); }

    // The invariant every consumer assumes: D4's skinning, overlays and exporter all rely on it,
    // and POE2's parser does NOT provide it (it resolves parents recursively instead). An adapter
    // calls sortParentsFirst() so the invariant holds family-wide rather than per project.
    bool parentsPrecedeChildren() const
    {
        for (int i = 0; i < joints.size(); ++i)
            if (joints[i].parent >= i) return false;
        return true;
    }

    // Stable topological sort; returns the old→new index map so vertex joints, hardpoints and clip
    // tracks can be remapped with it. A cycle (corrupt file) leaves the order untouched and returns
    // an empty map — fail closed, never silently reorder a rig we do not understand.
    QVector<int> sortParentsFirst();
};

// A socket: a named transform parented to a joint, exported as an empty.
// [D4 ModelHardpoint "HP_*"] [FOX fox::ConnectPoint "CNP_*"] [POE2 none]
struct Hardpoint {
    QString  name;
    int      joint = -1;
    uint32_t jointHash = 0;     // survives a skeleton reorder or reduction
    rm::Quat q = {0,0,0,1};
    float    t[3] = {0,0,0};
    float    s[3] = {1,1,1};
    // [D4] an authored (non-identity) socket is MODEL-space, so its local transform is
    // inverseBind · socket; FOX's are always bone-local because its bind is translation-only.
    bool modelSpace = false;
};

}  // namespace core
