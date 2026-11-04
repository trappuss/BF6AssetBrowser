# Kickoff — AssetBrowser core extraction, next session

Written 2026-09-23 at a model switch. Read this, then `README.md`, then `docs/PHASE5_REPORT.md` and `docs/PHASE6_REPORT.md`,
then plan §8 (`Diablo4AssetBrowser Native\docs\CORE_EXTRACTION_PLAN.md`, decisions 1–9). Do not
start from the summaries in your head: the state below was verified file-by-file.

**Where the folders are (moved 2026-10-05):** `G:\G Claude Projects\AssetBrowser\` holds the four
sibling projects — `AssetBrowser` (this monorepo), `FOXAssetBrowser`, `Diablo4AssetBrowser Native`,
`POE2AssetBrowser` — plus `DIAssetBrowser`, `FOXanimtool`, the D4 Blender add-on and the D4 wiki. The
old `C:\Users\notso\Downloads\Claude Current\` copies are gone. The presets and bats find siblings by
relative path, so nothing else changed; every existing `build\release\CMakeCache.txt` still named the
old place, so both bats now set a stale cache aside (`:fixmoved` renames it to `*.moved_<time>`) and
configure fresh — the first run after the move rebuilds everything. Run 23 (the first after the move)
failed d4_shot_rig / d4_attach / d4_attach_anim: the MONOREPO's D4 ini
(`build\release\games\d4\data\D4AssetBrowser\D4AssetBrowser.ini`, seeded from D4's once) still named
the old `tactKeysPath` and `d4dataDir`, so 0 TACT keys and 0 animation files loaded. Fixed in place on
2026-10-05 (the two lines; a copy of the old file sits beside it as `.before_move_fix`). Any other
setting that names a folder may need the same after a move.

## The goal, in the user's words
A "template": pick a game, crack its models, and use the template to quickly build a tool around
that engine — with the viewport replicating the game's own look as closely as possible, and every
file kind reachable (models, textures, animations, audio, the rest). Nothing game-specific in core.
**Rendering rule (user, 2026-09-23):** each game's viewport REIMPLEMENTS that game's rendering
engine — literally where its shaders/rules can be read or reconstructed from the data, and only
where that is impossible, replicated as closely as evidence allows (and marked as a replica). This
lives in the game's `ViewportExtension` (§8.9), never in core. (Claude's extension, NOT the user's
words, pending their answer: the same rule may apply to how a model is first shown, e.g. a pose the
game never displays.)

**Long-term direction (user, 2026-09-24):** one host program plus game **add-ons** that the user
builds and releases per game format, with a written guide so others can make their own (with
Claude or otherwise). Tabs in that host: Files (an explorer over the game's files, with a preview
chosen by file type: image, model, audio, video, text), Images, Models, Customize, and any
game-only tabs the add-on brings (D4's Catalogue exists because only D4 has a prominent shop; it
and Stable would ship as D4-only tabs). This is the continuation of this project, not a split:
the core / store / plugin split already is the add-on boundary. What is still missing:

- a runtime (DLL) plugin boundary;
- a general Files tab over a store-described file tree;
- previewers keyed by file type.

## State you inherit (all verified on disk by md5, 2026-09-23)
- Phases 1–4 done; phase 5 (FOX on core) first cut: store + plugin + Textures/Models/Bulk on core,
  Files preview on the core viewport, Customize still hosted as FOX's own class on FOX's viewport.
- **Plan §8.9 (settled 2026-09-23): the core viewport is the pipeline, not a look.** Lit features
  (shadow/SSAO/IBL/tonemap/specAA/subsurface/reflection) default OFF (`gl::ViewFeatures`); a game
  opts in via `AppPlugin::viewFeatures()`; `AppPlugin::viewportExtension()` attaches one extension
  to every core viewport; `ViewportExtension::fragmentShader()/vertexShader()` REPLACE the whole
  shader (inputs listed in `core/gl/ViewportExtension.h`). Verified under Mesa by
  `conformance/tools/viewport_check.cpp`; D4 parity (`viewport_parity.cpp`) opts into D4's rig and
  passes pixel-for-pixel.
- Harness: `--check --quit-when-ready --harness --tab --shot --settle --stay --density --fontscale`
  in core; `AppPlugin::parseArgs/applyArgs` for game flags. FOX has 15 of ~240 re-hosted
  (`--game --loose --dict --moddir --exportdir --games --model --clip --character --weapon
  --vehicle --camo --export --search`).
- Lazy clips: `Clip::lazy` + `IGameStore::bakeClip` + `model::ensureClip(s)` — FOX lists 20,921
  clips per model in ~1 s, decodes one on play/export (was 99 s to load).
- User's last `Run Everything.bat` (run 12, 2026-09-24 night): **40/40** on real installs (POE2, FOX,
  D4). The `cm_scol4_c07` puzzle is closed: the swatch is not in the install (PHASE6_REPORT run 12).
- **Audit 2026-09-25 (overnight): read docs/AUDIT_2026-09-25.md first.** About 30 defects were
  fixed across core, FOX, POE2 and D4. The worst: POE2 clip keys in the wrong frame, a FoxStore race
  on FOX's animation globals, bulk "Only new" overwriting files, Models loads outliving the store,
  and a hang on index failure. `Build and Verify.bat` now has one launch serving several checks
  (`:launch` / `:expect`) and 42 checks, plus a harness watchdog. **None of it has been through MSVC
  yet: the next Run Everything is its first build.** Open questions for the user are at the end of
  that file.
- Customize core, step 2 (FOX): stages 2a, 2b, 2c and 2e-1 (core seams) are built and
  container-verified (docs/customize/CUSTOMIZE_CORE_DESIGN.md, a status section each; 2a–2c also
  MSVC-verified, run 13). 2e is broken into seven parts there. **2e-2 (`FoxCustomizeSource`,
  characters) was built overnight 2026-09-25, container-tested only**: its first real run is the bat's
  `fox_core_customize` (harness flag `--core-customize`). Compare `_fox_core_character.log` /
  `fox_core_ssd_f.png` with `_fox_character.log` / `fox_ssd_f.png` (done: run 15/16 match, hair
  decided). **2e-3 (weapons) built 2026-09-25, container-tested; its first real run is the bat's
  `fox_core_weapon`** (read `_fox_core_weapon.log` for each part's seat and any `fcnp: … scale` line;
  run 19 passed). **2e-4 (vehicles, other characters) built 2026-09-26, container-tested; real
  runs 20 (46/46) and 21 (50/50) match FOX's own tab part for part.** Run 22's .glb comparison: the
  core places every ambulance piece where FOX does (one unskinned piece collapsed — fixed); FOX's
  own seating of glass / Wb / Hmic / Reci is questionable; the user chose to go BEYOND FOX — run 23's census gave the rule
  (design doc 2e-4: in place ≥ 0.25 m from the origin → not seated; at the origin, no plan →
  `CNP_<ROW>`), built 2026-10-05; numbered point families (one model at many points) are still open. **2e-5a (Camo / Variation and Gear Color rows) built 2026-09-26,
  container-tested; run 22 ran it on the ambulance (0 substitutions — nothing there to paint); the
  check now uses `vehicle:wav`, beside FOX's own `fox_vehicle_camo`** — read each piece's "look:" note.
  2e-5b (Survive slot colour, MGO dyes) and 2e-5c (avatar look rows, skin tones) are next. FOX's
  free-form "Parts" page: the user chose a core feature on the Models tab (design later). The container can
  now RUN FOX on a real 1,570-file loose tree built from the FOX project's `_probe` pulls (design doc,
  2e-5a status, last paragraph).
  2d (FOX's shader) must land before FOX's own tab retires.
- Phase 6a (2026-09-23): `games/d4` builds; container 11/11 self-tests + boot; real-data half pending.
- Phase 6b clips (2026-09-23): the D4 TREE was edited (user chose it): ModelsTab.cpp/.h,
  CMakeLists.txt, new index/ModelAnimIndex.{h,cpp}, util/ParallelMap.h. D4 standalone builds here
  with `-Dlz4_DIR=<repo>/conformance/tools/lz4shim` (a pkg-config shim for lz4).
  The container D4 tree (`/home/claude/work/d4`) was refreshed from the device that day (it had been
  6 days stale); re-check `cat src/**/*.{h,cpp} | md5sum` against the device before trusting it.

## Next, in order
1. **User-side check of phase 6a.** `Build and Verify.bat` now builds D4AssetBrowser and runs
   `d4_boot`, `d4_shot_rig`, `d4_shot_fur`, `d4_export`, `d4_shot_tex` against the real install.
   First MSVC build of `d4_engine` without D4's PCH may surface more missing includes (GCC found
   two, fixed) — fix each in the D4 tree, one line, in the file's own line endings.
2. **Phase 6b, rest** — clips are DONE (D4's anim/entity index extracted to the D4 tree's
   `index/ModelAnimIndex`, used by both D4's ModelsTab and D4Store; user-side check: D4's own app
   still lists/plays/exports clips as before — run D4's `Test -` bats). Next: attachments, icons,
   links, flags; then Wardrobe / Stable hosted and Catalogue re-pointed. `docs/PHASE6_REPORT.md`.
3. **Phase 6c** — D4's look as its ViewportExtension (cloth, dye, fur, detail, hair/eye/skin);
   gate: repro assets pixel-identical to D4's own viewport.
4. The core ModelsTab gap list (generic D4 features → core; D4-only → plugin panels), written from
   the source first.
5. Phase 7 (retire the old trees); FOX's whole shader into its extension; Customize onto core.

## Standing rules (each one cost something)
- No guessing: instrument, read the log, then act. A hunch is marked as a hunch and asked.
- Every device write: stage under `/mnt/user-data/outputs/…` → `device_commit_files` (force) →
  `md5sum` on the device vs container. The bridge has reported success without writing.
- New repo is LF except `Build and Verify.bat` (CRLF, deliberate). The plan is CRLF; D4's source
  files are MIXED (some LF) — detect each file's ending and keep it; edit in place with python bytes
  through `device_bash`. Never run git in the D4 folder
  through the bridge.
- cmd: a `::` comment INSIDE a parenthesised `if (...)` block aborts the parser silently. Comments
  stay outside blocks in the bat.
- After `exec()` FOX must `shutdown()` TextureUsers/RefIndex (`RunOptions::afterExec`) — a detached
  sweep outliving its singletons crashed on exit.
- `verify-src.py` substring baselines: raise with a reason, never silence.
- Container: repo `/home/claude/work/AssetBrowser` (mirror of the device folder); game trees
  `/home/claude/work/{d4,fox,poe2}`; build `/tmp/abbuild` (Ninja, Debug,
  `-DPOE2_SRC=/home/claude/work/poe2 -DFOX_SRC=/home/claude/work/fox -DD4_SRC=/home/claude/work/d4-qt64`
  — the D4 tree through `conformance/tools/d4_qt64_mirror.sh`, container Qt is 6.4); tests need
  `xvfb-run -a -s "-screen 0 1280x800x24"` (Qt offscreen has no QOpenGLWidget); full suite
  `bash conformance/run_tests.sh` (now fails when viewport_parity fails — it used not to).
- User wants concise reports; thorough and correct over fast; no loops re-verifying green steps.

## Open observations, unexplained (do not "fix" without evidence)
- FOX's EquipCatalog reads 0 weapon builds on the user's install (`equip: 0 game weapon build(s)`), while
  its compatibility tables load (168 receivers). So there are no named weapons or star tiers, in FOX's
  tab or the core's. Not investigated (2026-09-25, run 17).
- FOX AnimCatalog lists 20,920 clips for `bdf0_main0_def`; the first cut's full bake found 20,921
  valid. One clip the catalogue does not carry.
- POE2 Customize yaw `+π` "faceFront" is a marked HUNCH; the user has not yet said whether the
  figure opens facing the camera.
