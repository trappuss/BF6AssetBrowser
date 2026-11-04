#pragma once
// The bulk-extraction run (template §14 + §25) over IGameStore — POE2's BulkExtractor, "the cleanest
// worker/coordinator split" (tabs-store.md §5): a QObject on its own QThread whose run() spawns a
// std::thread pool over an atomic cursor and a coordinator loop that owns pause-time accounting,
// throttled progress, periodic manifest saves and every signal. Fail-closed:
//   • Only-new skips anything already written, tracked in _bulk_manifest.json in the run root.
//   • Failures go to _bulk_failed.txt with a reason each; one bad asset never takes the run down —
//     each item runs under seh::runGuarded (D4's per-item guard, which POE2 lacked).
//   • Cancel and Pause are thread-safe flags; paused time is excluded from the ETA.
// The layout groups are computed BEFORE the run on the GUI thread (D4), because a game's tag
// lookups are GUI-thread-only per the IGameStore contract; the worker only sees folders.
#include "core/export/ExportLayout.h"
#include "core/store/IGameStore.h"
#include "core/types/ExportOptions.h"

#include <QAtomicInt>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>
#include <memory>

namespace core {

struct BulkItem {
    layout::Item item;
    QString storeKind;          // the IGameStore kind the row came from
    QString folder;             // destination, relative to outDir ("" = the root), from the layout
    bool isModel = false;       // model → glTF · texture → PNG; neither → skipped (counted)
    bool isTexture = false;
};

class BulkExtractor : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString outDir;
        bool onlyNew = true;
        bool models = true;
        bool textures = true;
        bool rawOriginals = false;      // write the EXACT original files (dependencyFiles + readPath), mirroring game paths
        int  workers = 0;               // 0 = auto = idealThreadCount, clamped [1,32]
        ExportOptions glb;
        QString modelNameTemplate = QStringLiteral("{{FileName}}");
        QString textureNameTemplate = QStringLiteral("{{FileName}}");
    };

    BulkExtractor(IGameStore* store, QVector<BulkItem> items, Options opt, QObject* parent = nullptr);

    void requestCancel() { m_cancel.storeRelaxed(1); }
    void setPaused(bool p) { m_paused.storeRelaxed(p ? 1 : 0); }

public Q_SLOTS:
    void run();                         // invoke after moving to the worker thread

Q_SIGNALS:
    void progress(int done, int total, int exported, int skipped, int failed, const QString& current, int etaSeconds);
    void message(const QString& line);
    void finished(int exported, int skipped, int failed, const QString& summary);

private:
    bool exportModel(const BulkItem& it, const QString& outPath, QString* error);
    bool exportTexture(const BulkItem& it, const QString& outPath, QString* error);
    bool exportRaw(const BulkItem& it, QString* error);

    IGameStore* m_store;
    QVector<BulkItem> m_items;
    Options m_opt;
    QAtomicInt m_cancel{0};
    QAtomicInt m_paused{0};
    QMutex m_rawMutex;
    QMutex m_dirMutex;   // folder creation is serialised (ensureDir in the .cpp)
    QSet<QString> m_rawClaimed;
    std::unique_ptr<RunCache> m_runCache;
};

}  // namespace core
