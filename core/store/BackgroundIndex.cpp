#include "core/store/BackgroundIndex.h"
#include <QMetaObject>
#include <thread>

namespace core {

BackgroundIndex::~BackgroundIndex()
{
    // Supersede any in-flight build so it discards itself rather than calling into a dead object.
    m_generation.fetch_add(1);
}

void BackgroundIndex::setFingerprint(const QString& fp)
{
    if (fp == m_fingerprint) return;
    m_fingerprint = fp;
    reset();
}

void BackgroundIndex::reset()
{
    m_generation.fetch_add(1);          // in-flight builds now fail their generation check
    const bool was = m_ready.exchange(false);
    m_building.store(false);
    if (was) Q_EMIT readyChanged(false);
}

bool BackgroundIndex::start()
{
    if (m_building.load()) return false;

    if (loadCache()) {                  // already signed with this fingerprint
        m_installGeneration.fetch_add(1);
        m_ready.store(true);
        Q_EMIT readyChanged(true);
        return true;
    }

    m_building.store(true);
    const quint64 gen = m_generation.load();
    std::thread([this, gen] { runBuild(gen); }).detach();
    return true;
}

void BackgroundIndex::runBuild(quint64 generation)
{
    const auto cancelled = [this, generation] { return m_generation.load() != generation; };
    const auto reportFn  = [this, generation](const QString& m) {
        if (m_generation.load() == generation) report(m);
    };

    const bool ok = build(reportFn, cancelled);

    if (cancelled()) { return; }        // superseded: install nothing, touch nothing
    m_building.store(false);
    if (!ok) return;

    // Element 3: the swap happens on the owner thread, never on the worker.
    QMetaObject::invokeMethod(this, [this, generation] {
        if (m_generation.load() != generation) return;   // superseded between post and delivery
        install();
        saveCache();
        m_installGeneration.fetch_add(1);
        m_ready.store(true);
        Q_EMIT readyChanged(true);
    }, Qt::QueuedConnection);
}

void BackgroundIndex::report(const QString& message)
{
    QMetaObject::invokeMethod(this, [this, message] { Q_EMIT progress(message); }, Qt::QueuedConnection);
}

}  // namespace core
