#pragma once
// Portable, self-contained storage (template §1). Everything the tool writes — settings INI, caches,
// thumbnails, logs — lives in a "data" folder beside the executable: no registry, no %AppData%.
// Move the folder and it keeps working; delete it and nothing is left.
//
// Lifted from FOX (app/AppPaths.h), which is D4's/POE2's plus the data/cache split. The split is
// load-bearing: FOX's header records a user who cleared "the cache" and lost their configuration
// because the INI sat beside the cache files. Anything derived and rebuildable goes under cache/;
// anything the user authored stays in data/. POE2 had already moved its three caches there.
//
// pruneOldCaches takes the DIRECTORY: POE2's main.cpp pruned data/ while its caches lived in
// data/cache/ (AssetStore.cpp:19,38,55 vs main.cpp:50-52), so its prune never matched anything.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace core::paths {

inline QString dataDir()
{
    static const QString d = [] {
        QString base = QCoreApplication::applicationDirPath();
        if (base.isEmpty()) base = QDir::currentPath();
        const QString dir = QDir(base).filePath(QStringLiteral("data"));
        QDir().mkpath(dir);
        return dir;
    }();
    return d;
}

inline QString file(const QString& name) { return QDir(dataDir()).filePath(name); }

inline QString subDir(const QString& name)
{
    const QString d = QDir(dataDir()).filePath(name);
    QDir().mkpath(d);
    return d;
}

inline QString cacheDir()
{
    static const QString d = [] {
        const QString dir = QDir(dataDir()).filePath(QStringLiteral("cache"));
        QDir().mkpath(dir);
        return dir;
    }();
    return d;
}

inline QString cacheFile(const QString& name) { return QDir(cacheDir()).filePath(name); }

// Delete superseded versions of a versioned cache in `dir`: files matching <stem><digits><ext> with
// a LOWER number than currentVersion. Returns how many went.
inline int pruneOldCaches(const QString& dir, const QString& stem, int currentVersion, const QString& ext)
{
    const QDir d(dir);
    const QRegularExpression re(QStringLiteral("^%1(\\d+)%2$")
                                    .arg(QRegularExpression::escape(stem), QRegularExpression::escape(ext)));
    int gone = 0;
    for (const QString& fn : d.entryList(QDir::Files)) {
        const QRegularExpressionMatch m = re.match(fn);
        if (!m.hasMatch()) continue;
        const int v = m.captured(1).toInt();
        if (v > 0 && v < currentVersion && QFile::remove(d.filePath(fn))) ++gone;
    }
    return gone;
}

// Move caches an older build left in data/ into data/cache/ — one rename per file, never a
// rebuild (FOX measured 88.6 s to regenerate its deep-scan cache). `globs` names them.
inline int migrateCaches(const QStringList& globs)
{
    QDir data(dataDir());
    int moved = 0;
    for (const QString& name : data.entryList(globs, QDir::Files)) {
        const QString to = cacheFile(name);
        if (QFile::exists(to)) { QFile::remove(data.filePath(name)); continue; }
        if (QFile::rename(data.filePath(name), to)) ++moved;
    }
    return moved;
}

}  // namespace core::paths
