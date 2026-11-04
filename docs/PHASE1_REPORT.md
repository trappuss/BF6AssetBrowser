# Phase 1 — the contract

**Status: complete, with one gate deliberately left open.** Everything below compiles against
Qt 6.4 and runs headless; `conformance/run_tests.sh` reproduces all of it in one command.

## What exists now

```
AssetBrowser/
  CMakeLists.txt              root; assetbrowser_configure_target() replaces the three copy-pasted flag blocks
  core/CMakeLists.txt         abcore, a static library
  core/math/RigMath.{h,cpp}   the ONE matrix convention + the frame conversions + 6 self-test cases
  core/types/Geometry.h       Vertex · Part · PartGroup · Geometry
  core/types/Skeleton.{h,cpp} Joint · Skeleton (+ the parents-first topological sort) · Hardpoint
  core/types/Clip.{h,cpp}     Track · Clip (seconds, sparse, bound by joint index)
  core/types/Material.h       Material · TextureImage
  core/types/Asset.h          AssetId · AssetRow · TagGroup
  core/types/ExportOptions.*  every decision the three exporters honour, in one value struct
  core/store/IGameStore.h     the plugin interface, with the threading contract stated
  core/store/BackgroundIndex.*the template §2 index shape, once
  core/app/AppPlugin.h        the shell hook (tabs, indexes, settings pages, help, id policy, nouns)
  games/{d4,fox,poe2}/adapt/  the three geometry adapters
  conformance/                run_tests.sh + 6 test binaries
```

## The three adapters

| | D4 | FOX | POE2 |
|---|---|---|---|
| Input | `ModelGeometry` (per-primitive buffers) | `FmdlFile` sub-structs (SoA per mesh) | `ModelGeometry` (already shared-buffer) |
| Buffers | concatenate, rebase indices | flatten SoA→AoS, widen u16→u32, rebase | copy |
| Joints | u16 already | **palette-relative → global** | u8 → u16 |
| Frame | identity for vertices; **Z-up→Y-up for rest TRS and hardpoints** | identity (already Y-up) | **Z-down→Y-up for everything** |
| Tree | — | mesh-group tree preserved, visibility inherited | — |
| Extra | cloth/chain flags, nBaseBones, dropped-part counters, model-space hardpoint rule | shipped-hidden groups, translation-only bind | clips: **frame units → seconds** |

FOX's was the large one, as the audit predicted: Fox has no neutral geometry type, so this is the
first time its parser output exists in a form anything but its own viewport can consume.

## Three real bugs the tests caught (all in code written this phase)

1. **`Geometry::applyFrame` skipped `computeBounds()` on the identity path**, so every D4 and FOX
   model would have come out with a zero bounding box — while both adapters' comments claimed the
   bounds were computed. Found by the D4 bbox assertion.
2. **The PoE2 adapter picked the wrong inverse frame.** `kZUpToYUp` and `kZDownToYUp` are each
   other's inverse, so the two constants are numerically transposes and choosing "the inverse one"
   by name compiles, runs, and puts every bind pose in the wrong place. Fixed by adding
   `core::rm::reframe(m, toCanonical)`, which derives the inverse itself; hand-writing the
   conjugation is now impossible. A self-test case covers it.
3. **Linking two games' adapters into one binary segfaults.** `d4/src/model/ModelGeometry.h` and
   `poe2/src/model/ModelGeometry.h` both declare `struct ModelGeometry`, `MeshVertex` and
   `ModelJoint` in the **global namespace** with different layouts — an ODR violation that built,
   ran most of the way, then crashed. This is the concrete case for the plan's rule that core
   headers carry a `core/` prefix and each game is its own executable; the test harness is now one
   binary per game.

## Two audit HUNCHes now proven rather than argued

- **D4's `invertRigid` is convention-agnostic.** The audit reasoned that D4's column-vector matrix
  helpers operate on the same bytes as POE2's row-vector ones. `core::rm::selfTest` case 2 checks
  `m · inv(m) == I` and `inv(m) == inverse(m)` under the row-vector convention on a real rigid
  matrix, so the lift is verified, not assumed.
- **D4's `swapTRS` equals the matrix conjugation.** The D4 adapter uses D4's own proven swap for
  Z-up→Y-up rest transforms; the adapter test independently derives the same transform as
  `S⁻¹ · L · S` and asserts the two agree to 1e-3.

## Test inventory

| Suite | Cases | Proves |
|---|---|---|
| `core_math` | 6 groups | multiply order, `invertRigid`, TRS round-trip with non-uniform scale, both frame conversions are proper rotations (det +1, so winding survives), `reframe` picks the right inverse |
| `background_index` | 16 | ready/install once, install runs on the OWNER thread, **a superseded build installs nothing**, signed cache hit skips the worker, an unchanged fingerprint is not a reset |
| `d4 adapter` | 22 | shared-buffer rebase, per-part flags, u16 joints, COLOR_0 f32→u8, dropped-part counters, parent sort with index remap, Z-up→Y-up rest TRS, hardpoint model-space rule |
| `fox adapter` | 18 | SoA→AoS, **palette→global joints (per mesh)**, u16→u32 rebase, group tree with inherited visibility, translation-only bind with no invented rotation |
| `poe2 adapter` | 16 | authored vertex-format flags, one-time frame rotation of positions and normals, u8→u16 joints, parent sort, track remap, **frame units → seconds** |

## The gate that is still OPEN

The phase-1 gate in the plan reads: *each fixture model parsed through the adapter has the same
triangle count, bbox, joint count and parent order as the original path.* Every test above builds
its input as the games' own structs — which exercises every conversion, and is why three bugs
surfaced — but **none parses a real game file**, because no game install is reachable from the
connected folders. That end-to-end check needs the phase-0 fixtures and is the first thing to run
when they exist. Nothing downstream should be treated as verified on real data until it does.

## Where it has been verified

| Environment | What ran | Result |
|---|---|---|
| Cloud container, Qt 6.4 | CMake build + `ctest`; `conformance/run_tests.sh` compiled against the real `d4/src`, `fox/src`, `poe2/src` | 73 assertions, all pass |
| The Windows box's Linux VM (`device_bash`) | `core/math` compiled with g++ and `core::rm::selfTest` run | PASS — independent of the container |
| Windows / MSVC | not yet | the first real build is phase 2's |

The device VM has g++ but no Qt6 development packages, so only the Qt-free part of core runs there.
That is enough to confirm the frame and matrix conventions are not an artefact of one toolchain.

## Notes for the next phase

- `device_bash` on the Windows box is working again (the September update had it down), so a
  fixture pull no longer needs a manual copy.
- `core::Material::engine` is an opaque pointer for the viewport extensions D4 and FOX need; phase 3
  is where it earns its place or gets removed.
- `IGameStore::read` takes a `part` string because D4 alone needs two blobs per asset (meta and
  payload). If phase 5 finds no second user, collapse it.
