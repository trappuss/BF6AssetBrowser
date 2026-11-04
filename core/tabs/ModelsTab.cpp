#include "core/tabs/ModelsTab.h"
#include "core/app/AppPaths.h"
#include "core/app/AppPlugin.h"
#include "core/app/CoreSettings.h"
#include "core/app/ExportNotifier.h"
#include "core/app/Hotkeys.h"
#include "core/app/SehGuard.h"
#include "core/app/StatusLine.h"
#include "core/gl/GLModelWidget.h"
#include "core/gl/ThumbnailRenderer.h"
#include "core/ui/AssetListModel.h"
#include "core/ui/CardDelegate.h"
#include "core/ui/ExportCapture.h"
#include "core/ui/ExportDialogs.h"
#include "core/ui/FunnelFilter.h"
#include "core/ui/Glyphs.h"
#include "core/ui/HoverPreview.h"
#include "core/ui/MenuContext.h"
#include "core/ui/MenuDump.h"
#include "core/ui/MenuText.h"
#include "core/ui/NPanel.h"
#include "core/ui/PanelBox.h"
#include "core/ui/PanelPersist.h"
#include "core/ui/SearchBox.h"
#include "core/ui/SearchQuery.h"
#include "core/ui/TipBar.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QSet>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <QElapsedTimer>
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
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPointer>
#include <QProgressDialog>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QTableView>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QWheelEvent>
#include <algorithm>
#include <thread>

namespace core {

namespace {
const char* kSearchKey = "models/searchHistory";
const char* kFacetsKey = "facets/models";
const char* kFacetsAnyKey = "facets/modelsMatchAny";
const char* kGridKey = "models/grid";
const char* kIconPxKey = "models/iconPx";
const char* kTexturedIconsKey = "grid/texturedIcons";
const char* kShadingKey = "models/shading";
const char* kChannelKey = "models/channel";
const char* kCamFovKey = "models/cam/fov";
const char* kCamOrthoKey = "models/cam/ortho";
const char* kCamSpinKey = "models/cam/spin";
const char* kCamSpinSpeedKey = "models/cam/spinSpeed";
const char* kRenExposureKey = "models/render/exposure";
const char* kRenRigKey = "models/render/rig";
const char* kRenCullKey = "models/render/cull";
const char* kRenGradKey = "models/render/grad";
const char* kCamAutoFrameKey = "models/cam/autoFrame";
const char* kOvOverlaysKey = "models/ov/overlays";
const char* kOvGridKey = "models/ov/grid";
}  // namespace

ModelsTab::ModelsTab(AppPlugin& plugin, const QStringList& kinds, QWidget* parent,
                     const QString& persistKey)
    : BrowserTab(parent), m_plugin(plugin), m_kinds(kinds), m_persistKey(persistKey)
{
    m_model = new AssetListModel(this);
    auto* left = new QWidget(this);
    auto* right = new QWidget(this);
    buildLeft(left);
    buildRight(right);

    m_split = new QSplitter(Qt::Horizontal, this);
    m_split->addWidget(left);
    m_split->addWidget(right);
    m_split->setSizes({420, 860});
    PanelPersist::bind(m_split, m_persistKey + QStringLiteral("/splitter"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(new TipBar(m_persistKey + QStringLiteral("Viewport"),
        QStringLiteral("Tip: click a part to select it · drag to orbit, wheel to zoom · F1 lists every control · everything you can export is in the Export menu."), this), 0);
    outer->addWidget(m_split, 1);
}

ModelsTab::~ModelsTab()
{
    ++m_loadToken;   // any worker still running posts to a token that no longer matches
    waitForLoader("the Models tab is closing");   // the store it reads is destroyed after the tabs
}

void ModelsTab::waitForLoader(const char* why)
{
    std::unique_lock<std::mutex> l(m_loader->m);
    if (m_loader->running == 0) return;
    qInfo("models: waiting for the model being loaded (%s)", why);
    // No GUI-thread dependency in the worker (its result is POSTED), so this cannot deadlock. The
    // bound is for a store stuck in I/O: past it the wait gives up and says so.
    if (!m_loader->cv.wait_for(l, std::chrono::seconds(60), [&] { return m_loader->running == 0; }))
        qWarning("models: a model load did not finish in 60 s (%s) — continuing without it", why);
}

// ── Left: search + facets + list ─────────────────────────────────────────────────────────────
void ModelsTab::buildLeft(QWidget* left)
{
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(4, 4, 4, 4);
    auto* searchRow = new QHBoxLayout();
    m_search = new QLineEdit(left);
    m_search->setPlaceholderText(QStringLiteral("Search  (space = AND,  -exclude,  \"quoted phrase\",  a|b = OR,  #tag,  an id)"));
    m_search->setToolTip(searchTooltip());
    searchbox::attach(m_search, QLatin1String(kSearchKey));
    m_kindFacet = new QComboBox(left);
    m_kindFacet->addItem(QStringLiteral("all models"), QString());
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
    m_list->setMouseTracking(true);
    m_list->viewport()->installEventFilter(this);   // A7: rich hover preview on table rows
    m_list->installEventFilter(this);                // load-gated arrow scrub (hold Up/Down)
    m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->verticalHeader()->setVisible(false);
    m_list->horizontalHeader()->setStretchLastSection(true);
    m_list->setColumnWidth(AssetListModel::ColName, 340);
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
    // Card tiles: CardDelegate paints a framed thumbnail + caption with a clear hover/selection state,
    // so the grid reads as selectable cards rather than QListView's default icon+label. Its sizeHint is
    // FIXED (image square + two caption lines, independent of whether the thumbnail has rendered yet),
    // which makes uniformItemSizes safe AND fast here — the old blank-placeholder collapse that the POE2
    // note warned about only happened WITHOUT a delegate (the default sizeHint came from the icon).
    m_grid->setItemDelegate(new CardDelegate(m_grid));
    m_grid->setUniformItemSizes(true);
    m_grid->setWordWrap(false);
    m_grid->setSpacing(6);
    m_grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_grid->setSelectionModel(m_list->selectionModel());
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    m_grid->setMouseTracking(true);
    m_grid->viewport()->installEventFilter(this);
    m_grid->installEventFilter(this);                // load-gated arrow scrub (hold Up/Down)
    m_grid->hide();
    lv->addWidget(m_grid, 1);

    m_count = new QLabel(left);
    lv->addWidget(m_count);

    // Thumbnails: the core renderer on its own GL thread, fed by the shared load path.
    m_texturedIcons = QSettings().value(QLatin1String(kTexturedIconsKey), false).toBool();
    m_previewTexCap = qBound(0, QSettings().value(QStringLiteral("ui/previewTextureRes"), 0).toInt(), 8192);   // default 0 = full/native
    m_thumbTexCap   = qBound(0, QSettings().value(QStringLiteral("ui/thumbnailTextureRes"), 512).toInt(), 8192);
    m_thumbs = new gl::ThumbnailRenderer(this);
    // (the disk folder is per icon style — installThumbLoader)
    m_thumbs->setFaultGuard([](const std::function<void()>& body, QString* what) {
        seh::HardwareFault f;
        const bool ok = seh::runGuarded("thumbnail", body, &f);
        if (!ok && what) *what = f.what;
        return ok;
    });
    connect(m_thumbs, &gl::ThumbnailRenderer::ready, this, [this](const AssetId& id, int) { m_model->refreshIcon(id); hover::Preview::instance().refresh(); });
    // Proactively render the visible grid rows (+ a small margin) instead of waiting for every tile to
    // paint first: on a low-priority thread a purely paint-driven grid fills slowly and unevenly (a tile
    // caches a null icon on first paint and only repaints when its render lands), so thumbnails look like
    // they "never load". D4 prerenders the viewport window; this coalesces a scroll/filter burst into one
    // visible-range request so the queue stays near the viewport (the renderer already serves it LIFO).
    m_prerenderTimer = new QTimer(this); m_prerenderTimer->setSingleShot(true); m_prerenderTimer->setInterval(60);
    connect(m_prerenderTimer, &QTimer::timeout, this, &ModelsTab::prerenderVisible);
    connect(m_grid->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { schedulePrerender(); });
    {
        auto* gm = new QMenu(m_gridBtn);
        auto* tex = gm->addAction(QStringLiteral("Textured (base-colour) icons"));
        tex->setCheckable(true); tex->setChecked(m_texturedIcons);
        connect(tex, &QAction::toggled, this, [this](bool on) {
            m_texturedIcons = on;
            QSettings().setValue(QLatin1String(kTexturedIconsKey), on);
            installThumbLoader();   // the style is the loader's: it used to be fixed at index time
            m_thumbs->reset();
            m_model->refreshAllIcons();
        });
        m_gridBtn->setMenu(gm);
        m_gridBtn->setPopupMode(QToolButton::MenuButtonPopup);
    }

    m_debounce = new QTimer(this); m_debounce->setSingleShot(true); m_debounce->setInterval(180);
    connect(m_debounce, &QTimer::timeout, this, &ModelsTab::applyFilter);
    connect(m_search, &QLineEdit::textChanged, this, [this] { m_debounce->start(); });
    // Selecting a row (mouse or arrow keys) loads it — a short debounce coalesces rapid arrow-key
    // scrubbing so only the row you settle on is decoded.
    m_loadDebounce = new QTimer(this); m_loadDebounce->setSingleShot(true); m_loadDebounce->setInterval(110);
    connect(m_loadDebounce, &QTimer::timeout, this, &ModelsTab::loadCurrentRow);
    connect(m_list->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& cur, const QModelIndex&) { if (cur.isValid()) m_loadDebounce->start(); });
    connect(m_kindFacet, qOverload<int>(&QComboBox::currentIndexChanged), this, &ModelsTab::applyFilter);
    m_funnel->setActive(QSettings().value(QLatin1String(kFacetsKey)).toStringList(), QSettings().value(QLatin1String(kFacetsAnyKey)).toBool());
    connect(m_funnel, &FunnelFilter::changed, this, [this] {
        QSettings().setValue(QLatin1String(kFacetsKey), m_funnel->activeIds());
        QSettings().setValue(QLatin1String(kFacetsAnyKey), m_funnel->matchAny());
        applyFilter();
    });
    connect(m_list, &QTableView::activated, this, [this](const QModelIndex& idx) { if (idx.isValid()) loadModel(m_model->idAt(idx.row())); });
    connect(m_grid, &QListView::activated, this, [this](const QModelIndex& idx) { if (idx.isValid()) loadModel(m_model->idAt(idx.row())); });
    connect(m_list, &QTableView::customContextMenuRequested, this, [this](const QPoint& p) { showRowContextMenu(p, m_list); });
    connect(m_grid, &QListView::customContextMenuRequested, this, [this](const QPoint& p) { showRowContextMenu(p, m_grid); });
    connect(m_gridBtn, &QToolButton::toggled, this, [this](bool on) { setGridView(on); });
    m_iconPx = qBound(64, QSettings().value(QLatin1String(kIconPxKey), 160).toInt(), 320);
    if (QSettings().value(QLatin1String(kGridKey), false).toBool()) m_gridBtn->setChecked(true);
}

// ── Right: viewport + toolbar + anim bar + panels ────────────────────────────────────────────
void ModelsTab::buildRight(QWidget* right)
{
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(4, 4, 4, 4);
    auto* tool = new QHBoxLayout();
    m_shading = new QComboBox(right);
    for (int i = 0; i <= int(gl::ShadingMode::Rendered); ++i) m_shading->addItem(QString::fromLatin1(gl::shadingModeName(gl::ShadingMode(i))), i);
    m_shading->setCurrentIndex(int(gl::ShadingMode::Rendered));
    m_channel = new QComboBox(right);
    m_channel->installEventFilter(this);   // B2: wheel cycles channels
    m_channel->addItems({QStringLiteral("Shaded"), QStringLiteral("Base colour"), QStringLiteral("Normal"), QStringLiteral("Roughness"),
                         QStringLiteral("Metallic"), QStringLiteral("AO"), QStringLiteral("Emissive")});
    m_overlaysOn = new QCheckBox(QStringLiteral("Overlays"), right); m_overlaysOn->setChecked(true);
    m_overlaysOn->setToolTip(QStringLiteral("The master switch: off hides every overlay at once and remembers which were on."));
    m_gridChk = new QCheckBox(QStringLiteral("Grid"), right); m_gridChk->setChecked(true);
    m_skelChk = new QCheckBox(QStringLiteral("Skeleton"), right);
    m_hpChk = new QCheckBox(QStringLiteral("Hardpoints"), right);
    tool->addWidget(new QLabel(QStringLiteral("Shading:"))); tool->addWidget(m_shading);
    tool->addWidget(new QLabel(QStringLiteral("Channel:"))); tool->addWidget(m_channel);
    tool->addStretch(1);
    tool->addWidget(m_overlaysOn); tool->addWidget(m_gridChk); tool->addWidget(m_skelChk); tool->addWidget(m_hpChk);
    rv->addLayout(tool);

    auto* viewRow = new QHBoxLayout();
    viewRow->setContentsMargins(0, 0, 0, 0);
    m_view = new gl::GLModelWidget(right);
    m_view->setMinimumSize(360, 360);
    m_view->setFeatures(m_plugin.viewFeatures());          // the game's look, not the family's
    m_view->setExtension(m_plugin.viewportExtension());
    m_view->setFocusPolicy(Qt::ClickFocus);   // E3: viewport takes keys (H/Shift+H/Alt+H) after a click
    m_view->installEventFilter(this);
    viewRow->addWidget(m_view, 1);
    m_panels = new NPanel(m_persistKey + QStringLiteral("/npanel"), right);
    viewRow->addWidget(m_panels, 0);
    rv->addLayout(viewRow, 1);

    // ── Animation bar: clip · play/pause · scrub — hidden until a clip exists ──
    m_animBar = new QWidget(right);
    auto* ab = new QHBoxLayout(m_animBar); ab->setContentsMargins(0, 0, 0, 0);
    m_clipBox = new QComboBox(m_animBar);
    m_playBtn = new QToolButton(m_animBar); m_playBtn->setIcon(glyph::toolIcon(32)); m_playBtn->setCheckable(true); m_playBtn->setToolTip(QStringLiteral("Play / pause"));
    m_timeline = new QSlider(Qt::Horizontal, m_animBar); m_timeline->setRange(0, 1000);
    m_timeline->installEventFilter(this);   // E4: wheel steps the playhead
    m_timeLbl = new QLabel(QStringLiteral("0.00 / 0.00s"), m_animBar);
    auto* loopChk = new QCheckBox(QStringLiteral("Loop"), m_animBar);   // C9: loop toggle (playback was always-loop)
    loopChk->setChecked(QSettings().value(QStringLiteral("models/anim/loop"), true).toBool());
    connect(loopChk, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue(QStringLiteral("models/anim/loop"), on); });
    auto* speedBox = new QComboBox(m_animBar);   // C9: playback speed
    speedBox->addItem(QStringLiteral("0.25x"), 0.25f); speedBox->addItem(QStringLiteral("0.5x"), 0.5f);
    speedBox->addItem(QStringLiteral("1x"), 1.0f); speedBox->addItem(QStringLiteral("2x"), 2.0f);
    { const int si = speedBox->findData(QSettings().value(QStringLiteral("models/anim/speed"), 1.0f).toFloat()); speedBox->setCurrentIndex(si < 0 ? 2 : si); }
    connect(speedBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [speedBox](int) { QSettings().setValue(QStringLiteral("models/anim/speed"), speedBox->currentData().toFloat()); });
    ab->addWidget(new QLabel(QStringLiteral("Clip:"))); ab->addWidget(m_clipBox);
    ab->addWidget(m_playBtn); ab->addWidget(m_timeline, 1); ab->addWidget(m_timeLbl);
    ab->addWidget(loopChk); ab->addWidget(speedBox);
    m_animBar->setVisible(false);
    rv->addWidget(m_animBar);

    // ── The panels the common denominator fills (tabs-store.md §6) ──
    m_parts = new QTableWidget(0, 4, m_panels);
    m_parts->setHorizontalHeaderLabels({QStringLiteral("Part"), QStringLiteral("Tris"), QStringLiteral("Material"), QStringLiteral("Shown")});
    m_parts->horizontalHeader()->setStretchLastSection(false);
    m_parts->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_parts->verticalHeader()->setVisible(false);
    m_parts->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_parts->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_parts->setContextMenuPolicy(Qt::CustomContextMenu);
    m_parts->setSortingEnabled(true);   // C1: numeric Tris sort (col 1 is stored as an int)
    m_clipList = new QListWidget(m_panels);
    m_clipList->setToolTip(QStringLiteral("Animation clips this model carries — click one to play it"));
    m_attachList = new QListWidget(m_panels);
    m_attachList->setToolTip(QStringLiteral("Attached objects this model declares — tick to show each one placed on its bone"));
    m_info = new QLabel(m_panels);
    m_info->setAlignment(Qt::AlignTop | Qt::AlignLeft); m_info->setWordWrap(true);
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse); m_info->setMargin(6);
    m_panels->addPanel(QStringLiteral("parts"), QStringLiteral("PARTS"), 24, m_parts, QStringLiteral("Every part of the model: triangles, material, shown or hidden."));
    m_panels->addPanel(QStringLiteral("anims"), QStringLiteral("ANIMATIONS"), 14, m_clipList, QStringLiteral("The clips this model's rig carries."));
    m_panels->addPanel(QStringLiteral("attach"), QStringLiteral("ATTACHMENTS"), 15, m_attachList, QStringLiteral("Pieces declared on this model, placed on their bones."));
    m_panels->addPanel(QStringLiteral("info"), QStringLiteral("INFO"), 16, m_info, QStringLiteral("Name, id, path, counts, bones, materials."));
    // ── CAMERA panel (B7/B8): FOV, angle presets, framing, orthographic, turntable — all persisted,
    //    wiring the GLModelWidget camera API the tab previously left unexposed. ──
    {
        auto* cam = new QWidget(m_panels);
        auto* cv = new QVBoxLayout(cam); cv->setContentsMargins(6, 6, 6, 6); cv->setSpacing(5);
        auto* fovRow = new QHBoxLayout();
        fovRow->addWidget(new QLabel(QStringLiteral("FOV"), cam));
        auto* fov = new QSlider(Qt::Horizontal, cam); fov->setRange(20, 90);
        fov->setValue(qBound(20, QSettings().value(QLatin1String(kCamFovKey), 45).toInt(), 90));
        m_view->setFov(float(fov->value()));
        connect(fov, &QSlider::valueChanged, this, [this](int v) { m_view->setFov(float(v)); QSettings().setValue(QLatin1String(kCamFovKey), v); });
        fovRow->addWidget(fov, 1);
        cv->addLayout(fovRow);
        auto* preRow = new QHBoxLayout(); preRow->setSpacing(3);
        struct Preset { const char* label; const char* name; };
        static const Preset kPresets[] = {{"3/4","iso"},{"Fr","front"},{"Bk","back"},{"L","left"},{"R","right"},{"Top","top"}};
        for (const Preset& pr : kPresets) {
            auto* b = new QToolButton(cam); b->setText(QString::fromLatin1(pr.label));
            const QString name = QString::fromLatin1(pr.name);
            b->setToolTip(QStringLiteral("View from %1").arg(name));
            connect(b, &QToolButton::clicked, this, [this, name] { m_view->applyCameraPreset(name); });
            preRow->addWidget(b);
        }
        auto* frameBtn = new QToolButton(cam); frameBtn->setText(QStringLiteral("Frame"));
        frameBtn->setToolTip(QStringLiteral("Frame the whole model"));
        connect(frameBtn, &QToolButton::clicked, this, [this] { m_view->frameAll(false); });
        auto* autoFrame = new QCheckBox(QStringLiteral("Auto"), cam);
        autoFrame->setToolTip(QStringLiteral("Re-fit the view to each model as it loads, keeping your current angle (B8)"));
        autoFrame->setChecked(QSettings().value(QLatin1String(kCamAutoFrameKey), true).toBool());
        connect(autoFrame, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue(QLatin1String(kCamAutoFrameKey), on); });
        preRow->addWidget(frameBtn); preRow->addWidget(autoFrame); preRow->addStretch(1);
        cv->addLayout(preRow);
        auto* optRow = new QHBoxLayout();
        auto* ortho = new QCheckBox(QStringLiteral("Ortho"), cam);
        ortho->setChecked(QSettings().value(QLatin1String(kCamOrthoKey), false).toBool());
        m_view->setOrthographic(ortho->isChecked());
        connect(ortho, &QCheckBox::toggled, this, [this](bool on) { m_view->setOrthographic(on); QSettings().setValue(QLatin1String(kCamOrthoKey), on); });
        auto* tt = new QCheckBox(QStringLiteral("Turntable"), cam);
        tt->setChecked(QSettings().value(QLatin1String(kCamSpinKey), false).toBool());
        auto* spd = new QSlider(Qt::Horizontal, cam); spd->setRange(1, 20);
        spd->setValue(qBound(1, QSettings().value(QLatin1String(kCamSpinSpeedKey), 6).toInt(), 20));
        m_view->setSpinSpeed(spd->value() * 0.002f);
        m_view->setAutoSpin(tt->isChecked());
        connect(tt, &QCheckBox::toggled, this, [this](bool on) { m_view->setAutoSpin(on); QSettings().setValue(QLatin1String(kCamSpinKey), on); });
        connect(spd, &QSlider::valueChanged, this, [this](int v) { m_view->setSpinSpeed(v * 0.002f); QSettings().setValue(QLatin1String(kCamSpinSpeedKey), v); });
        optRow->addWidget(ortho); optRow->addWidget(tt); optRow->addWidget(spd, 1);
        cv->addLayout(optRow);
        cv->addStretch(1);
        m_panels->addPanel(QStringLiteral("camera"), QStringLiteral("CAMERA"), 6, cam, QStringLiteral("Field of view, angle presets, framing, orthographic and turntable."));
    }
    // ── RENDER panel (B9/B11): lighting + post. §8.9 — the game's viewFeatures() stays the per-launch
    //    default, so the lit-feature toggles start FROM features() and are a live override (not
    //    persisted); exposure, light rig, backface cull and backdrop are the viewer's, so they persist. ──
    {
        const gl::ViewFeatures f0 = m_view->features();
        auto* ren = new QWidget(m_panels);
        auto* rv = new QVBoxLayout(ren); rv->setContentsMargins(6, 6, 6, 6); rv->setSpacing(5);
        auto* expRow = new QHBoxLayout();
        expRow->addWidget(new QLabel(QStringLiteral("Exposure"), ren));
        auto* exp = new QSlider(Qt::Horizontal, ren); exp->setRange(25, 400);
        exp->setValue(qBound(25, QSettings().value(QLatin1String(kRenExposureKey), 100).toInt(), 400));
        m_view->setExposure(exp->value() / 100.0f);
        connect(exp, &QSlider::valueChanged, this, [this](int v) { m_view->setExposure(v / 100.0f); QSettings().setValue(QLatin1String(kRenExposureKey), v); });
        expRow->addWidget(exp, 1);
        rv->addLayout(expRow);
        auto* rigRow = new QHBoxLayout();
        rigRow->addWidget(new QLabel(QStringLiteral("Light"), ren));
        auto* rig = new QComboBox(ren); rig->addItems({QStringLiteral("Warm"), QStringLiteral("Neutral"), QStringLiteral("Cool")});
        rig->setCurrentIndex(qBound(0, QSettings().value(QLatin1String(kRenRigKey), m_plugin.defaultLightPreset()).toInt(), 2));
        { gl::GLModelWidget::LightRig r; r.preset = rig->currentIndex(); m_view->setLightRig(r); }
        connect(rig, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) { gl::GLModelWidget::LightRig r; r.preset = i; m_view->setLightRig(r); QSettings().setValue(QLatin1String(kRenRigKey), i); });
        rigRow->addWidget(rig, 1);
        rv->addLayout(rigRow);
        auto* togRow = new QHBoxLayout(); togRow->setSpacing(8);
        { auto* c = new QCheckBox(QStringLiteral("IBL"), ren); c->setChecked(f0.ibl); connect(c, &QCheckBox::toggled, this, [this](bool on){ m_view->setFeatureIbl(on); }); togRow->addWidget(c); }
        { auto* c = new QCheckBox(QStringLiteral("Tonemap"), ren); c->setChecked(f0.tonemap); connect(c, &QCheckBox::toggled, this, [this](bool on){ m_view->setFeatureTonemap(on); }); togRow->addWidget(c); }
        { auto* c = new QCheckBox(QStringLiteral("Spec AA"), ren); c->setChecked(f0.specAA); connect(c, &QCheckBox::toggled, this, [this](bool on){ m_view->setFeatureSpecAA(on); }); togRow->addWidget(c); }
        togRow->addStretch(1);
        rv->addLayout(togRow);
        auto* togRow2 = new QHBoxLayout(); togRow2->setSpacing(8);
        { auto* c = new QCheckBox(QStringLiteral("Shadows"), ren); c->setChecked(f0.shadow); connect(c, &QCheckBox::toggled, this, [this](bool on){ m_view->setShadowEnabled(on); }); togRow2->addWidget(c); }
        { auto* c = new QCheckBox(QStringLiteral("SSAO"), ren); c->setChecked(f0.ssao); connect(c, &QCheckBox::toggled, this, [this](bool on){ m_view->setSsaoEnabled(on); }); togRow2->addWidget(c); }
        { auto* c = new QCheckBox(QStringLiteral("Cull back"), ren); c->setChecked(QSettings().value(QLatin1String(kRenCullKey), true).toBool()); m_view->setBackfaceCull(c->isChecked()); connect(c, &QCheckBox::toggled, this, [this](bool on){ m_view->setBackfaceCull(on); QSettings().setValue(QLatin1String(kRenCullKey), on); }); togRow2->addWidget(c); }
        { auto* c = new QCheckBox(QStringLiteral("Gradient"), ren); c->setChecked(QSettings().value(QLatin1String(kRenGradKey), m_plugin.defaultBackgroundGradient()).toBool()); m_view->setBackgroundGradient(c->isChecked()); connect(c, &QCheckBox::toggled, this, [this](bool on){ m_view->setBackgroundGradient(on); QSettings().setValue(QLatin1String(kRenGradKey), on); }); togRow2->addWidget(c); }
        togRow2->addStretch(1);
        rv->addLayout(togRow2);
        rv->addStretch(1);
        m_panels->addPanel(QStringLiteral("render"), QStringLiteral("RENDER"), 45, ren, QStringLiteral("Lighting and post — exposure, light preset, IBL/tonemap/spec-AA, shadows, SSAO and the backdrop."));
    }
    m_panels->restoreState({QStringLiteral("parts"), QStringLiteral("info")});
    m_panels->attachToggle(m_view);

    // Playback: the tab owns the timer; the viewport reads it for animPlaying().
    m_playTimer = new QTimer(this);
    m_playTimer->setInterval(16);
    m_view->setPlaybackTimer(m_playTimer);
    connect(m_playTimer, &QTimer::timeout, this, [this, loopChk, speedBox] {
        const float dur = m_view->clipDuration();
        if (dur <= 0) return;
        float t = m_view->time() + 0.016f * speedBox->currentData().toFloat();
        if (t > dur) { if (loopChk->isChecked()) t -= dur; else { t = dur; setPlaying(false); } }
        m_view->setTime(t);
        if (!m_timeline->isSliderDown()) { QSignalBlocker b(m_timeline); m_timeline->setValue(int(t / dur * 1000.0f)); }
        m_timeLbl->setText(QStringLiteral("%1 / %2s").arg(t, 0, 'f', 2).arg(dur, 0, 'f', 2));
    });

    // ── Wiring ──
    connect(m_shading, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) { m_view->setShadingMode(gl::ShadingMode(i)); QSettings().setValue(QLatin1String(kShadingKey), i); });
    connect(m_view, &gl::GLModelWidget::shadingModeChanged, this, [this](gl::ShadingMode m) { QSignalBlocker b(m_shading); m_shading->setCurrentIndex(int(m)); });
    connect(m_channel, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) { m_view->setViewChannel(i); QSettings().setValue(QLatin1String(kChannelKey), i); });
    // B1/B2: restore the persisted shading mode and channel (index == stable id for both here).
    { const int sv = QSettings().value(QLatin1String(kShadingKey), int(gl::ShadingMode::Rendered)).toInt();
      if (sv >= 0 && sv < m_shading->count()) m_shading->setCurrentIndex(sv);
      const int cv = QSettings().value(QLatin1String(kChannelKey), 0).toInt();
      if (cv >= 0 && cv < m_channel->count()) m_channel->setCurrentIndex(cv); }
    connect(m_overlaysOn, &QCheckBox::toggled, this, [this](bool on) { m_view->setOverlaysEnabled(on); QSettings().setValue(QLatin1String(kOvOverlaysKey), on); });
    connect(m_gridChk, &QCheckBox::toggled, this, [this](bool on) { m_view->setShowGrid(on); QSettings().setValue(QLatin1String(kOvGridKey), on); });
    connect(m_skelChk, &QCheckBox::toggled, this, [this](bool on) { m_view->setShowSkeleton(on); });
    connect(m_hpChk, &QCheckBox::toggled, this, [this](bool on) { m_view->setShowHardpoints(on); });
    // B3: restore persisted overlay toggles through the master gate (grid first, master last).
    { const bool grid = QSettings().value(QLatin1String(kOvGridKey), true).toBool();
      if (m_gridChk->isChecked() != grid) m_gridChk->setChecked(grid);
      const bool ov = QSettings().value(QLatin1String(kOvOverlaysKey), true).toBool();
      if (m_overlaysOn->isChecked() != ov) m_overlaysOn->setChecked(ov); }
    connect(m_view, &gl::GLModelWidget::selectionChanged, this, [this](const QSet<int>& parts, int) {
        // One named path both surfaces call: sync the parts table without re-emitting.
        if (m_syncingSelection) return;
        m_syncingSelection = true;
        m_parts->clearSelection();
        for (int r = 0; r < m_parts->rowCount(); ++r)
            if (parts.contains(m_parts->item(r, 0)->data(Qt::UserRole).toInt())) m_parts->selectRow(r);
        m_syncingSelection = false;
    });
    connect(m_parts, &QTableWidget::itemSelectionChanged, this, [this] {
        if (m_syncingSelection) return;
        m_syncingSelection = true;
        QSet<int> sel;
        for (const QModelIndex& idx : m_parts->selectionModel()->selectedRows()) sel.insert(m_parts->item(idx.row(), 0)->data(Qt::UserRole).toInt());
        m_view->setSelectedParts(sel);
        m_syncingSelection = false;
    });
    // C1: the "Shown" column is a checkbox — toggling it shows/hides that part in the viewport.
    connect(m_parts, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
        if (m_syncingSelection || it->column() != 3) return;
        QTableWidgetItem* n = m_parts->item(it->row(), 0);
        if (!n) return;
        m_view->setPartVisible(n->data(Qt::UserRole).toInt(), it->checkState() == Qt::Checked);
        int shown = 0;
        for (int r = 0; r < m_parts->rowCount(); ++r)
            if (m_parts->item(r, 3) && m_parts->item(r, 3)->checkState() == Qt::Checked) ++shown;
        if (QLabel* t = m_panels->titleLabel(QStringLiteral("parts")))
            t->setText(m_parts->rowCount() == shown ? QStringLiteral("PARTS · %1").arg(shown)
                                                     : QStringLiteral("PARTS · %1 of %2 shown").arg(shown).arg(m_parts->rowCount()));
    });
    connect(m_parts, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        const QModelIndex idx = m_parts->indexAt(p);
        showViewportPartMenu(idx.isValid() ? m_parts->item(idx.row(), 0)->data(Qt::UserRole).toInt() : -1, m_parts->viewport()->mapToGlobal(p));
    });
    connect(m_view, &gl::GLModelWidget::partRightClicked, this, &ModelsTab::showViewportPartMenu);
    connect(m_view, &gl::GLModelWidget::partFocused, this, [this](int part) {
        QVector3D c; float r = 1.0f;
        if (part >= 0 && m_view->partsBounds({part}, c, r)) m_view->frameRegionKeepRotation(c, r, true);
    });
    connect(m_clipBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) { setClipIndex(i - 1); });
    connect(m_playBtn, &QToolButton::toggled, this, [this](bool on) { setPlaying(on); });
    connect(m_timeline, &QSlider::sliderMoved, this, [this](int v) {
        const float dur = m_view->clipDuration();
        if (dur > 0) { setPlaying(false); m_view->setTime(dur * v / 1000.0f); m_timeLbl->setText(QStringLiteral("%1 / %2s").arg(m_view->time(), 0, 'f', 2).arg(dur, 0, 'f', 2)); }
    });
    connect(m_clipList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || !m_clipList->isEnabled()) return;
        { QSignalBlocker b(m_clipBox); m_clipBox->setCurrentIndex(row); }
        setClipIndex(row - 1);
        setPlaying(row > 0);   // a click on a clip shows the motion; row 0 is the bind pose
    });
    connect(m_attachList, &QListWidget::itemChanged, this, [this](QListWidgetItem* it) {
        const int ai = it->data(Qt::UserRole).toInt();
        if (ai < 0 || ai >= m_attachmentShown.size()) return;
        // Deferred: applyAttachments clears this list, and the item emitting the signal is in it.
        const bool on = it->checkState() == Qt::Checked;
        QMetaObject::invokeMethod(this, [this, ai, on] { setAttachmentShown(ai, on); }, Qt::QueuedConnection);
    });
    m_view->setOverlayText(QStringLiteral("Select a model"));
}

// ── Lifecycle ────────────────────────────────────────────────────────────────────────────────
void ModelsTab::onIndexReady()
{
    m_model->setSource(m_store, m_kinds);
    QVector<TagGroup> groups;
    for (const QString& k : m_kinds) groups += m_store ? m_store->tagGroups(k) : QVector<TagGroup>();
    m_model->setTagGroups(groups);
    m_funnel->setGroups(m_model->tagGroups());
    {
        QSignalBlocker b(m_kindFacet);
        const QString keep = m_kindFacet->currentData().toString();
        m_kindFacet->clear();
        m_kindFacet->addItem(QStringLiteral("all models"), QString());
        for (const QString& k : m_model->rowKinds()) m_kindFacet->addItem(k, k);
        const int i = m_kindFacet->findData(keep);
        m_kindFacet->setCurrentIndex(i < 0 ? 0 : i);
        m_kindFacet->setVisible(m_kindFacet->count() > 2);
    }
    installThumbLoader();
    m_thumbs->setDiskEnabled(true);
    m_thumbs->reset();
    applyFilter();
    qInfo("models: %d row(s) in %s", m_model->totalInBaseSet(), qUtf8Printable(m_kinds.join(QStringLiteral("+"))));
}

// The thumbnail loader for the current store and icon style, with the style's own disk folder —
// one shared folder served the other style's PNGs after a toggle.
namespace {
// Model thumbnails on disk: core_thumbs_v<N>[_tex]/<data fingerprint>/ (audit 2026-09-25).
//  • The fingerprint: named by the asset's stable id alone, a model a game patch changed kept its
//    old PNG for good (D4 and POE2 ids survive patches).
//  • "core_": the core used to write thumbs_v<N>, which FOX's own thumbnail renderer (still used by
//    FOX's Customize tab) also writes and version-prunes in the same cache folder — a version bump
//    on either side would have deleted the other's.
// Pruned, once per run, only inside the cache folder and only names this code writes: this style's
// other fingerprints, other core_thumbs VERSIONS (both styles of the current version stay), and the
// old layout's files (s<scheme>_<key>_<size>.png — FOX's are hex-named and never match).
QString thumbDiskDirImpl(const QString& fingerprint, bool textured)
{
    const QString baseName = QStringLiteral("core_thumbs_v%1%2").arg(gl::kThumbCacheVersion).arg(textured ? QStringLiteral("_tex") : QString());
    const QDir cache(paths::cacheDir());
    const QString base = cache.filePath(baseName);
    const QString sub = fingerprint.isEmpty() ? QStringLiteral("nofingerprint")
        : QString::fromLatin1(QCryptographicHash::hash(fingerprint.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
    static QSet<QString> pruned;
    if (!pruned.contains(baseName + QLatin1Char('/') + sub)) {
        pruned.insert(baseName + QLatin1Char('/') + sub);
        int n = 0;
        const QRegularExpression coreDir(QStringLiteral("^core_thumbs_v(\\d+)(_tex)?$"));
        for (const QString& d : cache.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QRegularExpressionMatch m = coreDir.match(d);
            if (m.hasMatch() && m.captured(1).toUInt() != gl::kThumbCacheVersion)
                n += QDir(cache.filePath(d)).removeRecursively() ? 1 : 0;
        }
        const QRegularExpression fpDir(QStringLiteral("^([0-9a-f]{12}|nofingerprint)$"));
        const QDir b(base);
        for (const QString& d : b.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            if (d != sub && fpDir.match(d).hasMatch()) n += QDir(b.filePath(d)).removeRecursively() ? 1 : 0;
        const QRegularExpression legacy(QStringLiteral("^s\\d+_.+_\\d+\\.png$"));
        for (const QString& old : {QStringLiteral("thumbs_v1"), QStringLiteral("thumbs_v1_tex")}) {
            const QDir o(cache.filePath(old));
            if (!o.exists()) continue;
            for (const QString& f : o.entryList(QDir::Files))
                if (legacy.match(f).hasMatch()) n += QFile::remove(o.filePath(f)) ? 1 : 0;
            if (old.endsWith(QLatin1String("_tex")) && o.isEmpty()) cache.rmdir(old);   // the core's own; FOX has no _tex
        }
        if (n) qInfo("thumbnails: pruned %d stale thumbnail folder(s)/file(s) (%s/%s is current)", n, qUtf8Printable(baseName), qUtf8Printable(sub));
    }
    return QDir(base).filePath(sub);
}
}  // namespace

QString ModelsTab::thumbnailDiskDir(const QString& fingerprint, bool textured) { return thumbDiskDirImpl(fingerprint, textured); }

void ModelsTab::installThumbLoader()
{
    const bool textured = m_texturedIcons;
    m_thumbs->setDiskDir(thumbnailDiskDir(m_store && m_store->isOpen() ? m_store->fingerprint() : QString(), textured));
    m_thumbs->setDiskKey([store = m_store](const AssetId& id) { return store ? store->diskCacheKey(id) : QString(); });
    const int thumbCap = m_thumbTexCap;
    m_thumbs->setLoader([store = m_store, textured, thumbCap](const AssetId& id) {
        gl::ThumbnailModel m;
        if (!store) return m;
        store->setDecodeTextureCap(thumbCap);   // this render thread: thumbnails are tiny, keep textures small
        const model::Loaded l = model::load(*store, id, false, textured, false);
        if (!l.ok) return m;
        m.ok = true;
        m.geo = l.geo;
        if (textured) for (const Material& mat : l.materials) m.baseByMaterial << mat.baseColor.image;
        return m;
    });
}

void ModelsTab::onSecondaryReady()
{
    m_model->refreshFromStore();
    QVector<TagGroup> groups;
    for (const QString& k : m_kinds) groups += m_store ? m_store->tagGroups(k) : QVector<TagGroup>();
    m_model->setTagGroups(groups);
    m_funnel->setGroups(m_model->tagGroups());
    applyFilter();
    ++m_secondaryGen;
    relistFromSecondary();
}

void ModelsTab::relistFromSecondary()
{
    // A secondary index can be where a model's clips come from (D4 lists them from its animation
    // index, which finishes after the rows): a rigged model shown before it arrived listed none.
    // Ask the store again — the listing only; clips stay lazy, decoded when played or exported.
    // Same for attachments (D4 lists them from its entity index): a model shown before it listed none.
    if (m_store && m_current.ok && m_currentId.isValid() && m_current.attachments.isEmpty()) {
        QVector<Attachment> att;
        seh::HardwareFault fault;
        if (!seh::runGuarded("attachments", [&] { att = m_store->attachments(m_currentId); }, &fault))
            qWarning("models: re-listing attachments faulted (%s)", qUtf8Printable(fault.what));
        else if (!att.isEmpty()) {
            m_current.attachments = att;
            refreshAttachments();
            rebuildPartsPanel();
            qInfo("models: %d attachment(s) listed for %s now that the secondary indexes are in",
                  int(att.size()), qUtf8Printable(m_current.geo.sourcePath));
        }
    }
    if (m_store && m_current.ok && m_currentId.isValid() && !m_current.skeleton.isEmpty() && m_current.clips.isEmpty()) {
        QVector<Clip> clips;
        seh::HardwareFault fault;
        if (!seh::runGuarded("clips", [&] { m_store->loadSkeleton(m_currentId, m_current.geo, true, &clips); }, &fault))
            qWarning("models: re-listing clips faulted (%s)", qUtf8Printable(fault.what));
        else if (!clips.isEmpty()) {
            m_current.clips = clips;
            refreshAnimBar();
            refreshInfo();
            qInfo("models: %d clip(s) listed for %s now that the secondary indexes are in",
                  int(clips.size()), qUtf8Printable(m_current.geo.sourcePath));
            poseForAttachments();
        }
    }
}

void ModelsTab::refresh() {}

void ModelsTab::reset()
{
    ++m_loadToken;
    m_pendingLoad = AssetId();
    waitForLoader("the game data is being reloaded");   // before the plugin reopens the store
    m_inFlightId = AssetId();
    m_thumbs->setPaused(true);
    m_thumbs->reset();
    m_thumbs->setPaused(false);
    clearCurrentModel(QStringLiteral("Select a model"));
    m_info->clear();
    m_model->setSource(nullptr, m_kinds);
    applyFilter();
}

void ModelsTab::onSettingsChanged()
{
    // Texture-resolution settings for the grid thumbnails and the 3D preview (export has its own, applied
    // per-export via ExportOptions). Re-read; re-render thumbnails and reload the shown model if changed.
    const int pv = qBound(0, QSettings().value(QStringLiteral("ui/previewTextureRes"), 0).toInt(), 8192);   // default 0 = full/native
    const int th = qBound(0, QSettings().value(QStringLiteral("ui/thumbnailTextureRes"), 512).toInt(), 8192);
    const bool tx = QSettings().value(QLatin1String(kTexturedIconsKey), false).toBool();
    const bool thumbChanged = (th != m_thumbTexCap) || (tx != m_texturedIcons);
    const bool prevChanged  = (pv != m_previewTexCap);
    m_previewTexCap = pv; m_thumbTexCap = th; m_texturedIcons = tx;
    if (thumbChanged) { installThumbLoader(); if (m_thumbs) m_thumbs->reset(); schedulePrerender(); }
    if (prevChanged && m_currentId.isValid()) loadModel(m_currentId);
}

void ModelsTab::persistView()
{
    QSettings().setValue(QLatin1String(kGridKey), m_gridBtn->isChecked());
    QSettings().setValue(QLatin1String(kIconPxKey), m_iconPx);
}

void ModelsTab::applyFilter()
{
    m_model->applyFilters(m_search->text(), m_kindFacet->currentData().toString(), m_funnel->activeIds(), m_funnel->matchAny());
    m_funnel->setCounts(m_model->facetCounts());
    m_count->setText(QStringLiteral("%1 of %2 models").arg(m_model->rowCount()).arg(m_model->totalInBaseSet()));
    const bool filtered = m_model->rowCount() != m_model->totalInBaseSet();   // A11: accent the count when a filter is active
    m_count->setStyleSheet(filtered ? QStringLiteral("color: palette(highlight); font-weight: bold;") : QString());
    schedulePrerender();   // the visible set changed — fill the new top-of-list thumbnails
}

void ModelsTab::reloadList()
{
    if (!m_model) return;
    m_model->refreshFromStore();
    QVector<TagGroup> groups;
    for (const QString& k : m_kinds) groups += m_store ? m_store->tagGroups(k) : QVector<TagGroup>();
    m_model->setTagGroups(groups);
    m_funnel->setGroups(m_model->tagGroups());
    applyFilter();
}

// ── Loading (D4's worker path) ───────────────────────────────────────────────────────────────
void ModelsTab::loadCurrentRow()
{
    const QModelIndex cur = m_list->selectionModel()->currentIndex();
    if (!cur.isValid()) return;
    const AssetId id = m_model->idAt(cur.row());
    if (!id.isValid() || id == m_currentId) return;
    loadModel(id);
}

// Called when a load finishes: if the user is holding an arrow (held while the model loaded), advance
// one row now and load it immediately — the next hold-repeat is deferred again, so the viewport steps
// through every model, gated by decode speed. The selection moves with signals blocked so we drive the
// load directly (no debounce) rather than through the normal settle path.
void ModelsTab::applyNavHeld()
{
    if (m_navHeld == 0 || m_loading) return;
    QAbstractItemView* view = (m_gridBtn && m_gridBtn->isChecked())
                                  ? static_cast<QAbstractItemView*>(m_grid) : static_cast<QAbstractItemView*>(m_list);
    QItemSelectionModel* sel = view->selectionModel();
    const QModelIndex cur = sel->currentIndex();
    const int dir = m_navHeld;
    m_navHeld = 0;
    if (!cur.isValid()) return;
    const int nr = cur.row() + dir;
    if (nr < 0 || nr >= m_model->rowCount()) return;
    const QModelIndex ni = m_model->index(nr, AssetListModel::ColName);
    m_loadDebounce->stop();
    { QSignalBlocker b(sel); sel->setCurrentIndex(ni, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows); }
    view->scrollTo(ni, QAbstractItemView::EnsureVisible);
    const AssetId id = m_model->idAt(nr);
    if (id.isValid()) loadModel(id);
}

void ModelsTab::loadModel(const AssetId& id)
{
    if (!id.isValid() || !m_store || !m_store->isOpen()) return;
    bool busy;
    { std::lock_guard<std::mutex> l(m_loader->m); busy = m_loader->running > 0; }
    m_loading = true;
    m_view->setOverlayText(QStringLiteral("Loading…"));
    if (busy && id == m_inFlightId) {
        // Already on its way (a row selection and showModel both ask; or the user clicked away
        // and back): the newest selection is the one loading — make its result current again.
        m_pendingLoad = AssetId();
        m_loadToken = m_inFlightToken;
        return;
    }
    const quint64 token = ++m_loadToken;
    if (busy) { m_pendingLoad = id; return; }   // starts when the current one ends
    startLoad(id, token);
}

void ModelsTab::startLoad(const AssetId& id, quint64 token)
{
    m_loadStartGen = m_secondaryGen;
    m_pendingLoad = AssetId();
    m_inFlightId = id;
    m_inFlightToken = token;
    IGameStore* store = m_store;
    const int previewCap = m_previewTexCap;
    QPointer<ModelsTab> self(this);
    auto work = [self, store, id, token, previewCap] {
        model::Loaded m;
        seh::HardwareFault fault;
        // 3D-preview texture resolution: capped on THIS (worker) thread, restored after the parse.
        const int prevCap = store->setDecodeTextureCap(previewCap);
        // Under the guard: the parsers throw a bounded error for a truncated file, but an out-of-
        // bounds read in a malformed-but-plausible one is an access violation, and one bad asset
        // must report, never take the app down (template §2, §3).
        const bool survived = seh::runGuarded("parse", [&] { m = model::load(*store, id, true, true); }, &fault);
        store->setDecodeTextureCap(prevCap);
        if (!survived) { m = model::Loaded(); m.id = id; m.error = QStringLiteral("hardware fault while parsing (%1)").arg(fault.what); }
        QMetaObject::invokeMethod(qApp, [self, m, token] { if (self) self->onLoadFinished(m, token); }, Qt::QueuedConnection);
    };
    if (!store->isThreadSafe()) { work(); return; }   // served on the GUI thread; the result is posted
    std::shared_ptr<LoaderState> st = m_loader;
    { std::lock_guard<std::mutex> l(st->m); ++st->running; }
    std::thread([work, st] {
        work();
        { std::lock_guard<std::mutex> l(st->m); --st->running; }
        st->cv.notify_all();
    }).detach();
}

void ModelsTab::onLoadFinished(const model::Loaded& m, quint64 token)
{
    if (m_inFlightToken == token) m_inFlightId = AssetId();
    applyLoaded(m, token);
    // The newest selection made while that load ran — only the newest; the ones between are moot.
    if (m_pendingLoad.isValid() && m_store && m_store->isOpen()) {
        const AssetId next = m_pendingLoad;
        m_pendingLoad = AssetId();
        if (next != m_currentId) startLoad(next, m_loadToken);
        else m_loading = false;
    }
    applyNavHeld();   // a held arrow deferred during this load: advance one and load it now
}

void ModelsTab::applyLoaded(const model::Loaded& m, quint64 token)
{
    if (token != m_loadToken) return;   // superseded by a newer selection
    m_loading = false;
    if (!m.ok) {
        StatusLine::instance().report(this, QStringLiteral("load failed: %1").arg(m.error));
        clearCurrentModel(QStringLiteral("Could not load this model"));
        m_info->setText(m.error);
        qWarning("models: load failed for %llu — %s", (unsigned long long)m.id.value, qUtf8Printable(m.error));
        return;
    }
    m_current = m;
    m_currentId = m.id;
    m_shownGeo = m.geo;
    m_attachmentOfPart.clear();
    m_attachmentShown.clear();
    // GPU submission is the other classic fault site (a driver crash surfaces as an access violation).
    seh::HardwareFault fault;
    const bool ok = seh::runGuarded("render", [&] {
        m_view->setScene(m_shownGeo, m.skeleton, false);
        m_view->setMaterials(m.materials);
        m_view->setHardpoints(m.hardpoints);
    }, &fault);
    if (!ok) {
        const QString err = QStringLiteral("hardware fault while uploading to the GPU (%1)").arg(fault.what);
        StatusLine::instance().report(this, QStringLiteral("load failed: %1").arg(err));
        clearCurrentModel(QStringLiteral("Could not show this model"));
        m_info->setText(err);
        return;
    }
    m_view->setOverlayText(QString());
    m_skelChk->setEnabled(!m.skeleton.isEmpty());
    m_hpChk->setEnabled(!m.hardpoints.isEmpty());
    refreshAnimBar();
    m_assembledShown = false;   // the body alone was just uploaded
    refreshAttachments();
    rebuildPartsPanel();
    refreshInfo();
    if (QSettings().value(QLatin1String(kCamAutoFrameKey), true).toBool()) m_view->frameAll(true, false);   // B8: keep the viewing angle across model switches (auto-frame refits distance)
    // The secondary indexes landed while this load was in flight: its clips / attachments were
    // listed without them, and onSecondaryReady ran before it arrived.
    if (m_loadStartGen != m_secondaryGen) relistFromSecondary();
    StatusLine::instance().report(this, QStringLiteral("%1 — %2 parts · %3 tris · %4 bones · %5 clips")
        .arg(m.geo.sourcePath.isEmpty() ? QString::number(m.id.value) : m.geo.sourcePath.section(QLatin1Char('/'), -1))
        .arg(m.geo.parts.size()).arg(m.geo.triangleCount()).arg(m.skeleton.size()).arg(m.clips.size()));
    qInfo("models: loaded %s — %d parts, %d tris, %d bones, %d clips, %d materials",
          qUtf8Printable(m.geo.sourcePath), int(m.geo.parts.size()), m.geo.triangleCount(), m.skeleton.size(), int(m.clips.size()), int(m.materials.size()));
    emit modelLoaded(m.id);
}

void ModelsTab::clearCurrentModel(const QString& overlay)
{
    // No model is current, and every panel says so: a failed load used to leave the PREVIOUS
    // model's parts, clips and attachments on screen (and "Export animations only" exported its
    // clips), and a reload left all of them showing the old game (audit 2026-09-25).
    m_current = model::Loaded();
    m_currentId = AssetId();
    m_shownGeo = Geometry();
    m_attachmentShown.clear();
    m_attachmentOfPart.clear();
    m_assembledShown = false;
    { QSignalBlocker b(m_attachList); m_attachList->clear(); }
    m_view->clearScene();
    m_view->setOverlayText(overlay);
    refreshAnimBar();
    refreshAttachments();
    rebuildPartsPanel();
}

void ModelsTab::rebuildPartsPanel()
{
    m_syncingSelection = true;
    const bool wasSort = m_parts->isSortingEnabled();
    m_parts->setSortingEnabled(false);          // fill by part index, then restore sorting
    m_parts->setRowCount(m_shownGeo.parts.size());
    int shown = 0;
    for (int i = 0; i < m_shownGeo.parts.size(); ++i) {
        const Part& p = m_shownGeo.parts[i];
        auto* n = new QTableWidgetItem(p.name);
        n->setData(Qt::UserRole, i);
        m_parts->setItem(i, 0, n);
        auto* tris = new QTableWidgetItem;
        tris->setData(Qt::DisplayRole, p.triangleCount());   // int → numeric sort, not lexical
        tris->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_parts->setItem(i, 1, tris);
        const QString mat = p.materialIndex >= 0 && p.materialIndex < m_shownGeo.materialNames.size() ? m_shownGeo.materialNames[p.materialIndex] : QString();
        m_parts->setItem(i, 2, new QTableWidgetItem(QFileInfo(mat).fileName()));
        auto* vis = new QTableWidgetItem;
        vis->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        const bool on = m_view->partVisible(i);
        vis->setCheckState(on ? Qt::Checked : Qt::Unchecked);
        m_parts->setItem(i, 3, vis);
        if (on) ++shown;
    }
    m_parts->setSortingEnabled(wasSort);
    m_parts->setProperty(kPanelWantH, 22 * (m_shownGeo.parts.size() + 1));
    if (QLabel* t = m_panels->titleLabel(QStringLiteral("parts")))
        t->setText(m_shownGeo.parts.size() == shown ? QStringLiteral("PARTS · %1").arg(shown)
                                                     : QStringLiteral("PARTS · %1 of %2 shown").arg(shown).arg(m_shownGeo.parts.size()));
    m_syncingSelection = false;
}

void ModelsTab::refreshInfo()
{
    const model::Loaded& m = m_current;
    QStringList lines;
    const QString name = m.geo.sourcePath.section(QLatin1Char('/'), -1);
    lines << QStringLiteral("<b>%1</b>").arg(name.toHtmlEscaped());
    lines << m.geo.sourcePath.toHtmlEscaped();
    lines << QStringLiteral("%1: %2").arg(MenuText::nouns().id.toHtmlEscaped()).arg(m.id.value);
    if (!m.geo.formatNote.isEmpty()) lines << m.geo.formatNote.toHtmlEscaped();
    lines << QStringLiteral("%1 parts · %2 verts · %3 tris%4").arg(m.geo.parts.size()).arg(m.geo.vertices.size()).arg(m.geo.triangleCount())
                 .arg(m.geo.skinned ? QStringLiteral(" · skinned") : QString());
    {   // C11: bounding-box dimensions in the model's own units
        const float dx = m.geo.bboxMax[0] - m.geo.bboxMin[0], dy = m.geo.bboxMax[1] - m.geo.bboxMin[1], dz = m.geo.bboxMax[2] - m.geo.bboxMin[2];
        if (dx > 0.0f || dy > 0.0f || dz > 0.0f)
            lines << QStringLiteral("bounds %1 × %2 × %3").arg(dx, 0, 'f', 2).arg(dy, 0, 'f', 2).arg(dz, 0, 'f', 2);
    }
    if (!m.skeleton.isEmpty()) lines << QStringLiteral("%1 bones · %2 clips%3").arg(m.skeleton.size()).arg(m.clips.size())
                                            .arg(m.skeleton.note.isEmpty() ? QString() : QStringLiteral(" · %1").arg(m.skeleton.note.toHtmlEscaped()));
    if (!m.hardpoints.isEmpty()) lines << QStringLiteral("%1 hardpoints").arg(m.hardpoints.size());
    if (!m.geo.materialNames.isEmpty()) {
        lines << QStringLiteral("<b>Materials:</b>");
        for (int i = 0; i < m.geo.materialNames.size(); ++i) {
            QString facts;   // C3: alpha mode · double-sided · which maps the decoded material carries
            if (i < m.materials.size()) {
                const Material& mt = m.materials[i];
                QStringList maps;
                if (!mt.baseColor.image.isNull()) maps << QStringLiteral("base");
                if (!mt.normal.image.isNull())    maps << QStringLiteral("normal");
                if (!mt.orm.image.isNull())       maps << QStringLiteral("orm");
                if (!mt.emissive.image.isNull())  maps << QStringLiteral("emissive");
                static const char* const kAlpha[] = {"opaque", "mask", "blend", "additive"};
                const QString am = QString::fromLatin1(kAlpha[qBound(0, int(mt.alphaMode), 3)]);
                facts = QStringLiteral(" <span style='color:#999'>— %1%2 · %3</span>")
                            .arg(am)
                            .arg(mt.doubleSided ? QStringLiteral(" · 2-sided") : QString())
                            .arg(maps.isEmpty() ? QStringLiteral("(no maps)") : maps.join(QLatin1Char(',')));
            }
            lines << QStringLiteral("• %1%2").arg(m.geo.materialNames[i].toHtmlEscaped(), facts);
        }
    }
    m_info->setText(lines.join(QStringLiteral("<br>")));
}

void ModelsTab::refreshAnimBar()
{
    const model::Loaded& m = m_current;
    const bool haveClips = !m.clips.isEmpty() && m_view->skeletonMatchesMesh();
    m_animBar->setVisible(haveClips);
    if (m.geo.skinned && !m.skeleton.isEmpty() && !m_view->skeletonMatchesMesh())
        StatusLine::instance().report(this, QStringLiteral("skinned mesh shown at bind pose — its rig does not cover the mesh's joints (no clip playback)"));
    {
        QSignalBlocker b(m_clipBox);
        m_clipBox->clear();
        if (haveClips) {
            m_clipBox->addItem(QStringLiteral("(bind pose)"));
            for (const Clip& c : m.clips) m_clipBox->addItem(c.name.isEmpty() ? QStringLiteral("clip") : c.name);
            m_clipBox->setCurrentIndex(0);
        }
    }
    setPlaying(false);
    m_view->clearClip();
    m_timeline->setValue(0);
    m_timeLbl->setText(QStringLiteral("0.00 / 0.00s"));
    // The ANIMATIONS panel mirrors the bar: row 0 = bind pose, then one row per clip with duration.
    QSignalBlocker b(m_clipList);
    m_clipList->clear();
    if (!haveClips) {
        m_clipList->addItem(m.geo.skinned ? QStringLiteral("(no playable clips)") : QStringLiteral("(static mesh — no animation)"));
        m_clipList->setEnabled(false);
    } else {
        m_clipList->setEnabled(true);
        m_clipList->addItem(QStringLiteral("(bind pose)"));
        for (const Clip& c : m.clips)
            m_clipList->addItem(QStringLiteral("%1   ·   %2 s · %3 fps").arg(c.name.isEmpty() ? QStringLiteral("clip") : c.name).arg(c.duration, 0, 'f', 2).arg(c.fps, 0, 'f', 0));
        m_clipList->setCurrentRow(0);
    }
    if (QLabel* t = m_panels->titleLabel(QStringLiteral("anims")))
        t->setText(haveClips ? QStringLiteral("ANIMATIONS · %1").arg(m.clips.size()) : QStringLiteral("ANIMATIONS"));
}

void ModelsTab::setClipIndex(int i)
{
    // Every way a clip is chosen — the bar's box, the ANIMATIONS list, the harness's --clip — comes
    // through here, so the box, the list and the time readout follow from here (row 0 = bind pose).
    // A harness --clip used to decode and pose while the box still read "(bind pose)".
    auto syncUi = [this](int row) {
        { QSignalBlocker b(m_clipBox); if (row < m_clipBox->count()) m_clipBox->setCurrentIndex(row); }
        { QSignalBlocker b(m_clipList); if (m_clipList->isEnabled() && row < m_clipList->count()) m_clipList->setCurrentRow(row); }
        const float dur = m_view->hasClip() ? m_view->clipDuration() : 0.0f;
        m_timeLbl->setText(QStringLiteral("0.00 / %1s").arg(dur, 0, 'f', 2));
    };
    if (i < 0 || i >= m_current.clips.size()) { m_view->clearClip(); setPlaying(false); m_timeline->setValue(0); syncUi(0); return; }
    if (m_current.clips[i].lazy) {
        // On demand, as FOX decoded one clip at a time; said in the log with its cost.
        QElapsedTimer t; t.start();
        QString err;
        bool ok = false;
        seh::HardwareFault fault;
        if (!seh::runGuarded("clip", [&] { ok = model::ensureClip(*m_store, m_current, i, &err); }, &fault)) { ok = false; err = fault.what; }
        if (!ok) {
            qWarning("models: clip %s could not be decoded — %s", qUtf8Printable(m_current.clips[i].name), qUtf8Printable(err));
            StatusLine::instance().report(this, QStringLiteral("%1 — could not decode: %2").arg(m_current.clips[i].name, err));
            m_view->clearClip(); setPlaying(false); m_timeline->setValue(0); syncUi(0); return;
        }
        qInfo("models: decoded clip %s — %d frames, %d track(s)%s in %lld ms", qUtf8Printable(m_current.clips[i].name), m_current.clips[i].frameCount(),
              int(m_current.clips[i].tracks.size()), m_current.clips[i].tracks.isEmpty() ? " (every channel is the rest pose — plays as the bind pose)" : "", t.elapsed());
    }
    m_view->setClip(m_current.clips[i]);
    m_view->setTime(0.0f);
    m_timeline->setValue(0);
    syncUi(i + 1);
}

int ModelsTab::clipBoxRow() const { return m_clipBox->currentIndex(); }

bool ModelsTab::setAttachmentShown(int ai, bool on)
{
    if (ai < 0 || ai >= m_attachmentShown.size()) return false;
    if (on != m_attachmentShown[ai]) {
        m_attachmentShown[ai] = on;
        applyAttachments();   // re-assemble from the clean body, as D4's rebuildAssembledGeometry
        rebuildPartsPanel();
    }
    return m_attachmentOfPart.contains(ai) == on;
}

void ModelsTab::setPlaying(bool on)
{
    if (on && !m_view->hasClip()) on = false;
    if (on) m_playTimer->start(); else m_playTimer->stop();
    { QSignalBlocker b(m_playBtn); m_playBtn->setChecked(on); }
    m_playBtn->setIcon(glyph::toolIcon(on ? 33 : 32));
}

// The ATTACHMENTS panel: what the model declares, and the assembled view. Pieces are baked onto
// their bones by model::assemble (the same rule the export uses), so the viewport shows exactly
// what an assembled export will contain; a piece that could not be placed is listed, greyed.
void ModelsTab::refreshAttachments()
{
    // What is on at load is each piece's defaultOn (POE2: everything; D4: nothing — its panel lists
    // held and spawned models and attaches only what is ticked).
    m_attachmentShown.clear();
    for (const Attachment& a : m_current.attachments) m_attachmentShown.append(a.defaultOn);
    applyAttachments();
}

// Body + the ticked pieces, re-assembled from the clean body every time (a detach is exact), with
// the list rebuilt from the result: a piece that could not be placed is unticked and says why.
void ModelsTab::applyAttachments()
{
    QSignalBlocker b(m_attachList);
    m_attachList->clear();
    const model::Loaded& m = m_current;
    m_attachmentOfPart.clear();
    if (m.attachments.isEmpty()) {
        auto* it = new QListWidgetItem(QStringLiteral("(no attachments declared for this model)"), m_attachList);
        it->setFlags(it->flags() & ~Qt::ItemIsUserCheckable);
        if (QLabel* t = m_panels->titleLabel(QStringLiteral("attach"))) t->setText(QStringLiteral("ATTACHMENTS"));
        return;
    }
    // setScene clears the pose: keep the clip, its time and play state across the rebuild (D4 does).
    const int clipRow = m_clipBox->currentIndex();
    const float t = m_view->time();
    const bool playing = m_playTimer->isActive();
    seh::HardwareFault fault;
    model::Assembly a;
    if (!seh::runGuarded("attach", [&] { a = model::assemble(*m_store, m, m_attachmentShown, true); }, &fault)) {
        qWarning("models: attaching faulted (%s) — reverted to the body", qUtf8Printable(fault.what));
        StatusLine::instance().report(this, QStringLiteral("Couldn't attach that model — reverted."));
        a = model::Assembly();
        a.geo = m.geo; a.materials = m.materials; a.attachmentOfPart.fill(-1, m.geo.parts.size());
        m_attachmentShown.fill(false);
    }
    m_attachmentOfPart = a.attachmentOfPart;
    // Nothing placed and the viewport already holds the plain body (a load, or a relist): no upload.
    if (a.placed > 0 || m_assembledShown) {
        m_shownGeo = a.geo;
        const bool ok = seh::runGuarded("render", [&] {
            m_view->setScene(m_shownGeo, m.skeleton, true);
            m_view->setMaterials(a.materials);
        }, &fault);
        if (!ok) { qWarning("models: attachment upload faulted (%s)", qUtf8Printable(fault.what)); m_view->clearScene(); }
        m_assembledShown = a.placed > 0;
        if (clipRow > 0 && clipRow - 1 < m.clips.size() && !m.clips[clipRow - 1].lazy) {
            m_view->setClip(m.clips[clipRow - 1]);
            m_view->setTime(t);
            setPlaying(playing);
        }
    }
    int placedCount = 0;
    for (int ai = 0; ai < m.attachments.size(); ++ai) {
        const Attachment& p = m.attachments[ai];
        const bool placed = m_attachmentOfPart.contains(ai);
        if (m_attachmentShown.value(ai) && !placed) m_attachmentShown[ai] = false;   // tried, not placed
        QString why = a.whyNot.value(ai);
        if (why.isEmpty() && !p.mesh.isValid()) why = p.unresolvedNote.isEmpty() ? QStringLiteral("mesh unresolved") : p.unresolvedNote;
        if (!why.isEmpty()) why = QStringLiteral("(%1)").arg(why);
        QString text = p.group.isEmpty() ? p.label : QStringLiteral("%1  ›  %2").arg(p.group, p.label);
        if (!p.parentBone.isEmpty()) text += QStringLiteral("   ·   on %1").arg(p.parentBone);
        if (!why.isEmpty()) text += QStringLiteral("   ") + why;
        auto* item = new QListWidgetItem(text, m_attachList);
        const bool usable = p.mesh.isValid();
        item->setData(Qt::UserRole, usable ? ai : -1);
        item->setFlags(usable ? (item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled) : (item->flags() & ~Qt::ItemIsUserCheckable & ~Qt::ItemIsEnabled));
        item->setCheckState(placed ? Qt::Checked : Qt::Unchecked);
        if (placed) ++placedCount;
    }
    for (const QString& s : a.skipped) qInfo("models: attachment skipped — %s", qUtf8Printable(s));
    if (QLabel* tl = m_panels->titleLabel(QStringLiteral("attach")))
        tl->setText(QStringLiteral("ATTACHMENTS · %1 of %2").arg(placedCount).arg(m.attachments.size()));
    poseForAttachments();
}

void ModelsTab::poseForAttachments()
{
    // Only from the bind pose: a clip the person (or --clip) chose is never replaced.
    if (!m_store || !m_assembledShown || m_clipBox->currentIndex() != 0) return;
    QString who;
    for (int ai = 0; ai < m_current.attachments.size() && who.isEmpty(); ++ai)
        if (m_attachmentShown.value(ai) && m_attachmentOfPart.contains(ai) && m_store->attachmentNeedsPose(m_currentId, m_current.attachments[ai]))
            who = m_current.attachments[ai].label;
    if (who.isEmpty()) return;
    if (m_current.clips.isEmpty()) {   // relistFromSecondary calls again once clips are listed
        qInfo("models: %s has no rest position and no clips are listed yet — shown where the bind pose puts it", qUtf8Printable(who));
        return;
    }
    const int ri = m_store->restClip(m_currentId, m_current.clips);
    if (ri < 0 || ri >= m_current.clips.size()) {
        qInfo("models: %s has no rest position and the game's data names no idle clip for this model — shown in the bind pose", qUtf8Printable(who));
        return;
    }
    setClipIndex(ri);
    if (!m_view->hasClip()) return;   // setClipIndex logged why
    qInfo("models: posed on the idle clip %s (frame 0, paused) — %s has no rest position; only a clip places it",
          qUtf8Printable(m_current.clips[ri].name), qUtf8Printable(who));
    StatusLine::instance().report(this, QStringLiteral("Shown on the idle clip (%1): %2 has no rest position of its own.").arg(m_current.clips[ri].name, who));
}

// ── Selection helpers ────────────────────────────────────────────────────────────────────────
QVector<AssetId> ModelsTab::selectedIds() const
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

void ModelsTab::showModel(const AssetId& id)
{
    const int row = m_model->rowOfId(id);
    if (row >= 0) m_list->selectRow(row);
    loadModel(id);
}

bool ModelsTab::ensureLoaded(const AssetId& id)
{
    if (!id.isValid()) return false;
    if (id == m_currentId) return true;
    // Synchronous, for the batch capture paths: same load, same guard, no worker.
    const quint64 token = ++m_loadToken;
    m_loadStartGen = m_secondaryGen;
    model::Loaded m;
    seh::HardwareFault fault;
    if (!seh::runGuarded("parse", [&] { m = model::load(*m_store, id, true, true); }, &fault)) { m = model::Loaded(); m.error = fault.what; }
    applyLoaded(m, token);
    return id == m_currentId;
}

QVector<int> ModelsTab::currentPartSubset() const
{
    QVector<int> v;
    for (int p : m_view->selectedParts()) if (p < m_current.geo.parts.size()) v.append(p);
    std::sort(v.begin(), v.end());
    return v;
}

// What a whole-model export of the model on screen writes (D4's scope rule): an explicit part
// selection verbatim (hidden parts included); otherwise the body parts that are VISIBLE — a part
// hidden with Hide / Isolate used to be written anyway. Empty = everything.
QVector<int> ModelsTab::exportScopeParts() const
{
    const QVector<int> sel = currentPartSubset();
    if (!sel.isEmpty()) return sel;
    QVector<int> vis;
    bool anyHidden = false;
    for (int i = 0; i < m_current.geo.parts.size(); ++i) {
        if (m_view->partVisible(i)) vis.append(i); else anyHidden = true;
    }
    if (!anyHidden) return {};
    if (vis.isEmpty()) vis.append(-1);   // all hidden: nothing is in scope (the exporter says so)
    return vis;
}

// ── Export ───────────────────────────────────────────────────────────────────────────────────
bool ModelsTab::hasExportSelection() const { return m_store && m_store->isOpen() && !selectedIds().isEmpty(); }

QString ModelsTab::exportNoun() const
{
    const QVector<AssetId> ids = selectedIds();
    if (ids.size() == 1) {
        const int row = m_model->rowOfId(ids[0]);
        const QString name = row >= 0 ? QFileInfo(m_model->rowAt(row).name).completeBaseName()
                                      : QFileInfo(m_current.geo.sourcePath).completeBaseName();
        return name.isEmpty() ? QStringLiteral("model") : name;
    }
    return ids.isEmpty() ? QStringLiteral("model") : QStringLiteral("%1 models").arg(ids.size());
}

ExportOptions ModelsTab::optionsForExport(bool* cancelled, int modelCount)
{
    *cancelled = false;
    if (!settings::exportShowPrompt()) return settings::exportOptions(m_plugin.exportDefaults());
    ExportOptionsDialog dlg(m_plugin.exportDefaults(), this);
    dlg.setUnitScaleTip(m_plugin.unitScaleTip());
    const int clip = m_clipBox->currentIndex() - 1;
    dlg.setContext(m_current.clips.size(), m_view->hasClip() ? clip : -1, !m_current.attachments.isEmpty() || modelCount > 1, modelCount);
    if (dlg.exec() != QDialog::Accepted) { *cancelled = true; return {}; }
    return dlg.options();
}

QString ModelsTab::exportDirOrPrompt(const QString& title, bool* cancelled)
{
    *cancelled = false;
    const QString base = settings::lastExportDir().isEmpty() ? QDir::homePath() : settings::lastExportDir();
    if (settings::exportReuseLastDir() && !settings::lastExportDir().isEmpty() && QFileInfo::exists(base)) return base;
    const QString dir = QFileDialog::getExistingDirectory(this, title, base);
    if (dir.isEmpty()) { *cancelled = true; return {}; }
    settings::setLastExportDir(dir);
    return dir;
}

void ModelsTab::exportSelection()
{
    if (!m_store || !m_store->isOpen()) return;
    const QVector<AssetId> ids = selectedIds();
    if (ids.isEmpty()) { StatusLine::instance().report(this, QStringLiteral("Select one or more models to export.")); return; }
    bool cancelled = false;
    const ExportOptions opt = optionsForExport(&cancelled, ids.size());
    if (cancelled) return;
    const QString ext = opt.gltfSeparate ? QStringLiteral(".gltf") : QStringLiteral(".glb");
    const QString base = settings::lastExportDir().isEmpty() ? QDir::homePath() : settings::lastExportDir();
    auto stemOf = [this](const AssetId& id) {
        const int row = m_model->rowOfId(id);
        const QString n = row >= 0 ? m_model->rowAt(row).name : m_current.geo.sourcePath;
        return QFileInfo(n).completeBaseName();
    };
    if (ids.size() == 1) {
        QString out;
        if (settings::exportReuseLastDir() && !settings::lastExportDir().isEmpty() && QFileInfo::exists(base)) {
            out = model::uniquePath(base, stemOf(ids[0]), ext);
        } else {
            out = QFileDialog::getSaveFileName(this, QStringLiteral("Export model"), QDir(base).filePath(stemOf(ids[0]) + ext),
                                               opt.gltfSeparate ? QStringLiteral("glTF (*.gltf)") : QStringLiteral("glTF binary (*.glb)"));
            if (out.isEmpty()) return;
            if (!out.toLower().endsWith(ext)) out += ext;
        }
        settings::setLastExportDir(QFileInfo(out).absolutePath());
        const model::ExportResult r = model::exportOne(*m_store, ids[0], opt, out, ids[0] == m_currentId ? exportScopeParts() : QVector<int>(),
                                                             ids[0] == m_currentId ? m_attachmentShown : QVector<bool>());
        if (r.ok) { qInfo("export: %s", qUtf8Printable(r.summary)); ExportNotifier::instance().notify(QStringLiteral("Exported %1").arg(QFileInfo(out).fileName()), QFileInfo(out).absolutePath(), out); }
        else StatusLine::instance().report(this, QStringLiteral("export failed: %1").arg(r.error));
        return;
    }
    const QString dir = exportDirOrPrompt(QStringLiteral("Export %1 models to folder").arg(ids.size()), &cancelled);
    if (cancelled) return;
    int ok = 0, fail = 0;
    QProgressDialog prog(QStringLiteral("Exporting models…"), QStringLiteral("Cancel"), 0, ids.size(), this);
    prog.setWindowModality(Qt::WindowModal); prog.setMinimumDuration(400);
    for (int i = 0; i < ids.size(); ++i) {
        prog.setValue(i); QApplication::processEvents(); if (prog.wasCanceled()) break;
        const model::ExportResult r = model::exportOne(*m_store, ids[i], opt, model::uniquePath(dir, stemOf(ids[i]), ext), {},
                                                       ids[i] == m_currentId ? m_attachmentShown : QVector<bool>());
        if (r.ok) ++ok; else { ++fail; qWarning("export: %s — %s", qUtf8Printable(stemOf(ids[i])), qUtf8Printable(r.error)); }
    }
    prog.setValue(ids.size());
    ExportNotifier::instance().notify(QStringLiteral("Exported %1 model%2 to %3%4").arg(ok).arg(ok == 1 ? QString() : QStringLiteral("s"))
                                      .arg(QDir(dir).dirName()).arg(fail ? QStringLiteral(" (%1 failed)").arg(fail) : QString()), dir);
}

void ModelsTab::exportSelectionToLast()
{
    const QString dir = settings::lastExportDir();
    if (dir.isEmpty() || !QDir(dir).exists()) { StatusLine::instance().report(this, QStringLiteral("No export folder remembered yet — export once the long way.")); return; }
    const QVector<AssetId> ids = selectedIds();
    if (ids.isEmpty()) return;
    const ExportOptions opt = settings::exportOptions(m_plugin.exportDefaults());
    const QString ext = opt.gltfSeparate ? QStringLiteral(".gltf") : QStringLiteral(".glb");
    int ok = 0, fail = 0; QString lastFile;
    for (const AssetId& id : ids) {
        const int row = m_model->rowOfId(id);
        const QString stem = QFileInfo(row >= 0 ? m_model->rowAt(row).name : m_current.geo.sourcePath).completeBaseName();
        const QString out = model::uniquePath(dir, stem, ext);
        const model::ExportResult r = model::exportOne(*m_store, id, opt, out, ids.size() == 1 && id == m_currentId ? exportScopeParts() : QVector<int>(),
                                                             id == m_currentId ? m_attachmentShown : QVector<bool>());
        if (r.ok) { ++ok; lastFile = out; } else { ++fail; qWarning("export: %s — %s", qUtf8Printable(stem), qUtf8Printable(r.error)); }
    }
    ExportNotifier::instance().notify(ok == 1 ? QStringLiteral("Exported %1").arg(QFileInfo(lastFile).fileName())
                                              : QStringLiteral("Exported %1 model%2%3").arg(ok).arg(ok == 1 ? QString() : QStringLiteral("s")).arg(fail ? QStringLiteral(" (%1 failed)").arg(fail) : QString()),
                                      dir, ok == 1 ? lastFile : QString());
}

bool ModelsTab::selectModel(const QString& searchText)
{
    if (!m_store || !m_store->isOpen()) return false;
    m_search->setText(searchText);
    m_debounce->stop();
    applyFilter();
    if (m_model->rowCount() == 0) return false;
    const int row = m_model->bestRowFor(searchText);
    const AssetId id = m_model->idAt(row);
    m_list->selectRow(row);
    return ensureLoaded(id);
}

bool ModelsTab::exportTo(const QString& path, QString* error)
{
    if (!m_current.ok || !m_currentId.isValid()) { if (error) *error = QStringLiteral("no model is shown"); return false; }
    ExportOptions opt = settings::exportOptions(m_plugin.exportDefaults());
    opt.gltfSeparate = path.endsWith(QLatin1String(".gltf"), Qt::CaseInsensitive);
    const model::ExportResult r = model::exportOne(*m_store, m_currentId, opt, path, exportScopeParts(), m_attachmentShown);
    if (r.ok) qInfo("export: %s", qUtf8Printable(r.summary));
    else { qWarning("export: %s — %s", qUtf8Printable(path), qUtf8Printable(r.error)); if (error) *error = r.error; }
    return r.ok;
}

bool ModelsTab::hasAnimExport() const { return m_current.ok && !m_current.clips.isEmpty(); }

QString ModelsTab::animExportLabel() const
{
    return MenuText::prompts(QStringLiteral("Export animations only (%1 clip%2, .glb)").arg(m_current.clips.size()).arg(m_current.clips.size() == 1 ? QString() : QStringLiteral("s")));
}

void ModelsTab::exportAnimations()
{
    if (!hasAnimExport()) return;
    ExportOptions opt = settings::exportOptions(m_plugin.exportDefaults());
    opt.animationLibrary = true; opt.includeSkeleton = true; opt.includeAnimations = true; opt.embedTextures = false; opt.looseTextures = false;
    const QString base = settings::lastExportDir().isEmpty() ? QDir::homePath() : settings::lastExportDir();
    const QString stem = QFileInfo(m_current.geo.sourcePath).completeBaseName() + QStringLiteral("_anims");
    QString out = QFileDialog::getSaveFileName(this, QStringLiteral("Export animations only"), QDir(base).filePath(stem + QStringLiteral(".glb")), QStringLiteral("glTF binary (*.glb)"));
    if (out.isEmpty()) return;
    if (!out.toLower().endsWith(QStringLiteral(".glb"))) out += QStringLiteral(".glb");
    settings::setLastExportDir(QFileInfo(out).absolutePath());
    opt.gltfSeparate = false;
    const model::ExportResult r = model::exportOne(*m_store, m_currentId, opt, out);
    if (r.ok) { qInfo("export: %s", qUtf8Printable(r.summary)); ExportNotifier::instance().notify(QStringLiteral("Exported %1 (%2 clips, no mesh)").arg(QFileInfo(out).fileName()).arg(m_current.clips.size()), QFileInfo(out).absolutePath(), out); }
    else StatusLine::instance().report(this, QStringLiteral("export failed: %1").arg(r.error));
}

void ModelsTab::exportParts()
{
    if (!m_current.ok) { StatusLine::instance().report(this, QStringLiteral("Load a model and select parts to export.")); return; }
    const QVector<int> subset = currentPartSubset();
    if (subset.isEmpty()) { StatusLine::instance().report(this, QStringLiteral("Select one or more parts in the viewport first (click a part).")); return; }
    ExportOptionsDialog dlg(m_plugin.exportDefaults(), this);
    dlg.setUnitScaleTip(m_plugin.unitScaleTip());
    dlg.setContext(m_current.clips.size(), m_clipBox->currentIndex() - 1, false, 1);
    if (dlg.exec() != QDialog::Accepted) return;
    ExportOptions opt = dlg.options();
    opt.includeAttachments = false;   // a part subset is about the body's own parts
    const QString ext = opt.gltfSeparate ? QStringLiteral(".gltf") : QStringLiteral(".glb");
    const QString base = settings::lastExportDir().isEmpty() ? QDir::homePath() : settings::lastExportDir();
    QString out = QFileDialog::getSaveFileName(this, QStringLiteral("Export selected parts"),
        QDir(base).filePath(QFileInfo(m_current.geo.sourcePath).completeBaseName() + QStringLiteral("_parts") + ext),
        opt.gltfSeparate ? QStringLiteral("glTF (*.gltf)") : QStringLiteral("glTF binary (*.glb)"));
    if (out.isEmpty()) return;
    if (!out.toLower().endsWith(ext)) out += ext;
    settings::setLastExportDir(QFileInfo(out).absolutePath());
    const model::ExportResult r = model::exportOne(*m_store, m_currentId, opt, out, subset);
    if (r.ok) ExportNotifier::instance().notify(QStringLiteral("Exported %1 (%2 parts)").arg(QFileInfo(out).fileName()).arg(subset.size()), QFileInfo(out).absolutePath(), out);
    else StatusLine::instance().report(this, QStringLiteral("export failed: %1").arg(r.error));
}

// Non-destructive extraction: the EXACT original game files a model is made of, written by their
// own paths under the chosen folder (POE2's extractOriginals over IGameStore::dependencyFiles + read).
void ModelsTab::extractOriginals()
{
    const QVector<AssetId> ids = selectedIds();
    if (ids.isEmpty() || !m_store) { StatusLine::instance().report(this, QStringLiteral("Select one or more models to extract.")); return; }
    const QString base = settings::lastExportDir().isEmpty() ? QDir::homePath() : settings::lastExportDir();
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Extract original files to folder"), base);
    if (dir.isEmpty()) return;
    settings::setLastExportDir(dir);
    int files = 0, fail = 0;
    QSet<QString> written;
    for (const AssetId& id : ids) {
        const int row = m_model->rowOfId(id);
        QStringList deps = m_store->dependencyFiles(id);
        if (deps.isEmpty() && row >= 0) deps << m_model->rowAt(row).name;   // at least the asset itself
        for (const QString& gp : deps) {
            if (written.contains(gp)) continue;
            written.insert(gp);
            QString err;
            const QByteArray bytes = m_store->readPath(gp, &err);
            if (bytes.isEmpty()) { ++fail; qWarning("extract: %s — %s", qUtf8Printable(gp), qUtf8Printable(err)); continue; }
            const QString outPath = model::mirrorPath(dir, gp);
            QDir().mkpath(QFileInfo(outPath).absolutePath());
            QFile f(outPath);
            if (f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size()) ++files; else ++fail;
        }
    }
    ExportNotifier::instance().notify(QStringLiteral("Extracted %1 original file%2 for %3 model%4%5").arg(files).arg(files == 1 ? QString() : QStringLiteral("s"))
                                      .arg(ids.size()).arg(ids.size() == 1 ? QString() : QStringLiteral("s")).arg(fail ? QStringLiteral(" (%1 failed)").arg(fail) : QString()), dir);
}

// Save the current view as a PNG (template §15): options from the dialog or Settings, the render
// done offscreen at the chosen scale — a larger scale is a sharper re-render, not an upscale.
void ModelsTab::saveImage()
{
    const QVector<AssetId> ids = selectedIds();
    if (ids.isEmpty()) { StatusLine::instance().report(this, QStringLiteral("Load or select a model, then Save preview image.")); return; }
    int scale; bool tbg, crop;
    if (settings::imageShowPrompt()) {
        ImageExportDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted) return;
        scale = dlg.scalePercent(); tbg = dlg.transparentBg(); crop = dlg.cropToModel();
    } else {
        scale = settings::imageScalePercent(); tbg = settings::imageTransparentBg(); crop = settings::imageCropToModel();
    }
    auto stemOf = [this](const AssetId& id) { const int row = m_model->rowOfId(id); return QFileInfo(row >= 0 ? m_model->rowAt(row).name : m_current.geo.sourcePath).completeBaseName(); };
    const QString base = settings::lastExportDir().isEmpty() ? QDir::homePath() : settings::lastExportDir();
    if (ids.size() == 1) {
        if (!ensureLoaded(ids[0])) { StatusLine::instance().report(this, QStringLiteral("Couldn't load %1").arg(stemOf(ids[0]))); return; }
        const QImage img = m_view->renderToImage(scale, tbg, crop);
        if (img.isNull()) { StatusLine::instance().report(this, QStringLiteral("Couldn't render an image — the model isn't in the viewport.")); return; }
        QString out;
        if (settings::exportReuseLastDir() && !settings::lastExportDir().isEmpty() && QFileInfo::exists(base)) out = model::uniquePath(base, stemOf(ids[0]), QStringLiteral(".png"));
        else {
            out = QFileDialog::getSaveFileName(this, QStringLiteral("Save preview image"), QDir(base).filePath(stemOf(ids[0]) + QStringLiteral(".png")), QStringLiteral("PNG image (*.png)"));
            if (out.isEmpty()) return;
            if (!out.toLower().endsWith(QStringLiteral(".png"))) out += QStringLiteral(".png");
        }
        settings::setLastExportDir(QFileInfo(out).absolutePath());
        if (img.save(out, "PNG")) ExportNotifier::instance().notify(QStringLiteral("Saved %1 (%2×%3)").arg(QFileInfo(out).fileName()).arg(img.width()).arg(img.height()), QFileInfo(out).absolutePath(), out);
        else StatusLine::instance().report(this, QStringLiteral("Failed to save %1").arg(QFileInfo(out).fileName()));
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Save %1 preview images to folder").arg(ids.size()), base);
    if (dir.isEmpty()) return;
    settings::setLastExportDir(dir);
    QProgressDialog prog(QStringLiteral("Saving preview images…"), QStringLiteral("Cancel"), 0, ids.size(), this);
    prog.setWindowModality(Qt::WindowModal);
    int ok = 0, fail = 0;
    for (int i = 0; i < ids.size(); ++i) {
        prog.setValue(i); QApplication::processEvents(); if (prog.wasCanceled()) break;
        if (!ensureLoaded(ids[i])) { ++fail; continue; }
        const QImage img = m_view->renderToImage(scale, tbg, crop);
        if (!img.isNull() && img.save(model::uniquePath(dir, stemOf(ids[i]), QStringLiteral(".png")), "PNG")) ++ok; else ++fail;
    }
    prog.setValue(ids.size());
    ExportNotifier::instance().notify(QStringLiteral("Saved %1 image%2 to %3%4").arg(ok).arg(ok == 1 ? QString() : QStringLiteral("s")).arg(QDir(dir).dirName()).arg(fail ? QStringLiteral(" (%1 failed)").arg(fail) : QString()), dir);
}

void ModelsTab::turntableGif() { exportGifs(true); }
void ModelsTab::animLoopGif()  { exportGifs(false); }

void ModelsTab::exportGifs(bool turntable)
{
    const QVector<AssetId> ids = selectedIds();
    if (ids.isEmpty()) { StatusLine::instance().report(this, QStringLiteral("Load or select a model to export a GIF.")); return; }
    GifExportDialog dlg(turntable ? GifExportDialog::Turntable : GifExportDialog::AnimLoop, this);
    if (dlg.exec() != QDialog::Accepted) return;
    const capture::GifOptions opt = dlg.options();
    auto run = [&](const QString& outPath, const capture::ProgressFn& p) {
        return turntable ? capture::turntableGif(m_view, outPath, opt, p) : capture::animLoopGif(m_view, outPath, opt, p);
    };
    const QString kind = turntable ? QStringLiteral("Turntable GIF") : QStringLiteral("Animation-loop GIF");
    const QString base = settings::lastExportDir().isEmpty() ? QDir::homePath() : settings::lastExportDir();
    auto stemOf = [this](const AssetId& id) { const int row = m_model->rowOfId(id); return QFileInfo(row >= 0 ? m_model->rowAt(row).name : m_current.geo.sourcePath).completeBaseName(); };
    if (ids.size() == 1) {
        if (!ensureLoaded(ids[0])) { StatusLine::instance().report(this, QStringLiteral("Couldn't load %1").arg(stemOf(ids[0]))); return; }
        if (!turntable && !m_view->hasClip()) { StatusLine::instance().report(this, QStringLiteral("Select a clip first — an animation-loop GIF needs a clip.")); return; }
        QString out = QFileDialog::getSaveFileName(this, kind, QDir(base).filePath(stemOf(ids[0]) + QStringLiteral(".gif")), QStringLiteral("Animated GIF (*.gif)"));
        if (out.isEmpty()) return;
        if (!out.toLower().endsWith(QStringLiteral(".gif"))) out += QStringLiteral(".gif");
        settings::setLastExportDir(QFileInfo(out).absolutePath());
        QProgressDialog prog(QStringLiteral("Rendering %1…").arg(kind), QStringLiteral("Cancel"), 0, 100, this);
        prog.setWindowModality(Qt::WindowModal); prog.setMinimumDuration(0);
        const bool ok = run(out, [&](int done, int total) { prog.setMaximum(total); prog.setValue(done); QApplication::processEvents(); return !prog.wasCanceled(); });
        prog.close();
        if (ok) ExportNotifier::instance().notify(QStringLiteral("Saved %1").arg(QFileInfo(out).fileName()), QFileInfo(out).absolutePath(), out);
        else StatusLine::instance().report(this, QStringLiteral("%1 cancelled or failed").arg(kind));
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Save %1 GIFs to folder").arg(ids.size()), base);
    if (dir.isEmpty()) return;
    settings::setLastExportDir(dir);
    QProgressDialog prog(QStringLiteral("Rendering %1s…").arg(kind), QStringLiteral("Cancel"), 0, ids.size(), this);
    prog.setWindowModality(Qt::WindowModal); prog.setMinimumDuration(0);
    int ok = 0, fail = 0, skip = 0;
    for (int i = 0; i < ids.size(); ++i) {
        prog.setLabelText(QStringLiteral("Rendering %1s… (%2/%3)").arg(kind).arg(i + 1).arg(ids.size()));
        prog.setValue(i); QApplication::processEvents(); if (prog.wasCanceled()) break;
        if (!ensureLoaded(ids[i])) { ++fail; continue; }
        if (!turntable) {
            if (m_current.clips.isEmpty()) { ++skip; continue; }
            if (!m_view->hasClip()) setClipIndex(0);   // the first clip, so a batch loop has something to loop
        }
        if (run(model::uniquePath(dir, stemOf(ids[i]), QStringLiteral(".gif")), {})) ++ok; else ++fail;
    }
    prog.setValue(ids.size());
    ExportNotifier::instance().notify(QStringLiteral("Saved %1 GIF%2 to %3%4%5").arg(ok).arg(ok == 1 ? QString() : QStringLiteral("s")).arg(QDir(dir).dirName())
                                      .arg(fail ? QStringLiteral(" (%1 failed)").arg(fail) : QString()).arg(skip ? QStringLiteral(" (%1 had no clip)").arg(skip) : QString()), dir);
}

void ModelsTab::populateExportMenu(QMenu* menu)
{
    menu->addSeparator();
    QAction* parts = menu->addAction(MenuText::prompts(QStringLiteral("Export selected parts")), this, [this] { exportParts(); });
    parts->setEnabled(m_current.ok && !m_view->selectedParts().isEmpty());
    if (!parts->isEnabled()) parts->setToolTip(QStringLiteral("Click a part in the viewport first."));
    menu->addAction(MenuText::prompts(QStringLiteral("Extract original files (raw, unmodified)")), this, [this] { extractOriginals(); })
        ->setEnabled(hasExportSelection());
    menu->addSeparator();
    QAction* img = menu->addAction(MenuText::prompts(MenuText::kSaveViewportImage), this, [this] { saveImage(); });
    img->setEnabled(hasExportSelection());
    hotkeys::Role::set(img, hotkeys::Role::saveImage());
    menu->addSeparator();
    QAction* tt = menu->addAction(MenuText::prompts(MenuText::kTurntableGif), this, [this] { turntableGif(); });
    tt->setEnabled(hasExportSelection());
    hotkeys::Role::set(tt, hotkeys::Role::turntable());
    QAction* loop = menu->addAction(MenuText::prompts(MenuText::kAnimationGif), this, [this] { animLoopGif(); });
    loop->setEnabled(hasExportSelection() && hasAnimExport());
    if (!loop->isEnabled()) loop->setToolTip(QStringLiteral("Needs a model with clips."));
    hotkeys::Role::set(loop, hotkeys::Role::animGif());
}

// ── Menus ────────────────────────────────────────────────────────────────────────────────────
// The row menu, shared by the table and the grid, over the §2 selection rule (MenuContext).
void ModelsTab::showRowContextMenu(const QPoint& viewPos, QAbstractItemView* view)
{
    const QVector<int> rows = menuctx::contextFiles(view, viewPos, [](const QModelIndex& ix) { return ix.row(); });
    if (rows.isEmpty()) return;
    QVector<AssetId> ids;
    for (int r : rows) ids << m_model->idAt(r);
    const int clicked = menuctx::clickedFile(view, viewPos, [](const QModelIndex& ix) { return ix.row(); }, rows);
    const AssetRow& row = m_model->rowAt(clicked);
    // The menu acts on the context set: select it so the export paths (which read the selection) agree.
    {
        QItemSelection sel;
        for (int r : rows) sel.select(m_model->index(r, 0), m_model->index(r, m_model->columnCount() - 1));
        m_list->selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect);
    }
    QMenu menu(this);
    const QString stem = QFileInfo(row.name).completeBaseName();
    menu.addAction(QStringLiteral("Load / preview"), this, [this, id = row.id] { loadModel(id); });
    menu.addSeparator();
    menu.addAction(MenuText::withCopyValue(MenuText::kCopyName, stem), this, [stem] { qApp->clipboard()->setText(stem); });
    menu.addAction(MenuText::withCopyValue(MenuText::kCopyPath, row.name), this, [n = row.name] { qApp->clipboard()->setText(n); });
    if (ids.size() > 1)
        menu.addAction(MenuText::withRows(MenuText::kCopyId(), ids.size()), this, [ids] {
            QStringList v; for (const AssetId& id : ids) v << QString::number(id.value); qApp->clipboard()->setText(v.join(QLatin1Char('\n'))); });
    else
        menu.addAction(MenuText::withCopyValue(MenuText::kCopyId(), QString::number(row.id.value)), this, [v = row.id.value] { qApp->clipboard()->setText(QString::number(v)); });
    if (!MenuText::kCopyCollection().isEmpty()) {
        QAction* a = menu.addAction(MenuText::withCopyValue(MenuText::kCopyCollection(), row.collection), this, [c = row.collection] { qApp->clipboard()->setText(c); });
        a->setEnabled(!row.collection.isEmpty());   // §6: disable, don't hide, when this row has no value
    }
    menu.addSeparator();
    menu.addAction(MenuText::prompts(MenuText::exportLabel(QStringLiteral("Export"), ids.size(), stem, QStringLiteral("model"))), this, [this] { exportSelection(); });
    const QString last = settings::lastExportDir();
    if (!last.isEmpty() && QDir(last).exists())
        menu.addAction(MenuText::exportLastLabel(QStringLiteral("Export"), ids.size(), stem, QStringLiteral("model"), last), this, [this] { exportSelectionToLast(); });
    menu.addAction(MenuText::prompts(MenuText::kSaveViewportImage), this, [this] { saveImage(); });
    menu.addAction(MenuText::prompts(MenuText::kTurntableGif), this, [this] { turntableGif(); });
    menu.addAction(MenuText::prompts(MenuText::kAnimationGif), this, [this] { animLoopGif(); });
    menu.addAction(MenuText::prompts(QStringLiteral("Extract original files")), this, [this] { extractOriginals(); });
    if (m_gridBtn->isChecked()) {
        menu.addSeparator();
        menu.addAction(ids.size() > 1 ? MenuText::kRerenderThumbs.arg(ids.size()) : MenuText::kRerenderThumb, this, [this, ids] { for (const AssetId& id : ids) m_thumbs->refresh(id); });
    }
    emit rowMenuAboutToShow(&menu, ids);
    if (menudump::capture(&menu, QStringLiteral("models/rows"))) return;
    menu.exec(view->viewport()->mapToGlobal(viewPos));
}

// The viewport part menu (§11): frame · isolate · hide · export parts · copy names · show all.
void ModelsTab::showViewportPartMenu(int part, const QPoint& globalPos)
{
    if (!m_current.ok || m_view->partCount() == 0) return;
    // Right-click outside the selection acts on the clicked part alone (MenuContext's rule).
    QSet<int> sel = m_view->selectedParts();
    if (part >= 0 && !sel.contains(part)) { sel = {part}; m_view->setSelectedPart(part); }
    const int total = m_view->partCount();
    QMenu menu(this);
    if (!sel.isEmpty()) {
        const QString parts = MenuText::plural(QStringLiteral("part"), sel.size());
        const QString n = sel.size() == 1 ? QString() : QStringLiteral("%1 ").arg(sel.size());
        menu.addAction(QStringLiteral("Frame %1%2").arg(n, parts), this, [this, sel] {
            QVector3D c; float r = 1.0f; QVector<int> v(sel.begin(), sel.end());
            if (m_view->partsBounds(v, c, r)) m_view->frameRegionKeepRotation(c, r, true);
        });
        menu.addAction(QStringLiteral("Isolate %1%2").arg(n, parts), this, [this, sel] { m_view->isolateParts(sel); rebuildPartsPanel(); });
        menu.addAction(QStringLiteral("Hide %1%2").arg(n, parts), this, [this, sel] { for (int p : sel) m_view->setPartVisible(p, false); rebuildPartsPanel(); });
        menu.addAction(MenuText::prompts(MenuText::exportLabel(QStringLiteral("Export"), sel.size(), sel.size() == 1 ? m_shownGeo.parts.value(*sel.begin()).name : QString(), QStringLiteral("part"))), this, [this] { exportParts(); });
        menu.addAction(sel.size() == 1 ? MenuText::kCopyName : MenuText::withRows(MenuText::kCopyName, sel.size()), this, [this, sel] {
            QStringList names; for (int i = 0; i < m_shownGeo.parts.size(); ++i) if (sel.contains(i)) names << m_shownGeo.parts[i].name;
            qApp->clipboard()->setText(names.join(QLatin1Char('\n')));
        });
        if (sel.size() == 1) {
            const Part& p = m_shownGeo.parts[*sel.begin()];
            const QString mat = p.materialIndex >= 0 && p.materialIndex < m_shownGeo.materialNames.size() ? m_shownGeo.materialNames[p.materialIndex] : QString();
            QAction* a = menu.addAction(MenuText::withCopyValue(MenuText::kCopyMaterial, QFileInfo(mat).fileName()), this, [mat] { qApp->clipboard()->setText(mat); });
            a->setEnabled(!mat.isEmpty());
        }
        menu.addSeparator();
    }
    if (!m_view->hiddenParts().isEmpty())
        menu.addAction(QStringLiteral("%1 (%2 hidden)").arg(MenuText::kShowAll).arg(m_view->hiddenParts().size()), this, [this] { m_view->setAllPartsVisible(true); rebuildPartsPanel(); });
    menu.addAction(QStringLiteral("Select all parts"), this, [this, total] { QSet<int> all; for (int i = 0; i < total; ++i) all.insert(i); m_view->setSelectedParts(all); });
    if (total > 1 && !sel.isEmpty() && int(sel.size()) < total)   // E2: invert the current part selection
        menu.addAction(QStringLiteral("Invert selection"), this, [this, total, sel] {
            QSet<int> inv; for (int i = 0; i < total; ++i) if (!sel.contains(i)) inv.insert(i);
            m_view->setSelectedParts(inv);
        });
    if (m_view->hiddenParts().size() < total)   // E2: hide every part at once
        menu.addAction(QStringLiteral("Hide all parts"), this, [this, total] { for (int i = 0; i < total; ++i) m_view->setPartVisible(i, false); rebuildPartsPanel(); });
    if (!sel.isEmpty()) menu.addAction(QStringLiteral("Clear selection"), this, [this] { m_view->clearSelection(); });
    emit partMenuAboutToShow(&menu, sel);
    if (menudump::capture(&menu, QStringLiteral("models/parts"))) return;
    menu.exec(globalPos);
}

// ── Grid ─────────────────────────────────────────────────────────────────────────────────────
void ModelsTab::setGridView(bool on)
{
    m_model->setGridMode(on);
    if (on) {
        m_model->setIconProvider([this](const AssetId& id) {
            if (m_thumbs->has(id, 192)) return m_thumbs->cached(id, 192);
            m_thumbs->request(id, 192);
            return QPixmap();
        });
        setIconPx(m_iconPx);
        m_list->hide(); m_grid->show(); m_grid->setFocus();
        schedulePrerender();   // fill the first screenful without waiting for per-tile paints
    } else {
        m_model->setIconProvider(nullptr);
        m_grid->hide(); m_list->show(); m_list->setFocus();
    }
    if (m_gridBtn->isChecked() != on) { QSignalBlocker b(m_gridBtn); m_gridBtn->setChecked(on); }
}

void ModelsTab::schedulePrerender()
{
    if (m_prerenderTimer && m_gridBtn && m_gridBtn->isChecked()) m_prerenderTimer->start();
}

// Queue thumbnail renders for the rows in (and just past) the grid's viewport, so the visible window
// fills proactively rather than one tile at a time as it happens to paint. The renderer dedupes
// against what is cached or already queued, so calling this on every scroll/filter settle is cheap.
void ModelsTab::prerenderVisible()
{
    if (!m_grid || !m_grid->isVisible() || !m_thumbs) return;
    const int n = m_model->rowCount();
    if (n <= 0) return;
    const QRect vp = m_grid->viewport()->rect();
    const QModelIndex top = m_grid->indexAt(vp.topLeft());
    const QModelIndex bot = m_grid->indexAt(vp.bottomRight());
    int r0 = top.isValid() ? top.row() : 0;
    int r1 = bot.isValid() ? bot.row() : qMin(n - 1, r0 + 64);
    if (r1 < r0) r1 = qMin(n - 1, r0 + 64);
    // A margin either side so a small scroll never shows a blank tile while its render is queued.
    r0 = qMax(0, r0 - 8);
    r1 = qMin(n - 1, r1 + 24);
    QVector<AssetId> ids; ids.reserve(r1 - r0 + 1);
    for (int r = r0; r <= r1; ++r) { const AssetId id = m_model->idAt(r); if (id.isValid()) ids << id; }
    if (!ids.isEmpty()) m_thumbs->prerender(ids, 192);
}

void ModelsTab::setIconPx(int px)
{
    m_iconPx = qBound(64, px, 320);
    m_model->setIconPx(m_iconPx);
    m_grid->setIconSize(QSize(m_iconPx, m_iconPx));
    m_grid->setGridSize(CardDelegate::cellFor(m_iconPx));
}

bool ModelsTab::eventFilter(QObject* obj, QEvent* ev)
{
    // Load-gated arrow scrub: while a model is loading (or about to — the settle debounce is pending),
    // hold the next Up/Down instead of racing the selection ahead of the decode, so holding the key
    // steps through EVERY model (each shown before the next loads) rather than coalescing to wherever
    // the cursor settles.
    if ((obj == m_list || obj == m_grid) && ev->type() == QEvent::KeyPress
        && (m_loading || (m_loadDebounce && m_loadDebounce->isActive()))) {
        const int k = static_cast<QKeyEvent*>(ev)->key();
        if (k == Qt::Key_Down || k == Qt::Key_Up) { m_navHeld = (k == Qt::Key_Down) ? 1 : -1; return true; }
    }
    const bool onGrid = (m_grid && obj == m_grid->viewport());
    if (onGrid && ev->type() == QEvent::Wheel) {
        auto* we = static_cast<QWheelEvent*>(ev);
        if (we->modifiers() & Qt::ControlModifier) { setIconPx(m_iconPx + (we->angleDelta().y() > 0 ? 14 : -14)); return true; }
        return false;
    }
    if (onGrid && ev->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(ev);
        const QModelIndex idx = m_grid->indexAt(m_grid->viewport()->mapFromGlobal(he->globalPos()));
        if (idx.isValid()) {
            const AssetRow row = m_model->rowAt(idx.row());
            hover::Preview::instance().requestLazy(QStringLiteral("model:%1").arg(row.id.value), he->globalPos(), [this, row] {
                hover::Content c;
                c.image = m_thumbs->cached(row.id, 192);
                c.name = row.inGameName.isEmpty() ? QFileInfo(row.name).fileName() : row.inGameName;
                c.path = row.name;
                if (row.size) c.info << QLocale().formattedDataSize(qint64(row.size));
                return c;
            });
            return true;
        }
    }
    if (onGrid && ev->type() == QEvent::Leave) hover::Preview::instance().cancel();
    // A7: rich hover preview on the detail-table rows (parity with the grid's).
    if (m_list && obj == m_list->viewport() && ev->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(ev);
        const QModelIndex idx = m_list->indexAt(m_list->viewport()->mapFromGlobal(he->globalPos()));
        if (idx.isValid()) {
            const AssetRow row = m_model->rowAt(idx.row());
            hover::Preview::instance().requestLazy(QStringLiteral("model:%1").arg(row.id.value), he->globalPos(), [this, row] {
                hover::Content c;
                c.image = m_thumbs->cached(row.id, 192);
                if (c.image.isNull()) m_thumbs->request(row.id, 192);
                c.name = row.inGameName.isEmpty() ? QFileInfo(row.name).fileName() : row.inGameName;
                c.path = row.name;
                if (row.size) c.info << QLocale().formattedDataSize(qint64(row.size));
                return c;
            });
            return true;
        }
    }
    if (m_list && obj == m_list->viewport() && ev->type() == QEvent::Leave) hover::Preview::instance().cancel();
    // B2: wheel over the Channel box cycles channels (with a status note).
    if (obj == m_channel && ev->type() == QEvent::Wheel) {
        auto* we = static_cast<QWheelEvent*>(ev);
        const int n = m_channel->count();
        if (n > 0) {
            const int i = (m_channel->currentIndex() + (we->angleDelta().y() > 0 ? -1 : 1) + n) % n;
            m_channel->setCurrentIndex(i);
            StatusLine::instance().report(this, QStringLiteral("Channel: %1").arg(m_channel->currentText()));
        }
        return true;
    }
    // E4: wheel over the timeline steps the playhead ~one frame.
    if (obj == m_timeline && ev->type() == QEvent::Wheel && m_view->hasClip()) {
        auto* we = static_cast<QWheelEvent*>(ev);
        const float dur = m_view->clipDuration();
        if (dur > 0.0f) {
            setPlaying(false);
            const float t = qBound(0.0f, m_view->time() + (we->angleDelta().y() > 0 ? 1.0f : -1.0f) / 30.0f, dur);
            m_view->setTime(t);
            { QSignalBlocker b(m_timeline); m_timeline->setValue(int(t / dur * 1000.0f)); }
            m_timeLbl->setText(QStringLiteral("%1 / %2s").arg(t, 0, 'f', 2).arg(dur, 0, 'f', 2));
        }
        return true;
    }
    // E3: part-visibility hotkeys on the viewport — H hide selected, Shift+H isolate, Alt+H show all.
    if (obj == m_view && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_H) {
            const QSet<int> sel = m_view->selectedParts();
            if (ke->modifiers() & Qt::AltModifier) { m_view->setAllPartsVisible(true); rebuildPartsPanel(); StatusLine::instance().report(this, QStringLiteral("All parts shown.")); return true; }
            if (!sel.isEmpty() && (ke->modifiers() & Qt::ShiftModifier)) { m_view->isolateParts(sel); rebuildPartsPanel(); StatusLine::instance().report(this, QStringLiteral("Isolated %1 part%2.").arg(sel.size()).arg(sel.size() == 1 ? QString() : QStringLiteral("s"))); return true; }
            if (!sel.isEmpty()) { for (int p : sel) m_view->setPartVisible(p, false); rebuildPartsPanel(); StatusLine::instance().report(this, QStringLiteral("Hid %1 part%2.").arg(sel.size()).arg(sel.size() == 1 ? QString() : QStringLiteral("s"))); return true; }
        }
    }
    return BrowserTab::eventFilter(obj, ev);
}

}  // namespace core
