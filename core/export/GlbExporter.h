#pragma once
#include "core/types/Clip.h"
#include "core/types/ExportOptions.h"
#include "core/types/Geometry.h"
#include "core/types/Material.h"
#include "core/types/Skeleton.h"
#include <QByteArray>
#include <QString>
#include <QVector>

// ── The one glTF 2.0 writer ─────────────────────────────────────────────────────────────────────
//
// Hand-written, no glTF library — all three projects made that choice independently and it is the
// right one: the byte format stays verifiable, and every scar below is a fix that had to be made
// INSIDE the writer. (D4 carried fastgltf + tinygltf in its manifest without ever calling either;
// both were removed 2026-09-17.)
//
// FRAME: the exporter does NOT rotate anything. core::Geometry, core::Skeleton and core::Clip are
// already in the canonical Y-up frame — the adapter applied its one core::rm::k*ToYUp at ingest.
// This is the "swap once" rule paying off: POE2's exporter rotated positions, normals, tangents,
// bind locals, inverse binds, translation keys and rotation keys, in six separate places, each a
// chance to pick the wrong inverse. Here there are none. An upAxis or yaw request is a WRAPPER
// ROOT NODE, never a second pass over the data.
//
// The scars this writer encodes, each from a real bug in one of the three tools:
//
//  1. inverseBindMatrices are stored in ROW-MAJOR element order. core::rm::Mat4 is row-major /
//     row-vector (v·M); glTF stores column-major / column-vector (M·v). Dumping our row-major
//     array verbatim is read back by glTF as our transpose, which is exactly the column-vector
//     matrix that reproduces v·M. POE2 stored it column-major and every skinned mesh deformed at
//     rest while the armature looked right (bind deviation 2.6 m → 0 after the fix; verified
//     against a strict glTF-convention solver and a real Blender import). Node `matrix` values
//     take the same path, for the same reason.
//
//  2. Bone nodes are ALWAYS written as TRS, never `matrix`. glTF forbids a `matrix` on a node that
//     an animation targets, and a bone is an animation target the moment any clip is written.
//     Blender 4.4 refuses to reconstruct the rest pose from the invalid pair and the mesh deforms
//     wildly; Blender 5.0 tolerates it, which is what made this hard to see.
//
//  3. `accessors` and `bufferViews` are assigned to the root object AFTER the animation block,
//     because that block appends to them. QJsonArray is a value type, so an early assignment
//     freezes a copy and every animation sampler ends up pointing past the end of the array.
//
//  4. Time (input) accessors are SHARED within a clip. D4 wrote a fresh timestamp array per
//     CHANNEL — on a 318-bone rig that is 954 byte-identical float arrays, accessors and
//     bufferViews per clip. Core keys the cache on the timestamps THEMSELVES rather than on the
//     key count, because core tracks are sparse per channel: two channels can have the same count
//     and different times, and D4's count-keyed cache would have silently shared them.
//
//  5. Tracks that never leave the joint's rest transform are dropped. On a character that is most
//     of the skeleton (a walk drives legs, spine and arms and leaves the fingers and the face rig
//     exactly where the bind pose put them) and keeping them turned a 400 KB clip into 3 MB of
//     constants. Lossless here precisely because bone nodes carry the rest TRS (scar 2).
//
//  6. Only the materials that VISIBLE parts reference are written, and the primitives' material
//     indices are remapped after the roster is pruned. A one-part export of a 20-material body
//     otherwise carried 20 materials' worth of PNGs.
//
//  7. An out-of-range joint index has its WEIGHT zeroed, not just the index clamped to 0. Clamping
//     alone (POE2's behaviour) drags the vertex onto the root bone; zeroing leaves it influenced by
//     its valid bones only, or unweighted at bind if it had none. The count comes back in Result so
//     a corrupt palette is reported rather than silently reshaping the mesh.
//
//  8. A clip that produced no channels is not written, and `animations` is omitted when empty —
//     an animation with an empty `channels` array is invalid glTF.
namespace core::glb {

// One loose texture to write beside a .gltf: the file name referenced by URI, and the bytes.
struct LooseImage { QString name; QByteArray bytes; };

// Everything a scene needs. `geometry` may be null only for an animation-library export.
struct Scene {
    const Geometry*    geometry  = nullptr;
    const Skeleton*    skeleton  = nullptr;   // null or empty ⇒ static export
    QVector<Clip>      clips;
    QVector<Material>  materials;             // indexed by Geometry::materialNames
    QVector<Hardpoint> hardpoints;
};

// What the caller needs to say honestly what it wrote. Every count here has been a question a user
// asked at least once ("why is my export 80 MB?", "why is this bone not animating?").
struct Result {
    bool ok = false;
    QString error;

    int primitives = 0;
    int materialsWritten = 0;
    int materialsPruned = 0;
    int imagesWritten = 0;
    int imagesDeduped = 0;
    int animationsWritten = 0;
    int tracksWritten = 0;
    int tracksElided = 0;        // constant tracks dropped (scar 5)
    int hardpointsWritten = 0;
    int badInfluences = 0;       // out-of-range joint indices whose weight was zeroed (scar 7)

    QString summary() const;     // one log line
};

// Build the .glb bytes without touching disk. When opt.looseTextures is set AND looseOut is
// non-null, images are referenced by URI and appended to looseOut instead of being embedded.
QByteArray build(const Scene& scene, const ExportOptions& opt, Result* result,
                 QVector<LooseImage>* looseOut = nullptr);

// Write to `path`. A path ending in .gltf (or opt.gltfSeparate) is split into text glTF + a
// sidecar .bin; loose textures land beside it.
bool write(const Scene& scene, const ExportOptions& opt, const QString& path, Result* result);

// Startup self-test: "" on success, else the first failing case. Covers the magic/length framing,
// JSON validity, material pruning + index remap, TRS-not-matrix on bone nodes, the row-major IBM
// order, time-accessor sharing, constant-track elision and the bad-influence rule.
QString selfTest();

}  // namespace core::glb
