#include "core/gl/ThumbnailRenderer.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMatrix4x4>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QThread>
#include <QVector3D>
#include <cfloat>

namespace core::gl {
namespace {

constexpr int kMaxSize = 512;

// The render queue serves newest-first (LIFO): a grid scroll re-requests the rows now on screen, and
// those must render BEFORE the rows the user already scrolled past — a FIFO buried the visible region
// behind a stale backlog, so thumbnails "stopped loading" during a fast scroll. The backlog is capped
// at kMaxQueue; past that the OLDEST (furthest off-screen) pending request is dropped so the queue
// stays near the viewport and cannot grow without bound over a long scroll.
constexpr int kMaxQueue = 600;

const char* const kVert = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUv;
uniform mat4 uMvp;
uniform mat4 uModel;
out vec2 vUv;
out vec3 vN;
void main() {
    vUv = aUv;
    vN = mat3(uModel) * aNormal;
    gl_Position = uMvp * vec4(aPos, 1.0);
}
)";

// Base colour only. The two-term wrap is not lighting in any physical sense — it exists so a
// silhouette is legible at 96 pixels. Alpha is written opaque for the model and left at zero
// everywhere else, which is what makes the background transparent.
const char* const kFrag = R"(#version 330 core
in vec2 vUv;
in vec3 vN;
uniform sampler2D uTex;
uniform int uHasTex;
out vec4 fragColor;
void main() {
    vec3 base = uHasTex != 0 ? texture(uTex, vUv).rgb : vec3(0.72);
    vec3 n = normalize(vN);
    float key = clamp(dot(n, normalize(vec3(0.4, 0.7, 0.6))) * 0.5 + 0.5, 0.0, 1.0);
    float fill = clamp(dot(n, normalize(vec3(-0.5, 0.2, -0.4))) * 0.5 + 0.5, 0.0, 1.0);
    fragColor = vec4(base * (0.55 + 0.45 * key) + base * 0.18 * fill, 1.0);
}
)";

inline qint64 pixmapBytes(const QPixmap& pm) { return qint64(pm.width()) * pm.height() * 4 + 128; }

int registerTypes()
{
    qRegisterMetaType<core::AssetId>("core::AssetId");
    return 0;
}
const int s_registered = registerTypes();

}  // namespace

// ── worker ─────────────────────────────────────────────────────────────────────────────────────

ThumbnailWorker::ThumbnailWorker(QOpenGLContext* ctx, QOffscreenSurface* surface, ThumbnailLoader loader, ThumbnailGuard guard)
    : m_loader(std::move(loader)), m_guard(std::move(guard)), m_ctx(ctx), m_surface(surface)
{
}

// CONTRACT: this MUST emit done() exactly once on every path. The renderer's m_busy flag is
// cleared by that signal and by nothing else, so a path that returns without emitting stops the
// entire grid permanently and silently. Do not add an early return here.
void ThumbnailWorker::renderOne(const core::AssetId& id, int size, int generation, const QString& diskPath)
{
    QImage img;
    bool fromDisk = false;
    // THE DISK FIRST: a PNG read is a fraction of a millisecond against the parse-decode-draw it
    // stands in for. A file that will not load falls through to a render, so a corrupt entry costs
    // one re-render rather than a permanently blank cell.
    if (!m_dead && !diskPath.isEmpty() && QFile::exists(diskPath)) {
        img = QImage(diskPath);
        if (!img.isNull()) fromDisk = true;
    }
    if (!m_dead && !fromDisk) {
        bool ok = true;
        QString what;
        if (m_guard) ok = m_guard([&] { img = draw(id, size); }, &what);
        else img = draw(id, size);
        if (!ok) {
            qWarning("thumbnails: fault rendering %u:%llu — %s", unsigned(id.scheme), (unsigned long long)id.value, qUtf8Printable(what));
            img = QImage();
            // The context may be left current and the framebuffer bound. Drop the GL objects so
            // the next thumbnail rebuilds them cleanly.
            if (m_ctx && m_ctx->isValid()) { delete m_fbo; m_fbo = nullptr; m_fboSize = 0; m_ctx->doneCurrent(); }
        }
        // A render that failed is NOT written: a blank PNG would be read back for ever.
        if (!diskPath.isEmpty() && !img.isNull()) {
            QDir().mkpath(QFileInfo(diskPath).absolutePath());
            if (!img.save(diskPath, "PNG")) qWarning("thumbnails: could not write %s", qUtf8Printable(diskPath));
        }
    }
    emit done(id, size, img, generation, fromDisk);
}

void ThumbnailWorker::teardown()
{
    if (m_dead) return;
    m_dead = true;
    if (m_ctx && m_ctx->makeCurrent(m_surface)) {
        QOpenGLFunctions_3_3_Core gl;
        if (gl.initializeOpenGLFunctions() && m_vao) {
            gl.glDeleteVertexArrays(1, &m_vao);
            gl.glDeleteBuffers(1, &m_vbo);
            gl.glDeleteBuffers(1, &m_ibo);
            gl.glDeleteTextures(1, &m_tex);
        }
        delete m_fbo; m_fbo = nullptr;
        delete m_prog; m_prog = nullptr;
        m_ctx->doneCurrent();
    }
    delete m_ctx;
    m_ctx = nullptr;
}

QImage ThumbnailWorker::draw(const AssetId& id, int size)
{
    size = qBound(32, size, kMaxSize);
    if (!m_loader) return {};
    const ThumbnailModel lm = m_loader(id);
    if (!lm.ok || lm.geo.isEmpty()) return {};

    if (!m_ctx || !m_ctx->makeCurrent(m_surface)) return {};
    QOpenGLFunctions_3_3_Core gl;
    if (!gl.initializeOpenGLFunctions()) { m_ctx->doneCurrent(); return {}; }

    // Render at double size and scale down: one free supersample is cheaper than multisampling.
    const int render = qMin(size * 2, kMaxSize);
    if (!m_fbo || m_fboSize != render) {
        delete m_fbo;
        QOpenGLFramebufferObjectFormat ffmt;
        ffmt.setAttachment(QOpenGLFramebufferObject::Depth);
        ffmt.setInternalTextureFormat(GL_RGBA8);
        m_fbo = new QOpenGLFramebufferObject(render, render, ffmt);
        m_fboSize = render;
    }
    if (!m_fbo->isValid()) { m_ctx->doneCurrent(); return {}; }
    QOpenGLFramebufferObject& fbo = *m_fbo;
    fbo.bind();
    if (!m_prog) {
        m_prog = new QOpenGLShaderProgram;
        if (!m_prog->addShaderFromSourceCode(QOpenGLShader::Vertex, kVert)
            || !m_prog->addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag) || !m_prog->link()) {
            qWarning("thumbnails: shader failed — %s", qUtf8Printable(m_prog->log()));
            delete m_prog; m_prog = nullptr;
            fbo.release(); m_ctx->doneCurrent();
            return {};
        }
    }
    QOpenGLShaderProgram& prog = *m_prog;

    // Bounds over the VISIBLE parts (a group hidden in the file stays hidden here too).
    QVector<bool> shown(lm.geo.parts.size(), true);
    for (int i = 0; i < lm.geo.parts.size(); ++i) {
        const Part& p = lm.geo.parts[i];
        shown[i] = p.visible;
        for (int g = p.group; shown[i] && g >= 0 && g < lm.geo.groups.size(); g = lm.geo.groups[g].parent)
            if (!lm.geo.groups[g].visible) shown[i] = false;
    }
    QVector3D lo(FLT_MAX, FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (int i = 0; i < lm.geo.parts.size(); ++i) {
        if (!shown[i]) continue;
        const Part& p = lm.geo.parts[i];
        for (uint32_t k = p.indexStart; k < p.indexStart + p.indexCount && int(k) < lm.geo.indices.size(); ++k) {
            const int vi = int(lm.geo.indices[int(k)]);
            if (vi < 0 || vi >= lm.geo.vertices.size()) continue;   // out-of-range (restart/padding) index
            const Vertex& v = lm.geo.vertices[vi];
            lo.setX(qMin(lo.x(), v.px)); lo.setY(qMin(lo.y(), v.py)); lo.setZ(qMin(lo.z(), v.pz));
            hi.setX(qMax(hi.x(), v.px)); hi.setY(qMax(hi.y(), v.py)); hi.setZ(qMax(hi.z(), v.pz));
        }
    }
    if (lo.x() > hi.x()) { fbo.release(); m_ctx->doneCurrent(); return {}; }
    const QVector3D centre = (lo + hi) * 0.5f;
    const float radius = qMax(0.001f, (hi - lo).length() * 0.5f);

    QMatrix4x4 proj; proj.perspective(35.0f, 1.0f, radius * 0.05f, radius * 12.0f);
    QMatrix4x4 view;
    // A three-quarter view from slightly above: straight-on makes a helmet and a hat look the same.
    const QVector3D eye = centre + QVector3D(0.62f, 0.42f, 1.0f).normalized() * (radius * 3.0f);
    view.lookAt(eye, centre, QVector3D(0, 1, 0));
    const QMatrix4x4 model;

    gl.glViewport(0, 0, render, render);
    gl.glClearColor(0.0f, 0.0f, 0.0f, 0.0f);   // transparent
    gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    gl.glEnable(GL_DEPTH_TEST);
    gl.glDisable(GL_CULL_FACE);   // not every engine's meshes are consistently wound

    prog.bind();
    prog.setUniformValue("uMvp", proj * view * model);
    prog.setUniformValue("uModel", model);
    prog.setUniformValue("uTex", 0);

    if (!m_vao) { gl.glGenVertexArrays(1, &m_vao); gl.glGenBuffers(1, &m_vbo); gl.glGenBuffers(1, &m_ibo); gl.glGenTextures(1, &m_tex); }
    gl.glBindVertexArray(m_vao);
    // One interleaved buffer (pos, normal, uv) for the whole mesh, one index buffer, one draw per
    // visible part with its material's base map on the scratch texture.
    QVector<float> inter; inter.reserve(lm.geo.vertices.size() * 8);
    for (const Vertex& v : lm.geo.vertices) inter << v.px << v.py << v.pz << v.nx << v.ny << v.nz << v.u << v.v;
    gl.glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    gl.glBufferData(GL_ARRAY_BUFFER, inter.size() * qsizetype(sizeof(float)), inter.constData(), GL_STREAM_DRAW);
    gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo);
    gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER, lm.geo.indices.size() * qsizetype(sizeof(quint32)), lm.geo.indices.constData(), GL_STREAM_DRAW);
    const int stride = 8 * sizeof(float);
    gl.glEnableVertexAttribArray(0); gl.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
    gl.glEnableVertexAttribArray(1); gl.glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));
    gl.glEnableVertexAttribArray(2); gl.glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(6 * sizeof(float)));

    int lastUploaded = -2;
    for (int i = 0; i < lm.geo.parts.size(); ++i) {
        if (!shown[i]) continue;
        const Part& p = lm.geo.parts[i];
        if (p.indexCount == 0) continue;
        const int mi = p.materialIndex;
        const bool hasTex = mi >= 0 && mi < lm.baseByMaterial.size() && !lm.baseByMaterial[mi].isNull();
        if (hasTex && mi != lastUploaded) {
            QImage src = lm.baseByMaterial[mi];
            if (src.width() > 256 || src.height() > 256) src = src.scaled(256, 256, Qt::KeepAspectRatio, Qt::FastTransformation);
            src = src.convertToFormat(QImage::Format_RGBA8888);
            gl.glActiveTexture(GL_TEXTURE0);
            gl.glBindTexture(GL_TEXTURE_2D, m_tex);
            gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, int(src.bytesPerLine() / 4));
            gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, src.width(), src.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, src.constBits());
            gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            lastUploaded = mi;
        }
        prog.setUniformValue("uHasTex", hasTex ? 1 : 0);
        gl.glDrawElements(GL_TRIANGLES, int(p.indexCount), GL_UNSIGNED_INT, reinterpret_cast<void*>(qintptr(p.indexStart) * sizeof(quint32)));
    }
    gl.glBindVertexArray(0);
    prog.release();

    QImage out = fbo.toImage();
    fbo.release();
    m_ctx->doneCurrent();
    if (out.isNull()) return {};
    if (out.format() != QImage::Format_ARGB32) out = out.convertToFormat(QImage::Format_ARGB32);
    return out.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

// ── renderer ───────────────────────────────────────────────────────────────────────────────────

ThumbnailRenderer::ThumbnailRenderer(QObject* parent) : QObject(parent) {}
ThumbnailRenderer::~ThumbnailRenderer() { shutdown(); }

bool ThumbnailRenderer::has(const AssetId& id, int size) const { return m_cache.contains({id, size}); }
QPixmap ThumbnailRenderer::cached(const AssetId& id, int size) const { return m_cache.value({id, size}); }
int ThumbnailRenderer::outstanding() const { return m_queue.size() + (m_busy ? 1 : 0); }

void ThumbnailRenderer::request(const AssetId& id, int size)
{
    if (!id.isValid()) return;
    const ThumbKey key{id, size};
    if (m_cache.contains(key)) return;
    // pump() takes the key OUT of m_queued when it submits, so without this the cell being
    // rendered right now gets queued all over again on the next scroll settle.
    if (m_busy && key == m_inFlight) return;
    if (m_queued.contains(key)) {
        // Already pending: a re-request means the row just (re)entered the viewport, so make it the
        // freshest in the queue — the LIFO pump then renders it next, ahead of rows scrolled past.
        m_queue.removeOne(key);
        m_queue.append(key);
        return;
    }
    m_queued.insert(key);
    m_queue.append(key);
    while (m_queue.size() > kMaxQueue) { const ThumbKey old = m_queue.takeFirst(); m_queued.remove(old); }
    pump();
}

int ThumbnailRenderer::prerender(const QVector<AssetId>& ids, int size)
{
    int queued = 0;
    for (const AssetId& id : ids) {
        const ThumbKey key{id, size};
        if (!id.isValid() || m_cache.contains(key) || m_queued.contains(key)) continue;
        if (m_busy && key == m_inFlight) continue;
        m_queued.insert(key);
        m_queue.append(key);
        ++queued;
    }
    pump();
    return queued;
}

QString ThumbnailRenderer::diskPathFor(const AssetId& id, int size) const
{
    if (m_diskDir.isEmpty() || !id.isValid()) return QString();
    // The asset's own id, not a list row: rows are renumbered by every rescan. The scheme is in
    // the name so two schemes that happen to share bits never collide.
    const QString key = m_diskKey ? m_diskKey(id) : QString::number(id.value, 16).rightJustified(16, QLatin1Char('0'));
    if (key.isEmpty()) return QString();   // the store cannot name it stably: not cached on disk
    return QStringLiteral("%1/s%2_%3_%4.png").arg(m_diskDir).arg(id.scheme).arg(key).arg(size);
}

void ThumbnailRenderer::prewarm() { ensureWorker(); }
void ThumbnailRenderer::cancelQueued() { m_queue.clear(); m_queued.clear(); }

void ThumbnailRenderer::pump()
{
    if (m_busy || m_queue.isEmpty() || m_paused) return;
    if (!ensureWorker()) { cancelQueued(); return; }
    const ThumbKey key = m_queue.takeLast();   // LIFO: the most recently requested (on-screen) row first
    m_queued.remove(key);
    m_busy = true;
    m_inFlight = key;
    emit submit(key.id, key.size, m_generation, diskEnabled() ? diskPathFor(key.id, key.size) : QString());
}

void ThumbnailRenderer::onDone(const AssetId& id, int size, const QImage& img, int generation, bool fromDisk)
{
    if (!m_thread) return;   // torn down: the cache was dropped on purpose
    const ThumbKey done{id, size};
    // Clear the flag only for the submission that is actually outstanding: a late result from
    // before a reset() must not un-busy a render that has since started.
    if (m_busy && done == m_inFlight) { m_busy = false; m_inFlight = ThumbKey{}; }
    if (m_staleInFlight.remove(done)) {
        // A re-render was asked for while this one ran — it may have read the old PNG back.
        // Dropped, and drawn again (audit 2026-09-25: the request was ignored and this cached).
        request(id, size);
        pump();
        return;
    }
    if (generation == m_generation) {
        if (img.isNull()) ++m_stats.failed;
        else if (fromDisk) ++m_stats.fromDisk;
        else ++m_stats.rendered;
        // A failure caches a null pixmap on purpose: a model that cannot be loaded is otherwise
        // re-parsed every time it scrolls back into view.
        const QPixmap pm = img.isNull() ? QPixmap() : QPixmap::fromImage(img);
        if (!m_cache.contains(done)) m_order.append(done);
        else m_bytes -= pixmapBytes(m_cache.value(done));
        m_cache.insert(done, pm);
        m_bytes += pixmapBytes(pm);
        while (m_bytes > kMaxBytes && m_order.size() > 1) {
            const ThumbKey old = m_order.takeFirst();
            m_bytes -= pixmapBytes(m_cache.take(old));
        }
        emit ready(id, size);
    }
    pump();
}

void ThumbnailRenderer::setLoader(ThumbnailLoader loader)
{
    m_loader = std::move(loader);
    if (m_worker) {
        ThumbnailWorker* w = m_worker;
        const ThumbnailLoader copy = m_loader;
        // Queued: it lands between two renders, and every request submitted after this call runs
        // with the new loader (the worker's queue is FIFO).
        QMetaObject::invokeMethod(m_worker, [w, copy] { w->replaceLoader(copy); }, Qt::QueuedConnection);
    }
}

bool ThumbnailRenderer::ensureWorker()
{
    if (m_worker) return true;
    if (m_workerFailed) return false;
    if (!m_loader) { qWarning("thumbnails: no loader installed"); return false; }
    auto* ctx = new QOpenGLContext;
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setAlphaBufferSize(8);
    ctx->setFormat(fmt);
    if (!ctx->create()) {
        delete ctx; m_workerFailed = true;
        qWarning("thumbnails: no OpenGL context — the grid will show names only");
        return false;
    }
    // A QOffscreenSurface has to be CREATED on the GUI thread; it may then be made current from
    // another one. So it stays owned here and only the context moves.
    m_surface = new QOffscreenSurface;
    m_surface->setFormat(ctx->format());
    m_surface->create();
    if (!m_surface->isValid()) {
        delete m_surface; m_surface = nullptr; delete ctx; m_workerFailed = true;
        qWarning("thumbnails: no offscreen surface — the grid will show names only");
        return false;
    }
    m_thread = new QThread;
    m_thread->setObjectName(QStringLiteral("thumbnails"));
    ctx->moveToThread(m_thread);
    m_worker = new ThumbnailWorker(ctx, m_surface, m_loader, m_guard);
    m_worker->moveToThread(m_thread);
    connect(this, &ThumbnailRenderer::submit, m_worker, &ThumbnailWorker::renderOne);
    connect(m_worker, &ThumbnailWorker::done, this, &ThumbnailRenderer::onDone);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread->start(QThread::LowPriority);   // a thumbnail is never worth a dropped viewport frame
    if (auto* app = QCoreApplication::instance())
        connect(app, &QCoreApplication::aboutToQuit, this, &ThumbnailRenderer::shutdown);
    return true;
}

int ThumbnailRenderer::refresh(const AssetId& id)
{
    if (!id.isValid()) return 0;
    QVector<int> sizes;
    for (auto it = m_cache.constBegin(); it != m_cache.constEnd(); ++it)
        if (it.key().id == id) sizes.append(it.key().size);
    for (int sz : sizes) { const ThumbKey k{id, sz}; m_order.removeAll(k); m_bytes -= pixmapBytes(m_cache.take(k)); }
    // AND THE DISK, every size — a re-render that read the old PNG back would change nothing. Matched
    // exactly (s<scheme>_<key>_<digits>.png): cutting the name at its second '_' would, for a key
    // that holds one, match every thumbnail sharing the prefix.
    if (!m_diskDir.isEmpty()) {
        const QString key = m_diskKey ? m_diskKey(id) : QString::number(id.value, 16).rightJustified(16, QLatin1Char('0'));
        const QString prefix = QStringLiteral("s%1_%2_").arg(id.scheme).arg(key);
        QDir d(m_diskDir);
        if (!key.isEmpty())
            for (const QString& f : d.entryList({prefix + QStringLiteral("*.png")}, QDir::Files)) {
                const QString sz = f.mid(prefix.size()).chopped(4);
                bool num = false; sz.toInt(&num);
                if (num) d.remove(f);
            }
    }
    // The one rendering now is not in the cache yet: mark it, so its result is dropped and redrawn.
    if (m_busy && m_inFlight.id == id) { m_staleInFlight.insert(m_inFlight); if (!sizes.contains(m_inFlight.size)) sizes.append(m_inFlight.size); }
    for (int sz : sizes) if (!(m_busy && ThumbKey{id, sz} == m_inFlight)) request(id, sz);
    return sizes.size();
}

void ThumbnailRenderer::reset()
{
    ++m_generation;
    cancelQueued();
    if (m_worker && m_thread && m_thread->isRunning()) {
        // Wait for whatever is under way — it is reading the store that is about to be replaced.
        // The one place the GUI thread legitimately blocks on the render thread: once per
        // re-index, never while browsing.
        QMetaObject::invokeMethod(m_worker, [] {}, Qt::BlockingQueuedConnection);
    }
    // m_busy is deliberately NOT cleared: the drain guarantees the in-flight render has POSTED its
    // done(), which clears the flag itself and is discarded on the generation check.
    m_cache.clear(); m_order.clear(); m_bytes = 0;
}

void ThumbnailRenderer::shutdown()
{
    if (!m_thread) return;
    cancelQueued();
    if (m_worker && m_thread->isRunning())
        QMetaObject::invokeMethod(m_worker, "teardown", Qt::BlockingQueuedConnection);
    m_thread->quit();
    if (m_thread->wait(3000)) delete m_thread;
    else qWarning("thumbnails: render thread did not stop — leaking it");
    m_thread = nullptr; m_worker = nullptr;
    delete m_surface; m_surface = nullptr;
    m_busy = false; m_inFlight = ThumbKey{};
    m_staleInFlight.clear();
    m_cache.clear(); m_order.clear(); m_bytes = 0;
    m_workerFailed = true;   // do not build another one during teardown
}

}  // namespace core::gl
