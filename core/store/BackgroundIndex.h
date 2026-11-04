#pragma once
#include <QObject>
#include <QString>
#include <atomic>
#include <functional>

// ── The template §2 background-index shape, once ────────────────────────────────────────────────
//
// Today this shape is hand-rolled eleven times family-wide: seven D4 singletons with a
// readyChanged signal, FOX's ArchiveIndex / TextureUsers / RefIndex, and POE2's IndexLoader
// (core-audit/plugin-build.md §1.1, tabs-store.md §3). Every copy re-implements the same six
// elements and the copies disagree — three of D4's older indexes have no generation counter
// although a MainWindow comment says they do, and POE2 mutates its store on the worker with no
// install() step at all.
//
// The six elements, all provided here:
//   1. a detached worker                          — start()
//   2. a fingerprint-signed disk cache            — loadCache()/saveCache() around fingerprint()
//   3. install() posted back via QueuedConnection — install() runs on the owner thread
//   4. a readyChanged signal the UI repopulates on
//   5. reset() wired to a fingerprint change      — setFingerprint()
//   6. a generation counter so an in-flight build discards itself
//
// FOX's ArchiveIndex earned element 6 the expensive way and says so in its own header: a rebuild
// REQUEST bumps its counter and clears ready immediately, but the file list is not replaced until
// install() runs much later, so a cache keyed on the request counter would refill during the gap
// with positions into the OLD list and then sail past its own check. Hence TWO counters here:
// m_generation (bumped on request) and installGeneration() (bumped on install), and derived caches
// key on the latter.
namespace core {

class BackgroundIndex : public QObject {
    Q_OBJECT
public:
    explicit BackgroundIndex(QObject* parent = nullptr) : QObject(parent) {}
    ~BackgroundIndex() override;

    bool    isReady() const    { return m_ready.load(); }
    bool    isBuilding() const { return m_building.load(); }
    quint64 installGeneration() const { return m_installGeneration.load(); }

    // The data fingerprint this index was built against; a change means the cached build is stale.
    QString fingerprint() const { return m_fingerprint; }
    void    setFingerprint(const QString& fp);   // differs from the current one ⇒ reset()

    // Kick a build. Returns false if one is already running for the same fingerprint.
    bool start();
    // Drop everything and become not-ready. Safe while a build is in flight: that build's
    // generation no longer matches, so it discards itself instead of installing stale data.
    void reset();

Q_SIGNALS:
    void readyChanged(bool ready);
    void progress(const QString& message);

protected:
    // Runs on the WORKER. Must not touch Qt widgets, must not emit. Return false to abandon.
    // `cancelled()` goes true when a newer generation supersedes this build — check it in any loop.
    virtual bool build(const std::function<void(const QString&)>& report,
                       const std::function<bool()>& cancelled) = 0;
    // Runs on the OWNER thread, via a queued call. Swap the built data in here — this is the only
    // point at which readers can observe a change.
    virtual void install() = 0;
    // Optional disk cache, signed with fingerprint(). Return false to force a rebuild.
    virtual bool loadCache() { return false; }
    virtual void saveCache() {}

    void report(const QString& message);   // worker-safe progress

private:
    void runBuild(quint64 generation);

    std::atomic<bool>    m_ready{false};
    std::atomic<bool>    m_building{false};
    std::atomic<quint64> m_generation{0};
    std::atomic<quint64> m_installGeneration{0};
    QString              m_fingerprint;
};

}  // namespace core
