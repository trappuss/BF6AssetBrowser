#include "core/tabs/CustomizeTab.h"

#include "core/app/AppPlugin.h"
#include "core/app/CoreSettings.h"
#include "core/app/ExportNotifier.h"
#include "core/app/StatusLine.h"
#include "core/export/GlbExporter.h"
#include "core/gl/GLModelWidget.h"
#include "core/tabs/ModelLoad.h"
#include "core/ui/Glyphs.h"
#include "core/ui/NPanel.h"
#include "core/ui/SearchableCombo.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <thread>

namespace core {

using namespace customize;

namespace {

// A combo in the left column must not size itself to its longest entry: POE2's lists run to
// thousands of long names, and a combo that asks for their width pushed the whole column (and the
// slot rows' export buttons) past the panel edge — seen in the 2026-09-23 build shots. The list
// popup still shows full names; the closed box elides.
void narrowCombo(QComboBox* c)
{
    c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    c->setMinimumContentsLength(10);
    c->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}
constexpr int kUndoDepth = 30;   // FOX's and D4's depth
}

CustomizeTab::CustomizeTab(AppPlugin& plugin, ICustomizeSource& source, QWidget* parent)
    : BrowserTab(parent), m_plugin(plugin), m_src(source), m_cache(std::make_shared<MeshCache>())
{
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    auto* panel = new QWidget(this);
    buildLeftColumn(panel);
    auto* scroll = new QScrollArea(this);
    scroll->setWidget(panel);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);   // the column fits its width; nothing scrolls sideways
    scroll->setMinimumWidth(320);
    scroll->setMaximumWidth(440);
    root->addWidget(scroll, 0);

    auto* center = new QWidget(this);
    auto* cv = new QVBoxLayout(center); cv->setContentsMargins(0, 0, 0, 0); cv->setSpacing(4);
    m_view = new gl::GLModelWidget(center);
    m_view->setFeatures(m_plugin.viewFeatures());
    m_view->setExtension(m_plugin.viewportExtension());
    cv->addWidget(m_view, 1);
    auto* right = new QWidget(this);
    buildRightColumn(right);
    cv->addWidget(m_animBar);
    root->addWidget(center, 1);
    root->addWidget(right, 0);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(60);
    connect(m_debounce, &QTimer::timeout, this, &CustomizeTab::startAssembly);

    // Undo / redo while this tab has focus. (FOX's monorepo host bound neither.)
    auto* u = new QShortcut(QKeySequence::Undo, this);  u->setContext(Qt::WidgetWithChildrenShortcut);
    auto* r = new QShortcut(QKeySequence::Redo, this);  r->setContext(Qt::WidgetWithChildrenShortcut);
    connect(u, &QShortcut::activated, this, [this] { undo(); });
    connect(r, &QShortcut::activated, this, [this] { redo(); });
}

CustomizeTab::~CustomizeTab()
{
    ++m_gen;        // a worker still running discards its result…
    joinWorker();   // …and is waited for: it uses the store, which may be destroyed after this tab
}

void CustomizeTab::joinWorker()
{
    if (m_worker.joinable()) m_worker.join();
}

void CustomizeTab::report(const QString& text) { StatusLine::instance().report(this, text); }

// ── Left column ─────────────────────────────────────────────────────────────────────────────────

void CustomizeTab::buildLeftColumn(QWidget* panel)
{
    auto* pv = new QVBoxLayout(panel);
    pv->setContentsMargins(8, 8, 8, 8);

    auto* head = new QFormLayout();
    m_headForm = head;
    m_categoryBox = new QComboBox(panel);
    narrowCombo(m_categoryBox);
    m_categoryBox->setToolTip(QStringLiteral("What kind of thing is being built. Each keeps its own subjects and slots."));
    head->addRow(QStringLiteral("Category"), m_categoryBox);
    m_subjectBox = new SearchableCombo(panel);
    narrowCombo(m_subjectBox);
    head->addRow(m_src.subjectNoun(), m_subjectBox);
    m_variantBox = new SearchableCombo(panel);
    narrowCombo(m_variantBox);
    head->addRow(m_src.variantNoun(), m_variantBox);
    head->setRowVisible(m_categoryBox, false);
    head->setRowVisible(m_variantBox, false);
    m_setRow = new QWidget(panel);
    auto* sr = new QHBoxLayout(m_setRow); sr->setContentsMargins(0, 0, 0, 0);
    m_setBox = new SearchableCombo(m_setRow);
    narrowCombo(m_setBox);
    m_setBox->setToolTip(QStringLiteral("Fill every slot a set covers at once. Slots the set has no piece for are cleared."));
    sr->addWidget(m_setBox, 1);
    head->addRow(QStringLiteral("Set"), m_setRow);
    pv->addLayout(head);

    m_subjectInfo = new QLabel(panel);
    m_subjectInfo->setWordWrap(true);
    m_subjectInfo->setStyleSheet(QStringLiteral("color:#9aa;"));
    pv->addWidget(m_subjectInfo);

    m_toggleHost = new QWidget(panel);
    m_toggleRow = new QHBoxLayout(m_toggleHost);
    m_toggleRow->setContentsMargins(0, 0, 0, 0);
    pv->addWidget(m_toggleHost);

    // The game's rules and its compatibility narrowing, each shown only where the subject has them.
    // Both are how the person likes to work, not part of a build: kept in settings, not the text.
    m_rulesRow = new QWidget(panel);
    auto* rr = new QVBoxLayout(m_rulesRow); rr->setContentsMargins(0, 0, 0, 0); rr->setSpacing(2);
    m_compatOnly = new QCheckBox(QStringLiteral("Only what fits"), m_rulesRow);
    m_compatOnly->setToolTip(QStringLiteral("Narrow each list to what the game lets go with the rest of this build. "
                                            "A slot the rule would empty keeps its whole list, and says so."));
    m_unlocked = new QCheckBox(QStringLiteral("Unlocked — keep every slot as I set it"), m_rulesRow);
    m_unlocked->setToolTip(QStringLiteral("Off: a pick the game forbids clears what it conflicts with, and a pick "
                                          "that needs another item fits it. On: nothing changes on its own; the "
                                          "rules are only described in the status line."));
    {
        QSettings st;
        m_compatOnly->setChecked(st.value(m_src.settingsKey() + QStringLiteral("/compatOnly"), true).toBool());
        m_unlocked->setChecked(st.value(m_src.settingsKey() + QStringLiteral("/unlocked"), false).toBool());
    }
    rr->addWidget(m_compatOnly); rr->addWidget(m_unlocked);
    m_rulesRow->setVisible(false);
    pv->addWidget(m_rulesRow);

    m_find = new QLineEdit(panel);
    m_find->setPlaceholderText(QStringLiteral("Find in every slot…"));
    m_find->setClearButtonEnabled(true);
    m_find->setToolTip(QStringLiteral("Counts the matches in each slot; slots with none are dimmed. Each list also filters as you type in it."));
    pv->addWidget(m_find);

    auto* slotBox = new QGroupBox(QStringLiteral("Slots"), panel);
    m_slotLayout = new QVBoxLayout(slotBox);
    m_slotLayout->setContentsMargins(6, 6, 6, 6);
    m_slotHost = slotBox;
    pv->addWidget(slotBox);

    m_lookBox = new QGroupBox(QStringLiteral("Look"), panel);
    m_lookLayout = new QVBoxLayout(m_lookBox);
    m_lookLayout->setContentsMargins(6, 6, 6, 6);
    m_lookBox->setVisible(false);
    pv->addWidget(m_lookBox);

    auto* btnRow = new QHBoxLayout();
    auto* clearBtn = new QPushButton(QStringLiteral("Clear slots"), panel);
    clearBtn->setToolTip(QStringLiteral("Every slot back to none — the game's default look for this subject."));
    auto* randomBtn = new QPushButton(QStringLiteral("Surprise me"), panel);
    randomBtn->setToolTip(QStringLiteral("Fill each slot at random from the options this subject can use."));
    btnRow->addWidget(clearBtn); btnRow->addWidget(randomBtn);
    pv->addLayout(btnRow);

    auto* histRow = new QHBoxLayout();
    m_undoBtn = new QPushButton(QStringLiteral("↶ Undo"), panel);
    m_redoBtn = new QPushButton(QStringLiteral("↷ Redo"), panel);
    m_undoBtn->setToolTip(QStringLiteral("Ctrl+Z")); m_redoBtn->setToolTip(QStringLiteral("Ctrl+Shift+Z"));
    m_undoBtn->setEnabled(false); m_redoBtn->setEnabled(false);
    histRow->addWidget(m_undoBtn); histRow->addWidget(m_redoBtn);
    pv->addLayout(histRow);

    auto* exportBtn = new QPushButton(QStringLiteral("Export…"), panel);
    auto* exMenu = new QMenu(exportBtn);
    populateExportMenu(exMenu);
    exportBtn->setMenu(exMenu);
    pv->addWidget(exportBtn);

    auto* presetBox = new QGroupBox(QStringLiteral("Presets"), panel);
    auto* pl = new QVBoxLayout(presetBox);
    m_presetBox = new QComboBox(presetBox);
    narrowCombo(m_presetBox);
    m_presetBox->setToolTip(QStringLiteral("Saved builds. Selecting one loads it."));
    pl->addWidget(m_presetBox);
    auto* pRow = new QHBoxLayout();
    auto* saveBtn = new QPushButton(QStringLiteral("Save…"), presetBox);
    auto* delBtn = new QPushButton(QStringLiteral("Delete"), presetBox);
    auto* buildBtn = new QPushButton(QStringLiteral("Build ▾"), presetBox);
    auto* bm = new QMenu(buildBtn);
    bm->addAction(QStringLiteral("Copy build as text"), this, [this] { copyBuild(); });
    bm->addAction(QStringLiteral("Paste build from text"), this, [this] { pasteBuild(); });
    buildBtn->setMenu(bm);
    pRow->addWidget(saveBtn); pRow->addWidget(delBtn); pRow->addWidget(buildBtn);
    pl->addLayout(pRow);
    pv->addWidget(presetBox);

    m_statusLine = new QLabel(panel);
    m_statusLine->setWordWrap(true);
    m_statusLine->setStyleSheet(QStringLiteral("color:#9aa;"));
    pv->addWidget(m_statusLine);
    pv->addStretch(1);

    connect(m_categoryBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_syncing) return;
        setCategory(m_categoryBox->currentData().toString());
        if (m_subjects.isEmpty()) { report(QStringLiteral("Customize: nothing to build in this category.")); return; }
        loadSubject(m_subjects.first().id);
        repopulateOptions();
        scheduleAssembly();
    });
    connect(m_subjectBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        const QString id = m_subjectBox->currentData().toString();   // data, not index: captions are rows too
        if (m_syncing || id.isEmpty()) return;
        loadSubject(id);
        repopulateOptions();
        followBase(true);
        scheduleAssembly();
    });
    connect(m_variantBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_syncing || m_variantBox->currentData().toString().isEmpty()) return;
        repopulateOptions();   // a slot's list can depend on the base (FOX: parts that fit this receiver)
        followBase(false);
        scheduleAssembly();
    });
    connect(m_compatOnly, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(m_src.settingsKey() + QStringLiteral("/compatOnly"), on);
        if (m_syncing) return;
        repopulateOptions();
    });
    connect(m_unlocked, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(m_src.settingsKey() + QStringLiteral("/unlocked"), on);
    });
    connect(m_setBox, qOverload<int>(&QComboBox::activated), this, [this](int) {
        const QString id = m_setBox->currentData().toString();
        if (!id.isEmpty()) applySet(id);
    });
    connect(m_find, &QLineEdit::textChanged, this, &CustomizeTab::applyFind);
    connect(clearBtn, &QPushButton::clicked, this, &CustomizeTab::clearSlots);
    connect(randomBtn, &QPushButton::clicked, this, &CustomizeTab::randomize);
    connect(m_undoBtn, &QPushButton::clicked, this, [this] { undo(); });
    connect(m_redoBtn, &QPushButton::clicked, this, [this] { redo(); });
    connect(saveBtn, &QPushButton::clicked, this, &CustomizeTab::savePreset);
    connect(delBtn, &QPushButton::clicked, this, &CustomizeTab::deletePreset);
    connect(m_presetBox, qOverload<int>(&QComboBox::activated), this, [this](int) {
        const QString n = m_presetBox->currentData().toString();
        if (!n.isEmpty()) applyPreset(n);
    });
}

void CustomizeTab::populateExportMenu(QMenu* menu)
{
    menu->addAction(QStringLiteral("Whole build (one file)…"), this, [this] { exportBuild(Full); });
    menu->addAction(QStringLiteral("Whole build, without held items…"), this, [this] { exportBuild(NoHeld); });
    menu->addAction(QStringLiteral("Each piece separately…"), this, [this] { exportBuild(EachPiece); });
    menu->addSeparator();
    menu->addAction(QStringLiteral("Original files (raw, every piece)…"), this, [this] { exportBuild(RawOriginals); });
}

// ── Right column: panels + the anim bar ─────────────────────────────────────────────────────────

void CustomizeTab::buildRightColumn(QWidget* right)
{
    auto* rv = new QVBoxLayout(right); rv->setContentsMargins(0, 0, 0, 0);
    m_panels = new NPanel(m_src.settingsKey() + QStringLiteral("/npanel"), right);
    rv->addWidget(m_panels, 1);

    m_animBar = new QWidget(this);
    auto* ab = new QHBoxLayout(m_animBar); ab->setContentsMargins(0, 0, 0, 0);
    m_clipBox = new QComboBox(m_animBar);
    m_playBtn = new QToolButton(m_animBar); m_playBtn->setIcon(glyph::toolIcon(32)); m_playBtn->setCheckable(true);
    m_playBtn->setToolTip(QStringLiteral("Play / pause"));
    m_timeline = new QSlider(Qt::Horizontal, m_animBar); m_timeline->setRange(0, 1000);
    m_timeLbl = new QLabel(QStringLiteral("0.00 / 0.00s"), m_animBar);
    ab->addWidget(new QLabel(QStringLiteral("Clip:"), m_animBar));
    ab->addWidget(m_clipBox, 1); ab->addWidget(m_playBtn); ab->addWidget(m_timeline, 2); ab->addWidget(m_timeLbl);
    m_animBar->setVisible(false);

    m_partsTable = new QTableWidget(0, 3, m_panels);
    m_partsTable->setHorizontalHeaderLabels({QStringLiteral("Part"), QStringLiteral("Tris"), QStringLiteral("Material")});
    m_partsTable->horizontalHeader()->setStretchLastSection(true);
    m_partsTable->verticalHeader()->setVisible(false);
    m_partsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_partsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_clipList = new QListWidget(m_panels);
    m_clipList->setToolTip(QStringLiteral("The clips the figure's rig carries — click one to play it."));
    m_pieceList = new QListWidget(m_panels);
    m_pieceList->setToolTip(QStringLiteral("The pieces making up the figure — tick to show or hide each one."));
    m_infoPanel = new QLabel(m_panels);
    m_infoPanel->setAlignment(Qt::AlignTop | Qt::AlignLeft); m_infoPanel->setWordWrap(true);
    m_infoPanel->setTextInteractionFlags(Qt::TextSelectableByMouse); m_infoPanel->setMargin(6);
    m_panels->addPanel(QStringLiteral("parts"),  QStringLiteral("PARTS"),       24, m_partsTable, QStringLiteral("Every part of the assembled figure: triangles, material."));
    m_panels->addPanel(QStringLiteral("anims"),  QStringLiteral("ANIMATIONS"),  14, m_clipList,   QStringLiteral("The clips the figure's rig carries."));
    m_panels->addPanel(QStringLiteral("attach"), QStringLiteral("PIECES"),      15, m_pieceList,  QStringLiteral("The pieces making up the figure — tick to show or hide each one."));
    m_panels->addPanel(QStringLiteral("info"),   QStringLiteral("INFO"),        16, m_infoPanel,  QStringLiteral("Subject, body, counts, notes from the assembly."));
    m_panels->restoreState({QStringLiteral("attach"), QStringLiteral("info")});
    m_panels->attachToggle(m_view);

    m_playTimer = new QTimer(this);
    m_playTimer->setInterval(16);
    m_view->setPlaybackTimer(m_playTimer);
    connect(m_playTimer, &QTimer::timeout, this, [this] {
        const float dur = m_view->clipDuration();
        if (dur <= 0) return;
        float t = m_view->time() + 0.016f;
        if (t > dur) t -= dur;
        m_view->setTime(t);
        if (!m_timeline->isSliderDown()) { QSignalBlocker b(m_timeline); m_timeline->setValue(int(t / dur * 1000.0f)); }
        m_timeLbl->setText(QStringLiteral("%1 / %2s").arg(t, 0, 'f', 2).arg(dur, 0, 'f', 2));
    });
    connect(m_partsTable, &QTableWidget::itemSelectionChanged, this, [this] {
        QSet<int> sel;
        for (QTableWidgetItem* it : m_partsTable->selectedItems())
            if (it->column() == 0) sel.insert(it->data(Qt::UserRole).toInt());
        m_view->setSelectedParts(sel);
    });
    connect(m_pieceList, &QListWidget::itemChanged, this, [this](QListWidgetItem* it) {
        const int idx = it->data(Qt::UserRole).toInt();
        if (idx < 0 || idx >= m_assembly.pieces.size()) return;
        m_pieceShown[m_assembly.pieces[idx].label] = it->checkState() == Qt::Checked;
        applyVisibility();
    });
    connect(m_clipList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || !m_clipList->isEnabled()) return;
        { QSignalBlocker b(m_clipBox); m_clipBox->setCurrentIndex(row); }
        setClipIndex(row - 1);
        setPlaying(row > 0);
    });
    connect(m_clipBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int row) {
        if (row < 0) return;
        { QSignalBlocker b(m_clipList); m_clipList->setCurrentRow(row); }
        setClipIndex(row - 1);
        setPlaying(row > 0);
    });
    connect(m_playBtn, &QToolButton::toggled, this, [this](bool on) { setPlaying(on); });
    connect(m_timeline, &QSlider::sliderMoved, this, [this](int v) {
        const float dur = m_view->clipDuration();
        if (dur > 0) { setPlaying(false); m_view->setTime(dur * v / 1000.0f); m_timeLbl->setText(QStringLiteral("%1 / %2s").arg(dur * v / 1000.0f, 0, 'f', 2).arg(dur, 0, 'f', 2)); }
    });
}

// ── Lifecycle ───────────────────────────────────────────────────────────────────────────────────

void CustomizeTab::onIndexReady()
{
    ++m_gen;
    m_cache->clear();   // a new game build can change what lives at an id
    m_src.prepare();
    m_categories = m_src.categories();
    m_allSubjects = m_src.subjects();
    {
        QSignalBlocker b(m_categoryBox);
        m_categoryBox->clear();
        for (const Category& c : m_categories) m_categoryBox->addItem(c.label, c.id);
    }
    m_headForm->setRowVisible(m_categoryBox, m_categories.size() > 1);
    // The category the person was last in, else the first that has anything in it.
    QString cat = QSettings().value(m_src.settingsKey() + QStringLiteral("/category")).toString();
    bool known = false;
    for (const Category& c : m_categories) known = known || c.id == cat;
    if (!known) cat = m_categories.isEmpty() ? QString() : m_categories.first().id;
    setCategory(cat);
    m_undo.clear(); m_redo.clear(); m_haveSettled = false;
    reloadPresets();
    m_ready = true;
    if (m_subjects.isEmpty()) { report(QStringLiteral("Customize: this install has nothing to dress.")); return; }
    loadSubject(m_subjects.first().id);
    repopulateOptions();
    assembleNow();
}

void CustomizeTab::reset()
{
    ++m_gen;
    m_ready = false;
    m_debounce->stop();
    joinWorker();   // the store is about to be re-opened: nothing may still be reading it
    m_inFlight = false;
    ++m_workerSerial;   // its result, still queued, must not clear the flag for a NEWER worker
    m_dirtyWhileBusy = false;
    m_src.reset();
    m_cache = std::make_shared<MeshCache>();   // a fresh cache: a late insert can never reach the new one
    m_pieceShown.clear();
    m_assembly = Assembly();
    setPlaying(false);
    m_view->clearScene();
}

void CustomizeTab::setCategory(const QString& categoryId)
{
    m_category = categoryId;
    {
        QSignalBlocker b(m_categoryBox);
        const int at = m_categoryBox->findData(categoryId);
        if (at >= 0) m_categoryBox->setCurrentIndex(at);
    }
    if (!categoryId.isEmpty()) QSettings().setValue(m_src.settingsKey() + QStringLiteral("/category"), categoryId);
    // A game with no categories lists every subject; otherwise the subjects filed under this one.
    m_subjects.clear();
    for (const Subject& s : m_allSubjects)
        if (m_categories.isEmpty() || s.category == categoryId) m_subjects << s;
    QString noun = m_src.subjectNoun(), variantNoun = m_src.variantNoun();
    for (const Category& c : m_categories)
        if (c.id == categoryId) {
            if (!c.subjectNoun.isEmpty()) noun = c.subjectNoun;
            if (!c.variantNoun.isEmpty()) variantNoun = c.variantNoun;
        }
    m_noun = noun;
    if (auto* lbl = qobject_cast<QLabel*>(m_headForm->labelForField(m_subjectBox))) lbl->setText(noun);
    if (auto* lbl = qobject_cast<QLabel*>(m_headForm->labelForField(m_variantBox))) lbl->setText(variantNoun);
    QSignalBlocker b(m_subjectBox);
    m_subjectBox->clear();
    QString group;
    for (const Subject& s : m_subjects) {
        if (!s.group.isEmpty() && s.group != group) {   // a caption row per group (FOX's grouped list)
            group = s.group;
            m_subjectBox->addItem(QStringLiteral("— %1 —").arg(group), QString());
            if (auto* sm = qobject_cast<QStandardItemModel*>(m_subjectBox->model()))
                sm->item(m_subjectBox->count() - 1)->setFlags(Qt::NoItemFlags);
        }
        m_subjectBox->addItem(s.note.isEmpty() ? s.label : QStringLiteral("%1  (%2)").arg(s.label, s.note), s.id);
    }
}

void CustomizeTab::refreshVariants(const QString& keepId)
{
    Build b = currentBuild();
    b.subject = m_subject;
    m_variants = m_src.variants(m_subject, b);
    QSignalBlocker blk(m_variantBox);
    m_variantBox->clear();
    for (const Option& o : m_variants) {
        m_variantBox->addItem(o.label, o.id);
        if (!o.detail.isEmpty()) m_variantBox->setItemData(m_variantBox->count() - 1, o.detail, Qt::ToolTipRole);
    }
    const int at = keepId.isEmpty() ? 0 : m_variantBox->findData(keepId);
    m_variantBox->setCurrentIndex(at >= 0 ? at : 0);
    m_headForm->setRowVisible(m_variantBox, !m_variants.isEmpty());
}

void CustomizeTab::onSlotPicked(int row)
{
    if (m_syncing || row < 0 || row >= m_rows.size()) return;
    // What the pick changes elsewhere (always), then the game's rules (unless unlocked) — both on a
    // USER pick only: a preset, undo or paste restores what was saved.
    Build b = currentBuild();
    const Build before = b;
    const QStringList follows = m_src.followPick(b, m_rows[row].def.id);
    for (const QString& n : follows) qInfo("customize: %s", qUtf8Printable(n));
    const bool rules = m_src.hasRules(m_subject);
    const bool enforce = rules && !m_unlocked->isChecked();
    QStringList notes;
    if (rules) {
        const Build followed = b;
        notes = m_src.applyRules(b, m_rows[row].def.id, enforce);
        if (!enforce) b = followed;   // unlocked: a rule only says what it would do
        for (const QString& n : notes) qInfo("customize: rule — %s%s", qUtf8Printable(n), enforce ? "" : " (unlocked: not applied)");
    }
    if (!follows.isEmpty() || !notes.isEmpty() || b != before) {
        // The WHOLE result, set the way a build is applied: every slot including the one just
        // picked, the variant and toggles, each list built for the result. It used to skip the
        // picked slot, drop a pick the current list did not offer silently, and ignore variant and
        // toggle edits — while the status line said the rule was applied (audit 2026-09-25).
        QStringList couldNot;
        if (b != before) setControls(b, &couldNot);
        QString msg = (follows + notes).join(QStringLiteral(" · "));
        if (rules && !enforce && !notes.isEmpty()) msg += QStringLiteral("  (unlocked — nothing changed)");
        if (!couldNot.isEmpty()) {
            msg += QStringLiteral("  — could not apply: %1").arg(couldNot.join(QStringLiteral(", ")));
            qWarning("customize: rule result not fully applied — %s", qUtf8Printable(couldNot.join(QStringLiteral(", "))));
        }
        if (!msg.isEmpty()) report(msg);
    }
    if (m_src.optionsDependOnSlots() || m_src.hasCompat(m_subject)) repopulateOptions();
    updateSlotStates();
    scheduleAssembly();
}

void CustomizeTab::followBase(bool subjectChanged)
{
    // A USER's new subject or variant only (a preset, undo or paste restores what was saved).
    Build b = currentBuild();
    const Build before = b;
    const QStringList notes = m_src.followBase(b, subjectChanged);
    for (const QString& n : notes) qInfo("customize: %s", qUtf8Printable(n));
    QStringList couldNot;
    if (b != before) setControls(b, &couldNot);
    if (!couldNot.isEmpty()) qWarning("customize: not applied — %s", qUtf8Printable(couldNot.join(QStringLiteral(", "))));
    if (!notes.isEmpty()) report(notes.join(QStringLiteral(" · ")));
}

void CustomizeTab::loadSubject(const QString& subjectId)
{
    ++m_gen;                   // a result for the previous subject must not frame this one
    m_subject = subjectId;
    m_framedSubject = false;
    m_pieceShown.clear();
    {
        QSignalBlocker b(m_subjectBox);
        const int at = m_subjectBox->findData(subjectId);
        if (at >= 0) m_subjectBox->setCurrentIndex(at);
    }
    m_subjectInfo->setText(m_src.subjectInfo(subjectId));
    refreshVariants(QString());
    {
        const bool rules = m_src.hasRules(subjectId), compat = m_src.hasCompat(subjectId);
        m_unlocked->setVisible(rules);
        m_compatOnly->setVisible(compat);
        m_rulesRow->setVisible(rules || compat);
    }

    // Toggles: rebuilt per subject (a game may offer different ones per subject), state kept by id.
    QHash<QString, bool> keep;
    for (auto it = m_toggles.constBegin(); it != m_toggles.constEnd(); ++it) keep.insert(it.key(), it.value()->isChecked());
    m_toggles.clear();   // the checkboxes are owned by the row: deleted with its items below
    while (QLayoutItem* li = m_toggleRow->takeAt(0)) { delete li->widget(); delete li; }
    m_toggleDefs = m_src.toggles(subjectId);
    for (const Toggle& t : m_toggleDefs) {
        auto* cb = new QCheckBox(t.label, m_toggleHost);
        cb->setToolTip(t.tooltip);
        cb->setChecked(keep.value(t.id, t.defaultOn));
        m_toggleRow->addWidget(cb);
        m_toggles.insert(t.id, cb);
        connect(cb, &QCheckBox::toggled, this, [this] {
            if (m_syncing) return;
            repopulateOptions();
            scheduleAssembly();
        });
    }
    m_toggleRow->addStretch(1);
    m_toggleHost->setVisible(!m_toggleDefs.isEmpty());

    // Sets.
    const QVector<SetDef> sets = m_src.sets(subjectId);
    {
        QSignalBlocker b(m_setBox);
        m_setBox->clear();
        m_setBox->addItem(QStringLiteral("— none —"), QString());
        for (const SetDef& s : sets) m_setBox->addItem(s.label, s.id);
    }
    m_headForm->setRowVisible(m_setRow, !sets.isEmpty());   // the row with its label

    // Slot rows: rebuilt when the subject's slots differ (they usually do not).
    const QVector<SlotDef> defs = m_src.slotDefs(subjectId);
    bool same = defs.size() == m_rows.size();
    for (int i = 0; same && i < defs.size(); ++i) same = defs[i].id == m_rows[i].def.id;
    if (same)   // same ids: keep the widgets, take the new definitions (label, randomize, inSets)
        for (int i = 0; i < defs.size(); ++i) { m_rows[i].def = defs[i]; m_rows[i].label->setText(defs[i].label); }
    if (!same) {
        for (SlotRow& r : m_rows) delete r.combo->parentWidget();
        m_rows.clear();
        bool anyLook = false;
        for (const SlotDef& d : defs) anyLook = anyLook || d.kind == SlotKind::Look;
        m_lookBox->setVisible(anyLook);
        for (const SlotDef& d : defs) {
            SlotRow row;
            row.def = d;
            auto* line = new QWidget(m_slotHost);
            auto* hl = new QHBoxLayout(line); hl->setContentsMargins(0, 0, 0, 0); hl->setSpacing(4);
            row.label = new QLabel(d.label, line);
            row.label->setMinimumWidth(96);
            row.label->setContextMenuPolicy(Qt::CustomContextMenu);
            row.combo = new SearchableCombo(line);
            narrowCombo(row.combo);
            row.exportBtn = new QToolButton(line);
            row.exportBtn->setText(QStringLiteral("⤓"));
            row.exportBtn->setToolTip(QStringLiteral("Export this slot's piece…"));
            row.exportBtn->setAutoRaise(true);
            hl->addWidget(row.label); hl->addWidget(row.combo, 1); hl->addWidget(row.exportBtn);
            const bool look = d.kind == SlotKind::Look;
            row.exportBtn->setVisible(!look);   // a look is not a mesh: nothing of its own to export
            (look ? m_lookLayout : m_slotLayout)->addWidget(line);
            const int ri = m_rows.size();
            m_rows.push_back(row);
            connect(row.combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, ri] { onSlotPicked(ri); });
            connect(row.exportBtn, &QToolButton::clicked, this, [this, ri] {
                showSlotMenu(ri, m_rows[ri].exportBtn->mapToGlobal(m_rows[ri].exportBtn->rect().bottomLeft()));
            });
            connect(row.label, &QLabel::customContextMenuRequested, this, [this, ri](const QPoint& p) {
                showSlotMenu(ri, m_rows[ri].label->mapToGlobal(p));
            });
        }
    }
}

// Every slot's list for the current build; each keeps its selection when it is still offered.
void CustomizeTab::repopulateOptions(const Build* target)
{
    const Build b = target ? *target : currentBuild();
    const bool haveCompat = m_src.hasCompat(m_subject);
    const bool narrow = haveCompat && m_compatOnly->isChecked();
    m_syncing = true;
    for (SlotRow& r : m_rows) {
        const QString keep = target ? target->slot(r.def.id) : r.combo->currentData().toString();
        QVector<Option> all = m_src.options(m_subject, r.def.id, b);
        r.compat = haveCompat ? m_src.compat(m_subject, r.def.id, b) : Compat();
        r.narrowed = false;
        if (narrow && r.compat.state == Compat::Rule && !r.compat.allowed.isEmpty()) {
            // What fits, plus what is already fitted (a pick the rule dislikes is never silently
            // dropped). A rule that would leave nothing keeps the whole list (FOX).
            QVector<Option> fit;
            for (const Option& o : all) if (r.compat.allowed.contains(o.id) || o.id == keep) fit << o;
            if (!fit.isEmpty()) { r.narrowed = fit.size() < all.size(); all = fit; }
        }
        r.options = all;
        QSignalBlocker blk(r.combo);
        r.combo->clear();
        // Row 0 says what an empty slot means: the game's default dress when it has one.
        const QString def = m_src.defaultLabel(m_subject, r.def.id, b);
        r.combo->addItem(!def.isEmpty() ? QStringLiteral("— default — %1").arg(def)
                         : !r.def.noneLabel.isEmpty() ? r.def.noneLabel : QStringLiteral("— none —"), QString());
        if (!def.isEmpty()) r.combo->setItemData(0, QStringLiteral("What the game starts this %1 in.").arg((m_noun.isEmpty() ? m_src.subjectNoun() : m_noun).toLower()), Qt::ToolTipRole);
        auto* model = qobject_cast<QStandardItemModel*>(r.combo->model());
        QString section;
        for (const Option& o : r.options) {
            if (!o.section.isEmpty() && o.section != section) {
                section = o.section;
                r.combo->addItem(section, QString());           // an unselectable caption row
                if (model) model->item(r.combo->count() - 1)->setFlags(Qt::NoItemFlags);
            }
            if (o.swatch.isValid()) {
                QPixmap px(14, 14); px.fill(o.swatch);
                r.combo->addItem(QIcon(px), o.label, o.id);
            } else {
                r.combo->addItem(o.label, o.id);
            }
            if (!o.detail.isEmpty()) r.combo->setItemData(r.combo->count() - 1, o.detail, Qt::ToolTipRole);
        }
        const int at = keep.isEmpty() ? 0 : r.combo->findData(keep);
        r.combo->setCurrentIndex(at >= 0 ? at : 0);
    }
    m_syncing = false;
    updateSlotStates();
    applyFind(m_find->text());
}

void CustomizeTab::updateSlotStates()
{
    const bool haveCompat = m_src.hasCompat(m_subject);
    for (SlotRow& r : m_rows) {
        const bool fitted = !r.combo->currentData().toString().isEmpty();
        const bool unusable = r.compat.unusable;
        const bool usable = !r.options.isEmpty() && !unusable;
        // Fitted in the last assembly but its socket resolved nowhere (FOX's amber "unseated").
        bool unseated = false;
        QString seatNote;
        if (fitted && m_assembledBuild.slot(r.def.id) == r.combo->currentData().toString())
            for (const PieceRange& pr : m_assembly.pieces)
                if (pr.slot == r.def.id && pr.broughtBy.isEmpty()) {
                    if (!pr.seated) unseated = true;
                    if (seatNote.isEmpty()) seatNote = pr.seatNote;
                }
        QFont f = r.label->font();
        f.setBold(fitted);
        f.setStrikeOut(unusable);   // FOX: a slot the game never fills on this base is crossed out
        r.label->setFont(f);
        r.label->setStyleSheet(!usable ? QStringLiteral("color:rgba(160,160,160,90);")
                              : unseated ? QStringLiteral("color:#d8a23a;")
                              : fitted ? QString() : QStringLiteral("color:#8a8a8a;"));
        QString tip = unusable ? QStringLiteral("Not usable here%1").arg(r.compat.why.isEmpty() ? QString() : QStringLiteral(": ") + r.compat.why)
                    : r.options.isEmpty() ? QStringLiteral("Nothing this %1 can use goes here.").arg((m_noun.isEmpty() ? m_src.subjectNoun() : m_noun).toLower())
                    : fitted ? r.combo->currentText() : r.combo->itemText(0);
        // The narrowing's state, said rather than applied silently (FOX's three states). A look row
        // is not a part: no fit rule speaks to it (FOX's camouflage and colour rows, 2e-5a).
        if (haveCompat && !unusable && r.def.kind != SlotKind::Look) {
            if (!m_compatOnly->isChecked()) tip += QStringLiteral("\nFit rule off: every option listed.");
            else if (r.compat.state == Compat::NoRule)
                tip += QStringLiteral("\nNo fit rule for this%1: every option listed.").arg(r.compat.why.isEmpty() ? QString() : QStringLiteral(" (") + r.compat.why + QLatin1Char(')'));
            else if (r.narrowed) tip += QStringLiteral("\nShowing the %1 that fit%2.").arg(r.options.size()).arg(r.compat.why.isEmpty() ? QString() : QStringLiteral(" — ") + r.compat.why);
            else tip += QStringLiteral("\nEverything listed fits.");
        }
        if (!seatNote.isEmpty()) tip += QLatin1Char('\n') + seatNote.left(1).toUpper() + seatNote.mid(1) + QLatin1Char('.');
        else if (unseated) tip += QStringLiteral("\nNot seated: its socket is not on the figure — placed where it was authored (see the notes).");
        r.label->setToolTip(tip);
        r.exportBtn->setEnabled(fitted);
    }
}

void CustomizeTab::applyFind(const QString& text)
{
    const QString t = text.trimmed();
    updateSlotStates();   // the base state first, so a slot that matches again is no longer dimmed
    for (SlotRow& r : m_rows) {
        // Look rows are not searched (FOX): Find is for pieces, and "red" would light every dye row.
        if (t.isEmpty() || r.def.kind == SlotKind::Look) { r.label->setText(r.def.label); continue; }
        int n = 0;
        for (const Option& o : r.options)
            if (o.label.contains(t, Qt::CaseInsensitive) || o.detail.contains(t, Qt::CaseInsensitive)) ++n;
        r.label->setText(QStringLiteral("%1  · %2").arg(r.def.label).arg(n));
        if (n == 0) r.label->setStyleSheet(QStringLiteral("color:rgba(160,160,160,90);"));
    }
}

// ── The build ───────────────────────────────────────────────────────────────────────────────────

Build CustomizeTab::currentBuild() const
{
    Build b;
    if (m_categories.size() > 1) b.category = m_category;
    b.subject = m_subject;
    if (!m_variants.isEmpty()) b.variant = m_variantBox->currentData().toString();
    for (const SlotRow& r : m_rows) {
        const QString id = r.combo->currentData().toString();
        if (!id.isEmpty()) b.picks.insert(r.def.id, id);
    }
    for (auto it = m_toggles.constBegin(); it != m_toggles.constEnd(); ++it)
        b.toggles.insert(it.key(), it.value()->isChecked() ? QStringLiteral("1") : QStringLiteral("0"));
    return b;
}

bool CustomizeTab::applyBuild(const Build& b, bool now, QStringList* missing)
{
    if (!m_ready) return false;
    if (!b.subject.isEmpty() && b.subject != m_subject) {
        // Subjects are looked up across EVERY category: a build pasted from another page switches to it.
        const Subject* hit = nullptr;
        for (const Subject& s : m_allSubjects)
            if (s.id == b.subject && (b.category.isEmpty() || m_categories.isEmpty() || s.category == b.category)) { hit = &s; break; }
        if (!hit) { if (missing) *missing << QStringLiteral("subject %1").arg(b.subject); return false; }
        if (!m_categories.isEmpty() && hit->category != m_category) setCategory(hit->category);
        loadSubject(b.subject);
    }
    setControls(b, missing);
    updateSlotStates();
    if (now) return assembleNow();
    scheduleAssembly();
    return true;
}

void CustomizeTab::setControls(const Build& b, QStringList* missing)
{
    // Absent means default (Build.h): no variant = the subject's first, a toggle not named = its
    // default. They used to keep whatever was on screen (audit 2026-09-25).
    if (!m_variants.isEmpty()) {
        QSignalBlocker blk(m_variantBox);
        int at = b.variant.isEmpty() ? 0 : m_variantBox->findData(b.variant);
        if (at < 0) { if (missing) *missing << QStringLiteral("variant %1").arg(b.variant); at = m_variantBox->currentIndex(); }
        if (at != m_variantBox->currentIndex()) m_variantBox->setCurrentIndex(at);
    }
    m_syncing = true;
    for (const Toggle& t : m_toggleDefs)
        if (QCheckBox* cb = m_toggles.value(t.id)) cb->setChecked(b.toggle(t.id, t.defaultOn));
    m_syncing = false;
    // Every list is built for the build being APPLIED — a slot whose options depend on another's
    // pick (FOX: parts that fit this receiver and barrel) must already offer the saved pick. Built
    // from the build on screen, a preset, undo or paste lost exactly those picks (audit 2026-09-25).
    Build target = currentBuild();
    target.picks = b.picks;
    repopulateOptions(&target);
    m_syncing = true;
    for (SlotRow& r : m_rows) {
        const QString id = b.picks.value(r.def.id);
        int at = 0;
        if (!id.isEmpty()) {
            at = r.combo->findData(id);
            if (at < 0) { if (missing) *missing << QStringLiteral("%1: %2").arg(r.def.id, id); at = 0; }
        }
        r.combo->setCurrentIndex(at);
    }
    m_syncing = false;
    if (m_src.optionsDependOnSlots()) repopulateOptions();
}

void CustomizeTab::clearSlots()
{
    m_syncing = true;
    for (SlotRow& r : m_rows) r.combo->setCurrentIndex(0);
    { QSignalBlocker b(m_setBox); m_setBox->setCurrentIndex(0); }
    m_syncing = false;
    if (m_src.optionsDependOnSlots()) repopulateOptions();
    updateSlotStates();
    scheduleAssembly();
}

void CustomizeTab::randomize()
{
    m_syncing = true;
    for (SlotRow& r : m_rows) {
        // Row 0 (none/default) or any option — never a section caption.
        int at = 0;
        if (r.def.randomize && !r.options.isEmpty()) {
            const int k = int(QRandomGenerator::global()->bounded(int(r.options.size()) + 1)) - 1;
            if (k >= 0) at = r.combo->findData(r.options[k].id);
        }
        r.combo->setCurrentIndex(at >= 0 ? at : 0);
    }
    { QSignalBlocker b(m_setBox); m_setBox->setCurrentIndex(0); }
    m_syncing = false;
    if (m_src.optionsDependOnSlots()) repopulateOptions();
    updateSlotStates();
    scheduleAssembly();
    report(QStringLiteral("Customize: random build"));
}

void CustomizeTab::applySet(const QString& setId)
{
    const QMap<QString, QString> picks = m_src.setPicks(setId, currentBuild());
    m_syncing = true;
    for (SlotRow& r : m_rows) {
        if (!r.def.inSets) continue;
        const QString id = picks.value(r.def.id);
        const int at = id.isEmpty() ? 0 : r.combo->findData(id);
        r.combo->setCurrentIndex(at >= 0 ? at : 0);
    }
    m_syncing = false;
    if (m_src.optionsDependOnSlots()) repopulateOptions();
    updateSlotStates();
    scheduleAssembly();
    report(QStringLiteral("Customize: set ‘%1’").arg(m_setBox->currentText()));
}

QStringList CustomizeTab::listRows(const QString& slotId) const
{
    QStringList out;
    for (const SlotRow& r : m_rows)
        if (r.def.id == slotId)
            for (int i = 0; i < r.combo->count(); ++i) out << r.combo->itemText(i);
    return out;
}

bool CustomizeTab::pickOption(const QString& slotId, const QString& optionId)
{
    for (SlotRow& r : m_rows) {
        if (r.def.id != slotId) continue;
        const int at = optionId.isEmpty() ? 0 : r.combo->findData(optionId);
        if (at < 0) return false;
        r.combo->setCurrentIndex(at);   // not syncing: the same path a click takes (onSlotPicked)
        return true;
    }
    return false;
}

bool CustomizeTab::pickSubject(const QString& subjectId)
{
    const int at = m_subjectBox->findData(subjectId);
    if (at < 0) return false;
    if (at != m_subjectBox->currentIndex()) m_subjectBox->setCurrentIndex(at);   // the path a click takes
    return true;
}

bool CustomizeTab::pickVariant(const QString& variantId)
{
    const int at = m_variantBox->findData(variantId);
    if (at < 0) return false;
    if (at != m_variantBox->currentIndex()) m_variantBox->setCurrentIndex(at);
    return true;
}

void CustomizeTab::setUnlocked(bool on) { m_unlocked->setChecked(on); }
void CustomizeTab::setCompatOnly(bool on) { m_compatOnly->setChecked(on); }

QStringList CustomizeTab::slotIds() const
{
    QStringList out;
    for (const SlotRow& r : m_rows) out << r.def.id;
    return out;
}

QVector<SlotDef> CustomizeTab::slotDefs() const
{
    QVector<SlotDef> out;
    for (const SlotRow& r : m_rows) out << r.def;
    return out;
}

int CustomizeTab::subjectCountIn(const QString& categoryId) const
{
    const QString cat = categoryId.isEmpty() ? m_category : categoryId;
    int n = 0;
    for (const Subject& s : m_allSubjects) if (m_categories.isEmpty() || s.category == cat) ++n;
    return n;
}

QString CustomizeTab::subjectIdAt(const QString& categoryId, int i) const
{
    const QString cat = categoryId.isEmpty() ? m_category : categoryId;
    int n = 0;
    for (const Subject& s : m_allSubjects)
        if ((m_categories.isEmpty() || s.category == cat) && n++ == i) return s.id;
    return {};
}

int CustomizeTab::optionCount(const QString& slotId) const
{
    for (const SlotRow& r : m_rows) if (r.def.id == slotId) return r.options.size();
    return 0;
}

QString CustomizeTab::optionIdAt(const QString& slotId, int i) const
{
    for (const SlotRow& r : m_rows) if (r.def.id == slotId) return i >= 0 && i < r.options.size() ? r.options[i].id : QString();
    return {};
}

// ── Assembly ────────────────────────────────────────────────────────────────────────────────────

void CustomizeTab::scheduleAssembly()
{
    if (!m_ready) return;
    m_debounce->start();
}

void CustomizeTab::startAssembly()
{
    if (!m_ready || !m_store) return;
    if (m_inFlight) { m_dirtyWhileBusy = true; return; }   // one worker at a time; the newest build follows
    const Build b = currentBuild();
    const Resolved r = m_src.resolve(b);
    const quint64 gen = ++m_gen;
    IGameStore* store = m_store;
    std::shared_ptr<MeshCache> cache = m_cache;
    QPointer<CustomizeTab> self(this);
    m_inFlight = true;
    const quint64 serial = ++m_workerSerial;
    auto work = [self, store, cache, r, b, gen, serial] {
        Assembly a = assemble(*store, *cache, r, /*includeHeld*/true, /*wantClips*/true);
        // Posted to the application, and `self` checked THERE: the tab may be gone by then (its
        // destructor joins this thread, so the store it used outlived this call).
        QMetaObject::invokeMethod(qApp, [self, a, b, gen, serial] {
            if (!self) return;
            if (serial != self->m_workerSerial) return;   // a reset since: this worker is not the one in flight
            self->m_inFlight = false;
            if (gen == self->m_gen.load()) self->applyAssembly(a, b);
            if (self->m_dirtyWhileBusy) { self->m_dirtyWhileBusy = false; self->scheduleAssembly(); }
        }, Qt::QueuedConnection);
    };
    joinWorker();   // the previous one has finished (m_inFlight was false); reap it
    if (store->isThreadSafe()) m_worker = std::thread(work);
    else work();
}

bool CustomizeTab::assembleNow()
{
    if (!m_ready || !m_store) return false;
    m_debounce->stop();
    m_dirtyWhileBusy = false;
    ++m_gen;   // anything in flight is now stale
    const Build b = currentBuild();
    const Assembly a = assemble(*m_store, *m_cache, m_src.resolve(b), /*includeHeld*/true, /*wantClips*/true);
    applyAssembly(a, b);
    return a.ok;
}

void CustomizeTab::applyAssembly(const Assembly& a, const Build& b)
{
    // Keep what is playing across the rebuild (D4) — same clip name, same time.
    const int row = m_clipBox->currentIndex();
    const QString keepClip = row > 0 ? m_clipBox->itemText(row) : QString();
    const float keepTime = m_view->time();
    const bool keepPlaying = m_playTimer->isActive();
    setPlaying(false);
    m_view->clearClip();

    m_assembly = a;
    m_assembledBuild = b;
    if (!a.ok) {
        m_view->clearScene();
        refreshPanels();
        refreshAnimBar(QString(), 0, false);
        m_statusLine->setText(a.error);
        report(QStringLiteral("Customize: %1").arg(a.error));
        qWarning("customize: %s", qUtf8Printable(a.error));
        noteSettled(b);   // a build that fails is still where the user is: undo must know it
        return;
    }
    m_view->setScene(a.geo, a.skeleton, m_framedSubject);
    m_view->setMaterials(a.materials);
    if (!m_framedSubject) {
        if (m_src.firstViewYaw() != 0.0f) m_view->setOrbitYaw(m_view->camYaw() + m_src.firstViewYaw());
        m_framedSubject = true;
    }
    applyVisibility();
    refreshPanels();
    refreshAnimBar(keepClip, keepTime, keepPlaying);
    updateSlotStates();   // a slot whose piece found no socket turns amber
    m_statusLine->setText(a.notes.isEmpty() ? QString() : a.notes.join(QLatin1Char('\n')));
    for (const QString& n : a.notes) qInfo("customize: %s", qUtf8Printable(n));
    // Where each hardpoint-seated piece went, one line each: the unseated ones are notes already,
    // and without the seated ones a log cannot show that a chain went on the right host (2e-3).
    for (const PieceRange& pr : a.pieces)
        if (pr.seated && !pr.seatNote.isEmpty()) qInfo("customize: %s — %s", qUtf8Printable(pr.label), qUtf8Printable(pr.seatNote));
    qInfo("customize: assembled %s — %d parts, %d pieces, %d joints, %d clips%s", qUtf8Printable(b.subject),
          int(a.geo.parts.size()), int(a.pieces.size()), a.skeleton.size(), int(a.clips.size()),
          a.replacedBody ? " (body replaced)" : "");
    noteSettled(b);
}

void CustomizeTab::applyVisibility()
{
    if (!m_view->hasGeometry() || !m_assembly.ok) return;
    // The one rule (2c), shared with exportBuildTo. It starts from each part's shipped-and-looked
    // state: before 2c this set every piece part to its tick, which showed parts the game ships
    // hidden while the export left them out.
    const QSet<int> hidden = m_assembly.replacedBody ? QSet<int>() : m_src.hiddenBodyParts(m_assembledBuild, m_assembly.bodyGeo);
    const QVector<bool> on = partsShown(m_assembly, hidden, m_pieceShown);
    for (int i = 0; i < on.size(); ++i) m_view->setPartVisible(i, on[i]);
}

void CustomizeTab::refreshPanels()
{
    const Geometry& g = m_assembly.geo;
    {
        QSignalBlocker b(m_partsTable);
        m_partsTable->setRowCount(g.parts.size());
        for (int i = 0; i < g.parts.size(); ++i) {
            const Part& p = g.parts[i];
            auto* n = new QTableWidgetItem(p.name);
            n->setData(Qt::UserRole, i);
            m_partsTable->setItem(i, 0, n);
            m_partsTable->setItem(i, 1, new QTableWidgetItem(QString::number(p.triangleCount())));
            const QString mat = p.materialIndex >= 0 && p.materialIndex < g.materialNames.size() ? QFileInfo(g.materialNames[p.materialIndex]).fileName() : QString();
            m_partsTable->setItem(i, 2, new QTableWidgetItem(mat));
        }
        if (QLabel* t = m_panels->titleLabel(QStringLiteral("parts")))
            t->setText(g.parts.isEmpty() ? QStringLiteral("PARTS") : QStringLiteral("PARTS · %1").arg(g.parts.size()));
    }
    {
        QSignalBlocker b(m_pieceList);
        m_pieceList->clear();
        for (int i = 0; i < m_assembly.pieces.size(); ++i) {
            const PieceRange& r = m_assembly.pieces[i];
            auto* it = new QListWidgetItem(r.broughtBy.isEmpty() ? r.label : QStringLiteral("%1  (with %2)").arg(r.label, r.broughtBy), m_pieceList);
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(m_pieceShown.value(r.label, true) ? Qt::Checked : Qt::Unchecked);
            it->setData(Qt::UserRole, i);
            it->setToolTip(!r.broughtBy.isEmpty() ? QStringLiteral("Brought by %1's look").arg(r.broughtBy) : r.slot.isEmpty() ? r.tag : r.slot);
        }
        if (m_assembly.pieces.isEmpty())
            (new QListWidgetItem(m_assembly.replacedBody ? QStringLiteral("(a form that replaces the body)") : QStringLiteral("(the body alone — no pieces)"), m_pieceList))->setFlags(Qt::ItemIsEnabled);
        if (QLabel* t = m_panels->titleLabel(QStringLiteral("attach")))
            t->setText(m_assembly.pieces.isEmpty() ? QStringLiteral("PIECES") : QStringLiteral("PIECES · %1").arg(m_assembly.pieces.size()));
    }
    {
        QStringList lines;
        QString label = m_subject;
        for (const Subject& s : m_subjects) if (s.id == m_subject) { label = s.label; break; }
        lines << QStringLiteral("<b>%1</b>").arg(label.toHtmlEscaped());
        if (!m_assembly.ok) lines << m_assembly.error.toHtmlEscaped();
        else {
            lines << QStringLiteral("%1 part(s) · %2 verts · %3 tris").arg(g.parts.size()).arg(g.vertices.size()).arg(g.triangleCount());
            lines << QStringLiteral("%1 joints · %2 clip(s)").arg(m_assembly.skeleton.size()).arg(m_assembly.clips.size());
            if (m_assembly.replacedBody) lines << QStringLiteral("a form replaces the body");
            for (const QString& n : m_assembly.notes) lines << QStringLiteral("• %1").arg(n.toHtmlEscaped());
        }
        m_infoPanel->setText(lines.join(QStringLiteral("<br>")));
    }
}

void CustomizeTab::refreshAnimBar(const QString& keepClip, float keepTime, bool keepPlaying)
{
    int restore = 0;
    {
        QSignalBlocker b1(m_clipList), b2(m_clipBox);
        m_clipList->clear(); m_clipBox->clear();
        const bool have = !m_assembly.clips.isEmpty();
        m_animBar->setVisible(have);
        m_clipList->setEnabled(have);
        m_clipList->addItem(QStringLiteral("(bind pose)"));
        m_clipBox->addItem(QStringLiteral("(bind pose)"));
        for (int i = 0; i < m_assembly.clips.size(); ++i) {
            const QString n = m_assembly.clips[i].name.isEmpty() ? QStringLiteral("clip") : m_assembly.clips[i].name;
            m_clipList->addItem(n); m_clipBox->addItem(n);
            if (!keepClip.isEmpty() && n == keepClip && restore == 0) restore = i + 1;
        }
        if (!have) m_clipList->addItem(QStringLiteral("(no clips on this rig)"));
        m_clipList->setCurrentRow(restore);
        m_clipBox->setCurrentIndex(restore);
        if (QLabel* t = m_panels->titleLabel(QStringLiteral("anims")))
            t->setText(have ? QStringLiteral("ANIMATIONS · %1").arg(m_assembly.clips.size()) : QStringLiteral("ANIMATIONS"));
    }
    if (restore > 0) {
        setClipIndex(restore - 1);
        m_view->setTime(qMin(keepTime, m_view->clipDuration()));
        setPlaying(keepPlaying);
    }
}

void CustomizeTab::setClipIndex(int i)
{
    if (i < 0 || i >= m_assembly.clips.size()) { m_view->clearClip(); setPlaying(false); return; }
    if (m_assembly.clips[i].lazy && m_store) {
        // A store that lists clips lazily (FOX) decodes one on demand — as the Models tab does.
        QString err;
        Clip c = m_assembly.clips[i];
        if (m_store->bakeClip(m_assembly.figure, i, c, &err)) m_assembly.clips[i] = c;
        else { qWarning("customize: clip %s could not be decoded — %s", qUtf8Printable(c.name), qUtf8Printable(err)); m_view->clearClip(); setPlaying(false); return; }
    }
    m_view->setClip(m_assembly.clips[i]);
    m_view->setTime(0.0f);
    m_timeline->setValue(0);
}

void CustomizeTab::setPlaying(bool on)
{
    if (on && !m_view->hasClip()) on = false;
    if (on) m_playTimer->start(); else m_playTimer->stop();
    { QSignalBlocker b(m_playBtn); m_playBtn->setChecked(on); }
    m_playBtn->setIcon(glyph::toolIcon(on ? 33 : 32));
}

// ── Undo (diffs of settled builds) ──────────────────────────────────────────────────────────────

void CustomizeTab::noteSettled(const Build& b)
{
    if (m_haveSettled && b != m_settled && !m_applyingHistory) {
        m_undo.append(m_settled);
        while (m_undo.size() > kUndoDepth) m_undo.removeFirst();
        m_redo.clear();
    }
    m_settled = b;
    m_haveSettled = true;
    m_undoBtn->setEnabled(!m_undo.isEmpty());
    m_redoBtn->setEnabled(!m_redo.isEmpty());
}

bool CustomizeTab::undo()
{
    // A change still debouncing or assembling is undone first — one step, not two.
    if (m_haveSettled && currentBuild() != m_settled) {
        m_redo.append(currentBuild());
        m_applyingHistory = true;
        applyBuild(m_settled, true);
        m_applyingHistory = false;
    } else {
        if (m_undo.isEmpty()) return false;
        const Build prev = m_settled;          // captured first: applying settles the target
        const Build b = m_undo.last();
        m_applyingHistory = true;
        const bool ok = applyBuild(b, true);
        m_applyingHistory = false;
        // ok, or it assembled with an error but is now the settled build (still a step back).
        if (!ok && m_settled != b) { report(QStringLiteral("Undo: that build is not in this install.")); return false; }
        m_undo.removeLast();
        m_redo.append(prev);
    }
    m_undoBtn->setEnabled(!m_undo.isEmpty());
    m_redoBtn->setEnabled(!m_redo.isEmpty());
    return true;
}

bool CustomizeTab::redo()
{
    if (m_redo.isEmpty()) return false;
    const Build prev = m_settled;
    const Build b = m_redo.last();
    m_applyingHistory = true;
    const bool ok = applyBuild(b, true);
    m_applyingHistory = false;
    if (!ok && m_settled != b) { report(QStringLiteral("Redo: that build is not in this install.")); return false; }
    m_redo.removeLast();
    m_undo.append(prev);
    while (m_undo.size() > kUndoDepth) m_undo.removeFirst();
    m_undoBtn->setEnabled(!m_undo.isEmpty());
    m_redoBtn->setEnabled(!m_redo.isEmpty());
    return true;
}

// ── Presets (build texts under the game's settings key) and the clipboard ───────────────────────

QMap<QString, QString> CustomizeTab::presets() const
{
    QMap<QString, QString> out;
    const QJsonObject o = QJsonDocument::fromJson(QSettings().value(m_src.settingsKey() + QStringLiteral("/builds")).toString().toUtf8()).object();
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) out.insert(it.key(), it.value().toString());
    return out;
}

void CustomizeTab::storePresets(const QMap<QString, QString>& p)
{
    QJsonObject o;
    for (auto it = p.constBegin(); it != p.constEnd(); ++it) o.insert(it.key(), it.value());
    QSettings().setValue(m_src.settingsKey() + QStringLiteral("/builds"), QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
}

void CustomizeTab::reloadPresets()
{
    QMap<QString, QString> p = presets();
    // One-time import of the game's older presets (its own key, left in place).
    if (!QSettings().contains(m_src.settingsKey() + QStringLiteral("/builds"))) {
        const QMap<QString, Build> legacy = m_src.legacyPresets();
        for (auto it = legacy.constBegin(); it != legacy.constEnd(); ++it) p.insert(it.key(), it.value().toText());
        if (!legacy.isEmpty()) { storePresets(p); qInfo("customize: imported %d preset(s) from the old format", int(legacy.size())); }
    }
    QSignalBlocker b(m_presetBox);
    const QString keep = m_presetBox->currentData().toString();
    m_presetBox->clear();
    m_presetBox->addItem(QStringLiteral("— saved builds —"), QString());
    for (auto it = p.constBegin(); it != p.constEnd(); ++it) m_presetBox->addItem(it.key(), it.key());
    const int at = m_presetBox->findData(keep);
    m_presetBox->setCurrentIndex(at >= 0 ? at : 0);
}

void CustomizeTab::savePreset()
{
    if (!m_ready) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Save build"), QStringLiteral("Name:"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    QMap<QString, QString> p = presets();
    p.insert(name, currentBuild().toText());
    storePresets(p);
    reloadPresets();
    { QSignalBlocker b(m_presetBox); const int at = m_presetBox->findData(name); if (at >= 0) m_presetBox->setCurrentIndex(at); }
    report(QStringLiteral("Saved build ‘%1’").arg(name));
}

void CustomizeTab::deletePreset()
{
    const QString name = m_presetBox->currentData().toString();
    if (name.isEmpty()) return;
    QMap<QString, QString> p = presets();
    p.remove(name);
    storePresets(p);
    reloadPresets();
    report(QStringLiteral("Deleted build ‘%1’").arg(name));
}

void CustomizeTab::applyPreset(const QString& name)
{
    Build b;
    if (!Build::fromText(presets().value(name), &b)) { report(QStringLiteral("Build ‘%1’ is empty").arg(name)); return; }
    QStringList missing;
    bool knownSubject = b.subject.isEmpty();
    // Every category's subjects (m_allSubjects): a build saved on another page switches to it in
    // applyBuild. Checking only this page's list put a character's picks on a rifle (audit 2026-09-25).
    for (const Subject& s : m_allSubjects) if (s.id == b.subject) knownSubject = true;
    if (!knownSubject) { missing << QStringLiteral("subject %1 (applied to %2)").arg(b.subject, m_subject); b.subject = m_subject; }
    applyBuild(b, false, &missing);
    report(missing.isEmpty() ? QStringLiteral("Loaded build ‘%1’").arg(name)
                             : QStringLiteral("Loaded build ‘%1’ — not in this install: %2").arg(name, missing.join(QStringLiteral(", "))));
}

void CustomizeTab::copyBuild()
{
    QApplication::clipboard()->setText(currentBuild().toText());
    report(QStringLiteral("Build copied as text."));
}

void CustomizeTab::pasteBuild()
{
    Build b;
    QStringList unknown, missing;
    if (!Build::fromText(QApplication::clipboard()->text(), &b, &unknown)) { report(QStringLiteral("The clipboard holds no build.")); return; }
    applyBuild(b, false, &missing);
    missing += unknown;
    report(missing.isEmpty() ? QStringLiteral("Build pasted.") : QStringLiteral("Build pasted — skipped: %1").arg(missing.join(QStringLiteral(", "))));
}

// ── Export: the assembled build through core::glb; pieces through model::exportOne ──────────────

QString CustomizeTab::exportStem() const
{
    QString s = m_subject.toLower();
    for (QChar& c : s) if (!c.isLetterOrNumber()) c = QLatin1Char('_');
    return s.isEmpty() ? QStringLiteral("character") : s;
}

QString CustomizeTab::exportBuildTo(const QString& path, ExportMode mode, QString* error)
{
    if (!m_ready || !m_store) { if (error) *error = QStringLiteral("nothing loaded"); return {}; }
    const Assembly a = assemble(*m_store, *m_cache, m_src.resolve(currentBuild()), /*includeHeld*/mode != NoHeld, /*wantClips*/false);
    if (!a.ok) { if (error) *error = a.error; return {}; }
    // Hidden in the viewport = not written: the same rule as applyVisibility (partsShown — looks,
    // body parts the game hides, pieces unticked), applied to this export's own assembly.
    Geometry geo = a.geo;
    const QSet<int> hidden = a.replacedBody ? QSet<int>() : m_src.hiddenBodyParts(currentBuild(), a.bodyGeo);
    const QVector<bool> on = partsShown(a, hidden, m_pieceShown);
    for (int i = 0; i < on.size() && i < geo.parts.size(); ++i) geo.parts[i].visible = on[i];
    ExportOptions opt = settings::exportOptions(m_plugin.exportDefaults());
    opt.includeAnimations = false;      // an assembled build is written in its bind pose
    opt.includeAttachments = false;     // already assembled
    glb::Scene scene;
    scene.geometry = &geo;
    scene.skeleton = a.skeleton.isEmpty() ? nullptr : &a.skeleton;
    scene.materials = a.materials;
    glb::Result res;
    if (!glb::write(scene, opt, path, &res)) { if (error) *error = res.error.isEmpty() ? QStringLiteral("write failed") : res.error; return {}; }
    qInfo("customize: exported %s — %s", qUtf8Printable(QFileInfo(path).fileName()), qUtf8Printable(res.summary()));
    return path;
}

void CustomizeTab::exportSelectionToLast()
{
    if (!hasExportSelection()) { report(QStringLiteral("Nothing assembled to export.")); return; }
    const QString dir = settings::lastExportDir();
    if (dir.isEmpty() || !QFileInfo::exists(dir)) { exportBuild(Full); return; }
    const ExportOptions opt = settings::exportOptions(m_plugin.exportDefaults());
    const QString ext = opt.gltfSeparate ? QStringLiteral(".gltf") : QStringLiteral(".glb");
    QString err;
    const QString out = exportBuildTo(model::uniquePath(dir, exportStem() + QStringLiteral("_build"), ext), Full, &err);
    if (out.isEmpty()) { report(QStringLiteral("Export failed: %1").arg(err)); return; }
    ExportNotifier::instance().notify(QStringLiteral("Exported %1").arg(QFileInfo(out).fileName()), dir, out);
}

int CustomizeTab::extractOriginals(const QVector<AssetId>& ids, const QString& dir, int* failed)
{
    int files = 0, fail = 0;
    QSet<QString> written;
    for (const AssetId& id : ids) {
        if (!id.isValid()) { ++fail; continue; }
        const QStringList deps = m_store->dependencyFiles(id);
        if (deps.isEmpty()) { ++fail; continue; }
        for (const QString& gp : deps) {
            if (written.contains(gp)) continue;
            written.insert(gp);
            const QByteArray bytes = m_store->readPath(gp, nullptr);
            if (bytes.isEmpty()) { ++fail; continue; }
            const QString outPath = model::mirrorPath(dir, gp);
            QDir().mkpath(QFileInfo(outPath).absolutePath());
            QFile f(outPath);
            if (f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size()) ++files; else ++fail;
        }
    }
    if (failed) *failed = fail;
    return files;
}

QVector<Piece> CustomizeTab::shownPieces() const
{
    // What is on screen: a form that replaces the body alone; otherwise the body and every piece
    // that is ticked in PIECES, each with its look.
    const Resolved r = m_src.resolve(currentBuild());
    QVector<Piece> shown;
    for (const Piece& p : r.pieces) if (p.placement == Placement::ReplaceBody) shown << p;
    if (!shown.isEmpty()) return shown;
    if (r.body.isValid()) { Piece b; b.mesh = r.body; b.label = QStringLiteral("body"); b.look = r.bodyLook; shown << b; }
    // A model a look brings is left out when that mesh is already fitted — assemble()'s rule, so a
    // piece is never written twice.
    QSet<AssetId> fitted{r.body};
    for (const Piece& p : r.pieces) if (p.broughtBy.isEmpty()) fitted.insert(p.mesh);
    for (const Piece& p : r.pieces) {
        if (!m_pieceShown.value(p.label, true)) continue;
        if (!p.broughtBy.isEmpty()) { if (fitted.contains(p.mesh)) continue; fitted.insert(p.mesh); }
        shown << p;
    }
    return shown;
}

Piece CustomizeTab::slotPiece(const QString& slotId) const
{
    // Resolved from a build holding only this slot's pick — the full build can hide it (a
    // shapeshift form replaces the body and everything worn on it) — plus every look row, which is
    // what the piece looks like, not another piece.
    const Build cur = currentBuild();
    // The piece the FULL build fits first: a game can swap a slot's model for another slot's sake
    // (FOX: the covered hairstyle while headgear is worn, CTW.cpp:3722), and the export must be
    // the model the figure shows. The one-slot build below is for a piece the full build hides.
    for (const Piece& p : m_src.resolve(cur).pieces) if (p.slot == slotId && p.broughtBy.isEmpty()) return p;
    Build one = cur;
    one.picks = {{slotId, cur.slot(slotId)}};
    for (const SlotRow& sr : m_rows)
        if (sr.def.kind == SlotKind::Look && !cur.slot(sr.def.id).isEmpty()) one.picks.insert(sr.def.id, cur.slot(sr.def.id));
    for (const Piece& p : m_src.resolve(one).pieces) if (p.slot == slotId && p.broughtBy.isEmpty()) return p;
    return {};
}

model::ExportResult CustomizeTab::exportPiece(const AssetId& id, const QString& label, const Look& look,
                                             const ExportOptions& opt, const QString& out, const QSet<int>& hideParts)
{
    // The piece as it looked on the figure: its look applied the way assemble() applies it, the
    // parts the figure hides left out, and none of the store's own attachments — the figure never
    // showed them (exportBuildTo turns them off too; audit 2026-09-25).
    ExportOptions o = opt;
    o.includeAttachments = false;
    QStringList notes;
    const model::ExportResult er = model::exportOne(*m_store, id, o, out, {}, {},
        [&](model::Loaded& m, bool decode) {
            applyLook(*m_store, id, m.geo, m.skeleton, m.materials, look, decode, label, &notes);
            for (int i : hideParts) if (i >= 0 && i < m.geo.parts.size()) m.geo.parts[i].visible = false;
        });
    for (const QString& n : notes) qInfo("customize: %s", qUtf8Printable(n));
    return er;
}

void CustomizeTab::exportBuild(ExportMode mode)
{
    if (!m_ready || !m_assembly.ok) { report(QStringLiteral("Nothing assembled to export.")); return; }
    const ExportOptions opt = settings::exportOptions(m_plugin.exportDefaults());
    const QString ext = opt.gltfSeparate ? QStringLiteral(".gltf") : QStringLiteral(".glb");
    const QString base = settings::lastExportDir().isEmpty() ? QDir::homePath() : settings::lastExportDir();
    const QVector<Piece> shown = shownPieces();
    QVector<AssetId> ids;
    for (const Piece& s : shown) if (!ids.contains(s.mesh)) ids << s.mesh;

    if (mode == RawOriginals) {
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Extract original files to folder"), base);
        if (dir.isEmpty()) return;
        settings::setLastExportDir(dir);
        int fail = 0;
        const int files = extractOriginals(ids, dir, &fail);
        ExportNotifier::instance().notify(QStringLiteral("Extracted %1 original file%2%3").arg(files)
                                          .arg(files == 1 ? QString() : QStringLiteral("s"))
                                          .arg(fail ? QStringLiteral(" (%1 failed)").arg(fail) : QString()), dir);
        return;
    }
    if (mode == EachPiece) {
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Export each piece to folder"), base);
        if (dir.isEmpty()) return;
        settings::setLastExportDir(dir);
        ExportOptions one = opt;
        one.includeAnimations = false;
        int ok = 0, fail = 0;
        QString last;
        auto writeOne = [&](const Piece& sh) {
            QString stem = sh.label;
            for (QChar& c : stem) if (!c.isLetterOrNumber() && c != QLatin1Char('_')) c = QLatin1Char('_');
            const QString out = model::uniquePath(dir, exportStem() + QLatin1Char('_') + stem, ext);
            // The body: without the parts the game hides on it (the one visibility rule).
            const QSet<int> hide = (sh.mesh == m_assembly.figure && !m_assembly.replacedBody && m_assembly.ok)
                ? m_src.hiddenBodyParts(currentBuild(), m_assembly.bodyGeo) : QSet<int>();
            const model::ExportResult er = exportPiece(sh.mesh, sh.label, sh.look, one, out, hide);
            if (er.ok) { ++ok; last = out; } else { ++fail; qWarning("customize: %s — %s", qUtf8Printable(sh.label), qUtf8Printable(er.error)); }
        };
        for (const Piece& sh : shown) writeOne(sh);
        ExportNotifier::instance().notify(QStringLiteral("Exported %1 piece%2 to %3%4").arg(ok).arg(ok == 1 ? QString() : QStringLiteral("s"))
                                          .arg(QDir(dir).dirName()).arg(fail ? QStringLiteral(" (%1 failed)").arg(fail) : QString()), dir, last);
        return;
    }
    const QString suffix = mode == Full ? QStringLiteral("build") : QStringLiteral("build_noheld");
    const QString filter = opt.gltfSeparate ? QStringLiteral("glTF (*.gltf)") : QStringLiteral("glTF binary (*.glb)");
    QString out = QFileDialog::getSaveFileName(this, QStringLiteral("Export build"), QDir(base).filePath(QStringLiteral("%1_%2%3").arg(exportStem(), suffix, ext)), filter);
    if (out.isEmpty()) return;
    if (!out.toLower().endsWith(ext)) out += ext;
    settings::setLastExportDir(QFileInfo(out).absolutePath());
    QString err;
    if (exportBuildTo(out, mode, &err).isEmpty()) { report(QStringLiteral("Export failed: %1").arg(err)); return; }
    ExportNotifier::instance().notify(QStringLiteral("Exported %1").arg(QFileInfo(out).fileName()), QFileInfo(out).absolutePath(), out);
}

void CustomizeTab::showSlotMenu(int row, const QPoint& globalPos)
{
    if (row < 0 || row >= m_rows.size()) return;
    const SlotRow& r = m_rows[row];
    if (r.def.kind == SlotKind::Look) return;   // a look has no mesh of its own
    const bool fitted = !r.combo->currentData().toString().isEmpty();
    QMenu menu(this);
    QAction* conv = menu.addAction(QStringLiteral("Export %1 piece (model)…").arg(r.def.label));
    QAction* raw  = menu.addAction(QStringLiteral("Export %1 original files (raw)…").arg(r.def.label));
    conv->setEnabled(fitted); raw->setEnabled(fitted);
    if (!fitted) { menu.addSeparator(); menu.addAction(QStringLiteral("(nothing in this slot)"))->setEnabled(false); }
    connect(conv, &QAction::triggered, this, [this, row] { exportSlot(row, false); });
    connect(raw,  &QAction::triggered, this, [this, row] { exportSlot(row, true); });
    menu.exec(globalPos);
}

void CustomizeTab::exportSlot(int row, bool raw)
{
    if (row < 0 || row >= m_rows.size()) return;
    const QString slotId = m_rows[row].def.id;
    // Resolve a build holding only this slot's pick: the full build can hide it (a shapeshift form
    // replaces the body and everything worn on it).
    const Piece piece = slotPiece(slotId);
    const AssetId id = piece.mesh;
    if (!id.isValid()) { report(QStringLiteral("That slot has nothing to export.")); return; }
    const QString base = settings::lastExportDir().isEmpty() ? QDir::homePath() : settings::lastExportDir();
    if (raw) {
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Extract original files to folder"), base);
        if (dir.isEmpty()) return;
        settings::setLastExportDir(dir);
        int fail = 0;
        const int files = extractOriginals({id}, dir, &fail);
        ExportNotifier::instance().notify(QStringLiteral("Extracted %1 original file%2%3").arg(files)
                                          .arg(files == 1 ? QString() : QStringLiteral("s"))
                                          .arg(fail ? QStringLiteral(" (%1 failed)").arg(fail) : QString()), dir);
        return;
    }
    ExportOptions opt = settings::exportOptions(m_plugin.exportDefaults());
    opt.includeAnimations = false;
    const QString ext = opt.gltfSeparate ? QStringLiteral(".gltf") : QStringLiteral(".glb");
    const QString filter = opt.gltfSeparate ? QStringLiteral("glTF (*.gltf)") : QStringLiteral("glTF binary (*.glb)");
    QString out = QFileDialog::getSaveFileName(this, QStringLiteral("Export %1 piece").arg(m_rows[row].def.label),
                                               QDir(base).filePath(QStringLiteral("%1_%2%3").arg(exportStem(), slotId, ext)), filter);
    if (out.isEmpty()) return;
    if (!out.toLower().endsWith(ext)) out += ext;
    settings::setLastExportDir(QFileInfo(out).absolutePath());
    const model::ExportResult er = exportPiece(id, piece.label, piece.look, opt, out);
    if (!er.ok) { report(QStringLiteral("Export failed: %1").arg(er.error)); return; }
    ExportNotifier::instance().notify(QStringLiteral("Exported %1").arg(QFileInfo(out).fileName()), QFileInfo(out).absolutePath(), out);
}

}  // namespace core
