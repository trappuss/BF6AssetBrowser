# Phase 2 — the export core

**Status: complete for the glTF writer and the three export helpers; two items deferred with
reasons.** The phase-2 gate that phase 1 could not close — a real importer agreeing with the file —
**is closed here**. `conformance/run_tests.sh` reproduces everything in one command: 121 assertions,
0 failures.

## What exists now

```
core/export/GlbExporter.{h,cpp}   the ONE glTF 2.0 writer, + a 14-case self-test
core/export/ExportLayout.{h,cpp}  where a multi-file export puts its files, + a self-test
core/export/NameTemplate.{h,cpp}  export file-name templates, + a self-test
core/export/GifEncoder.{h,cpp}    lifted unchanged from D4 (see below)
conformance/tools/make_skin_fixture.cpp   builds the gate fixture through the core writer
conformance/tools/glb_skincheck.py        an independent, from-the-spec skinning verifier
conformance/tools/blender_check.py        the same check through a real Blender (pip bpy)
conformance/tests/test_glb.cpp            the writer's on-disk round-trips
conformance/tests/test_export_util.cpp    layout · names · gif
```

## The writer

Ported from POE2's `GlbExporter` — the most recently corrected of the three — with D4's and FOX's
fixes grafted in. The header lists eight scars; each is a bug one of the three tools actually
shipped, and each now has a self-test case:

| # | Scar | Where it came from |
|---|---|---|
| 1 | inverseBindMatrices (and node `matrix`) stored in **row-major** element order | POE2: bind deviation 2.6 m → 0 |
| 2 | bone nodes are **always TRS**, never `matrix` | POE2: Blender 4.4 failed, 5.0 tolerated it |
| 3 | `accessors`/`bufferViews` assigned to the root **after** the animation block | POE2: samplers pointing past the array |
| 4 | one **shared time accessor** per distinct timestamp array in a clip | D4: 954 duplicate arrays per clip on a 318-bone rig |
| 5 | **constant tracks dropped** against the joint's rest transform | FOX: a 400 KB clip was 3 MB of constants |
| 6 | only materials that **visible** parts reference are written, indices remapped | POE2, fixed 2026-09-17 |
| 7 | an **out-of-range joint index has its weight zeroed**, not the index clamped | new; see below |
| 8 | a clip with no surviving channels is **not written as an empty animation** | invalid glTF |

### What the port removed

POE2's writer rotated the frame in six places — positions, normals, tangents, bind locals, inverse
binds, translation keys, rotation keys — each an opportunity to pick the wrong inverse, and one of
them was wrong when phase 1 started. **The core writer rotates nothing.** The adapters put
everything in the canonical Y-up frame at ingest, so the "swap once" rule pays off exactly here: an
`upAxis` or `yawDegrees` request becomes a **wrapper root node**, never a second pass over the data.
`unitScale` keeps both forms because the projects genuinely need both — POE2 bakes 0.005 into the
data, FOX carries it on a node — and a self-test case checks each does what it says and not the
other.

### Three deliberate defaults

- **`COLOR_0` is off by default.** glTF multiplies `COLOR_0` into base colour, but D4's vertex
  colour is *detail-map blend weights*. Writing it would silently tint every D4 export in every
  importer, and nothing would look broken enough to investigate. A plugin whose vertex colour really
  is a colour turns it on. `TEXCOORD_1` has no such trap and is written whenever the mesh has one.
- **`TANGENT` is never invented.** An importer can derive a tangent from the UVs; it cannot tell
  that the one we wrote is a placeholder.
- **Scar 7.** POE2 clamped an out-of-range joint index to 0 and kept its weight, which drags the
  vertex onto the root bone. Zeroing the weight instead leaves the vertex influenced by its valid
  bones, or unweighted at bind if it had none — and the count comes back in `glb::Result` so a
  corrupt palette is *reported* rather than silently reshaping the mesh.

## The gate, closed two independent ways

The fixture is a two-bone rig with three vertices and one rotation clip, built so the correct
skinned positions are derivable **by hand from the spec** rather than by running our own maths.
`make_skin_fixture.cpp` writes both the `.glb` and the expectations; the derivation is in its header
comment.

1. **`glb_skincheck.py`** — a verifier written in another language with its own column-major /
   column-vector matrix code, reading the spec's formula
   `p' = Σ w · (globalTransform(joint) · inverseBind · p)`. Maximum deviation **1.34e-07**.
2. **`blender_check.py`** — a real Blender **5.0.1** import through the `bpy` module. Maximum
   deviation **4.77e-07** at bind and at the animated frame.

**Both are shown failing on a deliberately corrupted file.** `glb_skincheck.py --write-transposed`
rewrites the fixture with every inverse bind transposed — precisely the POE2 bug — and the suite
fails if either verifier accepts it. A check that cannot fail is not a check; this one moves the
vertex by 1.33 units and both catch it.

Three things were found while building the gate, and all three are recorded in the tools:

- `read_factory_settings(use_empty=True)` does **not** empty the scene under the `bpy` module;
  the startup objects survived and contributed 42 phantom vertices.
- Blender's glTF importer creates a bone **custom-shape** icosphere and parks it in a collection it
  names `glTF_not_exported`. Counting it turned a 3-vertex fixture into 45 points.
- Comparing two position lists by **sorting and zipping** was wrong: Blender's positions carry ~1e-7
  of noise, so points that should tie on x sorted differently from the exact expectations and every
  pair mismatched while the *set* was correct. The comparison is now nearest-neighbour with a
  used-once rule, which is order-independent and still fails a genuinely wrong pose.

## The three helpers

**`GifEncoder`** is the strongest single case in the whole audit for a shared core: the three
copies are **byte-identical apart from one include line each** (675 lines, three md5s differing only
at line 1). It is here unchanged except for being nested in `namespace core`, so a game tree still
carrying its own copy cannot collide with it at link time during the migration.

**`NameTemplate`** — D4's and POE2's are byte-identical (md5 `8ca7ffd9…`). Two changes on the way
in: the QSettings readers are gone (core never owns a key name — and D4's `ExportLayout` had already
had to add a `tpl` parameter because the convenience overload reopened QSettings once per exported
item), and `{{SNO}}` is now an alias for the portable `{{Id}}` so templates users have already saved
keep working. The Windows reserved-device-name rule, which D4 applied to folders only, now applies
to files too — `CON.glb` was unwritable with nothing in the log to say why.

**`ExportLayout`** is the one that genuinely differed, because the *mechanism* was shared and the
*vocabulary* was not (D4 Class/Type from AppearanceMeta, FOX game/category/family from ModelTags,
POE2 Type/Folder from the path). The mechanism is core; the vocabulary is a `Spec` the plugin fills
with a `tagFor` callback. Three modes need no vocabulary and are built in: Flat, MirrorPath (POE2's
"Folder") and PerItem. Both rules that cost real debugging time are kept and tested: an **unknown
mode id reads as Flat** (never as a tag name that matches nothing), and an **unplaceable item goes
to `_misc`** (never to the empty string, which scatters it into the parent). One thing is new: when
a tag mode is asked for while its tags are still building, the fallback to Flat is now **reported**
through an out-parameter instead of happening silently.

## Deferred, with reasons

- **`ExportCapture`** (the GIF budget ladder, template §26). It is viewport code — it captures
  frames from the GL widget — so it belongs with phase 3, not ahead of it. `GifEncoder`, the part
  that is not viewport code, is here.
- **`Retarget`.** D4's and POE2's are different enough that merging them is a judgement call about
  D4's cloth-chain collapse, which is bound up with the cloth extension that phase 6 decides. Both
  files stay where they are until then.

## Still open

- **The fixture gate from phase 1 is still open** for the same reason: no game install is reachable
  from this session, so nothing here has parsed a real game file. The moment phase 0 exists, run
  every fixture through both the core writer and the original writer and explain each diff.
  The synthetic gate above proves the writer is *correct*; it does not prove it is *equivalent to
  what each tool writes today*, and only the fixtures can.
- `blender_check.py` needs `pip install bpy` (~1 GB). The suite reports that half of the gate as
  SKIPPED and OPEN when the module is absent rather than passing quietly.
