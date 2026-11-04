# Diablo IV Wardrobe tab — reference for a generic "Customize" tab

Source read: `d4/src/tabs/WardrobeTab2.cpp` (11,490 lines), `WardrobeTab2_Panels.cpp` (1,334),
`WardrobeTab2.h` (702), plus what they call in `d4/src/{index,model,gl,util,app}` and `d4/docs`.

**Citation shorthand.** `W:n` = `d4/src/tabs/WardrobeTab2.cpp` line n · `P:n` =
`d4/src/tabs/WardrobeTab2_Panels.cpp` · `H:n` = `d4/src/tabs/WardrobeTab2.h`. Every other path is
relative to `d4/src/` (or `d4/docs/` for `.md` files). Nothing below is inferred from a name alone;
where the code says something only in a comment, that is stated.

---

## 1. Purpose

The user picks a **character** (class + gender, `W:1288-1295`), then dresses it: one appearance per
armour slot (helm / torso / gloves / legs / boots), held and sheathed weapons, a back trophy, and the
nine "character creator" choices (face, hair style/colour, eyes, facial hair, makeup, marking shape
and colour, jewelry) plus skin tone and a freckle/vitiligo overlay (`W:1629-1708`). Armour can be
tinted per slot with the game's 4-zone pigments or a custom colour (`W:1557-1568`, `W:6507-6617`).
The result is one assembled, textured, cloth-simulated, animatable model in a PBR viewport
(`H:93-96`), which can be saved as a named "ensemble" (`W:5747-5773`), undone step by step
(`W:5806-5826`), played with the class's own animations (`W:10780-10949`), and exported as a skinned
`.glb` of the whole outfit, one piece, a part subset, both genders, or a rig-only animation library
(`W:9502-9976`, `W:4331-4388`, `W:4402-4518`).

## 2. Data model

### 2.1 Axes
| Axis | Values | Source | Evidence |
|---|---|---|---|
| Class | 8 hero classes, shown alphabetically | `ItemDef::heroClassName/Code` (canonical table, `index/ItemDef.h:33`), `fubc` = eHeroClass index | `W:339-354`, `W:1289-1290` |
| Gender | Female `f` / Male `m` | hard-coded combo | `W:1291-1293` |
| Class prefix | `<code><gender>` lower-case, e.g. `barf` — the key for every name-based lookup | `classPrefix()` | `W:3132-3135` |
| Content drop | "Any content" or one TACT encryption key (a collab/season cohort) | `CascReader::encryptedSnos()` / `snosForTactKey()`; labelled by the three most common `stor###` tokens | `W:1299-1317`, `W:5328-5382`, `W:5316-5322` |

Changing class repopulates creator/slots/weapons in one batch then rebuilds once (`W:2659-2668`).
Changing gender additionally **retains the outfit**: each armour slot's name has its 4th character
swapped `M`↔`F` and the twin re-equipped if present, with one log line per slot
(`W:2669-2730`).

### 2.2 Slots (10 equipment + 9 creator + 2 skin)
Equipment slots are indexed 0–9 (`H:255-260`, `W:5564-5587`). Every slot is backed by a **hidden
QComboBox** that the icon grid drives (`W:1496-1499`, `W:1616-1626`); the combo's item data is the
appearance SNO, text (armour) or `UserRole+1` (weapons, trophy) the appearance name.

| # | Slot | Candidate list produced by | Evidence |
|---|---|---|---|
| 0–4 | Helm `HLM`, Torso `TRS`, Gloves `GLV`, Legs `LEG`, Boots `BTS` | SNO index group 9 (Appearance): name starts with `<prefix>_` and ends with `_<code>`, filtered by content drop; Torso additionally gets `_base0*`/`_test999`/`_nude` whole-body bases | `W:327-329`, `W:5384-5420` |
| 5 | Main hand (held) | all group-9 names matching a prefix in the 22-row `kWeapTypes` table (`sword_`, `twoHandAxe_`, `shield_`…), minus off-hand-only types, filtered by the ItemType's `fUsableByClass` (toggle `wardrobe2/weap/classRestrict`) and content drop | `W:289-319`, `W:4650-4664`, `W:4627-4645`, `W:4675-4749` |
| 6 | Off hand (held) | same pool minus two-handers (1H + shield/focus/totem) | `W:4702`, `W:4741` |
| 7 / 8 | Sheath / Sheath 2 (stowed main / off) | same pools as 5 / 6 | `W:4742-4745` |
| 9 | Back trophy | `BackTrophyIndex`: Item whose `snoItemType` is `ItemType/CosmeticBack` → `snoActor` → Actor → `snoAppearance`; labelled with the localized item name | `index/BackTrophyIndex.h:8-24`, `W:5421-5456` |
| C0–C8 | Face, Hair style, Hair colour, Eye colour, Facial hair, Makeup, Marking, Marking colour, Jewelry | every `json/base/meta/<Folder>/*.<ext>` def (`Face/*.fac.json` …) filtered by `fUsableByClass[fubc]` and `eClassRestriction`; Marking also appends the ~70 MarkingShapes that exist only in CASC (binary parse) | `W:362-373`, `W:389-416`, `W:4562-4595`, `W:4597-4625` |
| — | Skin tone | `Actor/barbarianF.acr.json` `ptPlayerData[0].arSkinColorChoices` (dead/debug/test labels skipped) | `W:447-468`, `W:1687-1698` |
| — | Skin detail | fixed: none / Freckle / Vitiligo; texture found by substring in the body/head skin material | `W:1699-1708`, `W:473-489`, `W:8399-8415` |

Weapon slots greyed when the class has nothing for that hand (`W:4756-4775`).

### 2.3 Per-slot state
- Selection (combo index → SNO / name) — `W:5524-5533`.
- Pigment per slot: `SlotDye { name, 4 hex }` (`H:263-264`); only slots 0–4 are ever dyed
  (`W:4804`, `W:6497`, `W:6692`).
- `m_slotDyeable[]` — true if any primitive of that slot has a `DYE_MASK` (`W:9159-9165`).
- After a build, a per-merged-primitive slot tag `m_partSlot` (-1 = character, 0–4 armour, 5–8
  weapons, 9 trophy) links viewport parts back to slots (`W:9158`, `W:9129-9136`).

### 2.4 Looks / variants / dyes
- **Pigments**: every `Dye/*.dye.json` not `NPC_`/`Debug`/`Bad Data`/`fHiddenFromUI`, colours from
  `rgbaWardrobeColorSwatch` (4 zones) (`W:1117-1147`); user custom pigments in
  `wardrobe2/customPigments` as `name\t#h0\t#h1\t#h2\t#h3` (`W:1149-1162`, `W:6567-6581`).
  Applied in the shader per part (`dyeZoneColor`, `gl/GLModelWidget.cpp:506`) via
  `setPartDye(on, colors12)` (`W:4789-4817`); no geometry rebuild.
- **Themes** ("variants" of a set): `m_sets` groups slot appearances by collection (StoreProduct
  Series) or by the name token between prefix and slot code (`stor007`), dropping collections that
  claim two pieces in one slot (`W:7389-7427`, `W:7429-7448`). `resolveTheme` also opens
  `StoreProduct/*_<class>_<token>.prd.json` bundles for creator picks (`tCustomizedPreview`) and
  weapons/markings (`arBundledProducts`) (`W:7452-7531`).

### 2.5 Everything else a user chooses (and where it lives)
Nude vs underwear base (`wardrobe2/nudeBase`, `W:8002-8005`), cloth/physics tuning
(`wardrobe2/cloth/*`, `W:10048-10140`), per-attachment clip (`wardrobe2/attachClip/<appr>`,
`W:11258-11301`), weapon flips/auto-upright (`wardrobe2/weap/*`, `W:5203-5242`), lighting rig
(`wardrobe2/light/*`, `W:9987-10032`), graphics (`wardrobe2/viewport/*`, `P:381-501`), camera
(`wardrobe2/cam/*`, `W:5678-5710`; presets `wardrobe2/campreset/N/*`, `P:322-367`).

### 2.6 Persistence (QSettings, all live-written)
Current selection: `wardrobe2/class`, `/gender` (`W:2661`, `W:2671`), `/slot/0..4` = appearance name
text (`W:2757-2762`), `/weapon`, `/weapon2`, `/weaponSheath`, `/weaponSheath2` = appearance name
(`W:2770-2789`, `W:4666-4673`), `/backTrophy` = SNO as string (`W:2790-2796`), `/creator/0..8` =
def stem (`W:2731-2740`), `/skinTone` = `#rrggbb`, `/skinDetail` = token (`W:2747-2756`),
`/dye/N` + `/dyehex/N` (`W:6487-6503`), `/anim` (`W:11361`).
Restore order: selection → slot dyes → weapon index → creator → slots (+rebuild) → deferred clip +
camera (`W:2933-2959`); combos restore by data with a text fallback for old saves (`W:3349-3357`,
`W:4719-4737`).

**Presets ("Ensembles")**: `saveLook` copies the 49 keys of `wardrobeLookKeys()` (10 scalar + 9 creator + 10 slot + 20 dye) from
`wardrobe2/<key>` into `wardrobe2/looks/<name>/<key>`, adds `slotSno/0..9` for the icon strip, appends
to `wardrobe2/lookNames`, and saves a 128px base-colour front snapshot to
`<dataDir>/ensembles/<sanitized>_<hash>.png` (`W:5716-5773`). `loadLook` copies back and replays
the same restore chain as startup (`W:5775-5792`). Delete/rename move the group and thumbnail
(`W:5888-5920`).
**Undo** (Ctrl+Z): each rebuild pushes the previous look-key snapshot, cap 30; undo writes it back
into QSettings and replays the restore chain (`W:7768-7778`, `W:5795-5826`, `W:2254-2257`).

## 3. Candidate discovery

- **Index**: all lists come from `SnoIndex::entries(9)` (Appearance names) + d4data JSON folders;
  nothing is hard-coded per item. Armour = prefix/suffix name rule (`W:5395-5399`); weapons =
  hand-written prefix→ItemType table (`W:296-319`) matched against names (`W:4654-4659`); trophies =
  Item→ItemType→Actor→Appearance crawl (`index/BackTrophyIndex.h:14-24`); creator = JSON folders +
  CASC gap fill (`W:389-416`, `W:4562-4595`).
- **Class/gender rules**: armour via the class prefix in the name; weapons via ItemType
  `fUsableByClass` (cached per type, `W:4630-4645`); creator via `fUsableByClass`/`eClassRestriction`
  — explicitly **no gender filter** on creator defs, gender only gates the beard mesh
  (`W:380-388`, `W:8104-8110`).
- **Filtering / search** (Equipment page): free-text over item text, appearance name, title and
  collection (`W:6106-6111`); "collection" dropdown rebuilt per slot from `AppearanceMeta`
  (`W:6324-6346`); content-drop combo (`W:5316-5322`). No sort control; list order is SNO-index order
  (`W:5395`). Appearance page: search over label + stem, sort A–Z / Z–A (`W:1657-1671`,
  `W:7178-7191`).
- **Theme pinning**: cards matching the active theme are copied to the top under a header naming the
  collection, and outlined blue (`W:6115-6174`, `W:6724-6745`); the Marking colour authored by the
  selected Marking is outlined green (`W:6797-6812`).
- **Icons**: `LookIcon::image(sno)` = `AppearanceMeta::iconFor(sno)` (inventory-icon handle from the
  Item/StoreProduct crawl) → `IconIndex` atlas crop (`util/LookIcon.h:28-40`). Missing icon ⇒ text
  card (`W:6258-6264`). Optional ✓/✗ "payload present" badge (`W:6251-6256`). Creator icons:
  `hIconImage`, gender/class-indexed `tIcons`, generated ramp swatches for hair/eye/marking colour,
  or a swatch drawn from the marking's own mask when the atlas has no rectangle (`W:6998-7070`,
  `W:6933-6994`).
- **Names shown**: armour/weapon title and collection from `AppearanceMeta` (StoreProduct `Name` /
  `Series` StringLists, `index/AppearanceMeta.cpp:523-538`); header
  "`<Slot> (N of M) - <title> - <collection> - <file>`" (`W:6815-6849`); hover tooltip from
  `ItemHoverIndex` (rarity, flavour, season, bundle text), each line toggleable
  (`W:6016-6070`). Creator names from `enUS_Text/.../StringList/<Folder>_<stem>.stl.json`, gendered
  variant picked for facial hair, else a de-noised stem (`W:528-554`, `W:507-526`).
- **Lazy grid**: first 60 cards, +48 when scrolled within 400 px of the end, reflow on width change
  keeping scroll fraction (`W:6176-6179`, `W:1611-1614`, `W:4218-4243`).

## 4. Assembly

`rebuildOutfitImpl` (`W:7753-9037`) builds a list of parsed pieces, merges them, then decodes
textures (optionally on a worker) and `applyOutfit` (`W:9040-9414`) pushes everything to the view.

1. **Rig carrier**: `<prefix>_base00` is parsed first and kept for its skeleton only (mesh dropped),
   so the merged rig's base bones and hardpoints are the full animation rig; Warlock is special-cased
   to draw base00 as its default body (`W:7931-7980`).
2. **Head**: `<prefix>_p<NN>` where NN = Face def `dwSubObjectStyle` (`W:7981-7996`, `W:418-429`).
3. **Body slots**: equipped armour, else the first of `test999/base01/02/03` (order set by
   `nudeBase`) that parses; helm slot has no fallback (bare head comes from the face)
   (`W:7998-8019`).
4. **Back trophy**: seated on the body socket — authored `tAttachmentHardpointLink` if any, else
   `HP_chestBack` → `HP_back` → `HP_spineMid` → `HP_chest`; baked static, or kept as a salted sub-rig
   if it has its own bones and clips (`W:8020-8085`, `W:4967-5012`).
5. **Creator meshes**: hair `<prefix>_h<NN>`, male beard `<prefix>_b<NN>`, jewelry
   `jwl<NN>_<prefix>` (NN from each def) (`W:8086-8116`).
6. **Weapons** (`W:8119-8201`): held weapons seated by `seatWeapon` = body bone world ·
   `HP_rightWeapon`/`HP_leftWeapon` socket · ItemType `tHardpointOffsets[classRow]` grip, plus an
   unconditional mesh-space 180° roll, user flip/invert, and a measured auto-upright correction; verts
   baked 100 % to the hand bone (`W:5020-5314`). Sheathed weapons go rigidly to a per-ItemType sheath
   socket list (`W:4872-4895`). Weapons with their own clips, or flails (`uRagdollDegrade`), keep a
   **salted sub-rig** (`ModelAttach::attachSubRigAt`, `model/Attachments.h:109`; salt per socket
   `W:4991-4999`; flail detection `W:5543-5562`).
7. **Skeleton merge**: `ModelParser::mergeGeometries` unifies bones by name hash, appends new bones
   parents-first, unions the cloth flag (`model/ModelParser.cpp:1146-1186`). Sub-rig bones are
   re-hashed with `saltBoneHash` so they cannot fuse (`model/Attachments.h:65-75`).
8. **Material resolution**: per piece, the material roster from `.app.json` or, for encrypted
   appearances, the CASC meta binary (`MaterialDecode::appearanceRosterAny`, `W:7880-7889`, weapons
   `W:8141-8149`). Substitutions: `skin_mat` placeholder → face's `_BOD` skin; facial-hair slot
   (found by name or by shader `hero_opaque_hollow`) → selected FacialHair shell material, with a
   rescue for an empty authored slot (`W:8545-8615`). Classification of each part into
   hair/head/body/eye/skin uses shader names first and name substrings second, gated so equipment is
   never "head" (`W:8619-8653`, `W:8560-8585`).
9. **Hiding body under armour**: normally by **replacement** — an equipped slot suppresses that
   slot's nude/underwear piece (`W:7998-8012`). For Warlock's whole-body base00 the dominant
   `slotHash` region of each equipped torso/gloves/legs/boots piece hides the base00 primitives with
   the same hash (`[COVERED]`, re-checkable) (`W:9178-9209`, `W:3434-3441`). No rule hides hair,
   face or beard under a helm: hair loads whenever a style resolves (`W:8095-8099`).
10. **Visibility categories**: FX (vfx_* shader, else name tokens), SIM (appearance's own cloth
    roster ∨ `Cloth/<name>.clt.json` ∨ name tokens), FORM (`demonform`), HED (head group expanded to
    the whole creator piece) (`W:9197-9270`), combined with the parts-tree checkboxes
    (`W:3365-3394`).
11. **Cloth/physics**: per-sim authored tuning from `Cloth/<name>_sim.clt.json`
    (`W:10401-10434`, `ModelParser::resolveClothTuning`), an outfit-average of
    `dmClothTuningMirror` (`W:10662-10732`), then slider overrides (`W:10048-10140`); solver lives in
    `GLModelWidget` (`gl/GLModelWidget.h:280`, `:332`).
12. **Hair / face / markings** (all CPU image compositing before upload): hair gradient-mapped
    through the HairColor ramp by `MASK_PRIMARY`, AO split into ORM (`W:645-793`, `W:8657-8665`,
    `W:8706-8733`); skin tone = luminance × normalized swatch (`W:8433-8449`); eyes composed from the
    shared `base_eyes_*` textures and EyeColor scalars (`W:863-1040`, `W:10739-10774`); makeup
    alpha-over on head (`W:1042-1072`, `W:8742-8744`); markings via `applyMarkingMaterial`
    (R = coverage, G = ramp position, `tabs/MarkingCompose.h:8-13`, `W:8738-8748`); freckle/vitiligo
    overlay at 0.6 (`W:8674-8677`). Plus fur shells, mesh FX, emissive, detail maps
    (`W:8837-8961`).

## 5. Animation

- **Clip list**: every `Anim/*.ani.json` whose name starts with the class prefix **and** whose
  `snoAppearance` is `<prefix>_base00` (`W:10849-10880`); cached to
  `wardrobe_anims_v1.json` signed by `buildVersion.txt` + dir mtimes (`W:10799-10841`,
  `W:10924-10938`).
- **Pull from another rig**: only the other gender of the same class, merged when it has more than
  twice as many clips (Paladin F, Spiritborn M, Warlock F) (`W:10885-10921`). Bones match by hash;
  there is no retargeting between different skeletons — clips decode against the merged skeleton's
  rest pose (`W:11092-11126`).
- **Presentation**: category filter by name token, sort by name/frames, search, readable action
  prefix from `AnimActionIndex` (AnimSet power → clip) (`W:1397-1422`, `W:10953-11012`,
  `index/AnimActionIndex.h:25`). Multi-select for export (`W:1426`).
- **Auto-animate** (opt-in `wardrobe2/autoAnimate`): derive the game's `eWeaponClass` for the two
  hands (incl. dual-wield/shield/off-hand combination classes tried against what the class ships),
  play the `*_ui_wardrobe` AnimSet's `ui_wardrobe_unSheathe` once, then loop `ui_wardrobe_idle`;
  only when class/gender/weapon-class changes (`W:2963-3130`, `index/WardrobeAnimIndex.h:8-33`,
  `W:11446-11455`).
- **Attachments**: rigged trophies/weapons play their own clips (from `BackTrophyIndex::clipsFor`,
  matched by the clip's `snoAppearance`) on independent clocks appended to the body clip with salted
  hashes (`W:11315-11349`, `W:11351-11390`); pinned "ATTACHED" list under the character's clips
  (`W:1461-1490`, `W:11023-11078`).
- **Continuity**: the playing clip and frame survive a rebuild (`W:7780-7784`, `W:9374-9386`); on
  class/gender change the clip is remapped by swapping the 4-char prefix, or cleared
  (`W:11475-11490`).

## 6. UI layout

1. **Hint bar** (dismissable): "double-click … · F fullscreen · Ctrl+Z · F1" (`W:1272-1276`).
2. **Left column** (scrollable, ≥470 px, `W:2620-2625`):
   - `CHARACTER` label; Class + Gender combos; content-drop combo (`W:1286-1317`).
   - **Equipment | Appearance** toggle over a stacked page (`W:1319-1334`).
   - *Equipment page*: one row of 10 **slot cells** (icon + label; 5 armour · gap · 4 weapons · gap ·
     back) with pigment badge (`W:1541-1551`, `W:6851-6894`); **Set Look | Set Pigment** toggle
     (`W:1557-1568`); header; pigment editor (hidden unless Set Pigment: apply-to-all, colour wheel,
     4 zone buttons with drag/drop, hex, Save Pigment, 8 memory swatches) (`W:6507-6617`); search +
     collection filter (`W:1576-1592`); **card grid** (QGridLayout of QToolButtons, lazy, weapon type
     dividers; pigment mode swaps it for 46 px swatches) (`W:1597-1615`, `W:6186-6322`,
     `W:6399-6483`).
   - *Appearance page*: 9 category cells in a 5×2 grid, header, search + sort, card grid (colour
     categories as dense swatches), Skin tone and Skin detail combos (`W:1629-1708`, `W:7152-7305`).
   - **ENSEMBLES**: one-row IconMode list of snapshot tiles + Save / Overwrite / Delete / Rename
     (`W:5922-5996`).
   - Settings / Reset to default / Copy debug buttons, status line (last two only with
     `wardrobe2/dbg/log`) (`W:1715-1744`, `W:3161-3164`).
3. **Centre**: toolbar — 4 shading spheres + ⌄ channel popover (9 channels incl. "Dye zones"),
   Overlays master toggle + ⌄ panel (grid, gizmo, skeleton, collision model, physics bones,
   hardpoints, bone names), **FX / SIM / HED / FORM** toggles (`W:1794-2139`); viewport with right-edge
   N-strip (» hide sidebar, Graphics, Camera, Lighting, Shaders, Detail maps, Physics, ⛶)
   (`W:2150-2192`; panels in `P:121-1334`); transport bar under the viewport (`W:1336-1385`).
4. **Right column**: icon strip toggling stackable PanelBoxes — PARTS (tree: source piece → submesh,
   checkbox + tris + `[FX]/[SIM]/[FORM]/[COVERED]` tags), MATERIALS (4 tabs), MATERIAL TEXTURES
   (Textures/Values/Shaders/Detail), TEXTURE PREVIEW (6 hover-zoom tiles), ANIMATIONS; order, heights
   and visibility persisted (`W:2344-2569`, `W:3413-3476`).

**Hotkeys**: F fullscreen (viewport-scoped) / Esc exits (`W:2251-2253`, `W:9447-9455`); Ctrl+Z undo
(`W:2255-2257`); Esc clears part selection (`W:4049-4056`); H hide · Shift+H solo · Alt+H show all
(viewport or tree, `W:4058-4094`); arrows move a cursor in the look grid, Enter/Space equips
(`W:7083-7123`); arrows move+apply in pigment/creator grids (`W:7126-7150`); wheel over the channel ⌄
cycles channels (`W:4018-4027`); click / Ctrl-click select parts, double-click frames and jumps to
that part's slot (`W:2197-2244`); middle-click reframes (`W:1769`).
**Context menus**: slot cell = Clear, Copy/Paste/Clear pigment, image, export, copy
(`W:1513-1538`, shared `addItemActions` `W:5476-5519`); look card = Equip, Equip Theme / Armor /
Markings / Weapons with a preview of what each equips, then the shared block (`W:6287-6315`);
creator card = same theme block + copy (`W:7256-7289`); pigment swatch = apply to slot/all, copy,
delete custom (`W:6437-6464`); viewport/parts part menu = one shared `ViewportPartMenu`
(hide/isolate/frame/export part or source piece/explain material) (`W:10202-10384`); animation
list = export library, copy file name (`W:1430-1454`); spec in `CONTEXT_MENUS.md:276`.

## 7. Export

| What | How | Evidence |
|---|---|---|
| Whole outfit `.glb` (skinned, per-part materials) | `exportSelection` → `exportOutfitGlb(visible parts)`; filename from `export/wardrobeNameTemplate` (`{{Class}}`, `{{Gender}}`, `{{Helm}}`…, `{{Collection}}`, `{{Name}}`, `{{Date}}`) | `W:9862-9886`, `W:9502-9699`, `W:9778-9860` |
| Scope | `export/wardrobeScope`: 0 = as seen, 1 = items only (character + armour-carried skin stripped), 2 = items + untextured body/head fit reference (marking-or-black base) | `W:9721-9776`, `W:9537-9615` |
| Options | include textures, bake dye into base colour, bake detail into normal/ORM, emissive, auto alpha-cutout, re-include hidden FX/SIM/FORM, hardpoint empties, retarget (collapse cloth / remap to anchors), Blender or translated bone names | `W:9505-9507`, `W:9616-9688`, `model/Retarget.h:12-32` |
| Animations | none / playing / Ctrl-selected / all listed (asks above 25) | `W:11179-11253` |
| Both genders | writes `_M`/`_F`, flips gender through the real handler, re-exports, flips back | `W:9888-9954`, `W:9974-9975` |
| Last folder | same scope rules without a dialog | `W:9956-9976` |
| Part / source piece | from the part menu, hidden parts included | `W:10162-10186`, `W:10354-10367` |
| One item model | from a card: re-parse the appearance, Models-tab material pipeline, optional detail bake and raw `deps/` (.app + .tex) | `W:4400-4518` |
| Animation library | rig only + selected clips | `W:4328-4388` |
| Images / GIFs | not in the tab: top Export menu → `ExportCapture::saveImage / turntableGif / animLoopGif` on `previewWidget()` | `W:9978-9979`, `app/MainWindow.cpp:337-342`, `app/ExportCapture.h:24-31` |
| Icon image | Copy / Save image on any card | `util/LookIcon.h:49-67` |

## 8. Robustness

- **Re-entrancy**: `m_rebuilding` guard on rebuild and apply (`W:7755-7762`, `W:9042-9045`);
  `m_restoring` suppresses handlers during batched repopulates (`H:480`, `W:2662-2666`).
- **Crash guards**: `seh::runGuarded` around every parse, merge, GPU upload, anim decode and
  setAnimation (`W:7865`, `W:7949`, `W:8139`, `W:8212`, `W:9087`, `W:11123`, `W:11362`). Crash
  breadcrumb `wardrobe2/_loading` written before load and removed by a scope guard; if it survives,
  `MainWindow` clears the remembered outfit and opens a safe tab (`W:7800-7816`,
  `app/MainWindow.cpp:130-172`). One-time wipe of a crash-looping saved outfit
  (`W:2847-2860`).
- **Debounce + async**: 35 ms coalescing timer, decode on the thread pool with a generation token;
  the old model stays until the new one is fully decoded (`W:7729-7751`, `W:9015-9036`,
  `W:8221-8223`).
- **Caches**: 256 MB LRU of raw decodes keyed `role|material` (`W:1256-1258`, `W:8514-8536`);
  per-build dedup hashes (`W:8504-8508`); optional GPU texture pool keyed material + colour epoch
  (`W:8298-8309`, `W:9078-9081`); anim list disk cache (`W:10799-10841`); static memo caches for
  creator icons, marking colours, weapon ItemTypes, flail flags (`W:7003`, `W:6759`, `W:4632`,
  `W:5546`).
- **Background indexes**: `AppearanceMeta`, `ItemHoverIndex`, `BackTrophyIndex`,
  `WardrobeAnimIndex`, `IconIndex`, each refilling the grids on `readyChanged` (`W:2861`,
  `W:2906-2931`).
- **Performance instrumentation**: per-stage timings logged on every rebuild (`W:8994-9012`,
  `W:9405-9413`, `W:10946-10948`).
- **Diagnostics** (env-gated): `D4_DUMP_PIECEROSTER`, `D4_DUMP_HAIRMASK`, `D4_DUMP_TROPHYANIM`,
  `D4_MARKING_SWEEP`, corpus audits `D4_CLOTH_AUDIT`/`D4_HEALTH_AUDIT`/`D4_MATSNO_SWEEP`/
  `D4_CHAINTEST` (`W:3494-3604`, `W:701-743`, `W:4928`, `W:8389`, `W:2862-2905`); decoder self-tests
  at construction (`W:1242-1255`); "Copy debug" console (`W:11396-11425`).

## 9. Game-specific vs generic

| Feature | Class | Reason |
|---|---|---|
| Axes (class, gender) selecting a base rig | GENERIC | every customizer has a body-type axis; values are data |
| Class×gender name prefix (`barf_`) as lookup key | GAME-SPECIFIC | D4 naming convention (`W:3132`) |
| Content-drop filter by TACT key | GAME-SPECIFIC | Blizzard encryption grouping (`W:5328`) |
| "Collection" filter + search + theme pinning | GENERIC | any game groups cosmetics into sets |
| Store-bundle theme resolution | GAME-SPECIFIC | StoreProduct `.prd.json` layout (`W:7481-7528`) |
| Slot list 0–9, creator categories C0–C8 | GAME-SPECIFIC (values) / GENERIC (shape) | slots are data; D4 hard-codes them (`W:328`, `W:364`) |
| Armour candidates by name prefix/suffix | GAME-SPECIFIC | D4 appearance naming (`W:5395-5399`) |
| Weapon prefix→ItemType table, hand eligibility | GAME-SPECIFIC | hand-written D4 table (`W:296-319`) |
| Trophy Item→ItemType→Actor→Appearance crawl | GAME-SPECIFIC | D4 record chain |
| Class restriction via `fUsableByClass` | GAME-SPECIFIC | D4 field; the idea "filter by current axes" is GENERIC |
| Hidden-combo-per-slot + icon card grid | GENERIC (grid) | the hidden combos are an implementation artefact |
| Icon lookup via inventory atlas | GAME-SPECIFIC | D4 icon handles/atlases; "an icon per candidate" is GENERIC |
| Hover info (rarity, season, flavour) | GENERIC (shape) | fields are D4 |
| Per-slot 4-zone pigments + custom pigments | GAME-SPECIFIC (4 DyeMask zones) | "tint a slot" is GENERIC |
| Pigment copy/paste/apply-all, colour memory | GENERIC | UI over any tint model |
| Rig carrier `_base00`, head `_pNN`, nude fallback tokens | GAME-SPECIFIC | D4 content layout (`W:7931-8019`) |
| Bone-hash skeleton merge, parents-first | GENERIC | any skinned modular character |
| Socket seating (bone · hardpoint · grip offset) | GENERIC | hardpoints exist in most engines |
| Class-row grip offsets, 180° held roll, auto-upright | GAME-SPECIFIC | D4 ItemType/rig quirks (`W:5080-5277`) |
| Sheath socket priority table | GAME-SPECIFIC | D4 hardpoint hashes (`W:4875-4892`) |
| Salted sub-rig for animated attachments | GENERIC | any engine with rigged props |
| Encrypted-material roster from CASC meta | GAME-SPECIFIC | D4 storage |
| Hide body under armour by slot replacement | GENERIC | "a slot's piece replaces the default" |
| Hide by `slotHash` region (Warlock) | GAME-SPECIFIC | D4 submesh tag |
| FX/SIM/FORM/HED category toggles | GENERIC (idea) / GAME-SPECIFIC (classifiers) | shaders/tokens are D4 |
| Cloth solver + authored tuning | LOOK | D4 viewport extension (`CORE_EXTRACTION_PLAN.md:289`) |
| Hair ramp, eye composite, skin recolour, makeup, markings, freckles | LOOK | D4 shader-model emulation on CPU |
| Fur shells, mesh FX, detail maps, emissive, dye shader | LOOK | D4 materials |
| Lighting rig from D4 character screen, reflection probe | LOOK | D4 assets (`W:3297-3320`) |
| Animation list scoped to the base rig | GENERIC | "clips that drive this rig" |
| Other-gender clip borrow | GENERIC (idea) | threshold rule is D4-measured |
| Auto-animate via `ui_wardrobe` AnimSets / weapon class | GAME-SPECIFIC | D4 data chain |
| Attachment clips on own clocks | GENERIC | |
| Ensembles (named presets with thumbnails) | GENERIC | |
| Undo stack | GENERIC | |
| Gender-swap outfit retention | GENERIC (idea) / GAME-SPECIFIC (4th-char swap) | |
| Parts tree, part menu, channel viewer, overlays | GENERIC | shared with Models tab |
| Camera snap to slot, follow, presets, turntable | GENERIC | uses part→slot tags only |
| Export outfit/scope/both genders/part/item/anim library | GENERIC | name template placeholders are the only slot-aware bit |
| Bake dye / detail on export | LOOK | depends on D4 material model |
| Crash breadcrumb, debounce, async decode, LRU, generation token | GENERIC | |

## 10. What a game must supply

A proposed provider interface — the questions the tab actually asks, and where D4 answers each.

| # | Question | D4 answer today |
|---|---|---|
| 1 | `axes()` — list the body axes and their values (class, gender…) | `kClasses()` `W:342-354`; gender combo `W:1291-1293` |
| 2 | `slots(axes)` — slot ids, labels, group (armour/weapon/back/creator/skin), kind (mesh / colour / paint), dyeable? | `kSlots` `W:328`, `slotLabel` `W:5576-5587`, `kCreator` `W:364-373`; dyeable measured after load `W:9159-9165` |
| 3 | `candidates(slot, axes, filters)` — id, file name, display name, collection, icon ref, tags | `populateSlots` `W:5384-5461`, `populateWeapons` `W:4675-4749`, `populateCreator` `W:4597-4625`, `AppearanceMeta` titles/collections |
| 4 | `slotAvailable(slot, axes)` — can this character use the slot at all | `updateWeaponSlotAvailability` `W:4756-4775` |
| 5 | `icon(candidate)` | `LookIcon::image` `util/LookIcon.h:28`; `creatorIconImage` `W:6998` |
| 6 | `hoverInfo(candidate)` | `infoTip` + `ItemHoverIndex` `W:6020-6070` |
| 7 | `groups(candidate)` / `theme(candidate)` — which other pieces complete its set | `populateSets` `W:7391`, `resolveTheme` `W:7452-7531` |
| 8 | `baseRig(axes)` and `defaultPiece(slot, axes)` when nothing is equipped | base00 `W:7931-7980`; face `W:7984-7996`; nude tokens `W:8002-8018` |
| 9 | `resolve(candidate)` → mesh(es) + material roster | `addPiece` `W:7837-7929`, `appearanceRosterAny` `W:7886` |
| 10 | `derivedPieces(selection)` — meshes implied by non-mesh choices (hair style → hair mesh) | creator meshes `W:8086-8116` |
| 11 | `placement(candidate, slot)` → Merge by bone hash, or Socket(hash, offset) baked, or Socket keeping a sub-rig | armour merge `W:8211`; `seatWeapon` `W:5020`; `sheathHardpointFor` `W:4872`; trophy `W:4967-5012`; sub-rig `W:8158-8195` |
| 12 | `hides(piece, selection)` — which default/body parts a piece suppresses | slot replacement `W:8010`; `slotHash` coverage `W:9178-9209` |
| 13 | `partCategory(part)` — FX / cloth / head / alternate form (for toggles and export scope) | `W:9197-9270`, `W:8812-8828` |
| 14 | `tints(slot)` + how a tint is applied (zones, shader uniform) | `loadPlayerDyes` `W:1120`, `setPartDye` `W:4815` |
| 15 | `compose(selection)` — colour/paint choices that edit textures (hair, eyes, skin, markings) | decode lambda `W:8487-8983` |
| 16 | `clips(rig, axes)` + optional `fallbackRig` | `populateAnims` `W:10780-10949` |
| 17 | `idleFor(loadout)` — what the game plays when this loadout is shown | `WardrobeAnimIndex::clipsFor` `W:3069-3107` |
| 18 | `clipsOf(attachment)` | `BackTrophyIndex::clipsFor` `index/BackTrophyIndex.h:64-69` |
| 19 | `counterpart(candidate, otherAxisValue)` — same piece for the other gender | 4th-char swap `W:2698-2707` |
| 20 | `sockets(rig)` — named hardpoints for overlays/export | `Hardpoints::readInto` `W:9174` |

Everything else (grid, presets, undo, parts tree, export plumbing, camera) needs nothing from the
game beyond these answers.

## 11. Strengths worth keeping / weaknesses

### Strengths
- **Data-driven chains with measured justifications**: trophies via ItemType reference, not name
  (`index/BackTrophyIndex.h:8-33`); weapon grips from the class's own row
  (`W:5080-5133`); auto-animate from the game's wardrobe AnimSets (`index/WardrobeAnimIndex.h:8-33`).
- **Encrypted-content support** end to end: CASC-only markings, meta-binary material rosters,
  content-drop filter (`W:4538-4595`, `W:7880-7889`, `W:5328-5382`).
- **Stable rebuild UX**: debounce, async decode with generation token, previous model kept on screen,
  animation and camera preserved (`W:7731-7751`, `W:9015-9036`, `W:9374-9386`).
- **Ensembles + 30-deep undo** over one key list (`W:5727-5745`, `W:5806-5826`).
- **Themes**: one right-click equips a whole set with a preview of what it will equip, and a count
  of missing pieces (`W:6298-6306`, `W:7707-7719`).
- **Sub-rig attachments** with per-socket salt and independent clocks — a clean, generic mechanism
  (`model/Attachments.h:64-121`, `W:11315-11349`).
- **Export scopes** that say what they did (`W:9756-9774`, `W:9882-9885`); both-genders reuses the
  real gender path (`W:9914-9919`).
- **Crash self-healing** breadcrumb (`W:7800-7816`, `app/MainWindow.cpp:130-172`).

### Weaknesses (with evidence)
- **God class**: 13.5 k lines; constructor 1,597 lines (`W:1240-2837`), `rebuildOutfitImpl` 1,285
  lines (`W:7753-9037`), `eventFilter` 281 lines (`W:4013-4294`). Game rules, UI and rendering
  emulation are interleaved.
- **Hidden QComboBoxes as the model** (`W:1496-1499`, `W:1616-1626`, `W:1631`): identity is a combo
  index; weapon display text collided for 149 entries and needed `UserRole+1` keys (`W:4719-4729`).
- **QSettings as the source of truth**: undo snapshots and presets read QSettings, rebuild reads the
  widgets; the code documents the two diverging (`W:3343-3348`). Presets are not portable files.
- **Out-of-bounds read on every outfit export**: `exportNameStem` indexes `m_slot[slot]` for slots 5
  and 6 (`{{Main}}`/`{{Off}}`) although `m_slot` has 5 entries (`W:9787-9806`, `W:9831`,
  `H:255`) — undefined behaviour; the next member is `m_slotCells` (`H:261`). Should use
  `slotCombo()`.
- **`reset()` is partial**: clears markings, texture map and `m_loaded` only (`H:168-173`); the
  weapon index (`m_weapReady`, `W:4652`), `m_texCache`, and static memo caches keyed without the
  data dir (`W:4632`, `W:5546`, `W:7003`, `util/LookIcon.h:32`) survive an install switch.
- **Dye changes are not independently undoable**: pigments apply without a rebuild
  (`W:6487-6503`), and undo snapshots are only taken inside a rebuild (`W:7771-7778`).
- **Name heuristics remain load-bearing**: FX/SIM token lists (`W:168-182`), head-group tokens
  (`brow`, `lash`…, `W:8819-8828`), `prefix.startsWith("war")` (`W:7955`), skin tones read from
  `barbarianF` only (`W:451`); `HYGIENE_TOOLING.md:39-45` counts 18 such sites in the classification
  loop.
- **Dead or vestigial members**: `m_env` never assigned but read four times (`W:2939`, `W:3331`,
  `W:7373`, `W:9365`); `m_fsBtn` always null (`W:1771-1772`); `m_iconProgress` never shown
  (`W:1593-1595`, `W:6182`); `m_weaponType/2` retired (`W:2763-2769`); `m_physChkBones/Axis` null
  (`H:570-573`); `region` always 0 (`W:8832`); `kHairColorCat` unused (`W:374`); `slot/5..9` in
  `wardrobeLookKeys` never written (`W:5742`, cf. `W:2757-2762`).
- **Unconditional side effects**: `fur_probe.txt` rewritten beside the exe on every rebuild
  (`W:9146-9152`); `dumpTrophyAnim` not env-gated (`W:4902-4905`); the debug log lives in a label's
  tooltip (`W:9398`, `W:11398`).
- **Repeated linear scans and uncached JSON reads per rebuild**: `resolveApp`/`addPiece` walk all
  Appearance entries per lookup (`W:7819-7823`, `W:7842-7844`); `baseAppJsonPath` likewise
  (`W:4829-4831`); `isFurMaterial` re-reads JSON per part (`W:245-257`, `W:8838`); `loadPlayerDyes`
  re-reads every Dye file per pigment-grid fill (`W:6408`); `rebuildPartList` builds a QSettings per
  primitive (`W:3447-3448`). The logged first-rebuild stall is still being chased (`W:8994-9010`).
- **UX gaps**: no sort in the equipment grid (only index order, `W:5395`); helm does not hide hair
  (`W:8095-8099`); Equip Theme Weapons fills only held hands, never sheaths (`W:7671-7672`); weapon
  orientation options live in the global Settings dialog, not the tab (`W:1586-1587`); two nested
  mode toggles (Equipment/Appearance, then Set Look/Set Pigment) before a user reaches a grid
  (`W:1319-1334`, `W:1557-1568`).
