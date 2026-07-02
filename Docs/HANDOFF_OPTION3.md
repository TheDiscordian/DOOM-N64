# HANDOFF — Option 3 full-scene Z renderer (reconstructed 2026-07-02)

> **CORRECTED SAME DAY — read `GPU_PORT_PLAN.md` § "THE GOAL, STATED BY THE USER
> (2026-07-02)" before acting on anything below.** The "correctness verified" world-Z
> claims repeated in this file failed user visual review (jagged edges); the "RSP
> no-readback emit" next-step this file recommends was attempted twice and reverted
> (`031ad31` — planes flickered). The corrected goal: geometry finished at level load,
> welded shared vertices, runtime = cull + transform + draw only, acceptance = the
> user's eye at the edges. This file remains as reconstruction context only.

The original handoff document was lost in a laptop crash on 2026-07-01. This is its
reconstruction from the repo itself (git history, `GPU_PORT_PLAN.md`, `RSP_PORT_PLAN.md`,
`CEILING_VOID_INVESTIGATION.md`, `PAST_BUGS.md`, and the code on
`perf/option3-fullscene-z` @ `242ebfb`). Nothing besides the prose was lost: the working
tree was clean and pushed at the time of the crash; the reflog shows no work after the
final commit (2026-06-30 23:25).

## TL;DR

Option 3 = "the whole level as a mesh": make the shared 16-bit Z-image the sole
visibility authority for the opaque world and, phase by phase, delete DOOM's CPU
visibility machinery (`R_AddLine` occlusion, `R_StoreWallRange`, `R_RenderSegLoop`
column fill, visplane `top[]/bottom[]` masks, drawseg sprite-clip arrays). Full plan:
`GPU_PORT_PLAN.md` §"DIRECTION CHANGE (2026-06-30)". Phase A (opaque world Z:
walls + floors + ceilings) has a first slice landed, correctness-verified, and
CPU-optimised — but it is still a perf loss vs shipping, exactly as the plan predicted
for a CPU-emit stepping stone. The named next levers are (1) RSP/no-readback emit for
the world-Z plane geometry (the same keystone the walls use) and (2) coarse
PVS/frustum culling. "Do those before judging Phase A on perf."

## Branch map

| Branch | Role |
|---|---|
| `perf/rdp-renderer` @ `383cd05` | Shipping / known-good: mesh walls + poly planes. A/B reference for everything. Do not dirty. |
| `perf/option3-fullscene-z` @ `242ebfb` | **The active Phase A branch** (this one). |
| `perf/rdp-mesh-planes-retry` @ `7251426` | Parked reference: the refuted mesh-plane attempt. Holds the cell bake, `EMIT_ZBIAS`, and the EMITDIAG/LEAFRB/LEAF_CPU_EMIT diagnostics. Cherry-pick source, not a path forward. |

## Phase A status (all commits 2026-06-30)

| Commit | Landed | mesh-worldz avg/p95 µs |
|---|---|---|
| `1e8b9cc` | First slice: `BENCH_FORCE_MESH_WORLDZ` — CPU-emitted baked leaf planes as opaque Z-tested geometry, non-sky poly planes suppressed (`r_plane.c:1611`) | 21222/46112 |
| `f6679d4` + `92f470e` | Per-leaf depth-range bound on band clipping; band ratio tuning | 20361/43232 |
| `d7395ff` + `ecd5ba5` | **Correctness fix**: per-vertex distance lighting (`yslope`+`zlight` per vertex's own screen row, gouraud SHADE, `TRIFMT_ZBUF_SHADE_TEX`); near band ratio 8→4. Frame-3200 floor underdraw −38 → −4.1 vs poly; frame 3712 within ±3 | 21540/46304 |
| `5bcd625` | Per-leaf side-clip + depth-range cache (side clip is height-independent — computed once, shared by floor/ceiling and every flat bucket) | **19141/38368** |

Shipping `mesh` reference: **17667/32416**. So Phase A currently costs ~+8% avg / +18% p95.
Correctness state: `BENCH_VOID_SCAN` clean demo-wide (only known-benign frames 0 and
3213 = melt wipe); region-luminance A/B vs poly + frozen software ref passes (see
Verification below). Phase A acceptance gate (`GPU_PORT_PLAN.md`): hole-free AND
avg/p95 ≤ shipping — the perf half is NOT met yet, by design.

The implementation lives in `rdp_view.c` (world-Z block ~4831–5176; dispatch from
`DL_DrawMeshLeaves` at ~5624, which routes `n64_rdp_mesh_worldz` away from the old
mesh-floors path). Bake: `P_BakeLeafFans` in `r_bake.c`; per-frame vis seed:
`R_MeshMarkSubsector` from `R_Subsector` (`r_bsp.c` — explicitly marked "Phase-A cull
seed; later replaced by PVS/frustum cull").

## Next work, in order

### 1. RSP no-readback emit for world-Z planes (the big one — start here)

Today the whole world-Z path is CPU work inside `dlbuild`: band clip + transform +
lighting + libdragon `rdpq_triangle()` setup per tri. The keystone that fixed the same
problem for walls (overlay B, `rsp/rsp_dlemit.S`) already exists and is
visually verified; the §8.5 LeafFan emit (`DLEmitCmd_LeafBatch`) already emits
shaded/textured/Z leaf fans with no CPU readback. Concrete gap list (verified against
the code 2026-07-02):

- **Per-vertex gouraud shade plumbing — the only real ucode gap.** LeafFan shade is
  one flat colour per fan (`rsp_leaf_desc_t.prim`); world-Z needs per-vertex distance
  light. The CPU already knows each vertex's view depth during band clipping
  (`DL_WZClipBandToFixed`), and `R_MapPlane`'s `planeheight·yslope[row]` identity IS
  that view depth — so the CPU can compute the colormap level per vertex without a
  transform round-trip, pack RGBA from `dl_prim_lut`, and carry it in the free pad
  words of `rsp_bleaf_in_t`/`rsp_bleaf_out_t` (0x0C / 0x1C). `StageLeafVtx` then loads
  per-vertex RGBA instead of `LF_light` (~10 instructions). zlight lookups stay
  CPU-side (RSP table lookups are hostile; walls already do this split).
- **Z-convention unification — mandatory, not optional.** CPU paths use
  `DL_WallZ` = depth/32768; overlay B uses screen-affine `0x7FFF − 2·invw`. Different
  monotone curves — mixing them in one Z-image breaks depth compares. RSP-emitting the
  planes therefore requires the walls on `BENCH_FORCE_MESH_RSP_EMIT` in the same build.
- **Port `EMIT_ZBIAS` from the retry branch** (13-line diff in `rsp_dlemit.S`,
  commit `fa3cd11`/`7251426`): per-path screen-Z bias (0 for walls, 32-toward-camera
  for leaves) so tilted planes' far edges win the 1/depth-precision z-fight. Bias Z
  only, never invw (W·INVW=1 must hold or textures warp — PAST_BUGS).
- **Per-descriptor S/T bias.** The descriptor already packs `ubias/vbias` per
  (leaf, band) — compute band-local 64-aligned biases CPU-side during the clip so no
  triangle's S/T span approaches the s10.5 ~1024-texel limit. The retry branch's cell
  bake (`BAKE_CELL_SIZE 512` static grid, in `7251426`) is the proven stronger answer
  if runtime bands prove too coarse or too costly — port it rather than re-derive it.
- **Dispatch skeleton exists.** `DL_RSPLeafDispatch` (staging + LeafBatch transform)
  and `DL_LeafRSPEmitFlush` (per-flat TMEM bind + descriptor batching + the
  never-rewind cursor + one `Send_End` drain per batch) are debugged on THIS branch.
  The work is routing `DL_DrawWorldZPlanes`'s gather (vis filter, live heights,
  per-surface sky skip, banding, per-vertex light) onto that machinery behind a new
  flag (suggest `BENCH_FORCE_MESH_WORLDZ_RSP_EMIT`, nested under WORLDZ, pulling in
  RSP_EMIT + LEAF_RSP + LEAF_EMIT; remember RSPASFLAGS as well as CFLAGS).

### 2. Coarse cull (Phase D foundation)

`bake_leafvis` currently comes from the full BSP walk — Phase A still runs all the CPU
visibility machinery it exists to delete. A frustum + sector/leaf PVS traversal
replaces it in Phase D; `BENCH_PVS`/`PVS_PROBE` (r_bsp.c) and `BENCH_FORCE_MESH_CULL`
are the existing probes/foundations. Unrun go/no-go: PVS worth building only if the
walk is >25% of the p95 tail (it is: bsp_walk tail ~9.3k µs was #2 lever pre-Option-3).

### 3. Phases B/C/D

Masked midtex → sprites → strip the walk. Roadmap + acceptance gates:
`GPU_PORT_PLAN.md` lines ~496–520. Not started. Gotchas that must survive every phase:
automap `ML_MAPPED`, live sector heights (never cache Z), sky pass skip, colormap /
CI4 damage-flash / fixedcolormap per surface, back-to-front sort where blending matters.

## Verification requirements (Phase A acceptance)

- `BENCH_VOID_SCAN` is a WEAK oracle — only catches ≥40% black. It CANNOT see the
  underdraw/darkening class that killed the mesh-plane retry. Every acceptance check
  pairs it with the **region-luminance A/B**: same-geometry `BENCH_MARKS` captures of
  candidate + poly baseline + frozen software ref
  (`~/.local/share/doom-n64-bench/ref-sw-frozen/` — never re-capture), active game
  rect normalised to 320×240, per-region mean luminance deltas. Key frames: 3200, 3712.
- Timing: `bench/bench.sh mesh mesh-worldz …` (deterministic — measure each config
  once; A/B within one ares session only). Never time a MARKS/VOID_SCAN build.
- Motion bugs (staleness/ghosting) are invisible in frozen captures — live grab needed.
- New work classes must be OR'd into the `I_FinishUpdate` render gate
  (`DL_Count()+DL_SpanCount()+DL_PolyCount()+DL_RSPEmitPending()`) or dropped frames
  reproduce the 3-buffer ghost.

## Trap index (full details in PAST_BUGS.md / CLAUDE.md — read before debugging)

- Any CPU read of RSP output needs `rspq_wait()` first — five separate "failed fixes"
  died on this once already.
- Descriptor buffers: frame-wide never-rewinding cursor; per-flat rebuild from index 0
  is a CPU-ahead-of-DMA race.
- `rsp_rdpq_tri` engine: VTX_ATTR offsets exact; REJFLAGS are negated (write 0 = cull
  everything); when valid geometry doesn't rasterise, check those two before texturing.
- Screen-Y through overlay B is s11.2 (±2047) with the clip path STUBBED — clip Y
  into range CPU-side before emit.
- cpp trap in `rsp_dlemit.S`: never a trailing `#` comment on a numeric `#define`
  used in `.ds.b` (truncates DMEM allocations, desyncs rspq).
- Flag parenting: a child `BENCH_*` flag without its parent silently builds all-software
  (md5-compare ROMs when numbers look identical). Full clean rebuild
  (`rm -rf filesystem build`) mandatory on any `-D` change.
- Docker builds run as root — chown the tree back (the 2026-06-30 session forgot;
  fixed 2026-07-02).
- ares only via the committed launchers; verify teardown after every run with BOTH
  `pgrep -x ares` and a hyprctl window grep.

## Operator crib

```bash
# Build + bench one preset (Docker build + ares + scrape; ~10 min):
bench/bench.sh mesh-worldz
# A/B vs shipping mesh (delta table vs the FIRST preset):
bench/bench.sh mesh mesh-worldz
# Marks ROM for visual A/B (never for timing):
docker run --rm -v "$PWD":/doom -w /doom -e N64_INST=/n64_toolchain doom-n64:tc \
    bash -c "rm -rf filesystem build && make BENCH=1 BENCH_FORCE_MESH=1 BENCH_FORCE_MESH_WORLDZ=1 BENCH_MARKS=1 -j4"
docker run --rm -v "$PWD":/doom doom-n64:tc chown -R "$(id -u):$(id -g)" /doom
bench/scan-marks.sh Doom-N64.z64 /tmp/cap/worldz
```

Full result line: `BENCH_RESULT … avg_us= p95_us= …` on stdout; phase table + worst-frame
rows in `/tmp/bench-<label>-ares.log`. The 2026-06-30 `/tmp` capture artifacts did not
survive the crash — regenerate as needed.
