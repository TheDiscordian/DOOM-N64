#!/usr/bin/env bash
#
# Run a DOOM-N64 ROM in ares with guaranteed teardown and fail-fast.
# For manual/visual runs (agents: NEVER launch ares directly -- use this).
#
# Usage:
#   bench/run-rom.sh <rom.z64> [label]
#
# Env overrides:
#   TIMEOUT=600          hard cap (seconds) on the run
#   WARMUP_DEADLINE=120  bail if a BENCH ROM never reaches warmup-done
#   WAIT_FOR=regex       success condition in the log (default ^BENCH_RESULT)
#   SCREENSHOT=path.png  grim-screenshot the ares window after WAIT_FOR matches
#                        (e.g. the frozen post-bench overlay frame)
#   FREEZE_GRACE=3       seconds to wait after WAIT_FOR before screenshot
#   ARES=/usr/bin/ares
#
# Behaviour:
#   - launches ares in its own process group, log -> /tmp/rom-<label>-ares.log
#   - fails fast on the crash inspector (ASSERTION FAILED / exception
#     backtrace hit the ISViewer log immediately; the screen freezes forever)
#   - fails fast on a boot wedge (BENCH ROM that never reaches warmup-done)
#   - exits 0 with the matched line on stdout when WAIT_FOR appears
#   - ALWAYS kills its own ares process group on exit -- success, crash,
#     timeout, or signal. Other ares instances are never touched.

set -u

ROM="${1:?usage: run-rom.sh <rom.z64> [label]}"
LABEL="${2:-$(basename "${ROM%.z64}")}"
ARES="${ARES:-/usr/bin/ares}"
TIMEOUT="${TIMEOUT:-600}"
WARMUP_DEADLINE="${WARMUP_DEADLINE:-120}"
WAIT_FOR="${WAIT_FOR:-^BENCH_RESULT}"
SCREENSHOT="${SCREENSHOT:-}"
FREEZE_GRACE="${FREEZE_GRACE:-3}"
LOG="/tmp/rom-${LABEL}-ares.log"

ARES_PGID=""
cleanup() {
    if [ -n "$ARES_PGID" ]; then
        kill -- "-$ARES_PGID" 2>/dev/null
        sleep 0.3
        kill -9 -- "-$ARES_PGID" 2>/dev/null
        # sweep strays, but ONLY ones in our process group
        local pid
        for pid in $(pgrep -x ares 2>/dev/null); do
            if [ "$(ps -o pgid= -p "$pid" 2>/dev/null | tr -d ' ')" = "$ARES_PGID" ]; then
                kill -9 "$pid" 2>/dev/null
            fi
        done
    fi
}
trap cleanup EXIT INT TERM

fail() { echo "ROM_ERROR $*" >&2; exit 1; }

[ -f "$ROM" ] || fail "ROM not found: $ROM"
: >"$LOG"
setsid stdbuf -oL "$ARES" --system "Nintendo 64" "$ROM" >"$LOG" 2>&1 &
ARES_PGID=$!
echo "[run-rom] ares pgid=$ARES_PGID log=$LOG (timeout ${TIMEOUT}s, wait_for=$WAIT_FOR)" >&2

shot() {
    [ -n "$SCREENSHOT" ] || return 0
    command -v grim >/dev/null 2>&1 && command -v hyprctl >/dev/null 2>&1 || return 0
    local geo
    geo="$(hyprctl clients -j 2>/dev/null | python3 -c "
import json,sys,subprocess
pids={int(p) for p in subprocess.run(['pgrep','-g','$ARES_PGID'],capture_output=True,text=True).stdout.split()}
for c in json.load(sys.stdin):
    if c.get('pid') in pids:
        x,y=c['at']; w,h=c['size']; print(f'{x},{y} {w}x{h}'); break
" 2>/dev/null)"
    if [ -n "$geo" ]; then
        grim -g "$geo" "$SCREENSHOT" 2>/dev/null \
            && echo "[run-rom] screenshot -> $SCREENSHOT" >&2
    fi
}

start_ts=$(date +%s)
deadline=$(( start_ts + TIMEOUT ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if ! kill -0 -- "-$ARES_PGID" 2>/dev/null; then
        fail "ares exited before '$WAIT_FOR' matched (log: $LOG)"
    fi
    if grep -qE '^ASSERTION FAILED|<EXCEPTION HANDLER>' "$LOG" 2>/dev/null; then
        sleep 2   # let the full backtrace flush through the line-buffered pipe
        fail "ROM crashed: $(grep -m1 '^ASSERTION FAILED' "$LOG" 2>/dev/null || echo 'CPU exception') (log: $LOG)"
    fi
    if grep -q '^BENCH: init' "$LOG" 2>/dev/null && ! grep -q 'warmup done' "$LOG" 2>/dev/null \
        && [ $(( $(date +%s) - start_ts )) -gt "$WARMUP_DEADLINE" ]; then
        fail "ROM wedged: no warmup-done after ${WARMUP_DEADLINE}s (log: $LOG)"
    fi
    if grep -qE "$WAIT_FOR" "$LOG" 2>/dev/null; then
        sleep "$FREEZE_GRACE"
        shot
        grep -m1 -E "$WAIT_FOR" "$LOG"
        exit 0
    fi
    sleep 1
done
fail "timeout (${TIMEOUT}s) waiting for '$WAIT_FOR' (log: $LOG)"
