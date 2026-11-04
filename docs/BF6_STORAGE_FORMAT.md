# BF6 Frostbite storage format (native read recipe)

Recovered 2026-11-02 by decompiling the reshax toolkit's `toc_bf6.exe` (a .NET assembly,
internal name `toctest`, ilspycmd) and verified against the retail install at
`E:\SteamLibrary\steamapps\common\Battlefield 6\Data`. This is the spec the native `Bf6Store`
implements — **no dump required**, everything is read in place and decompressed on demand.

**Headline:** the `.toc` files are **not encrypted and need no key**. Each `.toc`/`layout.toc`
begins with a 556-byte (0x22C) header (magic `00 D1 CE 01` + high-entropy bytes) that the
reader **skips entirely** (`Seek(556)`), then reads **big-endian**. The 21-byte per-superbundle
`.digest` and the per-folder `cas.digest` are unused by the reader (integrity only).

Verified: 61 `cas_NN.cas` files totalling 52.3 GB; `layout.toc`@556 = `01 "superBundles" …
"name" "Win32/characters" …`.

## On-disk layout
```
Data/
  layout.toc                     superbundle registry (persistentIndex -> installBundle path)
  Win32/
    characters.toc / .digest     superbundle TOC (big-endian, 556-byte header skipped)
    weapons / vehicles / globals / ui .toc+.digest
    installation/<pkg>/          e.g. commonbase, grainstallpackage, san2installpackage …
      cas_01.cas … cas_NN.cas    the content store (~1 GB each), + cas.digest
      en/ cas_*.cas              localized variants
    game/{glacierflow,glaciermp}/  level data
    loc/{en,voen}.{toc,digest}   localization string tables
```

## layout.toc  (via `readbin_ix`)
Seek 556, then parse the Frostbite binary tag stream. It yields `superBundles[]` with `name`
("Win32/characters", …) and, per install bundle, `installBundle` (string path) +
`persistentIndex` (int). Build `bundles[persistentIndex] = installBundle`; that path (under
`Gamefolder`) is where `cas_NN.cas` live.

### Binary tag stream (`readbin_ix` / `readbin_output`)
Each field: 1 control byte `c`. `flag = c>>7` (0 ⇒ a null-terminated field **name** follows via
`ReadName`), `type = c & 0x7F`:

| type | meaning |
|---|---|
| 0 | end of object |
| 1 | array — `ReadVarInt` count follows |
| 2 | object — `ReadVarInt` byte-size follows |
| 6 | bool (1 byte) |
| 7 | string — `ReadVarInt` length, bytes, null-terminated |
| 8 | int32 · 9 int64 · 10 varint · 11 float · 12 double |
| 15 | 16-byte GUID · 16 20-byte hash (SHA1) |
| 19/20 | blob — `ReadVarInt` length, skipped |

`ReadVarInt` = little-endian 7-bits/byte, up to 4 bytes. `ReadName` = null-terminated ASCII.
Map-building fact: the record with `installBundle` (type 7) then `persistentIndex` (type 8, int32)
registers `bundles[persistentIndex] = installBundle`.

## Superbundle .toc  (e.g. characters.toc) — big-endian from offset 556
Header (int32s from 556): `[0] skip · [1] bundleTableOff · [2] bundleCount · [3] skip ·
[4] chunkGuidTableOff · [5] chunkCount · [6] chunkCasTableOff · [7..14] skip` (offsets are
relative to 556).

Per bundle `j` in `[0,bundleCount)`:
- Entry at `556 + bundleTableOff + j*16`: four int32; the **4th** = `blockOff` (rel 556).
- Block at `556 + blockOff`: `a=int32(==0) · b=int32(==0) · flagsOff=int32 · flagsCount=int32 ·
  metaRefOff=int32`.
- `flags[] = flagsCount` bytes at `556 + blockOff + flagsOff` — one cas-reference flag per
  (meta, then each EBX, RES, chunk), consumed in order.
- Cas-reference table at `556 + blockOff + metaRefOff` (read from the .toc via `getcasname`):
  for each flag, if `flag>0` a **new** cas ref: `int16 skip · bun=int32 · cas=int16` →
  `Gamefolder\bundles[bun]\cas_<cas:D2>.cas`; always `casOff=int32 · casSize=int32`. `flag==0`
  reuses the previously opened cas file (only casOff/casSize). Seek that cas to casOff.

### Bundle meta blob (first cas ref, `flags[0]`) — little-endian
Stored like any resource in a cas: `metaSize=int32` (asserts `metaSize+4==casSize`), then
`metaSize` bytes:
`magic=uint32 (==0x9D798ED6) · total=int32 · ebxCount=int32 · resCount=int32 · chunkCount=int32 ·
strTableBase=int32 · skip int32×2`. Then:
- `hashes[total] = uint64` each +12 bytes skipped  (EBX hashes then RES hashes; keys into the
  global EBX/RES name-hash sets)
- `(nameOff,?)[ebxCount+resCount]` = int32 pairs — `nameOff` is offset into the string table
- `resTypeHash[resCount] = uint32`  (djb2: `h=5381; h=h*33 ^ lowercase(ch)`; resolve via
  `types.txt`, else hex)
- `[resCount]` int32 +12 skip ; `[resCount]` uint64
- EBX i: name at `strTableBase + nameOff[i]` → `<name>.ebx`; next cas ref = its bytes
- RES i: name at `strTableBase + nameOff[ebxCount+i]` → `<name>.<resType>`; next cas ref
- chunk i: 16-byte GUID → `<guid>.chunk`; `rangeFlag=int32` (dump when 0); int32 skip; next cas ref

### Superbundle chunk store (tail)
`chunkCount` GUIDs (16 bytes) + `int32 (&0xFFFFFF)` index at `556+chunkGuidTableOff`; each chunk's
cas ref at `556+chunkCasTableOff + index*4` via `getcasname(1)`.

## Cas resource payload = Frostbite compressed blocks
A resource occupies `[casOff, casOff+casSize)` in a cas file as a sequence of blocks, each:
```
uint32 BE decompressedSize
uint32 BE packed            // compType = packed>>24 ; compSize = packed & 0xFFFFF  (≤1 MB)
byte[compSize] payload
```
`compType`: **0 = stored/raw** (read `compSize`), **0x0F = zstd** (`ZSTD_decompress`; a trained
dictionary path exists via `ZSTD_decompress_usingDDict` but `toctest` uses plain Unwrap),
**0x11 = Oodle** (`OodleLZ_Decompress`, `oo2core_9_win64.dll`, to `decompressedSize`). Concatenate
block outputs for the full resource. (`toctest` uses a fixed 65792-byte block buffer ⇒ blocks are
≤ 64 KiB decompressed; size dynamically in the port.)

## Notes for the native port
- Two streams in lockstep: the **.toc** holds the cas-location table (bun/cas/off/size per
  resource); the **meta blob** (in cas) holds names/types/hashes. Walk them together.
- `casOff/casSize/bun/cas` are int32/int16 in the tool — fine for indexing; the port should use
  64-bit offsets since individual cas files reach ~1 GB.
- Classify rows by **resType** (authored, from the meta blob's type hash → `types.txt`), never by
  name substring (template §3). EBX = asset definitions; RES = cooked resources (MeshSet,
  DxTexture, EAClothData, HavokClothPhysicsData, RagdollResource, StrandHairSet, …); chunks =
  raw payload (vertex/index/texture-pixel data referenced by RES).
- Decompression deps already in the monorepo: zstd (games/dr uses it) + Oodle (`oo2core_9_win64.dll`
  ships with the game and the toolkit). bcdec (BC7) is vendored for textures later.

## Verified against retail (2026-11-02)
Implemented in the Qt-free `games/bf6/Bf6Toc.{h,cpp}` and exercised by
`conformance/tools/bf6_toc_check.cpp` against `E:\...\Battlefield 6\Data`:
- `layout.toc` → 135 install bundles (incl. localized `.../fr`, `.../ko`, `commonbase/tw`).
- `characters.toc` → **146,207 resources**: 58,787 EBX, 49,675 RES, 37,745 chunks.
- Top RES types: Texture 31,375 · MeshSet 5,153 · PSDResource 3,484 · ShaderBlockDepot/
  ShaderStateAssetId · SerializedExpressionNodeGraph · StrandHair{Resource,Set,Bind} 363 each ·
  EAClothEntityData 142 / EAClothAssetData 140. A few type hashes (1FC2A95B, 72564618, 456CE4E6)
  are not in `types.txt` → shown as hex (new BF6 types; fail-closed, don't guess).
- **200/200 sampled resources decompress OK** (raw + zstd + Oodle all exercised). EBX payloads
  are `RIFF`-wrapped (modern Frostbite EBX = RIFF container — relevant when we parse EBX).
- **MP character / variation naming** (cleartext in RES names):
  `common/characters/mp/main/<charId e.g. cha0024jinx>/set/<setId e.g. set_011>/…` with
  `…_mesh` (MeshSet) and `…/textures/t_<...>_cs` (color) / `_id` (id/mask). This is the
  character→variation tree the §33 Characters showpiece is built on.

Source of truth: `scratchpad/bf6re/toc_bf6.cs` (decompiled). Re-derive with
`ilspycmd <toolkit>\toc_bf6.exe`.
