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
#   BENCH_MP=N      build the scripted local split-screen bench (N players)
#   KEEP_ROM=path   copy the built ROM here after the run (e.g. baseline)
#   TIMEOUT=180     hard cap (seconds) on the ares run
#   ARES=/usr/bin/ares
#   DOCKER_IMAGE=doom-n64:tc
#   LOCK_TIMEOUT=1800  max seconds to queue for the build lock

set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL="${1:-bench}"
ARES="${ARES:-/usr/bin/ares}"
DOCKER_IMAGE="${DOCKER_IMAGE:-doom-n64:tc}"
TIMEOUT="${TIMEOUT:-180}"
RUN_ROM="${ROM:-}"
LOCK_TIMEOUT="${LOCK_TIMEOUT:-1800}"

WORKDIR="$(mktemp -d /tmp/doom-bench.XXXXXX)"
ARES_LOG="$WORKDIR/ares.log"
ARES_PGID=""

# --- teardown: kill OUR ares process group only -------------------------------
# Never sweep ares by name: concurrent/queued bench runs (and any ares the user
# has open themselves) must survive this run's cleanup.
cleanup() {
    if [ -n "$ARES_PGID" ]; then
        kill -- "-$ARES_PGID" 2>/dev/null
        sleep 0.3
        kill -9 -- "-$ARES_PGID" 2>/dev/null
        # belt-and-suspenders: ares forks a child; sweep strays, but ONLY ones
        # still in our process group.
        local pid
        for pid in $(pgrep -x ares 2>/dev/null); do
            if [ "$(ps -o pgid= -p "$pid" 2>/dev/null | tr -d ' ')" = "$ARES_PGID" ]; then
                kill -9 "$pid" 2>/dev/null
            fi
        done
    fi
    # Preserve the raw ares output (full ISViewer stream incl. phase report)
    # before discarding the workdir.
    [ -f "$ARES_LOG" ] && cp "$ARES_LOG" "/tmp/bench-${LABEL:-run}-ares.log" 2>/dev/null
    rm -rf "$WORKDIR" 2>/dev/null
}
trap cleanup EXIT INT TERM

fail() { echo "BENCH_ERROR $*" >&2; exit 1; }

# --- build (unless a prebuilt ROM was supplied) ------------------------------
# Only the BUILD is serialized: two builds share build/ and Doom-N64.z64 and
# would corrupt each other. The ares runs themselves may overlap freely -- the
# bench measures emulated CP0 ticks (host load doesn't bias results) and
# teardown/wait are scoped to this run's own process group.
if [ -z "$RUN_ROM" ]; then
    LOCKFILE=/tmp/doom-bench-build.lock
    exec 9>"$LOCKFILE"
    if ! flock -w "$LOCK_TIMEOUT" 9; then
        fail "timed out (${LOCK_TIMEOUT}s) waiting for $LOCKFILE held by another bench build"
    fi
    echo "[bench] build lock acquired ($LOCKFILE)" >&2
    echo "[bench] building BENCH ROM ($DOCKER_IMAGE)" >&2
    # Full wipe of build/: the n64_bench.o object must never cross-contaminate a
    # later non-BENCH build (the linker pulls in any stale .o left on disk).
    docker run --rm -v "$REPO":/doom -w /doom -e N64_INST=/n64_toolchain \
        "$DOCKER_IMAGE" bash -c \
        "rm -rf filesystem build && make BENCH=1 ${BENCH_MP:+BENCH_MP=$BENCH_MP }-j4" \
        >"$WORKDIR/build.log" 2>&1 \
        || { cat "$WORKDIR/build.log" >&2; fail "build failed"; }

    BUILT="$REPO/Doom-N64.z64"
    [ -f "$BUILT" ] || fail "no ROM produced at $BUILT"
    RUN_ROM="$WORKDIR/bench.z64"
    cp "$BUILT" "$RUN_ROM"

    if [ -n "${KEEP_ROM:-}" ]; then
        cp "$BUILT" "$KEEP_ROM" && echo "[bench] kept ROM -> $KEEP_ROM" >&2
    fi

    # ROM is snapshotted into our private workdir; release the build lock so a
    # queued run can build while our ares run proceeds concurrently.
    flock -u 9
fi
[ -f "$RUN_ROM" ] || fail "ROM not found: $RUN_ROM"

# --- guarantee a blank, deterministic save state for this run ----------------
# ares (AutoSaveMemory: true) writes the cart save next to the ROM when its
# Paths/Saves is empty -- which is the case in this repo's config -- so the
# per-run mktemp workdir already gives each run a fresh, blank EEPROM. But a
# DIFFERENT ares config with a NON-EMPTY Saves path keys the save by ROM
# basename: every run uses bench.z64, so one run's save would persist into the
# next and silently drift the A/B scenario (e.g. a saved widescreen/RDP toggle
# re-applied on the next launch). Make the guarantee config-independent: scrub
# any pre-existing save for this ROM, in BOTH locations, before launch.
ROM_BASE="${RUN_ROM%.z64}"          # workdir/bench
ARES_SAVES_DIR="$(awk '/^  Saves$/{getline; if ($1=="Path") {sub(/^  Path /,""); print; }}' \
                  "${HOME}/.local/share/ares/settings.bml" 2>/dev/null | head -1)"
for sav in "${ROM_BASE}".eeprom "${ROM_BASE}".sav "${ROM_BASE}".srm "${ROM_BASE}".flash; do
    rm -f "$sav" 2>/dev/null
done
if [ -n "$ARES_SAVES_DIR" ] && [ -d "$ARES_SAVES_DIR" ]; then
    rm -f "${ARES_SAVES_DIR%/}/bench".eeprom \
          "${ARES_SAVES_DIR%/}/bench".sav \
          "${ARES_SAVES_DIR%/}/bench".srm \
          "${ARES_SAVES_DIR%/}/bench".flash 2>/dev/null
    echo "[bench] cleared stale saves in $ARES_SAVES_DIR" >&2
fi
echo "[bench] save state: blank EEPROM guaranteed (workdir is fresh per run)" >&2

# --- launch ares in its own process group, stdout -> log ---------------------
echo "[bench] launching ares (timeout ${TIMEOUT}s)" >&2
# setsid makes the launched process a new session+group leader, so its PID is
# the new process-group id. Use it directly -- reading pgid back via ps races
# and can return the RUNNER's own group, which would make cleanup kill itself.
# stdbuf -oL: ares stdout is block-buffered through a pipe; line-buffer it so
# the post-result phase report survives the teardown kill.
setsid stdbuf -oL "$ARES" --system "Nintendo 64" "$RUN_ROM" >"$ARES_LOG" 2>&1 &
LAUNCH_PID=$!
ARES_PGID="$LAUNCH_PID"

# --- wait for the BENCH_RESULT line (or hard timeout) ------------------------
RESULT=""
deadline=$(( $(date +%s) + TIMEOUT ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if ! kill -0 -- "-$ARES_PGID" 2>/dev/null; then
        # OUR ares process group is gone (crash or quit) before we saw a result
        break
    fi
    RESULT="$(grep -m1 '^BENCH_RESULT' "$ARES_LOG" 2>/dev/null)"
    if [ -n "$RESULT" ]; then
        # The per-phase/tail report prints after BENCH_RESULT; wait for its
        # terminal BENCH_OUTLIER line (or a short grace) before teardown.
        for _i in $(seq 1 20); do
            grep -q '^BENCH_OUTLIER' "$ARES_LOG" 2>/dev/null && break
            sleep 1
        done
        break
    fi
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
