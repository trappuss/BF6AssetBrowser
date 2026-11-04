#pragma once
// The one load-and-export path every consumer shares — the Models tab's worker, the bulk workers,
// the thumbnail loader and the Customize-style tabs — so the viewport and the .glb can never
// disagree about what a model is (POE2's "one resolver" point, tabs-store.md §9.5: D4 had two
// material resolvers and they drifted).
//
// Thread contract: everything here calls only IGameStore's thread-safe set (loadModel, loadSkeleton,
// resolveMaterials, attachments, read) and touches no widget, so it runs on a worker.
#include "core/store/IGameStore.h"
#include "core/types/Clip.h"
#include "core/types/ExportOptions.h"
#include "core/types/Geometry.h"
#include "core/types/Material.h"
#include "core/types/Skeleton.h"

#include <QHash>
#include <QString>
#include <QVector>
#include <functional>

namespace core::model {

struct Loaded {
    bool ok = false;
    QString error;
    AssetId id;
    Geometry geo;
    Skeleton skeleton;
    QVector<Clip> clips;
    QVector<Material> materials;
    QVector<Hardpoint> hardpoints;
    QVector<Attachment> attachments;   // as declared; see assemble()
};

// Geometry + skeleton (+ clips) + materials (+ decoded images) for one asset. A store may list
// its clips lazily (Clip::lazy): the names are there, the tracks are not — ensure them below.
// `withAttachments` false skips IGameStore::attachments — a thumbnail, or an export that will not
// assemble, has no use for the list (D4 reads up to 80 actor files to build it).
Loaded load(IGameStore& store, const AssetId& id, bool decodeClips, bool decodeImages, bool withAttachments = true);

// Decode clip i if it is still lazy (IGameStore::bakeClip). True when it is decoded — a clip whose
// every channel matched the rest pose decodes to NO tracks and is still true (it plays as the bind
// pose; the exporter omits it); a clip the store cannot bake stays lazy and `error` says why.
bool ensureClip(IGameStore& store, Loaded& m, int i, QString* error = nullptr);
// Every clip (onlyClip < 0) or just that one; returns how many stayed lazy (each is logged).
int ensureClips(IGameStore& store, Loaded& m, int onlyClip = -1);

// Merge a body with the attachments that are ON into ONE geometry. Placement is the store's when it
// placesAttachments() (D4); otherwise the core's rule: baked at the parent bone's model-space bind
// and weighted 100% to that bone, so the shared skinning path animates it rigidly with the body —
// POE2's assembleForExport rule, over core types (bind = inverse(Joint::inverseBind), canonical
// frame). Parts and materials are concatenated; part names keep the attachment's own. A piece that
// cannot be placed is named in `skipped`, never dropped silently.
struct Assembly {
    Geometry geo;                    // the body when nothing is on or nothing could be placed
    QVector<Material> materials;     // one per geo.materialNames
    QVector<int> attachmentOfPart;   // per part: the attachment it came from, -1 = the body's own
    QStringList skipped;             // "<label> (<why>)", for the log
    QHash<int, QString> whyNot;      // attachment index → why it was not placed
    int placed = 0;
};
// `on`: one flag per m.attachments; empty = each piece's defaultOn.
Assembly assemble(IGameStore& store, const Loaded& m, const QVector<bool>& on, bool decodeImages);

struct ExportResult {
    bool ok = false;
    QString error;
    QString summary;      // glb::Result::summary()
    QString path;
};

// Export one asset to `outPath` with `opt`. `partSubset` (indices into the body's parts) restricts
// the written parts; empty = all visible. includeAttachments assembles first — the pieces in
// `attachmentsOn` (the viewport's ticks for the model on screen; empty = each piece's defaultOn).
// The materials are decoded only when the file will carry images.
// `adjust`, when set, edits the loaded asset (geometry and materials) before attachments are
// assembled and the file is written — Customize applies a piece's look through it. Its bool says
// whether images are wanted.
ExportResult exportOne(IGameStore& store, const AssetId& id, const ExportOptions& opt, const QString& outPath,
                       const QVector<int>& partSubset = {}, const QVector<bool>& attachmentsOn = {},
                       const std::function<void(Loaded&, bool decodeImages)>& adjust = {});

// Where a game path lands when originals are mirrored under `root`: always INSIDE it. Leading
// slashes and a drive prefix are dropped and "." / ".." segments removed — FOX's paths start with
// "/Assets/…", and QDir::filePath returns an absolute name unchanged on some platforms (audit
// 2026-09-25). Every raw-extraction path goes through this.
QString mirrorPath(const QString& root, const QString& gamePath);

// A collision-free file name in `dir`: stem.ext, then stem_1.ext… (POE2's rule).
QString uniquePath(const QString& dir, const QString& stem, const QString& ext);

}  // namespace core::model
