// MenuDump.h (FOX, lifted) — every context menu in the tool, as text, without opening one.
//
// ── WHY THIS EXISTS ────────────────────────────────────────────────────────
// A menu is its own top-level window, so it is in no screenshot, and
// `QMenu::exec()` spins a nested event loop a headless run never leaves. So
// the only way a check could ever see a context menu was for the surface to
// grow a bespoke "…ForShot()" that BUILT THE MENU AGAIN, beside the real
// handler. ModelsTab has one. It is two implementations of one menu, which is
// the exact defect docs/CONTEXT_MENUS.md exists to prevent, and the drift was
// measurable: `--filemenu` only ever reached the Models tab, and `--selectrows`
// silently selected ONE row whenever `--model` had filtered the list — so the
// set form of the shared file menu had never been dumped by anything, and the
// Files, Textures, outliner, PARTS and MATERIALS menus had never been dumped
// at all.
//
// This inverts it. The real handler builds the real menu and then, instead of
// exec'ing it, hands it here. One line at each `exec()` site:
//
//     if (menudump::capture(&menu, QStringLiteral("files/tree"))) return;
//     menu.exec(view->mapToGlobal(pos));
//
// Off (the default) `capture` does nothing and returns false, so the cost in a
// normal run is one atomic read per right-click.
//
// The pattern is ViewCapture's `partMenuDumpOnly()`, generalised — that flag
// solved this for exactly one surface and every other menu stayed invisible.
#pragma once
#include <QString>
#include <QStringList>

class QMenu;

namespace core::menudump {

// While active, every capture() records and returns true.
bool active();
void setActive(bool on);

// Record this menu's shape under `surface` and tell the caller not to exec.
// Returns false — and does nothing at all — when the census is not running.
//
// The SHAPE is the label of every action in order, with the things a check
// has to be able to see: a separator as "---", a submenu's item count, a
// disabled action marked. Ampersands are stripped because a mnemonic is not
// part of the label (§1) and its position is a layout accident.
bool capture(QMenu* menu, const QString& surface);

// Everything captured since the last call, as "<surface>\t<label> | <label>…"
// rows, and clears the store. A surface that was asked for and produced no
// menu is recorded too, as an empty row — "nothing appeared" is a result, and
// it is the one this instrument was built to catch.
QStringList taken();

// Record that a surface was reached but produced NO menu at all (the handler
// returned before building one). Kept distinct from an empty menu.
void noteEmpty(const QString& surface);

}  // namespace core::menudump
