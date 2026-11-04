// Density.h (FOX, lifted) — one setting for how tightly every list and tree packs its rows
// (sweep item 45), applied application-wide.
//
// The Models tab had its own "Compact rows" checkbox and its own row-zoom, and
// nothing else did — so a person who wanted a denser Files list or a taller
// Textures grid had no way to ask, and the one control that existed spoke only
// for one tab. This is the one control, and it speaks for the whole
// application: a stylesheet fragment on qApp that sets the vertical padding of
// every `QAbstractItemView` item at once. A stylesheet because it reaches
// views built long after the setting is read (the outliner, a dependency
// dialog, the mod folder table) without each one subscribing to anything.
//
// Three steps, not a pixel field: nobody knows what "18 px" means, and the
// three names do. The value stored is the STABLE key ("compact"/"standard"/
// "comfortable"), never the index — an unknown value falls to standard.
#pragma once
#include <QApplication>
#include <QSettings>
#include <QString>
#include <QStringList>

namespace core {
namespace density {

inline const QStringList& keys()
{
    static const QStringList k{QStringLiteral("compact"),
                               QStringLiteral("standard"),
                               QStringLiteral("comfortable")};
    return k;
}

inline QString label(const QString& key)
{
    if (key == QLatin1String("compact")) return QStringLiteral("Compact");
    if (key == QLatin1String("comfortable")) return QStringLiteral("Comfortable");
    return QStringLiteral("Standard");
}

// The per-item vertical padding each step adds. Standard is 0 — the platform
// style's own metric, so "Standard" really is what the app shipped as.
inline int padding(const QString& key)
{
    if (key == QLatin1String("compact")) return 0;       // and a tight line, below
    if (key == QLatin1String("comfortable")) return 4;
    return 2;
}

inline void apply();   // defined below, used by setSession

// A session override, set by the --density harness flag, so a headless run can
// SEE a density without writing the user's persisted choice — exactly what
// --game and --moddir do for their settings. Empty = use the stored value.
inline QString& sessionOverride()
{
    static QString s;
    return s;
}

inline QString current()
{
    if (!sessionOverride().isEmpty()) return sessionOverride();
    const QString v = QSettings().value(QStringLiteral("ui/density"),
                                        QStringLiteral("standard")).toString();
    return keys().contains(v) ? v : QStringLiteral("standard");
}

inline void setSession(const QString& key)
{
    sessionOverride() = keys().contains(key) ? key : QStringLiteral("standard");
    apply();
}

// An app-provided theme stylesheet (a plugin's visual reskin) folded into the ONE app stylesheet so
// it survives every density re-apply. Empty by default — a game opts in by calling setExtraTheme().
inline QString& extraTheme()
{
    static QString s;
    return s;
}

// Build the stylesheet fragment for a density. Kept separate so the harness
// can assert on it without a running app.
inline QString styleSheet(const QString& key)
{
    // Compact deliberately goes below Standard's 2 px to a hair under the
    // platform default, which is what "as tight as this reads" means; the
    // other two add padding symmetrically top and bottom.
    const int pad = padding(key);
    return QStringLiteral(
               "QAbstractItemView::item { padding-top: %1px; padding-bottom: %1px; }")
               .arg(pad)
        + extraTheme();
}

// Read the setting and apply it to the whole application. Called at startup
// and whenever the setting changes; idempotent.
inline void apply()
{
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance()))
        app->setStyleSheet(styleSheet(current()));
}

inline void set(const QString& key)
{
    QSettings().setValue(QStringLiteral("ui/density"),
                         keys().contains(key) ? key : QStringLiteral("standard"));
    apply();
}

// Register a plugin's theme stylesheet and apply it now. Included in every later density re-apply.
inline void setExtraTheme(const QString& qss)
{
    extraTheme() = qss;
    apply();
}

}  // namespace density
}  // namespace core
