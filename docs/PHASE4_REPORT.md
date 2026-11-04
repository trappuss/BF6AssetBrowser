# Phase 4 — the shell, the generic tabs, and POE2 on the core

**Status: built and green in the container; the gate is HALF closed.** `abcore` now carries the
whole application shell and the three generic tabs (12,019 lines under `core/app`, `core/ui`,
`core/tabs`), and `games/poe2` is the first game on it: `POE2AssetBrowser` links abcore + POE2's
own engine code and boots headless with 18/18 self-tests passing. `conformance/run_tests.sh` runs
the phase 1–3 suites unchanged plus a new phase-4 section: 46 shell checks over a synthetic store
(0 failures), the POE2 binary's `--check`, and three boot needles. What stays open is the half that
needs a Path of Exile 2 install — the container has no `Bundles2` — and that half is one command on
the user's machine (§ The gate).

## What exists now

```
core/app/   AppShell (menus · export menu · status bar · Ctrl+1..9 · hotkeys · toast + tray · index
            indicator · LazyTab · first-run page) · Application (runApp: the one main()) · AppPlugin
            (the game seam, extended) · SettingsDialog (Interface / Export / Hotkeys / Maintenance /
            Information + plugin pages, snapshot-on-Cancel, reset by removal) · Hotkeys registry ·
            AppLog + LogConsole + CrashHandler + SehGuard · ExportNotifier · StatusLine ·
            CoreSettings (the shipped key spellings) · Density · FontScale · StartupProfile ·
            CacheMaint · FirstRunPage · SelfTests · BrowserTab
core/ui/    QueryTerm (ONE matcher + injected id policy) · SearchQuery · SearchBox · FunnelFilter ·
            FilterChips · AssetListModel (kinds, tags, facets, icons) · NPanel/PanelBox/PanelPersist ·
            MenuText/MenuContext/MenuDump · TipBar · HoverInfo/HoverPreview · CsvCopy · Glyphs ·
            ThumbnailCache · TextReportDialog · SearchableCombo · ExportCapture (GIF ladder) ·
            ExportDialogs (options · image · GIF)
core/tabs/  ModelLoad (load · assemble · exportOne — the one load-and-export path) · ModelsTab ·
            TexturesTab · BulkExtractor (pool + coordinator, manifest, failures) · BulkTab
games/poe2/ Poe2Store (IGameStore over AssetStore) · Poe2Plugin (tabs, Directories page, Health
            check, caches, id policy, export defaults, Type layout, 10 self-tests) · main.cpp
            (14 lines of tables) · tabs/CustomizeTab (POE2-owned, ported to the core viewport) ·
            CMakeLists.txt (compiles POE2's engine from POE2_SRC until phase 7 moves it)
conformance/tools/shell_check.cpp   the shell over a synthetic store: 46 checks
verify-src.py                       ONE script, per-project tables (§6.6)
```

Whose code: the shell is FOX's (Hotkeys registry with hints, AppLog notifier + tail, CrashHandler,
LogConsole, StatusLine source filter, SettingsDialog skeleton, NPanel, SearchQuery, FilterChips,
MenuText/Context/Dump, Density/FontScale/StartupProfile, FirstRunPage, CacheMaint) with D4's grafts
(BrowserTab export hooks, LazyTab, the IndexSpec roster, export toast + tray, the settings clamp and
the Cancel snapshot, the live log-file toggle, HoverInfo, CsvCopy, the worker-thread load under
`seh::runGuarded` with a token) and POE2's contracts (FunnelFilter with counts, AssetListModel,
ExportCapture, the three export dialogs, BulkExtractor, TextReportDialog, SearchableCombo,
ThumbnailCache). Per-game vocabulary is injected, never hardcoded: nouns, hotkey table, id policy,
tag groups, layout spec, export defaults, cache defs, settings pages, help entries.

## POE2 on the core — what changed and what did not

`Poe2Store` is the plan's "AssetStore::* almost verbatim": every method is one AssetStore call plus
`poe2adapt::toCore`. Rows are the `.smd`/`.fmt` and `.dds` records with Facets' path taxonomy
attached once on the worker; the Shader facets resolve through `MaterialFamilyIndex` and — POE2's
rule — match everything until the secondary indexes are visible, so a chip never hides what it
cannot yet classify. The id policy is POE2's `QueryTerm` rule exactly: decimal or `0x`-hex,
matched as a substring of `"<decimal> <16-hex>"`. Settings keys keep their shipped spellings;
the only migration is a one-time VALUE rename of `export/folderLayout` (`Folder`→`_path`,
`_model`→`_item`) logged at startup; POE2's own `Type` mode is kept under its own id.

Three things the port fixed or changed, each recorded here rather than left implicit:

1. **Vertex joints were not remapped after the parents-first sort.** The phase-3 adapter sorts a
   POE2 rig parents-first and remaps the clip tracks by that map, but nothing remapped the mesh's
   joint indices, which are bound by the same bone index. `poe2adapt::toCore(sk, clips, remapOut)`
   now returns the map and `Poe2Store::loadModel` applies it to every weighted joint (identity —
   an empty map — for a rig already in order). The cost is one bones-only `.ast` parse per skinned
   load; the conformance adapter test already proves the track remap, and the vertex remap is the
   same map applied to the other index.
2. **CustomizeTab is one merged mesh.** POE2 drew the body plus per-piece "attachments" through a
   separate viewport path and rebuilt a MERGED figure only to animate; the core viewport has no
   attachment API on purpose (tabs-store.md), so the static view, the animated view and the export
   are now the same `core::Geometry`: in-place pieces keep their authored skinning, weapons bake
   rigidly at `inverse(inverseBind)` — `core::model::assemble`'s rule. Per-piece show/hide is
   part visibility on that mesh; the Head toggle hides the body's head parts by their authored
   names as before. Everything data-facing (class table, slot catalogue, family-validated
   attributes, outfit sets, presets under `customize/presets`, the four export modes, per-slot
   export) is POE2's verbatim. The tab is lazy — its index sweep runs the first time it is shown.
3. **The cache prune now prunes where the caches are.** POE2's main pruned `data/` while the
   caches lived in `data/cache/`; `RunOptions::Prune` with an empty dir means `data/cache/`,
   resolved after QApplication exists. Likewise POE2's Maintenance page listed `data/*.bin`
   (nothing, for the same reason); the core Maintenance page lists the plugin's `cacheDefs()`.

One thing marked as a HUNCH: the dressing room's "open on the character's front" is POE2's
`setOrbitYaw(yaw + π)` applied after the first `setScene` (both viewports default to yaw 0.6).
If the core viewport's yaw sign differs from POE2's the figure will open on its back; the fix is
the sign in `CustomizeTab::faceFront`, and only a run on real data can confirm it.

Two small core additions the port needed, both general: `IGameStore::readPath(gamePath)` beside
`dependencyFiles` (raw-originals extraction reads originals by the game's own path), and
`AppPlugin::takeReloadRequest()` — a settings page that moved the game folder asks the shell to
reload after the dialog closes (POE2's `gameDirChanged`). `BulkTab` gained `setOnlyNew` and
`setLayoutMode` for the harness. `runApp --quit-when-ready` on an unconfigured install prints
`shell: ready — … (unconfigured: first-run page shown)` and quits instead of hanging.

## The gate

Plan row: "`games/poe2` runs on the core end-to-end; the POE2 self-tests and `bulk_verify` pass;
stdout-needle checks for every tab under xvfb".

**Closed here (container, `bash conformance/run_tests.sh`):**

- `shell_check` (xvfb, Mesa): 46/46. Three tabs by stable id; reload → `indexBecameReady`; the
  one matcher (`#tag`, id term through the plugin policy, `-not`, `a|b`), funnel facets and counts;
  `showModel` → worker load → `modelLoaded` on the GUI thread with clips and rig; export-to-last
  writes a `.glb` with the glTF magic; Save preview image writes a `.png`; the Textures tab loads
  and exports; Bulk (lazy) lists both kinds and runs four times — 5 exported under the `Type`
  layout (3 `.glb` in `models/`, 2 `.png` in `textures/`) with `_bulk_manifest.json`; only-new
  skips all 5; one refused asset → 4 exported, 1 failed, named in `_bulk_failed.txt`; raw originals
  mirror the game paths. SettingsDialog `accept()` applies and emits; a rebound hotkey is read from
  `hotkeys/<key>`; the cheat sheet renders; reload repopulates.
- `POE2AssetBrowser --check`: 18 self-tests, 0 failures — the core's eight and POE2's ten
  (QueryTerm, MeshParser, DdsImage, AssetText, AstSkeleton, GlbExporter, ExportLayout, Facets,
  MaterialFamilyIndex, NameIndex).
- `POE2AssetBrowser --quit-when-ready --show-tab customize` under xvfb: `shell: 4 tab(s) built —
  textures, models, customize, bulk`, `panels: customize/npanel…`, `shell: ready — …`.
- `python3 verify-src.py`: 135 files, clean. It found and this phase fixed: 8 `qPrintable` calls
  (FOX's Windows-codepage rule — 2 in `core/gl/GLModelWidget.cpp`, the rest in conformance
  tools) and 7 files using `core::rm::` on a transitive include (D4's directive rule).
- `POE2AssetBrowser` compiles warning-free with `-Wall -Wextra` (games/poe2 and core; POE2's own
  engine sources are compiled as they are).

**Open — needs a Path of Exile 2 install (the user-side half):**

`bulk_verify` proper (`tools/bulk_verify.cpp` is referenced by POE2's CLAUDE.md but was not in
the staged tree; its claims are what `shell_check`'s bulk section asserts over the synthetic store)
and the real-index run. On the user's machine, after `cmake -S AssetBrowser -B build
-DPOE2_SRC=<POE2AssetBrowser folder>` and a build:

```
POE2AssetBrowser.exe --check                       → "self-test summary: 0 failure(s)"
POE2AssetBrowser.exe --quit-when-ready             → after Set game folder: "shell: index ready — Ready — N files in M bundles"
                                                     then "shell: secondary indexes ready" (materials + names)
POE2AssetBrowser.exe --quit-when-ready --show-tab models    → "models: N row(s) in model" and, on a click, "models: loaded …"
POE2AssetBrowser.exe --quit-when-ready --show-tab customize → "customize: catalogue — 12 classes, 8 slots, … options, … outfits"
                                                              "customize: assembled <class> — N parts, M pieces, K joints"
```

and one Bulk run over a small query (`Bulk ▸ Type layout ▸ Run`) compared against the old tool's
output of the same query: same file count, same `_bulk_manifest.json` keys. The three defects a
real run would surface that the synthetic one cannot: the joint-remap path on a rig that is not
already parents-first, the front-facing yaw sign (the hunch above), and the per-row cost of
`Facets` over 208k rows at open (measured nowhere yet — it runs on the worker, so it delays
"index ready", not the UI).

## Conventions this phase settled

- **Core owns the shell's settings keys; a plugin owns its game's.** The README's phase-1 line
  "core never owns a settings key" was true of the export library; the shell IS a settings
  consumer, and the audits fixed the spellings (`interface/*`, `session/tab`, `paths/*`,
  `export/*`, `image/*`, `gif/*`, `hotkeys/*`, `hover/*`, `tips/*`, `ui/*`, `log/autoFile`). A
  plugin reads its own (`paths/gameDir`) and passes values in. Reset is by REMOVAL.
- **A game contributes vocabulary, not widgets** — unless it owns the tab (CustomizeTab). The
  plugin seam gained: `configureFolder`, `firstRunText`, `reload(done, secondaryReady)`,
  `informationHtml`, `isIdTerm`/`idTermMatches`, `exportDefaults`, `unitScaleTip`, `layoutSpec`,
  `modelKinds`/`textureKinds`, `hotkeyDefs`, `decorateTab`, `nouns`, `selfTests`,
  `takeReloadRequest`.
- **Harness flags are core's**: `--check`, `--quit-when-ready`, `--harness`, `--show-tab <id>`;
  needles `shell: N tab(s) built`, `shell: reload started`, `shell: index ready`, `shell:
  secondary indexes ready`, `shell: ready`, `models: loaded`, `textures: loaded`, `bulk: run
  started`, `bulk: Done`, `export: …`, `self-test summary`.
- **Engine sources compile from where they live** (`POE2_SRC`) until phase 7; `games/poe2/` holds
  only what is new. The include root is the repo root, so POE2's same-named headers
  (`QueryTerm.h`, `ExportLayout.h`, `AppPaths.h`) never shadow core's.

## Deferred

- POE2's viewport-only material extensions (subsurface tint, FurV2 shells) are not drawn: they
  are `ViewportExtension` material (phase 6's mechanism) and would ride on `Material::engine`.
  The glTF output is unaffected (glTF has neither).
- The Customize tab's Animations panel plays the CLASS rig's clips on the outfit, as POE2 did; a
  shapeshift form is shown static, as POE2 did.
- `bulk_verify.cpp`, `render_offscreen`, `export_model` and the other POE2 `tools/` programs are
  not rebuilt against the core yet (they link POE2's viewport); phase 7 decides which survive.

## Still open

- The user-side half of the gate above.
- Phase 0 fixtures (still the family-wide gap: no suite here parses a real game file).
- Phases 5 (FOX on core), 6 (D4 on core), 7 (retire the old trees, one CLAUDE.md).
