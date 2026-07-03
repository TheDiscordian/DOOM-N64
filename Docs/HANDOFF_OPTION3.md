# HANDOFF — Option 3 full-scene Z renderer (reconstructed 2026-07-02)

> **CORRECTED SAME DAY, claims fixed in place — read `GPU_PORT_PLAN.md` § "THE GOAL,
> STATED BY THE USER (2026-07-02)" first.** The world-Z slice failed user visual
> review (jagged edges); the RSP plane-emit recipe this file originally recommended
> was implemented, flickered, and is reverted (`031ad31`) — the recipe is deleted
> below. The goal: geometry finished at level load, welded shared vertices, runtime =
> cull + transform + draw only, acceptance = the user's eye at the edges.

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
walls + floors + ceilings) has a first slice landed and CPU-optimised — but it is a
perf loss vs shipping AND it failed the user's visual review on 2026-07-02 (jagged
edges: T-junction cracks from the unwelded bake + per-frame band-cut seams). The
"RSP/no-readback emit" lever this file originally recommended was attempted twice the
same day and reverted (`031ad31` — planes flickered in/out). The actual next work is
the welded static bake (`GPU_PORT_PLAN.md` §THE GOAL).

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
| `d7395ff` + `ecd5ba5` | **Distance-lighting fix (luminance means only — NOT correctness; the slice later failed the user's eye)**: per-vertex distance lighting (`yslope`+`zlight` per vertex's own screen row, gouraud SHADE, `TRIFMT_ZBUF_SHADE_TEX`); near band ratio 8→4. Frame-3200 floor underdraw −38 → −4.1 vs poly; frame 3712 within ±3 | 21540/46304 |
| `5bcd625` | Per-leaf side-clip + depth-range cache (side clip is height-independent — computed once, shared by floor/ceiling and every flat bucket) | **19141/38368** |

Shipping `mesh` reference: **17667/32416**. So Phase A currently costs ~+8% avg / +18% p95.
Correctness state (corrected 2026-07-02): `BENCH_VOID_SCAN` clean and luminance means
within a few counts — but the slice **FAILED the user's visual review** (jagged edges:
T-junction cracks from the unwelded bake + per-frame band-cut seams). Phase A
acceptance gate: the user's eye at the edges AND avg/p95 ≤ shipping — NEITHER half is
met.

The implementation lives in `rdp_view.c` (world-Z block ~4831–5176; dispatch from
`DL_DrawMeshLeaves` at ~5624, which routes `n64_rdp_mesh_worldz` away from the old
mesh-floors path). Bake: `P_BakeLeafFans` in `r_bake.c`; per-frame vis seed:
`R_MeshMarkSubsector` from `R_Subsector` (`r_bsp.c` — explicitly marked "Phase-A cull
seed; later replaced by PVS/frustum cull").

## Next work, in order

### 1. The welded static bake (the real work — `GPU_PORT_PLAN.md` §THE GOAL)

Geometry finished at level load: floor/ceiling meshes with shared, welded vertices
(zero T-junctions), cut once on a fixed world grid, static per-piece texture biases.
Runtime = cull + transform + per-vertex distance light + draw; no per-frame side
clipping, near folding, band cutting, or texture rebiasing. CPU triangle emit until
the user's eye passes the image; RSP comes later, if ever, with a differential
root-cause first.

> This section originally recommended "RSP no-readback emit for world-Z planes —
> start here", with a full implementation recipe. That recipe was implemented on
> 2026-07-02 (per-vertex shade plumbing, Z unification, `EMIT_ZBIAS` port, then the
> retry cell bake) and produced planes that flicker in/out of existence. Both
> attempts are REVERTED (`031ad31`). The recipe is deleted from this file so it
> cannot be followed again; the emit machinery it targeted is condemned for planes.

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

- **The gate is the user's eye at the edges** (seams inside floors, band lines,
  wall junctions, stability in motion). No metric passes a build — the world-Z slice
  passed every metric below and still failed review (jagged edges, 2026-07-02).
- **Evidence for the user = EVERY mark frame as a FULL-frame A/B pair (RDP |
  software), in frame order, no zooms, no curation** (stated three times by
  2026-07-03; a curated top-5 + region zooms missed defects twice — the user's
  eye found the frame-768 seam wedges in a full pair after the assistant's
  picks skipped it). Zoomed crops are for the assistant's OWN diagnosis only,
  never the review deliverable. Machine sweeps run BOTH metrics: warm-pixel
  bands (bright defects) AND the dark-vs-lit structural count vs a baseline
  set (`darksweep`-style; black wedges are invisible to the warm metric).
- `BENCH_VOID_SCAN` is a WEAK oracle — only catches ≥40% black. The
  **region-luminance A/B** (same-geometry `BENCH_MARKS` captures of candidate + poly
  baseline + frozen software ref at `~/.local/share/doom-n64-bench/ref-sw-frozen/`
  — never re-capture — active game rect normalised to 320×240, per-region mean
  deltas; key frames 3200, 3712) catches area-brightness defects only. Both are
  SUPPORTING evidence for the eye, blind to edge defects, never sufficient alone.
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
