#pragma once
// Resolved build → ONE figure: geometry, materials, rig, clips, and which parts came from which
// piece. The viewport, playback and every export use this same result (POE2's single-merge rule,
// docs/customize/CUSTOMIZE_CORE_DESIGN.md §3). No widgets: it runs on the tab's assembly worker.
#include "core/customize/CustomizeSource.h"
#include "core/store/IGameStore.h"
#include "core/types/Clip.h"
#include "core/types/Geometry.h"
#include "core/types/Material.h"
#include "core/types/Skeleton.h"

#include <QHash>
#include <QMutex>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>

namespace core::customize {

// Parsed meshes, kept across rebuilds: one slot change re-assembles every piece, and both FOX and
// POE2 used to reload all of them from the archive each time. Thread-safe; bounded by entry count
// with least-recently-used eviction (POE2 cleared the whole cache at 96).
class MeshCache {
public:
    struct Entry {
        bool ok = false;
        QString error;
        Geometry geo;
        QVector<Material> materials;   // one per geo.materialNames (or per part index, see assemble)
        Skeleton skeleton;             // loaded on first ask (rig)
        QVector<Clip> clips;           // loaded on first ask (clips)
        bool rigLoaded = false, clipsLoaded = false;
    };
    explicit MeshCache(int capacity = 160) : m_capacity(capacity) {}
    // Loads on a miss. wantRig / wantClips add the skeleton (and its clips) to the entry.
    std::shared_ptr<const Entry> get(IGameStore& store, const AssetId& id, bool wantRig, bool wantClips);
    // A mesh's materials under a look (2c): `e`'s own when the look is empty, otherwise the store's
    // resolveLookedMaterials, kept per (asset, look.key()) — so a look swapped back and forth, or
    // shared by every part (FOX's whole-character colour), decodes once. *note is the store's word.
    QVector<Material> lookedMaterials(IGameStore& store, const AssetId& id, const Entry& e, const MaterialLook& look, QString* note);
    void clear();
    int size() const;

private:
    mutable QMutex m_mutex;
    QHash<AssetId, std::shared_ptr<const Entry>> m_map;
    QVector<AssetId> m_order;          // most recent last
    struct Looked { QVector<Material> materials; QString note; };
    QHash<QString, Looked> m_looked;   // "<id>|<look key>" → materials; same capacity, own order
    QStringList m_lookedOrder;
    int m_capacity;
};

struct PieceRange {
    QString label, slot, tag;
    AssetId mesh;
    Placement placement = Placement::InPlace;
    int firstPart = 0, partCount = 0;
    bool seated = true;                // false: a socket that resolved nowhere — placed where authored
    int firstJoint = -1, jointCount = 0;   // bones this piece appended to the rig (InPlace / SocketRig)
    QString broughtBy;                 // a model a look brought (Piece::broughtBy)
    QString seatNote;                  // where seatOn put it ("seated at CNP_X on the barrel"), or why not
};

struct Assembly {
    bool ok = false;
    QString error;
    Geometry geo;
    QVector<Material> materials;       // one per material slot parts index
    Skeleton skeleton;
    QVector<Clip> clips;
    QVector<PieceRange> pieces;        // in geo.parts order, after the body's own parts
    int  bodyParts = 0;                // the body's own parts come first
    Geometry bodyGeo;                  // the body as loaded (hiddenBodyParts reads its part names)
    bool replacedBody = false;         // a ReplaceBody piece is the figure
    AssetId figure;                    // whose rig and clips these are (the body, or the form) — bakeClip's id
    QStringList notes;                 // "remapped 12 joints of x by name", "bone y not in rig — …"
    // geo.parts[i].visible already holds the SHIPPED state with every look applied (2c), and the
    // groups' own flags are folded into it (every group reads visible), so partsShown() is the
    // only other thing a reader needs.
};

// A look on a mesh standing alone (a piece exported by itself): the same group rule and the same
// store call assemble() uses, so a piece's own file matches what it looked like on the figure.
// Group flags are folded into the parts. What assemble would note is appended to *notes.
void applyLook(IGameStore& store, const AssetId& id, Geometry& geo, const Skeleton& skeleton, QVector<Material>& materials,
               const Look& look, bool decodeImages, const QString& label, QStringList* notes);

// Look::keepSubtree on one mesh, by its own rig. Returns false (mesh untouched) when the rig has
// no such bone. Exposed for the harness and the tests.
bool keepBoneSubtree(Geometry& g, const Skeleton& skeleton, const QString& bone);

// THE visibility rule (2c: one function feeds the viewport and every export). Part i is shown when
// its looked, shipped state is on, the game does not hide it on the body (hiddenBodyParts, body
// parts only, not under a ReplaceBody form), and the user has not unticked its piece.
QVector<bool> partsShown(const Assembly& a, const QSet<int>& hiddenBodyParts, const QHash<QString, bool>& pieceShown);

// includeHeld = false leaves out every piece marked held (POE2's "no weapons" export) — not every
// Socket piece: a class's bone-attached default pieces stay.
// wantClips loads the figure's clips (the viewport wants them; a static export does not).
Assembly assemble(IGameStore& store, MeshCache& cache, const Resolved& r, bool includeHeld, bool wantClips);

}  // namespace core::customize
