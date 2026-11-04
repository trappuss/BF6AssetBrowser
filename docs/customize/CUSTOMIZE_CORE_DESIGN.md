# Customize — one core tab for character assembly

Written 2026-09-23 from three source studies done the same day, each read from the code with
file:line evidence:
[`WARDROBE_D4.md`](WARDROBE_D4.md) (D4 Wardrobe, ~13.5k lines),
[`CUSTOMIZE_FOX.md`](CUSTOMIZE_FOX.md) (FOX Customize, ~11.7k lines),
[`CUSTOMIZE_POE2.md`](CUSTOMIZE_POE2.md) (POE2 Customize, ~1.3k lines, already on the core).

## Scope decision

- **In the template:** character assembly — "pick a body, fill its slots, see it, animate it,
  export it". All three games have it, and a new game with characters will too.
- **Not in the template:** D4's **Stable** (mounts, D4-only) and **Catalogue** (D4's store
  bundles). They stay in the standalone D4AssetBrowser. A generic "bundle browser" is written
  only if a later game has an in-game store worth browsing; nothing is built for it ahead of time.
- **D4's Wardrobe is not ported as-is.** It is the richest of the three, and its data rules and
  rendering are D4-only. The core tab takes FOX's shape (below), borrows D4's best mechanisms, and
  D4 later answers the same interface as the other two games.

## What each study showed (in one line each)

| | D4 Wardrobe | FOX Customize | POE2 Customize |
|---|---|---|---|
| Shape | class × gender → 10 equipment slots + 9 creator categories, icon grids over hidden combos | category → subject → variant → slot rows; every list a searchable 3-line combo; looks are rows too | class → 8 hard-coded slots, combos |
| State | QSettings is the model; presets copy 49 keys | ONE sorted `key=value` text description: presets, undo, clipboard, `.foxbuild` | combos; presets miss the outfit and toggles |
| Assembly | skeleton merge by bone hash; hardpoint seating; sub-rigs keep their own bones | no merge — host rig elected, parts on connect points, partial rigs borrow | one merged geometry, rigid weapons at bind |
| Rebuild | debounced, async, previous model kept on screen | full reload on the GUI thread per change | full re-merge on the GUI thread |
| Best idea | themes/sets, sub-rigs, crash breadcrumb | one panel for all categories, honest row states, one build text, undo by diffing | single merged geometry = viewport = export |
| Worst problem | 13.5k-line god class, out-of-bounds read in export naming (**fixed**, below) | game rules inside the UI; Survive colour never saved | GUI-thread everything; shapeshift plays the wrong rig's clip |

## The spliced design

### 1. Shape (FOX's — the general one)

```
Category ▾        (FOX: Weapon/Character/Vehicle…; D4: Equipment/Appearance; POE2: one)
Subject  ▾        grouped list with captions (D4: class·gender; FOX: character; POE2: class)
Variant  ▾        optional (FOX tiers/bodies; D4 none; POE2 str/dex/int body type)
[Find in every slot …]                  one box, per-slot match counts (FOX)
Slot rows — one searchable list per slot, in the game's order:
  Helm        [ — default — (what the game starts it in) ▾ ]    bold fitted · dim empty ·
  Torso       [ barF_stor151_TRS · file · path          ▾ ]    faded unusable · amber unseated ·
  …                                                            struck-through hidden (FOX)
  └ Dye rows under a dyeable slot, shown only when the fitted piece can be dyed (FOX, D4)
Look rows — the same row type (skin, face, hair style …), so presets/undo/search treat them alike
Footer: Sets/Themes ▾ (fill slots from a set; D4 themes, POE2 outfits) · Clear · Surprise me ·
        Preset ▾ · Build ▾ (save / copy text / paste text / file) · Only-compatible ☐ · Unlocked ☐
```

- **Lists are `SearchableCombo`** (core already): name / file / path, optional icon or swatch,
  captions, type-to-filter. D4's **icon grid** is kept as an optional second view of the SAME
  list (a popup grid when the game supplies icons), not as a separate model.
- The right column is the core Models tab's N-panel (PARTS, MATERIALS, INFO, ANIMATIONS) — one
  implementation, not a third copy.

### 2. State (FOX's): one build description

A build is a sorted `key=value` text keyed by **stable ids** (asset path / SNO / table row), never
by combo index or display text (D4 convention 3). The same text is the preset, the undo entry, the
clipboard form and the file (`.build`). **Undo diffs settled descriptions** after each rebuild
(FOX), so no code path can forget to record, and rebuilds the user did not author push nothing.
D4's QSettings-as-model and hidden-combo identity are not carried.

### 3. Assembly (a core engine; the game says HOW each piece attaches)

One function turns a build into one `core::Geometry` + skeleton + materials — the **same result
feeds the viewport, playback and every export** (POE2, and FOX's "one visibility function").
Per piece the game answers `placement`, one of:

| Placement | Meaning | Today |
|---|---|---|
| `MergeByBone` | skinned; joints remapped onto the body rig by bone NAME/hash, missing bones appended parents-first | D4 armour (mergeGeometries), POE2 in-place (which does NOT remap today — a latent bug the core version fixes) |
| `Socket` (baked) | rigid on a socket/bone at an offset | D4 weapons/sheaths/trophy, POE2 weapons, FOX connect points; core `model::assemble` has this |
| `SocketRig` | keeps its own rig, parented at a socket (flails, animated trophies, FOX partial rigs) | D4 `attachSubRigAt`, FOX host/borrow |
| `ReplaceBody` | this piece becomes the body (shapeshift, D4's wrath form, mounts carrying the rider) | POE2 shapeshift; D4 wrath submeshes (user, 2026-09-23) |
| `StorePlaced` | the game bakes it itself (existing `IGameStore::placeAttachment`) | D4 attachments on the Models tab today |

`hides(piece, build)` returns body/part ids to switch off (D4 slot replacement + slot-hash
coverage, FOX neck/sleeves/hair-under-hat, POE2 head region) — applied by the one visibility
function, never by name tests in the tab.

**Rebuild policy (D4's):** debounced, assembled on a worker with a generation token, previous
model left on screen until the new one is ready, camera and clip kept. A **parsed-piece cache**
(FOX and POE2 both reload every piece on every change).

### 4. Looks: material and texture overrides, not tab code

Dyes, camo, skin tone, markings, hair colour: the game returns **overrides** — a material field
change, a replacement texture, or a composited image it made (D4's CPU compositing stays D4 code
behind this answer). The tab only applies them. Shader-level looks (cloth, fur, D4's hair/eye/skin
shading) stay in the game's `ViewportExtension` — the §8.9 rule.

### 5. The interface a game implements

Required (a new game answers these five and gets a working tab):

```cpp
class ICustomizeSource {
public:
    virtual QVector<Category> categories() = 0;
    virtual QVector<Subject>  subjects(const CategoryId&) = 0;                 // grouped, with captions
    virtual QVector<SlotDef>  slots(const SubjectId&) = 0;                     // id, caption, kind Part|Look, dyeable
    virtual QVector<Option>   options(const SubjectId&, const SlotId&, const Build&) = 0;   // incl. "default" row
    virtual QVector<Piece>    resolve(const SubjectId&, const Build&) = 0;     // the pieces the build needs:
                                                                              //   AssetId + Placement + hides
```

Optional (defaults make them no-ops):
`variants`, `defaultOption`, `compatible` (allowed set + "no rule" + "slot unusable"),
`rules` (excludes/requires), `sets` + `setPieces` (themes/outfits), `dyeChannels` + `applyDye`,
`applyLook`, `shippedPresets` (read-only game builds), `counterpart` (the same build for the
other gender/body — D4's both-genders export), `clips` + `idleFor` (D4's wardrobe idle),
`hoverInfo`. Icons come from the existing `IGameStore::gameIcon`; meshes, rigs, clips and
materials from the existing `IGameStore` — no second loader.

How the three games map (from the studies' section 10):

| Interface | D4 | FOX | POE2 |
|---|---|---|---|
| categories/subjects | class × gender (`W:342-354`) | `BuilderSource` categories/subjects (`PC.h:113-160`) | classes from `characters.datc64` |
| slots | `kSlots` + `kCreator` | `slotsForSubject` | hard-coded 8 → a table |
| options | SNO index prefix rules, weapon ItemTypes, creator folders | `partsForSubject` + `compatibleStems` | bundle-index sweep by folder |
| resolve/placement | armour Merge, weapons Socket, trophy SocketRig | host + connect points | in-place Merge, weapons Socket, shapeshift ReplaceBody |
| looks | `setPartDye`, CPU compositing | `.fv2` variations, avatar textures | none |

FOX's `BuilderSource` is already most of this interface — FOX is the easiest port after POE2.

### 6. Export

Whole build (`.glb`), per piece, per part, animated with chosen clips, and `counterpart` builds
(both genders). Filename template with placeholders keyed by slot id (D4). Everything goes through
`core::model::exportOne` / `glb::write` — the same assembled geometry the viewport shows.

## Build order

1. **Core engine + interface + tab, POE2 as the first game.** POE2's tab is small, already on the
   core, and its fixes come free: worker rebuild, name-based joint remap, the shapeshift clip bug,
   presets that keep everything. Gate: POE2's Customize does everything it does today (its bat
   checks) through the core tab.
2. **FOX** through its existing `BuilderSource` — the FOX-owned tab retires. Gate: FOX's
   Customize checks (character shot "look applied", its regression suite) through the core tab;
   fixes Survive colour persistence and the dead host hotkeys on the way.
3. **D4** last, as a source: slots/options from its indexes, placement from `seat`/`attachSubRigAt`,
   looks from its dye/compositing code. The standalone D4 Wardrobe stays untouched until the core
   tab matches it on D4's repro characters.

## Step 2 (FOX), broken down — written 2026-09-24 from `CUSTOMIZE_FOX.md` §2–§10

FOX's tab is ~11.7k lines, and every part of it that step 1 lacks is something D4 needs as well.
So step 2 is five stages. Each one ships, is checked, and leaves FOX's own tab running until the
last one:

| Stage | What | Why it comes here | Checked by |
|---|---|---|---|
| 2a | **Interface growth** that all three games use: categories and variants (FOX category → subject → variant; POE2 one category; D4 class × gender), **look rows** (a `SlotDef` kind whose options are not meshes), option icons, swatches and section captions, a default row that names what the game dresses the subject in, **rules** (excludes/requires with an Unlocked switch), and compatibility narrowing with rule / no-rule / off states | pure core + UI; a synthetic source exercises every branch; POE2 keeps working | `shell_check` synthetic source; POE2 bat checks unchanged |
| 2b | **Assembly growth:** pieces that keep their OWN rig (FOX never merges skeletons; D4's `attachSubRigAt`); seating on connect points with chains (FOX `.fcnp`, D4 hardpoints — the core `Hardpoint` type exists); host election and fragment borrowing; the rest-alignment offset | the core engine today merges into one skeleton, which is POE2's and D4's armour case but not FOX's | synthetic rigs in `shell_check` (chain, cycle refused, borrow all-or-nothing) |
| 2c | **Looks as overrides:** a variation / dye / look returns material or texture overrides, group hide/show and models it brings along (FOX `.fv2`, MGO dye `.fv2`, Survive swatches, avatar texture sets; D4 dyes later). The tab only applies them, and one visibility function feeds both viewport and export | the core `Material` path exists; the overrides are a new, generic answer | synthetic overrides; FOX's `--camodump`-style checks through the harness |
| 2d | **FOX's `ViewportExtension`:** FOX's own shader as-is (plan §8.9: a whole-shader replacement, not sliced), fed FOX's per-material inputs (SRM, TRM, layer + mask, FMTT presets, eye / hair / glass, dirt) | the user's rendering rule (2026-09-23): moving Customize onto a core viewport without FOX's look would be a visible regression | Mesa shader compile in `run_tests.sh`; a real-data FOX shot compared against FOX's own viewport |
| 2e | **`FoxCustomizeSource`** over `fox::BuilderSource` + the FOX-specific rules (sleeves, hair forms, neck, bandanna, skeleton-only bases, companions, context swaps, dirt), then FOX's tab retires to `_retired/` | last, when the core can express everything FOX's tab shows | FOX's bat checks (`fox_shot_character` "look applied", `fox_slotcolor`) and FOX's `Verify - Regression.bat` customize items through the core tab |

2a–2c are generic and serve D4's step 3 as well. 2d uses the same mechanism as D4's 6c. The
Survive colour and host hotkey fixes are already in (below), so nothing waits on the port.

## Stage 2e, broken down (written 2026-09-25 from FOX's code)

Read from `PlayerCatalog::builderSource` (`PC.cpp:2143-2302`), `rebuildWeapon`
(`CTW.cpp:3667-3945`), `attachPartTo` / `attachMatrixFor` (`CT.cpp:3572-3656`) and
`modelload::keepBoneSubtree` (`ModelLoader.cpp:1769`). FOX's own tab keeps running until the last
part. The core tab runs beside it for comparison, behind a harness flag, not in the normal UI.

| Part | What | Status |
|---|---|---|
| 2e-1 | **Core seams** FOX needs and the core lacked: seating on a named host's hardpoint (`Piece::seatOn`), the head-only base (`Look::keepSubtree`) | built (below) |
| 2e-2 | **`FoxCustomizeSource`, Character category** (`PlayerCatalog`), covering: subjects and groups; variants (the variant slot's parts, the character's own body first); per-subject slots and captions; defaults ("what the game dresses it in"); companion garments as a second piece; covered hair when headgear is worn; the skeleton-only base (hidden once a head is fitted, head-only before); anchors; MGO Exclude/Must as `applyRules` | built (below); first real run pending |
| 2e-3 | **Weapons** (`WeaponCatalog`): attach plans → `seatOn` (host slot → piece label), fit narrowing (`EquipCatalog::compatibleStems`) → `compat`, unusable slots, named weapons and star tiers | built (below); real run 19 passed |
| 2e-4 | **Vehicles and buddies** (`MechaCatalog`, measured seat table), all other characters (`CharacterCatalog`), free-form Parts | vehicles and other characters built (below); real runs 20 and 21 passed. Free-form Parts: a user decision (below) |
| 2e-5 | **Looks through `FoxStore::resolveLookedMaterials`**: `.fv2` variations (textures, groups, brought models), gear / slot colour, MGO dyes, the avatar look (presets, eye shape as a model swap, skin tones). FOX's order: variation first, look on top | split in three (below): **2e-5a** the build's Camo / Variation and Gear Color rows — built, container-tested; 2e-5b Survive slot colour and MGO dyes; 2e-5c the avatar look rows and skin tones |
| 2e-6 | **Rules measured on the assembled figure**: sleeves (`SleeveFit`), neck, hair shape, bandanna. Needs a post-assembly source hook, designed when this part starts | |
| 2e-7 | FOX-only extras: dirt splats, gear icons, legacy presets / outfits / `.foxbuild` import (`legacyPresets`) | |
| — | **2d, FOX's shader** as its `ViewportExtension`, before anything retires | |
| — | **Retire FOX's tab** when FOX's customize bat checks and `Verify - Regression.bat` customize items pass through the core tab | |

**Measured differences to keep in view.**
- FOX never merges skeletons: each part keeps its own, posed by bone hash. The core merges by
  name, which matches wherever a part's bone binds equal the figure's. Where they don't, 2b's
  `alignRoot` handles FOX's root-at-origin fragments.
- FOX's seat hop is `R(q)·T(pos)` on the bone's bind translation. The core uses the hardpoint's
  full TRS. FOX ignores the connect point's scale. Whether any shipped `.fcnp` has a non-unit
  scale is **not measured yet**; measure it before weapons go live.
- `FoxStore::hardpoints` now also matches a point's parent bone by StrCode32, as FOX's
  `attachMatrixFor` does (CT:3622). It logs `fcnp: <file> <point> has scale …` the first time a
  shipped point has a non-unit scale, so the scale question is answered by the next real run.

## Status — stage 2e-5a (the build's Camo / Variation and Gear Color rows) built (2026-09-26)

Built and container-tested; the first real run is the bat's `fox_core_looks`. Read from FOX's
`applyFovaToPart` (CT:4882-5075), `refreshWeaponCamoList` (CTW:2890-3015), `refreshWeaponColorList`
(CTW:3512-3557) and `modelload` (ML:25-76, ML:811-858).

- **Two look rows on every page**, after the part rows: `look:camo` ("Camo / Variation", row 0
  "— default —") and `look:gearcolor` ("Gear Color", row 0 "— as shipped —"; no row when the
  install has no layer swatches, as FOX hides it). Neither is filled by "Surprise me" (FOX's fills
  part rows only) nor by a set.
- **The variation list** is FOX's: the variant's variations (a player character's fitted parts'
  too), one entry per NAME, sorted, under CAMO PATTERN / BASE COLOR / SKIN TONE / MODEL VARIATION
  (`camoSectionFor`), labelled by the development list (`camoLabelFor`). A weapon lists its MODEL
  VARIATIONs only and opens on "clv" when it has one; "— none (the model's own textures) —" is then
  one pick away, which is FOX's "— default —".
- **Applying one is per model**, as FOX's is: each part (the variant, every piece, a companion half,
  covered hair) looks the NAME up among its OWN stem's variations. The source hands the store the
  .fv2 path (`MaterialLook::engine["fova"]`), and the .fv2's hidden / shown mesh groups become
  `Look::hide` / `show` (group names, from the model's own group table). Models it brings (bone form,
  in this install) are pieces `broughtBy` the part, skinned to the wearer as authored; one it cannot
  bring is said. `Piece::look` was already the core's (2c); nothing core-side was needed but one line:
  a look row no longer gets the fit rule's tooltip.
- **Gear Color** is the layer swatch's path (`engine["layercolor"]`), on every part and on what a
  variation brings (FOX paints those with the colour only, ML:845). The row's chip is the swatch's
  own colour (`LayerColorCatalog::colorOf`).
- **`FoxStore::resolveLookedMaterials`** does what `applyFovaToPart` does: ONE override table in FOX's
  order — `fovaOverrides`, then `layerColorOverrides` over it — and the three loaders
  (`loadBaseTextures` / `loadNormalMaps` / `loadPbrMaps`) through it in the model's own install, then
  the same glTF mapping `resolveMaterials` uses. It says per piece what it did
  ("<file>.fv2: N texture substitution(s)", "gear colour painted N of M paintable material(s)"), and
  says an input it does not know.
- **Both rows outlive a new subject or tier**, as FOX's do: the gear colour always, the variation
  when the new list has it.
- **Checks:** 12 new `shell_check` assertions (the rows, a weapon's list and default, per-model
  lookup, groups, brought models, none, gear colour everywhere, the chip, what a new subject keeps,
  a vehicle's four captions, a player character's part tables, the tab's rows). Mutation-verified:
  keeping every caption on a weapon fails 1, dropping the gear colour on a new subject fails 2.
- **Run 22:** `fox_core_looks` ran the path on the ambulance: `mfv_camo_c00` substituted 0 textures on
  every piece and nothing on it is paintable (0 of 0) — a civilian van, so the check proved the path,
  not a look. It now uses the armoured vehicle, and `fox_vehicle_camo` puts the same camouflage on it
  in FOX's own tab for comparison.
- **Not the same as FOX, on purpose or for now:**
  - The variation rows show no image chip (FOX draws the game's own UI icons); the core's Option
    carries a colour, not an image.
  - A brought model's shipped-hidden groups stay hidden; FOX draws them (its attachment path skips
    `dropGroups`, CT:2041).
  - A look's group names also reach the groups BELOW the named one (the core's 2c rule); FOX's
    `dropGroups` moves the named group's own meshes only. Not measured on a shipped .fv2 yet.

**Measured on the way: the core hides lower LODs FOX draws.** FOX hides a mesh by its own group's
flag only (`FmdlFile::shippedHiddenGroups`, `dropGroups`); the core's `FoxAdapt` also hides a mesh
under a hidden parent group. On the container's real test tree (616 models from the FOX project's
`_probe` pulls) that differs on 13 meshes in 2 models, and every one is a lower level of detail:
`tnk0_main0_def`'s `MESH_LV2_IV` / `MESH_LV3_IV` and `bu53_main0_def`'s sling LODs, whose parent groups
ship hidden and whose own groups do not. FOX draws them over LOD0; the core does not. The core's rule
stays.

**The container can run FOX now.** The FOX project's pulls (`_archive/work/_probe`: `sklpull`,
`mgofmdl`, `pulls/gearmdl` + `geartex`) and its `dict/` make a 1,570-file loose install; the app
runs on it headless with `--game <an empty folder> --loose <tree> --dict <dict>` and an isolated
`XDG_CONFIG_HOME`. It has player and other characters but no weapon packs, vehicles or layer
swatches, so 2e-5a's real check stays the user's run.

## Status — stage 2e-4 (FOX vehicles and other characters) built (2026-09-26)

Built and container-tested; the first real run is the bat's `fox_core_vehicle` and `fox_core_other`.

- **Two more categories, FOX's order.** `FoxBuilderSource::categories()` now lists, of what the
  install has: Weapon, Character, **Vehicle & buddy** (`vehicle`, nouns "Vehicle" / "Body") and
  **All other characters** (`other`, "Character" / "Body"), as FOX's category combo (CT:76) and the
  two catalogues' `builderSource` name them.
- **The same page as the weapon families** (CTW:659), so `FoxCustomizeSource` now fills all three
  from one pair of helpers (`builderRows`, `addFamilies`):
  - one subject per asset directory (`vehicle:<dir>`, `other:<dir>`), its body models the variants;
  - every other slot is a row, a slot with no parts in this install left out, and FOX's second
    sight / option rows asked for on every such page (only weapon slots have them);
  - row 0 is the subject's own part (`ownPartFor`, FOX's `applyFamilyDefaults`), "— none —" one
    pick away, as on the weapon rows.
- **Seating.** Vehicles: `MechaCatalog::attachPlanFor` (MC:132) becomes `seatOn`, as the weapons'
  plan does — a gun tries the turret, then the arm, then the hull's own points. Other characters
  have no plan (CC:269): their parts share the skeleton with FOX's rest alignment.
- **A new head takes the hair off** on the other-characters page. FOX's head pick sends the hair
  row back to its row 0 (CTW:2797); on this page row 0 is "— none —", so the hair comes off. This
  mirrors FOX; the player characters (2e-2) keep their "face's own hairstyle" rule.
- **Harness: `subject=@<n>`.** The n-th subject of the build's category (`CustomizeTab::subjectIdAt`),
  so a check need not name a subject this install may lack. An out-of-range one is named and no
  pick is resolved against another subject.
- **`describe()`** counts vehicles and other characters apart from the player characters.
- **Checks:** 8 new `shell_check` assertions (195 in all): category order and nouns; a vehicle's own
  parts, none row and plan; the gun on the turret's point and, with no turret, on the hull's; other
  characters' shared-skeleton parts and the hair rule, in the source and on the tab; the harness's
  `subject=@<n>`. Mutation-verified: disabling the hair rule fails 2, breaking `subjectIdAt` fails 1.
  Two bat checks, `fox_core_vehicle` and `fox_core_other`, open `@1` of each page. Their needles want
  the body decoded but no part fitted, because which vehicles own parts on this install is not known
  yet; the logs' "vehicle ids" / "other ids" lines and seat notes will say, and the checks tighten
  after that run.

**Run 20: 46/46.** Both pages open on real data.
- The install has 42 vehicles (57 rows: FOX gives every unmapped model kind its own row) and 146
  other characters (5 rows).
- `subject=@1` opened `vehicle:amb`, the ambulance: textured, 6 pieces, `Glass — seated at
  CNP_WIND0` by the plan. Its Belt, Hmic and Reci pieces (FOX's own row names) bring bones the hull
  lacks, added under their own parents. Two dark fragments float above the ambulance in the shot. Which
  piece they are, and whether FOX draws them too, is **not known**: the next run shoots the same
  ambulance on FOX's own tab (`fox_vehicle`) to compare.
- `other:avm` opened on a female body (`avf0_body0`, the directory's first) with the male head and
  hair (`avm0_type0`, `avm_hair_a0_v0_cov`), each the directory's first part. That is what
  `ownPartIn` picks in FOX too; `fox_other` shoots FOX's own tab for comparison. The hair draws
  white until 2e-5.
- The checks now name what run 20 showed: the ambulance with its seated glass, and `@1` of the
  other page with a piece fitted. `fox_core_wav` opens the wheeled armoured vehicle, to read whether
  its gun seats on its turret.

**Run 21: 50/50.** FOX's own tab, on the same two subjects, builds what the core does:
- The ambulance: 7 parts in FOX (the hull and the same six rows: Glass, Belt, Hmic, Rdor, Reci, Wb),
  and FOX's log says `seated: glass→CNP_WIND0`, the core's one seat. The avatar: 3 parts in FOX (the
  same `avf0_body0` body, `avm0_type0` head and `avm_hair_a0_v0_cov` hair); FOX's hair is textured
  by its look, the core's waits for 2e-5.
- FOX draws detached pieces on the ambulance too: grey glass shards (`amb0_wb00_bre`, a broken
  windscreen by its name) and a cord hanging below. The two cameras frame the vehicle differently,
  so the shots cannot say whether the pieces sit in the SAME place. The next run exports both
  ambulances as .glb (`fox_core_veh_export`, `fox_vehicle_export`) to compare them mesh by mesh.
- `vehicle:wav` owns a turret only (its gun is part of the turret model), seated at `CNP_TURRET`;
  its check now wants that seat. No vehicle on this install has yet shown a gun seated on a turret.

**Run 22 (54/54): the two ambulance .glb files, mesh by mesh.** Every primitive's skinned bounds in
the core's export match FOX's scene export to the centimetre — 23 of 24 — so the core places the
ambulance's pieces exactly where FOX does. The 24th, the broken windscreen (`amb0_wb00_bre`), has no
skinning at all: the core wrote it collapsed onto the origin (all weights zero on a skinned figure).
Fixed in `assemble`: such a piece is held by the figure's root, as authored, and says so (a new
`shell_check` assertion, mutation-verified).

What the comparison also shows is that FOX's own placement of four of the six is questionable, and
the core copies it:
- **Glass** (`amb0_glss0`) is authored in the hull's space, at the windscreen, and FOX's plan then
  seats it at `CNP_WIND0` (0, 1.82, 1.61): it lands 1.8 m above the roof, in both exports. These are
  the "dark fragments above" run 20's shot showed.
- **Wb** (the broken windscreen), **Hmic** (a hanging cord) and **Reci** are authored around their
  own origin and have no plan entry, so both tools leave them at the hull's origin. The hull's own
  .fcnp names points for exactly these parts: `CNP_HMIC`, `CNP_RECI`, and `CNP_amb0_wb01_bre` …
  `wb14_bre`, beside `CNP_WIND0`.
- **Rdor** (the rear doors) sits near the origin too.
Seating them where the hull's point names say would be a step BEYOND FOX's tab. **The user chose
that (2026-09-26): go beyond FOX — seat parts at the vehicle's own same-named points, and leave a part
that is modelled in place where it is.** The rule is read from the data first: the harness flag
`--vehicle-census <tsv>` (`games/fox/FoxCensus`) writes every vehicle's own parts (authored bounds,
skinned or not, FOX's plan for the row) and every connect point the body and those parts offer, in
model space; the bat's `fox_vehicle_census` runs it on the whole fleet (42 vehicles on this install).

**Run 23: the census, and the rule read from it.** 42 vehicles, 77 body models, 80 own parts, 1,048
connect points. How far the origin lies OUTSIDE a part's bounds separates the two kinds:
- **authored at its own origin** (needs a seat): 52 parts hold the origin; the offset-pivot ones sit
  within 0.1 m of it (an ammunition rack 0.075 m, a cockpit 0.096 m). Every glass pane FOX seats on
  `CNP_FRONTGLASS` / `CNP_window_a` is one of them, so FOX's plan is right for those.
- **modelled in place** in the hull's space: every part FOX's plan seats that is one of these lies
  0.64–2.14 m from its origin — the ambulance's windscreen (2.04 m, and FOX's `CNP_WIND0` moves it a
  second time), the helicopter's arm, the armoured vehicle's turret (its `CNP_TURRET` is at the origin,
  so FOX's seat moves nothing there), the SAV's gun, the helicopter's shields.
**The rule** (`FoxBuilderSource::seatVehiclePart`, vehicles only): a part 0.25 m or more from its
origin is modelled in place and is NOT seated; a part at its origin that FOX's plan has no row for
takes the body's point named `CNP_<ROW>` when the body has one. Applied to the census it changes
four parts and nothing else: the ambulance's glass (not seated), hanging mic (`CNP_HMIC`) and radio
cord (`CNP_RECI`), and the armoured vehicle's turret (not seated; no move). Each change is said in
the notes. Two `shell_check` assertions, mutation-verified (disabling the rule fails both).

**Still open (hunches, not built):** a few bodies carry NUMBERED families of points for one part —
`CNP_FOTO0_0…`, `CNP_MEMO0_0…` (a photo and a memo on the AI pod), `CNP_GRND_LA…RC`, `CNP_MSBL_L/R`, the
ambulance's `CNP_amb0_wb01…14_bre`, and twelve `CNP_AMMO_A…L` on the SAV's gun. They look like one
model drawn at every point; the core draws a piece once. The ambulance's broken windscreen (`Wb`,
authored at its origin) has no point named for its row, so it stays at the origin, as in FOX.

**Not done: FOX's "Parts (free-form)" page.** It is FOX's first category: search every model in the
archives, "Equip part" adds it to an equipped list, each part keeps its own skeleton, and outfits
are saved by name. It has no subject, no body and no slots, and the core tab is built on all three.
**The user chose (2026-09-26): a core feature on the core Models tab** — a list of freely added
models, shared by every game. Designed when its turn comes. The options were:
- keep it in FOX's own code as a small tab of its own when FOX's Customize tab retires;
- make it a core feature (a list of freely added models), most likely on the core Models tab, which
  already shows a model's own attachments with it;
- or a core Customize category with open rows (a new seam; heavier, and a poor fit for the tab).

## Status — stage 2e-3 (FOX weapons) built (2026-09-25)

Built and container-tested.

**Run 17 (2026-09-25):** `fox_core_weapon` failed, and the cause was the check, not the source.
- It asked for `named:WU S.PISTOL`, from a note in FOX's `EquipCatalog.h` measured on other data.
- On the user's install FOX's EquipCatalog reads **0 weapon builds**: every FOX log says `equip: 0
  game weapon build(s) (0 named, 0 families)`. Its compatibility tables do load (168 receivers,
  69 barrels). So there are no named weapons or tiers to offer, in FOX's own tab or the core's.
- The core found the install's 37 receiver families and 8 rows.
- The check now builds `family:ar00`, which that log lists.
- Why the builds read as 0 is **unexplained**. It is FOX-tree behaviour, not investigated yet.

**Run 18:** `family:ar00` assembled, but as the bare receiver (0 pieces).
- On this install no part carries the ar00 family prefix, so it has no own parts, and row 0 is none
  everywhere. That is correct behaviour; the check was what was wrong.
- The harness build text now takes `slot.<id>=@<n>`, the n-th option of that slot's list as the tab
  shows it. It has a `shell_check` assertion.
- The check fits `@1` for the barrel, the magazine and the muzzle, so it names no part.

**Run 19: 44/44.** The ar00 (MRS-4) receiver was built with the MRS TYPE BARREL, the 5.56 MAG x30 and
the MRS TYPE MUZZLE. The log shows the plan working on real data:
- `Barrel — seated at CNP_BARREL`, `Magazine — seated at CNP_AMMO`, and `Muzzle — seated at
  CNP_MUZZLE_OPTION on the Barrel`, the chain FOX measured (WC:287).
- No `fcnp: … scale` line, so none of these points carries a scale.

The receiver and barrel draw untextured white. Their camouflage / paint layer (2e-5) and FOX's
shader (2d) are not in yet. The magazine and muzzle, which do not use that layer, are textured.

- **One source for both categories.** `customize/FoxCharacterSource` became
  `customize/FoxBuilderSource`, after FOX's own `BuilderSource`. It holds the characters' rules
  and the weapons'. Weapon is the first category, as in FOX; its variant row is "Version".
- **Subjects:**
  - The game's named weapons come first, by class in the customize screen's order. Each is a
    `named:<name>` subject whose variants are its star tiers (`grade:<n>`, stable in the data).
  - Every receiver family in the data follows (`family:<id>`); its variants are its receivers.
- **Row 0 is what FOX fits.** For a tier, that is the tier's part for each row. For a family, it is
  its own part (`ownPartFor`). FOX's weapon rows keep "— none —", so a none row follows the part.
  A tier part this install lacks is said in the notes, through the new `Resolved::notes`.
- **Seating:** `attachPlanFor` becomes `Piece::seatOn`. The host SLOT becomes that slot's piece
  label, and each part keeps its own rig (`SocketRig`), so the barrel → muzzle → suppressor chain
  seats as FOX's does. A part with no plan shares the skeleton, as FOX's does.
- **Fit rule and crossed-out slots:**
  - From `EquipCatalog`: `compatibleStems` (an empty answer means NO rule), `hasBuildsFor` and
    `slotEverUsedOn`.
  - A crossed-out slot offers nothing, holds nothing, and says why.
- **New core seam: `ICustomizeSource::followBase`.** It is what a user's new subject or variant
  does to the rows:
  - FOX: a new weapon or character opens on its own dress.
  - A star tier is a whole build, so it re-fits every part.
  - A new receiver keeps the parts, as FOX does (CTW:4617).
  - POE2 keeps the default, which leaves the picks as they are.
- **Also in the core:**
  - `Category::variantNoun`, so the variant row says "Version" or "Body" per category.
  - The subject noun in row 0's tooltip now follows the category.
  - Each hardpoint-seated piece logs its seat.
  - `pickSubject` / `pickVariant` for tests.
- **Checks:** 14 new `shell_check` assertions (186 in all). Both core seams are
  mutation-verified: reverting `followBase` fails 2 checks, and reverting `Resolved::notes`
  fails 1.
- **Differences from FOX:**
  - A tier's parts are matched by STEM in the slot's list. FOX matched the first file with that
    stem by index, which could miss a part the list holds under another copy.
  - Camouflage and paint are 2e-5.
  - FOX's receiver-less build is refused with a message, as for characters.

## Status — stage 2e-2 (FOX characters) built (2026-09-25, overnight)

Built and container-tested; **first real run: run 15 (2026-09-25), MSVC-built, `fox_core_customize`
PASS.** Survivor — Female: the Face Preset, Inner, Arm and Leg defaults all placed where FOX's own tab
puts them, and the skeleton-only base was hidden. Two differences from FOX's harness shot:
- **No face textures.** That is the face preset's look (2e-5) and FOX's shader (2d); both expected.
- **No hair.** FOX's tab disagrees with itself here. Its UI opens a character with Hairstyle
  "none" until a face is picked (`onWeaponPicked`, CTW:4436, never applies the preset's hair),
  while its `--character` spec path applies the default face preset's hair afterwards
  (CTW:4199). **User decision (2026-09-25): the preset's hair.** The hairstyle's row 0 is now the
  worn face preset's own hairstyle, so it is the default when a character opens, after a new face,
  and after Clear slots. A "— none —" row keeps no hair one pick away. A face with no preset grid,
  or a preset naming none, has no hair as row 0. `followPick` now clears a picked hairstyle, so the
  new face's own follows. 4 more `shell_check` assertions (172), mutation-verified.
  `optionsDependOnSlots()` is now true, because row 0 changes with the face.

The core tab runs beside FOX's own only with `--core-customize "<build text>"` (tab "Customize (core)"); the bat's
new `fox_core_customize` check builds `ssd_f` there, next to `fox_character`'s build of the same
character in FOX's tab, so the two logs and shots compare.

- **Two halves.** `games/fox/customize/FoxCharacterSource` (now `FoxBuilderSource`, 2e-3) holds the rules and knows nothing of
  FOX's tree: it answers from a catalogue snapshot, so `shell_check` tests it on a synthetic one.
  `games/fox/FoxCustomizeSource` fills the snapshot from `PlayerCatalog::builderSource()` on the
  GUI thread (`prepare()`), and supplies three lookups: an id per file index, a connect point from
  the `.fcnp` beside a model (cached per index), and a face preset's hairstyle (`AvatarPresets`).
- **Ids are asset paths**, as FOX's own build text writes them (CTW:5285). A face preset row is
  `<path>#<n>`, because several presets share one head model. Builds say `category=character`.
- **Rules, each from FOX's code:**
  - Row 0 is the default part, and it is not listed again. A face-preset slot opens on its first
    preset.
  - A garment's second half is its own piece right after the first ("<slot> Top"). It has no
    anchor, as in FOX: FOX adds it with no slot.
  - The covered hairstyle is used while any headgear slot holds something. That includes a
    default; accessories don't count.
  - The skeleton-only base: `bodyLook.keepSubtree = SKL_004_HEAD` while no head is fitted, and
    every body part hidden once one is. A head slot's row 0 counts as fitted.
  - Every character part has `alignRoot`.
  - Anchor slots set `socket`. A part rooted at a bone nothing carries is seated on the slot's
    connect point, found on the fitted parts in FOX's order with the base first. It sits on the
    point's own parent bone, at the point's position, **translation only** as FOX adds it
    (CT:3828). With no point it goes on the slot's bone (exit 'a').
  - MGO Exclude / Must: `applyRules`. Must runs before Exclude, cascades one level, and puts on a
    required item that is a slot's default as row 0. An Exclude clash whose slot default clashes
    too is left in place and said. Unlocked only says what Exclude would clear, as FOX does.
- **New core seam: `ICustomizeSource::followPick`.** It holds what a pick changes elsewhere in the
  game's own editor, and Unlocked doesn't switch it off. FOX: a new face sets the hairstyle to
  that preset's, matched on the style. With no preset grid, a new face clears the hair (CTW:2754).
- **Two core fixes found on the way:**
  - The slot export now takes the piece the full build fits. The one-slot build is only the
    fallback for a piece the full build hides. Before, a hat's covered hair exported as the
    plain model.
  - The harness `--build` text now carries its category and variant. An absent variant means the
    subject's first. Before, after a subject change the old subject's variant was kept and read
    as missing.
- **Checks:** 20 new `shell_check` assertions. The core's three are mutation-verified (reverting
  each fails its check). 168 in all, all pass; `run_tests.sh` and `verify-src.py` are clean.
- **Known differences from FOX, and still open:**
  - The core's exit 'b' measures against the figure built so far: the body, then earlier
    InPlace pieces. FOX measures against the fullest skeleton among all parts. They agree when
    the base carries the fullest rig, which Survive's and MGO's skeleton bases do.
  - Exit 'c' in FOX falls back to the next part carrying the point, or to the slot's bone, when
    the point's parent bone is in no rig. The core places such a part unbaked and says so.
  - Bones are matched by NAME. FOX matches by StrCode32. A bone the dictionary does not name
    reads "0x…" and would not match; the notes would say so.
  - Not measured: which way FOX's characters face the default camera (`firstViewYaw` is left at
    0), and whether the anchor names resolve on the user's install.
  - Not in 2e-2:
    - the face preset's look rows and eye shape (2e-5);
    - the Hair shape row, sleeves and neck (2e-6);
    - FOX's legacy presets (2e-7);
    - a base-less install, which is refused with a message where FOX stands the parts on their
      own skeletons.

## Status — stage 2e-1 (core seams) built (2026-09-25)

- **`Piece::seatOn`** (`SocketOption {host, hardpoint}`): FOX's attach plan, generic.
  - **Seating:** options are tried in order. The first wins whose host piece (or the figure) is
    already fitted, carries that hardpoint (exact name, as `FcnpFile::find`), and hasn't given it
    to another piece.
  - **Order:** seated in list order, once, as FOX's rows are. A host listed later counts as not
    fitted.
  - **Placement:** the hardpoint's bone is found in the host's own rig through where that joint
    landed in the combined rig. Bone-local points use `local · bone`; model-space points (D4) use
    `local · inverseBind · bone`, the same rule as `GLModelWidget::hardpointWorld`.
  - **Rigs:** a `Socket` piece stays rigid. Any other placement keeps its own rig, so a later piece
    can chain onto its points.
  - **Nothing seats it:** it stays where it was authored. It counts as seated when it has no
    hardpoints of its own ("in the figure's own space", FOX's door armour); if it has some, it's
    unseated and said.
  - `PieceRange::seatNote` carries FOX's per-row wording into the slot tooltip.
- **`Look::keepSubtree`** is FOX's head-only rule. A triangle stays when any corner's heaviest bone
  (first of equals) is in the subtree. A part with no skinning goes entirely. A rig without the
  bone leaves the mesh whole, and this is said. It applies to the body, the pieces, and a piece
  exported alone.
- **Checks:** 6 `shell_check` assertions (SynthCustomize5): a figure point, a two-deep chain, one
  piece per point, list order, the seated and unseated fallbacks, and head-only plus its missing
  bone.

## Status — stage 2c built (2026-09-25)

A look (variation, dye, swatch, skin tone) is an answer the source gives per piece in `resolve()`.
The tab only applies it.

- **`Piece::look` and `Resolved::bodyLook`** (`Look`, CustomizeSource.h):
  - `material` (`core::MaterialLook`, in Material.h) holds texture swaps (source path →
    replacement) and `engine`, text inputs only the game's own code reads (FOX: a layer swatch).
  - `hide`, `show` and `forceHide` are group names, matched case-blind on a part's name or any
    group above it.
- **FOX's visibility rule, per part:** `((shipped hidden ∪ hide) − show) ∪ forceHide`. It is
  applied at assembly. Each part's flag then holds the shipped-and-looked state, with group flags
  folded in.
- **One visibility function.** `partsShown()` combines that state with the game's hidden body
  parts and the PIECES ticks. The viewport (`applyVisibility`) and the whole-build export both use
  it.
- **Fixed on the way:** `applyVisibility` used to set every piece part to its tick. That showed
  parts the game ships hidden, which the export left out. POE2's and D4's loaders never mark a part
  hidden, so they look the same as before. FOX's authored alternatives are what this changes.
- **Materials go through the store:** the new `IGameStore::resolveLookedMaterials`.
  - The default applies nothing and says so ("this game's store does not apply looks yet").
  - The result is cached per (asset, `look.key()`), so a look swapped back and forth decodes once.
  - The store's word is logged per piece ("coat — look: 1 texture(s) swapped").
- **Models a look brings** are pieces with `broughtBy`.
  - They are shown as "(with <owner>)" in PIECES.
  - One is skipped, and this is said, when its mesh is already the body, a piece of the build, or
    brought earlier. FOX matches by path.
- **A group name a mesh does not carry is said**, not silently ignored.
- **A piece exported by itself carries its look.** "Each piece" and the slot menu still write the
  asset in its own space and rig, through `model::exportOne`. Its new `adjust` hook calls
  `customize::applyLook`, which is the same group rule and store call as assembly.
  - `CustomizeTab::shownPieces()` is what Each piece and Original files write. It leaves out a
    brought model that is already fitted, so no mesh is written twice.
  - `slotPiece()` is what the slot export writes. It resolves the slot alone as before, but keeps
    the look rows. Before, it dropped them, so the look was always empty.
- **Checks:** 13 `shell_check` assertions (SynthCustomize4). They cover each branch above, and
  compare the viewport with the export's primitive count, both for the whole build and for a
  piece alone.

**Left to 2e:** FOX's `FoxStore::resolveLookedMaterials` (a `.fv2`'s table, gear, slot colour, MGO
dye, the avatar look, in FOX's order: variation first, look on top). The neck, sleeve and hair
rules, which are measured on the assembled geometry, also go there.

## Status — stage 2b, first part, built (2026-09-24)

`core::customize::assemble` now builds ONE rig that pieces can grow:

- **InPlace = merge by bone.** Weighted joints are bound to the figure's by name. A joint the
  figure lacks is added with its missing ancestors, under its own parent, at its own bind. The old
  fallback kept raw indices, which drove whichever body bone sat at that number. This is D4's
  `mergeGeometries` rule and what FOX's partial rigs need.
- **`Placement::SocketRig`.** On a socket, but the piece KEEPS its rig: its bones hang under the
  socket bone, re-bound so the bind pose reproduces the placement, and its weights stay on them.
  This covers D4's `attachSubRigAt` (flails, animated trophies) and FOX parts with sub-rigs.
- **`Piece::socketOffset`.** The socket's own transform in its bone's space (a POE2 `.ao` socket's
  translation, a FOX connect point, a D4 hardpoint). POE2's rotations still wait on the Euler
  order (CUSTOMIZE_POE2.md §12).
- **Chains.** A socket can name a bone an earlier piece brought. Sockets resolve in passes, so
  list order does not matter.
- **Unseated state.** A socket nothing provides leaves the piece where it was authored, on the
  root, marked `PieceRange::seated = false`, and named in the notes. Its slot label turns amber
  (FOX).

6 new `shell_check` assertions cover these. **Behaviour change to watch on POE2:** in-place pieces
with bones the class rig lacks are now grown into the rig instead of keeping raw indices (logged
as "… were added under their own parents"). Socket pieces are now assembled after the in-place
ones, so the PIECES panel lists them last.

**2b, second part (same day): FOX's rest alignment as `Piece::alignRoot`.** Read from
`tabs/CustomizeTab.cpp:3658-3856`. A FOX part that ships only the bones it needs roots its
topmost bone at the origin.

| FOX exit | Condition | Core behaviour |
|---|---|---|
| `b` | the figure has a bone of the root's name | the piece moves so its root lands there |
| none | that figure bone is itself a figure root | nothing is corrected: FOX's hat31 hood, which roots at WAIST like the body, was pushed half a metre up before FOX learned this |
| `c` / `a` | the figure has no such bone and the source gives a socket (connect point or anchor bone) | the piece is seated BY ITS ROOT on that socket and keeps its rig |

`f` (a forced offset) is a FOX debug switch and is not carried. 3 new `shell_check` assertions
cover the kept exits.

**Deliberately left to `FoxCustomizeSource` (2e), not core:**

- **Host election** (the part with the most bones and ≥ `kMinHostBones` drives the pose). In
  core the source names the body; FOX's source will name the fullest skeleton.
- **Anchor / connect-point lookup**: `anchorBoneFor`, `anchorCnpFor`, and the point's OWN parent
  bone.

**A known difference, to measure on real FOX parts:** FOX poses a fragment's extra bones from the
clip by bone hash, per part. In core, a clip is baked against the figure's skeleton, so bones a
piece appended ride their parent rigidly unless the clip names them.

## Status — stage 2a built (2026-09-24)

`ICustomizeSource` has grown the following, all optional. With none of them answered, POE2's build
text and tab are unchanged.

- `categories()` and `Subject::category`: a Category row, subjects filed per category, and the
  last category remembered.
- `variants()` and `variantNoun()`: a base row, stored as `Build::variant`.
- `SlotKind::Look` and `SlotDef::noneLabel`: look rows under their own Look caption, with no
  export button and not counted by Find.
- `Option::section` and `Option::swatch`: unselectable caption rows and colour chips.
- `defaultLabel()`: row 0 reads "— default — <what the game dresses it in>".
- `hasRules()` and `applyRules(build, changedSlot, enforce)`: run on a user pick only. A preset,
  undo or paste restores exactly what was saved. "Unlocked" makes the rules describe instead of
  act.
- `hasCompat()` and `compat()`: "Only what fits" narrows a list to the allowed ids plus the
  current pick. A rule that would empty a slot keeps the whole list. The label's tooltip says
  which state applies (rule off / no rule for this / showing the N that fit), and a slot never
  used on this base is struck through.

The build grammar gained `category=` and `variant=`. `Build`'s self-test covers both. Unlocked
and Only-what-fits are working preferences stored in settings (`<settingsKey>/unlocked`,
`/compatOnly`), not part of a build, as in FOX. Harness/test API: `listRows`, `pickOption`,
`setUnlocked`, `setCompatOnly`. 13 new `shell_check` assertions over a two-category synthetic game
cover every branch above.

## Status — step 1 built (2026-09-23)

- **Core:** `core/customize/` (`Build` — the text grammar, with a `--check` self-test;
  `CustomizeSource.h` — the interface; `Assemble` — a thread-safe mesh cache and the one assembly:
  InPlace joints bound by bone NAME, Socket baked at the bone's bind, ReplaceBody with the form's own
  rig and clips, `held` pieces for "without held items") and `core/tabs/CustomizeTab` (slot rows,
  Find with per-slot counts, sets, toggles, Clear / Surprise me, build text for presets, undo/redo
  (diffed settled builds, 30 deep), clipboard, an owned assembly worker with a generation token,
  playback kept across rebuilds, lazy clips baked on demand, PARTS / ANIMATIONS / PIECES / INFO,
  every export). Harness: `core::harness::applyCustomize` (`--build <text>` / `--build random:<seed>`,
  `--build-export <glb>`).
- **POE2 on it:** `games/poe2/Poe2CustomizeSource` holds exactly the old tab's rules; the old tab is in
  `_retired/poe2_tabs/`. Its old presets (`customize/presets`) are imported once into build texts
  (`customize/builds`), the old key left in place.
- **Behaviour changes, deliberate:** joints bound by bone name (was raw indices — identical when the
  rigs list the same bones in the same order; logged when it changes anything); assembly on a worker
  (was the GUI thread) with a cache that is not wiped at 96 entries; the playing clip survives a
  rebuild; a shapeshift form plays its OWN rig's clips (the old tab played the class's clip on it);
  parts hidden in the viewport are not exported; per-piece and raw exports write what is shown.
- **Checks:** 13 synthetic assertions in `shell_check` (name remap, socket bake, form, undo/redo
  depth and order, build-text round trip, hidden-body rule, both whole-build exports); a static
  audit found 6 defects (worker lifetime, held filter, per-slot export under a form, undo of a
  pending change, stale subject framing, Find dimming) — all fixed before delivery. Real install:
  `poe2_build` (Warrior assembles) and `poe2_build_rand` (seed 7, exported) in Build and Verify.bat.
- **Known, not changed:** Ctrl+Z pressed while a slot list's text field has focus goes to that field
  (Qt gives an editable field the Undo key); the Undo/Redo buttons always work.

## Found on the way (fixed or to fix)

- **D4 Wardrobe, fixed 2026-09-23:** `exportNameStem` read `m_slot[5]`/`m_slot[6]` (a 5-element
  array) for the `{{Main}}`/`{{Off}}` filename placeholders — undefined behaviour on every outfit
  export using the template; now `slotCombo()`. D4 builds; verify-src clean.
- **FOX, fixed 2026-09-24 (ahead of step 2, in the hosted tab):**
  - **Survive's per-slot colour now persists.** `captureSceneFields` writes
    `slotcolor:<slot>=<swatch path>` and `applySceneFieldsImpl` restores it, with an absent field
    meaning as shipped, as `gearcolor=` works. This is an edit in the FOX tree
    (`src/tabs/CustomizeTab_Weapon.cpp`), so the standalone FOX app gets it too.
  - **Hotkeys and export rows now reach the core shell.** `CustomizeTabHost` binds FOX's
    rebindable Undo/Redo rows. The core's Export rows delegate to FOX's (found by FOX's role in a
    scratch menu), FOX's duplicate rows are dropped from the real menu, and FOX's other roles are
    mirrored onto the core property.
  - Checked by `fox_slotcolor` (`--build-roundtrip`) and `fox_export_menu` (`--export-menu`, a new
    core harness flag that logs every Export row as `<role> on|off <text>`).
- **POE2, to fix at step 1:** a clip picked while a shapeshift is shown plays the class's clip on
  the shapeshift rig (`M:977-979`); in-place pieces keep raw joint indices (no rig check).
- **D4, noted:** `reset()` keeps weapon index and static caches across an install switch; dye
  changes are not undoable alone; `fur_probe.txt` written on every rebuild.
