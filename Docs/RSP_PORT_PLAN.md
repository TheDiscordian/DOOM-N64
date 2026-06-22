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
