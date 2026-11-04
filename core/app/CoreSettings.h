#pragma once
// The settings the CORE owns, one accessor pair per key (template §3.1; the FOX Config pattern),
// plus FOX's session override: a value set for this run only, consulted before QSettings and
// never persisted, so a headless check can drive a setting without rewriting the user's INI.
//
// Key names are a contract with shipped INIs, so each is the spelling one project already uses:
//   interface/rememberPanels      FOX  — window geometry, splitters and panels behind ONE toggle
//   interface/rememberSession     FOX  — reopen on the tab that was in front
//   session/tab                   FOX  — but storing the STABLE ID, not the title (app-shell §4.2)
//   paths/lastExportDir, paths/bulkOutDir, export/*, image/*, gif/*   POE2
//   log/autoFile                  D4
//   hover/*                       D4   (core/ui/HoverInfo.h)
//   tips/*                        FOX  (core/ui/TipBar)
//   ui/density, ui/fontScale      FOX  (Density.h, FontScale.h)
// A game that shipped a different spelling migrates it in its own main() table (phases 5/6).
#include "core/types/ExportOptions.h"

#include <QHash>
#include <QSettings>
#include <QString>
#include <QVariant>

namespace core::settings {

// ── Session overrides ───────────────────────────────────────────────────────────────────────
inline QHash<QString, QVariant>& sessionMap() { static QHash<QString, QVariant> m; return m; }
inline void setSession(const QString& key, const QVariant& v) { sessionMap().insert(key, v); }
inline void clearSession(const QString& key) { sessionMap().remove(key); }
inline QVariant value(const QString& key, const QVariant& def = QVariant())
{
    const auto it = sessionMap().constFind(key);
    return it != sessionMap().constEnd() ? it.value() : QSettings().value(key, def);
}
inline void set(const QString& key, const QVariant& v) { QSettings().setValue(key, v); }

// ── Interface ───────────────────────────────────────────────────────────────────────────────
inline bool rememberLayout()               { return value(QStringLiteral("interface/rememberPanels"), true).toBool(); }
inline void setRememberLayout(bool on)     { set(QStringLiteral("interface/rememberPanels"), on); }
inline bool rememberSession()              { return value(QStringLiteral("interface/rememberSession"), true).toBool(); }
inline void setRememberSession(bool on)    { set(QStringLiteral("interface/rememberSession"), on); }
inline QString lastTab()                   { return value(QStringLiteral("session/tab")).toString(); }
inline void setLastTab(const QString& id)  { set(QStringLiteral("session/tab"), id); }

// ── Folders ─────────────────────────────────────────────────────────────────────────────────
inline QString lastExportDir()                   { return value(QStringLiteral("paths/lastExportDir")).toString(); }
inline void    setLastExportDir(const QString& d){ set(QStringLiteral("paths/lastExportDir"), d); }
inline QString bulkOutDir()                      { return value(QStringLiteral("paths/bulkOutDir")).toString(); }
inline void    setBulkOutDir(const QString& d)   { set(QStringLiteral("paths/bulkOutDir"), d); }

// ── Export behaviour (not the options themselves) ───────────────────────────────────────────
inline bool exportShowPrompt()               { return value(QStringLiteral("export/showPrompt"), true).toBool(); }
inline void setExportShowPrompt(bool on)     { set(QStringLiteral("export/showPrompt"), on); }
inline bool exportReuseLastDir()             { return value(QStringLiteral("export/reuseLastDir"), false).toBool(); }
inline void setExportReuseLastDir(bool on)   { set(QStringLiteral("export/reuseLastDir"), on); }
inline int  bulkWorkers()                    { return value(QStringLiteral("export/bulkWorkers"), 0).toInt(); }   // 0 = auto
inline void setBulkWorkers(int n)            { set(QStringLiteral("export/bulkWorkers"), n); }
inline bool osNotify()                       { return value(QStringLiteral("export/osNotify"), true).toBool(); }   // D4
inline void setOsNotify(bool on)             { set(QStringLiteral("export/osNotify"), on); }

// ── Image export (POE2 keys) ────────────────────────────────────────────────────────────────
inline int  imageScalePercent()              { return value(QStringLiteral("image/scalePercent"), 100).toInt(); }
inline void setImageScalePercent(int p)      { set(QStringLiteral("image/scalePercent"), p); }
inline bool imageTransparentBg()             { return value(QStringLiteral("image/transparentBg"), true).toBool(); }
inline void setImageTransparentBg(bool on)   { set(QStringLiteral("image/transparentBg"), on); }
inline bool imageCropToModel()               { return value(QStringLiteral("image/cropToModel"), true).toBool(); }
inline void setImageCropToModel(bool on)     { set(QStringLiteral("image/cropToModel"), on); }
inline bool imageShowPrompt()                { return value(QStringLiteral("image/showPrompt"), true).toBool(); }
inline void setImageShowPrompt(bool on)      { set(QStringLiteral("image/showPrompt"), on); }

// ── The .glb options, from export/* (POE2 key names). `defaults` is the PLUGIN's default set
//    (POE2 unitScale 0.005, D4 1.0…), so an unconfigured install exports as that game always did.
inline ExportOptions exportOptions(const ExportOptions& defaults)
{
    ExportOptions o = defaults;
    o.unitScale          = float(value(QStringLiteral("export/unitScale"), double(defaults.unitScale)).toDouble());
    o.includeSkeleton    = value(QStringLiteral("export/skeleton"), defaults.includeSkeleton).toBool();
    o.includeAnimations  = value(QStringLiteral("export/animations"), defaults.includeAnimations).toBool();
    o.embedTextures      = value(QStringLiteral("export/embedTextures"), defaults.embedTextures).toBool();
    o.looseTextures      = value(QStringLiteral("export/looseTextures"), defaults.looseTextures).toBool();
    o.reconstructNormalZ = value(QStringLiteral("export/reconstructNormalZ"), defaults.reconstructNormalZ).toBool();
    o.includeAttachments = value(QStringLiteral("export/includeAttachments"), defaults.includeAttachments).toBool();
    o.gltfSeparate       = value(QStringLiteral("export/gltf"), defaults.gltfSeparate).toBool();
    o.maxTextureSize     = value(QStringLiteral("export/textureRes"), defaults.maxTextureSize).toInt();   // 0 = native
    // POE2 stores the 180° option as a bool; core carries degrees. 0/180 round-trip exactly.
    const bool yaw180    = value(QStringLiteral("export/yaw180"), defaults.yawDegrees == 180.0f).toBool();
    o.yawDegrees         = yaw180 ? 180.0f : 0.0f;
    o.hardpointEmpties   = value(QStringLiteral("export/hardpointEmpties"), defaults.hardpointEmpties).toBool();
    return o;
}
inline void saveExportOptions(const ExportOptions& o)
{
    QSettings s;
    s.setValue(QStringLiteral("export/unitScale"), double(o.unitScale));
    s.setValue(QStringLiteral("export/skeleton"), o.includeSkeleton);
    s.setValue(QStringLiteral("export/animations"), o.includeAnimations);
    s.setValue(QStringLiteral("export/embedTextures"), o.embedTextures);
    s.setValue(QStringLiteral("export/looseTextures"), o.looseTextures);
    s.setValue(QStringLiteral("export/reconstructNormalZ"), o.reconstructNormalZ);
    s.setValue(QStringLiteral("export/includeAttachments"), o.includeAttachments);
    s.setValue(QStringLiteral("export/gltf"), o.gltfSeparate);
    s.setValue(QStringLiteral("export/textureRes"), o.maxTextureSize);
    s.setValue(QStringLiteral("export/yaw180"), o.yawDegrees == 180.0f);
    s.setValue(QStringLiteral("export/hardpointEmpties"), o.hardpointEmpties);
}

// Reset by REMOVAL (template §3.10, POE2's restoreExportDefaults): every reader falls back to the
// default it already carries, so there is no second copy of any number to keep in sync.
inline int resetExportGroups()
{
    QSettings s;
    int n = 0;
    for (const char* g : {"export", "image", "gif"}) {
        s.beginGroup(QLatin1String(g));
        n += s.allKeys().size();
        s.remove(QString());
        s.endGroup();
    }
    return n;
}

}  // namespace core::settings
