# Phase 6 — D4 on the core (6a: the store)

**Status: 6a built and container-verified; the real-data half is user-side; 6b/6c open.**
`games/d4` builds `D4AssetBrowser` on the core: `D4Store` puts CASC + the SNO index + D4's parser,
material resolver and texture decode behind `core::IGameStore`; `D4Plugin` supplies the core's
Textures / Models / Bulk tabs, the Directories page, D4's export defaults, folder layouts, SNO id
policy and D4's look (plan §8.9: shadow, SSAO, IBL, spec AA, subsurface, reflection on — what D4's
widget has by construction). 11/11 self-tests and the headless boot needles pass in the container.
Nothing here has read a real install: the container has no CASC and no d4data.

D4 is the family's most refined tool, and its store layer lived in its tabs (core-audit
tabs-store.md §7.1). So phase 6 is split, and each part has its own gate:

- **6a — the store (this cut).** Everything a generic tab needs, taken from where D4 keeps it.
- **6b — what ModelsTab owns that is really store:** the animation-family index (clips), actor
  attachments, icons, links, the viewport material path. Then Wardrobe / Stable / Catalogue hosted.
- **6c — D4's look as its extension:** cloth, dye, fur, detail maps, hair/eye/skin — the
  `ViewportExtension` the phase-3 surgery moved out of the core widget, re-attached.

## What exists now

```
games/d4/D4Store.{h,cpp}   IGameStore: MainWindow::reload's open sequence (TACT keys → CASC → index
                           cache / CoreTOC / d4data → Latest → material-name resolver) on a worker;
                           finishReload's rows + data fingerprint on the GUI thread; parseApp for
                           geometry/skeleton/hardpoints (one parse shared by the three calls);
                           appearancePalette + buildExportMats for materials; texMetaFor + BcDecode
                           for textures; payload/meta reads
games/d4/D4Plugin.{h,cpp}  tabs · Directories page · reload + AppearanceMeta crawl · SNO id policy ·
                           export defaults · Flat/Class/Type/per-model layouts · hotkeys · 3 tests ·
                           viewFeatures · --model/--clip/--export/--texture
games/d4/D4AppLog.cpp      D4's AppLog (defined in D4's main.cpp) → the core log
games/d4/main.cpp          runApp + D4's renamed keys + D4's cache-prune list (beforeShell)
games/d4/CMakeLists.txt    d4_engine = EXACTLY the sources D4's own CMakeLists lists, minus main.cpp
                           and MainWindow (not a glob — see "Decisions")
```

Core additions, all general: `core/app/Harness.{h,cpp}` — `harness::applyModel` (--model / --clip
/ --export) and `harness::applyTexture` (--texture), lifted from FOX's plugin so every game shares
one implementation and one set of log lines (FOX now calls it); `TexturesTab::selectTexture`.
`d4adapt::skeletonToCore` gained `remapOut` — the parents-first latent gap phases 4 and 5 closed
for POE2 and FOX — and `D4Store` remaps vertex and hardpoint joints by it.

## Decisions recorded

- **Called, not re-implemented.** Every D4 behaviour in the store is D4's own function called in
  D4's order: `CascReader`, `SnoIndex`, `ModelParser::parseApp`, `appearancePalette` +
  `buildExportMats` (external linkage in D4's ModelsTab.cpp, declared exactly as D4's
  ModelsTab_Export.cpp declares them), `TexturesTab::texMetaFor` (static) + `BcDecode::decode`.
  The only copied function is `MainWindow.cpp`'s file-static `d4dataSignature` (verbatim).
- **One material resolver for viewport and file (6a).** D4 has two: `buildExportMats` for the .glb
  and `applyPartMaterials` for its viewport (dye, detail, looks). 6a uses the export one for both
  — so the core viewport shows what D4 exports. The viewport one is D4's look and belongs to 6c.
  The mapping to `core::Material` is ModelExporter.cpp's write, field for field: factors when no
  ORM, 1/1 under one; ORM R = occlusion; `normalGreenDown` (D4 ships DirectX, measured in
  ModelExporter.h) and `normalNeedsZ` (BC5, B ≈ 0); emissive colour only when meaningful, the HDR
  multiplier as strength; MASK at the authored cutoff.
- **D4's own source list, not a glob.** D4's `src/` holds work its own build does not compile yet
  (2026-09-23: `tabs/CollectionsTab.cpp`, whose header does not exist). The monorepo reads D4's
  CMakeLists and compiles exactly what it lists.
- **The data fingerprint and its resets moved with the store.** Game build | d4data folder |
  snapshot signature | TACT key count; a change resets the nine D4 indexes finishReload resets and
  deletes the two magic-number caches. **Not carried:** the diablo4.dad network refresh
  (`startDadRefresh` / `DadOverride`) D4 kicks at the same point.
- **Encrypted = listed and locked.** In the game's EncryptedSNOs manifest AND its key not loaded —
  the "present but unreadable" answer; ModelsTab's "Only encrypted" filter counts the same pair.
- **SNO search is exact.** D4 had a separate SNO box matched as a substring; the family's id rule
  is one box, exact.
- **Caches stay where D4 writes them** (data/, not data/cache/): the cache-maintenance rows point at
  `..` from the core's cache folder, and main prunes with D4's own `AppPaths::pruneOldCaches` list.

## Edits to the D4 tree

Two real defects, found because the container build has no precompiled header (D4's MSVC build
force-includes QtWidgets / QtCore through its PCH, which hid them): `index/MatSnoSweep.h` used
`QWidget*` with no declaration; `index/WardrobeAnimIndex.h` held a `QSet` with no `<QSet>`. One line
each, in the file's own line endings (LF). 6b's clip extraction (below) is the other edit.

**Container only:** Qt here is 6.4; D4 calls `QList::assign` (6.6) and
`QKeySequenceEdit::setMaximumSequenceLength` (6.5). `conformance/tools/d4_qt64_mirror.sh` builds a
patched mirror for the container (the phase-3 parity build already did the first); the real tree
and the user's MSVC build compile the originals.

## The gate

Plan row: "Every repro asset in d4/CLAUDE.md renders and exports as before; Audit - Asset
Health.bat shows no newly-broken; the Dump/Test bats still pass."

**Closed here:** `D4AssetBrowser --check` 11/11 (core 8 + bc decode, QueryTerm, normal
convention); headless boot (`shell: 3 tab(s) built — textures, models, bulk`); a shot; the suite
green with a phase-6 section; `verify-src.py` clean with a `d4` table.

**User-side (Build and Verify.bat):** `d4_boot` (index ready), `d4_shot_rig` (barF_base00),
`d4_shot_fur` (barF_stor151_TRS), `d4_export` (barF_base03_TRS → .glb with textures: needle
`image(s)`), `d4_shot_tex` (barM_P00_BOD_normal). These prove the store end to end on the real
install. What they cannot prove yet: "renders as before" — D4's look is 6c; "exports as before" —
clips and attachments are 6b.

## 6b, first part: clips (2026-09-23)

The animation index was ModelsTab member state. By the user's choice it was **extracted inside the
D4 tree** so D4's own Models tab and the core store run one copy:

- **New in the D4 tree:** `src/index/ModelAnimIndex.{h,cpp}` (the anim + entity scans, their disk
  cache, the family pass, rig families, the clip queries, the per-skeleton decode) and
  `src/util/ParallelMap.h` (`parallelMap`, moved verbatim); both listed in D4's CMakeLists. D4's
  standalone app builds in the container with `-Dlz4_DIR=conformance/tools/lz4shim` (Ubuntu ships
  lz4 with pkg-config only; the shim gives it vcpkg's `lz4::lz4` name).
- **D4's ModelsTab:** 11,676 → 10,922 lines. The index members are now REFERENCES into two blobs
  (`m_anim`, `m_ent`), so ~190 existing uses read exactly as before; `ensureAnimatedIndex` /
  `ensureEntityIndex` / `ensureRigIndex` call the module and keep every GUI-side step where it was
  (the family pass still on the GUI thread, list refresh, filter re-apply, cache write); the seven
  query members and `decodeAnimForSkeleton` are one-line wrappers. The skeleton fallback keeps its
  rule — only for the model on screen — through `animLookup(sno)`.
- **Evidence it is a move, not a rewrite:** a function-by-function diff of every moved body against
  its original shows only the intended member → parameter substitutions (79 lines across 12
  functions); both scan bodies are present verbatim; the cache format, magic numbers and file paths
  are unchanged, so an existing `index_cache/anim_index.bin` / `entity_index.bin` loads as before.
  D4's standalone app builds (container, Qt 6.4 mirror) and D4's own `verify-src.py` is clean.
- **Behaviour check:** `conformance/tools/d4_animindex_check` — 24 assertions over a synthetic
  d4data tree in the real files' shapes (Anim, AnimSet, Actor, Item, CoreTOC), expectations derived
  by hand from ModelsTab's documented rules (authored vs set vs inherited base, "no family ⇒ expand
  nothing", cache invalidation by game build). In the suite and the bat (`d4_animindex`).
- **The store:** after open, `D4Store::buildAnimIndexes` (worker) loads or scans both indexes
  (same cache key as ModelsTab), then a model lists its clips LAZILY — D4's three export sources,
  authored ∪ sets ∪ inherited base — and `bakeClip` decodes one through `decodeForSkeleton` with the
  model's own rest pose, binding tracks by bone name hash and swapping Z-up → Y-up per channel
  exactly as D4's exporter does, dense as D4 writes them. The data is an immutable snapshot behind a
  `shared_ptr`, so a reload never rewrites what an in-flight load is reading. "Secondary ready" now
  waits for both AppearanceMeta and the animation index.
- **Not the same as D4's panel:** the ANIMATIONS panel also lists skeleton-guessed families and
  pulled clips; the core tab lists the three export sources only.

## First real-install run (2026-09-23): 29/29, and what the logs showed beneath the needles

`Build and Verify.bat` passed 29/29 on the real install (D4 self-tests 11/11, boot, both shots, the
export with 4 images, the texture). The needles passing was not the whole story — the logs showed
three things, now handled:

- **Exit while the animation index was still scanning (defect, fixed).** Every harness run quits
  seconds after "index ready"; the anim scan and D4's AppearanceMeta crawl run on detached workers,
  so each run destroyed statics under them (`QMutex: destroying locked mutex`, up to 271
  `invalid QRegularExpression` lines per log), and the scan never finished, so its cache was never
  written and every run rescanned. Same class as FOX's exit crash. `D4Plugin::shutdown` (main's
  `afterExec`) now waits for its workers and the crawl, pumping their queued results so both caches
  get written, bounded at 180 s; past the bound it `quick_exit`s rather than destroy what a worker
  uses. D4's own app has the same detached workers — untouched there.
- **No clips in any shot (gap, fixed).** A model loaded before the animation index is in lists no
  clips, and nothing re-listed them. The core Models tab now re-asks the store for the shown model's
  clip list when the secondary indexes arrive (listing only; still lazy) — generic, as D4's own
  ModelsTab re-populates its ANIMATIONS panel when its scan lands. New harness flag
  `--wait-secondary` (core) acts after the secondary indexes; `d4_shot_rig` now uses it with
  `--clip 0`, needle `models: decoded clip`.
- **`--model barF_base00` loaded `npc_barF_base00` (harness defect, fixed).** Selection took the
  first substring row. `AssetListModel::bestRowFor`: an exact name or file stem wins, else row 0
  (Models and Textures). Synthetic checks for both core changes are in `shell_check`.

**Not a phase-6 regression:** `N sub-object(s) … sit on vertex buffer(s) other than 0 and were NOT
loaded` (barF_stor151_TRS 12 / ~20k verts, barF_base03_TRS 12, npc_barF_base00 4). That is D4's own
parser (ModelParser.cpp: sub-objects on a second vertex buffer are dropped, ~10% of wardrobe
appearances), already listed as open in D4's docs; D4's own app shows the same meshes. It is D4
parser work, tracked there. Also observed, not yet examined: the lion pelt's back cards render black
in the shot — plausibly D4's look (6c: its viewport material path is not attached yet); a HUNCH until
compared against D4's own viewport.

## Second real-install run (2026-09-23): 29/29, clips confirmed

- **Clips through the core, on the real install.** `barF_base00` lists **701** clips (d4/CLAUDE.md:
  "canonical rig, ~700 clips"); clip 0 `BarF_emotes_016_stor` decodes to 202 frames, 190 tracks in
  9 ms. `barF_stor151_TRS` — shown before the index was in — re-listed **691** clips when it
  arrived. The index: 7,845 clip owners, 7,526 families, 31,824 appearances with actors; the first
  scan took 104.6 s, every later run read it from cache in ~200 ms.
- **Exit is clean.** 0 `invalid QRegularExpression`, 0 `destroying locked mutex` across all five D4
  runs. The boot run (first run: both caches built) waited 168.7 s — the AppearanceMeta crawl is the
  long pole, close to the 180 s bound; past the bound the process `quick_exit`s, so a slower machine
  loses only that run's crawl cache, never corrupts it.
- **Clip bar out of step (defect, fixed).** A clip chosen by the harness posed the model while the
  bar still read "(bind pose) 0.00 / 0.00s". `ModelsTab::setClipIndex` — the one path the bar, the
  ANIMATIONS list and `--clip` share — now sets the box, the list row and the time readout.
- **barF_base00 renders headless:** 58 sub-objects (~40k verts) on a second vertex buffer, D4's
  parser limitation above — the head is among them. Worth raising in priority: it is the canonical
  rig. D4 parser work, in D4's tree.
- **Cloth cards render untextured (white skirt / black pelt back):** 6c (D4's viewport material
  path). HUNCH until compared with D4's own viewport.

## 6b, second part: row flags and attachments (2026-09-23)

- **The Special group (core).** D4's funnel had a "Special" section — Latest, Animated, Rigged,
  Orphaned — pseudo-tags, not tags. The core had the row flags (POE2 already set Rigged) and nothing
  read them. `AssetListModel::setTagGroups` now appends a **Special** group with one entry per flag
  the base set actually carries (New, Animated, Rigged, Orphaned, Encrypted, Unnamed, Shadowed), so a
  game sees exactly what it sets; facets `flag:<name>`. Bulk offers only flags every listed kind
  carries (its tag-group rule), and now re-applies groups when the secondary indexes land.
- **D4's flags.** `D4Store::row`, once the anim + entity indexes are in, with applyCategoryFilter's
  rules verbatim: Animated = owns clips or shares a clip-owning base family; Rigged = owns clips or
  on any base rig; Orphaned = no Actor uses it. Unrenderable is NOT carried: in D4 it is learned per
  session from thumbnail failures — a core thumbnail concern, open.
- **The attachment contract (core).** Where a piece goes is the game's rule. `core::Attachment`
  gained `group`, `key`, `defaultOn`; `IGameStore::placesAttachments()/placeAttachment()` lets a
  store bake a piece itself; `model::assemble` takes the loaded model + per-piece ticks and returns
  geometry, materials, part→attachment map and a per-piece reason. The Models tab re-assembles from
  the clean body on every tick (a detach is exact), keeps the playing clip, and exports what is
  ticked; POE2 keeps its rule (core-placed at the parent bone, all on).
- **D4's attachments.** `D4Store::attachments`: ModelsTab::scanAttachments' source — the actors that
  use the appearance (entity index, D4's cap of 80) → `ModelAttach::scanActor` — listed OFF (D4
  attaches nothing until ticked), cached per model. `placeAttachment` calls D4's `seat()` (hardpoint,
  grip, scale, the main-hand half-turn) or `seatMount()` (saddle), then the joint remap `loadModel`
  uses, and the child's own export materials. Harness `--attach <terms>`; bat check `d4_attach`:
  Barbarian_Ancient_Korlic, whose actors hold two `Barb_Ancient_axe_base08` on spawn (d4data Actor
  JSON, msgKey 1000 — measured, 220 of the first 400 actors with an AddObject hold something).
- **Cost, to measure:** D4 deferred the actor scan until its ATTACHMENTS panel opened; the core lists
  on every viewport load (on the worker; thumbnails and non-assembling exports skip it). Each scan
  logs `d4: N attachment(s) from M actor(s) of <model> — T ms`, so the choice rests on numbers.
- **Audit (static, subagent) found three defects, fixed and now tested:** a store that names no
  materials (D4) had its pieces index the BODY's materials; a part selection was ignored whenever a
  model listed attachments; the actor scan ran for thumbnails and bulk. And seven lesser ones fixed:
  the list cleared inside its own signal, model-only flags hiding textures in Bulk, a re-index
  dropping the cache between list and tick, a load in flight when the secondary indexes land never
  re-listed, a second upload on every load, the two multi-export paths disagreeing, reset keeping
  stale ticks.
- **The suite could pass with a failing check (defect, fixed).** `check | grep -v …` returns grep's
  status: blender, viewport, thumbnail and shell checks all swallowed their exit codes. `run_check`
  fails the suite on the check's own code (proven with a deliberately failing command).
- **Observed, not changed:** attached static pieces (a mount) and D4's own exporter both write
  all-zero weights on a skinned mesh — parity with D4, and not valid glTF skinning. A piece's vertex
  colours / UV1 are dropped when the body has none (the merge keeps only tangent state).

## Third real-install run (2026-09-23, overnight): 30/30 — and a parser defect under it

- 30/30. Attachments listed on the real install: `d4: 6 attachment(s) from 3 actor(s) of
  Barbarian_Ancient_Korlic — 6 ms`, `0 … from 1 actor(s) of barF_base00 — 15 ms`. **The actor scan
  costs 6–15 ms** — cheap enough to stay on every load; the deferral D4 did is not needed.
- **Two harness defects, fixed.** cmd splits `--attach axe,axe` at the comma, so only one term
  arrived (the bat now quotes it); and a shot after attaching kept the body's framing, so the
  attached piece was off-screen (the harness now frames the assembly — a person ticking still
  keeps their view, as D4).
- **The repro was wrong, and why.** Korlic rendered as ONE part, 256 triangles, 163 vertices. Its
  own .app.json says 20,873 LOD0 vertices, all on vertex buffer 0 — so not the second-buffer gap,
  and no warning fired.
- **Cause, from the code:** `ModelParser::findSubObjects` accepts a SubObject whose ptSegments
  array is exactly 32 bytes — ONE segment. A sub-object with 2+ segments is never matched, so all
  its geometry is absent, silently. **Measured over all 68,236 Appearance JSONs** (d4data,
  `_deliveries/measure/multiseg_all.tsv`): 338 of 64,787 with LOD0 geometry are affected (0.5%);
  123 lose more than half their LOD0 vertices; 34 of 1,058 `_base0*` rigs. Korlic loses 20,710 of
  20,873; **barF_base00 loses 23,113 of 41,426**.
- **Correction:** the second-run note said barF_base00's missing head is among the 58 sub-objects on
  a second vertex buffer. The LOD0 data says otherwise: its second-buffer LOD0 sub-objects are
  small cards (≤216 verts each); its one multi-segment sub-object (material 5, 18,876 + 4,237
  verts) is the likelier head/body loss. HUNCH until the shot with the fix confirms it.
- **The fix, gated (D4 tree, `model/ModelParser.cpp`).** With `D4_PARSE_MULTISEG=1` a 32·n-byte
  array whose later records scanSegments also found (32 bytes apart) is matched, and every segment
  feeds the sub-object's ONE primitive through its own bone palette — one primitive per
  sub-object, the invariant everything downstream (slot hashes, cloth, outliner) keys on. **Off by
  default: D4's behaviour is unchanged** (same control flow for a one-segment array; a multi-segment
  one is still skipped) except that the loss is now logged — `model: N multi-segment sub-object(s)
  (~V verts) were NOT loaded`. It builds (container, D4 standalone and core), D4's verify-src is
  clean. Not verified on real bytes — the container has no CASC. Bat checks: `d4_multiseg_off`
  (the warning appears for Korlic), `d4_multiseg_on` (Korlic with the gate), `d4_multiseg_rig`
  (barF_base00 with the gate, clip 0). **Decision for the user after those shots:** make it the
  default and remove the gate, or not.
- **New attachment repro:** `demon_butcher_miniboss` (the Butcher, 62,553 LOD0 verts on one buffer,
  no multi-segment sub-object) holds a sickle and a cleaver on spawn, the cleaver on HP_rightWeapon
  (D4's main-hand half-turn) — chosen by measuring the JSON, not by name.
- **Seen, not changed:** three `-Wformat` warnings in D4's `casc/CascReader.cpp:1480,1516` (`%d` given
  a `qsizetype`) — undefined behaviour in a log line on 64-bit; D4's tree, left for the user.
- **The ModelsTab gap list is written:** `docs/MODELSTAB_GAP.md` — 82 items from the source (53 core,
  13 D4 plugin, 9 D4 viewport extension, 7 drop), each with evidence and a size, and a top-10. It
  also found: parts hidden with Hide/Isolate are still exported — **confirmed and fixed** (a whole-
  model export of the model on screen now writes its visible body parts; an explicit selection is
  still written verbatim — D4's scope rule; tested); the Textured-icons toggle does not take effect
  — **confirmed and fixed**: the render thread kept the loader it was created with, and both icon
  styles shared one disk folder (a replaced loader now reaches the running worker; each style has
  its own folder, `thumbs_v1` / `thumbs_v1_tex`, and POE2's cache page lists both; tested); D4Store implements neither dependencyFiles nor readPath, so
  "Extract originals" is dead for D4.

## Fourth real-install run (2026-09-23 morning): 33/33

- **The multi-segment fix works on real bytes.** With `D4_PARSE_MULTISEG=1`: Barbarian_Ancient_Korlic
  1 part / 256 tris → **2 parts / 32,944 tris**, a complete, correctly shaped character (shot);
  barF_base00 16 parts / 20,305 tris → **17 parts / 54,621 tris**, head, horned helm and armour
  present (shot) and clip 0 still decodes (190 tracks) against it. Without the gate the new warning
  fires exactly as measured: `1 multi-segment sub-object(s) (~20710 verts) were NOT loaded`.
  **Made the default (user's decision, 2026-09-23)** after the user checked D4's own viewport:
  the glow is real — barF_base00 carries the barbarianF_wrath "transformed" version of the player
  character as submeshes in the same appearance (user). `D4_PARSE_MULTISEG=0` restores the old
  behaviour for diagnosis. Bat: `d4_multiseg` requires Korlic's full `32944 tris`; `d4_multiseg_off`
  checks the opt-out still logs the loss. Consequence for later work: an appearance can hold more
  than one "form" of a character — the Looks / part-visibility work (gap list C2, E2) must let those
  submeshes be shown one form at a time.
  HUNCH to check first: with the gate, barF_base00 shows an orange lava-like glow over skin and armour
  that the gate-off shot did not; it may be that look's real emissive, or an emissive mis-mapping on
  the newly loaded sub-object. D4's OWN app uses the same parser, so launching it with
  `D4_PARSE_MULTISEG=1` and opening barF_base00 settles it against D4's own viewport.
- **Attachments on the Butcher:** 12 pieces from 6 actors listed (30 ms); sickle and cleaver attached,
  textured with their own materials — but placed WRONG: the cleaver lies on the floor at the model
  origin, not in the hand, and the sickle is not visible. Ruled out from the data: the hardpoint
  resolves (bone 75 HP_rightWeapon, bone 49), its transform is identity (q w=-1 = the same
  rotation; `hpIsAuthored` tests the matrix), the grip transform is identity, scanActor takes the
  first non-zero link, parseApp fills the rest TRS jointWorldMat composes. Not decidable from JSON
  (the bones are binary). **Instrumented, not guessed:** placeAttachment now logs the bone, its world
  from the rest TRS and from the inverse bind, the placed bounds centre and the body's bounds —
  the next `d4_attach` run says which of "rest pose ≠ bind pose" or "the bone's world is at the
  origin" it is.

**Open, in order:**

1. **6b, rest — ModelsTab's other store state.** Attachments: `scanAttachments` (Actor JSON).
   Icons (`IconIndex` via `AppearanceMeta::iconFor`), links (`AssetLinks`), Unrenderable /
   Animated / Rigged flags (Animated/Rigged are now one lookup away: `animatedSnos`, rig families).
   ~~Then Wardrobe / Stable hosted and Catalogue re-pointed~~ — **dropped (user, 2026-09-23):**
   Stable and Catalogue are D4-only (Catalogue exists because only D4 has a prominent shop). They
   stay in the standalone D4AssetBrowser for now; in the add-on host (NEXT_SESSION.md,
   "Long-term direction") they would come back as D4-only tabs that D4's add-on brings. A
   generic bundle browser is written only if a later game has a store. Wardrobe is replaced by ONE core Customize tab
   spliced from D4 Wardrobe + FOX Customize + POE2 Customize: `docs/customize/CUSTOMIZE_CORE_DESIGN.md`
   (build order POE2 → FOX → D4; the three source studies beside it).
2. **6c — D4's look.** The D4 `ViewportExtension`: its shader blocks (or whole shader, §8.9), cloth
   solver, dye, fur shells, detail maps, hair/eye/skin, fed by `applyPartMaterials`' inputs through
   `Material::engine`. Gate: the repro assets pixel-identical to D4's own viewport.
3. **The ModelsTab gap.** D4's ModelsTab is 11,675 lines; the core's does a fraction. What it has
   that is generic — the outliner, the camera and lighting panels, looks (SOA), icon rendering with
   its crash guard, drag-out, grouping, filter persistence — belongs in the CORE tab so every game
   gets it (the template's point). What is D4's — dye, cloth physics, item browser, bundle links —
   is plugin panels. That list is written from the source before any of it is moved.

## Fifth real-install run (2026-09-23 evening, `Run Everything.bat`): 33/34

D4's own app rebuilt (rc 0: WardrobeTab2.cpp recompiled, linked). The monorepo: 33 of 34.

- **Core Customize on real POE2 data — passes.** Catalogue 12 classes · 8 slots · 7,864 options ·
  141 outfits; `--build "subject=Warrior"` assembled (73 parts, 5 pieces, 87 joints) and was
  photographed; `--build random:7` assembled a Druid with 7 slots filled (81 parts, 9 pieces) and
  exported it (81 primitives · 18 materials · 42 images). Two defects seen in the shots:
  - **The left column overflowed its panel** — every combo sized itself to its longest entry
    (thousands of long names), pushing the right-hand buttons and each slot's export button past
    the edge. Fixed: the column's combos ask for a 10-character minimum and stretch; no sideways
    scroll.
  - **The Warrior's default `head_attach · rig_c0e0a7d0` lies between the feet**: its bone
    `Head_Attachment` is not in the Warrior rig, so it is placed unbaked (the old tab's rule, kept).
    Not fixed by guessing a name: the note now lists the rig's nearest bone names, and the next run
    says whether it is a naming mismatch.
- **FAIL shell_synthetic (Windows only): the raw-originals bulk run lost both pieces under
  `art/models/items`** — "could not create …/raw/art/models/items" even after phase 5's one 5 ms
  retry; the sibling folder `npc` was created. The concurrent-`CreateDirectory` race that retry was
  for. Fixed properly: folder creation in `BulkExtractor` is serialised, retried with backoff
  (~1.3 s), and a final failure reports the OS's own error text and code.
- **The Butcher's weapons — the instrumentation answered.** Both hardpoint bones (49, 75 of 197)
  have world translation **zero in the rest pose AND in the inverse bind**, so `seat()` put the
  pieces exactly where the data says the bones are: at the origin. Nothing in the seating maths is
  wrong. **Hunch, marked as one:** these are prop bones that only a clip moves into the hands (a
  common rig pattern). This run adds the bone's name and parent chain to the log line and a
  `d4_attach_anim` check (the same model and attachments with clip 0 playing, photographed beside
  `d4_attach.png`); if the weapons are in the hands there, the rest-pose placement follows the
  game's data and the open question becomes a display choice (show such bodies on an idle clip's
  first frame), which is the user's call.

## Sixth real-install run (2026-09-23 evening): 35/35

- **shell_synthetic passes on Windows** with serialised, retried folder creation.
- **The Butcher's weapons are right under animation.** With clip 0 playing
  (`BSK_hellfire_butcher_miniboss_attk_headbutt`) the cleaver is in the right hand and the sickle
  in the left (`d4_attach_anim.png`). The earlier hunch (a root-parented prop bone) is **refuted**:
  bone 49 hangs 10 bones deep (`bone_34ca60e3 < bone_f4dd810d < …`; this rig's names are
  unresolved hashes). Its rest and bind translations are nonetheless zero, so the file carries
  no rest placement for it and only a clip puts it anywhere. The seating maths is correct. How
  to show such a body when no clip is playing is a display decision, put to the user.
- **POE2 Warrior's stray head piece:** the log names the rig's closest bones as `head_jntBnd` and
  `aux_Head_attachment`. FORMATS.md §7 records that the `.ao` ClientAnimationController declares
  sockets (`socket = Back / parent = aux_Back_attachment`), which `parseAo` never reads. So
  `Head_Attachment` is very likely a SOCKET name, not a bone name. Rather than add a name rule,
  this cut logs, once per class, every socket / parent / attached_object line of the class `.ao`
  and of the files it extends. Socket resolution will be built from what that log shows. The core
  also gains `--dump-paths "<p>;<p>" [--dump-to dir]`: raw game files out through the store, with
  a UTF-8 copy of any UTF-16 text, for format research in any game.
- **User direction recorded (NEXT_SESSION.md):** each game's viewport reimplements that game's
  renderer, literally where possible and otherwise replicated as closely as the evidence allows.
- **Resolved (user's choice, 2026-09-23): idle pose when an attached piece has no rest data.**
  `IGameStore::attachmentNeedsPose` / `restClip`. Only from the bind pose, the Models tab opens
  such a model on the game's idle clip at frame 0, paused; a clip the user has chosen is never
  replaced. D4's rule comes from its data:
  - a piece needs posing when its hardpoint bone is not a root and has zero rest AND zero bind
    translation;
  - the idle is the clip an AnimSet binds to the `AnimKey_Neutral` power. That power is the first
    entry in 207 of 250 sampled AnimSets and present in 218. Clips from the appearance's own
    actors' sets are preferred.
  Three new shell_check tests cover it. `d4_attach`'s needle is now
  "models: posed on the idle clip".

## Seventh real-install run (2026-09-23 evening): 35/35

- **The idle pose works on real data.** On the Butcher, D4Store picked
  `demon_butcher_miniboss_nav_idle`, the one AnimKey_Neutral clip, from its own actors' AnimSet,
  and the tab opened on it with the weapons in hand.
- **POE2 sockets, from the log.** `metadata/characters/character.ao`, which every class `.ao`
  extends, declares `socket = "Head_Attachment"` / `parent = "aux_Head_attachment"`, alongside ~60
  other sockets (Back, R_Weapon → aux_R_Weapon_attachment_jntBnd, sheaths…). The Warrior's hair
  (`BruteHairLong.ao`) hangs from that socket; the Druid's hangs from the bone `head_jntBnd`.
  The chain walk is confirmed: all 12 classes resolved `extends "Metadata/X"` as `metadata/x.ao`,
  ending at `metadata/parent.ao`. Built from that:
  - `Poe2CustomizeSource::socketsOf` reads every socket in the chain, the nearest declaration
    winning, and a default piece whose bone is a declared socket is placed on the socket's parent
    bone.
  - Socket lines other than `parent` (`ignore_parent_axis`, `ignore_parent_rotation`) are logged
    as not applied; they are runtime orientation rules.
  - **Hunch, marked:** a name that is both a socket and a bone resolves as the socket. No such
    name has been seen yet.
  - New checks: `poe2_socket` (needle "socket Head_Attachment") and `poe2_dump`, which writes
    both `.ao` files whole so every line a socket block carries can be read.

## Eighth real-install run (2026-09-23 evening): 36/37

- **POE2 sockets confirmed.** The Warrior's hair reads "socket Head_Attachment → bone
  aux_Head_attachment" and now sits on his head in `poe2_build.png`; nothing is left at his feet.
  The dumped `character.ao` shows sockets also carry translation / rotation / scale. Written up in
  `docs/customize/CUSTOMIZE_POE2.md` §12; the offsets are not applied yet (Euler order unknown,
  and no current piece uses one).
- **FAIL viewport: "scalePercent re-renders at the larger size" (Windows).** The check expected
  exactly 640×480 from 200 % of a 320×240 widget. `renderToImage` works in device pixels
  (width × devicePixelRatio × scale). This run's screenshots came out 2560×1600 where the last
  run's were 1280×800, so the display's scale factor changed between runs. The check was wrong,
  not the renderer: it now expects width × ratio × 2 and prints both sizes and the ratio if it
  ever fails again.

## 2026-09-24 (user away): FOX Customize fixes, step 2 planned

- **Survive per-slot colour persists** (FOX tree `src/tabs/CustomizeTab_Weapon.cpp`:
  `slotcolor:<slot>=<swatch path>` written and read back; an absent field means as shipped).
  **Customize host:** Undo/Redo bound to FOX's rebindable rows. Core Export rows delegate to FOX's
  by role, FOX's duplicates are removed, and roles are mirrored so Ctrl+E / Ctrl+Shift+A work.
- **New core harness flag** `--export-menu`: logs every Export row as `<role> on|off <text>`.
  **New FOX flag** `--build-roundtrip`: build text written, applied back, written again, compared.
- Checks `fox_slotcolor` and `fox_export_menu`. **Hunch, marked:** the swatch name
  `cm_scol4_c07` comes from FOX's own harness comment; if this install has no such swatch the log
  will say "slotcolor fields 0".
- Step 2 broken into five stages (`docs/customize/CUSTOMIZE_CORE_DESIGN.md`). FOX's look
  (its `ViewportExtension`) comes before FOX's tab retires, per the user's rendering rule.
- Container: all suites pass; verify-src clean on both trees (FoxPlugin.cpp baseline 3 → 4 for the
  `slotcolor:` text field).
- **Stage 2a built** (same day): the core Customize tab now covers categories, variants, look
  rows, section captions and swatches, named default rows, game rules with Unlocked, and
  compatibility narrowing with honest states. Details are in `CUSTOMIZE_CORE_DESIGN.md`
  ("Status — stage 2a"). 13 new synthetic assertions pass; POE2's build text is unchanged
  (no categories and no variants means no new keys).

## Ninth real-install run (2026-09-24 morning): 38/39

- `fox_export_menu` passes: Export character, …to last folder, parts, per-variation, image,
  turntable, GIF (off without a clip). The Customize host now reaches the core menu and its
  hotkeys.
- **FAIL fox_slotcolor — and what it uncovered.** The round trip itself was the same (7 of 7
  fields), but the harness's `slotcolor:torso=cm_scol4_c07` was "NOT matched". The swatch
  exists by FOX's own measurement (`index/LayerColors.h`: cm_scol4 has 25 swatches). The cause
  was that **the monorepo never built `LayerColorCatalog`**, nor `MaterialPresetTable` (FMTT) or
  `ShaderTable`. FOX's MainWindow builds all three on the index-ready edge
  (`MainWindow.cpp:311-330`); the port copied the rest of that edge but not these. Consequences
  beyond the colour rows: every FOX material in the monorepo has had its FMTT F0 / specular
  presets fall back to the dielectric default (`ModelLoader.cpp:1092-1110`), and the shader table
  was absent. Fixed in `FoxPlugin::reload`, which now builds all three before the store and the
  tabs hear the index is ready, with FOX's own log lines. New check `fox_catalogues` (needle
  "fmtt: "). My earlier note calling the swatch name a hunch was right to flag it; the name was
  never the problem.

## 2026-09-24: startup / lifecycle parity audit of all three ports

After the missing FOX catalogues, two read-only audits compared each standalone app's startup,
index-ready, reload and shutdown steps with its port. Each finding was checked against the code
before being fixed. **Fixed:**

| # | Game | Gap (standalone evidence) | Fix |
|---|---|---|---|
| 1 | all | A reload asked for during one was **dropped** (`AppShell::reload: if (m_reloading) return`); FOX restarted and D4 queued | queued, and runs when the current one finishes (shell_check: 2 new assertions) |
| 2 | all | Reload **during a bulk export**: the workers keep reading the index as it is replaced (FOX refused, `MainWindow.cpp:1653-1661`) | `BrowserTab::busyReason()`; BulkTab answers while running; the shell refuses and says why |
| 3 | all | **No window icon**: `CMAKE_AUTORCC` was never on, so no game's `.qrc` was compiled (D4's log: "app icon … missing from the Qt resource") | AUTORCC on; the D4 and FOX `.exe` icons via their `app.rc`; FOX's window icon set |
| 4 | FOX | Model **thumbnail disk cache keyed by file index**, which a rescan renumbers: wrong thumbnails after a mod/folder change, persisting across launches | `IGameStore::diskCacheKey`; FOX names by path hash as its own cache did (`gl/ThumbnailRenderer.cpp:392-404`) |
| 5 | FOX | FOX's own code reads `paths/exportDir`, `session/remember`, `interface/hoverPreviews`, but the port **renamed and deleted** those keys at every start: the FOX tabs lost their "…to last folder" | an additive `Config::setKeyAlias` seam in the FOX tree (the standalone never calls it); the port points those accessors at the family keys, so both sides share one value |
| 6 | FOX | Derived sweeps were reset only after the new index was ready, while the old sweep could still be reading the index being replaced; FOX's own thumbnail renderer was never reset | `TextureUsers` / `RefIndex` / `fox::ThumbnailRenderer` reset before the rebuild starts (`MainWindow.cpp:358-367`, `ModelsTab.cpp:1432`) |
| 7 | FOX | Mod-folder replace / revert **never rescanned** (`MainWindow.cpp:438-441`) | `AppPlugin::requestShellReload()`; FOX's `modfolder` hook calls it. The scene restore and "Revert" bar button are not carried yet |
| 8 | FOX | Files preview kept the old model after a rescan when the same file INDEX came up again | the comparison also checks the path hash |
| 9 | FOX | Settings did not reach Customize: PBR switches and rebound viewport / animation keys took effect only after a restart | host `onSettingsChanged` (and the legacy dialog) runs FOX's `syncPbrFromSettings` + shortcut installs |
| 10 | FOX | No log file by default (FOX always wrote one; its regression bat copies it) | `logFileDefault = true` |
| 11 | D4 | A reload while the previous session's anim scan / metadata build still ran reopened CASC under them (use-after-free) | D4's reload waits, polling on the GUI thread, until they stop |
| 12 | POE2 | The index worker was joined after `QApplication` was gone and posted to a null `qApp` | `Poe2Plugin::shutdown()` via `afterExec` |

**Not fixed, and why:**

- **D4 IconIndex / AssetLinks / ItemHoverIndex / texture format table / category rules.** These
  are features (icons, links, texture facets) on the "D4 icons/links" list, not lifecycle fixes.
- **D4 pre-rename INI copy.** The port's data folder is its own, so there is no pre-rename file for
  it to find.
- **FOX game-tick filter on the core lists.** Possibly deliberate (the funnel's `game` group); to
  confirm with the user.
- **Status-bar index progress and index roster for FOX and POE2**, FOX's session restore, and
  FOX's scene restore after a mod change. These are UI still to be re-hosted.
- **POE2's GL 4.5 requirement** (standalone asked for 3.3). This comes from the core viewport.
- **Uncertain, not changed:** FOX `AnimCatalog` rebuilt from a worker thread by a model load
  racing Customize.
- **Stage 2b, first part** (same day, container-only): pieces can now grow the figure's rig. This
  covers merge-by-bone for in-place pieces, `SocketRig`, socket offsets, chains, and an unseated
  state with an amber label. Details are in `CUSTOMIZE_CORE_DESIGN.md` ("Status — stage 2b"). 6
  new synthetic assertions pass. Watch POE2's first real run for the "added under their own
  parents" note.

## Tenth real-install run (2026-09-24 evening): 39/40

- **All of today's container work behaves on real data.**
  - No "app icon … missing" line, now that AUTORCC is on.
  - The D4 idle pose still engages.
  - POE2 assembles exactly as before (87 joints, so no bones appended; the same part counts).
  - The FOX export menu passes.
- **FAIL fox_slotcolor, second layer found.** The catalogues now build, but EMPTY: `layer: 0 solid
  colour(s) and 0 pattern(s)`, no `material_params.fmtt`, no shader pack. FOX finds all three by
  NAME. With no dictionary folder set, FOX reads its name dictionaries from `<exe folder>\dict`
  (`AppPaths::dictDir`), and the monorepo's FOX exe folder has never had one: every run's index
  line says "0 named" top-level entries. The consequences run wider than colours:
  - FOX's top-level file names have been unresolved in the port all along;
  - its materials fell back to F0 0.04;
  - the shader pack was never read.

  **Fix:** `Build and Verify.bat` copies FOX's dictionaries (from `FOXAssetBrowser\build\release\dict`,
  the folder FOX's own exe reads, which carries the recovered name lists) beside the monorepo FOX
  exe (`:foxdict`, newer files only, `*.txt`). FoxPlugin now writes `catalogues: all three loaded`
  only when all three found their data, and warns with the dictionary folder otherwise.
  `fox_catalogues` checks that line, where it used to accept any `fmtt:` line.
- **Stage 2b, second part** (container-only): FOX's rest alignment in core as `Piece::alignRoot`,
  covering exits b, c/a and the hat31 "figure root" rule. 3 new synthetic assertions pass.
  Details are in `CUSTOMIZE_CORE_DESIGN.md`.

## Fourteenth real-install run (2026-09-24, night): 40/40, Customize 2e-1 under MSVC

- The 2e-1 core seams build under MSVC. `Piece::seatOn` seats on a host's hardpoint, and
  `Look::keepSubtree` gives the head-only base. Their 6 `shell_check` assertions pass there.
- POE2, FOX and D4 on real installs: unchanged, all green.

## Thirteenth real-install run (2026-09-24, late): 40/40, first MSVC build of Customize 2c

- **2c (looks as overrides, per-piece looked exports) builds under MSVC.** All of its
  `shell_check` assertions pass there, as in the container.
- **The crash note is said once.** Only `_fox_selftests.log`, the first FOX start of the run,
  carries it, now naming `FOXAssetBrowser-crash-previous.txt`. No later FOX run repeats it.
- POE2, FOX and D4 on real installs: unchanged, all green.

## Twelfth real-install run (2026-09-24 night): 40/40

- **fox_shot_files passes** with the `.fmdl` search ("files: preview").
- **fox_slotcolor passes**: "build roundtrip same; slotcolor fields 1", and the apply built
  9 of 9 fields and 6 parts. The slot colour survives the round trip through the build text.
- **Why `cm_scol4_c07` never matched: it is not in this install.** The logged list holds
  `cm_scol3_c00`–`c21` plus only `cm_scol4_c02`, `c17` and `c19`. The name came from my test
  spec, not from the game. It was not a lookup bug.
- **The stale crash note** (2026-09-22, the exit crash phase 5 fixed) was announced on every
  start. `crash::install` now moves a note it finds to `<product>-crash-previous.txt` and
  announces it once. Copy diagnostics names it as "Last crash note". Tested in the container:
  planted a note and started twice; the first start warned, the second did not, and the
  note was kept.
- D4: all 9 checks pass, as in run 11.

## Eleventh real-install run (2026-09-24 evening): 38/40, and FOX now has its names

- **The dictionary fix worked.**
  - Top-level named entries went from 0 to **246,919** (654,116 named in all, was 407,197).
  - `catalogues: all three loaded`: 25 solid swatches + 18 patterns, 256 FMTT presets (148
    metallic), 2,450 shaders from 22 packs.
  - Survive female Customize went from 13 slots / 5 parts to **22 slots / 6 parts**. The face
    rows are live and the face is retextured ("skin 1 · wrinkles 1, 4 of 12 materials"), where
    before it was the placeholder the model ships with.
- **FAIL fox_shot_files — caused by the names.** The search `bdf0_main0_def` now matches 27
  files, and the first is a texture (`…_c00_bsm.1.ftexs`, shown in hex), which has no model
  preview. The check now searches `bdf0_main0_def.fmdl`.
- **FAIL fox_slotcolor — still "NOT matched"** for `cm_scol4_c07`, with 25 solids loaded. The
  cause is not known yet. The run now logs every swatch name the harness matches against. The
  check asks for the first solid (`slotcolor:inner=first`, on the slot ssd_f wears by default),
  so it no longer depends on a name.
- A crash note shown at FOX start-up is from 2026-09-22, the exit crash phase 5 fixed; the note
  file stays until deleted.
