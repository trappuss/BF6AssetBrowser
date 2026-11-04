# CUSTOMIZE — Fox Engine (MGSV / MGO3 / GZ / Survive)

This document describes FOXAssetBrowser's Customize tab as the code has it today. It is written
to be compared with D4's Wardrobe and POE2's Customize, and to help design a Customize tab for the
shared core that does not depend on any one game.

**Citation key.** Paths are relative to `/home/claude/work/fox/src/` unless they start with
`AB/`, which means `/home/claude/work/AssetBrowser/`. These short names are used throughout:

| short | file |
|---|---|
| `CT.h` | `tabs/CustomizeTab.h` (1,096 lines) |
| `CT.cpp` | `tabs/CustomizeTab.cpp` (5,104) |
| `CTW.cpp` | `tabs/CustomizeTab_Weapon.cpp` (6,254) — despite its name, this is the builder for **every** category |
| `CTB.cpp` | `tabs/CustomizeTab_Build.cpp` (375) |
| `PC.h` | `index/PartCatalog.h` (the shared vocabulary) |
| `PC.cpp` | `index/PlayerCatalog.cpp` (the Character category); its header is cited in full as `index/PlayerCatalog.h` |
| `SC.h/.cpp` | `util/SearchableCombo.*` |

**How the monorepo hosts it today.** `AB/games/fox/FoxPlugin.cpp:173` registers the tab as
`CustomizeTabHost`. That host wraps FOX's unchanged class, which still draws through FOX's own
viewport (`AB/games/fox/tabs/FoxOwnedTabs.h:1-6`, `FoxOwnedTabs.cpp:20-28`). The host forwards
three calls and nothing else: `onIndexReady`, `reset` and `populateExportMenu`
(`FoxOwnedTabs.h:25-31`). Moving the tab onto the core viewport is still open
(`AB/docs/PHASE5_REPORT.md:135-170`). The harness flags `--character`, `--weapon`, `--vehicle`
and `--camo` are passed through to `CustomizeTab::buildFromSpec` (`AB/games/fox/FoxPlugin.cpp:459-475`).

---

## 1. Purpose

The user builds one posable model out of several separate Fox model files (.fmdl) and looks at
it in one viewport. It can be a player character (Survive survivor, MGO3 avatar, Snake, Diamond
Dogs soldier, and so on), a weapon (a receiver plus up to ten attachments), a vehicle or buddy
(D-Walker, helicopter), or any free-form mix of models.

The user works in this order:

1. Pick a category and a subject.
2. Pick a variant (body, receiver or star tier).
3. Fill the subject's slots from lists discovered in the game's own data.
4. Set the appearance: face rows, skin tone, camo or variation (.fv2), gear colours and per-item
   dyes, sleeves, hair shape, dirt splats.
5. Play any animation clip on the assembled result.
6. Export it as .glb/.obj (the whole scene, one file per part, one file per variation, animated
   with several clips, or selected submeshes), or save it as a preset, outfit, clipboard text or
   `.foxbuild` file.

The game's own rules can be reproduced (items that cannot be worn together, required items, parts
that fit a given receiver), and each rule can be switched off. Sources: `CT.h:1-5`,
`CT.cpp:73-89`, `PC.h:1-9`, `CTW.cpp:376-407`.

---

## 2. Data model

### 2.1 The shared vocabulary (one per category)

`fox::BuilderSource` (`PC.h:113-160`) is the whole contract between the panel and a category. It
provides:

- **Labels:** subject label, variant label and an empty-list hint.
- **Slots:** `slotNames`.
- **Subjects:** `subjects`. A `CatalogSubject` (`PC.h:98-111`) carries an id, a group name, a
  display name, a flag for a hidden (skeleton-only) variant, the list of variants, and how many
  parts it owns.
- **Candidate queries:** `partsFor(slot)`, `ownPartFor(subject, slot)`,
  `variationsFor(modelStem)` and `attachPlanFor(slot)`.
- **Optional per-subject hooks** (`PC.h:127-156`), which make a category *contextual*:
  `slotsForSubject`, `partsForSubject`, `slotLabelFor`, `subjectNote`, `defaultsFor` (slot → the
  default model), `variantHidden`, `anchorBoneFor` and `anchorCnpFor`. A category counts as
  contextual when `slotsForSubject` and `partsForSubject` are both set (`PC.h:159`).

A candidate is a `CatalogPart` (`PC.h:19-76`) with these fields:

- `slot`, `id` (the model stem), `displayName` and `modelFileIdx`.
- `presetIndex`: this row is one of the game's numbered face presets, e.g. Survive's 28 presets
  over 7 heads (`PC.h:24-27`).
- MGO gear only: `gearId`, `gearExclude`, `gearMust` and `gearIcon` (`PC.h:28-44`).
- `companionGearId` / `companionFileIdx`: a two-piece garment, one row that fits two models
  (`PC.h:45-54`).
- `coveredFileIdx`: the "hair under a hat" model to swap in (`PC.h:55-66`).
- `hairForms`: several covered forms of one hairstyle (`PC.h:67-75`).

A variation is a `CatalogVariation {name, path, fileIdx}`, where `name` is the suffix "cam",
"clv", "v01", "camo_c12" and so on (`PC.h:78-82`). A seating option is
`AttachOption {hostSlot, cnp}`. Options are tried in order, because a weapon is a *chain*, not a
star (`PC.h:84-96`).

`setBuilderCategory` swaps the whole source when the category changes (`CTW.cpp:6155-6185`):

| # | Category | Source | Contextual? |
|---|---|---|---|
| 0 | Parts (free-form) | none (empty `BuilderSource`) | — |
| 1 | Weapon | `WeaponCatalog::builderSource` (`index/WeaponCatalog.cpp:346-363`) | no |
| 2 | Character (player characters) | `PlayerCatalog::builderSource` (`PC.cpp:2143-2302`) | yes |
| 3 | Vehicle & buddy | `MechaCatalog::builderSource` (`index/MechaCatalog.cpp:341-360`) | no |
| 4 | All other characters | `CharacterCatalog::builderSource` (`index/CharacterCatalog.cpp:248-270`) | no |

### 2.2 Base body / rig

**The base comes from the variant combo.** For a weapon it is the receiver; for a character it
is the body or outfit. For player characters, the "variants" are the parts of the subject's
`variantSlot`, and the character's own body is moved to the front of that list
(`PC.cpp:2166-2196`). `rebuildWeapon` loads this model first, as part 0 and the host everything
else seats onto (`CTW.cpp:3683-3700`).

**Some bases are skeleton-only.** Survive's `bsm0/bsf0_main0_def` set
`baseIsSkeletonOnly = true` (`PC.cpp:556`; comment at `PC.h:106-108` and `index/PlayerCatalog.h:132-136`). Such a base is
loaded for its bones and then:

- hidden completely once a head is fitted, or
- drawn head-only (bone subtree `SKL_004_HEAD`) while no head is fitted.

Sources: `CTW.cpp:3880-3908`, `CT.cpp:1871-1873`.

**There is no skeleton merge.** Each part keeps its own skeleton. Bones are concatenated into one
palette at a per-part `boneBase` (`CT.h:1-5, 310`; `CT.cpp:1859, 1869, 1939-1940, 2020`). Each
part also loads its own rig (`.frig`), help bones (`.frdv`) and connect points (`.fcnp`) from
sibling files (`CT.cpp:1155-1180`).

### 2.3 Slots, per category, and where each candidate list comes from

#### Weapon (category 1)

**Slots.** `receiver` (the variant combo), `barrel`, `magazine`, `stock`, `muzzle`,
`muzzleOption`, `sight`, `sight2`, `option`, `option2` and `underBarrel`, in the game's own order
(`index/WeaponCatalog.cpp:26-29`). The captions are the game's (Base, Barrel, … Laser Sight,
Underbarrel; `CTW.cpp:85-109`). `sight2` and `option2` exist only where the game's shipped
builds use a second one (`CTW.cpp:659-666`).

**Candidates.** Directories named `/pack/collectible/chimera/<slot>/<id>.fpk` and the models
inside those packs (`index/WeaponCatalog.h:4-13`, `WeaponCatalog.cpp:98-101`).

**Subjects** are weapon families. When the development tables are present, the game's named
weapons are listed first, grouped by class, followed by "ALL RECEIVERS IN THE GAME DATA"
(`CTW.cpp:4236-4303`). For a named weapon the variant combo lists its **star tiers**, and each
tier is a whole build (`CTW.cpp:4406-4434, 4617-4638`).

**Narrowing (optional).** "Only parts that fit this weapon" filters by
`EquipCatalog::compatibleStems(slot, receiver, barrel)`. Its sources are
WeaponPartsCombinationSettings and the parts the game's shipped builds fit
(`CTW.cpp:168-174, 4779-4783`). A slot that no shipped build ever fills on this receiver is shown
crossed out as "not usable" (`CTW.cpp:4730-4759`).

#### Character (category 2, `PlayerCatalog`)

Every subject and slot is read off shipped data (`index/PlayerCatalog.h:9-44`). Empty slots are dropped
(`PC.cpp:75-78`). Slots are sorted by one face-down order (`PC.cpp:56-73`).

**Survive: `ssd_m`, `ssd_f`.**

- Six prefix slots (`PC.cpp:593-600`):

  | slot id | prefix | caption |
  |---|---|---|
  | `head_equipment` | hd | Head Equipment |
  | `arm` | ar | Arm Equipment |
  | `inner` | bd | Inner |
  | `torso` | ua | Torso Equipment |
  | `leg` | lg | Leg Equipment |
  | `chest_rig` | rg | Chest Rig |

  Each is filtered by the gender letter and by Survive's `GearConfig.lua` (`PC.cpp:602-613`).
  Captions come from `ssd_common.lng2`, looked up by StrCode32 key (`PC.cpp:480-505`).
- `accessory` (Accessory / Head), `glasses` and `mask`, taken item by item from the gear table
  (`PC.cpp:655-716`).
- `head` (Face Preset, the game's numbered preset grid) and `hair` (Hairstyle, folded into
  forms) (`PC.cpp:855-868`).
- Head-slot anchor: bone `SKL_004_HEAD` (`PC.cpp:884`).

**MGO3: `mgo_m`, `mgo_f`.**

- The GearConfig categories: `mgo_headgear` Headgear, `mgo_chest` Chest Gear, `mgo_base` Base and
  `mgo_accessory` Accessory (`index/MgoGearConfig.cpp:93-96`).
- Each item carries its name from the language table, plus Exclude/Must lists and an icon
  (`PC.cpp:1366-1377`).
- A Chest row that belongs to a Base row is folded into that row as a companion
  (`PC.cpp:1354-1362`).
- `head` (Face Preset / Face), `hair` and `horn` (`PC.cpp:1519-1527`).
- `mgo_base` defaults to the BDU (`PC.cpp:1505-1510`).
- Headgear and accessory anchor on `SKL_004_HEAD` + `CNP_HEAD` (`PC.cpp:1557-1565`).

**TPP.**

- **Snake:** `body` Uniform (the variant combo), `face` Head Option and `arm` Arm
  (`PC.cpp:1799-1801`).
- **Diamond Dogs soldier, male and female:** `body` Outfit and `head` Headgear
  (`PC.cpp:1851-1852`).
- **"Other characters":** slots come from the part token in each model name and are labelled
  "slots from model names" (`PC.cpp:2030-2059, 2159-2163`). Possible slot ids include `band`,
  `hood`, `sling`, `lead`, `coat`, `hand`, `beard`, `brow` and `other` (`PC.cpp:64-68`).

**GZ: `gz_sna`.** A `body` list only (`PC.cpp:2091-2112`).

**Variations** for every character come from CharacterCatalog's .fv2 map (`PC.cpp:2294-2300`).
Characters use no seating plan (`CharacterCatalog.cpp:262-268`).

#### Vehicle & buddy (category 3, `MechaCatalog`)

- **Subjects** are grouped by the three-letter directory under `/Assets/tpp/mecha`, because one
  vehicle spans several model prefixes (`index/MechaCatalog.h:12-25`).
- **Slots** come from the kind token in the name (`MechaCatalog.h:48-50`).
- **Seating** uses a table measured from the shipped `.fcnp` files (`MechaCatalog.cpp:132-157`).
- **Paint** comes from the global `mfv_camo_cNN` and `mfv_scol_cNN` tables
  (`MechaCatalog.h:64-68`).

#### All other characters (category 4, `CharacterCatalog`)

- **Slots** come from name tokens: `body`, `face`, `hair`, `head`, `beard`, `eyebrow`, `tattoo`,
  `arm` and `gear` (`index/CharacterCatalog.cpp:100-129`).
- **Subjects** are character ids.
- **Variations** come from `.fv2` files whose stem matches the model's after `_def`, `_ply`,
  `_cov` and `_sta` are stripped (`CharacterCatalog.h:47-53`).

#### Parts (category 0, free-form)

Any `.fmdl` in the index, searched through `FmdlListModel` (`CT.cpp:97-117`,
`tabs/ModelsTab.cpp:96-126`).

### 2.4 Rows that are not model slots ("look" rows, `isLook`, slot id prefix `look:`)

These are stored in the same `m_weaponRows` list (`CT.h:794-816`), but their payload is an
option index, not a file index.

**Avatar appearance rows.** They appear only when the avatar art is present and the head slot
holds game presets (`CTW.cpp:801-831`), in this order (`CTW.cpp:788-798`): Eye Shape, Right Eye
Color, Left Eye Color, Skin Color, Wrinkles Type, Eyebrow Style, Hair Color, Facial Hair and
Feature. What each one does:

- **Eye Shape** swaps the head *model* (`CTW.cpp:3716-3721`). Every other row is a set of
  texture substitutions (`CT.cpp:793-797`).
- The rows are filled from `AvatarTextures` (`CTW.cpp:1699-1923`).
- They are reset to "from face preset" whenever the face preset changes
  (`CTW.cpp:2754-2790`).

**Survive-only rows:**

- **Skin Tone** (`look:skin`, the same id as the avatar row, `CTW.cpp:858-930`).
- **Sleeves** (`look:sleeves`: Auto, A, B, C or None; a scene option, not a face option;
  `CTW.cpp:932-982`).

**Any subject with a hair slot:** **Hair shape** (`look:hairshape`: As shipped, b, c, d or e;
`CTW.cpp:1471-1504`).

**Dirt.** A push button that opens a splat editor holding at most 64 splats. It is not a
`WeaponSlotRow` (`CTW.cpp:1298-1346`; `preview/DirtSplats.h:1-40`).

### 2.5 Colour state

- **Gear Color** (whole character or weapon): one layer swatch, `m_gearColor`, a PathCode64.
  Sources are `LayerColorCatalog` solids and patterns (`CT.h:908-920`;
  `CTW.cpp:3512-3557`).
- **Per-slot colour, Survive:** one row per gear slot, `m_slotGearColor` (`CT.h:860-865`;
  `CTW.cpp:745-775, 2344-2383`).
- **Per-item dyes, MGO:** up to four rows per slot (primary and secondary, twice when the item is
  a two-piece garment), `m_mgoColours`. The palette comes from `GearConfig.lua`, and each colour
  id is its own `.fv2` under `/Assets/mgo/fova/chara/` (`CT.h:822-878`; `CTW.cpp:711-744,
  2385-2468, 2557-2589`).
- **Camo / Variation:** one `.fv2` name applied to every fitted part, where each part resolves the
  name against its own stem. The list is split into CAMO PATTERN, BASE COLOR, SKIN TONE and MODEL
  VARIATION using the game's own naming (`CTW.cpp:2856-3015`). On a weapon only MODEL VARIATION is
  listed (`CTW.cpp:2976-3009`), and the default is "clv" (`CTW.cpp:3338-3358`).

### 2.6 Per-part and per-slot state

**`Part`** (`CT.h:286-355`) holds:

- file index, path, slot, the parsed `FmdlFile` and its texture, normal and PBR sets;
- the `.frdv`, `.frig` and `.fcnp` data;
- `attachPart` and `attachCnp`;
- `fovaName` and `boneBase`;
- **seven separate visibility sets:** `hiddenGroups`, `fovaHiddenGroups`, `fovaShownGroups`,
  `lookShownGroups`, `sleeveHiddenGroups`, `hairShownGroups` and `hairHiddenGroups`;
- `fovaAttached` / `fovaAttachedFiles`: models the variation brings with it, which live and die
  with it.

**`WeaponSlotRow`** (`CT.h:794-816`) holds the slot, its combo, `partIdx`, `companionPartIdx`,
`unusable`, `seated`, `note`, `isLook` and `sceneOption`.

**Scene-level state:**

- `m_hiddenPartKeys`: parts hidden but still equipped (`CT.h:242-247, 907`).
- `m_headOnlyPart` and `m_hideBasePart` (`CT.h:969-977`).
- The dirt state (`CT.h:674-676`).

### 2.7 Persistence: one grammar for presets, outfits, undo, clipboard and files

`captureSceneFields()` (`CTW.cpp:5259-5422`) is the single text description of a built scene.
It writes a sorted list of `key=value` fields:

```
<slot>=<asset path>[#<preset n>]     look:<row>=#<option>      __host=<path>   __subject=<catalogue id>
camo=<variation name>                gearcolor=<swatch asset path>
dirt=<splat list>  dirtlook=<amount>,<mask>        mgocolor:<slot>=<primaryId>,<secondaryId>
fova:<slot>=<name>  attach:<slot>=<hostSlot>@<CNP>  hidepart:<slot>=1
hidegroup:<slot>=<ids>  hidemesh:<slot>=<part-local mesh ids>
```

**Paths and ids, never file indices.** This keeps a saved build valid after a re-index
(`CTW.cpp:5273-5283, 5303-5308, 5316-5318`). A part without a slot is keyed `#<path>`
(`CTW.cpp:5227-5246`). The list is sorted so that two equal scenes compare equal
(`CTW.cpp:5415-5420`).

**`applySceneFieldsImpl`** (`CTW.cpp:5610-6042`) reads the list back:

- The subject is restored first.
- An absent field is a statement: no `camo=` means the default (`CTW.cpp:5935-5951`), and no
  colour means "as shipped" (`CTW.cpp:5929-5932`).
- Slot renames are aliased by model family: `torso`→`inner` for `bd*` parts,
  `head_equipment`→`accessory`/`glasses`/`mask`, and `face`→`head` (`CTW.cpp:5838-5896`).
- Per-part state is applied once the parts exist, and submeshes last (`CTW.cpp:5972-6031`).
- Anything it cannot place is reported as "not in this install" or "not offered on this
  subject" (`CTW.cpp:6032-6041`).

**Where builds are stored:**

- **Presets:** QSettings groups `weaponPresets`, `characterPresets`, `vehiclePresets` and
  `otherCharacterPresets`, one blob joined with `|` per name (`CTW.cpp:6144-6153, 5424-5443`).
  The combo lists the user's presets (`u:<name>`) and then the game's own builds (`g:<index>`,
  Weapon only) (`CTW.cpp:5141-5203`).
- **Outfits** (the Parts page): `customize/outfits` plus `customize/outfitfields_<name>`, using
  the same grammar. The older paths-only format is migrated the first time it is read
  (`CT.cpp:619-734`).
- **Text and files:** a header line `# FOXAssetBrowser build`, then `category=<word>`, optionally
  `name=`, then one field per line. It goes to the clipboard (Copy/Paste build) or to a
  `.foxbuild` file; importing a file saves it as a preset (`CTB.cpp:31-209`).
- **Other persisted settings:**
  - `customize/category`, default Character (`CT.cpp:598-614`)
  - `weapon/compatOnly` (`CTW.cpp:379-380`)
  - `customize/gearUnlocked` (`CTW.cpp:404-406`)
  - search history `customize/searchHistory` (`CT.cpp:102`)
  - N-panel layout `customize/npanel` (`CT.cpp:262`)
  - splitter sizes `customize/splitter` (`CT.cpp:380`)

**Not persisted (a gap):** Survive's per-slot colour `m_slotGearColor`. The harness can set it
through `slotcolor:` (`CTW.cpp:4070-4092`), but `captureSceneFields` never writes it
(`CTW.cpp:5259-5421`). Presets, outfits, `.foxbuild` files and undo therefore all lose it.

---

## 3. Candidate discovery

**Which list a slot shows.** The panel asks `partsForSubject(subject, slot)` for a contextual
category and `partsFor(baseSlot)` otherwise (`CTW.cpp:4776-4778`). The list is rebuilt:

- whenever the subject changes (`CTW.cpp:4465-4493`);
- whenever the barrel changes, because muzzle compatibility is keyed on the barrel
  (`CTW.cpp:2040-2044`);
- on a rescan, when every cache keyed on a file index is dropped (`CT.cpp:751-791`).

Every catalogue folds the game-filter generation into its cache key
(`index/WeaponCatalog.h:79-82`).

**Row 0 of every slot:**

- **Game presets present:** row 0 is `— default — <preset 1>` (`CTW.cpp:4794-4813`).
- **The subject has a default for the slot:** row 0 is `— default — <item>`, captioned "what the
  game starts this character in" (`CTW.cpp:4814-4842`). Clearing a slot therefore returns to that
  default rather than leaving a hole (`CTW.cpp:4761-4764`).
- **Otherwise:** row 0 is `— none —` (`CTW.cpp:4843-4845`).

**Filtering:**

- **Games bar:** TPP, MGO, GZ and Survive checkboxes narrow every catalogue at once. The info line
  says what is hidden (`CTW.cpp:330-374, 568-582`).
- **Weapon compatibility:** a filter that would empty a slot falls back to the full list. A part
  already fitted, or about to be selected by a restore (`m_pendingKeep`), is never filtered out
  (`CTW.cpp:4769-4775, 4854-4856, 4925-4939`).
- **Rule states in the tooltip:** the slot tooltip says which of three states applies — rule off,
  rule on with data, or rule on with no data for this receiver (`CTW.cpp:4941-4987`).

**Search:**

- **Find box** above the slot rows. It counts matches per slot in the label (e.g. "Torso · 3"),
  pre-filters every slot's popup, and Enter opens the first slot with a hit (`CTW.cpp:272-293`,
  `CTB.cpp:213-239`).
- **Typing in an open popup** filters by substring across the name, file and path lines
  (`SC.h:1-13`, `SC.cpp:502-543, 591-616`).
- **Free-form page:** the list uses the app's query grammar (tags plus text) with a 200 ms
  debounce (`CT.cpp:386-391`; `tabs/ModelsTab.cpp:96-126`).

**Names shown:**

- A weapon part uses the game's display name when `NameCatalog` resolves it, then gear or suit
  names, and otherwise the stem tidied up (`CTW.cpp:111-137`).
- An MGO item uses its language-table name (`PC.cpp:1366-1370`).
- A face preset is "Face n" (`PC.cpp:855-860`).
- A camo uses the development list's name when there is one, "Skin tone n" for tone tables, and
  the raw token on line 2 (`CTW.cpp:3421-3449, 2966-2973`).
- MGO colours are "Solid n" or "Pattern n" with the id underneath, because the game ships no
  colour names (`CTW.cpp:2425-2459`).

**Icons and thumbnails:**

- **Game UI icons.** `IconCatalog` reads `WeaponPartsIcon/…` and `EquipIcon/…`, trims them to the
  alpha box and caches them by (stem, height) (`index/IconCatalog.h:1-50`). The icon column
  (88×42) is reserved only when some row actually has an icon (`SC.h:44-47`,
  `SC.cpp:429-444`).
- **Swatches.** UI textures are drawn square and not tinted: gear icons, camo chips (the `camo4`
  set for weapons, `camo3` for vehicles), preset portraits and hair tiles (`SC.h:31-35`;
  `CTW.cpp:3390-3419, 4862-4873, 4884-4890, 4910-4913`).
- **Appearance rows:**
  - Skin tones: an average of a cheek patch, `avgSpec` (`CTW.cpp:1795-1798`).
  - Wrinkles: the game's tile, or a crop of the brow band (`CTW.cpp:1814-1827`).
  - Eye shape: the game's tile, or a `model:<fileIdx>` render (`CTW.cpp:1734-1747`). Those
    renders come from `ThumbnailRenderer`, which runs on its own thread with a memory and disk
    cache (`gl/ThumbnailRenderer.h:1-30`) and is pre-warmed before the rows are built
    (`CTW.cpp:833-837`).
- **Hover preview.** Resting on a popup row shows a large preview after 500 ms; the mouse wheel
  resizes it between 128 and 768 px (`util/HoverPreview.h:1-37`). It is built lazily
  (`SC.cpp:545-590`). The closed combo shows the fitted item's icon (`SC.h:103-105`).

---

## 4. Assembly

`rebuildWeapon()` builds the scene for every builder category (`CTW.cpp:3667-3945`). It works
in this order:

1. **Tear down.** Remove every part (`CTW.cpp:3679-3681`), then read the face into `m_look`
   before any part loads (`CTW.cpp:3688-3691`).
2. **Load the host** from the variant combo (`CTW.cpp:3692-3700`).
3. **Load each slot in row order:**
   - A look row loads nothing (`CTW.cpp:3714`).
   - **Model swaps:** Eye Shape replaces the head model (`CTW.cpp:3716-3721`). For hair, a chosen
     Hair-shape form wins; otherwise the covered model is used when headgear is worn
     (`CTW.cpp:3722-3743`, `headgearWorn` at `CTW.cpp:2067-2081`).
   - `addPart(file, fova, slot)` (`CTW.cpp:3746`).
   - The **companion** model of a two-piece garment is added with it (`CTW.cpp:3749-3767`).
4. **Seat.** The slot's `attachPlanFor` is tried against the parts already registered, one CNP
   per host at most (`CTW.cpp:3769-3802`):
   - An empty plan means the part shares the skeleton; this is the character case
     (`CTW.cpp:3780`).
   - A part with no `.fcnp` of its own is "in the body's own space" and counts as seated
     (`CTW.cpp:3803-3815`).
   - Otherwise it is reported as unseated, sitting at the origin (`CTW.cpp:3816-3822`).
5. **Whole-build passes:** the look is re-applied to every part (`CTW.cpp:3826-3844`), then
   `fitSleeves()` and `fitHairShape()` run. A hair-form change rebuilds once more
   (`CTW.cpp:3845-3855`).
6. **Base-body decision:** head-only or hidden (`CTW.cpp:3880-3908`), then `rebuildScene()`.

**`addPart`** (`CT.cpp:1119-1270`):

- Loads the model with `modelload::load(fileIdx, pbrMode)` on the GUI thread (`CT.cpp:1140`).
- Loads the sibling `.frdv`, `.frig` and `.fcnp` (`CT.cpp:1155-1180`).
- Runs `applyFovaToPart` when a variation, gear colour, slot colour or MGO dye applies
  (`CT.cpp:1209-1220`).
- Runs `applyAvatarLookToPart` (`CT.cpp:1222-1231`).
- Logs load and apply times and texture-cache hits and evictions (`CT.cpp:1125-1142,
  1232-1246`).

**Seating onto connect points.** `attachMatrixFor` walks up the host chain with a bound of 16.
Each hop is the CNP's quaternion and position, in its parent bone's bind frame (found by
StrCode32), multiplied by that bone's posed matrix (`CT.cpp:3593-3656`). Chains are allowed and
cycles are refused (`CT.cpp:3572-3591`). The user can also attach by hand: equipped-list ▸
Attach to ▸ host ▸ CNP (`CT.cpp:478-509`).

**Skinned parts with fragment skeletons.** `restAlignmentFor` (`CT.cpp:3658-3856`) computes a
bind-frame offset `d` and an anchor bone. It has four exits:

| exit | case |
|---|---|
| `b` | another part carries this part's root bone; the fullest skeleton wins |
| `c` | the slot's connect point, on the point's own parent bone |
| `a` | the slot's anchor bone |
| `f` | forced through an environment variable |

A part whose root is shared by another part's root needs no offset (`CT.cpp:3752-3781`). At bind
pose `d` is applied as a group transform (`CT.cpp:4198-4221`). While a clip plays it is folded in
front of the bone matrices (`CT.cpp:4572-4670`).

**Material and texture swaps.** `applyFovaToPart` (`CT.cpp:4882-5075`):

1. Resolves the variation name against the part's own stem through the active category, so one
   model's table is never applied to another (`CT.cpp:4894-4944`).
2. Collects the texture overrides, the mesh groups to hide and show, and the **models the
   variation attaches** (`CT.cpp:4936-4940`). Group sets are always reassigned, so choosing
   "none" really does undo a variation (`CT.cpp:4945-4956`).
3. Rebinds the gear colour on top: the slot colour if set, otherwise the whole-character colour,
   through `layerColorOverrides` (`CT.cpp:4958-4983`).
4. Applies the MGO dye `.fv2` for each channel (`CT.cpp:4984-5015`).
5. Reloads the base, normal and PBR sets through the same overrides (`CT.cpp:5017-5036`).

**Order matters:** the variation first, then the look on top (`CT.cpp:1182-1192`;
`CTW.cpp:3653-3665`).

**The avatar look** (`CT.cpp:896-1117`) is a set of texture substitutions for face, brows,
beard, iris and SRMs on heads and hair. Skin inside clothing follows the game's tone tables per
atlas (`CT.cpp:817-894, 953-956`). The bandanna mesh welded into the head is shown only when a
`hat21*` item is worn (`CT.cpp:970-982, 1029-1032`).

**Part visibility rules.** There is one answer, `hiddenGroupsOf` (`CT.cpp:1719-1725`), and both
the viewport and the export read it (`CT.h:356-364`):

```
((shipped-invisible ∪ variation-hidden) − variation-shown − look-shown − neck-shown − hair-shown)
  ∪ look-hidden ∪ sleeve-hidden ∪ hair-hidden
```

Around that set:

- **Neck:** the head's neck piece is shown unless a worn torso reaches the head's skin, measured
  geometrically (`CT.cpp:1727-1796`).
- **Sleeves:** Survive's alternate forearm groups, fitted by measurement (`CTW.cpp:984-…`,
  `preview/SleeveFit.h`).
- **Already-fitted attachments:** an attachment that is already fitted as a part, or brought by
  another part, is skipped, matched by *path* (`CT.cpp:1305-1332`).
- **Hidden-but-equipped parts:** `setPartHidden` (`CTB.cpp:320-343`).
- **Submesh switches:** from the PARTS tree (`CT.cpp:441-442`).

**Game rules.** `applyGearExcludes` (`CTW.cpp:2148-2311`):

- **Must** first, and it cascades: e.g. a helmet puts its suit on.
- Then **Exclude**, checked in both directions because the shipped lists are one-sided.
- A cleared slot drops its dye.
- If the slot's default itself conflicts, the conflict is logged and the slot is left as chosen.
- With "Unlocked" ticked the rules are only described in the log (`CTW.cpp:2170-2189`).

**Physics.** There is none. Poses come only from the model, the clip, the rig and the `.frdv`
help-bone operations (`CT.cpp:4405-4407, 4488-4502`).

**Dirt.** Up to 64 bone-relative splats are turned into a per-material coverage mask in the PBR
set while the scene is assembled (`CT.cpp:1981-1986, 2243-2262`; `CTW.cpp:1402-1413`).

**Scene assembly into the viewport.** `rebuildScene()` (`CT.cpp:1798-2326`):

- **One scene per build.** While a build is running, rebuilds are deferred and one queued flush
  is scheduled (`CT.cpp:1800-1835`).
- **Rebasing:** mesh ids, material slots, joints and bones are rebased per part and per
  attachment (`CT.cpp:1859-2124`).
- **`m_meshOwner`** maps each scene mesh id to (part, attachment, local mesh), and every reader
  goes through it (`CT.h:989-996`).
- **Connect points** are collected for the hardpoint overlay (`CT.cpp:1999-2018, 2267`).
- **Lighting environment:** chosen by a per-game majority vote (`CT.cpp:2222-2242`).
- **Group visibility** is re-synced afterwards (`CT.cpp:2297-2306`).

---

## 5. Animation

**Archive combo.** Lists every motion archive (`.mtar`), grouped by game and shelf
(`CT.cpp:4232-4251`; `util/AnimCombo.h:25-40`). The previous selection is kept across a refill.
This tab passes **no scope** to it (`CT.cpp:4248`), unlike the Models tab
(`tabs/ModelsTab.cpp:1493`).

**Clip combo.** Lists the clips of the chosen archive, grouped by category, with starred clips at
the top (`CT.cpp:529-539, 4275-4306`; `AnimCombo.h:44-51`). Changing clip or archive keeps
playback running (`CT.cpp:541-571`).

**ANIMATIONS panel.** Lists every clip with one filter, a category filter, and a scope that
narrows the list to what the character's rig can play (`CT.cpp:330-358`). The scope model is
the first indexed part in the scene (`CT.cpp:3330-3349`). Choosing a clip in the panel goes
through the combos (`CT.cpp:348-356`).

**Transport.** The shared `AnimTransport` runs on a wall clock at 30 fps (`CT.h:1063-1067`;
`CT.cpp:518-528`). It also drives the root-motion path and ghost skeletons of the host
(`CT.cpp:4751-4812`) and the animated-GIF capture, which is available only while a clip *and*
parts are present (`CT.cpp:2328-2363`).

**Posing** (`setFrame`, `CT.cpp:4350-4749`):

- **Host election:** the host is the unseated part with the most bones, and at least
  `kMinHostBones` (`CT.cpp:4376-4392`).
- **Rig choice:** each part uses its own `.frig`, and falls back to one global rig found by
  scoring (`human_finger.frig` is preferred) (`CT.cpp:4253-4273, 4402-4404, 4426-4428`).
- **Fragment parts** borrow the host's palette when every bone resolves; otherwise they pose on
  their own hierarchy (`CT.cpp:4429-4503`).
- **Seated parts** are held at identity and ride their seat (`CT.cpp:4447-4448`).
- **Rest-alignment pass:** runs next (`CT.cpp:4572-4670`).
- **Flatten:** all palettes are joined into one combined palette in the same layout
  `rebuildScene` used, and the per-contributor palettes are stored for export
  (`CT.cpp:4680-4731`).
- **Camera:** re-centred once after a clip loads (`CT.cpp:4733-4742`).

**Failure handling.** A clip that fails to decode clears the pose and the transport and says so
(`CT.cpp:4308-4330`).

---

## 6. UI layout

The tab is a three-way splitter with default widths 300 / 660 / 320; the sizes are remembered
(`CT.cpp:66, 367-380`).

### 6.1 Left column (top to bottom)

1. **Category** combo: Parts (free-form), Weapon, Character, Vehicle & buddy, All other
   characters (`CT.cpp:73-89`). Switching category clears the scene and rebuilds from what the
   combos still hold (`CT.cpp:144-156`). The tab opens on the remembered category, default
   Character (`CT.cpp:579-614`).
2. **Stack page 0: Parts (free-form).** In order:
   - search box (Esc clears, ↓ recalls history; `CT.cpp:97-104`);
   - model list (`CT.cpp:105-117`);
   - **Equip part** button; double-clicking a row also equips (`CT.cpp:118-124, 393-402`);
   - "Equipped:" checklist, where the checkbox shows or hides the part (`CT.cpp:125-127,
     405-410`);
   - **Remove selected** (`CT.cpp:128-129`);
   - outfits combo with **Save…** and **Delete** (`CT.cpp:131-139`).
3. **Stack page 1: the builder** (the same panel for categories 1–4, `CTW.cpp:242-522`):
   - **Header form:**
     - **Subject** combo, labelled Weapon/Base/Character from the source
       (`CTW.cpp:253-262, 4309-4314`). It is a grouped list: captions per class or game group,
       rows showing name, `id — N variants, M own part slots`, and the icon of the first variant
       (`CTW.cpp:4272-4303, 188-198`).
     - **Variant** combo, labelled Version/Tier/Body (`CTW.cpp:263-269`), holding star tiers or
       bare variants (`CTW.cpp:4406-4434, 4510-4530`).
     - **Find** box (`CTW.cpp:272-293`).
   - **Slot rows:** one form row per slot, labelled with the slot's caption (`CTW.cpp:669-706`).
     The label also shows state (`CTW.cpp:3451-3510`):
     - **bold:** fitted;
     - **dimmed:** empty;
     - **30 % opacity:** not usable;
     - **amber:** fitted but not seated (weapon or vehicle only);
     - **struck through:** hidden.

     Directly under an MGO slot there are up to 4 dye rows; under a Survive slot, one colour row.
     These stay hidden until the fitted item can be dyed (`CTW.cpp:707-775, 2336-2468`).
   - **Look rows**, in the avatar screen's order, then Skin Tone, Sleeves, Hair shape and Dirt
     (`CTW.cpp:777-856`).
   - **Footer form:**
     - **Camo / Variation**, a sectioned swatch list (`CTW.cpp:302-312`);
     - **Gear Color**, hidden when the install has no colour table (`CTW.cpp:314-326,
       3543-3553`);
     - **Games** checkboxes (`CTW.cpp:330-374`);
     - **Only parts that fit this weapon**, Weapon only (`CTW.cpp:376-381, 6178`);
     - **Unlocked — keep every slot as I set it**, only on pages whose parts carry rules
       (`CTW.cpp:383-407, 597-606`);
     - **Clear slots** and **Surprise me** (random fill) (`CTW.cpp:410-432, 1987-2013`);
     - **Preset** combo plus a **Build ▾** menu (`CTW.cpp:434-486`).
   - **Info line:** what was seated or not, missing parts and the face note (`CTW.cpp:489-491,
     3912-3944`).

### 6.2 Centre column

- A tip bar that can be dismissed (`CT.cpp:177-188`).
- The viewport (`CT.cpp:190-202`).
- The animation bar: archive combo, clip combo and transport (`CT.cpp:229-244`).

### 6.3 Right column: the N-panel

It holds these panels (`CT.cpp:254-365`):

| panel | contents | source |
|---|---|---|
| PARTS | scene tree of part ▸ mesh group ▸ submesh, each with a switch; selection is two-way with the viewport and the materials panel | `CT.cpp:204-222, 441-459` |
| MATERIALS | the material inspector | `CT.cpp:284-297, 1421-1482` |
| INFO | a scene section, then one section per part | `CT.cpp:299-306, 1574-1667` |
| RENDER | colour-pipeline settings | `CT.cpp:308-328` |
| ANIMATIONS | clip list with scope | `CT.cpp:330-358` |

Only PARTS is open by default (`CT.cpp:360-364`). A panel is filled when it is opened
(`CT.cpp:426-440, 1517-1547`).

### 6.4 The list format and category structure in detail

The category structure has three layers:

1. **Category**, which picks a data source.
2. **Subject**, a grouped list with captions.
3. **Variant**, then a flat set of **slot rows**, each of which is a searchable list.

**Appearance options live in the same row form as parts.** They are the same row type
(`WeaponSlotRow`) with `isLook` set (`CT.h:806-815`), so presets, undo and search treat them
uniformly (`CTW.cpp:5280-5284`; `CTB.cpp:220-224`).

**Every list is a `SearchableCombo`** (`SC.h:1-139`). Each row has:

- three lines: **name**, **file** and **full path** (`SC.h:23-29`);
- an optional game icon at the left (88×42 px, trimmed line art) or a square swatch (34 px)
  (`SC.h:43-51`);
- an optional star for favourites (`SC.h:39-44`).

Around the rows:

- **Group captions** are unselectable header rows that the arrow keys skip (`SC.h:98-101`).
  Examples: weapon class; "ALL RECEIVERS IN THE GAME DATA"; CAMO PATTERN / BASE COLOR /
  SKIN TONE / MODEL VARIATION; SOLID COLOURS / CAMO PATTERNS.
- **The popup** is wider than the combo, up to 55 % of the screen, so the path stays readable
  (`SC.cpp:448-475`).
- **Typing** filters all three lines. Filtered rows are hidden, not proxied, so indices stay
  stable (`SC.cpp:502-543`).
- **Payloads** are file indices, option indices or tags such as `n:` and `g:`/`u:`, read through
  `currentPayload()` and `selectPayload()` (`SC.h:111-113`).

### 6.5 Context menus

- **Slot row and equipped row.** Both use one builder, `addSlotMenuActions`
  (`CT.cpp:2588-2680`). It holds:
  - a disabled header ("<slot> — <file>" or "— empty");
  - a ☑ **Shown in viewport** toggle;
  - **Export this part (posed / variation / as shown)…** and its "to last folder" twin, which
    picks a unique name;
  - the part file's own file actions.

  The builder is reached from:
  - a slot combo's right-click (`CTW.cpp:689-706`);
  - an equipped-list row, which also gets **Attach to ▸ host ▸ CNP**, **Detach** and **Remove
    from scene** (`CT.cpp:465-516`);
  - the Export menu, for the selected equipped row (`CT.cpp:3561-3569`).
- **Viewport.** The shared part menu, where `describeSubject` names the part and submesh clicked,
  or the whole scene when nothing is under the cursor. It adds copy path / hash / material and
  export submeshes, with a silent twin (`CT.cpp:192-198, 2693-2753`).
- **PARTS panel:** the same hook (`CT.cpp:273-282`).
- **MATERIALS panel:** Export images (`CT.cpp:284-290`).
- **Build ▾ menu:** Save as preset…, Delete preset (enabled only for your own presets), Copy build
  as text, Paste build from text, Export build to file…, Import build from file…
  (`CTW.cpp:445-481`).

### 6.6 Hotkeys

| action | key | source |
|---|---|---|
| Undo / Redo, only while Customize is in front | Ctrl+Z / Ctrl+Shift+Z | `app/Hotkeys.h:47-56`; `app/MainWindow.cpp:1324-1334` |
| Export scene (role `export.selection`) | Ctrl+E | `CT.cpp:3481` |
| Animated export (role `export.animations`) | Ctrl+Shift+A | `CT.cpp:3520`; `app/Hotkeys.h:67-69` |
| Viewport: select / add to selection | double-click, Shift/Ctrl+click | `CT.cpp:183-186` |
| Viewport: hide / show all / isolate | H / Alt+H / Shift+H | `CT.cpp:183-186` |
| Viewport: frame / fullscreen / reset camera / list keys | . / F / middle-click / F1 | `CT.cpp:183-186` |
| Popup: filter, delete a character, clear the filter | type / Backspace / Esc | `SC.cpp:598-615` |
| Find box: open the first slot with a match | Enter | `CTW.cpp:287-293` |

**In the monorepo host, three of these are dead:**

- **Undo and Redo.** Nothing in `AB/` binds them. The only binding is in FOX's `MainWindow`.
- **Ctrl+E and Ctrl+Shift+A.** FOX tags its actions with the property `foxabExportRole`
  (`app/Hotkeys.h:261-266`), but the core looks for `abExportRole` (`AB/core/app/Hotkeys.h:64`).
  The core's hotkeys therefore land on its own "Export selection" row, which is disabled because
  the host does not override `hasExportSelection`, or find no animation row at all
  (`AB/core/app/AppShell.cpp:284-302, 323-331`).

---

## 7. Export

Everything goes through one scene walk, `exportSceneTo(path, onlyPart, anims)`
(`CT.cpp:2365-2583`). It walks the scene in the same order and with the same skip tests as the
viewport:

- Unticked parts and the hidden base body are left out, and named in the log
  (`CT.cpp:2415-2431`).
- `hiddenGroupsOf` is applied, and submeshes switched off in the tree are mapped back through
  `m_meshOwner` (`CT.cpp:2398-2408, 2438-2453`).
- Models a variation attached are exported as parts of their own (`CT.cpp:2493-2531`).
- A seated part goes in as a rigid transform. A posed part goes in with the current palette,
  vertices baked. With no clip loaded, the bind-pose offset `d` is written as a wrapper node
  (`CT.cpp:2459-2485`).
- Connect points are written into the file (`CT.cpp:2443-2446`).

It is written with `fox::writeModelFile` and the user's `ExportOptions`: Z-up, skeleton,
colour-layer bake, normal maps, connect points, name template, texture format, quality and size,
SRMs, readable or mirrored bone names, rig reduction, and so on
(`export/ExportOptions.h:38-152`). An animated export forces the skeleton on
(`CT.cpp:2539-2551`).

| Export | Entry | Notes |
|---|---|---|
| Scene / character / weapon / vehicle (.glb/.obj), plus a "to last folder" twin | Export menu, viewport | The noun follows the category (`CT.cpp:3451-3481, 3410-3441`). |
| One file per part | Export menu | Unique names on disk and within the run. **With a clip loaded each part is posed, with vertices baked and no skeleton** (`CT.cpp:3009-3100, 3527-3534`). |
| This part | slot or equipped menu | Name includes the variation (`CT.cpp:2642-2676, 2814-2834`). |
| One file per variation | Export menu | Selects each Camo/Variation row in turn and restores the original selection; refused when there is only the default row (`CT.cpp:2836-2950, 3535-3558`). |
| Animated scene, several clips | Export menu (current clip); harness spec `all`, indices or names (`CT.cpp:3102-3151`) | Poses the whole scene once per sample through `setFrame`, seats the scene at the first frame, and reports clips *written* rather than requested (`CT.cpp:3153-3319`). |
| Selected submeshes (+ silent twin) | viewport and PARTS menus | Temporarily hides everything else and restores the user's hidden set afterwards (`CT.cpp:3359-3408`). |
| Material images | MATERIALS panel | `exportactions::exportMaterialImages` (`CT.cpp:284-290`). |
| Viewport captures (still, turntable, GIF) | Export menu | `addViewportCaptureActions` (`CT.cpp:3560`); GIF needs a clip (`CT.cpp:2328-2363`). |
| The part's source file | file actions | `exportactions::addFileActions` (`CT.cpp:2679`). |
| Build as text / `.foxbuild` | Build ▾ | Section 2.7. |

---

## 8. Robustness

**Guards and invariants:**

- **Construction order.** The category is restored last, so the handler never touches a widget
  that does not exist yet. This fixed a segfault on launch (`CT.cpp:589-611`).
- **Deferred rebuilds.** A rebuild requested during a build is deferred, and a queued flush
  follows (`CT.cpp:1825-1834`).
- **PBR array length.** If the PBR set and the texture set differ in length, the PBR set is
  padded and a warning logged (`CT.cpp:2211-2221`). Each part always contributes one normal and
  one PBR entry per material (`CT.cpp:1971-1986`).
- **Removal remapping.** Removing a part renumbers the attachments that point at it and the
  slot-row indices, including companions (`CT.cpp:1272-1298`).
- **Attachment chains** are bounded at 16 and cycles are refused (`CT.cpp:3579-3585,
  3602-3608`).
- **Empty combos.** An invalid QVariant is never read as file index 0 (`CTW.cpp:4689-4696,
  5262-5268`).
- **Absent tier.** A missing tier role is never read as preset 0 (`CTW.cpp:4626-4629`).
- **`selectHostVariant`** puts the panel back if it fails (`CTW.cpp:4544-4562`).
- **QSettings keys** are sanitised (`/` and `\` replaced, 120-character cap) so one name cannot
  delete another's group (`CT.cpp:636-653`; `CTW.cpp:5427-5432`).
- **Empty presets.** Loading an empty or missing preset leaves the scene alone, but undo can still
  apply an empty scene (`CTW.cpp:5577-5584`).
- **The dirt button** is held as a `QPointer` (`CT.h:677-681`).

**Caches:**

- **Stem → file index,** built once per index generation. Unshadowed copies win
  (`CTW.cpp:4346-4377`).
- **MGO colour id → `.fv2`** (`CTW.cpp:2557-2589`).
- **Camo index read out of each table** (`CTW.cpp:3362-3388`).
- **Hair-model summary** (`CT.h:150-154`).
- **Icons,** by (stem, height) (`index/IconCatalog.h:13-15`).
- **Model thumbnails,** in memory and on disk (`gl/ThumbnailRenderer.h:26-31`).
- **The texture LRU** in `util/Extract.h:115-127`. The comment cites 256 MB, and loads report
  evictions (`CT.cpp:1133-1142`).
- **Stale answers** (icons, camo names, swatches, the material decode cache, the held colour) are
  dropped on a rescan (`CT.cpp:758-777`).

**Background work:**

- Only thumbnail rendering runs off the GUI thread (`gl/ThumbnailRenderer.h:12-16`), plus the
  lazy hover previews (`SC.cpp:567-582`).
- Model parsing and texture decoding in `addPart` run **synchronously on the GUI thread**
  (`CT.cpp:1140`).
- Every slot change tears the whole scene down and reloads every part (`CTW.cpp:3679-3681,
  3697, 3746`). There is no parsed-model cache; only the texture LRU softens this.

**Undo.** 30 levels deep. It works on the *authored description*, never on the loaded parts: a
step is pushed only when the settled description differs from the last one (`CT.h:762-781`;
`CTW.cpp:5450-5516`). Applying a description counts as one step (`CTW.cpp:5587-5608`).

**Instrumentation:**

- Logged all the time: one line per build and per part (`CTW.cpp:3857-3870`; `CT.cpp:1233-1246`).
- Behind environment variables:
  - `FOXAB_DUMP_RESTALIGN` (`CT.cpp:3858-3885`)
  - `FOXAB_DUMP_MATERIALS` (`CT.cpp:2126-2209`)
  - `FOXAB_NO_BORROW` (`CT.cpp:4433-4436`)
  - `FOXAB_NO_RESTALIGN` / `FOXAB_FORCE_RESTALIGN` (`CT.cpp:3673-3695`)
  - `FOXAB_FORCE_SKINTONE` (`CT.cpp:801-815`)
  - `FOXAB_NO_NEUTRAL_LOOK` (`CTW.cpp:2632-2640`)
- Many harness entry points: public methods of `CT.h:57-257`, e.g. `--restalignsweep`,
  `--camodump` and `--undoseq`.

---

## 9. Game-specific vs generic

| Feature | Class | Why |
|---|---|---|
| Category → subject → variant → slot rows (`BuilderSource`) | GENERIC | Any customiser has "what am I building, which base, which slots" (`PC.h:113-160`). |
| Contextual slots per subject (`slotsForSubject` / `partsForSubject`) | GENERIC | Different characters have different slots, e.g. male vs female, class vs class (`PC.h:127-140`). |
| Default row per slot = what the game dresses the subject in | GENERIC | "Empty" is rarely naked (`CTW.cpp:4761-4842`). |
| Searchable 3-line rich list with icons, swatches, captions and hover preview | GENERIC | A list UI any game can use (`SC.h`). |
| Page-wide Find with per-slot counts | GENERIC | `CTB.cpp:213-239`. |
| Appearance options as rows in the same form (`isLook`) | GENERIC | Tone, eye colour and similar exist in every character creator (`CT.h:806-815`). |
| One scene-description grammar (presets / outfits / undo / clipboard / file), path-keyed | GENERIC | `CTW.cpp:5248-5422`. |
| Undo by diffing settled descriptions | GENERIC | `CTW.cpp:5450-5477`. |
| Hide-but-keep a part; per-submesh switches; one visibility answer shared by viewport and export | GENERIC | `CT.cpp:1719-1725`; `CTB.cpp:320-343`. |
| Exports: scene / per part / per variation / animated multi-clip / submeshes / silent twins | GENERIC | `CT.cpp:2365-3570`. |
| Game rules (exclude / require), with an Unlocked switch | GENERIC | The mechanism is generic; the Exclude/Must lists are MGO data (`CTW.cpp:2148-2311`). |
| Compatibility narrowing with "rule / no rule / off" states | GENERIC | The mechanism; the tables are Fox's (`CTW.cpp:4711-4987`). |
| Seating on sockets through a per-slot plan with chains | GENERIC | Weapons and vehicles in many games (`PC.h:84-96`; `CT.cpp:3593-3656`). |
| Per-part rigs + host election + fragment borrowing + rest alignment | GENERIC | Any engine that ships partial skeletons per part (`CT.cpp:4376-4670, 3671-3856`). |
| Named variations (texture swap + group hide/show + attached models) per model | GENERIC | The concept; `.fv2` is the Fox format (`CT.cpp:4882-5075`). |
| Two-piece garments (companion model) | GENERIC | One item that fits several meshes (`PC.h:45-54`). |
| Model swap by context (covered hair under a hat, eye shape = head model) | GENERIC | Hats flattening hair is universal (`CTW.cpp:3716-3743`). |
| Game-toggle filter across a multi-game install | GAME-SPECIFIC | Four Fox games share one install (`CTW.cpp:330-374`). |
| Chimera packs, EquipCatalog dev tables, named weapons and star tiers | GAME-SPECIFIC | TPP data (`CTW.cpp:4229-4434`). |
| PlayerCatalog slot rules (prefix + gender letter, GearConfig, lng2 captions) | GAME-SPECIFIC | Survive / MGO / TPP naming (`PC.cpp:411-2112`). |
| Avatar look (preset grid, wrinkle / brow / beard / iris texture sets, deco) | GAME-SPECIFIC | MGSV avatar editor data (`CT.cpp:896-1117`; `CTW.cpp:1699-1923`). |
| Survive sleeves, hair-shape letters, neck coverage, bandanna | GAME-SPECIFIC | Fox mesh-group conventions (`CTW.cpp:932-1127, 1471-1665`; `CT.cpp:1727-1796, 970-982`). |
| MGO per-item dye channels as `.fv2`; Survive layer-swatch slot colour | GAME-SPECIFIC | Fox colour mechanisms (`CTW.cpp:2336-2589`). |
| Camo section split (camo_c / scol_c / skin / model) and "clv" default | GAME-SPECIFIC | Fox naming (`CTW.cpp:2856-3015, 3338-3358`). |
| Preset-blob slot aliases (torso → inner, head_equipment → …, face → head) | GAME-SPECIFIC | Migrations of Fox's own slot ids (`CTW.cpp:5838-5896`). |
| Skeleton-only base shown head-only or hidden | GAME-SPECIFIC | Survive's `bs*0` mannequin (`CTW.cpp:3880-3908`). |
| Dirt splats (≤64 bone / position / radius) | GAME-SPECIFIC | Survive save format (`preview/DirtSplats.h:1-40`). |
| PBR set (SRM, TRM, layer + mask, FMTT presets), dirt composite, eye / hair / glass shading | LOOK | FOX's viewport extension (`CT.cpp:1974-1986`; `AB/docs/PHASE5_REPORT.md:145-170`). |
| Gear colour visible only through a layer-multiply shader | LOOK | A texture rebind that the Fox shader composites (`CTW.cpp:314-326`). |
| Scene-game majority vote picks the lighting environment | LOOK | `CT.cpp:2222-2242`. |
| Root path, ghost skeletons, hardpoint overlay | LOOK | Viewport overlays (`CT.cpp:4751-4812, 2267`). |

---

## 10. What a game must supply

Below is the smallest set of questions the tab asks of game data, written as an interface. In
spirit it is `BuilderSource` plus the hooks that `CT.cpp`/`CTW.cpp` currently hard-code.
Game-independent code would own the panel, the build loop, visibility, posing, export and the
grammar.

```cpp
struct CustomizeCategory {                    // one per category ("Weapon", "Character")
  QString name, subjectNoun, variantNoun, emptyHint;
  QVector<Subject>   subjects();                               // id, display name, group caption, icon
  QVector<Option>    variants(subjectId);                      // bases; flag: skeleton-only
  QVector<SlotDef>   slots(subjectId);                         // id, caption, kind: Part | Look | SceneOption
  QVector<Option>    options(subjectId, slotId, const Selection&); // incl. default row, name/file/path/icon/swatch, section caption
  OptionId           defaultOption(subjectId, slotId);
  Compat             compatible(subjectId, slotId, const Selection&); // allowed set + "no rule" + "slot unusable"
  Resolved           resolve(subjectId, slotId, optionId, const Selection&); // model(s): primary + companions, context swaps
  QVector<Seat>      seatPlan(subjectId, slotId);              // [(hostSlot|"", socket)] in preference order; empty = skinned
  Anchor             anchorFor(subjectId, slotId);             // fallback bone + socket for foreign-rooted parts
  QVector<Variation> variations(modelId);                      // name, caption, section, swatch
  Retexture          applyVariation(modelId, variationId);     // texture overrides, groups hide/show, attached models
  QVector<DyeChannel> dyeChannels(subjectId, slotId, optionId);// palettes per channel (+ "as shipped")
  Retexture          applyDye(modelId, channel, colourId);
  Retexture          applyLook(modelId, const LookState&);     // skin tone / face rows as texture swaps
  VisibilityRules    hides(const SceneParts&, const Selection&); // neck, sleeves, hair-under-hat, bandanna, base-body
  Rules              rules(optionId);                          // excludes / requires
  QVector<Preset>    shippedPresets(subjectId);                // game builds / tiers (read-only)
  QVector<RigSource> rigFor(modelId);                          // own rig/help bones; fallback rig
};
```

| Question | Where FOX answers it today |
|---|---|
| List categories | Hard-coded combo items (`CT.cpp:76-83`) mapped to catalogues (`CTW.cpp:6168-6172`) |
| List subjects and group captions | `BuilderSource::subjects` (`PC.h:120`); weapons also use named weapons from EquipCatalog (`CTW.cpp:4239-4270`) |
| List variants (bases), skeleton-only flag | `CatalogSubject::variants` / `hiddenVariant` (`PC.h:98-111`); `variantHidden` (`PC.h:145-146`) |
| List slots for a subject, with captions | `slotsForSubject` / `slotLabelFor` (`PC.h:134-138`); weapons use `slotNames` + `prettySlot` (`CTW.cpp:85-109, 655-667`) |
| Look rows (kind = Look) | Hard-coded in the tab (`CTW.cpp:788-798, 874-982, 1471-1504`) |
| Options for slot X given the current selection | `partsForSubject` / `partsFor` + `compatibleStems(slot, receiver, barrel)` (`CTW.cpp:4776-4783`) |
| Default option | `defaultsFor` (`PC.h:141-144`); preset row 0 (`CTW.cpp:4794-4813`) |
| Slot unusable on this base | `EquipCatalog::slotEverUsedOn` (`CTW.cpp:4730-4735`) |
| Resolve option → model(s) | `CatalogPart::modelFileIdx`, `companionFileIdx`, `coveredFileIdx`, `hairForms` (`PC.h:19-76`); swaps done in `rebuildWeapon` (`CTW.cpp:3715-3767`) |
| Seat on a socket | `attachPlanFor` (`PC.h:125`; `index/WeaponCatalog.cpp:330-344`; `MechaCatalog.cpp:132-157`) |
| Anchor for foreign-rooted parts | `anchorBoneFor` / `anchorCnpFor` (`PC.h:147-156`; `PC.cpp:884, 1559-1565`) |
| Variations for a model | `variationsFor(stem)` (`PC.h:124`); section split in the tab (`CTW.cpp:2868-2877`) |
| Apply a variation | `.fv2` via `modelload::fovaOverrides` / `fovaGroupVisibility` / `fovaAttachedModels` (`CT.cpp:4927-4943`) |
| Dye channels and palettes | MGO: `MgoGearConfig` (`CTW.cpp:2404-2459`); Survive and global: `LayerColorCatalog` (`CTW.cpp:2365-2378, 3521-3538`) |
| Apply a dye | `layerColorOverrides` / MGO colour `.fv2` (`CT.cpp:4958-5015`) |
| Apply the look | `AvatarTextures` / `avatarOverrides` / `skinToneOverrides` (`CT.cpp:817-1117`) |
| What an option hides | `hiddenGroupsOf` inputs: shipped flags, variation, look, `neckCoverage`, `fitSleeves`, hair shape, bandanna (`CT.cpp:1719-1796`; `CTW.cpp:984, 1506`) |
| Excludes / requires | `CatalogPart::gearExclude` / `gearMust` (`PC.h:28-40`) |
| Shipped presets / tiers | `EquipCatalog::presets` / `namedWeapons` (`CTW.cpp:5159-5193, 4406-4434`); face presets `AvatarPresets` |
| Rig per model | Sibling `.frig` / `.frdv` / `.fcnp` (`CT.cpp:1155-1180`); fallback rig by scoring (`CT.cpp:4253-4273`) |

---

## 11. Strengths worth keeping and weaknesses

### Strengths

1. **One panel, many categories.** Weapon, character, vehicle and every-other-character pages
   are all driven by `BuilderSource`. "Contextual" is just two optional hooks (`PC.h:1-9,
   127-159`). This is the list-plus-categories design the core should adopt.
2. **Look options are rows like parts.** They share the row type, the grammar, undo, search
   exclusion and the reset-on-preset rule (`CT.h:806-815`; `CTW.cpp:5280-5284, 2754-2790`).
3. **One description of a build.** Presets, outfits, undo, clipboard text and `.foxbuild` files
   all use one path-keyed, sorted grammar with explicit absent-field semantics and slot aliasing.
   The old outfit format that lost data was removed and is migrated on read (`CT.cpp:667-677,
   693-733`; `CTW.cpp:5248-5258, 5929-5951`).
4. **Undo cannot be forgotten by a code path.** It diffs settled descriptions in one place
   (`rebuildScene` → `noteSceneSettled`, `CT.cpp:2320-2325`). Rebuilds the user did not author,
   such as a PBR toggle, push nothing (`CT.h:768-774`).
5. **The viewport and every export share one walk and one visibility function**, with
   `m_meshOwner` as the only record of the layout (`CT.h:356-364, 978-996`; `CT.cpp:2398-2408`).
   The comments record real bugs this removed, e.g. a hidden submesh that still appeared in the
   .glb (`CT.cpp:2755-2759`).
6. **Honest list states.** Default rows name what the game starts the subject in. Crossed-out
   unusable slots, amber unseated labels and struck-through hidden parts are visible without
   opening anything. Tooltips say "no rule" rather than filtering silently (`CTW.cpp:3451-3510,
   4941-4987`).
7. **Search across every slot at once**, with counts in the labels and popups opened already
   filtered (`CTB.cpp:213-239`; `SC.cpp:486-493`).
8. **Posing that survives partial skeletons.** Host election, fragment borrowing that requires
   *all* bones to resolve, socket-anchored rigid carry, and the rest offset folded in front of
   the bone matrix so accessories stay seated during clips (`CT.cpp:4376-4670`). An offline
   sweep measures how far each part lands from its seat (`CT.h:122-127, 519-552`).
9. **Variation-brought models are owned by the variation.** Changing the variation cannot leave
   the old hat behind, and duplicates are removed by path (`CT.h:322-332`; `CT.cpp:1305-1332`).
10. **Game rules can be reproduced or overridden.** Unlocked governs only the automatic changes
    (`CTW.cpp:383-407, 2170-2189`).

### Weaknesses

1. **A full rebuild on every change, on the GUI thread.** Any slot, dye or look change runs
   `rebuildWeapon`, which removes every part and reloads each model from the archive
   (`CTW.cpp:2064, 2483, 3679-3746`). `modelload::load` runs synchronously (`CT.cpp:1140`). There
   is no parsed-model cache, only the texture LRU, and the code logs evictions when the build is
   larger than that cache (`CT.cpp:1133-1142`). A hair-form change rebuilds twice
   (`CTW.cpp:3850-3855`).
2. **Game logic is mixed into the tab.** It relies on stem tests (`av*` + `_type` for heads,
   `CT.cpp:908-910, 1734-1737`; `hat21` for the bandanna, `CT.cpp:978-982`), subject-id prefixes
   (`ssd_`, `CTW.cpp:760, 956`), slot-id literals (`mgo_headgear` / `head_equipment` / `hats`,
   `CTW.cpp:2074-2076`; torso slots, `CT.cpp:1753-1757`) and preset migrations
   (`CTW.cpp:5838-5896`). Category numbers are hard-coded (`m_builderCategory == 1/3/4`,
   `CTW.cpp:2995, 3356, 3476, 4240, 4716, 4730, 6146-6152`; `CT.cpp:3453-3459`). None of this can
   move to a core tab until it goes behind the interface in section 10.
3. **Misleading names.** The generic machinery is called "weapon": `m_weaponRows`,
   `rebuildWeapon`, `m_weaponCamo`, `m_weaponPick` and the file `CustomizeTab_Weapon.cpp`, all
   of which serve every category (`CTW.cpp:1-11`; `CT.h:572-957`).
4. **Two UIs for one question.** The free-form page uses a `QListView` plus an "Equipped"
   checklist (`CT.cpp:97-129`), while the builder uses per-slot combos. The equipped list is also
   the hidden-part *state* store even on builder pages where it cannot be seen (`CTB.cpp:314-318`).
5. **A choice that does not persist.** Survive's per-slot colour is never written by
   `captureSceneFields`. Presets, outfits, `.foxbuild` files and undo all drop it
   (`CTW.cpp:5259-5421` vs `CT.h:860-865`, `CTW.cpp:2470-2484`).
6. **The animation scope is inconsistent.** The ANIMATIONS panel narrows to the character's rig,
   but the archive combo under the viewport is never narrowed: `fillArchives(m_mtarCombo)`
   (`CT.cpp:4248`). Its own header says the two must agree (`util/AnimCombo.h:32-40`), and the
   Models tab does pass the scope (`tabs/ModelsTab.cpp:1493`). The scope model is simply "the
   first part" (`CT.cpp:3333-3343`).
7. **The manual Attach-to menu is stricter than the engine.** It refuses hosts that are
   themselves attached, i.e. no chains (`CT.cpp:482-487`), although `attachPartTo` allows chains
   (`CT.cpp:3579-3585`).
8. **Surprising per-part export.** With a clip loaded, parts are exported posed with vertices
   baked and no skeleton. This is documented only in a tooltip and a status line
   (`CT.cpp:3081-3085, 3527-3534`).
9. **Monorepo integration gaps.**
   - The host forwards only 3 virtuals (`AB/games/fox/tabs/FoxOwnedTabs.h:25-31`).
   - Undo and redo keys are unbound; only FOX's `MainWindow` binds them
     (`app/MainWindow.cpp:1327-1334`).
   - The export-role property names differ (`app/Hotkeys.h:261` vs `AB/core/app/Hotkeys.h:64`),
     so Ctrl+E and Ctrl+Shift+A miss this tab's actions (`AB/core/app/AppShell.cpp:284-302,
     323-331`).
   - The legacy settings entry does not call `syncPbrFromSettings`
     (`AB/games/fox/FoxPlugin.cpp:260-271` vs `app/MainWindow.cpp:763`).
   - The tab still draws on FOX's own viewport, not the core one
     (`AB/docs/PHASE5_REPORT.md:135-170`).
10. **Size and coupling.** About 12,800 lines. Harness hooks, diagnostic dumps and
    census/report writers are interleaved with UI and assembly code (`CT.h:57-257, 500-570`).
    Presets are stored only in per-machine QSettings blobs (`CTW.cpp:5434-5436`); `.foxbuild`
    is the only portable form.
