#pragma once
// "Export for Blender": turn a .glb this tool wrote into a ready-to-open .blend by driving a local
// Blender headlessly — no Blender add-on, no import step for the user. Blender's built-in glTF
// importer already builds a correct Principled BSDF from our PBR glb (base colour, metallic-roughness
// via a Separate Color on the ORM map, a Normal Map node, KHR_materials_ior), so the helper just imports
// the glb, packs the textures into the .blend (so it is self-contained) and saves it.
//
// Generic on purpose (core, not a plugin): every AssetBrowser tool exports glb, so every one can offer
// a .blend. The plugin supplies the glb and the blender executable path; core owns neither a settings
// key nor AppPaths.
#include <QString>

namespace core::blender {

// Locate a Blender executable, best-effort, in order: `savedPath` (a settings value) if it runs; the
// $BLENDER env var; the Blender Launcher library (%LOCALAPPDATA%\Blender Launcher); a standard Blender
// Foundation install (highest version). Returns "" when none is found — the caller then asks the user.
QString findBlender(const QString& savedPath = QString());

// Import `glbPath` into a fresh Blender (`blenderExe`) and save a self-contained `blendPath`. Blocks
// until Blender exits (a deliberate export action). Returns false + *err on failure; *log (optional)
// receives Blender's combined stdout/stderr for diagnostics.
bool toBlend(const QString& blenderExe, const QString& glbPath, const QString& blendPath,
             QString* log, QString* err, int timeoutMs = 300000);

}  // namespace core::blender
