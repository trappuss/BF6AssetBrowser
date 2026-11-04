// FontScale.h (FOX, lifted) — one application-wide text size, the other half of the
// density control (core/app/Density.h).
//
// Density is item padding and says nothing about the type: a person on a
// 4K panel who finds 9 pt unreadable had one answer, the Models tab's own
// row-zoom, which reaches one list. This is the application's font, scaled
// as a percentage of the platform default the tool started with, and applied
// through QApplication::setFont so every widget — the ones built already and
// the ones a dialog builds later — follows it. Dialogs, panels, tables and
// the settings pages all resize with their text, which is what a single scale
// means; the 3D viewport's HUD draws with its own metrics and is untouched.
//
// Stored as the PERCENT (an integer, 90..150), never a combo index and never
// a point size: a point size is only right for the font the platform gave
// this machine, and an index reorders the moment a step is added. Unknown or
// out-of-range values fall to 100.
#pragma once
#include <QApplication>
#include <QFont>
#include <QSettings>
#include <QString>
#include <QVector>

namespace core {
namespace fontscale {

inline const QVector<int>& steps()
{
    static const QVector<int> s{90, 100, 110, 125, 150};
    return s;
}

inline QString label(int pct)
{
    return pct == 100 ? QStringLiteral("100% (default)")
                      : QStringLiteral("%1%").arg(pct);
}

// The font the platform handed the application before anything scaled it.
// Captured on the first call, which apply() makes from main() before the
// window exists — so it is the true default even after a live change.
inline const QFont& baseFont()
{
    static const QFont base = QApplication::font();
    return base;
}

inline int clampPct(int pct)
{
    return pct >= 90 && pct <= 150 ? pct : 100;
}

// A session override, set by the --fontscale harness flag, so a headless run
// can photograph a size without rewriting the user's stored choice — the same
// shape as density's and --game's.
inline int& sessionOverride()
{
    static int s = 0;
    return s;
}

inline int current()
{
    if (sessionOverride() > 0) return sessionOverride();
    return clampPct(QSettings().value(QStringLiteral("ui/fontScale"), 100).toInt());
}

inline void apply()
{
    auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!app) return;
    const QFont& base = baseFont();
    QFont f = base;
    const qreal pt = base.pointSizeF() > 0 ? base.pointSizeF()
                                           : qreal(base.pointSize() > 0 ? base.pointSize() : 9);
    f.setPointSizeF(pt * current() / 100.0);
    app->setFont(f);
}

inline void setSession(int pct)
{
    sessionOverride() = clampPct(pct);
    apply();
}

inline void set(int pct)
{
    QSettings().setValue(QStringLiteral("ui/fontScale"), clampPct(pct));
    apply();
}

}  // namespace fontscale
}  // namespace core
