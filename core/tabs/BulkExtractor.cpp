#include "core/tabs/BulkExtractor.h"
#include "core/app/SehGuard.h"
#include "core/export/NameTemplate.h"
#include "core/tabs/ModelLoad.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QSaveFile>
#include <QThread>

#include <atomic>
#include <filesystem>
#include <system_error>
#include <thread>
#include <vector>

namespace core {

namespace {
// The manifest is a flat JSON object { "<asset key>": "<relative output path>" } in the run root.
// Only-new consults it AND checks the file still exists, so deleting an output re-exports it.
QString manifestPath(const QString& root) { return QDir(root).filePath(QStringLiteral("_bulk_manifest.json")); }
QString failedPath(const QString& root)   { return QDir(root).filePath(QStringLiteral("_bulk_failed.txt")); }

QJsonObject loadManifest(const QString& root)
{
    QFile f(manifestPath(root));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}
void saveManifest(const QString& root, const QJsonObject& m)
{
    QFile f(manifestPath(root));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(m).toJson(QJsonDocument::Indented));
}
// Create a folder (and its parents) for a worker. Workers create sibling folders concurrently, and
// on Windows a CreateDirectory racing another on a shared parent can fail (the 2026-09-23 run lost
// both raw originals under art/models/items even after one 5 ms retry). So creation is serialised
// in-process, retried with backoff for outside interference (a scanner holding a new folder), and
// when it still fails the OS's own reason is returned instead of a bare "could not create".
bool ensureDir(QMutex& m, const QString& dir, QString* why)
{
    QMutexLocker l(&m);
    std::error_code ec;
    for (int attempt = 0; attempt < 7; ++attempt) {
        if (QFileInfo(dir).isDir()) return true;
        ec.clear();
        std::filesystem::create_directories(std::filesystem::path(dir.toStdU16String()), ec);
        if (QFileInfo(dir).isDir()) return true;
        QThread::msleep(10u << attempt);   // 10 ms … 640 ms, ~1.3 s in all
    }
    if (why) *why = ec ? QStringLiteral("%1 (os error %2)").arg(QString::fromLocal8Bit(ec.message().c_str())).arg(ec.value())
                       : QStringLiteral("the folder does not exist after creating it");
    return false;
}
// The manifest key: the asset's path when it has one (survives a re-index), else scheme:id.
QString keyOf(const BulkItem& it)
{
    return it.item.sourcePath.isEmpty() ? QStringLiteral("%1:%2").arg(it.item.id.scheme).arg(it.item.id.value) : it.item.sourcePath;
}
}  // namespace

BulkExtractor::BulkExtractor(IGameStore* store, QVector<BulkItem> items, Options opt, QObject* parent)
    : QObject(parent), m_store(store), m_items(std::move(items)), m_opt(std::move(opt)) {}

bool BulkExtractor::exportModel(const BulkItem& it, const QString& outPath, QString* error)
{
    const model::ExportResult r = model::exportOne(*m_store, it.item.id, m_opt.glb, outPath);
    if (!r.ok && error) *error = r.error;
    return r.ok;
}

bool BulkExtractor::exportTexture(const BulkItem& it, const QString& outPath, QString* error)
{
    QString err;
    const QImage img = m_store->loadTexture(it.item.id, nullptr, &err);
    if (img.isNull()) { if (error) *error = err.isEmpty() ? QStringLiteral("decode returned no image") : err; return false; }
    if (!img.save(outPath, "PNG")) { if (error) *error = QStringLiteral("PNG write failed: %1").arg(outPath); return false; }
    return true;
}

// Raw mode: this item's EXACT original bytes (a model pulls its whole dependency set) under outDir,
// mirroring the game path. Each unique output path is written exactly ONCE across the run, and
// atomically, so two workers can never interleave the same file (POE2).
bool BulkExtractor::exportRaw(const BulkItem& it, QString* error)
{
    QStringList files = m_store->dependencyFiles(it.item.id);
    if (files.isEmpty() && !it.item.sourcePath.isEmpty()) files << it.item.sourcePath;
    if (files.isEmpty()) { if (error) *error = QStringLiteral("the store lists no original files for this asset"); return false; }
    int wrote = 0;
    // Every file's REAL reason ("could not be read" once hid a Windows write failure). An asset is
    // exported only when ALL its originals are there: a partial one used to count as exported with
    // its per-file errors dropped (audit 2026-09-25).
    QStringList errors;
    for (const QString& gp : files) {
        const QString outPath = model::mirrorPath(m_opt.outDir, gp);
        const QString key = QDir::cleanPath(outPath).toLower();
        {
            QMutexLocker cl(&m_rawMutex);
            if (m_rawClaimed.contains(key)) { ++wrote; continue; }
            m_rawClaimed.insert(key);
        }
        // Kept only on an "Only new" run; otherwise rewritten, so originals from an older game build
        // are refreshed (they were skipped whatever the setting).
        if (m_opt.onlyNew && QFileInfo::exists(outPath)) { ++wrote; continue; }
        QString readErr;
        const QByteArray bytes = m_store->readPath(gp, &readErr);
        if (bytes.isEmpty()) { errors << QStringLiteral("%1: read: %2").arg(gp, readErr.isEmpty() ? QStringLiteral("no bytes") : readErr); continue; }
        const QString dir = QFileInfo(outPath).absolutePath();
        QString why;
        if (!ensureDir(m_dirMutex, dir, &why)) { errors << QStringLiteral("%1: could not create %2 — %3").arg(gp, dir, why); continue; }
        QSaveFile f(outPath);
        if (!f.open(QIODevice::WriteOnly)) { errors << QStringLiteral("%1: open: %2").arg(gp, f.errorString()); continue; }
        if (f.write(bytes) != bytes.size()) { errors << QStringLiteral("%1: write: %2").arg(gp, f.errorString()); f.cancelWriting(); continue; }
        if (!f.commit()) { errors << QStringLiteral("%1: commit: %2").arg(gp, f.errorString()); continue; }
        ++wrote;
    }
    if (!errors.isEmpty()) {
        if (error) *error = QStringLiteral("%1 of %2 original file(s) written — %3").arg(wrote).arg(files.size()).arg(errors.join(QStringLiteral("; ")));
        return false;
    }
    return true;
}

void BulkExtractor::run()
{
    const int total = m_items.size();
    QDir().mkpath(m_opt.outDir);
    m_runCache = m_store->beginRunCache();

    QMutex agg;
    QJsonObject manifest = loadManifest(m_opt.outDir);
    // THIS run's failures only: appended across runs with no separator, it listed failures that
    // later runs had fixed (audit 2026-09-25). A failed item is not in the manifest, so the next run
    // retries it and lists it again if it still fails.
    QFile::remove(failedPath(m_opt.outDir));
    QFile failLog(failedPath(m_opt.outDir));
    bool failLogOpen = false;
    QSet<QString> claimedOut;
    auto norm = [](const QString& p) { return QDir::cleanPath(p).toLower(); };
    // Names an earlier run gave THIS run's assets stay theirs (audit 2026-09-25). Without this, an
    // "Only new" run skipped asset A without claiming its foo.glb, and asset B — which had been
    // disambiguated to foo_<id>.glb — took foo.glb and overwrote A's file; the manifest then named
    // foo.glb for both and A was skipped for good. Only files that still exist are reserved.
    QHash<QString, QString> reservedBy;   // normalised output path → the asset key that owns it
    if (!m_opt.rawOriginals)
        for (const BulkItem& it : m_items) {
            const QString rel = manifest.value(keyOf(it)).toString();
            if (rel.isEmpty()) continue;
            const QString abs = QDir(m_opt.outDir).filePath(rel);
            if (QFileInfo::exists(abs)) reservedBy.insert(norm(abs), keyOf(it));
        }
    int exported = 0, skipped = 0, failed = 0, doneCount = 0;
    QStringList pendingMsgs;
    std::atomic<int> lastIdx{-1};

    int nWorkers = m_opt.workers > 0 ? m_opt.workers : QThread::idealThreadCount();
    nWorkers = qBound(1, nWorkers, 32);
    if (!m_store->isThreadSafe()) nWorkers = 1;   // a store that cannot be entered concurrently is served serially
    if (total > 0) nWorkers = qMin(nWorkers, total);

    std::atomic<int> nextIdx{0};
    std::atomic<int> finishedWorkers{0};
    QElapsedTimer clock; clock.start();

    auto worker = [&]() {
        seh::installSehTranslator();
        for (;;) {
            if (m_cancel.loadRelaxed()) break;
            while (m_paused.loadRelaxed() && !m_cancel.loadRelaxed()) QThread::msleep(60);
            if (m_cancel.loadRelaxed()) break;
            const int i = nextIdx.fetch_add(1);
            if (i >= total) break;
            lastIdx.store(i, std::memory_order_relaxed);
            // .at(): QVector's non-const operator[] detaches, which races across workers.
            const BulkItem& it = m_items.at(i);
            const bool isModel = it.isModel, isTex = it.isTexture;
            if ((isModel && !m_opt.models) || (isTex && !m_opt.textures) || (!isModel && !isTex)) {
                QMutexLocker l(&agg); ++skipped; ++doneCount;
                continue;
            }
            const bool raw = m_opt.rawOriginals;
            QString destDir, rel, absOut;
            bool doSkip = false;
            if (raw) {
                rel = it.item.sourcePath.isEmpty() ? keyOf(it) : it.item.sourcePath;
                absOut = model::mirrorPath(m_opt.outDir, rel);
                QMutexLocker l(&agg);
                if (m_opt.onlyNew && manifest.contains(keyOf(it)) && QFileInfo::exists(absOut)) doSkip = true;
            } else {
                destDir = it.folder.isEmpty() ? m_opt.outDir : QDir(m_opt.outDir).filePath(it.folder);
                name::Fields f;
                f.fileName = QFileInfo(it.item.name).completeBaseName();
                f.id = qint64(it.item.id.value);
                const QString stem = name::apply(isModel ? m_opt.modelNameTemplate : m_opt.textureNameTemplate, f);
                const QString ext = isModel ? (m_opt.glb.gltfSeparate ? QStringLiteral(".gltf") : QStringLiteral(".glb")) : QStringLiteral(".png");
                QString cand = QDir(destDir).filePath(stem + ext);
                const QString key = keyOf(it);
                QMutexLocker l(&agg);
                // Taken = claimed in this run, or reserved for ANOTHER asset by an earlier one.
                auto taken = [&](const QString& c) {
                    const QString n = norm(c);
                    if (claimedOut.contains(n)) return true;
                    const auto r = reservedBy.constFind(n);
                    return r != reservedBy.constEnd() && r.value() != key;
                };
                if (taken(cand)) {
                    // A DIFFERENT asset has this name (two assets share a stem in one folder —
                    // routine in Flat). Disambiguate with the stable id, then a counter.
                    const QString h = QString::number(it.item.id.value, 16);
                    cand = QDir(destDir).filePath(stem + QStringLiteral("_") + h + ext);
                    for (int n = 1; taken(cand); ++n)
                        cand = QDir(destDir).filePath(QStringLiteral("%1_%2_%3%4").arg(stem, h).arg(n).arg(ext));
                }
                claimedOut.insert(norm(cand));   // skipped or written, the name is this asset's
                rel = QDir(m_opt.outDir).relativeFilePath(cand);
                absOut = cand;
                // Skip only when the name it resolves to NOW is the one it was written under.
                if (m_opt.onlyNew && manifest.value(key).toString() == rel && QFileInfo::exists(cand)) doSkip = true;
            }
            if (doSkip) { QMutexLocker l(&agg); ++skipped; ++doneCount; continue; }
            QString dirWhy;
            if (!raw && !ensureDir(m_dirMutex, destDir, &dirWhy)) {
                QMutexLocker l(&agg);
                ++failed; ++doneCount;
                if (!failLogOpen) failLogOpen = failLog.open(QIODevice::WriteOnly | QIODevice::Append);
                if (failLogOpen) failLog.write(QStringLiteral("%1\tcould not create %2 — %3\n").arg(keyOf(it), destDir, dirWhy).toUtf8());
                pendingMsgs << QStringLiteral("FAIL  %1  (mkpath)").arg(it.item.name);
                continue;
            }
            // Heavy work, WITHOUT the lock. One malformed asset reports to _bulk_failed.txt and the
            // run continues; without the guard an access violation in a parser took the run down.
            QString err;
            bool ok = false;
            seh::HardwareFault fault;
            const bool survived = seh::runGuarded("bulk-export", [&]() {
                ok = raw ? exportRaw(it, &err) : (isModel ? exportModel(it, absOut, &err) : exportTexture(it, absOut, &err));
            }, &fault);
            if (!survived) { ok = false; err = QStringLiteral("hardware fault: %1").arg(fault.what); }
            QMutexLocker l(&agg);
            if (ok) { ++exported; manifest.insert(keyOf(it), rel); }
            else {
                ++failed;
                if (!failLogOpen) failLogOpen = failLog.open(QIODevice::WriteOnly | QIODevice::Append);
                if (failLogOpen) failLog.write(QStringLiteral("%1\t%2\n").arg(keyOf(it), err.isEmpty() ? QStringLiteral("unknown error") : err).toUtf8());
                pendingMsgs << QStringLiteral("FAIL  %1  (%2)").arg(it.item.name, err);
            }
            ++doneCount;
        }
        finishedWorkers.fetch_add(1);
    };

    std::vector<std::thread> pool;
    pool.reserve(size_t(nWorkers));
    for (int w = 0; w < nWorkers; ++w) pool.emplace_back(worker);

    // Coordinator (this thread): pause accounting, throttled progress, periodic manifest saves, and
    // all signal emission — so signals always originate from the worker QThread.
    qint64 pausedAccum = 0, pauseStart = 0, lastSave = 0;
    bool wasPaused = false;
    for (;;) {
        QThread::msleep(80);
        const qint64 now = clock.elapsed();
        const bool p = m_paused.loadRelaxed();
        if (p && !wasPaused) { pauseStart = now; wasPaused = true; }
        else if (!p && wasPaused) { pausedAccum += now - pauseStart; wasPaused = false; }
        const bool doSave = (now - lastSave > 1000);
        int d, e, s, f; QStringList msgs; QByteArray mfBytes;
        {
            QMutexLocker l(&agg);
            d = doneCount; e = exported; s = skipped; f = failed;
            msgs.swap(pendingMsgs);
            if (doSave) mfBytes = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
        }
        for (const QString& m : msgs) emit message(m);
        const int li = lastIdx.load(std::memory_order_relaxed);
        const QString cur = (li >= 0 && li < total) ? QFileInfo(m_items.at(li).item.name).fileName() : QString();
        const qint64 activeMs = now - pausedAccum - (wasPaused ? now - pauseStart : 0);
        int eta = -1;
        if (d > 0 && d < total) eta = int((double(activeMs) / d) * (total - d) / 1000.0);
        emit progress(d, total, e, s, f, cur, eta);
        if (doSave) {
            QFile mf(manifestPath(m_opt.outDir));
            if (mf.open(QIODevice::WriteOnly | QIODevice::Truncate)) mf.write(mfBytes);
            lastSave = now;
        }
        if (finishedWorkers.load() >= nWorkers) break;
    }
    for (std::thread& t : pool) t.join();

    int e, s, f; QStringList msgs;
    {
        QMutexLocker l(&agg);
        e = exported; s = skipped; f = failed; msgs.swap(pendingMsgs);
        if (failLogOpen) failLog.close();
    }
    for (const QString& m : msgs) emit message(m);
    saveManifest(m_opt.outDir, manifest);
    m_runCache.reset();
    if (m_cancel.loadRelaxed()) emit message(QStringLiteral("Cancelled."));
    const QString summary = QStringLiteral("Done — %1 exported, %2 skipped, %3 failed → %4").arg(e).arg(s).arg(f).arg(QDir::toNativeSeparators(m_opt.outDir));
    emit message(summary);
    emit progress(e + s + f, total, e, s, f, QString(), 0);
    emit finished(e, s, f, summary);
}

}  // namespace core
