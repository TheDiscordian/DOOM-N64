# GPU Port Plan — static world mesh + cull + RSP-fed raster

> **DIRECTION (2026-06-30): pursuing Option 3 — a full-scene Z-buffered renderer.** The
> mesh floor/ceiling retry was refuted (planes cannot mimic visplane opening masks as isolated
> leaf fans); the decision is to make the shared Z-buffer the authority for ALL opaque world
> visibility and retire the CPU visibility machinery in phases. Read **§DIRECTION CHANGE
> (2026-06-30): Option 3** at the end of this file FIRST — it supersedes the Phase-3 /
> floors-as-poly-plane-drop-in framing below. Motivating evidence: `Docs/CEILING_VOID_INVESTIGATION.md`.

Goal: collapse the p95 CPU tail toward locked-60 by replacing DOOM's per-frame
CPU geometry generation (BSP-walk projection + per-column wall fill + per-frame
visplane tessellation) with a **static world-space polygon mesh** baked once at
level load, a **cull** pass that picks the visible subset, and the existing RDP
raster path. The RDP is idle (`rdpbusy ~5µs`); the bottleneck is the CPU turning
the world into screen triangles every frame (`bsp_walk` + `seg_rast` + `planes` +
`dlbuild` ≈ 7ms on dense frames). This plan moves that work off the per-frame path.

This plan was produced by a design pass (4 parallel readers) and **corrected by an
adversarial review that refuted the first synthesis' central premise** — see §Vehicle.

> **Correction (Z-buffer, post-Phase-2).** The original "no Z-image, BSP
> back-to-front painter's order suffices" premise was **refuted in practice** for
> opaque walls. We bake at **sidedef** granularity (§Architecture rationale: fewer
> quads, no T-junctions) — but that deliberately discards the BSP **seg** split, and
> the seg split is exactly what makes a back-to-front total order *valid*. Whole
> sidedef quads can **mutually overlap** in screen space (one quad's left edge in
> front of wall B, its right edge behind it) — the cyclic-overlap case painter's
> algorithm cannot resolve without splitting. Observed as "walls draw through each
> other / far walls drawn inside" across the flagged frames. **Fix: a 16-bit Z-image
> attached for the opaque mesh-wall flush only** (`TRIFMT_ZBUF_TEX`, per-corner Z from
> `invw`). This does **not** contradict the sprite/masked note in §gotchas — that
> path still needs the back-to-front **drawseg clip arrays**; the Z-image governs only
> opaque wall-vs-wall pixels. See §Z-buffer.

## Architecture

> **Historical note (2026-06-30):** this section records the original incremental mesh-port
> architecture. The current direction is Option 3 (§DIRECTION CHANGE below): a full-scene
> Z-buffered renderer. In particular, the old "BSP order / no Z-image" and "leaf floors as a
> drop-in for visplanes" assumptions are superseded; keep reading this section as background only.

1. **Bake (once, at level load).** `P_SetupLevel` (p_setup.c:651) gains a
   `P_BakeWorldMesh` (new `r_bake.c`) that converts the static lumps into a
   world-space mesh in **absolute map-unit coordinates** (DOOM map coords are
   on-disk shorts, p_setup.c:214 — they fit int16 with no scaling; validate range
   at bake):
   - **Walls** from sidedefs (NOT segs — segs are BSP-split fragments that multiply
     count + add T-junctions): up to 3 quads per sidedef — single-sided = 1 mid
     quad `[floorheight..ceilingheight]`; two-sided = a top quad
     `[back.ceil..front.ceil]` + a bottom quad `[front.floor..back.floor]`; the
     optional see-through midtexture is handled separately (masked). Texture index 0
     (`'-'` sentinel, r_data.c:704) = no-draw, so 1/2/3 quads emit per sidedef.
   - **Floors/ceilings** as one convex **leaf fan per subsector** (the DOOM-64
     model; subsector_t is a convex BSP leaf, r_defs.h:227). ⚠️ Vanilla nodes have
     **no minisegs**, so a subsector's seg loop is an *open* fan — the bake must
     **close each leaf to its convex hull** (or clip against the BSP partition
     stack) or floors gap along partition edges. This is the one genuinely hard
     bake step.
2. **Cull (per frame).** A **stripped BSP walk, cull-only**: keep `R_RenderBSPNode`
   + `R_CheckBBox` node prune + the 1-D `solidsegs` occlusion in `R_AddLine`
   (r_bsp.c), but strip the rasterization tail (`R_StoreWallRange`/`R_RenderSegLoop`).
   It outputs the **visible-subsector list in BSP back-to-front order** + the cheap
   per-seg side-effects (below). This is strictly cheaper than today and gives the
   draw order we need with **no Z-buffer**.
3. **Transform + raster (per frame).** For each visible quad/fan, a **hand-rolled
   fixed-point world→screen transform** (the 4 corners) feeds the **existing
   `rdpq_triangle(&TRIFMT_*)` path** with DOOM's existing CI4/CI8 + colormap
   combiner + TLUT. Back-to-front order gives correct opaque overlap with **no
   Z-image**.

## Vehicle — why NOT tiny3d (adversary's key finding)

The first synthesis chose tiny3d for transform, claiming `t3d_frame_start` "sets
neither combiner nor TLUT" so DOOM's state coexists. **That is false in-tree:**
`t3d_frame_start` (tiny3d/src/t3d/t3d.c:170) calls `rdpq_set_mode_standard()`
(rdpq_mode.c:99) which resets the combiner to a plain TEX0 passthrough, clears the
TLUT mode, and force-enables `zbuf`/`persp`/AA/dither/fog. t3d's triangle format
also hardwires a Z attribute (rsp_tiny3d.rspl:23) and every t3d example attaches
`display_get_zbuf()`, whereas DOOM attaches `rdpq_attach(disp, NULL)` — **no Z**
(i_video_n64.c:1241). So "transform-only t3d, DOOM owns the combiner, no Z" is not a
mode t3d supports.

**Decision:** hand-rolled VR4300 fixed-point transform → existing `rdpq_triangle`.
Same RSP triangle-**setup** ucode (the shared `rspq_triangle` the rdpq path already
uses), DOOM keeps its combiner/TLUT, no Z-image. (A project-owned t3d *fork* that
strips `set_mode_standard`/zbuf/persp + the Z attr is the alternative, but that is
real work, not "additive, zero-risk". Start hand-rolled; move the transform onto the
RSP later only if the CPU transform of 4 verts/quad ever shows up as a cost — it
won't initially, vs today's per-column projection.)

> T&L offload does **not** lower the RSP triangle-setup floor (~150–173 cyc/tri,
> shared ucode; RDP_PRIOR_ART.md:124-176). The win must come from the **cull cutting
> drawn-primitive count** + killing the per-frame CPU geometry generation — not from
> "using the RSP" per se. Measure recs/uploads/tris every phase.

## DOOM gotchas (must handle, or the game breaks)

- **Moving sectors (doors/lifts/crushers/stairs):** only `sector_t.floorheight`/
  `ceilingheight` (r_defs.h:103) move; runtime movers all route per-tic motion
  through `T_MovePlane` (p_floor.c:71). No vertex XY, topology, or texture changes.
  → bake static, mark a sector mesh-dirty in `T_MovePlane`, and once per frame
  Z-patch that sector's wall-quad top/bottom + re-fan its subsector at the new
  height. Dirty set = active thinkers only (single-digit typical; bounded by
  MAXPLATS/MAXCEILINGS + door/floor thinkers).
- **Sprites/masked ordering without the BSP:** `R_DrawSprite`/`R_DrawMasked`
  (r_things.c:916/1034) read per-drawseg `silhouette`/`sprtopclip`/`sprbottomclip`/
  `scale` in BSP back-to-front order. ⚠️ The clip **arrays** are produced by the
  per-column clip walk in `R_RenderSegLoop` (r_segs.c:436-541) — that is NOT free,
  so Phase 2 must **keep the clip walk and only remove the texel fill** (the
  seg_rast saving is smaller than a naive "it all collapses"). The cull preserves
  back-to-front order for **sprites/masked**; the opaque-wall Z-image (§Z-buffer)
  does not feed this path — the clip arrays still come from the drawseg walk.
- **Automap:** `ML_MAPPED` is set only in `R_StoreWallRange` (r_segs.c:626); the
  cull walk must set it per visible wall or walked-past walls never map.
- **Sky:** the sky flat (r_plane.c:1607) stays on the CPU column path; the bake
  skips `picnum==skyflatnum` ceilings and reproduces the r_segs sky hack.
- **Transparent midtextures:** route two-sided midtex quads through the **existing**
  masked path (`R_RenderMaskedSegRange`), not the opaque mesh (Phase 5).
- **Colormap/sector light + CI4 flash:** lightlevel → per-quad/fan SHADE through the
  existing combiner; the CI4 sub-palette pre-quant (DL_PrequantTexture) and the
  damage-flash re-tint (DL_RetintSlot) + uniform plane-flash overlay (d76a750) are
  reused unchanged (the transform doesn't own the combiner).

## Phases (each A/B-verified against the current renderer behind a flag)

- **Phase 0 — Measure & gate (no renderer change).** Run the two UNRUN probes:
  `BENCH_PVS` (r_bsp.c:535 — does REJECT cull buy >25% of the p95 tail? <10% = drop
  the extra PVS layer) and `BAKEFAN_PROBE` (r_bsp.c:570 — leaf-fan tri count vs
  trapezoid tessellation) — but **fix BAKEFAN to count the CLOSED hull**, not the
  open `numsegs-2`. Validate int16 map-coord range. Resolve "opaque tris ordered
  with no Z" on paper (answer: cull's back-to-front order via the non-Z rdpq path).
- **Phase 1 — Bake + render the start subsector's single-sided walls** ✅ DONE.
- **Phase 2 — All walls** ✅ DONE + VALIDATED (2026-06-22). Bake (`r_bake.c`
  `P_BakeWorldMesh`, 491 quads on E1M1) + per-frame transform/cull/draw
  (`DL_MeshDrawWalls`). Opaque occlusion via a **Z-buffer** (`e61c096`) — baking at
  sidedef (not seg) granularity loses the BSP split that makes painter's order valid,
  so whole quads mutually overlap → Z required (§Correction at top). SW fill suppressed
  in the seg loop (`mesh_route`, `01537be`) so the mesh actually displays (the present
  blits CI8 over the RDP — un-suppressed SW walls hid the mesh). Perspective screen-edge
  clip (`473c512`, killed the close-wall texture shear) + near-plane clip of straddlers
  (`1c73638`). **Measured: avg 16769µs / p95 28256µs = −16.6% / −30.3% vs full-RDP
  per-column; beats the planes-only ship too.** FIDELITY since CLOSED: texture
  `textureoffset`/`rowoffset`/pegging ARE threaded now (bake `bake_wall_t.textureoffset/
  rowoffset/peg_*` populated, resolved live each frame in the transform -- rdp_view.c:3500+);
  grazing-angle S-precision smear mitigated by period-bias + S-span split (`7226518`), with
  s10.5 saturation (>1024 texels/tri) the residual RDP physical limit.
- **Phase 3 — Baked leaf-fan floors/ceilings** BUILT, but SHELVED / SUPERSEDED
  (default-OFF). The convex-leaf bake (`P_BakeLeafFans`, r_bake.c -- the
  Sutherland-Hodgman partition-half-plane clip below, 237/237 E1M1 subsectors filled +
  convex, `a5ea814`) and the per-frame leaf transform + RDP emit (`DL_DrawMeshLeaves`,
  rdp_view.c, CPU + an RSP-leaf variant) were built and simple-frame validated. But it is
  gated behind `BENCH_FORCE_MESH_FLOORS` and **OFF by default** (`n64_rdp_mesh_floors=0`):
  it first MEASURED A PERF LOSS (~+5% avg / +21% p95), and the 2026-06-30 retry later
  refuted it as a drop-in visual replacement for poly planes (leaf/cell fans do not consume
  visplane opening masks; see `Docs/CEILING_VOID_INVESTIGATION.md`). So shipping
  floors/ceilings render via `DL_FlushPlanePolys` (RDP trapezoid plane polys), NOT the leaf
  mesh and NOT software spans.
  (Original design note, still accurate, kept below.) ⚠️ **THE HARD PART = closing
  each subsector leaf to its convex polygon.** Vanilla nodes have NO minisegs, so
  `segs[firstline..]` only cover the leaf's *wall* edges — the boundary that runs along
  a BSP **partition line** has no seg. A centroid/seg fan therefore GAPS along every
  partition edge (a scout suggested this — it is WRONG). Correct bake: walk the BSP
  tree from the root; at each node accumulate the partition half-plane for the
  front/back branch taken; at each `NF_SUBSECTOR` leaf, **Sutherland-Hodgman clip a
  map-bounds quad by the accumulated half-planes** to get the true convex leaf polygon
  (`node_t`/partition in r_defs.h + p_setup.c node load; R_RenderBSPNode descent in
  r_bsp.c is the traversal model). Then fan-triangulate that closed polygon. Per frame:
  mark visible subsectors during the cull walk (mirror `R_MeshMarkLine`/`bake_linevis`),
  re-read live `sector->floorheight`/`ceilingheight` (doors/lifts move every tic — do
  NOT cache the screen Y), transform corners, reuse the existing RDP plane-poly emit
  (`DL_EmitPlanePoly`/`DL_FlushPlanePolys`, gouraud per-corner light, perspective on).
  Skip `picnum==skyflatnum` leaves (sky stays on the CPU column path). Validate with a
  geometry trace + A/B, not the eye.
- **Phase 4 — Moving sectors** ✅ DONE -- but as a LIVE per-frame resolve, NOT the planned
  `T_MovePlane` dirty-mark/Z-patch. The wall transform (`DL_MeshDrawWalls`,
  rdp_view.c:3484-3491) re-reads `sectors[...].ceilingheight/floorheight` EVERY frame from the
  quad's baked sector references (`bake_wall_t.ztop_sec/zbot_sec/zbot_ceil/ztop_ceil`), so
  doors/lifts/crushers follow the geometry with no ghost (a step whose top drops to/below its
  bottom is skipped). Same live resolve for pegging (`peg_sec`) and lighting (`lightsec`). No
  dirty-mark needed -- it never caches a Z. (The shelved leaf-floor path, Phase 3, resolves its
  sector heights live the same way.)
- **Phase 5 — Transparent midtex + flash/colormap + end-state sweep:** PARTIAL.
  Flash + colormap CONFIRMED done on the mesh path: CI4 damage-flash re-tint (`DL_RetintSlot`)
  and fixedcolormap (invuln/light-amp visor) for walls AND RDP planes (`8c2d5e7` / `5ec60bb`).
  STILL OPEN: transparent/masked MIDTEX is on the software path (`R_RenderMaskedSegRange`), not
  yet meshed; and the full RDP-vs-software avg+p95 end-state sweep.

## Open go/no-go numbers (Phase 0 settles these)

- `BENCH_PVS cull_pct_p95tail` — is an extra REJECT/PVS cull layer worth building?
- `BAKEFAN` closed-hull tri count vs the current trapezoid tessellation.
- int16 posA range across the map (start room is safe; map-wide overflow possible).
- The realized per-quad transform cost vs today's per-column projection.

## Mesh-build perf lever map (verified 2026-06-23, BENCH_FORCE_MESH E1M1)
Default mesh build baseline avg ~17.9k / p95 ~30.8k us. Per-phase tail (p95): `dlbuild`
~9.8k (25%) > `bsp_walk` ~7.3k > `seg_rast` ~4.6k > `audio`/`planes`/`hud` ~4k each >
`present`. What's actually movable, checked against the code (not guessed):
- **`dlbuild` (the dominant lever) = ~95% EMIT, ~3-5% wall transform.** The emit's
  LOAD_TILE/autosync thrash is **already fixed** — the Stage-3 per-texture dedup collapsed
  it from ~150 band uploads/frame to ~30 (`mean_uploads=30`; the "~7.5ms" comment at
  rdp_view.c:3555 is the historical PRE-dedup number). No cheap emit win remains. The real
  `dlbuild` reduction is the **RSP transform port** (move projection to the idle RSP), which
  now WINS once the dispatch processes only the ~30 visible walls (compaction, `1d4fef1`):
  RSP-mesh-walls beat pure-CPU-mesh-walls avg −6.0% / p95 −6.2%. Still gated behind
  `BENCH_FORCE_MESH_RSP`; banking it in the default build is the default-on call (the
  fixed-point transform differs ~1px sub-pixel from CPU float, emit decisions agree). See
  Docs/RSP_PORT_PLAN.md §7.
- **Floor leaves: the transform IS the lever (unlike walls).** Leaves have many verts so
  the `65536/depth` divide dominates. Sharing the floor+ceiling projection (one transform,
  not two — `df533b8`) cut floor-mesh `dlbuild` -25% / p95 -28%, moving floor-as-mesh from a
  clear loss to ~parity. `BENCH_FORCE_MESH_FLOORS` still off by default pending the RSP win.
- **Wall occlusion: the Z-buffer is the fix, and it's nearly free** — the z-image was already
  allocated + cleared every mesh frame; `dl_wall_z` just needed decoupling from the floors
  flag (`4c6424d`). Painter's nearest-corner sort can't be correct on overlapping-depth walls.
- **`present` (~3.5ms) is irreducible** — the keyed CI8 blit + `display_get` vsync wait; not
  a bug. **`bsp_walk`**: a precomputed-facing backface-skip before `R_AddLine` is the only
  cheap candidate, but uncertain (DOOM already span-culls; risks dropping visible walls).
- **`seg_rast` + DOOR MOTION (`3f19228`):** door/mover walls were excluded from the bake
  (no-Z ghost era) and rendered software, so door MOTION spiked seg_rast to 12-17k us on the
  worst frames. Now that wall-Z is on by default, re-including them in the Z-mesh (gate the
  exclusion on `n64_rdp_mesh`, not the floors flag) killed those spikes (16867->6945,
  12859->2927) and dropped seg_rast mean 2645->1587us; total -3.2% avg. The residual seg_rast
  is the BSP-occlusion setup for visible segs (needed for the mesh's vis gate) -- only the big
  BSP-walk-as-visibility replacement reduces that further.
- **Method note:** measuring one phase in isolation hides cross-phase cost (the RSP offload
  dropped `dlbuild` but added more wait; the Z-buffer "loss" was the same trap). Always check
  TOTAL frame time, not the single phase you touched.

## BSP-walk-replacement roadmap (2026-06-24) — where the GPU port stands

> **Superseded by Option 3 (2026-06-30):** the old sequence "floors on mesh, then sprites,
> then strip R_AddLine" assumed mesh floors could replace poly planes independently. The live
> roadmap is now Phase A/B/C/D in §DIRECTION CHANGE below. This section remains as historical
> performance context only.

The RSP wall-transform offload is DEFAULT-ON and WON (walls off the CPU). That made `bsp_walk`
(~23% tail) the new #1 cost: the per-seg BSP occlusion walk (R_AddLine: 2× R_PointToAngle +
solidsegs + R_StoreWallRange). The plan is to delete it and let the mesh do its OWN cull
(frustum + the wall Z-buffer). Sequencing, with what's done:
1. **DONE — mesh frustum wall vis (`73acd81`, BENCH_FORCE_MESH_CULL, gated).** R_Subsector marks
   ALL walls of a frustum-visible subsector; Z discards the overdraw. Render-verified equivalent.
   Perf LOSS today (adds overdraw walls AND R_AddLine still runs) — it's the foundation.
2. **NEXT — floors on the mesh, leaf transform on the RSP (Phase 4).** Floors-on already
   suppresses the software visplanes (`planes` 1544→60us) and is an AVG win (16633 / 60fps) but a
   p95 LOSS (30176) because the floor-leaf transform is still CPU and inflates dlbuild p95.
   Putting the leaf transform on the RSP (extend the just-landed wall compaction: dense vis-list
   pack + dispatch, but leaves are N-vertex not 4-corner, so the rsp_dlwall ucode needs a leaf
   path) should drop leaf dlbuild like it did for walls → floors-on becomes a clear win.
3. **THEN — sprites on the mesh** (remove the drawsegs dependency), then **strip R_AddLine**:
   with planes + sprites off the BSP and walls on frustum+Z, the walk becomes a pure frustum
   traversal (no per-seg angle math, no solidsegs) → `bsp_walk` collapses. That is the BSP-walk
   replacement the whole port is for. NOTE: even the frustum cull currently leans on solidsegs
   (R_CheckBBox uses them); the pure-frustum walk drops solidsegs entirely and lets Z do ALL
   occlusion — visits/draws more, but kills the per-seg cost. Measure the balance when it lands.

## HISTORICAL PROFILE + corrected next levers (2026-06-24, then-default experiment = mesh-floors)

> **Not the current shipping baseline.** Current known-good/default mesh work is mesh walls +
> poly planes (`BENCH_FORCE_MESH=1`, no `BENCH_FORCE_MESH_FLOORS`). These numbers are retained
> to explain why the old mesh-floor route was attractive, then refuted.

`bench/bench.sh mesh-floors`, E1M1 4117 frames, avg 16633 / p95 30112 (60.1 / 33.2 fps).
Per-phase MEAN (pct of mean_total 19522us):

| phase | mean us | pct | p95 us | tail (worst-5%) us |
|---|---|---|---|---|
| present  | 4654 | 23.8 | 11360 | 843 |
| dlbuild  | 3455 | 17.7 | 11424 | 13084 |
| bsp_walk | 2925 | 14.9 | 9120  | 9294 |
| hud      | 2112 | 10.8 | 4192  | 3401 |
| audio    | 2111 | 10.8 | 6176  | 4863 |
| seg_rast | 1590 | 8.1  | 3360  | 3150 |
| masked   | 1457 | 7.4  | 3360  | 2216 |
| planes   | 60   | 0.3  | 288   | 254  |
| rdpbusy  | 3    | 0.0  | -     | 3    |

**Two distinct regimes — pick the lever by which you're cutting:**
- **MEAN is `present`-dominated (23.8%).** NEW finding (prior notes were tail-focused).
  `present` = the CI8-band `rdpq_tex_blit` emits (the still-software HUD / sprites / status
  bar blitted around the RDP view box) + buffer flip. The RDP is idle (rdpbusy ~3us), so
  this is CPU command-emit, not raster. copy-forward memcpy is wipe-only (not the cost).
  Cutting it means moving the 2D overlay off the per-frame CI8 blit — DELICATE path (ghost
  / TLUT / keyed-box correctness history); do not optimise speculatively.
- **TAIL is `dlbuild` + `bsp_walk` (34% + 24%).** The worst frame (412) is a **154 ms**
  dlbuild spike (= the max_us=172114 outlier) — a first-touch texture event the
  R_PrecacheLevel prequant missed. Killing that one spike alone collapses max_us. bsp_walk
  tail (9294) is the BSP-walk-as-visibility cost the port's endgame (strip R_AddLine) targets.

**CORRECTION to "NEXT — leaf transform on the RSP" above: DONE and REFUTED.** Putting the
floor-leaf transform on the RSP is a STRUCTURAL LOSS (+3.6% avg / +10.4% p95), not a win —
the readback round-trip (overlay-reload DMA + buffer DMA + sync) costs more than the ~85
cheap CPU divides it offloads, and no barrier placement fixes it (three builds tried; see
RSP_PORT_PLAN.md §8.2/§8.3). Floors stay on the CPU-leaf path. The only floor win is RSP
T&L that EMITS the rdpq triangles directly (no CPU readback) — a much larger ucode effort.

**Recommended order now:** (a) the frame-412 dlbuild spike (isolated, biggest max-frame
win, likely a missing precache); (b) sprites→mesh then strip R_AddLine (the bsp_walk
collapse — the port's whole point); (c) present/2D-overlay offload (biggest MEAN win, but
highest risk). RSP-emits-triangles underlies both (a-floors) and a long-term present cut.

### UPDATE (2026-06-24): the frame-412 dlbuild spike is FIXED (prequant was off)
Lever (a) above landed. Root cause was NOT a missed precache SET — it was that
`DL_PrequantTexture` guarded on `n64_rdp_wall_ab`, which the mesh build forces to 0
(mesh replaces the RDP wall route, d_main.c:2219). So the level-load prequant was a
complete no-op for the ENTIRE mesh effort; every wall texture quantised lazily on first
sight. Fix (commit f1cffea): prequant when `wall_ab || n64_rdp_mesh`. Clean-build A/B
(mesh-floors, 4117 frames): **max_us 175159 -> 35991 (-79%), min_fps 5.8 -> 27.7**; avg
16633->16592 / p95 30112->30240 (flat -- the spikes were too rare to move the aggregates,
but they wrecked the worst case + felt smoothness on every room entry). Render-time
texture builds 19 -> 0 (all 34 now prequant'd at load), confirmed via the new
`DLBUILD_TRACE=1` probe. The MEAN levers (present 23.8%, bsp_walk, dlbuild steady) are
unchanged and remain next.

### CORRECTION (2026-06-24): `present` is mostly VSYNC IDLE, not a compute lever
The "present = 23.8% mean = biggest MEAN win" framing above is WRONG. Evidence: present
mean 4654us but present TAIL (worst-5% frames) 843us -- it DROPS 5x on slow frames. A fixed
CPU-blit cost can't do that; a vsync-coupled wait does (a slow frame eats the display-buffer
slack, so the acquire wait shrinks to ~0). By default the `display_get()` free-framebuffer
acquire is INSIDE the PRESENT bracket (it's only split out under the -- currently
mesh-build-broken -- RDPWAIT_PROBE). So:
  present CPU cost  ~= 843us   (the tail floor = the CI8 HUD/sprite/status blit emit)
  present vsync idle ~= 3811us  (mean 4654 - 843; the CPU finished the frame early)

So the MEAN frame is VSYNC-BOUND: avg 16633us = 60.1fps = the 60Hz cap, with ~3.8ms/frame
spent idling in display_get. Cutting mean compute below vsync does NOTHING for avg fps. The
ONLY lever that improves the experience is the TAIL -- frames whose compute exceeds 16.67ms
drop below 60fps and are felt. That's why the prequant fix (max 175ms->36ms) mattered and
yet barely moved avg/p95: avg was already vsync-capped; the spike was a pure tail/max defect.

REVISED lever priority (all TAIL, since mean is vsync-capped):
  1. (DONE) prequant area-transition spike -- the 154ms outlier, fixed.
  2. bsp_walk tail (9294us p95) -- the BSP-walk-as-visibility; the port endgame (strip
     R_AddLine once planes+sprites are mesh-driven).
  3. dlbuild steady/tail + audio tail (4863us) -- audio is a known hard floor.
The present CPU blit (~843us) is small and the path is delicate -- NOT worth touching.
(Side note: RDPWAIT_PROBE=1 currently OOM-asserts at I_InitGraphics in the mesh build --
a pre-existing diagnostic-only bit-rot, off by default; fix if that probe is needed.)

### MEASURED bsp_walk breakdown (2026-06-24, BSPWALK_PROBE) -- mesh emit is #1
Sub-bracketed the bsp_walk phase (BSPWALK_PROBE=1, call-site CP0 brackets, geometry
fingerprint verified identical). The design-workflow GUESSED R_AddLine 35-50% dominant +
DL_MeshDrawWalls a 5-15% residual. MEASURED (mesh-floors, E1M1; mean bsp_walk 3132us /
tail 10311us), it's the opposite ranking:

| bsp_walk sub-part | mean us (share) | tail us (share) | tail/mean |
|---|---|---|---|
| mesh (DL_MeshDrawWalls) | 1126 (36%) | 3728 (36%) | 3.3x |
| addline_net (R_AddLine vis+setup, MINUS raster) | 874 (28%) | 3147 (31%) | 3.6x |
| leftover (R_FindPlane + recursion glue) | ~745 (24%) | ~1417 (14%) | |
| checkbbox (node cull) | 204 (6%) | 583 (6%) | |
| sprite (R_AddSprites collect) | 187 (6%) | 436 (4%) | |
| (segloop = 1895/seg_rast, EXCLUDED from bsp_walk) | | | |

**The GPU wall emit (DL_MeshDrawWalls) is the #1 bsp_walk cost in BOTH mean and tail (~36%)**
-- it is the per-wall emit/depth-sort + the wall RSP-transform `rspq_wait` (DL_RSPBatchProbe
runs inside it). R_AddLine-as-visibility (addline_net) is a close #2 (~31% tail). Both scale
~3.3-3.6x into the tail; checkbbox/sprite are minor. R_AddLine looked huge only before
subtracting the SEG_RASTER column loop nested inside it (segloop 1895us, correctly in
seg_rast not bsp_walk).

REVISED tail levers (bsp_walk, the biggest tail phase):
  1. mesh emit (~3.7ms tail) -- cut DL_MeshDrawWalls: the RSP-transform wait + per-wall
     emit/sort. The end state is RSP-emits-triangles (no CPU emit/readback), the same path
     the floor leaves need. BIGGEST single bsp_walk lever.
  2. addline_net (~3.1ms tail) -- the R_AddLine-strip endgame (frustum+Z replacing the
     per-seg solidsegs occlusion). Comparable to mesh, not the dominant cost the plan assumed.
  3. checkbbox/sprite -- minor; not worth attacking alone.

### Mesh #1-lever decomposed: it's the RSP readback STALL (2026-06-24, BSPWALK_PROBE)
mesh_rspwait sub-bracket (DL_RSPBatchProbe = wall pack+dispatch+rspq_wait): mesh_mean 1128us,
of which **mesh_rspwait 776us (69%)** -- the CPU STALLING on the wall RSP transform. CPU
emit/sort is only ~352us. mesh-cpu A/B (BENCH_FORCE_MESH_RSP=0, CPU transform) = +2.4% avg /
+4.4% p95 WORSE, so the RSP transform is NET-POSITIVE (CPU transform ~1183us > RSP round-trip
776us); reverting LOSES. The 776us is the cost of reading transformed verts back to the CPU.

Two ways to attack it:
  A. RSP-EMITS-TRIANGLES (keystone): the RSP transforms AND writes the rdpq_triangle commands
     (edge coeffs + persp-tex + Z) -> CPU never reads batch_out, never waits. Removes BOTH the
     776us stall AND the 352us emit. Feasible (tiny3d does it) but a multi-session ucode rewrite
     (and tiny3d itself is NO-GO: it resets combiner/TLUT, stomping DOOM CI4 -- must hand-roll).
  B. OVERLAP THE TRANSFORM (bounded): rspq_wait is a full-queue drain so it can't overlap LATER
     RDP work -- BUT the BSP walk is ~3ms of PURE CPU with zero RSP/RDP activity. Dispatch the
     wall transform BEFORE the BSP walk, consume at DL_MeshDrawWalls -> the RSP transforms during
     that window, wait ~0. CATCH: early dispatch can't use the BSP-vis compaction (vis unknown
     until the walk ends), so it transforms ALL ~475 walls not the ~30 visible -- and transform-
     all was a -6% LOSS without overlap (the compaction commit). So B trades compaction for
     overlap; net is unknown -> MEASURE behind a flag, don't assume. If the 475-wall transform
     fits in the 3ms BSP window, B hides most of the 776us for a fraction of A's effort.

### Option B (overlap-via-transform-all) = MEASURED LOSS, dead (2026-06-24)
Implemented + benched (BENCH_FORCE_MESH_RSP_EARLY, commit). plain mesh 16774/28896 vs
early-overlap 20588/36064 = **+22.7% / +24.8% WORSE**. Transforming all ~475 walls (to
dispatch before the BSP walk, since vis isn't known yet) is ~16x the RSP work + the full-bake
CPU pack + ~44KB/frame DMA coherency -- that swamps the overlap (the 475-wall transform
exceeds the 3ms walk, so the consume wait still stalls for the remainder). The compaction
(~30 walls) is too valuable to trade for the overlap. **B is dead. The ONLY way to remove the
776us readback stall is A: RSP-emits-triangles** (the RSP transforms AND writes the
rdpq_triangle commands -> CPU never reads batch_out back, keeps compaction, drops both the
776us stall and the 352us emit). That's the keystone; it's a multi-session hand-rolled ucode
effort (tiny3d is NO-GO: resets combiner/TLUT, stomps DOOM CI4).

## DIRECTION CHANGE (2026-06-30): Option 3 — full-scene Z-buffered renderer

Ryan's call after the mesh-plane retry (below): stop trying to make **mesh floors/ceilings
mimic visplanes**, and instead make the shared **Z-buffer the authority for ALL opaque world
visibility**. This is the "whole level as a mesh" architecture. It supersedes the
"floors-on-the-mesh as a drop-in for poly planes" framing that Phase 3 / RSP_PORT_PLAN §8 chased.

### Why the mesh-plane retry did not ship (the finding that forced the pivot)
See `Docs/CEILING_VOID_INVESTIGATION.md` for the full log. Short version, all confirmed on the
retry branch `perf/rdp-mesh-planes-retry` with same-geometry captures at E1M1 frame 3712/3200:
- The no-readback mesh leaf/cell path **emits the descriptors** (EMITDIAG: `ceil vis≈desc`,
  `dcap_break=0`), the cells are on-screen and non-degenerate (LEAFRB), and it is **not** a
  simple z-test/cap/cull drop.
- Cells vs whole-leaf bands: **same class of mismatch**. Not the tessellation.
- Emitting the SAME staged cells through libdragon `rdpq_triangle()` on the CPU (and even
  recomputing the projection on the CPU) **still** diverges from the working poly-plane baseline.
  So it is **not** overlay-B / the RSP emitter / the RSP transform in isolation.
- A same-geometry diff vs the working **mesh-walls + poly-planes** build shows **broad darker /
  underdrawn plane regions**, not one missing grey sliver.
- ROOT CAUSE (design, not a bug): the poly-plane path draws visplanes **clipped to DOOM's real
  per-column openings** (`visplane->top[]/bottom[]`, produced by `R_RenderSegLoop` in the wall
  pass). The mesh-leaf path only marks visible subsectors/cells and draws whole convex leaf
  polygons — it never consumes those opening masks — so in busy stepped-ceiling scenes its
  coverage/lighting/order cannot match. Even in the mesh build, `R_RenderSegLoop` still fills
  `ceilingplane/floorplane->top[]/bottom[]`; the poly path is a GPU plane path that is *already*
  clipped to visibility, while mesh leaves are not.

**Conclusion:** meshing planes in isolation is the wrong shape for THIS renderer. The renderer is
still a hybrid — mesh walls on Z, poly planes on visplane masks, sprites+masked on the drawseg
clip arrays, 2D overlay on the CI8 software buffer. Floors can only become plain mesh geometry
once the whole opaque world is depth-tested and no class still depends on the CPU visibility
products (visplane opening masks + drawseg sprite-clip arrays).

### The end-state architecture
```
opaque world (walls + floors + ceilings [+ masked-opaque]) --> ONE shared 16-bit Z-image
    visibility = per-pixel Z, NOT visplane top[]/bottom[], NOT solidsegs, NOT drawsegs
sprites / transparent midtex --> textured billboards / quads, Z-TESTED against that world,
    alpha/keyed transparency, sorted only where blending order matters
sky --> background pass, no Z write
2D overlay (HUD / status bar / menu / wipe) --> its own pass (unchanged for now)
BSP walk --> demoted to a COARSE culler (frustum + sector/leaf PVS), or dropped for a
    frustum/portal traversal; it no longer produces per-column visibility
```
The win is deleting the CPU visibility machinery: `R_AddLine` occlusion, `R_StoreWallRange`,
`R_RenderSegLoop` column fill, visplane `top[]/bottom[]` construction, and the drawseg sprite
clip arrays. The cost is more RDP fill/overdraw + more RSP triangle setup + command bandwidth —
acceptable ONLY with coarse culling (the RDP is idle today, `rdpbusy` ~3–5µs, but N64 triangle
setup + command volume are real limits; measure every phase).

### Phased roadmap (each phase behind a flag, A/B vs the shipping mesh-walls+poly-planes build)
- **Phase A — Opaque world Z renderer (walls + floors + ceilings).** Draw baked floor/ceiling
  geometry into the SAME Z-image as the mesh walls, with Z as the only occlusion authority — NO
  dependence on `visplane->top[]/bottom[]`. This is the real replacement for poly planes.
  The geometry source MUST be the WELDED bake (shared vertices, zero T-junctions, fixed-grid
  cut at level load — see THE GOAL at the end of this file). The 2026-06-30 slice below used
  the unwelded Phase-3 `P_BakeLeafFans` polygons plus per-frame band cutting instead, and the
  user's visual review failed it for exactly that (jagged edges, 2026-07-02). Needs a
  conservative floor/ceiling cull (frustum + which sectors are potentially visible) so we are not
  drawing the whole map. GATE: new build flag; keep poly planes as the shipping default until A
  passes the user's eye AND is not a perf loss.
  - Acceptance: the USER'S EYE at the edges (seams, junctions, stability in motion) vs the
    software/poly image — averaged metrics and `BENCH_VOID_SCAN` are supporting evidence only,
    never sufficient; plus avg/p95 ≤ shipping build.
  - **First slice landed (`BENCH_FORCE_MESH_WORLDZ`, 2026-06-30, commit `1e8b9cc`):** CPU-emitted
    baked leaf planes draw as opaque Z-tested world geometry and suppress non-sky poly planes.
    Full E1M1 bench completes: shipping `mesh` = `17667/32416`; first `mesh-worldz` =
    `21222/46112` (`+20.1% avg / +42.3% p95`). A cheap per-leaf depth-range bound on band
    clipping improved it to `20784/44128`; raising the world-Z band ratio to 8 improved again
    to `20361/43232` (`dlbuild mean/p95 8526/24160`, still a clear loss).
    `BENCH_VOID_SCAN` reports only known startup frame 0 + death/respawn wipe frame 3213 — no
    3200/3712 black void. Phase table confirms the trade: `planes` CPU work collapses
    `1744->59us`, but CPU `rdpq_triangle` world-Z emit moves cost into `dlbuild`.
  - **Distance lighting fixed; luminance means matched — NOT correctness (2026-06-30, commits
    `d7395ff`, `ecd5ba5`; verdict corrected 2026-07-02).** What was actually established:
    `BENCH_VOID_SCAN` clean, and a same-geometry region-luminance A/B (world-Z vs the shipping
    poly baseline vs the frozen software ref, active-rect normalised to 320x240) within a few
    counts. The lighting work was real: the first slice shaded each leaf with ONE flat
    sector-light PRIM while the poly path shades by planar distance
    (`planeheight*yslope[row] >> LIGHTZSHIFT -> planezlight`), reading as a broad `-30..-38`
    underdraw band at frame 3200; per-vertex colormap levels via `yslope`+`zlight` as gouraud
    SHADE (`TRIFMT_ZBUF_SHADE_TEX`, `fixedcolormap` forcing the worn level flat) brought that to
    `-4.1` and frame 3712 within `+-3`. Cost after lighting: `mesh-worldz` `21540/46304`.
    **But luminance means measure area brightness, not edges — the slice FAILED the user's
    visual review (2026-07-02): jagged edges from the unwelded bake's T-junctions and the
    per-frame band cutting. Region-average metrics must never again be reported as
    "correctness verified".**
  - **CPU waste cut (`5bcd625`).** Side-plane clipping is height-independent but the first slice
    recomputed it for floor and ceiling / every flat bucket. Caching each visible leaf's side-
    clipped polygon + depth range once per frame drops `mesh-worldz` to `19141/38368`; void-scan
    build `19477/39008`. Void-scan remains clean (only known frame 0 + wipe 3213).
  - **Slice verdict (2026-07-02): neither correct nor fast — do not iterate on it.** It failed
    the user's eye (jagged edges) and costs `+8% avg / +18% p95` vs shipping. The "real perf
    lever is RSP/no-readback emit" claim that used to close this section was tried the same
    day — whole-leaf bands, then the retry cells — and produced planes that flicker in/out;
    both attempts REVERTED (`031ad31`). The path forward is the welded static bake (THE GOAL,
    end of this file); this slice's per-frame clipping/banding machinery is superseded, not
    tunable.
- **Phase B — GPU masked/transparent midtextures.** Move two-sided midtex quads
  (`R_RenderMaskedSegRange`) to RDP geometry with alpha-compare / keyed transparency, Z-tested
  against the opaque world, Z-write on opaque texels. Removes one of the two remaining
  drawseg dependencies. GATE: flag; verify grates/bars/windows (and their sorting) vs software.
- **Phase C — GPU sprites.** Billboard sprites on the RDP, Z-tested against the world Z-image,
  alpha/keyed transparency, an explicit z-write + sort policy for sprite-vs-sprite and
  sprite-vs-transparent. Removes the LAST drawseg dependency (`sprtopclip`/`sprbottomclip`/
  `silhouette`). Weapon/HUD sprites stay in the 2D overlay pass. GATE: flag; verify partial
  occlusion (monster behind a step/rail), thing sort, and translucency.
- **Phase D — Strip the BSP visibility walk.** ONLY after A+B+C no longer consume any CPU
  visibility product: delete/collapse `R_AddLine` occlusion + `R_StoreWallRange` +
  `R_RenderSegLoop` fill + visplane mask construction + drawseg clip arrays. Replace the walk
  with a frustum + sector/leaf PVS (or portal) traversal that only picks the drawn set. Measure
  the `bsp_walk` collapse against the added RDP/RSP cost — this is the phase the whole port is
  for, and the one that can regress perf if culling is too loose.

### Gotchas Option 3 MUST still honour (carried from §DOOM gotchas, re-scoped)
- **Automap `ML_MAPPED`** is set in `R_StoreWallRange` today. If that dies in Phase D, the cull/
  traversal must set `ML_MAPPED` per visited wall or walked-past walls never map.
- **Moving sectors** stay a LIVE per-frame height resolve (Phase 4 pattern), never a cached Z.
- **Sky** stays its own pass; opaque world geometry skips `picnum==skyflatnum`.
- **Colormap / sector light / CI4 damage-flash / fixedcolormap** must keep working per surface on
  every newly-meshed class (walls + planes already do; masked + sprites must match when moved).
- **Ordering where blending matters** (translucent midtex, sprite translucency) is NOT solved by
  the opaque Z-image alone — Phases B/C still need a back-to-front sort for the blended fraction.

### Open risks / uncertainties (flagged, not yet measured)
- **Overdraw budget.** Drawing conservatively-culled floors/ceilings + walls every frame into Z
  may raise RDP fill beyond the idle headroom on dense frames. Unknown until Phase A benches;
  the coarse cull (frustum + potentially-visible sectors) is the lever.
- **Sprite occlusion fidelity.** Z-testing billboards against the world replaces DOOM's exact
  column clip; thin gaps / grazing steps may differ by a pixel. Needs a live-motion grab, not
  just frozen marks.
- **Command/triangle-setup volume.** More geometry = more `rspq` commands + RSP triangle setup
  (~150–173 cyc/tri floor, shared ucode). The RSP-emits-triangles keystone (RSP_PORT_PLAN §9) is
  the lever that keeps WALL volume affordable. PLANES must NOT ride it (corrected
  2026-07-02): both plane-emit attempts flickered and were reverted (`031ad31`); the
  overlay-B emitter has never produced a user-accepted plane image. Planes are CPU-emitted
  from the welded bake until the user's eye passes, and any future RSP plane emit needs a
  differential root-cause (same staging, CPU vs RSP final emit) first.
- **2D overlay stays software for now.** The CI8 HUD/status/wipe blit (`present`) is explicitly
  out of Option 3's opaque-world scope (delicate ghost/TLUT/keyed-box history); a later, separate
  effort.

### Status / branch hygiene (2026-06-30)
- **Shipping / known-good:** `perf/rdp-renderer` @ `383cd05` = mesh walls + poly planes. UNCHANGED.
  This stays the default until Phase A is proven hole-free and not a perf loss.
- **Mesh-plane retry (parked):** `perf/rdp-mesh-planes-retry` (from the parked
  `perf/rdp-mesh-planes-wip`). Holds the no-readback leaf/cell emit + the diagnostics that
  produced the finding above. Kept for reference; NOT the Option 3 path.
- **Next:** start the Option 3 work (Phase A) on a fresh branch off the known-good checkpoint;
  keep every phase behind a build flag and A/B against the shipping build.

## THE GOAL, STATED BY THE USER (2026-07-02) — READ THIS BEFORE TOUCHING PLANES

**A full bake, done properly, like a real 3D game: the geometry is FINISHED at level
load.** Floor and ceiling meshes with **shared, welded vertices** — no T-junctions
anywhere — cut once on a fixed world grid. The runtime only **culls, transforms, and
draws**. NO per-frame polygon surgery of any kind: no per-frame side clipping, no
per-frame near folding, no per-frame depth-band cutting, no per-frame texture
rebiasing. Per-frame geometry cutting IS the defect: it produces edges that differ
between neighbours and between frames, which the user sees as jagged seams and
flicker. If a design requires cutting polygons at render time, it is the wrong design.

### Corrections applied to this file (2026-07-02)
The claims in the body above have been edited in place to match these findings; this
section records what changed and why, so the history is auditable:
- **The Phase A world-Z CPU slice FAILED user visual review**: jagged edges across
  planes (T-junction cracks from the unwelded `P_BakeLeafFans` polygons + seams from
  the runtime depth-band cutting). Its former "correctness verified" entries meant
  ONLY "region-average luminance within a few counts + no >=40%-black frames".
  Region averages are structurally blind to edge defects — the repo's own debugging
  guidance (CLAUDE.md: regional means hide localized structural defects) applies to
  the acceptance test itself. **Acceptance for planes is the user's eye at the edges
  vs software/poly, full stop.**
- **The RSP no-readback plane emit (2026-07-02, whole-leaf bands, then retry cells)
  produced planes that flicker in/out — both attempts REVERTED (`031ad31`).** The
  overlay-B plane emitter has never produced a user-accepted plane image on any
  branch. Do not point it at planes again without a differential root-cause (same
  staging, CPU vs RSP final emit) — and not before the welded bake exists.
- **History note (user-corrected):** the visplane-mimicry direction that consumed the
  pre-Option-3 sessions was the agent's, not the user's. The user's goal was the
  proper bake throughout.

### The plan from here (welded bake first, everything else after)
1. **Bake (level load):** weld the plane geometry — every vertex that lies on a
   neighbouring polygon's edge is inserted into that edge (exact shared coordinates),
   so all adjacent pieces share edge endpoints bit-for-bit. Then cut once on the fixed
   512-unit world grid with direction-canonicalized intersection arithmetic (both
   sides of a shared edge compute the identical cut vertex). Static per-piece
   64-aligned ubias/vbias (bounded texel span by construction). Bake-time numeric
   self-checks printed at load: T-junction count MUST be 0, max texel span, piece and
   vert counts.
2. **Runtime:** per visible piece — cull, transform, per-vertex distance light, draw.
   Near/guard clipping happens per TRIANGLE at transform time from shared endpoints
   (both neighbours derive identical clip vertices), never by re-cutting polygons.
   CPU emit first; no RSP anywhere near planes until the user's eye passes the CPU
   image.
3. **Gate:** the user looks at it. Not a luminance table. Then Phases B/C/D as above.

### Welded bake: built + machine-verified, awaiting the user's eye (2026-07-03)
- **Stage 1 (bake, `5670167`+`9f2cb3e`):** `P_BakeWeldedPlanes` -- footprint trim,
  weld, 512-grid cut, exact PU_LEVEL pools. E1M1: pieces=367 verts=1748 maxv=10
  weld_ins=238 **TJUNC=0** spanmax=512/575. First boot trapped the FPU (2^31 grid
  bound on map-edge overhang) -- fixed by clamping + trimming leaves to the sector
  footprint before welding. Bake is load-time only (mesh bench unchanged).
- **Stage 2 (runtime, `46cfb7f`+`a676e60`):** `DL_DrawPMeshPlanes` behind
  `BENCH_FORCE_MESH_PMESH` (preset `mesh-pmesh`). Cull + transform + per-vertex
  distance light + CPU emit; per-TRIANGLE canonical world-space clips (near, two
  side guards, and a per-surface VERTICAL guard -- first capture review found
  giant dark screen smears from |sy| overflowing the RDP's s11.2 edge-Y on
  near-clipped verts under tall ceilings; the fourth clip plane fixed it,
  re-verified gone at frames 896/4096).
- **Verification run (agent protocol -- NOT the acceptance gate):** full 4117-frame
  demo boots and completes; 36-frame capture sweep vs the frozen software refs
  reviewed zoomed (floors, ceilings, junctions): no cracks, no seams, no smears,
  no missing surfaces. `BENCH_VOID_SCAN` demo-wide: only known-benign frames 0 and
  3213 (wipe). Residual notes: far-ceiling light reads slightly brighter than
  software in dark distant areas (mild; same class as the RDP walls' look);
  RDP point-sampling blockiness as on the accepted mesh walls.
- **Perf:** `mesh-pmesh` 19227/37472 vs `mesh` 17667/32416 (+8.8% avg / +15.6% p95)
  -- CPU triangle emit, no culling levers yet; correctness first, per this plan.
- **GATE: the user's eye — STILLS PASSED (2026-07-03, "it's looking good" on the
  zoomed capture panels).** The LIVE-MOTION check remains open (frozen captures
  cannot show motion-class defects; the user declined a live run for now — re-offer
  when convenient). ROMs: `/tmp/pmesh-timing.z64`, `/tmp/pmesh-marks2.z64`;
  captures `/tmp/cap/pmesh/`, comparison panels `/tmp/cap/cmp/`.

### Phase B: masked midtextures on Z — built + machine-verified (2026-07-03)
- `BENCH_FORCE_MESH_MASKED` (preset `mesh-pmesh-masked`, commits `48f1431`..`3ef077b`):
  baked midtex quads (`P_BakeMidtex`, 13 on E1M1; opening resolves LIVE from both
  sectors), drawn by `DL_DrawMaskedQuads` after the opaque world — Z-tested +
  Z-written under RDP alpha-compare. The masked CI4 block cache post-walks the
  columns, keeps the 15 most frequent opaque colours, reserves index 15 as the
  TLUT-alpha-0 key: transparent texels write neither colour nor Z, so no sorting.
  Per-corner scalelight distance light; segment t-interval clips (near/side/Y).
  Software `R_RenderMaskedSegRange` suppressed; drawsegs remain for sprites.
- **Two defects found + fixed during the protocol:** (1) CI4 blocks cannot be
  LOAD_TILE'd directly — first boot crashed the RDP ("4-bit VRAM pointer"); fixed
  with the wall path's I8-byte-view two-step load (`0a1e6d1`). (2) Grates carry the
  midtexture on BOTH sidedefs; drawing both quads z-fought a mirrored-S copy over
  the correct one (grate rendered displaced at frame 512) — fixed by drawing only
  the viewer-facing side per vanilla's `R_PointOnSide` convention (`3ef077b`).
- **Verification (agent protocol — NOT the acceptance gate):** full 4117-frame demo
  completes; frame-512 grate matches software's position/pattern/transparency in
  the registered zoom; capture sweep shows no new deltas vs the pmesh build beyond
  sprite sub-states; void scan clean (only frames 0 + 3213). Perf: 19344/38432
  (+0.6% over `mesh-pmesh` — 13 quads).
- **Slice-1 limitations (recorded, graceful):** blocks point-sample-halved to
  <=64x64 (soft grates at close range); >128-wide/tall masked textures refused
  (none on E1M1); multi-patch masked behaves as vanilla (Medusa-class parity);
  masked can Z-leak over SOFTWARE-drawn walls (movable-sector exclusions write no
  Z) — resolved when Phases C/D put everything on Z.
- **GATE: the user's eye — pending.**

### Phase C: sprites on Z — built + machine-verified (2026-07-03)
- `BENCH_FORCE_MESH_SPRITES` (preset `mesh-sprites` = A+B+C): vissprites
  collected at `R_DrawMasked` time (already sorted far→near) into a
  present-time arena; `DL_DrawSpriteQuads` draws them after the masked pass
  under alpha-compare. Per-patch **CI8 full-fidelity blocks** (`0e4b9ca`):
  exact PLAYPAL indices at native resolution, transparency key = an index the
  sprite never uses, private 256-entry master-TLUT copy (tracks palette
  flashes), T-banded through TMEM at draw. Sprites are constant-depth
  screen-aligned quads: S/T linear in screen space, Y-guard clip exact. Flat
  per-sprite light from the vissprite colormap level. Fuzz (MF_SHADOW) +
  weapon psprites stay software.
- **Boot defects found + fixed by the protocol:** missing `m_swap.h`
  (SHORT/LONG link failure); the aligned(8) TLUT member vs Z_Malloc's 4-byte
  guarantee (misaligned doubleword store trap — fixed by 8-aligning both cache
  bases, latent in Phase B too); arbitrary sprite widths vs the 8-byte TMEM
  pitch rule; +28 KB of link-time BSS starving I_InitGraphics' scratch screens
  (`9b0df43` — renderer caches must live in the zone, not BSS).
- **The Z policy took five iterations to get right (2026-07-03).** The user
  caught wrong sprite colours (CI4 15-colour quantization hue-shifted the
  zombieman — replaced by CI8, verified by pixel values). A blast at capture
  frame 2816 lost its right half; the defect survived four theories, each
  disproven by a machine check: sprite z-write holes (`be73c82` — bbox
  byte-identical), constant z-bias (`13b7315` — byte-identical), PU_CACHE
  eviction mid-scatter (`af98f9e` — the SPRBLK FNV-1a hash cross-checked
  against a host WAD decode matched 79/79 pinned AND 78/78 unpinned, so the
  blocks were never corrupt; the pin stays as defence), and plane z-write
  (`c391a91` — truncation unchanged with planes z-test-only). The z16 probe
  then showed the occluder at ~8 map units vs the blast at ~10.6: the camera
  was hugging an oblique wall. **Software's occlusion is line-based, not
  ray-based** — `R_DrawSprite` clips against a seg only when the sprite is on
  the seg's FAR side (`R_PointOnSegSide`), so software draws the blast over
  the nearer wall pixels and no constant bias can reproduce that. Fix
  (`2b20b7e`): z-test per sprite — only when a visible line overlaps its
  columns, is nearer, and has the sprite on its far side. Sprites never
  z-write (co-located blast pairs resolve by painter order).
- **Supporting architecture (same date):** planes z-test only, painted
  back-to-front by height — exact for horizontal planes (`c391a91`); z-only
  seg silhouette skirts reproduce SIL_BOTTOM/SIL_TOP at the line's depth
  (`800b112`); masked z-writes explicitly. The z-buffer sprites test against
  is exactly software's silhouette set: walls + skirts + masked.
- **Verification (2026-07-03, screen-free under the locked session):**
  `BENCH_MARK_FBSCAN` banded warm-pixel bbox of the VI-displayed framebuffer +
  raw z16 probe rows (`be16203`, `800b112`), measured in the ares log. Frame
  2816 bands 0/1 right edge: software 181/194 (host-measured on the frozen
  refs), old builds 167/170, fixed build **181/195** with pixel counts
  matching the no-z-test control. All 36 marks diffed old-vs-new: every
  changed frame moved TOWARD the software reference (frame 1792's clipped
  sprite top restored; frame 384's spurious warm pixels gone). Full demo
  completes; teardown clean on every run.
- **Slice-1 limitations:** >256-wide sprites refused (none in DOOM1);
  colour-translation (multiplayer suits) not wired; fuzz still consumes
  drawseg clips — Phase D must move or accept it; the per-sprite z decision
  is per-sprite, not per-column, so a sprite simultaneously hugging a near
  wall AND legitimately clipped by a far-side line keeps the z-test (software
  would split per column; not observed on E1M1's marks).
- **GATE: the user's eye — pending (screenshot galleries queued for after the
  session unlocks; the lockscreen blocked all display capture tonight).**

### Phase D design (2026-07-03) — traversal, sky, fuzz, and what blocks it
Numbers measured on the CURRENT mesh-sprites build (BSPWALK_PROBE rerun
2026-07-03 after the probe frame-cap unblocked the boot; means over 3072
frames, tail = worst 5%).

**The prize.** Strip-able CPU on the current path: `addline_net` **696 us
mean / 2155 us tail** + `segloop` **1828 us mean** (the clip-array/visplane
fill that ONLY sky and fuzz still consume) + recursion/`R_FindPlane` glue.
`checkbbox` (184/468) stays — the traversal keeps the node prune; the
`R_AddSprites` collect (171/391) stays, called from the traversal instead.
`DL_MeshDrawWalls` (983 mean, 742 of that the RSP-transform wait) is NOT
Phase D's target — that is the RSP-emit keystone's lever. Net: roughly
**2.5+ ms mean** of pure visibility/fill work dies with the walk.

**Traversal.** Replace `R_RenderBSPNode`→`R_AddLine`→solidsegs with a
frustum-only node walk: keep `R_CheckBBox` node pruning verbatim, visit every
surviving subsector front-to-back, and per subsector (a) mark `bake_leafvis`,
(b) call `R_AddSprites(frontsector)`, (c) per seg with a linedef: facing test
+ frustum overlap → mark `bake_linevis` + set `ML_MAPPED`. No solidsegs, no
per-seg angle clipping, no drawsegs, no openings, no visplanes. Occlusion is
entirely the z-buffer's job (walls + skirts + masked — the Phase C
architecture already assumes exactly this set). Overdraw is the risk the plan
already flags: conservatively-visible leaves/walls behind walls get drawn and
z-discarded. **MEASURED (PVS_PROBE, 2026-07-03, current build): the walk
visits only ~9 subsectors/frame mean — solidsegs prunes E1M1 brutally — and
REJECT would cull 0.8% mean / 4.1% tail of even that. Two conclusions: REJECT
is useless as a Phase D filter, and a frustum-ONLY traversal balloons the
visited set (E1M1 has 237 subsectors; a view cone passes tens), multiplying
the per-visit CPU (mesh wall emit, pmesh pieces, sprite collect) that
currently scales with ~9.** D3 therefore needs a real PVS: a baked
per-subsector potentially-visible-set (computed at level load from portal
windows, DOOM 64's model — fits the bake-everything architecture), so the
traversal visits PVS∩frustum, not frustum. Without it the walk strip can
easily cost more emit CPU than the 2.5 ms it saves — the exact "culling too
loose" regression the plan warns about.

**Sky.** With the walk gone there are no sky visplanes — and none are needed:
draw the sky FIRST as an angle-mapped screen quad with z-test and z-write
OFF, then let the world paint over it. Sky remains wherever skyflat surfaces
left holes (pmesh already skips `picnum==skyflatnum`), which is exactly
DOOM's semantics (sky = background through skyflat holes). Software maps
column→texture as `(viewangle + xtoviewangle[x]) >> ANGLETOSKYSHIFT`;
`xtoviewangle` is arctan-shaped, so one quad's linear S would warp the sky —
split into 16 x-strips with the exact per-edge S and the interpolation error
drops under a texel. CI8 block for the 256x128 sky patch, T-banded like
sprites. Its own slice with its own eye gate.

**Fuzz (MF_SHADOW).** The last drawseg consumer. Options: (a) keep software
fuzz — then `R_StoreWallRange` + clip arrays must survive just for spectres,
which keeps most of the walk alive and defeats the phase; (b) fuzz on Z —
draw the sprite quad alpha-keyed with a screen-space dither/decimation
(fuzzoffset-style column jitter is reproducible in the combiner/blender with
a noise or checkerboard alpha), z-tested like any sprite. (b) is the design
choice; it changes fuzz appearance subtly and gets its OWN eye-gated slice
before the walk dies. Weapon psprites stay software (screen-space, no world
clip).

**Order of slices.** D1 sky (kills sky visplanes; walk still on) → D2 fuzz on
Z (kills the last drawseg consumer; walk still on) → D3 the traversal switch
(`BENCH_FORCE_MESH_WALK`), at which point `R_AddLine`/`R_StoreWallRange`/
`R_RenderSegLoop`/visplane construction stop being called on mesh builds —
delete after the eye gate + perf gate pass. Automap and demos must be checked
on D3 (ML_MAPPED semantics move to the traversal's facing+frustum mark).

**Memory ceiling (found 2026-07-03).** The RDRAM budget is now exactly at the
edge: +28 KB of BSS killed `I_InitGraphics` scratch-screen allocation (fixed
by moving the sprite occluder list into the zone, `9b0df43`), and the
BSPWALK_PROBE build (per-frame probe fields across the ~4.1k-frame bench
array) no longer boots at all — same scratch-screen death. The CI8 sprite
cache (`0e4b9ca`) holds every drawn sprite lump at native res + a 512-byte
TLUT each (~250 KB on the E1M1 demo) with no demote schedule. Levers, in
order: a sprite-block demote schedule (the wall/flat pattern), probe builds
shrinking the bench frame array, and auditing the CI8 blocks for lumps drawn
once (eviction candidates).

**Perf debt from the Phase C correctness architecture (2026-07-03).**
`mesh-sprites` 21029/44448 vs 19172/38112 before the plane/skirt/line-rule
changes (baseline `mesh` 17667/32416). Known costs and their levers: the
height-major plane sort breaks flat batching (re-batch by flat WITHIN equal
heights — already the minor key; measure how many switches remain), skirt
quads add RDP fill (gate on lines with an actual live step; skip lines whose
screen interval is empty), the occluder list rebuilds per frame (rebuild only
when viewangle/viewx/viewy changed beyond epsilon), and per-sprite zbuf
toggles cost pipeline syncs (partition the arena into ON/OFF runs while
preserving far→near order within each — or accept, sprites are ~20/frame).
Correctness first was the standing order; none of these levers may regress
the Phase C fidelity wins.
