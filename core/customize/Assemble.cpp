#include "core/customize/Assemble.h"
#include "core/math/RigMath.h"

#include <QFileInfo>
#include <algorithm>
#include <QSet>
#include <cmath>
#include <deque>
#include <QMutexLocker>

namespace core::customize {

// ── MeshCache ─────────────────────────────────────────────────────────────────────────────────

std::shared_ptr<const MeshCache::Entry> MeshCache::get(IGameStore& store, const AssetId& id, bool wantRig, bool wantClips)
{
    std::shared_ptr<const Entry> have;
    {
        QMutexLocker l(&m_mutex);
        const auto it = m_map.constFind(id);
        if (it != m_map.constEnd()) {
            have = it.value();
            m_order.removeOne(id);
            m_order.append(id);
        }
    }
    const bool needRig = wantRig && (!have || !have->rigLoaded);
    const bool needClips = wantClips && (!have || !have->clipsLoaded);
    if (have && !needRig && !needClips) return have;

    // Load outside the lock (the store's thread-safe set); a racing loader of the same id only
    // costs a duplicate load, never a torn entry — entries are immutable once published.
    auto e = std::make_shared<Entry>(have ? *have : Entry());
    if (!have) {
        QString err;
        if (id.isValid() && store.loadModel(id, e->geo, &err)) {
            e->materials = store.resolveMaterials(id, e->geo, /*decodeImages*/true);
            e->ok = true;
        } else {
            e->error = err.isEmpty() ? QStringLiteral("did not load") : err;
        }
    }
    if (e->ok && (needRig || needClips)) {
        QVector<Clip> clips;
        e->skeleton = store.loadSkeleton(id, e->geo, needClips, needClips ? &clips : nullptr);
        e->rigLoaded = true;
        if (needClips) { e->clips = clips; e->clipsLoaded = true; }
    }
    QMutexLocker l(&m_mutex);
    m_map.insert(id, e);
    m_order.removeOne(id);
    m_order.append(id);
    while (m_order.size() > m_capacity) m_map.remove(m_order.takeFirst());
    return e;
}

QVector<Material> MeshCache::lookedMaterials(IGameStore& store, const AssetId& id, const Entry& e, const MaterialLook& look, QString* note)
{
    if (look.isEmpty() || !e.ok) return e.materials;
    const QString key = QString::number(id.scheme) + QLatin1Char(':') + QString::number(id.value, 16) + QLatin1Char('|') + look.key();
    {
        QMutexLocker l(&m_mutex);
        const auto it = m_looked.constFind(key);
        if (it != m_looked.constEnd()) {
            m_lookedOrder.removeOne(key);
            m_lookedOrder.append(key);
            if (note) *note = it->note;
            return it->materials;
        }
    }
    Looked lk;
    lk.materials = store.resolveLookedMaterials(id, e.geo, /*decodeImages*/true, look, &lk.note);
    if (note) *note = lk.note;
    QMutexLocker l(&m_mutex);
    m_looked.insert(key, lk);
    m_lookedOrder.removeOne(key);
    m_lookedOrder.append(key);
    while (m_lookedOrder.size() > m_capacity) m_looked.remove(m_lookedOrder.takeFirst());
    return lk.materials;
}

void MeshCache::clear()
{
    QMutexLocker l(&m_mutex);
    m_map.clear();
    m_order.clear();
    m_looked.clear();
    m_lookedOrder.clear();
}

int MeshCache::size() const
{
    QMutexLocker l(&m_mutex);
    return m_map.size();
}

// ── assemble ──────────────────────────────────────────────────────────────────────────────────

namespace {

// Material slots a mesh's parts address: its names, its resolved list, or its highest part index + 1
// (a store may leave materialNames empty and index resolved materials directly — D4 does).
int materialSlots(const Geometry& g, int resolved)
{
    int n = qMax(int(g.materialNames.size()), resolved);
    for (const Part& p : g.parts) n = qMax(n, p.materialIndex + 1);
    return n;
}

// A look's group rule on one mesh (CustomizeSource.h, Look): each part's shipped state — its own
// flag AND every group above it — then ((shipped hidden ∪ hide) − show) ∪ forceHide, matched on the
// part's name or any group's above it. Returns the per-part result; `unmatched` gets the look's
// names that nothing in this mesh carries (said, so a misspelt group is seen, not guessed).
QVector<bool> lookedVisibility(const Geometry& g, const Look& look, QStringList* unmatched)
{
    auto lower = [](const QSet<QString>& in) { QSet<QString> o; for (const QString& n : in) o.insert(n.toLower()); return o; };
    const QSet<QString> hide = lower(look.hide), show = lower(look.show), force = lower(look.forceHide);
    QSet<QString> seen;
    QVector<bool> out(g.parts.size(), true);
    for (int i = 0; i < g.parts.size(); ++i) {
        const Part& part = g.parts[i];
        bool shipped = part.visible;
        QStringList names{part.name.toLower()};
        for (int gi = part.group, guard = 0; gi >= 0 && gi < g.groups.size() && guard <= g.groups.size(); gi = g.groups[gi].parent, ++guard) {
            shipped = shipped && g.groups[gi].visible;
            names << g.groups[gi].name.toLower();
        }
        auto any = [&](const QSet<QString>& set) {
            bool hit = false;
            for (const QString& n : names) if (set.contains(n)) { hit = true; seen.insert(n); }
            return hit;
        };
        const bool h = any(hide), s = any(show), f = any(force);
        out[i] = !(((!shipped || h) && !s) || f);
    }
    if (unmatched) {
        QStringList u;
        for (const QSet<QString>* set : {&hide, &show, &force})
            for (const QString& n : *set) if (!seen.contains(n) && !u.contains(n)) u << n;
        std::sort(u.begin(), u.end());
        *unmatched = u;
    }
    return out;
}

void noteLook(Assembly& a, const QString& label, const QString& storeNote, const QStringList& unmatched)
{
    if (!storeNote.isEmpty()) a.notes << QStringLiteral("%1 — look: %2").arg(label, storeNote);
    if (!unmatched.isEmpty())
        a.notes << QStringLiteral("%1 — look names group(s) it does not have: %2").arg(label, unmatched.join(QStringLiteral(", ")));
}

void append(Assembly& a, const Geometry& g, const QVector<Material>& mats, const Piece& p)
{
    const int baseMat = a.materials.size();
    const int nSlots = materialSlots(g, int(mats.size()));
    const uint32_t baseVert = uint32_t(a.geo.vertices.size());
    const int firstPart = a.geo.parts.size();
    a.geo.materialNames.resize(baseMat);
    for (int i = 0; i < nSlots; ++i) a.geo.materialNames.append(g.materialNames.value(i));
    QVector<Material> m = mats;
    m.resize(nSlots);
    a.materials += m;
    a.geo.vertices += g.vertices;
    QStringList unmatched;
    const QVector<bool> vis = lookedVisibility(g, p.look, &unmatched);
    if (!unmatched.isEmpty()) noteLook(a, p.label, QString(), unmatched);
    for (int pi = 0; pi < g.parts.size(); ++pi) {
        const Part& part = g.parts[pi];
        Part np = part;
        np.visible = vis[pi];
        np.indexStart = uint32_t(a.geo.indices.size());
        np.materialIndex = part.materialIndex >= 0 ? baseMat + part.materialIndex : -1;
        np.group = -1;
        for (uint32_t k = 0; k < part.indexCount; ++k) a.geo.indices.append(baseVert + g.indices[part.indexStart + k]);
        a.geo.parts.append(np);
    }
    a.geo.hasTangent = a.geo.hasTangent && g.hasTangent;
    a.geo.hasUv1 = a.geo.hasUv1 || g.hasUv1;
    a.geo.hasColor = a.geo.hasColor || g.hasColor;
    a.pieces.append({p.label, p.slot, p.tag, p.mesh, p.placement, firstPart, int(a.geo.parts.size()) - firstPart});
    a.pieces.last().broughtBy = p.broughtBy;
}

// The figure's own mesh (the body, or a ReplaceBody form) under its look: the part flags carry the
// looked state, and every group reads visible because its flag is folded into its parts.
// Look::keepSubtree on one mesh, said when its rig lacks the bone (the mesh is then left whole).
void cutSubtree(Assembly& a, Geometry& g, const Skeleton& sk, const Look& look, const QString& label)
{
    if (look.keepSubtree.isEmpty()) return;
    if (!keepBoneSubtree(g, sk, look.keepSubtree))
        a.notes << QStringLiteral("%1 — look keeps the subtree of %2, which its rig does not have; left whole").arg(label, look.keepSubtree);
}

void lookFigure(Assembly& a, const Skeleton& sk, const Look& look, const QString& label)
{
    cutSubtree(a, a.geo, sk, look, label);
    QStringList unmatched;
    const QVector<bool> vis = lookedVisibility(a.geo, look, &unmatched);
    for (int i = 0; i < a.geo.parts.size(); ++i) a.geo.parts[i].visible = vis[i];
    for (PartGroup& gr : a.geo.groups) gr.visible = true;
    if (!unmatched.isEmpty()) noteLook(a, label, QString(), unmatched);
}

}  // namespace

Assembly assemble(IGameStore& store, MeshCache& cache, const Resolved& r, bool includeHeld, bool wantClips)
{
    Assembly a;
    if (!r.error.isEmpty()) { a.error = r.error; return a; }
    a.notes = r.notes;   // what the source had to say about the build (FOX: parts not in this install)

    // A ReplaceBody piece IS the figure: its own mesh, rig and clips (a shapeshift form).
    for (const Piece& p : r.pieces) {
        if (p.placement != Placement::ReplaceBody) continue;
        const auto e = cache.get(store, p.mesh, true, wantClips);
        if (!e->ok) { a.notes << QStringLiteral("%1 — %2; the body is shown instead").arg(p.label, e->error); break; }
        a.geo = e->geo;
        QString lookNote;
        a.materials = cache.lookedMaterials(store, p.mesh, *e, p.look.material, &lookNote);
        noteLook(a, p.label, lookNote, {});
        a.materials.resize(materialSlots(a.geo, int(a.materials.size())));
        lookFigure(a, e->skeleton, p.look, p.label);
        a.skeleton = e->skeleton;
        a.clips = e->clips;
        a.bodyGeo = e->geo;
        a.bodyParts = a.geo.parts.size();
        a.replacedBody = true;
        a.figure = p.mesh;
        a.geo.formatNote = QStringLiteral("customize: %1").arg(p.label);
        a.geo.computeBounds();
        a.ok = true;
        return a;
    }

    const auto body = cache.get(store, r.body, true, wantClips);
    if (!body->ok) { a.error = QStringLiteral("the body %1 did not load (%2)").arg(r.bodyLabel, body->error); return a; }
    a.geo = body->geo;
    {
        QString lookNote;
        a.materials = cache.lookedMaterials(store, r.body, *body, r.bodyLook.material, &lookNote);
        noteLook(a, r.bodyLabel, lookNote, {});
    }
    a.materials.resize(materialSlots(a.geo, int(a.materials.size())));
    a.geo.materialNames.resize(a.materials.size());
    lookFigure(a, body->skeleton, r.bodyLook, r.bodyLabel);
    a.skeleton = body->skeleton;
    a.clips = body->clips;
    a.bodyGeo = body->geo;
    a.bodyParts = a.geo.parts.size();
    a.figure = r.body;
    const int bodyJoints = a.skeleton.size();
    // Bone name (lower case) → index in the COMBINED rig: the body's, then every bone a piece
    // appends — so a later piece can sit on a bone an earlier one brought (a chain).
    QHash<QString, int> bone;
    for (int i = 0; i < bodyJoints; ++i) bone.insert(a.skeleton.joints[i].name.toLower(), i);

    // Row-vector convention (RigMath): world = local · parentWorld, a point is placed as v · M.
    auto bindWorld = [&](int j) { return rm::inverse(a.skeleton.joints[j].inverseBind); };
    // Append one joint of a piece's own rig under `parent` (-1 = a root) with bind WORLD `world` in
    // the figure's space. Its rest TRS is made local to that parent, so the bind pose reproduces
    // `world` exactly whatever the parent's own bind is. The name is registered only when free: a
    // sub-rig's "root" must never shadow the body's (D4 salts sub-rig bone hashes for this reason).
    auto appendJoint = [&](const Joint& src, int parent, const rm::Mat4& world) {
        Joint j = src;
        j.parent = parent;
        const rm::Mat4 local = parent >= 0 ? rm::mul(world, rm::inverse(bindWorld(parent))) : world;
        rm::decomposeTRS(local, j.restT, j.restQ.data(), j.restS);
        j.inverseBind = rm::inverse(world);
        a.skeleton.joints.append(j);
        const int idx = a.skeleton.joints.size() - 1;
        if (!bone.contains(j.name.toLower())) bone.insert(j.name.toLower(), idx);
        return idx;
    };
    auto placeVertices = [](Geometry& g, const rm::Mat4& W) {
        for (Vertex& v : g.vertices) {
            float o[3];
            rm::transformPoint(W, v.px, v.py, v.pz, o); v.px = o[0]; v.py = o[1]; v.pz = o[2];
            rm::transformDir  (W, v.nx, v.ny, v.nz, o); v.nx = o[0]; v.ny = o[1]; v.nz = o[2];
            rm::transformDir  (W, v.tx, v.ty, v.tz, o); v.tx = o[0]; v.ty = o[1]; v.tz = o[2];
        }
    };

    // InPlace first (they may bring bones a socket names), then the sockets, in passes until a pass
    // seats nothing more: a chain resolves in as many passes as it is long, and a cycle or a bone
    // no piece brings is left over — placed where it was authored, and said.
    // A piece's materials under its look, the store's word on it said once per piece.
    auto matsFor = [&](const Piece& p, const MeshCache::Entry& e) {
        QString note;
        const QVector<Material> m = cache.lookedMaterials(store, p.mesh, e, p.look.material, &note);
        noteLook(a, p.label, note, {});
        return m;
    };
    // Models a look brings (Piece::broughtBy) are skipped when that mesh is already fitted — as the
    // body, as any piece of the build's own, or brought earlier (FOX matches by path, CT.cpp:1305).
    QSet<AssetId> fitted{r.body};
    for (const Piece& p : r.pieces)
        if (p.broughtBy.isEmpty() && p.placement != Placement::ReplaceBody && (includeHeld || !p.held)) fitted.insert(p.mesh);
    QSet<const Piece*> skipBrought;
    for (const Piece& p : r.pieces) {
        if (p.broughtBy.isEmpty() || (p.held && !includeHeld)) continue;
        if (fitted.contains(p.mesh)) {
            skipBrought.insert(&p);
            a.notes << QStringLiteral("%1 — already fitted; not brought again by %2's look").arg(p.label, p.broughtBy);
        } else {
            fitted.insert(p.mesh);
        }
    }

    // What can HOST a hardpoint seat (Piece::seatOn): the figure and every piece placed so far whose
    // bones are in the combined rig — its mesh, its OWN rig (hardpoints name its joints) and where
    // each of those joints landed in the combined rig (-1 = not there).
    // `skel` is the host's rig AS AUTHORED (hardpoints name its joints; a model-space point uses its
    // inverse binds), `place` what moved the host's model space into the figure's (alignRoot's C, a
    // seat's W; identity for the figure). A growable host (InPlace) brought only the joints its
    // weights needed, so a point on another of its bones adds that bone when a seat asks for it.
    struct Host { AssetId mesh; Skeleton skel; QVector<int> map; rm::Mat4 place = rm::identity(); bool growable = false; };
    QHash<QString, Host> hosts;
    {
        Host fig{r.body, body->skeleton, {}};
        for (int j = 0; j < bodyJoints; ++j) fig.map << j;
        hosts.insert(QString(), fig);
    }

    // Every piece that is not InPlace (and the alignRoot pieces rerouted to a socket), IN LIST ORDER:
    // a hardpoint seat can only use a host placed before it — whichever kind that host is.
    QVector<const Piece*> ordered;
    std::deque<Piece> rerouted;   // alignRoot pieces whose root the figure lacks: seated by their root
    for (const Piece& p : r.pieces) {
        if (p.placement == Placement::ReplaceBody) continue;
        if (p.held && !includeHeld) continue;
        if (skipBrought.contains(&p)) continue;
        if (!p.seatOn.isEmpty() || p.placement != Placement::InPlace) { ordered << &p; continue; }
        const auto e = cache.get(store, p.mesh, true, false);
        if (!e->ok || e->geo.isEmpty()) { a.notes << QStringLiteral("%1 — %2").arg(p.label, e->ok ? QStringLiteral("empty mesh") : e->error); continue; }
        Geometry g = e->geo;
        QVector<int> hostMap;
        rm::Mat4 hostPlace = rm::identity();
        Skeleton alignedRig;
        const Skeleton* psp = &e->skeleton;
        if (p.alignRoot && !e->skeleton.isEmpty()) {
            int root = -1;
            for (int j = 0; j < e->skeleton.size() && root < 0; ++j) if (e->skeleton.joints[j].parent < 0) root = j;
            const int fj = root >= 0 ? bone.value(e->skeleton.joints[root].name.toLower(), -1) : -1;
            if (root >= 0 && fj < 0 && !p.socket.isEmpty()) {
                // Rooted at a bone the figure does not have: seat it by its root on the socket.
                Piece q = p;
                q.placement = Placement::SocketRig;
                rerouted.push_back(q);
                ordered << &rerouted.back();
                continue;
            }
            if (root >= 0 && fj >= 0 && a.skeleton.joints[fj].parent >= 0) {
                // FOX exit 'b': move the piece so its root sits where the figure's bone of that name is.
                const rm::Mat4 C = rm::mul(e->skeleton.joints[root].inverseBind, bindWorld(fj));   // v · inv(Wroot) · Wfig
                placeVertices(g, C);
                alignedRig = e->skeleton;
                for (Joint& j : alignedRig.joints) j.inverseBind = rm::inverse(rm::mul(rm::inverse(j.inverseBind), C));
                psp = &alignedRig;
                hostPlace = C;
                float d[3]; rm::translationOf(C, d);
                if (std::abs(d[0]) + std::abs(d[1]) + std::abs(d[2]) > 1e-5f)
                    a.notes << QStringLiteral("%1 — root %2 aligned to the figure's (moved %3, %4, %5)")
                                   .arg(p.label, e->skeleton.joints[root].name).arg(d[0], 0, 'f', 3).arg(d[1], 0, 'f', 3).arg(d[2], 0, 'f', 3);
            }
        }
        // After the reroute above (a rerouted piece is cut once, by seatAt), before any placement:
        // the cut reads the mesh's OWN joint indices.
        cutSubtree(a, g, e->skeleton, p.look, p.label);
        const Skeleton& ps = *psp;
        const int jointsBefore = a.skeleton.size();
        if (!ps.isEmpty() && bodyJoints > 0) {
            // Bind the piece's weighted joints to the figure's by NAME. A joint the figure lacks is
            // appended (with its missing ancestors) instead of keeping a raw index — which drove
            // whatever body bone happened to sit at that number (the old fallback).
            QSet<int> used;
            bool corrupt = false;
            for (const Vertex& v : g.vertices)
                for (int k = 0; k < 4; ++k)
                    if (v.weights[k] > 0.0f) { if (v.joints[k] >= ps.size()) corrupt = true; else used.insert(v.joints[k]); }
            if (corrupt) {
                a.notes << QStringLiteral("%1 — weights name joints past its own rig (%2 joints); raw indices kept").arg(p.label).arg(ps.size());
            } else {
                QVector<int> map(ps.size(), -1);
                QVector<bool> need(ps.size(), false);
                for (int j = 0; j < ps.size(); ++j) map[j] = bone.value(ps.joints[j].name.toLower(), -1);
                for (int u : used)
                    for (int j = u, guard = 0; j >= 0 && guard <= ps.size(); j = ps.joints[j].parent, ++guard) need[j] = true;
                QStringList added;
                for (int j = 0; j < ps.size(); ++j) {   // parents precede children (core invariant)
                    if (!need[j] || map[j] >= 0) continue;
                    const int par = ps.joints[j].parent >= 0 ? map[ps.joints[j].parent] : -1;
                    map[j] = appendJoint(ps.joints[j], par, rm::inverse(ps.joints[j].inverseBind));
                    added << ps.joints[j].name;
                }
                hostMap = map;
                int changed = 0;
                for (int u : used) if (map[u] != u) ++changed;
                if (changed > 0)
                    for (Vertex& v : g.vertices)
                        for (int k = 0; k < 4; ++k)
                            if (v.weights[k] > 0.0f) v.joints[k] = uint16_t(map[v.joints[k]]);
                if (!added.isEmpty())
                    a.notes << QStringLiteral("%1 — %2 of its bones are not in the figure's rig and were added under their own parents (%3%4)")
                                   .arg(p.label).arg(added.size()).arg(added.mid(0, 4).join(QStringLiteral(", ")))
                                   .arg(added.size() > 4 ? QStringLiteral(", …") : QString());
                else if (changed > 0)
                    a.notes << QStringLiteral("%1 — %2 of %3 joints remapped onto the body's rig by name").arg(p.label).arg(changed).arg(used.size());
            }
        } else if (g.jointPaletteSize() > bodyJoints) {
            a.notes << QStringLiteral("%1 — no rig to match by name and it uses %2 joints (the body has %3)")
                           .arg(p.label).arg(g.jointPaletteSize()).arg(bodyJoints);
        }
        // A piece with NO skinning at all (FOX's amb0_wb00_bre: no bones) on a skinned figure: every
        // weight is zero, so the skinned figure drew and wrote it collapsed onto the origin (run 22's
        // .glb). Held by the figure's root instead, it sits where it was authored — FOX draws such a
        // part rigid, as authored (its scene export, same run).
        if (bodyJoints > 0) {
            bool anyWeight = false;
            for (const Vertex& v : g.vertices) if (v.weights[0] + v.weights[1] + v.weights[2] + v.weights[3] > 0.0f) { anyWeight = true; break; }
            if (!anyWeight && !g.vertices.isEmpty()) {
                for (Vertex& v : g.vertices) {
                    v.joints[0] = 0; v.joints[1] = v.joints[2] = v.joints[3] = 0;
                    v.weights[0] = 1.0f; v.weights[1] = v.weights[2] = v.weights[3] = 0.0f;
                }
                a.notes << QStringLiteral("%1 — has no skinning; held by the figure's root, as authored").arg(p.label);
            }
        }
        append(a, g, matsFor(p, *e), p);
        a.pieces.last().firstJoint = jointsBefore < a.skeleton.size() ? jointsBefore : -1;
        a.pieces.last().jointCount = a.skeleton.size() - jointsBefore;
        if (!hostMap.isEmpty()) hosts.insert(p.label, Host{p.mesh, e->skeleton, hostMap, hostPlace, true});
    }

    // Seat `p` with its socket at W (figure space) under joint `bi` (-1: placed where it is, its own
    // bones as new roots). Returns false when the mesh did not load (said).
    auto seatAt = [&](const Piece& p, rm::Mat4 W, int bi) -> bool {
        const bool keepRig = p.placement != Placement::Socket;
        const auto e = cache.get(store, p.mesh, true, false);
        if (!e->ok || e->geo.isEmpty()) { a.notes << QStringLiteral("%1 — %2").arg(p.label, e->ok ? QStringLiteral("empty mesh") : e->error); return false; }
        Geometry g = e->geo;
        cutSubtree(a, g, e->skeleton, p.look, p.label);
        const int jointsBefore = a.skeleton.size();
        const Skeleton& ps = e->skeleton;
        QVector<int> map(ps.size(), -1);
        if (p.alignRoot && !ps.isEmpty()) {
            // Seat the piece's ROOT on the socket, not its origin (FOX exits 'c' / 'a').
            for (int j = 0; j < ps.size(); ++j)
                if (ps.joints[j].parent < 0) { W = rm::mul(ps.joints[j].inverseBind, W); break; }
        }
        placeVertices(g, W);
        if (keepRig && !ps.isEmpty()) {
            // Its own bones, under the socket bone, placed where the socket puts them; its weights
            // stay on them. A clip that drives the figure carries them rigidly; one that names them
            // animates them.
            for (int j = 0; j < ps.size(); ++j) {
                const int par = ps.joints[j].parent >= 0 ? map[ps.joints[j].parent] : bi;
                map[j] = appendJoint(ps.joints[j], par, rm::mul(rm::inverse(ps.joints[j].inverseBind), W));
            }
            for (Vertex& v : g.vertices)
                for (int k = 0; k < 4; ++k)
                    if (v.weights[k] > 0.0f && v.joints[k] < map.size()) v.joints[k] = uint16_t(map[v.joints[k]]);
                    else if (v.weights[k] > 0.0f) { v.joints[k] = uint16_t(qMax(bi, 0)); }
        } else {
            for (Vertex& v : g.vertices) {
                v.joints[0] = uint16_t(qMax(bi, 0)); v.joints[1] = v.joints[2] = v.joints[3] = 0;
                v.weights[0] = a.skeleton.isEmpty() ? 0.0f : 1.0f; v.weights[1] = v.weights[2] = v.weights[3] = 0.0f;   // rigid
            }
            if (keepRig && p.placement == Placement::SocketRig) a.notes << QStringLiteral("%1 — no rig of its own to keep; seated rigidly").arg(p.label);
        }
        append(a, g, matsFor(p, *e), p);
        a.pieces.last().firstJoint = jointsBefore < a.skeleton.size() ? jointsBefore : -1;
        a.pieces.last().jointCount = a.skeleton.size() - jointsBefore;
        if (keepRig && !ps.isEmpty()) hosts.insert(p.label, Host{p.mesh, ps, map, W, false});
        return true;
    };
    auto seat = [&](const Piece& p, int bi) {
        seatAt(p, rm::mul(p.socketOffset, bindWorld(bi)), bi);   // the socket's bind, in the figure's space
    };

    // Hardpoint seats (Piece::seatOn), in list order, once — FOX's rows. A point serves one piece.
    QSet<QString> usedPoints;
    auto seatOnHardpoint = [&](const Piece& p) {
        bool done = false;
        for (const SocketOption& opt : p.seatOn) {
            if (opt.host == p.label) continue;
            const auto h = hosts.constFind(opt.host);
            if (h == hosts.constEnd()) continue;                        // that host is not fitted
            const QString key = opt.host + QLatin1Char('/') + opt.hardpoint;
            if (usedPoints.contains(key)) continue;                     // another piece has it
            const QVector<Hardpoint> hps = store.hardpoints(h->mesh, h->skel);
            const Hardpoint* hp = nullptr;
            for (const Hardpoint& x : hps) if (x.name == opt.hardpoint) { hp = &x; break; }
            if (!hp) continue;                                          // this host does not carry it
            int cj = hp->joint >= 0 && hp->joint < h->map.size() ? h->map[hp->joint] : -1;
            if (cj < 0 && h->growable && hp->joint >= 0 && hp->joint < h->skel.size()) {
                // An InPlace host brought only its weighted bones; this point hangs from another.
                // Add it, with any missing ancestors, where the host put them (its bind × place).
                Host& hm = hosts[opt.host];
                QVector<int> chain;
                for (int j = hp->joint, guard = 0; j >= 0 && hm.map[j] < 0 && guard <= hm.skel.size(); j = hm.skel.joints[j].parent, ++guard)
                    chain.prepend(j);
                for (int j : chain) {
                    const int par = hm.skel.joints[j].parent >= 0 ? hm.map[hm.skel.joints[j].parent] : -1;
                    hm.map[j] = appendJoint(hm.skel.joints[j], par, rm::mul(rm::inverse(hm.skel.joints[j].inverseBind), hm.place));
                }
                cj = hm.map[hp->joint];
                a.notes << QStringLiteral("%1 — %2 of %3 added to the rig for %4's seat")
                               .arg(opt.host, hm.skel.joints[hp->joint].name, opt.host, p.label);
            }
            if (cj < 0) {
                a.notes << QStringLiteral("%1 — %2 on %3 hangs from a bone that is not in the rig; tried the next option")
                               .arg(p.label, opt.hardpoint, opt.host.isEmpty() ? r.bodyLabel : opt.host);
                continue;
            }
            // Bone-local: local · bone; model-space (D4): local · inverseBind · bone (GLModelWidget's rule).
            const rm::Mat4 local = rm::composeTRS(hp->t, hp->q.data(), hp->s);
            const Host& hc = hosts[opt.host];   // (the grow above may have rehashed; re-read)
            const rm::Mat4 Wh = hp->modelSpace ? rm::mul(rm::mul(local, hc.skel.joints[hp->joint].inverseBind), bindWorld(cj))
                                               : rm::mul(local, bindWorld(cj));
            if (!seatAt(p, rm::mul(p.socketOffset, Wh), cj)) { done = true; break; }
            usedPoints.insert(key);
            a.pieces.last().seatNote = opt.host.isEmpty() ? QStringLiteral("seated at %1").arg(opt.hardpoint)
                                                          : QStringLiteral("seated at %1 on the %2").arg(opt.hardpoint, opt.host);
            done = true;
            break;
        }
        if (done) return;
        // Nothing seated it: where it was authored. Seated when it has no hardpoints of its own.
        const auto e = cache.get(store, p.mesh, true, false);
        const bool own = e->ok && !store.hardpoints(p.mesh, e->skeleton).isEmpty();
        if (!seatAt(p, rm::identity(), -1)) return;
        a.pieces.last().seated = !own;
        a.pieces.last().seatNote = own
            ? QStringLiteral("fitted, but nothing fitted offers a connect point for it — placed where it was authored")
            : QStringLiteral("in the figure's own space — it carries no connect points of its own, so it needs no placing");
        if (own) a.notes << QStringLiteral("%1 — %2").arg(p.label, a.pieces.last().seatNote);
    };
    // List order: a hardpoint seat now, a bone socket now if its bone is there — else it waits for
    // the passes below (a chain onto bones a later piece brings resolves there, as before).
    QVector<const Piece*> sockets;
    for (const Piece* pp : ordered) {
        if (!pp->seatOn.isEmpty()) { seatOnHardpoint(*pp); continue; }
        const int bi = bone.value(pp->socket.toLower(), -1);
        if (bi >= 0) seat(*pp, bi); else sockets << pp;
    }
    for (bool progress = true; progress && !sockets.isEmpty();) {
        progress = false;
        for (int i = 0; i < sockets.size();) {
            const int bi = bone.value(sockets[i]->socket.toLower(), -1);
            if (bi < 0) { ++i; continue; }
            seat(*sockets[i], bi);
            sockets.removeAt(i);
            progress = true;
        }
    }
    for (const Piece* pp : sockets) {
        const Piece& p = *pp;
        const auto e = cache.get(store, p.mesh, true, false);   // the rig: keepSubtree cuts by it
        if (!e->ok || e->geo.isEmpty()) { a.notes << QStringLiteral("%1 — %2").arg(p.label, e->ok ? QStringLiteral("empty mesh") : e->error); continue; }
        // As POE2 did: placed where it was authored, and said — with the rig's nearest names, so a
        // naming mismatch (a suffix, a prefix, a socket name) is visible in the log, not guessed.
        QStringList near;
        for (const Joint& j : a.skeleton.joints)
            if (near.size() < 4 && !j.name.isEmpty() && (j.name.contains(p.socket, Qt::CaseInsensitive) || p.socket.contains(j.name, Qt::CaseInsensitive)
                                    || (p.socket.size() >= 4 && j.name.contains(p.socket.left(4), Qt::CaseInsensitive))))
                near << j.name;
        a.notes << QStringLiteral("%1 — bone %2 not in the rig (%3 joints%4); placed unbaked")
                       .arg(p.label, p.socket).arg(a.skeleton.size())
                       .arg(near.isEmpty() ? QString() : QStringLiteral("; nearest: ") + near.join(QStringLiteral(", ")));
        // On the figure's first (root) joint: its weights named ITS rig's joints, which mean nothing
        // in the figure's. Not zero weights — a glTF importer can collapse an all-zero vertex.
        Geometry g = e->geo;
        cutSubtree(a, g, e->skeleton, p.look, p.label);
        for (Vertex& v : g.vertices) {
            v.joints[0] = v.joints[1] = v.joints[2] = v.joints[3] = 0;
            v.weights[0] = a.skeleton.isEmpty() ? 0.0f : 1.0f; v.weights[1] = v.weights[2] = v.weights[3] = 0.0f;
        }
        append(a, g, matsFor(p, *e), p);
        a.pieces.last().seated = false;
    }
    // Vertex joint indices are 16-bit: a combined rig past that would bind vertices to the wrong
    // bones with nothing said (audit 2026-09-25; no shipped rig comes near it).
    if (a.skeleton.size() > 65536)
        a.notes << QStringLiteral("the combined rig has %1 joints — past 65,536 vertices cannot address them; the figure will skin wrongly").arg(a.skeleton.size());
    a.geo.skinned = bodyJoints > 0;
    a.geo.sourcePath = body->geo.sourcePath;
    a.geo.formatNote = QStringLiteral("customize: %1").arg(r.bodyLabel);
    a.geo.computeBounds();
    a.ok = true;
    return a;
}

bool keepBoneSubtree(Geometry& g, const Skeleton& sk, const QString& bone)
{
    int root = -1;
    for (int j = 0; j < sk.size() && root < 0; ++j) if (sk.joints[j].name.compare(bone, Qt::CaseInsensitive) == 0) root = j;
    if (root < 0) return false;
    QVector<bool> in(sk.size(), false);
    for (int j = 0; j < sk.size(); ++j)
        for (int k = j, guard = 0; k >= 0 && guard <= sk.size(); k = sk.joints[k].parent, ++guard)
            if (k == root) { in[j] = true; break; }
    // The heaviest bone of each vertex; FOX's rule takes the FIRST of equal weights (strict >).
    // A vertex with no weight at all is unskinned.
    auto heaviest = [&](const Vertex& v) {
        int best = -1; float bw = 0.0f;
        for (int k = 0; k < 4; ++k) if (v.weights[k] > bw) { bw = v.weights[k]; best = v.joints[k]; }
        return best;
    };
    QVector<uint32_t> out;
    out.reserve(g.indices.size());
    for (Part& part : g.parts) {
        const uint32_t start = uint32_t(out.size());
        bool skinned = false;
        for (uint32_t k = 0; k < part.indexCount && !skinned; ++k) skinned = heaviest(g.vertices[g.indices[part.indexStart + k]]) >= 0;
        if (skinned) {
            for (uint32_t t = 0; t + 2 < part.indexCount; t += 3) {
                bool keep = false;
                for (int c = 0; c < 3 && !keep; ++c) {
                    const int b = heaviest(g.vertices[g.indices[part.indexStart + t + c]]);
                    keep = b >= 0 && b < in.size() && in[b];
                }
                if (keep) for (int c = 0; c < 3; ++c) out.append(g.indices[part.indexStart + t + c]);
            }
        }   // unskinned: the body by definition (a head is always skinned) — it goes entirely
        part.indexStart = start;
        part.indexCount = uint32_t(out.size()) - start;
    }
    g.indices = out;
    return true;
}

void applyLook(IGameStore& store, const AssetId& id, Geometry& geo, const Skeleton& skeleton, QVector<Material>& materials,
               const Look& look, bool decodeImages, const QString& label, QStringList* notes)
{
    if (look.isEmpty()) return;
    Assembly tmp;
    cutSubtree(tmp, geo, skeleton, look, label);
    QStringList unmatched;
    const QVector<bool> vis = lookedVisibility(geo, look, &unmatched);
    for (int i = 0; i < geo.parts.size(); ++i) geo.parts[i].visible = vis[i];
    for (PartGroup& g : geo.groups) g.visible = true;
    QString storeNote;
    if (!look.material.isEmpty()) materials = store.resolveLookedMaterials(id, geo, decodeImages, look.material, &storeNote);
    if (notes) {
        noteLook(tmp, label, storeNote, unmatched);
        *notes += tmp.notes;
    }
}

QVector<bool> partsShown(const Assembly& a, const QSet<int>& hiddenBodyParts, const QHash<QString, bool>& pieceShown)
{
    QVector<bool> on(a.geo.parts.size());
    for (int i = 0; i < on.size(); ++i) on[i] = a.geo.parts[i].visible;
    if (!a.replacedBody)
        for (int i = 0; i < a.bodyParts && i < on.size(); ++i) if (hiddenBodyParts.contains(i)) on[i] = false;
    for (const PieceRange& r : a.pieces)
        if (!pieceShown.value(r.label, true))
            for (int i = r.firstPart; i < r.firstPart + r.partCount && i < on.size(); ++i) on[i] = false;
    return on;
}

}  // namespace core::customize
