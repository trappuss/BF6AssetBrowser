#pragma once
// Preview-capture helpers behind the Export menu's GIF entries (template §15/§26). The GIF budget
// ladder (palette → dither-off → aimed downscale, ship the smallest attempt) is D4's ExportCapture,
// carried verbatim through POE2 — "learned the expensive way" and engine-agnostic; the constants
// (¾, 32, 0.93, 96 px) were identical in all three copies (plan §3.4). The CAPTURE drivers are
// POE2's (renderToImage + orbit yaw + clip time; turntable clip-snap to whole loops) over the core
// viewport. A game with a pre-capture step (D4's cloth settle) does it in its own wrapper before
// calling these — the viewport's CaptureScope already freezes time and idle stepping.
#include <QString>
#include <functional>

namespace core::gl { class GLModelWidget; }

namespace core::capture {

// (framesDone, framesTotal) after each frame; return false to cancel — the capture aborts, restores
// the viewport state and returns false.
using ProgressFn = std::function<bool(int done, int total)>;

struct GifOptions {
    int  fps = 25;               // 1..60 (turntable only; a loop samples at the clip's own rate)
    int  turntableFrames = 48;   // 8..240 requested; snapped to whole clip loops when a clip plays
    int  scalePercent = 100;     // 25..100 — GIF is not a detail medium, so downscale only
    int  maxColors = 256;        // 2..256
    bool dither = true;          // ordered Bayer 8x8 (position-only, so no frame-to-frame shimmer)
    bool transparentBg = false;  // 1-bit-alpha GIF vs the viewport's dark backdrop
    bool cropToModel = false;    // trim to the silhouette across the WHOLE sequence
    bool optimize = false;       // re-encode down toward targetMB (ships the smallest attempt)
    int  targetMB = 10;          // 1..200
};

// Settings ⇄ GifOptions (gif/* keys, POE2's).
GifOptions loadGifOptions();
void saveGifOptions(const GifOptions& o);

// One full 360° revolution → looping GIF. If a clip is PLAYING it plays through the spin and the
// frame count snaps to whole clip cycles (seamless wrap); paused orbits the static pose.
bool turntableGif(gl::GLModelWidget* view, const QString& path, const GifOptions& opt, const ProgressFn& progress = {});

// The current clip sampled at its own fps across its duration, one frame each. False without a clip.
bool animLoopGif(gl::GLModelWidget* view, const QString& path, const GifOptions& opt, const ProgressFn& progress = {});

}  // namespace core::capture
