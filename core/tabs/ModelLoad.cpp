#include "core/tabs/ModelLoad.h"
#include "core/export/GlbExporter.h"

#include <QDir>
#include <QFileInfo>
#include <QScopeGuard>

namespace core::model {

Loaded load(IGameStore& store, const AssetId& id, bool decodeClips, bool decodeImages, bool withAttachments)
{
    Loaded out;
    out.id = id;
    QString err;
    if (!store.loadModel(id, out.geo, &err)) { out.error = err.isEmpty() ? QStringLiteral("load failed") : err; return out; }
    out.skeleton  = store.loadSkeleton(id, out.geo, decodeClips, &out.clips);
    out.materials = store.resolveMaterials(id, out.geo, decodeImages);
    out.hardpoints = store.hardpoints(id, out.skeleton);
    if (withAttachments) out.attachments = store.attachments(id);
    out.ok = true;
    return out;
}

bool ensureClip(IGameStore& store, Loaded& m, int i, QString* error)
{
    if (i < 0 || i >= m.clips.size()) { if (error) *error = QStringLiteral("no clip %1").arg(i); return false; }
    Clip& c = m.clips[i];
    if (!c.lazy) return true;
    QString err;
    if (!store.bakeClip(m.id, i, c, &err)) { if (error) *error = err.isEmpty() ? QStringLiteral("the store could not decode it") : err; return false; }
    c.lazy = false;
    return true;
}

int ensureClips(IGameStore& store, Loaded& m, int onlyClip)
{
    int stillLazy = 0;
    const int from = onlyClip >= 0 ? onlyClip : 0, to = onlyClip >= 0 ? onlyClip + 1 : m.clips.size();
    for (int i = from; i < to && i < m.clips.size(); ++i) {
        QString err;
        if (!m.clips[i].lazy) continue;
        if (!ensureClip(store, m, i, &err)) { ++stillLazy; qWarning("clips: %s not decoded — %s", qUtf8Printable(m.clips[i].name), qUtf8Printable(err)); }
    }
    return stillLazy;
}

namespace {
// How many material nSlots a piece's parts address: its names, or its highest part index + 1 — a
// store may leave materialNames empty and index its resolved materials directly (D4 does).
int materialSlots(const Geometry& g, int resolved)
{
    int n = qMax(int(g.materialNames.size()), resolved);
    for (const Part& p : g.parts) n = qMax(n, p.materialIndex + 1);
    return n;
}

// Append `ag` (already in the body's model space, joints = body joint indices) to `merged`, its
// material nSlots starting at `baseMat` (the merged MATERIAL count, which is the roster parts index —
// not materialNames.size(), which a store may leave empty).
void appendPiece(Geometry& merged, const Geometry& ag, int attachmentIndex, QVector<int>& ofPart, int baseMat, int nSlots)
{
    const uint32_t baseVert = uint32_t(merged.vertices.size());
    merged.materialNames.resize(baseMat);                    // names line up with the material nSlots
    for (int i = 0; i < nSlots; ++i) merged.materialNames.append(ag.materialNames.value(i));
    merged.vertices += ag.vertices;
    for (const Part& part : ag.parts) {
        Part np = part;
        np.indexStart = uint32_t(merged.indices.size());
        np.materialIndex = part.materialIndex >= 0 ? baseMat + part.materialIndex : -1;
        np.group = -1;
        for (uint32_t k = 0; k < part.indexCount; ++k) merged.indices.append(baseVert + ag.indices[part.indexStart + k]);
        merged.parts.append(np);
        ofPart.append(attachmentIndex);
    }
    merged.hasTangent = merged.hasTangent && ag.hasTangent;
}
}  // namespace

Assembly assemble(IGameStore& store, const Loaded& m, const QVector<bool>& on, bool decodeImages)
{
    Assembly out;
    out.geo = m.geo;
    out.materials = m.materials;
    out.materials.resize(materialSlots(m.geo, int(m.materials.size())));
    out.attachmentOfPart.fill(-1, m.geo.parts.size());
    const bool storePlaces = store.placesAttachments();
    bool coreBroughtMaterials = false;
    for (int pi = 0; pi < m.attachments.size(); ++pi) {
        const Attachment& p = m.attachments[pi];
        if (!(on.isEmpty() ? p.defaultOn : on.value(pi, false))) continue;
        auto skip = [&](const QString& why) { out.whyNot.insert(pi, why); out.skipped << QStringLiteral("%1 (%2)").arg(p.label, why); };
        if (!p.mesh.isValid()) { skip(p.unresolvedNote.isEmpty() ? QStringLiteral("mesh unresolved") : p.unresolvedNote); continue; }
        if (storePlaces) {
            Geometry ag; QVector<Material> mats; QString e;
            if (!store.placeAttachment(m.id, m.skeleton, p, decodeImages, ag, mats, &e) || ag.isEmpty()) {
                skip(e.isEmpty() ? QStringLiteral("not placed") : e);
                continue;
            }
            const int nSlots = materialSlots(ag, int(mats.size()));
            mats.resize(nSlots);
            appendPiece(out.geo, ag, pi, out.attachmentOfPart, int(out.materials.size()), nSlots);
            out.materials += mats;
            ++out.placed;
            continue;
        }
        int bi = -1;
        for (int i = 0; i < m.skeleton.joints.size(); ++i)
            if (m.skeleton.joints[i].name.compare(p.parentBone, Qt::CaseInsensitive) == 0) { bi = i; break; }
        if (bi < 0) { skip(QStringLiteral("bone %1 not in rig").arg(p.parentBone)); continue; }
        Geometry ag; QString e;
        if (!store.loadModel(p.mesh, ag, &e) || ag.isEmpty()) { skip(e.isEmpty() ? QStringLiteral("load failed") : e); continue; }
        // The parent bone's model-space bind, canonical frame: bind = inverse(inverseBind).
        const rm::Mat4 T = rm::inverse(m.skeleton.joints[bi].inverseBind);
        for (Vertex& v : ag.vertices) {
            float o[3];
            rm::transformPoint(T, v.px, v.py, v.pz, o); v.px = o[0]; v.py = o[1]; v.pz = o[2];
            rm::transformDir  (T, v.nx, v.ny, v.nz, o); v.nx = o[0]; v.ny = o[1]; v.nz = o[2];
            rm::transformDir  (T, v.tx, v.ty, v.tz, o); v.tx = o[0]; v.ty = o[1]; v.tz = o[2];
            v.joints[0] = uint16_t(bi); v.joints[1] = v.joints[2] = v.joints[3] = 0;
            v.weights[0] = 1.0f; v.weights[1] = v.weights[2] = v.weights[3] = 0.0f;   // rigid to the bone
        }
        const int nSlots = materialSlots(ag, 0);
        appendPiece(out.geo, ag, pi, out.attachmentOfPart, int(out.materials.size()), nSlots);
        out.materials.resize(out.materials.size() + nSlots);
        if (nSlots > 0) coreBroughtMaterials = true;
        ++out.placed;
    }
    if (out.placed > 0) {
        // Skinned when the body was, or a piece is weighted to a bone (the core rule always is; a
        // store may place a piece static — D4's mount, or anything on a static body).
        bool weighted = m.geo.skinned;
        for (int i = m.geo.vertices.size(); !weighted && i < out.geo.vertices.size(); ++i)
            weighted = out.geo.vertices[i].weights[0] > 0.0f;
        out.geo.skinned = weighted;
        out.geo.computeBounds();
    }
    if (coreBroughtMaterials) {
        // Core-placed pieces brought their own materials: resolve them off the merged roster (a
        // store that resolves by geometry — POE2 — names each piece's own).
        out.materials = store.resolveMaterials(m.id, out.geo, decodeImages);
        if (out.materials.size() < out.geo.materialNames.size()) out.materials.resize(out.geo.materialNames.size());
    }
    return out;
}

QString mirrorPath(const QString& root, const QString& gamePath)
{
    QString p = gamePath;
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (p.size() >= 2 && p.at(1) == QLatin1Char(':')) p = p.mid(2);   // "C:…" (a drive prefix)
    QStringList kept;
    for (const QString& seg : p.split(QLatin1Char('/'), Qt::SkipEmptyParts))
        if (seg != QLatin1String(".") && seg != QLatin1String("..")) kept << seg;
    return kept.isEmpty() ? QDir(root).filePath(QStringLiteral("_unnamed")) : QDir(root).filePath(kept.join(QLatin1Char('/')));
}

QString uniquePath(const QString& dir, const QString& stem, const QString& ext)
{
    QString out = QDir(dir).filePath(stem + ext);
    for (int n = 1; QFileInfo::exists(out); ++n) out = QDir(dir).filePath(QStringLiteral("%1_%2%3").arg(stem).arg(n).arg(ext));
    return out;
}

ExportResult exportOne(IGameStore& store, const AssetId& id, const ExportOptions& opt, const QString& outPath,
                       const QVector<int>& partSubset, const QVector<bool>& attachmentsOn,
                       const std::function<void(Loaded&, bool)>& adjust)
{
    ExportResult r;
    r.path = outPath;
    // A saved export wants full-resolution textures even though the live UI decodes smaller for speed;
    // bracket the whole load+assemble so EVERY export path gets it (not just a plugin's own action).
    // capPx from the options (0 = native). Restored on every exit.
    const int prevCap = store.setDecodeTextureCap(opt.maxTextureSize);
    const auto restoreQ = qScopeGuard([&store, prevCap] { store.setDecodeTextureCap(prevCap); });
    const bool wantImages = opt.embedTextures || opt.looseTextures;
    Loaded m = load(store, id, opt.includeAnimations, wantImages, opt.includeAttachments);
    if (!m.ok) { r.error = m.error; return r; }
    if (adjust) adjust(m, wantImages);
    if (opt.includeAnimations && !m.clips.isEmpty()) {
        // Lazy clips are decoded here, for the file — the only place every clip is wanted at once.
        ensureClips(store, m, opt.onlyClip);
        QVector<Clip> ready;
        for (const Clip& c : m.clips) if (!c.lazy) ready.append(c);
        if (ready.size() != m.clips.size()) qInfo("export: %d of %d clips could not be decoded and are not in the file", int(m.clips.size() - ready.size()), int(m.clips.size()));
        m.clips = ready;
    }

    Geometry geo = m.geo;
    QVector<Material> mats = m.materials;
    if (opt.includeAttachments && !m.attachments.isEmpty()) {
        const Assembly a = assemble(store, m, attachmentsOn, wantImages);
        geo = a.geo;
        mats = a.materials;
        for (const QString& s : a.skipped) qInfo("export: attachment skipped — %s", qUtf8Printable(s));
    }
    // The viewport part selection names the BODY's parts (the first m.geo.parts.size() of an
    // assembly); the attached pieces follow their ticks.
    if (!partSubset.isEmpty())
        for (int i = 0; i < m.geo.parts.size() && i < geo.parts.size(); ++i) geo.parts[i].visible = partSubset.contains(i);

    // glTF animates node transforms, so a file with no joints has nothing to animate: an animated
    // export carries the skeleton whatever the checkbox says (FOX's rule, and it is said in the log).
    ExportOptions eff = opt;
    if (eff.includeAnimations && !eff.includeSkeleton && !m.clips.isEmpty()) {
        eff.includeSkeleton = true;
        qInfo("export: animations requested without a skeleton — the skeleton is written (glTF animates nodes)");
    }
    glb::Scene scene;
    scene.geometry = &geo;
    scene.skeleton = eff.includeSkeleton ? &m.skeleton : nullptr;
    scene.clips = eff.includeAnimations ? m.clips : QVector<Clip>();
    scene.materials = mats;
    scene.hardpoints = m.hardpoints;
    glb::Result res;
    if (!glb::write(scene, eff, outPath, &res)) { r.error = res.error.isEmpty() ? QStringLiteral("write failed") : res.error; return r; }
    r.ok = true;
    r.summary = res.summary();
    return r;
}

}  // namespace core::model
