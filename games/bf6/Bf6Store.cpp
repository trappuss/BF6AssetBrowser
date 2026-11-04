#include "games/bf6/Bf6Store.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QRegularExpression>
#include <QStringList>

#include "core/math/RigMath.h"
#include "bcdec.h"   // declarations only; BcdecImpl.cpp holds the single BCDEC_IMPLEMENTATION TU

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace bf6game {

QString Bf6Store::areaOf(const std::string& name)
{
    if (name.empty()) return QStringLiteral("other");
    const QString q = QString::fromStdString(name);
    if (!q.contains(QLatin1Char('/'))) return QStringLiteral("chunk");   // GUID-named chunk
    const QStringList seg = q.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (seg.isEmpty()) return QStringLiteral("other");
    if (seg.first() == QLatin1String("common") && seg.size() > 1) return seg.at(1);
    return seg.first();
}

QStringList Bf6Store::facetsFor(const bf6::Resource& r) const
{
    QStringList out;
    QString type;
    switch (r.kind) {
    case bf6::ResKind::Ebx:   type = QStringLiteral("EBX"); break;
    case bf6::ResKind::Chunk: type = QStringLiteral("Chunk"); break;
    case bf6::ResKind::Res:   type = r.typeName.empty() ? QStringLiteral("(unknown)") : QString::fromStdString(r.typeName); break;
    }
    out << QStringLiteral("type:") + type;
    out << QStringLiteral("area:") + areaOf(r.name);
    return out;
}

static core::TagGroup makeGroup(const QString& id, const QString& label,
                                const QHash<QString, int>& counts)
{
    core::TagGroup g; g.id = id; g.label = label;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
        g.entries.push_back({ id + QLatin1Char(':') + it.key(), it.key(), it.value() });
    std::sort(g.entries.begin(), g.entries.end(),
              [](const core::TagGroup::Entry& a, const core::TagGroup::Entry& b) {
                  return a.count != b.count ? a.count > b.count : a.label < b.label;
              });
    return g;
}

namespace {
// §33 character classification from the AUTHORED path (template §3): a MeshSet under
// common/characters/<mode>/main/<charId cha####name>/set/<setId>/<...>_mesh.
struct CharInfo { bool ok = false; QString charId, display, set, mode, part; };
CharInfo parseChar(const QString& name)
{
    CharInfo ci;
    const QStringList seg = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    const int ck = seg.indexOf(QStringLiteral("characters"));
    if (ck < 0 || ck + 1 >= seg.size()) return ci;
    ci.mode = seg.value(ck + 1);   // mp / sp / _soldier / shared …
    static const QRegularExpression reChar(QStringLiteral("^cha\\d"));
    static const QRegularExpression reSet(QStringLiteral("^set_?\\d"));
    static const QRegularExpression reTail(QStringLiteral("^cha\\d+(.*)$"));
    QString& charId = ci.charId;
    for (int i = ck + 1; i + 1 < seg.size(); ++i) {   // path folders only, not the filename
        if (charId.isEmpty() && reChar.match(seg[i]).hasMatch()) charId = seg[i];
        if (ci.set.isEmpty() && reSet.match(seg[i]).hasMatch()) ci.set = seg[i];
    }
    if (charId.isEmpty()) return ci;
    const QRegularExpressionMatch m = reTail.match(charId);
    const QString tail = (m.hasMatch() && !m.captured(1).isEmpty()) ? m.captured(1) : charId;
    ci.display = tail.left(1).toUpper() + tail.mid(1);
    QString fn = seg.last();
    fn.remove(charId + QLatin1Char('_'));
    if (!ci.set.isEmpty()) fn.remove(ci.set + QLatin1Char('_'));
    fn.remove(QStringLiteral("_mesh"));
    while (fn.endsWith(QLatin1Char('_'))) fn.chop(1);
    ci.part = fn.isEmpty() ? QStringLiteral("body") : fn;
    ci.ok = true;
    return ci;
}

// §33 weapon classification from the AUTHORED path (template §3): a MeshSet under
//   common/hardware/weapons/<class>/<weaponId>/art[/skins/<skinId>]/ob_wep_<class>_<weaponId>_<part>_<1p|3p>_mesh
// or, for attachments, .../weapons/_attachments/<type>/<id>/art[/skins/<skinId>]/ob_wepatt_<type>_<id>_<part>_*.
// The weapon id IS the recognizable designation (m4a1, g36, pkm…), so no name layer is needed — just casing.
struct WepInfo { bool ok = false; QString cls, weapon, skin, view, part, attachType; bool attachment = false; };
WepInfo parseWeapon(const QString& name)
{
    WepInfo wi;
    const QStringList seg = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    const int wk = seg.indexOf(QStringLiteral("weapons"));
    if (wk < 1 || wk + 2 >= seg.size()) return wi;
    // Real SHIPPED weapon art only, failing closed on dev/editor trees: under common/hardware/weapons/
    // (excludes the test/testdata/granite dev tree), a proper class folder (excludes the _forgeeditor
    // placeholder/editor set and any other _dev folder), and an art mesh filename ob_wep_/ob_wepatt_
    // (excludes lod/shadow/helper meshes such as meshp_invisibletriangle).
    if (seg.first() != QLatin1String("common")) return wi;
    if (seg.value(wk - 1) != QLatin1String("hardware")) return wi;
    const QString cat = seg.value(wk + 1);
    if (cat.startsWith(QLatin1Char('_')) && cat != QLatin1String("_attachments")) return wi;
    const QString fn = seg.last();
    const bool att = fn.startsWith(QLatin1String("ob_wepatt_"));
    if (!att && !fn.startsWith(QLatin1String("ob_wep_"))) return wi;   // the art meshes only, not lod/shadow
    wi.attachment = att;
    if (att) {
        if (wk + 3 >= seg.size()) return wi;
        wi.cls        = QStringLiteral("Attachment");
        wi.attachType = seg.value(wk + 2);     // angledgrip / reflex / muzzlebrake …
        wi.weapon     = seg.value(wk + 3);     // the attachment model id (folder)
    } else {
        wi.cls    = cat;                       // assaultrifle / smg / shotgun / melee …
        wi.weapon = seg.value(wk + 2);         // weapon id (folder) — already the real designation
    }
    if (wi.weapon.isEmpty()) return wi;
    const int sk = seg.indexOf(QStringLiteral("skins"));
    wi.skin = (sk >= 0 && sk + 1 < seg.size()) ? seg.value(sk + 1) : QStringLiteral("base");
    wi.view = fn.endsWith(QLatin1String("_1p_mesh")) ? QStringLiteral("1p")
            : fn.endsWith(QLatin1String("_3p_mesh")) ? QStringLiteral("3p")
            : QStringLiteral("shared");
    // Part from the filename: drop the ob_wep[att]_ prefix, the class/type token, the id token, an
    // optional skin token and the trailing view + _mesh. What remains names the component (base, sight,
    // magazine, barrel349mm …); nothing left means the weapon/attachment body itself → "base".
    QString body = fn;
    body.remove(QRegularExpression(QStringLiteral("^ob_wepatt_|^ob_wep_")));
    body.remove(QRegularExpression(QStringLiteral("_(1p|3p)?_?mesh$")));
    QStringList tok = body.split(QLatin1Char('_'), Qt::SkipEmptyParts);
    if (!tok.isEmpty()) tok.removeFirst();     // class / attachType
    if (!tok.isEmpty()) tok.removeFirst();     // id
    if (!tok.isEmpty() && tok.first() == wi.skin) tok.removeFirst();   // skin id (also carried in the path)
    wi.part = tok.isEmpty() ? QStringLiteral("base") : tok.join(QLatin1Char('_'));
    wi.ok = true;
    return wi;
}

// Row display: the weapon id upper-cased (gun designations like M4A1/G36/PKM read right), the skin when
// it isn't the base, the component part, and the view. Attachments lead with their type + model id.
QString displayWeapon(const WepInfo& wi)
{
    const QString head = wi.attachment ? (wi.attachType + QLatin1Char('/') + wi.weapon.toUpper())
                                        : wi.weapon.toUpper();
    QStringList bits;
    if (wi.skin != QLatin1String("base")) bits << wi.skin;
    bits << wi.part;
    return QStringLiteral("%1 · %2 (%3)").arg(head, bits.join(QStringLiteral(" · ")), wi.view);
}

// §33 vehicle classification — the weapons recipe with a vehicles root and three mesh prefixes:
//   common/hardware/vehicles/<class>/<vehicleId>/art[/skins/<skinId>]/ob_veh_<class>_<vehicleId>_<part>[_1p|_3p]_mesh
//   .../vehicles/_attachments/<type>/<id>/art[/skins]/ob_vehatt_<type>_<id>_<part>_mesh   (camera/weapon/rws)
//   .../vehicles/_projectiles/<type>/<id>/art/ob_prj_<type>_<id>_mesh                      (missile/bomb)
// The vehicle id is the real designation (cv90, bradley, uh60, f16…), so no name layer — just casing.
struct VehInfo { bool ok = false; QString cls, vehicle, skin, view, part, sub; bool attachment = false, projectile = false; };
VehInfo parseVehicle(const QString& name)
{
    VehInfo vi;
    const QStringList seg = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    const int vk = seg.indexOf(QStringLiteral("vehicles"));
    if (vk < 1 || vk + 2 >= seg.size()) return vi;
    if (seg.first() != QLatin1String("common")) return vi;              // real content only (not test data)
    if (seg.value(vk - 1) != QLatin1String("hardware")) return vi;      // the hardware/vehicles tree
    const QString cat = seg.value(vk + 1);
    const QString fn  = seg.last();
    const bool att = fn.startsWith(QLatin1String("ob_vehatt_"));
    const bool prj = fn.startsWith(QLatin1String("ob_prj_"));
    if (!att && !prj && !fn.startsWith(QLatin1String("ob_veh_"))) return vi;   // art meshes only
    vi.attachment = att; vi.projectile = prj;
    if (att || prj) {
        if (vk + 3 >= seg.size()) return vi;
        vi.cls     = att ? QStringLiteral("Attachment") : QStringLiteral("Projectile");
        vi.sub     = seg.value(vk + 2);        // camera / weapon / rws  OR  missile / bomb
        vi.vehicle = seg.value(vk + 3);        // the attachment/projectile model id (folder)
    } else {
        if (cat.startsWith(QLatin1Char('_'))) return vi;   // an unknown _dev folder (editor/placeholder)
        vi.cls     = cat;                      // tank / car / helicopter / boat / airplane / stationary
        vi.vehicle = seg.value(vk + 2);        // vehicle id (folder) — already the real designation
    }
    if (vi.vehicle.isEmpty()) return vi;
    const int sk = seg.indexOf(QStringLiteral("skins"));
    vi.skin = (sk >= 0 && sk + 1 < seg.size()) ? seg.value(sk + 1) : QStringLiteral("base");
    vi.view = fn.endsWith(QLatin1String("_1p_mesh")) ? QStringLiteral("1p")
            : fn.endsWith(QLatin1String("_3p_mesh")) ? QStringLiteral("3p")
            : QStringLiteral("shared");
    QString body = fn;
    body.remove(QRegularExpression(QStringLiteral("^ob_vehatt_|^ob_veh_|^ob_prj_")));
    body.remove(QRegularExpression(QStringLiteral("_(1p|3p)?_?mesh$")));
    QStringList tok = body.split(QLatin1Char('_'), Qt::SkipEmptyParts);
    if (!tok.isEmpty()) tok.removeFirst();     // class / type
    if (!tok.isEmpty()) tok.removeFirst();     // id
    if (!tok.isEmpty() && tok.first() == vi.skin) tok.removeFirst();   // skin id (also in the path)
    vi.part = tok.isEmpty() ? QStringLiteral("base") : tok.join(QLatin1Char('_'));
    vi.ok = true;
    return vi;
}

// Row display: the vehicle id upper-cased (CV90 / BRADLEY / F16), the skin when it isn't base, the part,
// and the view only when it's a 1p/3p variant (most vehicle parts are view-agnostic). Attachments and
// projectiles lead with their sub-type + model id (weapon/BROWNINGM2, missile/AGM88).
QString displayVehicle(const VehInfo& vi)
{
    const QString head = (vi.attachment || vi.projectile) ? (vi.sub + QLatin1Char('/') + vi.vehicle.toUpper())
                                                           : vi.vehicle.toUpper();
    QStringList bits;
    if (vi.skin != QLatin1String("base")) bits << vi.skin;
    bits << vi.part;
    QString s = QStringLiteral("%1 · %2").arg(head, bits.join(QStringLiteral(" · ")));
    if (vi.view != QLatin1String("shared")) s += QStringLiteral(" (%1)").arg(vi.view);
    return s;
}
}  // namespace

bool Bf6Store::open(const QString& dataDir, QString* error, core::ProgressFn progress)
{
    m_open = false;
    m_dir = dataDir;
    m_toc = bf6::Toc{};
    m_model.clear(); m_texture.clear(); m_file.clear(); m_tagGroups.clear();
    m_character.clear(); m_charFacets.clear(); m_charName.clear(); m_chunkByGuid.clear();
    m_weapon.clear(); m_weaponFacets.clear(); m_weaponName.clear();
    m_vehicle.clear(); m_vehicleFacets.clear(); m_vehicleName.clear();
    m_fullKeys.clear(); m_fullFacets.clear(); m_fullName.clear();
    m_hdToc = bf6::Toc{}; m_hdChunkByGuid.clear();

    const std::string dd = dataDir.toStdString();
    std::string err;

    if (progress) progress(QStringLiteral("Reading layout.toc…"));
    if (!m_toc.loadLayout(dd, &err)) { if (error) *error = QString::fromStdString(err); return false; }

    // Frostbite type names (shipped in the exe's Qt resource — reference data, not game content).
    { QFile tf(QStringLiteral(":/bf6/types.txt"));
      if (tf.open(QIODevice::ReadOnly)) m_toc.loadTypesData(tf.readAll().toStdString()); }

    // The name layer: user-confirmed codename+set -> real Squad/Operator/Skin (seed in the Qt
    // resource, overlaid by data/bf6_names.json). BF6 display names are server-side, so this is the
    // only source of real names; it fails closed to the raw codename stem when a codename is unknown.
    m_names.load();

    // Oodle runtime for 0x11 blocks — ships at the game root, beside Data/.
    { QDir up(dataDir); up.cdUp();
      const QString oodle = up.filePath(QStringLiteral("oo2core_9_win64.dll"));
      std::string oerr;
      if (!bf6::initOodle(oodle.toStdString(), &oerr) && progress)
          progress(QStringLiteral("Oodle not loaded (%1) — zstd/raw resources still read").arg(QString::fromStdString(oerr))); }

    if (progress) progress(QStringLiteral("Parsing characters.toc…"));
    const QString charToc = QDir(dataDir).filePath(QStringLiteral("Win32/characters.toc"));
    if (!m_toc.loadSuperbundle(charToc.toStdString(), &err)) { if (error) *error = QString::fromStdString(err); return false; }
    // Weapons superbundle — a sibling of characters.toc in the same Data/Win32, referencing the same
    // layout bundles, so it loads into the SAME m_toc: its MeshSets / Textures / Chunks append to the
    // shared resource list and resolve through the identical mesh/texture/export path. The §33 Weapons
    // tab is a ModelsTab over a "weapon" kind classified from these authored paths (see parseWeapon).
    // Graceful no-op when the pack isn't present.
    if (progress) progress(QStringLiteral("Parsing weapons.toc…"));
    const QString wepToc = QDir(dataDir).filePath(QStringLiteral("Win32/weapons.toc"));
    if (QFile::exists(wepToc)) { std::string werr; m_toc.loadSuperbundle(wepToc.toStdString(), &werr); }
    // Vehicles superbundle — same story as weapons: a sibling toc over the same layout, loaded into the
    // shared m_toc; its MeshSets feed the §33 Vehicles tab (a ModelsTab over a "vehicle" kind). No-op if absent.
    if (progress) progress(QStringLiteral("Parsing vehicles.toc…"));
    const QString vehToc = QDir(dataDir).filePath(QStringLiteral("Win32/vehicles.toc"));
    if (QFile::exists(vehToc)) { std::string verr; m_toc.loadSuperbundle(vehToc.toStdString(), &verr); }
    // ── HD texture pack (true 4K) ───────────────────────────────────────────────────────────────────
    // A 4K character texture keeps its top mip (the "HD mip0", 16 MiB for 4096²) in a SEPARATE streamed
    // chunk — in the HRES texture-pack superbundle under the Update/ overlay, NOT in characters.toc
    // (which carries only the 2048-down resident chain). Load that pack's chunk store into m_hdToc,
    // keyed by the chunk GUID, so hdMip0Chunk()/the exporters can emit true 4K. The same install-bundle
    // path ("…/detail/initialexperienceinstallpackage") is overlaid under Update/<pkg>/Data, so every
    // such Data dir is registered as a fallback cas root. Gracefully no-op when the HD pack isn't present.
    {
        QDir gameRoot(dataDir); gameRoot.cdUp();                         // …/Battlefield 6
        const QDir updDir(gameRoot.filePath(QStringLiteral("Update")));
        if (updDir.exists()) {
            m_hdToc.loadLayout(dd, nullptr);                            // persistentIndex→bundle from the base layout
            const QFileInfoList pkgs = updDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QFileInfo& pkg : pkgs) {
                const QString pkgData = pkg.filePath() + QStringLiteral("/Data");
                if (QDir(pkgData).exists()) m_hdToc.addCasRoot(pkgData.toStdString());
            }
            for (const QFileInfo& pkg : pkgs) {
                const QString hdToc = pkg.filePath()
                    + QStringLiteral("/Data/Win32/installation/detail/initialexperienceinstallpackage/characters.toc");
                if (QFile::exists(hdToc)) { std::string e2; m_hdToc.loadSuperbundle(hdToc.toStdString(), &e2); }
            }
            for (int i = 0; i < int(m_hdToc.resources().size()); ++i) {
                const bf6::Resource& r = m_hdToc.resources()[size_t(i)];
                if (r.kind != bf6::ResKind::Chunk) continue;
                const auto it = m_hdChunkByGuid.find(r.name);          // keep the largest per GUID (the mip0)
                if (it == m_hdChunkByGuid.end()) m_hdChunkByGuid.emplace(r.name, i);
                else if (r.size > m_hdToc.resources()[size_t(it->second)].size) it->second = i;
            }
            if (progress && !m_hdChunkByGuid.empty())
                progress(QStringLiteral("HD texture pack: %1 streamed 4K chunk(s) available").arg(m_hdChunkByGuid.size()));
        }
    }

    // Per-kind row lists + funnel tallies, classified by authored type (template §3).
    QHash<QString, int> mType, mArea, tType, tArea, fType, fArea;
    QHash<QString, int> cChar, cSet, cMode, cPart;   // §33 Characters funnel tallies
    QHash<QString, int> cSquad, cOp, cNamed;         // name-layer funnel tallies (squad/operator/identified)
    QHash<QString, int> fullKeyIndex;                // char+set key -> m_fullKeys index (dedup, one full row each)
    QHash<QString, int> wClass, wWeapon, wSkin, wView, wPart;   // §33 Weapons funnel tallies
    QHash<QString, int> vClass, vVehicle, vSkin, vPart, vView;  // §33 Vehicles funnel tallies
    const auto& res = m_toc.resources();
    m_file.reserve(int(res.size()));
    for (int i = 0; i < int(res.size()); ++i) {
        const bf6::Resource& r = res[size_t(i)];
        const QStringList f = facetsFor(r);
        const QString typeLbl = f.at(0).mid(5);   // after "type:"
        const QString areaLbl = f.at(1).mid(5);   // after "area:"
        m_file.push_back(i); ++fType[typeLbl]; ++fArea[areaLbl];
        if (r.kind == bf6::ResKind::Res) {
            if (r.typeName == "MeshSet") {
                m_model.push_back(i); ++mType[typeLbl]; ++mArea[areaLbl];
                const CharInfo ci = parseChar(QString::fromStdString(r.name));
                if (ci.ok) {
                    m_character.push_back(i);
                    QStringList cf{ QStringLiteral("char:") + ci.display };
                    if (!ci.set.isEmpty()) cf << QStringLiteral("set:") + ci.set;
                    cf << QStringLiteral("mode:") + ci.mode << QStringLiteral("part:") + ci.part;
                    // Name layer: override the display with the real Operator — Skin when identified,
                    // and add squad/operator/identified facets (fail closed to the codename otherwise).
                    const Bf6Names::Entry ne = m_names.resolve(ci.charId, ci.set);
                    const QString named = ne.named ? QStringLiteral("Identified") : QStringLiteral("Unidentified");
                    cf << QStringLiteral("named:") + named;
                    if (!ne.squad.isEmpty()) cf << QStringLiteral("squad:") + ne.squad;
                    if (!ne.op.isEmpty())    cf << QStringLiteral("operator:") + ne.op;
                    m_charFacets.insert(i, cf);
                    const QString real = m_names.displayFor(ci.charId, ci.set, ci.part);
                    m_charName.insert(i, !real.isEmpty() ? real
                                         : ci.set.isEmpty() ? ci.display
                                         : QStringLiteral("%1 · %2 · %3").arg(ci.display, ci.set, ci.part));
                    ++cChar[ci.display]; if (!ci.set.isEmpty()) ++cSet[ci.set]; ++cMode[ci.mode]; ++cPart[ci.part];
                    ++cNamed[named]; if (!ne.squad.isEmpty()) ++cSquad[ne.squad]; if (!ne.op.isEmpty()) ++cOp[ne.op];
                    // One assembled "full character" row per char+set — the whole outfit merged on the rig.
                    if (!ci.set.isEmpty()) {
                        const QString fkey = ci.charId + QLatin1Char('_') + ci.set;   // cha0040tank_set_005
                        if (!fullKeyIndex.contains(fkey)) {
                            const int fidx = m_fullKeys.size();
                            fullKeyIndex.insert(fkey, fidx);
                            m_fullKeys << fkey;
                            QStringList ff{ QStringLiteral("char:") + ci.display, QStringLiteral("set:") + ci.set,
                                            QStringLiteral("mode:") + ci.mode, QStringLiteral("part:full"),
                                            QStringLiteral("named:") + named };
                            if (!ne.squad.isEmpty()) ff << QStringLiteral("squad:") + ne.squad;
                            if (!ne.op.isEmpty())    ff << QStringLiteral("operator:") + ne.op;
                            m_fullFacets.insert(fidx, ff);
                            const QString rfull = m_names.displayFor(ci.charId, ci.set, QStringLiteral("full"));
                            m_fullName.insert(fidx, !rfull.isEmpty() ? rfull
                                                 : QStringLiteral("%1 · %2 · full").arg(ci.display, ci.set));
                            ++cPart[QStringLiteral("full")];   // a "full" entry in the Part funnel selects the assemblies
                        }
                    }
                }
                else {
                    // §33 Weapons: a MeshSet under common/hardware/weapons/ (same ModelsTab mechanism as
                    // Characters, a different authored-path classifier). Not a character and not a weapon →
                    // it stays in the flat "model"/"file" kinds only.
                    const WepInfo wi = parseWeapon(QString::fromStdString(r.name));
                    if (wi.ok) {
                        m_weapon.push_back(i);
                        QStringList wf{ QStringLiteral("class:") + wi.cls, QStringLiteral("weapon:") + wi.weapon,
                                        QStringLiteral("skin:") + wi.skin, QStringLiteral("view:") + wi.view,
                                        QStringLiteral("part:") + wi.part };
                        if (wi.attachment) wf << QStringLiteral("attach:") + wi.attachType;
                        m_weaponFacets.insert(i, wf);
                        m_weaponName.insert(i, displayWeapon(wi));
                        ++wClass[wi.cls]; ++wWeapon[wi.weapon]; ++wSkin[wi.skin]; ++wView[wi.view]; ++wPart[wi.part];
                    }
                    else {
                        const VehInfo vi = parseVehicle(QString::fromStdString(r.name));
                        if (vi.ok) {
                            m_vehicle.push_back(i);
                            QStringList vf{ QStringLiteral("class:") + vi.cls, QStringLiteral("vehicle:") + vi.vehicle,
                                            QStringLiteral("skin:") + vi.skin, QStringLiteral("view:") + vi.view,
                                            QStringLiteral("part:") + vi.part };
                            if (!vi.sub.isEmpty()) vf << QStringLiteral("sub:") + vi.sub;
                            m_vehicleFacets.insert(i, vf);
                            m_vehicleName.insert(i, displayVehicle(vi));
                            ++vClass[vi.cls]; ++vVehicle[vi.vehicle]; ++vSkin[vi.skin]; ++vPart[vi.part]; ++vView[vi.view];
                        }
                    }
                }
            }
            else if (r.typeName == "Texture" || r.typeName == "DxTexture") { m_texture.push_back(i); ++tType[typeLbl]; ++tArea[areaLbl]; }
        }
        else if (r.kind == bf6::ResKind::Chunk) {
            // GUID -> largest chunk with it (duplicates differ in size; the full vertex/pixel chunk wins).
            const auto it = m_chunkByGuid.find(r.name);
            if (it == m_chunkByGuid.end()) m_chunkByGuid.emplace(r.name, i);
            else if (r.size > res[size_t(it->second)].size) it->second = i;
        }
    }
    m_tagGroups[QStringLiteral("model")]   = { makeGroup(QStringLiteral("area"), QStringLiteral("Area"), mArea) };
    m_tagGroups[QStringLiteral("texture")] = { makeGroup(QStringLiteral("area"), QStringLiteral("Area"), tArea) };
    m_tagGroups[QStringLiteral("file")]    = { makeGroup(QStringLiteral("type"), QStringLiteral("Type"), fType),
                                               makeGroup(QStringLiteral("area"), QStringLiteral("Area"), fArea) };
    // Characters funnel: the real-name filters (Squad / Operator / Identified) lead, then the
    // codename-level facets. Squad/Operator appear only once something is identified.
    QVector<core::TagGroup> charGroups;
    if (!cSquad.isEmpty()) charGroups << makeGroup(QStringLiteral("squad"),    QStringLiteral("Squad"),    cSquad);
    if (!cOp.isEmpty())    charGroups << makeGroup(QStringLiteral("operator"), QStringLiteral("Operator"), cOp);
    charGroups << makeGroup(QStringLiteral("named"), QStringLiteral("Identified"), cNamed)
               << makeGroup(QStringLiteral("char"), QStringLiteral("Codename"), cChar)
               << makeGroup(QStringLiteral("set"),  QStringLiteral("Set"),  cSet)
               << makeGroup(QStringLiteral("mode"), QStringLiteral("Mode"), cMode)
               << makeGroup(QStringLiteral("part"), QStringLiteral("Part"), cPart);
    m_tagGroups[QStringLiteral("character")] = charGroups;
    // Weapons funnel: Class (assault rifle / SMG / shotgun / attachment …) → Weapon (the real
    // designation) → Skin → View (1p/3p) → Part. The weapon id is the recognizable name, so no name layer.
    m_tagGroups[QStringLiteral("weapon")] = {
        makeGroup(QStringLiteral("class"),  QStringLiteral("Class"),  wClass),
        makeGroup(QStringLiteral("weapon"), QStringLiteral("Weapon"), wWeapon),
        makeGroup(QStringLiteral("skin"),   QStringLiteral("Skin"),   wSkin),
        makeGroup(QStringLiteral("view"),   QStringLiteral("View"),   wView),
        makeGroup(QStringLiteral("part"),   QStringLiteral("Part"),   wPart),
    };
    // Vehicles funnel: Class (tank / car / helicopter / … / attachment / projectile) → Vehicle (the real
    // designation) → Skin → Part → View. Same ModelsTab mechanism as Characters/Weapons, no name layer.
    m_tagGroups[QStringLiteral("vehicle")] = {
        makeGroup(QStringLiteral("class"),   QStringLiteral("Class"),   vClass),
        makeGroup(QStringLiteral("vehicle"), QStringLiteral("Vehicle"), vVehicle),
        makeGroup(QStringLiteral("skin"),    QStringLiteral("Skin"),    vSkin),
        makeGroup(QStringLiteral("part"),    QStringLiteral("Part"),    vPart),
        makeGroup(QStringLiteral("view"),    QStringLiteral("View"),    vView),
    };

    // Fingerprint: the superbundle toc's size + mtime (changes on a game patch) + a render revision.
    // The thumbnail disk cache is keyed on this; BUMP the "rN" tag whenever loadModel / resolveMaterials
    // change what a model LOOKS like (e.g. per-region materials), so stale thumbnails are discarded and
    // re-rendered instead of served from disk. r2 = per-region materials + shadow-section skip;
    // r3 = full PBR (_cm roughness/metalness → ORM) + normal Z reconstruction;
    // r4 = opaque normal (drop _nx alpha) + metalness from _cm.B (was G);
    // r5 = roughness = 255-_cm.R (R read as gloss); r6 = roughness = _cm.R direct + ior 1.0;
    // r7 = roughness = 255-_cm.R again + ior 1.45 + opaque base colour; r8 = roughness = _cm.R direct
    // (user-directed); r9 = back to 255-_cm.R; r10 = roughness = 255 - _cs.ALPHA — a reference .blend
    // (CS|Color Smooth) proved SMOOTHNESS is the base map's alpha, not a _cm channel; r11 = metalness
    // from _nx.ALPHA too (reference + our data: sparse, on hardware, 0 on skin), dropping the _cm.B
    // metalness that wrongly metallised cloth. _cm is now unused in the ORM (it is a colour/tint mask).
    // r12 = emissive: the "glow" REGION's _cs is the emission colour → glTF emissive (was read as base).
    // r13 = vertex normals NEGATED — the authored normals pointed inward vs our winding (shading read
    // inverted; in Blender it needed a manual Alt+N Reset Vectors), now aligned with the faces.
    const QFileInfo fi(charToc);
    m_fingerprint = QStringLiteral("bf6-r13-%1-%2").arg(fi.size()).arg(fi.lastModified().toSecsSinceEpoch());

    if (progress) progress(QStringLiteral("Indexed %1 resources (%2 meshes, %3 textures, %4 character + %5 weapon + %6 vehicle meshes)")
                               .arg(res.size()).arg(m_model.size()).arg(m_texture.size())
                               .arg(m_character.size()).arg(m_weapon.size()).arg(m_vehicle.size()));
    m_open = true;
    return true;
}

const bf6::Resource* Bf6Store::resAt(const core::AssetId& id) const
{
    if (id.scheme != kScheme) return nullptr;
    const auto& res = m_toc.resources();
    if (id.value >= res.size()) return nullptr;
    return &res[size_t(id.value)];
}

int Bf6Store::rowCount(const QString& kind) const
{
    if (kind == QLatin1String("model"))     return m_model.size();
    if (kind == QLatin1String("texture"))   return m_texture.size();
    if (kind == QLatin1String("file"))      return m_file.size();
    if (kind == QLatin1String("character")) return m_character.size() + m_fullKeys.size();  // parts + assembled rows
    if (kind == QLatin1String("weapon"))    return m_weapon.size();
    if (kind == QLatin1String("vehicle"))   return m_vehicle.size();
    return 0;
}

core::AssetRow Bf6Store::row(const QString& kind, int i) const
{
    // Assembled "full character" rows are appended after the character part rows.
    if (kind == QLatin1String("character") && i >= m_character.size()) {
        const int fidx = i - m_character.size();
        core::AssetRow out;
        if (fidx < 0 || fidx >= m_fullKeys.size()) return out;
        out.id   = { kScheme, kFullBit | uint64_t(fidx) };
        out.name = m_fullKeys[fidx] + QStringLiteral("_full");
        out.kind = kind;
        out.inGameName = m_fullName.value(fidx);
        out.tags = m_fullFacets.value(fidx);
        out.flags = core::AssetRow::Rigged;
        return out;
    }
    const QVector<int>* list = (kind == QLatin1String("model"))     ? &m_model
                             : (kind == QLatin1String("texture"))   ? &m_texture
                             : (kind == QLatin1String("file"))      ? &m_file
                             : (kind == QLatin1String("character")) ? &m_character
                             : (kind == QLatin1String("weapon"))    ? &m_weapon
                             : (kind == QLatin1String("vehicle"))   ? &m_vehicle : nullptr;
    core::AssetRow out;
    if (!list || i < 0 || i >= list->size()) return out;
    const int idx = list->at(i);
    const bf6::Resource& r = m_toc.resources()[size_t(idx)];
    out.id   = { kScheme, uint64_t(idx) };
    out.name = QString::fromStdString(r.name);
    out.kind = kind;
    out.size = uint64_t(r.size);
    out.tags = facetsFor(r);
    if (kind == QLatin1String("character")) {
        out.inGameName = m_charName.value(idx);
        out.tags += m_charFacets.value(idx);
    }
    else if (kind == QLatin1String("weapon")) {
        out.inGameName = m_weaponName.value(idx);
        out.tags += m_weaponFacets.value(idx);
    }
    else if (kind == QLatin1String("vehicle")) {
        out.inGameName = m_vehicleName.value(idx);
        out.tags += m_vehicleFacets.value(idx);
    }
    return out;
}

QString Bf6Store::searchBlob(const core::AssetId& id) const
{
    if (id.value & kFullBit) {
        const int fidx = int(id.value & ~kFullBit);
        return (fidx >= 0 && fidx < m_fullKeys.size())
                   ? m_fullKeys[fidx] + QStringLiteral(" full character ") + m_fullName.value(fidx) : QString();
    }
    const bf6::Resource* r = resAt(id);
    if (!r) return {};
    return QStringLiteral("%1 %2 %3").arg(QString::fromStdString(r->name),
                                          QString::fromStdString(r->typeName),
                                          QString::fromStdString(r->superbundle));
}

QVector<core::TagGroup> Bf6Store::tagGroups(const QString& kind) const { return m_tagGroups.value(kind); }

bool Bf6Store::hasTag(const core::AssetId& id, const QString& tag) const
{
    if (id.value & kFullBit) return m_fullFacets.value(int(id.value & ~kFullBit)).contains(tag);
    const bf6::Resource* r = resAt(id);
    if (!r) return false;
    return facetsFor(*r).contains(tag) || m_charFacets.value(int(id.value)).contains(tag)
        || m_weaponFacets.value(int(id.value)).contains(tag)
        || m_vehicleFacets.value(int(id.value)).contains(tag);
}

QByteArray Bf6Store::read(const core::AssetId& id, const QString& /*part*/, QString* error)
{
    const bf6::Resource* r = resAt(id);
    if (!r) { if (error) *error = QStringLiteral("unknown BF6 resource id"); return {}; }
    std::vector<uint8_t> out; std::string err;
    if (!m_toc.readResource(*r, out, &err)) { if (error) *error = QString::fromStdString(err); return {}; }
    return QByteArray(reinterpret_cast<const char*>(out.data()), int(out.size()));
}

// Bf6Store::loadModel is implemented after the DDS/texture helpers below (it shares the chunk lookup).

// Bf6Store::loadSkeleton is implemented after loadModel (it shares the Rd reader).

QVector<core::Material> Bf6Store::resolveMaterials(const core::AssetId& id, const core::Geometry& geo, bool decodeImages)
{
    // One material per cooked region (geo.materialNames, set by loadModel), each resolved to the set's
    // region textures by the AUTHORED name: …_<region>_cs (base, sRGB), _nx (normal, R,G + rebuilt Z),
    // and _cm (packed roughness/metalness → glTF ORM). A character set has a texture per region
    // (upperbody/lowerbody/hands/headgear/…), so a single shared material left every part but one
    // untextured — this maps each part to its own region textures, with full PBR.
    QString key;   // cha0024jinx_set_011
    if (id.value & kFullBit) {
        // Assembled full character: all parts share this char+set key, so the per-region loop below
        // resolves each merged region (upperbody/headgear/glow/…) exactly as the single-part path does.
        const int fidx = int(id.value & ~kFullBit);
        if (fidx >= 0 && fidx < m_fullKeys.size()) key = m_fullKeys[fidx];
    } else if (const bf6::Resource* ms = resAt(id)) {
        const CharInfo ci = parseChar(QString::fromStdString(ms->name));
        if (ci.ok && !ci.charId.isEmpty() && !ci.set.isEmpty()) key = ci.charId + QLatin1Char('_') + ci.set;
    }
    // Skin regions with no texture of their own share one (arms are part of the upper-body skin set).
    auto texRegionFor = [](const QString& r) -> QString {
        if (r == QLatin1String("arms")) return QStringLiteral("upperbody");
        return r;
    };
    // The set texture named …_<region><suffix> (e.g. "_upperbody_cs"); first match wins.
    auto findTex = [&](const QString& region, const QStringList& suffixes) -> core::AssetId {
        if (key.isEmpty() || region.isEmpty()) return {};
        for (const QString& suf : suffixes) {
            const QString tail = QLatin1Char('_') + region + suf;
            for (int ti : m_texture) {
                const QString tn = QString::fromStdString(m_toc.resources()[size_t(ti)].name);
                if (tn.contains(key) && tn.endsWith(tail)) return core::AssetId{ kScheme, uint64_t(ti) };
            }
        }
        return {};
    };

    const QStringList regions = geo.materialNames.isEmpty() ? QStringList{ QStringLiteral("bf6") } : geo.materialNames;
    QVector<core::Material> out; out.reserve(regions.size());
    for (const QString& region : regions) {
        core::Material mat;
        mat.name = region;
        mat.doubleSided = true;
        mat.ior = 1.45f;  // Physical dielectric — the reference .blend's BF6 shader authors IOR 1.45 too.
                          // An earlier cut forced 1.0 to tame excess gloss, but that gloss was the roughness
                          // bug (wrong texture; see the ORM below), not the IOR. KHR_materials_ior.
        if (decodeImages) {
            const QString tr = texRegionFor(region);
            QImage csRgba;   // _cs decoded RGBA: RGB = base colour, ALPHA = authored SMOOTHNESS (the BF6
                             // "CS | Color Smooth" map); kept so the ORM below can read the alpha for roughness.
            if (const core::AssetId c = findTex(tr, { QStringLiteral("_cs") }); c.isValid()) {
                csRgba = loadTexture(c, nullptr, nullptr).convertToFormat(QImage::Format_RGBA8888);
                // Base colour = _cs RGB, exported OPAQUE. The _cs ALPHA is NOT transparency — it is the
                // authored smoothness (confirmed against a reference .blend's "CS | Color Smooth" input), so
                // it drives roughness below; we only drop it from the base image. The raw _cs with its alpha
                // still goes out through the loose-texture export for anyone solving by hand.
                mat.baseColor.image = csRgba.convertToFormat(QImage::Format_RGB888);
                mat.baseColor.srgb = true; mat.baseColor.sourcePath = searchBlob(c).section(QLatin1Char(' '), 0, 0);
            }
            // Emissive (r12): BF6 authors glowing parts as a dedicated "glow" REGION with its own
            // _cs/_nx/_cm/_id (e.g. Aegis VII = cha0040tank set_005 — its glow _cs is a near-uniform bright
            // cyan). The glow _cs IS the emission colour, so route it to the glTF emissive channel; without
            // this the glow mesh read as a merely bright base colour and never lit up. Only this region
            // emits; everything else leaves emissive unset. Strength 1.0 / factor {1,1,1} (tunable).
            if (region.compare(QLatin1String("glow"), Qt::CaseInsensitive) == 0 && !csRgba.isNull()) {
                mat.emissive.image = csRgba.convertToFormat(QImage::Format_RGB888);
                mat.emissive.srgb = true;
            }
            QImage nxRgba;   // _nx decoded RGBA: R,G = tangent normal; ALPHA = METALNESS (sparse, on metal
                             // hardware); kept so the ORM below can read the alpha. (B is an AO/cavity term.)
            if (const core::AssetId n = findTex(tr, { QStringLiteral("_nx"), QStringLiteral("_na") }); n.isValid()) {
                // BF6 "_nx" (BC7): tangent normal in R,G; reconstruct Z from them (greenDown → DirectX,
                // flipped to glTF/OpenGL by the exporter). The normal is exported OPAQUE (RGB888) — keeping
                // _nx's alpha on the PNG made importers read it transparent/flat. That alpha is NOT normal
                // data: it is the authored METALNESS (sparse, on hardware; used in the ORM below).
                nxRgba = loadTexture(n, nullptr, nullptr).convertToFormat(QImage::Format_RGBA8888);
                mat.normal.image = nxRgba.convertToFormat(QImage::Format_RGB888);
                mat.normal.srgb = false; mat.normalGreenDown = true; mat.normalNeedsZ = true;
            }
            // ORM (occlusion / roughness / metalness), r11. ROUGHNESS = 255 - _cs.alpha (BF6 authors
            // SMOOTHNESS in the base map's ALPHA — the reference .blend input "CS | Color Smooth"; this
            // ended the r5-r9 saga, which read the wrong texture). METALNESS = _nx.alpha: the reference
            // wires metalness to the normal map's alpha, and our data confirms it — _nx.alpha is sparse,
            // localized on metal hardware (buckles/clips) and ZERO on skin. _cm is NOT ORM: it is the
            // per-zone COLOUR MASK (reference "CM | Color Mask"), a broad 25-40% tint zone that does not
            // match the metal; reading its B as metalness (through r10) wrongly metallised cloth, so that
            // is dropped. _cm is otherwise unused here (reserved for future per-zone tint). Occlusion unset
            // (none authored; the viewport does SSAO). Fail closed: a degenerate (constant) _cs alpha falls
            // back to 255 - _cm.R so a missing smoothness map never makes the surface a mirror.
            QImage cmImg;
            if (const core::AssetId cm = findTex(tr, { QStringLiteral("_cm") }); cm.isValid())
                cmImg = loadTexture(cm, nullptr, nullptr).convertToFormat(QImage::Format_RGBA8888);
            bool csAlphaUsable = false;
            if (!csRgba.isNull()) {
                int mn = 255, mx = 0;
                for (int y = 0; y < csRgba.height(); y += 8) {
                    const uchar* s = csRgba.constScanLine(y);
                    for (int x = 0; x < csRgba.width(); x += 8) { const int a = s[x*4+3]; mn = qMin(mn, a); mx = qMax(mx, a); }
                }
                csAlphaUsable = (mx - mn) > 4;   // a varying alpha = a real smoothness map
            }
            const QImage sizer = !csRgba.isNull() ? csRgba : (!nxRgba.isNull() ? nxRgba : cmImg);   // ORM size
            if (!sizer.isNull()) {
                const QSize sz = sizer.size();
                auto fit = [&](const QImage& im) { return (!im.isNull() && im.size() != sz) ? im.scaled(sz, Qt::IgnoreAspectRatio, Qt::SmoothTransformation) : im; };
                const QImage csS = fit(csRgba);
                const QImage nxS = fit(nxRgba);
                const QImage cmS = fit(cmImg);
                QImage orm(sz, QImage::Format_RGBA8888);
                for (int y = 0; y < sz.height(); ++y) {
                    const uchar* cs = csS.isNull() ? nullptr : csS.constScanLine(y);
                    const uchar* nx = nxS.isNull() ? nullptr : nxS.constScanLine(y);
                    const uchar* cm = cmS.isNull() ? nullptr : cmS.constScanLine(y);
                    uchar* d = orm.scanLine(y);
                    for (int x = 0; x < sz.width(); ++x) {
                        const int rough = (csAlphaUsable && cs) ? 255 - cs[x*4+3]   // 1 - smoothness(_cs.alpha)
                                        : cm                    ? 255 - cm[x*4+0]   // fallback: 1 - _cm.R
                                                                : 230;             // matte default
                        d[x*4+0] = 255;                           // occlusion — none authored
                        d[x*4+1] = uchar(rough);                  // roughness
                        d[x*4+2] = nx ? nx[x*4+3] : 0;            // metalness <- _nx.alpha (sparse; 0 on skin/cloth)
                        d[x*4+3] = 255;
                    }
                }
                mat.orm.image = orm; mat.orm.srgb = false; mat.ormHasOcclusion = false;
                mat.metalFactor = 1.0f; mat.roughFactor = 1.0f;   // else glTF multiplies the map by the 0 default
            }
        }
        out << mat;
    }
    return out;
}

// Bf6Store::loadTexture is implemented after the DDS helpers below (it shares the format decode).

QStringList Bf6Store::dependencyFiles(const core::AssetId&) { return {}; }

// ── cooked export support ────────────────────────────────────────────────────────────────────────
static bool writeBytes(const QString& path, const std::vector<uint8_t>& d)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    if (!d.empty()) f.write(reinterpret_cast<const char*>(d.data()), qint64(d.size()));
    return f.error() == QFile::NoError;
}

bool Bf6Store::isMeshId(const core::AssetId& id) const
{
    if (id.value & kFullBit) return true;   // the assembled full-character pseudo-mesh
    const bf6::Resource* r = resAt(id);
    return r && r->kind == bf6::ResKind::Res && r->typeName == "MeshSet";
}

bool Bf6Store::assembleCharacter(const QString& key, core::Geometry& geo, QString* error)
{
    // Every cosmetic part of this char+set, merged (the same selection the full-character export uses):
    // real `_mesh` parts only, no 1st-person arms. All share the 291-bone soldier rig, so joint indices
    // line up with no remap — we just offset vertices / indices / material slots and concatenate.
    const QStringList all = meshNamesMatching(key, 1000000);
    QStringList meshes;
    for (const QString& n : all) {
        if (!n.endsWith(QLatin1String("_mesh"))) continue;
        if (n.contains(QLatin1String("_1p_mesh")) || n.contains(QLatin1String("_1parms_mesh"))) continue;
        if (!meshes.contains(n)) meshes << n;
    }
    for (const QString& n : meshes) {
        const core::AssetId mid = findMesh(n);
        if (!mid.isValid()) continue;
        core::Geometry pg;
        if (!loadModel(mid, pg, nullptr) || pg.vertices.isEmpty()) continue;
        const int vBase = geo.vertices.size(), iBase = geo.indices.size(), mBase = geo.materialNames.size();
        geo.vertices += pg.vertices;
        for (quint32 ix : pg.indices) geo.indices << (quint32(vBase) + ix);
        for (core::Part p : pg.parts) { p.indexStart += quint32(iBase); if (p.materialIndex >= 0) p.materialIndex += mBase; geo.parts << p; }
        geo.materialNames += pg.materialNames;
        geo.hasUv1 = geo.hasUv1 || pg.hasUv1; geo.hasColor = geo.hasColor || pg.hasColor;
    }
    if (geo.vertices.isEmpty()) { if (error) *error = QStringLiteral("no cosmetic meshes for %1").arg(key); return false; }
    geo.skinned = true;
    geo.sourcePath = key + QStringLiteral("_full");
    geo.formatNote = QStringLiteral("bf6 assembled character · %1 part(s)").arg(geo.parts.size());
    geo.computeBounds();
    return true;
}

core::AssetId Bf6Store::findMesh(const QString& nameSubstr) const
{
    const std::string s = nameSubstr.toStdString();
    const auto& res = m_toc.resources();
    for (size_t i = 0; i < res.size(); ++i)
        if (res[i].kind == bf6::ResKind::Res && res[i].typeName == "MeshSet" && res[i].name.find(s) != std::string::npos)
            return { kScheme, uint64_t(i) };
    return {};
}

QStringList Bf6Store::meshNamesMatching(const QString& nameSubstr, int max) const
{
    QStringList out;
    const std::string s = nameSubstr.toStdString();
    for (int idx : m_model) {
        const bf6::Resource& r = m_toc.resources()[size_t(idx)];
        if (r.name.find(s) != std::string::npos) { out << QString::fromStdString(r.name); if (out.size() >= max) break; }
    }
    return out;
}

bool Bf6Store::extractMeshDump(const core::AssetId& meshId, const QString& tempDir, QString& meshFile, QString* error)
{
    const bf6::Resource* ms = resAt(meshId);
    if (!ms || ms->kind != bf6::ResKind::Res || ms->typeName != "MeshSet") { if (error) *error = QStringLiteral("not a MeshSet"); return false; }
    std::vector<uint8_t> mb; std::string e;
    if (!m_toc.readResource(*ms, mb, &e)) { if (error) *error = QString::fromStdString(e); return false; }
    QDir().mkpath(tempDir + QStringLiteral("/bundles"));
    QDir().mkpath(tempDir + QStringLiteral("/chunks"));
    const QString base = QString::fromStdString(ms->name).section(QLatin1Char('/'), -1);
    meshFile = QDir(tempDir).filePath(QStringLiteral("bundles/") + base + QStringLiteral(".MeshSet"));
    if (!writeBytes(meshFile, mb)) { if (error) *error = QStringLiteral("could not write MeshSet"); return false; }

    // The MeshSet embeds its vertex/index chunk's GUID (per Fb_bf6_mesh): num2 = 16 + le32(@48); the
    // 16-byte GUID is at num2+116. Extract that global chunk alongside.
    if (mb.size() < 52) { if (error) *error = QStringLiteral("MeshSet too small"); return false; }
    auto le32 = [&](size_t o) { return uint32_t(mb[o]) | (uint32_t(mb[o+1])<<8) | (uint32_t(mb[o+2])<<16) | (uint32_t(mb[o+3])<<24); };
    const size_t num2 = 16 + le32(48);
    const size_t goff = num2 + 116;
    if (goff + 16 > mb.size()) { if (error) *error = QStringLiteral("chunk GUID offset out of range"); return false; }
    QString guid;
    for (int k = 0; k < 16; ++k) guid += QStringLiteral("%1").arg(mb[goff + size_t(k)], 2, 16, QLatin1Char('0')).toUpper();

    const bf6::Resource* ch = nullptr;
    for (const auto& r : m_toc.resources())
        if (r.kind == bf6::ResKind::Chunk && QString::fromStdString(r.name) == guid) { ch = &r; break; }
    if (!ch) { if (error) *error = QStringLiteral("referenced chunk %1 not found in this superbundle").arg(guid); return false; }
    std::vector<uint8_t> cb;
    if (!m_toc.readResource(*ch, cb, &e)) { if (error) *error = QString::fromStdString(e); return false; }
    if (!writeBytes(QDir(tempDir).filePath(QStringLiteral("chunks/") + guid + QStringLiteral(".chunk")), cb)) {
        if (error) *error = QStringLiteral("could not write chunk"); return false; }
    return true;
}

bool Bf6Store::extractEbx(const QString& nameSubstr, const QString& outFile, QString* error)
{
    const std::string sub = nameSubstr.toStdString();
    const bf6::Resource* sk = nullptr;
    for (const auto& r : m_toc.resources())
        if (r.kind == bf6::ResKind::Ebx && r.name.find(sub) != std::string::npos) { sk = &r; break; }
    if (!sk) { if (error) *error = QStringLiteral("no EBX matching %1").arg(nameSubstr); return false; }
    std::vector<uint8_t> b; std::string e;
    if (!m_toc.readResource(*sk, b, &e)) { if (error) *error = QString::fromStdString(e); return false; }
    if (!writeBytes(outFile, b)) { if (error) *error = QStringLiteral("could not write %1").arg(outFile); return false; }
    return true;
}

int Bf6Store::dumpResourcesMatching(const QString& nameSubstr, const QString& outDir, int max)
{
    QDir().mkpath(outDir);
    QString manifest = QStringLiteral("# kind\ttype\tnameHash\tsize\tname  (resources matching \"%1\")\n").arg(nameSubstr);
    int n = 0;
    for (const bf6::Resource& r : m_toc.resources()) {
        const QString nm = QString::fromStdString(r.name);
        if (!nm.contains(nameSubstr, Qt::CaseInsensitive)) continue;
        const char* kind = r.kind == bf6::ResKind::Ebx ? "ebx" : r.kind == bf6::ResKind::Res ? "res" : "chunk";
        manifest += QStringLiteral("%1\t%2\t%3\t%4\t%5\n")
                        .arg(QLatin1String(kind))
                        .arg(r.typeName.empty() ? QStringLiteral("-") : QString::fromStdString(r.typeName))
                        .arg(QString::number(r.nameHash, 16))
                        .arg(r.size)
                        .arg(nm);
        // Dump decompressed bytes for EBX + small RES (skip raw chunks and huge payloads).
        if (r.kind != bf6::ResKind::Chunk && r.size < 4 * 1024 * 1024) {
            std::vector<uint8_t> b; std::string e;
            if (m_toc.readResource(r, b, &e)) {
                QString base = nm; base.replace(QLatin1Char('/'), QLatin1Char('@'));
                writeBytes(QDir(outDir).filePath(base + QLatin1Char('.') + QLatin1String(kind)), b);
            }
        }
        if (++n >= max) break;
    }
    QFile mf(QDir(outDir).filePath(QStringLiteral("_manifest.txt")));
    if (mf.open(QIODevice::WriteOnly)) mf.write(manifest.toUtf8());
    return n;
}

int Bf6Store::dumpEbxGuidIndex(const QString& outFile)
{
    QFile f(outFile);
    if (!f.open(QIODevice::WriteOnly)) return 0;
    static const char* const kHex = "0123456789abcdef";
    QByteArray line;
    int n = 0;
    for (const bf6::Resource& r : m_toc.resources()) {
        if (r.kind != bf6::ResKind::Ebx) continue;
        std::vector<uint8_t> b; std::string e;
        if (!m_toc.readResource(r, b, &e)) continue;
        // Find the EBXD chunk, FileGUID is its data+12.
        QString guid;
        if (b.size() > 12 && std::memcmp(b.data(), "RIFF", 4) == 0) {
            size_t off = 12;
            while (off + 8 <= b.size()) {
                uint32_t csz; std::memcpy(&csz, &b[off + 4], 4);
                if (std::memcmp(&b[off], "EBXD", 4) == 0) {
                    const size_t g = off + 8 + 12;
                    if (g + 16 <= b.size()) for (int k = 0; k < 16; ++k) { const uint8_t x = b[g + k]; guid += QLatin1Char(kHex[x >> 4]); guid += QLatin1Char(kHex[x & 0xF]); }
                    break;
                }
                off += 8 + csz + (csz & 1);
            }
        }
        line = guid.toLatin1(); line += '\t'; line += QByteArray::fromStdString(r.name); line += '\n';
        f.write(line);
        ++n;
    }
    return n;
}

// ── native texture -> DDS (no external tool) ───────────────────────────────────────────────────────
namespace {
// DX10 DDS header templates, verbatim from the decompiled Batch_texture_Converter (button3_Click).
const uint8_t kDdsT0[12]  = {68,68,83,32,124,0,0,0,7,16,0,0};
const uint8_t kDdsT1[64]  = {0,0,2,0,0,0,2,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,0,0,4,0,0,0};
const uint8_t kDdsDX10[4] = {68,88,49,48};
const uint8_t kDdsT2[40]  = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
const uint8_t kDdsT3[16]  = {3,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0};
int32_t dxgiFromFb(int32_t f) {
    switch (f) {
    case 6: return 61; case 18: return 28; case 29: return 28; case 40: return 10;
    case 54: return 71; case 55: return 72; case 56: return 74; case 57: return 75;
    case 58: return 75; case 59: return 77; case 60: return 78; case 61: return 78;
    case 62: return 80; case 63: return 83; case 64: return 96; case 65: return 96;
    case 66: return 98; case 67: return 99; default: return 71;
    }
}
void appendU32(std::vector<uint8_t>& v, uint32_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x>>8)); v.push_back(uint8_t(x>>16)); v.push_back(uint8_t(x>>24)); }
}  // namespace

bool Bf6Store::isTextureId(const core::AssetId& id) const
{
    const bf6::Resource* r = resAt(id);
    return r && r->kind == bf6::ResKind::Res && (r->typeName == "Texture" || r->typeName == "DxTexture");
}

core::AssetId Bf6Store::findTexture(const QString& nameSubstr) const
{
    const std::string s = nameSubstr.toStdString();
    const auto& res = m_toc.resources();
    for (size_t i = 0; i < res.size(); ++i)
        if (res[i].kind == bf6::ResKind::Res && (res[i].typeName == "Texture" || res[i].typeName == "DxTexture")
            && res[i].name.find(s) != std::string::npos)
            return { kScheme, uint64_t(i) };
    return {};
}

int Bf6Store::hdMip0Chunk(const std::vector<uint8_t>& tb) const
{
    if (m_hdChunkByGuid.empty()) return -1;
    // The DxTexture header carries the HD chunk's GUID as raw bytes (its offset shifts with the group-name
    // string length), so scan every 16-byte window for one that names an HD-pack chunk. The primary
    // (resident-chain) GUID is only in characters.toc, never in m_hdChunkByGuid, so any hit is the HD mip0.
    static const char H[] = "0123456789ABCDEF";
    for (size_t o = 0; o + 16 <= tb.size(); ++o) {
        std::string g; g.reserve(32);
        for (int k = 0; k < 16; ++k) { g += H[tb[o + size_t(k)] >> 4]; g += H[tb[o + size_t(k)] & 0xF]; }
        const auto it = m_hdChunkByGuid.find(g);
        if (it != m_hdChunkByGuid.end()) return it->second;
    }
    return -1;
}

bool Bf6Store::exportTextureDds(const core::AssetId& texId, const QString& outDir, QString* outPath, QString* error)
{
    const bf6::Resource* tex = resAt(texId);
    if (!tex || !isTextureId(texId)) { if (error) *error = QStringLiteral("not a texture"); return false; }
    std::vector<uint8_t> tb; std::string e;
    if (!m_toc.readResource(*tex, tb, &e)) { if (error) *error = QString::fromStdString(e); return false; }
    if (tb.size() < 56) { if (error) *error = QStringLiteral("texture header too small"); return false; }

    const int32_t fmt = int32_t(uint32_t(tb[12]) | uint32_t(tb[13])<<8 | uint32_t(tb[14])<<16 | uint32_t(tb[15])<<24);
    const int mipflag = tb[21];
    const uint32_t declW = uint32_t(tb[22]) | uint32_t(tb[23])<<8;   // FULL declared size incl. the 4K top mip
    const uint32_t declH = uint32_t(tb[24]) | uint32_t(tb[25])<<8;
    uint32_t w = declW, h = declH;
    // When mipflag>0 the 4K top mip ("HD mip0") is STREAMED in the HRES pack, not in characters.toc, so
    // the resident chunk starts one mip DOWN (2048). We halve to match THAT chunk; if the HD pack is
    // installed, hdMip0Chunk() below recovers the real 4K top mip. (Header map, from BF6_DUMP_TEX: per-mip
    // size table at off 56 (mip0 = 0x01000000 for 4096²), group name at off 112, HD chunk's GUID further on.)
    if (mipflag > 0) { w /= 2; h /= 2; }
    QString guid;
    for (int k = 0; k < 16; ++k) guid += QStringLiteral("%1").arg(tb[40 + k], 2, 16, QLatin1Char('0')).toUpper();

    // Env-gated texture-header dump (permanent diagnostic): header hex + the chunks the primary GUID and
    // any secondary header GUID resolve to. The entry point for recovering the streamed 4K HD mip.
    if (qEnvironmentVariableIsSet("BF6_DUMP_TEX")) {
        QString hx; for (int k = 0; k < qMin<int>(208, int(tb.size())); ++k) hx += QStringLiteral("%1 ").arg(tb[k], 2, 16, QLatin1Char('0'));
        qInfo("TEXDUMP %s | res=%zu fmt=0x%X mipflag=%d w=%u h=%u(post/2) guid=%s", tex->name.c_str(),
              tb.size(), unsigned(fmt), mipflag, w, h, qUtf8Printable(guid));
        qInfo("TEXDUMP hdr0..96: %s", qUtf8Printable(hx));
        std::unordered_set<std::string> chunkNames;
        for (const auto& r : m_toc.resources()) if (r.kind == bf6::ResKind::Chunk) chunkNames.insert(r.name);
        const auto listChunks = [&](const QString& g, const char* tag, size_t off) {
            for (const auto& r : m_toc.resources())
                if (r.kind == bf6::ResKind::Chunk && QString::fromStdString(r.name) == g)
                    qInfo("TEXDUMP  %s @%zu guid=%s bundle=%d casNo=%d size=%lld sb=%s", tag, off,
                          qUtf8Printable(g), r.bundleIndex, r.casNo, (long long)r.size, r.superbundle.c_str());
        };
        listChunks(guid, "PRIMARY", 40);
        const size_t scan = qMin<size_t>(tb.size(), 4096);
        for (size_t o = 0; o + 16 <= scan; ++o) {
            std::string g; g.reserve(32);
            static const char* hd = "0123456789ABCDEF";
            for (int k = 0; k < 16; ++k) { g += hd[tb[o+k]>>4]; g += hd[tb[o+k]&0xF]; }
            if (chunkNames.count(g)) {
                const QString gq = QString::fromStdString(g);
                if (gq != guid) listChunks(gq, "SECOND", o);
            }
        }
    }

    // Pixel source: the streamed 4K HD mip0 (HRES pack) when present — that gives true 4096² — else the
    // resident chain chunk (2048-down) from characters.toc. The HD chunk is exactly the top mip, so the
    // DDS is single-mip at the full declared size.
    std::vector<uint8_t> px;
    uint32_t outW = w, outH = h;
    const int hdIdx = hdMip0Chunk(tb);
    bool haveHd = false;
    if (hdIdx >= 0) {
        std::string he;
        if (m_hdToc.readResource(m_hdToc.resources()[size_t(hdIdx)], px, &he) && !px.empty()) {
            outW = declW; outH = declH; haveHd = true;
        } else px.clear();
    }
    if (!haveHd) {
        // The resident chain is the streamed GLOBAL chunk (bundleIndex < 0); the per-bundle entry is only
        // the resident tail. Prefer the global one.
        const bf6::Resource* ch = nullptr; const bf6::Resource* anyc = nullptr;
        for (const auto& r : m_toc.resources()) {
            if (r.kind != bf6::ResKind::Chunk || QString::fromStdString(r.name) != guid) continue;
            if (r.bundleIndex < 0) { ch = &r; break; }
            if (!anyc) anyc = &r;
        }
        if (!ch) ch = anyc;
        if (!ch) { if (error) *error = QStringLiteral("pixel chunk %1 not found").arg(guid); return false; }
        if (!m_toc.readResource(*ch, px, &e)) { if (error) *error = QString::fromStdString(e); return false; }
    }

    std::vector<uint8_t> dds;
    dds.insert(dds.end(), kDdsT0, kDdsT0 + 12);
    appendU32(dds, outH); appendU32(dds, outW);
    dds.insert(dds.end(), kDdsT1, kDdsT1 + 64);
    dds.insert(dds.end(), kDdsDX10, kDdsDX10 + 4);
    dds.insert(dds.end(), kDdsT2, kDdsT2 + 40);
    appendU32(dds, uint32_t(dxgiFromFb(fmt)));
    dds.insert(dds.end(), kDdsT3, kDdsT3 + 16);
    dds.insert(dds.end(), px.begin(), px.end());

    QDir().mkpath(outDir);
    const QString base = QString::fromStdString(tex->name).section(QLatin1Char('/'), -1);
    const QString path = QDir(outDir).filePath(base + QStringLiteral(".dds"));
    if (!writeBytes(path, dds)) { if (error) *error = QStringLiteral("could not write %1").arg(path); return false; }
    if (outPath) *outPath = path;
    return true;
}

int Bf6Store::dumpCharSourceTextures(const QString& charSetKey, const QString& outDir, QString* outManifest)
{
    if (outManifest) outManifest->clear();
    if (charSetKey.isEmpty()) return 0;
    QDir().mkpath(outDir);

    // Every texture whose name carries this char+set. Dedupe by basename (a map can appear in several
    // bundles — resident tail vs streamed global); loadTexture already prefers the full global chunk.
    struct Entry { QString base, tail; int ti; };
    QVector<Entry> entries;
    QStringList seen;
    const auto& res = m_toc.resources();
    for (int ti : m_texture) {
        const QString name = QString::fromStdString(res[size_t(ti)].name);
        if (!name.contains(charSetKey)) continue;
        const QString base = name.section(QLatin1Char('/'), -1);
        if (seen.contains(base)) continue;
        seen << base;
        const int kp = base.indexOf(charSetKey);
        entries.append({ base, kp >= 0 ? base.mid(kp + charSetKey.size()) : base, ti });   // tail e.g. "_upperbody_cs"
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b){ return a.base < b.base; });

    QString man;
    man += QStringLiteral("# Raw source textures for %1\n").arg(charSetKey);
    man += QStringLiteral("# Decoded straight from the game (BC blocks -> RGBA), untouched. Channel meaning:\n");
    man += QStringLiteral("#   _cs  base colour in RGB (sRGB); ALPHA = SMOOTHNESS (glTF roughness = 1 - alpha)\n");
    man += QStringLiteral("#   _nx  tangent normal: X in R, Y in G (green = DirectX, points DOWN); reconstruct\n");
    man += QStringLiteral("#        Z = sqrt(1 - R^2 - G^2). B = AO/cavity; ALPHA = METALNESS (sparse, on hardware)\n");
    man += QStringLiteral("#   _cm  per-zone COLOUR/TINT MASK (selects appearance tint slots), not ORM\n");
    man += QStringLiteral("#   _id  256^2 zone/material-slot id mask\n");
    man += QStringLiteral("#   a \"glow\" region (its own _cs/_nx/_cm/_id) is EMISSIVE: glow _cs = emission colour\n");
    man += QStringLiteral("# Any other suffix below is a map the cooked exporter does NOT currently interpret.\n\n");

    int n = 0;
    for (const Entry& e : entries) {
        QString err;
        const QImage img = loadTexture(core::AssetId{ kScheme, uint64_t(e.ti) }, nullptr, &err);
        if (img.isNull()) {
            man += QStringLiteral("%1  FAILED (%2)\n").arg(e.base, -44).arg(err.isEmpty() ? QStringLiteral("decode") : err);
            continue;
        }
        const QString path = QDir(outDir).filePath(e.base + QStringLiteral(".png"));
        if (!img.save(path, "PNG")) {
            man += QStringLiteral("%1  FAILED (could not write png)\n").arg(e.base, -44);
            continue;
        }
        man += QStringLiteral("%1  %2x%3  %4  [%5]\n")
                   .arg(e.base + QStringLiteral(".png"), -44)
                   .arg(img.width()).arg(img.height())
                   .arg(img.hasAlphaChannel() ? QStringLiteral("RGBA") : QStringLiteral("RGB"), -4)
                   .arg(e.tail);
        ++n;
    }
    man += QStringLiteral("\n%1 texture(s) written.\n").arg(n);

    QFile mf(QDir(outDir).filePath(QStringLiteral("TEXTURES.txt")));
    if (mf.open(QIODevice::WriteOnly | QIODevice::Text)) mf.write(man.toUtf8());
    if (outManifest) *outManifest = man;
    return n;
}

// Per-thread texture-decode cap for loadTexture: -1 = the UI default (512 px), 0 = native (full res,
// incl. the streamed 4K HD mip0), N>0 = cap the longest side. Thread-local so the thumbnail render
// thread and the GUI thread (3D preview / export) each choose their own without a shared-flag race.
static thread_local int t_texCap = -1;

int Bf6Store::setDecodeTextureCap(int capPx) { const int prev = t_texCap; t_texCap = capPx; return prev; }

QImage Bf6Store::loadTexture(const core::AssetId& id, core::TextureInfo* info, QString* error)
{
    const bf6::Resource* tex = resAt(id);
    if (!tex || !isTextureId(id)) { if (error) *error = QStringLiteral("not a texture"); return {}; }
    std::vector<uint8_t> tb; std::string e;
    if (!m_toc.readResource(*tex, tb, &e)) { if (error) *error = QString::fromStdString(e); return {}; }
    if (tb.size() < 56) { if (error) *error = QStringLiteral("texture header too small"); return {}; }
    const int32_t fmt = int32_t(uint32_t(tb[12]) | uint32_t(tb[13])<<8 | uint32_t(tb[14])<<16 | uint32_t(tb[15])<<24);
    const int mipflag = tb[21];
    const uint32_t declW = uint32_t(tb[22]) | uint32_t(tb[23])<<8;   // FULL (4K) declared size
    const uint32_t declH = uint32_t(tb[24]) | uint32_t(tb[25])<<8;
    uint32_t w = declW, h = declH;
    if (mipflag > 0) { w /= 2; h /= 2; }   // resident chain top (one mip down); the 4K mip0 streams from the
                                           // HRES pack and is picked up below for a full-res decode
    if (w == 0 || h == 0) { if (error) *error = QStringLiteral("zero texture dimensions"); return {}; }
    QString guid;
    for (int k = 0; k < 16; ++k) guid += QStringLiteral("%1").arg(tb[40 + k], 2, 16, QLatin1Char('0')).toUpper();

    const int32_t dxgi = dxgiFromFb(fmt);
    int bc = 0, blockBytes = 16; QString fmtName;
    switch (dxgi) {
    case 71: case 72: bc = 1; blockBytes = 8;  fmtName = QStringLiteral("BC1"); break;
    case 74: case 75: bc = 2; blockBytes = 16; fmtName = QStringLiteral("BC2"); break;
    case 77: case 78: bc = 3; blockBytes = 16; fmtName = QStringLiteral("BC3"); break;
    case 80: case 81: bc = 4; blockBytes = 8;  fmtName = QStringLiteral("BC4"); break;
    case 83: case 84: bc = 5; blockBytes = 16; fmtName = QStringLiteral("BC5"); break;
    case 98: case 99: bc = 7; blockBytes = 16; fmtName = QStringLiteral("BC7"); break;
    default:          fmtName = QStringLiteral("DXGI %1").arg(dxgi); break;   // BC6H / uncompressed — export still works
    }
    if (info) { info->width = int(w); info->height = int(h); info->format = fmtName; info->mips = 1; }
    if (bc == 0) { if (error) *error = QStringLiteral("in-app preview not supported for %1 yet — Export (DDS) works").arg(fmtName); return {}; }

    // Pixel source. For a full-res decode, prefer the streamed 4K HD mip0 (HRES pack) when present — it is
    // exactly the top mip, so no mip walk. Otherwise read the resident chain chunk (the streamed GLOBAL
    // chunk, bundleIndex < 0; per-bundle entries are the resident tail) and walk down to a cap: 512 keeps
    // the 31k-texture grid + model thumbnails responsive, lifted (setDecodeFullRes) for export.
    // Per-thread cap (setDecodeTextureCap): the UI default caps the grid/thumbnails/preview; an export
    // sets 0 (native) or an explicit size.
    const int capReq = t_texCap;
    const uint32_t cap = (capReq < 0)  ? 512u              // UI default
                       : (capReq == 0) ? 0xFFFFFFFFu       // native (incl. the 4K HD mip0)
                                       : uint32_t(capReq); // explicit cap
    std::vector<uint8_t> px;
    uint32_t mw, mh; size_t off = 0;
    // Use the streamed 4K HD mip0 only when the cap actually wants the full declared size; otherwise the
    // resident chain (walked down to the cap) already covers it, with no 16 MiB HD read.
    const int hdIdx = (cap >= declW) ? hdMip0Chunk(tb) : -1;
    if (hdIdx >= 0 && m_hdToc.readResource(m_hdToc.resources()[size_t(hdIdx)], px, &e) && !px.empty()) {
        mw = declW; mh = declH;            // the HD chunk is the top mip (off 0)
    } else {
        px.clear();
        const bf6::Resource* ch = nullptr; const bf6::Resource* anyc = nullptr;
        for (const auto& r : m_toc.resources()) {
            if (r.kind != bf6::ResKind::Chunk || QString::fromStdString(r.name) != guid) continue;
            if (r.bundleIndex < 0) { ch = &r; break; }
            if (!anyc) anyc = &r;
        }
        if (!ch) ch = anyc;
        if (!ch) { if (error) *error = QStringLiteral("pixel chunk %1 not found").arg(guid); return {}; }
        if (!m_toc.readResource(*ch, px, &e)) { if (error) *error = QString::fromStdString(e); return {}; }
        mw = w; mh = h;
        while ((mw > cap || mh > cap) && mw > 4 && mh > 4) {
            off += size_t((mw + 3) / 4) * size_t((mh + 3) / 4) * size_t(blockBytes);
            mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1;
        }
    }
    const size_t bw = (mw + 3) / 4, bh = (mh + 3) / 4;
    if (off + bw * bh * size_t(blockBytes) > px.size()) { if (error) *error = QStringLiteral("pixel data too short for %1").arg(fmtName); return {}; }

    QImage img(int(mw), int(mh), QImage::Format_RGBA8888);
    img.fill(Qt::transparent);
    for (size_t by = 0; by < bh; ++by) {
        for (size_t bx = 0; bx < bw; ++bx) {
            const uint8_t* src = px.data() + off + (by * bw + bx) * size_t(blockBytes);
            uint8_t rgba[64];
            switch (bc) {
            case 1: bcdec_bc1(src, rgba, 4 * 4); break;
            case 2: bcdec_bc2(src, rgba, 4 * 4); break;
            case 3: bcdec_bc3(src, rgba, 4 * 4); break;
            case 7: bcdec_bc7(src, rgba, 4 * 4); break;
            case 4: { uint8_t r[16]; bcdec_bc4(src, r, 4);      for (int i = 0; i < 16; ++i) { rgba[i*4]=rgba[i*4+1]=rgba[i*4+2]=r[i]; rgba[i*4+3]=255; } } break;
            case 5: { uint8_t rg[32]; bcdec_bc5(src, rg, 4 * 2); for (int i = 0; i < 16; ++i) { rgba[i*4]=rg[i*2]; rgba[i*4+1]=rg[i*2+1]; rgba[i*4+2]=255; rgba[i*4+3]=255; } } break;
            }
            for (int py = 0; py < 4; ++py) {
                const int y = int(by * 4) + py; if (y >= int(mh)) break;
                uint8_t* dstRow = img.scanLine(y);
                for (int pxx = 0; pxx < 4; ++pxx) {
                    const int x = int(bx * 4) + pxx; if (x >= int(mw)) continue;
                    const uint8_t* s = rgba + (py * 4 + pxx) * 4;
                    uint8_t* d = dstRow + x * 4;
                    d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3];
                }
            }
        }
    }
    return img;
}

// ── native MeshSet decode (loadModel) ──────────────────────────────────────────────────────────────
namespace {
float halfToFloat(uint16_t h)
{
    const uint32_t sign = uint32_t(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1F, mant = h & 0x3FF, f;
    if (exp == 0) {
        if (mant == 0) f = sign;
        else { int e = 0; while ((mant & 0x400) == 0) { mant <<= 1; ++e; } mant &= 0x3FF; f = sign | (uint32_t(113 - e) << 23) | (mant << 13); }
    } else if (exp == 0x1F) f = sign | 0x7F800000u | (mant << 13);
    else f = sign | ((exp + 112) << 23) | (mant << 13);
    float o; std::memcpy(&o, &f, 4); return o;
}
// Little-endian scalar reads over a byte buffer; reads past the end return 0 (fail soft).
struct Rd {
    const uint8_t* d; size_t n;
    uint8_t  u8 (size_t p) const { return p < n ? d[p] : 0; }
    uint16_t u16(size_t p) const { return p + 2 <= n ? uint16_t(d[p] | d[p+1]<<8) : 0; }
    int16_t  i16(size_t p) const { return int16_t(u16(p)); }
    uint32_t u32(size_t p) const { return p + 4 <= n ? (uint32_t(d[p]) | uint32_t(d[p+1])<<8 | uint32_t(d[p+2])<<16 | uint32_t(d[p+3])<<24) : 0; }
    int32_t  i32(size_t p) const { return int32_t(u32(p)); }
    float    f32(size_t p) const { uint32_t v = u32(p); float o; std::memcpy(&o, &v, 4); return o; }
    float    hf (size_t p) const { return halfToFloat(u16(p)); }
    int64_t  i64(size_t p) const { uint64_t v = 0; for (int k = 0; k < 8; ++k) v |= uint64_t(u8(p + size_t(k))) << (8*k); return int64_t(v); }
};
// A blend index: 15-bit, top bit flags a physics/extra bone (idx>>1 + bone count). Static load has no
// skeleton, so nb=0 — the index is only used for export fidelity, not the bind-pose render.
uint16_t readBone(const Rd& c, size_t p)
{
    const int num = c.i16(p);
    int idx = num & 0x7FFF;
    if (num & 0x8000) idx >>= 1;
    return uint16_t(idx);
}
}  // namespace

bool Bf6Store::loadModel(const core::AssetId& id, core::Geometry& geo, QString* error)
{
    if (id.value & kFullBit) {   // assembled full-character pseudo-mesh (merge every cosmetic part)
        const int fidx = int(id.value & ~kFullBit);
        if (fidx < 0 || fidx >= m_fullKeys.size()) { if (error) *error = QStringLiteral("bad full-character id"); return false; }
        return assembleCharacter(m_fullKeys[fidx], geo, error);
    }
    const bf6::Resource* ms = resAt(id);
    if (!ms || !isMeshId(id)) { if (error) *error = QStringLiteral("not a MeshSet"); return false; }
    static const bool kDumpLoad = qEnvironmentVariableIsSet("BF6_DUMP_LOAD");   // env-gated timing diag
    QElapsedTimer tmr; if (kDumpLoad) { tmr.start(); qInfo("bf6 loadModel ENTER idx=%llu %s", (unsigned long long)id.value, ms->name.c_str()); }
    std::vector<uint8_t> mb; std::string e;
    if (!m_toc.readResource(*ms, mb, &e)) { if (error) *error = QString::fromStdString(e); return false; }
    if (mb.size() < 256) { if (error) *error = QStringLiteral("MeshSet too small"); return false; }
    const Rd M{ mb.data(), mb.size() };

    const size_t num2 = size_t(16 + M.i32(48));
    if (num2 + 136 > mb.size()) { if (error) *error = QStringLiteral("bad MeshSet header"); return false; }
    const int    sectionCount = M.i32(num2 + 8);
    const size_t secTable     = size_t(16 + M.i32(num2 + 12));
    const int32_t num5        = M.i32(num2 + 84);
    const size_t  idxBase     = size_t(M.i32(num2 + 92));   // index-buffer base in the chunk
    const int idxSize = (num5 == 46) ? 4 : 2;
    if (sectionCount <= 0 || sectionCount > 4096) { if (error) *error = QStringLiteral("implausible section count %1").arg(sectionCount); return false; }
    static const char* const kHex = "0123456789ABCDEF";
    std::string guid; guid.reserve(32);
    for (int k = 0; k < 16; ++k) { const uint8_t b = mb[num2 + 116 + k]; guid += kHex[b >> 4]; guid += kHex[b & 0xF]; }

    // vertex + index buffers live in the mesh's chunk — O(1) via the open()-time GUID index (largest wins)
    const auto ci = m_chunkByGuid.find(guid);
    if (ci == m_chunkByGuid.end()) { if (error) *error = QStringLiteral("mesh chunk %1 not found").arg(QString::fromStdString(guid)); return false; }
    const bf6::Resource* ch = &m_toc.resources()[size_t(ci->second)];
    std::vector<uint8_t> cb;
    if (!m_toc.readResource(*ch, cb, &e)) { if (error) *error = QString::fromStdString(e); return false; }
    const Rd C{ cb.data(), cb.size() };

    bool anySkin = false;
    QStringList matNames;                 // region name per material index (geo.materialNames)
    QHash<QString, int> matIndex;
    for (int s = 0; s < sectionCount; ++s) {
        const size_t sdh = secTable + size_t(368) * size_t(s);       // section descriptor
        if (sdh + 198 > mb.size()) break;
        // Per-section COOKED material binding (region): the ref at sdh+8 points to a 16-byte material
        // GUID followed by the inline name "M_<Region>" ("…_Shadow" for the shadow-caster proxy). This
        // is how one MeshSet maps its sections to the set's region textures (upperbody/lowerbody/hands/
        // headgear/…) — without it every part shared one texture.
        QString region;
        { const size_t nameOff = size_t(M.i32(sdh + 8)) + 16;
          QString nm; for (size_t p = nameOff; p < mb.size() && mb[p] >= 32 && mb[p] < 127 && nm.size() < 48; ++p) nm += QLatin1Char(char(mb[p]));
          if (nm.startsWith(QLatin1String("M_"), Qt::CaseInsensitive)) nm = nm.mid(2);
          region = nm.toLower(); }
        if (region.contains(QLatin1String("shadow"))) continue;      // shadow-caster proxy: redundant geometry
        const size_t sd = sdh + 32;                                  // Fb_bf6_mesh: num4 + 368*i + 32
        const int idxCount  = M.i32(sd);         // triangles in this section
        const int idxStart  = M.i32(sd + 4);     // first index (units of idxSize)
        const size_t vtxOff = size_t(M.i32(sd + 8));
        const int vtxCount  = M.i32(sd + 12);
        if (vtxCount <= 0 || vtxCount > 8000000 || idxCount < 0) continue;

        const size_t ad = sd + 68;               // 16 bytes (num18..21) + 52 skipped
        uint8_t usage[16], fmt[16]; int stride[16];
        for (int k = 0; k < 16; ++k) { usage[k] = M.u8(ad + k*4); fmt[k] = M.u8(ad + k*4 + 1); }
        for (int k = 0; k < 16; ++k) stride[k] = M.u16(ad + 64 + k*2);
        const int activeAttr = M.u8(ad + 96);

        const int base = geo.vertices.size();
        geo.vertices.resize(base + vtxCount);
        size_t block = vtxOff;                   // SoA: one contiguous block per attribute
        for (int k = 0; k < activeAttr && k < 16; ++k) {
            const int u = usage[k], f = fmt[k];
            size_t p = block;
            for (int j = 0; j < vtxCount; ++j, p += size_t(stride[k] > 0 ? stride[k] : 0)) {
                core::Vertex& v = geo.vertices[base + j];
                if (u == 1)        { if (f == 8) { v.px = C.hf(p); v.py = C.hf(p+2); v.pz = C.hf(p+4); } else { v.px = C.f32(p); v.py = C.f32(p+4); v.pz = C.f32(p+8); } }
                else if (u == 6)   { v.nx = -C.hf(p); v.ny = -C.hf(p+2); v.nz = -C.hf(p+4); }   // NEGATED: the authored half3 normal points INWARD relative to our (reversed) winding — stored·face ≈ -1, so shading read inverted (in Blender it took a manual Alt+N Reset Vectors to fix). Negating aligns the normal with the face and keeps the authored hard-edge smoothing.
                else if (u == 2)   { anySkin = true; v.joints[3]=readBone(C,p); v.joints[2]=readBone(C,p+2); v.joints[1]=readBone(C,p+4); v.joints[0]=readBone(C,p+6); }
                else if (u == 4)   { v.weights[3]=C.u8(p)/255.f; v.weights[2]=C.u8(p+1)/255.f; v.weights[1]=C.u8(p+2)/255.f; v.weights[0]=C.u8(p+3)/255.f; }
                else if (u >= 33 && u <= 40) { const int n = u - 33; float uu, vv; if (f == 6) { uu = C.hf(p); vv = C.hf(p+2); } else { uu = C.f32(p); vv = C.f32(p+4); } if (n == 0) { v.u=uu; v.v=vv; } else if (n == 1) { v.u2=uu; v.v2=vv; geo.hasUv1 = true; } }
                else if (u == 30)  { if (f == 4) { for (int c=0;c<4;++c) v.color[c]=uint8_t(qBound(0.f, C.f32(p+c*4)*255.f, 255.f)); } else { for (int c=0;c<4;++c) v.color[c]=C.u8(p+c); } geo.hasColor = true; }
            }
            block += size_t(stride[k] > 0 ? stride[k] : 0) * size_t(vtxCount);
        }
        // index buffer
        int mi = matIndex.value(region, -1);
        if (mi < 0) { mi = matNames.size(); matNames << region; matIndex.insert(region, mi); }
        core::Part part;
        part.name = region.isEmpty() ? QStringLiteral("section_%1").arg(s) : region;
        part.materialIndex = mi;   // per-region cooked material (resolveMaterials maps it to a texture)
        part.indexStart = uint32_t(geo.indices.size());
        part.indexCount = uint32_t(idxCount) * 3;
        size_t ip = idxBase + size_t(idxStart) * size_t(idxSize);
        for (int j = 0; j < idxCount; ++j) {
            uint32_t a, b, c;
            if (idxSize == 4) { a = C.u32(ip); b = C.u32(ip+4); c = C.u32(ip+8); ip += 12; }
            else              { a = C.u16(ip); b = C.u16(ip+2); c = C.u16(ip+4); ip += 6; }
            geo.indices.push_back(uint32_t(base) + c);
            geo.indices.push_back(uint32_t(base) + b);
            geo.indices.push_back(uint32_t(base) + a);
        }
        geo.parts.push_back(part);
    }
    if (geo.vertices.isEmpty() || geo.indices.isEmpty()) { if (error) *error = QStringLiteral("no geometry decoded from %1 section(s)").arg(sectionCount); return false; }
    geo.materialNames = matNames.isEmpty() ? QStringList{ QStringLiteral("bf6") } : matNames;
    geo.skinned = anySkin;
    geo.sourcePath = QString::fromStdString(ms->name);
    geo.formatNote = QStringLiteral("bf6 MeshSet · %1 section(s)").arg(geo.parts.size());
    geo.computeBounds();
    if (kDumpLoad) qInfo("bf6 loadModel OK    idx=%llu %dms %d verts %d tris", (unsigned long long)id.value, int(tmr.elapsed()), geo.vertices.size(), geo.indices.size()/3);
    return true;
}

core::Skeleton Bf6Store::loadSkeleton(const core::AssetId&, const core::Geometry&, bool, QVector<core::Clip>*)
{
    // v1: the shared soldier rig (ske_soldier_3p), as the mesh export uses. Per-character skeleton
    // resolution is a refinement. From Fb_bf6_mesh: skip 72, int64 offset table → num11(names),
    // offset(parents), offset2(world bind matrices, 64 B/bone laid out as a row-major Mat4).
    core::Skeleton skel;
    const bf6::Resource* ske = nullptr;
    for (const auto& r : m_toc.resources())
        if (r.kind == bf6::ResKind::Ebx && r.name.find("ske_soldier_3p") != std::string::npos) { ske = &r; break; }
    if (!ske) { skel.note = QStringLiteral("ske_soldier_3p.ebx not found"); return skel; }
    std::vector<uint8_t> sb; std::string e;
    if (!m_toc.readResource(*ske, sb, &e)) { skel.note = QString::fromStdString(e); return skel; }
    if (sb.size() < 200) { skel.note = QStringLiteral("skeleton ebx too small"); return skel; }
    const Rd S{ sb.data(), sb.size() };

    const size_t num11   = size_t(int64_t(104) + S.i64(104));   // name table (bone count at num11-4)
    const size_t offP    = size_t(int64_t(120) + S.i64(120));   // parent index[nb]
    const size_t offBind = size_t(int64_t(144) + S.i64(144));   // world bind Mat4[nb] (64 B each)
    if (num11 < 4 || num11 > sb.size()) { skel.note = QStringLiteral("bad skeleton header"); return skel; }
    const int nb = S.i32(num11 - 4);
    if (nb <= 0 || nb > 100000) { skel.note = QStringLiteral("implausible bone count %1").arg(nb); return skel; }

    std::vector<core::rm::Mat4> world(static_cast<size_t>(nb));
    skel.joints.resize(nb);
    for (int i = 0; i < nb; ++i) {
        const size_t np = num11 + size_t(i) * 8;
        const size_t namePos = size_t(int64_t(np) + S.i64(np));
        QString nm; for (size_t p = namePos; p < sb.size() && sb[p]; ++p) nm += QLatin1Char(char(sb[p]));
        core::Joint& jt = skel.joints[i];
        jt.name = nm.isEmpty() ? QStringLiteral("bone_%1").arg(i) : nm;
        const int parent = S.i32(offP + size_t(i) * 4);
        jt.parent = (parent >= 0 && parent < nb) ? parent : -1;
        core::rm::Mat4 w;
        const size_t mp = offBind + size_t(i) * 64;
        for (int k = 0; k < 16; ++k) w[size_t(k)] = S.f32(mp + size_t(k) * 4);
        // Frostbite stores the bind matrix's homogeneous [3][3] element as 0, so the raw 4x4 is
        // SINGULAR — rm::inverse() then fails closed to identity, making every bone's local = its
        // world and the exported glb rig explode when a viewer applies the skin (the in-app static
        // bind-pose preview hides this). Restore it to a proper affine transform.
        w[3] = 0.0f; w[7] = 0.0f; w[11] = 0.0f; w[15] = 1.0f;
        world[size_t(i)] = w;
    }
    for (int i = 0; i < nb; ++i) {
        core::Joint& jt = skel.joints[i];
        jt.inverseBind = core::rm::inverse(world[size_t(i)]);
        const core::rm::Mat4 local = (jt.parent < 0) ? world[size_t(i)]
                                     : core::rm::mul(world[size_t(i)], core::rm::inverse(world[size_t(jt.parent)]));
        float t[3], q[4], s[3];
        core::rm::decomposeTRS(local, t, q, s);
        jt.restT[0]=t[0]; jt.restT[1]=t[1]; jt.restT[2]=t[2];
        jt.restQ = { q[0], q[1], q[2], q[3] };
        jt.restS[0]=s[0]; jt.restS[1]=s[1]; jt.restS[2]=s[2];
    }
    if (!skel.parentsPrecedeChildren()) skel.sortParentsFirst();
    if (qEnvironmentVariableIsSet("BF6_DUMP_LOAD")) {
        // Bind-pose self-check: composedWorld(restTRS) · inverseBind must be identity, or the exported
        // glb rig deforms when a glTF viewer applies the skin (the in-app static preview hides this).
        std::vector<core::rm::Mat4> cw(skel.joints.size());
        int worst = -1, negDet = 0; float worstErr = 0;
        const core::rm::Mat4 I = core::rm::identity();
        for (int i = 0; i < int(skel.joints.size()); ++i) {
            const core::Joint& j = skel.joints[size_t(i)];
            const core::rm::Mat4 L = core::rm::composeTRS(j.restT, j.restQ.data(), j.restS);
            cw[size_t(i)] = (j.parent < 0) ? L : core::rm::mul(L, cw[size_t(j.parent)]);
            const core::rm::Mat4 P = core::rm::mul(cw[size_t(i)], j.inverseBind);
            float e = 0;
            for (int k = 0; k < 16; ++k) e = qMax(e, float(qAbs(P[size_t(k)] - I[size_t(k)])));
            if (core::rm::determinant3(core::rm::inverse(j.inverseBind)) < 0) ++negDet;
            if (e > worstErr) { worstErr = e; worst = i; }
        }
        qInfo("bf6 skeleton self-check: %d joints · worst bind-pose err %.3f at joint %d '%s' · %d bone(s) with negative determinant",
              int(skel.joints.size()), double(worstErr), worst, worst>=0?qUtf8Printable(skel.joints[size_t(worst)].name):"", negDet);
    }
    skel.note = QStringLiteral("bf6 ske_soldier_3p · %1 bones").arg(nb);
    return skel;
}

QString Bf6Store::diagnostics() const
{
    if (!m_open) return QStringLiteral("not open");
    return QStringLiteral("%1 resources — %2 meshes, %3 textures, %4 install bundles; "
                          "name layer: %5 codenames / %6 skins identified")
        .arg(m_toc.resources().size()).arg(m_model.size()).arg(m_texture.size()).arg(m_toc.bundles().size())
        .arg(m_names.codenameCount()).arg(m_names.skinCount());
}

}  // namespace bf6game
