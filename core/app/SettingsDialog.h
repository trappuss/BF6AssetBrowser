#pragma once
// The settings dialog skeleton (template §16): tabs in the stated order — setup → presentation →
// per-area → what lands on disk → keys → upkeep → reference — every page in a scroll area, the tab
// bar never eliding. FOX's skeleton (addPage, showTab by NAME with a sub-tab, the hotkey-clash
// detector, live density/text-size with Cancel revert) plus D4's two: the first-show screen clamp
// (SettingsDialog.cpp:2802-2828 — the only implementation that sizes from the PAGES, because
// QScrollArea::sizeHint caps itself) and the Cancel snapshot for live-written keys.
//
// Core owns the generic pages (Interface, Export, Hotkeys, Maintenance, Information frame); the
// plugin contributes the rest through AppPlugin::settingsPages() and they interleave by `order`.
// Reset is BY REMOVAL (§3.10): "Restore export defaults" removes export/, image/, gif/ and every
// plugin page's resetGroups (minus its keepPrefixes), then every page re-reads.
#include "core/app/AppPlugin.h"

#include <QDialog>
#include <QHash>
#include <QVariant>
#include <QVector>

class QKeySequenceEdit;
class QLabel;
class QTabWidget;

namespace core {

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(AppPlugin& plugin, QWidget* parent = nullptr);
    ~SettingsDialog() override;

    void accept() override;
    void reject() override;
    // "Export", "Export/Images", "Hotkeys"… An unknown name opens on the first tab.
    void showTab(const QString& name);

    // The core pages, so a plugin can reuse one inside its own (e.g. put Export ▸ Models under a
    // game-specific Export tab). Each is a fresh SettingsPage bound to this plugin's defaults.
    static QVector<SettingsPage> corePages(AppPlugin& plugin);

Q_SIGNALS:
    void settingsChanged();     // OK pressed (or Cancel reverted a live change)

protected:
    void showEvent(QShowEvent* e) override;

private:
    void addPage(const QString& title, QWidget* page);
    void snapshotLive();
    void restoreDefaults();

    AppPlugin& m_plugin;
    QTabWidget* m_tabs = nullptr;
    QVector<SettingsPage> m_pages;
    QHash<QString, QVariant> m_snapshot;
    QStringList m_livePrefixes;
    bool m_sized = false;
};

}  // namespace core
