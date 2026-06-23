# CLAUDE.md — DOOM-N64 RDP renderer: lessons & workflow

This file captures hard-won lessons from the `perf/rdp-renderer` effort (CPU game
logic + RDP rasterization) AND the day-to-day navigation/build/bench workflow. Read
the **Build** + **Project map** + **Cheatsheet** sections below before touching
anything; read the renderer/bench/ares sections before changing those subsystems.

> ⚠️ `AGENTS.md` is the STALE upstream doc. Its "Building The Project" section (WSL,
> `README.md`, `DEBUG=1`/`UPLOAD=1`) is **wrong for this fork** — ignore it. Builds run
> in Docker (see below). AGENTS.md is still good for the **hardware notes only** (RDRAM
> bandwidth, FPU denormal-flush, R4300i cache/branch behaviour).

## Build — ALWAYS in Docker, NEVER on the host
**The host has no N64 toolchain.** `libdragon/bin/mips64-elf-gcc` does not exist on the
host; a bare `make` ALWAYS dies with `mips64-elf-gcc: No such file or directory` (error
127). Every build goes through the `doom-n64:tc` image (toolchain baked at
`/n64_toolchain`). The container runs as **root**, so it writes `build/` + `filesystem/`
back as `root:root` — chown them back afterward or the tree goes un-rebuildable.

Prefer a committed wrapper; only hand-roll `docker run` for a marks/standalone build.
- **Release ROMs:** `./build-roms.sh` — the two cart ROMs, ownership-safe (chown-back via EXIT trap).
- **Timing bench:** `bench/run-bench.sh [label]` — builds (Docker) + runs ares + scrapes the result.
  Env flags: `BENCH_FORCE_RDP=1`, `BENCH_FORCE_PLANES_ONLY=1`, `BENCH_FORCE_WALLS_ONLY=1`,
  `BENCH_MARKS=1`, `BENCH_MP=N`, `KEEP_ROM=/path` (keep the ROM), `ROM=/path` (skip build, run existing).
- **Standalone build (e.g. a marks ROM for capture)** — the canonical docker line, then chown back:
  ```bash
  docker run --rm -v "$PWD":/doom -w /doom -e N64_INST=/n64_toolchain doom-n64:tc \
      bash -c "rm -rf filesystem build && make BENCH=1 <FLAGS> -j4"      # ROM -> ./Doom-N64.z64
  docker run --rm -v "$PWD":/doom doom-n64:tc chown -R "$(id -u):$(id -g)" /doom   # un-root the tree
  ```
  `rm -rf filesystem build` is the in-container equivalent of the mandatory `make clean`
  (see Build & bench flags) — a `-D` flag change does NOT trigger a recompile otherwise.

## Project map — where things live
- `linuxdoom-1.10/` — the DOOM engine (this is where 99% of edits go):
  - `d_main.c` — `D_DoomMain`; the `BENCH_FORCE_*` startup block (~L2185-2216) sets the renderer
    gates per build flag. Neither sub-flag ⇒ **full RDP** (walls+planes) since `764b334`.
  - `rdp_view.c` / `.h` — RDP display-list builder. CI4 wall path: `DL_RowMajorBlock`,
    `DL_BuildSubPalette` (median-cut quantizer), `DL_PrequantTexture` (level-load pre-quant).
    Route gates `DL_WallRouteOn()` / `DL_PlaneRouteOn()`.
  - `r_plane.c` — RDP plane (floor+ceiling) path: `R_DrawPlanes`→`R_EmitPlanePolys`→
    `R_EmitIslandRuns`→`R_EmitRunPoly`. `PLANE_UV_TRACE` / `PLANE_GEOM_TRACE` diagnostics.
  - `i_video_n64.c` — N64 video glue, framebuffer, `I_N64ScanReadLump` (font-stomp fix), `I_ReadScreen`.
  - `r_data.c` — texture/flat caching; `R_PrecacheLevel` calls `DL_PrequantTexture`.
  - `m_menu.c` — options menu (framerate selector removed). `n64_bench.c` — bench timers +
    `BENCH_MARK` markers + `N64Bench_FrameNo()`. `g_game.c`/`m_misc.c`/`i_system_n64.c` — timing/defaults.
- `bench/` — tooling: `run-bench.sh` (build+time), `scan-marks.sh` (frozen-marks capture),
  `ares-run.sh` (one-off boot). `ref-frames-off/` = cached software reference frames + log.
- `Docs/` — design docs (PORTING_PLAN, RDP_RENDERER_DESIGN/NOTES, RDP_PRIOR_ART, CI4_WALL_FEASIBILITY).
- `libdragon/`, `tiny3d/` — external libs, **DO NOT modify**. `WADs/` (+`WADs_rom1/2/`) — asset
  staging the build swaps in. `filesystem/` — generated asset stage. `build/` — generated objects.

## Renderer gates (runtime flags)
- `n64_use_rdp_renderer` (master) · `n64_rdp_wall_ab` (RDP walls) · `n64_rdp_plane_ab` (RDP planes).
- `DL_WallRouteOn()` = `use_rdp && wall_ab`; `DL_PlaneRouteOn()` = `use_rdp && plane_ab`.
- Shipped default: RDP **off** (software). `BENCH_FORCE_RDP=1` pins master on; with neither
  sub-flag ⇒ both walls+planes (full RDP); `PLANES_ONLY`/`WALLS_ONLY` pin exactly one.

## Cheatsheet
```bash
# Timing-bench one config (Docker build + ares run + scrape), reuse recorded baselines:
bench/run-bench.sh full-rdp            # software baseline is CACHED — see Build & bench flags
BENCH_FORCE_RDP=1 bench/run-bench.sh full-rdp
BENCH_FORCE_RDP=1 BENCH_FORCE_PLANES_ONLY=1 bench/run-bench.sh planes-only

# Build a frozen-marks ROM for visual A/B, then capture frames (grim on frozen markers):
docker run --rm -v "$PWD":/doom -w /doom -e N64_INST=/n64_toolchain doom-n64:tc \
    bash -c "rm -rf filesystem build && make BENCH=1 BENCH_FORCE_RDP=1 BENCH_MARKS=1 -j4"
cp Doom-N64.z64 /tmp/full-rdp-marks.z64
docker run --rm -v "$PWD":/doom doom-n64:tc chown -R "$(id -u):$(id -g)" /doom
bench/scan-marks.sh /tmp/full-rdp-marks.z64 /tmp/cap/full 40 200   # -> /tmp/cap/full/frame-N.png

# One-off boot / repro (scrubs EEPROM, SIGKILL-proof teardown):
bench/ares-run.sh /tmp/full-rdp-marks.z64 120
```

## Bench demo timeline (E1M1)
The demo dies + respawns on its own. Bench frames (`N64Bench_FrameNo()`): death **3120**
(lt 1594), reborn **3212** (lt 1640), level reload **3213** (lt resets to 0), demo ends
~4117. Single-player respawn = `ga_loadlevel` (a full level RELOAD). Post-respawn play =
frames **3213→end**; post-respawn capture markers: 3328/3456/3584/3712/3840/3968/4096.

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
- Timing builds: **`make clean` FIRST is mandatory** for any BENCH/flag build.
  Incremental `make` does NOT recompile when only a `-D` flag changes, so you get a
  Frankenstein mix of BENCH and non-BENCH objects — link errors (undefined
  `I_N64ForceSelectedWad`) or a silent boot hang before `S_Init`. The hours-long
  "bench won't boot" was exactly this, nothing deeper.
- **The bench is DETERMINISTIC: measure each config ONCE, record the number, reuse
  it. NEVER re-bench software or any config you did not change — it LITERALLY
  returns the same result every time.** Only re-bench the thing you actually
  changed, and A/B it against the recorded baseline.
- Recorded baselines (current ares, clean builds; absolute µs are ares-version-
  dependent so A/B within one session): **software avg 28193µs / p95 49632µs**. The
  tail is plane-dominated (planes p95 20320, worst-frame 31899µs), then seg_rast
  (p95 12960), with audio spiking to ~13000µs on the worst frames. Fingerprint
  (drawsegs=14 visplanes=8 vissprites=4 mean) drift means a real behaviour change.

## Past bugs — read before re-diagnosing
Diagnosed bugs (symptom, dead ends already ruled out, root cause, repro, fix) live in
[`Docs/PAST_BUGS.md`](Docs/PAST_BUGS.md). Check it before chasing a renderer/HUD bug — it
exists so we don't run the same circles twice. (E.g. "HUD flickers after death" is a
post-RESPAWN bug — single-player respawn reloads the level — so a forced-death probe MUST
revive the player; a kill-only corpse probe tests the wrong state.)

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
