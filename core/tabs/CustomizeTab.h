#pragma once
// The core Customize tab — character assembly for any game (docs/customize/CUSTOMIZE_CORE_DESIGN.md).
// The game answers ICustomizeSource (who, which slots, which options, what a build is made of); this
// tab owns everything else: the slot lists (FOX's shape), the build text that presets, undo, the
// clipboard and the harness share, the assembly worker, the viewport, playback, the panels and every
// export — so a new game gets all of it by answering four questions.
#include "core/app/BrowserTab.h"
#include "core/customize/Assemble.h"
#include "core/customize/Build.h"
#include "core/customize/CustomizeSource.h"
#include "core/tabs/ModelLoad.h"

#include <QHash>
#include <QPointer>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <memory>
#include <thread>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QGroupBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QPushButton;
class QSlider;
class QTableWidget;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace core {

class AppPlugin;
class NPanel;
namespace gl { class GLModelWidget; }

class CustomizeTab : public BrowserTab {
    Q_OBJECT
public:
    CustomizeTab(AppPlugin& plugin, customize::ICustomizeSource& source, QWidget* parent = nullptr);
    ~CustomizeTab() override;

    // BrowserTab
    void onIndexReady() override;
    void reset() override;
    gl::GLModelWidget* previewWidget() override { return m_view; }
    bool    hasExportSelection() const override { return m_assembly.ok; }
    void    exportSelection() override { exportBuild(Full); }
    void    exportSelectionToLast() override;
    QString exportNoun() const override { return QStringLiteral("build"); }
    void    populateExportMenu(QMenu* menu) override;

    // ── The build, as text (presets, undo, clipboard, harness all go through these) ────────────
    customize::Build currentBuild() const;
    // Apply a build: subject, slots and toggles. Options the current lists do not hold are skipped
    // and named in `missing`. assembleNow = true assembles synchronously (harness, export).
    bool applyBuild(const customize::Build& b, bool assembleNow, QStringList* missing = nullptr);
    // Waits for any queued/in-flight assembly and applies it. True when a figure is shown.
    bool assembleNow();
    const customize::Assembly& assembly() const { return m_assembly; }

    // ── Harness / evidence ───────────────────────────────────────────────────────────────────
    int  subjectCount() const { return m_subjects.size(); }
    // A category's subjects in the source's order ("" = the current category) — the harness's
    // "subject=@<n>", so a check need not name a subject an install may lack.
    int  subjectCountIn(const QString& categoryId) const;
    QString subjectIdAt(const QString& categoryId, int i) const;   // "" when out of range
    QStringList slotIds() const;
    QVector<customize::SlotDef> slotDefs() const;
    int  optionCount(const QString& slotId) const;               // excluding "none"
    QString optionIdAt(const QString& slotId, int i) const;       // i in [0, optionCount)
    QStringList listRows(const QString& slotId) const;          // the slot list's rows as shown (row 0, captions, options)
    bool pickOption(const QString& slotId, const QString& optionId);   // as a person picks it (the game's rules run)
    bool pickSubject(const QString& subjectId);                         // as a person picks it (followBase runs)
    bool pickVariant(const QString& variantId);                         // (in the current category's list)
    void loadPreset(const QString& name) { applyPreset(name); }        // as the Builds menu loads one
    void setUnlocked(bool on);
    void setCompatOnly(bool on);
    bool undo();
    bool redo();
    int  undoDepth() const { return m_undo.size(); }
    enum ExportMode { Full, NoHeld, EachPiece, RawOriginals };
    // Non-interactive whole-build export (Full / NoHeld) → path, or "" with `error`.
    QString exportBuildTo(const QString& path, ExportMode mode, QString* error);
    // What "Each piece" and "Original files" write: the body (or a form) and the ticked pieces,
    // each with its look, a brought model already fitted left out.
    QVector<customize::Piece> shownPieces() const;
    // The piece a slot's own export writes (look rows kept; invalid mesh = nothing in the slot).
    customize::Piece slotPiece(const QString& slotId) const;
    // One piece in its own space and rig, with its look (Each piece, the slot menu, the harness).
    // `hideParts`: part indices of that mesh to leave out (the body's parts the game hides).
    model::ExportResult exportPiece(const AssetId& id, const QString& label, const customize::Look& look,
                                    const ExportOptions& opt, const QString& out, const QSet<int>& hideParts = {});

private:
    struct SlotRow {
        customize::SlotDef def;
        QLabel* label = nullptr;
        QComboBox* combo = nullptr;
        QToolButton* exportBtn = nullptr;
        QVector<customize::Option> options;   // the selectable rows after row 0, in order (section headers are not options)
        customize::Compat compat;             // the source's narrowing answer, for the label's state and tooltip
        bool narrowed = false;                // options were filtered to what fits
    };

    void buildLeftColumn(QWidget* panel);
    void buildRightColumn(QWidget* right);
    void setCategory(const QString& categoryId);         // lists that category's subjects (does not load one)
    void refreshVariants(const QString& keepId);         // the subject's bases, keeping `keepId` when offered
    void onSlotPicked(int row);                          // a user pick: the game's rules, then the lists
    void followBase(bool subjectChanged);                // a user's new subject / variant: the game re-dresses
    void loadSubject(const QString& subjectId);          // slots + toggles + sets for a subject
    // Every slot's list for the current build — or for `target`, the build about to be applied
    // (its picks are what each list must offer and keep; see applyBuild).
    void repopulateOptions(const customize::Build* target = nullptr);
    // Variant, toggles, every list (built for `b`) and every slot's pick, from `b` — shared by
    // applyBuild and the rules' result. Names what a list could not show in *missing.
    void setControls(const customize::Build& b, QStringList* missing);
    void updateSlotStates();                             // bold fitted · dim empty · faded no options
    void applyFind(const QString& text);

    void scheduleAssembly();                             // debounced
    void startAssembly();                                // → worker
    void applyAssembly(const customize::Assembly& a, const customize::Build& b);
    void applyVisibility();
    void refreshPanels();
    void refreshAnimBar(const QString& keepClip, float keepTime, bool keepPlaying);
    void setClipIndex(int i);
    void setPlaying(bool on);

    void noteSettled(const customize::Build& b);         // undo: diff settled builds
    void reloadPresets();
    void savePreset();
    void deletePreset();
    void applyPreset(const QString& name);
    QMap<QString, QString> presets() const;              // name → build text
    void storePresets(const QMap<QString, QString>& p);
    void copyBuild();
    void pasteBuild();
    void randomize();
    void clearSlots();
    void applySet(const QString& setId);

    void exportBuild(ExportMode mode);
    void exportSlot(int row, bool raw);
    void showSlotMenu(int row, const QPoint& globalPos);
    int  extractOriginals(const QVector<AssetId>& ids, const QString& dir, int* failed);
    QString exportStem() const;
    void report(const QString& text);

    AppPlugin& m_plugin;
    customize::ICustomizeSource& m_src;
    std::shared_ptr<customize::MeshCache> m_cache;   // shared with the worker (it may outlive a rebuild)

    // Left column.
    QFormLayout* m_headForm = nullptr;
    QComboBox*   m_categoryBox = nullptr;
    QComboBox*   m_subjectBox = nullptr;
    QComboBox*   m_variantBox = nullptr;
    QWidget*     m_rulesRow = nullptr;     // Unlocked / Only-compatible, shown when the subject has them
    QCheckBox*   m_unlocked = nullptr;
    QCheckBox*   m_compatOnly = nullptr;
    QGroupBox*   m_lookBox = nullptr;      // look rows (SlotKind::Look) under their own caption
    QVBoxLayout* m_lookLayout = nullptr;
    QLabel*      m_subjectInfo = nullptr;
    QComboBox*   m_setBox = nullptr;
    QWidget*     m_setRow = nullptr;
    QHBoxLayout* m_toggleRow = nullptr;
    QWidget*     m_toggleHost = nullptr;
    QLineEdit*   m_find = nullptr;
    QWidget*     m_slotHost = nullptr;
    QVBoxLayout* m_slotLayout = nullptr;
    QComboBox*   m_presetBox = nullptr;
    QPushButton* m_undoBtn = nullptr;
    QPushButton* m_redoBtn = nullptr;
    QLabel*      m_statusLine = nullptr;

    QVector<customize::Category> m_categories;
    QString m_category;
    QString m_noun;                 // the current category's subject noun ("Weapon", "Character")
    QVector<customize::Subject> m_allSubjects;           // every category's
    QVector<customize::Subject> m_subjects;              // the current category's (what m_subjectBox lists)
    QVector<customize::Option> m_variants;
    QVector<SlotRow> m_rows;
    QVector<customize::Toggle> m_toggleDefs;
    QHash<QString, QCheckBox*> m_toggles;
    QString m_subject;
    bool m_ready = false;
    bool m_syncing = false;
    bool m_framedSubject = false;

    // Assembly: debounced, on a worker, newest wins (generation token); the previous figure stays
    // on screen until the new one is ready (D4's rebuild policy).
    QTimer* m_debounce = nullptr;
    std::atomic<quint64> m_gen{0};
    std::thread m_worker;              // joined before the next one starts, in reset() and on destruction
    void joinWorker();
    bool m_inFlight = false;
    quint64 m_workerSerial = 0;   // which worker m_inFlight belongs to (GUI thread only)
    bool m_dirtyWhileBusy = false;
    customize::Assembly m_assembly;
    customize::Build m_assembledBuild;
    QHash<QString, bool> m_pieceShown;   // piece label → ticked in ATTACHMENTS (kept across rebuilds)

    // Undo: settled builds, diffed (FOX) — no code path can forget to record.
    QVector<customize::Build> m_undo, m_redo;
    customize::Build m_settled;
    bool m_haveSettled = false;
    bool m_applyingHistory = false;

    // Centre + right.
    gl::GLModelWidget* m_view = nullptr;
    NPanel*       m_panels = nullptr;
    QTableWidget* m_partsTable = nullptr;
    QListWidget*  m_pieceList = nullptr;
    QLabel*       m_infoPanel = nullptr;
    QListWidget*  m_clipList = nullptr;
    QWidget*      m_animBar = nullptr;
    QComboBox*    m_clipBox = nullptr;
    QToolButton*  m_playBtn = nullptr;
    QSlider*      m_timeline = nullptr;
    QLabel*       m_timeLbl = nullptr;
    QTimer*       m_playTimer = nullptr;
};

}  // namespace core
