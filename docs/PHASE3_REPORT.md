# Phase 3 — the viewport core

**Status: complete; the synthetic half of the gate is CLOSED pixel-exact, the fixture half stays
open.** `conformance/run_tests.sh` now runs 181 assertions, 0 failures; the three viewport suites
run headless under xvfb + Mesa llvmpipe, and every one was also run under AddressSanitizer.

## What exists now

```
core/gl/ViewportExtension.h       the engine seam: GLSL splice blocks + per-frame/per-part hooks
core/gl/Shaders.{h,cpp}           the core GLSL, assembled with an extension's blocks spliced in
core/gl/Pose.{h,cpp}              ONE pose evaluator over core::Clip (local · world · palette)
core/gl/GLModelWidget.{h,cpp}     the viewport — D4's pipeline, stripped, with FOX/POE2 grafts
core/gl/GLTextureWidget.{h,cpp}   POE2's, lifted (engine-agnostic: takes a QImage)
core/gl/ThumbnailRenderer.{h,cpp} FOX's structure, POE2's API, AssetId addressing
conformance/tools/SyntheticScene.h      the shared test scene (two-bone rig, three materials)
conformance/tools/viewport_parity.cpp   D4 original vs core, pixel comparison
conformance/tools/viewport_check.cpp    core-only checks (29)
conformance/tools/thumbnail_check.cpp   thumbnails + texture widget (19)
```

## The strip

D4's `GLModelWidget` was 6,727 + 856 lines. The core one is 2,156 + 407, and the difference is
exactly the list in plan §3.3: the cloth solver (~1,900 lines), the ~60 dye/fur/FX/detail/hair/
eye/skin/wetness/snow setters and their texture arrays, `Config::d4dataDir()` and the cloth-tuning
JSON read in `setGeometry`, the `detail_mask_probe.txt` write on every load, the bone-hash
dictionaries, the raw-CASC cubemap layout, channel-viewer values 7/8, and the two QSettings reads.
Everything that left has a place to come back: a `ViewportExtension` hook (pose modification,
per-frame and per-part uniforms, an after-part redraw for fur shells, an extra pass for mesh FX,
overlay lines, a joint-head override for simulated bones) or a plain setter the app calls.

The shader was the hard part. D4's fragment shader is one 775-line program with 120 uniforms and
the Diablo code interleaved with the PBR core at eleven points. Rather than one copy per game, the
core shader has **eleven splice points** and an extension supplies GLSL text for each;
`assembleShaders()` does the splice and the suite compiles the result under Mesa so a bad block
fails there and not on a user's GPU. With every block empty it is the plain PBR viewport — which is
what the parity test renders.

Two generalisations were made on the way, both deliberate: `uAlphaMode`/`uAlphaCutoff` replace
D4's hard-coded 0.35 (hair 0.16 becomes the D4 extension's `fragCutout` block), and the ORM
texture is **multiplied** by `metalFactor`/`roughFactor` (glTF semantics, matching the exporter)
where D4 used the texture raw and ignored the scalars. A D4 material adapter reproduces D4 exactly
by setting the factors to 1 when an ORM exists and to the scalar value when it does not — the
parity scene does precisely that.

## The grafts

From FOX: `ShadingMode` as the one state (D4 composed `m_pbr`/`m_showTex`/`m_wireframe` in each
tab); the overlay **master gate inside the widget** (D4 kept three copies, one per tab, and a
settings replay that bypassed it re-enabled overlays the user had turned off — the check
"re-enabling the gate restores exactly the previous frame" is that bug's test); a selection **set**
the widget owns with `selectionChanged(set, active)` mirrored out; `CameraPose` with
`toString/fromString` and the eight built-in presets through one setter; `renderAtSize`; and
context-loss recovery (`QOpenGLContext::aboutToBeDestroyed` → release, `initializeGL` →
re-upload from the CPU copies). From POE2: the `AlphaMode` two-pass draw (opaque+mask with
alpha-to-coverage, then blend/additive with depth writes off), `renderToImage(scale,
transparentBg, cropToModel)`, and the `skeletonMatchesMesh` fail-closed gate (a mesh skinned past
the rig gets bind pose and no clip). The modifier-double-click fix from the September FOX defect
list is in too.

## The gate

**Parity.** `viewport_parity.cpp` compiles D4's ORIGINAL widget from a container copy of `d4/src`
(one line patched for Qt 6.4 — `QList::assign` is 6.6 — and three symbols stubbed that the scene
never reaches) and renders the same synthetic skinned scene through both widgets at 640×480:

| Comparison | Differing pixels |
|---|---|
| bind pose (frame 0) | **0 / 307,200** |
| mid-clip (frame 15, a 45° pose) | **0** |
| end of clip (frame 30) | **0** |
| D4 pbr-off vs core `Shaded` | **0** |
| D4 pbr-off + textures-off vs core `Flat` | **0** |
| normal debug channel | **0** |
| red selection silhouette | **0** |
| a hidden part (also out of the shadow and SSAO passes) | **0** |

Pixel-exact through shadow map, SSAO, the analytic IBL, the alpha-to-coverage cutout, the normal
map, the ORM map and CPU skinning. The scene has no emissive on purpose: D4's emissive pulse reads
the wall clock, so two widgets can never agree on it (see the D4 finding below).

**Core-only checks** cover the grafts D4 has no counterpart for: selection mirroring, the overlay
gate, blend and additive materials, the four shading modes, transparent + cropped capture, picking,
camera presets, a real context loss (the widget moved between top-level windows renders the
identical frame afterwards), and the palette gate. **Thumbnails**: rendered on the worker thread,
transparent background, PNG to disk, served from disk after `reset()` without a load, a failed load
cached as null and never written, `refresh()` re-rendering from the model, pause/unpause.

## Three things found on the way

1. **A use-after-free in the first draft of the core widget**, caught by ASAN on the parity
   harness's first run: the context-loss hook fired inside `~QOpenGLWidget`, after the derived
   class's members were gone. Fixed by disconnecting it in the widget's own destructor. This is
   why every viewport suite runs under ASAN in the container.
2. **`Clip::frameCount()` was off by one** (a 1-second 30 fps clip counted 30 frames, so
   `setFrame(30)` sampled 29/30 s). The parity test's last frame differed by 14,376 pixels until
   the count was made inclusive of both ends, as D4's decoder counts.
3. **A latent D4 bug**: `GLModelWidget::setCaptureTime()` and the header's promise of a
   "deterministic FX shader time for captures" are not wired — `paintGL` reads `m_fxClock`
   unconditionally (`d4/src/gl/GLModelWidget.cpp:4158`), so an exported GIF's emissive pulse and
   FX scroll are wall-clock after all. The core widget uses the capture time when capturing. Not
   fixed in D4's tree (it is being retired); recorded here.

## Deferred

- `ExportCapture` (the GIF budget ladder) rides on this widget's `renderAtSize`/`setCaptureMode`
  and is phase-4 shell work with the export dialog.
- The D4 extension itself — cloth, dye, fur, FX, detail — is phase 6, where the gate is the one the
  plan states: D4's repro assets pixel-identical through this widget with the extension attached.

## Still open

- The fixture gate: no game install is reachable, so no real model has been rendered here. The
  synthetic parity proves the pipeline is D4's; only fixtures prove a real D4 mesh through the
  adapter still looks like D4.
- `viewport_parity` needs `d4/src` beside the repo and xvfb; the suite reports that half OPEN when
  either is missing rather than passing quietly.
