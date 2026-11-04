#pragma once
// The generic Models tab (tabs-store.md §2.1, §6): the shared asset list + the one core viewport,
// shading modes, a channel viewer, overlays behind the master gate, an N-panel column with the
// panels every game fills — PARTS (name/tris/material/visibility), ANIMATIONS, ATTACHMENTS, INFO —
// single and multi-model export, image and GIF capture, raw-original extraction.
//
// Whose it is: POE2's structure (store-driven, one resolver, the export paths), D4's LOAD PATH —
// the worker thread with a token check under seh::runGuarded, so a slow parse never freezes the
// list and a malformed asset reports instead of taking the app down (FOX and POE2 parsed on the GUI
// thread; plan §8.4 settled the core on D4's) — and FOX's NPanel column. A game adds its own panels
// and menu rows through the hooks at the bottom (AppPlugin::decorateTab).
#include "core/app/BrowserTab.h"
#include "core/tabs/ModelLoad.h"
#include "core/types/ExportOptions.h"

#include <QCache>
#include <QPointer>
#include <QSet>
#include <QVector>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

class QAbstractItemView;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QSlider;
class QSplitter;
class QTableView;
class QTableWidget;
class QTimer;
class QToolButton;

namespace core {

class AppPlugin;
class AssetListModel;
class FunnelFilter;
class NPanel;
namespace gl { class GLModelWidget; class ThumbnailRenderer; }

class ModelsTab : public BrowserTab {
    Q_OBJECT
public:
    // `kinds` are the IGameStore kinds this tab lists (AppPlugin::modelKinds()).
    // `persistKey` namespaces this tab's remembered layout (panel open-set/order, splitter
    // sizes, tip-bar dismissal). It defaults to "models"; a second ModelsTab in the same
    // product (e.g. DR's §33 Weapons / Characters showpiece tabs) MUST pass its own key, or
    // the tabs share one QSettings entry and stomp each other's panel state.
    ModelsTab(AppPlugin& plugin, const QStringList& kinds, QWidget* parent = nullptr,
              const QString& persistKey = QStringLiteral("models"));
    ~ModelsTab() override;

    // BrowserTab
    void onIndexReady() override;
    void onSecondaryReady() override;
    void refresh() override;
    void reset() override;
    void onSettingsChanged() override;
    void persistView() override;
    gl::GLModelWidget* previewWidget() override { return m_view; }
    bool    hasExportSelection() const override;
    void    exportSelection() override;
    void    exportSelectionToLast() override;
    QString exportNoun() const override;
    bool    hasAnimExport() const override;
    QString animExportLabel() const override;
    void    exportAnimations() override;
    void    populateExportMenu(QMenu* menu) override;

    // ── What a plugin reaches for ────────────────────────────────────────────────────────────
    gl::GLModelWidget* view() const { return m_view; }
    NPanel* panels() const { return m_panels; }
    AssetListModel* listModel() const { return m_model; }
    AssetId currentId() const { return m_currentId; }
    const model::Loaded* current() const { return m_current.ok ? &m_current : nullptr; }
    int clipBoxRow() const;   // what the clip bar shows: 0 = bind pose, k = clip k-1
    // Tick / untick attachment `ai` of the model on screen (the ATTACHMENTS list does the same).
    // True when it ends up placed as asked (a piece that cannot be placed stays off).
    bool setAttachmentShown(int ai, bool on);
    const QVector<bool>& attachmentsShown() const { return m_attachmentShown; }
    QVector<AssetId> selectedIds() const;           // the list selection (falls back to the shown model)
    void showModel(const AssetId& id);              // select + load
    bool ensureLoaded(const AssetId& id);           // synchronous; true when the viewport holds it
    // Re-read the row set from the store (after a plugin changed which rows a kind lists) and re-apply
    // the current filters — e.g. a DR toggle that hides non-loadable records from the Models list.
    void reloadList();
    // Harness (FOX's --model / --export, re-hosted): filter the list by this search text now — no
    // debounce — and load the first match synchronously; false when nothing matched. Export the
    // shown model to this path with the saved export options (.gltf → separate files); false with
    // the reason in *error when there is no model or the write failed.
    bool selectModel(const QString& searchText);
    bool exportTo(const QString& path, QString* error = nullptr);
    // Playback, as the animation bar drives it (a lazy clip is decoded here — FOX's --clip).
    void setClipIndex(int i);          // -1 = bind pose
    void setPlaying(bool on);

    // Actions the Export menu and the row menu share.
    void exportParts();
    void extractOriginals();
    void saveImage();
    void turntableGif();
    void animLoopGif();

Q_SIGNALS:
    void modelLoaded(const core::AssetId& id);
    // Decoration points for a plugin: add rows to the list's right-click menu / the viewport's part menu.
    void rowMenuAboutToShow(QMenu* menu, const QVector<core::AssetId>& ids);
    void partMenuAboutToShow(QMenu* menu, const QSet<int>& parts);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void buildLeft(QWidget* left);
    void buildRight(QWidget* right);
    void applyFilter();
    void loadCurrentRow();
    void loadModel(const AssetId& id);
    void applyLoaded(const model::Loaded& m, quint64 token);
    void startLoad(const AssetId& id, quint64 token);
    void clearCurrentModel(const QString& overlay);   // nothing current; every panel emptied
public:
    // The model-thumbnail folder for a data fingerprint and icon style; prunes stale core thumbnail
    // folders/files once per run (see ModelsTab.cpp). Public for the tests.
    static QString thumbnailDiskDir(const QString& fingerprint, bool textured);
private:
    void onLoadFinished(const model::Loaded& m, quint64 token);
    void waitForLoader(const char* why);   // blocks until no loader thread is inside the store
    void rebuildPartsPanel();
    void refreshInfo();
    void refreshAnimBar();
    void refreshAttachments();
    void installThumbLoader();   // loader + disk folder for the current icon style
    void applyNavHeld();   // advance one row by a deferred arrow press, once the current model loaded
    void applyAttachments();   // re-assemble the body + the ticked pieces, rebuild the list
    void poseForAttachments(); // a shown piece with no rest placement → the game's idle, frame 0 (IGameStore::restClip)
    void relistFromSecondary();   // clips / attachments a secondary index brings, for the model shown
    void showRowContextMenu(const QPoint& viewPos, QAbstractItemView* view);
    void showViewportPartMenu(int part, const QPoint& globalPos);
    void setGridView(bool on);
    void setIconPx(int px);
    void prerenderVisible();        // proactively render thumbnails for the visible grid rows (+ margin)
    void schedulePrerender();       // coalesce scroll/filter bursts into one prerenderVisible()
    QVector<int> currentPartSubset() const;
    QVector<int> exportScopeParts() const;   // selection verbatim, else the visible body parts
    ExportOptions optionsForExport(bool* cancelled, int modelCount);
    void exportGifs(bool turntable);
    QString exportDirOrPrompt(const QString& title, bool* cancelled);

    AppPlugin& m_plugin;
    QStringList m_kinds;
    QString m_persistKey;   // QSettings namespace for this tab's layout (see ctor)
    AssetListModel* m_model = nullptr;
    QLineEdit* m_search = nullptr;
    QComboBox* m_kindFacet = nullptr;
    FunnelFilter* m_funnel = nullptr;
    QTableView* m_list = nullptr;
    QListView* m_grid = nullptr;
    QToolButton* m_gridBtn = nullptr;
    QLabel* m_count = nullptr;
    QTimer* m_debounce = nullptr;
    QTimer* m_loadDebounce = nullptr;

    gl::GLModelWidget* m_view = nullptr;
    QComboBox* m_shading = nullptr;
    QComboBox* m_channel = nullptr;
    QCheckBox* m_overlaysOn = nullptr;
    QCheckBox* m_gridChk = nullptr;
    QCheckBox* m_skelChk = nullptr;
    QCheckBox* m_hpChk = nullptr;
    QWidget* m_animBar = nullptr;
    QComboBox* m_clipBox = nullptr;
    QToolButton* m_playBtn = nullptr;
    QSlider* m_timeline = nullptr;
    QLabel* m_timeLbl = nullptr;
    QTimer* m_playTimer = nullptr;
    NPanel* m_panels = nullptr;
    QTableWidget* m_parts = nullptr;
    QListWidget* m_clipList = nullptr;
    QListWidget* m_attachList = nullptr;
    QLabel* m_info = nullptr;
    QSplitter* m_split = nullptr;

    gl::ThumbnailRenderer* m_thumbs = nullptr;
    QTimer* m_prerenderTimer = nullptr;   // coalesces grid scroll/filter into one visible-range prerender
    int m_iconPx = 160;
    bool m_texturedIcons = false;
    int m_previewTexCap = 0;      // 3D preview texture resolution (px; 0 = native/full default) — ui/previewTextureRes
    int m_thumbTexCap = 512;      // grid-thumbnail texture resolution (px; 0 = native) — ui/thumbnailTextureRes

    model::Loaded m_current;
    AssetId m_currentId;
    Geometry m_shownGeo;                 // what the viewport holds (assembled when attachments are on)
    QVector<int> m_attachmentOfPart;     // -1 = the body's own part
    QVector<bool> m_attachmentShown;
    std::atomic<quint64> m_loadToken{0};
    quint64 m_secondaryGen = 0;       // bumped by onSecondaryReady
    quint64 m_loadStartGen = 0;       // m_secondaryGen when the current load was dispatched
    bool m_assembledShown = false;    // the viewport holds an assembly, not the plain body
    bool m_syncingSelection = false;
    bool m_loading = false;
    int  m_navHeld = 0;   // a held Up(-1)/Down(+1) deferred while a model loads (load-gated arrow scrub)
    // ONE loader thread at a time, and never one outliving what it reads (audit 2026-09-25: loads
    // were detached threads nothing waited for — a reload reopened the store under a parse, and on
    // exit the plugin and store were destroyed with a parse still inside them). A selection made
    // while one runs becomes m_pendingLoad and starts when it finishes. The state is shared with
    // the thread, so it outlives the tab if it must.
    struct LoaderState { std::mutex m; std::condition_variable cv; int running = 0; };
    std::shared_ptr<LoaderState> m_loader = std::make_shared<LoaderState>();
    AssetId m_pendingLoad;
    AssetId m_inFlightId;             // what the running loader was started for (GUI thread only)
    quint64 m_inFlightToken = 0;
};

}  // namespace core
