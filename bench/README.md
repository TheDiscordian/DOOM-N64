# DOOM-N64 renderer benchmark

Deterministic, fully-scripted A/B benchmark for the renderer work on
`perf/rdp-renderer`, run on the [ares](https://ares-emu.net) emulator (no real
hardware). Each frame is timed in **VR4300 CP0 ticks** (`get_ticks()`), so the
numbers are **host-independent**: the same ROM + same scenario gives the same
µs/frame on any machine, fast or slow ares. Absolute µs are comparable across
runs — A/B by the numbers, not by feel.

---

## TL;DR — use the preset front-end

```bash
bench/bench.sh mesh                       # build + bench one preset
bench/bench.sh software mesh              # A/B, prints a delta table
bench/bench.sh mesh-floors mesh-leaf-rsp  # the Phase 4 floors A/B
bench/bench.sh --list                     # all presets + their flags
```

`bench.sh` maps a preset to the correct flag set and calls `run-bench.sh`
(builds in Docker, runs ares, scrapes the result). With ≥2 presets it prints a
summary table with avg/p95 deltas vs the first (baseline) preset. **This path
can't trip the footguns below — prefer it.**

### Presets

| preset | what it builds |
|---|---|
| `software` | flag-off software renderer (served from `bench/ref-frames-off/` cache unless `BENCH_OFF_CACHE=0`) |
| `rdp` | full RDP renderer, no mesh |
| `planes-only` | SW walls + RDP planes (isolation) |
| `walls-only` | RDP walls + SW planes (isolation) |
| `mesh` | **default mesh build**: RSP-transformed walls + doors on the Z-mesh |
| `mesh-cpu` | mesh walls with the CPU transform (opt out of the RSP offload) — the RSP-vs-CPU A/B |
| `mesh-cull` | mesh's own frustum visibility instead of BSP solidsegs occlusion |
| `mesh-floors` | mesh + baked floor/ceiling leaf fans (CPU leaf transform) |
| `mesh-leaf-rsp` | mesh-floors + the leaf vertex transform on the RSP (Phase 4) |

---

## The flag dependency tree (why the footguns exist)

The Makefile **nests** the flags. A flag set without its parent is silently
ignored — no error, just a build that doesn't do what you think:

```
BENCH=1                         # ALWAYS. Without it the ROM is interactive ->
                                #   boots to the WAD picker, never benches.
BENCH_FORCE_RDP=1               # the RDP renderer. REQUIRED parent of all mesh:
  ├ BENCH_FORCE_PLANES_ONLY=1
  ├ BENCH_FORCE_WALLS_ONLY=1
  └ BENCH_FORCE_MESH=1          # the GPU-port mesh. Parent of:
      ├ BENCH_FORCE_MESH_FLOORS=1
      │   └ BENCH_FORCE_MESH_LEAF_RSP=1   # also needs RSPASFLAGS (wired in Makefile)
      ├ BENCH_FORCE_MESH_CULL=1
      └ BENCH_FORCE_MESH_RSP   (auto = 1; set =0 to opt OUT for a CPU-mesh A/B)
```

`run-bench.sh` now defends both ends: it **always** sets `BENCH=1`, and if
**any** `BENCH_FORCE_MESH*` flag is present it **auto-enables `BENCH_FORCE_RDP=1`**
(printing a note). So even the raw-env path is safe:

```bash
BENCH_FORCE_MESH=1 BENCH_FORCE_MESH_FLOORS=1 bench/run-bench.sh my-label
```

## Footgun symptoms → cause (recognise them fast)

| symptom | cause |
|---|---|
| ares boots to the **WAD selection menu**, `BENCH_ERROR no BENCH_RESULT` | built without `BENCH=1` (e.g. a hand-rolled `docker run … make …` missing it). Use the scripts. |
| two **different** flag sets give **byte-identical** numbers (down to max/min) | the differing flags didn't take effect — almost always a mesh flag without `BENCH_FORCE_RDP=1`, so both built pure software. `md5sum` the ROMs to confirm; they'll match. |
| `BENCH_ERROR ROM crashed` | assert/exception — full backtrace in `/tmp/bench-<label>-ares.log`. |
| `BENCH_ERROR ROM wedged: no warmup-done` | boot hang. Same log. |

Confirm a flag actually compiled in by printing the resolved defines:

```bash
docker run --rm -v "$PWD":/src -w /src -e N64_INST=/n64_toolchain doom-n64:tc \
  bash -lc 'BENCH=1 BENCH_FORCE_RDP=1 BENCH_FORCE_MESH=1 make -p 2>/dev/null \
            | grep "^CFLAGS " | grep -oE "DBENCH_FORCE[A-Z_]*"'
```

---

## Manual / lower-level use (`run-bench.sh`)

`bench.sh` is just a preset map over `run-bench.sh`. Call the latter directly
for a flag combo without a preset, or to bench a prebuilt ROM:

```bash
# build from flags + bench:
BENCH_FORCE_MESH=1 BENCH_FORCE_MESH_CULL=1 bench/run-bench.sh cull-test

# bench an already-built ROM (skips the build) -- it MUST have been built with
# BENCH=1 (+ flags) or it boots to the WAD picker:
ROM=/tmp/some.z64 bench/run-bench.sh some-label
```

Env overrides (both scripts forward them):

| var | effect |
|---|---|
| `TIMEOUT=180` | hard cap (s) on the ares run |
| `BENCH_OFF_CACHE=0` | force a live software run instead of the cached reference |
| `KEEP_ROM=path` | copy the built ROM out (single run) |
| `DOCKER_IMAGE=doom-n64:tc` | build image |
| `ARES=/usr/bin/ares` | emulator binary |

Output is one machine-readable line on stdout:

```
BENCH_RESULT frames=4117 avg_us=16638 p95_us=30176 max_us=172115 min_us=6959 \
             avg_fps=60.1 min_fps=5.8 p95_fps=33.1 label=...
```

The full ares stream (incl. the per-phase `BENCH_PHASE`/`BENCH_TAIL` report) is
preserved at `/tmp/bench-<label>-ares.log` after each run.

---

## What BENCH mode does (mechanism)

`make BENCH=1` (`-DN64_BENCH=1`) compiles a scripted-scenario mode that:

- skips the WAD browser and auto-loads `rom:/DOOM1.WAD`,
- warps to E1M1 and plays a fixed scripted input sequence (no demo lumps, no
  RNG — identical every run; the current scenario runs ~4117 frames, including
  the natural death + single-player respawn on E1M1),
- drives the **shipping uncapped + interpolated** render path (`render_uncapped`
  in `d_main.c`; `demoplayback`/`singletics` are never set, so the
  capped/singletics path is bypassed),
- times each rendered frame in VR4300 ticks via `get_ticks()`, converted to µs
  with `TICKS_TO_US` (`TICKS_PER_SECOND = CPU_FREQUENCY/2`; retail N64 =
  46.875 MHz).

### Result sinks

1. **ISViewer → ares stdout (primary).** `debugf()` writes the `BENCH_RESULT`
   line over the emulated ISViewer; ares prints it to stdout. ares disables
   ISViewer for ROMs > 64 MB; the shareware ROM is ~10 MB.
2. **On-screen frozen overlay (fallback).** At the end, large red FPS digits are
   drawn over the frozen scene every frame. `run-bench.sh` screenshots the ares
   window with `grim` (`bench/last-overlay.png`) if no ISViewer line is caught.

### Visual capture (pictures, not timing)

For *render* A/Bs (verifying a visual change), build with `BENCH_MARKS=1` and use
`bench/scan-marks.sh` — it grabs frozen `BENCH_MARK` frames with `grim`. **Never**
use `BENCH_MARKS` for timing runs (the mark logging skews the numbers).

### Footgun: stale bench object

`make BENCH=1` builds `linuxdoom-1.10/n64_bench.o` into `build/`. The linker
pulls in any `.o` left there, so a **non-BENCH** build in the same tree without
wiping `build/` first can silently link the bench code. The scripts wipe
`build/` fully before each BENCH build; for shipping builds use `rm -rf build`.

### Code layout

Bench code lives in `linuxdoom-1.10/n64_bench.{c,h}` plus minimal `#ifdef
N64_BENCH` hooks: `i_main_n64.c` (skip WAD browser), `d_main.c` (autostart E1M1,
uncapped path, frame timing), `g_game.c` (scripted ticcmd + per-tic hook),
`i_wad_browser_n64.c` (`I_N64ForceSelectedWad`). With `BENCH` unset none of it
compiles and the shipping ROM is unchanged.
