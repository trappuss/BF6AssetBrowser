# Phase 5 — FOX on the core (first cut)

**Status: PARTIAL, and said so.** `games/fox` builds and boots on the core shell: `FoxStore` puts
Fox Engine behind `core::IGameStore`, `FoxPlugin` supplies the five tabs, and `FOXAssetBrowser`
passes 13/13 self-tests and the headless boot needles in the container. Textures, Models and Bulk
are the core's generic tabs over the FOX store. Files and Customize are FOX's own classes hosted
as `BrowserTab`s — they still draw through FOX's own viewport stack, which is exactly the half the
plan row asks for ("consume core viewport / panels / menus / export") and which is NOT done. The
acceptance suite — `Verify - Regression.bat`, 186 checks driving 247 harness flags — has not been
re-hosted on the core shell and so cannot have been run. Nothing in this phase has touched a real
archive: the container has none.

What this phase is, then: the store and the plugin (the POE2-sized part, done to the same standard
as phase 4), plus an honest map of the two larger parts that remain.

## What exists now

```
games/fox/FoxStore.{h,cpp}      IGameStore over ArchiveIndex + FmdlFile/FtexFile/BcDecode/FcnpFile
                                + MtarFile/GaniAnim/AnimPose (clips BAKED to core::Clip) + ModelTags
                                + NameCatalog + TextureUsers/RefIndex + IconCatalog + blobcache
games/fox/FoxPlugin.{h,cpp}     tabs · FOX's FirstRunPage · Directories page · Handbook + the legacy
                                FOX settings dialog as Help entries · cache roster · 16-hex id policy ·
                                export defaults · game/category/family layouts · hotkey rows · 5 tests
games/fox/tabs/FoxOwnedTabs.*   FilesTabHost / CustomizeTabHost — FOX's tabs as BrowserTabs
games/fox/main.cpp              runApp + FOX's renamed keys + cache prune and --games (beforeShell)
games/fox/CMakeLists.txt        fox_engine = every FOX source but main.cpp and MainWindow (FOX_SRC)
```

Core additions the port needed, all general: `RunOptions::beforeShell` (a game's own startup
step after settings exist), `AppShell` connects a plugin first-run page's `startRequested()` by
name and `reload()` is now a slot, `AppPlugin::parseArgs/applyArgs` and the harness flags (below),
`ModelsTab::selectModel/exportTo`, `foxadapt::skeletonToCore(bones, remapOut)` returns the
parents-first map (the same latent gap phase 4 closed for POE2 — vertex joints are now remapped
by it in `FoxStore::loadModel`).

## Decisions recorded

- **Identity is the file index, not the hash.** One install carries the same PathFileNameCode
  twice (Survive's copies of `/Assets/tpp/`, a mod's copy shadowing the shipped one). The id
  policy still matches a typed 16-hex code against `IndexedFile::hash`.
- **Clips are listed at load and baked on demand.** Fox clips drive rig units; the pose solver is
  the only faithful reading. The first cut baked every clip `animbind` binds to a model at load;
  the real-data run measured it: `bdf0_main0_def` binds 20,921 clips and took **99 s** to load
  (index ready 18:44:23 → `models: loaded` 18:46:02). So the contract grew the lazy source it had
  room for: `core::Clip::lazy` + `IGameStore::bakeClip`. `FoxStore::loadSkeleton` now lists the
  clips from the AnimCatalog (names, no archive parsed) and `bakeClip` decodes one — sampled at
  30 fps into a dense `core::Clip` (`baked = true`), world → local as FOX's exporter did, constant
  channels elided — when it is played (`ModelsTab::setClipIndex`, logged with its cost) or
  exported (`model::ensureClips` in `exportOne`). One-entry model and archive caches sit behind a
  mutex so playing a model's clips in a row parses neither again. FOX decoded one clip at a time
  too; this is that behaviour on the core. A clip posed for another skeleton now fails at bake
  (status line + log) instead of being filtered from the list.
- **FOX's model export writes no animations by default.** FOX's `ModelsTab::exportTo` wrote the
  posed mesh; clips went out through the Animations panel's own action, selected. So
  `exportDefaults().includeAnimations = false` (the first cut had it on, which with lazy clips
  would have baked all 20,921 into every export).
- **Materials are the exporter's mapping, at load.** base (colour layer baked through its mask)
  + `Mask 0.35` when the base has alpha; ORM from the SRM with R/B neutral and
  `ormHasOcclusion = false`; metalness eased from the FMTT F0; the normal map kept in Fox's frame
  with `normalGreenDown = true` so the core exporter flips it unless "keep green down" is chosen.
  Viewport-only fields (TRM, layer tiling, eye maps, hair, glass) are not carried — they are a
  `ViewportExtension`'s (phase 6's mechanism).
- **Attachments are not exposed.** Fox attaches through a user-chosen `.fv2` variation, not a
  declaration on the model; that is the Files/Customize tabs' feature and stays theirs.
- **Three shared keys migrate once** (`session/remember` → `interface/rememberSession`,
  `paths/exportDir` → `paths/lastExportDir`, `hotkeys/exportSelectionLast` → `exportToLast`,
  `hotkeys/rescan` → `reload`, `interface/hoverPreviews` → `hover/imagePreview`). FOX-only keys
  (`view/*`, `textures/alphaBg`, `paths/gameDirs`…) stay FOX's, read by FOX's own code.
- **Three FOX sources were edited**, all real defects: two missing includes under the container's
  include order (`export/ViewCapture.h`: `<QImage>`, `<QVector>`; `gl/GLModelWidget.cpp`:
  `<QLocale>`), and `fox/Fox2Refs.cpp`'s `static const QRegularExpression` made `thread_local` —
  the RefIndex sweep is multi-threaded and a shared `QRegularExpression` is not thread-safe to
  match on; it surfaced as 46 "invalid QRegularExpression" warnings against a pattern that is
  valid in isolation. A fourth edit is the `ModelPreviewHost` seam (below), additive.
- **The harness mechanism is core's now.** `--shot <png>`, `--settle <ms>`, `--stay`, `--tab`
  (a label works, case-insensitively), `--density`, `--fontscale` are `runApp` flags; a plugin
  claims its own with `AppPlugin::parseArgs` (consumed tokens never reach Qt) and acts on them in
  `AppPlugin::applyArgs(shell)` once the index is ready — before a shot settles, before
  `--quit-when-ready` quits, so a flag that exports or dumps needs no shot. The sequence is FOX's:
  schedule → ready → tab → flags → settle → grab → quit.
- **Poe2 (phase 4) fix found by the real-index run:** `Poe2Store::hasTag` rebuilt the facet table
  per call, ~28 s of GUI stall at the Textures rows; it is a static hash now.

## The gate

Plan row: "Customize and Files tabs stay FOX-owned but consume core viewport/panels/menus/export.
The ~190-check `Verify - Regression.bat` and `regress.sh` are the acceptance suite."

**Closed on the user's install (2026-09-22 run of `Build and Verify.bat`):** FOX boots on the
real index (1,111,128 files / 32 archives), `--model bdf0_main0_def` loads on the core Models tab
(3 parts, 117 bones, 20,921 clips listed) and `--character ssd_f` builds the Survive female in the
hosted Customize tab (5 parts, 699 bones) — both photographed. Two things it found and this cut
fixed: the 99 s clip bake (above) and a raw-originals bulk write that failed on Windows only
(`BulkExtractor::exportRaw` now checks `mkpath`, retries once — concurrent `CreateDirectory` on
a shared parent can fail — and reports the real `QSaveFile` error instead of "could not be read").

Second run (same day, after the lazy clips): 21/21; `bdf0_main0_def` loads in 1.07 s (was 99 s).
Two more things that run's logs showed and this cut fixed: the FOX process died with 0xc0000005
on exit after the character shot — the detached TextureUsers/RefIndex sweeps outliving their
singletons, exactly what FOX's `main.cpp` called `shutdown()` after `exec()` for and the port had
dropped; `RunOptions::afterExec` now carries it. And `--clip 0` reported "devshot: clip" but no
"models: decoded clip" line: the on-demand decode failed and only the status line said so. Logged
(with the store's reason) and run under the SEH guard, the third run said: the reason was EMPTY —
which only one path produces: a successful bake whose track list is empty, i.e. a clip whose every
channel equals the rest pose, elided to nothing. `ensureClip` now counts that as decoded (it plays
as the bind pose; the exporter already omits such clips) and the log line carries the track count.
Marked as the reading of the evidence, not yet seen: the next run's "decoded clip … 0 track(s)"
line confirms it. Also observed, unexplained: the AnimCatalog lists 20,920 clips for that
model where the first cut's bake found 20,921 valid ones — one clip the catalogue does not carry.

**Closed here:** `FOXAssetBrowser --check` 13/13 (core 8 + FOX bc decode/encode, QueryTerm,
searchq, animpose); headless boot with all five tabs (`shell: 5 tab(s) built — files, textures,
models, customize, bulk`, FOX's own `files: columns…` and `customize: built 0 part(s)` lines
prove the hosted tabs constructed); `verify-src.py` clean with a `fox` table; the whole
`run_tests.sh` green.

**Open — the two large halves, in order:**

1. **Re-host the harness — mechanism done, flags begun.** FOX's `DevShot` (≈250 fields,
   `MainWindow::takeDevShot` ≈5,000 lines) is what `Verify - Regression.bat` drives. The
   mechanism is in core `runApp` (above). Re-hosted so far, in `FoxPlugin::parseArgs/applyArgs`:
   the session overrides `--game <dir>` (repeatable), `--loose`, `--dict`, `--moddir`,
   `--exportdir` (FOX's `Config::setSession*`, never the INI), `--games <spec>` (applied in
   `beforeShell`, before the index), `--model <text>` (core `ModelsTab::selectModel` — filter now,
   load the first match synchronously), `--character` / `--weapon` / `--vehicle` / `--camo`
   (forwarded to the hosted `CustomizeTab::buildFromSpec`, `other:` → category 4 as before) and
   `--export <glb>` (Customize's `exportSceneTo` when a scene flag is beside it, else the core
   `ModelsTab::exportTo`). That is 13 of ~240. The rest: flags addressing Files/Customize forward
   to the hosted FOX tabs largely as they are; those addressing Models/Textures/Bulk must be
   re-expressed against the core tabs' harness APIs, growing those APIs where a flag needs
   something they lack. `Build and Verify.bat` runs the first two real-data harness checks
   (`fox_shot_model`, `fox_shot_character`); `run_tests.sh` checks the mechanism on an
   unconfigured install.
2. **Files and Customize onto the core viewport — Files done, Customize open.** FOX's
   `PreviewPane` grew a seam, `fox::ModelPreviewHost` (`preview/ModelPreviewHost.h`, additive: with
   no factory installed it builds its own page exactly as before, so the standalone app is
   unchanged). `games/fox/tabs/FoxFilesPreview` is the core implementation: the .fmdl is loaded
   through `FoxStore` (the exporter's material mapping, as the Models tab) into a
   `core::gl::GLModelWidget`, exported through `core::model::exportOne`; `FoxPlugin::tabs()`
   installs the factory before the tab exists. `--search` is re-hosted (Files search + first result)
   and `Build and Verify.bat` photographs `bdf0_main0_def` on it (`fox_shot_files`, needle
   `files: preview`). Not carried by the core viewport yet, so not shown in the Files preview:
   FOX's TRM/layer-tiling/eye/hair/glass shading — that is the `ViewportExtension` Customize needs.
   The Files tab's other pages (image, text, hex, fox2, fv2, strings, audio) are Fox-format viewers
   and stay FOX's. (The preview keeps a model already on the viewport — FilesTab shows a file up
   to three times per selection — so the 2026-09-22 run's triple load is one load now.)

   **The FOX `ViewportExtension`, scoped from the source (not started).** FOX's fragment shader
   is 731 lines with 72 uniforms and its own lighting model (key/fill/sky/ground, exposure) over
   per-material inputs the core `Material` does not carry: the raw SRM (R = specular mask,
   G = roughness, B = reflection boost — not ORM), TRM subsurface, layer + layer mask + tiling,
   sub-normal + mask, the MTM region map selecting four FMTT presets (F0, spec tint, translucency,
   anisotropy), eye (iris, lens height, view reflection, height scale), hair (anisotropic lobe,
   shift map), glass (tint, opacity, reflect roughness), dirt (DTM + splats), and the
   skin/no-metal/unlit family switches from the shader name. Plan: `FoxStore` exposes FOX's
   `GLPbrMaterial` per material beside the core `Material` (the loader already builds it);
   `FoxViewportExtension` uploads those on units 6–13/17+ and splices — first slice
   `fragSurface` (SRM unpack, preset F0 → metalness eased, reflection boost, sub-normal) and
   `fragLighting` (TRM wrap term); second slice eye/hair/glass (`fragEarly`, `fragSpec`,
   `partInMainPass` + `extraPass` for glass); dirt last. Each slice compiles under the conformance
   suite's Mesa shader check before it reaches the user. **Superseded 2026-09-23 by plan §8.9:** a
   `ViewportExtension` may replace the whole shader, so FOX's goes in as-is (one file, its own
   lighting) after phase 6 — not sliced into the core's blocks. The core's lit features now default
   off; the phase-3 parity check opts into D4's rig explicitly and still passes. Also found while
   flipping them: `run_tests.sh` did not fail when `viewport_parity` failed (the pipe swallowed its
   exit code) — it does now. 1,232 + 11,733 lines built around FOX's
   `GLModelWidget`/`ViewportPanel`/`PreviewPane`. The Customize composer needs the core viewport
   to take what FOX's does today (PBR full set, layer colour, eye/hair/glass shading, dirt
   splats, sleeve fit) — that is FOX's `ViewportExtension`, the same mechanism D4 needs in phase 6.
   Until then two viewports live in one binary; nothing is lost, and nothing is shared.

Also open, smaller: the Games bar (per-game tick filter) is not on the core shell — the `game`
tag group in the funnel is the interim; Textures → Models cross-tab jumps
(`openModelRequested`) are not wired; FOX's PBR/view/texture settings live in the legacy dialog
under Help ▸ Fox Engine settings… until the tabs read core settings.

## Still open

- Real-data run of FOX on the core (user-side; needs the harness first for the suite, but a
  manual launch works today: `FOXAssetBrowser.exe` after `cmake -DFOX_SRC=<FOXAssetBrowser>`).
- Phase 6 (D4). Not started, deliberately: D4's store layer is tab-owned (clips, tags, search
  blob, attachments, two material resolvers live in `ModelsTab`, tabs-store.md §7.1) and the
  gate is pixel-identical repro assets. Doing it without the data would be guessing.
- Phase 7.
