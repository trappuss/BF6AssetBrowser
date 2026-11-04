#pragma once
#include "core/app/Hotkeys.h"
#include "core/app/SelfTests.h"
#include "core/export/ExportLayout.h"
#include "core/gl/ViewportExtension.h"
#include "core/store/IGameStore.h"
#include "core/types/Asset.h"
#include "core/types/ExportOptions.h"
#include <QString>
#include <QVector>
#include <QWidget>
#include <functional>

// ── What a game contributes to the shell ────────────────────────────────────────────────────────
// The shell owns the menu bar, the export menu, the status bar, hotkeys, the log, the settings
// skeleton, geometry/tab persistence and the index indicator (core-audit/app-shell.md §2.5). The
// three MainWindows hardcode their tab lists today — that list is the first thing this replaces.
namespace core {

class BrowserTab;

struct TabSpec {
    QString title;              // tab-bar label — may be renamed without breaking anything
    QString stableId;           // "textures" — what session/tab and Ctrl+N persist (NEVER the title:
                                // FOX stores the title today, so renaming a tab drops the restore)
    bool    lazy = false;       // [D4 LazyTab] construct on first show
    std::function<BrowserTab*(QWidget* parent)> make;
};

struct IndexSpec {              // one File ▸ Index row + its status indicator   [D4 IndexDesc]
    QString name;
    QString what;               // one line: what it makes possible
    std::function<bool()> ready;
    std::function<bool()> building;
    std::function<void()> start;
    std::function<void()> reset;
    bool hasPercent = false;
};

struct HelpEntry {
    QString label;
    QString tooltip;
    std::function<void()> run;
};

struct SettingsPage {
    QString title;
    // Template §16 order: setup 0 · presentation 1 · per-area 2 · disk 3 · keys 4 · upkeep 5 ·
    // reference 6 · experimental 7. Core owns 0–6's generic halves; a plugin inserts its own.
    int     order = 2;
    std::function<QWidget*(QWidget* parent)> build;
    std::function<void()> apply;        // on OK
    std::function<void()> revert;       // on Cancel, for pages that write live
    std::function<void()> reload;       // re-read every widget from settings (after a reset)
    QStringList resetGroups;            // QSettings groups cleared BY REMOVAL (§3.10)
    QStringList keepPrefixes;           // user-authored subtrees a reset must not touch
    // Keys a live-writing page touches, by prefix: snapshotted on open and restored on Cancel
    // (D4's "Cancel truly cancels", SettingsDialog.cpp:2482-2579). Empty for pages that apply on OK.
    QStringList liveKeyPrefixes;
};

struct CacheDef {                       // [FOX cachemaint::defs()]
    QString label, subdir, glob, tooltip;
};

class AppPlugin {
public:
    virtual ~AppPlugin() = default;

    virtual QString productName() const = 0;        // window title, About, log stem, INI name
    virtual QString version() const = 0;
    virtual IGameStore* store() = 0;

    virtual QVector<TabSpec>      tabs() = 0;
    virtual QVector<IndexSpec>    indexRoster() { return {}; }
    virtual QVector<SettingsPage> settingsPages() { return {}; }
    virtual QVector<HelpEntry>    helpEntries() { return {}; }
    virtual QVector<CacheDef>     cacheDefs() { return {}; }

    // Is the game folder configured? Drives the first-run page. configureFolder() is what the
    // core default first-run page and File ▸ Set folder call: open the game's own picker and
    // persist the choice; return true if something was set.
    virtual bool isConfigured() = 0;
    virtual QWidget* firstRunPage(QWidget* parent) { return nullptr; }
    virtual bool configureFolder(QWidget* parent) = 0;
    virtual QString firstRunText() const { return {}; }   // rich text under the default page's title
    // Open/re-open the store on a worker; `done` is called on the GUI thread. `secondaryReady` is
    // called on the GUI thread when the store's secondary indexes finish (may be called at once).
    virtual void reload(std::function<void(bool ok, const QString& status)> done,
                        std::function<void()> secondaryReady) = 0;
    virtual QString diagnosticInfo() { return {}; }
    virtual QString informationHtml() const { return {}; }   // Settings ▸ Information prose
    // A settings page whose apply() moved the game folder sets this; the shell reads it once after
    // the dialog closes and reloads (POE2's SettingsDialog::gameDirChanged → MainWindow::reloadIndex).
    virtual bool takeReloadRequest() { return false; }
    // Ask the shell to reload NOW, from anywhere (FOX: a mod-folder replacement or revert ends in a
    // rescan, MainWindow.cpp:438-441). The shell installs this when it is built; a reload asked for
    // during one is queued, and one asked for during a bulk run is refused with the reason.
    void requestShellReload() { if (m_shellReload) m_shellReload(); }
    void setShellReload(std::function<void()> fn) { m_shellReload = std::move(fn); }

    // ── the look ────────────────────────────────────────────────────────────────────────────
    // Which of the core's lit features this game's renderer has (ViewportExtension.h, "Which look
    // a viewport has"). All off by default: the plain read of the store's materials. A game whose
    // renderer has shadows/AO/IBL turns them on here; every core viewport the shell builds asks.
    virtual gl::ViewFeatures viewFeatures() const { return {}; }
    // Per-game defaults for the RENDER panel's persisted controls: the Light preset (0=Warm, 1=Neutral,
    // 2=Cool) and the backdrop gradient. These seed the fresh-INI value only — once the viewer changes
    // the control it persists. (The lit-feature toggles are not persisted; they come from viewFeatures.)
    // Family default: Warm key + gradient on.
    virtual int  defaultLightPreset() const { return 0; }
    virtual bool defaultBackgroundGradient() const { return true; }
    // The game's viewport extension — its shading (whole shader or spliced blocks), extra passes,
    // pose hooks. One instance, owned by the plugin, attached to every core viewport the shell
    // builds. Null = the plain core look.
    virtual gl::ViewportExtension* viewportExtension() { return nullptr; }

    // ── harness ─────────────────────────────────────────────────────────────────────────────
    // The command line minus the core's own flags, before QApplication. Return every token the
    // plugin consumed (flag AND its value) so Qt never sees them. FOX's --games / --character… go here.
    virtual QStringList parseArgs(const QStringList& args) { Q_UNUSED(args); return {}; }
    // Called once the index is ready (or at once when no game folder is configured) whenever
    // parseArgs consumed anything, after --tab has shown its tab and before a --shot settles:
    // drive the tabs into the state the flags asked for (the plugin holds what parseArgs kept).
    virtual void applyArgs(class AppShell& shell) { Q_UNUSED(shell); }

    // ── search vocabulary the plugin owns ───────────────────────────────────────────────────
    // "Is this search term an id?" and "does this id term name this row?" are engine-specific:
    // D4 = a run of digits (SNO, exact), FOX = a 16-hex PathFileNameCode (exact), POE2 = decimal
    // or 0x-hex MurmurHash matched as a SUFFIX of the 16-digit hex form. The one matcher stays
    // in core; only these two vary (app-shell.md §1.1).
    virtual bool isIdTerm(const QString& term) const { return false; }
    virtual bool idTermMatches(const QString& term, const AssetId& id) const { return false; }

    // ── export ──────────────────────────────────────────────────────────────────────────────
    // The game's default ExportOptions (unit scale, yaw, normal-map handling…). Settings and the
    // at-export dialog start from these, so an unconfigured install exports as the game always did.
    virtual ExportOptions exportDefaults() const { return {}; }
    virtual QString unitScaleTip() const { return QStringLiteral("Metres per native game unit. 1.0 keeps raw native units."); }
    // Bulk folder layout: the built-in modes plus the game's tag modes (core::layout::Spec).
    virtual layout::Spec layoutSpec() { layout::Spec s; s.modes = layout::builtinModes(); return s; }
    // Model kinds the Models tab lists and texture kinds the Textures tab lists (IGameStore::kinds()
    // names). Empty ⇒ the tab is not built.
    virtual QStringList modelKinds() const { return {}; }
    virtual QStringList textureKinds() const { return {}; }

    // ── hotkeys ─────────────────────────────────────────────────────────────────────────────
    // The registry rows. Start from hotkeys::coreDefs(); drop a row whose action the game cannot
    // offer, append the game's own.
    virtual QVector<hotkeys::Def> hotkeyDefs() const { return hotkeys::coreDefs(); }

    // A hook to decorate a generic tab after construction (add a panel, a menu row, a column).
    virtual void decorateTab(const QString& stableId, BrowserTab* tab) { Q_UNUSED(stableId); Q_UNUSED(tab); }

    // Menu nouns, so core's MenuText can say "Copy SNO" in D4 and "Copy hash" in FOX without
    // either word living in core.
    struct Nouns {
        QString id = QStringLiteral("id");          // "SNO" / "hash"
        QString collection;                          // "collection" — empty ⇒ the action is omitted
        QString modelExt = QStringLiteral("model");
    };
    virtual Nouns nouns() const { return {}; }

    // Startup self-tests this plugin contributes (name + "" on success / first failing case).
    virtual QVector<selftest::Test> selfTests() { return {}; }
private:
    std::function<void()> m_shellReload;
};

}  // namespace core
