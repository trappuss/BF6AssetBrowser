# ModelsTab gap list — D4's Models tab vs the core Models tab

Written 2026-09-23 from the source (read-only pass). Rule applied throughout (plan §8.9, PHASE6_REPORT
"Open" 3): the core viewport is a **pipeline, not a look** — anything that recreates D4's rendering
goes to D4's `ViewportExtension` (6c); generic UI/workflow goes into the **core tab**; D4 data
features become **D4 plugin panels** (through `AppPlugin::decorateTab`, `rowMenuAboutToShow`,
`partMenuAboutToShow`, which already exist and are unused by `games/d4`).

Paths, abbreviated below:

| Tag | File |
|---|---|
| `MT` | `d4/src/tabs/ModelsTab.cpp` (10,921 lines) |
| `MTx` | `d4/src/tabs/ModelsTab_Export.cpp` |
| `MTp` | `d4/src/tabs/ModelsTab_Panels.cpp` |
| `MW` | `d4/src/app/MainWindow.cpp` |
| `cMT` | `AssetBrowser/core/tabs/ModelsTab.cpp` (1,274 lines) |
| `cGL` | `AssetBrowser/core/gl/GLModelWidget.{h,cpp}` |
| `IGS` | `AssetBrowser/core/store/IGameStore.h` |
| `D4S` | `AssetBrowser/games/d4/D4Store.{h,cpp}` |

Sizes: **S** ≲ 1 day / a few hundred lines · **M** a few days · **L** a week or more.

## Summary

| Classification | Count | Meaning |
|---|---:|---|
| CORE | 53 | generic; belongs in `core/tabs/ModelsTab` (or core ui/gl helpers) |
| PLUGIN-D4 | 13 | D4 data or D4-only workflow; a `games/d4` panel, menu row or store method |
| VIEWPORT-EXT | 9 | D4's look; phase 6c `ViewportExtension` |
| DROP | 7 | dead, hidden, or dev-only, with evidence |
| **Total** | **82** | |

### Verified "already known" facts

| Claim | Verdict | Evidence |
|---|---|---|
| Lazy clips | **yes** | `cMT` `setClipIndex` 622-653 (`model::ensureClip` under `seh::runGuarded`) |
| Attachments, re-assembled per tick, store-placed | **yes** | `cMT` `applyAttachments` 690-752; `IGS` `placesAttachments`; `D4S` overrides it (h:85) and caches the actor scan (cpp:467-484). Flat list, not D4's tree (see C10) |
| Funnel with tag groups + flag-based "Special" group | **yes** | `cMT` 112, 191-196, 349-352; `core/ui/AssetListModel.cpp` 79-87 (New/Animated/Rigged/Orphaned/Encrypted/Unnamed/Shadowed). No **Unrenderable** facet, no negation (see A10) |
| Grid view with thumbnails | **yes** | `cMT` 135-151, `setGridView` 1220-1236, Ctrl+wheel 1249-1252. **Bug:** the textured-icons toggle does not take effect until re-index (A5) |
| Export .glb, per-part subset | **yes** | `cMT` `exportSelection` 834-880, `exportParts` 952-974. Hidden parts are still exported (D1) |
| Save preview image | **yes** | `cMT` `saveImage` 1010-1054 (single + batch, scale/transparent/crop) |
| GIF export | **yes** | `cMT` `exportGifs` 1059-1106: turntable + anim-loop, single + batch |
| Extract original files | **present, dead for D4** | `cMT` 978-1006 uses `dependencyFiles/readPath`; `D4S` implements neither (D7) |

### Top 10 for the template

Ranked by how much every game gets out of it, divided by cost.

1. **D1 · Export only what is visible.** S. This is a correctness gap. Today the core exports parts
   the user has hidden (`ModelLoad.cpp:179-180` only narrows by the selection). Every game is
   affected.
2. **F1–F4 · Crash learning** (load sentinel, render blocklist, thumbnail sentinel, learning which
   assets won't render). M in total. FOX and POE2 parse data nobody has fully mapped. The core
   survives a fault inside one session but forgets it, so the same asset crashes again after a
   restart.
3. **A1 · Outliner display mode** (the scene tree under the loaded model's row). L. It is the
   biggest structural gap. `core::Geometry::groups` already carries FOX's mesh-group tree and
   nothing draws it.
4. **B7 + B8 · Camera panel and load framing.** M. The core widget already has the whole API
   (`CameraPose`, `applyCameraPreset`, FOV, ortho, turntable), but the tab exposes none of it. The
   tab also resets the orbit on every load (`cMT:532`).
5. **C3 · Material inspector with channel tiles.** M. `core::Material` already holds base, normal,
   ORM and emissive images. Today the core only lists material names in INFO.
6. **C9 + C5 · Timeline transport and animation-list UX** (step, frame field, speed, loop,
   search, multi-select clip export). S each. Every rigged game needs these, and the core bar is a
   combo box and a 0..1000 slider.
7. **B9 + B11 · Lighting and Graphics panels (generic half).** M. The widget has every setter:
   `LightRig`, shadow/SSAO/IBL/grade/background. The defaults must come from
   `AppPlugin::viewFeatures()` (§8.9).
8. **C2 · Looks (material variants).** M. PHASE6 names it core. It needs a new `IGameStore`
   look API. FOX camo and POE2 variants fit the same shape (unconfirmed).
9. **Persistence bundle** (B1/B2/B3 view state, A3 header state, A9 "remember filters", A12/A13
   auto-load and last model). S each. The core persists only the grid toggle and icon size
   (`cMT:447-451`).
10. **E2 + E3 + C1 · Part-visibility workflow**: parity for the shared part menu, the
    H / Shift+H / Alt+H keys, and a clickable "Shown" column. S. Today the core table shows `●/○`
    as text that cannot be clicked.

Next after these: A2 group-by, D5 drag-out export, A4 game icons in the list, C6 clip grouping.

---

## A. List and browsing

**A1 · Display modes: List / Outliner / Grid, with an outliner scene tree** · CORE · L
- **What it does:** one header drop-down switches between a flat list, a tree and a grid. In
  Outliner mode the loaded model's row expands into a subtree:
  - Looks, with exclusive visibility toggles.
  - Animations · N, with one leaf per clip.
  - Armature, with bone, named and physics-bone counts, then one node per bone.
  - One node per part, each with a visibility toggle (the "eye") and an export toggle (a camera
    icon). Under each part: its material, a channel-tile strip, then Textures (with thumbnails
    and RGBA strips), Values and Shaders.

  Hovering a node highlights the matching parts in the viewport and the matching material rows.
  Double-clicking a node loads the model, applies a look, plays or stops a clip, or
  expands/collapses. "Outliner shows" toggles choose which node kinds appear, and "Auto-expand
  loaded model" is an option. Right-click menus: texture leaves and tiles get Copy/Save image; a
  single part gets the shared part menu; a group gets Solo/Show all/Invert; any other node gets
  Copy label.
- **D4:**
  - `applyDisplayMode` MT:6159
  - `buildOutlinerSubtree` MT:7684-7904
  - header drop-down MT:1773-1840
  - subtree menus MT:1235-1362
  - double-click MT:3261-3295
  - hover MT:3182-3188
  - lazy thumbnails `queueTextureIcons` MT:8172
  - `d4/src/tabs/ModelOutliner.{h,cpp}` (848 lines)
- **Core today:** flat `QTableView` + grid (`cMT` 121-151). `Geometry::groups` (FOX tree) is not
  drawn anywhere.
- **Needs:**
  - Existing: `Geometry::groups`, `Part::name/materialIndex`, `Skeleton`, `Material`.
  - New: a per-material "values/shaders" leaf provider for games that have them, as an optional
    `IGameStore::materialFacts(materialName)` returning label/value rows.
  - Bone labels, see B4.
- **Risk:**
  - The largest single item.
  - D4 keeps the flat model 1:1 at the top level so `entryAt(row)` stays valid. The core
    `AssetListModel` would need the same wrapper-proxy discipline.
  - The Grid restore once bricked D4 at start-up (F5).

**A2 · Group-by with header rows** · CORE · M
- **What it does:** clusters the list under headers by Collection, Appearance set, Set (M+F),
  Class, Type or Category.
- **D4:** `applyGrouping` MT:5257-5307; combo MT:996-1004 (lives in the display menu, 1837).
  `SnoListModel::setGroupKey` (index/SnoListModel.cpp:215).
- **Core today:** none.
- **Needs:**
  - Core grouping engine in `AssetListModel`, with generic keys: collection, kind, any tag group.
  - New `IGameStore::groupKeys(kind)` and `groupOf(id, key)` for game keys (D4: set M+F,
    appearance set, class-from-prefix).
- **Risk:** headers interleaved with rows break `idAt(row)`. D4 handles it with -1 display rows.

**A3 · Column show/hide, id column, persisted header state, persisted sort** · CORE · S
- **What it does:**
  - Right-click a column header to show or hide columns (plus a Columns button).
  - Width, order and sort are saved.
  - A one-time migration moves the SNO column next to FILENAME.
  - A Sort combo (Name/SNO/Collection) is saved as well.
- **D4:** MT:1199-1226, `showColumnMenu` MT:3723; sort combo MT:1605-1617.
- **Core today:** sortable columns Name / In-game / Kind / Size / Collection (`AssetListModel.h`
  Column enum). There is no id column, and nothing about the header is persisted (`cMT` 121-133).
- **Needs:** an `ColId` column showing `AppPlugin::nouns().id`, and `QHeaderView::saveState` under
  `models/listHeader`.
- **Risk:** low. Restore the saved state *before* forcing the stretch column (D4 notes the order
  at MT:1206-1209).

**A4 · Icon column in the list; icon style Original / 3D / both; Ctrl+scroll icon size** · CORE · M
- **What it does:**
  - An Icon column in the list/outliner.
  - Style is either the game's own 2D inventory icon, the rendered 3D thumbnail, or the original
    falling back to 3D.
  - Ctrl+wheel resizes list icons from 24 to 96 px.
  - Optional auto-render of visible rows, throttled to 3 per tick and off by default because of
    driver TDRs.
- **D4:**
  - mode combo MT:1021-1034
  - `originalIcon` MT:3652
  - `listIconPixmap` MT:3673
  - `updateIconMode` MT:3691
  - `setListIconSize` MT:3709
  - `scheduleVisibleIconRender`/`renderVisibleIcons` MT:3952-4019
  - batch `renderIcons` MT:8812
- **Core today:** icons exist only in the grid (3D thumbnails from `ThumbnailRenderer` on its own
  GL thread). `IGS::gameIcon` exists, but `cMT` never calls it and `D4S` does not implement it.
- **Needs:**
  - Core: `IGameStore::gameIcon` (exists).
  - PLUGIN work: `D4S::gameIcon` over `IconIndex` + `AppearanceMeta::iconFor`. This is the 6b
    "icons" item.
- **Risk:** the core renders thumbnails off the viewport, so it is safer than D4, which swapped
  geometry into the live widget (MT:8823-8833). Keep the "never auto-render a blocklisted id"
  rule (F2).

**A5 · Icon render settings, cache variant signature, and the textured-toggle bug** · CORE · S
- **What it does:**
  - Render size 48-256, crop to silhouette, alpha cut-out on/off, base-colour vs clay.
  - Separate on-disk folders per style.
  - A `.variant` signature file wipes stale PNGs when size, crop or alpha change.
- **D4:** `renderIconImage` MT:9761-9802; `ensureThumbCacheVariant` MT:9899-9920;
  `thumbCacheDir` MT:513-533; `onSettingsChanged` MT:8612-8630.
- **Core today:** only a "Textured (base-colour) icons" toggle (`cMT` 169-176). The disk folder is
  `thumbs_v<N>` and does not change with style (`cMT:159`).
- **Bug found (PLAUSIBLE; reasoned from the code, not run):** the thumbnail loader captures
  `textured = m_texturedIcons` **by value** in `onIndexReady` (`cMT:363`). The toggle only calls
  `m_thumbs->reset()` (`cMT:174`), and `reset()` clears memory, not disk
  (`ThumbnailRenderer.cpp:400-412`). Result: flipping the toggle changes nothing until the next
  re-index, and even after re-indexing, rows already cached on disk keep the old style.
- **Needs:** nothing from the store.
- **Risk:** low. Put the style in the disk-dir name, and re-install the loader on toggle.

**A6 · Icon image actions: copy / save to last folder / save as (multi-row)** · CORE · S
- **What it does:** copies a row's icon to the clipboard, or saves the icons of N rows as PNGs.
- **D4:** `addRowImageActions` MT:10147-10166; `copyIconImage` MT:9098; `saveIconImages` MT:9104.
- **Core today:** none. "Save preview image" re-renders the viewport, which is a different thing.
- **Needs:** nothing new.
- **Risk:** low.

**A7 · Hover preview on list rows, with configurable info lines** · CORE · M
- **What it does:** after a dwell delay, a popup shows the icon (only when the cursor is over the
  Icon column) plus coloured info lines, each toggle-able per setting:
  - title, id·file, collection, tags, counts, variants
  - rarity, introduced-in, "new this update", animated

  Wheel resizes the popup. It also works in List mode (any column).
- **D4:** `showIconPreview` MT:3818-3909; list hover MT:9625-9693; `HoverInfo` keys `mdl/*`.
- **Core today:** grid only, showing image + name + path + size (`cMT` 1254-1269). The table has
  no hover.
- **Needs:**
  - Generic lines come from `AssetRow` (inGameName, collection, tags, flags).
  - Game lines need a new `IGameStore::hoverLines(id)`. D4's rarity/introduced come from
    `ItemHoverIndex`, which is plugin data.
- **Risk:** low.

**A8 · Search extras: `c:` field prefix, per-component chips, colour-coded query** · CORE · S
- **What it does:** `c:text` searches only the collection, and reads to the end of the line. Each
  parsed component (id / name / `c:`) becomes a removable chip. The text is tinted when the whole
  query is a single kind.
- **D4:** MT:1685-1732; chip rebuild MT:3590-3610.
- **Core today:**
  - Has: space-AND, `-`, quotes, `a|b`, `#tag`, the id term via `AppPlugin::isIdTerm`, history
    (`searchbox::attach`, `cMT:109`), and Ctrl+F (`AppShell::focusCurrentSearch`).
  - Missing: field prefixes; the query chips (`core/ui/FilterChips` exists but no tab uses it).
- **Needs:** nothing from the store.
- **Risk:** low. Keep it in `SearchQuery` so Bulk matches the Models tab.

**A9 · Funnel extras: tag search, Clear, data-toggle row, opt-in "Remember filters" (search text included)** · CORE · S
- **What it does:** a search box inside the tag popup, a Clear button, and linked checkboxes for
  the data filters. "Remember filters" restores search text + tags + match mode + filters at
  launch.
- **D4:** tag panel MT:1496-1676; `saveFilterState`/`restoreFilterState` MT:6086-6134.
- **Core today:** the funnel persists facets always (`cMT` 191-196) and has no inner search or
  Clear. Search text is not restored (history only).
- **Needs:** none.
- **Risk:** low.

**A10 · "Only decrypted" and "Hide un-renderable" facets (negation + Unrenderable)** · CORE · S
- **What it does:** "Only decrypted" hides assets whose key isn't held, which is the negation of
  Encrypted. "Hide un-renderable" hides blocklisted and no-geometry rows.
- **D4:** MT:914-940 and 4074-4079 (decrypted); MT:1058-1068 and 4273-4275 (un-renderable).
- **Core today:** `flag:encrypted` shows *only* encrypted rows, and there is no way to exclude
  them. `AssetRow::Unrenderable` exists (`types/Asset.h:48`), but it has no facet
  (`AssetListModel.cpp:79-87`) and nothing ever sets it.
- **Needs:** F4 (learning) sets Unrenderable. No store change.
- **Risk:** low. Add a generic "exclude" state to facets rather than one-off checkboxes.

**A11 · Filtered-count styling, "indexing…" count, active-filter badge** · CORE · S
- **What it does:** "X of Y" in accent colour when filtered. The count reads "indexing
  animations…" while a facet's index is still building, instead of "0 of 67,144".
- **D4:** `updateCount` MT:3501-3542.
- **Core today:** a plain "X of Y models" (`cMT:457`).
- **Needs:** a "secondary index pending" signal. The core already has `onSecondaryReady`.
- **Risk:** low.

**A12 · Auto-load toggle (browse without loading; double-click loads)** · CORE · S
- **What it does:** selection only fills metadata; double-click, or a force-load, loads the model.
  The viewport explains why nothing is loading.
- **D4:** MT:1858-1873, 5621-5639, `forceLoadSno` MT:5502.
- **Core today:** every settled selection loads (`cMT` 186-189). There is no toggle.
- **Needs:** none.
- **Risk:** low.

**A13 · Remember last-viewed model (reselect at launch, never auto-load)** · CORE · S
- **D4:** MT:4152-4170, 5539-5540; opt-in `models/rememberLast`.
- **Core today:** none.
- **Needs:** none.
- **Risk:** the restore must not load, so a crashing asset can't brick startup (D4's comment at
  MT:4159-4161).

**A14 · Variants jump (skin siblings) in the row menu and as INFO links** · CORE · S
- **D4:** `addRowExportCopyActions` MT:10250-10260; INFO links MT:4724-4739; hidden button
  MT:1087-1091.
- **Core today:** none.
- **Needs:** `IGameStore::variants(id)` (exists). `D4S` does not implement it (PLUGIN work, from
  the `apprVariantSnos` entity blob).
- **Risk:** low.

**A15 · Presence badge (✓ has mesh / ✗ icon-only) and dim + ⚠ on failed rows** · CORE · S
- **D4:** `updateIconMode` MT:3695-3706; `IconBadge`.
- **Core today:** `AssetListModel` dims Unrenderable, but nothing sets the flag. No badge.
- **Needs:** F4.
- **Risk:** low.

**A16 · Browse-by-item dialog** · DROP
- **D4:** `openItemBrowser` MT:4758-4797.
- **Evidence:** no callers. The only reference is the comment at MT:1086 ("removed with the View
  row — remains for a future home").
- **Note:** if it is revived, it is a PLUGIN-D4 panel over `EntityBlob::itemAppr`.

**A17 · Class / Gender / Type / Category drop-downs, the "Filters ▾" section and its Clear button** · DROP
- **D4:** combos MT:954-993, `filterSection` MT:1093-1105 (`setVisible(false)` at 1105, never
  shown), `m_filtersToggle->hide()` MT:1132.
- **Evidence:** the section is permanently hidden. The combos survive only as state holders for
  the funnel's Special group and for Bulk's `FilterSpec`. The core funnel already shows
  Class/Gender/Type/Category as tag groups (`D4S::tagGroups` cpp:241-258).

**A18 · "Multi select" checkbox** · DROP
- **D4:** MT:1056, linked copy in the funnel MT:1619.
- **Evidence:** no `toggled` connection exists anywhere, and selection is forced to
  `ExtendedSelection` unconditionally (MT:3078-3079). The box does nothing.

## B. Viewport controls

**B1 · Shading mode persisted; "Rendered" couples the post features** · CORE · S (partial today)
- **D4:** shading balls MT:1966-2009, restore MT:2267-2274.
- **Core today:** a shading combo (`cMT` 212-214, 290-291), not persisted.
- **Needs:** none.
- **Risk:** §8.9. "Rendered" must turn on only what `viewFeatures()` offers, never D4's set for
  every game.

**B2 · Channel viewer persisted; wheel over the button cycles channels, with a toast** · CORE · S (partial)
- **D4:** MT:1941-1957, 2013-2046, eventFilter MT:9475-9490.
- **Core today:** a channel combo (`cMT` 215-217, 292), not persisted, no wheel.
- **Needs:** none.
- **Risk:** low.

**B3 · Overlays panel: axis gizmo, coloured grid axes, bone names, statistics overlay; every box persisted** · CORE · S
- **What it does:**
  - A sticky popup of guide toggles behind the master gate. The gate itself is already core.
  - A Statistics overlay with live verts/tris/parts/bones for the *visible* parts.
- **D4:** MT:2049-2191, `reapplyOverlays` MT:7667, `updateStatsOverlay` MT:7908, label MT:2418.
- **Core today:**
  - Checkboxes for Overlays/Grid/Skeleton/Hardpoints (`cMT` 218-226, 293-296), not persisted.
  - The widget already has `setShowAxisGizmo`, `setGridAxisColors` and `setShowBoneNames`
    (`cGL.h` 149-152), but the tab doesn't expose them.
  - No stats overlay.
- **Needs:** none.
- **Risk:** low. Keep D4's rule that every replay goes through the gate (CLAUDE.md "Overlay
  master gate").

**B4 · Translated bone names (+ "hide unnamed bones")** · PLUGIN-D4 · S
- **What it does:** labels bones from D4's verified hash dictionary instead of `bone_<hash>`.
  Identified bones draw green.
- **D4:** MT:2149-2157; `GLModelWidget::translateBoneName` (d4 gl/GLModelWidget.h:431).
- **Core today:** bone names show `Joint::name` only.
- **Needs:** a new hook, either `IGameStore::boneDisplayName(nameHash)` or the store filling
  readable names at `loadSkeleton` time. The same hook serves outliner bone nodes (A1) and export
  `readableBoneNames`.
- **Risk:** low.

**B5 · Cloth overlays: collision model, physics bones, per-bone axes** · VIEWPORT-EXT · S
- **D4:** MT:2138-2148; `applyModelRig` MT:10307.
- **Core today:** none. This depends on the cloth solver (6c).
- **Needs:** the extension contributes overlay toggles. A new `ViewportExtension::overlayToggles()`
  list would let the core Overlays panel show them.

**B6 · Temporary skeleton when an armature or bone node is selected** · CORE · S
- **D4:** MT:3158-3179. It turns itself off again only if it was the one that turned the skeleton
  on.
- **Core today:** none. Depends on A1.
- **Risk:** low.

**B7 · Camera panel** · CORE · M
- **What it does:**
  - FOV slider.
  - ¾ / Front / Back / Left / Right angle presets.
  - "Frame full model".
  - Turntable toggle + speed.
  - Numeric yaw/pitch rows, where editing unticks the turntable.
  - Orthographic.
  - Three saved camera slots (angle, zoom, FOV, projection).
  - Everything persisted.
- **D4:** `buildCameraPanel` MTp:77-246; `d4/src/util/CameraOrbitRow.h`.
- **Core today:** nothing in the tab. The widget has all of it: `CameraPose` + `toString/fromString`,
  `applyCameraPreset` with builtin presets, `setFov`, `setOrthographic`, `setAutoSpin`/`setSpinSpeed`
  (`cGL.h` 199-229).
- **Needs:** none.
- **Risk:** low.

**B8 · Load framing: keep the orbit across loads, auto-frame option, frame-part-on-select option, middle-click re-frames** · CORE · S
- **D4:** `applyLoadedGeometry` MT:7586-7596 (`keepView=true`, `frameAll(keepRotation=true)`
  gated on `models/autoFrame`); options MTp:141-162; middle-click / `framePartOnPick` (d4
  GLModelWidget.cpp:5819, 5862).
- **Core today:**
  - `frameAll(false, false)` on every load resets the rotation (`cMT:532`).
  - Double-click always frames (`cMT` 319-322).
  - Middle-click calls `resetView()` (`cGL.cpp:1978`), which resets rather than re-frames.
- **Needs:** none.
- **Risk:** low.

**B9 · Lighting panel (generic half)** · CORE · M
- **What it does:**
  - Rig preset.
  - Key / rim / fill / ambient / exposure.
  - Key azimuth and elevation.
  - Reflections on/off and strength.
  - Lock lights to world.
  - Emissive scale.
  - Shadow strength, softness, bias, normal-bias, range, resolution.
  - SSAO strength and radius.
  - Colour grade (contrast, saturation, split-tone).
  - Default / Save preset / Restore preset.
- **D4:** `buildLightingPanel` MTp:260-400; `applyLightRig` MTp:401-434.
- **Core today:** nothing in the tab. The widget has every setter: `setLightRig`, `setLightLock`,
  `setExposure`, `setColorGrade`, `setShadowParams/Extra`, `setSsaoParams`,
  `setReflectionEnabled/Strength`, `setEmissiveScale` (`cGL.h` 157-193).
- **Needs:** defaults per game come from `AppPlugin::viewFeatures()`.
- **Risk:**
  - The preset labels say "D4 Wardrobe (campfire)" etc. The core should name them neutrally
    (the widget comment already does: warm / neutral / cool).
  - Keys should be per product.

**B10 · Lighting "Surface" sliders: subsurface %, skin warmth, wetness, snow** · VIEWPORT-EXT · S
- **D4:** MTp:356-360, applied MTp:415-418 (`setSkinWarmth`, `setSssStrength`, `setWetness`,
  `setSnow`).
- **Core today:** none. Removed from the widget in phase 3 (`cGL.h` header comment).
- **Needs:** extension-provided panel rows. The same `ViewportExtension` settings-rows hook as B5.

**B11 · Graphics panel (generic half) + backdrop** · CORE · S
- **What it does:**
  - Toggles: IBL, self-shadows, SSAO, tonemap, specular AA, show back faces.
  - Backdrop: preset chips (Dark / Charcoal / Grey / Light), a custom colour, and a gradient.
- **D4:** `buildPreviewPanel` MT:10327-10443 (generic rows 10364-10372, 10381-10386; backdrop
  10393-10442); restore MT:2335-2340.
- **Core today:** none in the tab. The widget has `setFeatureIbl/Tonemap/SpecAA`,
  `setShadowEnabled`, `setSsaoEnabled`, `setBackfaceCull`, `setBackgroundColor/Gradient`.
- **Needs:** `viewFeatures()` for the defaults.
- **Risk:** per §8.9, a user override is fine as UI, but the *default* must remain the game's.

**B12 · Graphics D4 shading toggles: detail maps, subsurface/translucency, hair anisotropy, primary mask** · VIEWPORT-EXT · S
- **D4:** MT:10374-10388; restore MT:2351-2362.
- **Core today:** `setFeatureSubsurface` exists and is forwarded to the extension. The others left
  the widget in phase 3.
- **Needs:** extension settings rows.

**B13 · Shaders panel: shell fur (length, density, shells, gravity, comb, coverage), mesh FX (brightness, scroll, wobble)** · VIEWPORT-EXT · M
- **D4:** `buildShaderPanel` MTp:446-548; restore MT:2387-2396.
- **Core today:** none.

**B14 · Detail-maps discovery panel** · VIEWPORT-EXT · M
- **What it does:** Auto vs manual; dye-zone → detail map; metal routing; thresholds; copy
  config.
- **D4:** MTp:550-720.
- **Core today:** none.
- **Note:** it is a dev tool. It could stay dev-gated inside the extension.

**B15 · Cloth physics panel + the "cloth sim" setting (off by default in Models)** · VIEWPORT-EXT · M
- **D4:** `buildPhysicsPanel` MTp:731-923, `applyClothParams` MTp:924; `setClothEnabled`
  MT:2332-2333, 8664-8665.
- **Core today:** none.

**B16 · Pigment (dye) panel** · VIEWPORT-EXT · M
- **What it does:**
  - Dye recolour toggle.
  - A combo of real player dyes (d4data `Dye/*.dye.json`), plus custom saved pigments.
  - Four zone slots driven by an HSV wheel, hex entry and drag-drop.
  - Eight memory swatches.
  - A DyeMask debug dump.
- **D4:** `buildDyePanel` MT:10456-10618, `loadPlayerDyes` MT:421, slot DnD eventFilter
  MT:9316-9398, `dumpDyeDebug` MT:10726.
- **Core today:** none.
- **Split:** the dye list is D4 data, so the panel is PLUGIN-hosted. The recolour itself lives in
  the extension. Export dye bake: see D11.

**B17 · Fill skin (black `…skin_mat` placeholders render the class body skin)** · VIEWPORT-EXT · S
- **D4:** `bodySkinMaterial` MT:8241, `applyPartMaterials` MT:8427-8443, 8596-8606.
- **Core today:** none. The core viewport uses the export resolver (PHASE6 "One material
  resolver").
- **Note:** it is material substitution, which is D4's look. Could be an extension or D4Store
  option.

**B18 · "Base colour only" performance mode (decode 1 map instead of ~10)** · CORE · S
- **D4:** MT:8448-8463, 8631-8640.
- **Core today:** `resolveMaterials(..., decodeImages)` is all or nothing.
- **Needs:** a new decode granularity on `resolveMaterials`, e.g. `DecodeBaseOnly`.
- **Risk:** low value on modern GPUs. Keep for low-end machines.

**B19 · Part-category toggles (D4: FX / SIM / GIB) that bulk-hide flagged parts** · CORE · M
- **What it does:** toolbar toggles, persisted, that tick or untick every part of a category in
  the visibility source of truth. Parts added by attachments are classified the same way, and
  attachment FX default to hidden.
- **D4:** MT:2193-2201; classification MT:5779-5787, 5831-5845, 6465-6481, `matIsFxByShader`
  MT:363; `setFlaggedPartsChecked` MT:6827.
- **Core today:** none. `core::Part` has no category.
- **Needs:** a new `Part::categories` (or tags) filled by the store. The core shows one toggle per
  category that any part carries (like the Special facet). D4's name/shader heuristics move into
  `D4S`.
- **Risk:** medium. D4's heuristics are name-based. Keep them in the plugin.

**B20 · Maximise viewport in place (F, ⛶ button, Esc, floating "Exit")** · CORE · S
- **D4:** `toggleFullscreen` MTp:995-1056; F shortcut MT:2259-2263; strip button MT:2317-2324.
- **Core today:** none. NPanel can collapse only the side column.
- **Risk:** low. Scope `F` to the viewport widget so typing "f" in search boxes still works (D4
  comment MT:2257-2258).

**B21 · GIF export warning when cloth physics is live** · VIEWPORT-EXT · S
- **D4:** `confirmGifPhysics` MW:831-880.
- **Core today:** the GIFs exist (`cMT` 1059-1106) with no warning.
- **Needs:** `ViewportExtension` reports a non-deterministic simulation, or makes capture mode
  deterministic. The core already forwards `setCaptureMode`.

## C. Panels

**C1 · PARTS panel parity** · CORE · S
- **What it does:**
  - A visibility checkbox per row.
  - Numeric sorting on tris.
  - A slot column.
  - A "PARTS · N of M shown" header.
  - The selection survives a rebuild.
  - Viewport click selects with Ctrl/Shift add or toggle.
  - Right-click raises the shared part menu.
- **D4:** MT:2919-2962, `fillPartsPage` MT:7954-8003, viewport click MT:3228-3252.
- **Core today:**
  - A 4-column table where "Shown" is non-clickable `●/○` text (`cMT` 253-260, 544-562).
  - Row selection is mirrored to the widget ✓.
  - Right-click menu ✓.
  - Items are text, so tri sorting is lexical.
- **Needs:** the slot label needs a store name for `Part::slotHash`, as a new
  `IGameStore::slotName(hash)`. Or drop the column for games without slots.
- **Risk:** low.

**C2 · LOOKS (material variants / SOA)** · CORE · M
- **What it does:**
  - A table of looks. Selecting one re-textures the viewport.
  - An empty slot hides that material's parts.
  - The outliner shows exclusive look toggles.
  - Hovering a look highlights its material rows.
  - Export uses the active look.
- **D4:** MT:2718-2725, 3121-3148, `applyLook` MT:8561-8581, `highlightMaterialsForLook` MT:6879;
  export uses the active roster MTx:659-662.
- **Core today:** none.
- **Needs:**
  - New `IGameStore::looks(id)` → names.
  - `resolveMaterials(id, geo, decode, look)`, or `materialsForLook(...)` that returns empty to
    mean "hidden".
  - `model::exportOne` has to take a look index.
- **Risk:** medium. D4's own SOA roster is JSON-only; the meta-binary fallback has "no look table"
  (MT:5830). PHASE6 lists "looks (SOA)" as core.

**C3 · Material inspector (generic)** · CORE · M
- **What it does:**
  - For the selected part or material: name, factors, alpha mode, and each texture slot with a
    thumbnail.
  - A six-tile channel strip (colour / rough / metal / normal / alpha / emissive, or R/G/B/A
    of one texture), with hover zoom, copy, save to last, and save as.
  - A facts line for the previewed texture: size, mips, format.
- **D4:**
  - tiles MT:2801-2871
  - `showMaterialChannels` MT:10879
  - `previewTexture` MT:9211-9256
  - `saveTileImage` MT:9153
  - tile menus MT:2839-2850
  - facts MT:9225-9248
- **Core today:** INFO lists material names only (`cMT` 578-581).
- **Needs:** `core::Material` already carries decoded `TextureImage`s. Facts need `TextureInfo`
  per material texture (exists on `loadTexture`), or a `TextureImage` source id.
- **Risk:** decode under `runGuarded` and name the texture that faulted (F7).

**C4 · D4 raw material tables** · PLUGIN-D4 · M
- **What it does:**
  - App Materials (hash, name, SOA, flags, material / cloth / override SNOs).
  - SubObject Apps (the LOD0 draw calls).
  - Materials by SNO.
  - Vertex Buffers (stride and semantics).
  - Values (runtime scalars and vectors) and Shaders (ShaderMap refs).
  - A "⚠ FALLBACK — no base texture, renders black" row.
- **D4:** MT:2727-2797, `showAppearance` MT:5718-5863, `showMaterialTextures` MT:6903-7072, VB
  table MT:7603-7627.
- **Core today:** none.
- **Needs:** `decorateTab` + `NPanel::addPanel` (exist).
- **Risk:** it reads d4data JSON and CASC. Keep it in `games/d4`.

**C5 · ANIMATIONS list UX** · CORE · S
- **What it does:**
  - A search box.
  - Multi-select (Ctrl/Shift) to export chosen clips.
  - Click the playing clip again to stop.
  - Esc stops and clears.
  - Arrow keys preview clips as you move.
  - Right-click: Play, Export selected (prompt / last folder), Copy file name.
  - The header reads "N (M would export)".
- **D4:** MT:2552-2633, 3326-3336, 9434-9451, header MT:5216-5229.
- **Core today:** a single-select list; clicking a row plays it (`cMT` 329-334).
- **Needs:** none.
- **Risk:** low.

**C6 · Clip grouping and labels** · CORE · M (mechanism; the data is PLUGIN)
- **What it does:**
  - Clips sorted under coloured group headers (D4 AnimSets) with a stable palette.
  - A readable action label on each row (from snoPower).
  - Provenance tooltips.
  - "Play whole set" as a queue (loops with Loop).
  - "Select all in set" and "Export set".
  - A female-variant preview toggle.
- **D4:** `populateAnimList` MT:5097-5231, palette MT:5054-5095, `prettyPower` MT:4975,
  `playAnimationSet` MT:7391, queue in `tickAnimation` MT:7421-7436, menu MT:2610-2627.
- **Core today:** none. `core::Clip` has only a name.
- **Needs:** new `Clip` fields `group`, `label`, `variantOf` (and `source`, D4). `D4S::loadSkeleton`
  fills them from `ModelAnimIndex`.
- **Risk:** low once the fields exist.

**C7 · Pull animations from another model / "Pull suggested" / "Clear pulls"** · CORE · M (uncertain)
- **What it does:** lists another model's clips on this skeleton (retargeted by bone hash). Rows
  are marked gold, and "suggested" picks the base rig automatically.
- **D4:** `pullAnimsFromModel` MT:4902, `pullSuggestedAnims` MT:4878, `suggestedAnimSource`
  MT:4839, buttons MT:2523-2546.
- **Core today:** none.
- **Needs:**
  - New `IGameStore::clipsFor(sourceId, targetSkeleton)`, or core retargeting by joint name/hash.
  - "Suggested" needs `IGameStore::suggestedClipSource(id)`.
- **Risk:** generic only if FOX and POE2 share skeletons across assets in the same way. That is
  unverified here, so it is marked uncertain. D4's rig-family logic stays in the store.

**C8 · Skeleton-compatible fallback clips (muted cyan "unconfirmed")** · PLUGIN-D4 · S
- **D4:** `modelAnimRows` MT:5018-5033, colouring MT:5182-5185.
- **Core today:** none.
- **Needs:** `Clip::source = Guess` (with C6). The listing logic belongs in `D4S`.

**C9 · Timeline transport** · CORE · S
- **What it does:**
  - Step back and step forward.
  - An editable frame field "/ N".
  - Frame ticks.
  - Speed 0.25x–2x.
  - A Loop toggle.
  - Play at the end restarts.
  - Wheel steps one frame; Shift+wheel changes speed (with a toast).
- **D4:** MT:2437-2491, 3301-3324, `applyAnimSpeed` MT:7401, `tickAnimation` MT:7414, wheel
  MT:9460-9473.
- **Core today:** clip combo + play/pause + a 0..1000 slider + a time label; always loops, fixed
  16 ms step (`cMT` 241-250, 276-287).
- **Needs:** none. The widget has `setFrame/frameCount/clipFps`.
- **Risk:** low.

**C10 · ATTACHMENTS panel parity** · CORE · S (partial today)
- **What it does:**
  - A tree grouped actor → trigger → child, with full tooltips (child actor, appearance,
    hardpoint, trigger).
  - Double-click toggles.
  - The scan is deferred until the panel is open.
  - Attachment FX parts are hidden by default.
- **D4:** `buildAttachPage` MT:6258, `populateAttachTree` MT:6341-6388, `scanAttachments`
  MT:6297-6339, FX default MT:6517-6529.
- **Core today:** a flat list, "group › label · on bone (why)", with ticks (`cMT` 732-748). The
  clip is kept across rebuilds ✓.
- **Needs:** `Attachment::group` exists. Tooltip fields would need a new `Attachment::detail`.
- **Risk:** low.

**C11 · INFO page (generic rows)** · CORE · S
- **What it does:**
  - In-game title, collection, tags, file size.
  - Bounds.
  - Textures count, animations count.
  - Used-by, as a summary plus the full list in the tooltip.
  - Variants as jump links.
  - Values clip rather than widen the column.
- **D4:** MT:2873-2917, `setInfo` MT:4173, `showAppearance` MT:5587-5700, `updateEntityInfo`
  MT:4571-4740.
- **Core today:** name, path, id, format note, counts, bones/clips, hardpoints, materials
  (`cMT` 564-583).
- **Needs:** `AssetRow` fields (exist); `IGameStore::usedBy/uses/variants` (exist, `D4S`
  implements none of them); bounds from `Geometry::bboxMin/Max` (exist).
- **Risk:** low.

**C12 · INFO page (D4 rows)** · PLUGIN-D4 · M
- **What it does:**
  - Family; Actor (anim tree, scale, death FX); Physics (wind, ragdoll).
  - Items; Item facts (type, required level, transmog, seasonal, classes, default dyes).
  - **Sold in**, as links that open the bundle in the Catalogue (`revealBundleRequested`).
  - Appearance Sets; LOD ladder.
- **D4:** MT:4582-4723, 5596-5598, 5680-5694; Catalogue link MT:2987-2992.
- **Core today:** none.
- **Needs:** a new `IGameStore::infoRows(id)` → {label, text, optional link target}, or a
  plugin-appended NPanel section via `decorateTab`. Cross-tab reveal needs a shell hook, e.g.
  `AppShell::reveal(stableTabId, id)`.
- **Risk:** it reads d4data JSON per selection. Keep it lazy.

**C13 · CLOTH page (authored NvCloth tuning: GAME vs LIVE, differences in gold)** · PLUGIN-D4 · S
- **D4:** `fillClothPage` MT:8009-8128.
- **Core today:** none.
- **Needs:** the GAME column is D4 data. The LIVE column needs the 6c cloth solver's params.

**C14 · Breadcrumb (model › part › material, each segment a link) and the "No model loaded" veil** · CORE · S
- **D4:** MT:2653-2669, `updateBreadcrumb` MT:8132-8166, veil MT:2689-2692.
- **Core today:** none. `setOverlayText` covers only the viewport.
- **Needs:** A1.
- **Risk:** low.

## D. Export

**D1 · Export "what you see" (viewport-hidden parts excluded) + a per-part export toggle** · CORE · S
- **What it does:**
  - With no explicit subset, a part is exported only if it is visible **and** its outliner export
    toggle (the camera icon) is on.
  - An explicit part-menu export ignores both filters.
  - "Every part hidden" is reported, not written as an empty file.
- **D4:** `exportCurrentModelGlb` MTx:601-623; batch current-model path MTx:997-1005; `ExportRole`
  (d4 ModelOutliner.h:38, 56, 77).
- **Core today:**
  - `model::exportOne` narrows only by the *selected* parts (`ModelLoad.cpp:179-180`).
  - Parts hidden with Hide/Isolate are still written (`cMT:860-861` passes `currentPartSubset()`,
    i.e. the selection, not visibility).
  - This is a correctness gap for every game.
- **Needs:** none from the store. Pass the widget's visibility mask.
- **Risk:** low.

**D2 · Multi-model export from the Models tab: name template + folder layout + run cache + parallel** · CORE · S
- **What it does:** batch exports use the export name template and the Settings folder layout
  (Flat / Class / Type / per-model). They also share one decode cache and optionally run in
  parallel.
- **D4:** `exportModels` MTx:829-1202 (layout 943-960, `NameTemplate::model` 989, cache 853,
  parallel 1108-1143).
- **Core today:**
  - `exportSelection` batch uses the bare file stem into one folder, serially, with no
    `beginRunCache` (`cMT` 866-879).
  - The core **Bulk** tab already has layout, templates and workers (`BulkTab.cpp` 82-256).
- **Needs:** `AppPlugin::layoutSpec`, `IGameStore::beginRunCache` (both exist).
- **Risk:** low. Reuse `BulkExtractor`.

**D3 · Animation export controls** · CORE · S
- **What it does:**
  - Export only the clips selected in the list.
  - "To last folder" with its own remembered folder.
  - A count-aware menu label ("— 378 clips…").
  - Clip filters: a max-frames cap and name-exclude patterns, applied to data-derived clips only,
    never to hand-picked ones.
- **D4:** `exportAnimationsOnly` MTx:1302-1354, `animExportLabel` MTx:1425-1435,
  `plannedExportAnimNames` MT:7223-7286, `AnimClipFilter` (d4 util/AnimClipFilter.h).
- **Core today:** "Export animations only" writes every clip after a Save-As (`cMT` 935-950), with
  a static label (930-933). `ExportOptions::onlyClip` can hold a single clip.
- **Needs:** `ExportOptions` needs a clip *subset* (a vector), not just `onlyClip`.
- **Risk:** low.

**D4 · Animation export scope sources (previewed / original / sets / base / pulled)** · PLUGIN-D4 · S
- **D4:** `AnimExportScope` (d4 util/AnimExportScope.h); MT:7248-7281; batch MTx:890-900,
  1039-1055.
- **Core today:** all clips the store lists.
- **Needs:** `Clip::source` (C6). Then the scope filter could become core; today the sources are
  D4 concepts.

**D5 · Drag-out export (selected rows → real .glb files dropped into Explorer or Blender)** · CORE · M
- **What it does:** dragging an already-selected row exports up to 10 models to a temp folder
  and hands the OS their file URLs. It never repoints the "last export folder".
- **D4:** `startModelDrag` MTx:1360-1398; press/move handling MT:9599-9624.
- **Core today:** none. There is no `QDrag` anywhere in core.
- **Needs:** none.
- **Risk:** the export is synchronous under a wait cursor, and D4 caps it at 10 for a reason.
  Pressing on an unselected row must still rubber-band select.

**D6 · Dependencies dialog (model → materials → textures, sizes, Copy / Copy all)** · CORE · S
- **D4:** `showDependencies` MTx:1209-1299; row menu MT:10267-10269.
- **Core today:** none. "Extract originals" uses `dependencyFiles` silently.
- **Needs:** `IGameStore::uses(id)` / `dependencyFiles` + sizes (exist; `D4S` implements neither).
- **Risk:** low.

**D7 · Raw source files for D4 (`.app` + every `.tex` into a shared `deps/`)** · PLUGIN-D4 · S
- **D4:** `exportModelDeps` MTx:126-173; `export/withDeps` MTx:692-694, 1089-1090.
- **Core today:** `extractOriginals` (`cMT` 978-1006) is the generic mechanism. For D4 it only
  writes the row name, because `D4S` has no `dependencyFiles`/`readPath`.
- **Needs:** implement `D4S::dependencyFiles` and `readPath` (existing virtuals).
- **Risk:** D4 has no game paths. `readPath` needs a synthetic `"<sno>.tex"` addressing scheme.

**D8 · Loose textures: the composited four, and every raw material map with a `textures.txt` manifest** · CORE · M
- **What it does:** writes PNGs beside the .glb, in two independent scopes:
  - `textures/`: the maps glTF uses.
  - `textures/material_maps/`: every bound texture as the game stores it (detail maps, masks,
    dye, translucency), with slot, role, id and UV scale in a manifest.

  Identical textures are copied, not re-encoded.
- **D4:** `writeLooseTextures` MTx:175-329.
- **Core today:** `ExportOptions::looseTextures` covers sibling PNGs for `.gltf` output only.
- **Needs:** a new `IGameStore::rawTextures(materialName)` → {role, slot, id, uScale, vScale},
  plus `loadTexture`.
- **Risk:** medium. The "all maps" half is only meaningful for games with more slots than glTF.

**D9 · Opposite-gender twin + set-aware expansion (HLM/TRS/GLV/LEG/BTS) + `manifest.json`** · PLUGIN-D4 · M
- **D4:** `genderTwinName` MTx:98-108; single-export twin MTx:707-732; batch expansion MTx:856-941;
  manifest MTx:1076-1087, 1160-1168.
- **Core today:** none.
- **Needs:** a new `AppPlugin::expandExportSet(ids)` → ids + an optional manifest entry. The core
  calls it in every export path.
- **Risk:** it must not double-expand. D4 filters the twin's twin (MTx:711-715).

**D10 · Base body / base head fit-reference geometry appended to the export** · PLUGIN-D4 · M
- **D4:** `appendBaseBody` MTx:351-445, `appendBaseHead` MTx:447-536; MTx:649-652, 1020-1023.
- **Core today:** none.
- **Needs:** a new export hook, `AppPlugin::prepareExportGeometry(id, Geometry&, Materials&)`.
  The same hook serves D11.
- **Risk:** D4 forces serial export when it is on (MTx:1111-1122).

**D11 · Export transforms: retarget (collapse cloth, remap to anchors, 26-bone rig) + dye/detail bake** · PLUGIN-D4 · M
- **D4:** `Retarget::applyFromSettings` MTx:671, 1059; dye and detail bake inside `buildExportMats`
  MT:707-824.
- **Core today:** `ExportOptions::reduceRig` (FOX, weight-based) only.
- **Needs:** the D10 hook. The dye bake needs the 6c dye state, so it is look-coupled.
- **Risk:** medium. The bake must match the extension's shader maths (MT:453-502).

**D12 · "Explain this material / this model's materials…" (text report of where each fact came from)** · PLUGIN-D4 · S
- **D4:** `showMaterialReport` MT:6639-6651; row menu MT:10264-10266; part menu `explainMaterial`
  MT:6776; `d4/src/model/MaterialReport.*`.
- **Core today:** none. `core/ui/TextReportDialog.h` exists.
- **Needs:** `rowMenuAboutToShow` / `partMenuAboutToShow` (exist), or a new
  `IGameStore::explain(id, material)`.

## E. Context menus and hotkeys

**E1 · Row-menu parity** · CORE · S
- **What D4's row menu adds** (on top of the image actions A6 and variants A14):
  - Copy in-game name.
  - Multi-row copy of ids, file names, names and collections.
  - "Export N models to <named last folder>" (omitted when there is none).
  - Un-block & render; Clear render blocklist (N).
  - Show dependencies (D6), Explain (D12).
  - Show all / Hide all / Invert on the loaded model's row.
- **D4:** list MT:1363-1408, grid MT:1444-1466, `addRowImageActions` MT:10147-10193,
  `addRowExportCopyActions` MT:10195-10271.
- **Core today:**
  - Load, copy name/path/id/collection, Export / to last, image, GIFs, extract, re-render (grid
    only), plugin hook (`cMT` 1132-1177).
  - Multi-row copy covers ids only.
- **Needs:** none.
- **Risk:** low.

**E2 · Part-menu parity (the shared builder, one menu for viewport, parts table and outliner)** · CORE · S
- **What D4's part menu adds:**
  - Export *model* and export *part* to the last folder and via prompt, with triangle counts in
    the labels.
  - Copy file name / id / collection.
  - Copy N material names.
  - A Show/Hide toggle.
  - Hide all; Invert.
  - Select (without moving the camera).
  - Explain (D12).
  - The blue outline marks what the menu acts on.
- **D4:** `showPartContextMenu` MT:6653-6804; `d4/src/util/ViewportPartMenu.h`.
- **Core today:** frame, isolate, hide, export parts (prompt only), copy names, copy material,
  show all, select all, clear (`cMT` 1180-1217). No "to last folder", no hide all or invert. The
  context outline is available (`setContextParts`) but unused.
- **Needs:** none.
- **Risk:** low.

**E3 · Part-visibility hotkeys: H hide, Shift+H solo, Alt+H show all (from list or viewport, with toasts)** · CORE · S
- **D4:** eventFilter MT:9519-9562, including the `ShortcutOverride` that keeps Alt+H away from
  the Help mnemonic. The viewport gets `Qt::ClickFocus` for this (MT:2253-2256).
- **Core today:** none. The core viewport has no key handling.
- **Needs:** `hotkeys::Def` rows (core registry) so they show in F1.
- **Risk:** low.

**E4 · Wheel gestures** · CORE · S
- **What it does:**
  - Ctrl+wheel resizes outliner icons.
  - Wheel over a live hover popup resizes it.
  - Wheel on the timeline steps a frame; Shift+wheel changes speed.
  - Wheel on the channel button cycles channels.
- **D4:** MT:9460-9490, 9582-9593, 9680-9693.
- **Core today:** Ctrl+wheel on the grid only (`cMT` 1249-1252).
- **Needs:** none.
- **Risk:** low. The same items appear under A4, A7, B2 and C9; listed once here as the
  mouse-sheet rows.

## F. Robustness

**F1 · Load-crash sentinel** · CORE · S
- **What it does:** `models/loadGuard = id` is flushed to disk before the parse and GPU upload. If
  it survives to the next launch, that model is blocklisted, cleared as "last selected", and
  logged.
- **D4:** set MT:7461, cleared MT:7514, 7575, 7597; recovery MT:4126-4151.
- **Core today:** `seh::runGuarded` covers faults inside a session (`cMT` 485, 512). A fault SEH
  cannot catch (a driver kill or stack overflow) is forgotten, so the tool will auto-load the same
  model again.
- **Needs:** none.
- **Risk:** low.

**F2 · Render blocklist + in-session quarantine** · CORE · M
- **What it does:**
  - A caught fault blocklists the id (persisted).
  - The viewport explains what happened.
  - The row dims with a ⚠.
  - Selecting a blocklisted row does not load it.
  - "Un-block & render" and "Clear render blocklist" in the row menu, plus Settings ▸ Maintenance.
- **D4:** `handleModelFault` MT:8773-8810; gate in `showAppearance` MT:5611-5618; list
  MT:873-874, 8659-8662; menu MT:10167-10192.
- **Core today:** a caught fault is reported and nothing is remembered (`cMT` 498-523).
- **Needs:** `AssetRow::Unrenderable` (exists) is set from the blocklist.
- **Risk:** low.

**F3 · Thumbnail-render sentinel + startup recovery** · CORE · S
- **What it does:** `model_render.guard` records id, name and stage. On the next launch it
  blocklists the model, reverts icons to Original, shows a one-time dialog, and appends to a crash
  log.
- **D4:** `recoverFromRenderCrash` MT:8720-8751, `renderGuardStage/endRenderGuard` MT:8755-8767,
  dialog in `showEvent` MT:9272-9290.
- **Core today:** `ThumbnailRenderer` has an SEH fault guard (`cMT` 160-165) and renders on its
  own context. There is no on-disk sentinel, so a hard crash in a thumbnail repeats on every
  launch.
- **Needs:** none.
- **Risk:** low.

**F4 · Unrenderable learning (parsed but has no geometry)** · CORE · S
- **What it does:** remembers ids that loaded with no geometry. They get ✗ and are skipped by
  auto-render; a later successful load clears the mark.
- **D4:** `m_noRenderSnos` MT:7533-7540; skip MT:4002, 8863.
- **Core today:** "Could not load" only (`cMT` 498-503).
- **Needs:** none. This is the source of A10 and A15.
- **Risk:** low. Persist per `IGameStore::fingerprint()` so a patch re-tests.

**F5 · Display-mode restore sentinel (PID-aware)** · CORE · S
- **What it does:** if restoring Grid killed the previous launch, fall back to Outliner. The
  sentinel records the PID so a live sibling instance is not mistaken for a crash.
- **D4:** MT:3367-3404.
- **Core today:** grid restore is unguarded (`cMT` 203).
- **Needs:** none.
- **Risk:** low. The general rule: "a setting applied at startup that can fault must be able to
  undo itself".

**F6 · Parsed-geometry and decoded-texture caches (instant re-select)** · CORE · S
- **D4:** `m_geoCache` 64 MB / `m_texCache` 192 MB, MT:876-879, 7462-7467, 7497-7502, 8288-8297.
- **Core today:** every selection re-runs `model::load` (`cMT` 470-492).
- **Needs:** none. A `QCache<AssetId, model::Loaded>` bounded by cost.
- **Risk:** medium. `model::Loaded` holds decoded images, so budget carefully and copy out of
  the cache.

**F7 · Guarded panel fill that names the texture being decoded** · CORE · S
- **D4:** MT:5867-5900 (`m_decodingTexSno`).
- **Core today:** n/a until C3 exists.
- **Needs:** C3.
- **Risk:** low. Build C3 this way from the start.

**F8 · No-geometry diagnostics (payload vs paylow stored sizes, TACT key held or missing)** · PLUGIN-D4 · S
- **D4:** MT:7516-7527.
- **Core today:** `m.error` is shown.
- **Needs:** `D4S::loadModel` puts this into `error`.
- **Risk:** low.

## G. Everything else

**G1 · Icon perf benchmark (`D4_ICONPERF_AUTO`, `D4_DUMP_ICONPERF`)** · DROP
- **D4:** `runIconPerfSweep` MT:9036, `writeIconPerf` MT:9804, `IconPerf` struct (ModelsTab.h:658-698),
  arming MT:4104-4125.
- **Evidence:** env-gated dev harness tied to D4's in-viewport icon path, which the core replaced
  with `ThumbnailRenderer` (it has its own `Stats`).

**G2 · `D4_DUMP_MATSNO` meta-binary material-table probe** · DROP
- **D4:** MT:5933-6052.
- **Evidence:** the research it served is done. `MaterialDecode::appearanceRosterFromMeta` is in
  use at MT:5827 and in `appearancePalette` MT:676.

**G3 · `D4_DUMP_ICONH` icon-handle probe** · PLUGIN-D4
- **D4:** MT:5904-5931.
- **Note:** still-open research into why encrypted appearances have no original icon. Keep it
  env-gated inside `D4S`/`IconIndex` if A4 needs it.

**G4 · Dead or hidden members** · DROP
- `m_exportBtn`: never constructed. There is no assignment anywhere, only null-checks (MT:4060,
  5586, 7528, 7656, 8801).
- `m_infoOverlay` / `m_infoVals`: hidden sink (MT:2399-2415).
- `m_variantsBtn` + `showVariantsMenu`: hidden orphan (MT:1087-1091, 1116-1118). It was
  superseded by the row-menu Variants submenu.
- `m_colBtn`, `m_gridBtn`: hidden, replaced by the header menu and the display drop-down.
- `m_pvHeadW` / `m_autoLoadBtn`: hidden state holders (MT:1895-1903).
- `applyViewportDevGating`: always shows (MTp:1058-1063).
- `m_fsBtn`: stays null (MT:2225-2227).

**G5 · Pre-index animation fallback (prefix-glob of `Anim/*.ani.json` per selection)** · DROP
- **D4:** `loadDeferredMeta` MT:7111-7145.
- **Evidence:** in the core the store lists clips, and the tab re-lists once secondary indexes
  land (`cMT` `relistFromSecondary` 391-423, and 535). A GUI-thread directory glob has no place
  in the core tab.

---

## Proposed interface additions (collected)

Existing and unused by `games/d4` today:
- `IGameStore::variants`, `usedBy`, `uses`, `gameIcon`, `dependencyFiles`, `readPath`,
  `beginRunCache`
- `AppPlugin::decorateTab`
- `ModelsTab::rowMenuAboutToShow` / `partMenuAboutToShow`

New (each is optional with a default, so FOX and POE2 are unaffected):

| Addition | For |
|---|---|
| `IGameStore::looks(id)` + look index on `resolveMaterials` / `exportOne` | C2 |
| `IGameStore::groupKeys(kind)` / `groupOf(id, key)` | A2 |
| `IGameStore::infoRows(id)` (label, text, link) | C12; also feeds A7 hover lines |
| `IGameStore::hoverLines(id)` (or reuse `infoRows`) | A7 |
| `IGameStore::boneDisplayName(nameHash)` (or readable names filled at `loadSkeleton`) | B4, A1 |
| `IGameStore::slotName(hash)` | C1 |
| `IGameStore::materialFacts(material)` | A1, C3 |
| `IGameStore::rawTextures(material)` | D8 |
| `IGameStore::clipsFor(sourceId, targetSkeleton)` / `suggestedClipSource(id)` | C7 |
| `IGameStore::explain(id, material)` | D12 (or plugin menu rows) |
| `Part::categories` | B19 |
| `Clip::group`, `label`, `variantOf`, `source` | C6, C8, D4 |
| `Attachment::detail` | C10 |
| `ExportOptions` clip subset | D3 |
| `resolveMaterials` decode granularity (`BaseOnly`) | B18 |
| `AppPlugin::expandExportSet(ids)` | D9 |
| `AppPlugin::prepareExportGeometry(...)` | D10, D11 |
| `ViewportExtension` settings rows / overlay toggles / "non-deterministic capture" flag | B5, B10, B12, B21 |
| Shell `reveal(tabId, id)` for cross-tab links | C12 "Sold in" |
