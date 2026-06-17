#!/usr/bin/env bash
#
# Frozen-marks A/B frame capture: grab every BENCH_MARK frame from a BENCH_MARKS
# ROM via grim, into <outdir>/frame-<N>.png. The marks build freezes ~2s on each
# marker so screenshot timing is forgiving.
#
# *** Why this is a committed script, not an ad-hoc /tmp one ***
# ares teardown here is bulletproof against the leak that hand-rolled capture
# scripts caused: ares is launched under `setsid` (own session) wrapped in
# `timeout -s KILL`, and we trap EXIT *and* INT/TERM/HUP. So ares is reaped:
#   - on normal completion (trap),
#   - on Ctrl-C / SIGTERM / SIGHUP (trap),
#   - and even if THIS script (or the agent running it) is hard-killed before any
#     trap can run -- the setsid'd `timeout` lives on and SIGKILLs ares at the
#     deadline (-s KILL because ares ignores SIGTERM).
# The earlier ad-hoc capture scripts trapped EXIT only and had no timeout, so an
# interrupted capture orphaned the ares window forever (~"half don't close").
#
# Usage: scan-marks.sh <marks-rom> <outdir> [max_frames=40] [cap_seconds=340]
set -u
ROM="${1:?usage: scan-marks.sh <marks-rom> <outdir> [max_frames] [cap_seconds]}"
OUT="${2:?need an output dir}"
MAXF="${3:-40}"
CAPSECS="${4:-340}"
LOG="$OUT/ares.log"
mkdir -p "$OUT"; rm -f "$OUT"/frame-*.png "$LOG"
[ -f "$ROM" ] || { echo "scan-marks: no ROM at $ROM" >&2; exit 2; }
ARES="$(command -v ares)" || { echo "scan-marks: ares not found" >&2; exit 2; }

# Scrub any stale per-ROM save state before launch. ares (AutoSaveMemory) writes the
# cart EEPROM next to the ROM; a flaked launch can poison it so EVERY subsequent
# launch boots straight back into the WAD-selector menu (the "over and over" stall).
# run-bench.sh scrubs for exactly this reason; scan-marks must too.
ROM_BASE="${ROM%.z64}"
rm -f "${ROM_BASE}".eeprom "${ROM_BASE}".sav "${ROM_BASE}".srm "${ROM_BASE}".flash 2>/dev/null
SAVES="$(awk '/^  Saves$/{getline; if($1=="Path"){sub(/^  Path /,"");print}}' \
        "$HOME/.local/share/ares/settings.bml" 2>/dev/null | head -1)"
if [ -n "$SAVES" ] && [ -d "$SAVES" ]; then
    _b="$(basename "$ROM_BASE")"
    rm -f "${SAVES%/}/${_b}".eeprom "${SAVES%/}/${_b}".sav "${SAVES%/}/${_b}".srm "${SAVES%/}/${_b}".flash 2>/dev/null
fi

# Launch ares in its own session under a SIGKILL-proof timeout, and capture its
# REAL process-group id from the session leader's own $$ (written to a file) --
# never from $!, which is wrong when the caller has job control on. cap+30 so the
# trap path wins normally and the timeout only fires if we are already gone.
PGID_FILE="$OUT/.ares.pgid"; rm -f "$PGID_FILE"
setsid bash -c 'echo $$ >"$1"; shift; exec stdbuf -oL "$@"' _ "$PGID_FILE" \
    timeout -s KILL "$(( CAPSECS + 30 ))" "$ARES" \
    --setting DebugServer/Enabled=false --system "Nintendo 64" "$ROM" \
    > "$LOG" 2>&1 &
LAUNCH=$!
for _ in $(seq 1 100); do [ -s "$PGID_FILE" ] && break; sleep 0.02; done
APGID="$(cat "$PGID_FILE" 2>/dev/null)"; [ -n "$APGID" ] || APGID="$LAUNCH"

cleanup() {
    [ -n "${APGID:-}" ] || return 0
    kill -- -"$APGID" 2>/dev/null
    sleep 0.2
    kill -9 -- -"$APGID" 2>/dev/null
}
trap cleanup EXIT INT TERM HUP

# wait for the first marker (boot + warmup); ABORT if the ROM never loads.
# ares has a ~1-in-4 silent launch flake (window opens, ROM never boots). Without
# this, the capture loop below would hold that empty no-ROM window open for the full
# CAPSECS cap -- the "ares window with no rom loaded" that lingers. Bail at the boot
# deadline so trap cleanup reaps it within ~45s instead.
booted=0
for _ in $(seq 1 45); do
    grep -q 'BENCH_MARK' "$LOG" 2>/dev/null && { booted=1; break; }
    kill -0 -- -"$APGID" 2>/dev/null || break   # ares already died during boot
    sleep 1
done
if [ "$booted" -eq 0 ]; then
    echo "scan-marks: no BENCH_MARK within the boot window -- ares launch flaked (no ROM loaded); aborting" >&2
    exit 3   # trap cleanup EXIT reaps the empty ares window
fi

GEOM="$(hyprctl clients -j 2>/dev/null | python3 -c "
import json,sys
best=None
for c in json.load(sys.stdin):
    # Match the ares emulator window by its WINDOW CLASS only ('Ares'), never by
    # title -- the agent terminal driving this script carries 'ares' in its title
    # (the ROM path / 'scan-marks' / 'ares.log' in the running command) and would
    # otherwise be grabbed instead, capturing the terminal rather than the game.
    cl=str(c.get('class','')).lower(); ic=str(c.get('initialClass','')).lower()
    if cl=='ares' or ic=='ares':
        x,y=c['at']; w,h=c['size']
        if w>200 and h>150: best=f'{x},{y} {w}x{h}'
print(best or '')")"
echo "scan-marks: GEOM=[$GEOM]"
[ -z "$GEOM" ] && echo "scan-marks: WARNING no ares window found (will grim full screen)" >&2

end=$(( SECONDS + CAPSECS )); seen=""; n=0; last_new=$SECONDS
STALL="${STALL_SECS:-12}"
while [ "$SECONDS" -lt "$end" ]; do
    # stop early if ares died on its own (ROM ended/crashed) -- no point looping
    kill -0 -- -"$APGID" 2>/dev/null || { echo "scan-marks: ares exited; stopping"; break; }
    fr="$(grep -oE 'BENCH_MARK frame=[0-9]+' "$LOG" 2>/dev/null | tail -1 | grep -oE '[0-9]+$')"
    if [ -n "$fr" ] && [ "$fr" != "$seen" ]; then
        seen="$fr"; last_new="$SECONDS"
        if [ -n "$GEOM" ]; then grim -g "$GEOM" "$OUT/frame-$fr.png" 2>/dev/null
        else grim "$OUT/frame-$fr.png" 2>/dev/null; fi
        n=$(( n + 1 )); echo "scan-marks: captured frame-$fr (n=$n)"
        [ "$n" -ge "$MAXF" ] && break
    elif [ "$n" -gt 0 ] && [ $(( SECONDS - last_new )) -ge "$STALL" ]; then
        # demo stopped producing new markers (ended/looped): the useful capture is
        # done. Exit NOW so cleanup() reaps ares, instead of holding the frozen
        # window open to the full CAPSECS cap -- THAT is what made finished captures
        # look "stuck" long after they were done.
        echo "scan-marks: no new frame for ${STALL}s after $n captured; demo done, stopping"
        break
    fi
    sleep 0.3
done
# cleanup() runs on EXIT
echo "=== scan-marks: $OUT done: $(ls "$OUT"/frame-*.png 2>/dev/null | wc -l) frames ==="
