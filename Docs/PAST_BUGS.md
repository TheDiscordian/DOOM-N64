# Past bugs — diagnoses & resolutions

A log of bugs that cost real time to diagnose, so we don't run the same circles twice.
Each entry: **Symptom**, **Wrong turns** (dead ends already ruled out — do not retry),
**Root cause**, **Repro**, **Resolution** (commit, or OPEN). Newest first.

## Demo timeline (E1M1 bench demo) — STOP re-deriving this
The standard bench demo (`bench/run-bench.sh`, `BENCH_FORCE_MESH`) DOES die and respawn
on its own. Exact bench FRAME numbers (= `N64Bench_FrameNo()`, the capture-marker space):

| Event | bench frame | leveltime (tic) | playerstate |
|---|---|---|---|
| level start (alive) | 0 | 0 | LIVE, health 100 |
| **player dies** | **3120** | 1594 | DEAD, health 0 |
| **reborn pressed** | **3212** | 1640 | REBORN |
| **level reloads** (single-player respawn = ga_loadlevel) | **3213** | 0 (reset) | LIVE, health 100 |
| demo ends | ~4117 | — | — |

So: pre-death play = frames 0–3120, corpse = 3120–3212, **post-respawn play = 3213→end**.
The "HUD flicker after death" lives in the post-3213 frames. Capture markers fire every
128 frames, so the post-respawn markers are 3328, 3456, 3584, 3712, 3840, 3968, 4096.

Renderer context: `perf/rdp-renderer`, the GPU-port mesh renderer (`n64_rdp_mesh`,
`BENCH_FORCE_MESH`). Walls occlude via a painter's-order depth sort (no Z-buffer by
default); floors/sprites are still CPU; the keyed CI8 present blits the software buffer
over the RDP world (i_video_n64.c). 3 hardware framebuffers, 2 CI8 software buffers.

---

## OPEN: HUD/status-bar flickers after death
- **Symptom:** the status bar flickers badly once the player dies. Ryan: "ONLY after
  death, maybe it just needs to be redrawn after respawn".
- **Wrong turns (do NOT retry):**
  - It is NOT the corpse/dead state. The bug is **after RESPAWN**. Single-player respawn
    **reloads the level** (`G_DoReborn`, g_game.c: `!netgame` -> `gameaction = ga_loadlevel`),
    so the bug state is post-level-reload, after play resumes.
  - A forced-death bench probe that only zeroes health and pins the corpse (never
    `playerstate = PST_REBORN`) tests the WRONG state and **cannot reproduce it**. Built
    this twice; hours wasted. A correct probe MUST revive: kill at tic T, set
    `players[consoleplayer].playerstate = PST_REBORN` ~70 tics later (one-shot — statics
    survive the reload, else infinite death-reload loop), then inspect the POST-RESUME
    frames, not the dead frames.
  - The bar widget layer is sound on its own: `STlib_drawNum` redraws every frame;
    multicon/binicon track per-buffer via `oldinum[idx]`/`I_N64DrawBufferIndex()`; the
    dead face is pinned (no idle-glance). `ST_Start` (called on respawn via
    P_SpawnPlayer->PST_REBORN, p_mobj.c:715) re-creates widgets + sets `st_firsttime`.
- **Suspected root cause:** `ST_Start`'s `st_firsttime` arms a 2-frame ping-pong bar
  refresh (st_stuff.c N64 block: `st_n64_refresh_left = 2`). On a level reload the refresh
  is consumed during the reload/melt-WIPE (only ~1 CI8 buffer drawn before the wipe takes
  over), so once normal drawing resumes the *other* CI8 buffer still holds a stale bar ->
  the bar alternates every present. The fix is to **re-arm the full bar refresh into both
  CI8 buffers AFTER the wipe completes / play resumes**, not during the reload.
- **Repro:** `BENCH_FORCE_DEATH=<tic>` build (p_tick.c kill+revive one-shot) + the
  per-buffer bar-CRC `BARDIFF` probe (i_video_n64.c). Divergence in the POST-reload frames
  (second pass through low `leveltime`) confirms it.
- **Resolution:** OPEN.

## OPEN: mesh walls render geometry that is behind them (usually mid-screen)
- **Symptom (Ryan):** some geometry, usually in the middle, renders what's behind it.
- **Root cause:** the wall painter's-order sort (`DL_MeshDrawWalls`, sort key =
  min-corner depth) is imperfect for walls with crossing/overlapping depth — the classic
  case a Z-buffer exists to solve. Worse at the low corpse viewpoint.
- **Resolution:** OPEN. Proper fix is exact per-pixel occlusion = the Z-buffer (the full
  mesh + Z direction). A no-Z painter's sort cannot be fully correct here.

## FIXED: camera-pan ghost — whole view trails as the camera moves
- **Symptom:** a trailing ghost of the whole view while moving (worst on doors). Hard
  vertical seam mid-turn = stale framebuffer from before the turn.
- **Root cause:** 3 rotating hardware framebuffers, and the mesh path cleared only the
  Z-buffer, never the COLOUR buffer. The view was never fully repainted, so moving
  geometry left its old position holding the 3-frames-ago image.
- **Resolution:** `ace12f2` — per-frame fill of the view rect before the mesh wall flush
  (mesh-only). Verified by live (non-frozen) grab: no-clear shows the seam, clear is
  clean. NOTE: frozen-marks captures CANNOT show this (they converge); use a live grab.

## FIXED: door/mover walls render black or smear
- **Symptom:** walls beside doors go black / smear as the door moves.
- **Root cause:** a moving-sector wall baked into the static mesh ghosts or (mesh-skipped)
  blacks out; texture pegging alone (an earlier mis-fix, 2c60f52) did not address it.
- **Resolution:** `9648d0a` — exclude movable-sector walls from the bake (per-sector
  "movable" flag from special lines); they render via the software path. r_segs.c gates
  the CPU-fill suppression on `bake_line_meshed` so excluded lines keep their fill.

---

## Reference: perf — why incremental RDP pieces each "lose" but the full mesh wins
The Z-buffer measured as a loss only because it was bolted onto the hybrid (CPU BSP walk +
software seg loop + painter's order ALL kept). The Z cost (~2.2ms) is FIXED and paid once;
each one-at-a-time test paid it in full to offload a single thing. Amortised across floors
+ doors + sprites it nets ahead (door re-inclusion alone: seg_rast 2522->1571). And the
CPU mesh transform (~1.3ms walls, isolated) must move to the idle RSP — `rdpbusy` is ~5us,
the CPU is the sole bottleneck. Full mesh = RSP T&L + idle-RDP Z-raster + deleted CPU
occlusion (~8-9ms off a ~20ms frame -> ~60fps). See Docs/RSP_PORT_PLAN.md, GPU_PORT_PLAN.md.
