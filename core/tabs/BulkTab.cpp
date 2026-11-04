#include "core/tabs/BulkTab.h"
#include "core/app/AppPlugin.h"
#include "core/app/CoreSettings.h"
#include "core/app/ExportNotifier.h"
#include "core/app/StatusLine.h"
#include "core/tabs/BulkExtractor.h"
#include "core/ui/AssetListModel.h"
#include "core/ui/FunnelFilter.h"
#include "core/ui/SearchBox.h"
#include "core/ui/SearchQuery.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableView>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

namespace core {

namespace {
const char* kSearchKey = "bulk/searchHistory";
const char* kFacetsKey = "facets/bulk";
const char* kFacetsAnyKey = "facets/bulkMatchAny";
}  // namespace

BulkTab::BulkTab(AppPlugin& plugin, const QStringList& modelKinds, const QStringList& textureKinds, QWidget* parent)
    : BrowserTab(parent), m_plugin(plugin), m_modelKinds(modelKinds), m_textureKinds(textureKinds)
{
    m_model = new AssetListModel(this);
    auto* outer = new QVBoxLayout(this);

    auto* filterRow = new QHBoxLayout();
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(QStringLiteral("Filter  (space = AND,  -exclude,  a|b = OR,  #tag,  an id)"));
    m_search->setToolTip(searchTooltip());
    searchbox::attach(m_search, QLatin1String(kSearchKey));
    m_kindFacet = new QComboBox(this);
    m_kindFacet->addItem(QStringLiteral("models + textures"), QString());
    m_funnel = new FunnelFilter(this);
    filterRow->addWidget(m_search, 1); filterRow->addWidget(m_kindFacet); filterRow->addWidget(m_funnel);
    outer->addLayout(filterRow);
    outer->addWidget(m_funnel->chipBar());

    m_list = new QTableView(this);
    m_list->setModel(m_model);
    m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_list->verticalHeader()->setVisible(false);
    m_list->horizontalHeader()->setStretchLastSection(true);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setColumnWidth(0, 420);
    m_list->setSortingEnabled(true);
    outer->addWidget(m_list, 1);
    m_count = new QLabel(this); outer->addWidget(m_count);

    auto* controls = new QGroupBox(QStringLiteral("Run"), this);
    auto* cv = new QVBoxLayout(controls);
    auto* dirRow = new QHBoxLayout();
    dirRow->addWidget(new QLabel(QStringLiteral("Output folder:")));
    m_outDir = new QLineEdit(settings::bulkOutDir(), this);
    auto* browse = new QPushButton(QStringLiteral("Browse…"), this);
    dirRow->addWidget(m_outDir, 1); dirRow->addWidget(browse);
    cv->addLayout(dirRow);

    auto* optRow = new QHBoxLayout();
    optRow->addWidget(new QLabel(QStringLiteral("Layout:")));
    m_layout = new QComboBox(this);
    for (const layout::Mode& m : m_plugin.layoutSpec().modes) { m_layout->addItem(m.label, m.id); m_layout->setItemData(m_layout->count() - 1, m.hint, Qt::ToolTipRole); }
    optRow->addWidget(m_layout);
    m_onlyNew = new QCheckBox(QStringLiteral("Only new (skip already exported)"), this); m_onlyNew->setChecked(true);
    m_doModels = new QCheckBox(QStringLiteral("Models → glTF"), this); m_doModels->setChecked(true);
    m_doTextures = new QCheckBox(QStringLiteral("Textures → .png"), this); m_doTextures->setChecked(true);
    m_rawOriginals = new QCheckBox(QStringLiteral("Raw originals (exact files, unmodified)"), this);
    m_rawOriginals->setToolTip(QStringLiteral("Extract the pristine authored files (a model pulls its dependencies), byte-for-byte, in their\n"
                                              "original game folder structure — instead of converting to glTF/PNG."));
    connect(m_rawOriginals, &QCheckBox::toggled, this, [this](bool on) { m_doModels->setEnabled(!on); m_doTextures->setEnabled(!on); m_layout->setEnabled(!on); });
    optRow->addSpacing(12); optRow->addWidget(m_onlyNew);
    optRow->addSpacing(12); optRow->addWidget(m_doModels); optRow->addWidget(m_doTextures);
    optRow->addSpacing(12); optRow->addWidget(m_rawOriginals);
    optRow->addStretch(1);
    optRow->addWidget(new QLabel(QStringLiteral("Threads:")));
    m_workers = new QSpinBox(this);
    m_workers->setRange(0, 32);
    m_workers->setSpecialValueText(QStringLiteral("Auto"));
    m_workers->setValue(settings::bulkWorkers());
    m_workers->setToolTip(QStringLiteral("Parallel extraction threads. Auto uses one per CPU core."));
    connect(m_workers, qOverload<int>(&QSpinBox::valueChanged), this, [](int n) { settings::setBulkWorkers(n); });
    optRow->addWidget(m_workers);
    cv->addLayout(optRow);

    auto* btnRow = new QHBoxLayout();
    m_run = new QPushButton(QStringLiteral("Extract matches"), this);
    m_pause = new QPushButton(QStringLiteral("Pause"), this); m_pause->setEnabled(false);
    m_cancel = new QPushButton(QStringLiteral("Cancel"), this); m_cancel->setEnabled(false);
    m_progress = new QProgressBar(this); m_progress->setTextVisible(true);
    btnRow->addWidget(m_run); btnRow->addWidget(m_pause); btnRow->addWidget(m_cancel); btnRow->addWidget(m_progress, 1);
    cv->addLayout(btnRow);
    m_eta = new QLabel(this); cv->addWidget(m_eta);
    m_console = new QPlainTextEdit(this); m_console->setReadOnly(true); m_console->setMaximumBlockCount(2000); m_console->setMinimumHeight(120);
    cv->addWidget(m_console);
    outer->addWidget(controls);

    // The layout choice is a STABLE STRING (findData; unknown → Flat), shared with Settings.
    { const QString mode = layout::resolveMode(m_plugin.layoutSpec(), settings::value(QStringLiteral("export/folderLayout")).toString());
      const int i = m_layout->findData(mode); m_layout->setCurrentIndex(i < 0 ? 0 : i); }

    m_debounce = new QTimer(this); m_debounce->setSingleShot(true); m_debounce->setInterval(180);
    connect(m_debounce, &QTimer::timeout, this, &BulkTab::applyFilter);
    connect(m_search, &QLineEdit::textChanged, this, [this] { m_debounce->start(); });
    connect(m_kindFacet, qOverload<int>(&QComboBox::currentIndexChanged), this, &BulkTab::applyFilter);
    m_funnel->setActive(QSettings().value(QLatin1String(kFacetsKey)).toStringList(), QSettings().value(QLatin1String(kFacetsAnyKey)).toBool());
    connect(m_funnel, &FunnelFilter::changed, this, [this] {
        QSettings().setValue(QLatin1String(kFacetsKey), m_funnel->activeIds());
        QSettings().setValue(QLatin1String(kFacetsAnyKey), m_funnel->matchAny());
        applyFilter();
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose output folder"), m_outDir->text().isEmpty() ? QDir::homePath() : m_outDir->text());
        if (!d.isEmpty()) { m_outDir->setText(d); settings::setBulkOutDir(d); }
    });
    connect(m_layout, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        const QString mode = m_layout->currentData().toString();
        if (mode.isEmpty()) QSettings().remove(QStringLiteral("export/folderLayout")); else settings::set(QStringLiteral("export/folderLayout"), mode);
    });
    connect(m_run, &QPushButton::clicked, this, &BulkTab::startRun);
    connect(m_pause, &QPushButton::clicked, this, &BulkTab::togglePause);
    connect(m_cancel, &QPushButton::clicked, this, &BulkTab::cancelRun);
}

BulkTab::~BulkTab()
{
    // Cancel is honoured between items, so the item in hand finishes first — a large model can
    // take longer than any fixed bound. The first cut waited 3 s and then destroyed a RUNNING
    // QThread (Qt treats that as fatal) with the worker still inside the store (audit 2026-09-25).
    if (m_worker) { m_worker->requestCancel(); m_worker->setPaused(false); }
    if (m_thread) {
        m_thread->quit();
        if (!m_thread->wait(3000)) {
            qInfo("bulk: closing — waiting for the item being exported to finish");
            m_thread->wait();
        }
    }
}

void BulkTab::onIndexReady()
{
    m_model->setSource(m_store, m_modelKinds + m_textureKinds);
    applyGroups();
    {
        QSignalBlocker b(m_kindFacet);
        const QString keep = m_kindFacet->currentData().toString();
        m_kindFacet->clear();
        m_kindFacet->addItem(QStringLiteral("models + textures"), QString());
        for (const QString& k : m_model->rowKinds()) m_kindFacet->addItem(k, k);
        const int i = m_kindFacet->findData(keep);
        m_kindFacet->setCurrentIndex(i < 0 ? 0 : i);
    }
    applyFilter();
}

// Re-run when the secondary indexes land too: a store whose tag vocabulary comes from one (D4's
// AppearanceMeta) has no groups at "index ready", and Bulk's funnel stayed empty for it.
void BulkTab::applyGroups()
{
    QVector<TagGroup> groups;
    // Only the groups EVERY listed kind shares: a facet that classifies models alone would silently
    // hide every texture (POE2 dropped its Shader group from Bulk for exactly this reason).
    QVector<TagGroup> first;
    bool have = false;
    for (const QString& k : m_modelKinds + m_textureKinds) {
        const QVector<TagGroup> g = m_store ? m_store->tagGroups(k) : QVector<TagGroup>();
        if (!have) { first = g; have = true; continue; }
        QVector<TagGroup> keep;
        for (const TagGroup& a : first) for (const TagGroup& b : g) if (a.id == b.id) { keep << a; break; }
        first = keep;
    }
    groups = first;
    m_model->setTagGroups(groups, /*flagsInEveryKind=*/true);
    m_funnel->setGroups(m_model->tagGroups());
    m_funnel->setVisible(!m_model->tagGroups().isEmpty());
}

void BulkTab::onSecondaryReady() { m_model->refreshFromStore(); applyGroups(); applyFilter(); }

void BulkTab::reset()
{
    m_model->setSource(nullptr, m_modelKinds + m_textureKinds);
    applyFilter();
}

void BulkTab::persistView() {}

void BulkTab::applyFilter()
{
    m_model->applyFilters(m_search->text(), m_kindFacet->currentData().toString(), m_funnel->activeIds(), m_funnel->matchAny());
    m_funnel->setCounts(m_model->facetCounts());
    m_count->setText(QStringLiteral("%1 of %2 assets match — this is what will be extracted").arg(m_model->rowCount()).arg(m_model->totalInBaseSet()));
}

void BulkTab::setQuery(const QString& q) { m_search->setText(q); m_debounce->stop(); applyFilter(); }
void BulkTab::setOutDir(const QString& dir) { m_outDir->setText(dir); }
void BulkTab::setRawOriginals(bool on) { m_rawOriginals->setChecked(on); }
void BulkTab::setOnlyNew(bool on) { m_onlyNew->setChecked(on); }
void BulkTab::setLayoutMode(const QString& id) { const int i = m_layout->findData(id); if (i >= 0) m_layout->setCurrentIndex(i); }

void BulkTab::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape && m_worker) { cancelRun(); e->accept(); return; }
    BrowserTab::keyPressEvent(e);
}

void BulkTab::startRun()
{
    if (m_worker) return;
    if (!m_store || !m_store->isOpen()) { StatusLine::instance().report(this, QStringLiteral("Game data not loaded yet.")); return; }
    const QString out = m_outDir->text().trimmed();
    if (out.isEmpty()) { StatusLine::instance().report(this, QStringLiteral("Choose an output folder first.")); return; }
    settings::setBulkOutDir(out);

    // Snapshot the matched set now (the count you see is the set you export, §14), and compute the
    // layout groups HERE — the plugin's tag lookups are GUI-thread-only.
    QVector<layout::Item> items;
    QVector<BulkItem> bulk;
    items.reserve(m_model->rowCount());
    for (int r = 0; r < m_model->rowCount(); ++r) {
        const AssetRow& row = m_model->rowAt(r);
        layout::Item it; it.id = row.id; it.name = row.name; it.sourcePath = row.name.contains(QLatin1Char('/')) ? row.name : QString();
        items.push_back(it);
        BulkItem b; b.item = it;
        b.storeKind = m_model->storeKindAt(r);
        b.isModel = m_modelKinds.contains(b.storeKind);
        b.isTexture = !b.isModel && m_textureKinds.contains(b.storeKind);
        bulk.push_back(b);
    }
    if (bulk.isEmpty()) { StatusLine::instance().report(this, QStringLiteral("No matches to extract.")); return; }
    bool fellBack = false;
    const QVector<layout::Group> groups = layout::group(m_plugin.layoutSpec(), m_layout->currentData().toString(), items, &fellBack);
    QHash<QString, QString> folderOf;   // "scheme:value" → folder
    for (const layout::Group& g : groups) for (const layout::Item& it : g.items) folderOf.insert(QStringLiteral("%1:%2").arg(it.id.scheme).arg(it.id.value), g.folder);
    for (BulkItem& b : bulk) b.folder = folderOf.value(QStringLiteral("%1:%2").arg(b.item.id.scheme).arg(b.item.id.value));

    BulkExtractor::Options opt;
    opt.outDir = out;
    opt.onlyNew = m_onlyNew->isChecked();
    opt.rawOriginals = m_rawOriginals->isChecked();
    opt.models = opt.rawOriginals ? true : m_doModels->isChecked();
    opt.textures = opt.rawOriginals ? true : m_doTextures->isChecked();
    opt.workers = m_workers->value();
    opt.glb = settings::exportOptions(m_plugin.exportDefaults());
    opt.modelNameTemplate = settings::value(QStringLiteral("export/modelNameTemplate"), QStringLiteral("{{FileName}}")).toString();
    opt.textureNameTemplate = settings::value(QStringLiteral("export/textureNameTemplate"), QStringLiteral("{{FileName}}")).toString();

    m_console->clear();
    m_console->appendPlainText(QStringLiteral("Extracting %1 assets → %2 (%3%4)").arg(bulk.size()).arg(QDir::toNativeSeparators(out))
                               .arg(m_layout->currentText()).arg(fellBack ? QStringLiteral(" — tags not ready, Flat") : QString()));
    m_progress->setRange(0, bulk.size()); m_progress->setValue(0);
    qInfo("bulk: run started — %d item(s), %d group(s), out %s", int(bulk.size()), int(groups.size()), qUtf8Printable(out));

    m_thread = new QThread(this);
    m_worker = new BulkExtractor(m_store, std::move(bulk), opt);
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::started, m_worker, &BulkExtractor::run);
    connect(m_worker, &BulkExtractor::progress, this, [this](int done, int total, int e, int s, int f, const QString& cur, int eta) {
        m_progress->setMaximum(total); m_progress->setValue(done);
        m_progress->setFormat(QStringLiteral("%1/%2  (%3 ok, %4 skip, %5 fail)").arg(done).arg(total).arg(e).arg(s).arg(f));
        m_eta->setText(eta >= 0 ? QStringLiteral("~%1s remaining · %2").arg(eta).arg(cur) : cur);
    });
    connect(m_worker, &BulkExtractor::message, this, [this](const QString& line) { m_console->appendPlainText(line); });
    connect(m_worker, &BulkExtractor::finished, this, [this, out](int e, int s, int f, const QString& summary) {
        m_console->appendPlainText(summary);
        qInfo("bulk: %s", qUtf8Printable(summary));
        ExportNotifier::instance().notify(QStringLiteral("Bulk: %1 exported, %2 skipped, %3 failed").arg(e).arg(s).arg(f), out);
        m_thread->quit(); m_thread->wait();
        m_worker->deleteLater(); m_worker = nullptr;
        m_thread->deleteLater(); m_thread = nullptr;
        setRunning(false);
        emit runFinished(e, s, f);
    });
    setRunning(true);
    m_thread->start();
}

void BulkTab::cancelRun()
{
    if (m_worker) { m_worker->requestCancel(); m_worker->setPaused(false); m_console->appendPlainText(QStringLiteral("Cancelling…")); }
}

void BulkTab::togglePause()
{
    if (!m_worker) return;
    m_paused = !m_paused;
    m_worker->setPaused(m_paused);
    m_pause->setText(m_paused ? QStringLiteral("Resume") : QStringLiteral("Pause"));
}

void BulkTab::setRunning(bool running)
{
    m_run->setEnabled(!running);
    m_pause->setEnabled(running);
    m_cancel->setEnabled(running);
    m_search->setEnabled(!running);
    m_kindFacet->setEnabled(!running);
    m_outDir->setEnabled(!running);
    m_layout->setEnabled(!running && !m_rawOriginals->isChecked());
    m_workers->setEnabled(!running);
    if (!running) { m_paused = false; m_pause->setText(QStringLiteral("Pause")); }
}

}  // namespace core
