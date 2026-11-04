#include "core/export/GlbExporter.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace core::glb {
namespace {

// glTF constants, spelled once.
constexpr int kFloat = 5126, kUByte = 5121, kUShort = 5123, kUInt = 5125;
constexpr int kArrayBuffer = 34962, kElementArrayBuffer = 34963;
constexpr int kTriangles = 4;

// A growable little-endian binary buffer for the BIN chunk. Every accessor records its byte offset;
// the buffer is 4-byte aligned between views, as glTF requires.
struct Bin {
    QByteArray data;
    int append(const void* p, int n)
    {
        const int off = data.size();
        data.append(reinterpret_cast<const char*>(p), n);
        return off;
    }
    void align4() { while (data.size() & 3) data.append('\0'); }
};

QJsonObject accessorObj(int bufferView, int compType, int count, const char* type,
                        const QVector<double>& mn = {}, const QVector<double>& mx = {},
                        int byteOffset = 0, bool normalized = false)
{
    QJsonObject a;
    a[QStringLiteral("bufferView")] = bufferView;
    if (byteOffset) a[QStringLiteral("byteOffset")] = byteOffset;
    a[QStringLiteral("componentType")] = compType;
    a[QStringLiteral("count")] = count;
    a[QStringLiteral("type")] = QLatin1String(type);
    if (normalized) a[QStringLiteral("normalized")] = true;
    if (!mn.isEmpty()) { QJsonArray j; for (double v : mn) j.append(v); a[QStringLiteral("min")] = j; }
    if (!mx.isEmpty()) { QJsonArray j; for (double v : mx) j.append(v); a[QStringLiteral("max")] = j; }
    return a;
}

// Every accessor goes through here so a bufferView is never written without its accessor, and the
// 4-byte alignment happens in exactly one place.
struct Writer {
    Bin bin;
    QJsonArray bufferViews, accessors;

    int view(const void* p, int bytes, int target)
    {
        bin.align4();
        const int off = bin.append(p, bytes);
        QJsonObject v{{QStringLiteral("buffer"), 0},
                      {QStringLiteral("byteOffset"), off},
                      {QStringLiteral("byteLength"), bytes}};
        if (target) v[QStringLiteral("target")] = target;
        bufferViews.append(v);
        return bufferViews.size() - 1;
    }
    int acc(const QJsonObject& a) { accessors.append(a); return accessors.size() - 1; }

    int floats(const QVector<float>& f, int comps, const char* type, int target = kArrayBuffer,
               const QVector<double>& mn = {}, const QVector<double>& mx = {})
    {
        const int bv = view(f.constData(), f.size() * 4, target);
        return acc(accessorObj(bv, kFloat, f.size() / comps, type, mn, mx));
    }
};

// ── Textures ───────────────────────────────────────────────────────────────────────────────────
// The two source-convention fixes happen HERE, once, instead of under three different names in
// three projects (D4 flipNormalGreen, FOX normalsGreenDown, POE2 reconstructNormalZ).
QImage prepareNormal(const QImage& src, bool flipGreen, bool rebuildZ)
{
    if (src.isNull() || (!flipGreen && !rebuildZ)) return src;
    QImage img = src.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < img.height(); ++y) {
        uchar* row = img.scanLine(y);
        for (int x = 0; x < img.width(); ++x) {
            uchar* px = row + x * 4;
            if (flipGreen) px[1] = uchar(255 - px[1]);
            if (rebuildZ) {
                const float nx = px[0] / 127.5f - 1.0f, ny = px[1] / 127.5f - 1.0f;
                const float zz = 1.0f - nx * nx - ny * ny;
                const float nz = zz > 0.0f ? std::sqrt(zz) : 0.0f;
                px[2] = uchar(std::lround(std::fmin(255.0f, std::fmax(0.0f, (nz + 1.0f) * 127.5f))));
            }
        }
    }
    return img;
}

struct Encoded { QByteArray bytes; const char* mime = "image/png"; const char* ext = "png"; };

// FOX's two size controls. JPEG is offered only where FOX offers it: an opaque map the plugin has
// marked lossy-safe. A normal or ORM map through JPEG is how a rig gets banding artefacts that look
// like a decode bug.
Encoded encodeImage(const QImage& src, bool lossyOk, const ExportOptions& opt)
{
    QImage img = src;
    if (opt.maxTextureSize > 0 && (img.width() > opt.maxTextureSize || img.height() > opt.maxTextureSize))
        img = img.scaled(opt.maxTextureSize, opt.maxTextureSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    Encoded e;
    QBuffer buf(&e.bytes);
    buf.open(QIODevice::WriteOnly);
    if (opt.jpegQuality > 0 && lossyOk && !img.hasAlphaChannel()) {
        img.save(&buf, "JPEG", opt.jpegQuality);
        e.mime = "image/jpeg";
        e.ext = "jpg";
    } else {
        img.save(&buf, "PNG");
    }
    return e;
}

QString sanitize(QString s)
{
    static const QRegularExpression bad(QStringLiteral("[^A-Za-z0-9_.-]"));
    s.replace(bad, QStringLiteral("_"));
    return s;
}

// glTF quaternion (xyzw) for `deg` degrees about `axis`.
rm::Quat axisQuat(int axis, float deg)
{
    const float h = float(deg) * 0.5f * 3.14159265358979323846f / 180.0f;
    rm::Quat q{0, 0, 0, std::cos(h)};
    q[axis] = std::sin(h);
    return q;
}

}  // namespace

QString Result::summary() const
{
    if (!ok) return QStringLiteral("export failed: %1").arg(error);
    QStringList p;
    p << QStringLiteral("%1 primitive(s)").arg(primitives);
    p << QStringLiteral("%1 material(s)").arg(materialsWritten);
    if (materialsPruned) p << QStringLiteral("%1 unused pruned").arg(materialsPruned);
    if (imagesWritten)   p << QStringLiteral("%1 image(s)").arg(imagesWritten);
    if (imagesDeduped)   p << QStringLiteral("%1 shared").arg(imagesDeduped);
    if (animationsWritten)
        p << QStringLiteral("%1 clip(s), %2 track(s)").arg(animationsWritten).arg(tracksWritten);
    if (tracksElided)      p << QStringLiteral("%1 constant track(s) dropped").arg(tracksElided);
    if (hardpointsWritten) p << QStringLiteral("%1 hardpoint(s)").arg(hardpointsWritten);
    if (badInfluences)     p << QStringLiteral("%1 OUT-OF-RANGE influence(s) zeroed").arg(badInfluences);
    return p.join(QStringLiteral(" · "));
}

QByteArray build(const Scene& scene, const ExportOptions& opt, Result* result,
                 QVector<LooseImage>* looseOut)
{
    Result r;
    const auto fail = [&](const QString& why) -> QByteArray {
        r.ok = false;
        r.error = why;
        if (result) *result = r;
        return QByteArray();
    };

    const Geometry* geo = scene.geometry;
    const Skeleton* skel = scene.skeleton;
    const bool haveSkel = opt.includeSkeleton && skel && !skel->isEmpty();
    const bool wantMesh = !opt.animationLibrary;

    if (opt.animationLibrary) {
        if (!haveSkel) return fail(QStringLiteral("an animation library needs a skeleton"));
    } else if (!geo || geo->isEmpty()) {
        return fail(QStringLiteral("nothing to export (no geometry)"));
    }

    const bool skinned = wantMesh && haveSkel && geo && geo->skinned;

    // unitScale is either baked into the data or carried by a wrapper node (FOX's choice). Exactly
    // one of the two, so `s` below is the only place the distinction exists.
    const bool bakeScale = opt.scaleMode == ExportOptions::BakeIntoData;
    const float s = bakeScale ? opt.unitScale : 1.0f;

    Writer w;
    QJsonArray meshPrims, jMaterials, jTextures, jImages, jSamplers;

    int posAcc = -1, nrmAcc = -1, tanAcc = -1, uvAcc = -1, uv1Acc = -1, colAcc = -1;
    int jointsAcc = -1, weightsAcc = -1;

    if (wantMesh) {
        const int nv = geo->vertices.size();

        // POSITION — the one accessor glTF requires min/max on.
        {
            QVector<float> pos; pos.reserve(nv * 3);
            double mn[3] = {1e30, 1e30, 1e30}, mx[3] = {-1e30, -1e30, -1e30};
            for (const Vertex& v : geo->vertices) {
                const float o[3] = {v.px * s, v.py * s, v.pz * s};
                for (int i = 0; i < 3; ++i) {
                    pos.append(o[i]);
                    mn[i] = std::min(mn[i], double(o[i]));
                    mx[i] = std::max(mx[i], double(o[i]));
                }
            }
            posAcc = w.floats(pos, 3, "VEC3", kArrayBuffer, {mn[0], mn[1], mn[2]}, {mx[0], mx[1], mx[2]});
        }

        // NORMAL — re-normalised, with a defined fallback. A zero normal is not rare in game data
        // (degenerate triangles, unwelded seams) and glTF requires unit normals.
        {
            QVector<float> nrm; nrm.reserve(nv * 3);
            for (const Vertex& v : geo->vertices) {
                float o[3] = {v.nx, v.ny, v.nz};
                float len = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
                if (len < 1e-6f) { o[0] = 0; o[1] = 0; o[2] = 1; len = 1; }
                nrm.append(o[0] / len); nrm.append(o[1] / len); nrm.append(o[2] / len);
            }
            nrmAcc = w.floats(nrm, 3, "VEC3");
        }

        // TANGENT (VEC4, w = handedness) — only when the mesh actually had one. Inventing a tangent
        // is worse than omitting it: an importer can derive one from the UVs, but it cannot tell
        // that the one we wrote is a placeholder.
        if (geo->hasTangent && opt.writeTangents) {
            QVector<float> tan; tan.reserve(nv * 4);
            for (const Vertex& v : geo->vertices) {
                float o[3] = {v.tx, v.ty, v.tz};
                float len = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
                if (len < 1e-6f) { o[0] = 1; o[1] = 0; o[2] = 0; len = 1; }
                tan.append(o[0] / len); tan.append(o[1] / len); tan.append(o[2] / len);
                tan.append(v.tw >= 0 ? 1.0f : -1.0f);
            }
            tanAcc = w.floats(tan, 4, "VEC4");
        }

        {
            QVector<float> uv; uv.reserve(nv * 2);
            for (const Vertex& v : geo->vertices) { uv.append(v.u); uv.append(v.v); }
            uvAcc = w.floats(uv, 2, "VEC2");
        }
        if (geo->hasUv1) {
            QVector<float> uv; uv.reserve(nv * 2);
            for (const Vertex& v : geo->vertices) { uv.append(v.u2); uv.append(v.v2); }
            uv1Acc = w.floats(uv, 2, "VEC2");
        }
        // See ExportOptions::writeVertexColor for why this is opt-in.
        if (geo->hasColor && opt.writeVertexColor) {
            QVector<uint8_t> c; c.reserve(nv * 4);
            for (const Vertex& v : geo->vertices) for (int k = 0; k < 4; ++k) c.append(v.color[k]);
            const int bv = w.view(c.constData(), c.size(), kArrayBuffer);
            colAcc = w.acc(accessorObj(bv, kUByte, nv, "VEC4", {}, {}, 0, /*normalized*/ true));
        }

        if (skinned) {
            const int nj = skel->size();
            QVector<uint16_t> joints; joints.reserve(nv * 4);
            QVector<float> weights; weights.reserve(nv * 4);
            for (const Vertex& v : geo->vertices) {
                float wk[4];
                for (int k = 0; k < 4; ++k) {
                    // Scar 7: an out-of-range index cannot be honoured, so its WEIGHT goes too.
                    // Clamping the index alone drags the vertex onto the root bone.
                    const bool bad = int(v.joints[k]) >= nj;
                    if (bad && v.weights[k] > 0.0f) ++r.badInfluences;
                    joints.append(bad ? 0 : v.joints[k]);
                    wk[k] = bad ? 0.0f : v.weights[k];
                }
                const float sum = wk[0] + wk[1] + wk[2] + wk[3];
                const float inv = sum > 0 ? 1.0f / sum : 0.0f;
                for (int k = 0; k < 4; ++k) weights.append(wk[k] * inv);
            }
            const int jv = w.view(joints.constData(), joints.size() * 2, kArrayBuffer);
            jointsAcc = w.acc(accessorObj(jv, kUShort, nv, "VEC4"));
            weightsAcc = w.floats(weights, 4, "VEC4");
        }
    }

    // ── Primitives ─────────────────────────────────────────────────────────────────────────────
    QVector<bool> matUsed(scene.materials.size(), false);
    QVector<int> primMatIdx;
    if (wantMesh) {
        const int iv = w.view(geo->indices.constData(), geo->indices.size() * 4, kElementArrayBuffer);
        for (const Part& part : geo->parts) {
            if (!part.visible || part.indexCount == 0) continue;
            // A part inside a hidden group is hidden. Groups are a tree (FOX authors one), and
            // visibility is inherited, so walk to the root rather than testing the leaf.
            bool shown = true;
            for (int g = part.group; g >= 0 && g < geo->groups.size(); g = geo->groups[g].parent) {
                if (!geo->groups[g].visible) { shown = false; break; }
            }
            if (!shown) continue;

            const int acc = w.acc(accessorObj(iv, kUInt, int(part.indexCount), "SCALAR", {}, {},
                                              int(part.indexStart) * 4));
            QJsonObject attrs{{QStringLiteral("POSITION"), posAcc},
                              {QStringLiteral("NORMAL"), nrmAcc},
                              {QStringLiteral("TEXCOORD_0"), uvAcc}};
            if (tanAcc >= 0) attrs[QStringLiteral("TANGENT")] = tanAcc;
            if (uv1Acc >= 0) attrs[QStringLiteral("TEXCOORD_1")] = uv1Acc;
            if (colAcc >= 0) attrs[QStringLiteral("COLOR_0")] = colAcc;
            if (skinned) {
                attrs[QStringLiteral("JOINTS_0")] = jointsAcc;
                attrs[QStringLiteral("WEIGHTS_0")] = weightsAcc;
            }
            QJsonObject prim;
            prim[QStringLiteral("attributes")] = attrs;
            prim[QStringLiteral("indices")] = acc;
            prim[QStringLiteral("mode")] = kTriangles;
            const bool hasMat = part.materialIndex >= 0 && part.materialIndex < scene.materials.size();
            if (hasMat) matUsed[part.materialIndex] = true;
            primMatIdx.append(hasMat ? part.materialIndex : -1);
            meshPrims.append(prim);
        }
        // "Everything hidden ⇒ nothing to write" — and say so, rather than emitting an empty mesh
        // that imports as a nameless empty and reads as a decode failure.
        if (meshPrims.isEmpty()) return fail(QStringLiteral("nothing visible to export"));
    }
    r.primitives = meshPrims.size();

    // ── Materials + images ─────────────────────────────────────────────────────────────────────
    const bool loose = opt.looseTextures && looseOut != nullptr;
    const bool writeTex = opt.embedTextures || loose;
    QSet<QString> extUsed;
    QVector<int> matRemap(scene.materials.size(), -1);
    QHash<QByteArray, int> imageByHash;   // content hash → texture index (dedupe)

    for (int mi = 0; mi < scene.materials.size(); ++mi) {
        if (opt.pruneUnusedMaterials && !matUsed[mi]) { ++r.materialsPruned; continue; }
        matRemap[mi] = jMaterials.size();
        const Material& m = scene.materials[mi];
        const QString mname = m.name.isEmpty() ? QStringLiteral("material_%1").arg(mi) : m.name;

        QJsonObject mat{{QStringLiteral("name"), mname}};
        QJsonObject pbr, matExt;

        auto addTexture = [&](const QImage& img, bool lossyOk, const char* role) -> int {
            if (img.isNull()) return -1;
            const Encoded enc = encodeImage(img, lossyOk, opt);
            if (opt.dedupeImages) {
                const QByteArray key = QCryptographicHash::hash(enc.bytes, QCryptographicHash::Md5);
                const auto hit = imageByHash.constFind(key);
                if (hit != imageByHash.constEnd()) { ++r.imagesDeduped; return hit.value(); }
                // Inserted below, once the texture index is known.
                const int tx = jTextures.size();
                imageByHash.insert(key, tx);
            }
            const int im = jImages.size();
            if (loose) {
                const QString fname = QStringLiteral("%1_%2_%3.%4")
                                          .arg(sanitize(mname)).arg(mi)
                                          .arg(QLatin1String(role), QLatin1String(enc.ext));
                looseOut->append({fname, enc.bytes});
                jImages.append(QJsonObject{{QStringLiteral("uri"), fname}});
            } else {
                const int bv = w.view(enc.bytes.constData(), enc.bytes.size(), 0);
                jImages.append(QJsonObject{{QStringLiteral("bufferView"), bv},
                                           {QStringLiteral("mimeType"), QLatin1String(enc.mime)}});
            }
            ++r.imagesWritten;
            jTextures.append(QJsonObject{{QStringLiteral("source"), im}, {QStringLiteral("sampler"), 0}});
            return jTextures.size() - 1;
        };

        if (writeTex) {
            const int base = addTexture(m.baseColor.image, m.baseColor.lossyOk, "base");
            if (base >= 0) pbr[QStringLiteral("baseColorTexture")] = QJsonObject{{QStringLiteral("index"), base}};

            const QImage nrm = prepareNormal(m.normal.image,
                                             opt.flipNormalGreen && m.normalGreenDown,
                                             opt.reconstructNormalZ && m.normalNeedsZ);
            const int nt = addTexture(nrm, false, "normal");
            if (nt >= 0) mat[QStringLiteral("normalTexture")] = QJsonObject{{QStringLiteral("index"), nt}};

            const int mr = addTexture(m.orm.image, false, "orm");
            if (mr >= 0) {
                pbr[QStringLiteral("metallicRoughnessTexture")] = QJsonObject{{QStringLiteral("index"), mr}};
                // glTF's default occlusion channel is R, which is where our ORM packing puts it —
                // but only a source that actually HAS an AO channel gets an occlusionTexture.
                // FOX's SRM has none, and writing one there would invent data.
                if (m.ormHasOcclusion) {
                    QJsonObject occ{{QStringLiteral("index"), mr}};
                    if (m.occlusionStrength < 1.0f) occ[QStringLiteral("strength")] = double(m.occlusionStrength);
                    mat[QStringLiteral("occlusionTexture")] = occ;
                }
            }
            const int em = addTexture(m.emissive.image, m.emissive.lossyOk, "emissive");
            if (em >= 0) mat[QStringLiteral("emissiveTexture")] = QJsonObject{{QStringLiteral("index"), em}};
        }

        pbr[QStringLiteral("metallicFactor")] = double(m.metalFactor);
        pbr[QStringLiteral("roughnessFactor")] = double(m.roughFactor);
        mat[QStringLiteral("pbrMetallicRoughness")] = pbr;
        if (m.doubleSided) mat[QStringLiteral("doubleSided")] = true;

        if (!m.emissive.isNull()) {
            mat[QStringLiteral("emissiveFactor")] = QJsonArray{double(m.emissiveFactor[0]),
                                                               double(m.emissiveFactor[1]),
                                                               double(m.emissiveFactor[2])};
            if (m.emissiveStrength > 1.0f) {
                matExt[QStringLiteral("KHR_materials_emissive_strength")] =
                    QJsonObject{{QStringLiteral("emissiveStrength"), double(m.emissiveStrength)}};
                extUsed.insert(QStringLiteral("KHR_materials_emissive_strength"));
            }
        }
        if (m.dielectricSpec) {
            QJsonObject spec;
            if (writeTex && !m.specularColor.isNull()) {
                const int sc = addTexture(m.specularColor.image, false, "spec");
                if (sc >= 0) spec[QStringLiteral("specularColorTexture")] = QJsonObject{{QStringLiteral("index"), sc}};
            }
            matExt[QStringLiteral("KHR_materials_specular")] = spec;   // defaults (factor 1) with no map
            extUsed.insert(QStringLiteral("KHR_materials_specular"));
        }
        if (m.transmission > 0.0f) {
            matExt[QStringLiteral("KHR_materials_transmission")] =
                QJsonObject{{QStringLiteral("transmissionFactor"), double(m.transmission)}};
            extUsed.insert(QStringLiteral("KHR_materials_transmission"));
        }
        // KHR_materials_ior. Written only when the plugin sets one; without it an importer uses the glTF
        // default (1.5). A source that authors a different dielectric index (BF6 = 1.0, i.e. no Fresnel
        // specular on the matte gear) says so here.
        if (m.ior > 0.0f) {
            matExt[QStringLiteral("KHR_materials_ior")] = QJsonObject{{QStringLiteral("ior"), double(m.ior)}};
            extUsed.insert(QStringLiteral("KHR_materials_ior"));
        }
        if (!matExt.isEmpty()) mat[QStringLiteral("extensions")] = matExt;

        // glTF has no additive mode; BLEND is the closest portable representation, which is why
        // core keeps Additive as its own value and loses it only here, at the boundary.
        if (m.alphaMode == Material::Mask) {
            mat[QStringLiteral("alphaMode")] = QStringLiteral("MASK");
            mat[QStringLiteral("alphaCutoff")] = double(m.alphaCutoff);
        } else if (m.alphaMode == Material::Blend || m.alphaMode == Material::Additive) {
            mat[QStringLiteral("alphaMode")] = QStringLiteral("BLEND");
        }
        jMaterials.append(mat);
    }
    r.materialsWritten = jMaterials.size();

    // Scar 6: the roster was pruned, so every primitive's material index is remapped.
    for (int pi = 0; pi < meshPrims.size(); ++pi) {
        const int mi = primMatIdx.at(pi);
        if (mi < 0 || matRemap.at(mi) < 0) continue;
        QJsonObject prim = meshPrims.at(pi).toObject();
        prim[QStringLiteral("material")] = matRemap.at(mi);
        meshPrims[pi] = prim;
    }
    if (!jTextures.isEmpty())
        jSamplers.append(QJsonObject{{QStringLiteral("magFilter"), 9729},
                                     {QStringLiteral("minFilter"), 9987},
                                     {QStringLiteral("wrapS"), 10497},
                                     {QStringLiteral("wrapT"), 10497}});

    // ── Nodes ──────────────────────────────────────────────────────────────────────────────────
    QJsonArray nodes, sceneNodes;
    int meshNodeIndex = -1;
    if (wantMesh) {
        QJsonObject mn{{QStringLiteral("mesh"), 0}};
        if (!geo->sourcePath.isEmpty())
            mn[QStringLiteral("name")] = QFileInfo(geo->sourcePath).completeBaseName();
        meshNodeIndex = nodes.size();
        nodes.append(mn);
        sceneNodes.append(meshNodeIndex);
    }

    QJsonArray jSkins, jAnimations;
    const int boneBase = nodes.size();
    if (haveSkel) {
        const int nj = skel->size();
        // Scar 2: TRS, never `matrix`. core::Joint carries the bind-LOCAL TRS directly, so there is
        // no decompose here and no chance of losing a non-uniform scale to one.
        for (const Joint& j : skel->joints) {
            QJsonObject n{{QStringLiteral("name"), j.name}};
            n[QStringLiteral("translation")] = QJsonArray{double(j.restT[0] * s), double(j.restT[1] * s), double(j.restT[2] * s)};
            n[QStringLiteral("rotation")]    = QJsonArray{double(j.restQ[0]), double(j.restQ[1]), double(j.restQ[2]), double(j.restQ[3])};
            n[QStringLiteral("scale")]       = QJsonArray{double(j.restS[0]), double(j.restS[1]), double(j.restS[2])};
            nodes.append(n);
        }

        // Hardpoint empties, parented to their joint. Never animation targets, so a `matrix` is
        // legal here — and for an authored model-space socket it is the honest form, because the
        // local transform is a product, not an authored TRS.
        QVector<QJsonArray> children(nj);
        if (opt.hardpointEmpties) {
            for (const Hardpoint& hp : scene.hardpoints) {
                if (hp.joint < 0 || hp.joint >= nj) continue;
                QJsonObject hn{{QStringLiteral("name"), hp.name}};
                if (hp.modelSpace) {
                    // The socket is authored in MODEL space and the joint is only what it FOLLOWS,
                    // so the child-of-joint local is socket · inverseBind (row-vector: "socket, then
                    // into bone space"). Verified on the D4 mount rigs: bind-pose world identical.
                    rm::Mat4 local = rm::mul(rm::composeTRS(hp.t, hp.q.data(), hp.s),
                                             skel->joints[hp.joint].inverseBind);
                    local[12] *= s; local[13] *= s; local[14] *= s;
                    // Scar 1 again: row-major element order is what glTF reads back as our transpose.
                    QJsonArray jm;
                    for (int k = 0; k < 16; ++k) jm.append(double(local[k]));
                    hn[QStringLiteral("matrix")] = jm;
                } else {
                    hn[QStringLiteral("translation")] = QJsonArray{double(hp.t[0] * s), double(hp.t[1] * s), double(hp.t[2] * s)};
                    hn[QStringLiteral("rotation")]    = QJsonArray{double(hp.q[0]), double(hp.q[1]), double(hp.q[2]), double(hp.q[3])};
                    hn[QStringLiteral("scale")]       = QJsonArray{double(hp.s[0]), double(hp.s[1]), double(hp.s[2])};
                }
                nodes.append(hn);
                children[hp.joint].append(nodes.size() - 1);
                ++r.hardpointsWritten;
            }
        }

        for (int bi = 0; bi < nj; ++bi) {
            const int p = skel->joints[bi].parent;
            if (p >= 0 && p < nj) children[p].append(boneBase + bi);
            else                  sceneNodes.append(boneBase + bi);
        }
        for (int bi = 0; bi < nj; ++bi) {
            if (children[bi].isEmpty()) continue;
            QJsonObject n = nodes[boneBase + bi].toObject();
            n[QStringLiteral("children")] = children[bi];
            nodes[boneBase + bi] = n;
        }
    }

    if (skinned) {
        // Scar 1: inverseBindMatrices in ROW-MAJOR element order.
        QVector<float> ibm; ibm.reserve(skel->size() * 16);
        for (const Joint& j : skel->joints) {
            rm::Mat4 inv = j.inverseBind;
            inv[12] *= s; inv[13] *= s; inv[14] *= s;
            for (int k = 0; k < 16; ++k) ibm.append(inv[k]);
        }
        const int bv = w.view(ibm.constData(), ibm.size() * 4, 0);
        const int ibmAcc = w.acc(accessorObj(bv, kFloat, skel->size(), "MAT4"));
        QJsonArray joints;
        for (int bi = 0; bi < skel->size(); ++bi) joints.append(boneBase + bi);
        jSkins.append(QJsonObject{{QStringLiteral("inverseBindMatrices"), ibmAcc},
                                  {QStringLiteral("joints"), joints}});
        QJsonObject mn = nodes[meshNodeIndex].toObject();
        mn[QStringLiteral("skin")] = 0;
        nodes[meshNodeIndex] = mn;
    }

    // ── Wrapper roots ──────────────────────────────────────────────────────────────────────────
    // A yaw, an up-axis change and (in WrapperNode mode) the unit scale are ROOT NODES, never a
    // second pass over positions, bind locals, inverse binds and every translation key. Skinning
    // carries a root transform for free: each joint's world gains it, the IBMs are unchanged, so
    // the deformed mesh is rigidly transformed with zero deformation.
    const auto wrap = [&](const QJsonObject& proto) {
        QJsonObject n = proto;
        n[QStringLiteral("children")] = sceneNodes;
        nodes.append(n);
        sceneNodes = QJsonArray{nodes.size() - 1};
    };
    if (opt.yawDegrees != 0.0f) {
        const rm::Quat q = axisQuat(1, opt.yawDegrees);
        wrap(QJsonObject{{QStringLiteral("name"), QStringLiteral("yaw")},
                         {QStringLiteral("rotation"), QJsonArray{double(q[0]), double(q[1]), double(q[2]), double(q[3])}}});
    }
    if (opt.upAxis == ExportOptions::ZUp) {
        // Y-up → Z-up is +90° about X: (0,1,0) → (0,0,1). glTF itself is always Y-up, so this is a
        // convenience for Z-up DCCs and is off by default.
        const rm::Quat q = axisQuat(0, 90.0f);
        wrap(QJsonObject{{QStringLiteral("name"), QStringLiteral("z_up")},
                         {QStringLiteral("rotation"), QJsonArray{double(q[0]), double(q[1]), double(q[2]), double(q[3])}}});
    }
    if (!bakeScale && opt.unitScale != 1.0f) {
        const double u = double(opt.unitScale);
        wrap(QJsonObject{{QStringLiteral("name"), QStringLiteral("unit_scale")},
                         {QStringLiteral("scale"), QJsonArray{u, u, u}}});
    }

    // ── Assemble ───────────────────────────────────────────────────────────────────────────────
    QJsonObject root;
    root[QStringLiteral("asset")] = QJsonObject{
        {QStringLiteral("version"), QStringLiteral("2.0")},
        {QStringLiteral("generator"), opt.generator.isEmpty() ? QStringLiteral("AssetBrowser") : opt.generator}};
    root[QStringLiteral("scene")] = 0;
    root[QStringLiteral("scenes")] = QJsonArray{QJsonObject{{QStringLiteral("nodes"), sceneNodes}}};
    root[QStringLiteral("nodes")] = nodes;
    if (wantMesh) root[QStringLiteral("meshes")] = QJsonArray{QJsonObject{{QStringLiteral("primitives"), meshPrims}}};
    if (!jMaterials.isEmpty()) root[QStringLiteral("materials")] = jMaterials;
    if (!jTextures.isEmpty()) {
        root[QStringLiteral("textures")] = jTextures;
        root[QStringLiteral("images")] = jImages;
        root[QStringLiteral("samplers")] = jSamplers;
    }
    if (!jSkins.isEmpty()) root[QStringLiteral("skins")] = jSkins;
    if (!extUsed.isEmpty()) {
        QStringList names(extUsed.cbegin(), extUsed.cend());
        names.sort();   // stable output: two identical exports must be byte-identical
        QJsonArray ext;
        for (const QString& e : names) ext.append(e);
        root[QStringLiteral("extensionsUsed")] = ext;
    }
    // NOTE (scar 3): `accessors` and `bufferViews` are assigned to `root` AFTER the animation block
    // below, because that block appends to them.

    // ── Animations ─────────────────────────────────────────────────────────────────────────────
    if (haveSkel && opt.includeAnimations) {
        const int nj = skel->size();
        for (int ci = 0; ci < scene.clips.size(); ++ci) {
            if (opt.onlyClip >= 0 && ci != opt.onlyClip) continue;
            const Clip& clip = scene.clips[ci];
            if (clip.tracks.isEmpty()) continue;

            QJsonArray channels, samplers;
            // Scar 4: one input accessor per distinct TIMESTAMP ARRAY within the clip. D4 keyed
            // this on the key COUNT, which is exact only for its dense tracks; core tracks are
            // sparse per channel, so two channels can share a count and differ in times.
            QHash<QByteArray, int> timeAccFor;
            const auto timeAcc = [&](const QVector<float>& times) -> int {
                const QByteArray key(reinterpret_cast<const char*>(times.constData()), times.size() * 4);
                const auto hit = timeAccFor.constFind(key);
                if (hit != timeAccFor.constEnd()) return hit.value();
                const double lo = times.isEmpty() ? 0.0 : double(times.first());
                const double hi = times.isEmpty() ? 0.0 : double(times.last());
                const int a = w.floats(times, 1, "SCALAR", 0, {lo}, {hi});
                timeAccFor.insert(key, a);
                return a;
            };
            const auto addChannel = [&](int node, const char* path, int in, int out) {
                samplers.append(QJsonObject{{QStringLiteral("input"), in},
                                            {QStringLiteral("output"), out},
                                            {QStringLiteral("interpolation"), QStringLiteral("LINEAR")}});
                channels.append(QJsonObject{
                    {QStringLiteral("sampler"), samplers.size() - 1},
                    {QStringLiteral("target"), QJsonObject{{QStringLiteral("node"), node},
                                                           {QStringLiteral("path"), QLatin1String(path)}}}});
            };

            for (const Track& tr : clip.tracks) {
                if (tr.joint < 0 || tr.joint >= nj || !tr.wellFormed()) continue;
                const Joint& j = skel->joints[tr.joint];
                const int node = boneBase + tr.joint;

                if (!tr.t.isEmpty()) {
                    if (tr.tIsConstant(j.restT)) { ++r.tracksElided; }
                    else {
                        QVector<float> out; out.reserve(tr.t.size() * 3);
                        for (const auto& v : tr.t) { out.append(v[0] * s); out.append(v[1] * s); out.append(v[2] * s); }
                        addChannel(node, "translation", timeAcc(tr.tTimes), w.floats(out, 3, "VEC3", 0));
                        ++r.tracksWritten;
                    }
                }
                if (!tr.q.isEmpty()) {
                    if (tr.qIsConstant(j.restQ)) { ++r.tracksElided; }
                    else {
                        QVector<float> out; out.reserve(tr.q.size() * 4);
                        for (const auto& v : tr.q) {
                            float o[4] = {v[0], v[1], v[2], v[3]};
                            float len = std::sqrt(o[0]*o[0] + o[1]*o[1] + o[2]*o[2] + o[3]*o[3]);
                            if (len < 1e-8f) { o[0] = o[1] = o[2] = 0; o[3] = 1; len = 1; }
                            for (int k = 0; k < 4; ++k) out.append(o[k] / len);
                        }
                        addChannel(node, "rotation", timeAcc(tr.qTimes), w.floats(out, 4, "VEC4", 0));
                        ++r.tracksWritten;
                    }
                }
                if (!tr.s.isEmpty()) {
                    if (tr.sIsConstant(j.restS)) { ++r.tracksElided; }
                    else {
                        QVector<float> out; out.reserve(tr.s.size() * 3);
                        for (const auto& v : tr.s) { out.append(v[0]); out.append(v[1]); out.append(v[2]); }
                        addChannel(node, "scale", timeAcc(tr.sTimes), w.floats(out, 3, "VEC3", 0));
                        ++r.tracksWritten;
                    }
                }
            }
            // Scar 8: an animation with an empty `channels` array is invalid glTF. A clip whose
            // every track was constant is not an error — it is a clip that does nothing.
            if (channels.isEmpty()) continue;
            jAnimations.append(QJsonObject{{QStringLiteral("name"), clip.name},
                                           {QStringLiteral("channels"), channels},
                                           {QStringLiteral("samplers"), samplers}});
        }
        if (!jAnimations.isEmpty()) root[QStringLiteral("animations")] = jAnimations;
    }
    r.animationsWritten = jAnimations.size();

    // Scar 3: only now, after the animation block appended to them.
    root[QStringLiteral("accessors")] = w.accessors;
    root[QStringLiteral("bufferViews")] = w.bufferViews;

    w.bin.align4();
    root[QStringLiteral("buffers")] = QJsonArray{QJsonObject{{QStringLiteral("byteLength"), w.bin.data.size()}}};

    QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    while (json.size() & 3) json.append(' ');
    const QByteArray& blob = w.bin.data;   // already 4-aligned

    QByteArray glb;
    const auto put32 = [&](uint32_t v) { v = qToLittleEndian(v); glb.append(reinterpret_cast<char*>(&v), 4); };
    put32(0x46546C67);                                    // "glTF"
    put32(2);                                             // version
    put32(12 + 8 + json.size() + 8 + blob.size());        // total length
    put32(uint32_t(json.size())); put32(0x4E4F534A);      // JSON chunk
    glb.append(json);
    put32(uint32_t(blob.size())); put32(0x004E4942);      // BIN chunk
    glb.append(blob);

    r.ok = true;
    if (result) *result = r;
    return glb;
}

bool write(const Scene& scene, const ExportOptions& opt, const QString& path, Result* result)
{
    Result local;
    Result& r = result ? *result : local;

    const bool wantGltf = opt.gltfSeparate || path.endsWith(QStringLiteral(".gltf"), Qt::CaseInsensitive);
    ExportOptions eff = opt;
    // Loose textures are sibling files, which only a text .gltf can reference; a .glb is
    // self-contained by definition, so embedding is forced there.
    eff.looseTextures = opt.looseTextures && wantGltf;

    QVector<LooseImage> loose;
    const QByteArray glb = build(scene, eff, &r, eff.looseTextures ? &loose : nullptr);
    if (glb.isEmpty()) return false;

    const QString dir = QFileInfo(path).absolutePath();
    for (const LooseImage& li : loose) {
        QFile tf(dir + QLatin1Char('/') + li.name);
        if (!tf.open(QIODevice::WriteOnly) || tf.write(li.bytes) != li.bytes.size()) {
            r.ok = false;
            r.error = QStringLiteral("cannot write texture %1").arg(li.name);
            return false;
        }
    }

    const auto failWrite = [&](const QString& why) {
        r.ok = false;
        r.error = why;
        return false;
    };

    if (wantGltf) {
        // Split the .glb we just built into text glTF + a sidecar .bin and re-point the buffer at
        // it; every bufferView-referenced image still resolves through the same bytes.
        if (glb.size() < 20) return failWrite(QStringLiteral("internal: glb too small to split"));
        uint32_t jsonLen = 0;
        std::memcpy(&jsonLen, glb.constData() + 12, 4);
        jsonLen = qFromLittleEndian(jsonLen);
        const int binHdr = 20 + int(jsonLen);
        if (binHdr + 8 > glb.size()) return failWrite(QStringLiteral("internal: glb chunk layout"));
        uint32_t binLen = 0;
        std::memcpy(&binLen, glb.constData() + binHdr, 4);
        binLen = qFromLittleEndian(binLen);

        const QString binName = QFileInfo(path).completeBaseName() + QStringLiteral(".bin");
        QJsonObject root = QJsonDocument::fromJson(glb.mid(20, jsonLen)).object();
        QJsonArray buffers = root.value(QStringLiteral("buffers")).toArray();
        if (!buffers.isEmpty()) {
            QJsonObject b0 = buffers[0].toObject();
            b0[QStringLiteral("uri")] = binName;
            buffers[0] = b0;
            root[QStringLiteral("buffers")] = buffers;
        }
        const QByteArray binBytes = glb.mid(binHdr + 8, binLen);
        QFile bf(dir + QLatin1Char('/') + binName);
        if (!bf.open(QIODevice::WriteOnly) || bf.write(binBytes) != binBytes.size())
            return failWrite(QStringLiteral("cannot write %1").arg(binName));
        const QByteArray jsonOut = QJsonDocument(root).toJson(QJsonDocument::Indented);
        QFile jf(path);
        if (!jf.open(QIODevice::WriteOnly) || jf.write(jsonOut) != jsonOut.size())
            return failWrite(QStringLiteral("cannot write %1").arg(path));
        return true;
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return failWrite(QStringLiteral("cannot write %1: %2").arg(path, f.errorString()));
    if (f.write(glb) != glb.size())
        return failWrite(QStringLiteral("short write to %1").arg(path));
    return true;
}


// ── Self-test ──────────────────────────────────────────────────────────────────────────────────
// Every case here is a bug one of the three tools actually shipped. Runs at startup (the family
// habit: a self-test that never runs is a comment).
namespace {

struct Parsed {
    QJsonObject root;
    QByteArray  bin;
    QString     err;
};

Parsed parseGlb(const QByteArray& glb)
{
    Parsed p;
    if (glb.size() < 20 || glb.left(4) != QByteArray("glTF")) { p.err = QStringLiteral("bad magic"); return p; }
    uint32_t total = 0; std::memcpy(&total, glb.constData() + 8, 4); total = qFromLittleEndian(total);
    if (int(total) != glb.size()) { p.err = QStringLiteral("length field %1 != file %2").arg(total).arg(glb.size()); return p; }
    uint32_t jsonLen = 0; std::memcpy(&jsonLen, glb.constData() + 12, 4); jsonLen = qFromLittleEndian(jsonLen);
    if (20 + int(jsonLen) + 8 > glb.size()) { p.err = QStringLiteral("chunk layout"); return p; }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(glb.mid(20, jsonLen), &pe);
    if (pe.error != QJsonParseError::NoError) { p.err = QStringLiteral("JSON chunk invalid"); return p; }
    p.root = doc.object();
    uint32_t binLen = 0; std::memcpy(&binLen, glb.constData() + 20 + jsonLen, 4); binLen = qFromLittleEndian(binLen);
    p.bin = glb.mid(20 + int(jsonLen) + 8, int(binLen));
    return p;
}

// Read one float out of the BIN chunk through an accessor, as an importer would.
float accFloat(const Parsed& p, int accIndex, int element)
{
    const QJsonObject a = p.root.value(QStringLiteral("accessors")).toArray().at(accIndex).toObject();
    const QJsonObject v = p.root.value(QStringLiteral("bufferViews")).toArray()
                              .at(a.value(QStringLiteral("bufferView")).toInt()).toObject();
    const int off = v.value(QStringLiteral("byteOffset")).toInt() + a.value(QStringLiteral("byteOffset")).toInt();
    float f = 0;
    std::memcpy(&f, p.bin.constData() + off + element * 4, 4);
    return f;
}

Geometry oneTriangle(bool skinned = false)
{
    Geometry g;
    g.vertices.resize(3);
    g.vertices[1].px = 1;
    g.vertices[2].py = 1;
    g.indices = {0, 1, 2};
    Part part; part.name = QStringLiteral("tri"); part.indexStart = 0; part.indexCount = 3;
    g.parts.append(part);
    g.skinned = skinned;
    if (skinned) for (Vertex& v : g.vertices) v.weights[0] = 1.0f;
    g.computeBounds();
    return g;
}

Skeleton twoBones()
{
    Skeleton sk;
    Joint a; a.name = QStringLiteral("root");
    Joint b; b.name = QStringLiteral("child"); b.parent = 0; b.restT[1] = 2.0f;
    // A distinctive inverse bind: translation (7, 8, 9) in ROW-MAJOR slots 12..14.
    a.inverseBind = rm::identity();
    b.inverseBind = rm::identity(); b.inverseBind[12] = 7; b.inverseBind[13] = 8; b.inverseBind[14] = 9;
    sk.joints = {a, b};
    return sk;
}

}  // namespace

QString selfTest()
{
    const auto bad = [](const QString& m) { return QStringLiteral("GlbExporter self-test: ") + m; };

    // 1. Framing: magic, total length, parseable JSON chunk.
    {
        const Geometry g = oneTriangle();
        Scene sc; sc.geometry = &g;
        Result r;
        const Parsed p = parseGlb(build(sc, ExportOptions{}, &r));
        if (!r.ok) return bad(QStringLiteral("static build failed (%1)").arg(r.error));
        if (!p.err.isEmpty()) return bad(p.err);
        if (r.primitives != 1) return bad(QStringLiteral("expected 1 primitive, got %1").arg(r.primitives));
    }

    // 2. "Everything hidden ⇒ nothing to write", said out loud rather than emitting an empty mesh.
    {
        Geometry g = oneTriangle();
        g.parts[0].visible = false;
        Scene sc; sc.geometry = &g;
        Result r;
        build(sc, ExportOptions{}, &r);
        if (r.ok) return bad(QStringLiteral("an all-hidden mesh must fail, not export empty"));
    }

    // 2b. A part inside a hidden GROUP is hidden too (FOX's mesh-group tree, visibility inherited).
    {
        Geometry g = oneTriangle();
        PartGroup root; root.name = QStringLiteral("body"); root.visible = false;
        PartGroup leaf; leaf.name = QStringLiteral("pouch"); leaf.parent = 0;   // visible itself
        g.groups = {root, leaf};
        g.parts[0].group = 1;
        Scene sc; sc.geometry = &g;
        Result r;
        build(sc, ExportOptions{}, &r);
        if (r.ok) return bad(QStringLiteral("a part under a hidden ancestor group must not be written"));
    }

    // 3. Scar 6 — unused-material pruning and the index remap that must follow it.
    {
        Geometry g;
        g.vertices.resize(6);
        g.vertices[1].px = 1; g.vertices[2].py = 1; g.vertices[4].px = 1; g.vertices[5].py = 1;
        g.indices = {0, 1, 2, 3, 4, 5};
        Part a; a.name = QStringLiteral("hidden"); a.indexStart = 0; a.indexCount = 3; a.materialIndex = 0; a.visible = false;
        Part b; b.name = QStringLiteral("shown");  b.indexStart = 3; b.indexCount = 3; b.materialIndex = 1;
        g.parts = {a, b};
        g.materialNames = {QStringLiteral("m0"), QStringLiteral("m1")};
        g.computeBounds();
        Material m0; m0.name = QStringLiteral("m0");
        Material m1; m1.name = QStringLiteral("m1");
        Scene sc; sc.geometry = &g; sc.materials = {m0, m1};
        Result r;
        const Parsed p = parseGlb(build(sc, ExportOptions{}, &r));
        if (!r.ok) return bad(QStringLiteral("prune build failed (%1)").arg(r.error));
        const QJsonArray mats = p.root.value(QStringLiteral("materials")).toArray();
        if (mats.size() != 1 || mats.at(0).toObject().value(QStringLiteral("name")).toString() != QStringLiteral("m1"))
            return bad(QStringLiteral("expected the one used material (m1), got %1").arg(mats.size()));
        const QJsonArray prims = p.root.value(QStringLiteral("meshes")).toArray().at(0).toObject()
                                     .value(QStringLiteral("primitives")).toArray();
        if (prims.size() != 1 || prims.at(0).toObject().value(QStringLiteral("material")).toInt(-1) != 0)
            return bad(QStringLiteral("primitive material index not remapped to 0"));
        if (r.materialsPruned != 1) return bad(QStringLiteral("materialsPruned should be 1"));
    }

    const Geometry gs = oneTriangle(true);
    const Skeleton sk = twoBones();

    // 4. Scar 2 (bone nodes are TRS, never `matrix`) and scar 1 (IBM in ROW-MAJOR element order).
    {
        Scene sc; sc.geometry = &gs; sc.skeleton = &sk;
        Result r;
        const Parsed p = parseGlb(build(sc, ExportOptions{}, &r));
        if (!r.ok) return bad(QStringLiteral("skinned build failed (%1)").arg(r.error));
        const QJsonArray nodes = p.root.value(QStringLiteral("nodes")).toArray();
        if (nodes.size() < 3) return bad(QStringLiteral("expected a mesh node + 2 bone nodes"));
        for (int i = 1; i <= 2; ++i) {
            const QJsonObject n = nodes.at(i).toObject();
            if (n.contains(QStringLiteral("matrix")))
                return bad(QStringLiteral("bone node %1 carries a matrix; glTF forbids that on an animation target").arg(i));
            if (!n.contains(QStringLiteral("translation")) || !n.contains(QStringLiteral("rotation"))
                || !n.contains(QStringLiteral("scale")))
                return bad(QStringLiteral("bone node %1 is not full TRS").arg(i));
        }
        if (std::fabs(nodes.at(2).toObject().value(QStringLiteral("translation")).toArray().at(1).toDouble() - 2.0) > 1e-6)
            return bad(QStringLiteral("child bone rest translation lost"));
        const QJsonArray skins = p.root.value(QStringLiteral("skins")).toArray();
        if (skins.size() != 1) return bad(QStringLiteral("expected one skin"));
        const int ibm = skins.at(0).toObject().value(QStringLiteral("inverseBindMatrices")).toInt(-1);
        if (ibm < 0) return bad(QStringLiteral("skin has no inverseBindMatrices"));
        // Joint 1's IBM, elements 12..14 — row-major storage puts the translation there. A
        // column-major dump would put 7,8,9 at 3,7,11 and leave these zero: the exact bug that
        // deformed every POE2 mesh at rest.
        const float t0 = accFloat(p, ibm, 16 + 12), t1 = accFloat(p, ibm, 16 + 13), t2 = accFloat(p, ibm, 16 + 14);
        if (std::fabs(t0 - 7) > 1e-5f || std::fabs(t1 - 8) > 1e-5f || std::fabs(t2 - 9) > 1e-5f)
            return bad(QStringLiteral("inverseBindMatrices not stored row-major (got %1,%2,%3 at 12..14)")
                           .arg(double(t0)).arg(double(t1)).arg(double(t2)));
    }

    // 5. Scar 7 — an out-of-range joint index has its weight zeroed, not the index silently clamped
    //    onto the root bone.
    {
        Geometry g = oneTriangle(true);
        g.vertices[0].joints[0] = 99;          // no such bone
        g.vertices[0].weights[0] = 1.0f;
        g.vertices[0].joints[1] = 1;
        g.vertices[0].weights[1] = 1.0f;
        Scene sc; sc.geometry = &g; sc.skeleton = &sk;
        Result r;
        const Parsed p = parseGlb(build(sc, ExportOptions{}, &r));
        if (!r.ok) return bad(QStringLiteral("bad-influence build failed (%1)").arg(r.error));
        if (r.badInfluences != 1) return bad(QStringLiteral("expected 1 bad influence, got %1").arg(r.badInfluences));
        const QJsonObject prim = p.root.value(QStringLiteral("meshes")).toArray().at(0).toObject()
                                     .value(QStringLiteral("primitives")).toArray().at(0).toObject();
        const int wa = prim.value(QStringLiteral("attributes")).toObject().value(QStringLiteral("WEIGHTS_0")).toInt(-1);
        if (wa < 0) return bad(QStringLiteral("no WEIGHTS_0"));
        if (std::fabs(accFloat(p, wa, 0)) > 1e-6f)
            return bad(QStringLiteral("the out-of-range influence kept its weight (vertex would snap to the root)"));
        if (std::fabs(accFloat(p, wa, 1) - 1.0f) > 1e-6f)
            return bad(QStringLiteral("surviving influence not renormalised to 1"));
    }

    // 6. Scar 4 (one input accessor per distinct timestamp array) and scar 5 (constant tracks
    //    dropped), in one clip so the interaction is covered too.
    {
        Clip clip;
        clip.name = QStringLiteral("walk");
        Track moving;                 // joint 0: rotates, and translates away from rest
        moving.joint = 0;
        moving.qTimes = {0.0f, 0.5f};
        moving.q = {{{0, 0, 0, 1}}, {{0, 0.7071f, 0, 0.7071f}}};
        moving.tTimes = {0.0f, 0.5f};
        moving.t = {{{0, 0, 0}}, {{1, 0, 0}}};
        Track constantT;              // joint 1: same times, but never leaves its rest translation
        constantT.joint = 1;
        constantT.tTimes = {0.0f, 0.5f};
        constantT.t = {{{0, 2, 0}}, {{0, 2, 0}}};
        Track otherTimes;             // joint 1: same key COUNT, different times — must NOT share
        otherTimes.joint = 1;
        otherTimes.qTimes = {0.0f, 0.9f};
        otherTimes.q = {{{0, 0, 0, 1}}, {{0.7071f, 0, 0, 0.7071f}}};
        clip.tracks = {moving, constantT, otherTimes};
        clip.recomputeDuration();

        Scene sc; sc.geometry = &gs; sc.skeleton = &sk; sc.clips = {clip};
        Result r;
        const Parsed p = parseGlb(build(sc, ExportOptions{}, &r));
        if (!r.ok) return bad(QStringLiteral("animated build failed (%1)").arg(r.error));
        if (r.tracksElided != 1) return bad(QStringLiteral("expected 1 constant track dropped, got %1").arg(r.tracksElided));
        if (r.tracksWritten != 3) return bad(QStringLiteral("expected 3 tracks written, got %1").arg(r.tracksWritten));
        const QJsonArray anims = p.root.value(QStringLiteral("animations")).toArray();
        if (anims.size() != 1) return bad(QStringLiteral("expected one animation"));
        const QJsonArray samplers = anims.at(0).toObject().value(QStringLiteral("samplers")).toArray();
        if (samplers.size() != 3) return bad(QStringLiteral("expected 3 samplers, got %1").arg(samplers.size()));
        QSet<int> inputs;
        for (const QJsonValue& v : samplers) inputs.insert(v.toObject().value(QStringLiteral("input")).toInt(-1));
        // Two identical time arrays share one accessor; the third differs and gets its own.
        if (inputs.size() != 2)
            return bad(QStringLiteral("time accessors: expected 2 distinct inputs (shared + one), got %1").arg(inputs.size()));
        // Scar 3: every sampler index must be inside the accessor array that was written.
        const int nAcc = p.root.value(QStringLiteral("accessors")).toArray().size();
        for (const QJsonValue& v : samplers) {
            const QJsonObject smp = v.toObject();
            if (smp.value(QStringLiteral("input")).toInt(-1) >= nAcc || smp.value(QStringLiteral("output")).toInt(-1) >= nAcc)
                return bad(QStringLiteral("a sampler points past the accessor array (accessors assigned too early)"));
        }
    }

    // 7. A clip whose every track sits at rest is not an error and is not written as an empty
    //    animation (which is invalid glTF) — it is simply absent.
    {
        Clip clip;
        clip.name = QStringLiteral("idle");
        Track t; t.joint = 1; t.tTimes = {0.0f, 1.0f}; t.t = {{{0, 2, 0}}, {{0, 2, 0}}};
        clip.tracks = {t};
        Scene sc; sc.geometry = &gs; sc.skeleton = &sk; sc.clips = {clip};
        Result r;
        const Parsed p = parseGlb(build(sc, ExportOptions{}, &r));
        if (!r.ok) return bad(QStringLiteral("all-constant clip build failed (%1)").arg(r.error));
        if (p.root.contains(QStringLiteral("animations")))
            return bad(QStringLiteral("an all-constant clip must not produce an empty animation"));
    }

    // 8. COLOR_0 is opt-in (D4's vertex colour is detail-map blend weights, and glTF would multiply
    //    it into base colour), TEXCOORD_1 is not.
    {
        Geometry g = oneTriangle();
        g.hasColor = true; g.hasUv1 = true;
        Scene sc; sc.geometry = &g;
        Result r;
        const auto attrs = [](const Parsed& p) {
            return p.root.value(QStringLiteral("meshes")).toArray().at(0).toObject()
                       .value(QStringLiteral("primitives")).toArray().at(0).toObject()
                       .value(QStringLiteral("attributes")).toObject();
        };
        const QJsonObject off = attrs(parseGlb(build(sc, ExportOptions{}, &r)));
        if (off.contains(QStringLiteral("COLOR_0"))) return bad(QStringLiteral("COLOR_0 must be off by default"));
        if (!off.contains(QStringLiteral("TEXCOORD_1"))) return bad(QStringLiteral("TEXCOORD_1 should be written when present"));
        ExportOptions withCol; withCol.writeVertexColor = true;
        if (!attrs(parseGlb(build(sc, withCol, &r))).contains(QStringLiteral("COLOR_0")))
            return bad(QStringLiteral("COLOR_0 not written when asked for"));
    }

    // 9. A TANGENT is never invented for a mesh that never had one.
    {
        Geometry g = oneTriangle();
        Scene sc; sc.geometry = &g;
        Result r;
        const Parsed p = parseGlb(build(sc, ExportOptions{}, &r));
        if (p.root.value(QStringLiteral("meshes")).toArray().at(0).toObject()
                .value(QStringLiteral("primitives")).toArray().at(0).toObject()
                .value(QStringLiteral("attributes")).toObject().contains(QStringLiteral("TANGENT")))
            return bad(QStringLiteral("TANGENT written for a mesh with hasTangent = false"));
    }

    // 10. unitScale: BakeIntoData multiplies the data and leaves the node graph alone; WrapperNode
    //     leaves the data alone and adds exactly one scaled root. Both must be available, because
    //     POE2 needs the first (0.005 metres per art unit) and FOX chose the second.
    {
        Scene sc; sc.geometry = &gs; sc.skeleton = &sk;
        ExportOptions baked; baked.unitScale = 0.5f;
        Result r;
        Parsed p = parseGlb(build(sc, baked, &r));
        if (!r.ok) return bad(QStringLiteral("baked-scale build failed (%1)").arg(r.error));
        const QJsonObject childNode = p.root.value(QStringLiteral("nodes")).toArray().at(2).toObject();
        if (std::fabs(childNode.value(QStringLiteral("translation")).toArray().at(1).toDouble() - 1.0) > 1e-6)
            return bad(QStringLiteral("baked unit scale did not reach the bone rest translation"));

        ExportOptions node = baked; node.scaleMode = ExportOptions::WrapperNode;
        p = parseGlb(build(sc, node, &r));
        if (!r.ok) return bad(QStringLiteral("wrapper-scale build failed (%1)").arg(r.error));
        const QJsonArray nodes = p.root.value(QStringLiteral("nodes")).toArray();
        if (std::fabs(nodes.at(2).toObject().value(QStringLiteral("translation")).toArray().at(1).toDouble() - 2.0) > 1e-6)
            return bad(QStringLiteral("wrapper scale must not touch the data"));
        const QJsonArray roots = p.root.value(QStringLiteral("scenes")).toArray().at(0).toObject()
                                     .value(QStringLiteral("nodes")).toArray();
        if (roots.size() != 1) return bad(QStringLiteral("wrapper scale should leave exactly one scene root"));
        if (std::fabs(nodes.at(roots.at(0).toInt()).toObject().value(QStringLiteral("scale")).toArray().at(0).toDouble() - 0.5) > 1e-6)
            return bad(QStringLiteral("wrapper root does not carry the unit scale"));
    }

    // 11. Hardpoint empties: a bone-local socket is a TRS child of its joint; a model-space socket
    //     is inverseBind-composed and emitted as a matrix (an empty is never an animation target).
    {
        Hardpoint local;  local.name = QStringLiteral("HP_hand"); local.joint = 1; local.t[0] = 3;
        Hardpoint world;  world.name = QStringLiteral("HP_mount"); world.joint = 1; world.modelSpace = true;
        world.t[0] = 1; world.t[1] = 2; world.t[2] = 3;
        Scene sc; sc.geometry = &gs; sc.skeleton = &sk; sc.hardpoints = {local, world};
        ExportOptions opt; opt.hardpointEmpties = true;
        Result r;
        const Parsed p = parseGlb(build(sc, opt, &r));
        if (!r.ok) return bad(QStringLiteral("hardpoint build failed (%1)").arg(r.error));
        if (r.hardpointsWritten != 2) return bad(QStringLiteral("expected 2 hardpoints, got %1").arg(r.hardpointsWritten));
        const QJsonArray nodes = p.root.value(QStringLiteral("nodes")).toArray();
        const QJsonArray kids = nodes.at(2).toObject().value(QStringLiteral("children")).toArray();
        if (kids.size() != 2) return bad(QStringLiteral("hardpoints not parented to their joint"));
        const QJsonObject n0 = nodes.at(kids.at(0).toInt()).toObject();
        if (n0.value(QStringLiteral("name")).toString() != QStringLiteral("HP_hand")
            || std::fabs(n0.value(QStringLiteral("translation")).toArray().at(0).toDouble() - 3.0) > 1e-6)
            return bad(QStringLiteral("bone-local hardpoint not emitted as TRS at its authored offset"));
        const QJsonObject n1 = nodes.at(kids.at(1).toInt()).toObject();
        const QJsonArray m = n1.value(QStringLiteral("matrix")).toArray();
        if (m.size() != 16) return bad(QStringLiteral("model-space hardpoint should be a matrix node"));
        // socket(1,2,3) · inverseBind(7,8,9) = translation (8,10,12), in row-major slots 12..14.
        if (std::fabs(m.at(12).toDouble() - 8.0) > 1e-5 || std::fabs(m.at(13).toDouble() - 10.0) > 1e-5
            || std::fabs(m.at(14).toDouble() - 12.0) > 1e-5)
            return bad(QStringLiteral("model-space hardpoint local is not socket · inverseBind in row-major order"));
    }

    // 12. Animation library: a rig and its clips with no mesh at all (D4's option).
    {
        Clip clip;
        clip.name = QStringLiteral("swing");
        Track t; t.joint = 0; t.qTimes = {0.0f, 1.0f};
        t.q = {{{0, 0, 0, 1}}, {{0, 0.7071f, 0, 0.7071f}}};
        clip.tracks = {t};
        Scene sc; sc.skeleton = &sk; sc.clips = {clip};
        ExportOptions opt; opt.animationLibrary = true;
        Result r;
        const Parsed p = parseGlb(build(sc, opt, &r));
        if (!r.ok) return bad(QStringLiteral("animation-library build failed (%1)").arg(r.error));
        if (p.root.contains(QStringLiteral("meshes"))) return bad(QStringLiteral("an animation library must carry no mesh"));
        if (p.root.value(QStringLiteral("animations")).toArray().size() != 1)
            return bad(QStringLiteral("animation library lost its clip"));
        // Bones are the scene roots now, and the animation targets must follow them.
        const int node = p.root.value(QStringLiteral("animations")).toArray().at(0).toObject()
                             .value(QStringLiteral("channels")).toArray().at(0).toObject()
                             .value(QStringLiteral("target")).toObject().value(QStringLiteral("node")).toInt(-1);
        if (node != 0) return bad(QStringLiteral("animation targets node %1, but bone 0 is node 0 here").arg(node));
    }

    // 13. onlyClip selects exactly one clip, by index into the ORIGINAL list.
    {
        Clip a; a.name = QStringLiteral("a");
        Track ta; ta.joint = 0; ta.qTimes = {0.0f, 1.0f}; ta.q = {{{0, 0, 0, 1}}, {{0, 0.7071f, 0, 0.7071f}}};
        a.tracks = {ta};
        Clip b = a; b.name = QStringLiteral("b");
        Scene sc; sc.geometry = &gs; sc.skeleton = &sk; sc.clips = {a, b};
        ExportOptions opt; opt.onlyClip = 1;
        Result r;
        const Parsed p = parseGlb(build(sc, opt, &r));
        const QJsonArray anims = p.root.value(QStringLiteral("animations")).toArray();
        if (anims.size() != 1 || anims.at(0).toObject().value(QStringLiteral("name")).toString() != QStringLiteral("b"))
            return bad(QStringLiteral("onlyClip did not select clip 1"));
    }

    // 14. Image dedupe: one image shared by two materials is embedded once.
    {
        Geometry g;
        g.vertices.resize(6);
        g.vertices[1].px = 1; g.vertices[2].py = 1; g.vertices[4].px = 1; g.vertices[5].py = 1;
        g.indices = {0, 1, 2, 3, 4, 5};
        Part a; a.indexStart = 0; a.indexCount = 3; a.materialIndex = 0;
        Part b; b.indexStart = 3; b.indexCount = 3; b.materialIndex = 1;
        g.parts = {a, b};
        g.computeBounds();
        QImage img(4, 4, QImage::Format_RGBA8888);
        img.fill(Qt::red);
        Material m0; m0.name = QStringLiteral("m0"); m0.baseColor.image = img;
        Material m1; m1.name = QStringLiteral("m1"); m1.baseColor.image = img;
        Scene sc; sc.geometry = &g; sc.materials = {m0, m1};
        Result r;
        const Parsed p = parseGlb(build(sc, ExportOptions{}, &r));
        if (!r.ok) return bad(QStringLiteral("dedupe build failed (%1)").arg(r.error));
        if (p.root.value(QStringLiteral("images")).toArray().size() != 1 || r.imagesDeduped != 1)
            return bad(QStringLiteral("two materials sharing one image should embed it once"));
    }

    // 15. KHR_materials_ior: written (and listed in extensionsUsed) only when the material sets one.
    {
        Geometry g = oneTriangle();
        g.parts[0].materialIndex = 0;
        g.materialNames = {QStringLiteral("m")};
        g.computeBounds();
        Material plain; plain.name = QStringLiteral("m");          // ior unset (0) ⇒ no extension
        Scene sc; sc.geometry = &g; sc.materials = {plain};
        Result r;
        Parsed p = parseGlb(build(sc, ExportOptions{}, &r));
        if (!r.ok) return bad(QStringLiteral("ior-unset build failed (%1)").arg(r.error));
        if (p.root.value(QStringLiteral("materials")).toArray().at(0).toObject().contains(QStringLiteral("extensions")))
            return bad(QStringLiteral("a material with no ior must not carry KHR_materials_ior"));

        Material withIor = plain; withIor.ior = 1.0f;
        sc.materials = {withIor};
        p = parseGlb(build(sc, ExportOptions{}, &r));
        if (!r.ok) return bad(QStringLiteral("ior build failed (%1)").arg(r.error));
        const QJsonObject ext = p.root.value(QStringLiteral("materials")).toArray().at(0).toObject()
                                    .value(QStringLiteral("extensions")).toObject()
                                    .value(QStringLiteral("KHR_materials_ior")).toObject();
        if (std::fabs(ext.value(QStringLiteral("ior")).toDouble() - 1.0) > 1e-6)
            return bad(QStringLiteral("KHR_materials_ior not written with the authored value"));
        if (!p.root.value(QStringLiteral("extensionsUsed")).toArray().contains(QStringLiteral("KHR_materials_ior")))
            return bad(QStringLiteral("KHR_materials_ior missing from extensionsUsed"));
    }

    return QString();
}

}  // namespace core::glb
