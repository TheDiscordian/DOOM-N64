#!/usr/bin/env bash
#
# bench/bench.sh -- preset front-end for the DOOM-N64 renderer benchmark.
#
# Wraps bench/run-bench.sh so you never hand-assemble BENCH_FORCE_* flag combos
# (and never trip the two footguns: forgetting BENCH=1 -> WAD-picker boot, or a
# mesh flag without BENCH_FORCE_RDP=1 -> silent pure-software build). Each PRESET
# expands to a known-good flag set; run-bench.sh builds + benches it in Docker.
#
# Usage:
#   bench/bench.sh <preset> [<preset> ...]      # build + bench each, A/B table
#   bench/bench.sh --list                       # show presets
#
# Examples:
#   bench/bench.sh software mesh                # software vs default mesh build
#   bench/bench.sh mesh-floors mesh-leaf-rsp    # the Phase 4 A/B
#   bench/bench.sh mesh                         # one build, just the number
#
# Env passthrough (forwarded to run-bench.sh): TIMEOUT, ARES, DOCKER_IMAGE,
# BENCH_OFF_CACHE, KEEP_ROM (KEEP_ROM only meaningful with a single preset).
#
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN="$REPO/bench/run-bench.sh"

# --- preset -> flag set ------------------------------------------------------
# Keep these in lockstep with the Makefile's BENCH_FORCE_* tree (bench/README.md
# §flags has the dependency map). "flags" is the env assignment string handed to
# run-bench.sh; run-bench auto-adds BENCH=1 and auto-implies BENCH_FORCE_RDP=1
# for any mesh flag, so presets list only what's meaningful.
preset_flags() {
    case "$1" in
        software|sw)    echo "" ;;                                            # flag-off SW path (served from cache unless BENCH_OFF_CACHE=0)
        rdp)            echo "BENCH_FORCE_RDP=1" ;;                            # full RDP renderer, no mesh
        planes-only)    echo "BENCH_FORCE_RDP=1 BENCH_FORCE_PLANES_ONLY=1" ;; # SW walls + RDP planes
        walls-only)     echo "BENCH_FORCE_RDP=1 BENCH_FORCE_WALLS_ONLY=1" ;;  # RDP walls + SW planes
        mesh)           echo "BENCH_FORCE_MESH=1" ;;                          # default mesh: RSP walls + doors on Z (RSP auto-on)
        mesh-cpu)       echo "BENCH_FORCE_MESH=1 BENCH_FORCE_MESH_RSP=0" ;;   # mesh walls, CPU transform (opt out of RSP offload)
        mesh-cull)      echo "BENCH_FORCE_MESH=1 BENCH_FORCE_MESH_CULL=1" ;;  # mesh's own frustum visibility vs BSP solidsegs
        mesh-worldz)    echo "BENCH_FORCE_MESH=1 BENCH_FORCE_MESH_WORLDZ=1" ;;# Option 3 Phase A: opaque Z-tested world planes
        mesh-pmesh)     echo "BENCH_FORCE_MESH=1 BENCH_FORCE_MESH_PMESH=1" ;; # THE GOAL: welded static plane mesh (no runtime cutting)
        mesh-pmesh-masked) echo "BENCH_FORCE_MESH=1 BENCH_FORCE_MESH_PMESH=1 BENCH_FORCE_MESH_MASKED=1" ;; # + Phase B: midtex on Z (alpha-keyed)
        mesh-floors)    echo "BENCH_FORCE_MESH=1 BENCH_FORCE_MESH_FLOORS=1" ;;# + baked floor/ceiling leaf fans (CPU leaf transform)
        mesh-leaf-rsp)  echo "BENCH_FORCE_MESH=1 BENCH_FORCE_MESH_FLOORS=1 BENCH_FORCE_MESH_LEAF_RSP=1" ;; # + leaf transform on the RSP (Phase 4)
        mesh-rsp-emit)  echo "BENCH_FORCE_MESH=1 BENCH_FORCE_MESH_RSP_EMIT=1" ;; # KEYSTONE: RSP transforms AND emits the wall RDP tris (no batch_out readback)
        *)              return 1 ;;
    esac
}

PRESET_LIST="software rdp planes-only walls-only mesh mesh-cpu mesh-cull mesh-worldz mesh-pmesh mesh-pmesh-masked mesh-floors mesh-leaf-rsp mesh-rsp-emit"

usage() {
    echo "usage: bench/bench.sh <preset> [<preset> ...]   (or --list)" >&2
    echo "presets:" >&2
    for p in $PRESET_LIST; do printf '  %-14s %s\n' "$p" "$(preset_flags "$p" | sed 's/  */ /g')" >&2; done
    echo "  (software expands to no flags = the flag-off software path)" >&2
}

[ $# -eq 0 ] && { usage; exit 2; }
case "$1" in --list|-l|-h|--help) usage; exit 0 ;; esac

# Validate every preset up front so a typo fails before a 10-minute build.
for p in "$@"; do
    preset_flags "$p" >/dev/null || { echo "bench: unknown preset '$p'" >&2; usage; exit 2; }
done

declare -a NAMES RESULTS
rc=0
for p in "$@"; do
    flags="$(preset_flags "$p")"
    echo "========================================================================" >&2
    echo "[bench.sh] preset '$p'  ->  ${flags:-<software, no flags>}" >&2
    echo "========================================================================" >&2
    # Capture stdout (the BENCH_RESULT line); let run-bench's stderr progress
    # flow straight through to ours so the build/run log stays visible live.
    # shellcheck disable=SC2086
    out="$(env $flags "$RUN" "$p")" || rc=1
    line="$(echo "$out" | grep -m1 '^BENCH_RESULT')"
    echo "$out"
    NAMES+=("$p")
    RESULTS+=("${line:-BENCH_RESULT (none)}")
done

# --- comparison table --------------------------------------------------------
echo ""
echo "==================== bench.sh summary ===================="
printf '%-15s %10s %10s %9s %9s\n' "preset" "avg_us" "p95_us" "avg_fps" "p95_fps"
base_avg=""; base_p95=""
for i in "${!NAMES[@]}"; do
    r="${RESULTS[$i]}"
    avg="$(echo "$r"  | grep -oE 'avg_us=[0-9]+'  | cut -d= -f2)"
    p95="$(echo "$r"  | grep -oE 'p95_us=[0-9]+'  | cut -d= -f2)"
    afps="$(echo "$r" | grep -oE 'avg_fps=[0-9.]+'| cut -d= -f2)"
    pfps="$(echo "$r" | grep -oE 'p95_fps=[0-9.]+'| cut -d= -f2)"
    printf '%-15s %10s %10s %9s %9s' "${NAMES[$i]}" "${avg:-?}" "${p95:-?}" "${afps:-?}" "${pfps:-?}"
    if [ -z "$base_avg" ] && [ -n "$avg" ]; then
        base_avg="$avg"; base_p95="$p95"; printf '   (baseline)'
    elif [ -n "$avg" ] && [ -n "$base_avg" ]; then
        da=$(awk "BEGIN{printf \"%+.1f\", ($avg-$base_avg)*100.0/$base_avg}")
        dp=$(awk "BEGIN{printf \"%+.1f\", ($p95-$base_p95)*100.0/$base_p95}")
        printf '   avg %s%%  p95 %s%%' "$da" "$dp"
    fi
    printf '\n'
done
echo "=========================================================="
exit $rc
