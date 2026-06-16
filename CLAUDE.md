# CLAUDE.md — DOOM-N64 RDP renderer: lessons & workflow

`AGENTS.md` holds the upstream project summary, N64 hardware notes, and build basics.
This file captures hard-won lessons from the `perf/rdp-renderer` effort (CPU game
logic + RDP rasterization). Read it before touching the plane/wall renderer or the
bench/capture tooling.

## Renderer layout
- The **RDP plane path** (runtime flag `n64_rdp_plane_ab`) renders floors **and**
  ceilings — both are visplanes through the same code: `R_DrawPlanes` loops every
  visplane → `R_EmitPlanePolys` → `R_EmitIslandRuns` → `R_EmitRunPoly`
  (`linuxdoom-1.10/r_plane.c`). Only the **sky** stays on the CPU (separate column
  path in `R_DrawPlanes`). Call it the "plane" path, not "floor".
- The RDP **wall path** is behind `n64_rdp_wall_ab`. Isolation A/B builds:
  `BENCH_FORCE_PLANES_ONLY` (SW walls + RDP planes), `BENCH_FORCE_WALLS_ONLY`
  (RDP walls + SW planes).

## Debug the renderer with OBJECTIVE TRACES, never by eye
Every plane bug in this effort was pinned by instrumenting emitted-vs-expected
NUMBERS. Visual/montage glances and analytical hypotheses were wrong repeatedly
(the band-split and horizon-crossing guesses were both red herrings); the trace was
right first try. Default to the trace, not your eye, and not a single averaged score.
- `PLANE_UV_TRACE` (r_plane.c, compile-gated): per emitted corner, dumps the texel
  `(u, v, invw)` vs `R_MapPlane`'s exact expected texel at that pixel. For
  texel-scale / perspective bugs.
- `PLANE_GEOM_TRACE`: dumps each emitted poly's screen coverage vs the visplane's
  true `top[]/bottom[]` per column (the poly-vs-visplane row delta). For
  coverage / degenerate-geometry bugs. Key it to `N64Bench_FrameNo()` and set the
  target frame(s).
- A regional-mean metric (floor brightness, a sub-region NCC) HIDES localized
  structural defects — a stray triangle or smear barely moves the mean. Trust the
  per-poly trace and the per-frame eye over an averaged number.

## Run-fitter: split at MAX deviation, not first deviation
`R_EmitIslandRuns` / `R_CountIslandRuns` tessellate a visplane into trapezoid runs.
A curved (non-monotonic) island must be split at the column of WORST edge deviation
(Ramer–Douglas–Peucker) — converges in O(log width) levels. Splitting at the
*first* deviating column peels one narrow sliver per recursion level (O(width)
levels), hits the `depth < 10` recursion cap, and emits the wide remainder as one
straight trapezoid that misses the curved visplane by 10–24 rows = stray/degenerate
polys (the stray red ceiling triangle, the floor smear into nukage, the wall-floor
pull — fixed in `0951cb3`).
- **The emit twin (`R_EmitIslandRuns`) and the count twin (`R_CountIslandRuns`)
  MUST use identical split logic**, or the counted tessellation diverges from what is
  emitted and the bench fingerprint breaks.
- Separate concern: the depth-band split inside `R_EmitRunPoly` (`PLANE_INVW_RATIO`,
  ~4.0) caps each emitted quad's far/near INV_W ratio so the RDP's perspective divide
  stays precise on deep runs. It operates *within* the run-fitter's corners and
  cannot cause or cure run-fitter degeneracy — keep the two issues distinct.

## Build & bench flags
- `BENCH_FORCE_PLANES_ONLY=1` and `BENCH_FORCE_WALLS_ONLY=1` are **nested under
  `BENCH_FORCE_RDP=1`** in both the Makefile and `d_main.c`. Passing them WITHOUT
  `BENCH_FORCE_RDP=1` silently builds an **all-software** ROM (tell: identical md5 to
  the plain build). Always pass both; confirm the ares log prints
  `BENCH: BENCH_FORCE_PLANES_ONLY -> planes RDP`.
- `BENCH_MARKS=1`: frame-keyed capture — prints `BENCH_MARK frame=N` and freezes
  ~2s per marker. With the deterministic virtual-tic clock, **frame N is the
  identical game state on every build**, so frame-N captures are directly A/B
  comparable across builds. NEVER use a BENCH_MARKS build for timing (the marker
  debugf + freeze skew the numbers).
- Timing runs: `bench/run-bench.sh` (no BENCH_MARKS). Software baseline ≈
  **avg 19793µs / p95 31648µs**. Determinism is byte-stable run-to-run, so a
  fingerprint drift (drawsegs/visplanes/etc.) means a real behaviour change.

## Capture & comparison reliability
- **Software (RDP-off) output is deterministic — capture the reference ONCE and
  reuse it.** Canonical frozen software frames live at
  `~/.local/share/doom-n64-bench/ref-sw-frozen/` (32 frames + README). Do NOT
  re-capture software per scan.
- The ares window opens a few px different each launch (HiDPI rounding, e.g. 812 vs
  815 wide), so cross-build frame-N captures do NOT pixel-align. **Register (search a
  small dx/dy shift for min mean-abs-diff) before diffing.** A 1px shift alone tanks
  a raw sub-region NCC.
- A floor sub-region NCC is wall-dominated on edge-on / corridor frames — unreliable
  there. Use NCC only as a candidate filter; confirm each hit with the trace or eye.

## ares lifecycle — MANDATORY (leaked / stuck windows are a hard no)
- ares **ignores SIGTERM**. Teardown must `kill -9` the process GROUP:
  `kill -9 -- -<pgid>` (pgid via `ps -o pgid= -p <pid>`).
- After EVERY ares run, verify ZERO via BOTH `pgrep -x ares` (exact name — `-f`
  matches your own shell) AND a hyprctl window grep for class/title `ares` (pgrep
  alone has missed a live window). Kill any lingering window by its hyprctl pid.
- Run captures FOREGROUND under `timeout`; never background-and-abandon a capture.
  Never touch an ares the user is running themselves.

## Perf reality (strategic verdict)
- Locked 60fps (p95 < 16670µs) is **not reachable incrementally** on this dense
  PC-DOOM geometry. The p95 tail is dominated by `seg_rast` (wall raster) + audio +
  bsp, NOT planes — even a perfect plane/wall offload leaves p95 ~31–38k.
- Both the RDP wall path and the RDP plane path land at **~parity** with the
  already-optimized software renderer at full fidelity (planes-only ≈ 19417 / 31456).
  Realistic goal: correctness + ~parity + shaving the tail, not 60.
- The per-frame visplane→trapezoid tessellation is **NOT** a meaningful tail cost:
  sub-timers measure it at ~2µs, and full-early-returning `R_DrawPlanes` leaves the
  `planes` bracket at ~1644µs with **total frame time unchanged**. **Offline-baking
  the tessellation (DOOM 64's leaf-fans) is a dead lever** — it saves ~2µs and adds
  +57% triangles. The ~1644µs charged to `planes` is an **async RDP/RDRAM stall**
  (the VR4300 blocking on the GPU) attributed to whichever bracket is open; the real
  tail driver is **CPU↔RDP serialization**, not any single CPU phase. (Proven by the
  `doom-three-levers` workflow, 2026-06-16.)

## Profiling caveat — `BPH_*` phase timers are WALL-CLOCK, not CPU counters
The `BPH_*` brackets measure wall-clock between `PhaseSwitch` boundaries, so an
**async RDP/RDRAM stall** (CPU blocked on the GPU) is charged to whichever bracket
happens to be open — NOT to the work that caused it. **Before targeting any phase,
prove it is real CPU work: sub-bracket it AND no-op its body — if the phase time
survives the body no-op (frame time unchanged), it is a stall-attribution artifact,
not optimizable work in that phase.** Two phases proven artifactual this way: `planes`
(~1644µs survives a full `R_DrawPlanes` early-return) and `hud` (a derived
`display_wall − Σphases` residual that absorbs cumulative `get_ticks()` slippage across
the dozens of per-seg `PhaseSwitch` boundaries). The earlier "broad multi-phase CPU
swell" tail read mislabels these — frame time is real, the per-phase attribution is not.
