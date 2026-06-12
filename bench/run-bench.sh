#!/usr/bin/env bash
#
# Deterministic A/B renderer benchmark for DOOM-N64 on the ares emulator.
#
# Builds the BENCH ROM (make BENCH=1, shareware DOOM1.WAD, Docker), launches it
# in ares, runs a fixed scripted scenario through the shipping uncapped+
# interpolated render path, scrapes the result, and ALWAYS tears ares down.
#
# Frame cost is measured inside the ROM in VR4300 ticks (get_ticks(), the CP0
# cycle counter) and reported as emulated-hardware us/frame and FPS -- this is
# independent of host emulation speed, so an ares that can't hit full speed does
# not bias the numbers.
#
# Result sink (primary): libdragon debugf() -> emulated ISViewer -> ares stdout,
# captured to a log and grepped for the BENCH_RESULT line. ares disables ISViewer
# for ROMs > 64 MB; the shareware ROM is ~10 MB so this is fine.
#
# Output: one machine-readable line on stdout:
#   BENCH_RESULT frames=N avg_us=.. p95_us=.. max_us=.. min_us=.. \
#                avg_fps=.. min_fps=.. p95_fps=..
#
# Usage:
#   bench/run-bench.sh [label]
# Env overrides:
#   ROM=path        skip the build, run an existing BENCH .z64
#   KEEP_ROM=path   copy the built ROM here after the run (e.g. baseline)
#   TIMEOUT=180     hard cap (seconds) on the ares run
#   ARES=/usr/bin/ares
#   DOCKER_IMAGE=doom-n64:tc

set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL="${1:-bench}"
ARES="${ARES:-/usr/bin/ares}"
DOCKER_IMAGE="${DOCKER_IMAGE:-doom-n64:tc}"
TIMEOUT="${TIMEOUT:-180}"
RUN_ROM="${ROM:-}"

WORKDIR="$(mktemp -d /tmp/doom-bench.XXXXXX)"
ARES_LOG="$WORKDIR/ares.log"
ARES_PGID=""

# --- teardown: kill the ares process group AND sweep any stray ares ----------
cleanup() {
    if [ -n "$ARES_PGID" ]; then
        kill -- "-$ARES_PGID" 2>/dev/null
        sleep 0.3
        kill -9 -- "-$ARES_PGID" 2>/dev/null
    fi
    # belt-and-suspenders: ares forks a child, $! is only the launcher.
    local pids
    pids="$(pgrep -x ares 2>/dev/null)"
    if [ -n "$pids" ]; then
        # shellcheck disable=SC2086
        kill -9 $pids 2>/dev/null
    fi
    rm -rf "$WORKDIR" 2>/dev/null
}
trap cleanup EXIT INT TERM

fail() { echo "BENCH_ERROR $*" >&2; exit 1; }

# --- build (unless a prebuilt ROM was supplied) ------------------------------
if [ -z "$RUN_ROM" ]; then
    echo "[bench] building BENCH ROM ($DOCKER_IMAGE)" >&2
    # Full wipe of build/: the n64_bench.o object must never cross-contaminate a
    # later non-BENCH build (the linker pulls in any stale .o left on disk).
    docker run --rm -v "$REPO":/doom -w /doom -e N64_INST=/n64_toolchain \
        "$DOCKER_IMAGE" bash -c \
        "rm -rf filesystem build && make BENCH=1 -j4" \
        >"$WORKDIR/build.log" 2>&1 \
        || { cat "$WORKDIR/build.log" >&2; fail "build failed"; }

    BUILT="$REPO/Doom-N64.z64"
    [ -f "$BUILT" ] || fail "no ROM produced at $BUILT"
    RUN_ROM="$WORKDIR/bench.z64"
    cp "$BUILT" "$RUN_ROM"

    if [ -n "${KEEP_ROM:-}" ]; then
        cp "$BUILT" "$KEEP_ROM" && echo "[bench] kept ROM -> $KEEP_ROM" >&2
    fi
fi
[ -f "$RUN_ROM" ] || fail "ROM not found: $RUN_ROM"

# --- launch ares in its own process group, stdout -> log ---------------------
echo "[bench] launching ares (timeout ${TIMEOUT}s)" >&2
# setsid makes the launched process a new session+group leader, so its PID is
# the new process-group id. Use it directly -- reading pgid back via ps races
# and can return the RUNNER's own group, which would make cleanup kill itself.
setsid "$ARES" --system "Nintendo 64" "$RUN_ROM" >"$ARES_LOG" 2>&1 &
LAUNCH_PID=$!
ARES_PGID="$LAUNCH_PID"

# --- wait for the BENCH_RESULT line (or hard timeout) ------------------------
RESULT=""
deadline=$(( $(date +%s) + TIMEOUT ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if ! pgrep -x ares >/dev/null 2>&1; then
        # ares exited on its own (crash or quit) before we saw a result
        break
    fi
    RESULT="$(grep -m1 '^BENCH_RESULT' "$ARES_LOG" 2>/dev/null)"
    [ -n "$RESULT" ] && break
    sleep 1
done

# --- screenshot fallback (the overlay is frozen, so timing is forgiving) ------
if [ -z "$RESULT" ] && command -v grim >/dev/null 2>&1 && command -v hyprctl >/dev/null 2>&1; then
    GEO="$(hyprctl clients -j 2>/dev/null | python3 -c '
import json,sys
for c in json.load(sys.stdin):
    if "ares" in c.get("class","").lower():
        x,y=c["at"]; w,h=c["size"]; print(f"{x},{y} {w}x{h}"); break
' 2>/dev/null)"
    if [ -n "$GEO" ]; then
        grim -g "$GEO" "$WORKDIR/overlay.png" 2>/dev/null \
            && cp "$WORKDIR/overlay.png" "$REPO/bench/last-overlay.png" \
            && echo "[bench] no ISViewer result; saved overlay screenshot -> bench/last-overlay.png" >&2
    fi
fi

# cleanup() runs on EXIT and always kills ares

if [ -n "$RESULT" ]; then
    echo "$RESULT label=$LABEL"
    exit 0
fi

fail "no BENCH_RESULT captured (see screenshot fallback bench/last-overlay.png)"
