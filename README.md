# DRAssetBrowser

A desktop asset browser for **Destiny Rising** (NetEase *Messiah* / G108) — browse, preview,
and export the game's cooked assets straight from its own `MPK` archives. Built on `abcore`, the
shared **AssetBrowser** core (the same engine-neutral framework behind the sibling
`POE2AssetBrowser` / `FOXAssetBrowser` / `D4AssetBrowser` projects); this repository carries the
core plus the Destiny Rising plugin.

> **No game assets are included.** You point the browser at your own `LocalData/Patch` folder. This
> repo is source only — decoders, UI, and build files.

## What it decodes

All format work is a C++ port of verified Python reverse-engineering tools, re-checked against real
game data. The DR plugin (`games/dr/`) handles:

- **MPK container** (`DrMpk`) — the `1084`=LZ4 / `108D`=zstd wrapper with the 256-byte XOR scramble,
  and the paired `.mpkinfo` index. A conformance tool (`dr_mpk_check`) checks the codec against
  oracle vectors.
- **Meshes** (`DrMesh`) — cooked `.MESSIAH` v8: positions, UVs, unorm8 normals, and `W4B_I4B`
  skin (bone indices + weights), including the u32-index path for large (>65535-vert) meshes.
- **Textures** (`DrTexture`) — cooked `.6` / `.1` BC7, single- and multi-mip.
- **Materials** (`DrMaterial`) — `.MESSIAH` `_material`, their plain-text name + `slot=guid`
  texture references.
- **Skeletons** (`DrBxDoc` / `DrSkeleton`) — the `BXDoc` reflection document (bind-pose rig).
- **Animations** (`DrAnim` / `DrAcl`) — the `CHAR::ANIM` container with ACL-compressed tracks
  (ACL 2.x and 1.x both decode).

### Assemble character
`DrCharBind` finds a named character's **body mesh + skeleton with no decryption**: DR's materials
are named in plain text, and a texture painted for a mesh changes abruptly along that mesh's UV
chart borders — so correlating a character's material textures against every skinned mesh's UV seams
identifies the body, with a z-score that fails loud when nothing separates. The skeleton is resolved
from the character's own body-rig folder. In the tool: **Models tab → "Assemble character" panel →
type a name**. Headless: `--assemble <name> [--part …] [--pose …]`.

## Build

Windows, MSVC + Qt 6 via vcpkg (manifest `vcpkg.json`):

```
cmake --preset windows-msvc-release
cmake --build build/release --target DRAssetBrowser
```

The Qt runtime is not auto-deployed next to the exe; copy the Qt DLLs + `platforms`/`imageformats`
plugins beside `DRAssetBrowser.exe` (or use your usual deploy step).

## Layout

```
core/            abcore — the shared, engine-neutral framework (types, store, export, viewport, shell, tabs)
games/dr/        the Destiny Rising plugin (store, decoders, UI, the Assemble-character matcher)
conformance/tools/dr_mpk_check.cpp   the MPK codec conformance tool
CMakeLists.txt   builds core + whichever game folders are present
```

The build gates each game on its folder being present, so this core-plus-DR checkout configures and
builds on its own; the other AssetBrowser games live in their own repositories.
</content>
