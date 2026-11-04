#include "core/app/AppShell.h"
#include "core/app/AppLog.h"
#include "core/app/AppPaths.h"
#include "core/app/CoreSettings.h"
#include "core/app/CrashHandler.h"
#include "core/app/ExportNotifier.h"
#include "core/app/FirstRunPage.h"
#include "core/app/Hotkeys.h"
#include "core/app/LogConsole.h"
#include "core/app/SettingsDialog.h"
#include "core/app/StatusLine.h"
#include "core/gl/GLModelWidget.h"
#include "core/ui/MenuText.h"
#include "core/ui/TextReportDialog.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QSysInfo>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <functional>

namespace core {

// ── Lazy tab host (D4) ────────────────────────────────────────────────────────────────────────
// Constructing every tab up front made startup pay for ALL of them. This host IS a BrowserTab (so
// every static_cast<BrowserTab*> page walk stays valid) that builds its real tab on FIRST
// refresh() — when the tab first becomes visible, or the store loads while it is current. The
// factory takes the PARENT and constructs the tab as its child: a parentless heavy constructor
// flashed a phantom top-level window on Windows.
namespace {
class LazyTab : public BrowserTab {
public:
    LazyTab(std::function<BrowserTab*(QWidget*)> make, AppPlugin& plugin, QString stableId)
        : m_make(std::move(make)), m_plugin(plugin), m_id(std::move(stableId))
    {
        auto* l = new QVBoxLayout(this);
        l->setContentsMargins(0, 0, 0, 0);
    }
    BrowserTab* ensure()
    {
        if (!m_child) {
            m_child = m_make(this);
            m_child->setStore(m_store);
            m_plugin.decorateTab(m_id, m_child);
            layout()->addWidget(m_child);
            if (m_indexReady) m_child->onIndexReady();
            if (m_secondaryReady) m_child->onSecondaryReady();
        }
        return m_child;
    }
    BrowserTab* built() const { return m_child; }
    // Creation trigger: becoming visible (refresh). Everything else forwards only if built —
    // except the two readiness notes, which are remembered so a late build catches up.
    void refresh() override { ensure()->refresh(); }
    void reset() override { m_indexReady = m_secondaryReady = false; if (m_child) m_child->reset(); }
    void onIndexReady() override { m_indexReady = true; if (m_child) m_child->onIndexReady(); }
    void onSecondaryReady() override { m_secondaryReady = true; if (m_child) m_child->onSecondaryReady(); }
    void onSettingsChanged() override { if (m_child) m_child->onSettingsChanged(); }
    void persistView() override { if (m_child) m_child->persistView(); }
    QString busyReason() const override { return m_child ? m_child->busyReason() : QString(); }
    gl::GLModelWidget* previewWidget() override { return m_child ? m_child->previewWidget() : nullptr; }
    bool    hasExportSelection() const override { return m_child && m_child->hasExportSelection(); }
    void    exportSelection() override { if (m_child) m_child->exportSelection(); }
    void    exportSelectionToLast() override { if (m_child) m_child->exportSelectionToLast(); }
    QString exportNoun() const override { return m_child ? m_child->exportNoun() : BrowserTab::exportNoun(); }
    bool    hasAnimExport() const override { return m_child && m_child->hasAnimExport(); }
    QString animExportLabel() const override { return m_child ? m_child->animExportLabel() : BrowserTab::animExportLabel(); }
    void    exportAnimations() override { if (m_child) m_child->exportAnimations(); }
    void    populateExportMenu(QMenu* m) override { if (m_child) m_child->populateExportMenu(m); }
private:
    std::function<BrowserTab*(QWidget*)> m_make;
    AppPlugin& m_plugin;
    QString m_id;
    BrowserTab* m_child = nullptr;
    bool m_indexReady = false, m_secondaryReady = false;
};

const char* kTabIdProp = "abTabId";
}  // namespace

AppShell::AppShell(AppPlugin& plugin, QWidget* parent) : QMainWindow(parent), m_plugin(plugin)
{
    setWindowTitle(m_plugin.productName());
    resize(1280, 800);
    MenuText::setNouns(m_plugin.nouns());
    hotkeys::setDefs(m_plugin.hotkeyDefs());
    m_plugin.setShellReload([this] { QTimer::singleShot(0, this, &AppShell::reload); });

    m_stack = new QStackedWidget(this);
    QWidget* first = m_plugin.firstRunPage(m_stack);
    if (first) {
        // A plugin page says "go" through a startRequested() signal, if it has one (FOX's does).
        if (first->metaObject()->indexOfSignal("startRequested()") >= 0)
            connect(first, SIGNAL(startRequested()), this, SLOT(reload()));
    }
    if (!first) {
        m_firstRun = new FirstRunPage(m_plugin, m_stack);
        connect(m_firstRun, &FirstRunPage::startRequested, this, &AppShell::reload);
        first = m_firstRun;
    }
    m_stack->addWidget(first);
    buildTabs();
    m_stack->addWidget(m_tabs);
    setCentralWidget(m_stack);

    buildStatusBar();
    buildMenus();
    applyHotkeys();

    connect(&StatusLine::instance(), &StatusLine::reported, this, [this](QWidget* source, const QString& text) {
        m_tabStatusSource = source;
        m_tabStatusText = text;
        syncTabStatus();
    });
    connect(&ExportNotifier::instance(), &ExportNotifier::exported, this, &AppShell::showExportToast);

    // Window layout: behind the same key as the panels and splitters (FOX).
    if (!m_harness && settings::rememberLayout()) {
        const QByteArray geo = QSettings().value(QStringLiteral("window/geometry")).toByteArray();
        if (!geo.isEmpty()) restoreGeometry(geo);
    }
}

void AppShell::setHarnessWindow(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    setWindowState(windowState() & ~(Qt::WindowMaximized | Qt::WindowFullScreen | Qt::WindowMinimized));
    resize(w, h);
    qInfo("harness: window %dx%d (fixed, so shots compare across runs; --window WxH)", w, h);
}

AppShell::~AppShell()
{
    m_plugin.setShellReload({});   // the plugin outlives the shell: its callback must not reach a freed one
}

// ── Tabs ──────────────────────────────────────────────────────────────────────────────────────
void AppShell::buildTabs()
{
    m_tabs = new QTabWidget(m_stack);
    m_tabs->setDocumentMode(true);
    m_specs = m_plugin.tabs();
    for (const TabSpec& spec : m_specs) {
        BrowserTab* w = nullptr;
        if (spec.lazy) {
            w = new LazyTab(spec.make, m_plugin, spec.stableId);
        } else {
            w = spec.make(m_tabs);
            m_plugin.decorateTab(spec.stableId, w);
        }
        w->setStore(m_plugin.store());
        w->setProperty(kTabIdProp, spec.stableId);
        m_tabs->addTab(w, spec.title);
    }
    connect(m_tabs, &QTabWidget::currentChanged, this, &AppShell::onTabChanged);
    // The remembered tab, by STABLE ID.
    if (settings::rememberSession()) {
        const QString last = settings::lastTab();
        for (int i = 0; i < m_tabs->count(); ++i)
            if (m_tabs->widget(i)->property(kTabIdProp).toString() == last) { m_tabs->setCurrentIndex(i); break; }
    }
    qInfo("shell: %d tab(s) built — %s", m_tabs->count(), qUtf8Printable(tabIds().join(QStringLiteral(", "))));
}

BrowserTab* AppShell::currentTab() const
{
    return m_tabs ? static_cast<BrowserTab*>(m_tabs->currentWidget()) : nullptr;
}

BrowserTab* AppShell::tab(const QString& stableId) const
{
    for (int i = 0; m_tabs && i < m_tabs->count(); ++i)
        if (m_tabs->widget(i)->property(kTabIdProp).toString() == stableId) {
            auto* w = static_cast<BrowserTab*>(m_tabs->widget(i));
            if (auto* lazy = dynamic_cast<LazyTab*>(w)) return lazy->ensure();
            return w;
        }
    return nullptr;
}

QStringList AppShell::tabIds() const
{
    QStringList out;
    for (int i = 0; m_tabs && i < m_tabs->count(); ++i) out << m_tabs->widget(i)->property(kTabIdProp).toString();
    return out;
}

bool AppShell::showTab(const QString& stableId)
{
    int at = -1;
    for (int i = 0; m_tabs && i < m_tabs->count() && at < 0; ++i)
        if (m_tabs->widget(i)->property(kTabIdProp).toString() == stableId) at = i;
    // FOX's harness names tabs by their LABEL (--tab Customize); accept that too, case-insensitively.
    for (int i = 0; m_tabs && i < m_tabs->count() && at < 0; ++i)
        if (m_tabs->tabText(i).compare(stableId, Qt::CaseInsensitive) == 0) at = i;
    if (at < 0) { qWarning("shell: no tab named %s", qUtf8Printable(stableId)); return false; }
    m_tabs->setCurrentIndex(at);
    // The line a check holds to: the tab exists and was brought up (a lazy one is built by now).
    qInfo("shell: shown %s", qUtf8Printable(m_tabs->widget(at)->property(kTabIdProp).toString()));
    return true;
}

void AppShell::onTabChanged(int index)
{
    if (index < 0) return;
    QWidget* w = m_tabs->widget(index);
    if (!m_refreshed.contains(w)) {
        m_refreshed.insert(w);
        static_cast<BrowserTab*>(w)->refresh();
    }
    if (!m_harness && settings::rememberSession()) settings::setLastTab(w->property(kTabIdProp).toString());
    syncTabStatus();
}

// ── Menus ─────────────────────────────────────────────────────────────────────────────────────
void AppShell::buildMenus()
{
    auto* file = menuBar()->addMenu(QStringLiteral("&File"));
    file->addAction(QStringLiteral("Set game folder…"), this, [this] {
        if (m_plugin.configureFolder(this)) { if (m_firstRun) m_firstRun->refresh(); reload(); }
    });
    m_reloadAction = file->addAction(QStringLiteral("&Reload game data"), this, &AppShell::reload);
    if (!m_plugin.indexRoster().isEmpty()) buildIndexMenu(file);
    file->addSeparator();
    m_settingsAction = file->addAction(QStringLiteral("&Settings…"), this, [this] { openSettings(); });
    m_consoleAction = file->addAction(QStringLiteral("Log &console"), this, &AppShell::showConsole);
    file->addSeparator();
    file->addAction(QStringLiteral("E&xit"), qApp, &QApplication::quit);

    // The one place every export is done. Rebuilt on aboutToShow from the current tab's hooks, so
    // the labels name what will happen ("Export 12 models…") and disabled rows say why.
    m_exportMenu = menuBar()->addMenu(QStringLiteral("&Export"));
    connect(m_exportMenu, &QMenu::aboutToShow, this, &AppShell::populateExportMenu);
    populateExportMenu();

    auto* help = menuBar()->addMenu(QStringLiteral("&Help"));
    for (const HelpEntry& h : m_plugin.helpEntries()) {
        QAction* a = help->addAction(h.label, this, [fn = h.run] { if (fn) fn(); });
        a->setToolTip(h.tooltip);
    }
    if (!m_plugin.helpEntries().isEmpty()) help->addSeparator();
    help->addAction(QStringLiteral("Keyboard && mouse"), this, &AppShell::showShortcuts)->setShortcut(QKeySequence(Qt::Key_F1));
    help->addSeparator();
    help->addAction(QStringLiteral("Copy log"), this, [] { QApplication::clipboard()->setText(log::tail().join(QLatin1Char('\n'))); });
    help->addAction(QStringLiteral("Export log…"), this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export log"),
            QDir::homePath() + QStringLiteral("/%1.log").arg(m_plugin.productName()), QStringLiteral("Log (*.log *.txt)"));
        if (path.isEmpty()) return;
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) f.write(log::tail().join(QLatin1Char('\n')).toUtf8());
    });
    help->addAction(QStringLiteral("Copy diagnostic info"), this, &AppShell::copyDiagnostics);
    help->addAction(QStringLiteral("Open data folder"), this, [] { QDesktopServices::openUrl(QUrl::fromLocalFile(paths::dataDir())); });
    help->addSeparator();
    help->addAction(QStringLiteral("About"), this, [this] {
        QMessageBox::about(this, QStringLiteral("About %1").arg(m_plugin.productName()),
            QStringLiteral("<b>%1</b> %2<br>%3<br><br>Built on the AssetBrowser core.<br>Data folder: %4")
                .arg(m_plugin.productName().toHtmlEscaped(), m_plugin.version().toHtmlEscaped(),
                     m_plugin.store() ? m_plugin.store()->gameName().toHtmlEscaped() : QString(),
                     QDir::toNativeSeparators(paths::dataDir()).toHtmlEscaped()));
    });

    // Ctrl+1..9 (D4).
    for (int i = 0; i < m_tabs->count() && i < 9; ++i) {
        auto* a = new QAction(this);
        a->setShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(i + 1)));
        connect(a, &QAction::triggered, this, [this, i] { m_tabs->setCurrentIndex(i); });
        addAction(a);
    }
}

void AppShell::populateExportMenu()
{
    if (!m_exportMenu) return;
    m_exportMenu->clear();
    qDeleteAll(m_exportMenu->findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly));
    if (!m_indexReady) {
        m_exportMenu->addAction(QStringLiteral("(game data not loaded)"))->setEnabled(false);
        m_exportMenu->addSeparator();
        m_exportMenu->addAction(QStringLiteral("Export &settings…"), this, [this] { openSettings(QStringLiteral("Export")); });
        return;
    }
    BrowserTab* cur = currentTab();
    const QString noun = cur ? cur->exportNoun() : QStringLiteral("selection");
    const bool has = cur && cur->hasExportSelection();

    QAction* sel = m_exportMenu->addAction(MenuText::prompts(QStringLiteral("Export %1").arg(noun)), this, [cur] { if (cur) cur->exportSelection(); });
    sel->setEnabled(has);
    sel->setShortcut(hotkeys::seq(hotkeys::kExportSelection()));
    hotkeys::Role::set(sel, hotkeys::Role::exportSelection());
    const QString lastDir = settings::lastExportDir();
    QAction* last = m_exportMenu->addAction(MenuText::withValue(QStringLiteral("Export %1 to last folder").arg(noun), MenuText::condensePath(lastDir)),
                                            this, [cur] { if (cur) cur->exportSelectionToLast(); });
    last->setEnabled(has && !lastDir.isEmpty() && QDir(lastDir).exists());
    if (lastDir.isEmpty()) last->setToolTip(QStringLiteral("No export folder remembered yet — export once the long way."));
    last->setShortcut(hotkeys::seq(hotkeys::kExportToLast()));
    hotkeys::Role::set(last, hotkeys::Role::exportSelectionLast());
    if (cur && cur->hasAnimExport()) {
        QAction* anim = m_exportMenu->addAction(cur->animExportLabel(), this, [cur] { cur->exportAnimations(); });
        anim->setShortcut(hotkeys::seq(hotkeys::kExportAnimations()));
        hotkeys::Role::set(anim, hotkeys::Role::exportAnimations());
    }
    // The tab's own rows (captures, parts, raw originals…), stamped with roles by the tab.
    if (cur) {
        const int before = m_exportMenu->actions().size();
        cur->populateExportMenu(m_exportMenu);
        if (m_exportMenu->actions().size() > before) {
            // The capture rows carry the registry's keys so the menu and Settings ▸ Hotkeys agree.
            if (QAction* a = hotkeys::Role::find(m_exportMenu->actions(), hotkeys::Role::saveImage())) a->setShortcut(hotkeys::seq(hotkeys::kSaveImage()));
            if (QAction* a = hotkeys::Role::find(m_exportMenu->actions(), hotkeys::Role::turntable())) a->setShortcut(hotkeys::seq(hotkeys::kTurntable()));
            if (QAction* a = hotkeys::Role::find(m_exportMenu->actions(), hotkeys::Role::animGif()))   a->setShortcut(hotkeys::seq(hotkeys::kAnimLoop()));
        }
    }
    m_exportMenu->setToolTipsVisible(true);
    // LAST: the way out of this menu into the dialog belongs at the bottom of it (FOX §6).
    m_exportMenu->addSeparator();
    m_exportMenu->addAction(QStringLiteral("Export &settings…"), this, [this] { openSettings(QStringLiteral("Export")); });
}

QStringList AppShell::exportMenuReport()
{
    populateExportMenu();
    QStringList out;
    std::function<void(const QList<QAction*>&, const QString&)> walk = [&](const QList<QAction*>& acts, const QString& indent) {
        for (QAction* a : acts) {
            if (a->isSeparator()) continue;
            const QString role = a->property(hotkeys::Role::kProperty).toString();
            out << QStringLiteral("%1%2 %3 %4").arg(indent, role.isEmpty() ? QStringLiteral("-") : role,
                                                   a->isEnabled() ? QStringLiteral("on") : QStringLiteral("off"),
                                                   a->text().remove(QLatin1Char('&')));
            if (a->menu()) walk(a->menu()->actions(), indent + QStringLiteral("  "));
        }
    };
    if (m_exportMenu) walk(m_exportMenu->actions(), QString());
    return out;
}

// Fire one of the Export menu's entries without opening it — the SAME code path the mouse takes,
// so a hotkey can never do something the menu cannot. By ROLE, never by label (FOX).
void AppShell::triggerExportAction(const QString& role)
{
    populateExportMenu();
    QAction* hit = hotkeys::Role::find(m_exportMenu->actions(), role);
    if (!hit) { statusBar()->showMessage(QStringLiteral("Nothing on this tab to do that with."), 4000); return; }
    if (!hit->isEnabled()) {
        statusBar()->showMessage(QStringLiteral("%1 — not available right now.").arg(hit->text().remove(QLatin1Char('&'))), 4000);
        return;
    }
    hit->trigger();
}

void AppShell::applyHotkeys()
{
    qDeleteAll(m_hotkeyActions);
    m_hotkeyActions.clear();
    QStringList unbound;
    const auto add = [this, &unbound](const QString& key, auto&& fn) {
        const hotkeys::Def* d = hotkeys::find(key);
        if (!d) return;   // the plugin dropped this row — no action, honestly
        const QKeySequence seq = hotkeys::seq(d->key, d->def);
        if (seq.isEmpty()) return;   // deliberately unbound
        auto* a = new QAction(d->label, this);
        a->setShortcut(seq);
        a->setShortcutContext(Qt::ApplicationShortcut);
        connect(a, &QAction::triggered, this, fn);
        addAction(a);
        m_hotkeyActions.append(a);
    };
    // Rows the MENU carries the key for (one binding, and the menu shows it).
    if (m_settingsAction) m_settingsAction->setShortcut(hotkeys::seq(hotkeys::kSettings()));
    if (m_reloadAction)   m_reloadAction->setShortcut(hotkeys::seq(hotkeys::kReload()));
    if (m_consoleAction)  m_consoleAction->setShortcut(hotkeys::seq(hotkeys::kToggleConsole()));
    add(hotkeys::kFocusSearch(),      [this] { focusCurrentSearch(); });
    add(hotkeys::kExportSelection(),  [this] { triggerExportAction(hotkeys::Role::exportSelection()); });
    add(hotkeys::kExportToLast(),     [this] {
        const QString dir = settings::lastExportDir();
        if (dir.isEmpty() || !QDir(dir).exists()) {
            statusBar()->showMessage(QStringLiteral("No export folder remembered yet — export once with the prompting action and this key will reuse its folder."), 5000);
            return;
        }
        triggerExportAction(hotkeys::Role::exportSelectionLast());
    });
    add(hotkeys::kExportAnimations(), [this] { triggerExportAction(hotkeys::Role::exportAnimations()); });
    add(hotkeys::kSaveImage(),        [this] { triggerExportAction(hotkeys::Role::saveImage()); });
    add(hotkeys::kTurntable(),        [this] { triggerExportAction(hotkeys::Role::turntable()); });
    add(hotkeys::kAnimLoop(),         [this] { triggerExportAction(hotkeys::Role::animGif()); });
    // A registry row the shell has no binding for is a key the editor shows and nothing fires —
    // said in the log so a plugin author sees it (FOX: "a missing row is honest; a dead key is not").
    const QStringList known = {hotkeys::kExportSelection(), hotkeys::kExportToLast(), hotkeys::kExportAnimations(),
                               hotkeys::kSaveImage(), hotkeys::kTurntable(), hotkeys::kAnimLoop(), hotkeys::kFocusSearch(),
                               hotkeys::kSettings(), hotkeys::kReload(), hotkeys::kToggleConsole()};
    for (const hotkeys::Def& d : hotkeys::defs())
        if (!known.contains(d.key) && !d.key.startsWith(QStringLiteral("hotkeys/view")) && !d.key.startsWith(QStringLiteral("hotkeys/anim")))
            unbound << d.key;
    if (!unbound.isEmpty())
        qInfo("hotkeys: %d registry row(s) the shell does not bind (the plugin's tabs must): %s",
              int(unbound.size()), qUtf8Printable(unbound.join(QStringLiteral(", "))));
}

// Ctrl+F: the search box of whatever tab is in front, found by the object name searchbox::attach sets.
void AppShell::focusCurrentSearch()
{
    QWidget* cur = m_tabs ? m_tabs->currentWidget() : nullptr;
    if (!cur) return;
    for (QLineEdit* e : cur->findChildren<QLineEdit*>()) {
        if (e->objectName() != QLatin1String("abSearchBox") || !e->isVisible()) continue;
        e->setFocus(Qt::ShortcutFocusReason);
        e->selectAll();
        return;
    }
}

// ── Status bar ────────────────────────────────────────────────────────────────────────────────
void AppShell::buildStatusBar()
{
    m_status = new QLabel(this);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // never widens the window
    statusBar()->addWidget(m_status, 1);
    m_tabStatus = new QLabel(this);
    m_tabStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusBar()->addWidget(m_tabStatus, 1);

    // ONE consistent export-completion notice (template §15): summary + "Show in folder" (POE2).
    m_revealBtn = new QToolButton(this);
    m_revealBtn->setText(QStringLiteral("Show in folder"));
    m_revealBtn->setAutoRaise(true);
    m_revealBtn->setCursor(Qt::PointingHandCursor);
    m_revealBtn->hide();
    statusBar()->addPermanentWidget(m_revealBtn);
    connect(m_revealBtn, &QToolButton::clicked, this, [this] {
        if (!m_lastExportFolder.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(m_lastExportFolder));
    });

    if (!m_plugin.indexRoster().isEmpty()) {
        m_idxStatus = new QLabel(this);
        m_idxStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
        statusBar()->addPermanentWidget(m_idxStatus);
        auto* t = new QTimer(this);
        t->setInterval(700);
        connect(t, &QTimer::timeout, this, &AppShell::refreshIndexIndicator);
        t->start();
    }
}

void AppShell::setStatus(const QString& text) { if (m_status) m_status->setText(text); }

void AppShell::syncTabStatus()
{
    if (!m_tabStatus) return;
    QWidget* cur = m_tabs ? m_tabs->currentWidget() : nullptr;
    bool mine = m_tabStatusSource.isNull();   // null source = from nowhere in particular
    for (QWidget* w = m_tabStatusSource; w && !mine; w = w->parentWidget())
        if (w == cur) mine = true;
    m_tabStatus->setText(mine ? m_tabStatusText : QString());
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────────────────────
void AppShell::start()
{
    if (m_plugin.isConfigured()) reload();
    else showFirstRun(true);
}

void AppShell::showFirstRun(bool on)
{
    if (!m_stack) return;
    m_stack->setCurrentIndex(on ? 0 : 1);
    if (on && m_firstRun) m_firstRun->refresh();
    if (on) setStatus(QStringLiteral("Set your game folder to begin."));
}

void AppShell::reload()
{
    // A reload asked for while one runs (a folder changed in Settings during the first open) is
    // QUEUED and runs when this one is done — it used to be dropped, so the index finished with
    // the old folders. D4 and FOX queued or restarted it; none of them ignored it.
    if (m_reloading) {
        m_reloadPending = true;
        setStatus(QStringLiteral("Reload queued — it runs when the current one finishes."));
        qInfo("shell: reload queued (one is running)");
        return;
    }
    for (int i = 0; i < m_tabs->count(); ++i) {
        const QString why = static_cast<BrowserTab*>(m_tabs->widget(i))->busyReason();
        if (!why.isEmpty()) {
            // Kept, not dropped (audit 2026-09-25): a folder changed in Settings during a bulk run
            // used to leave the old game on screen until a manual F5. It runs when the tab is free.
            setStatus(QStringLiteral("Reload waits while %1 — it runs when that finishes (or cancel it).").arg(why));
            if (!m_reloadWaiting) qInfo("shell: reload waits — %s", qUtf8Printable(why));
            m_reloadWaiting = true;
            QTimer::singleShot(1000, this, &AppShell::reload);
            return;
        }
    }
    m_reloadWaiting = false;
    if (!m_plugin.isConfigured()) { showFirstRun(true); return; }
    m_reloading = true;
    m_indexReady = false;
    for (int i = 0; i < m_tabs->count(); ++i) static_cast<BrowserTab*>(m_tabs->widget(i))->reset();
    showFirstRun(false);
    setStatus(QStringLiteral("Reading the game data…"));
    qInfo("shell: reload started");
    m_plugin.reload([this](bool ok, const QString& status) { onIndexDone(ok, status); },
                    [this] { onSecondaryReady(); });
}

void AppShell::onIndexDone(bool ok, const QString& status)
{
    m_reloading = false;
    setStatus(status);
    if (m_reloadPending) {
        // The index just read is for the OLD settings: go straight to the queued reload rather
        // than hand the tabs an index that is about to be replaced.
        m_reloadPending = false;
        qInfo("shell: running the queued reload");
        QTimer::singleShot(0, this, &AppShell::reload);
        return;
    }
    if (!ok) {
        qWarning("shell: reload failed — %s", qUtf8Printable(status));
        emit indexFailed(status);
        return;
    }
    m_indexReady = true;
    for (int i = 0; i < m_tabs->count(); ++i) static_cast<BrowserTab*>(m_tabs->widget(i))->onIndexReady();
    if (QWidget* cur = m_tabs->currentWidget()) if (!m_refreshed.contains(cur)) { m_refreshed.insert(cur); static_cast<BrowserTab*>(cur)->refresh(); }
    refreshIndexMenu();
    refreshIndexIndicator();
    qInfo("shell: index ready — %s", qUtf8Printable(status));
    emit indexBecameReady();
}

void AppShell::onSecondaryReady()
{
    for (int i = 0; i < m_tabs->count(); ++i) static_cast<BrowserTab*>(m_tabs->widget(i))->onSecondaryReady();
    refreshIndexMenu();
    refreshIndexIndicator();
    qInfo("shell: secondary indexes ready");
    emit secondaryBecameReady();
}

void AppShell::openSettings(const QString& tab)
{
    SettingsDialog dlg(m_plugin, this);
    if (!tab.isEmpty()) dlg.showTab(tab);
    connect(&dlg, &SettingsDialog::settingsChanged, this, [this] {
        for (int i = 0; i < m_tabs->count(); ++i) static_cast<BrowserTab*>(m_tabs->widget(i))->onSettingsChanged();
        applyHotkeys();   // rebindings take effect without a restart
        populateExportMenu();
    });
    dlg.exec();
    if (m_plugin.takeReloadRequest()) reload();
}

void AppShell::showConsole()
{
    if (!m_console) m_console = new LogConsole(this);
    m_console->showConsole();
}

void AppShell::closeEvent(QCloseEvent* e)
{
    for (int i = 0; m_tabs && i < m_tabs->count(); ++i) static_cast<BrowserTab*>(m_tabs->widget(i))->persistView();
    if (!m_harness && settings::rememberLayout()) QSettings().setValue(QStringLiteral("window/geometry"), saveGeometry());
    QMainWindow::closeEvent(e);
}

void AppShell::resizeEvent(QResizeEvent* e)
{
    QMainWindow::resizeEvent(e);
    positionExportToast();
}

// ── Index roster (D4) ─────────────────────────────────────────────────────────────────────────
void AppShell::buildIndexMenu(QMenu* fileMenu)
{
    m_indexMenu = fileMenu->addMenu(QStringLiteral("&Index"));
    m_indexMenu->setToolTipsVisible(true);
    m_indexMenu->addAction(QStringLiteral("Index &all"), this, [this] {
        if (!m_indexReady) { setStatus(QStringLiteral("Index all: game data isn't open yet — try again once it is.")); return; }
        for (const IndexSpec& d : m_plugin.indexRoster()) {
            if ((d.ready && d.ready()) || (d.building && d.building())) continue;
            if (d.start) d.start();
        }
        refreshIndexMenu(); refreshIndexIndicator();
    })->setToolTip(QStringLiteral("Start every index that is not already built or running."));
    m_indexMenu->addAction(QStringLiteral("&Re-index everything…"), this, [this] {
        if (!m_indexReady) { setStatus(QStringLiteral("Re-index: game data isn't open yet.")); return; }
        if (QMessageBox::question(this, QStringLiteral("Re-index everything"),
                QStringLiteral("Drop every index cache and rebuild all of them from scratch?\n\nUse \"Index all\" instead to only finish what has not been built."),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        for (const IndexSpec& d : m_plugin.indexRoster()) { if (d.reset) d.reset(); if (d.start) d.start(); }
        refreshIndexMenu(); refreshIndexIndicator();
    })->setToolTip(QStringLiteral("Drop every cache and rebuild from scratch."));
    m_indexMenu->addSeparator();
    m_indexActions.clear();
    for (const IndexSpec& d : m_plugin.indexRoster()) {
        const QString name = d.name;
        QAction* a = m_indexMenu->addAction(name);
        a->setToolTip(d.what + QStringLiteral("\nClick to build it; if it is already done, this drops its cache and rebuilds just this one."));
        connect(a, &QAction::triggered, this, [this, name] {
            if (!m_indexReady) { setStatus(name + QStringLiteral(": game data isn't open yet — try again once it is.")); return; }
            for (const IndexSpec& e : m_plugin.indexRoster()) {
                if (e.name != name) continue;
                if (e.building && e.building()) { setStatus(name + QStringLiteral(": already running.")); return; }
                if (e.ready && e.ready() && e.reset) e.reset();
                if (e.start) e.start();
                setStatus(name + QStringLiteral(": indexing started."));
                break;
            }
            refreshIndexMenu(); refreshIndexIndicator();
        });
        m_indexActions.push_back(a);
    }
    connect(m_indexMenu, &QMenu::aboutToShow, this, &AppShell::refreshIndexMenu);
    refreshIndexMenu();
}

void AppShell::refreshIndexMenu()
{
    const auto roster = m_plugin.indexRoster();
    for (int i = 0; i < roster.size() && i < m_indexActions.size(); ++i) {
        const IndexSpec& d = roster[i];
        const bool rdy = d.ready && d.ready();
        const bool bld = d.building && d.building();
        const QString state = bld ? QStringLiteral("building…") : rdy ? QStringLiteral("done") : QStringLiteral("not started");
        m_indexActions[i]->setText(QStringLiteral("%1  —  %2").arg(d.name, state));
    }
}

void AppShell::refreshIndexIndicator()
{
    if (!m_idxStatus) return;
    const auto roster = m_plugin.indexRoster();
    int running = 0, done = 0;
    QString phase, rows;
    for (const IndexSpec& d : roster) {
        const bool rdy = d.ready && d.ready();
        const bool bld = d.building && d.building();
        if (bld) { if (!running) phase = d.name; ++running; }
        if (rdy) ++done;
        rows += QStringLiteral("<tr><td><b>%1</b></td><td>&nbsp;%2&nbsp;</td><td style='color:#999'>%3</td></tr>")
                    .arg(d.name, bld ? QStringLiteral("⟳ building") : rdy ? QStringLiteral("✓ done") : QStringLiteral("— not started"), d.what);
    }
    m_idxStatus->setToolTip(QStringLiteral("<b>Background indexing</b> — %1 of %2 built<br>Start or re-run any of them from <b>File ▸ Index</b>.<hr><table cellspacing='0'>%3</table>")
                                .arg(done).arg(roster.size()).arg(rows));
    if (running > 0) {
        m_idxStatus->setStyleSheet(QStringLiteral("color:#e8c46a;"));
        m_idxStatus->setText(QStringLiteral("⟳ Indexing %1%2").arg(phase, running > 1 ? QStringLiteral(" (+%1 more)").arg(running - 1) : QString()));
    } else if (done == roster.size()) {
        m_idxStatus->setStyleSheet(QStringLiteral("color:#6f9f6f;"));
        m_idxStatus->setText(QStringLiteral("Indexes ✓"));
    } else {
        m_idxStatus->setStyleSheet(QStringLiteral("color:#999;"));
        m_idxStatus->setText(QStringLiteral("Indexes %1/%2").arg(done).arg(roster.size()));
    }
}

// ── Export confirmation (D4 toast + tray, POE2 reveal button) ─────────────────────────────────
void AppShell::showExportToast(const QString& text, const QString& folder, const QString& file)
{
    setStatus(text);
    m_lastExportFolder = folder;
    m_revealBtn->setVisible(!folder.isEmpty());
    if (!folder.isEmpty()) m_revealBtn->setToolTip(QStringLiteral("Open %1").arg(QDir::toNativeSeparators(folder)));
    qInfo("export: %s%s", qUtf8Printable(text), folder.isEmpty() ? "" : qUtf8Printable(QStringLiteral(" → ") + QDir::toNativeSeparators(folder)));

    if (!m_exportToast) {
        m_exportToast = new QLabel(this);
        m_exportToast->setTextFormat(Qt::RichText);
        m_exportToast->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
        m_exportToast->setStyleSheet(QStringLiteral(
            "QLabel{ background:rgba(26,26,28,0.96); color:#e8e8e8; border:1px solid #4a4a4a; border-radius:6px; padding:8px 14px; }"));
        connect(m_exportToast, &QLabel::linkActivated, this, [](const QString& href) { QDesktopServices::openUrl(QUrl::fromLocalFile(href)); });
        m_exportToastTimer = new QTimer(this);
        m_exportToastTimer->setSingleShot(true);
        connect(m_exportToastTimer, &QTimer::timeout, this, [this] { if (m_exportToast) m_exportToast->hide(); });
    }
    QString html = QStringLiteral("<b>✓</b>  %1").arg(text.toHtmlEscaped());
    if (!folder.isEmpty())
        html += QStringLiteral("     <a style='color:#e8c46a; text-decoration:none;' href=\"%1\">Show in folder</a>").arg(folder.toHtmlEscaped());
    m_exportToast->setText(html);
    m_exportToast->adjustSize();
    positionExportToast();
    m_exportToast->show();
    m_exportToast->raise();
    m_exportToastTimer->start(6000);

    const QString after = ExportNotifier::runAfter(folder, file);
    if (!after.isEmpty()) qInfo("%s", qUtf8Printable(after));

    // Desktop notification: the toast is inside the window, invisible precisely when it matters.
    if (m_harness || !settings::osNotify() || !QSystemTrayIcon::isSystemTrayAvailable()) return;
    if (!m_tray) {
        QIcon ic = windowIcon();
        if (ic.isNull()) ic = style()->standardIcon(QStyle::SP_DialogSaveButton);
        m_tray = new QSystemTrayIcon(ic, this);
        m_tray->setToolTip(m_plugin.productName());
        m_tray->show();
    }
    if (!m_tray->isVisible()) m_tray->show();
    m_tray->showMessage(QStringLiteral("%1 — export finished").arg(m_plugin.productName()), text, QSystemTrayIcon::Information, 8000);
}

void AppShell::positionExportToast()
{
    if (!m_exportToast) return;
    const QSize s = m_exportToast->size();
    m_exportToast->move((width() - s.width()) / 2, height() - s.height() - 44);
}

void AppShell::showShortcuts()
{
    TextReport::show(this, QStringLiteral("Keyboard && mouse"), hotkeys::cheatSheetText(m_plugin.productName()));
}

QString AppShell::diagnosticInfo() const
{
    QStringList s;
    s << QStringLiteral("%1 %2").arg(m_plugin.productName(), m_plugin.version());
    s << QStringLiteral("Qt %1 · %2").arg(QString::fromLatin1(qVersion()), QSysInfo::prettyProductName());
    s << QStringLiteral("GL: %1").arg(gl::GLModelWidget::glInfo());
    s << QStringLiteral("Data folder: %1").arg(QDir::toNativeSeparators(paths::dataDir()));
    s << QStringLiteral("Log file: %1 (%2)").arg(QDir::toNativeSeparators(log::filePath()), log::fileLogging() ? QStringLiteral("writing") : QStringLiteral("off"));
    if (QFile::exists(crash::filePath())) s << QStringLiteral("Crash note from a previous session: %1").arg(QDir::toNativeSeparators(crash::filePath()));
    else if (!crash::previousNotePath().isEmpty()) s << QStringLiteral("Last crash note: %1").arg(QDir::toNativeSeparators(crash::previousNotePath()));
    s << QStringLiteral("Tabs: %1").arg(tabIds().join(QStringLiteral(", ")));
    s << QStringLiteral("Game data: %1").arg(m_indexReady ? QStringLiteral("loaded") : QStringLiteral("not loaded"));
    const QString game = m_plugin.diagnosticInfo();
    if (!game.isEmpty()) s << QString() << game;
    return s.join(QLatin1Char('\n'));
}

void AppShell::copyDiagnostics()
{
    QApplication::clipboard()->setText(diagnosticInfo());
    setStatus(QStringLiteral("Diagnostic info copied to the clipboard."));
}

}  // namespace core
