#pragma once
// The table of caches the application leaves in data/cache, and the operations the Settings ▸
// Maintenance page performs on them (FOX app/CacheMaint.h). ONE table: FOX's page once carried its
// own list and its own delete loop, and the loop deleted from data/ while the size was measured in
// data/cache — "the button reported 'empty' over files it had never touched". Measuring and
// clearing go through the same functions on the same folder.
//
// The defs come from the plugin (AppPlugin::cacheDefs()); the operations are core's.
#include "core/app/AppPaths.h"
#include "core/app/AppPlugin.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QVector>

namespace core::cachemaint {

inline QVector<CacheDef>& registry() { static QVector<CacheDef> r; return r; }
inline void setDefs(const QVector<CacheDef>& d) { registry() = d; }
inline const QVector<CacheDef>& defs() { return registry(); }

inline QString dirOf(const CacheDef& c)
{
    return c.subdir.isEmpty() ? paths::cacheDir() : QDir(paths::cacheDir()).filePath(c.subdir);
}

inline qint64 bytes(const CacheDef& c)
{
    qint64 total = 0;
    QDir d(dirOf(c));
    if (!d.exists()) return 0;
    for (const QFileInfo& fi : d.entryInfoList({c.glob}, QDir::Files | QDir::NoDotAndDotDot)) total += fi.size();
    return total;
}

inline int count(const CacheDef& c)
{
    QDir d(dirOf(c));
    if (!d.exists()) return 0;
    return int(d.entryList({c.glob}, QDir::Files | QDir::NoDotAndDotDot).size());
}

inline QDateTime newest(const CacheDef& c)
{
    QDir d(dirOf(c));
    if (!d.exists()) return {};
    QDateTime latest;
    for (const QFileInfo& fi : d.entryInfoList({c.glob}, QDir::Files | QDir::NoDotAndDotDot))
        if (!latest.isValid() || fi.lastModified() > latest) latest = fi.lastModified();
    return latest;
}

inline qint64 totalBytes()
{
    qint64 t = 0;
    for (const CacheDef& c : defs()) t += bytes(c);
    return t;
}

// Delete what matches. Returns how many went; *kept is how many the OS refused (a file still held
// open — Windows will not delete it), which is the number the page shows rather than "empty".
inline int clear(const CacheDef& c, int* kept)
{
    QDir d(dirOf(c));
    int gone = 0, held = 0;
    for (const QString& f : d.entryList({c.glob}, QDir::Files | QDir::NoDotAndDotDot)) (d.remove(f) ? gone : held)++;
    if (kept) *kept = held;
    return gone;
}

inline QString report()
{
    QString out = QStringLiteral("Caches under data/cache\n\n");
    const QDateTime nowT = QDateTime::currentDateTime();
    for (const CacheDef& c : defs()) {
        const QDateTime nw = newest(c);
        out += QStringLiteral("%1\n    %2 byte(s), %3 file(s)%4\n").arg(c.label).arg(bytes(c)).arg(count(c))
                   .arg(nw.isValid() ? QStringLiteral(", newest %1 (%2 day(s) old)")
                                           .arg(nw.toString(QStringLiteral("yyyy-MM-dd HH:mm"))).arg(nw.daysTo(nowT))
                                     : QString());
    }
    out += QStringLiteral("\nTotal: %1 byte(s).\n").arg(totalBytes());
    return out;
}

inline const CacheDef* find(const QString& needle)
{
    for (const CacheDef& c : defs())
        if (c.label.contains(needle, Qt::CaseInsensitive)) return &c;
    return nullptr;
}

}  // namespace core::cachemaint
