# Past bugs — diagnoses & resolutions

A log of bugs that cost real time to diagnose, so we don't run the same circles twice.
Each entry: **Symptom**, **Wrong turns** (dead ends already ruled out — do not retry),
**Root cause**, **Repro**, **Resolution** (commit, or OPEN). Newest first.

Renderer context: `perf/rdp-renderer`, the GPU-port mesh renderer (`n64_rdp_mesh`,
`BENCH_FORCE_MESH`). Walls occlude via a painter's-order depth sort (no Z-buffer by
default); floors/sprites are still CPU; the keyed CI8 present blits the software buffer
over the RDP world (i_video_n64.c). 3 hardware framebuffers, 2 CI8 software buffers.

---

## FIXED: HUD/status-bar shimmers after death (post-respawn arms-number flicker)
- **Symptom:** the status bar flickers once the player dies. Ryan: "ONLY after death,
  maybe it just needs to be redrawn after respawn". Localised: the **arms-number digits**
  (the grey "2 3 4 / 5 6 7" grid, x=111-138 y=172-187) shimmer between grey shades at
  present rate. Single-player respawn **reloads the level** (`G_DoReborn`, g_game.c:
  `!netgame` -> `gameaction = ga_loadlevel`), so the trigger is the post-reload melt-WIPE,
  and it appears once play resumes -- NOT in the corpse/dead state.
- **Root cause:** the two CI8 software buffers (i_video_n64.c, `doom_screen8[2]`) are
  ping-ponged each present and the bar is rebuilt per-buffer only when a widget VALUE
  changes (st_lib tracks `oldinum[idx]` per buffer). The arms-number patches have
  transparent edge pixels that show the bar background, which a full `ST_refreshBackground`
  builds via a transient scratch (`screens[BG]`) whose arms recess holds frame-dependent
  content. On a reload the two buffers get their full refresh on DIFFERENT frames (the
  melt-WIPE runs inside one `D_Display`, splitting the refreshes across pre/post-melt and
  different scratch states), so they FREEZE with slightly different greys at the digit
  edges (37 px, ~12-36 grey-level swing). Post-respawn the arms value is static (pistol
  only, no pickups), so no widget ever redraws and it never self-corrects.
- **Wrong turns (do NOT retry):**
  - Forcing BOTH CI8 buffers to `ST_doRefresh` does NOT fix it -- both refresh, but from
    different transient scratch states, so they still differ. PROVEN: a per-buffer refresh
    MASK vs the old `st_n64_refresh_left=2` 2-frame counter gave **byte-identical** 37px
    divergence (A/B, BARDIFF unchanged). A `d_main.c wipe_just_ended` post-wipe re-arm was
    likewise inert. The refresh path is not the lever; the per-buffer DIVERGENCE is.
  - It is NOT a stale-whole-bar swap. BARDIFF (CI8[0] vs CI8[1] over the bottom 32 rows)
    showed only 37px differ, ALL on the arms digits -- the rest of the bar is identical
    between buffers. The "flicker" is the arms digits shimmering, not a gross bar alternation.
  - A forced-death probe that only zeroes health and pins the corpse (never
    `playerstate = PST_REBORN`) tests the WRONG state. The natural E1M1 demo already dies
    (~frame 3120) and reborns (~3212, reload ~3213) -- use it, no forced death needed.
- **Fix:** `ST_doRefresh` now mirrors the just-built bar onto the OTHER CI8 buffer
  (`I_N64SyncRegionToOtherBuffer(ST_Y, ST_HEIGHT)`, i_video_n64.c), so both buffers are
  byte-identical regardless of which transient scratch each was built from; static widgets
  keep them matched. Paired with a per-buffer refresh MASK (st_stuff.c, replacing the
  fragile 2-frame counter) so a full-refresh+sync is GUARANTEED to run on a frame AFTER the
  melt settles -- the counter could spend both refreshes pre-melt, leaving the first
  post-melt frame a diff-draw that re-leaks scratch before any sync (the sync alone, on the
  old counter, regressed back to 37px).
- **Repro/verify:** BARDIFF probe (CI8[0] vs CI8[1], bottom 32 rows) over the post-respawn
  window. Pre-fix = 145 frames of `nd=37` at x=111-138; post-fix = MATCH (the only residual
  is 2 transient single-frame blips in the FACE region x=151-168 -- normal per-buffer
  1-frame animation lag, present in ordinary play, not the bug).
- **Resolution:** FIXED (st_stuff.c refresh mask + ST_doRefresh sync, i_video_n64.c
  `I_N64SyncRegionToOtherBuffer`).

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
