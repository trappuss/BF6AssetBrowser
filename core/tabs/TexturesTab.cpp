#include "core/tabs/TexturesTab.h"
#include "core/app/AppPlugin.h"
#include "core/app/CoreSettings.h"
#include "core/app/ExportNotifier.h"
#include "core/app/SehGuard.h"
#include "core/app/StatusLine.h"
#include "core/gl/GLTextureWidget.h"
#include "core/tabs/ModelLoad.h"
#include "core/ui/AssetListModel.h"
#include "core/ui/CardDelegate.h"
#include "core/ui/FunnelFilter.h"
#include "core/ui/Glyphs.h"
#include "core/ui/HoverPreview.h"
#include "core/ui/MenuContext.h"
#include "core/ui/MenuDump.h"
#include "core/ui/MenuText.h"
#include "core/ui/PanelPersist.h"
#include "core/ui/SearchBox.h"
#include "core/ui/SearchQuery.h"
#include "core/ui/ThumbnailCache.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace core {

namespace {
const char* kSearchKey = "textures/searchHistory";
const char* kFacetsKey = "facets/textures";
const char* kFacetsAnyKey = "facets/texturesMatchAny";
const char* kGridKey = "textures/grid";
const char* kIconPxKey = "textures/iconPx";
}  // namespace

TexturesTab::TexturesTab(AppPlugin& plugin, const QStringList& kinds, QWidget* parent)
    : BrowserTab(parent), m_plugin(plugin), m_kinds(kinds)
{
    m_model = new AssetListModel(this);

    auto* left = new QWidget(this);
    auto* lv = new QVBoxLayout(left); lv->setContentsMargins(4, 4, 4, 4);
    auto* searchRow = new QHBoxLayout();
    m_search = new QLineEdit(left);
    m_search->setPlaceholderText(QStringLiteral("Search textures  (space = AND,  -exclude,  a|b = OR,  #tag)"));
    m_search->setToolTip(searchTooltip());
    searchbox::attach(m_search, QLatin1String(kSearchKey));
    m_kindFacet = new QComboBox(left);
    m_kindFacet->addItem(QStringLiteral("all textures"), QString());
    m_funnel = new FunnelFilter(left);
    m_gridBtn = new QToolButton(left);
    m_gridBtn->setIcon(glyph::toolIcon(17));
    m_gridBtn->setCheckable(true);
    m_gridBtn->setToolTip(QStringLiteral("Toggle icon grid / detail list  (Ctrl+scroll to resize icons)"));
    searchRow->addWidget(m_search, 1); searchRow->addWidget(m_kindFacet); searchRow->addWidget(m_funnel); searchRow->addWidget(m_gridBtn);
    lv->addLayout(searchRow);
    lv->addWidget(m_funnel->chipBar());
    m_list = new QTableView(left);
    m_list->setModel(m_model);
    m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->verticalHeader()->setVisible(false);
    m_list->horizontalHeader()->setStretchLastSection(true);
    m_list->setColumnWidth(AssetListModel::ColName, 330);
    m_list->setColumnWidth(AssetListModel::ColInGameName, 190);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setSortingEnabled(true);
    m_list->horizontalHeader()->setSortIndicatorShown(true);
    lv->addWidget(m_list, 1);
    m_grid = new QListView(left);
    m_grid->setModel(m_model);
    m_grid->setModelColumn(AssetListModel::ColName);
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setItemDelegate(new CardDelegate(m_grid));   // same card tiles as the Models grid
    m_grid->setUniformItemSizes(true);
    m_grid->setWordWrap(false);
    m_grid->setSpacing(6);
    m_grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_grid->setSelectionModel(m_list->selectionModel());
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    m_grid->setMouseTracking(true);
    m_grid->viewport()->installEventFilter(this);
    m_grid->hide();
    lv->addWidget(m_grid, 1);
    m_count = new QLabel(left); lv->addWidget(m_count);

    // Decode thumbnails off the GUI thread (loadTexture is in the store's thread-safe set).
    m_thumbs = new ThumbnailCache([this](const AssetId& id) -> QImage {
        if (!m_store) return QImage();
        // Guarded like the model loader: one malformed texture must report, not take the app down.
        QImage img;
        seh::runGuarded("thumbnail", [&] { QString err; img = m_store->loadTexture(id, nullptr, &err); });
        return img.isNull() ? QImage() : img.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }, /*background*/true, this);
    connect(m_thumbs, &ThumbnailCache::ready, this, [this](const AssetId& id) { m_model->refreshIcon(id); hover::Preview::instance().refresh(); });

    auto* right = new QWidget(this);
    auto* rv = new QVBoxLayout(right); rv->setContentsMargins(4, 4, 4, 4);
    auto* tool = new QHBoxLayout();
    m_channel = new QComboBox(right);
    m_channel->addItems({QStringLiteral("RGB"), QStringLiteral("R"), QStringLiteral("G"), QStringLiteral("B"), QStringLiteral("A")});
    m_checker = new QCheckBox(QStringLiteral("Alpha checker"), right);
    tool->addWidget(new QLabel(QStringLiteral("Channel:"))); tool->addWidget(m_channel);
    tool->addWidget(m_checker); tool->addStretch(1);
    auto* save = new QPushButton(QStringLiteral("Save PNG…"), right);
    tool->addWidget(save);
    rv->addLayout(tool);
    m_view = new gl::GLTextureWidget(right);
    m_view->setMinimumSize(360, 360);
    rv->addWidget(m_view, 1);
    m_info = new QLabel(right);
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rv->addWidget(m_info);

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(left); split->addWidget(right); split->setSizes({440, 700});
    PanelPersist::bind(split, QStringLiteral("textures/splitter"));
    auto* outer = new QVBoxLayout(this); outer->setContentsMargins(0, 0, 0, 0); outer->addWidget(split);

    m_debounce = new QTimer(this); m_debounce->setSingleShot(true); m_debounce->setInterval(180);
    connect(m_debounce, &QTimer::timeout, this, &TexturesTab::applyFilter);
    connect(m_search, &QLineEdit::textChanged, this, [this] { m_debounce->start(); });
    connect(m_kindFacet, qOverload<int>(&QComboBox::currentIndexChanged), this, &TexturesTab::applyFilter);
    m_funnel->setActive(QSettings().value(QLatin1String(kFacetsKey)).toStringList(), QSettings().value(QLatin1String(kFacetsAnyKey)).toBool());
    connect(m_funnel, &FunnelFilter::changed, this, [this] {
        QSettings().setValue(QLatin1String(kFacetsKey), m_funnel->activeIds());
        QSettings().setValue(QLatin1String(kFacetsAnyKey), m_funnel->matchAny());
        applyFilter();
    });
    connect(m_list, &QTableView::activated, this, [this](const QModelIndex& idx) { if (idx.isValid()) loadTexture(m_model->idAt(idx.row())); });
    connect(m_grid, &QListView::activated, this, [this](const QModelIndex& idx) { if (idx.isValid()) loadTexture(m_model->idAt(idx.row())); });
    m_loadDebounce = new QTimer(this); m_loadDebounce->setSingleShot(true); m_loadDebounce->setInterval(110);
    connect(m_loadDebounce, &QTimer::timeout, this, &TexturesTab::loadCurrentRow);
    connect(m_list->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& cur, const QModelIndex&) { if (cur.isValid()) m_loadDebounce->start(); });
    connect(m_gridBtn, &QToolButton::toggled, this, [this](bool on) { setGridView(on); });
    connect(m_list, &QTableView::customContextMenuRequested, this, [this](const QPoint& p) { showRowContextMenu(p, m_list); });
    connect(m_grid, &QListView::customContextMenuRequested, this, [this](const QPoint& p) { showRowContextMenu(p, m_grid); });
    connect(m_channel, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) { m_view->setChannel(gl::GLTextureWidget::Channel(i)); });
    connect(m_checker, &QCheckBox::toggled, this, [this](bool on) { m_view->setCheckerboard(on); });
    connect(m_view, &gl::GLTextureWidget::hoverPixel, this, [this](QPoint px, QColor c) {
        if (px.x() < 0) return;
        StatusLine::instance().report(this, QStringLiteral("(%1, %2)  RGBA %3 %4 %5 %6").arg(px.x()).arg(px.y()).arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha()));
    });
    connect(save, &QPushButton::clicked, this, [this] { if (m_currentId.isValid()) savePng({m_currentId}, false); });
    m_iconPx = qBound(48, QSettings().value(QLatin1String(kIconPxKey), 128).toInt(), 320);
    if (QSettings().value(QLatin1String(kGridKey), false).toBool()) m_gridBtn->setChecked(true);
}

void TexturesTab::onIndexReady()
{
    m_model->setSource(m_store, m_kinds);
    QVector<TagGroup> groups;
    for (const QString& k : m_kinds) groups += m_store ? m_store->tagGroups(k) : QVector<TagGroup>();
    m_model->setTagGroups(groups);
    m_funnel->setGroups(m_model->tagGroups());
    m_funnel->setVisible(!m_model->tagGroups().isEmpty());
    {
        QSignalBlocker b(m_kindFacet);
        const QString keep = m_kindFacet->currentData().toString();
        m_kindFacet->clear();
        m_kindFacet->addItem(QStringLiteral("all textures"), QString());
        for (const QString& k : m_model->rowKinds()) m_kindFacet->addItem(k, k);
        const int i = m_kindFacet->findData(keep);
        m_kindFacet->setCurrentIndex(i < 0 ? 0 : i);
        m_kindFacet->setVisible(m_kindFacet->count() > 2);
    }
    m_thumbs->clear();
    applyFilter();
    qInfo("textures: %d row(s) in %s", m_model->totalInBaseSet(), qUtf8Printable(m_kinds.join(QStringLiteral("+"))));
}

void TexturesTab::onSecondaryReady()
{
    m_model->refreshFromStore();
    QVector<TagGroup> groups;
    for (const QString& k : m_kinds) groups += m_store ? m_store->tagGroups(k) : QVector<TagGroup>();
    m_model->setTagGroups(groups);
    m_funnel->setGroups(m_model->tagGroups());
    m_funnel->setVisible(!m_model->tagGroups().isEmpty());
    applyFilter();
}

void TexturesTab::reset()
{
    m_thumbs->clear();
    m_currentId = AssetId();
    m_currentImage = QImage();
    m_view->clearImage();
    m_model->setSource(nullptr, m_kinds);
    applyFilter();
}

void TexturesTab::persistView()
{
    QSettings().setValue(QLatin1String(kGridKey), m_gridBtn->isChecked());
    QSettings().setValue(QLatin1String(kIconPxKey), m_iconPx);
}

void TexturesTab::applyFilter()
{
    m_model->applyFilters(m_search->text(), m_kindFacet->currentData().toString(), m_funnel->activeIds(), m_funnel->matchAny());
    m_funnel->setCounts(m_model->facetCounts());
    m_count->setText(QStringLiteral("%1 of %2 textures").arg(m_model->rowCount()).arg(m_model->totalInBaseSet()));
}

void TexturesTab::loadCurrentRow()
{
    const QModelIndex cur = m_list->selectionModel()->currentIndex();
    if (!cur.isValid()) return;
    const AssetId id = m_model->idAt(cur.row());
    if (!id.isValid() || id == m_currentId) return;
    loadTexture(id);
}

bool TexturesTab::selectTexture(const QString& searchText)
{
    if (!m_store || !m_store->isOpen()) return false;
    m_search->setText(searchText);
    m_debounce->stop();
    applyFilter();
    if (m_model->rowCount() == 0) return false;
    const int row = m_model->bestRowFor(searchText);
    m_list->selectRow(row);
    loadTexture(m_model->idAt(row));
    return m_currentId == m_model->idAt(row);
}

void TexturesTab::showTexture(const AssetId& id)
{
    const int row = m_model->rowOfId(id);
    if (row >= 0) m_list->selectRow(row);
    loadTexture(id);
}

TexturesTab::~TexturesTab()
{
    // The thumbnail jobs read m_store: retire them while this tab is still whole (the cache is a
    // child, so it would otherwise be deleted only after this object's members are gone).
    if (m_thumbs) m_thumbs->clear();
}

void TexturesTab::loadTexture(const AssetId& id)
{
    if (!id.isValid() || !m_store || !m_store->isOpen()) return;
    QString err; TextureInfo info;
    QImage img;
    seh::HardwareFault fault;
    if (!seh::runGuarded("texture", [&] { img = m_store->loadTexture(id, &info, &err); }, &fault))
        err = QStringLiteral("hardware fault while decoding (%1)").arg(fault.what);
    // Current only when it decoded: a failed decode used to leave it "current", so the harness's
    // selectTexture reported success and clicking the row again never retried (audit 2026-09-25).
    m_currentId = img.isNull() ? AssetId() : id;
    if (img.isNull()) {
        StatusLine::instance().report(this, QStringLiteral("decode failed: %1").arg(err));
        m_info->setText(err);
        m_currentImage = QImage();
        m_view->clearImage();
        return;
    }
    m_currentImage = img;
    m_view->setImage(img);
    const QString name = stemOf(id);
    m_info->setText(QStringLiteral("%1   %2×%3   %4   mips %5%6").arg(name).arg(info.width ? info.width : img.width()).arg(info.height ? info.height : img.height())
                        .arg(info.format).arg(info.mips).arg(info.note.isEmpty() ? QString() : QStringLiteral("   %1").arg(info.note)));
    qInfo("textures: loaded %s — %dx%d %s", qUtf8Printable(name), img.width(), img.height(), qUtf8Printable(info.format));
    emit textureLoaded(id);
}

QString TexturesTab::stemOf(const AssetId& id) const
{
    const int row = m_model->rowOfId(id);
    return row >= 0 ? QFileInfo(m_model->rowAt(row).name).completeBaseName() : QString::number(id.value);
}

QVector<AssetId> TexturesTab::selectedIds() const
{
    QVector<AssetId> ids;
    if (m_list->selectionModel())
        for (const QModelIndex& idx : m_list->selectionModel()->selectedRows()) {
            const AssetId id = m_model->idAt(idx.row());
            if (id.isValid() && !ids.contains(id)) ids << id;
        }
    if (ids.isEmpty() && m_currentId.isValid()) ids << m_currentId;
    return ids;
}

bool TexturesTab::hasExportSelection() const { return m_store && m_store->isOpen() && !selectedIds().isEmpty(); }

QString TexturesTab::exportNoun() const
{
    const QVector<AssetId> ids = selectedIds();
    if (ids.size() == 1) return stemOf(ids[0]) + QStringLiteral(" (PNG)");
    return ids.isEmpty() ? QStringLiteral("texture") : QStringLiteral("%1 textures (PNG)").arg(ids.size());
}

void TexturesTab::exportSelection() { savePng(selectedIds(), false); }
void TexturesTab::exportSelectionToLast() { savePng(selectedIds(), true); }

void TexturesTab::savePng(const QVector<AssetId>& ids, bool toLast)
{
    if (ids.isEmpty() || !m_store) { StatusLine::instance().report(this, QStringLiteral("No texture selected.")); return; }
    const QString base = settings::lastExportDir().isEmpty() ? QDir::homePath() : settings::lastExportDir();
    QString dir;
    if (ids.size() == 1 && !toLast) {
        QString out = QFileDialog::getSaveFileName(this, QStringLiteral("Save PNG"), QDir(base).filePath(stemOf(ids[0]) + QStringLiteral(".png")), QStringLiteral("PNG (*.png)"));
        if (out.isEmpty()) return;
        if (!out.toLower().endsWith(QStringLiteral(".png"))) out += QStringLiteral(".png");
        settings::setLastExportDir(QFileInfo(out).absolutePath());
        QString err;
        const QImage img = m_store->loadTexture(ids[0], nullptr, &err);
        if (img.isNull()) { StatusLine::instance().report(this, QStringLiteral("Couldn't decode the texture: %1").arg(err)); return; }
        if (img.save(out, "PNG")) ExportNotifier::instance().notify(QStringLiteral("Saved %1 (%2×%3)").arg(QFileInfo(out).fileName()).arg(img.width()).arg(img.height()), QFileInfo(out).absolutePath(), out);
        else StatusLine::instance().report(this, QStringLiteral("Failed to save %1").arg(QFileInfo(out).fileName()));
        return;
    }
    if (toLast) {
        dir = settings::lastExportDir();
        if (dir.isEmpty() || !QDir(dir).exists()) { StatusLine::instance().report(this, QStringLiteral("No export folder remembered yet — export once the long way.")); return; }
    } else {
        dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Save %1 textures as PNG to folder").arg(ids.size()), base);
        if (dir.isEmpty()) return;
        settings::setLastExportDir(dir);
    }
    int ok = 0, fail = 0; QString lastFile;
    for (const AssetId& id : ids) {
        const QImage img = m_store->loadTexture(id, nullptr, nullptr);
        const QString out = model::uniquePath(dir, stemOf(id), QStringLiteral(".png"));
        if (!img.isNull() && img.save(out, "PNG")) { ++ok; lastFile = out; } else ++fail;
    }
    ExportNotifier::instance().notify(ok == 1 && ids.size() == 1 ? QStringLiteral("Saved %1").arg(QFileInfo(lastFile).fileName())
                                      : QStringLiteral("Saved %1 PNG%2 to %3%4").arg(ok).arg(ok == 1 ? QString() : QStringLiteral("s")).arg(QDir(dir).dirName()).arg(fail ? QStringLiteral(" (%1 failed)").arg(fail) : QString()),
                                      dir, ids.size() == 1 ? lastFile : QString());
}

// The EXACT original bytes (and any sidecars the store lists as dependencies) — no decode, no convert.
void TexturesTab::extractOriginals(const QVector<AssetId>& ids)
{
    if (!m_store || ids.isEmpty()) return;
    if (ids.size() == 1) { saveOriginal(ids.first()); return; }   // one file → the named save dialog
    const QString base = settings::lastExportDir().isEmpty() ? QDir::homePath() : settings::lastExportDir();
    const QString dir = QFileDialog::getExistingDirectory(this,
        QStringLiteral("Extract %1 original files to folder").arg(ids.size()), base);
    if (dir.isEmpty()) return;
    settings::setLastExportDir(dir);
    int ok = 0, fail = 0, extra = 0;
    for (const AssetId& id : ids) {
        const int row = m_model->rowOfId(id);
        const QString gamePath = row >= 0 ? m_model->rowAt(row).name : QString::number(id.value);
        QString err;
        const QByteArray bytes = m_store->read(id, QString(), &err);
        if (bytes.isEmpty()) { ++fail; continue; }
        const QString out = model::uniquePath(dir, QFileInfo(gamePath).completeBaseName(),
                                              QStringLiteral(".%1").arg(QFileInfo(gamePath).suffix()));
        QFile f(out);
        if (f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size()) { ++ok; } else { ++fail; continue; }
        for (const QString& dep : m_store->dependencyFiles(id)) {
            if (dep.compare(gamePath, Qt::CaseInsensitive) == 0) continue;
            const QByteArray d = m_store->readPath(dep, nullptr);
            if (d.isEmpty()) continue;
            QFile sf(QDir(dir).filePath(QFileInfo(dep).fileName()));
            if (sf.open(QIODevice::WriteOnly) && sf.write(d) == d.size()) ++extra;
        }
    }
    ExportNotifier::instance().notify(
        QStringLiteral("Extracted %1 original file%2%3%4").arg(ok).arg(ok == 1 ? QString() : QStringLiteral("s"))
            .arg(extra ? QStringLiteral(" (+%1 sidecar)").arg(extra) : QString())
            .arg(fail ? QStringLiteral(" — %1 failed").arg(fail) : QString()),
        dir, QString());
}

void TexturesTab::saveOriginal(const AssetId& id)
{
    if (!m_store) return;
    const int row = m_model->rowOfId(id);
    if (row < 0) return;
    const QString gamePath = m_model->rowAt(row).name;
    QString err;
    const QByteArray bytes = m_store->read(id, QString(), &err);
    if (bytes.isEmpty()) { StatusLine::instance().report(this, QStringLiteral("Couldn't read the original texture: %1").arg(err)); return; }
    const QString base = settings::lastExportDir().isEmpty() ? QDir::homePath() : settings::lastExportDir();
    const QString fileName = QFileInfo(gamePath).fileName();
    QString out = QFileDialog::getSaveFileName(this, QStringLiteral("Save original texture"), QDir(base).filePath(fileName),
                                               QStringLiteral("Original (*.%1)").arg(QFileInfo(fileName).suffix()));
    if (out.isEmpty()) return;
    settings::setLastExportDir(QFileInfo(out).absolutePath());
    QFile f(out);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size()) { StatusLine::instance().report(this, QStringLiteral("Failed to write %1").arg(QFileInfo(out).fileName())); return; }
    f.close();
    // Sidecars (POE2's .dds.header carries the real dimensions): whatever the store lists beyond the file itself.
    int extra = 0;
    for (const QString& dep : m_store->dependencyFiles(id)) {
        if (dep.compare(gamePath, Qt::CaseInsensitive) == 0) continue;
        const QByteArray d = m_store->readPath(dep, nullptr);
        if (d.isEmpty()) continue;
        QFile sf(QDir(QFileInfo(out).absolutePath()).filePath(QFileInfo(dep).fileName()));
        if (sf.open(QIODevice::WriteOnly) && sf.write(d) == d.size()) ++extra;
    }
    ExportNotifier::instance().notify(QStringLiteral("Saved original %1%2").arg(QFileInfo(out).fileName(), extra ? QStringLiteral(" (+ %1 sidecar)").arg(extra) : QString()),
                                      QFileInfo(out).absolutePath(), out);
}

void TexturesTab::populateExportMenu(QMenu* menu)
{
    const QVector<AssetId> ids = selectedIds();
    QAction* orig = menu->addAction(MenuText::prompts(ids.size() > 1 ? QStringLiteral("Extract %1 original files (raw)").arg(ids.size())
                                                                      : QStringLiteral("Save original file")),
                                    this, [this, ids] { extractOriginals(ids); });
    orig->setEnabled(ids.size() == 1);
    if (ids.size() > 1) orig->setToolTip(QStringLiteral("One at a time — use Bulk for a set."));
}

void TexturesTab::showRowContextMenu(const QPoint& viewPos, QAbstractItemView* view)
{
    const QVector<int> rows = menuctx::contextFiles(view, viewPos, [](const QModelIndex& ix) { return ix.row(); });
    if (rows.isEmpty()) return;
    QVector<AssetId> ids;
    for (int r : rows) ids << m_model->idAt(r);
    const int clicked = menuctx::clickedFile(view, viewPos, [](const QModelIndex& ix) { return ix.row(); }, rows);
    const AssetRow row = m_model->rowAt(clicked);
    {
        QItemSelection sel;
        for (int r : rows) sel.select(m_model->index(r, 0), m_model->index(r, m_model->columnCount() - 1));
        m_list->selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect);
    }
    QMenu menu(this);
    const QString stem = QFileInfo(row.name).completeBaseName();
    menu.addAction(QStringLiteral("Preview"), this, [this, id = row.id] { loadTexture(id); });
    menu.addSeparator();
    menu.addAction(MenuText::prompts(MenuText::exportLabel(QStringLiteral("Export"), ids.size(), stem, QStringLiteral("texture")) + QStringLiteral(" as PNG")), this, [this, ids] { savePng(ids, false); });
    const QString last = settings::lastExportDir();
    if (!last.isEmpty() && QDir(last).exists())
        menu.addAction(MenuText::exportLastLabel(QStringLiteral("Export"), ids.size(), stem, QStringLiteral("texture"), last), this, [this, ids] { savePng(ids, true); });
    QAction* orig = menu.addAction(MenuText::prompts(ids.size() > 1 ? QStringLiteral("Extract %1 original files (raw)").arg(ids.size())
                                                                     : QStringLiteral("Save original file")),
                                   this, [this, ids] { extractOriginals(ids); });
    orig->setEnabled(ids.size() == 1);
    menu.addSeparator();
    menu.addAction(MenuText::withCopyValue(MenuText::kCopyName, stem), this, [stem] { qApp->clipboard()->setText(stem); });
    menu.addAction(MenuText::withCopyValue(MenuText::kCopyPath, row.name), this, [n = row.name] { qApp->clipboard()->setText(n); });
    menu.addAction(MenuText::withCopyValue(MenuText::kCopyId(), QString::number(row.id.value)), this, [v = row.id.value] { qApp->clipboard()->setText(QString::number(v)); });
    if (m_currentId == row.id && !m_currentImage.isNull())
        menu.addAction(MenuText::kCopyImage, this, [this] { qApp->clipboard()->setImage(m_currentImage); });
    emit rowMenuAboutToShow(&menu, ids);
    if (menudump::capture(&menu, QStringLiteral("textures/rows"))) return;
    menu.exec(view->viewport()->mapToGlobal(viewPos));
}

void TexturesTab::setGridView(bool on)
{
    m_model->setGridMode(on);
    if (on) {
        m_model->setIconProvider([this](const AssetId& id) { return m_thumbs->get(id); });
        setIconPx(m_iconPx);
        m_list->hide(); m_grid->show(); m_grid->setFocus();
    } else {
        m_model->setIconProvider(nullptr);
        m_grid->hide(); m_list->show(); m_list->setFocus();
    }
    if (m_gridBtn->isChecked() != on) { QSignalBlocker b(m_gridBtn); m_gridBtn->setChecked(on); }
}

void TexturesTab::setIconPx(int px)
{
    m_iconPx = qBound(48, px, 320);
    m_model->setIconPx(m_iconPx);
    m_grid->setIconSize(QSize(m_iconPx, m_iconPx));
    m_grid->setGridSize(CardDelegate::cellFor(m_iconPx));
}

bool TexturesTab::eventFilter(QObject* obj, QEvent* ev)
{
    const bool onGrid = (m_grid && obj == m_grid->viewport());
    if (onGrid && ev->type() == QEvent::Wheel) {
        auto* we = static_cast<QWheelEvent*>(ev);
        if (we->modifiers() & Qt::ControlModifier) { setIconPx(m_iconPx + (we->angleDelta().y() > 0 ? 12 : -12)); return true; }
        return false;
    }
    if (onGrid && ev->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(ev);
        const QModelIndex idx = m_grid->indexAt(m_grid->viewport()->mapFromGlobal(he->globalPos()));
        if (idx.isValid()) {
            const AssetRow row = m_model->rowAt(idx.row());
            hover::Preview::instance().requestLazy(QStringLiteral("tex:%1").arg(row.id.value), he->globalPos(), [this, row] {
                hover::Content c;
                TextureInfo info;
                const QImage img = m_store ? m_store->loadTexture(row.id, &info, nullptr) : QImage();
                c.image = img.isNull() ? m_thumbs->get(row.id) : QPixmap::fromImage(img);
                c.name = row.inGameName.isEmpty() ? QFileInfo(row.name).fileName() : row.inGameName;
                c.path = row.name;
                if (!img.isNull()) c.info << QStringLiteral("%1×%2 %3").arg(img.width()).arg(img.height()).arg(info.format);
                return c;
            });
            return true;
        }
    }
    if (onGrid && ev->type() == QEvent::Leave) hover::Preview::instance().cancel();
    return BrowserTab::eventFilter(obj, ev);
}

}  // namespace core
