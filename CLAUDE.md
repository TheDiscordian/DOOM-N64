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
- **NEVER launch ares directly. Use a committed launcher:** `bench/run-bench.sh`
  for timing runs, `bench/scan-marks.sh <marks-rom> <outdir>` for frozen-marks
  frame capture, `bench/ares-run.sh <rom> [seconds]` for any one-off boot/repro
  run. They `setsid` ares into its own session under a `timeout -s KILL`
  backstop and trap EXIT/INT/TERM, so ares is reaped on normal exit, on a signal,
  AND even if the script (or the agent running it) is hard-killed before any trap
  runs — the detached `timeout` still SIGKILLs ares at the deadline.
- **The leak that plagued this effort was hand-rolled `/tmp` capture scripts** that
  ran `setsid ares & ; trap cleanup EXIT` with NO `timeout` backstop: a capture that
  ran to completion closed ares, but one interrupted partway (agent death, abandon,
  force-stop) orphaned the `setsid`'d window forever (~"half don't close"). Do NOT
  write another, and do NOT bare-`ares`: for a one-off launch run `bench/ares-run.sh`
  (it scrubs the per-ROM save first AND has the SIGKILL-proof teardown). A bare `ares`
  skips the scrub — a poisoned EEPROM then boots EVERY later launch straight to the WAD
  selector (reads as a "flake" but is stale-save). And run any launcher SINGLE-
  backgrounded (the harness's run_in_background) — NEVER `nohup … &` inside another
  background, which detaches it untracked and leaks the window.
  (Separately: `setsid ares & ARES_PGID=$!` captures the WRONG pgid when the caller
  has job control ON — `$!` is the dead fork-parent — so a later `kill -- -$!` misses
  ares. Read the pgid from the setsid leader's own `$$`, as the committed scripts do.)
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
- **END-STATE (2026-06-16): planes-only (RDP floors/ceilings + SW walls) is the shipped
  config and BEATS software** — avg ~18908 / p95 ~30624 vs software 19793/31648. A
  full-demo RDP-vs-software sweep flagged ZERO frames on the plane-band MAD metric, but
  that averaged number undersold the palette-flash TINT match — a localized mismatch the
  mean buries (trust the eye, not the averaged number). RESOLVED in `d76a750` via a UNIFORM
  screen-space flash overlay (Ryan's idea): under a flash the planes draw through the
  un-flashed BASE palette, then one translucent tint quad (alpha 5/8) is laid over the
  plane region — uniform like software's palette filter, killing the per-quad
  `TEX0*SHADE`-multiply mismatch; Ryan-accepted ("good enough"). Not pixel-identical (a
  linear blend ≈ the non-linear REDS palette remap) but uniform. The loop-hoist audit's one
  candidate (memoize shared band-corner un-projections) sized BELOW jitter (92% of runs
  single-band, ~7µs) → not landed, confirming the ceiling. Perf-wise it sits at the **CPU ceiling** — every perf lever is measured to a hard
  floor:
  - **Run-fitter** per-column divide strength-reduced to a reciprocal (`f1d7782`, −4% p95).
    `seg_rast`'s remaining setup divide (`dc_iscale = 1/rw_scale`) CANNOT be similarly
    reduced — `rw_scale` varies every column, unlike `f1d7782`'s loop-invariant divisor;
    the rest is incremental DDA and the gather-FILL is the known dead end. `seg_rast` is
    irreducible.
  - **Plane lighting:** per-band colormap (`31fd2f1`) + per-corner gouraud SHADE
    (`b4ac15b`) smooth the depth-light to software's per-row falloff (no banding stairs);
    cost +4-6% p95, NOT recoverable via flat/gouraud adaptivity (the costly quads are the
    multi-light ones that must stay gouraud). The damage/palette flash re-tints the plane
    SHADE LUT (`26886ce` / `DL_RetintPrimLUT`) so floors flash uniformly like software (the
    LUT was baked once at startup + never re-tinted — that was the "blotchy flash" bug).
  - **OFFLINE WALL BAKE = measured NO-GO (FEED-bound — do NOT re-attempt):** RDP walls
    even with geometry baked = 22889/45472, worse than software; the only bakeable work
    (run-fitter ~1977µs) is HALF the irreducible per-frame feed + keyed-present (~4023µs).
    Wall screen quads are 100% view-dependent (rebuilt every frame from the BSP walk +
    per-column clip — and you need that walk for the planes' `top[]/bottom[]` anyway), and
    the static tile/TMEM layout is already cached/amortized.
- The no-op-verified fact stands (planes IS real per-visplane CPU work — a build-verified
  `R_DrawPlanes` no-op dropped p95 29920→24352, scaling ~250µs/visplane), but the
  run-fitter hot op is now optimized and the remaining plane work is needed for the
  de-banding. 60fps stays structurally out; the realized goal — correctness + verified
  fidelity + beating software — is DONE.

## Profiling caveat — `BPH_*` phase timers are WALL-CLOCK, not CPU counters
The `BPH_*` brackets measure wall-clock between `PhaseSwitch` boundaries, so an async
RDP/RDRAM stall (CPU blocked on the GPU) *would* be charged to whichever bracket is open,
NOT to the work that caused it. **Before targeting OR dismissing any phase, prove what it
is: sub-bracket it AND no-op its body in a BUILD-VERIFIED binary — the ROM md5 MUST differ
and you MUST confirm the behaviour actually changed (e.g. the output visibly changes / a
load-time debugf prints). If the phase time then collapses and total frame time drops, it
is real CPU work; if it survives the no-op with frame time unchanged, it is a
stall-attribution artifact.** WORKED CAUTION: an early agent wrongly called `planes` an
async stall because a no-op left the bracket at ~1644µs with frame time unchanged — but
that build had hit the `BENCH_FORCE_PLANES_ONLY`-without-`BENCH_FORCE_RDP` silent-
all-software trap, so the no-op never took effect (always confirm md5 changed first). A
verified no-op proved `planes` is real CPU work (above). Measured async RDP wait in the
ship config is only ~9µs/frame (`RDPWAIT_PROBE`), buffer-flip spin = 0 iterations — there
is **no meaningful CPU↔RDP stall to overlap**; the present seam is already fully
overlapped, and the p95 tail is genuinely CPU-bound (`seg_rast` ~45% software walls, then
audio / hud / bsp_walk / planes).
