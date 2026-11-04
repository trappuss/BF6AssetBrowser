# Customize tab — Path of Exile 2

Research note written to be compared with CUSTOMIZE_D4.md and CUSTOMIZE_FOX.md, as input to a
game-independent Customize tab in the shared core. It describes what the code does; nothing here was
run. Every claim cites file:line.

**Abbreviations used in citations**

| Tag | File |
|---|---|
| `M` / `M.h` | `AssetBrowser/games/poe2/tabs/CustomizeTab.cpp` / `.h` (monorepo, on the shared core) |
| `O` / `O.h` | `poe2/src/tabs/CustomizeTab.cpp` / `.h` (original standalone POE2AssetBrowser) |
| `PS` / `PS.h` | `AssetBrowser/games/poe2/Poe2Store.cpp` / `.h` |
| `PP` | `AssetBrowser/games/poe2/Poe2Plugin.cpp` |
| `ML` / `ML.h` | `AssetBrowser/core/tabs/ModelLoad.cpp` / `.h` |
| `GL.h` / `GL` | `AssetBrowser/core/gl/GLModelWidget.h` / `.cpp` |
| `AS` / `AS.h` | `poe2/src/store/AssetStore.cpp` / `.h` |
| `AT.h` | `poe2/src/model/AssetText.h` |

---

## 1. Purpose

A PoE2 "dressing room". You pick a character class. The tab reads that class's body from the game's
own class table and `.ao` chain, and puts the class's default cosmetics on it (hair, beard, starter
clothes). You can then swap in any wearable mesh for eight fixed slots: shapeshift form, body armour,
helmet, gloves, boots, back, main hand and off hand. You can also fill a whole cosmetic set at once,
lock weapons to the class's theme, randomise, save named presets, play the class rig's clips on the
dressed figure, and export the result: as one merged glTF, without weapons, piece by piece, or as the
raw game files (M.h:2-17, M:98-111, M:250-257). In the monorepo, the figure is one merged
`core::Geometry` on the shared `core::gl::GLModelWidget`. The static view, the animated view and the
exported file are built by the same function (M.h:11-17, M:799-865).

## 2. Data model

**Base body / rig**
- Class table: `data/balance/characters.datc64`, read with POE2's `DatFile`. The columns are fixed
  byte offsets: display name @8, base `.ao` @16, attribute tag @492 (M:317-332). Rows with no name or
  no `.ao` are dropped (M:329). A hardcoded set of eight names marks "released" classes, and the list is
  sorted released-first, then alphabetically (M:323, M:333-336). `CharClass{display, aoPath, attr,
  released}` (M.h:150).
- Body resolution: class `.ao` → `AssetText::parseAo` → `smPath` → `parseSm` → `smdPath`
  (M:475-483; parsers AT.h:20-28, AT.h:103-114). The `.ao`'s `skeletonAst` is captured as `astHint`
  but never used (M:480, M:579-580; same in O:454, O:547-548).
- Rig and clips: `Poe2Store::loadSkeleton(idForPath(body), body.geo, decodeClips=true, &m_clips)`
  (M:588). That calls `AssetStore::loadSkeletonFor`, which looks for a co-located `.ast`, then the
  `.ast` declared by the body `.ao`, then (only under `/items/armours/`) `onerig.ast`
  (PS:266-276, AS:510-570).

**Slots**: a hardcoded table `SlotDef{id,label,folders,mode,bone,weapon}` (M.h:79-80, M:99-111):

| id | label | candidate folders (index path prefixes) | attach mode | bone |
|---|---|---|---|---|
| shapeshift | Shapeshift Form | `art/models/charactersfour/shapeshifters/` | ReplaceBody | — |
| body | Body Armour | `art/models/items/armours/bodyarmours/` | InPlace | — |
| helmet | Helmet | `…/armours/helmets/` | InPlace | — |
| gloves | Gloves | `…/armours/gloves/` | InPlace | — |
| boots | Boots | `…/armours/boots/` | InPlace | — |
| cape | Back (Cape) | `…/armours/capes/` (plus any `/wings/` path, rerouted: M:410) | InPlace | — |
| mainhand | Main Hand | `…/weapons/onehandweapons/`, `…/weapons/twohandweapons/` | Bone | `aux_R_Weapon_attachment_jntBnd` |
| offhand | Off Hand | `…/weapons/onehandweapons/`, `…/armours/shields/` | Bone | `aux_L_Weapon_attachment_jntBnd` |

Every candidate list comes from one sweep of the bundle index (M:386-458, §3). The header comment
says slots also come from `data/balance/microtransactionslot.datc64` (M.h:8, O.h:27). No code reads
that table; a repo-wide grep finds the string only in those two comments.

**Per-slot state**: the selected row of that slot's combo. The item data is the `.smd` path, and an
empty string means "— none —" (M:558, M:565). `Option{name, smd, attr, set}` (M.h:77) is the
catalogue entry. `attr` is a body-type tag (`str|dex|int|strdex|strint|dexint`, or empty =
universal), and `set` is the `/microtransactions/<set>/` folder name.

**Looks / variants**: the only variant axis is the attribute (body type). Options are filtered to the
class's `attr` (M:560). There are no colour/dye variants and no material swaps.

**Other chosen state**
- Outfit (cosmetic set). The combo's data is the set folder name. Choosing a set fills every armour
  slot with that set's piece that matches the class attribute, and clears armour slots the set does
  not cover. It never touches weapons or shapeshift (M:603-624).
- `m_lockWeapons`: a thematic weapon filter (M:133-137, M:542-564).
- Display toggles: Hair (rebuilds, M:224), Head (visibility only, M:225), FX (rebuilds, M:226).
- Class default pieces: `DefaultPiece{smd, bone, aliasSlot}` (M.h:82). They come from each
  `AttachedAnimatedObject` entry `"<bone> <child.ao> alias <slot_attach>"` in the class `.ao`, and a
  bone of `<root>` means in place (M:485-500).
- Per-piece visibility ticks: `PieceRange.visible` (M.h:86). These are not persisted, and every
  rebuild resets them (M:1062, M:858).

**Persisted**
- Presets: a JSON array in `QSettings` key `customize/presets` (M:86-92). Each entry is
  `{name, class (display string), lock (bool), slots {slotId: smdPath}}` (M:646-655). Saving a name
  that already exists replaces the old preset (M:657). Loading restores the lock, then the class
  (which refills the combos), then each slot via `findData(smd)`. A path that is missing falls back to
  "— none —" (M:676-699). The outfit set and the Hair/Head/FX toggles are **not** saved in a preset.
- NPanel open/order state under `customize/npanel` (M:872, M:903).
- `paths/lastExportDir` plus the shared `export/*` keys (CoreSettings.h:45-46, :72-80).
- Not persisted: the current class, the slot picks, the toggles and the lock. `persistView()` is
  empty (M:531-534).

## 3. Candidate discovery

- **Source**: one linear pass over `BundleIndex::files()`. It keeps `.smd` records only (M:396-399)
  and drops "noise" paths by substring: `/monsters/ /attachments/ /fx/ /vfx/ /particles/ _lod /lod
  drop _attach trail .mb .japan …` (M:306-315, M:401). Each path goes to the first slot whose folder
  prefix matches (M:402-404).
- **Stem normalisation**: a trailing `_<hex≥6>` hash is dropped (M:407-409). A shapeshifter mesh
  whose stem is `rig` is skipped, because it is an after-image effect (M:411). A generic stem
  (`rig`/`model`/`mesh`) takes its identity from the parent folder, which is how the ~1000 weapon
  meshes named `rig_<hash>.smd` get names (M:412-415, comment O:371-373). The set name is taken from
  the `/microtransactions/<set>/` path segment (M:416-418).
- **Attribute detection** is family-validated. Each stem is split at every occurrence of each of the
  six attribute tokens (M:341-351). A reading counts only if its base occurs with at least 2 distinct
  attributes in the same slot, and the largest family wins (M:425-441). Orphans fall back to a
  true-end suffix match that tolerates one trailing `a|b|r` variant letter (M:353-365, M:442-449).
  The purpose is that a stem which merely contains "int" is never read as Int (O:286-289).
- **Dedup**: by `prettyName|attr` within a slot, so the first mesh wins (M:451-454). Options are then
  sorted with `localeAwareCompare` (M:456-457).
- **Outfit list**: the set folders that have pieces in 2 or more *armour* slots
  (`body/helmet/gloves/boots/cape`), sorted, with the first letter capitalised (M:395, M:420,
  M:460-467).
- **Per-class filtering at fill time**: options whose `attr` is non-empty and differs from the class
  are hidden (M:560). With the lock on, weapon options must contain one of the class's hardcoded
  keywords in the **path** (`Warrior→mace`, `Ranger→bow`, `Monk→quarterstaff|warstaff|staff`, …),
  M:542-564. The current pick is kept across a refill when it is still present (M:556, M:567-568).
- **Search**: every slot combo and the outfit combo is a `core::SearchableCombo`. It is an editable
  combo that opens on click and filters with a case-insensitive `MatchContains` completer
  (SearchableCombo.h:22-38; used at M:124, M:156). The class and preset combos are plain `QComboBox`
  (M:122, M:190).
- **Names shown**: synthesised from paths by `prettify(base, set)`. It strips decorations
  (`_armour`, `_rig`, `_held`, …), removes a redundant set prefix, and returns `"Set · Base"`, `"Set"`
  or `"Base"` in sentence case (M:367-382). No in-game names: the original's `onMaterialsReady`
  documents that `NameIndex` returns nothing for these body-type variant meshes, so there is nothing
  to relabel (O:495-500). The monorepo does not override `onSecondaryReady`.
- **Icons / thumbnails**: none. The code has no icon, thumbnail or `gameIcon` use; options are text
  rows only.

## 4. Assembly

One function, `buildOutfit(includeWeapons, merged, mats, ranges, err)` (M:799-865), serves the
viewport and the merged export. The rules, in order:

1. **Piece list** (`currentPieces`, M:728-750):
   - The class defaults come first. A default whose alias contains `body`/`gloves`/`boots` is dropped
     when the user filled that slot (M:733-738).
   - Hair/beard defaults are dropped when Hair is off (M:739). A default counts as hair when its alias
     contains `head`/`face` and its path contains `hair`/`beard` (M:79-84).
   - Then each filled slot is added. Shapeshift is excluded, and bone-mode slots carry their bone
     (M:742-748).
2. **FX**: with FX on, each piece's effect meshes are appended in place, deduplicated (M:810-816).
   They are found by prefix search under `<piece dir>/fx` and `<…/microtransactions/<set>>/fx`, LODs
   skipped (M:771-793, `AssetStore::filesUnderPrefix` AS:195-212).
3. **Body**: the body `.smd`'s geometry and materials seed the merge (M:804-807).
4. **Seating**:
   - **In place** (armour, defaults with a `<root>` bone, FX): the vertices are copied unchanged,
     including their joint indices and weights. The code assumes the piece is authored against the
     same rig as the body (M:795-798, M:836-846). The joints are **not** remapped by name. Each mesh's
     joints were already remapped by the parents-first sort of *its own* resolved rig
     (PS:251-261); armour resolves to `onerig.ast` (AS:552-566). The code does not check that the
     piece's rig and the class body's rig have the same joint order.
   - **Bone** (weapons, defaults with a named bone): the bone is looked up by case-insensitive name in
     the class skeleton (M:822-825). The piece is baked rigidly at `inverse(inverseBind)` and weighted
     100% to that bone (M:835-844). This is the same rule `core::model::assemble` uses (ML:111-119).
     An unknown bone logs a warning and places the piece **unbaked**, at its authored coordinates
     (M:826-827). In the same situation `core::model::assemble` *skips* the piece (ML:108).
   - **ReplaceBody** (shapeshift): handled outside `buildOutfit`. The form's mesh plus its own
     skeleton (no clips) replace the whole scene (M:1067-1085).
5. **Materials**: each piece's `resolveMaterials(id, geo, decodeImages=true)` result, cached per
   `.smd` (M:763), is concatenated. Part `materialIndex` values are offset and `group` is reset to -1
   (M:832-834, M:847-853). The material mapping POE2 → core is `poe2::toCore` (PS:41-63).
6. **Visibility rules** (`applyVisibility`, M:993-1002). This is per-part visibility on the one
   merged mesh:
   - Head off hides the body's parts whose **name contains** one of 19 anatomical substrings (M:68-76).
     It applies only to the body's own part range, and never while a shapeshift form is shown.
   - Each ATTACHMENTS tick hides that piece's parts through its `PieceRange`.
   - Nothing hides hair automatically when a helmet is worn.

**Core use**: the tab does **not** call `core::model::assemble` or `core::model::load`. It reuses
the store calls directly (`loadModel`, `loadSkeleton`, `resolveMaterials`) and re-implements the
merge inline. The reason is that `assemble` only knows `IGameStore::attachments()` pieces and only
the rigid-to-bone rule (ML.h:47-62), while Customize needs arbitrary user-chosen pieces and
in-place skinned merging.

## 5. Animation

- **Clips offered**: every clip on the class body's rig, decoded eagerly on class change (M:588).
  They are listed as `(bind pose)` followed by the clip names, and a rig with no clips shows
  `(no clips on this rig)` (M:954-972). No clips are offered for weapons, pieces or the shapeshift
  rig (the shapeshift skeleton is loaded with `decodeClips=false`, M:1074).
- **Playback**:
  - Clips play on the merged outfit mesh, so the armour deforms with its skinning and weapons follow
    their bone (M:974-982).
  - The tab owns a 16 ms `QTimer` that advances `setTime` by a fixed 0.016 s and loops
    (M:907-918). The timer is handed to the viewport with `setPlaybackTimer` (M:909, GL.h:99).
  - Scrubbing pauses playback (M:948-951). The panel list and the anim-bar combo mirror each other
    (M:935-946).
- Any rebuild (a slot change, a toggle) stops playback and returns to bind pose (M:1055-1061).
- **Shapeshift and clips (defect in the monorepo, found by reading the code; not run)**:
  - `setClipIndex` says "a clip plays on the OUTFIT". It clears `m_shapeshifted` and calls
    `rebuildAssembly()` (M:977-978). But `rebuildAssembly` re-reads the shapeshift combo, which is
    still set, so it shows the shapeshift form again (M:1068-1083). The class clip is then applied to
    the shapeshift skeleton (M:979).
  - `GLModelWidget::setClip` checks only that a skeleton is present and matches the mesh (GL:492-498).
    The pose evaluator ignores out-of-range track joints (`core/gl/Pose.cpp:68-69`), so the result is
    wrong motion, not a crash.
  - The original avoided this: its `enterAnimation` always built the class outfit (O:954-982).
- Export never includes animation: `includeAnimations=false` for merged and per-item export
  (M:1110, M:1189, M:1259).

## 6. UI layout

Three columns in an `QHBoxLayout` (M:115-219):

**Left: a scrollable control column** (`QScrollArea`, width 320-420, M:201-206), top to bottom:
1. Form rows: **Class** (`QComboBox`; unreleased classes are suffixed "(unreleased)", M:510-513),
   then **Outfit (cosmetic set)** (`SearchableCombo`) (M:121-126).
2. The class info label: "`Class · ATTR · unreleased · body mesh not found`", greyed (M:128-131,
   M:581-584).
3. The checkbox "Lock weapons to class (thematic)", with a tooltip saying it is not a game rule
   (M:133-137).
4. The toggle row "Show: [Hair] [Head] [FX]". FX is off by default (M:139-151).
5. A **Slots** group box. It is a `QFormLayout`: one row per slot with the label, a `SearchableCombo`
   and a `⤓` tool button. The button, or a right-click on the label, opens a per-slot menu:
   "Export <slot> item (model)…" / "Export <slot> original files (raw)…", disabled when the slot is
   empty (M:153-173, M:1220-1236). The slots are a **flat list** in the fixed order of §2: no tree,
   no grid, no categories.
6. The buttons "Reset to class default" and "Random" (M:175-180). Random skips shapeshift and can
   land on "— none —" (M:710-723).
7. "Export outfit…" is a push button with a menu (M:182-186): Full outfit (one file) · Full outfit,
   no weapons · Each item separately · separator · Original files (raw, every piece) (M:250-257).
8. A **Presets** group box: a combo ("— saved outfits —" plus the names; *activating* an entry loads
   it), "Save current…" (an `QInputDialog` for the name) and "Delete" (M:188-198, M:628-699).

**Centre**: the shared `core::gl::GLModelWidget`, with the plugin's `viewFeatures()` and
`viewportExtension()` (M:209-214). POE2 overrides neither, so it gets the core defaults: all features
off and no extension (AppPlugin.h:101, :105; `Poe2Plugin.h` declares no override). Under the viewport
is the animation bar: "Clip:" combo · play/pause tool button (glyph 32/33) · a 0-1000 slider ·
"t / dur s". The bar is hidden when the rig has no clips (M:875-884, M:959). On the first build for
a class, the camera frames the figure and turns a half-turn to face it (`faceFront`, M:240-248,
M:594). Later rebuilds keep the view (`setScene(..., keepView=m_framed)`, M:1094).

**Right: a `core::NPanel` column** (`customize/npanel`), M:869-904. It has four panels, and ATTACHMENTS
and INFO are open on the first run (M:903):
- **PARTS** (a `QTableWidget`: Part · Tris · Material, row select). Lists **every** part of the merged
  figure; selecting a row highlights that part via `setSelectedParts` (M:886-891, M:921-926,
  M:1007-1021).
- **ANIMATIONS** (a `QListWidget` of clips; its title shows the count) (M:892-893, M:970-971).
- **ATTACHMENTS** (a checkable `QListWidget`). One row per assembled piece, labelled
  `default:<alias>`, the slot label, or `fx`, with the `.smd` path as tooltip; a tick shows or hides
  the piece (M:894-895, M:928-933, M:1022-1036).
- **INFO** (rich label): class, body path, part and vertex counts, joints and clips, and the piece
  list (M:896-898, M:1037-1048).
- `attachToggle(m_view)` puts the NPanel's collapse arrow on the viewport (M:904, NPanel.h:114).

**Hotkeys and menus**:
- The tab defines no hotkeys of its own.
- Through `BrowserTab`, the shell's Export menu shows "Export outfit…" (Ctrl+E →
  `exportSelection` → `exportOutfit(0)`) and "Export outfit to last folder" (Ctrl+Shift+E →
  `exportSelectionToLast`). It then appends the tab's four `populateExportMenu` rows (M.h:56-61;
  AppShell.cpp:284-306; Hotkeys.cpp:25-27).
- The tab's rows are not role-stamped.
- The tab is **lazy**: it is built on first show (PP:166-170).

## 7. Export

| Mode | What | How |
|---|---|---|
| 0 Full outfit | body + defaults + slots (+FX if on), weapons included, one file | `buildOutfit(true)` → `glb::write` (M:1105-1120, M:1206-1217) |
| 1 No weapons | same minus `weapon` pieces | `buildOutfit(false)` (M:819, M:1207) |
| 2 Each item | body + every current piece, each its own file in a chosen folder, named `<class>_<label>` | `core::model::exportOne(store, id, opt)` per piece; each piece gets its own skeleton lookup (M:1182-1204) |
| 3 Raw originals | the game files of body + every piece, mirrored under the chosen folder, de-duplicated | `IGameStore::dependencyFiles` + `readPath` (M:1122-1142, M:1168-1181; PS:228-239 → `collectAssetFiles(textures, skeleton)`) |
| Per slot | one slot's item, as a model or raw | `exportSlot` (M:1238-1271) |
| To last folder | mode 0 into `lastExportDir`, collision-free name `<class>_outfit` | M:1144-1157, `model::uniquePath` (ML.h:79) |

- **Options**: `settings::exportOptions(plugin.exportDefaults())`, which gives scale, skeleton,
  embedded/loose textures, normal-Z and `.glb` vs `.gltf` (CoreSettings.h:72-80; the POE2 defaults
  are scale 0.005, skeleton on, embedded textures, PP:340-352).
- The tab forces `includeAnimations=false` and `includeAttachments=false` (M:1109-1111).
- The tab shows no options dialog of its own; the merged-outfit export goes through a plain save-file
  dialog (M:1211).
- The merged file is skinned and carries the class skeleton, posed at bind (M:1112-1115, M:860).
- Results go through `ExportNotifier` (M:1156, M:1177, M:1201, M:1217).
- **FX**: because the merged export calls the same `buildOutfit`, the FX meshes are exported when the
  toggle is on (M:810-816). The original's `buildOutfitGeometry` never added FX (O:767).

## 8. Robustness

- **Re-entrancy**: an `m_syncing` flag and `QSignalBlocker` around every programmatic combo fill
  (M:552-570, M:589-592, M:610-621, M:689-696). Index bounds are checked on every class and slot
  access (M:280-281, M:540, M:575, M:1222).
- **Fail closed and say so**:
  - No class table → a status message (M:518).
  - Body not found → the info label says so (M:584).
  - The body fails to load → the scene is cleared and the error is reported (M:803-805, M:1089-1092).
  - A mesh fails to load → `qWarning` and the piece is skipped (M:765-766, M:821).
  - An unknown weapon bone → a warning, and the piece is placed unbaked (M:827).
  - Per-item and raw export count failures and report them (M:1197, M:1201-1202).
- **Mesh cache**: `m_meshCache` (smd → geometry + decoded materials) (M:752-769).
  - It is capped at 96 entries and cleared **wholesale** when full (M:758).
  - It is cleared on index ready and on reset, because a new build can change what a path holds
    (M:506, M:524).
  - `decodedFor` returns a reference into the `QHash`, which the next insert or clear can invalidate.
    Every current caller copies the data before calling it again (M:804-807, M:820-853), but this is
    fragile.
- **Background work: none.** Everything runs on the GUI thread:
  - the full-index sweep in `buildSlotCatalogue` (M:396-423), run on `onIndexReady` when the lazy tab
    is first shown;
  - the class table read;
  - every mesh decode, plus DDS decode, because `decodeImages=true` (M:763);
  - the eager decode of all clips (M:588);
  - the whole-figure merge and GPU re-upload on **every** slot change or toggle (M:600, M:1053-1101).
  The store is thread-safe (PS.h:56), so the work could move off the GUI thread; the tab does not do
  that.
- **Diagnostics**: catalogue counts (M:470) and assembly summaries (M:1081, M:1100) go to `qInfo`.
  The original's `POE2AB_CUSTOMIZE_AUDIT` per-slot dump was dropped (O:424-431).
- **Harness API**: `classCount`, `selectClass`, `slotIds`, `slotOptionCount`, `selectSlotOption`,
  `assembledPieceCount`, `assembledPartCount` and `exportOutfitTo` (M.h:63-73). A grep of both
  trees finds **no caller**.
- **Dead code**:
  - `m_sets` is declared and never filled (M.h:155; O.h:122).
  - `attrOfStem` is never called (M:295).
  - `astHint` is never used (M:579-580).

## 9. Game-specific vs generic

| Feature | Class | Reason |
|---|---|---|
| Base-body picker (class list) | GENERIC | Every game has "pick a body"; only the source table differs |
| Class list read from `characters.datc64` @8/@16/@492 | GAME-SPECIFIC | POE2 table and byte offsets (M:317-332) |
| Hardcoded "released" set | GAME-SPECIFIC | POE2 release status, not in the data (M:323) |
| `.ao` → `.sm` → `.smd` body + default pieces with alias | GAME-SPECIFIC | POE2 text descriptor chain (M:475-500) |
| Slot table (id, label, mode, bone) | GENERIC shape / GAME-SPECIFIC contents | A slot list with an attach rule is universal; the folders and bone names are POE2's (M:102-111) |
| Candidate sweep by path prefix | GAME-SPECIFIC | POE2 uses its folder taxonomy as the slot key (M:386-423) |
| Noise-path filter | GAME-SPECIFIC | POE2 path conventions (M:306-315) |
| Body-type attribute and family validation | GAME-SPECIFIC | PoE2's str/dex/int mesh variants (M:339-365, M:425-449) |
| Filter options by body variant | GENERIC | "Only show what fits this body" is universal; the key is per game (M:560) |
| Cosmetic set → fill slots | GENERIC | D4/FOX also have sets or outfits; POE2 derives them from `/microtransactions/<set>/` (M:460-467, M:603-624) |
| Thematic weapon lock | GAME-SPECIFIC | Hardcoded class → keyword table matched on path substrings (M:542-564) |
| Default pieces replaced when a slot is filled | GENERIC | "A slot pick overrides the body's default for that slot" (M:733-738) |
| Hair toggle / hair detection | GENERIC idea / GAME-SPECIFIC rule | Alias plus path substring (M:79-84) |
| Head toggle by part-name substrings | GAME-SPECIFIC | Anatomical part names of PoE2 bodies (M:68-76) |
| FX meshes via `/fx` sibling folders | GAME-SPECIFIC | POE2 folder convention (M:771-793) |
| In-place skinned merge (same rig) | GENERIC | Joint-index-compatible merge with part ranges (M:829-858) |
| Rigid bake to bone at bind | GENERIC | Already core's `assemble` rule (ML:111-119) |
| Whole-body replacement (shapeshift) | GENERIC | A "form" slot that swaps the rig |
| Per-piece show/hide on the merged mesh | GENERIC | Part ranges plus `setPartVisible` (M:993-1002) |
| Presets as JSON of slot → asset | GENERIC | Stores stable ids; restores with `findData` (M:640-699) |
| Randomise / reset | GENERIC | Pure UI over slot lists (M:701-723) |
| Searchable slot combos | GENERIC | `core::SearchableCombo` |
| NPanel PARTS / ANIMATIONS / ATTACHMENTS / INFO | GENERIC | Core widget, same panels as the Models tab |
| Animation bar and clip list | GENERIC | Core viewport clip API |
| Face-front half-turn | LOOK | Characters authored facing −Z (M:240-248) |
| Viewport features / extension | LOOK | Plugin-supplied; POE2 uses the plain core look |
| Merged / no-weapons / per-item / raw export | GENERIC | Uses `glb::write`, `model::exportOne`, `dependencyFiles` |
| Names from path `prettify` | GAME-SPECIFIC | POE2 has no in-game names for these meshes (M:367-382, O:495-500) |

## 10. What a game must supply

These are the questions the tab asks of game data. Each is proposed as a method on a
`ICustomizeSource` that a plugin implements, beside `IGameStore`. The last column says where POE2
answers the question today.

| # | Question | Proposed signature | POE2 today | Already core? |
|---|---|---|---|---|
| 1 | Which bodies can be dressed? | `QVector<BodyDef> bodies()` — `{id, label, variantKey, note}` | `loadClasses` (M:318-337) | No, POE2 code |
| 2 | Body → mesh + rig + clips | `BodyResolved resolveBody(id)` — `{AssetId mesh, defaults}`; rig/clips via `IGameStore::loadSkeleton` | `resolveBodyMesh` (M:475-502) + `Poe2Store::loadSkeleton` (M:588, PS:266-276) | The rig/clip half is core (`IGameStore`) |
| 3 | The body's default pieces | `QVector<Piece> defaults(body)` — `{AssetId, seat, slotId?, tag("hair")}` | `.ao` `attached_object` + alias (M:485-500) | No. `IGameStore::attachments()` exists (PS:292-309), but it drops `alias`, so Customize cannot use it |
| 4 | Which slots exist, and how each seats | `QVector<SlotDef> slots()` — `{id, label, seat: InPlaceSkinned / RigidToBone(bone) / ReplaceBody, overridesDefaultSlot, excludeFromSets, excludeFromRandom}` | Hardcoded (M:102-111) | No |
| 5 | Candidates per slot | `QVector<Option> options(slotId)` — `{AssetId, name, variantKey, setId, iconId?}` | `buildSlotCatalogue` (M:386-458) | No (the sweep is POE2 code; the `BundleIndex` access is POE2-native) |
| 6 | Which options fit this body? | `bool fits(const BodyDef&, const Option&)` (default: `variantKey` empty or equal) | M:560 | No |
| 7 | Optional thematic filter | `QStringList filterPresets(body)` / `bool passesFilter(...)` | Weapon lock (M:542-564) | No |
| 8 | Sets / outfits | `QVector<SetDef> sets()` + `pieceFor(set, slot, body)` | M:460-467, M:616-617 | No |
| 9 | Companion meshes of a piece (FX) | `QVector<AssetId> companions(pieceId)` | `fxMeshesFor` (M:771-793) | No |
| 10 | Body regions to hide ("head") | `QSet<int> regionParts(bodyGeo, "head")` | `isHeadPartName` (M:68-76) | No |
| 11 | Load a piece | `IGameStore::loadModel` / `resolveMaterials` | M:762-763 | **Yes**, core (IGameStore.h:97, :103) |
| 12 | Seat a piece | Core: in-place merge + `RigidToBone`; optional store override `placeAttachment` | Inline in `buildOutfit` (M:818-859) | The rigid half is core (`ML:111-119`, IGameStore.h:112-115); the in-place half is POE2 code |
| 13 | Raw files of a piece | `IGameStore::dependencyFiles` / `readPath` | PS:223-239 | **Yes**, core |
| 14 | Export one piece | `core::model::exportOne` | M:1196, M:1268 | **Yes**, core |
| 15 | Display name / icon of an option | `name` in #5; `IGameStore::gameIcon(id,px)` | Path `prettify` only (M:367-382); no icon | `gameIcon` exists in core (IGameStore.h:123); POE2 does not implement it |

**Already generic core code the tab uses** (so a core Customize tab starts with these):
- `core::BrowserTab`: its lifecycle and export hooks (BrowserTab.h:30-49), with lazy construction
  through `TabSpec` (AppPlugin.h:22-28, PP:170).
- `core::gl::GLModelWidget` calls:
  - `setScene(geo, skel, keepView)`, `setMaterials`, `clearScene`, `hasGeometry` (GL.h:72-76);
  - `setClip`, `clearClip`, `hasClip`, `setTime`, `time`, `clipDuration`, `setPlaybackTimer`
    (GL.h:89-99);
  - `setPartVisible` (GL.h:109) and `setSelectedParts` (GL.h:121);
  - `setOrbitYaw` and `camYaw` (GL.h:215-217);
  - `setFeatures` and `setExtension`.
- `core::NPanel` (NPanel.h:52-73, :114).
- `core::SearchableCombo`.
- `core::glyph::toolIcon`.
- `core::StatusLine`, `core::ExportNotifier`, `core::settings::{exportOptions, lastExportDir,
  setLastExportDir}`.
- `core::glb::write` / `glb::Scene` (GlbExporter.h:72, :107).
- `core::model::exportOne` and `uniquePath` (ML.h:75-79). The `core::rm` matrix helpers.
- The core types `Geometry`, `Part`, `Vertex`, `Material`, `Skeleton`, `Clip`.
- `IGameStore` through `Poe2Store`: `loadModel`, `loadSkeleton`, `resolveMaterials`,
  `dependencyFiles`, `readPath`.
- `AppPlugin` through `Poe2Plugin`: `viewFeatures`, `viewportExtension`, `exportDefaults`.

**POE2 code inside the tab that bypasses the core seam** (it needs `Poe2Plugin&` and `Poe2Store&`,
not `IGameStore*`; M.h:130-131):
- `m_store.native()`, which is POE2's `AssetStore`, used for `readFile`, `index()` and
  `filesUnderPrefix` (M:322, M:390, M:477, M:787);
- `Poe2Store::idForPath`, which is not on `IGameStore` (PS.h:51);
- `DatFile`, `AssetText::parseAo` / `parseSm` and `BundleIndex` (M:17-20).

## 11. Strengths worth keeping / weaknesses

**Strengths**
- **One assembly for view, animation and export.** `buildOutfit` feeds the viewport (M:1089), the
  merged export (M:1108) and, through the shared rig, playback. The original had two paths: rigid
  attachments while static (O:1271-1314 via `GLModelWidget::setAttachments`), and a separate merge on
  animate (O:954-982). It also left FX out of export (O:767).
- **Per-piece toggles work during playback.** Visibility is per part on the merged mesh (M:993-1002).
  The original disabled the toggles while animating (O:855, O:863).
- **Head toggle without a rebuild.** It is visibility only (M:225); the original rebuilt (O:216).
- **Camera kept across slot changes.** `keepView=m_framed` (M:1094), and the face-front turn happens
  only on a new class (M:594). The original re-framed on every rebuild (O:1243, O:1315).
- **PARTS lists every merged part** (M:1009-1018). The original listed only the body's parts
  (O:903-913).
- **Data-backed candidate classification** instead of naive suffixes: family validation stops "int"
  inside a word from being read as Int (M:339-351, M:425-449). Outfits are limited to real
  multi-armour sets (M:460-462).
- **Default-piece override** by the slot alias from the `.ao` (M:733-738) keeps the in-game starting
  look and swaps cleanly.
- **Export coverage**: merged, no weapons, per item, raw originals, per slot, and to last folder, all
  through core writers and shared settings (M:1103-1271).
- **Searchable combos** for lists with more than 1000 entries (SearchableCombo.h:22-38).
- **Presets store stable ids** (smd paths, the class display name) and restore with `findData`,
  falling back to "none" (M:690-695).

**Weaknesses**
- **All work runs on the GUI thread.** That includes the full index sweep, texture decode on every
  new piece, clip decode, and a full re-merge and re-upload per change (§8). The store is thread-safe
  (PS.h:56), so this is a choice, not a constraint.
- **Classification by path and name substrings** in several places, which the project's own rule
  forbids ("Classify by authored data, never a name substring", `poe2/CLAUDE.md` rule 2): the head
  parts (M:68-76), hair detection (M:79-84), the noise filter (M:306-315), the weapon lock (M:562)
  and wings → cape (M:410).
- **Header claims a table the code never reads** (`microtransactionslot.datc64`, M.h:8).
- **Hardcoded data**: the released-class set (M:323), the slot table and bone names (M:102-111), and
  the class → weapon keywords (M:542-548). A new class or slot needs a code change.
- **In-place pieces are not joint-remapped by name.** The merge trusts that each piece's
  index-order joint palette equals the class body's rig (M:836-846 vs PS:251-261). Nothing checks
  this or reports a mismatch.
- **Different rule from core for an unknown bone.** The tab places the piece unbaked (M:827); core
  `assemble` skips it and names it (ML:108).
- **Shapeshift and clip defect** (§5): selecting a clip while a shapeshift is chosen plays the class
  clip on the shapeshift rig (M:977-979).
- **Tick state lost on every rebuild**: per-piece ticks reset whenever any slot changes
  (M:1062, M:858).
- **Presets are partial**: they omit the outfit and the Hair/Head/FX toggles (M:651-655). The
  current look is not restored at startup (M:531-534).
- **Timer-driven playback**: the fixed 0.016 s step per 16 ms tick means playback speed drifts with
  timer jitter (M:913).
- **Cache is cleared wholesale at 96 entries** (M:758). `decodedFor` returns a reference that a
  later call can invalidate (M:754-769).
- **No names or icons**: labels are path-derived (M:367-382), and there are no thumbnails, although
  the core has `gameIcon` (IGameStore.h:123) and a `ThumbnailRenderer` (`core/gl/ThumbnailRenderer.h`).
- **Dead code and an unexercised harness**: `m_sets`, `attrOfStem`, `astHint`, and the public
  harness methods have no callers (§8).

### Differences: original (`poe2/src`) vs monorepo

| Area | Original | Monorepo |
|---|---|---|
| Base / store | `QWidget` over `AssetStore*`, emits `status` (O.h:32-41) | `core::BrowserTab` over `Poe2Plugin&` / `Poe2Store&`, uses `StatusLine` (M.h:46-50, M:238) |
| Tab construction | Eager; `onIndexReady` + no-op `onMaterialsReady` from MainWindow (`poe2/src/app/MainWindow.cpp:44,56,195`) | Lazy `TabSpec` (PP:170); no `onSecondaryReady` |
| Static view | Body + `GLModelWidget::setAttachments` (rigid bake at the bone's bind; in-place for `""`) (O:1271-1314) | One merged `core::Geometry` via `setScene` (M:1087-1098) |
| Animation | `enterAnimation` builds the merged figure; `exitAnimation` rebuilds the static one (O:954-991) | One path; the clip plays on the already merged mesh (M:974-982) |
| Bone bake | `bones[bi].bind`, requires `bi<=255` (u8 joints) (O:776-790) | `inverse(joints[bi].inverseBind)`, u16 joints (M:835-843) |
| FX in export | No (O:767) | Yes, when the toggle is on (M:810-816) |
| Head toggle | Rebuilds (O:216) | Visibility only (M:225) |
| Camera | +π at construction, `frameAll` each rebuild (O:204, O:1315) | `faceFront` once per class, `keepView` afterwards (M:240-248, M:1094) |
| Panels | `QTabWidget` Parts / Animations / Attachments / Info; Parts = body only (O:824-851, O:903-913) | `core::NPanel` with the same four panels, persisted; PARTS = the whole figure (M:869-904) |
| Playback clock | Widget-owned, `animTimeChanged` signal (O:882-894) | Tab-owned 16 ms `QTimer` (M:906-918) |
| Export options | `Config::*` keys built by hand (O:1000-1007) | `settings::exportOptions(plugin.exportDefaults())` (M:1109) |
| Export writers | `GlbExporter::write` per call; raw via `collectAssetFiles` (O:1022, O:1052) | `glb::write`, `model::exportOne`, `dependencyFiles` / `readPath`; `ExportNotifier`; Ctrl+E / Ctrl+Shift+E via the shell (M:1105-1271) |
| Export mode | Member `m_exportMode` (O.h:131) | Argument `exportOutfit(int)` (M.h:112) |
| Audit | `POE2AB_CUSTOMIZE_AUDIT` env dump (O:424-431) | Removed; one `qInfo` summary (M:470) |
| Harness | none | Public no-widget API (M.h:63-73), no callers |
| Unchanged | Slot table, the catalogue sweep, attribute and family logic, `prettify`, presets key and schema, the lock table, randomise and reset, per-slot export menu (M:98-111 = O:78-91; M:386-458 = O:343-422; M:626-699 = O:614-701) | |

## 12. Sockets (measured 2026-09-23, from `metadata/characters/character.ao` dumped by `--dump-paths`)

- An `attached_object = "<name> <child.ao> alias <slot>"` names a **bone or a socket**. The Warrior's
  hair uses the socket `Head_Attachment`; the Druid's uses the bone `head_jntBnd`.
- Sockets live in the `ClientAnimationController { … }` block of a class `.ao` and of every file it
  `extends`: `extends "Metadata/X"` is the file `metadata/x.ao`, and the chain ends at
  `metadata/parent.ao` (`extends "nothing"`). `character.ao` declares 61 sockets, and each class `.ao`
  adds 5 more (eyes, above_head).
- A socket entry is `socket = "<name>"` followed by indented keys:
  - `parent` (a bone);
  - `translation = "x y z"`;
  - `rotation = "a b c"` (radians; values up to ±π);
  - `scale = "x y z"`;
  - `ignore_parent_axis`, `ignore_parent_rotation`.

  One socket (`aux_STRIKE_POSITION`) has no parent. Of the 61 in `character.ao`, `Head_Attachment`,
  `Back`, `L_Weapon` and `R_Weapon` carry no offset; the sheaths and FX sockets carry translation and
  rotation.
- **Applied now:** socket → parent bone (`Poe2CustomizeSource::socketsOf`). **Not applied:** the
  offsets. No piece the tab places today uses an offset socket, and the rotation's Euler order
  is not known (no public documentation found). A socket with offsets that gets used logs them
  as "not applied". Applying them needs the axis order measured first, for example by comparing
  a sheathed weapon in-game against each order.
