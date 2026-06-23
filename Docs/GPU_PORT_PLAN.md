# GPU Port Plan — static world mesh + cull + RSP-fed raster

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
  per-column; beats the planes-only ship too.** OPEN (fidelity, not occlusion/perf):
  texture `textureoffset`/`rowoffset`/`ML_DONTPEG*` not threaded (S=0, top-pegged →
  misaligns vs software); grazing-angle S-precision smear.
- **Phase 3 — Baked leaf-fan floors/ceilings** (NEXT). ⚠️ **THE HARD PART = closing
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
- **Phase 4 — Moving sectors:** the `T_MovePlane` dirty-mark + per-frame Z-patch;
  A/B a frame mid door/lift animation.
- **Phase 5 — Transparent midtex + flash/colormap + end-state sweep:** masked
  midtex through the existing path; confirm flash/colormap; full RDP-vs-software
  avg+p95 sweep.

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
  `dlbuild` reduction is the **RSP transform port** (move projection to the idle RSP) — see
  Docs/RSP_PORT_PLAN.md §7: Step B offloads it but is sync-bound; async overlap is the win.
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
- **Method note:** measuring one phase in isolation hides cross-phase cost (the RSP offload
  dropped `dlbuild` but added more wait; the Z-buffer "loss" was the same trap). Always check
  TOTAL frame time, not the single phase you touched.
