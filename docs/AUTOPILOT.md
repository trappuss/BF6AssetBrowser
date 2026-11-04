# AssetBrowser — Autopilot progress

Handed off **2026-10-26**: take the monorepo to **1:1 parity** for every existing tool (D4, FOX, POE2, DR) with its original, and leave the **core as a ready-made template** for future tools. Owner checking in from phone periodically.

This file is the single live status. I update it at every milestone. (Mockup of the shell is a separate artifact; this is the real work.)

## How I'm working
- **Verified increments.** Build stays green; I don't move on until the relevant check passes.
- **No git** (per project note — it says confirm before `git init`). Safety net = source snapshots under `_deliveries/_snapshots/` at each milestone. *Recommendation for you: let me enable a local git repo for real history/rollback — one word and I will.*
- **Build/verify:** `VCPKG_ROOT` = VS18 vcpkg; `Build and Verify.bat --nopause`; logs in `_deliveries/verify/`. Going forward I build specific targets + run targeted `--check`/harness checks for speed, full verify at milestones.
- Standing rules respected: instrument before acting; edit originals in place keeping line endings; never git the sibling original trees; concise reports.

## Baseline (starting point)
Oct-5 build: **53 passed / 3 failed.** Core + POE2 + FOX fully green. The 3 fails are all D4, all clip/attachment in the core path:
- `d4_shot_rig` — "models: decoded clip" missing
- `d4_attach` — not posed on idle clip
- `d4_attach_anim` — devshot clip missing

**Resolved:** those were the pre-move INI paths (now corrected and present). A fresh VS18 build is **58 passed / 0 failed** — D4 clip playback and attachments work. Nothing is regressed; the gaps are *un-built* features, not broken ones.

## Status (live)
- **Baseline re-verified: 58 / 0 green** on a fresh build (VS18 toolchain confirmed working).
- **Audit done (2026-10-26).** `MODELSTAB_GAP.md` is **largely still accurate** — only load-path items got done since (e.g. D1 export-only-visible is implemented: `exportScopeParts`, `ModelsTab.cpp:962`/`:1040`). The big UI panels are genuinely still MISSING/PARTIAL. Verified worklist is in the Plan below.
- **Confirmed real D4 gaps:** (a) **Wardrobe / Stable / Catalogue tabs not hosted** — `D4Plugin::tabs()` registers only Textures/Models/Bulk; this is the "missing 2/3" you saw. (b) **D4's look not attached** — no `viewportExtension()` override in `D4Plugin` (phase 6c). The hosting mechanism already exists (FOX's `FilesTabHost`/`CustomizeTabHost : core::BrowserTab`), so restoring the tabs is tractable; the look (6c) is the hard one.
- Honest scope: the harness is green, but "1:1 in every way" (host 3 complex D4 tabs + reproduce D4's rendering + DLL add-on boundary) is a long road. I'll land it in verified increments, biggest real gap first, with a snapshot at each green step.

## Plan (ordered)

**Track A — Core Models-tab parity + template foundation** (verified MISSING/PARTIAL on 2026-10-26; helps D4+FOX+POE2+DR+template at once; mostly wiring existing `GLModelWidget` APIs). Execution order:
1. [done at baseline] D1 export-only-visible.
2. **C1 PARTS parity** — clickable Shown checkbox, numeric tri sort, "N of M shown" header. *(DONE — verified: links + self-tests PASS on POE2/FOX/D4, 2026-10-26)*
3. **Persistence** — shading + channel persist *(DONE)*; B3 overlay-toggle persistence + axis-gizmo/bone-names toggles + stats overlay still to do.
4. **B7 Camera panel** — FOV, angle presets, Frame, ortho, turntable+speed, persisted. *(DONE — verified on D4 boot: 68,890 models indexed + rendered)*  · B8 framing options (keep-orbit/auto-frame) still to do.
5. **B9/B11 Render panel** — exposure, light preset, IBL/tonemap/spec-AA, shadows, SSAO, cull, gradient backdrop (lit-features are a live §8.9 override; exposure/rig/backdrop persist). *(DONE — verified)*
6. **C9 timeline transport + C5 animation UX** — step/frame/speed/loop; clip search/multi-select/export/stop.
7. **C3 Material inspector + channel tiles.**
8. **C11 INFO rows** (bounds/used-by/variants) ; **A8/A9/A10/A11** search+funnel extras+counts ; **A7** list hover.
9. **E1/E2/E3/E4** row/part menu parity + H/Shift+H/Alt+H + wheel gestures.
10. **A3 persistence** (columns/sort/id column) ; **A12/A13** auto-load/last-model.
11. **F1–F4 crash learning** (load/render/thumb sentinels, blocklist, unrenderable) ; **F6** geo/tex cache ; **A1 Outliner** (largest; last).

**Track B — D4 phase 6b** (restores the missing tabs): clips → attachments → icons → links/flags → Wardrobe + Stable hosted → Catalogue re-pointed.

**Track C — D4 phase 6c** (the look): cloth, dye, fur, detail maps, hair/eye/skin as `ViewportExtension`; gate = pixel-identical to D4's own viewport.

**Track D — FOX / POE2 / DR parity passes.** FOX: 2d shader into extension, Customize 2e-5b/5c, numbered point families, Parts page. DR: confirm every original tab/decoder (Models/Textures/Materials/Skeletons/Animations/Bulk) reaches parity. POE2: confirm parity.

**Track E — Template finish / phase 7.** Retire old trees; the **Assets tab** (unified file explorer + previewers by type) into core; runtime DLL add-on boundary; the written add-on guide.

Order of attack: **confirm baseline → Track A correctness items (D1 first) → diagnose+fix the 3 D4 fails (6b) → work outward.** Low-risk, broad-benefit core items first so every tool and the template improve together; the heavy 6c and DLL boundary later with checkpoints.

## Decisions log
- Theme: neutral **Light/Dark/OLED** base + **one game accent** (your settled direction). Default accent gold.
- **Assets tab** promoted to a core tab (your call) — unified explorer, preview dispatched by file type.

## Open questions (won't block — answer when you can)
1. Enable a local **git** repo for history/rollback? (currently source snapshots only)
2. Default **launch tab**: Assets (catch-all) vs last-used?
3. 6c rendering follows your rule (reimplement from data where readable; replica, marked, only where not). I'll confirm per feature as I reach it.

## Blockers / needs you
- **`DRAssetBrowser.exe` (PID 49196, ~828 MB, running since 06:59) is holding the DR exe's file lock**, so the build can't relink DR. I am **not** force-killing it (it may be a session you left open, and the sandbox blocks me interfering with running processes). **Please close that DRAssetBrowser window when convenient** (or tell me it's safe to end it). Until then I build/verify POE2/FOX/D4 (same shared core, so my changes are fully exercised) and fold DR in afterward. Side note: `Build and Verify.bat`'s startup taskkill list covers POE2/FOX/D4 but **not DR** — I'll add DR to it (with your ok) so this can't recur.

## Changelog
- 2026-10-26 — Took over. Confirmed VS18 toolchain, snapshotted source, captured baseline (fresh build 58/0 green — Oct-5's 3 D4 fails were pre-move INI). Ran audit (gap doc largely accurate). **Increment 1:** C1 PARTS parity (clickable Shown, numeric Tris sort, "N of M shown") + B1/B2 shading/channel persistence in core `ModelsTab` — verified green (self-tests 12/0) on POE2/FOX/D4; DR pending its process.
- 2026-10-26 — **Increment 2:** Camera panel (FOV/presets/Frame/ortho/turntable, persisted) in core `ModelsTab`. Verified green on POE2/FOX/D4 (D4 Models-tab boot indexed 68,890 models + rendered a shot).
- 2026-10-26 — **Increment 3:** Render panel (exposure/light-preset/IBL/tonemap/spec-AA/shadows/SSAO/cull/gradient; §8.9-correct — lit features live-override, viewer prefs persist). Verified green on POE2/FOX/D4.
- 2026-10-26 — **Increment 4:** A11 filtered-count accent styling. *(DONE — verified)*
- 2026-10-26 — **Increment 5:** B8 keep-viewing-angle across model switches + Auto-frame toggle. Verified end-to-end (barF_base00 loaded 17 parts/54,621 tris/701 clips + posed render, exit 0).
- 2026-10-26 — **Increment 6:** C11 INFO bounding-box line + B3 overlay-toggle persistence (Overlays/Grid, restored through the master gate). Verified (self-tests 12/0, model-load boot clean).
- 2026-10-26 — **Git enabled** (local repo, this monorepo only). Baseline commit `39a7639` captured increments 1–6. Per-increment commits from here, authored trappuss / Co-Authored-By Claude.
- 2026-10-26 — **Increment 7:** C9 animation timeline — Loop toggle + 0.25x–2x playback speed (persisted). Verified (D4 clip decodes + plays; self-tests 12/0).
- 2026-10-26 — **Increment 8:** E2 part-menu parity — Invert selection + Hide all parts. Verified (self-tests 12/0, boot clean).
- 2026-10-26 — **Increment 9:** C3-lite — per-material facts (alpha mode · double-sided · maps present) in the INFO panel. Verified (15-material D4 model; self-tests 12/0). Committed.
- 2026-10-26 — **Increment 10:** E3 part-visibility hotkeys on the viewport — H hide selected, Shift+H isolate, Alt+H show all (eventFilter + ClickFocus). Verified (self-tests 12/0, boot clean).
- 2026-10-26 — **Increment 11:** B2/E4 wheel gestures — wheel over Channel box cycles channels (+status note); wheel over the timeline steps the playhead. Verified (self-tests 12/0, clip boot clean).
- 2026-10-26 — **Increment 12:** A7 rich hover preview on the detail-table rows (thumbnail + name/path/size), parity with the grid. Verified (self-tests 12/0, boot clean).
- 2026-10-26 — **Increment 13:** A3 sortable **Id column** in the shared list model (inserted before Collection so the hide-logic is unchanged; numeric sort). Appears in Models + Textures. Verified (D4 Models 68,890 rows, Textures 148,454 rows; self-tests 12/0).

## Where this leaves us (for your return)
**Thirteen** verified increments are in the **shared core** (`ModelsTab` + the shared list model), so D4/FOX/POE2 (and DR once relinked) + the template all gained: PARTS parity, Camera panel, Render panel, keep-angle framing, INFO bounds + per-material facts, animation Loop/speed, part-menu Invert/Hide-all, H/Shift+H/Alt+H part hotkeys, wheel gestures (channel cycle / timeline step), table hover preview, a sortable Id column, and persistence of shading/channel/overlays/count. Build is green (POE2/FOX/D4 self-tests 12/0); each step is snapshotted under `_deliveries/_snapshots/` **and committed to git** (baseline `39a7639`, then one commit per increment, authored trappuss / Co-Authored-By Claude).

**Resume pointers for the heavier items (code facts gathered so this restarts fast):**
- **C3 full material inspector** (channel tiles): `core/types/Material.h` — `Material{ name, alphaMode{Opaque,Mask,Blend,Additive}, doubleSided, metalFactor, roughFactor, baseColor/normal/orm/emissive: TextureImage{QImage image}, emissiveFactor[3], emissiveStrength }`. `m_current.materials[i]` aligns with `m.geo.materialNames[i]`. Needs a panel + per-part-selection update (hook the viewport `selectionChanged` sync at the `m_parts` sync site). QLabel rich-text does NOT reliably render data-URI images — use real QLabel pixmaps in a dedicated panel, not `<img>` in INFO.
- **C5 anim list** (multi-select export / stop-on-reclick): `m_clipList` `QListWidget`; export subset needs `ExportOptions` a clip-vector (today only `onlyClip`).
- **A1 Outliner**: `Geometry::groups` tree is parsed, undrawn; largest item, do last.
- **D4 tabs → reimplement on core** (decided): the originals `Diablo4AssetBrowser Native/src/tabs/{WardrobeTab2,StableTab2,CatalogueTab}.cpp` are `MainWindow`-coupled; rebuild on the shared viewport + `D4Store`. **6c look**: attach a D4 `ViewportExtension` (`core/gl/ViewportExtension.h`); `D4Plugin` currently sets `viewFeatures()` only.

**Decisions I need from you before the biggest items:**
1. **Git** — enable a local repo for real history/rollback? (I've been snapshotting instead.)
2. **D4's un-hosted tabs (Wardrobe/Stable/Catalogue)** — these are deeply coupled to D4's original `MainWindow`. **Host D4's own tab classes** (like FOX hosts its Files/Customize), or **reimplement on the core**? This is the "missing 2/3" and the single biggest D4 gap; the approach is your call.
3. **D4's look (6c)** — reproduce the renderer (cloth/dye/fur/detail/hair-eye-skin) as the `ViewportExtension`; largest and hardest. Confirm you want me to take it on next, or prioritize finishing the core Models-tab parity (timeline/anim UX, material inspector, outliner, robustness) first.

Remaining core worklist (tractable, benefits every tool) stays in the Plan above. My recommendation: finish the core Models-tab parity next (lower risk, broad benefit), then tackle D4 tab-hosting, then 6c.
