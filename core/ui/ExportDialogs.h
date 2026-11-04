#pragma once
// The three at-export dialogs (POE2's, generalised over core::ExportOptions and the gif/* keys).
// Each is the same set of knobs as Settings, chosen at export time and REMEMBERED back, so the next
// export defaults to the last choice. Scale ladders are stored as the PERCENT, never a combo index.
#include "core/types/ExportOptions.h"
#include "core/ui/ExportCapture.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;

namespace core {

class ExportOptionsDialog : public QDialog {
    Q_OBJECT
public:
    // `defaults` is the plugin's default set (the unit scale and its tooltip differ per game).
    explicit ExportOptionsDialog(const ExportOptions& defaults, QWidget* parent = nullptr);
    // clipCount/currentClip drive "Current clip only"; hasAttachments enables the assembled export.
    void setContext(int clipCount, int currentClip, bool hasAttachments, int modelCount);
    void setUnitScaleTip(const QString& tip);
    ExportOptions options() const;    // onlyClip set for "current clip only"
private:
    void load();
    void save() const;
    ExportOptions m_defaults;
    int m_currentClip = -1;
    QDoubleSpinBox* m_unitScale = nullptr;
    QCheckBox* m_yaw180 = nullptr;
    QCheckBox* m_skeleton = nullptr;
    QComboBox* m_animMode = nullptr;      // 0 all · 1 current clip only · 2 none
    QCheckBox* m_attachments = nullptr;
    QCheckBox* m_embedTextures = nullptr;
    QCheckBox* m_looseTextures = nullptr;
    QCheckBox* m_normalZ = nullptr;
    QComboBox* m_format = nullptr;        // 0 .glb · 1 .gltf + .bin
};

class ImageExportDialog : public QDialog {
    Q_OBJECT
public:
    explicit ImageExportDialog(QWidget* parent = nullptr);
    int  scalePercent() const;
    bool transparentBg() const;
    bool cropToModel() const;
private:
    void load();
    void save() const;
    QComboBox* m_scale = nullptr;
    QCheckBox* m_transparent = nullptr;
    QCheckBox* m_crop = nullptr;
};

class GifExportDialog : public QDialog {
    Q_OBJECT
public:
    enum Mode { Turntable, AnimLoop };
    explicit GifExportDialog(Mode mode, QWidget* parent = nullptr);
    capture::GifOptions options() const;
private:
    void load();
    void save() const;
    Mode m_mode;
    QSpinBox*  m_fps = nullptr;
    QSpinBox*  m_frames = nullptr;
    QComboBox* m_scale = nullptr;
    QSpinBox*  m_colors = nullptr;
    QCheckBox* m_dither = nullptr;
    QCheckBox* m_transparent = nullptr;
    QCheckBox* m_crop = nullptr;
    QCheckBox* m_optimize = nullptr;
    QSpinBox*  m_targetMB = nullptr;
};

}  // namespace core
