#pragma once
// The application window (app-shell.md §2.5). It owns none of the game: the tab list, the store,
// the index roster, the help rows, the settings pages and the id policy all come through AppPlugin.
//
// What is here, and whose it was:
//   FOX — the first-run stack, StatusLine with source filtering, applyHotkeys() by registry KEY
//         (never by array position), triggerExportAction() by ROLE, focusCurrentSearch() by object
//         name, populateExportMenu delegation, restoreWindowLayout/closeEvent behind one key.
//   D4  — BrowserTab export hooks relabelling the Export menu on aboutToShow, LazyTab, the IndexSpec
//         roster → File ▸ Index submenu + status-bar indicator, the export toast + tray
//         notification, Help ▸ Copy diagnostic info, Ctrl+1..9.
//   POE2 — the "Show in folder" status-bar button fed by ExportNotifier.
// Fixed on the way in: POE2 bound its hotkeys once at construction ("apply after the next
// launch"); here Settings ▸ OK re-applies them. FOX persisted the current tab by TITLE; here it is
// the TabSpec::stableId.
#include "core/app/AppPlugin.h"
#include "core/app/BrowserTab.h"

#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QVector>

class QLabel;
class QMenu;
class QStackedWidget;
class QSystemTrayIcon;
class QTabWidget;
class QTimer;
class QToolButton;

namespace core {

class FirstRunPage;
class LogConsole;

class AppShell : public QMainWindow {
    Q_OBJECT
public:
    explicit AppShell(AppPlugin& plugin, QWidget* parent = nullptr);
    ~AppShell() override;

    // Build is complete after the constructor; start() shows the first-run page or reloads.
    void start();
    bool indexReady() const { return m_indexReady; }
public Q_SLOTS:
    void reload();
public:

    BrowserTab* currentTab() const;
    BrowserTab* tab(const QString& stableId) const;      // built or not (a LazyTab proxy counts)
    QStringList tabIds() const;
    bool showTab(const QString& stableId);   // false (and said) when no tab has that id or label
    void setStatus(const QString& text);
    void openSettings(const QString& tab = QString());
    void showConsole();
    QString diagnosticInfo() const;                      // the shell half + the plugin half

    // Harness: suppress every layout/session write (FOX m_harness) so a headless run leaves no trace.
    void setHarness(bool on) { m_harness = on; }
    // Harness: one fixed window size, so every run's shots compare pixel for pixel (C13, 2026-09-25:
    // the constructor had already restored the user's saved geometry, maximized included). Never saved.
    void setHarnessWindow(int w, int h);
    // Harness: the Export menu as the current tab builds it, one line per row — "<role> on|off
    // <text>" (role "-" when unstamped) — the same rows the hotkeys find by role.
    QStringList exportMenuReport();

Q_SIGNALS:
    void indexBecameReady();
    void secondaryBecameReady();
    // The game data could not be read (and no reload is queued behind it). The harness quits on it
    // with a failure rather than waiting forever for a ready that will not come (audit 2026-09-25).
    void indexFailed(const QString& status);

protected:
    void closeEvent(QCloseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void buildTabs();
    void buildMenus();
    void buildStatusBar();
    void applyHotkeys();
    void populateExportMenu();
    void triggerExportAction(const QString& role);
    void focusCurrentSearch();
    void onTabChanged(int index);
    void syncTabStatus();
    void showFirstRun(bool on);
    void onIndexDone(bool ok, const QString& status);
    void onSecondaryReady();
    // index roster
    void buildIndexMenu(QMenu* fileMenu);
    void refreshIndexMenu();
    void refreshIndexIndicator();
    // export confirmation
    void showExportToast(const QString& text, const QString& folder, const QString& file);
    void positionExportToast();
    void showShortcuts();
    void copyDiagnostics();

    AppPlugin& m_plugin;
    QStackedWidget* m_stack = nullptr;
    FirstRunPage* m_firstRun = nullptr;
    QTabWidget* m_tabs = nullptr;
    QVector<TabSpec> m_specs;
    QSet<QWidget*> m_refreshed;
    QMenu* m_exportMenu = nullptr;
    QMenu* m_indexMenu = nullptr;
    QVector<QAction*> m_indexActions;
    QAction* m_settingsAction = nullptr;
    QAction* m_reloadAction = nullptr;
    QAction* m_consoleAction = nullptr;
    QList<QAction*> m_hotkeyActions;
    QLabel* m_status = nullptr;
    QLabel* m_tabStatus = nullptr;
    QPointer<QWidget> m_tabStatusSource;
    QString m_tabStatusText;
    QLabel* m_idxStatus = nullptr;
    QToolButton* m_revealBtn = nullptr;
    QString m_lastExportFolder;
    QLabel* m_exportToast = nullptr;
    QTimer* m_exportToastTimer = nullptr;
    QSystemTrayIcon* m_tray = nullptr;
    QPointer<LogConsole> m_console;
    bool m_indexReady = false;
    bool m_reloading = false;
    bool m_reloadPending = false;   // a reload asked for during one: run when it is done
    bool m_reloadWaiting = false;   // a reload retrying until no tab is busy (logged once)
    bool m_harness = false;
};

}  // namespace core
