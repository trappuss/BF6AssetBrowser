#pragma once
// Small offscreen renders of indexed models, for a grid view. Ported from FOXAssetBrowser's
// gl/ThumbnailRenderer (the structure is FOX's: a dedicated thread owns one GL context and one
// framebuffer, base colour only, a bounded memory cache in front of a disk cache) with POE2's
// simpler API on the front (render a core::Geometry + base colours) and the addressing changed
// from FOX's archive file index to core::AssetId, so any plugin can feed it.
//
// Why it is built this way (FOX's reasoning, kept because every point was learned the hard way):
//   • IT DOES NOT RUN ON THE GUI THREAD. Parsing a model and decoding its textures is tens to
//     hundreds of milliseconds of CPU, and doing that between paint events is what made scrolling
//     stutter. The GUI thread only hands over an id and receives a finished image.
//   • One context and one framebuffer for the whole application, made once and reused.
//   • BASE COLOUR ONLY, a fixed two-light wrap so the silhouette reads at 96 px.
//   • Cached by (id, size); memory bounded by BYTES; a disk cache behind it keyed by the asset's own
//     id so it survives a re-index. A second launch reads PNGs instead of models.
//
// What the app supplies: a LOADER (called on the render thread — the IGameStore threading contract
// says loadModel/resolveMaterials are safe there), the disk directory (core owns no AppPaths), and
// whether the disk cache is on (core owns no settings key). ThumbnailRenderer itself must be used
// from the GUI thread; every entry point returns immediately.
#include "core/types/Asset.h"
#include "core/types/Geometry.h"

#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>

class QOpenGLContext;
class QOffscreenSurface;
class QOpenGLFramebufferObject;
class QOpenGLShaderProgram;
class QThread;

namespace core::gl {

// The disk cache's layout version, in the folder name. Bump it when the render changes in a way
// that makes old pictures wrong; the app prunes superseded folders at startup.
constexpr quint32 kThumbCacheVersion = 1;

// What the loader hands back: a mesh and one base-colour image per material (null = flat grey).
struct ThumbnailModel {
    bool ok = false;
    Geometry geo;
    QVector<QImage> baseByMaterial;
};
using ThumbnailLoader = std::function<ThumbnailModel(const AssetId& id)>;

// A fault guard around one thumbnail's load+draw, for the app that has one (D4/POE2/FOX all wrap
// this in their SEH translator: a partial install can make a parse fault, and so can a driver).
// Returns false if the body faulted. The default runs the body unguarded.
using ThumbnailGuard = std::function<bool(const std::function<void()>& body, QString* what)>;

struct ThumbKey {
    AssetId id; int size = 0;
    bool operator==(const ThumbKey& o) const { return id == o.id && size == o.size; }
};
inline uint qHash(const ThumbKey& k, uint seed = 0) { return qHash(k.id, seed) ^ uint(k.size * 2654435761u); }

// Lives on the render thread and owns every GL object.
class ThumbnailWorker : public QObject {
    Q_OBJECT
public:
    ThumbnailWorker(QOpenGLContext* ctx, QOffscreenSurface* surface, ThumbnailLoader loader, ThumbnailGuard guard);
public Q_SLOTS:
    void renderOne(const core::AssetId& id, int size, int generation, const QString& diskPath);
    void teardown();   // release GL while the context is still current on this thread
public:
    // Called ON the render thread (ThumbnailRenderer::setLoader queues it), between two renders.
    void replaceLoader(const ThumbnailLoader& loader) { m_loader = loader; }
Q_SIGNALS:
    void done(const core::AssetId& id, int size, const QImage& img, int generation, bool fromDisk);
private:
    QImage draw(const AssetId& id, int size);
    ThumbnailLoader m_loader;
    ThumbnailGuard m_guard;
    QOpenGLContext* m_ctx = nullptr;
    QOffscreenSurface* m_surface = nullptr;
    QOpenGLShaderProgram* m_prog = nullptr;
    QOpenGLFramebufferObject* m_fbo = nullptr;
    int m_fboSize = 0;
    unsigned int m_vao = 0, m_vbo = 0, m_ibo = 0, m_tex = 0;
    bool m_dead = false;
};

class ThumbnailRenderer : public QObject {
    Q_OBJECT
public:
    explicit ThumbnailRenderer(QObject* parent = nullptr);
    ~ThumbnailRenderer() override;

    // Wiring. The loader runs on the render thread. Replacing it later (a re-index, a style
    // toggle) reaches a running worker too — it used to keep the copy it was created with, so a
    // "textured icons" toggle never took effect. Pair a style change with its own setDiskDir.
    void setLoader(ThumbnailLoader loader);
    void setFaultGuard(ThumbnailGuard guard) { m_guard = std::move(guard); }
    void setDiskDir(const QString& dir) { m_diskDir = dir; }
    // How an asset is named on disk (IGameStore::diskCacheKey); unset = the id's value in hex.
    void setDiskKey(std::function<QString(const AssetId&)> key) { m_diskKey = std::move(key); }
    QString diskDir() const { return m_diskDir; }
    void setDiskEnabled(bool on) { m_diskEnabled = on; }
    bool diskEnabled() const { return m_diskEnabled && !m_diskDir.isEmpty(); }
    // The app's index is being rebuilt: hold requests until reset() (a render straddling an index
    // swap would read freed memory in FOX; the same hazard exists for any store).
    void setPaused(bool on) { m_paused = on; if (!on) pump(); }

    QPixmap cached(const AssetId& id, int size) const;
    bool has(const AssetId& id, int size) const;
    void request(const AssetId& id, int size);      // returns at once; ready() fires later
    int  prerender(const QVector<AssetId>& ids, int size);
    void cancelQueued();
    void prewarm();                                 // build the context + thread ahead of time
    int  outstanding() const;
    void reset();                                   // the index changed; blocks on an in-flight render
    int  refresh(const AssetId& id);                // forget one model (memory AND disk), re-queue
    int  cachedCount() const { return m_cache.size(); }
    QString diskPathFor(const AssetId& id, int size) const;

    struct Stats { int fromDisk = 0, rendered = 0, failed = 0; };
    Stats stats() const { return m_stats; }
    void resetStats() { m_stats = Stats(); }

Q_SIGNALS:
    void submit(const core::AssetId& id, int size, int generation, const QString& diskPath);   // internal
    void ready(const core::AssetId& id, int size);

private:
    bool ensureWorker();
    void pump();
    void onDone(const AssetId& id, int size, const QImage& img, int generation, bool fromDisk);
    void shutdown();

    ThumbnailLoader m_loader;
    ThumbnailGuard m_guard;
    QString m_diskDir;
    std::function<QString(const AssetId&)> m_diskKey;
    bool m_diskEnabled = true;
    bool m_paused = false;
    Stats m_stats;
    QThread* m_thread = nullptr;
    ThumbnailWorker* m_worker = nullptr;
    QOffscreenSurface* m_surface = nullptr;
    bool m_workerFailed = false;
    bool m_busy = false;
    ThumbKey m_inFlight;
    int m_generation = 0;
    QList<ThumbKey> m_queue;
    QSet<ThumbKey> m_queued;
    QSet<ThumbKey> m_staleInFlight;   // refresh() asked while this was rendering: drop it, draw again
    QHash<ThumbKey, QPixmap> m_cache;
    QList<ThumbKey> m_order;
    qint64 m_bytes = 0;
    // In-memory cache of rendered thumbnails (cost = pixmap bytes). Generous so scrolling back over a
    // large grid is instant and never re-renders — the heavy cost was the GL render, not the RAM.
    static constexpr qint64 kMaxBytes = 256ll * 1024 * 1024;
};

}  // namespace core::gl
