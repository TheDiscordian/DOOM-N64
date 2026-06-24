# RSP Wall-Transform Port — Plan

Goal: move the mesh vertex TRANSFORM (`DL_MeshDrawWalls`, the proven −16.6%/−30.3%
wall win) from the VR4300 CPU onto the **RSP** (the N64's real hardware T&L unit, idle
in this port), so adding geometry stops costing main-CPU time. Produced by a parallel
mapping pass (`wf_36c2c97b-f4f`), adversarially scoped.

## 1. Approach: hand-written `.S` minimal ucode (NOT RSPL, NOT t3d reuse)
- **RSPL compiler is absent in this tree** — `.rspl` exists only under `tiny3d/` with
  pre-generated `.S` checked in; no compiler, no `.rspl→.S` rule. libdragon's native path
  is hand `.S` (`n64.mk:83,168-209`, `rsp.ld`), confirmed present in the Docker image.
- **t3d reuse is the wrong fit:** t3d wants `T3DVertPacked` (packed normals, hardwired Z
  attr, per-vertex lighting DOOM doesn't use). Marshalling DOOM verts in + stripping all
  that out is more code than a purpose-built ~30-line transform emitting `rdp_wall_t`.

## 2. Architecture: Integration-Shape A (RSP → `rdp_wall_t[]` in RDRAM → unchanged CPU `DL_Flush`)
Keeps every fragile piece — CI4 sub-palette TMEM slotting, PRIM/upload/tile dedup,
combiner/scissor — on the CPU exactly where it is. The RSP produces the SAME `rdp_wall_t`
records the CPU produces today. (Shape B — RSP emits RDP triangles directly — rejected:
would replicate `rdpq_triangle` gradient setup + a brittle combiner-sequencing dependency.)

Per frame, when `n64_rdp_mesh_rsp=1`: CPU packs a small DMA-aligned **view block** (viewx/y,
viewzf, vcos/vsin, centerx/centery, **resolved per-quad ztop/zbot snapshotted from live
sector heights** — so the RSP never touches `sectors[]`; this composes with the moving-
sector fix c3c4857) + writes it back; `rspq_write` the overlay command; RSP DMAs the view
block + `bake_wall_t` batch in, transforms, DMAs `rdp_wall_t` out + writes nrec; CPU
invalidates + reads, then the UNCHANGED sort + `DL_EmitWallTier` + `DL_Flush`.

Transform mirrors `rdp_view.c` `DL_MeshDrawWalls` exactly (depth, near-clip, lat, invw,
sx/sy, screen-edge clip, light) — nothing more.

## 3. Build
RSP asm rule auto-fires on `rsp_*.S` (`n64.mk:168-209`); toolchain present in Docker.
- Create `linuxdoom-1.10/rsp_dlwall.S` (`#include <rsp_queue.inc>`).
- Add `$(BUILD_DIR)/linuxdoom-1.10/rsp_dlwall.o` to the `.z64` ROM prereqs (only Makefile edit).
- `DEFINE_RSP_UCODE(rsp_dlwall);` + `rspq_overlay_register(&rsp_dlwall)` in rdp_view.c.
- Docker-only build. WATCH: `-Wa,--fatal-warnings` — RSP asm warnings are hard errors.

## 4. Phases (flag: runtime `n64_rdp_mesh_rsp` under `n64_rdp_mesh`; compile `BENCH_FORCE_MESH_RSP`)
- **Phase 0 — loopback probe (no math).** Overlay DMAs N `bake_wall_t` in and straight
  back; CPU memcmp's the round-trip. Proves overlay registration + 8-byte DMA alignment +
  cache writeback/invalidate coherency. THE FIRST THING TO BUILD.
- **Phase 1 — ONE wall, bit-exact compare** vs the CPU `DL_MeshDrawWalls` math. The
  load-bearing gate: if the RSP output can't match within epsilon (0.5px screen / 1 texel),
  the precision risks (§5) are real → NO-GO. Nothing renders yet.
- **Phase 2 — full batch, CPU still authoritative. DONE (with a characterized precision
  residual).** `DLWallCmd_Batch` transforms the WHOLE visible wall set on the RSP each
  frame (DMEM-chunked, `BATCH_N=8`), reproducing every `DL_MeshDrawWalls` skip/clip so the
  RSP and CPU emit decisions align slot-for-slot; `DL_RSPBatchProbe` compares the whole
  batch every frame and logs `RSP-BATCH frame=.. walls=.. mismatch=.. emit_disagree=.. worst_*`.
  Over the full E1M1 demo (495 frames): **emit_disagree ≈ 0** (0-2 walls, a sub-pixel sx
  flipping an off-screen boundary) — the structural/slot-alignment goal is met. The field
  residual is the single-pass `vrcp` precision: ~70% of frames are within the Phase-1 gate
  (invw ≤ 1%), the rest reach ~1-2% invw / ~1px sx on depths far from `NORMBIT`, plus rare
  large-sx spikes on near-clipped walls whose slid corner sits at depth≈nearz (huge invw).
  **Phase-3 prerequisite:** a `vrcp` + Newton-Raphson refinement (§5.1) to pull the divide
  under the gate before the render cutover. Bugs found+fixed en route, all load-bearing:
  (a) batch loop state / per-wall scratch were emitted in `.text`/IMEM — RSP `lw/sw` only
  reach DMEM, so every spill silently corrupted; moved to `.bss`. (b) the near-clip `u` was
  kept in a t-reg across `FixedMulVU` (which clobbers it) — spilled to DMEM. (c) the fixed
  `FIXEDDIV_SH` only suited ONE divisor magnitude — added divisor-exponent normalization
  (scalar msb-search → `NORMBIT`) so the divide tracks depth; the parked shift collided with
  `FixedMulVU`'s operand slot and `RecipFixVU` clobbered Phase-1's live `top16/bot16` regs,
  both fixed.
- **Phase 3 — cut over.** Render off the RSP output; A/B pixel-identical; confirm the
  dlbuild wall cost drops (host-independent CP0 ticks).
- **Phase 4 — floors.** Extend to the leaf-fan transform, same compare→cutover discipline.

## 5. Risks / SHOWSTOPPERS (resolve in Phase 1)
1. **`65536/depth` divide on the VU** — no float divide; `vrcp`+Newton or keep the divide
   on the scalar core. Drift compounds into sx/sy + the perspective `s = si/invw` recovery.
2. **16-bit VU `FixedMul`** — needs the `vmudh`+`vmadm`+`vmadl` dual-precision pattern to
   reproduce the 64-bit-intermediate `>>16` exactly.
3. **IEEE32 float output** — `rdp_wall_t` has 12 floats; **the RSP has no FPU.** This is the
   deepest risk: either produce fixed-point on the VU + change `DL_DrawRecord` to consume
   fixed-point, or software-float (slow). Phase 1 decides. (The mapping agent's "RSP scalar
   FPU" is wrong — the RSP scalar core has no COP1.)
4. **DMA coherency** — writeback `bake_walls` once at bake (static) + the view block each
   frame; invalidate the out arena before reading. Precedent: rdp_view.c:1259,1436,3944.

Non-showstoppers: overlay sequencing (our ucode finishes + DMAs before any `rdpq_triangle`),
combiner/TLUT (set by `DL_Flush` after our ucode; our ucode issues no RDP command), arena
cap (`DL_WALL_ARENA=512`).

## 6. First code = the Phase 0 loopback in `rsp_dlwall.S`.

## 7. Phase 3 RESULT (Step B landed, `180b68d`) — offload works, but it's SYNC-BOUND
The cutover is real now: `DL_MeshDrawWalls` skips the CPU projection (the two
`65536/depth` divides + lat/sc/sx/screen-Y) under `n64_rdp_mesh_rsp` and renders straight
off `batch_out`; `DL_RSPBatchProbe` is transform-only (the per-frame CPU-reference recompute
is gated behind `dl_rsp_verify`, default 0). Render is identical to Phase 3a and the prior
`emit_disagree=0` agreement holds.

**Measured (BENCH_FORCE_MESH_RSP, E1M1 bench):** `dlbuild` mean 2931->2232us (-24%), p95
8800->6432us. The projection genuinely left the CPU. **But total frame time is a NET LOSS:**
avg 19783 vs pure-CPU-walls 17901, p95 33632 vs 30816, and min-frame 6000->9882us — a FIXED
~1.9ms/frame overhead was added that swamps the ~700us the projection saved.

**Root cause = synchronous dispatch.** `DL_RSPBatchProbe` does `rspq_write(... BATCH ...)`
then `rspq_wait()` immediately — the CPU dispatches the transform and then BLOCKS on it, so
there is zero CPU/RSP overlap. On top of the wait, every frame it (a) packs all `bake_numwalls`
(~475 on E1M1) wall inputs even though only ~30 are visible, (b) `memset(batch_out, 0xA5, ...)`
poisons the whole out arena (a diagnostic), and (c) writeback `batch_in` + invalidate
`batch_out` across all 475 walls. That pack+coherency+wait is the ~1.9ms.

**To make the offload WIN — progress + the remaining lever:**
- DONE `b43c9cc`: dropped the per-frame `0xA5` output poison (diagnostic only) → −1700us/frame.
- DONE `8950cfa`: skip the sector-height lookups in the pack for BSP-culled walls.
- These took the RSP path from +17% to +4% over pure-CPU. The remaining ~1.9ms is structural.

**The lever was COMPACTION, not async overlap — and it LANDED as a WIN (`1d4fef1`).** The fixed
overhead was the per-frame pack + cache-coherency + RSP loop over ALL ~475 baked walls; async
overlap could only hide the ~30-wall CPU prep (a provably small window vs the 475-wall round-trip)
so it was never the lever. `DL_RSPBatchProbe` now builds a dense vis-list (walls passing
`bake_linevis && ztop>zbot` packed into slots `0..batch_nvis-1`, `batch_vislist[j]` = original
index) and packs + flushes + `rspq_write`s only `nvis` (~30). `DL_MeshDrawWalls`'s RSP draw loop
and the `dl_rsp_verify` compare loop both walk the vis-list, reading the dense `batch_out[j]`. The
per-wall RSP transform is unchanged, so it's render-equivalent by construction.

**Result — the offload now BEATS pure-CPU walls:** avg 17901→16823us (−6.0%), p95 30816→28896
(−6.2%), min frame back to ~6000us (the dispatch overhead is gone), 59.4 fps. dlbuild mean
2931→2383us. Verified with `dl_rsp_verify=1`: `walls=30`, `cpu_emit==rsp_emit`, `emit_disagree=0`
every frame; residual `mismatch` is sub-pixel fixed-point-vs-float (~1px, emit decisions agree).
- Re-measure rule (confirmed by this saga): total must beat the pure-CPU path, not just `dlbuild`
  in isolation — measuring one phase hid the dispatch cost (same trap as the early Z-buffer "loss").

**Still GATED behind `BENCH_FORCE_MESH_RSP`.** To bank the −6% in the shipped build, the RSP path
must go default-on — a Ryan call, since the fixed-point transform differs from the CPU float path
by ~1px sub-pixel (emit decisions agree, so no walls appear/vanish). Next: extend the same
compaction to the floor-leaf transform (Phase 4), then evaluate default-on.

## 8. Phase 4 design — floor-leaf transform on the RSP (examined 2026-06-24)
Goal: move the floor/ceiling leaf-vertex projection off the CPU (it's what makes floors-on a
p95 loss). The leaf transform is the SAME per-vertex projection as a wall corner — project
world XY → screen-x `cx`, scale `sc`, `invw`, depth `z` — only `cy = centery - hf*sc` (the
per-surface height term) and the u/v bias stay CPU (cheap; this is the existing CPU
transform-share `df533b8`).

Concrete plan (mirrors the wall offload, behind a NEW flag e.g. BENCH_FORCE_MESH_LEAF_RSP so
the DEFAULT rsp_dlwall ucode/build stays untouched until verified):
- rsp_dlwall.S: add `DLWallCmd_LeafBatch` (a 4th RSPQ_DefineCommand). Element = ONE leaf
  vertex (world x,y) → output {cx, sc, invw, z} (float-as-fixed, 1/65536 like the wall out).
  Reuse the wall command's projection block + the `vrcp`+Newton reciprocal. DMA leaf verts in
  in BATCH_N chunks through buffers sized like BIN_BUF/BOUT_BUF — WATCH DMEM (it's shared with
  rspq state; the wall BIN/BOUT already sit near the cap, so the leaf buffers may need to
  REUSE the wall ones, not add new ones).
- rdp_view.c DL_DrawMeshLeaves: pack the visible leaves' verts into a dense input (the leaf
  pre-pass from df533b8 already collects them), dispatch the LeafBatch, read back {cx,sc,invw,z}
  per vert, and the draw loop applies cy+bias+emit (already split out). Verify with the same
  dl_rsp_verify pattern (CPU-reference compare → emit/coord agreement).
- Then floors-on should become a clear win (leaf dlbuild drops like wall dlbuild did), and it
  can go default-on too — at which point the software visplanes are gone and R_StoreWallRange's
  plane-clip work can start coming off the BSP walk (step toward the bsp_walk collapse).

### 8.1 Phase 4 STATUS (built + verified, NOT cutover-ready) — precision wall
The leaf ucode (DLWallCmd_LeafBatch + XformLeaf, `62f9a9a`) and the CPU verify
(DL_RSPLeafProbe, `e4e5cc9`) are in, gated behind BENCH_FORCE_MESH_LEAF_RSP; the default
overlay stays byte-identical (the leaf code is #ifdef'd, since a bigger overlay cost the
default wall path +394us/frame in reload DMA — measured).

Verify result (BENCH_FORCE_MESH+FLOORS+LEAF_RSP, E1M1): **structurally correct** —
`emit_dis=0` every frame (RSP emits exactly the CPU's drawable verts; the cull/depth gate
is right). **But coord precision blocks cutover:** cx mismatches the CPU by ~5-12px on
~15-40% of verts (`worst_invw` ~1%). Walls stay ~1px because their lateral offset is
bounded; floor verts span wide, and `cx = centerx - centerx*lat/depth` magnifies the ~1%
ratio error into pixels.

**Ruled out:** a 2nd Newton-Raphson step in RecipFixVU was a NO-OP — the per-frame numbers
were byte-identical with/without it (frame4 worst_cx=12331, worst_invw=7619 both ways). So
the reciprocal is already converged; the residual is NOT reciprocal-iteration error. The
~1% lives downstream — the FixedDiv un-normalization (the NORMBIT `sh` shift + FIXEDDIV_SH
16-bit truncation in FixedDivApply) and/or near-`nearz` verts (huge invw, tiny absolute
error → big relative). Localized: the big cx errors are OFF-SCREEN verts (culled by the
X-window test anyway); ON-SCREEN verts hold ~1px, so a cutover renders correctly.

### 8.2 Phase 4 CUTOVER MEASURED — net LOSS, stays flagged-off (2026-06-24)
Wired the cutover: `DL_RSPLeafXform()` dispatches all visible leaf verts to the RSP
`DLWallCmd_LeafBatch` once per frame, then `DL_DrawMeshLeaves`'s pre-pass reads
`leaf_out_buf[leaf_rsp_slot[pv]]` (cx, invw) instead of the CPU divide. Floors render
correctly (frame-count identical, on-screen ~1px as predicted).

**A/B (BENCH_FORCE_RDP+MESH+FLOORS, E1M1 demo, 4117 frames):**
| build | avg µs | p95 µs |
|---|---|---|
| CPU-leaf (baseline) | 16638 | 30176 |
| leaf-RSP cutover | 17463 | 34144 |

**+5% avg / +13% p95 — a clear regression.** Root cause is the SAME as the pre-compaction
wall RSP Step B: a SEPARATE per-frame dispatch is a SECOND full RSP round-trip (overlay
reload + DMA + `rspq_wait` CPU stall) layered on top of the wall batch's round-trip. The
leaf divide it offloads is cheaper than that sync cost, so it loses. Compaction can't save
it the way it saved walls — the cost here is the second *barrier*, not the per-vert work.

**Next lever (the real fix):** fold leaves into the EXISTING wall batch — one combined
in-buffer, one `DLWallCmd_BATCH`-style dispatch, ONE `rspq_wait` for both walls and leaves
— so floors-on-RSP rides the wall round-trip already being paid instead of adding its own.
Only then can floors-on-RSP plausibly net ahead (and let the software visplanes go, opening
the bsp_walk plane-clip work). Until then leaf-RSP stays behind BENCH_FORCE_MESH_LEAF_RSP,
default-off. The cutover code is committed (working, correct) so the fold can build on it.
