#pragma once
// Common base for every tab the shell hosts. D4's BrowserTab virtuals (the export hooks the top-level
// Export menu relabels itself from; refresh/reset/onSettingsChanged/persistView) minus the
// CascReader/SnoIndex setters — a tab talks to its game through IGameStore — plus FOX's
// populateExportMenu(QMenu*) with role stamps, so a tab can add engine-only export rows without the
// window growing a member per row (D4's m_actExportFrames* were the cost of not having it).
//
// Plain QWidget base with no Q_OBJECT here: derived tabs declare their own. The shell holds tabs
// uniformly and, for a lazy tab, through the LazyTab proxy in AppShell.cpp — EVERY virtual added
// here must be forwarded there in the same edit, or the feature is dead in the shipped app while
// looking correct in the tab's own source (D4's comment, verbatim in spirit).
#include "core/store/IGameStore.h"

#include <QMenu>
#include <QString>
#include <QWidget>

namespace core {

namespace gl { class GLModelWidget; }

class BrowserTab : public QWidget {
public:
    using QWidget::QWidget;

    void setStore(IGameStore* store) { m_store = store; }
    IGameStore* store() const { return m_store; }

    // The store opened (or re-opened). Repopulate.
    virtual void onIndexReady() {}
    // Secondary indexes (names, tags, links) arrived after the store opened. Re-filter/relabel.
    virtual void onSecondaryReady() {}
    virtual void refresh() {}                 // first shown, or storage loaded while current
    virtual void reset() {}                   // drop cached state so the next refresh() reloads
    virtual void onSettingsChanged() {}       // the Settings dialog closed with OK
    virtual void persistView() {}             // flush per-tab view state on app close
    // Non-empty while the tab is doing work that reads the store and cannot be interrupted safely
    // (a bulk export's workers hold references into the index). The shell will not reload then —
    // FOX refused a rescan during a bulk run (MainWindow.cpp:1653-1661) and the port did not.
    virtual QString busyReason() const { return {}; }

    // ── Export hooks (the top-level Export menu) ──────────────────────────────────────────
    virtual gl::GLModelWidget* previewWidget() { return nullptr; }
    virtual bool    hasExportSelection() const { return false; }
    virtual void    exportSelection() {}
    virtual void    exportSelectionToLast() {}
    virtual QString exportNoun() const { return QStringLiteral("selection"); }
    virtual bool    hasAnimExport() const { return false; }
    virtual QString animExportLabel() const { return QStringLiteral("Export animations only (.glb)…"); }
    virtual void    exportAnimations() {}
    // Engine-only rows, appended after the generic ones. Stamp roles with hotkeys::Role::set so a
    // hotkey can find them.
    virtual void    populateExportMenu(QMenu*) {}

protected:
    IGameStore* m_store = nullptr;
};

}  // namespace core
