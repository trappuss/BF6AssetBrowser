# DzKit gap analysis & work queue

**Source:** 44 s screen recording posted by `@4v_em73137` (Twitter, 2026-10-31), showing a
rival Destiny Rising asset tool titled **"DzKit"**. Frames were extracted and read at
2 fps / native res. This doc is monorepo-only (lives in `docs/`, which is not part of the
`DRAssetBrowser` standalone publish) so it may reference game structure freely; **no extracted
asset data is stored here.**

---

## 1. What DzKit is — observed facts

DzKit is a single-window Qt-style asset browser with three top tabs:
**Asset browser · Guns & Characters · ETSB visualizer**. Toolbar (shared):
`Root: …` path field · **Browse…** · **Scan** · **Save manifest** · **Export all ETSB** · ☑ **Read headers**.

### 1a. Ingestion — reads the emulator VDI, not the loose patch folder  ← the root difference
- Root is `\\?\D:\Program Files\Netease\MuMuPlayer\vms\MuMuPlayerGlobal-12.0-0\data.vdi`.
- Status line: **"Loaded 249717 assets from VDI (4bb889c0d392)"**.
- i.e. it opens the MuMu Android emulator's **VirtualBox disk image** and reads the game's
  *full installed data* — crucially the **resource manifest** that maps each asset GUID to a
  **readable logical path**. That manifest is what we do not currently have.

### 1b. Asset browser — full type taxonomy with readable names
Left "Asset types" list with live counts (total **All 249717**):

| Type | Count | | Type | Count |
|---|---:|---|---|---:|
| Model | 61636 | | Split Asset | 2767 |
| Texture Bundle | 43071 | | Script | 1196 |
| Unknown | 36146 | | Config | 921 |
| Audio | 21169 | | Motion | 390 |
| Material | 20612 | | Cinematic | 334 |
| Skeleton | 13464 | | Sound Bank | 248 |
| Animation | 12687 | | Audio Package | 202 |
| Face Animation | 9473 | | Video | 97 |
| Particle | 9395 | | Shader | 67 |
| Physics | 8737 | | UI Script | 27 |
| UI Layout | 4119 | | | |
| Graph | 2959 | | | |

- Every row has a **readable name + path** (e.g. `…/OG_Achieve_TaskProgress.csb`,
  `OG_Activity_Content_Crota.csb`), not a GUID. Columns: **Name · Type · Size · Detail · Path · GUID**.
- Search box: "name, path…". Multi-select with **Select all / Clear selection / N selected**.
- Right pane: **Model preview** ("Select a model asset to preview").

### 1c. Guns & Characters — curated, deterministic entity browser
- Two sub-tabs **Guns | Characters**. Guns shows **307 entries**.
- Filter chips: **all · weapon · relic · item**. Search "name, code, type or model".
- Rows: **display name + weapon code + family**, e.g.
  `'Til Eternal Death — aw08s05 · Hand Cannon`, `Arbalest — aw05s03 · Linear Fusion Rifle`,
  `A67 "Javan Rhino" — aw15b02 · Rocket Launcher`. Families seen: Hand Cannon, Sniper Rifle,
  Scout Rifle, Pulse Rifle, Submachine Gun, Grenade Launcher, Rocket Launcher, Linear Fusion Rifle.
- **Detail view** for the selected weapon:
  - Header: `category: weapon · name source: localized · assets: 37`
  - `model folder: char/weapon/handcannon/gdn_hc_<name>_lv4`  ← resolved from the path manifest
  - `tags: @weapon_launch$aw08s05, @weapon_launch$aw08s05@6900031, …` (parses the game tag system)
  - Controls: **LOD [LOD0–4] · Variant [All assets ▾] · Preview whole · Export variant…**
  - Asset sub-tabs with counts: **Models (11) · Materials (6) · Textures (13) · Skeletons (7) · Other (0)**,
    plus **Select in asset browser**.
  - Per-row table **Name · Type · Actions(Browse/Preview/Copy) · Folder · GUID**. Rows show the full
    LOD chain + variants: `…_main_lv4_lod0_mesh … lod4_mesh`, `…_shadow_mesh`, `…_red_lv4_lod0_mesh`.

### 1d. ETSB visualizer — scene/prefab reflection-tree viewer
- Opens `.etsb` files (e.g. `…/Patch/Prefab/Client/Ets_bin/SQ_OldCity-M1.etsb`) as a **Document** tree.
- Entity nodes keyed by GUID, each with **Edit {n} · EntityID · GuideUI · PrefabRootEntityID · Property {n}**.
- Properties are the engine's typed component fields (floats/bools/strings/vec arrays), e.g.
  `capsuleModelBornAnglePitch: -23.00`, `knockPlayerMultiplier: 5.0`, `monsterBornTag: 8`,
  `SHSampleOffsetVec3 [3]`, `position [3]`. **Export all ETSB** dumps them in bulk.

---

## 2. Gap table — DzKit vs our DRAssetBrowser

| # | DzKit capability | Ours today | Gap |
|---|---|---|---|
| G0 | **GUID→logical-path manifest** (readable names + folders for ~250k assets) | names only for cooked-material-named entities; rest are GUIDs | **root enabler, missing** |
| G1 | Deterministic entity→mesh by folder path (`char/weapon/handcannon/…`) | heuristic **UV-seam matcher** over a 3.8 GB signature blob | matcher is a workaround for G0 |
| G2 | Reads emulator **VDI** (whole install) | reads loose `LocalData/Patch` MPKs | may be missing the manifest/other asset classes |
| G3 | Full type taxonomy incl. Texture Bundle, Face Animation, Particle, Physics, UI Layout, Graph, Split Asset, Config, Motion, Cinematic, Audio Package, Video, UI Script | Models/Textures/Materials/Skeleton/Anim/Sound | ~12 asset classes unsurfaced |
| G4 | **ETSB visualizer** (scene/prefab document tree, typed props, export all) | BXDoc for skeletons only | no general reflection-doc browser |
| G5 | Weapon detail: LOD selector, Variant, Preview whole, Export variant, per-type sub-tabs, Browse/Preview/Copy per row | single mesh + apply-material | detail view much thinner |
| G6 | LOD-chain + variant awareness (lod0–4, shadow, colour variants) | not modelled | missing |
| G7 | Unified search by name/code/type/model; chips all/weapon/relic/item; **relic & item** categories | name/tag search; Weapons/Characters/Combatants | no code search, no relic/item |
| G8 | **Scan + Save manifest** (persist discovered index) + **Read headers** toggle (lazy vs full) | TSV catalog of resolved meshes only | no full manifest persist / lazy-header mode |
| G9 | `.csb` UI-layout browsing with readable names | not surfaced | missing |

---

## 3. Work queue (prioritized)

### P0 — Root enabler: the asset manifest  *(unblocks G0, G1, G3, G7, G9)*
1. **Locate the GUID→logical-path manifest.** DzKit gets it from the full install inside the
   MuMu VDI. Determine whether the same table exists in our `LocalData/Patch` set (a resource
   DB / catalog file), or whether we must read it from the VDI the way DzKit does.
   - Research task (REA/hex): find the file that maps `guid → char/weapon/handcannon/…`.
   - If only in the VDI: evaluate mounting/reading `data.vdi` (VirtualBox VDI → ext4) read-only
     and pulling the install's resource manifest. Record findings in a field note.
2. **Ingest the manifest** into `DrStore`: every record gains `logicalName` + `folderPath`.
   Surface **Path** and readable **Name** columns in the list/tooltip/export.

### P1 — Retire the matcher, deterministic binding  *(G1, G5, G6)*
3. With paths available, bind weapon/character → body mesh **by folder convention**
   (`char/weapon/<family>/gdn_<type>_<name>_lv4/…`), not UV-seam scoring. Keep the matcher
   only as a fallback for assets with no path. This also makes "Load all" instant.
4. Model the **LOD chain + variants** (`lod0..lod4`, `shadow`, colour variants) as a group on the
   entity; add a **LOD** selector and **Variant** dropdown to the detail/showpiece view.

### P2 — Detail view parity  *(G5)*
5. Entity detail panel: asset-type **sub-tabs with counts** (Models/Materials/Textures/Skeletons/Other),
   per-row **Browse / Preview / Copy**, **Preview whole**, **Export variant**, **Select in asset browser**,
   and the header line (`category · name source · assets · model folder · tags`).

### P3 — New asset classes  *(G3, G9)*
6. Add type detection + list facets for the unsurfaced classes, ordered by payoff:
   **Texture Bundle**, **Particle**, **Physics**, **UI Layout (.csb)**, **Face Animation**,
   **Graph**, **Config**, **Motion**, **Cinematic**, **Audio Package**, **Video**, **Split Asset**, **UI Script**.
   Even name-only listing (no decode) is useful once G0 lands.

### P4 — ETSB visualizer  *(G4)*
7. Parse `.etsb` (Prefab/Client/Ets_bin) into an entity/property document tree; reuse the BXDoc
   reflection reader if the container overlaps. Read-only tree first; **Export all ETSB** (JSON) after.

### P5 — Search & workflow polish  *(G7, G8)*
8. Extend the shared matcher: search by **weapon code** (`aw08s05`) and by **model name**; add
   **relic** / **item** category facets if the manifest distinguishes them.
9. **Save manifest** (persist the full discovered index, version-stamped) and a **Read headers**
   lazy/full toggle so first open is fast on 250k assets.

---

## 3b. P0 investigation findings (2026-11-01)

Investigated with the REA tooling against the real Patch set
(`DRAssetBrowser-gamedata/Patch`, 8 × `.mpk`/`.mpkinfo`) and `REA-Destiny-Rising/`.
The "manifest" is not one file; the data splits three ways:

1. **~20% of assets are ALREADY logical-path-named in the mpkinfo we parse.** The `.mpkinfo`
   `name` field is the real logical path for the "support" classes: `Char/...` (incl.
   `Char/weapon/handcannon/<weapon>/{ani/*.anim, *.skeleton}`), `SoundBanks/`, `AIHead/`,
   `UIScript/layout/*.csb`, `Prefab/Client/Ets_bin/*.etsb`, `Graph/weapon/<code>.graph`,
   `Montage/ui/gun_show/gdn_*.{monb,json}`, `Shader/`, `Video/`, `GameConfig/`. **We already have
   these names — we just don't surface them as a browsable Name/Path tree.** (The gun-show `.json`
   even lists a weapon's `skeletonResList`/`animResList` by path.)
2. **The full GUID→content inventory exists in plaintext (decoded this session).**
   `LocalData/Patch/patchlist_android64_emulator.txt` (86 MB) is **TransformString-obfuscated JSON**
   (same XOR-0x2B/0x35 scheme as the mpkinfo names). Decoded, it is
   `{"android64_common": { "<bucket>/<asset-GUID>": ["<content-md5>", <size>], … }}` — ~588k
   entries, i.e. the complete **GUID → md5 → size** table. Already on disk, no VDI, no decryption.
3. **The ~80% GUID-addressed geometry has NO plaintext logical name anywhere we can read.**
   Meshes/materials/most textures are stored by GUID/hash bucket (`xx/<guid>`). Confirmed by decoding
   a real mesh (`60/6034968f-…`): it is `.MESSIAH` v8 — vertex-format tags (`P3F_N4B_T2F`, `W4B_I4B`),
   geometry, a `None` material slot — but **no embedded name and no material/texture GUID refs.**
   That is precisely why `DrCharBind` (the UV-seam matcher) has to exist. These names live in:
   - the engine **ResourceRepository** (`Patch/Repository` + `ResourceRepository`, merged by
     `ClientResource::_OnLoadRepository_on_io`; bootstrap copy `patch.repository` in the APK
     `Resources.mpk`), and/or
   - the **encrypted gameplay/data tables** (`42 a6` Python modules — off-limits).
   The ResourceRepository is **NOT** in our extracted Patch set (only the 8 mpk pairs), and the MuMu
   `data.vdi` is **not on this machine** (searched). The APK bootstrap repo
   (`Resources.mpk`, a `marshal` dict, 1,996 entries path/bucket → (md5,size)) keys GUID assets by
   bucket too — so it does not add geometry names either.

**Verdict.** DzKit's readable geometry names come from the full install's **ResourceRepository**,
read out of the VDI. For us there are two routes, neither needing script decryption:
- **Route A (match DzKit):** obtain `Patch/Repository` + `ResourceRepository` from the full install
  — via `REA-Destiny-Rising/tools/vdiext4.py` against the user's `data.vdi`, or have the user copy
  those two files out of `…/com.netease.g108na/files/LocalData/Patch/`. Parse → logical-path↔GUID →
  (via the decoded patchlist) md5 → record. Gives readable names for everything and retires the matcher.
- **Route B (no VDI) — RULED OUT for naming geometry.** Probed every readable container for a
  name→geometry bridge and found none: the mesh record has no name/material ref (above); the weapon
  `.graph` is an AnimationCore event graph (no asset refs); `GameConfig/.../en.json` is the display-name
  table (already used); the `.etsb` is MessagePack scene data whose 22-char base64 IDs are **scene-local
  entity instance IDs — 0 of 283 resolve to any of the 576,109 patchlist GUIDs.** Nothing path-named
  references the hash-stored geometry GUIDs, so without the Repository the binding stays heuristic
  (i.e. collapses back into the UV-seam matcher). Reconstruction is not a viable substitute for the Repository.

**Bottom line: Route A (the ResourceRepository, from the full install / VDI) is required** to name the
geometry and retire the matcher — exactly DzKit's source. It is not in our Patch set and the VDI is not
on this machine, so it needs the user to supply `data.vdi` (read via `vdiext4.py`) or copy
`LocalData/Patch/Repository` + `ResourceRepository` out of the install.

**Cheap immediate win regardless of route:** surface finding #1 (the already-named 20%) as a
Name/Path/Type browse tree, and list the decoded patchlist inventory. That alone closes most of G3/G9.

## 4. Open questions for the user
- Do we want to read the **MuMu VDI** directly (DzKit's route), or keep reading `LocalData/Patch`
  and hunt for the manifest there?
- Is **relic/item** in scope, or stay weapons/characters/combatants?
- ETSB visualizer: nice-to-have, or a priority (it exposes gameplay/scene data, not just art)?
