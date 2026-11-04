#pragma once
// The Bulk extraction tab (template §14): filter the whole index with the SAME search + funnel as the
// asset lists (the one matcher — the count you see is the set you export), pick an output folder and
// layout, then export everything in one background run. Run controls only; the export options are
// the shared Settings ▸ Export keys. The run machinery (manifest / cancel / pause / ETA / failures,
// §25) lives in BulkExtractor. POE2's tab over IGameStore, with the layout groups computed here on
// the GUI thread through the plugin's layout::Spec (D4).
#include "core/app/BrowserTab.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableView;
class QThread;
class QTimer;

namespace core {

class AppPlugin;
class AssetListModel;
class BulkExtractor;
class FunnelFilter;

class BulkTab : public BrowserTab {
    Q_OBJECT
public:
    BulkTab(AppPlugin& plugin, const QStringList& modelKinds, const QStringList& textureKinds, QWidget* parent = nullptr);
    ~BulkTab() override;

    void onIndexReady() override;
    void onSecondaryReady() override;
    void reset() override;
    void persistView() override;

    AssetListModel* listModel() const { return m_model; }
    bool running() const { return m_worker != nullptr; }
    QString busyReason() const override { return running() ? QStringLiteral("a bulk export is running") : QString(); }
    // Harness: set the query/folder and start a run without the widgets being clicked.
    void setQuery(const QString& q);
    void setOutDir(const QString& dir);
    void setRawOriginals(bool on);
    void setOnlyNew(bool on);
    void setLayoutMode(const QString& id);   // a layout::Mode id; unknown ids are ignored
    void startRun();
    void cancelRun();

Q_SIGNALS:
    void runFinished(int exported, int skipped, int failed);

protected:
    void keyPressEvent(QKeyEvent*) override;   // Esc cancels a run

private:
    void applyGroups();   // the funnel vocabulary: the groups every listed kind shares, + Special
    void applyFilter();
    void togglePause();
    void setRunning(bool running);

    AppPlugin& m_plugin;
    QStringList m_modelKinds, m_textureKinds;
    AssetListModel* m_model = nullptr;
    QLineEdit* m_search = nullptr;
    QComboBox* m_kindFacet = nullptr;
    FunnelFilter* m_funnel = nullptr;
    QLabel* m_count = nullptr;
    QTableView* m_list = nullptr;
    QLineEdit* m_outDir = nullptr;
    QComboBox* m_layout = nullptr;
    QCheckBox* m_onlyNew = nullptr;
    QCheckBox* m_doModels = nullptr;
    QCheckBox* m_doTextures = nullptr;
    QCheckBox* m_rawOriginals = nullptr;
    QSpinBox* m_workers = nullptr;
    QPushButton* m_run = nullptr;
    QPushButton* m_pause = nullptr;
    QPushButton* m_cancel = nullptr;
    QProgressBar* m_progress = nullptr;
    QPlainTextEdit* m_console = nullptr;
    QLabel* m_eta = nullptr;
    QTimer* m_debounce = nullptr;
    QThread* m_thread = nullptr;
    BulkExtractor* m_worker = nullptr;
    bool m_paused = false;
};

}  // namespace core
