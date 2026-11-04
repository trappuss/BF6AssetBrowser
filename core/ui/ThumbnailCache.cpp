#include "core/ui/ThumbnailCache.h"

#include <QMetaObject>
#include <QPointer>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

namespace core {
namespace {
class FnRunnable : public QRunnable {
public:
    explicit FnRunnable(std::function<void()> fn) : m_fn(std::move(fn)) { setAutoDelete(true); }
    void run() override { m_fn(); }
private:
    std::function<void()> m_fn;
};
}  // namespace

QThreadPool* ThumbnailCache::pool()
{
    static QThreadPool* p = [] {
        auto* tp = new QThreadPool;   // never destroyed: jobs are retired by their caches before exit
        tp->setMaxThreadCount(qMax(1, QThread::idealThreadCount() - 1));   // one core left for the GUI
        return tp;
    }();
    return p;
}

ThumbnailCache::ThumbnailCache(std::function<QImage(const AssetId&)> render, bool background, QObject* parent)
    : QObject(parent), m_render(std::move(render)), m_background(background)
{
    if (!m_background) {
        m_timer = new QTimer(this);
        m_timer->setInterval(16);          // ~one screen refresh; renders a few heavy thumbnails per tick
        connect(m_timer, &QTimer::timeout, this, &ThumbnailCache::processGui);
    }
}

ThumbnailCache::~ThumbnailCache()
{
    retireJobs("the thumbnail cache is going away");
}

void ThumbnailCache::retireJobs(const char* why)
{
    std::unique_lock<std::mutex> l(m_jobs->m);
    ++m_jobs->gen;
    if (m_jobs->running == 0) return;
    // A render posts its result; it never waits on the GUI thread, so this cannot deadlock.
    if (!m_jobs->cv.wait_for(l, std::chrono::seconds(30), [&] { return m_jobs->running == 0; }))
        qWarning("thumbnails: %d render(s) still running after 30 s (%s)", m_jobs->running, why);
}

QPixmap ThumbnailCache::get(const AssetId& id)
{
    if (QPixmap* hit = m_cache.object(id)) return *hit;
    schedule(id);
    return QPixmap();
}

void ThumbnailCache::schedule(const AssetId& id)
{
    if (m_pending.contains(id)) return;
    m_pending.insert(id);
    if (m_background) {
        // QThreadPool bounds real parallelism to its maxThreadCount, so a burst never oversubscribes.
        // QPointer: a job that finishes after the cache is gone must post to nothing, not to freed
        // memory (POE2 captured a raw this here).
        QPointer<ThumbnailCache> self(this);
        const auto render = m_render;
        std::shared_ptr<JobState> st = m_jobs;
        quint64 gen;
        { std::lock_guard<std::mutex> l(st->m); gen = st->gen; }
        pool()->start(new FnRunnable([self, id, render, st, gen]() {
            {
                std::lock_guard<std::mutex> l(st->m);
                if (st->gen != gen) return;   // cleared or destroyed since it was queued: render nothing
                ++st->running;
            }
            const QImage img = render(id);
            { std::lock_guard<std::mutex> l(st->m); --st->running; }
            st->cv.notify_all();
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, id, img]() { if (self) self->finishOne(id, img); },
                                      Qt::QueuedConnection);
        }));
    } else {
        m_queue.enqueue(id);
        if (m_timer && !m_timer->isActive()) m_timer->start();
    }
}

void ThumbnailCache::processGui()
{
    int budget = m_guiBudget;
    while (budget-- > 0 && !m_queue.isEmpty()) {
        const AssetId id = m_queue.dequeue();
        if (!m_pending.contains(id)) continue;   // was cleared
        finishOne(id, m_render(id));
    }
    if (m_queue.isEmpty() && m_timer) m_timer->stop();
}

void ThumbnailCache::finishOne(const AssetId& id, const QImage& img)
{
    if (!m_pending.remove(id)) return;          // cleared while in flight: drop the stale result
    if (!img.isNull()) {
        QPixmap pm = QPixmap::fromImage(img);
        const int costKB = qMax(1, int((qint64(pm.width()) * pm.height() * 4) / 1024));
        m_cache.insert(id, new QPixmap(pm), costKB);
    } else {
        // A failed render: a 1x1 transparent tile ends the retry loop (a null would re-schedule
        // on every paint) and reads as "no icon" in the model.
        QPixmap blank(1, 1); blank.fill(Qt::transparent);
        m_cache.insert(id, new QPixmap(blank), 1);
    }
    emit ready(id);
}

void ThumbnailCache::clear()
{
    retireJobs("the data source changed");
    m_cache.clear();
    m_pending.clear();
    m_queue.clear();
    if (m_timer) m_timer->stop();
}

}  // namespace core
