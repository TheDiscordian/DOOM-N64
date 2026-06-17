#!/usr/bin/env bash
#
# build-roms.sh -- build the two release ROMs in the doom-n64:tc toolchain
# container, the ownership-safe way.
#
#   DOOM-DOOM2-MasterLevels.z64   (from WADs_rom1)
#   FinalDoom-TNT-Plutonia.z64    (from WADs_rom2)
#
# *** Why this wrapper exists ***
# The doom-n64:tc image runs as ROOT. A bare `docker run ... bash -c "make"`
# that overrides the image CMD writes build/ and filesystem/ back to the host
# as root:root (the image's own trailing `chown` only runs in its baked CMD).
# That left the tree root-owned and un-rebuildable -- a deploy fumble. This
# wrapper ALWAYS restores ownership (even on a mid-build abort, via the EXIT
# trap) and preserves the working WADs/ dir the build swaps out, so a clean
# rebuild is one understood command:  ./build-roms.sh
#
# A `make clean` runs first so the ROMs are a guaranteed-clean (non-BENCH)
# build -- never a relink against stale bench objects from a prior marks/bench
# build.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

IMAGE="${DOOM_TC_IMAGE:-doom-n64:tc}"
UG="$(id -u):$(id -g)"
WADS_BAK="$(mktemp -d)"

restore_state() {
    # 1) never leave the tree root-owned, even on a mid-build failure. The
    #    container runs as root, so a throwaway root container does the chown.
    docker run --rm -v "$PWD":/work "$IMAGE" chown -R "$UG" /work >/dev/null 2>&1 || true
    # 2) restore the working WADs/ the container clobbered (it rm -rf's WADs and
    #    swaps in each rom-set in turn).
    if [ -d "$WADS_BAK/WADs" ]; then
        rm -rf WADs && cp -a "$WADS_BAK/WADs" WADs
    fi
    rm -rf "$WADS_BAK"
}
trap restore_state EXIT

[ -d WADs ] && cp -a WADs "$WADS_BAK/WADs"

docker run --rm -v "$PWD":/work "$IMAGE" bash -c '
    set -e
    export N64_INST=/n64_toolchain
    cd /work
    make clean
    rm -rf WADs && cp -r WADs_rom1 WADs && make -j"$(nproc)"
    mv -f Doom-N64.z64 DOOM-DOOM2-MasterLevels.z64
    echo "BUILT DOOM-DOOM2-MasterLevels.z64 $(stat -c%s DOOM-DOOM2-MasterLevels.z64) bytes"
    rm -rf WADs && cp -r WADs_rom2 WADs && make -j"$(nproc)"
    mv -f Doom-N64.z64 FinalDoom-TNT-Plutonia.z64
    echo "BUILT FinalDoom-TNT-Plutonia.z64 $(stat -c%s FinalDoom-TNT-Plutonia.z64) bytes"
'

echo "=== ROMs built ==="
ls -la DOOM-DOOM2-MasterLevels.z64 FinalDoom-TNT-Plutonia.z64
