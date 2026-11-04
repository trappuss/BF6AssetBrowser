#include "core/gl/GLModelWidget.h"
#include "core/gl/Pose.h"
#include "core/gl/Shaders.h"

#include <QEnterEvent>
#include <QFont>
#include <QLabel>
#include <QLineF>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QPaintEvent>
#include <QPainter>
#include <QTimer>
#include <QVector4D>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace core::gl {

const char* shadingModeName(ShadingMode m)
{
    switch (m) {
    case ShadingMode::Wireframe: return "Wireframe";
    case ShadingMode::Flat:      return "Flat";
    case ShadingMode::Shaded:    return "Shaded";
    case ShadingMode::Rendered:  return "Rendered";
    }
    return "Rendered";
}

namespace {

constexpr int kStride = 11;   // px py pz nx ny nz u v tx ty tz
constexpr float kPi = 3.14159265f;
constexpr float kPitchLimit = 1.553f;   // ~89 degrees, avoids the gimbal flip at the poles

// Blender-style axis-orientation gizmo: a small ball in the viewport's top-right that projects the
// world axes with the SAME yaw/pitch math paintGL uses. Clicking an axis end glides the camera to
// that axis view (centre + zoom preserved). A plain child widget — no GL state interplay, and it
// naturally receives the clicks so they never reach the orbit-drag handler. Lifted from D4.
class AxisGizmoOverlay : public QWidget {
public:
    explicit AxisGizmoOverlay(GLModelWidget* host) : QWidget(host), m_host(host) {
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QStringLiteral("Orbit to an axis view (click a ball) · double-click: ortho"));
        setMouseTracking(true);
    }
protected:
    struct End { QPointF p; float toward; int axis; bool pos; int idx; };

    QVector<End> ends() const
    {
        const float cp = std::cos(m_host->camPitch()), sp = std::sin(m_host->camPitch());
        const float cy = std::cos(m_host->camYaw()),   sy = std::sin(m_host->camYaw());
        const QVector3D dir(cp * sy, sp, cp * cy);
        const QVector3D f = -dir;
        QVector3D r = QVector3D::crossProduct(f, QVector3D(0, 1, 0));
        if (r.lengthSquared() < 1e-6f) r = QVector3D(1, 0, 0);
        r.normalize();
        const QVector3D u = QVector3D::crossProduct(r, f);
        const QPointF c(width() / 2.0, height() / 2.0);
        const float R = qMin(width(), height()) / 2.0f - 9.0f;
        QVector<End> out;
        static const QVector3D ax[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        for (int a = 0; a < 3; ++a)
            for (int s = 0; s < 2; ++s) {
                const QVector3D w = s == 0 ? ax[a] : -ax[a];
                out << End{c + QPointF(QVector3D::dotProduct(w, r) * R, -QVector3D::dotProduct(w, u) * R),
                           -QVector3D::dotProduct(w, f), a, s == 0, a * 2 + s};
            }
        return out;
    }
    int endAt(const QPointF& p) const
    {
        int best = -1; double bd = 1e9;
        for (const End& n : ends()) { const double d = QLineF(p, n.p).length(); if (d < bd) { bd = d; best = n.idx; } }
        return bd <= 9.5 ? best : -1;
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        if (!m_hover) g.setOpacity(0.35);
        g.setPen(Qt::NoPen);
        if (m_hover) { g.setBrush(QColor(255, 255, 255, 26)); g.drawEllipse(rect().adjusted(1, 1, -1, -1)); }
        static const QColor col[3] = {QColor(226, 84, 77), QColor(118, 183, 66), QColor(74, 132, 222)};
        QVector<End> e = ends();
        std::sort(e.begin(), e.end(), [](const End& a, const End& b) { return a.toward < b.toward; });
        const QPointF c(width() / 2.0, height() / 2.0);
        for (const End& n : e) {
            const bool hot = m_hover && n.idx == m_hotEnd;
            if (n.pos) { g.setPen(QPen(col[n.axis], 1.6)); g.drawLine(c, n.p); }
            g.setPen(Qt::NoPen);
            if (n.pos) {
                const qreal rad = hot ? 8.0 : 6.5;
                g.setBrush(hot ? col[n.axis].lighter(125) : col[n.axis]);
                g.drawEllipse(n.p, rad, rad);
                if (hot) { g.setPen(QPen(QColor(255, 255, 255, 220), 1.3)); g.setBrush(Qt::NoBrush); g.drawEllipse(n.p, rad, rad); g.setPen(Qt::NoPen); }
                g.setPen(QColor(25, 25, 25));
                QFont f = g.font(); f.setPointSizeF(7.5); f.setBold(true); g.setFont(f);
                g.drawText(QRectF(n.p.x() - rad, n.p.y() - rad, rad * 2, rad * 2), Qt::AlignCenter, QString(QLatin1Char('X' + n.axis)));
            } else if (hot) {
                g.setBrush(col[n.axis].lighter(115)); g.drawEllipse(n.p, 7.5, 7.5);
                g.setPen(QPen(QColor(255, 255, 255, 220), 1.3)); g.setBrush(Qt::NoBrush); g.drawEllipse(n.p, 7.5, 7.5);
                g.setPen(QColor(25, 25, 25));
                QFont f = g.font(); f.setPointSizeF(6.5); f.setBold(true); g.setFont(f);
                g.drawText(QRectF(n.p.x() - 7.5, n.p.y() - 7.5, 15, 15), Qt::AlignCenter, QStringLiteral("−") + QString(QLatin1Char('X' + n.axis)));
            } else {
                g.setBrush(col[n.axis].darker(230)); g.drawEllipse(n.p, 5.0, 5.0);
                g.setPen(QPen(col[n.axis].darker(140), 1.2)); g.setBrush(Qt::NoBrush); g.drawEllipse(n.p, 5.0, 5.0);
            }
        }
    }
    void enterEvent(QEnterEvent*) override { m_hover = true; update(); }
    void leaveEvent(QEvent*) override { m_hover = false; m_hotEnd = -1; update(); }
    void mouseMoveEvent(QMouseEvent* ev) override { const int hot = endAt(ev->position()); if (hot != m_hotEnd) { m_hotEnd = hot; update(); } }
    void mousePressEvent(QMouseEvent* ev) override
    {
        const QVector<End> e = ends();
        int best = -1; double bd = 1e9;
        for (int i = 0; i < e.size(); ++i) { const double d = QLineF(ev->position(), e[i].p).length(); if (d < bd) { bd = d; best = i; } }
        if (best < 0 || bd > 9.5) return;
        float yaw = m_host->camYaw(), pitch = 0.0f;
        const bool pos = e[best].pos;
        switch (e[best].axis) {
        case 0: yaw = pos ? kPi / 2 : -kPi / 2; break;
        case 1: pitch = pos ? 1.55f : -1.55f;   break;
        case 2: yaw = pos ? 0.0f : kPi;         break;
        }
        m_host->orbitToAxis(yaw, pitch);
        ev->accept();
    }
    void mouseDoubleClickEvent(QMouseEvent* ev) override
    {
        for (const End& n : ends()) if (QLineF(ev->position(), n.p).length() <= 9.5) { ev->accept(); return; }
        m_host->setOrthographic(!m_host->orthographic());
        ev->accept();
    }
private:
    GLModelWidget* m_host;
    bool m_hover = false;
    int  m_hotEnd = -1;
};

// Transparent overlay child that paints labels at pre-projected screen positions. Click-through.
class LabelOverlay : public QWidget {
public:
    explicit LabelOverlay(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
    }
    struct Item { QPoint pos; QString text; };
    QVector<Item> items;
protected:
    void paintEvent(QPaintEvent*) override {
        if (items.isEmpty()) return;
        QPainter g(this);
        g.setRenderHint(QPainter::TextAntialiasing, true);
        QFont f = g.font(); f.setPointSizeF(8.0); g.setFont(f);
        for (const auto& it : items) {
            const QRect r(it.pos + QPoint(4, -7), QSize(240, 14));
            g.setPen(QColor(0, 0, 0, 190));
            g.drawText(r.translated(1, 1), Qt::AlignLeft | Qt::AlignVCenter, it.text);
            g.setPen(QColor(110, 235, 165));
            g.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, it.text);
        }
    }
};

// Alpha-cutout surfaces: edge-dilate the RGB (kills the black halo) and soften the alpha band so
// MSAA can anti-alias a 1-bit-ish mask. Lifted from D4.
QImage dilateOpaqueRGB(QImage img, int passes)
{
    const int W = img.width(), H = img.height();
    if (W < 2 || H < 2) return img;
    QByteArray valid(qsizetype(W) * H, 0);
    for (int y = 0; y < H; ++y) {
        const uchar* s = img.constScanLine(y);
        for (int x = 0; x < W; ++x) valid[qsizetype(y) * W + x] = (s[x * 4 + 3] >= 89) ? 1 : 0;
    }
    for (int pass = 0; pass < passes; ++pass) {
        const QImage prev = img;
        const QByteArray prevValid = valid;
        bool changed = false;
        for (int y = 0; y < H; ++y) {
            uchar* d = img.scanLine(y);
            for (int x = 0; x < W; ++x) {
                if (prevValid[qsizetype(y) * W + x]) continue;
                int r = 0, g = 0, b = 0, n = 0;
                auto tap = [&](int xx, int yy) {
                    if (xx < 0 || yy < 0 || xx >= W || yy >= H) return;
                    if (!prevValid[qsizetype(yy) * W + xx]) return;
                    const uchar* s = prev.constScanLine(yy);
                    r += s[xx * 4]; g += s[xx * 4 + 1]; b += s[xx * 4 + 2]; ++n;
                };
                tap(x - 1, y); tap(x + 1, y); tap(x, y - 1); tap(x, y + 1);
                if (n) { d[x * 4] = uchar(r / n); d[x * 4 + 1] = uchar(g / n); d[x * 4 + 2] = uchar(b / n); valid[qsizetype(y) * W + x] = 1; changed = true; }
            }
        }
        if (!changed) break;
    }
    return img;
}

void blurAlphaChannel(QImage& img, int r)
{
    const int W = img.width(), H = img.height();
    if (W < 3 || H < 3 || r < 1) return;
    QByteArray a(qsizetype(W) * H, 0);
    for (int y = 0; y < H; ++y) { const uchar* s = img.constScanLine(y); for (int x = 0; x < W; ++x) a[qsizetype(y) * W + x] = char(s[x * 4 + 3]); }
    QByteArray tmp(a.size(), 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int sum = 0, c = 0;
            for (int dx = -r; dx <= r; ++dx) { const int xx = x + dx; if (xx < 0 || xx >= W) continue; sum += uchar(a[qsizetype(y) * W + xx]); ++c; }
            tmp[qsizetype(y) * W + x] = char(sum / c);
        }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int sum = 0, c = 0;
            for (int dy = -r; dy <= r; ++dy) { const int yy = y + dy; if (yy < 0 || yy >= H) continue; sum += uchar(tmp[qsizetype(yy) * W + x]); ++c; }
            a[qsizetype(y) * W + x] = char(sum / c);
        }
    for (int y = 0; y < H; ++y) { uchar* d = img.scanLine(y); for (int x = 0; x < W; ++x) d[x * 4 + 3] = uchar(a[qsizetype(y) * W + x]); }
}

}  // namespace

QString GLModelWidget::s_glInfo;

GLModelWidget::GLModelWidget(QWidget* parent) : QOpenGLWidget(parent)
{
    applyRig();
    m_gizmo = new AxisGizmoOverlay(this);
    m_gizmo->setVisible(m_overlaysOn && m_showAxisGizmo);
    m_clock.start();
}

GLModelWidget::~GLModelWidget()
{
    // The context-loss hook must not outlive THIS class: QOpenGLWidget's own destructor tears the
    // context down after our members are gone, and the hook would run over freed memory (caught by
    // ASAN in the parity harness the first time it ran).
    disconnect(m_ctxLostConn);
    if (context()) {
        makeCurrent();
        if (m_ext) m_ext->destroyGL(*this);
        destroyBuffers();
        if (m_prog) glDeleteProgram(m_prog);
        if (m_bgProg) glDeleteProgram(m_bgProg);
        if (m_bgVao) glDeleteVertexArrays(1, &m_bgVao);
        if (m_shadowProg) glDeleteProgram(m_shadowProg);
        if (m_posProg)    glDeleteProgram(m_posProg);
        if (m_shadowFbo)  glDeleteFramebuffers(1, &m_shadowFbo);
        if (m_posFbo)     glDeleteFramebuffers(1, &m_posFbo);
        if (m_posDepth)   glDeleteRenderbuffers(1, &m_posDepth);
        if (m_shadowTex)  glDeleteTextures(1, &m_shadowTex);
        if (m_posTex)     glDeleteTextures(1, &m_posTex);
        if (m_reflCube)   glDeleteTextures(1, &m_reflCube);
        if (m_lutTex)     glDeleteTextures(1, &m_lutTex);
        const GLuint vaos[] = { m_skelVao, m_hpVao, m_gridVao, m_lineVao };
        const GLuint vbos[] = { m_skelVbo, m_hpVbo, m_gridVbo, m_lineVbo };
        for (GLuint v : vaos) if (v) glDeleteVertexArrays(1, &v);
        for (GLuint b : vbos) if (b) glDeleteBuffers(1, &b);
        doneCurrent();
    }
}

void GLModelWidget::setExtension(ViewportExtension* ext) { m_ext = ext; }

void GLModelWidget::releaseGL()
{
    // Handles only — the context that owned them is gone. CPU-side state is intact and re-uploads.
    m_prog = m_shadowProg = m_posProg = m_bgProg = 0;
    m_vao = m_vbo = m_ibo = m_bgVao = 0;
    m_shadowFbo = m_shadowTex = m_posFbo = m_posTex = m_posDepth = 0;
    m_posW = m_posH = -1;
    m_reflCube = m_lutTex = 0;
    m_gridVao = m_gridVbo = m_skelVao = m_skelVbo = m_hpVao = m_hpVbo = m_lineVao = m_lineVbo = 0;
    m_gridVerts = m_skelVerts = m_hpAxisVerts = 0;
    m_indexCount = 0;
    m_thumbFbo.reset(); m_thumbFboSize = 0;
    m_matTex.clear();
    m_uniLoc.clear();
    m_hasPending = !m_verts.isEmpty();
    m_hasPendingTex = !m_materials.isEmpty();
    m_hasLut = false; m_lutDirty = !m_pendingLut.isNull();
}

// Memoized glGetUniformLocation: first query per (program, name) hits the driver; every later
// frame reads the cached location. -1 for a null program (matches GL's "not found").
GLint GLModelWidget::uni(GLuint prog, const char* name)
{
    if (!prog) return -1;
    QHash<QByteArray, GLint>& m = m_uniLoc[prog];
    const auto it = m.constFind(QByteArray::fromRawData(name, int(qstrlen(name))));
    if (it != m.constEnd()) return it.value();
    const GLint loc = glGetUniformLocation(prog, name);
    m.insert(QByteArray(name), loc);
    return loc;
}

GLuint GLModelWidget::compileProgram(const QByteArray& vsrc, const QByteArray& fsrc, const char* what)
{
    auto compile = [&](GLenum type, const QByteArray& src) -> GLuint {
        GLuint s = glCreateShader(type);
        const char* p = src.constData();
        glShaderSource(s, 1, &p, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            m_error = QStringLiteral("%1 %2 shader: %3").arg(QLatin1String(what), type == GL_VERTEX_SHADER ? QStringLiteral("vertex") : QStringLiteral("fragment"), QString::fromLatin1(log));
            qWarning("GLModelWidget: %s", qUtf8Printable(m_error));
            glDeleteShader(s);
            return 0;
        }
        return s;
    };
    const GLuint vs = compile(GL_VERTEX_SHADER, vsrc);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, fsrc);
    if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return 0; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        m_error = QStringLiteral("%1 link: %2").arg(QLatin1String(what), QString::fromLatin1(log));
        qWarning("GLModelWidget: %s", qUtf8Printable(m_error));
        glDeleteProgram(prog); prog = 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

// ── Scene ──────────────────────────────────────────────────────────────────────────────────────

void GLModelWidget::setScene(const Geometry& geo, const Skeleton& skeleton, bool keepView)
{
    m_verts.clear(); m_indices.clear(); m_parts.clear();
    m_followParts.clear();
    // Stale part indices from the previous model: an index still IN RANGE for the new model would
    // outline the WRONG part rather than be filtered out.
    m_selected.clear(); m_context.clear(); m_active = -1;
    m_vJoints.clear(); m_vWeights.clear();
    m_bindVerts.clear();   // before clearClip(), which restores the bind pose into m_verts
    m_skeleton = skeleton;
    clearClip();
    if (m_ext) m_ext->sceneChanged();

    const int nv = geo.vertices.size();
    m_verts.reserve(qsizetype(nv) * kStride);
    m_indices = geo.indices;
    m_vJoints.reserve(nv);
    m_vWeights.reserve(nv);

    // Tangents: authored when the mesh has them, else Lengyel from positions + UVs over every
    // part's triangles (what D4 does for every mesh — D4 carries no authored tangent).
    QVector<QVector3D> tan;
    if (!geo.hasTangent) {
        tan.fill(QVector3D(0, 0, 0), nv);
        for (const core::Part& part : geo.parts) {
            const int end = qMin(int(part.indexStart + part.indexCount), m_indices.size());
            for (int t = int(part.indexStart); t + 2 < end; t += 3) {
                const int i0 = int(m_indices[t]), i1 = int(m_indices[t + 1]), i2 = int(m_indices[t + 2]);
                if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= nv || i1 >= nv || i2 >= nv) continue;
                const Vertex& a = geo.vertices[i0]; const Vertex& b = geo.vertices[i1]; const Vertex& c = geo.vertices[i2];
                const QVector3D e1(b.px - a.px, b.py - a.py, b.pz - a.pz);
                const QVector3D e2(c.px - a.px, c.py - a.py, c.pz - a.pz);
                const float du1 = b.u - a.u, dv1 = b.v - a.v, du2 = c.u - a.u, dv2 = c.v - a.v;
                const float det = du1 * dv2 - du2 * dv1;
                const float r = (std::fabs(det) > 1e-8f) ? 1.0f / det : 0.0f;
                const QVector3D tg = (e1 * dv2 - e2 * dv1) * r;
                tan[i0] += tg; tan[i1] += tg; tan[i2] += tg;
            }
        }
    }
    const int nbJ = m_skeleton.size();
    int paletteMax = -1;
    QVector3D mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
    for (int vi = 0; vi < nv; ++vi) {
        const Vertex& v = geo.vertices[vi];
        QVector3D tg;
        if (geo.hasTangent) tg = QVector3D(v.tx, v.ty, v.tz);
        else {
            const QVector3D n(v.nx, v.ny, v.nz);
            tg = tan[vi] - n * QVector3D::dotProduct(n, tan[vi]);
            if (tg.lengthSquared() < 1e-12f) {
                tg = QVector3D::crossProduct(n, QVector3D(0, 1, 0));
                if (tg.lengthSquared() < 1e-12f) tg = QVector3D::crossProduct(n, QVector3D(1, 0, 0));
            }
            tg.normalize();
        }
        m_verts << v.px << v.py << v.pz << v.nx << v.ny << v.nz << v.u << v.v << tg.x() << tg.y() << tg.z();
        // The exporter's scar 7, applied here too: an out-of-range joint index has its WEIGHT
        // zeroed, not just the index clamped, so a corrupt palette leaves the vertex at bind
        // instead of dragging it onto the root bone.
        std::array<quint16, 4> J{{v.joints[0], v.joints[1], v.joints[2], v.joints[3]}};
        std::array<float, 4>   W{{v.weights[0], v.weights[1], v.weights[2], v.weights[3]}};
        for (int k = 0; k < 4; ++k) {
            if (W[k] > 0.0f && int(J[k]) > paletteMax) paletteMax = int(J[k]);
            if (nbJ <= 0 || int(J[k]) >= nbJ) { J[k] = 0; W[k] = 0.0f; }
        }
        m_vJoints.push_back(J);
        m_vWeights.push_back(W);
        mn.setX(qMin(mn.x(), v.px)); mn.setY(qMin(mn.y(), v.py)); mn.setZ(qMin(mn.z(), v.pz));
        mx.setX(qMax(mx.x(), v.px)); mx.setY(qMax(mx.y(), v.py)); mx.setZ(qMax(mx.z(), v.pz));
    }
    m_skinned = geo.skinned && paletteMax >= 0;
    m_skelMatchesMesh = m_skinned && nbJ > 0 && paletteMax < nbJ;

    int pi = 0;
    for (const core::Part& p : geo.parts) {
        Part part;
        part.offset = int(p.indexStart);
        part.count = int(qMin<quint32>(p.indexCount, quint32(qMax(0, m_indices.size() - int(p.indexStart)))));
        part.visible = p.visible;
        part.material = p.materialIndex;
        part.doubleSided = p.doubleSided;
        part.name = p.name.isEmpty() ? QStringLiteral("part %1").arg(pi) : p.name;
        // A part inside a hidden group is hidden (FOX's authored-hidden alternatives).
        for (int g = p.group; g >= 0 && g < geo.groups.size(); g = geo.groups[g].parent)
            if (!geo.groups[g].visible) { part.visible = false; break; }
        m_parts.push_back(part);
        ++pi;
    }
    if (m_verts.isEmpty() || m_indices.isEmpty()) { clearScene(); return; }
    m_bindVerts = m_verts;

    m_gridVerts = 0; m_skelVerts = 0; m_hpAxisVerts = 0;
    m_homeCenter = (mn + mx) * 0.5f;
    m_radius = qMax(0.001f, (mx - mn).length() * 0.5f);
    if (!keepView) {
        m_center = m_homeCenter;
        m_dist = m_radius * 2.6f;
        m_yaw = 0.6f;
        m_pitch = 0.25f;
        emit cameraChanged();
    }
    m_hasPending = true;
    m_hasPendingTex = true;   // material roster may have changed size
    m_error.clear();
    update();
}

void GLModelWidget::setMaterials(const QVector<Material>& materials)
{
    m_materials = materials;
    m_hasPendingTex = true;
    update();
}

void GLModelWidget::setHardpoints(const QVector<Hardpoint>& hps)
{
    m_hardpoints = hps;
    m_hpAxisVerts = 0;
    if (m_showHardpoints) update();
}

void GLModelWidget::clearScene()
{
    m_hasPending = false;
    m_verts.clear(); m_indices.clear(); m_parts.clear();
    m_bindVerts.clear(); m_vJoints.clear(); m_vWeights.clear();
    m_skeleton = Skeleton{};
    m_materials.clear();
    m_hardpoints.clear();
    m_skinned = m_skelMatchesMesh = false;
    m_selected.clear(); m_context.clear(); m_active = -1;
    m_hasClip = false; m_clip = Clip{}; m_time = 0;
    if (m_ext) m_ext->sceneChanged();
    if (context()) { makeCurrent(); destroyBuffers(); doneCurrent(); }
    m_indexCount = 0;
    update();
}

// ── Animation ──────────────────────────────────────────────────────────────────────────────────

void GLModelWidget::setClip(const Clip& clip)
{
    m_clip = clip;
    m_hasClip = !clip.tracks.isEmpty() && !m_skeleton.isEmpty() && m_skelMatchesMesh;
    if (m_ext) m_ext->clipChanged();
    setTime(0.0f);
}

void GLModelWidget::clearClip()
{
    m_hasClip = false;
    m_clip = Clip{};
    m_time = 0.0f;
    m_followParts.clear();
    if (m_ext) m_ext->clipChanged();
    if (!m_bindVerts.isEmpty()) { m_verts = m_bindVerts; m_hasPending = true; update(); }
}

void GLModelWidget::setTime(float seconds)
{
    if (!m_hasClip) return;
    m_time = qBound(0.0f, seconds, qMax(0.0f, m_clip.duration));
    applySkinning();
    // Camera Snap + follow: pan the target to the followed parts' live position (keep zoom/angle).
    if (!m_followParts.isEmpty()) {
        QVector3D c; float r;
        if (partsBounds(m_followParts, c, r)) {
            if (m_camAnim) m_camAnim->stop();
            m_center = c; m_tgtCenter = c;
        }
    }
    if (m_showSkeleton) m_skelVerts = 0;
    if (m_showHardpoints) m_hpAxisVerts = 0;
    m_hasPending = true;
    update();
}

void GLModelWidget::setFrame(int f)
{
    if (!m_hasClip || m_clip.fps <= 0) return;
    const int n = qMax(1, m_clip.frameCount());
    setTime(float(qBound(0, f, n - 1)) / m_clip.fps);
}

int GLModelWidget::frame() const
{
    return (m_hasClip && m_clip.fps > 0) ? int(std::lround(m_time * m_clip.fps)) : 0;
}

void GLModelWidget::setPlaybackTimer(QTimer* t) { m_playbackTimer = t; }
QTimer* GLModelWidget::playbackTimer() const { return qobject_cast<QTimer*>(m_playbackTimer.data()); }

bool GLModelWidget::animPlaying() const
{
    const QTimer* t = qobject_cast<QTimer*>(m_playbackTimer.data());
    return t && t->isActive();
}

bool GLModelWidget::snapshotPose(Geometry& geo) const
{
    if (geo.vertices.size() * kStride != m_verts.size() || geo.vertices.isEmpty()) return false;
    for (int vi = 0; vi < geo.vertices.size(); ++vi) {
        Vertex& v = geo.vertices[vi];
        const float* s = m_verts.constData() + qsizetype(vi) * kStride;
        v.px = s[0]; v.py = s[1]; v.pz = s[2];
        v.nx = s[3]; v.ny = s[4]; v.nz = s[5];
        v.joints[0] = v.joints[1] = v.joints[2] = v.joints[3] = 0;
        v.weights[0] = v.weights[1] = v.weights[2] = v.weights[3] = 0.0f;
    }
    geo.skinned = false;
    geo.computeBounds();
    return true;
}

QVector<rm::Mat4> GLModelWidget::currentWorldPose() const
{
    return worldPose(m_skeleton, localPose(m_skeleton, m_hasClip ? &m_clip : nullptr, m_time));
}

void GLModelWidget::applySkinning()
{
    if (!m_skinned || m_skeleton.isEmpty() || m_bindVerts.isEmpty()) { m_verts = m_bindVerts; return; }
    QVector<rm::Mat4> world = currentWorldPose();
    if (m_ext) {
        const qint64 now = m_clock.elapsed();
        const float dt = (m_lastPoseMs < 0) ? 0.0f : float(now - m_lastPoseMs) * 0.001f;
        m_lastPoseMs = now;
        m_ext->modifyPose(world, m_skeleton.nBaseBones, dt);
    }
    const QVector<rm::Mat4> palette = skinPalette(m_skeleton, world);
    const int nb = palette.size();

    m_verts = m_bindVerts;
    const int vcount = m_vJoints.size();
    for (int v = 0; v < vcount; ++v) {
        const float* bp = m_bindVerts.constData() + qsizetype(v) * kStride;
        const auto& J = m_vJoints[v];
        const auto& W = m_vWeights[v];
        const float wsum = W[0] + W[1] + W[2] + W[3];
        if (wsum <= 0.0f) continue;   // unskinned vertex keeps bind pose
        float px = 0, py = 0, pz = 0, nx = 0, ny = 0, nz = 0, tx = 0, ty = 0, tz = 0;
        for (int k = 0; k < 4; ++k) {
            const float w = W[k];
            if (w == 0.0f) continue;
            const int b = J[k];
            if (b < 0 || b >= nb) continue;
            // Row-vector v·M on the row-major palette: x' = x m[0] + y m[4] + z m[8] + m[12].
            const rm::Mat4& m = palette[b];
            px += w * (m[0]*bp[0] + m[4]*bp[1] + m[8]*bp[2]  + m[12]);
            py += w * (m[1]*bp[0] + m[5]*bp[1] + m[9]*bp[2]  + m[13]);
            pz += w * (m[2]*bp[0] + m[6]*bp[1] + m[10]*bp[2] + m[14]);
            nx += w * (m[0]*bp[3] + m[4]*bp[4] + m[8]*bp[5]);
            ny += w * (m[1]*bp[3] + m[5]*bp[4] + m[9]*bp[5]);
            nz += w * (m[2]*bp[3] + m[6]*bp[4] + m[10]*bp[5]);
            tx += w * (m[0]*bp[8] + m[4]*bp[9] + m[8]*bp[10]);
            ty += w * (m[1]*bp[8] + m[5]*bp[9] + m[9]*bp[10]);
            tz += w * (m[2]*bp[8] + m[6]*bp[9] + m[10]*bp[10]);
        }
        float* out = m_verts.data() + qsizetype(v) * kStride;
        out[0] = px; out[1] = py; out[2] = pz;
        const float nl = std::sqrt(nx*nx + ny*ny + nz*nz);
        if (nl > 1e-9f) { out[3] = nx/nl; out[4] = ny/nl; out[5] = nz/nl; }
        const float tl = std::sqrt(tx*tx + ty*ty + tz*tz);
        if (tl > 1e-9f) { out[8] = tx/tl; out[9] = ty/tl; out[10] = tz/tl; }
    }
}

// ── GL lifetime ────────────────────────────────────────────────────────────────────────────────

void GLModelWidget::initializeGL()
{
    const bool funcsOk = initializeOpenGLFunctions();
    // Everything from a previous context is gone. Drop the handles (never delete them — the
    // context that owned them no longer exists) and re-upload from the CPU copies.
    releaseGL();
    {
        QOpenGLContext* ctx = context();
        const QSurfaceFormat f = ctx ? ctx->format() : QSurfaceFormat();
        qInfo("GLModelWidget GL: funcs=%d version=%d.%d renderer=%s", funcsOk ? 1 : 0,
              f.majorVersion(), f.minorVersion(), reinterpret_cast<const char*>(glGetString(GL_VERSION)));
        s_glInfo = QStringLiteral("%1 — GL %2")
            .arg(QString::fromLatin1(reinterpret_cast<const char*>(glGetString(GL_RENDERER))),
                 QString::fromLatin1(reinterpret_cast<const char*>(glGetString(GL_VERSION))));
        if (ctx) {
            // FOX's context-loss handling: when the context is torn down (widget reparented to
            // another top-level, driver reset), release the GL side while it is still current.
            disconnect(m_ctxLostConn);
            m_ctxLostConn = connect(ctx, &QOpenGLContext::aboutToBeDestroyed, this, [this] {
                makeCurrent();
                if (m_ext) m_ext->destroyGL(*this);
                destroyBuffers();
                if (m_prog) glDeleteProgram(m_prog);
                if (m_shadowProg) glDeleteProgram(m_shadowProg);
                if (m_posProg) glDeleteProgram(m_posProg);
                if (m_bgProg) glDeleteProgram(m_bgProg);
                releaseGL();
                doneCurrent();
            }, Qt::DirectConnection);
        }
    }
    glClearColor(0.10f, 0.10f, 0.11f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    m_maxAniso = 1.0f;
    glGetFloatv(0x84FF, &m_maxAniso);   // GL_MAX_TEXTURE_MAX_ANISOTROPY (EXT token == 4.6 core token)

    const ShaderSources src = m_ext ? assembleShaders(m_ext->shaderBlocks(), m_ext->vertexShader(), m_ext->fragmentShader())
                                    : assembleShaders(ShaderBlocks{});
    m_prog       = compileProgram(src.vert, src.frag, "main");
    m_shadowProg = compileProgram(src.shadowVert, src.shadowFrag, "shadow");
    m_posProg    = compileProgram(src.posVert, src.posFrag, "ssao-pos");
    m_bgProg     = compileProgram(src.bgVert, src.bgFrag, "backdrop");
    if (m_bgProg) glGenVertexArrays(1, &m_bgVao);   // core profile needs A bound VAO

    // Shadow-map depth FBO.
    glGenFramebuffers(1, &m_shadowFbo);
    glGenTextures(1, &m_shadowTex);
    glBindTexture(GL_TEXTURE_2D, m_shadowTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, m_shadowSize, m_shadowSize, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const float sbord[4] = {1.0f, 1.0f, 1.0f, 1.0f};   // outside the map = far (fully lit)
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, sbord);
    glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_shadowTex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, targetFbo());
    m_shadowResDirty = false;

    // SSAO G-buffer (storage sized on demand in renderPos).
    glGenFramebuffers(1, &m_posFbo);
    glGenTextures(1, &m_posTex);
    glGenRenderbuffers(1, &m_posDepth);

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ibo);
    if (m_ext) m_ext->initializeGL(*this);
}

void GLModelWidget::resizeGL(int w, int h)
{
    m_fbW = w; m_fbH = h;
    glViewport(0, 0, w, h);
    if (m_overlayLabel) m_overlayLabel->setGeometry(rect());
    if (m_gizmo) { m_gizmo->setGeometry(width() - 96, 8, 88, 88); m_gizmo->raise(); }
}

void GLModelWidget::destroyBuffers()
{
    if (m_ibo) { glDeleteBuffers(1, &m_ibo); m_ibo = 0; }
    if (m_vbo) { glDeleteBuffers(1, &m_vbo); m_vbo = 0; }
    if (m_vao) { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
    m_thumbFbo.reset(); m_thumbFboSize = 0;
    destroyTextures();
    m_indexCount = 0;
}

void GLModelWidget::uploadPending()
{
    m_hasPending = false;
    if (m_vao == 0) glGenVertexArrays(1, &m_vao);
    if (m_vbo == 0) glGenBuffers(1, &m_vbo);
    if (m_ibo == 0) glGenBuffers(1, &m_ibo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, m_verts.size() * sizeof(float), m_verts.constData(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_indices.size() * sizeof(quint32), m_indices.constData(), GL_STATIC_DRAW);
    const GLsizei st = kStride * sizeof(float);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(6 * sizeof(float)));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(8 * sizeof(float)));
    glBindVertexArray(0);
    m_indexCount = m_indices.size();
}

void GLModelWidget::uploadLineVao(GLuint& vao, GLuint& vbo, const QVector<float>& v11, bool dynamic)
{
    if (vao == 0) glGenVertexArrays(1, &vao);
    if (vbo == 0) glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, v11.size() * sizeof(float), v11.constData(), dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    const GLsizei st = kStride * sizeof(float);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(6 * sizeof(float)));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(8 * sizeof(float)));
    glBindVertexArray(0);
}

// ── Textures ───────────────────────────────────────────────────────────────────────────────────

void GLModelWidget::uploadTextures()
{
    m_hasPendingTex = false;
    destroyTextures();
    auto uploadOne = [this](const QImage& srcImg, bool dilate) -> GLuint {
        if (srcImg.isNull()) return 0;
        QImage img = srcImg.convertToFormat(QImage::Format_RGBA8888);
        if (dilate) {
            const int W2 = img.width(), H2 = img.height();
            bool hasCut = false;
            for (int y = 0; y < H2 && !hasCut; ++y) { const uchar* s = img.constScanLine(y);
                for (int x = 0; x < W2; ++x) if (s[x * 4 + 3] == 0) { hasCut = true; break; } }
            img = dilateOpaqueRGB(img, 6);
            if (hasCut) blurAlphaChannel(img, 2);
        }
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width(), img.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
        glGenerateMipmap(GL_TEXTURE_2D);
        if (m_maxAniso > 1.0f) glTexParameterf(GL_TEXTURE_2D, 0x84FE, qMin(m_maxAniso, 8.0f));   // GL_TEXTURE_MAX_ANISOTROPY
        return tex;
    };
    // One GL texture per distinct image (QImage::cacheKey), shared across materials.
    QHash<qint64, GLuint> byKey;
    auto get = [&](const TextureImage& ti, bool dilate) -> GLuint {
        if (ti.isNull()) return 0;
        const qint64 key = ti.image.cacheKey();
        const auto it = byKey.constFind(key);
        if (it != byKey.constEnd()) return it.value();
        const GLuint t = uploadOne(ti.image, dilate);
        byKey.insert(key, t);
        return t;
    };
    m_matTex.resize(m_materials.size());
    for (int i = 0; i < m_materials.size(); ++i) {
        const Material& m = m_materials[i];
        MatTex& mt = m_matTex[i];
        mt.base      = get(m.baseColor, m.alphaMode == Material::Mask);   // cutouts: edge-bleed
        mt.normal    = get(m.normal, false);
        mt.orm       = get(m.orm, false);
        mt.emissive  = get(m.emissive, false);
        mt.specColor = get(m.specularColor, false);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    if (m_ext && m_ext->texturesPending()) m_ext->uploadTextures(*this, m_parts.size());
}

void GLModelWidget::destroyTextures()
{
    QSet<GLuint> uniq;
    for (const MatTex& mt : m_matTex) for (GLuint t : {mt.base, mt.normal, mt.orm, mt.emissive, mt.specColor}) if (t) uniq.insert(t);
    for (GLuint t : uniq) glDeleteTextures(1, &t);
    m_matTex.clear();
}

void GLModelWidget::setReflectionCubemap(const QVector<QImage>& faces)
{
    m_pendingRefl = faces.size() >= 6 ? faces : QVector<QImage>{};
    m_hasPendingRefl = true;
    update();
}

void GLModelWidget::uploadReflectionCubemap()
{
    m_hasPendingRefl = false;
    if (m_pendingRefl.size() < 6) {
        if (m_reflCube) { glDeleteTextures(1, &m_reflCube); m_reflCube = 0; }
        return;
    }
    const int sz = m_pendingRefl[0].width();
    for (const QImage& f : m_pendingRefl) if (f.isNull() || f.width() != sz || f.height() != sz) return;   // bad layout → fallback
    if (m_reflCube == 0) glGenTextures(1, &m_reflCube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_reflCube);
    for (int f = 0; f < 6; ++f) {
        // HDR faces stay half-float; anything else becomes RGBA8.
        if (m_pendingRefl[f].format() == QImage::Format_RGBA16FPx4) {
            const QImage& img = m_pendingRefl[f];
            glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA16F, sz, sz, 0, GL_RGBA, GL_HALF_FLOAT, img.constBits());
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        } else {
            const QImage img = m_pendingRefl[f].convertToFormat(QImage::Format_RGBA8888);
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, sz, sz, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
        }
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    m_reflMaxMip = int(std::floor(std::log2(float(qMax(1, sz)))));
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    m_pendingRefl.clear();
}

// ── Passes ─────────────────────────────────────────────────────────────────────────────────────

void GLModelWidget::drawPartIndices(int part)
{
    if (part < 0 || part >= m_parts.size()) return;
    const Part& p = m_parts[part];
    if (p.count == 0) return;
    glDrawElements(GL_TRIANGLES, p.count, GL_UNSIGNED_INT, reinterpret_cast<void*>(qintptr(p.offset) * sizeof(quint32)));
}

// SSAO prepass: the model's world-space position into the RGBA16F G-buffer (a=1 geometry).
void GLModelWidget::renderPos(const QMatrix4x4& mvp, const QMatrix4x4& model)
{
    if (m_posFbo == 0 || m_posProg == 0 || m_indexCount == 0) return;
    if (m_posW != m_fbW || m_posH != m_fbH) {
        m_posW = m_fbW; m_posH = m_fbH;
        glBindTexture(GL_TEXTURE_2D, m_posTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_posW, m_posH, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindRenderbuffer(GL_RENDERBUFFER, m_posDepth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, m_posW, m_posH);
        glBindFramebuffer(GL_FRAMEBUFFER, m_posFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_posTex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_posDepth);
        glBindFramebuffer(GL_FRAMEBUFFER, targetFbo());
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_posFbo);
    glViewport(0, 0, m_posW, m_posH);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(m_posProg);
    glUniformMatrix4fv(uni(m_posProg, "uMVP"), 1, GL_FALSE, mvp.constData());
    glUniformMatrix4fv(uni(m_posProg, "uModel"), 1, GL_FALSE, model.constData());
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(m_vao);
    // Same visibility as the main pass so HIDDEN parts don't occlude in SSAO (a hidden mesh would
    // otherwise leave a dark halo where it used to be). Blended parts and extension-owned parts
    // don't occlude either.
    for (int i = 0; i < m_parts.size(); ++i) {
        const Part& p = m_parts[i];
        if (!p.visible || p.count == 0) continue;
        if (m_ext && !m_ext->partInMainPass(i)) continue;
        const Material* m = (p.material >= 0 && p.material < m_materials.size()) ? &m_materials[p.material] : nullptr;
        if (m && (m->alphaMode == Material::Blend || m->alphaMode == Material::Additive)) continue;
        drawPartIndices(i);
    }
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, targetFbo());
    glViewport(0, 0, m_fbW, m_fbH);
}

// Depth-only pass from the key light's POV into the shadow map.
void GLModelWidget::renderShadow(const QMatrix4x4& lightMvp)
{
    if (m_shadowFbo == 0 || m_shadowProg == 0 || m_indexCount == 0) return;
    if (m_shadowResDirty) {
        m_shadowResDirty = false;
        glBindTexture(GL_TEXTURE_2D, m_shadowTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, m_shadowSize, m_shadowSize, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFbo);
    glViewport(0, 0, m_shadowSize, m_shadowSize);
    glClear(GL_DEPTH_BUFFER_BIT);
    glUseProgram(m_shadowProg);
    glUniformMatrix4fv(uni(m_shadowProg, "uLightMVP"), 1, GL_FALSE, lightMvp.constData());
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(m_vao);
    for (int i = 0; i < m_parts.size(); ++i) {
        const Part& p = m_parts[i];
        if (!p.visible || p.count == 0) continue;                 // hidden → no shadow
        if (m_ext && !m_ext->partInMainPass(i)) continue;         // transparent FX → no shadow
        const Material* m = (p.material >= 0 && p.material < m_materials.size()) ? &m_materials[p.material] : nullptr;
        if (m && (m->alphaMode == Material::Blend || m->alphaMode == Material::Additive)) continue;
        drawPartIndices(i);
    }
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, targetFbo());
    glViewport(0, 0, m_fbW, m_fbH);
}

void GLModelWidget::drawLines(const QVector<float>& xyz, float r, float g, float b)
{
    if (xyz.size() < 6 || m_prog == 0) return;
    QVector<float> v; v.reserve(xyz.size() / 3 * kStride);
    for (int i = 0; i + 2 < xyz.size(); i += 3) v << xyz[i] << xyz[i + 1] << xyz[i + 2] << 0 << 1 << 0 << 0 << 0 << 1 << 0 << 0;
    uploadLineVao(m_lineVao, m_lineVbo, v, true);
    glDisable(GL_DEPTH_TEST);
    glUniform1i(uni(m_prog, "uHasTex"), 0); glUniform1i(uni(m_prog, "uHasNormal"), 0);
    glUniform1i(uni(m_prog, "uHasEmissive"), 0); glUniform1i(uni(m_prog, "uPbr"), 0);
    glUniform1i(uni(m_prog, "uAlphaMode"), 0);
    glUniform3f(uni(m_prog, "uBase"), r, g, b);
    glBindVertexArray(m_lineVao);
    glDrawArrays(GL_LINES, 0, v.size() / kStride);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}

// ── The frame ──────────────────────────────────────────────────────────────────────────────────

void GLModelWidget::cameraBasis(QVector3D& eye, QMatrix4x4& proj, QMatrix4x4& view, float aspect) const
{
    const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
    const float cy = std::cos(m_yaw),   sy = std::sin(m_yaw);
    const QVector3D dir(cp * sy, sp, cp * cy);
    eye = m_center + dir * m_dist;
    proj = QMatrix4x4();
    if (m_ortho) {
        // Match the perspective framing at the orbit centre: half-height = dist·tan(fov/2). Depth
        // range scaled by BOTH dist and radius so it never collapses when m_radius is stale.
        const float h = m_dist * std::tan(m_fov * 0.5f * kPi / 180.0f);
        const float w = h * aspect;
        const float ext = qMax(m_radius, m_dist * 0.5f) * 10.0f;
        proj.ortho(-w, w, -h, h, qMax(0.001f, m_dist - ext), m_dist + ext);
    } else {
        // Far plane follows the camera distance so very large models never clip into it.
        proj.perspective(m_fov, aspect, qMax(0.001f, m_radius * 0.02f), qMax(m_radius * 20.0f, m_dist + m_radius * 4.0f));
    }
    view = QMatrix4x4();
    view.lookAt(eye, m_center, QVector3D(0, 1, 0));
}

void GLModelWidget::paintGL()
{
    if (m_hasPending) uploadPending();
    if (m_hasPendingTex) uploadTextures();
    if (m_hasPendingRefl) uploadReflectionCubemap();

    // a=0 → native-alpha capture. Coverage mode clears to 0 as well but keeps DRAWING the backdrop,
    // so the colour is the normal opaque render while the alpha channel still says where the model
    // is. That is what lets "crop to model" find the subject without a second render.
    glClearColor(m_bg[0], m_bg[1], m_bg[2], (m_transparentClear || m_coverageAlpha) ? 0.0f : 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (m_bgGradient && !m_transparentClear && m_bgProg) {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glUseProgram(m_bgProg);
        glUniform3f(uni(m_bgProg, "uTop"), qMin(1.0f, m_bg[0] * 1.55f + 0.05f), qMin(1.0f, m_bg[1] * 1.55f + 0.05f), qMin(1.0f, m_bg[2] * 1.55f + 0.05f));
        glUniform3f(uni(m_bgProg, "uBot"), m_bg[0] * 0.40f, m_bg[1] * 0.40f, m_bg[2] * 0.40f);
        glUniform1f(uni(m_bgProg, "uA"), m_coverageAlpha ? 0.0f : 1.0f);
        glBindVertexArray(m_bgVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
    }
    if (m_indexCount == 0 || m_prog == 0) return;

    const float aspect = height() > 0 ? float(width()) / float(height()) : 1.0f;
    QVector3D eye; QMatrix4x4 proj, view;
    cameraBasis(eye, proj, view, aspect);
    const QMatrix4x4 model;   // identity
    const QMatrix4x4 mvp = proj * view * model;
    m_lastViewProj = mvp;

    glUseProgram(m_prog);
    glUniformMatrix4fv(uni(m_prog, "uMVP"), 1, GL_FALSE, mvp.constData());
    glUniformMatrix4fv(uni(m_prog, "uModel"), 1, GL_FALSE, model.constData());

    // Camera-relative three-point rig (a portrait set-up that tracks the orbit): key offset
    // up/right, cool back rim behind and above, front fill opposite the key.
    const QVector3D viewDir = (eye - m_center).normalized();
    QVector3D rightV = QVector3D::crossProduct(viewDir, QVector3D(0, 1, 0));
    if (rightV.lengthSquared() < 1e-6f) rightV = QVector3D(1, 0, 0);
    rightV.normalize();
    const QVector3D worldUp(0, 1, 0);
    const float D2R = 0.01745329252f;
    const float kaz = m_rig.keyAzimuth * D2R, kel = m_rig.keyElevation * D2R;
    const QVector3D camKey  = (viewDir*std::cos(kel) + worldUp*std::sin(kel) + rightV*std::sin(kaz)).normalized();
    const QVector3D camRim  = (-viewDir*0.70f + worldUp*0.55f + rightV*0.30f).normalized();
    const QVector3D camFill = ( viewDir*0.70f + worldUp*0.05f - rightV*0.50f).normalized();
    QVector3D keyDir, rimDir, fillDir;
    if (m_lightLock) {
        if (!m_lockValid) { m_lockKey = camKey; m_lockRim = camRim; m_lockFill = camFill; m_lockValid = true; }
        keyDir = m_lockKey; rimDir = m_lockRim; fillDir = m_lockFill;
    } else {
        keyDir = camKey; rimDir = camRim; fillDir = camFill;
    }

    QMatrix4x4 shadowMvp;
    if (m_shadowOn) {
        const QVector3D lpos = m_center + keyDir * (m_radius * 3.0f);
        const QVector3D lup  = (std::abs(keyDir.y()) > 0.98f) ? QVector3D(0, 0, 1) : QVector3D(0, 1, 0);
        QMatrix4x4 lview; lview.lookAt(lpos, m_center, lup);
        QMatrix4x4 lproj; const float sr = m_radius * m_shadowRange;
        lproj.ortho(-sr, sr, -sr, sr, m_radius * 0.5f, m_radius * 6.0f);
        shadowMvp = lproj * lview;
        renderShadow(shadowMvp);
        glUseProgram(m_prog);
    }
    if (m_ssaoOn) {
        renderPos(mvp, model);
        glUseProgram(m_prog);
    }
    glUniformMatrix4fv(uni(m_prog, "uMVP"), 1, GL_FALSE, mvp.constData());
    glUniform3f(uni(m_prog, "uLightDir"), keyDir.x(),  keyDir.y(),  keyDir.z());
    glUniform3f(uni(m_prog, "uRimDir"),   rimDir.x(),  rimDir.y(),  rimDir.z());
    glUniform3f(uni(m_prog, "uFillDir"),  fillDir.x(), fillDir.y(), fillDir.z());
    glUniform3f(uni(m_prog, "uViewPos"), eye.x(), eye.y(), eye.z());

    const bool wire = m_shading == ShadingMode::Wireframe;
    const bool pbr  = m_shading == ShadingMode::Rendered;
    const bool tex  = m_showTex && (m_shading == ShadingMode::Shaded || m_shading == ShadingMode::Rendered);

    const GLint uBase = uni(m_prog, "uBase"), uSolid = uni(m_prog, "uSolid"), uNdcOffset = uni(m_prog, "uNdcOffset");
    const GLint uHasTex = uni(m_prog, "uHasTex"), uHasNormal = uni(m_prog, "uHasNormal"), uPbr = uni(m_prog, "uPbr");
    const GLint uMetal = uni(m_prog, "uMetal"), uRough = uni(m_prog, "uRough"), uHasOrm = uni(m_prog, "uHasOrm"), uOrmHasAo = uni(m_prog, "uOrmHasAo");
    const GLint uHasEmissive = uni(m_prog, "uHasEmissive"), uEmisMul = uni(m_prog, "uEmisMul"), uEmisColor = uni(m_prog, "uEmisColor");
    const GLint uHasSpecColor = uni(m_prog, "uHasSpecColor"), uAlphaMode = uni(m_prog, "uAlphaMode"), uAlphaCutoff = uni(m_prog, "uAlphaCutoff");
    const GLint uA2C = uni(m_prog, "uA2C");
    glUniform1i(uni(m_prog, "uTex"), kUnitBase);
    glUniform1i(uni(m_prog, "uNormalTex"), kUnitNormal);
    glUniform1i(uni(m_prog, "uOrmTex"), kUnitOrm);
    glUniform1i(uni(m_prog, "uEmissiveTex"), kUnitEmissive);
    glUniform1i(uni(m_prog, "uSpecColorTex"), kUnitSpecColor);
    glUniform1f(uni(m_prog, "uEmisScale"), m_emisScale);
    const float timeS = m_capturing ? m_captureTime : float(m_clock.elapsed()) * 0.001f;
    glUniform1f(uni(m_prog, "uTime"), timeS);
    glUniform1i(uni(m_prog, "uSpecAA"),   m_fSpecAA ? 1 : 0);
    glUniform1i(uni(m_prog, "uFSubsurf"), m_fSubsurf ? 1 : 0);
    glUniform1i(uni(m_prog, "uFIbl"),     m_fIbl ? 1 : 0);
    // Tonemap mode: 0 off, 1 ACES filmic (default when on), 2 neutral (engine-style — no filmic
    // curve, just exposure -> sRGB; see Shaders.cpp and notes/messiah-shader-format.md). AB_TONEMAP
    // overrides the curve for calibration against the real engine without a rebuild; unset = ACES,
    // and it never turns tonemapping on when it is off, so the default path is unchanged.
    static const int kTonemapEnv = [] {
        const QByteArray v = qgetenv("AB_TONEMAP");
        bool ok = false; const int n = v.toInt(&ok); return ok ? n : -1;
    }();
    int tonemapMode = m_fTonemap ? 1 : 0;
    if (tonemapMode == 1 && kTonemapEnv >= 0) tonemapMode = kTonemapEnv;
    glUniform1i(uni(m_prog, "uFTonemap"), tonemapMode);
    glUniform1i(uni(m_prog, "uViewChannel"), m_viewChannel);
    glUniform1f(uni(m_prog, "uExposure"), m_exposure);
    glUniform1i(uni(m_prog, "uColorGrade"), m_colorGrade ? 1 : 0);
    glUniform1f(uni(m_prog, "uCgContrast"), m_cgContrast);
    glUniform1f(uni(m_prog, "uCgSat"),      m_cgSat);
    glUniform1f(uni(m_prog, "uCgWarmth"),   m_cgWarmth);
    if (m_lutDirty) {
        m_lutDirty = false;
        if (m_pendingLut.isNull()) m_hasLut = false;
        else {
            if (m_lutTex == 0) glGenTextures(1, &m_lutTex);
            glActiveTexture(GL_TEXTURE0 + kUnitLut);
            glBindTexture(GL_TEXTURE_2D, m_lutTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, m_pendingLut.constBits());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glActiveTexture(GL_TEXTURE0);
            m_hasLut = true;
        }
    }
    glActiveTexture(GL_TEXTURE0 + kUnitLut); glBindTexture(GL_TEXTURE_2D, m_lutTex); glActiveTexture(GL_TEXTURE0);
    glUniform1i(uni(m_prog, "uLut"), kUnitLut);
    glUniform1i(uni(m_prog, "uHasLut"), (m_lutTex && m_hasLut) ? 1 : 0);
    glUniform3fv(uni(m_prog, "uEnvSky"), 1, m_envSky);
    glUniform3fv(uni(m_prog, "uEnvHor"), 1, m_envHor);
    glUniform3fv(uni(m_prog, "uEnvGnd"), 1, m_envGnd);
    glUniform3fv(uni(m_prog, "uLightCol"), 1, m_lightCol);
    glUniform3fv(uni(m_prog, "uRimCol"),   1, m_rimCol);
    glUniform3fv(uni(m_prog, "uFillCol"),  1, m_fillCol);
    glUniform1f(uni(m_prog, "uAmbScale"), m_ambScale);
    const bool reflOn = (m_reflCube != 0) && m_reflEnabled;
    glActiveTexture(GL_TEXTURE0 + kUnitReflCube); glBindTexture(GL_TEXTURE_CUBE_MAP, reflOn ? m_reflCube : 0); glActiveTexture(GL_TEXTURE0);
    glUniform1i(uni(m_prog, "uReflCube"), kUnitReflCube);
    glUniform1i(uni(m_prog, "uHasReflCube"), reflOn ? 1 : 0);
    glUniform1f(uni(m_prog, "uReflMaxMip"), float(m_reflMaxMip));
    glUniform1f(uni(m_prog, "uReflStrength"), m_reflStrength);
    const bool shOn = m_shadowOn && m_shadowTex != 0;
    glActiveTexture(GL_TEXTURE0 + kUnitShadow); glBindTexture(GL_TEXTURE_2D, shOn ? m_shadowTex : 0); glActiveTexture(GL_TEXTURE0);
    glUniform1i(uni(m_prog, "uShadowMap"), kUnitShadow);
    glUniform1i(uni(m_prog, "uShadowOn"), shOn ? 1 : 0);
    glUniformMatrix4fv(uni(m_prog, "uLightMVP"), 1, GL_FALSE, shadowMvp.constData());
    glUniform1f(uni(m_prog, "uShadowStr"),  m_shadowStr);
    glUniform1f(uni(m_prog, "uShadowSoft"), m_shadowSoft);
    glUniform1f(uni(m_prog, "uShadowBias"), m_shadowBias);
    glUniform1f(uni(m_prog, "uShadowNBias"), m_shadowNBias * m_radius);
    glUniform1f(uni(m_prog, "uShadowSize"), float(m_shadowSize));
    const bool ssOn = m_ssaoOn && m_posTex != 0;
    glActiveTexture(GL_TEXTURE0 + kUnitSsaoPos); glBindTexture(GL_TEXTURE_2D, ssOn ? m_posTex : 0); glActiveTexture(GL_TEXTURE0);
    glUniform1i(uni(m_prog, "uPosTex"), kUnitSsaoPos);
    glUniform1i(uni(m_prog, "uSsaoOn"), ssOn ? 1 : 0);
    glUniform1f(uni(m_prog, "uSsaoStr"), m_ssaoStr);
    glUniform1f(uni(m_prog, "uSsaoRad"), m_ssaoRad * m_radius * 0.1f);
    glUniform1i(uHasEmissive, 0); glUniform1i(uHasSpecColor, 0); glUniform1i(uHasOrm, 0); glUniform1i(uOrmHasAo, 1);
    glUniform1f(uEmisMul, 1.0f); glUniform3f(uEmisColor, 1.0f, 1.0f, 1.0f);
    glUniform1i(uAlphaMode, 0); glUniform1f(uAlphaCutoff, 0.5f);
    glUniform1i(uPbr, pbr ? 1 : 0);
    glUniform1f(uMetal, 0.0f); glUniform1f(uRough, 0.6f);
    glUniform1f(uni(m_prog, "uIor"), 1.5f);   // dielectric default; bindMaterial overrides per material
    glUniform2f(uNdcOffset, 0.0f, 0.0f); glUniform1i(uSolid, 0);

    GLint fbSamples = 0;
    glGetIntegerv(GL_SAMPLES, &fbSamples);   // reports the bound DRAW target
    const bool msaa = fbSamples > 1;
    glUniform1i(uA2C, msaa ? 1 : 0);

    FrameContext fc;
    fc.prog = m_prog; fc.radius = m_radius; fc.timeSeconds = timeS; fc.pbr = pbr; fc.texturesOn = tex;
    fc.wireframe = wire; fc.msaa = msaa; fc.partCount = m_parts.size();
    if (m_ext) m_ext->frameUniforms(*this, fc);

    const bool ovl = m_overlaysOn;
    // Ground grid (drawn filled regardless of wireframe mode).
    if (ovl && m_showGrid) {
        if (m_gridVerts == 0) buildGrid();
        glUniform1i(uHasTex, 0); glUniform1i(uHasNormal, 0); glUniform1i(uPbr, 0);
        glUniform3f(uBase, 0.34f, 0.34f, 0.38f);
        glBindVertexArray(m_gridVao);
        if (m_gridAxisColors && m_gridVerts - m_gridAxisFirst == 4) {
            glDrawArrays(GL_LINES, 0, m_gridAxisFirst);
            glUniform3f(uBase, 0.72f, 0.27f, 0.25f); glDrawArrays(GL_LINES, m_gridAxisFirst, 2);
            glUniform3f(uBase, 0.24f, 0.42f, 0.72f); glDrawArrays(GL_LINES, m_gridAxisFirst + 2, 2);
        } else {
            glDrawArrays(GL_LINES, 0, m_gridVerts);
        }
        glBindVertexArray(0);
        glUniform1i(uPbr, pbr ? 1 : 0);
    }

    glPolygonMode(GL_FRONT_AND_BACK, wire ? GL_LINE : GL_FILL);
    if (m_backfaceCull) { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); } else glDisable(GL_CULL_FACE);
    glBindVertexArray(m_vao);

    // Bind one part's material for the main shader.
    auto bindMaterial = [&](int i) -> const Material* {
        const Part& p = m_parts[i];
        const Material* m = (p.material >= 0 && p.material < m_materials.size()) ? &m_materials[p.material] : nullptr;
        const MatTex* mt = (m && p.material < m_matTex.size()) ? &m_matTex[p.material] : nullptr;
        const GLuint baseT = (mt && tex) ? mt->base : 0;
        if (baseT) { glActiveTexture(GL_TEXTURE0 + kUnitBase); glBindTexture(GL_TEXTURE_2D, baseT); glUniform1i(uHasTex, 1); }
        else       { glUniform1i(uHasTex, 0); glUniform3f(uBase, 0.78f, 0.78f, 0.78f); }
        const GLuint nT = (mt && pbr) ? mt->normal : 0;
        if (nT) { glActiveTexture(GL_TEXTURE0 + kUnitNormal); glBindTexture(GL_TEXTURE_2D, nT); glUniform1i(uHasNormal, 1); } else glUniform1i(uHasNormal, 0);
        glUniform1f(uMetal, m ? m->metalFactor : 0.0f);
        glUniform1f(uRough, m ? m->roughFactor : 0.6f);
        glUniform1f(uni(m_prog, "uIor"), (m && m->ior > 1.0f) ? m->ior : 1.5f);
        const GLuint oT = (mt && pbr) ? mt->orm : 0;
        if (oT) { glActiveTexture(GL_TEXTURE0 + kUnitOrm); glBindTexture(GL_TEXTURE_2D, oT); glUniform1i(uHasOrm, 1); glUniform1i(uOrmHasAo, m->ormHasOcclusion ? 1 : 0); } else glUniform1i(uHasOrm, 0);
        const GLuint eT = (mt && tex) ? mt->emissive : 0;
        if (eT) {
            glActiveTexture(GL_TEXTURE0 + kUnitEmissive); glBindTexture(GL_TEXTURE_2D, eT); glUniform1i(uHasEmissive, 1);
            glUniform1f(uEmisMul, m->emissiveStrength);
            glUniform3f(uEmisColor, m->emissiveFactor[0], m->emissiveFactor[1], m->emissiveFactor[2]);
        } else glUniform1i(uHasEmissive, 0);
        const GLuint sT = (mt && pbr) ? mt->specColor : 0;
        if (sT) { glActiveTexture(GL_TEXTURE0 + kUnitSpecColor); glBindTexture(GL_TEXTURE_2D, sT); glUniform1i(uHasSpecColor, 1); } else glUniform1i(uHasSpecColor, 0);
        // Alpha mode: only meaningful with a base texture. Wireframe forces every part opaque.
        const int am = (baseT && m && !wire) ? int(m->alphaMode) : 0;
        glUniform1i(uAlphaMode, am);
        glUniform1f(uAlphaCutoff, m ? m->alphaCutoff : 0.5f);
        glActiveTexture(GL_TEXTURE0);
        if (m_ext) m_ext->partUniforms(*this, fc, i);
        return m;
    };
    auto isBlended = [&](int i) {
        const Part& p = m_parts[i];
        if (wire) return false;
        const Material* m = (p.material >= 0 && p.material < m_materials.size()) ? &m_materials[p.material] : nullptr;
        const bool hasBase = m && p.material < m_matTex.size() && m_matTex[p.material].base && tex;
        return hasBase && (m->alphaMode == Material::Blend || m->alphaMode == Material::Additive);
    };
    const DrawPartFn drawFn = [this](int part) { drawPartIndices(part); };

    // ── Pass 1: opaque + mask, alpha-to-coverage anti-aliasing the cutouts. ──
    if (msaa) {
        glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        // ALPHA_TO_ONE stops a transparent capture double-counting the cutout: otherwise the
        // fragment's fractional alpha picks coverage AND is written to every covered sample, so the
        // resolve lands on alpha squared and cutouts saved to PNG come out far more see-through
        // than on screen.
        glEnable(GL_SAMPLE_ALPHA_TO_ONE);
    }
    for (int i = 0; i < m_parts.size(); ++i) {
        const Part& p = m_parts[i];
        if (!p.visible || p.count == 0) continue;
        if (m_ext && !m_ext->partInMainPass(i)) continue;
        if (isBlended(i)) continue;
        const Material* m = bindMaterial(i);
        const bool twoSided = p.doubleSided || (m && m->doubleSided);
        if (twoSided && !wire) glDisable(GL_CULL_FACE);
        drawPartIndices(i);
        if (twoSided && !wire && m_backfaceCull) { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); }
        if (m_ext) m_ext->afterPart(*this, fc, i, drawFn);
    }
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glDisable(GL_SAMPLE_ALPHA_TO_ONE);

    // ── Selection SILHOUETTE pass ──
    // Selected parts keep their real material (a flat tint hid the textures you are trying to
    // inspect). The part is stamped into the STENCIL buffer, then redrawn as a flat colour offset
    // by a few screen pixels in eight directions with the stencil test rejecting the interior —
    // what survives is a constant-width ring hugging the outline. Depth test off, so a part buried
    // inside the body still shows. RED = selection, BLUE = the context-menu set, drawn last.
    if (ovl && m_showSelection) {
        QVector<QPair<int, int>> outline;
        for (int i : m_selected) if (i >= 0 && i < m_parts.size() && m_parts[i].visible && m_parts[i].count) outline.push_back({i, 0});
        for (int c : m_context) {
            if (c < 0 || c >= m_parts.size() || !m_parts[c].visible || !m_parts[c].count) continue;
            for (int k = outline.size() - 1; k >= 0; --k) if (outline[k].first == c) outline.removeAt(k);
            outline.push_back({c, 1});
        }
        if (!outline.isEmpty()) {
            // Ask the BOUND framebuffer whether it has a stencil attachment; the attachment enum
            // differs by target (GL_STENCIL for the default framebuffer, GL_STENCIL_ATTACHMENT for
            // a named one) and asking the wrong one silently disables this whole pass.
            GLint drawFbo = 0;
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
            GLint stencilAttach = GL_NONE;
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, drawFbo == 0 ? GL_STENCIL : GL_STENCIL_ATTACHMENT,
                                                  GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &stencilAttach);
            const bool haveStencil = (stencilAttach != GL_NONE) && m_fbW > 0 && m_fbH > 0;
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glDisable(GL_CULL_FACE);
            glDisable(GL_BLEND);
            glUniform1i(uHasNormal, 0); glUniform1i(uHasEmissive, 0); glUniform1i(uPbr, 0);
            // The base texture stays bound so the shader can honour the alpha cutout: without it
            // the outline of cut-out cloth traces the card RECTANGLES rather than the visible shape.
            auto drawParts = [&](int colour) {
                for (const auto& o : outline) {
                    if (o.second != colour) continue;
                    const Part& p = m_parts[o.first];
                    const Material* m = (p.material >= 0 && p.material < m_materials.size()) ? &m_materials[p.material] : nullptr;
                    const GLuint t = (m && p.material < m_matTex.size() && tex) ? m_matTex[p.material].base : 0;
                    if (t) {
                        glActiveTexture(GL_TEXTURE0 + kUnitBase); glBindTexture(GL_TEXTURE_2D, t);
                        glUniform1i(uHasTex, 1);
                        glUniform1i(uAlphaMode, int(m->alphaMode)); glUniform1f(uAlphaCutoff, m->alphaCutoff);
                    } else { glUniform1i(uHasTex, 0); glUniform1i(uAlphaMode, 0); }
                    drawPartIndices(o.first);
                }
            };
            if (haveStencil) {
                glUniform1i(uSolid, 1);
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                glEnable(GL_STENCIL_TEST);
                glStencilMask(0xFF);
                const float r = 2.0f * float(devicePixelRatioF());   // half-width in DEVICE pixels
                const float sx = 2.0f * r / float(m_fbW), sy = 2.0f * r / float(m_fbH);
                static const float kDir[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0.707f, 0.707f}, {-0.707f, 0.707f}, {0.707f, -0.707f}, {-0.707f, -0.707f}};
                for (int colour = 0; colour <= 1; ++colour) {
                    bool any = false;
                    for (const auto& o : outline) if (o.second == colour) { any = true; break; }
                    if (!any) continue;
                    glClear(GL_STENCIL_BUFFER_BIT);
                    glStencilFunc(GL_ALWAYS, 1, 0xFF);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                    glUniform2f(uNdcOffset, 0.0f, 0.0f);
                    drawParts(colour);
                    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                    glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
                    if (colour == 1) glUniform3f(uBase, 0.25f, 0.60f, 1.00f); else glUniform3f(uBase, 1.00f, 0.15f, 0.15f);
                    for (const auto& d : kDir) { glUniform2f(uNdcOffset, d[0] * sx, d[1] * sy); drawParts(colour); }
                }
                glUniform2f(uNdcOffset, 0.0f, 0.0f);
                glUniform1i(uSolid, 0);
                glDisable(GL_STENCIL_TEST);
                glPolygonMode(GL_FRONT_AND_BACK, wire ? GL_LINE : GL_FILL);
            } else {
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                glLineWidth(1.0f);
                for (const auto& o : outline) {
                    if (o.second == 1) glUniform3f(uBase, 0.25f, 0.60f, 1.00f); else glUniform3f(uBase, 1.00f, 0.15f, 0.15f);
                    glUniform1i(uHasTex, 0);
                    drawPartIndices(o.first);
                }
                glPolygonMode(GL_FRONT_AND_BACK, wire ? GL_LINE : GL_FILL);
            }
            if (m_backfaceCull) { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); }
            glDepthMask(GL_TRUE);
            glEnable(GL_DEPTH_TEST);
            glUniform1i(uHasTex, 0); glUniform1i(uAlphaMode, 0);
            glUniform1i(uPbr, pbr ? 1 : 0);
        }
    }

    // ── Extension pass (D4's mesh FX) ──
    if (m_ext && !wire) {
        m_ext->extraPass(*this, fc, drawFn);
        glUseProgram(m_prog);
        glBindVertexArray(m_vao);
        glActiveTexture(GL_TEXTURE0);
    }

    // ── Pass 2 (POE2): blend / additive parts — depth-tested against the opaque pass, no depth
    // write, composited in part order (not depth-sorted among themselves). ──
    if (!wire) {
        bool any = false;
        for (int i = 0; i < m_parts.size() && !any; ++i) any = m_parts[i].visible && m_parts[i].count && isBlended(i);
        if (any) {
            glEnable(GL_BLEND);
            glDepthMask(GL_FALSE);
            glDisable(GL_CULL_FACE);
            for (int i = 0; i < m_parts.size(); ++i) {
                const Part& p = m_parts[i];
                if (!p.visible || p.count == 0 || !isBlended(i)) continue;
                if (m_ext && !m_ext->partInMainPass(i)) continue;
                const Material* m = bindMaterial(i);
                if (m && m->alphaMode == Material::Additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                drawPartIndices(i);
            }
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            if (m_backfaceCull) { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); }
        }
    }
    glBindVertexArray(0);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_CULL_FACE);

    // ── Overlays (all behind the master gate) ──
    if (ovl && m_showSkeleton) {
        if (m_skelVerts == 0) buildSkeleton();
        if (m_skelVerts > 0) {
            glDisable(GL_DEPTH_TEST);
            glUniform1i(uHasTex, 0); glUniform1i(uHasNormal, 0); glUniform1i(uHasEmissive, 0); glUniform1i(uPbr, 0); glUniform1i(uAlphaMode, 0);
            glUniform3f(uBase, 0.20f, 1.0f, 0.55f);
            glBindVertexArray(m_skelVao);
            glDrawArrays(GL_LINES, 0, m_skelVerts);
            glBindVertexArray(0);
            glEnable(GL_DEPTH_TEST);
        }
    }
    if (ovl && m_showHardpoints) {
        if (m_hpAxisVerts == 0) buildHardpoints();
        if (m_hpAxisVerts > 0) {
            glDisable(GL_DEPTH_TEST);
            glUniform1i(uHasTex, 0); glUniform1i(uHasNormal, 0); glUniform1i(uHasEmissive, 0); glUniform1i(uPbr, 0); glUniform1i(uAlphaMode, 0);
            glBindVertexArray(m_hpVao);
            const int n = m_hpAxisVerts;
            glUniform3f(uBase, 1.0f, 0.25f, 0.25f); glDrawArrays(GL_LINES, 0,     n);
            glUniform3f(uBase, 0.30f, 1.0f, 0.35f); glDrawArrays(GL_LINES, n,     n);
            glUniform3f(uBase, 0.35f, 0.55f, 1.0f); glDrawArrays(GL_LINES, 2 * n, n);
            glBindVertexArray(0);
            glEnable(GL_DEPTH_TEST);
        }
    }
    if (ovl && m_ext) {
        glUseProgram(m_prog);
        m_ext->drawOverlays([this](const QVector<float>& xyz, float r, float g, float b) { drawLines(xyz, r, g, b); });
    }
    if ((ovl && m_showBoneNames) || m_boneLabels) updateBoneLabels();
    if ((ovl && m_showHardpoints) || m_hpLabels) updateHardpointLabels();
    if (m_gizmo && m_gizmo->isVisible()) m_gizmo->update();

    // Keep repainting while something animates on its own (an extension's FX).
    const bool wantAnim = m_ext && m_ext->wantsContinuousRepaint() && !m_capturing;
    if (wantAnim) {
        if (!m_repaintTimer) { m_repaintTimer = new QTimer(this); connect(m_repaintTimer, &QTimer::timeout, this, [this] { update(); }); }
        if (!m_repaintTimer->isActive()) m_repaintTimer->start(33);
    } else if (m_repaintTimer && m_repaintTimer->isActive()) {
        m_repaintTimer->stop();
    }
}

// ── Overlay geometry ───────────────────────────────────────────────────────────────────────────

void GLModelWidget::buildGrid()
{
    const float r = qMax(0.001f, m_radius);
    const float ext = r * 2.0f, step = r * 0.25f;
    const float y = m_homeCenter.y() - r * 1.02f;
    const float cx = m_homeCenter.x(), cz = m_homeCenter.z();
    QVector<float> v;
    auto line = [&](float x0, float z0, float x1, float z1) {
        v << x0 << y << z0 << 0 << 1 << 0 << 0 << 0 << 1 << 0 << 0;
        v << x1 << y << z1 << 0 << 1 << 0 << 0 << 0 << 1 << 0 << 0;
    };
    for (float t = -ext; t <= ext + 1e-3f; t += step) {
        if (std::abs(t) < step * 0.5f) continue;   // the two centre lines are appended LAST (axes)
        line(cx - ext, cz + t, cx + ext, cz + t);
        line(cx + t, cz - ext, cx + t, cz + ext);
    }
    m_gridAxisFirst = v.size() / kStride;
    line(cx - ext, cz, cx + ext, cz);   // along X
    line(cx, cz - ext, cx, cz + ext);   // along Z
    uploadLineVao(m_gridVao, m_gridVbo, v, false);
    m_gridVerts = v.size() / kStride;
}

void GLModelWidget::buildSkeleton()
{
    m_skelVerts = 0;
    const int nb = m_skeleton.size();
    if (nb == 0) return;
    const QVector<rm::Mat4> global = currentWorldPose();
    auto headOf = [&](int j, float o[3]) {
        if (m_ext && m_ext->jointHeadOverride(j, o)) return;
        o[0] = global[j][12]; o[1] = global[j][13]; o[2] = global[j][14];
    };
    QVector<float> v;
    auto push = [&](const float p[3]) { v << p[0] << p[1] << p[2] << 0 << 1 << 0 << 0 << 0 << 1 << 0 << 0; };
    for (int j = 0; j < nb; ++j) {
        const int p = m_skeleton.joints[j].parent;
        if (p < 0 || p >= nb) continue;
        float a[3], b[3]; headOf(p, a); headOf(j, b);
        push(a); push(b);
    }
    if (v.isEmpty()) return;
    uploadLineVao(m_skelVao, m_skelVbo, v, false);
    m_skelVerts = v.size() / kStride;
}

// A socket's model-space matrix in the CURRENT pose. Authored (model-space) sockets are skinned
// like a vertex — socket · inverseBind · world — and are the joint itself at rest; bone-local ones
// are local · world.
static rm::Mat4 hardpointWorld(const Hardpoint& hp, const Skeleton& sk, const QVector<rm::Mat4>& world)
{
    const rm::Mat4 local = rm::composeTRS(hp.t, hp.q.data(), hp.s);
    if (hp.modelSpace) return rm::mul(rm::mul(local, sk.joints[hp.joint].inverseBind), world[hp.joint]);
    return rm::mul(local, world[hp.joint]);
}

void GLModelWidget::buildHardpoints()
{
    m_hpAxisVerts = 0;
    const int nb = m_skeleton.size();
    if (m_hardpoints.isEmpty() || nb == 0) return;
    const QVector<rm::Mat4> global = currentWorldPose();
    const float len = qMax(0.02f, m_radius * 0.05f);
    QVector<float> vx, vy, vz;
    auto push = [](QVector<float>& out, float x, float y, float z) { out << x << y << z << 0 << 1 << 0 << 0 << 0 << 1 << 0 << 0; };
    for (const Hardpoint& hp : m_hardpoints) {
        if (hp.joint < 0 || hp.joint >= nb) continue;
        const rm::Mat4 m = hardpointWorld(hp, m_skeleton, global);
        const float ox = m[12], oy = m[13], oz = m[14];
        // Row-vector: the local axes are the matrix ROWS.
        push(vx, ox, oy, oz); push(vx, ox + len*m[0], oy + len*m[1], oz + len*m[2]);
        push(vy, ox, oy, oz); push(vy, ox + len*m[4], oy + len*m[5], oz + len*m[6]);
        push(vz, ox, oy, oz); push(vz, ox + len*m[8], oy + len*m[9], oz + len*m[10]);
    }
    if (vx.isEmpty()) return;
    QVector<float> v; v.reserve(vx.size() * 3);
    v << vx << vy << vz;
    uploadLineVao(m_hpVao, m_hpVbo, v, true);
    m_hpAxisVerts = vx.size() / kStride;
}

void GLModelWidget::updateBoneLabels()
{
    if (!m_boneLabels) m_boneLabels = new LabelOverlay(this);
    auto* ov = static_cast<LabelOverlay*>(m_boneLabels);
    ov->items.clear();
    const int nb = m_skeleton.size();
    if (!(m_overlaysOn && m_showBoneNames) || nb == 0) { ov->hide(); return; }
    const QVector<rm::Mat4> global = currentWorldPose();
    const int W = width(), H = height();
    for (int j = 0; j < nb; ++j) {
        float hp[3];
        if (!(m_ext && m_ext->jointHeadOverride(j, hp))) { hp[0] = global[j][12]; hp[1] = global[j][13]; hp[2] = global[j][14]; }
        const QVector4D clip = m_lastViewProj * QVector4D(hp[0], hp[1], hp[2], 1.0f);
        if (clip.w() <= 0.0001f) continue;
        const float sx = (clip.x() / clip.w() * 0.5f + 0.5f) * W;
        const float sy = (1.0f - (clip.y() / clip.w() * 0.5f + 0.5f)) * H;
        if (sx < -40 || sy < -20 || sx > W + 40 || sy > H + 20) continue;
        ov->items.append({QPoint(int(sx), int(sy)), m_skeleton.joints[j].name});
    }
    ov->setGeometry(rect());
    ov->show(); ov->raise(); ov->update();
}

void GLModelWidget::updateHardpointLabels()
{
    if (!m_hpLabels) m_hpLabels = new LabelOverlay(this);
    auto* ov = static_cast<LabelOverlay*>(m_hpLabels);
    ov->items.clear();
    const int nb = m_skeleton.size();
    if (!(m_overlaysOn && m_showHardpoints) || m_hardpoints.isEmpty() || nb == 0) { ov->hide(); return; }
    const QVector<rm::Mat4> global = currentWorldPose();
    const int W = width(), H = height();
    for (const Hardpoint& hp : m_hardpoints) {
        if (hp.joint < 0 || hp.joint >= nb) continue;
        const rm::Mat4 m = hardpointWorld(hp, m_skeleton, global);
        const QVector4D clip = m_lastViewProj * QVector4D(m[12], m[13], m[14], 1.0f);
        if (clip.w() <= 0.0001f) continue;
        const float sx = (clip.x() / clip.w() * 0.5f + 0.5f) * W;
        const float sy = (1.0f - (clip.y() / clip.w() * 0.5f + 0.5f)) * H;
        if (sx < -40 || sy < -20 || sx > W + 40 || sy > H + 20) continue;
        ov->items.append({QPoint(int(sx), int(sy)), hp.name});
    }
    ov->setGeometry(rect());
    ov->show(); ov->raise(); ov->update();
}

void GLModelWidget::setOverlayText(const QString& text)
{
    if (text.isEmpty()) { if (m_overlayLabel) m_overlayLabel->hide(); return; }
    if (!m_overlayLabel) {
        m_overlayLabel = new QLabel(this);
        m_overlayLabel->setAlignment(Qt::AlignCenter);
        m_overlayLabel->setWordWrap(true);
        m_overlayLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_overlayLabel->setStyleSheet(QStringLiteral("QLabel { color: rgba(190,190,200,220); background: transparent; font-size: 13px; }"));
    }
    m_overlayLabel->setText(text);
    m_overlayLabel->setGeometry(rect());
    m_overlayLabel->show();
    m_overlayLabel->raise();
}

// ── Parts and selection ────────────────────────────────────────────────────────────────────────

int  GLModelWidget::partTriangles(int i) const { return (i >= 0 && i < m_parts.size()) ? m_parts[i].count / 3 : 0; }
void GLModelWidget::setPartVisible(int i, bool on) { if (i >= 0 && i < m_parts.size() && m_parts[i].visible != on) { m_parts[i].visible = on; update(); } }
bool GLModelWidget::partVisible(int i) const { return (i >= 0 && i < m_parts.size()) ? m_parts[i].visible : true; }
void GLModelWidget::setAllPartsVisible(bool on) { for (Part& p : m_parts) p.visible = on; update(); }
void GLModelWidget::isolateParts(const QSet<int>& keep) { for (int i = 0; i < m_parts.size(); ++i) m_parts[i].visible = keep.contains(i); update(); }
QSet<int> GLModelWidget::hiddenParts() const { QSet<int> h; for (int i = 0; i < m_parts.size(); ++i) if (!m_parts[i].visible) h.insert(i); return h; }

void GLModelWidget::emitSelection() { emit selectionChanged(m_selected, m_active); update(); }

void GLModelWidget::setSelectedParts(const QSet<int>& parts, int active)
{
    QSet<int> s;
    for (int p : parts) if (p >= 0 && p < m_parts.size()) s.insert(p);
    m_selected = s;
    m_active = (active >= 0 && s.contains(active)) ? active : (s.isEmpty() ? -1 : *std::min_element(s.begin(), s.end()));
    emitSelection();
}
void GLModelWidget::setSelectedPart(int part) { setSelectedParts(part >= 0 ? QSet<int>{part} : QSet<int>{}, part); }
void GLModelWidget::addToSelection(int part)
{
    if (part < 0 || part >= m_parts.size()) return;
    m_selected.insert(part); m_active = part; emitSelection();
}
void GLModelWidget::removeFromSelection(int part)
{
    if (!m_selected.remove(part)) return;
    if (m_active == part) m_active = m_selected.isEmpty() ? -1 : *std::min_element(m_selected.begin(), m_selected.end());
    emitSelection();
}
void GLModelWidget::toggleInSelection(int part) { if (m_selected.contains(part)) removeFromSelection(part); else addToSelection(part); }
void GLModelWidget::clearSelection() { if (m_selected.isEmpty() && m_active < 0) return; m_selected.clear(); m_active = -1; emitSelection(); }
void GLModelWidget::setContextParts(const QSet<int>& parts) { m_context = parts; update(); }

// ── Shading, overlays, features ────────────────────────────────────────────────────────────────

void GLModelWidget::setShadingMode(ShadingMode m) { if (m_shading == m) return; m_shading = m; emit shadingModeChanged(m); update(); }
void GLModelWidget::setViewChannel(int c) { if (m_viewChannel != c) { m_viewChannel = c; update(); } }
void GLModelWidget::setOverlaysEnabled(bool on)
{
    if (m_overlaysOn == on) return;
    m_overlaysOn = on;
    if (m_gizmo) m_gizmo->setVisible(on && m_showAxisGizmo);
    if (!on) { if (m_boneLabels) m_boneLabels->hide(); if (m_hpLabels) m_hpLabels->hide(); }
    update();
}
void GLModelWidget::setShowGrid(bool on)       { if (m_showGrid != on) { m_showGrid = on; if (on) m_gridVerts = 0; update(); } }
void GLModelWidget::setShowSkeleton(bool on)   { if (m_showSkeleton != on) { m_showSkeleton = on; if (on) m_skelVerts = 0; update(); } }
void GLModelWidget::setShowHardpoints(bool on) { if (m_showHardpoints != on) { m_showHardpoints = on; if (on) m_hpAxisVerts = 0; if (!on && m_hpLabels) m_hpLabels->hide(); update(); } }
void GLModelWidget::setShowBoneNames(bool on)  { if (m_showBoneNames != on) { m_showBoneNames = on; if (!on && m_boneLabels) m_boneLabels->hide(); update(); } }
void GLModelWidget::setShowSelection(bool on)  { if (m_showSelection != on) { m_showSelection = on; update(); } }
void GLModelWidget::setShowAxisGizmo(bool on)  { m_showAxisGizmo = on; if (m_gizmo) m_gizmo->setVisible(m_overlaysOn && on); }
void GLModelWidget::setGridAxisColors(bool on) { if (m_gridAxisColors != on) { m_gridAxisColors = on; update(); } }
void GLModelWidget::setShowTextures(bool on)   { if (m_showTex != on) { m_showTex = on; update(); } }

void GLModelWidget::applyRig()
{
    // Three presets of key/rim/fill colour and hemisphere gradient. Preset 0 is D4's authored
    // character-screen rig (warm campfire key, cool cyan rim, cool white fill); 1 is neutral; 2 a
    // cool studio three-point. The numbers are just numbers — the names live in the app.
    static const float K[3][3][3] = {
        {{1.000f,0.616f,0.341f}, {0.769f,0.980f,1.000f}, {0.898f,0.980f,1.000f}},
        {{1.000f,1.000f,1.000f}, {0.800f,0.850f,1.000f}, {1.000f,1.000f,1.000f}},
        {{0.950f,0.970f,1.000f}, {0.750f,0.900f,1.000f}, {0.850f,0.920f,1.000f}},
    };
    static const float E[3][3][3] = {
        {{0.10f,0.12f,0.16f}, {0.17f,0.13f,0.11f}, {0.22f,0.12f,0.07f}},
        {{0.30f,0.32f,0.36f}, {0.26f,0.26f,0.27f}, {0.14f,0.13f,0.12f}},
        {{0.34f,0.40f,0.52f}, {0.26f,0.28f,0.32f}, {0.12f,0.12f,0.14f}},
    };
    const int p = qBound(0, m_rig.preset, 2);
    const float KEY_BASE = 3.0f, RIM_BASE = 2.2f, FILL_BASE = 1.1f;
    for (int i = 0; i < 3; ++i) {
        m_lightCol[i] = K[p][0][i] * m_rig.keyInt  * KEY_BASE;
        m_rimCol[i]   = K[p][1][i] * m_rig.rimInt  * RIM_BASE;
        m_fillCol[i]  = K[p][2][i] * m_rig.fillInt * FILL_BASE;
        m_envSky[i] = E[p][0][i]; m_envHor[i] = E[p][1][i]; m_envGnd[i] = E[p][2][i];
    }
    m_ambScale = m_rig.ambInt;
}
void GLModelWidget::setLightRig(const LightRig& r) { m_rig = r; applyRig(); update(); }
void GLModelWidget::setLightLock(bool worldFixed) { if (m_lightLock != worldFixed) { m_lightLock = worldFixed; if (worldFixed) m_lockValid = false; update(); } }
void GLModelWidget::setExposure(float v) { m_exposure = v; update(); }
void GLModelWidget::setFeatureIbl(bool on) { if (m_fIbl != on) { m_fIbl = on; update(); } }
void GLModelWidget::setFeatureTonemap(bool on) { if (m_fTonemap != on) { m_fTonemap = on; update(); } }
void GLModelWidget::setFeatureSpecAA(bool on) { if (m_fSpecAA != on) { m_fSpecAA = on; update(); } }
void GLModelWidget::setFeatureSubsurface(bool on) { if (m_fSubsurf != on) { m_fSubsurf = on; update(); } }
void GLModelWidget::setColorGrade(bool on, float contrast, float sat, float warmth)
{
    m_colorGrade = on; m_cgContrast = qBound(0.5f, contrast, 2.0f); m_cgSat = qBound(0.0f, sat, 2.0f); m_cgWarmth = qBound(-0.2f, warmth, 0.2f);
    update();
}
void GLModelWidget::setColorGradeLut(const QImage& lut)
{
    m_pendingLut = (lut.width() == 256 && lut.height() == 16) ? lut.convertToFormat(QImage::Format_RGBA8888) : QImage();
    m_lutDirty = true;
    update();
}
void GLModelWidget::setEmissiveScale(float v) { v = qMax(0.0f, v); if (m_emisScale != v) { m_emisScale = v; update(); } }
void GLModelWidget::setReflectionEnabled(bool on) { if (m_reflEnabled != on) { m_reflEnabled = on; update(); } }
void GLModelWidget::setReflectionStrength(float v) { v = qMax(0.0f, v); if (m_reflStrength != v) { m_reflStrength = v; update(); } }
void GLModelWidget::setShadowEnabled(bool on) { if (m_shadowOn != on) { m_shadowOn = on; update(); } }
void GLModelWidget::setShadowParams(float strength, float softness, float bias)
{
    m_shadowStr = qBound(0.0f, strength, 1.0f); m_shadowSoft = qMax(0.0f, softness); m_shadowBias = qBound(0.0f, bias, 0.02f); update();
}
void GLModelWidget::setShadowExtra(float rangeMul, float normalBiasFrac, int resolution)
{
    m_shadowRange = qBound(1.0f, rangeMul, 4.0f);
    m_shadowNBias = qMax(0.0f, normalBiasFrac);
    int r = qBound(512, resolution, 4096);
    r = (r / 512) * 512;
    if (r != m_shadowSize) { m_shadowSize = r; m_shadowResDirty = true; }
    update();
}
void GLModelWidget::setFeatures(const ViewFeatures& f)
{
    m_shadowOn = f.shadow; m_ssaoOn = f.ssao; m_fIbl = f.ibl; m_fTonemap = f.tonemap;
    m_fSpecAA = f.specAA; m_fSubsurf = f.subsurface; m_reflEnabled = f.reflection;
    update();
}
ViewFeatures GLModelWidget::features() const
{
    ViewFeatures f;
    f.shadow = m_shadowOn; f.ssao = m_ssaoOn; f.ibl = m_fIbl; f.tonemap = m_fTonemap;
    f.specAA = m_fSpecAA; f.subsurface = m_fSubsurf; f.reflection = m_reflEnabled;
    return f;
}
void GLModelWidget::setSsaoEnabled(bool on) { if (m_ssaoOn != on) { m_ssaoOn = on; update(); } }
void GLModelWidget::setSsaoParams(float strength, float radius) { m_ssaoStr = qBound(0.0f, strength, 4.0f); m_ssaoRad = qMax(0.001f, radius); update(); }
void GLModelWidget::setBackgroundColor(const QColor& c)
{
    if (!c.isValid()) return;
    m_bg[0] = float(c.redF()); m_bg[1] = float(c.greenF()); m_bg[2] = float(c.blueF());
    update();
}
QColor GLModelWidget::backgroundColor() const { return QColor::fromRgbF(m_bg[0], m_bg[1], m_bg[2]); }
void GLModelWidget::setFov(float deg) { deg = qBound(10.0f, deg, 100.0f); if (qAbs(m_fov - deg) > 0.01f) { m_fov = deg; update(); } }
void GLModelWidget::setCaptureMode(bool on)
{
    if (m_capturing == on) return;
    m_capturing = on;
    if (m_ext) m_ext->setCaptureMode(on);
    if (on && m_repaintTimer && m_repaintTimer->isActive()) m_repaintTimer->stop();
}

// ── Camera ─────────────────────────────────────────────────────────────────────────────────────

QString GLModelWidget::CameraPose::toString() const
{
    return QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8").arg(double(yaw)).arg(double(pitch)).arg(double(dist))
        .arg(double(center.x())).arg(double(center.y())).arg(double(center.z())).arg(ortho ? 1 : 0).arg(double(fov));
}

bool GLModelWidget::CameraPose::fromString(const QString& s, CameraPose* out)
{
    const QStringList p = s.split(QLatin1Char(','));
    if (p.size() != 8 || !out) return false;
    bool ok = true; float v[8];
    for (int i = 0; i < 8 && ok; ++i) v[i] = p[i].toFloat(&ok);
    if (!ok) return false;
    out->yaw = v[0]; out->pitch = v[1]; out->dist = v[2]; out->center = QVector3D(v[3], v[4], v[5]);
    out->ortho = v[6] != 0.0f; out->fov = v[7]; out->valid = true;
    return true;
}

GLModelWidget::CameraPose GLModelWidget::cameraPose() const
{
    CameraPose s;
    s.yaw = m_yaw; s.pitch = m_pitch; s.dist = m_dist; s.fov = m_fov; s.center = m_center; s.ortho = m_ortho; s.valid = true;
    return s;
}

void GLModelWidget::setCameraPose(const CameraPose& s)
{
    if (!s.valid) return;
    if (m_camAnim) m_camAnim->stop();
    m_followParts.clear();
    m_yaw = s.yaw; m_pitch = qBound(-kPitchLimit, s.pitch, kPitchLimit); m_dist = qMax(m_radius * 0.05f, s.dist);
    m_fov = qBound(10.0f, s.fov, 100.0f); m_center = s.center; m_ortho = s.ortho;
    m_tgtYaw = m_yaw; m_tgtPitch = m_pitch; m_tgtDist = m_dist; m_tgtCenter = m_center;
    emit cameraChanged();
    update();
}

const QStringList& GLModelWidget::builtinCameraPresets()
{
    static const QStringList k = {QStringLiteral("default"), QStringLiteral("front"), QStringLiteral("back"),
                                  QStringLiteral("left"), QStringLiteral("right"), QStringLiteral("top"),
                                  QStringLiteral("bottom"), QStringLiteral("iso")};
    return k;
}

bool GLModelWidget::applyCameraPreset(const QString& name, bool animate)
{
    // Camera dir = (cos p sin y, sin p, cos p cos y): front looks down -Z from +Z (yaw 0), left
    // is the view from -X, top from +Y.
    float yaw, pitch;
    const QString n = name.toLower();
    if      (n == QLatin1String("default")) { yaw = 0.6f; pitch = 0.25f; }
    else if (n == QLatin1String("front"))   { yaw = 0.0f; pitch = 0.0f; }
    else if (n == QLatin1String("back"))    { yaw = kPi; pitch = 0.0f; }
    else if (n == QLatin1String("right"))   { yaw = kPi / 2; pitch = 0.0f; }
    else if (n == QLatin1String("left"))    { yaw = -kPi / 2; pitch = 0.0f; }
    else if (n == QLatin1String("top"))     { yaw = m_yaw; pitch = 1.55f; }
    else if (n == QLatin1String("bottom"))  { yaw = m_yaw; pitch = -1.55f; }
    else if (n == QLatin1String("iso"))     { yaw = kPi / 4; pitch = 0.6f; }
    else return false;
    QVector3D c; float r;
    liveBounds(c, r);
    frameRegion(c, r, yaw, pitch, animate);
    return true;
}

void GLModelWidget::setOrbitAngles(float yawRad, float pitchRad)
{
    if (m_camAnim) m_camAnim->stop();
    m_followParts.clear();
    m_yaw = yawRad; m_pitch = qBound(-kPitchLimit, pitchRad, kPitchLimit);
    m_tgtYaw = m_yaw; m_tgtPitch = m_pitch;
    emit cameraChanged();
    update();
}

// Glide to an axis view keeping BOTH the orbit centre and the current zoom.
void GLModelWidget::orbitToAxis(float yawRad, float pitchRad)
{
    const float half = m_fov * 0.5f * kPi / 180.0f;
    const float keepR = m_dist * qMax(0.05f, std::sin(half));
    frameRegion(m_center, keepR, yawRad, pitchRad, true);
}

void GLModelWidget::liveBounds(QVector3D& center, float& radius) const
{
    const int n = m_verts.size(), nv = n / kStride;
    if (n < kStride) { center = m_homeCenter; radius = m_radius; return; }
    float mnx = 1e30f, mny = 1e30f, mnz = 1e30f, mxx = -1e30f, mxy = -1e30f, mxz = -1e30f;
    bool any = false;
    // Frame only the VISIBLE parts, so hiding submeshes re-frames the camera on what's shown.
    for (const Part& p : m_parts) {
        if (!p.visible || p.count == 0) continue;
        const int end = qMin(p.offset + p.count, m_indices.size());
        for (int k = p.offset; k < end; ++k) {
            const int vi = int(m_indices[k]);
            if (vi < 0 || vi >= nv) continue;
            const float x = m_verts[vi*kStride], y = m_verts[vi*kStride+1], z = m_verts[vi*kStride+2];
            mnx = qMin(mnx, x); mny = qMin(mny, y); mnz = qMin(mnz, z);
            mxx = qMax(mxx, x); mxy = qMax(mxy, y); mxz = qMax(mxz, z);
            any = true;
        }
    }
    if (!any) {
        for (int i = 0; i + 2 < n; i += kStride) {
            const float x = m_verts[i], y = m_verts[i+1], z = m_verts[i+2];
            mnx = qMin(mnx, x); mny = qMin(mny, y); mnz = qMin(mnz, z);
            mxx = qMax(mxx, x); mxy = qMax(mxy, y); mxz = qMax(mxz, z);
        }
    }
    center = QVector3D((mnx + mxx) * 0.5f, (mny + mxy) * 0.5f, (mnz + mxz) * 0.5f);
    radius = qMax(0.001f, QVector3D(mxx - mnx, mxy - mny, mxz - mnz).length() * 0.5f);
}

bool GLModelWidget::partsBounds(const QVector<int>& partIndices, QVector3D& center, float& radius) const
{
    const int nv = m_verts.size() / kStride;
    if (nv <= 0 || m_indices.isEmpty()) return false;
    float mnx = 1e30f, mny = 1e30f, mnz = 1e30f, mxx = -1e30f, mxy = -1e30f, mxz = -1e30f;
    bool any = false;
    for (int pi : partIndices) {
        if (pi < 0 || pi >= m_parts.size()) continue;
        const Part& p = m_parts[pi];
        const int end = qMin(p.offset + p.count, m_indices.size());
        for (int k = p.offset; k < end; ++k) {
            const int vi = int(m_indices[k]);
            if (vi < 0 || vi >= nv) continue;
            const float x = m_verts[vi*kStride], y = m_verts[vi*kStride+1], z = m_verts[vi*kStride+2];
            mnx = qMin(mnx, x); mny = qMin(mny, y); mnz = qMin(mnz, z);
            mxx = qMax(mxx, x); mxy = qMax(mxy, y); mxz = qMax(mxz, z);
            any = true;
        }
    }
    if (!any) return false;
    center = QVector3D((mnx + mxx) * 0.5f, (mny + mxy) * 0.5f, (mnz + mxz) * 0.5f);
    radius = qMax(0.05f, QVector3D(mxx - mnx, mxy - mny, mxz - mnz).length() * 0.5f * 1.25f);
    return true;
}

void GLModelWidget::followParts(const QVector<int>& parts) { m_followParts = parts; }

void GLModelWidget::resetView()
{
    m_followParts.clear();
    QVector3D c; float r;
    liveBounds(c, r);
    if (m_camAnim) m_camAnim->stop();
    m_center = c; m_dist = r * 2.6f; m_yaw = 0.6f; m_pitch = 0.25f;
    m_tgtCenter = m_center; m_tgtDist = m_dist; m_tgtYaw = m_yaw; m_tgtPitch = m_pitch;
    emit cameraChanged();
    update();
}

void GLModelWidget::frameAll(bool keepRotation, bool animate)
{
    m_followParts.clear();
    if (!keepRotation) { resetView(); return; }
    QVector3D c; float r;
    liveBounds(c, r);
    frameRegionKeepRotation(c, r, animate);
}

void GLModelWidget::frameRegionKeepRotation(const QVector3D& center, float radius, bool animate)
{
    const bool gliding = m_camAnim && m_camAnim->isActive();
    frameRegion(center, radius, gliding ? m_tgtYaw : m_yaw, gliding ? m_tgtPitch : m_pitch, animate);
}

void GLModelWidget::frameRegion(const QVector3D& center, float radius, float yawRad, float pitchRad, bool animate)
{
    const float half = m_fov * 0.5f * kPi / 180.0f;
    const float dist = qMax(radius, 0.001f) / qMax(0.05f, std::sin(half));   // sphere fits the FOV
    m_tgtCenter = center; m_tgtYaw = yawRad; m_tgtPitch = pitchRad; m_tgtDist = dist;
    if (!animate) {
        if (m_camAnim) m_camAnim->stop();
        m_center = center; m_yaw = yawRad; m_pitch = pitchRad; m_dist = dist;
        emit cameraChanged();
        update();
        return;
    }
    while (m_yaw - m_tgtYaw >  kPi) m_yaw -= 2.0f * kPi;   // shortest angular path
    while (m_tgtYaw - m_yaw >  kPi) m_yaw += 2.0f * kPi;
    if (!m_camAnim) {
        m_camAnim = new QTimer(this);
        connect(m_camAnim, &QTimer::timeout, this, [this]() {
            const float t = 0.28f;
            m_yaw   += (m_tgtYaw   - m_yaw)   * t;
            m_pitch += (m_tgtPitch - m_pitch) * t;
            m_dist  += (m_tgtDist  - m_dist)  * t;
            m_center += (m_tgtCenter - m_center) * t;
            const bool done = qAbs(m_yaw - m_tgtYaw) < 1e-3f && qAbs(m_pitch - m_tgtPitch) < 1e-3f
                           && qAbs(m_dist - m_tgtDist) < 1e-3f && (m_center - m_tgtCenter).length() < 1e-3f;
            if (done) { m_yaw = m_tgtYaw; m_pitch = m_tgtPitch; m_dist = m_tgtDist; m_center = m_tgtCenter; m_camAnim->stop(); emit cameraChanged(); }
            update();
        });
    }
    m_camAnim->start(16);
}

void GLModelWidget::setAutoSpin(bool on)
{
    if (on) {
        // A camera glide eases m_yaw toward a fixed target every tick; if it's still running when
        // the turntable starts, the two fight over m_yaw. Finalise the glide first.
        if (m_camAnim && m_camAnim->isActive()) { m_center = m_tgtCenter; m_dist = m_tgtDist; m_pitch = m_tgtPitch; m_camAnim->stop(); }
        if (!m_spinTimer) {
            m_spinTimer = new QTimer(this);
            connect(m_spinTimer, &QTimer::timeout, this, [this]() { m_yaw += m_spinSpeed; update(); });
        }
        m_spinTimer->start(33);
    } else if (m_spinTimer) {
        m_spinTimer->stop();
    }
}

// Ray-pick the nearest visible triangle under a widget-space point: the same view/projection
// paintGL uses, the pixel unprojected into a world ray, Möller–Trumbore against the LIVE vertices.
int GLModelWidget::pickPart(const QPoint& posPx) const
{
    const int nv = m_verts.size() / kStride;
    if (nv <= 0 || m_indices.isEmpty() || width() <= 0 || height() <= 0) return -1;
    QVector3D eye; QMatrix4x4 proj, view;
    cameraBasis(eye, proj, view, float(width()) / float(height()));
    bool ok = false;
    const QMatrix4x4 invVP = (proj * view).inverted(&ok);
    if (!ok) return -1;
    const float nx = 2.0f * float(posPx.x()) / float(width()) - 1.0f;
    const float ny = 1.0f - 2.0f * float(posPx.y()) / float(height());
    const QVector4D pn = invVP * QVector4D(nx, ny, -1.0f, 1.0f);
    const QVector4D pf = invVP * QVector4D(nx, ny,  1.0f, 1.0f);
    if (qFuzzyIsNull(pn.w()) || qFuzzyIsNull(pf.w())) return -1;
    const QVector3D ro = pn.toVector3D() / pn.w();
    const QVector3D rd = (pf.toVector3D() / pf.w() - ro).normalized();
    float best = 1e30f; int bestPart = -1;
    for (int pi = 0; pi < m_parts.size(); ++pi) {
        const Part& p = m_parts[pi];
        if (!p.visible) continue;
        const int end = qMin(p.offset + p.count, m_indices.size());
        for (int k = p.offset; k + 2 < end; k += 3) {
            const int i0 = int(m_indices[k]), i1 = int(m_indices[k + 1]), i2 = int(m_indices[k + 2]);
            if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= nv || i1 >= nv || i2 >= nv) continue;
            const QVector3D v0(m_verts[i0*kStride], m_verts[i0*kStride+1], m_verts[i0*kStride+2]);
            const QVector3D v1(m_verts[i1*kStride], m_verts[i1*kStride+1], m_verts[i1*kStride+2]);
            const QVector3D v2(m_verts[i2*kStride], m_verts[i2*kStride+1], m_verts[i2*kStride+2]);
            const QVector3D e1 = v1 - v0, e2 = v2 - v0;
            const QVector3D pv = QVector3D::crossProduct(rd, e2);
            const float det = QVector3D::dotProduct(e1, pv);
            if (std::fabs(det) < 1e-8f) continue;
            const float inv = 1.0f / det;
            const QVector3D tv = ro - v0;
            const float u = QVector3D::dotProduct(tv, pv) * inv;
            if (u < 0.0f || u > 1.0f) continue;
            const QVector3D qv = QVector3D::crossProduct(tv, e1);
            const float vp = QVector3D::dotProduct(rd, qv) * inv;
            if (vp < 0.0f || u + vp > 1.0f) continue;
            const float t = QVector3D::dotProduct(e2, qv) * inv;
            if (t > 1e-4f && t < best) { best = t; bestPart = pi; }
        }
    }
    return bestPart;
}

// ── Mouse ──────────────────────────────────────────────────────────────────────────────────────

void GLModelWidget::mousePressEvent(QMouseEvent* e)
{
    m_lastPos = e->pos();
    if (e->button() == Qt::RightButton) m_rightPressPx = e->pos();
    if (e->button() == Qt::LeftButton) {
        m_leftPressPx = e->pos();
        // Cleared on EVERY press, not only when consumed: the flag is set by a double-click and
        // eaten by the release that follows — but that release can be a drag, which fails the
        // threshold test and would leave the flag set to swallow the next genuine click.
        m_swallowLeftClick = false;
    }
    if (e->button() == Qt::MiddleButton) { resetView(); e->accept(); }
}

void GLModelWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::RightButton && (e->pos() - m_rightPressPx).manhattanLength() < 4) {
        emit partRightClicked(pickPart(e->pos()), e->globalPosition().toPoint());
        e->accept();
    }
    // Left-click select on RELEASE, gated on the cursor barely having moved: a left-DRAG orbits
    // and must never change the selection. The modifiers ride along — what Ctrl means is the
    // parts panel's business.
    if (e->button() == Qt::LeftButton && (e->pos() - m_leftPressPx).manhattanLength() < 4) {
        if (m_swallowLeftClick) m_swallowLeftClick = false;
        else emit partClicked(pickPart(e->pos()), e->modifiers());
    }
}

void GLModelWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    // A modifier double-click is inert (FOX's 2026-09 fix): the press already applied the gesture,
    // and a second toggle would undo it.
    if (e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) { m_swallowLeftClick = true; return; }
    m_swallowLeftClick = true;
    const int part = pickPart(e->pos());
    if (part >= 0) {
        QVector3D c; float r;
        if (partsBounds(QVector<int>{part}, c, r)) { m_followParts.clear(); frameRegionKeepRotation(c, r, true); }
    }
    emit partFocused(part);
}

void GLModelWidget::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint d = e->pos() - m_lastPos;
    m_lastPos = e->pos();
    if (e->buttons() & Qt::LeftButton) {
        m_yaw   -= d.x() * 0.01f;
        m_pitch += d.y() * 0.01f;
        m_pitch = qBound(-kPitchLimit, m_pitch, kPitchLimit);
        emit cameraChanged();
        update();
    } else if (e->buttons() & Qt::RightButton) {
        const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
        const float cy = std::cos(m_yaw),   sy = std::sin(m_yaw);
        const QVector3D viewDir = -QVector3D(cp * sy, sp, cp * cy);
        QVector3D right = QVector3D::crossProduct(viewDir, QVector3D(0, 1, 0));
        if (right.lengthSquared() < 1e-8f) right = QVector3D(1, 0, 0);
        right.normalize();
        const QVector3D camUp = QVector3D::crossProduct(right, viewDir).normalized();
        const float s = m_dist * 0.0015f;
        m_center -= right * (d.x() * s);
        m_center += camUp * (d.y() * s);
        emit cameraChanged();
        update();
    }
}

void GLModelWidget::wheelEvent(QWheelEvent* e)
{
    const float steps = e->angleDelta().y() / 120.0f;
    m_dist *= std::pow(0.9f, steps);
    m_dist = qMax(m_radius * 0.05f, m_dist);   // never through the model; no maximum
    emit cameraChanged();
    update();
}

// ── Capture ────────────────────────────────────────────────────────────────────────────────────

// The whole pipeline into an off-screen target of the given size. Every pass sizes itself from
// m_fbW/m_fbH and every "back to the screen" bind goes through targetFbo(), so pointing those at
// our own FBO renders the entire thing — SSAO G-buffer, shadow pass, stencil silhouette — with no
// per-pass special-casing.
QImage GLModelWidget::renderOffscreen(int W, int H, bool /*keepAlphaAsCoverage*/)
{
    if (W <= 0 || H <= 0 || m_prog == 0) return {};
    makeCurrent();
    GLint maxTex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
    if (maxTex <= 0) maxTex = 8192;
    if (W > maxTex || H > maxTex) { doneCurrent(); return {}; }
    // CombinedDepthStencil, not Depth: the silhouette is a stencil pass and a target without a
    // stencil buffer would drop it.
    // Single-sampled on purpose (D4): the shader sees uA2C=0 and hard-cuts, and the downscale of a
    // supersampled capture does the anti-aliasing — coverage on a 1-sample target dithers to speckle.
    std::unique_ptr<QOpenGLFramebufferObject> fbo =
        std::make_unique<QOpenGLFramebufferObject>(W, H, QOpenGLFramebufferObject::CombinedDepthStencil);
    if (!fbo->isValid()) { doneCurrent(); return {}; }
    const int prevW = m_fbW, prevH = m_fbH;
    m_captureFbo = fbo->handle();
    m_fbW = W; m_fbH = H;
    fbo->bind();
    glViewport(0, 0, W, H);
    paintGL();
    glFinish();
    glGetError();
    fbo->release();
    QImage img = fbo->toImage();
    m_captureFbo = 0;
    m_fbW = prevW; m_fbH = prevH;
    doneCurrent();
    update();
    return img;
}

QImage GLModelWidget::grabSupersampled(int factor)
{
    if (factor <= 1 || m_prog == 0) return {};
    const qreal dpr = devicePixelRatioF();
    const int baseW = qMax(1, int(width() * dpr)), baseH = qMax(1, int(height() * dpr));
    // Shrink the factor until the driver accepts the size rather than returning nothing.
    for (int f = factor; f > 1; --f) {
        QImage img = renderOffscreen(baseW * f, baseH * f, false);
        if (!img.isNull()) return img;
    }
    return {};
}

QImage GLModelWidget::renderAtSize(int w, int h) { return renderOffscreen(w, h, false); }

QImage GLModelWidget::renderToImage(int scalePercent, bool transparentBg, bool cropToModel)
{
    if (m_indexCount == 0 && !m_hasPending) return {};
    const qreal dpr = devicePixelRatioF();
    const int W = qMax(1, int(width() * dpr * qMax(10, scalePercent) / 100.0));
    const int H = qMax(1, int(height() * dpr * qMax(10, scalePercent) / 100.0));
    // Guides off for the capture, restored after.
    const bool ovl = m_overlaysOn, grad = m_bgGradient, tclear = m_transparentClear;
    m_overlaysOn = false; m_bgGradient = grad && !transparentBg; m_transparentClear = transparentBg;
    QImage img = renderOffscreen(W, H, false);
    m_overlaysOn = ovl; m_bgGradient = grad; m_transparentClear = tclear;
    if (img.isNull()) return img;
    img = img.convertToFormat(QImage::Format_RGBA8888);
    if (cropToModel && transparentBg) {
        int x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
        for (int y = 0; y < img.height(); ++y) {
            const uchar* row = img.constScanLine(y);
            for (int x = 0; x < img.width(); ++x)
                if (row[x * 4 + 3] >= 8) { x0 = qMin(x0, x); x1 = qMax(x1, x); y0 = qMin(y0, y); y1 = qMax(y1, y); }
        }
        if (x1 >= x0 && y1 >= y0) img = img.copy(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    }
    return img;
}

QImage GLModelWidget::grabThumbnail(int size)
{
    if (m_prog == 0) return {};
    makeCurrent();
    if (m_hasPending) uploadPending();
    if (m_indexCount == 0) { doneCurrent(); return {}; }
    if (!m_thumbFbo || m_thumbFboSize != size) {
        m_thumbFbo = std::make_unique<QOpenGLFramebufferObject>(size, size, QOpenGLFramebufferObject::Depth);
        m_thumbFboSize = size;
    }
    QOpenGLFramebufferObject& fbo = *m_thumbFbo;
    if (!fbo.isValid()) { doneCurrent(); return {}; }
    fbo.bind();
    glViewport(0, 0, size, size);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.12f, 0.12f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const float cp = std::cos(0.25f), sp = std::sin(0.25f), cy = std::cos(0.6f), sy = std::sin(0.6f);
    const QVector3D dir(cp * sy, sp, cp * cy);
    const QVector3D eye = m_center + dir * (m_radius * 2.6f);
    QMatrix4x4 proj; proj.perspective(45.0f, 1.0f, qMax(0.001f, m_radius * 0.02f), m_radius * 20.0f);
    QMatrix4x4 view; view.lookAt(eye, m_center, QVector3D(0, 1, 0));
    const QMatrix4x4 model, mvp = proj * view * model;
    glUseProgram(m_prog);
    glUniformMatrix4fv(uni(m_prog, "uMVP"), 1, GL_FALSE, mvp.constData());
    glUniformMatrix4fv(uni(m_prog, "uModel"), 1, GL_FALSE, model.constData());
    const QVector3D ld = (eye - m_center).normalized();
    glUniform3f(uni(m_prog, "uLightDir"), ld.x(), ld.y(), ld.z());
    glUniform3f(uni(m_prog, "uViewPos"), eye.x(), eye.y(), eye.z());
    glUniform3f(uni(m_prog, "uBase"), 0.78f, 0.78f, 0.78f);
    glUniform1i(uni(m_prog, "uHasTex"), 0); glUniform1i(uni(m_prog, "uHasNormal"), 0); glUniform1i(uni(m_prog, "uPbr"), 0);
    glUniform1i(uni(m_prog, "uViewChannel"), 0); glUniform1i(uni(m_prog, "uA2C"), 0);
    glUniform1i(uni(m_prog, "uHasOrm"), 0); glUniform1i(uni(m_prog, "uHasEmissive"), 0); glUniform1i(uni(m_prog, "uAlphaMode"), 0);
    glUniform1i(uni(m_prog, "uSolid"), 0); glUniform2f(uni(m_prog, "uNdcOffset"), 0.0f, 0.0f);
    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
    // glFinish localises a driver fault to the model that caused it (a burst of thumbnails
    // otherwise queues GPU work that can TDR during an unrelated model's parse).
    glFinish();
    glGetError();
    fbo.release();
    QImage img = fbo.toImage();
    doneCurrent();
    update();
    return img;
}

}  // namespace core::gl
