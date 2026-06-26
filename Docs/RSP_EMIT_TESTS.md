# RSP-emit wall renderer — test registry

Persistent record of which build config produces which VISUAL result, so we
**never re-run a capture we already have an answer for**. Check here before
building a marks ROM. Timing numbers live in the memory file / PAST_BUGS; this
file is purely the rendering-correctness A/B map.

Capture method (all rows): `bench/scan-marks.sh <marks.z64> <outdir>` (grim on
frozen BENCH_MARK frames). Build adds `BENCH=1 BENCH_MARKS=1 BENCH_FORCE_RDP=1`.
Frozen software ground truth (deterministic, **NEVER rebuild**):
`~/.local/share/doom-n64-bench/ref-sw-frozen/frame-<N>.png`.

## Wall-render correctness bisection (warp diagnosis, 2026-06-26)

The question: where does the wall-texture WARP ("diagonal arcing bands", worst
on receding walls, visible while still) come from? Each row swaps ONE stage.

| # | Build flags (+ BENCH/MARKS/RDP) | transform | emit | result | capture |
|---|---|---|---|---|---|
| 1 | `BENCH_FORCE_MESH=1 BENCH_FORCE_MESH_RSP=0` | CPU | CPU `DL_DrawRecord` | **correct** (matches SW) | (prior) `/tmp/cap-mesh` |
| 2 | `BENCH_FORCE_MESH=1` (MESH_RSP default-on) | **RSP overlay A** | CPU `DL_DrawRecord` | **correct** — no arcing | `/tmp/cap-xform` |
| 3 | `BENCH_FORCE_MESH=1 BENCH_FORCE_MESH_RSP_EMIT=1` | RSP overlay A | **RSP overlay B** (`rsp_dlemit.S`) | **WARPED** — diagonal arcing | `/tmp/cap-rsp` |

### Conclusion (decisive)
Rows 2 and 3 feed the **identical** overlay-A `batch_out` (same transform: sx/sy/
invw/depth/S/slopes). The ONLY thing that changes between correct (2) and warped
(3) is the EMIT: CPU `rdpq_triangle`/`DL_DrawRecord` vs overlay B's inlined
`rsp_rdpq_tri.inc` + T-band walk in `rsp/rsp_dlemit.S`.

**=> The warp lives entirely in overlay B (`rsp/rsp_dlemit.S`), NOT in the
transform (`rsp/rsp_dlwall.S`) and NOT in the bake.** Do not re-investigate the
transform for the warp — row 2 proves it is clean.

### ROOT CAUSE + FIX (2026-06-26, RESOLVED)
The triangle engine's perspective normalization (`rsp_rdpq_tri.inc:429-447`) treats
`min(W)` as `1/max(INVW)` -- valid ONLY when `W == 1/INVW` per vertex. Instrumenting
`DL_FlushRSPEmit` with a `W*INVW` (WxIV) column showed almost all walls at ~1.000 but
the **clipped receding walls at 1.1-1.8** -- exactly the arcing ones. Cause: both the
near-plane clip (overlay A) and the screen-edge X-clip carried `W` (depth) as a LINEAR
lerp while `INVW` (= 1/w, correctly linear in screen-x) was lerped separately, so they
drifted apart. A wrong `min(W)` pushes the normalized INVW out of [0,1] -> the per-pixel
texture divide shears into diagonal arcing bands.

Fix (`DL_FlushRSPEmit`): rebuild `W` as the EXACT reciprocal of the final (clipped) INVW
for every emitted wall (`dA = 1/iwl`, `dB = 1/iwr`). No-op for already-consistent walls;
corrects every clipped one. After the fix, WxIV is 0.992-1.000 across the whole demo and
the arcs are gone (full 32-frame A/B vs SW, frames 128-4096, matches software; later
frames re-captured clean after a focus-steal artifact). Perf: +2 float reciprocals per
emitted wall (~tens/frame), negligible.

### Ruled out earlier (do not re-test for the warp)
- Missing horizontal subdivision (forced `nseg>=8` in `DL_RSPBatchProbe`): no change.
- Bilinear filtering (`FILTER_POINT`): no change.
- T-band cap size (halved `DL_TMEM_HALF/pitchb`): band count unchanged, no change.
- Geometry/perspective precision of the transform: row 2 (RSP transform + CPU emit)
  is correct, so transform precision is NOT the warp.
