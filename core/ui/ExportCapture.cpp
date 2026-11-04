#include "core/ui/ExportCapture.h"
#include "core/export/GifEncoder.h"
#include "core/gl/GLModelWidget.h"

#include <QColor>
#include <QSettings>
#include <QTimer>
#include <QVector3D>

#include <QImage>
#include <QElapsedTimer>
#include <QtGlobal>
#include <cstring>
#include <cmath>
#include <vector>
#include <utility>

namespace core::capture {
namespace {

using gl::GLModelWidget;

// ── Frame buffer helpers (ported verbatim from D4AssetBrowser ExportCapture — [U]) ────────────────

// Append a QImage as tightly-packed RGBA to `frames`; the first frame fixes the size. scalePct<100
// downscales the whole GIF (the single biggest file-size lever, since size is ~quadratic in dimension).
bool pushFrame(const QImage& srcIn, std::vector<std::vector<uint8_t>>& frames, int& gw, int& gh, int scalePct = 100)
{
    QImage f = srcIn.convertToFormat(QImage::Format_RGBA8888);
    if (f.isNull()) return false;
    if (gw == 0) {
        int w = f.width(), h = f.height();
        if (scalePct > 0 && scalePct < 100) {
            w = qMax(16, w * scalePct / 100);
            h = qMax(16, h * scalePct / 100);
        }
        gw = w; gh = h;
    }
    if (gw == 0 || gh == 0) return false;
    if (f.width() != gw || f.height() != gh) f = f.scaled(gw, gh);
    std::vector<uint8_t> buf(size_t(gw) * size_t(gh) * 4u);
    for (int y = 0; y < gh; ++y)
        std::memcpy(buf.data() + size_t(y) * size_t(gw) * 4u, f.constScanLine(y), size_t(gw) * 4u);
    frames.push_back(std::move(buf));
    return true;
}

// Downscale every captured RGBA frame in-place to nw x nh (cheap, no re-render).
void downscaleFrames(std::vector<std::vector<uint8_t>>& frames, int& gw, int& gh, int nw, int nh)
{
    if (nw >= gw && nh >= gh) return;
    for (auto& f : frames) {
        QImage im(reinterpret_cast<const uchar*>(f.data()), gw, gh, QImage::Format_RGBA8888);
        QImage s = im.scaled(nw, nh, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                     .convertToFormat(QImage::Format_RGBA8888);
        std::vector<uint8_t> nb(size_t(nw) * size_t(nh) * 4u);
        for (int y = 0; y < nh; ++y)
            std::memcpy(nb.data() + size_t(y) * size_t(nw) * 4u, s.constScanLine(y), size_t(nw) * 4u);
        f = std::move(nb);
    }
    gw = nw; gh = nh;
}

// Crop every frame to the union of the model's silhouette across the WHOLE sequence — one box for all
// frames, because a per-frame box would make the subject swim as the crop chased it. The alpha channel
// is the silhouette (our frames carry model-coverage alpha). keepAlpha=false forces alpha back to 255
// after cropping (for an opaque GIF, where the coverage was only a means to find the box). Returns
// false when nothing opaque was found (an empty viewport must not crop to nothing).
bool cropFramesToModel(std::vector<std::vector<uint8_t>>& frames, int& gw, int& gh, bool keepAlpha)
{
    if (frames.empty() || gw <= 0 || gh <= 0) return false;
    constexpr int kMinAlpha = 8;   // AA edges fade to near-zero; a strict >0 chases stray pixels out
    int x0 = gw, y0 = gh, x1 = -1, y1 = -1;
    for (const auto& f : frames)
        for (int y = 0; y < gh; ++y) {
            const uint8_t* row = f.data() + size_t(y) * size_t(gw) * 4u;
            for (int x = 0; x < gw; ++x)
                if (row[x * 4 + 3] >= kMinAlpha) {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
        }
    if (x1 < 0) return false;   // nothing drawn
    constexpr int kPad = 4;
    x0 = qMax(0, x0 - kPad); y0 = qMax(0, y0 - kPad);
    x1 = qMin(gw - 1, x1 + kPad); y1 = qMin(gh - 1, y1 + kPad);
    const int nw = x1 - x0 + 1, nh = y1 - y0 + 1;
    if (nw <= 0 || nh <= 0) return false;
    if (nw == gw && nh == gh && keepAlpha) return false;   // already tight
    for (auto& f : frames) {
        std::vector<uint8_t> nb(size_t(nw) * size_t(nh) * 4u);
        for (int y = 0; y < nh; ++y) {
            const uint8_t* src = f.data() + (size_t(y + y0) * size_t(gw) + size_t(x0)) * 4u;
            uint8_t* dst = nb.data() + size_t(y) * size_t(nw) * 4u;
            std::memcpy(dst, src, size_t(nw) * 4u);
            if (!keepAlpha)
                for (int x = 0; x < nw; ++x) dst[x * 4 + 3] = 255;
        }
        f = std::move(nb);
    }
    gw = nw; gh = nh;
    return true;
}

// Composite RGBA frames (straight, unpremultiplied alpha = model coverage) over an opaque backdrop and
// force alpha to 255 — for an opaque GIF whose background should be the viewport's dark ground, not the
// black a coverage capture leaves behind. POE2-specific: renderToImage's transparent capture clears to
// (0,0,0,0), so without this an opaque GIF would show a black rather than a neutral backdrop.
void compositeOverBg(std::vector<std::vector<uint8_t>>& frames, uint8_t br, uint8_t bgc, uint8_t bb)
{
    for (auto& f : frames)
        for (size_t i = 0; i + 3 < f.size(); i += 4) {
            const int a = f[i + 3];
            f[i]     = uint8_t((f[i]     * a + br  * (255 - a)) / 255);
            f[i + 1] = uint8_t((f[i + 1] * a + bgc * (255 - a)) / 255);
            f[i + 2] = uint8_t((f[i + 2] * a + bb  * (255 - a)) / 255);
            f[i + 3] = 255;
        }
}

// Encode the GIF, and when Optimize is on, keep re-encoding until it fits `targetBytes`. Frames are
// captured once, so retries only re-encode. The ORDER of the ladder is the point (ported verbatim from
// D4 — each step was learned the expensive way): palette down to a 32-colour floor → dither off (at a
// coarse palette, killing the Bayer pattern hands LZW back the flat runs, often the single biggest
// saving — cutting colours with dither ON can make files BIGGER) → aimed downscale in ≤5 passes at
// sqrt(target/actual)×0.93 floored at 96px, never nibbled. Ships the SMALLEST attempt, not the last.
bool encodeWithBudget(const QString& path, std::vector<std::vector<uint8_t>>& buf,
                      int gw, int gh, int delayCs, bool loop, int transThresh, int maxColors,
                      bool wantDither, bool optimize, qint64 targetBytes)
{
    QElapsedTimer clock; clock.start();
    std::vector<uint8_t> bytes, best;
    int bestW = gw, bestH = gh, bestColors = maxColors;
    bool bestDither = wantDither;

    auto attempt = [&](int colors, bool dither) -> qint64 {
        if (!GifEncoder::encodeToBuffer(bytes, buf, gw, gh, delayCs, loop, transThresh, colors, dither))
            return -1;
        const qint64 sz = qint64(bytes.size());
        if (best.empty() || bytes.size() < best.size()) {
            best = std::move(bytes); bestW = gw; bestH = gh; bestColors = colors; bestDither = dither;
        }
        return sz;   // read size before the possible move above
    };

    qint64 sz = attempt(maxColors, wantDither);
    if (sz < 0) return false;
    if (!optimize || sz <= targetBytes)
        return GifEncoder::writeBuffer(path.toStdString(), best.empty() ? bytes : best);

    // 1. Palette, down to a 32-colour floor.
    int colors = maxColors;
    while (sz > targetBytes && colors > 32) {
        colors = qMax(32, colors * 3 / 4);
        sz = attempt(colors, wantDither);
        if (sz < 0) return false;
    }
    // 2. Dither off — at a coarse palette often the single largest saving available.
    if (sz > targetBytes && wantDither) {
        sz = attempt(colors, false);
        if (sz < 0) return false;
    }
    const bool ditherNow = (sz <= targetBytes) ? bestDither : false;
    // 3. Resolution, aimed at the target rather than stepped toward it.
    for (int pass = 0; pass < 5 && sz > targetBytes; ++pass) {
        const double ratio = double(targetBytes) / double(sz);
        const double k = qBound(0.35, std::sqrt(ratio) * 0.93, 0.92);
        const int nw = qMax(96, int(gw * k));
        const int nh = qMax(96, int(gh * k));
        if (nw >= gw && nh >= gh) break;
        downscaleFrames(buf, gw, gh, nw, nh);
        sz = attempt(colors, ditherNow);
        if (sz < 0) return false;
    }

    const bool hit = !best.empty() && qint64(best.size()) <= targetBytes;
    qInfo("gif: %s — %.2f MB vs %.2f MB target · %dx%d px, %d colours, dither %s · %lld ms",
          hit ? "target met" : "TARGET NOT REACHABLE — shipping the smallest encode",
          double(best.size()) / (1024.0 * 1024.0), double(targetBytes) / (1024.0 * 1024.0),
          bestW, bestH, bestColors, bestDither ? "on" : "off", clock.elapsed());
    return GifEncoder::writeBuffer(path.toStdString(), best);
}

// One captured frame: render the asset alone with model-coverage alpha (renderToImage's transparent
// path), regardless of the final background — the coverage alpha is what crop and opaque compositing
// both need. Rendered at 100% here; pushFrame applies the GIF downscale.
QImage grabCoverageFrame(GLModelWidget* v)
{
    return v->renderToImage(100, /*transparentBg=*/true, /*cropToModel=*/false);
}

// Shared tail: crop → (opaque? composite over backdrop) → budget-encode. Backdrop matches the
// viewport's dark ground so an opaque GIF looks like the preview.
bool finishGif(const QString& path, std::vector<std::vector<uint8_t>>& buf, int gw, int gh,
               int delayCs, const GifOptions& opt, const QColor& bg)
{
    if (buf.empty() || gw == 0) return false;
    if (opt.cropToModel) {
        const int fw = gw, fh = gh;
        if (cropFramesToModel(buf, gw, gh, /*keepAlpha=*/true))   // keep coverage for compositing/transp.
            qInfo("gif: cropped to model — %dx%d from %dx%d", gw, gh, fw, fh);
    }
    if (!opt.transparentBg)
        compositeOverBg(buf, uint8_t(bg.red()), uint8_t(bg.green()), uint8_t(bg.blue()));   // the viewport's own clear colour
    const int transThresh = opt.transparentBg ? 128 : -1;
    const qint64 targetBytes = qint64(qBound(1, opt.targetMB, 200)) * 1024 * 1024;
    return encodeWithBudget(path, buf, gw, gh, delayCs, /*loop=*/true, transThresh,
                            qBound(2, opt.maxColors, 256), opt.dither, opt.optimize, targetBytes);
}

}  // namespace

GifOptions loadGifOptions()
{
    QSettings s;
    GifOptions o;
    o.fps             = s.value(QStringLiteral("gif/fps"), o.fps).toInt();
    o.turntableFrames = s.value(QStringLiteral("gif/turntableFrames"), o.turntableFrames).toInt();
    o.scalePercent    = s.value(QStringLiteral("gif/scale"), o.scalePercent).toInt();
    o.maxColors       = s.value(QStringLiteral("gif/maxColors"), o.maxColors).toInt();
    o.dither          = s.value(QStringLiteral("gif/dither"), o.dither).toBool();
    o.transparentBg   = s.value(QStringLiteral("gif/transparentBg"), o.transparentBg).toBool();
    o.cropToModel     = s.value(QStringLiteral("gif/cropToModel"), o.cropToModel).toBool();
    o.optimize        = s.value(QStringLiteral("gif/optimize"), o.optimize).toBool();
    o.targetMB        = s.value(QStringLiteral("gif/targetMB"), o.targetMB).toInt();
    return o;
}

void saveGifOptions(const GifOptions& o)
{
    QSettings s;
    s.setValue(QStringLiteral("gif/fps"), o.fps);
    s.setValue(QStringLiteral("gif/turntableFrames"), o.turntableFrames);
    s.setValue(QStringLiteral("gif/scale"), o.scalePercent);
    s.setValue(QStringLiteral("gif/maxColors"), o.maxColors);
    s.setValue(QStringLiteral("gif/dither"), o.dither);
    s.setValue(QStringLiteral("gif/transparentBg"), o.transparentBg);
    s.setValue(QStringLiteral("gif/cropToModel"), o.cropToModel);
    s.setValue(QStringLiteral("gif/optimize"), o.optimize);
    s.setValue(QStringLiteral("gif/targetMB"), o.targetMB);
}

bool turntableGif(GLModelWidget* view, const QString& path, const GifOptions& opt, const ProgressFn& progress)
{
    if (!view || path.isEmpty()) return false;
    const int fps      = qBound(1, opt.fps, 60);
    const int delayCs  = qMax(2, 100 / fps);
    const int scalePct = qBound(25, opt.scalePercent, 100);
    int frames         = qBound(8, opt.turntableFrames, 240);

    // A clip plays THROUGH the spin only if it is actually PLAYING — a paused clip is a pose the user
    // chose to look at, so the turntable orbits that pose rather than turning it into a walk cycle.
    const bool animate = view->animPlaying() && view->hasClip() && view->clipDuration() > 0.0f;
    const float dur = view->clipDuration();

    // A GIF loops the whole sequence, so BOTH orbit and pose must return to their start. The orbit
    // always does (a full revolution). The pose does only if a whole number of clip cycles fits the
    // capture, so snap the frame count to whole clip loops using the clip's OWN frame count (native
    // frames = round(duration · clip-fps), not the turntable fps — the clip's authored rate, matching
    // D4's animFrameCount). When whole loops exceed the 240 ceiling, map one clip cycle across the
    // revolution instead (still seamless).
    const int clipFps = animate ? qMax(1, view->clipFps() > 0 ? int(std::lround(view->clipFps())) : 30) : 30;
    bool authoredRate = false;
    int loopFrames = 0;
    if (animate) {
        loopFrames = qMax(1, int(std::lround(double(dur) * double(clipFps))));   // the clip's native frame count
        const int loops = qMax(1, int(std::lround(double(frames) / double(loopFrames))));
        if (loops * loopFrames <= 240) { frames = qMax(8, loops * loopFrames); authoredRate = true; }
    }
    auto timeFor = [&](int i) -> float {
        if (authoredRate) return float(i % loopFrames) / float(clipFps);   // one native clip frame per GIF frame
        return float(std::fmod(double(i) / double(frames) * double(dur), double(dur)));
    };

    // Save and restore the interactive state the capture perturbs.
    const float startYaw   = view->camYaw();
    const float prevTime   = view->time();
    const bool  prevPlay   = view->animPlaying();
    // A turntable must spin the object IN PLACE, so orbit the model's own centre — not any panned
    // look-at target the user left the viewport on (which would make the object swing off-frame).
    const GLModelWidget::CameraPose prevPose = view->cameraPose();
    {
        GLModelWidget::CameraPose p = prevPose;
        QVector3D c; float r = 1.0f;
        view->sceneBounds(c, r);
        p.center = c;
        view->setCameraPose(p);
    }
    QTimer* play = view->playbackTimer();
    if (animate && play) play->stop();   // we set the time explicitly per frame
    GLModelWidget::CaptureScope scope(view);
    auto restore = [&] {
        view->setCameraPose(prevPose);
        view->setOrbitYaw(startYaw);
        if (animate) { view->setTime(prevTime); if (prevPlay && play) play->start(); }
    };

    std::vector<std::vector<uint8_t>> buf; int gw = 0, gh = 0;
    for (int i = 0; i < frames; ++i) {
        view->setOrbitYaw(startYaw + float(i) / float(frames) * 2.0f * float(M_PI));
        if (animate) view->setTime(timeFor(i));
        if (!pushFrame(grabCoverageFrame(view), buf, gw, gh, scalePct)) { restore(); return false; }
        if (progress && !progress(i + 1, frames)) { restore(); return false; }
    }
    restore();

    if (animate)
        qInfo("gif turntable: %d frame(s) — playing a clip %s", frames,
              authoredRate ? "at its authored rate (whole loops per revolution)"
                           : "mapped to one cycle per revolution (whole loops exceed the 240 cap)");
    else
        qInfo("gif turntable: %d frame(s) — static pose (%s)", frames,
              view->hasClip() ? "a clip is loaded but not playing — press Play to animate it"
                              : "no clip loaded");
    return finishGif(path, buf, gw, gh, delayCs, opt, view->backgroundColor());
}

bool animLoopGif(GLModelWidget* view, const QString& path, const GifOptions& opt, const ProgressFn& progress)
{
    if (!view || path.isEmpty()) return false;
    if (!view->hasClip() || view->clipDuration() <= 0.0f) return false;   // no clip to loop

    // Sample the clip at its OWN authored frame rate — one GIF frame per native clip frame — so the
    // loop is exact and plays at the clip's real speed (D4's animFrameRate/animFrameCount, not a
    // user-guessed fps). opt.fps is not used here; it governs only the turntable's orbit.
    const int   fps     = qBound(1, view->clipFps() > 0 ? int(std::lround(view->clipFps())) : 30, 240);
    const int   delayCs = qMax(2, int(std::lround(100.0 / double(fps))));
    const int   scalePct = qBound(25, opt.scalePercent, 100);
    const float dur     = view->clipDuration();
    // Native frame count = round(duration · fps). Ceiling keeps a very long clip from an enormous GIF.
    const int   n       = qBound(2, int(std::lround(double(dur) * double(fps))), 600);

    const float prevTime = view->time();
    const bool  prevPlay = view->animPlaying();
    QTimer* play = view->playbackTimer();
    if (play) play->stop();
    GLModelWidget::CaptureScope scope(view);
    auto restore = [&] { view->setTime(prevTime); if (prevPlay && play) play->start(); };

    std::vector<std::vector<uint8_t>> buf; int gw = 0, gh = 0;
    for (int f = 0; f < n; ++f) {
        view->setTime(float(f) / float(fps));
        if (!pushFrame(grabCoverageFrame(view), buf, gw, gh, scalePct)) { restore(); return false; }
        if (progress && !progress(f + 1, n)) { restore(); return false; }
    }
    restore();
    qInfo("gif anim-loop: %d frame(s) at %d fps (clip %.2fs)", n, fps, double(dur));
    return finishGif(path, buf, gw, gh, delayCs, opt, view->backgroundColor());
}

}  // namespace core::capture
