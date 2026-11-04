#pragma once
#include <QString>

// Every decision the three exporters honour, in one value struct (geometry-export.md §6.5).
// FOX's "one converter from settings, one describe() line in the log" is the pattern; the QSettings
// keys stay per app so core never owns a key name.
namespace core {

struct ExportOptions {
    bool includeSkeleton   = true;
    bool includeAnimations = true;
    int  onlyClip          = -1;      // -1 = all, >= 0 = that clip           [POE2]
    bool animationLibrary  = false;   // rig + clips, no mesh                 [D4]
    bool includeAttachments = false;

    bool embedTextures  = true;
    bool looseTextures  = false;      // sibling .png files (.gltf output)    [POE2]
    int  jpegQuality    = 0;          // 0 = PNG only                         [FOX]
    int  maxTextureSize = 0;          // 0 = no cap                           [FOX]
    bool dedupeImages   = true;       // by content hash                      [FOX]
    bool pruneUnusedMaterials = true; // only what visible parts reference    [FOX; fixed in POE2 2026-09]

    float unitScale = 1.0f;           // D4 1.0 · FOX 1.0 · POE2 0.005
    enum ScaleMode { BakeIntoData, WrapperNode } scaleMode = BakeIntoData;   // FOX uses WrapperNode
    enum UpAxis { YUp, ZUp } upAxis = YUp;
    float yawDegrees = 0.0f;          // D4 -90 (Blender), POE2 180

    bool flipNormalGreen   = true;
    bool reconstructNormalZ = true;

    bool hardpointEmpties  = false;
    bool readableBoneNames = false;   // [FOX]
    bool mirrorBoneNames   = false;   // .L/.R                                [FOX/D4]
    bool reduceRig         = false;   // [FOX] weight-based
    bool xMirrorSymmetrize = false;   // [D4]
    bool writeTangents     = true;
    // COLOR_0 is OFF by default and that is a deliberate, load-bearing default. glTF multiplies
    // COLOR_0 into base colour, but D4's vertex colour is DETAIL-MAP BLEND WEIGHTS, not a tint —
    // writing it silently darkens or tints every D4 export in every importer. A plugin whose
    // COLOR_0 really is a colour turns it on. (TEXCOORD_1 has no such trap and is written whenever
    // the mesh has one.)
    bool writeVertexColor  = false;

    bool gltfSeparate = false;        // .gltf + .bin instead of .glb         [POE2]
    QString generator;                // "D4AssetBrowser" / …

    QString describe() const;         // one log line; the FOX habit worth keeping
};

}  // namespace core
