#include "core/ui/ExportDialogs.h"
#include "core/app/CoreSettings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace core {

// ── Export options ─────────────────────────────────────────────────────────────────────────────
ExportOptionsDialog::ExportOptionsDialog(const ExportOptions& defaults, QWidget* parent)
    : QDialog(parent), m_defaults(defaults)
{
    setWindowTitle(QStringLiteral("Export options"));
    auto* box = new QGroupBox(QStringLiteral("glTF export"), this);
    auto* form = new QFormLayout(box);

    m_unitScale = new QDoubleSpinBox(box);
    m_unitScale->setDecimals(5); m_unitScale->setRange(0.00001, 1000.0); m_unitScale->setSingleStep(0.001);
    m_unitScale->setToolTip(QStringLiteral("Metres per native game unit. 1.0 keeps raw native units."));
    form->addRow(QStringLiteral("Unit scale (m per unit):"), m_unitScale);

    m_format = new QComboBox(box);
    m_format->addItems({QStringLiteral("Single file (.glb)"), QStringLiteral("Text + buffer (.gltf + .bin)")});
    form->addRow(QStringLiteral("Format:"), m_format);

    m_animMode = new QComboBox(box);
    m_animMode->addItems({QStringLiteral("All clips"), QStringLiteral("Current clip only"), QStringLiteral("None")});
    form->addRow(QStringLiteral("Animations:"), m_animMode);

    m_skeleton = new QCheckBox(QStringLiteral("Include skeleton"), box); form->addRow(QString(), m_skeleton);
    m_attachments = new QCheckBox(QStringLiteral("Include attachments (assembled)"), box);
    m_attachments->setToolTip(QStringLiteral("Bake this model's attachments onto their bones so the exported file matches the\n"
        "assembled view. Attachments are weighted to their parent bone, so they follow the animation."));
    form->addRow(QString(), m_attachments);
    m_yaw180 = new QCheckBox(QStringLiteral("Rotate 180° about up (face camera / thumbnails)"), box); form->addRow(QString(), m_yaw180);
    m_embedTextures = new QCheckBox(QStringLiteral("Embed textures"), box); form->addRow(QString(), m_embedTextures);
    m_looseTextures = new QCheckBox(QStringLiteral("Loose textures (.gltf → sibling .png files)"), box);
    m_looseTextures->setToolTip(QStringLiteral("Write textures as separate .png files next to a .gltf instead of embedding them. Only applies to .gltf."));
    form->addRow(QString(), m_looseTextures);
    m_normalZ = new QCheckBox(QStringLiteral("Reconstruct normal-map Z"), box); form->addRow(QString(), m_normalZ);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Export"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { save(); accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* outer = new QVBoxLayout(this);
    outer->addWidget(box);
    outer->addWidget(buttons);
    load();
}

void ExportOptionsDialog::setUnitScaleTip(const QString& tip) { m_unitScale->setToolTip(tip); }

void ExportOptionsDialog::setContext(int clipCount, int currentClip, bool hasAttachments, int modelCount)
{
    m_currentClip = currentClip;
    setWindowTitle(modelCount > 1 ? QStringLiteral("Export %1 models").arg(modelCount) : QStringLiteral("Export options"));
    const bool canCurrent = currentClip >= 0 && clipCount > 0;
    if (!canCurrent && m_animMode->currentIndex() == 1) m_animMode->setCurrentIndex(0);
    m_attachments->setEnabled(hasAttachments);
    if (!hasAttachments) { m_attachments->setChecked(false); m_attachments->setToolTip(QStringLiteral("This model declares no attachments.")); }
}

void ExportOptionsDialog::load()
{
    const ExportOptions o = settings::exportOptions(m_defaults);
    m_unitScale->setValue(o.unitScale);
    m_yaw180->setChecked(o.yawDegrees == 180.0f);
    m_skeleton->setChecked(o.includeSkeleton);
    m_animMode->setCurrentIndex(o.includeAnimations ? 0 : 2);   // remembered All/None; Current is a live pick
    m_attachments->setChecked(o.includeAttachments);
    m_embedTextures->setChecked(o.embedTextures);
    m_looseTextures->setChecked(o.looseTextures);
    m_normalZ->setChecked(o.reconstructNormalZ);
    m_format->setCurrentIndex(o.gltfSeparate ? 1 : 0);
}

void ExportOptionsDialog::save() const
{
    ExportOptions o = options();
    o.onlyClip = -1;   // a live pick, never remembered
    settings::saveExportOptions(o);
}

ExportOptions ExportOptionsDialog::options() const
{
    ExportOptions o = settings::exportOptions(m_defaults);
    o.unitScale          = float(m_unitScale->value());
    o.yawDegrees         = m_yaw180->isChecked() ? 180.0f : 0.0f;
    o.includeSkeleton    = m_skeleton->isChecked();
    o.includeAnimations  = m_animMode->currentIndex() != 2;
    o.onlyClip           = m_animMode->currentIndex() == 1 ? m_currentClip : -1;
    o.includeAttachments = m_attachments->isChecked();
    o.embedTextures      = m_embedTextures->isChecked();
    o.looseTextures      = m_looseTextures->isChecked();
    o.reconstructNormalZ = m_normalZ->isChecked();
    o.gltfSeparate       = m_format->currentIndex() == 1;
    return o;
}

// ── Image ──────────────────────────────────────────────────────────────────────────────────────
static const int kImageScales[] = { 25, 50, 100, 200, 400 };

ImageExportDialog::ImageExportDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Save preview image"));
    auto* box = new QGroupBox(QStringLiteral("PNG image"), this);
    auto* form = new QFormLayout(box);
    m_scale = new QComboBox(box);
    for (int p : kImageScales) m_scale->addItem(QStringLiteral("%1%").arg(p), p);
    m_scale->setToolTip(QStringLiteral("Renders the view again at this size. 200% re-renders at double resolution\n"
        "for a sharper image — it is NOT an upscale of the on-screen pixels."));
    form->addRow(QStringLiteral("Scale:"), m_scale);
    m_transparent = new QCheckBox(QStringLiteral("Transparent background"), box);
    m_transparent->setToolTip(QStringLiteral("Save the model on a transparent background (alpha PNG) instead of the viewport backdrop."));
    form->addRow(QString(), m_transparent);
    m_crop = new QCheckBox(QStringLiteral("Crop to model"), box);
    m_crop->setToolTip(QStringLiteral("Trim the empty margin so the image is tight around the model. Only with a transparent background."));
    form->addRow(QString(), m_crop);
    connect(m_transparent, &QCheckBox::toggled, m_crop, &QCheckBox::setEnabled);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Save…"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { save(); accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* outer = new QVBoxLayout(this);
    outer->addWidget(box);
    outer->addWidget(buttons);
    load();
}

int  ImageExportDialog::scalePercent() const  { return m_scale->currentData().toInt(); }
bool ImageExportDialog::transparentBg() const { return m_transparent->isChecked(); }
bool ImageExportDialog::cropToModel() const   { return m_transparent->isChecked() && m_crop->isChecked(); }

void ImageExportDialog::load()
{
    int idx = m_scale->findData(settings::imageScalePercent());
    if (idx < 0) idx = m_scale->findData(100);
    m_scale->setCurrentIndex(qMax(0, idx));
    m_transparent->setChecked(settings::imageTransparentBg());
    m_crop->setChecked(settings::imageCropToModel());
    m_crop->setEnabled(m_transparent->isChecked());
}

void ImageExportDialog::save() const
{
    settings::setImageScalePercent(m_scale->currentData().toInt());
    settings::setImageTransparentBg(m_transparent->isChecked());
    settings::setImageCropToModel(m_crop->isChecked());
}

// ── GIF ────────────────────────────────────────────────────────────────────────────────────────
static const int kGifScales[] = { 25, 50, 75, 100 };

GifExportDialog::GifExportDialog(Mode mode, QWidget* parent) : QDialog(parent), m_mode(mode)
{
    setWindowTitle(mode == Turntable ? QStringLiteral("Turntable GIF") : QStringLiteral("Animation-loop GIF"));
    auto* box = new QGroupBox(QStringLiteral("Animated GIF"), this);
    auto* form = new QFormLayout(box);
    if (mode == Turntable) {
        m_fps = new QSpinBox(box); m_fps->setRange(1, 60); m_fps->setSuffix(QStringLiteral(" fps"));
        form->addRow(QStringLiteral("Frame rate:"), m_fps);
        m_frames = new QSpinBox(box); m_frames->setRange(8, 240);
        m_frames->setToolTip(QStringLiteral("Frames in one full revolution. When a clip is playing this is snapped\n"
            "to a whole number of clip cycles so the loop wraps seamlessly."));
        form->addRow(QStringLiteral("Frames / turn:"), m_frames);
    } else {
        auto* note = new QLabel(QStringLiteral("Plays the clip at its own authored frame rate."), box);
        note->setStyleSheet(QStringLiteral("color: gray;"));
        form->addRow(QString(), note);
    }
    m_scale = new QComboBox(box);
    for (int p : kGifScales) m_scale->addItem(QStringLiteral("%1%").arg(p), p);
    m_scale->setToolTip(QStringLiteral("Downscale the GIF — the single biggest lever on file size."));
    form->addRow(QStringLiteral("Scale:"), m_scale);
    m_colors = new QSpinBox(box); m_colors->setRange(2, 256);
    form->addRow(QStringLiteral("Max colours:"), m_colors);
    m_dither = new QCheckBox(QStringLiteral("Dither (reduce banding)"), box); form->addRow(QString(), m_dither);
    m_transparent = new QCheckBox(QStringLiteral("Transparent background"), box); form->addRow(QString(), m_transparent);
    m_crop = new QCheckBox(QStringLiteral("Crop to model"), box); form->addRow(QString(), m_crop);
    m_optimize = new QCheckBox(QStringLiteral("Optimise to target size"), box);
    m_targetMB = new QSpinBox(box); m_targetMB->setRange(1, 200); m_targetMB->setSuffix(QStringLiteral(" MB"));
    m_optimize->setToolTip(QStringLiteral("Re-encode (palette → dither → downscale) until the file fits the target,\n"
        "shipping the smallest attempt. Frames are captured once, so this only re-encodes."));
    connect(m_optimize, &QCheckBox::toggled, m_targetMB, &QSpinBox::setEnabled);
    form->addRow(m_optimize, m_targetMB);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Save…"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { save(); accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* outer = new QVBoxLayout(this);
    outer->addWidget(box);
    outer->addWidget(buttons);
    load();
}

capture::GifOptions GifExportDialog::options() const
{
    capture::GifOptions o = capture::loadGifOptions();
    if (m_fps) o.fps = m_fps->value();
    if (m_frames) o.turntableFrames = m_frames->value();
    o.scalePercent  = m_scale->currentData().toInt();
    o.maxColors     = m_colors->value();
    o.dither        = m_dither->isChecked();
    o.transparentBg = m_transparent->isChecked();
    o.cropToModel   = m_crop->isChecked();
    o.optimize      = m_optimize->isChecked();
    o.targetMB      = m_targetMB->value();
    return o;
}

void GifExportDialog::load()
{
    const capture::GifOptions o = capture::loadGifOptions();
    if (m_fps) m_fps->setValue(qBound(1, o.fps, 60));
    if (m_frames) m_frames->setValue(qBound(8, o.turntableFrames, 240));
    const int idx = m_scale->findData(qBound(25, o.scalePercent, 100));
    m_scale->setCurrentIndex(idx < 0 ? m_scale->count() - 1 : idx);
    m_colors->setValue(qBound(2, o.maxColors, 256));
    m_dither->setChecked(o.dither);
    m_transparent->setChecked(o.transparentBg);
    m_crop->setChecked(o.cropToModel);
    m_optimize->setChecked(o.optimize);
    m_targetMB->setValue(qBound(1, o.targetMB, 200));
    m_targetMB->setEnabled(m_optimize->isChecked());
}

void GifExportDialog::save() const { capture::saveGifOptions(options()); }

}  // namespace core
