#!/usr/bin/env bash
#
# ares-run.sh -- leak-safe, scrub-first one-off ares launcher.
#
# Use this for ANY ad-hoc ROM run (boot test, repro, manual bench) instead of a
# bare `ares ...`. A bare launch has two failure modes this wraps:
#   1. It skips the per-ROM save scrub. ares (AutoSaveMemory) writes the cart
#      EEPROM next to the ROM; a flaked/interrupted launch poisons it so EVERY
#      later launch boots straight to the WAD-selector menu (looks like a hang).
#   2. It has no SIGKILL-proof teardown, so an interrupted launch orphans the
#      window forever ("stray ares windows").
#
# This scrubs the saves first, then runs ares under `setsid` + `timeout -s KILL`
# with a trap, so the window is reaped on normal exit, on a signal, AND even if
# this script (or the agent running it) is hard-killed before any trap runs.
#
# RUN IT SINGLE-BACKGROUNDED (the harness's run_in_background), NEVER `nohup ... &`
# inside another background -- double-backgrounding detaches it untracked and is
# exactly what leaks windows.
#
# Usage: ares-run.sh <rom> [seconds=120] [logfile=/tmp/ares-run.log]
set -u
ROM="${1:?usage: ares-run.sh <rom> [seconds] [logfile]}"
SECS="${2:-120}"
LOG="${3:-/tmp/ares-run.log}"
[ -f "$ROM" ] || { echo "ares-run: no ROM at $ROM" >&2; exit 2; }
ARES="$(command -v ares)" || { echo "ares-run: ares not found" >&2; exit 2; }

# Scrub stale per-ROM saves next to the ROM AND in ares' configured Saves dir.
base="${ROM%.z64}"
rm -f "$base".eeprom "$base".sav "$base".srm "$base".pak "$base".flash 2>/dev/null
SAVES="$(awk '/^  Saves$/{getline; if($1=="Path"){sub(/^  Path /,"");print}}' \
        "$HOME/.local/share/ares/settings.bml" 2>/dev/null | head -1)"
if [ -n "$SAVES" ] && [ -d "$SAVES" ]; then
    b="$(basename "$base")"
    rm -f "${SAVES%/}/$b".eeprom "${SAVES%/}/$b".sav "${SAVES%/}/$b".srm \
          "${SAVES%/}/$b".pak "${SAVES%/}/$b".flash 2>/dev/null
fi

# Launch in its own session under a SIGKILL-proof timeout; capture the REAL pgid
# from the session leader's own $$ (not $!, which is the dead fork-parent when
# the caller has job control on).
PGID_FILE="$(mktemp)"
setsid bash -c 'echo $$ >"$1"; shift; exec stdbuf -oL "$@"' _ "$PGID_FILE" \
    timeout -s KILL "$SECS" "$ARES" \
    --setting DebugServer/Enabled=false --system "Nintendo 64" "$ROM" \
    > "$LOG" 2>&1 &
LAUNCH=$!
for _ in $(seq 1 100); do [ -s "$PGID_FILE" ] && break; sleep 0.02; done
APGID="$(cat "$PGID_FILE" 2>/dev/null)"; [ -n "$APGID" ] || APGID="$LAUNCH"
rm -f "$PGID_FILE"

cleanup() {
    [ -n "${APGID:-}" ] || return 0
    kill -- -"$APGID" 2>/dev/null
    sleep 0.2
    kill -9 -- -"$APGID" 2>/dev/null
}
trap cleanup EXIT INT TERM HUP

wait "$LAUNCH" 2>/dev/null
# cleanup runs on EXIT -- the window is gone no matter how we got here.
echo "=== ares-run: done ($ROM, cap ${SECS}s); log: $LOG ==="
