#pragma once
// The central, rebindable shortcut registry (template §11). FOX's shape — rows with hints, seq(key)
// by table lookup, cheat-sheet text/HTML from the rows AS BOUND, a Role stamp so a hotkey finds
// its action by role and never by label — with the ROW TABLE injected per game (a registry row
// for an action a game does not have "is a shortcut the editor shows as bound, the tooltip
// describes, and nothing on earth will fire" — FOX Hotkeys.h).
//
// The keys the SHELL binds are named here once, so a plugin's table and AppShell::applyHotkeys
// cannot spell one differently. Binding is by KEY, never by array position: D4's positional walk
// bound the wrong keys silently when a row moved (CONTEXT_MENUS §6).
#include <QAction>
#include <QKeySequence>
#include <QList>
#include <QMenu>
#include <QSettings>
#include <QString>
#include <QVector>

namespace core::hotkeys {

struct Def {
    QString key;    // QSettings key ("hotkeys/…")
    QString label;  // shown in the Settings editor
    QString def;    // default, portable text; empty = deliberately unbound
    QString hint;   // one line for the editor's tooltip
};

// The keys the shell itself acts on.
inline QString kExportSelection() { return QStringLiteral("hotkeys/exportSelection"); }
inline QString kExportToLast()    { return QStringLiteral("hotkeys/exportToLast"); }
inline QString kExportAnimations(){ return QStringLiteral("hotkeys/exportAnimations"); }
inline QString kSaveImage()       { return QStringLiteral("hotkeys/saveImage"); }
inline QString kTurntable()       { return QStringLiteral("hotkeys/turntable"); }
inline QString kAnimLoop()        { return QStringLiteral("hotkeys/animLoop"); }
inline QString kFocusSearch()     { return QStringLiteral("hotkeys/focusSearch"); }
inline QString kSettings()        { return QStringLiteral("hotkeys/settings"); }
inline QString kReload()          { return QStringLiteral("hotkeys/reload"); }
inline QString kToggleConsole()   { return QStringLiteral("hotkeys/toggleConsole"); }

// The rows every game gets by default (D4's bindings, which FOX also adopted "so a person who
// uses both tools does not learn two sets"). A plugin starts from this and appends its own, or
// drops a row whose action it cannot offer.
QVector<Def> coreDefs();

// The table in force. Set once by the plugin before the window is built.
void setDefs(const QVector<Def>& table);
const QVector<Def>& defs();
const Def* find(const QString& key);

// Resolve one shortcut from settings (falling back to its default). Empty = unbound; a caller
// must never bind an empty sequence, which Qt matches against every keystroke.
QKeySequence seq(const QString& key, const QString& def);
QKeySequence seq(const QString& key);   // default from the table

// ── The cheat sheet: every binding as it is RIGHT NOW, plus the fixed mouse rows ─────────────
struct SheetRow { QString what; QString keys; QString note; };
void setMouseRows(const QVector<SheetRow>& rows);   // the viewport's non-rebindable controls
QVector<SheetRow> sheetRows();
QString cheatSheetText(const QString& productName);
QString cheatSheetHtml(const QString& productName);

// ── How a hotkey finds the action it fires: by ROLE stamped on the QAction ──────────────────
namespace Role {
inline const char* kProperty = "abExportRole";
inline QAction* set(QAction* a, const QString& role) { if (a) a->setProperty(kProperty, role); return a; }
QAction* find(const QList<QAction*>& acts, const QString& role);   // recurses into submenus
inline QString exportSelection()     { return QStringLiteral("export.selection"); }
inline QString exportSelectionLast() { return QStringLiteral("export.selection.last"); }
inline QString exportAnimations()    { return QStringLiteral("export.animations"); }
inline QString saveImage()           { return QStringLiteral("capture.image"); }
inline QString turntable()           { return QStringLiteral("capture.turntable"); }
inline QString animGif()             { return QStringLiteral("capture.animgif"); }
}  // namespace Role

}  // namespace core::hotkeys
