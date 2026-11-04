#pragma once
// Lazily produces per-asset thumbnails for a grid view, with a bounded cache so scrolling a large
// grid never re-renders what is already done (POE2 util/ThumbnailCache — "the only one with zero
// coupling", app-shell.md §1.1 — keyed by AssetId). Two modes, chosen by whether the render fn is
// thread-safe:
//   • background = true  — pure/thread-safe (a DDS decode): run on the global thread pool.
//   • background = false — must run on the GUI thread: a few per QTimer tick.
// Model thumbnails do NOT use this: core::gl::ThumbnailRenderer owns its own GL thread (phase 3).
// The render fn returns a QImage — a QPixmap can only be built on the GUI thread, which happens
// when the image lands. get() returns the finished pixmap or a null one while it schedules;
// ready(id) fires when a thumbnail becomes available so the model can repaint that row.
#include "core/types/Asset.h"

#include <QCache>
#include <QImage>
#include <QObject>
#include <QPixmap>
#include <QQueue>
#include <QSet>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

class QThreadPool;
class QTimer;

namespace core {

class ThumbnailCache : public QObject {
    Q_OBJECT
public:
    ThumbnailCache(std::function<QImage(const AssetId&)> render, bool background, QObject* parent = nullptr);
    ~ThumbnailCache() override;

    QPixmap get(const AssetId& id);        // cached pixmap, or a null pixmap + schedule a render
    bool    has(const AssetId& id) const { return m_cache.contains(id); }
    void    clear();                       // drop everything (the data source changed)
    void    setBudgetKB(int kb) { m_cache.setMaxCost(qMax(1024, kb)); }
    void    setGuiBudget(int n) { m_guiBudget = qMax(1, n); }   // GUI mode: renders per tick
    int     pending() const { return m_pending.size(); }
    // The pool background renders run on — the thumbnails' OWN, never QThreadPool::globalInstance():
    // Qt's image code (smooth scaling, conversions, border-pixmap painting) splits its work across
    // the global pool and waits for it, so decodes filling that pool stalled the GUI's own painting
    // (audit 2026-09-25 — found as a hang in shell_check). Exposed for tests.
    static QThreadPool* pool();

Q_SIGNALS:
    void ready(const core::AssetId& id);

private:
    void schedule(const AssetId& id);
    void processGui();
    void finishOne(const AssetId& id, const QImage& img);

    std::function<QImage(const AssetId&)> m_render;
    bool m_background;
    QCache<AssetId, QPixmap> m_cache{ 128 * 1024 };   // cost = KB of pixmap
    QSet<AssetId> m_pending;
    QQueue<AssetId> m_queue;
    QTimer* m_timer = nullptr;
    int m_guiBudget = 3;
    // Background jobs vs. what they read (audit 2026-09-25): the render fn reaches into its owner
    // (TexturesTab's store). A job queued before clear() or the destructor must not render, and
    // one already rendering must finish before either returns — the store may be reopened (a
    // reload) or destroyed (exit) right after. Shared with the jobs, so it outlives this object.
    struct JobState { std::mutex m; std::condition_variable cv; quint64 gen = 0; int running = 0; };
    std::shared_ptr<JobState> m_jobs = std::make_shared<JobState>();
    void retireJobs(const char* why);   // bump the generation, wait for renders in flight
};

}  // namespace core
