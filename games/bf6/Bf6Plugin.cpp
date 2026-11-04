#include "games/bf6/Bf6Plugin.h"

#include "core/app/AppShell.h"
#include "core/export/BlenderExport.h"
#include "core/export/GlbExporter.h"
#include "core/gl/GLModelWidget.h"
#include "core/tabs/BulkTab.h"
#include "core/tabs/ModelLoad.h"
#include "core/tabs/ModelsTab.h"
#include "core/tabs/TexturesTab.h"
#include "core/types/ExportOptions.h"
#include "core/types/Skeleton.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QGridLayout>
#include <QMenu>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QThread>
#include <QVBoxLayout>

namespace bf6game {

namespace {
constexpr auto kGameDir = "paths/dataDir";
constexpr auto kToolsDir = "paths/toolsDir";

// Configured when the folder holds the character superbundle TOC.
bool hasDataAt(const QString& dir)
{
    if (dir.isEmpty()) return false;
    return QFileInfo::exists(QDir(dir).filePath(QStringLiteral("Win32/characters.toc")));
}
}  // namespace

Bf6Plugin::Bf6Plugin() = default;
Bf6Plugin::~Bf6Plugin() = default;

QString Bf6Plugin::gameDir() { return QSettings().value(QLatin1String(kGameDir)).toString(); }
void Bf6Plugin::setGameDir(const QString& d) { QSettings().setValue(QLatin1String(kGameDir), d); }
QString Bf6Plugin::toolsDir() { return QSettings().value(QLatin1String(kToolsDir)).toString(); }
void Bf6Plugin::setToolsDir(const QString& d) { QSettings().setValue(QLatin1String(kToolsDir), d); }

QVector<core::TabSpec> Bf6Plugin::tabs()
{
    QVector<core::TabSpec> t;
    t.append({ QStringLiteral("Models"), QStringLiteral("models"), false,
               [this](QWidget* p) -> core::BrowserTab* { return new core::ModelsTab(*this, modelKinds(), p); } });
    // §33 showpiece: the character meshes grouped by the authored character/variation path, with a
    // Character / Set / Mode / Part funnel. A ModelsTab over the "character" kind, so it inherits the
    // viewport, list, filters and the mesh-export action.
    t.append({ QStringLiteral("Characters"), QStringLiteral("characters"), false,
               [this](QWidget* p) -> core::BrowserTab* { return new core::ModelsTab(*this, { QStringLiteral("character") }, p, QStringLiteral("characters")); } });
    // §33 showpiece: the weapon meshes grouped by the authored common/hardware/weapons path, with a
    // Class / Weapon / Skin / View / Part funnel. A ModelsTab over the "weapon" kind, so it reuses the
    // viewport, list, filters and the mesh/glTF export actions — the weapon id is already the real name.
    t.append({ QStringLiteral("Weapons"), QStringLiteral("weapons"), false,
               [this](QWidget* p) -> core::BrowserTab* { return new core::ModelsTab(*this, { QStringLiteral("weapon") }, p, QStringLiteral("weapons")); } });
    // §33 showpiece: the vehicle meshes grouped by the authored common/hardware/vehicles path, with a
    // Class / Vehicle / Skin / Part / View funnel. A ModelsTab over the "vehicle" kind — same reuse.
    t.append({ QStringLiteral("Vehicles"), QStringLiteral("vehicles"), false,
               [this](QWidget* p) -> core::BrowserTab* { return new core::ModelsTab(*this, { QStringLiteral("vehicle") }, p, QStringLiteral("vehicles")); } });
    t.append({ QStringLiteral("Textures"), QStringLiteral("textures"), false,
               [this](QWidget* p) -> core::BrowserTab* { return new core::TexturesTab(*this, textureKinds(), p); } });
    t.append({ QStringLiteral("Bulk"), QStringLiteral("bulk"), true,
               [this](QWidget* p) -> core::BrowserTab* {
                   return new core::BulkTab(*this, modelKinds(), textureKinds(), p);
               } });
    return t;
}

bool Bf6Plugin::isConfigured() { return hasDataAt(gameDir()); }

bool Bf6Plugin::configureFolder(QWidget* parent)
{
    const QString start = gameDir().isEmpty() ? QDir::homePath() : gameDir();
    const QString dir = QFileDialog::getExistingDirectory(
        parent, QStringLiteral("Select the Battlefield 6 folder (or its Data folder)"), start);
    if (dir.isEmpty()) return false;
    QString data = dir;
    // Accept the game root (…/Battlefield 6) or the Data folder directly.
    if (!hasDataAt(data) && hasDataAt(QDir(data).filePath(QStringLiteral("Data"))))
        data = QDir(data).filePath(QStringLiteral("Data"));
    setGameDir(data);
    if (!isConfigured())
        QMessageBox::warning(parent, QStringLiteral("No game data"),
                             QStringLiteral("Couldn't find Win32\\characters.toc under %1.\nPick the "
                                            "Battlefield 6 install folder (or its Data folder).").arg(data));
    return true;
}

QString Bf6Plugin::firstRunText() const
{
    return QStringLiteral(
        "Pick your <b>Battlefield 6</b> install folder (the one with <code>bf6.exe</code>) or its "
        "<code>Data</code> folder. The browser reads the game's Frostbite superbundles in place — "
        "nothing is copied, dumped or modified.<br><br>"
        "This early build lists, searches, filters and raw-exports every character resource "
        "(decompressed). In-app 3D rendering and cooked mesh/texture export are the next milestone.");
}

void Bf6Plugin::reload(std::function<void(bool, const QString&)> done, std::function<void()> secondaryReady)
{
    const QString dir = gameDir();
    if (!hasDataAt(dir)) { done(false, QStringLiteral("No Win32\\characters.toc under the configured folder.")); return; }

    Bf6Store* store = &m_store;
    auto onGui = [](std::function<void()> fn) { QMetaObject::invokeMethod(qApp, std::move(fn), Qt::QueuedConnection); };
    ++m_workers;
    QThread* th = QThread::create([store, dir, done, secondaryReady, onGui, this] {
        QString err;
        auto progress = [onGui](const QString& s) { onGui([s] { qInfo("index: %s", qUtf8Printable(s)); }); };
        const bool ok = store->open(dir, &err, progress);
        const QString status = ok ? QStringLiteral("Ready — %1").arg(store->diagnostics())
                                   : QStringLiteral("Index failed: %1").arg(err);
        onGui([done, ok, status, secondaryReady, this] {
            done(ok, status);
            if (ok) secondaryReady();
            --m_workers;
        });
    });
    QObject::connect(th, &QThread::finished, th, &QObject::deleteLater);
    th->start();
}

QVector<core::SettingsPage> Bf6Plugin::settingsPages()
{
    core::SettingsPage dirs;
    dirs.title = QStringLiteral("Directories");
    dirs.order = 0;
    dirs.build = [this](QWidget* parent) -> QWidget* {
        auto* w = new QWidget(parent);
        auto* v = new QVBoxLayout(w);
        auto* box = new QGroupBox(QStringLiteral("Directories"), w);
        auto* form = new QFormLayout(box);
        auto* row = new QWidget(box);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        m_dirEdit = new QLineEdit(row);
        auto* btn = new QPushButton(QStringLiteral("Browse…"), row);
        QLineEdit* e = m_dirEdit;
        QObject::connect(btn, &QPushButton::clicked, row, [row, e] {
            const QString d = QFileDialog::getExistingDirectory(row, QStringLiteral("Battlefield 6 Data folder"), e->text());
            if (!d.isEmpty()) e->setText(d);
        });
        h->addWidget(m_dirEdit, 1);
        h->addWidget(btn);
        form->addRow(QStringLiteral("Game Data folder:"), row);

        // The reshax BF6 tools folder (holds Fb_bf6_mesh.exe + its DLLs) — drives cooked mesh export.
        auto* trow = new QWidget(box);
        auto* th = new QHBoxLayout(trow);
        th->setContentsMargins(0, 0, 0, 0);
        m_toolsEdit = new QLineEdit(trow);
        auto* tbtn = new QPushButton(QStringLiteral("Browse…"), trow);
        QLineEdit* te = m_toolsEdit;
        QObject::connect(tbtn, &QPushButton::clicked, trow, [trow, te] {
            const QString d = QFileDialog::getExistingDirectory(trow, QStringLiteral("reshax BF6 tools folder"), te->text());
            if (!d.isEmpty()) te->setText(d);
        });
        th->addWidget(m_toolsEdit, 1);
        th->addWidget(tbtn);
        form->addRow(QStringLiteral("Export tools folder:"), trow);

        v->addWidget(box);
        auto* note = new QLabel(QStringLiteral("Changing the Data folder reloads the asset index when you press OK. "
                                               "The export tools folder is the extracted reshax \"Battlefield 6 tools\" "
                                               "(with Fb_bf6_mesh.exe) used by \"Export mesh\" on the Models tab."), w);
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color: palette(mid);"));
        v->addWidget(note);
        v->addStretch(1);
        m_origDir = gameDir();   m_dirEdit->setText(m_origDir);
        m_origTools = toolsDir(); m_toolsEdit->setText(m_origTools);
        return w;
    };
    dirs.reload = [this] {
        if (m_dirEdit)   { m_origDir = gameDir();   m_dirEdit->setText(m_origDir); }
        if (m_toolsEdit) { m_origTools = toolsDir(); m_toolsEdit->setText(m_origTools); }
    };
    dirs.apply = [this] {
        if (m_dirEdit) {
            const QString g = m_dirEdit->text().trimmed();
            setGameDir(g);
            if (g != m_origDir) m_reloadRequested = true;
        }
        if (m_toolsEdit) setToolsDir(m_toolsEdit->text().trimmed());
    };
    dirs.keepPrefixes = { QLatin1String(kGameDir), QLatin1String(kToolsDir) };
    return { dirs };
}

QString Bf6Plugin::informationHtml() const
{
    return QStringLiteral(
        "<h3>Battlefield 6 storage</h3>"
        "<p>Content lives in Frostbite <b>superbundles</b> (<code>Win32/characters.toc</code> …) whose "
        "tables locate resources inside the <code>cas_NN.cas</code> content store. The TOC is read in "
        "place after a fixed 556-byte header; resources are concatenated compressed blocks "
        "(<b>raw</b> / <b>zstd</b> / <b>Oodle</b>). No dump and no key are needed.</p>"
        "<p>Resources are classified by their <b>authored Frostbite type</b> (MeshSet, Texture, "
        "EAClothData …), not by name. The cooked object formats are not decoded yet, so <b>Models</b>/"
        "<b>Textures</b> list and raw-export now; in-app rendering is the next milestone.</p>");
}

core::AppPlugin::Nouns Bf6Plugin::nouns() const
{
    Nouns n;
    n.id = QStringLiteral("name");
    n.modelExt = QStringLiteral("resource");
    return n;
}

void Bf6Plugin::decorateTab(const QString& stableId, core::BrowserTab* tab)
{
    if (stableId == QStringLiteral("models") || stableId == QStringLiteral("characters")
        || stableId == QStringLiteral("weapons") || stableId == QStringLiteral("vehicles")) {
        auto* mt = static_cast<core::ModelsTab*>(tab);
        const bool isChars = stableId == QStringLiteral("characters");
        // BF6 meshes aren't reliably wound; show both sides so nothing goes missing (DR does the same).
        if (mt->view()) mt->view()->setBackfaceCull(false);
        QObject::connect(mt, &core::ModelsTab::rowMenuAboutToShow, mt,
            [this, mt, isChars](QMenu* menu, const QVector<core::AssetId>& ids) {
                if (ids.size() != 1 || !m_store.isMeshId(ids.first())) return;
                const core::AssetId id = ids.first();
                menu->addSeparator();
                // On the Characters tab, the first action assembles the WHOLE outfit (every cosmetic
                // mesh for this char+set) rather than the single clicked mesh part.
                if (isChars) {
                    menu->addAction(QStringLiteral("Export FULL character (all cosmetics → one glTF; loose option)…"), mt, [this, mt, id] { exportChar(mt, id); });
                    menu->addAction(QStringLiteral("Export as .blend for Blender (ready to open)…"), mt, [this, mt, id] { exportBlend(mt, id); });
                }
                menu->addAction(QStringLiteral("Export model (glTF .glb — mesh + textures + rig, one file)…"), mt, [this, mt, id] { exportGlb(mt, id); });
                menu->addAction(QStringLiteral("Export mesh (SMD + ASCII) via tools…"), mt, [this, mt, id] { exportMesh(mt, id); });
            });
        return;
    }
    if (stableId == QStringLiteral("textures")) {
        auto* tt = static_cast<core::TexturesTab*>(tab);
        QObject::connect(tt, &core::TexturesTab::rowMenuAboutToShow, tt,
            [this, tt](QMenu* menu, const QVector<core::AssetId>& ids) {
                QVector<core::AssetId> texs;
                for (const core::AssetId& id : ids) if (m_store.isTextureId(id)) texs << id;
                if (texs.isEmpty()) return;
                menu->addSeparator();
                const QString label = texs.size() > 1 ? QStringLiteral("Export %1 textures (DDS)…").arg(texs.size())
                                                      : QStringLiteral("Export texture (DDS, full-res)…");
                menu->addAction(label, tt, [this, tt, texs] { exportTextures(tt, texs); });
            });
        return;
    }
}

void Bf6Plugin::exportTextures(QWidget* parent, const QVector<core::AssetId>& ids)
{
    if (ids.isEmpty()) return;
    const QString outDir = QFileDialog::getExistingDirectory(parent, QStringLiteral("Export texture(s) to folder…"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    if (outDir.isEmpty()) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    int ok = 0; QString lastErr;
    for (const core::AssetId& id : ids) {
        QString path, err;
        if (m_store.exportTextureDds(id, outDir, &path, &err)) ++ok; else lastErr = err;
    }
    QApplication::restoreOverrideCursor();
    if (ok > 0)
        QMessageBox::information(parent, QStringLiteral("Export complete"),
            QStringLiteral("Wrote %1 full-resolution .dds file(s) to:\n%2").arg(ok).arg(outDir));
    else
        QMessageBox::warning(parent, QStringLiteral("Export failed"), lastErr);
}

void Bf6Plugin::exportGlb(QWidget* parent, const core::AssetId& id)
{
    if (!m_store.isMeshId(id)) return;
    const QString name = m_store.searchBlob(id).section(QLatin1Char(' '), 0, 0).section(QLatin1Char('/'), -1);
    const QString start = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                              .filePath((name.isEmpty() ? QStringLiteral("model") : name) + QStringLiteral(".glb"));
    const QString path = QFileDialog::getSaveFileName(parent, QStringLiteral("Export model (glTF .glb)…"),
                                                      start, QStringLiteral("glTF binary (*.glb)"));
    if (path.isEmpty()) return;
    core::ExportOptions opt;          // self-contained: embed textures + rig, single .glb
    opt.generator = productName();
    opt.maxTextureSize = QSettings().value(QStringLiteral("export/textureRes"), 0).toInt();   // Export-settings resolution (0 = highest)
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const core::model::ExportResult res = core::model::exportOne(m_store, id, opt, path);
    QApplication::restoreOverrideCursor();
    if (res.ok)
        QMessageBox::information(parent, QStringLiteral("Export complete"),
            QStringLiteral("Wrote %1\n\n%2\n\nMesh, textures and the 291-bone rig + skin weights are all "
                           "embedded in this one file — no external dependencies.").arg(path, res.summary));
    else
        QMessageBox::warning(parent, QStringLiteral("Export failed"), res.error);
}

QString Bf6Plugin::runCharExport(const QString& matchSubstr, const QString& outDir, bool loose, int texCap, bool* okOut)
{
    if (okOut) *okOut = false;
    // A saved export decodes at full resolution; texCap (max texture side in px, 0 = native incl. 4K)
    // optionally limits it. The live grid/preview caps are a separate concern. Restored on exit.
    const int prevCap = m_store.setDecodeTextureCap(texCap);
    const auto restoreCap = qScopeGuard([this, prevCap] { m_store.setDecodeTextureCap(prevCap); });
    // Assemble the FULL character for one char+set: every visible cosmetic mesh (body, headgear, cloth,
    // patches, velcro, ghillie, backpack, goggles — NOT the 1st-person arms) merged into ONE scene. All
    // share the 291-bone ske_soldier_3p, so joint indices line up with no remap; we just offset
    // vertices/indices/material slots and concatenate.
    const QStringList all = m_store.meshNamesMatching(matchSubstr, 1000000);
    QStringList meshes;
    for (const QString& n : all) {
        if (!n.endsWith(QLatin1String("_mesh"))) continue;
        if (n.contains(QLatin1String("_1p_mesh")) || n.contains(QLatin1String("_1parms_mesh"))) continue;
        if (!meshes.contains(n)) meshes << n;   // the index lists duplicates
    }
    core::Geometry g;
    QVector<core::Material> mats;
    core::Skeleton skel;
    QStringList parts;
    for (const QString& n : meshes) {
        const core::AssetId id = m_store.findMesh(n);
        if (!id.isValid()) continue;
        const core::model::Loaded l = core::model::load(m_store, id, false, true, false);
        if (!l.ok || l.geo.vertices.isEmpty()) continue;
        if (skel.joints.isEmpty()) skel = l.skeleton;
        const int vBase = g.vertices.size(), iBase = g.indices.size(), mBase = g.materialNames.size();
        g.vertices += l.geo.vertices;
        for (quint32 idx : l.geo.indices) g.indices << (quint32(vBase) + idx);
        for (core::Part p : l.geo.parts) { p.indexStart += quint32(iBase); if (p.materialIndex >= 0) p.materialIndex += mBase; g.parts << p; }
        g.materialNames += l.geo.materialNames;
        mats += l.materials;
        g.skinned = g.skinned || l.geo.skinned; g.hasUv1 = g.hasUv1 || l.geo.hasUv1; g.hasColor = g.hasColor || l.geo.hasColor;
        parts << n.section(QLatin1Char('/'), -1);
    }
    if (g.vertices.isEmpty())
        return QStringLiteral("no cosmetic meshes matched '%1'").arg(matchSubstr);

    g.skinned = !skel.joints.isEmpty();
    g.computeBounds();
    const QRegularExpression re(QStringLiteral("(cha\\d+\\w*_set_\\d+)"));
    const QRegularExpressionMatch nm = re.match(meshes.isEmpty() ? matchSubstr : meshes.first());
    const QString key = nm.hasMatch() ? nm.captured(1) : QString();   // cha0045hawk_set_002
    const QString stem = (key.isEmpty() ? QStringLiteral("character") : key) + QStringLiteral("_full");
    QDir().mkpath(outDir);
    // Loose mode writes a text .gltf + sidecar .bin + external PNGs (our interpreted maps) instead of a
    // single self-contained .glb; the default stays .glb.
    const QString path = QDir(outDir).filePath(stem + (loose ? QStringLiteral(".gltf") : QStringLiteral(".glb")));
    core::glb::Scene scene; scene.geometry = &g; scene.skeleton = skel.joints.isEmpty() ? nullptr : &skel; scene.materials = mats;
    core::ExportOptions opt; opt.generator = productName();
    if (loose) { opt.looseTextures = true; opt.gltfSeparate = true; }
    core::glb::Result r;
    if (!core::glb::write(scene, opt, path, &r))
        return QStringLiteral("export failed: %1").arg(r.error);

    // Loose mode also dumps EVERY source texture for this char+set (not just the three maps we interpret)
    // into a sidecar _src folder with a manifest, so the material can be rebuilt by hand and any map type
    // we don't yet read is still visible. The .gltf carries our interpreted maps; the folder the originals.
    int srcTex = 0;
    if (loose && !key.isEmpty()) {
        QString manifest;
        srcTex = m_store.dumpCharSourceTextures(key, QDir(outDir).filePath(stem + QStringLiteral("_src")), &manifest);
    }
    if (okOut) *okOut = true;
    QString s = QStringLiteral("%1 | %2 part(s), %3 material(s), %4 image(s), %5 bones | %6")
                    .arg(path).arg(g.parts.size()).arg(r.materialsWritten).arg(r.imagesWritten)
                    .arg(skel.size()).arg(parts.join(QLatin1Char('+')));
    if (loose) s += QStringLiteral(" | + %1 raw source texture(s) in %2_src/").arg(srcTex).arg(stem);
    return s;
}

QString Bf6Plugin::runBlendExport(const QString& matchSubstr, const QString& outDir, int texCap,
                                  const QString& blenderExe, bool* okOut)
{
    if (okOut) *okOut = false;
    const QString exe = core::blender::findBlender(blenderExe.isEmpty()
        ? QSettings().value(QStringLiteral("bf6/blenderPath")).toString() : blenderExe);
    if (exe.isEmpty())
        return QStringLiteral("no Blender found — pass --bf6-blender <blender.exe>, set bf6/blenderPath, or use the GUI dialog");

    // Assemble the full character to a self-contained .glb (non-loose), then import that into Blender.
    bool gok = false;
    const QString gmsg = runCharExport(matchSubstr, outDir, /*loose*/ false, texCap, &gok);
    if (!gok) return QStringLiteral("glb assembly failed: %1").arg(gmsg);

    QDir d(outDir);
    const QStringList glbs = d.entryList(QStringList{ QStringLiteral("*_full.glb") }, QDir::Files, QDir::Time);
    if (glbs.isEmpty()) return QStringLiteral("assembled glb not found in %1").arg(outDir);
    const QString glb = d.filePath(glbs.first());
    const QString blend = glb.chopped(4) + QStringLiteral(".blend");
    QString log, err;
    if (!core::blender::toBlend(exe, glb, blend, &log, &err))
        return QStringLiteral("Blender step failed: %1").arg(err);
    if (okOut) *okOut = true;
    return QStringLiteral("%1  (via %2)").arg(blend, QFileInfo(exe).fileName());
}

void Bf6Plugin::exportBlend(QWidget* parent, const core::AssetId& id)
{
    const QString name = m_store.searchBlob(id).section(QLatin1Char(' '), 0, 0);
    const QRegularExpressionMatch nm = QRegularExpression(QStringLiteral("(cha\\d+\\w*_set_\\d+)")).match(name);
    if (!nm.hasMatch()) {
        QMessageBox::warning(parent, QStringLiteral("Export .blend"),
            QStringLiteral("This row isn't a character-set mesh (no cha####_set_### in its name)."));
        return;
    }
    const QString key = nm.captured(1);

    // Blender executable: a saved path, else auto-detect, else ask once and remember it.
    QString exe = core::blender::findBlender(QSettings().value(QStringLiteral("bf6/blenderPath")).toString());
    if (exe.isEmpty()) {
        QMessageBox::information(parent, QStringLiteral("Locate Blender"),
            QStringLiteral("Point to your Blender executable once — it'll be remembered. "
                           "(Blender's glTF importer is built in; no add-on is needed.)"));
        const QString filter =
#ifdef Q_OS_WIN
            QStringLiteral("Blender (blender.exe)");
#else
            QString();
#endif
        exe = QFileDialog::getOpenFileName(parent, QStringLiteral("Locate Blender"), QString(), filter);
        if (exe.isEmpty()) return;
        QSettings().setValue(QStringLiteral("bf6/blenderPath"), exe);
    }

    const QString dir = QFileDialog::getExistingDirectory(parent,
        QStringLiteral("Export %1 as .blend — choose a folder…").arg(key),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    if (dir.isEmpty()) return;

    // A busy, window-modal dialog instead of a frozen window: the Blender step waits on an event loop
    // (BlenderExport), so the dialog animates and the window stays live while Blender runs; modal blocks
    // a second export. The brief glb assembly before it is synchronous (a second or two).
    QProgressDialog prog(QStringLiteral("Exporting %1 to Blender…\nAssembling the character and importing "
                                        "it — this can take up to a minute.").arg(key),
                         QString() /* no cancel */, 0, 0, parent);
    prog.setWindowTitle(QStringLiteral("Export .blend"));
    prog.setWindowModality(Qt::WindowModal);
    prog.setMinimumDuration(0);
    prog.setValue(0);
    prog.show();
    QApplication::processEvents();
    bool ok = false;
    const int texCap = QSettings().value(QStringLiteral("export/textureRes"), 0).toInt();   // Export-settings resolution
    const QString msg = runBlendExport(key, dir, texCap, exe, &ok);
    prog.close();
    if (ok)
        QMessageBox::information(parent, QStringLiteral("Export complete"),
            QStringLiteral("Saved a ready Blender file — textures packed in, materials already wired "
                           "(base colour, metallic-roughness, normal, IOR):\n\n%1\n\nJust open it in Blender; "
                           "no import step and no add-on.").arg(msg));
    else
        QMessageBox::warning(parent, QStringLiteral("Export failed"), msg);
}

void Bf6Plugin::exportChar(QWidget* parent, const core::AssetId& id)
{
    // The clicked row is one mesh part; derive its char+set and assemble the whole outfit from that.
    const QString name = m_store.searchBlob(id).section(QLatin1Char(' '), 0, 0);
    const QRegularExpressionMatch nm = QRegularExpression(QStringLiteral("(cha\\d+\\w*_set_\\d+)")).match(name);
    if (!nm.hasMatch()) {
        QMessageBox::warning(parent, QStringLiteral("Export full character"),
            QStringLiteral("This row isn't a character-set mesh (no cha####_set_### in its name)."));
        return;
    }
    const QString key = nm.captured(1);

    // A non-native folder picker so we can host the loose-textures checkbox inside it.
    QFileDialog dlg(parent, QStringLiteral("Export full %1 — choose a folder…").arg(key),
                    QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    dlg.setFileMode(QFileDialog::Directory);
    dlg.setOption(QFileDialog::ShowDirsOnly, true);
    dlg.setOption(QFileDialog::DontUseNativeDialog, true);
    auto* cb = new QCheckBox(QStringLiteral("Loose textures — write .gltf + external PNGs, and dump every raw "
                                            "source map (incl. the _id masks) to a _src folder"), &dlg);
    if (auto* lay = qobject_cast<QGridLayout*>(dlg.layout()))
        lay->addWidget(cb, lay->rowCount(), 0, 1, lay->columnCount());
    if (dlg.exec() != QDialog::Accepted || dlg.selectedFiles().isEmpty()) return;

    const int texCap = QSettings().value(QStringLiteral("export/textureRes"), 0).toInt();   // Export-settings resolution
    QApplication::setOverrideCursor(Qt::WaitCursor);
    bool ok = false;
    const QString msg = runCharExport(key, dlg.selectedFiles().first(), cb->isChecked(), texCap, &ok);
    QApplication::restoreOverrideCursor();
    if (ok)
        QMessageBox::information(parent, QStringLiteral("Export complete"),
            QStringLiteral("Wrote the full %1 character (every cosmetic mesh merged on the shared 291-bone "
                           "rig):\n\n%2%3").arg(key, msg,
                cb->isChecked() ? QStringLiteral("\n\nLoose mode: the textures are external PNGs and the _src "
                                                 "folder holds the untouched source maps — see TEXTURES.txt.")
                                : QString()));
    else
        QMessageBox::warning(parent, QStringLiteral("Export failed"), msg);
}

QString Bf6Plugin::runMeshExport(const core::AssetId& id, const QString& tools, const QString& outDir, bool* okOut)
{
    if (okOut) *okOut = false;
    const QString exe = QDir(tools).filePath(QStringLiteral("Fb_bf6_mesh.exe"));
    if (tools.isEmpty() || !QFileInfo::exists(exe))
        return QStringLiteral("tools folder not set, or Fb_bf6_mesh.exe not found there");
    QTemporaryDir temp;
    if (!temp.isValid()) return QStringLiteral("could not create a temp folder");

    QString meshFile, err;
    if (!m_store.extractMeshDump(id, temp.path(), meshFile, &err)) return QStringLiteral("extract: %1").arg(err);
    // The mesh tool resolves arg[1] relative to its OWN exe dir, so the skeleton goes beside the exe.
    const QString skelFile = QDir(tools).filePath(QStringLiteral("__bf6ab_skel.ebx"));
    QString skelArg, skErr;
    if (m_store.extractEbx(QStringLiteral("ske_soldier_3p"), skelFile, &skErr)) skelArg = QStringLiteral("__bf6ab_skel.ebx");

    QProcess proc;
    proc.setWorkingDirectory(temp.path());
    QStringList args{ meshFile };
    if (!skelArg.isEmpty()) args << skelArg;
    proc.start(exe, args);
    const bool finished = proc.waitForStarted(10000) && proc.waitForFinished(180000);
    QFile::remove(skelFile);

    const QString base = QFileInfo(meshFile).completeBaseName();
    QDir().mkpath(outDir);
    int moved = 0;
    for (const QString& ext : { QStringLiteral(".smd"), QStringLiteral(".ascii") }) {
        const QString src = QDir(temp.path()).filePath(base + ext);
        if (QFile::exists(src)) {
            const QString dst = QDir(outDir).filePath(base + ext);
            QFile::remove(dst);
            if (QFile::rename(src, dst)) ++moved;
        }
    }
    if (moved > 0) { if (okOut) *okOut = true; return QStringLiteral("%1.smd + .ascii -> %2").arg(base, outDir); }
    return (finished ? QStringLiteral("tool ran but wrote no output: ") : QStringLiteral("tool did not finish: "))
           + QString::fromLocal8Bit(proc.readAllStandardOutput()).left(800);
}

void Bf6Plugin::exportMesh(QWidget* parent, const core::AssetId& id)
{
    const QString tools = toolsDir();
    if (tools.isEmpty() || !QFileInfo::exists(QDir(tools).filePath(QStringLiteral("Fb_bf6_mesh.exe")))) {
        QMessageBox::warning(parent, QStringLiteral("Export tools not set"),
            QStringLiteral("Set the reshax BF6 tools folder (the extracted \"Battlefield 6 tools\" with "
                           "Fb_bf6_mesh.exe) in Settings ▸ Directories first."));
        return;
    }
    const QString outDir = QFileDialog::getExistingDirectory(parent, QStringLiteral("Export mesh to folder…"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    if (outDir.isEmpty()) return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    bool ok = false;
    const QString msg = runMeshExport(id, tools, outDir, &ok);
    QApplication::restoreOverrideCursor();
    if (ok)
        QMessageBox::information(parent, QStringLiteral("Export complete"),
            QStringLiteral("Wrote %1\n\nRigged mesh with skeleton + skin weights (Valve SMD and XNALara ASCII).").arg(msg));
    else
        QMessageBox::warning(parent, QStringLiteral("Export failed"), msg);
}

core::gl::ViewFeatures Bf6Plugin::viewFeatures() const
{
    // Neutral, undistracting viewing defaults (user spec 2026-11-03): ACES tonemap (filmic contrast +
    // highlight rolloff) and geometric spec-AA (kills sparkle on semi-gloss fabric). IBL, shadows and
    // SSAO are OFF — a flat, even read of the model with no distracting studio lighting; all five stay
    // live toggles in the RENDER panel. Neutral light preset + no gradient come from the overrides.
    core::gl::ViewFeatures f;
    f.tonemap = f.specAA = true;
    return f;
}

QStringList Bf6Plugin::parseArgs(const QStringList& args)
{
    QStringList consumed;
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--bf6-export") && i + 1 < args.size()) { m_exportSubstr = args[i + 1]; consumed << args[i] << args[i + 1]; ++i; }
        else if (args[i] == QLatin1String("--bf6-export-tex") && i + 1 < args.size()) { m_exportTexSubstr = args[i + 1]; consumed << args[i] << args[i + 1]; ++i; }
        else if (args[i] == QLatin1String("--bf6-export-glb") && i + 1 < args.size()) { m_exportGlbSubstr = args[i + 1]; consumed << args[i] << args[i + 1]; ++i; }
        else if (args[i] == QLatin1String("--bf6-sheet") && i + 1 < args.size()) { m_sheetSubstr = args[i + 1]; consumed << args[i] << args[i + 1]; ++i; }
        else if (args[i] == QLatin1String("--bf6-export-char") && i + 1 < args.size()) { m_exportCharSubstr = args[i + 1]; consumed << args[i] << args[i + 1]; ++i; }
        else if (args[i] == QLatin1String("--bf6-show-tex") && i + 1 < args.size()) { m_showTexSubstr = args[i + 1]; consumed << args[i] << args[i + 1]; ++i; }
        else if (args[i] == QLatin1String("--bf6-show-model") && i + 1 < args.size()) { m_showModelSubstr = args[i + 1]; consumed << args[i] << args[i + 1]; ++i; }
        else if (args[i] == QLatin1String("--bf6-list") && i + 1 < args.size()) { m_listSubstr = args[i + 1]; consumed << args[i] << args[i + 1]; ++i; }
        else if (args[i] == QLatin1String("--bf6-dump-res") && i + 1 < args.size()) { m_dumpResSubstr = args[i + 1]; consumed << args[i] << args[i + 1]; ++i; }
        else if (args[i] == QLatin1String("--bf6-ebx-index")) { m_ebxIndex = true; consumed << args[i]; }
        else if (args[i] == QLatin1String("--bf6-chars")) { m_listChars = true; consumed << args[i]; }
        else if (args[i] == QLatin1String("--bf6-weapons")) { m_listWeapons = true; consumed << args[i]; }
        else if (args[i] == QLatin1String("--bf6-vehicles")) { m_listVehicles = true; consumed << args[i]; }
        else if (args[i] == QLatin1String("--bf6-out") && i + 1 < args.size()) { m_exportOut = args[i + 1]; consumed << args[i] << args[i + 1]; ++i; }
        else if (args[i] == QLatin1String("--bf6-loose")) { m_exportLoose = true; consumed << args[i]; }
        else if (args[i] == QLatin1String("--bf6-texcap") && i + 1 < args.size()) { m_exportTexCap = args[i + 1].toInt(); consumed << args[i] << args[i + 1]; ++i; }
        else if (args[i] == QLatin1String("--bf6-blend") && i + 1 < args.size()) { m_exportBlendSubstr = args[i + 1]; consumed << args[i] << args[i + 1]; ++i; }
        else if (args[i] == QLatin1String("--bf6-blender") && i + 1 < args.size()) { m_blenderExe = args[i + 1]; consumed << args[i] << args[i + 1]; ++i; }
    }
    return consumed;
}

void Bf6Plugin::applyArgs(core::AppShell& shell)
{
    const QString out = m_exportOut.isEmpty() ? QDir::tempPath() : m_exportOut;
    QDir().mkpath(out);   // the glTF/glb writers don't create the target dir; a fresh --bf6-out must still work
    if (!m_exportSubstr.isEmpty()) {
        const core::AssetId id = m_store.findMesh(m_exportSubstr);
        if (!id.isValid()) qWarning("bf6-export: no MeshSet matching '%s'", qUtf8Printable(m_exportSubstr));
        else { bool ok = false; const QString msg = runMeshExport(id, toolsDir(), out, &ok); qInfo("bf6-export: %s -- %s", ok ? "OK" : "FAIL", qUtf8Printable(msg)); }
    }
    if (!m_exportTexSubstr.isEmpty()) {
        const core::AssetId id = m_store.findTexture(m_exportTexSubstr);
        if (!id.isValid()) qWarning("bf6-export-tex: no texture matching '%s'", qUtf8Printable(m_exportTexSubstr));
        else { QString path, err; const bool ok = m_store.exportTextureDds(id, out, &path, &err); qInfo("bf6-export-tex: %s -- %s", ok ? "OK" : "FAIL", ok ? qUtf8Printable(path) : qUtf8Printable(err)); }
    }
    if (!m_exportGlbSubstr.isEmpty()) {
        const core::AssetId id = m_store.findMesh(m_exportGlbSubstr);
        if (!id.isValid()) qWarning("bf6-export-glb: no MeshSet matching '%s'", qUtf8Printable(m_exportGlbSubstr));
        else {
            const QString name = m_store.searchBlob(id).section(QLatin1Char(' '), 0, 0).section(QLatin1Char('/'), -1);
            const QString path = QDir(out).filePath((name.isEmpty() ? QStringLiteral("model") : name) + QStringLiteral(".glb"));
            core::ExportOptions opt; opt.generator = productName();
            const core::model::ExportResult res = core::model::exportOne(m_store, id, opt, path);
            qInfo("bf6-export-glb: %s -- %s", res.ok ? "OK" : "FAIL", res.ok ? qUtf8Printable(res.summary) : qUtf8Printable(res.error));
        }
    }
    if (!m_sheetSubstr.isEmpty()) {
        // Batch: one default-body glb per codename matching the substring (e.g. "mp/main"), for an
        // identification contact sheet — the lowest set number is the operator's default look.
        const QStringList names = m_store.meshNamesMatching(m_sheetSubstr, 1000000);
        const QRegularExpression re(QStringLiteral("/(cha\\d+\\w*)/set/(set_\\d+)/\\1_\\2_mesh$"));
        QMap<QString, QPair<QString, QString>> best;   // codename -> (set, full name) with the lowest set
        for (const QString& n : names) {
            const QRegularExpressionMatch m = re.match(n);
            if (!m.hasMatch()) continue;
            const QString code = m.captured(1), set = m.captured(2);
            if (!best.contains(code) || set < best[code].first) best[code] = { set, n };
        }
        int ok = 0;
        for (auto it = best.constBegin(); it != best.constEnd(); ++it) {
            const core::AssetId id = m_store.findMesh(it.value().second);
            if (!id.isValid()) continue;
            core::ExportOptions opt; opt.generator = productName();
            opt.maxTextureSize = 512;   // identification contact sheet — small thumbs, not a full-res dump
            const QString path = QDir(out).filePath(it.key() + QStringLiteral(".glb"));
            if (core::model::exportOne(m_store, id, opt, path).ok) ++ok;
        }
        qInfo("bf6-sheet: exported %d/%d default-body glb(s) to %s", ok, int(best.size()), qUtf8Printable(out));
    }
    if (!m_exportCharSubstr.isEmpty()) {
        bool ok = false;
        const QString msg = runCharExport(m_exportCharSubstr, out, m_exportLoose, m_exportTexCap, &ok);
        qInfo("bf6-export-char: %s -- %s", ok ? "OK" : "FAIL", qUtf8Printable(msg));
    }
    if (!m_exportBlendSubstr.isEmpty()) {
        bool ok = false;
        const QString msg = runBlendExport(m_exportBlendSubstr, out, m_exportTexCap, m_blenderExe, &ok);
        qInfo("bf6-blend: %s -- %s", ok ? "OK" : "FAIL", qUtf8Printable(msg));
    }
    if (!m_showTexSubstr.isEmpty()) {
        shell.showTab(QStringLiteral("textures"));
        if (auto* tt = dynamic_cast<core::TexturesTab*>(shell.tab(QStringLiteral("textures"))))
            qInfo("bf6-show-tex: select '%s' -> %s", qUtf8Printable(m_showTexSubstr), tt->selectTexture(m_showTexSubstr) ? "ok" : "not found");
    }
    if (m_listChars) {
        // Funnel summary (Squad / Operator / Identified / Codename …).
        const QVector<core::TagGroup> g = m_store.tagGroups(QStringLiteral("character"));
        for (const auto& grp : g) {
            qInfo("bf6-chars funnel [%s]: %d", qUtf8Printable(grp.label), grp.entries.size());
            for (const auto& en : grp.entries) qInfo("    %-22s  %d", qUtf8Printable(en.label), en.count);
        }
        // Name layer end-to-end: the distinct real names now shown on character rows.
        QStringList seen;
        const int n = m_store.rowCount(QStringLiteral("character"));
        for (int i = 0; i < n; ++i) {
            const core::AssetRow r = m_store.row(QStringLiteral("character"), i);
            if (!r.inGameName.contains(QStringLiteral(" — "))) continue;   // identified rows carry "Operator — Skin"
            const QString key = r.inGameName.section(QStringLiteral(" · "), 0, 0);   // drop the part suffix
            if (seen.contains(key)) continue; seen << key;
            qInfo("  NAMED  %s", qUtf8Printable(key));
        }
        qInfo("bf6-chars: %d distinct identified operator/skin names shown", seen.size());
        // Verify the assembled "full character" rows: count them and assemble the first one.
        int fullRows = 0;
        for (int i = 0; i < n; ++i) if (m_store.row(QStringLiteral("character"), i).name.endsWith(QLatin1String("_full"))) ++fullRows;
        qInfo("bf6-chars: %d assembled full-character row(s)", fullRows);
        for (int i = 0; i < n; ++i) {
            const core::AssetRow r = m_store.row(QStringLiteral("character"), i);
            if (!r.name.endsWith(QLatin1String("_full"))) continue;
            core::Geometry g; QString err;
            const bool ok = m_store.loadModel(r.id, g, &err);
            qInfo("  full[%s] -> %s (%d verts, %d parts)%s", qUtf8Printable(r.inGameName),
                  ok ? "OK" : "FAIL", g.vertices.size(), g.parts.size(), ok ? "" : qUtf8Printable(QStringLiteral(" : %1").arg(err)));
            break;   // one is enough for the smoke test
        }
    }
    if (m_listWeapons) {
        // Weapons funnel summary (Class / Weapon / Skin / View / Part) + a sample of row display names,
        // so the §33 Weapons tab can be verified headlessly (no GL window needed).
        const QVector<core::TagGroup> g = m_store.tagGroups(QStringLiteral("weapon"));
        for (const auto& grp : g) {
            qInfo("bf6-weapons funnel [%s]: %d", qUtf8Printable(grp.label), grp.entries.size());
            int shown = 0;
            for (const auto& en : grp.entries) {
                qInfo("    %-20s  %d", qUtf8Printable(en.label), en.count);
                if (++shown >= 12) { qInfo("    … (%d more)", grp.entries.size() - shown); break; }
            }
        }
        const int n = m_store.rowCount(QStringLiteral("weapon"));
        qInfo("bf6-weapons: %d weapon mesh row(s); sample:", n);
        for (int i = 0; i < n && i < 20; ++i)
            qInfo("  row  %s", qUtf8Printable(m_store.row(QStringLiteral("weapon"), i).inGameName));
    }
    if (m_listVehicles) {
        // Vehicles funnel summary (Class / Vehicle / Skin / Part / View) + a sample of row display names.
        const QVector<core::TagGroup> g = m_store.tagGroups(QStringLiteral("vehicle"));
        for (const auto& grp : g) {
            qInfo("bf6-vehicles funnel [%s]: %d", qUtf8Printable(grp.label), grp.entries.size());
            int shown = 0;
            for (const auto& en : grp.entries) {
                qInfo("    %-20s  %d", qUtf8Printable(en.label), en.count);
                if (++shown >= 12) { qInfo("    … (%d more)", grp.entries.size() - shown); break; }
            }
        }
        const int n = m_store.rowCount(QStringLiteral("vehicle"));
        qInfo("bf6-vehicles: %d vehicle mesh row(s); sample:", n);
        for (int i = 0; i < n && i < 20; ++i)
            qInfo("  row  %s", qUtf8Printable(m_store.row(QStringLiteral("vehicle"), i).inGameName));
    }
    if (!m_listSubstr.isEmpty()) {
        const QStringList names = m_store.meshNamesMatching(m_listSubstr);
        qInfo("bf6-list '%s': %d mesh(es)", qUtf8Printable(m_listSubstr), names.size());
        for (const QString& n : names) qInfo("  mesh  %s", qUtf8Printable(n));
    }
    if (!m_dumpResSubstr.isEmpty()) {
        const int n = m_store.dumpResourcesMatching(m_dumpResSubstr, out, 100000);
        qInfo("bf6-dump-res '%s': %d resource(s) -> %s", qUtf8Printable(m_dumpResSubstr), n, qUtf8Printable(out));
    }
    if (m_ebxIndex) {
        const QString idx = QDir(out).filePath(QStringLiteral("ebx_index.tsv"));
        const int n = m_store.dumpEbxGuidIndex(idx);
        qInfo("bf6-ebx-index: %d EBX -> %s", n, qUtf8Printable(idx));
    }
    if (!m_showModelSubstr.isEmpty()) {
        const core::AssetId id = m_store.findMesh(m_showModelSubstr);
        shell.showTab(QStringLiteral("models"));
        if (id.isValid()) {
            if (auto* mt = dynamic_cast<core::ModelsTab*>(shell.tab(QStringLiteral("models")))) {
                mt->showModel(id);
                if (mt->view()) mt->view()->setShowSkeleton(true);   // draw the rig overlay in the shot
            }
        }
        qInfo("bf6-show-model: '%s' -> %s", qUtf8Printable(m_showModelSubstr), id.isValid() ? "shown" : "not found");
    }
}

void Bf6Plugin::shutdown()
{
    for (int i = 0; i < 200 && m_workers.load() > 0; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
}

}  // namespace bf6game
