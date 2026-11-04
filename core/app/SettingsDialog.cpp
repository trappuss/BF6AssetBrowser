#include "core/app/SettingsDialog.h"
#include "core/app/AppLog.h"
#include "core/app/AppPaths.h"
#include "core/app/CacheMaint.h"
#include "core/app/CoreSettings.h"
#include "core/app/Density.h"
#include "core/app/ExportNotifier.h"
#include "core/app/FontScale.h"
#include "core/app/Hotkeys.h"
#include "core/ui/ExportCapture.h"
#include "core/ui/TipBar.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QShowEvent>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>

namespace core {

namespace {

QString humanBytes(qint64 n)
{
    if (n <= 0) return QStringLiteral("empty");
    if (n < 1024) return QStringLiteral("%1 B").arg(n);
    if (n < 1024 * 1024) return QStringLiteral("%1 KB").arg(n / 1024);
    return QStringLiteral("%1 MB").arg(n / (1024 * 1024));
}

QLabel* note(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setStyleSheet(QStringLiteral("color: palette(mid);"));
    return l;
}

// A texture-resolution picker. Value (itemData) is the max side in px; 0 = native/highest. Shared by
// the export, 3D-preview and thumbnail settings so they read the same way.
QComboBox* texResCombo(QWidget* parent, bool highestLabel)
{
    auto* c = new QComboBox(parent);
    c->addItem(highestLabel ? QStringLiteral("Highest (native, up to 4K)") : QStringLiteral("Native (full)"), 0);
    for (int px : {4096, 2048, 1024, 512, 256}) c->addItem(QStringLiteral("%1 px").arg(px), px);
    return c;
}
void setTexRes(QComboBox* c, int px) { const int i = c->findData(px); c->setCurrentIndex(i < 0 ? 0 : i); }

// ── Interface ─────────────────────────────────────────────────────────────────────────────────
struct InterfaceWidgets {
    QCheckBox* rememberLayout = nullptr;
    QCheckBox* rememberSession = nullptr;
    QComboBox* density = nullptr;
    QComboBox* fontScale = nullptr;
    QString densityOnOpen; int fontOnOpen = 0;
    QDoubleSpinBox* hoverDelay = nullptr;
    QCheckBox* hoverPreview = nullptr;
    QCheckBox* hoverZoom = nullptr;
    QSpinBox* hoverPx = nullptr;
    QCheckBox* logFile = nullptr;
    QCheckBox* osNotify = nullptr;
    QComboBox* previewTex = nullptr;
    QComboBox* thumbTex = nullptr;
};

SettingsPage interfacePage()
{
    auto w = std::make_shared<InterfaceWidgets>();
    SettingsPage p;
    p.title = QStringLiteral("Interface");
    p.order = 1;
    p.liveKeyPrefixes = {QStringLiteral("ui/")};
    p.build = [w](QWidget* parent) -> QWidget* {
        auto* page = new QWidget(parent);
        auto* v = new QVBoxLayout(page);

        auto* start = new QGroupBox(QStringLiteral("Startup && layout"), page);
        auto* sf = new QFormLayout(start);
        w->rememberLayout = new QCheckBox(QStringLiteral("Remember window, splitter and panel layout"), start);
        w->rememberLayout->setToolTip(QStringLiteral("The window, the panels and the splitters are one layout behind one toggle. "
                                                     "Off: the tool opens the same way every time."));
        sf->addRow(w->rememberLayout);
        w->rememberSession = new QCheckBox(QStringLiteral("Reopen on the tab that was in front"), start);
        sf->addRow(w->rememberSession);
        w->density = new QComboBox(start);
        for (const QString& k : density::keys()) w->density->addItem(density::label(k), k);
        w->density->setToolTip(QStringLiteral("How tightly every list and tree packs its rows. Applies at once; Cancel puts it back."));
        sf->addRow(QStringLiteral("Row density:"), w->density);
        w->fontScale = new QComboBox(start);
        for (int pct : fontscale::steps()) w->fontScale->addItem(fontscale::label(pct), pct);
        w->fontScale->setToolTip(QStringLiteral("The application's text size, as a percentage of the platform default. Applies at once."));
        sf->addRow(QStringLiteral("Text size:"), w->fontScale);
        auto* tips = new QPushButton(QStringLiteral("Show the tips again"), start);
        tips->setToolTip(QStringLiteral("Every dismissed hint strip comes back."));
        QObject::connect(tips, &QPushButton::clicked, page, [] { TipBar::resetAll(); });
        sf->addRow(QString(), tips);
        v->addWidget(start);

        auto* hover = new QGroupBox(QStringLiteral("On-hover previews"), page);
        auto* hf = new QFormLayout(hover);
        w->hoverPreview = new QCheckBox(QStringLiteral("Show a preview card when the pointer rests on a row"), hover);
        hf->addRow(w->hoverPreview);
        w->hoverDelay = new QDoubleSpinBox(hover); w->hoverDelay->setRange(0.0, 5.0); w->hoverDelay->setSingleStep(0.1); w->hoverDelay->setSuffix(QStringLiteral(" s"));
        hf->addRow(QStringLiteral("Dwell before it appears:"), w->hoverDelay);
        w->hoverPx = new QSpinBox(hover); w->hoverPx->setRange(64, 1024); w->hoverPx->setSingleStep(32); w->hoverPx->setSuffix(QStringLiteral(" px"));
        hf->addRow(QStringLiteral("Starting size:"), w->hoverPx);
        w->hoverZoom = new QCheckBox(QStringLiteral("Mouse wheel resizes the preview while it is up"), hover);
        hf->addRow(w->hoverZoom);
        v->addWidget(hover);

        auto* tex = new QGroupBox(QStringLiteral("Texture resolution"), page);
        auto* tf = new QFormLayout(tex);
        tf->addRow(note(QStringLiteral("How large textures are decoded for the LIVE views — kept modest so the grid and "
                                       "viewport stay fast. The size textures are EXPORTED at is a separate setting on the "
                                       "Export page (default: highest)."), tex));
        w->thumbTex = texResCombo(tex, false);
        w->thumbTex->setToolTip(QStringLiteral("Textures decoded for the grid thumbnails. They render small, so a low value is "
                                               "plenty and fastest."));
        tf->addRow(QStringLiteral("Grid thumbnails:"), w->thumbTex);
        w->previewTex = texResCombo(tex, false);
        w->previewTex->setToolTip(QStringLiteral("Textures decoded for the 3D preview viewport. Higher looks crisper but each "
                                                 "model takes longer to load."));
        tf->addRow(QStringLiteral("3D preview:"), w->previewTex);
        v->addWidget(tex);

        auto* diag = new QGroupBox(QStringLiteral("Diagnostics"), page);
        auto* df = new QFormLayout(diag);
        w->logFile = new QCheckBox(QStringLiteral("Write a log file"), diag);
        w->logFile->setToolTip(QStringLiteral("Takes effect at once — the reason to want the file is that something is misbehaving right now."));
        df->addRow(w->logFile);
        auto* path = new QLabel(QDir::toNativeSeparators(log::filePath()), diag);
        path->setTextInteractionFlags(Qt::TextSelectableByMouse);
        df->addRow(QStringLiteral("Log file:"), path);
        w->osNotify = new QCheckBox(QStringLiteral("Desktop notification when an export finishes"), diag);
        w->osNotify->setToolTip(QStringLiteral("The in-window toast is invisible precisely when it matters — a long bulk run you switched away from."));
        df->addRow(w->osNotify);
        v->addWidget(diag);
        v->addStretch(1);

        // Density and text size apply LIVE (FOX): the page is the preview.
        QObject::connect(w->density, &QComboBox::currentIndexChanged, page, [w] { density::set(w->density->currentData().toString()); });
        QObject::connect(w->fontScale, &QComboBox::currentIndexChanged, page, [w] { fontscale::set(w->fontScale->currentData().toInt()); });
        return page;
    };
    p.reload = [w] {
        if (!w->rememberLayout) return;
        w->rememberLayout->setChecked(settings::rememberLayout());
        w->rememberSession->setChecked(settings::rememberSession());
        w->densityOnOpen = density::current();
        w->fontOnOpen = fontscale::current();
        { QSignalBlocker b(w->density); const int i = w->density->findData(w->densityOnOpen); w->density->setCurrentIndex(i < 0 ? 1 : i); }
        { QSignalBlocker b(w->fontScale); const int i = w->fontScale->findData(w->fontOnOpen); w->fontScale->setCurrentIndex(i < 0 ? 1 : i); }
        w->hoverPreview->setChecked(settings::value(QStringLiteral("hover/imagePreview"), true).toBool());
        w->hoverDelay->setValue(settings::value(QStringLiteral("hover/delaySec"), 0.5).toDouble());
        w->hoverPx->setValue(settings::value(QStringLiteral("hover/previewPx"), 256).toInt());
        w->hoverZoom->setChecked(settings::value(QStringLiteral("hover/scrollZoom"), true).toBool());
        w->logFile->setChecked(log::fileLogging());
        w->osNotify->setChecked(settings::osNotify());
        setTexRes(w->thumbTex,   settings::value(QStringLiteral("ui/thumbnailTextureRes"), 512).toInt());
        setTexRes(w->previewTex, settings::value(QStringLiteral("ui/previewTextureRes"), 0).toInt());   // default 0 = full/native
    };
    p.apply = [w] {
        settings::setRememberLayout(w->rememberLayout->isChecked());
        settings::setRememberSession(w->rememberSession->isChecked());
        settings::set(QStringLiteral("hover/imagePreview"), w->hoverPreview->isChecked());
        settings::set(QStringLiteral("hover/delaySec"), w->hoverDelay->value());
        settings::set(QStringLiteral("hover/previewPx"), w->hoverPx->value());
        settings::set(QStringLiteral("hover/scrollZoom"), w->hoverZoom->isChecked());
        log::setFileLogging(w->logFile->isChecked());
        settings::setOsNotify(w->osNotify->isChecked());
        settings::set(QStringLiteral("ui/thumbnailTextureRes"), w->thumbTex->currentData().toInt());
        settings::set(QStringLiteral("ui/previewTextureRes"), w->previewTex->currentData().toInt());
    };
    p.revert = [w] {
        if (!w->densityOnOpen.isEmpty()) density::set(w->densityOnOpen);
        if (w->fontOnOpen > 0) fontscale::set(w->fontOnOpen);
    };
    p.resetGroups = {QStringLiteral("hover"), QStringLiteral("ui"), QStringLiteral("interface")};
    return p;
}

// ── Export ────────────────────────────────────────────────────────────────────────────────────
struct ExportWidgets {
    QDoubleSpinBox* unitScale = nullptr;
    QComboBox* format = nullptr;
    QComboBox* texRes = nullptr;
    QComboBox* animations = nullptr;
    QCheckBox *skeleton = nullptr, *attachments = nullptr, *yaw180 = nullptr, *embed = nullptr, *loose = nullptr, *normalZ = nullptr;
    QCheckBox *showPrompt = nullptr, *reuseLast = nullptr;
    QSpinBox* workers = nullptr;
    QComboBox* layoutMode = nullptr;
    QComboBox* after = nullptr;
    QLineEdit* afterCmd = nullptr;
    QLineEdit *modelTpl = nullptr, *textureTpl = nullptr;
    QSpinBox* imageScale = nullptr;
    QCheckBox *imageTransparent = nullptr, *imageCrop = nullptr, *imageShowPrompt = nullptr;
    QSpinBox *gifFps = nullptr, *gifFrames = nullptr, *gifColors = nullptr, *gifTarget = nullptr;
    QComboBox* gifScale = nullptr;
    QCheckBox *gifDither = nullptr, *gifTransparent = nullptr, *gifCrop = nullptr, *gifOptimize = nullptr;
};

SettingsPage exportPage(AppPlugin& plugin)
{
    auto w = std::make_shared<ExportWidgets>();
    SettingsPage p;
    p.title = QStringLiteral("Export");
    p.order = 3;
    p.build = [w, &plugin](QWidget* parent) -> QWidget* {
        auto* sub = new QTabWidget(parent);
        sub->tabBar()->setElideMode(Qt::ElideNone);
        sub->tabBar()->setExpanding(false);
        auto scroll = [sub](QWidget* content) {
            auto* sc = new QScrollArea(sub); sc->setWidget(content); sc->setWidgetResizable(true); sc->setFrameShape(QFrame::NoFrame); return sc; };

        {   // Models
            auto* pg = new QWidget; auto* form = new QFormLayout(pg);
            w->unitScale = new QDoubleSpinBox(pg); w->unitScale->setDecimals(5); w->unitScale->setRange(0.00001, 1000.0); w->unitScale->setSingleStep(0.001);
            w->unitScale->setToolTip(plugin.unitScaleTip());
            form->addRow(QStringLiteral("Unit scale (m per unit):"), w->unitScale);
            w->format = new QComboBox(pg); w->format->addItems({QStringLiteral("Single file (.glb)"), QStringLiteral("Text + buffer (.gltf + .bin)")});
            form->addRow(QStringLiteral("Format:"), w->format);
            w->texRes = texResCombo(pg, true);
            w->texRes->setToolTip(QStringLiteral("Resolution of the textures written into the export. Highest uses each map's "
                                                 "native size, including the game's streamed 4K HD pack. Lower re-scales down."));
            form->addRow(QStringLiteral("Texture resolution:"), w->texRes);
            w->animations = new QComboBox(pg); w->animations->addItems({QStringLiteral("All clips"), QStringLiteral("None")});
            w->animations->setToolTip(QStringLiteral("Default animation scope. The per-export dialog can also pick the current clip only."));
            form->addRow(QStringLiteral("Animations:"), w->animations);
            w->skeleton = new QCheckBox(QStringLiteral("Include skeleton"), pg); form->addRow(QString(), w->skeleton);
            w->attachments = new QCheckBox(QStringLiteral("Include attachments (assembled)"), pg);
            w->attachments->setToolTip(QStringLiteral("Bake a model's attachments onto their bones so the export matches the assembled view."));
            form->addRow(QString(), w->attachments);
            w->yaw180 = new QCheckBox(QStringLiteral("Rotate 180° about up (face camera / thumbnails)"), pg); form->addRow(QString(), w->yaw180);
            w->embed = new QCheckBox(QStringLiteral("Embed textures"), pg); form->addRow(QString(), w->embed);
            w->loose = new QCheckBox(QStringLiteral("Loose textures (.gltf → sibling .png files)"), pg);
            w->loose->setToolTip(QStringLiteral("Only applies to .gltf export; a .glb is always self-contained."));
            form->addRow(QString(), w->loose);
            w->normalZ = new QCheckBox(QStringLiteral("Reconstruct normal-map Z"), pg); form->addRow(QString(), w->normalZ);
            w->showPrompt = new QCheckBox(QStringLiteral("Show export options each time"), pg);
            w->showPrompt->setToolTip(QStringLiteral("When off, exports skip the options dialog and use these saved defaults."));
            form->addRow(QString(), w->showPrompt);
            w->reuseLast = new QCheckBox(QStringLiteral("Export to last-used folder (skip file picker)"), pg);
            form->addRow(QString(), w->reuseLast);
            sub->addTab(scroll(pg), QStringLiteral("Models"));
        }
        {   // Images && GIFs
            auto* pg = new QWidget; auto* v = new QVBoxLayout(pg);
            auto* img = new QGroupBox(QStringLiteral("Preview image (PNG)"), pg); auto* f1 = new QFormLayout(img);
            w->imageScale = new QSpinBox(img); w->imageScale->setRange(25, 400); w->imageScale->setSingleStep(25); w->imageScale->setSuffix(QStringLiteral(" %"));
            w->imageScale->setToolTip(QStringLiteral("Render scale. 100% is on-screen resolution; higher RE-RENDERS larger (true supersampling)."));
            f1->addRow(QStringLiteral("Render scale:"), w->imageScale);
            w->imageTransparent = new QCheckBox(QStringLiteral("Transparent background"), img); f1->addRow(QString(), w->imageTransparent);
            w->imageCrop = new QCheckBox(QStringLiteral("Crop to model (needs transparent background)"), img); f1->addRow(QString(), w->imageCrop);
            w->imageShowPrompt = new QCheckBox(QStringLiteral("Show image options each time"), img); f1->addRow(QString(), w->imageShowPrompt);
            v->addWidget(img);
            auto* gif = new QGroupBox(QStringLiteral("Animated GIF"), pg); auto* f2 = new QFormLayout(gif);
            w->gifFps = new QSpinBox(gif); w->gifFps->setRange(1, 60); w->gifFps->setSuffix(QStringLiteral(" fps"));
            f2->addRow(QStringLiteral("Turntable frame rate:"), w->gifFps);
            w->gifFrames = new QSpinBox(gif); w->gifFrames->setRange(8, 240);
            f2->addRow(QStringLiteral("Frames per turn:"), w->gifFrames);
            w->gifScale = new QComboBox(gif); for (int s : {25, 50, 75, 100}) w->gifScale->addItem(QStringLiteral("%1%").arg(s), s);
            f2->addRow(QStringLiteral("Scale:"), w->gifScale);
            w->gifColors = new QSpinBox(gif); w->gifColors->setRange(2, 256);
            f2->addRow(QStringLiteral("Max colours:"), w->gifColors);
            w->gifDither = new QCheckBox(QStringLiteral("Dither"), gif); f2->addRow(QString(), w->gifDither);
            w->gifTransparent = new QCheckBox(QStringLiteral("Transparent background"), gif); f2->addRow(QString(), w->gifTransparent);
            w->gifCrop = new QCheckBox(QStringLiteral("Crop to model"), gif); f2->addRow(QString(), w->gifCrop);
            w->gifOptimize = new QCheckBox(QStringLiteral("Optimise to target size"), gif);
            w->gifTarget = new QSpinBox(gif); w->gifTarget->setRange(1, 200); w->gifTarget->setSuffix(QStringLiteral(" MB"));
            QObject::connect(w->gifOptimize, &QCheckBox::toggled, w->gifTarget, &QSpinBox::setEnabled);
            f2->addRow(w->gifOptimize, w->gifTarget);
            v->addWidget(gif);
            v->addStretch(1);
            sub->addTab(scroll(pg), QStringLiteral("Images && GIFs"));
        }
        {   // Bulk && after
            auto* pg = new QWidget; auto* form = new QFormLayout(pg);
            w->workers = new QSpinBox(pg); w->workers->setRange(0, 32); w->workers->setSpecialValueText(QStringLiteral("Auto"));
            w->workers->setToolTip(QStringLiteral("Worker threads for bulk extraction. 0 = Auto (one per core)."));
            form->addRow(QStringLiteral("Bulk worker threads:"), w->workers);
            w->layoutMode = new QComboBox(pg);
            for (const layout::Mode& m : plugin.layoutSpec().modes) { w->layoutMode->addItem(m.label, m.id); w->layoutMode->setItemData(w->layoutMode->count() - 1, m.hint, Qt::ToolTipRole); }
            w->layoutMode->setToolTip(QStringLiteral("How bulk/multi-select exports group files into folders. Single-model export ignores this."));
            form->addRow(QStringLiteral("Bulk folder layout:"), w->layoutMode);
            w->after = new QComboBox(pg);
            w->after->addItem(QStringLiteral("Nothing"), QStringLiteral("none"));
            w->after->addItem(QStringLiteral("Open the folder"), QStringLiteral("reveal"));
            w->after->addItem(QStringLiteral("Run a command"), QStringLiteral("command"));
            form->addRow(QStringLiteral("After an export:"), w->after);
            w->afterCmd = new QLineEdit(pg);
            w->afterCmd->setPlaceholderText(QStringLiteral("gltfpack -i {{File}} -o {{Folder}}/packed.glb"));
            w->afterCmd->setToolTip(QStringLiteral("{{File}} and {{Folder}} are filled in, quoted. Runs detached."));
            form->addRow(QStringLiteral("Command:"), w->afterCmd);
            QObject::connect(w->after, &QComboBox::currentIndexChanged, pg, [w] { w->afterCmd->setEnabled(w->after->currentData().toString() == QLatin1String("command")); });
            sub->addTab(scroll(pg), QStringLiteral("Bulk && after"));
        }
        {   // File names (bulk/multi-select exports; a single export writes exactly where you point it)
            auto* pg = new QWidget; auto* form = new QFormLayout(pg);
            w->modelTpl = new QLineEdit(pg); w->textureTpl = new QLineEdit(pg);
            form->addRow(QStringLiteral("Model file name:"), w->modelTpl);
            form->addRow(QStringLiteral("Texture file name:"), w->textureTpl);
            form->addRow(note(QStringLiteral("Placeholders: {{FileName}} the asset's own name · {{Id}} its %1 · {{Collection}} where the game "
                                             "has one. Path separators, trailing dots and reserved device names are sanitised; an empty result "
                                             "falls back to the file name.").arg(plugin.nouns().id), pg));
            sub->addTab(scroll(pg), QStringLiteral("File names"));
        }
        return sub;
    };
    p.reload = [w, &plugin] {
        if (!w->unitScale) return;
        const ExportOptions o = settings::exportOptions(plugin.exportDefaults());
        w->unitScale->setValue(o.unitScale);
        w->format->setCurrentIndex(o.gltfSeparate ? 1 : 0);
        setTexRes(w->texRes, o.maxTextureSize);
        w->animations->setCurrentIndex(o.includeAnimations ? 0 : 1);
        w->skeleton->setChecked(o.includeSkeleton);
        w->attachments->setChecked(o.includeAttachments);
        w->yaw180->setChecked(o.yawDegrees == 180.0f);
        w->embed->setChecked(o.embedTextures);
        w->loose->setChecked(o.looseTextures);
        w->normalZ->setChecked(o.reconstructNormalZ);
        w->showPrompt->setChecked(settings::exportShowPrompt());
        w->reuseLast->setChecked(settings::exportReuseLastDir());
        w->imageScale->setValue(settings::imageScalePercent());
        w->imageTransparent->setChecked(settings::imageTransparentBg());
        w->imageCrop->setChecked(settings::imageCropToModel());
        w->imageShowPrompt->setChecked(settings::imageShowPrompt());
        const capture::GifOptions g = capture::loadGifOptions();
        w->gifFps->setValue(qBound(1, g.fps, 60));
        w->gifFrames->setValue(qBound(8, g.turntableFrames, 240));
        { const int i = w->gifScale->findData(qBound(25, g.scalePercent, 100)); w->gifScale->setCurrentIndex(i < 0 ? w->gifScale->count() - 1 : i); }
        w->gifColors->setValue(qBound(2, g.maxColors, 256));
        w->gifDither->setChecked(g.dither);
        w->gifTransparent->setChecked(g.transparentBg);
        w->gifCrop->setChecked(g.cropToModel);
        w->gifOptimize->setChecked(g.optimize);
        w->gifTarget->setValue(qBound(1, g.targetMB, 200));
        w->gifTarget->setEnabled(g.optimize);
        w->workers->setValue(settings::bulkWorkers());
        { const QString mode = layout::resolveMode(plugin.layoutSpec(), settings::value(QStringLiteral("export/folderLayout")).toString());
          const int i = w->layoutMode->findData(mode); w->layoutMode->setCurrentIndex(i < 0 ? 0 : i); }
        w->modelTpl->setText(settings::value(QStringLiteral("export/modelNameTemplate"), QStringLiteral("{{FileName}}")).toString());
        w->textureTpl->setText(settings::value(QStringLiteral("export/textureNameTemplate"), QStringLiteral("{{FileName}}")).toString());
        { const int i = w->after->findData(ExportNotifier::afterMode()); w->after->setCurrentIndex(i < 0 ? 0 : i); }
        w->afterCmd->setText(ExportNotifier::afterCommand());
        w->afterCmd->setEnabled(ExportNotifier::afterMode() == QLatin1String("command"));
    };
    p.apply = [w, &plugin] {
        ExportOptions o = settings::exportOptions(plugin.exportDefaults());
        o.unitScale = float(w->unitScale->value());
        o.gltfSeparate = w->format->currentIndex() == 1;
        o.maxTextureSize = w->texRes->currentData().toInt();
        o.includeAnimations = w->animations->currentIndex() == 0;
        o.includeSkeleton = w->skeleton->isChecked();
        o.includeAttachments = w->attachments->isChecked();
        o.yawDegrees = w->yaw180->isChecked() ? 180.0f : 0.0f;
        o.embedTextures = w->embed->isChecked();
        o.looseTextures = w->loose->isChecked();
        o.reconstructNormalZ = w->normalZ->isChecked();
        settings::saveExportOptions(o);
        settings::setExportShowPrompt(w->showPrompt->isChecked());
        settings::setExportReuseLastDir(w->reuseLast->isChecked());
        settings::setImageScalePercent(w->imageScale->value());
        settings::setImageTransparentBg(w->imageTransparent->isChecked());
        settings::setImageCropToModel(w->imageCrop->isChecked());
        settings::setImageShowPrompt(w->imageShowPrompt->isChecked());
        capture::GifOptions g;
        g.fps = w->gifFps->value(); g.turntableFrames = w->gifFrames->value();
        g.scalePercent = w->gifScale->currentData().toInt(); g.maxColors = w->gifColors->value();
        g.dither = w->gifDither->isChecked(); g.transparentBg = w->gifTransparent->isChecked();
        g.cropToModel = w->gifCrop->isChecked(); g.optimize = w->gifOptimize->isChecked(); g.targetMB = w->gifTarget->value();
        capture::saveGifOptions(g);
        settings::setBulkWorkers(w->workers->value());
        // The layout is a STABLE STRING; Flat (empty) removes the key so a later default change is picked up.
        const QString mode = w->layoutMode->currentData().toString();
        if (mode.isEmpty()) QSettings().remove(QStringLiteral("export/folderLayout")); else settings::set(QStringLiteral("export/folderLayout"), mode);
        ExportNotifier::setAfter(w->after->currentData().toString(), w->afterCmd->text().trimmed());
        auto tpl = [](QLineEdit* e, const char* key) {
            const QString t = e->text().trimmed();
            if (t.isEmpty() || t == QLatin1String("{{FileName}}")) QSettings().remove(QLatin1String(key)); else settings::set(QLatin1String(key), t);
        };
        tpl(w->modelTpl, "export/modelNameTemplate");
        tpl(w->textureTpl, "export/textureNameTemplate");
    };
    return p;
}

// ── Hotkeys ───────────────────────────────────────────────────────────────────────────────────
struct HotkeyWidgets {
    QVector<QPair<QString, QKeySequenceEdit*>> edits;
    QLabel* warning = nullptr;
    void checkClashes()
    {
        if (!warning) return;
        QHash<QString, QStringList> byKey;
        for (const auto& row : edits) {
            const QString seq = row.second->keySequence().toString(QKeySequence::PortableText);
            if (seq.isEmpty()) continue;   // unbound is never a clash
            const hotkeys::Def* d = hotkeys::find(row.first);
            byKey[seq].append(d ? d->label : row.first);
        }
        QStringList clashes;
        for (auto it = byKey.constBegin(); it != byKey.constEnd(); ++it)
            if (it.value().size() > 1) clashes << QStringLiteral("%1 — %2").arg(it.key(), it.value().join(QStringLiteral(" and ")));
        clashes.sort();   // a QHash reshuffles per run; a warning that reorders itself reads as a new warning
        warning->setText(clashes.isEmpty() ? QString()
            : QStringLiteral("Two actions share a shortcut, which makes Qt fire NEITHER of them:\n%1").arg(clashes.join(QLatin1Char('\n'))));
        warning->setVisible(!clashes.isEmpty());
    }
};

SettingsPage hotkeysPage(AppPlugin& plugin)
{
    auto w = std::make_shared<HotkeyWidgets>();
    SettingsPage p;
    p.title = QStringLiteral("Hotkeys");
    p.order = 4;
    p.resetGroups = {QStringLiteral("hotkeys")};
    p.build = [w, &plugin](QWidget* parent) -> QWidget* {
        auto* keys = new QWidget(parent);
        auto* kv = new QVBoxLayout(keys);
        kv->addWidget(note(QStringLiteral("Click a box and press the combination you want. Clear a box to unbind that action — "
                                          "an unbound action is a real choice, and the GIF exports ship that way because they are slow."), keys));
        auto* form = new QFormLayout();
        w->edits.clear();
        for (const hotkeys::Def& d : hotkeys::defs()) {
            auto* edit = new QKeySequenceEdit(keys);
            edit->setToolTip(d.hint);
            auto* label = new QLabel(d.label, keys);
            label->setToolTip(d.hint);
            form->addRow(label, edit);
            w->edits.append({d.key, edit});
        }
        kv->addLayout(form);
        auto* reset = new QPushButton(QStringLiteral("Put every shortcut back to its default"), keys);
        QObject::connect(reset, &QPushButton::clicked, keys, [w] {
            for (auto& row : w->edits)
                if (const hotkeys::Def* d = hotkeys::find(row.first))
                    row.second->setKeySequence(d->def.isEmpty() ? QKeySequence() : QKeySequence(d->def));
        });
        kv->addWidget(reset);
        auto* sheetRow = new QHBoxLayout();
        auto* copySheet = new QPushButton(QStringLiteral("Copy cheat sheet"), keys);
        copySheet->setToolTip(QStringLiteral("Every shortcut and mouse control, as bound right now, as plain text on the clipboard."));
        const QString product = plugin.productName();
        QObject::connect(copySheet, &QPushButton::clicked, keys, [product] { QGuiApplication::clipboard()->setText(hotkeys::cheatSheetText(product)); });
        auto* saveSheet = new QPushButton(QStringLiteral("Save cheat sheet…"), keys);
        saveSheet->setToolTip(QStringLiteral("The same table as an HTML page that prints on one sheet."));
        QObject::connect(saveSheet, &QPushButton::clicked, keys, [keys, product] {
            const QString path = QFileDialog::getSaveFileName(keys, QStringLiteral("Save cheat sheet"),
                QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/%1 keys.html").arg(product),
                QStringLiteral("HTML (*.html)"));
            if (path.isEmpty()) return;
            QFile f(path);
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) f.write(hotkeys::cheatSheetHtml(product).toUtf8());
        });
        sheetRow->addWidget(copySheet); sheetRow->addWidget(saveSheet); sheetRow->addStretch(1);
        kv->addLayout(sheetRow);
        w->warning = new QLabel(keys);
        w->warning->setWordWrap(true);
        w->warning->setStyleSheet(QStringLiteral("color:#b04040;"));
        w->warning->hide();
        kv->addWidget(w->warning);
        for (const auto& row : w->edits)
            QObject::connect(row.second, &QKeySequenceEdit::keySequenceChanged, keys, [w] { w->checkClashes(); });
        kv->addStretch(1);
        return keys;
    };
    p.reload = [w] {
        for (auto& row : w->edits)
            if (const hotkeys::Def* d = hotkeys::find(row.first)) row.second->setKeySequence(hotkeys::seq(d->key, d->def));
        w->checkClashes();
    };
    p.apply = [w] {
        // PORTABLE TEXT, and an empty string IS the stored form of "unbound" — a real setting
        // (FOX). POE2 removed the key instead, which made "unbound" indistinguishable from "default".
        QSettings s;
        for (const auto& row : w->edits)
            s.setValue(row.first, row.second->keySequence().toString(QKeySequence::PortableText));
    };
    return p;
}

// ── Maintenance ───────────────────────────────────────────────────────────────────────────────
SettingsPage maintenancePage(AppPlugin& plugin)
{
    SettingsPage p;
    p.title = QStringLiteral("Maintenance");
    p.order = 5;
    p.build = [&plugin](QWidget* parent) -> QWidget* {
        auto* maint = new QWidget(parent);
        auto* mv = new QVBoxLayout(maint);
        auto* cacheBox = new QGroupBox(QStringLiteral("Caches && reset"), maint);
        auto* cb = new QVBoxLayout(cacheBox);
        cb->addWidget(note(QStringLiteral("Everything this tool writes lives in data\\ beside the executable — settings, caches, logs. "
                                          "Nothing goes to the registry or AppData. A cache is safe to clear at any time: it is rebuilt "
                                          "on demand, and the only cost is the rebuild. Caches left by an older version are pruned at startup."), cacheBox));
        cachemaint::setDefs(plugin.cacheDefs());
        for (const CacheDef& c : cachemaint::defs()) {
            auto* row = new QHBoxLayout();
            const qint64 bytes = cachemaint::bytes(c);
            const int cnt = cachemaint::count(c);
            const QDateTime nw = cachemaint::newest(c);
            const QString age = nw.isValid() ? QStringLiteral(" · %1 file(s) · newest %2 day(s) old").arg(cnt).arg(nw.daysTo(QDateTime::currentDateTime())) : QString();
            auto* lbl = new QLabel(QStringLiteral("%1 — %2%3").arg(c.label, humanBytes(bytes), age), cacheBox);
            lbl->setToolTip(c.tooltip);
            row->addWidget(lbl, 1);
            auto* btn = new QPushButton(QStringLiteral("Clear"), cacheBox);
            btn->setEnabled(bytes > 0);
            const CacheDef def = c;
            QObject::connect(btn, &QPushButton::clicked, cacheBox, [def, lbl, btn] {
                int kept = 0;
                const int gone = cachemaint::clear(def, &kept);
                const qint64 left = cachemaint::bytes(def);
                lbl->setText(kept == 0 ? QStringLiteral("%1 — empty").arg(def.label)
                                       : QStringLiteral("%1 — %2 (%3 file(s) in use)").arg(def.label, humanBytes(left)).arg(kept));
                btn->setEnabled(left > 0);
                qInfo("settings: cleared %d cache file(s) of '%s', %d in use", gone, qUtf8Printable(def.label), kept);
            });
            row->addWidget(btn);
            cb->addLayout(row);
        }
        auto* total = new QLabel(QStringLiteral("Total cache on disk: %1").arg(humanBytes(cachemaint::totalBytes())), cacheBox);
        QFont tf = total->font(); tf.setBold(true); total->setFont(tf);
        cb->addWidget(total);
        auto* resetAll = new QPushButton(QStringLiteral("Reset every setting…"), cacheBox);
        resetAll->setToolTip(QStringLiteral("Back to a fresh install: every option, layout and search history. The game folder goes too."));
        QObject::connect(resetAll, &QPushButton::clicked, maint, [maint] {
            if (QMessageBox::question(maint, QStringLiteral("Reset every setting"),
                    QStringLiteral("Put every option, layout and history back to its default? The game folder is cleared too.\n\nThis cannot be undone."))
                != QMessageBox::Yes) return;
            QSettings s; s.clear(); s.sync();
            QMessageBox::information(maint, QStringLiteral("Reset"), QStringLiteral("Settings cleared. Restart the tool."));
            // CLOSE by rejecting (FOX): leaving the dialog open let the OK pressed next write every
            // widget straight back into the file that had just been cleared.
            if (auto* dlg = maint->window()) if (auto* d = qobject_cast<QDialog*>(dlg)) d->reject();
        });
        cb->addWidget(resetAll);
        mv->addWidget(cacheBox);
        mv->addStretch(1);
        return maint;
    };
    return p;
}

SettingsPage informationPage(AppPlugin& plugin)
{
    SettingsPage p;
    p.title = QStringLiteral("Information");
    p.order = 6;
    p.build = [&plugin](QWidget* parent) -> QWidget* {
        auto* host = new QWidget(parent);
        auto* v = new QVBoxLayout(host);
        auto* browser = new QTextBrowser(host);
        browser->setOpenExternalLinks(true);
        browser->setHtml(plugin.informationHtml().isEmpty()
            ? QStringLiteral("<p>%1 %2</p>").arg(plugin.productName().toHtmlEscaped(), plugin.version().toHtmlEscaped())
            : plugin.informationHtml());
        v->addWidget(browser, 1);
        return host;
    };
    return p;
}

}  // namespace

QVector<SettingsPage> SettingsDialog::corePages(AppPlugin& plugin)
{
    return {interfacePage(), exportPage(plugin), hotkeysPage(plugin), maintenancePage(plugin), informationPage(plugin)};
}

SettingsDialog::SettingsDialog(AppPlugin& plugin, QWidget* parent) : QDialog(parent), m_plugin(plugin)
{
    setWindowTitle(QStringLiteral("Settings"));
    resize(660, 620);
    auto* layout = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    // NEVER elide a tab label (§16): a scrollable bar is a worse outcome than a wide dialog and a
    // much better one than a tab called "eneral".
    m_tabs->tabBar()->setElideMode(Qt::ElideNone);
    m_tabs->tabBar()->setExpanding(false);
    m_tabs->tabBar()->setUsesScrollButtons(true);
    layout->addWidget(m_tabs, 1);

    m_pages = corePages(m_plugin) + m_plugin.settingsPages();
    std::stable_sort(m_pages.begin(), m_pages.end(), [](const SettingsPage& a, const SettingsPage& b) { return a.order < b.order; });
    for (const SettingsPage& p : m_pages) {
        if (!p.build) continue;
        addPage(p.title, p.build(m_tabs));
        m_livePrefixes += p.liveKeyPrefixes;
    }
    for (const SettingsPage& p : m_pages) if (p.reload) p.reload();
    snapshotLive();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::RestoreDefaults | QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::RestoreDefaults)->setText(QStringLiteral("Restore defaults…"));
    buttons->button(QDialogButtonBox::RestoreDefaults)->setToolTip(QStringLiteral(
        "Every option on these pages back to its default, by REMOVING the keys (so a later default change is "
        "picked up): export, image, GIF, interface, hover, hotkeys and the game's own pages. Folders, saved "
        "layouts and caches are kept."));
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, &SettingsDialog::restoreDefaults);
    layout->addWidget(buttons);
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::addPage(const QString& title, QWidget* page)
{
    // A scroll area per tab: a page taller than the dialog otherwise clips, silently, and the first
    // thing to go is whatever is at the bottom of it.
    auto* area = new QScrollArea(m_tabs);
    area->setWidget(page);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    m_tabs->addTab(area, title);
}

void SettingsDialog::showTab(const QString& name)
{
    if (!m_tabs) return;
    const int slash = name.indexOf(QLatin1Char('/'));
    const QString top = slash < 0 ? name : name.left(slash);
    const QString sub = slash < 0 ? QString() : name.mid(slash + 1);
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (m_tabs->tabText(i).compare(top, Qt::CaseInsensitive) != 0) continue;
        m_tabs->setCurrentIndex(i);
        if (sub.isEmpty()) return;
        if (QWidget* page = m_tabs->widget(i))
            for (QTabWidget* inner : page->findChildren<QTabWidget*>())
                for (int j = 0; j < inner->count(); ++j)
                    if (inner->tabText(j).startsWith(sub, Qt::CaseInsensitive)) { inner->setCurrentIndex(j); return; }
        return;
    }
}

void SettingsDialog::showEvent(QShowEvent* e)
{
    QDialog::showEvent(e);
    if (m_sized) return;
    m_sized = true;   // only the FIRST show; after that the user's own size wins
    const QScreen* scr = screen() ? screen() : QGuiApplication::primaryScreen();
    if (!scr) return;
    const QRect avail = scr->availableGeometry();
    // The wanted size comes from the PAGES, not from sizeHint(): QScrollArea::sizeHint() ends with a
    // hard cap, so with every page inside one the tab widget's hint is ~576x384 whatever the content.
    QSize want = sizeHint();
    for (int i = 0; m_tabs && i < m_tabs->count(); ++i)
        if (auto* sa = qobject_cast<QScrollArea*>(m_tabs->widget(i)))
            if (QWidget* pg = sa->widget()) want = want.expandedTo(pg->sizeHint() + QSize(48, 120));
    // …and wide enough for the TAB BAR itself, whose full un-elided width nothing else accounts for.
    if (m_tabs && m_tabs->tabBar()) want.setWidth(qMax(want.width(), m_tabs->tabBar()->sizeHint().width() + 24));
    want = want.expandedTo(size());
    resize(qMin(want.width(), int(avail.width() * 0.90)), qMin(want.height(), int(avail.height() * 0.90)));
    move(avail.center() - QPoint(width() / 2, height() / 2));
}

void SettingsDialog::snapshotLive()
{
    m_snapshot.clear();
    if (m_livePrefixes.isEmpty()) return;
    QSettings s;
    for (const QString& k : s.allKeys())
        for (const QString& p : m_livePrefixes)
            if (k.startsWith(p)) { m_snapshot.insert(k, s.value(k)); break; }
}

void SettingsDialog::accept()
{
    for (const SettingsPage& p : m_pages) if (p.apply) p.apply();
    QSettings().sync();
    emit settingsChanged();
    QDialog::accept();
}

void SettingsDialog::reject()
{
    // Undo everything the live pages wrote since the dialog opened, so Cancel truly cancels (D4).
    // Only keys that actually changed → a plain Cancel with no edits is a no-op. A key that
    // APPEARED under a live prefix while the dialog was open is removed: the snapshot can only
    // record keys that already existed.
    for (const SettingsPage& p : m_pages) if (p.revert) p.revert();
    QSettings s;
    bool changed = false;
    for (auto it = m_snapshot.constBegin(); it != m_snapshot.constEnd(); ++it) {
        if (s.value(it.key()) == it.value()) continue;
        changed = true;
        s.setValue(it.key(), it.value());
    }
    QStringList appeared;
    for (const QString& k : s.allKeys())
        for (const QString& p : m_livePrefixes)
            if (k.startsWith(p) && !m_snapshot.contains(k)) { appeared << k; break; }
    for (const QString& k : appeared) { s.remove(k); changed = true; }
    if (changed) { s.sync(); emit settingsChanged(); }
    QDialog::reject();
}

void SettingsDialog::restoreDefaults()
{
    if (QMessageBox::question(this, QStringLiteral("Restore defaults"),
            QStringLiteral("Put every option on these pages back to its default? Folders, saved layouts and caches are kept."))
        != QMessageBox::Yes) return;
    int n = settings::resetExportGroups();
    QSettings s;
    for (const SettingsPage& p : m_pages) {
        for (const QString& g : p.resetGroups) {
            s.beginGroup(g);
            for (const QString& k : s.allKeys()) {
                bool keep = false;
                for (const QString& kp : p.keepPrefixes) if (k.startsWith(kp)) { keep = true; break; }
                if (!keep) { s.remove(k); ++n; }
            }
            s.endGroup();
        }
    }
    s.sync();
    for (const SettingsPage& p : m_pages) if (p.reload) p.reload();
    qInfo("settings: restore defaults removed %d key(s)", n);
}

}  // namespace core
