#pragma once
// ── Battlefield 6 behind core::IGameStore (BF6 phase 1: storage + index) ─────────────────────────
//
// Opens the game's Frostbite superbundles IN PLACE (template §1, no dump) via the Qt-free Bf6Toc,
// enumerates every resource, classifies it by its AUTHORED Frostbite type (template §3 — the meta
// blob's type hash → types.txt, never a name guess), and read() returns the decompressed bytes of
// any resource (raw / zstd / Oodle) for single-asset and bulk RAW export (§6, §14).
//
// Native cooked decode is implemented (BF6 MeshSet / DxTexture / ske_soldier_3p skeleton, RE'd from the
// reshax tools): loadModel, loadTexture, loadSkeleton and resolveMaterials drive the in-app 3D viewport,
// textured thumbnails and self-contained glTF (.glb) export. External reshax tools remain available for
// SMD/ASCII + DDS cooked export; raw-originals export covers every resource (§14).
#include "core/store/IGameStore.h"
#include "games/bf6/Bf6Toc.h"
#include "games/bf6/Bf6Names.h"

#include <QHash>
#include <QString>
#include <QVector>
#include <atomic>
#include <string>
#include <unordered_map>

namespace bf6game {

class Bf6Store : public core::IGameStore {
public:
    static constexpr uint16_t kScheme = 6;   // AssetId.value = index into Bf6Toc::resources()
    // A synthetic "full assembled character" row: AssetId.value = kFullBit | index into m_fullKeys.
    // loadModel merges every cosmetic part of that char+set; resolveMaterials resolves them by the key.
    static constexpr uint64_t kFullBit = 1ull << 63;

    QString gameName() const override { return QStringLiteral("Battlefield 6"); }
    bool    isThreadSafe() const override { return true; }   // read() opens a fresh cas handle per call

    // `dataDir` is the game's Data folder (holding layout.toc and Win32/*.toc). The Oodle runtime
    // (oo2core_9_win64.dll) is loaded from the game root beside it.
    bool    open(const QString& dataDir, QString* error, core::ProgressFn progress = {}) override;
    bool    isOpen() const override { return m_open; }
    QString fingerprint() const override { return m_fingerprint; }

    // Models (every MeshSet), Textures, and the §33 showpiece Characters (character meshes grouped by
    // the authored character/variation path). The flat "file" kind (all 161k resources) is built for
    // raw export/Bulk but not shown as a tab (the core list isn't virtualised for that count yet).
    QVector<QString> kinds() const override { return { QStringLiteral("model"), QStringLiteral("texture"), QStringLiteral("character"), QStringLiteral("weapon"), QStringLiteral("vehicle") }; }
    int      rowCount(const QString& kind) const override;
    core::AssetRow row(const QString& kind, int i) const override;
    QString  searchBlob(const core::AssetId& id) const override;
    QVector<core::TagGroup> tagGroups(const QString& kind) const override;
    bool     hasTag(const core::AssetId& id, const QString& tag) const override;

    QByteArray read(const core::AssetId& id, const QString& part, QString* error) override;

    // Native cooked decode (BF6 formats RE'd from the reshax tools) — drives the viewport + glb export.
    bool loadModel(const core::AssetId& id, core::Geometry& geo, QString* error) override;
    core::Skeleton loadSkeleton(const core::AssetId& id, const core::Geometry& geo,
                                bool decodeClips, QVector<core::Clip>* clipsOut) override;
    QVector<core::Material> resolveMaterials(const core::AssetId& id, const core::Geometry& geo,
                                             bool decodeImages) override;
    QImage loadTexture(const core::AssetId& id, core::TextureInfo* info, QString* error) override;

    // loadTexture caps its decoded mip at 512px so the 31k-texture grid and model thumbnails stay
    // responsive. An EXPORT wants the full top mip (BF6 character maps are 2K), so the export entry
    // points flip this on for the duration of the export and off again after. Atomic because background
    // thumbnail workers may read it concurrently — a thumbnail decoded full-res during an export is
    // merely slower, never wrong. The standalone DDS export is always full-res and ignores this.
    // Per-thread texture-decode cap (the IGameStore hook). The live UI decodes capped (texture grid,
    // model thumbnails, the 3D preview); a saved export sets 0 (native, incl. the streamed 4K HD mip0) or
    // an explicit cap, choosing a lower mip at DECODE time so a 2K cap never pays the 4K HD-chunk read.
    // Thread-scoped, so the thumbnail render thread and the GUI thread never fight over it. Returns the
    // thread's previous cap for restore. px: 0 = native, N>0 = cap longest side, <0 = the UI default.
    int setDecodeTextureCap(int capPx) override;

    // A character texture's 4K top mip ("HD mip0", 16 MiB for 4096²) is NOT in characters.toc — it is a
    // separate streamed chunk in the HRES texture-pack superbundle, referenced by a SECOND GUID in the
    // DxTexture header. open() loads that pack's chunk store into m_hdToc; this finds the header GUID that
    // resolves to an HD chunk and returns its index into m_hdToc.resources() (or -1 when absent — no HD
    // pack installed, or a texture with no 4K mip). loadTexture/exportTextureDds read it for true 4K.
    int hdMip0Chunk(const std::vector<uint8_t>& texHeader) const;

    // The game path of a resource, for raw-originals export (§14 raw mode).
    QStringList dependencyFiles(const core::AssetId& id) override;

    QString diagnostics() const;

    // ── cooked export support (drives the external reshax tools; stated §1 deviation) ──────────────
    bool isMeshId(const core::AssetId& id) const;   // a MeshSet resource
    core::AssetId findMesh(const QString& nameSubstr) const;   // first MeshSet whose name contains substr (harness)
    QStringList   meshNamesMatching(const QString& nameSubstr, int max = 60) const;   // mesh names containing substr (harness)
    // Write the dump layout Fb_bf6_mesh.exe expects for one MeshSet into `tempDir`:
    //   tempDir/bundles/<name>.MeshSet  and  tempDir/chunks/<GUID>.chunk (its referenced global chunk).
    // `meshFile` ← the absolute .MeshSet path to pass as the tool's arg[0]. Fails closed with *error.
    bool extractMeshDump(const core::AssetId& meshId, const QString& tempDir, QString& meshFile, QString* error);
    // Extract the first EBX whose name contains `nameSubstr` (e.g. "ske_soldier_3p") to `outFile`, as
    // the decompressed .ebx bytes the mesh tool reads for the skeleton.
    bool extractEbx(const QString& nameSubstr, const QString& outFile, QString* error);

    bool isTextureId(const core::AssetId& id) const;                   // a Texture / DxTexture resource
    core::AssetId findTexture(const QString& nameSubstr) const;        // first texture whose name contains substr (harness)
    // Write a full-resolution DX10 .dds for one texture, NATIVELY (no external tool). BF6 textures are
    // streamed: the full BC7 mip chain is the texture's GLOBAL chunk (the per-bundle entry is only the
    // resident tail). Parses the texture RES, pulls the global chunk, wraps a DDS header. *outPath ←
    // the written file. Fails closed with *error.
    bool exportTextureDds(const core::AssetId& texId, const QString& outDir, QString* outPath, QString* error);

    // Decode and write EVERY source texture belonging to one char+set (name contains `charSetKey`,
    // e.g. "cha0045hawk_set_002") into `outDir` as loose PNGs, plus a TEXTURES.txt manifest that lists
    // each map by region/suffix with its size — so a material can be rebuilt by hand and any authored
    // map type the cooked path doesn't interpret is still visible. Returns the count written; never
    // throws. `outManifest` ← the manifest text (also written to disk).
    int dumpCharSourceTextures(const QString& charSetKey, const QString& outDir, QString* outManifest);

    // Diagnostic (RE): dump every resource whose name contains `nameSubstr` — a _manifest.txt (kind,
    // type, size, name per line) plus the decompressed bytes of each EBX/RES under 4 MiB, for offline
    // analysis of the material/appearance EBX (per-zone tint palette). Returns the match count.
    int dumpResourcesMatching(const QString& nameSubstr, const QString& outDir, int max = 100000);

    // Diagnostic (RE): write "FileGUID<tab>name" for EVERY EBX resource (FileGUID = 16 B at the EBXD
    // chunk data + 12) to `outFile`, so import GUIDs (e.g. a MeshVariationDatabase's referenced
    // materials) can be resolved to asset names. Returns the EBX count indexed.
    int dumpEbxGuidIndex(const QString& outFile);

private:
    const bf6::Resource* resAt(const core::AssetId& id) const;
    // Merge every cosmetic part of char+set `key` (the full assembled character) into `geo`, on the
    // shared rig (joint indices line up across parts). Materials are resolved separately by the key.
    bool assembleCharacter(const QString& key, core::Geometry& geo, QString* error);
    // Authored facets for one resource: "type:<T>" and "area:<segment>" (template §3 classification).
    QStringList facetsFor(const bf6::Resource& r) const;
    static QString areaOf(const std::string& name);   // top authored path area ("characters","shaders"…)

    bool              m_open = false;
    bf6::Toc          m_hdToc;                    // HRES texture-pack chunk store (the streamed 4K mip0s)
    std::unordered_map<std::string, int> m_hdChunkByGuid;  // HD chunk GUID -> index into m_hdToc.resources()
    QString           m_dir;
    QString           m_fingerprint;
    bf6::Toc          m_toc;
    QVector<int>      m_model;      // resource indices for kind "model" (MeshSet)
    QVector<int>      m_texture;    // kind "texture" (Texture / DxTexture)
    QVector<int>      m_file;       // kind "file" (every resource + chunk)
    QVector<int>      m_character;  // kind "character" (§33): MeshSets under common/characters/
    QHash<int, QStringList> m_charFacets;  // resource index -> "char:/set:/mode:/part:" facet ids
    QHash<int, QString>     m_charName;    // resource index -> display "Jinx · set_011 · backpack"
    QVector<int>            m_weapon;      // kind "weapon" (§33): MeshSets under common/hardware/weapons/
    QHash<int, QStringList> m_weaponFacets; // resource index -> "class:/weapon:/skin:/view:/part:/attach:"
    QHash<int, QString>     m_weaponName;  // resource index -> display "M4A1 · magazine (3p)"
    QVector<int>            m_vehicle;     // kind "vehicle" (§33): MeshSets under common/hardware/vehicles/
    QHash<int, QStringList> m_vehicleFacets; // resource index -> "class:/vehicle:/skin:/view:/part:/sub:"
    QHash<int, QString>     m_vehicleName; // resource index -> display "CV90 · base", "F16 · cockpit (1p)"
    // Assembled "full character" rows appended to the "character" kind: one per cha####_set_### that has
    // cosmetic parts. Selecting one merges every part on the shared rig (see assembleCharacter + kFullBit).
    QVector<QString>        m_fullKeys;    // fullIndex -> cha####_set_### key
    QHash<int, QStringList> m_fullFacets;  // fullIndex -> char/set/mode/operator/squad + "part:full"
    QHash<int, QString>     m_fullName;    // fullIndex -> display "Operator — Skin · full"
    Bf6Names                m_names;       // user-confirmed codename+set -> Squad/Operator/Skin (§33)
    // GUID (uppercase-hex chunk name) -> resource index of the LARGEST chunk with that GUID, built once
    // at open(). loadModel references a mesh's vertex/index chunk by GUID; a per-load linear scan over
    // ~38k chunk resources (building a QString for each) ran on the thumbnail render thread for every
    // grid tile, so a grid paid it hundreds of times. O(1) here instead. (The texture paths select by
    // bundleIndex, not size, so they keep their own scan.)
    std::unordered_map<std::string, int> m_chunkByGuid;
    // Cached funnel vocab per kind (built at open()).
    QHash<QString, QVector<core::TagGroup>> m_tagGroups;
};

}  // namespace bf6game
