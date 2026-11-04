#pragma once
#include "core/types/Asset.h"
#include "core/types/Clip.h"
#include "core/types/Geometry.h"
#include "core/types/Material.h"
#include "core/types/Skeleton.h"
#include <QImage>
#include <QPixmap>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>

// ── The one seam between the generic UI and a game ──────────────────────────────────────────────
//
// Every method below is the generalisation of a function that EXISTS in all three tools today; the
// per-project mapping is the table in core-audit/tabs-store.md §7.1. Nothing here is speculative:
// POE2's AssetStore is already almost exactly this shape, which is why its BulkExtractor needs only
// the store and not the tab.
//
// THREADING — stated because the three projects disagree today and the core ModelsTab loads on a
// worker thread for every game (plan §8.4, settled):
//   * open / fingerprint / buildSecondaryIndexes : called once, on a worker; not re-entrant.
//   * read / readPath / dependencyFiles / loadModel / loadSkeleton / bakeClip / resolveMaterials /
//     resolveLookedMaterials / hardpoints / attachments / placeAttachment / loadTexture : MUST be
//     thread-safe — the model loader, the Customize assembly worker and the bulk workers call them
//     concurrently (the list was written before Customize; audit 2026-09-25 matched it to the callers).
//   * rowCount / row / searchBlob / tagGroups / hasTag / gameIcon : GUI thread only (D4's
//     SnoIndex name caches and FOX's lazily-rebuilt ModelTags are not safe otherwise).
// A plugin that cannot honour the thread-safe set says so with isThreadSafe() == false and the
// core loader serialises it rather than corrupting an index quietly.
namespace core {

using ProgressFn = std::function<void(const QString&)>;

struct TextureInfo {
    int width = 0, height = 0, mips = 1, slices = 1;
    QString format;             // "BC7", "ETC2", "DXT5"…
    QString note;               // e.g. "placeholder 128² substituted"
};

struct Attachment {
    QString label;              // "coat", "feathers"
    QString parentBone;         // [POE2] the bone the core bakes it on; [D4] the hardpoint's name (display)
    AssetId mesh;               // resolved drawable, invalid when it could not be resolved
    QString unresolvedNote;     // why, when mesh is invalid — never silently dropped
    QString group;              // [D4] "<actor> · On spawn (held)" — the panel shows it; empty = none
    QString key;                // the store's own identity for placeAttachment; empty when unused
    bool    defaultOn = true;   // [D4] false: listed, attached only when ticked (D4 attaches none by default)
};

struct Use {
    AssetId id;
    QString role;               // "albedo", "material", "actor"
    QString label;
};

// RAII cache that lives only for one bulk run. [D4 MaterialDecode::TextureCacheScope]
// [FOX extract::blobcache::Scope] [POE2 none — returns null, which is a valid answer]
class RunCache { public: virtual ~RunCache() = default; };

class IGameStore {
public:
    virtual ~IGameStore() = default;

    // ── identity ────────────────────────────────────────────────────────────────────────────
    virtual QString gameName() const = 0;           // "Diablo IV", "Fox Engine", "Path of Exile 2"
    virtual bool    isThreadSafe() const { return true; }

    // ── lifecycle ───────────────────────────────────────────────────────────────────────────
    virtual bool    open(const QString& dataDir, QString* error, ProgressFn progress = {}) = 0;
    virtual bool    isOpen() const = 0;
    // Signs every derived cache. Must change when the game build, the community data, or the
    // archive set changes: D4 folds in the TACT key set, FOX every archive's mtime, POE2 the index
    // file's size+mtime.
    virtual QString fingerprint() const = 0;
    virtual void    buildSecondaryIndexes(ProgressFn progress = {}) {}

    // ── rows ────────────────────────────────────────────────────────────────────────────────
    virtual QVector<QString> kinds() const = 0;     // which list tabs exist, in tab order
    virtual int      rowCount(const QString& kind) const = 0;
    virtual AssetRow row(const QString& kind, int i) const = 0;
    virtual QString  searchBlob(const AssetId&) const { return {}; }   // the '#' metadata haystack
    virtual QVector<TagGroup> tagGroups(const QString& kind) const { return {}; }
    virtual bool     hasTag(const AssetId&, const QString& tag) const { return false; }

    // ── bytes ───────────────────────────────────────────────────────────────────────────────
    // `part` selects among several blobs for one id: D4 needs "meta" and "payload"; the others
    // ignore it. A plugin lists what it accepts in parts().
    virtual QByteArray read(const AssetId&, const QString& part, QString* error) = 0;
    virtual QVector<QString> parts() const { return { QString() }; }
    // The ORIGINAL files an asset depends on (its descriptor, materials, textures, rig…), as the
    // game's own paths, and how to read one — the raw-originals extraction (POE2 collectAssetFiles +
    // readFile). A store with no path addressing returns nothing and the action stays disabled.
    virtual QStringList dependencyFiles(const AssetId&) { return {}; }
    virtual QByteArray readPath(const QString& gamePath, QString* error) { Q_UNUSED(gamePath); Q_UNUSED(error); return {}; }

    // ── models ──────────────────────────────────────────────────────────────────────────────
    virtual bool loadModel(const AssetId&, Geometry&, QString* error) = 0;
    virtual Skeleton loadSkeleton(const AssetId&, const Geometry&, bool decodeClips, QVector<Clip>* clipsOut) = 0;
    // A store that lists clips with Clip::lazy = true decodes one here (thread-safe set). `clip` is
    // the stub loadSkeleton returned at `clipIndex`; on success its tracks/duration are filled and
    // lazy is cleared. A store that never returns lazy clips leaves the default.
    virtual bool bakeClip(const AssetId&, int clipIndex, Clip& clip, QString* error) { Q_UNUSED(clipIndex); Q_UNUSED(clip); Q_UNUSED(error); return false; }
    virtual QVector<Material> resolveMaterials(const AssetId&, const Geometry&, bool decodeImages) = 0;
    // resolveMaterials with a Customize look applied (texture swaps, the game's own inputs). A
    // store that does not know how applies nothing and SAYS so in *note, so a look never seems to
    // have worked when it did not. A store that does may say what it did ("3 textures swapped").
    // Thread-safe set. The core caches the result per (asset, look.key()).
    virtual QVector<Material> resolveLookedMaterials(const AssetId& id, const Geometry& g, bool decodeImages,
                                                     const MaterialLook& look, QString* note)
    {
        if (note && !look.isEmpty()) *note = QStringLiteral("this game's store does not apply looks yet — shown as shipped");
        return resolveMaterials(id, g, decodeImages);
    }
    virtual QVector<Hardpoint> hardpoints(const AssetId&, const Skeleton&) { return {}; }
    virtual QVector<Attachment> attachments(const AssetId&) { return {}; }
    // Where an attachment goes is the game's rule: the core's own (model::assemble) bakes a piece
    // rigidly at its parentBone's bind — POE2's. A store with a richer rule (D4: hardpoint × grip
    // transform × scale, a mount seated under the rider by its saddle) says placesAttachments() and
    // bakes the piece itself: `placed` in the body's model space (canonical frame), vertices weighted
    // to `skel`'s joints BY INDEX (or unweighted = static), and `materials` one per
    // placed.materialNames. False + `error` = not placed (listed, named as skipped).
    virtual bool placesAttachments() const { return false; }
    virtual bool placeAttachment(const AssetId& body, const Skeleton& skel, const Attachment& a, bool decodeImages,
                                 Geometry& placed, QVector<Material>& materials, QString* error)
    { Q_UNUSED(body); Q_UNUSED(skel); Q_UNUSED(a); Q_UNUSED(decodeImages); Q_UNUSED(placed); Q_UNUSED(materials); if (error) *error = QStringLiteral("not placed by this store"); return false; }
    // A placed piece whose parent bone has NO rest placement in the game's data: only a clip puts
    // it anywhere, so in the bind pose it sits at the origin (D4's Butcher: both weapon bones hang
    // ten bones deep with zero rest and zero bind translation — measured 2026-09-23). When a shown
    // piece says so and no clip is chosen, the Models tab shows the model on restClip() at frame 0,
    // paused: the pose the game shows (user decision, 2026-09-23).
    virtual bool attachmentNeedsPose(const AssetId& body, const Attachment& a) { Q_UNUSED(body); Q_UNUSED(a); return false; }
    // The clip the game plays when the model is doing nothing — its idle — as an index into
    // `clips` (the list loadSkeleton returned), or -1 when the game's data names none. From the
    // game's own designation (D4: the AnimSet's AnimKey_Neutral power), never from a clip's name.
    virtual int restClip(const AssetId& id, const QVector<Clip>& clips) { Q_UNUSED(id); Q_UNUSED(clips); return -1; }

    // ── textures ────────────────────────────────────────────────────────────────────────────
    virtual QImage loadTexture(const AssetId&, TextureInfo* info, QString* error) = 0;

    // Texture-decode cap for the NEXT loadTexture/resolveMaterials calls on THIS thread. A store may
    // decode smaller for the live UI (thumbnails, 3D preview) and full-size for a saved export; this lets
    // each context choose independently and race-free, because it is scoped to the calling thread (the
    // thumbnail renderer runs on its own thread). `capPx`: 0 = native (full res, incl. a streamed HD
    // pack), N > 0 = cap the longest side to N px. Returns an opaque previous value to pass back to
    // restore it (RAII). No-op default (a store that always decodes full res needs nothing):
    //   const int prev = store.setDecodeTextureCap(0);          // full res for an export
    //   QScopeGuard g([&]{ store.setDecodeTextureCap(prev); }); // back to the thread's prior cap
    virtual int setDecodeTextureCap(int capPx) { (void)capPx; return 0; }

    // ── links and icons ─────────────────────────────────────────────────────────────────────
    virtual QVector<Use> usedBy(const AssetId&) { return {}; }
    virtual QVector<Use> uses(const AssetId&)   { return {}; }
    virtual QPixmap gameIcon(const AssetId&, int px) { return {}; }
    // The name a per-asset disk cache (model thumbnails) files this asset under. It must survive a
    // rescan: the default is the id, right for a store whose ids are stable (D4's SNO, POE2's path
    // hash). FOX's ids are file indices, renumbered by every rescan, so FOX names it by path hash —
    // as FOX's own thumbnail cache did (gl/ThumbnailRenderer.cpp:392-404).
    virtual QString diskCacheKey(const AssetId& id) const { return QString::number(id.value, 16).rightJustified(16, QLatin1Char('0')); }
    virtual QVector<AssetId> variants(const AssetId&) { return {}; }

    // ── run-scoped cache ────────────────────────────────────────────────────────────────────
    virtual std::unique_ptr<RunCache> beginRunCache() { return nullptr; }
};

}  // namespace core
