# Renderer benchmark harness

A deterministic, fully-scripted A/B benchmark for the DOOM-N64 renderer, run on
the [ares](https://ares-emu.net) emulator. No real hardware.

## What it measures

A compile-time `BENCH` mode (`make BENCH=1` -> `-DN64_BENCH=1`) that:

- skips the WAD browser and auto-loads `rom:/DOOM1.WAD`,
- warps to E1M1 and plays a fixed scripted input sequence (forward-walk with
  turns from the spawn -- no demo lumps, no RNG, identical every run),
- drives the **shipping uncapped + interpolated** render path (`render_uncapped`
  in `d_main.c`; `demoplayback`/`singletics` are never set in bench mode, so the
  capped/singletics path is bypassed),
- times each rendered frame in **VR4300 ticks** via `get_ticks()` (the CP0 cycle
  counter), which advances with emulated CPU cycles and is therefore independent
  of host emulation speed. ares running below full speed does **not** bias the
  numbers, and the result is reproducible cycle-for-cycle.

Window: 35*60 = 2100 gametics (60 emulated seconds), after a 1 s warm-up that is
discarded (level load / cache fill). Ticks are converted to microseconds with
`TICKS_TO_US` (keyed on `TICKS_PER_SECOND = CPU_FREQUENCY/2`; retail N64 =
46.875 MHz).

Reported: `frames`, `avg_us`, `p95_us`, `max_us` (worst frame), `min_us` (best),
and the derived `avg_fps`, `min_fps` (from the worst frame), `p95_fps`.

## Result sinks

1. **ISViewer -> ares stdout (primary).** `debugf()` writes the
   `BENCH_RESULT ...` line over the emulated ISViewer, which ares prints to
   stdout when launched from a terminal. The runner greps the captured log.
   ares disables ISViewer for ROMs > 64 MB; the shareware ROM is ~10 MB, well
   under.
2. **On-screen frozen overlay (fallback).** When the run finishes, large red
   digits (AVG/P95/MIN FPS and avg us) are drawn over the frozen scene every
   frame, indefinitely. The runner screenshots the ares window with `grim` if no
   ISViewer line is captured (`bench/last-overlay.png`). The freeze makes
   screenshot timing irrelevant.

## Running it

```bash
bench/run-bench.sh [label]
```

Builds the BENCH ROM (Docker, whatever WADs are in `WADs/` -- only DOOM1.WAD is
needed and it is git-tracked), launches ares in the background, waits for the
`BENCH_RESULT` line (hard cap `TIMEOUT`, default 180 s), and **always** kills
ares on exit (process group + `pgrep -x ares` sweep, via a `trap`). Prints one
machine-readable line:

```
BENCH_RESULT frames=2564 avg_us=19603 p95_us=31072 max_us=44306 min_us=14657 \
             avg_fps=51.0 min_fps=22.5 p95_fps=32.1 label=...
```

### Env overrides

- `ROM=path`   run an existing BENCH `.z64`, skip the build.
- `KEEP_ROM=path`   copy the built ROM out after the run.
- `TIMEOUT=180`   hard cap (seconds) on the ares run.
- `ARES`, `DOCKER_IMAGE`   tool/image paths.

## A/B workflow (orchestrator)

After each optimization merge into `perf/renderer`, from a clean worktree of the
candidate:

```bash
bench/run-bench.sh candidate
```

Compare `avg_us` / `p95_us` / `max_us` against the baseline line below. The
scenario is deterministic, so any change in the numbers is attributable to the
code change (lower us = faster). The baseline ROM is kept at
`bench/bench-baseline.z64`; re-bench it with `ROM=bench/bench-baseline.z64` to
re-confirm the reference at any time.

## Baseline (unmodified perf/renderer)

Measured on this machine, three runs, **byte-identical** each time (zero
run-to-run variance -- emulated tick counts are reproducible):

```
frames=2564 avg_us=19603 p95_us=31072 max_us=44306 min_us=14657
avg_fps=51.0 min_fps=22.5 p95_fps=32.1
```

So the shipping uncapped path on emulated N64-hardware timing averages ~19.6 ms/
frame (~51 fps), with a 95th-percentile frame of ~31 ms (~32 fps) and a worst
frame of ~44 ms (~22.5 fps) over the E1M1 walk.

## Footgun: stale bench object

`make BENCH=1` builds `linuxdoom-1.10/n64_bench.o` into `build/`. The linker
pulls in any `.o` left in `build/`, so a **non-BENCH** build done in the same
tree without wiping `build/` first can silently link the bench code. The runner
wipes `build/` fully before its BENCH build; for shipping builds use
`rm -rf build` (not just `build/doom.dfs`) if a BENCH build preceded it.

## Code layout

All bench code is contained in `linuxdoom-1.10/n64_bench.{c,h}` plus minimal
`#ifdef N64_BENCH` hooks:

- `i_main_n64.c` -- skip the WAD browser, force `rom:/DOOM1.WAD`.
- `d_main.c` -- autostart E1M1 + `N64Bench_Init`; force the uncapped+interpolated
  path; `FrameBegin`/`FrameEnd` around `D_Display`; `DrawOverlay` before present.
- `g_game.c` -- inject the scripted ticcmd at the end of `G_BuildTiccmd`;
  `TicHook` once per gametic in `G_Ticker`.
- `i_wad_browser_n64.c` -- `I_N64ForceSelectedWad` setter.

With `BENCH` unset, none of this is compiled and the shipping ROM is unchanged.
