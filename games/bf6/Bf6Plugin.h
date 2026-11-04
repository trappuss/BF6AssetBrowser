#pragma once
// What Battlefield 6 contributes to the shell: its Frostbite store, the core Models / Characters /
// Textures / Bulk tabs over it (native 3D viewport, textured thumbnails, 291-bone rig), a Directories
// settings page (game Data + reshax tools folders), and export — self-contained glTF (.glb) through the
// shared core exporter, plus external-tool SMD/ASCII + native DDS. See docs/BF6_STORAGE_FORMAT.md.
#include "core/app/AppPlugin.h"
#include "games/bf6/Bf6Store.h"

#include <QPointer>
#include <QString>
#include <atomic>

class QLineEdit;

namespace bf6game {

class Bf6Plugin : public core::AppPlugin {
public:
    Bf6Plugin();
    ~Bf6Plugin() override;

    QString productName() const override { return QStringLiteral("BF6AssetBrowser"); }
    QString version() const override { return QStringLiteral("0.1.0"); }
    core::IGameStore* store() override { return &m_store; }

    QVector<core::TabSpec>      tabs() override;
    QVector<core::SettingsPage> settingsPages() override;

    bool    isConfigured() override;
    bool    configureFolder(QWidget* parent) override;
    QString firstRunText() const override;
    void    reload(std::function<void(bool ok, const QString& status)> done,
                   std::function<void()> secondaryReady) override;
    QString diagnosticInfo() override { return m_store.diagnostics(); }
    QString informationHtml() const override;
    bool    takeReloadRequest() override { const bool r = m_reloadRequested; m_reloadRequested = false; return r; }

    QStringList modelKinds() const override { return { QStringLiteral("model") }; }
    QStringList textureKinds() const override { return { QStringLiteral("texture") }; }
    Nouns nouns() const override;

    // The preview's look (user spec): neutral, undistracting viewing — tonemap + geometric spec-AA on;
    // IBL, shadows and SSAO off (they stay live toggles in the RENDER panel). Plus a Neutral light
    // preset and no backdrop gradient as the panel's persisted defaults.
    core::gl::ViewFeatures viewFeatures() const override;
    int  defaultLightPreset() const override { return 1; }        // Neutral (combo: Warm/Neutral/Cool)
    bool defaultBackgroundGradient() const override { return false; }

    // Add the row-menu export actions: "Export model (glTF .glb)…" (native, self-contained) and
    // "Export mesh (SMD + ASCII)…" (the external reshax tool, stated §1 deviation), plus the Textures
    // tab's native DDS export.
    void decorateTab(const QString& stableId, core::BrowserTab* tab) override;

    // Headless export harness (for end-to-end verification), to --bf6-out (default temp):
    //   --bf6-export-glb <meshSubstr>  self-contained glTF (what the glb row action does)
    //   --bf6-export <meshSubstr>      SMD + ASCII via Fb_bf6_mesh   · --bf6-export-tex <substr>  DDS
    QStringList parseArgs(const QStringList& args) override;
    void        applyArgs(core::AppShell& shell) override;

    void shutdown();

    static QString gameDir();
    static void    setGameDir(const QString&);
    static QString toolsDir();          // the reshax BF6 tools folder (holds Fb_bf6_mesh.exe)
    static void    setToolsDir(const QString&);

private:
    // Extract the selected MeshSet + its chunk + skeleton to a temp dir, run Fb_bf6_mesh.exe from the
    // tools folder, and deliver <name>.smd / .ascii to a user-chosen folder.
    void exportMesh(QWidget* parent, const core::AssetId& id);
    // The dialog-free core of exportMesh (also used by the harness): returns a status string; *ok set.
    QString runMeshExport(const core::AssetId& id, const QString& tools, const QString& outDir, bool* ok);
    // Export the selected texture(s) to full-resolution .dds natively (no external tool), to a folder.
    void exportTextures(QWidget* parent, const QVector<core::AssetId>& ids);
    // Export one model to a self-contained glTF binary (.glb) via the shared core exporter — geometry,
    // the 291-bone rig + skin weights, and the textures all embedded in ONE file, no external deps.
    void exportGlb(QWidget* parent, const core::AssetId& id);
    // Export the FULL assembled character for the clicked row's char+set (every cosmetic mesh merged on
    // the shared rig) to a folder; a checkbox chooses loose output (.gltf + external PNGs + raw _src dump).
    void exportChar(QWidget* parent, const core::AssetId& id);
    // Dialog-free core of the full-character export (also used by the --bf6-export-char harness): assemble
    // the char+set matched by `matchSubstr`, write <key>_full.glb (or .gltf + external PNGs + a _src dump
    // of the raw source maps when `loose`) into `outDir`. Returns a one-line summary; *okOut ← success.
    QString runCharExport(const QString& matchSubstr, const QString& outDir, bool loose, int texCap, bool* okOut);
    // "Export for Blender": assemble the char+set to a .glb (full character, non-loose), then drive a
    // local Blender headlessly to import it and save a ready, self-contained .blend beside it (no add-on,
    // no import step). `blenderExe` empty → auto-detect (core::blender::findBlender). Dialog-free core,
    // shared by the GUI action and the --bf6-blend harness. Returns a one-line summary; *okOut ← success.
    QString runBlendExport(const QString& matchSubstr, const QString& outDir, int texCap,
                           const QString& blenderExe, bool* okOut);
    // GUI: "Export as .blend for Blender…" on the clicked character row (locates/remembers Blender).
    void exportBlend(QWidget* parent, const core::AssetId& id);

    Bf6Store          m_store;
    bool              m_reloadRequested = false;
    std::atomic<int>  m_workers{ 0 };
    QPointer<QLineEdit> m_dirEdit;
    QPointer<QLineEdit> m_toolsEdit;
    QString           m_origDir;
    QString           m_origTools;
    QString           m_exportSubstr;    // --bf6-export (mesh)
    QString           m_exportTexSubstr; // --bf6-export-tex
    QString           m_showTexSubstr;   // --bf6-show-tex (select a texture to preview)
    QString           m_showModelSubstr; // --bf6-show-model (select a mesh to render)
    QString           m_listSubstr;      // --bf6-list (print mesh names matching)
    QString           m_dumpResSubstr;   // --bf6-dump-res (dump EBX/RES bytes + manifest for matching resources)
    bool              m_ebxIndex = false;   // --bf6-ebx-index (write FileGUID->name for every EBX to <out>/ebx_index.tsv)
    bool              m_listChars = false;  // --bf6-chars (print the Character funnel)
    bool              m_listWeapons = false; // --bf6-weapons (print the Weapons funnel + sample rows)
    bool              m_listVehicles = false; // --bf6-vehicles (print the Vehicles funnel + sample rows)
    QString           m_exportGlbSubstr; // --bf6-export-glb (self-contained glTF)
    QString           m_sheetSubstr;     // --bf6-sheet (batch: default body glb per codename)
    QString           m_exportCharSubstr; // --bf6-export-char (full assembled character glb)
    QString           m_exportOut;       // --bf6-out
    bool              m_exportLoose = false; // --bf6-loose (loose .gltf + external PNGs + raw source maps)
    int               m_exportTexCap = 0;    // --bf6-texcap <px> (0 = native/4K; caps export texture side)
    QString           m_exportBlendSubstr;   // --bf6-blend (assemble char -> ready .blend for Blender)
    QString           m_blenderExe;          // --bf6-blender <exe> (override; else auto-detect)
};

}  // namespace bf6game
