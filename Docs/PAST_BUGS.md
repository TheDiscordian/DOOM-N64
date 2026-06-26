# Past bugs — diagnoses & resolutions

A log of bugs that cost real time to diagnose, so we don't run the same circles twice.
Each entry: **Symptom**, **Wrong turns** (dead ends already ruled out — do not retry),
**Root cause**, **Repro**, **Resolution** (commit, or OPEN). Newest first.

Renderer context: `perf/rdp-renderer`, the GPU-port mesh renderer (`n64_rdp_mesh`,
`BENCH_FORCE_MESH`). Walls occlude via a painter's-order depth sort (no Z-buffer by
default); floors/sprites are still CPU; the keyed CI8 present blits the software buffer
over the RDP world (i_video_n64.c). 3 hardware framebuffers, 2 CI8 software buffers.

---

## FIXED: RSP-emit up-close MOTION SMEAR / whole-view ghost (world-render gate drops RSP-emit-only frames)
- **Symptom:** under `BENCH_FORCE_MESH_RSP_EMIT`, "up-close it OFTEN SMEARS EVERYTHING too,
  including the gun/smoke" (Ryan) -- MOTION-dependent (only while the player moves; static
  frames fine). Separate from the wall-texture warp.
- **Wrong turns (do NOT retry):** the "stale CI8 sprite/smoke pixels" theory (4 independent
  readers all gave it) is REFUTED -- `I_N64KeyClearView` (i_video_n64.c:1184-1228, called every
  3D frame from r_main.c) clears the FULL view window (which CONTAINS the gun/smoke) to the
  transparency key each frame, so old CI8 sprite pixels inside the view are overwritten; the
  smear is in the RDP world layer, not the CI8 overlay.
- **Root cause:** the I_FinishUpdate world-render gate (i_video_n64.c:1326)
  `if (rdp_on && (DL_Count()+DL_SpanCount()+DL_PolyCount()) > 0)` guards BOTH the per-frame
  16bpp colour-clear (the ace12f2 moving-wall ghost fix, 1368-1373) AND `DL_Flush()` (1395, the
  ONLY caller of `DL_FlushRSPEmit`). RSP-emit walls are tracked by `dl_rspemit_pending`/
  `batch_nvis`, NOT `dl_wall_count` (DL_MeshDrawWalls returns before DL_EmitWallTier in this
  build), so `DL_Count()` reads 0 for them. On an RSP-emit-ONLY frame -- up-close facing a static
  mesh wall with NO door/movable wall (the only thing that bumps DL_Count via r_segs) and NO
  routed plane (DL_SpanCount/PolyCount) in view -- all three counts are 0, the gate is FALSE, and
  the WHOLE world block is skipped: no colour-clear AND no wall draw. The N64 rotates 3 hardware
  framebuffers (display_init(...,3,...)), so `disp` still holds its image from 3 PRESENTS ago;
  the keyed CI8 present blits the (key-cleared, transparent) view over it -> the whole world is
  3 presents stale. Still camera => N-3 ~= N (invisible); MOVING camera => N-3 from a different
  angle => whole-view judder/ghost, worst UP-CLOSE (a near wall fills the view and occludes the
  floor + distant door/movable walls, forcing all counts to 0). The gun/HUD are current (CI8) so
  the stale world shows through their transparent edges = "smears everything incl. the gun".
- **Repro + proof:** static BENCH_MARKS freezes HIDE it (frozen camera => N-3==N). Instrumented
  the gate over the normal demo: **482 of 2304 frames (~21%) hit `rdp_on && base==0 &&
  dl_rspemit_pending>0`** -- the dropped frames (rescued=0 for the opening room frames 0-256
  where floors are visible, then climbing through the corridors). A motion-burst grim capture
  (non-marks ROM, `/tmp/motion-burst.sh`) of the broken region: pre-fix shows stale-frame
  reversions (world matches the 3-presents-ago frame), post-fix shows none.
- **Resolution (`b956aeb`):** add `DL_RSPEmitPending()` (returns `dl_rspemit_pending`, 0 in
  non-RSP-emit builds) and OR it into the gate at i_video_n64.c:1326. Now every RSP-emit frame
  runs the colour-clear (retires the 3-presents-ago fb) and DL_Flush -> DL_FlushRSPEmit (draws
  THIS frame's walls). Flag-OFF / non-RSP-emit builds are byte-identical (the term is 0 there);
  the change only ADDS the clear+draw on the frames that were being dropped.

---

## FIXED: RSP-emit wall-texture WARP (diagonal arcing bands on clipped receding walls)
- **Symptom:** under `BENCH_FORCE_MESH_RSP_EMIT`, wall textures "draw, they just warp a
  lot" (Ryan) — visible while standing still, animating only as the player moves. On a
  receding wall a flat texture sprouts bright **diagonal arcing bands** ("triangles appear
  and pull the texture in weird directions"). Worst on long receding / near-the-camera walls.
- **Bisection (decisive):** built RSP-transform + **CPU-emit** vs RSP-transform + **RSP-emit**
  off the *identical* overlay-A `batch_out`. CPU-emit = correct, RSP-emit = warped → the warp
  is entirely in overlay B's emit (`rsp/rsp_dlemit.S` + `DL_FlushRSPEmit`), NOT the transform.
  (Logged in `Docs/RSP_EMIT_TESTS.md`.)
- **Root cause:** the `rsp_rdpq_tri` engine's perspective normalization (`rsp_rdpq_tri.inc:
  429-447`) uses `min(W)` as a stand-in for `1/max(INVW)` — valid ONLY when `W == 1/INVW`
  per vertex. Both the near-plane clip (overlay A) and the screen-edge X-clip
  (`DL_FlushRSPEmit`) carried **W (depth) as a LINEAR lerp** while INVW (= 1/w, correctly
  linear in screen-x) was lerped separately, so `W·INVW` drifted to **1.1–1.8** on clipped
  receding walls. A wrong `min(W)` pushes the normalized INVW out of [0,1] → the per-pixel
  S/T divide shears → diagonal arcing. Found by adding a `W·INVW` (WxIV) column to a
  `DL_FlushRSPEmit` dump: ~1.000 on all the good walls, 1.1–1.8 on exactly the arcing ones.
- **Wrong turns (do NOT retry for the warp):** forced horizontal subdivision (`nseg≥8`),
  `FILTER_POINT` (bilinear), halved T-band cap, transform precision — all measured, none
  changed the warp. The transform is clean (CPU-emit off the same `batch_out` is correct).
- **Repro:** any `BENCH_FORCE_MESH_RSP_EMIT` marks ROM; the warp is general (most lit
  receding-wall frames). The capture's `bench_frame_count` decouples from game-state in the
  marks build (debugf makes frames outliers, and outliers don't increment the counter), so
  gate diagnostics on wall CONTENT (wide span + invw spread), not the frame number.
- **Resolution (`df09a66`):** in `DL_FlushRSPEmit`, rebuild W as the EXACT reciprocal of the
  final (clipped) INVW for every emitted wall (`dA = 1/iwl`, `dB = 1/iwr`), not a depth lerp.
  No-op for the already-consistent unclipped walls; corrects every clipped one. After: WxIV
  0.992–1.000 demo-wide; full 32-frame A/B vs the frozen SW ref (128–4096) shows the arcs
  gone and the walls tracking software. Perf: +2 float reciprocals per emitted wall, negligible.

---

## FIXED: RSP-emit close-wall BLACK VOID (near-clipped wall → off-screen Y → RDP overflow)
- **Symptom:** under `BENCH_FORCE_MESH_RSP_EMIT`, walls "USUALLY render correctly, but
  sometimes on some angles OR up close weird things start to happen" (Ryan). Up close / in a
  corridor, the big close wall(s) that should fill the view render as a BLACK VOID. Bench
  frames 256 + 384 reproduce it (the demo walks into a corridor).
- **Root cause (TWO layers):**
  1. **The mechanism:** a close wall whose near corner is near-clipped (slid to
     `depth==nearz=4`) projects to a screen Y *far* outside the RDP's 14-bit s11.2 edge-Y
     range (±2047.75); measured `ytA=-4005`. Overlay B's `rsp_rdpq_tri` clip path is STUBBED
     (`RDPQ_Triangle_Clip: jr ra`, CLIPFLAGS forced 0) so it feeds the raw vertex straight to
     the RDP -> the Y edge field overflows -> garbage edge walk -> BLACK. (X is fine: the RDP
     X edges are s15.16 + scissored; only **Y** overflows.) The CPU "mesh" path never hits
     this because its screen-edge X-clip drags the off-screen corner to x∈[0,SCRW-1] and
     re-lerps Y at the clip, which brings Y back in range. B had no such clip.
  2. **Why the committed clip (`a9636d7`) did NOT fix it:** that clip was correct math but ran
     on the CPU over `batch_out` **without `rspq_wait`**. The EMIT pack (`DL_RSPBatchProbe`,
     the `BENCH_FORCE_MESH_RSP_EMIT` branch) is readback-free and deliberately skips the wait
     (only the `#else` branch waits). So the CPU read of `batch_out` got STALE/partial overlay-A
     output and its writeback raced overlay B's RSP-side read -> the clip operated on garbage
     and did nothing. **One-line fix: `rspq_wait()` before the clip reads `batch_out`** (`545fe6a`).
- **Wrong turns (do NOT retry):** saturation split, slope-overflow→frac, "screen-edge X-clip"
  (the clip was RIGHT but read stale memory — this is the trap), Y-clamp ±1536, a multi-agent
  X-clip verdict, a CPU-emit fallback routing extreme walls to `DL_EmitWallTier`. ALL failed
  for the SAME hidden reason: **any CPU read of `batch_out` in `DL_FlushRSPEmit` is stale
  without `rspq_wait`** (the EMIT pack doesn't wait). A coherent dump (rspq_wait + the CPU's
  own projection beside A's) at frame 256 showed overlay A is CORRECT (AsxA=-1917 vs the CPU's
  CsxA=-1912, emit=1) — the bug was always downstream of A.
- **Repro:** `BENCH_MARKS=1` build of `mesh-rsp-emit` + `bench/scan-marks.sh`, view frame 256;
  A/B vs `~/.local/share/doom-n64-bench/ref-sw-frozen/frame-256.png`. The `mesh` preset (pure
  CPU projection) renders 256 correctly — the anchor proving the walls ARE in the render set.
- **Resolution:** `545fe6a`. Perf IMPROVED (the wait is ~free — A is done by `DL_Flush` time):
  software 19860/31840 → mesh-rsp-emit 16255/26720 us avg/p95 (−18.2% / −16.1%, 61.5/37.4 fps).
  Note: the CPU clip + wait re-introduces a `batch_out` readback the keystone removed, but it
  costs nothing measurable here; an overlay-A-side clip (truly readback-free) is a future option.

---

## FIXED: RSP-emit walls render BLACK / no texture / saturate up close
- **Symptom (evolved over the session):** under `BENCH_FORCE_MESH_RSP_EMIT` the GPU wall
  path (overlay A transforms -> overlay B emits the RDP tris, no CPU readback) drew BLACK
  walls -- no texture, no visible geometry. After the emit was fixed, 128-wide textures
  smeared vertically; then walls were full-bright; then long walls "went fucky up close OR
  on some angles". (~22 builds were burned on the BLACK stage before this session.)
- **Root cause -- the BLACK walls were THREE stacked bugs, none of them the binding:**
  1. Overlay B was dispatched from the render-view pack (`DL_RSPBatchProbe`, called in
     `R_RenderPlayerView`) -- BEFORE `I_FinishUpdate` does `rdpq_attach` + the ghost-fix
     view-fill + the world textured-mode setup. B's tris hit no attached fb with no CI4
     tile/TLUT. Fix: dispatch B from `DL_Flush` (`DL_FlushRSPEmit`), after attach/mode/bind.
  2. `rsp_dlemit.S` vertex layout did NOT match the `rsp_rdpq_tri` engine: it interleaved
     phantom `CLIPPOSi/CLIPPOSf` words, pushing `Wi/Wf/INVWi/INVWf` to 0x16/0x1E/0x20/0x22.
     The engine reads them at 0x10/0x12/0x14/0x16 -> it read W/INV_W from garbage ->
     degenerate perspective -> tris never rasterised. Fix: correct the offsets (compact
     24-byte engine vertex, no CLIPPOS).
  3. `StageVtx` wrote a 16-bit zero over `CLIPFLAGS`(0x06)+`REJFLAGS`(0x07). The engine
     CULLS unless `(rej1|rej2|rej3)&0x3F == 0x3F` (rsp_rdpq_tri.inc:277) -- negated
     trivial-reject. rejflags=0 -> EVERY tri trivially-rejected. Fix: write `0x00FF`.
- **Wrong turns (do NOT retry) -- the ~22 BLACK-stage builds:** master-TLUT re-assert
  timing/sync, moving the bind into `DL_Flush`, TEX_FLAT mode setup, `rdpq_sync_pipe`
  between bind and B, per-run vs per-wall dispatch, `rspq_wait` full isolation, band
  `src_lo` clamp, T-period reduction. ALL of these chase the TEXTURE BIND -- but the tris
  were never rasterising at all (bugs 2+3), so no bind could ever show. **The diagnostic
  that cracked it:** force a bright flat PRIM + Z off and dump `batch_out` -> geometry was
  VALID (emit=1, sane sx/sy) yet nothing drew => the failure is the engine vertex contract
  (offsets + reject flags), NOT the bind. When valid geometry doesn't rasterise, check the
  `rsp_rdpq_tri` VTX_ATTR offsets and the REJFLAGS before touching texturing.
- **The later issues (each its own fix):** 128-wide smear = no T-banding (load 1 of 4
  bands + clamp) -> RSP T-band walk; full-bright = PRIM can't vary within one B dispatch
  -> per-wall `TEX0*SHADE` via vertex RGBA + tricmd 0x0B00->0x0F00; "fucky up close OR on
  some angles" = S-span saturation (StageVtx packs S as s10.5, saturates >1024 texels; a
  diag proved E1M1's longest visible wall is 832 texels, which + the base bias crosses
  1024 on 256-wide textures) -> port the CPU's documented S-span run-split (blkw-aware).
- **Repro:** `bench/bench.sh mesh-rsp-emit` (perf), or a `BENCH_MARKS=1` build +
  `bench/scan-marks.sh` for frozen frames; A/B vs the canonical frozen software at
  `~/.local/share/doom-n64-bench/ref-sw-frozen/` (NEVER re-run software). Note: the
  close-wall saturation is viewpoint-dependent and the static demo does not always hit it.
- **Resolution:** `3230e77` (the 3 BLACK-wall fixes), `6108bcd` (T-banding), `ac413f8`
  (lighting), `7226518`+`4b61395` (S-span split, blkw-aware), `29cd5b9` (empty-band
  elision). Result: textured/lit/correct, 16616/28384 us avg/p95 vs software 19860/31840
  (-16.3%/-10.8%). Viewpoint-dependent close-wall still pending Ryan's interactive drive.

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

## FIXED: mesh walls render geometry that is behind them (usually mid-screen)
- **Symptom (Ryan):** some geometry, usually in the middle, renders what's behind it.
- **Root cause:** `DL_MeshDrawWalls` painted walls in painter's order sorted by the
  NEAREST corner depth (`min(dA,dB)`). That key is only an approximation: two walls whose
  depth ranges overlap mis-order in the columns where the far-by-nearest-corner wall is
  actually in front, so it gets overwritten and the geometry behind it shows through.
  Worse at the low corpse viewpoint. The Z-buffer infrastructure already existed
  (`dl_zbuf`, allocated + attached + `rdpq_clear_z` every mesh frame, i_video_n64.c) but
  `dl_wall_z` gated wall z-test on `n64_rdp_mesh_floors` too -- on the wrong premise that
  "walls alone occlude correctly via the painter's sort" (rdp_view.c comment). The user's
  report disproves that premise.
- **Fix:** `dl_wall_z = n64_rdp_mesh && dl_zbuf_attached` (rdp_view.c) -- enable per-pixel
  wall z-test/write whenever the z-image is attached, independent of the floors flag. The
  z-image is already cleared every mesh frame, so only the per-pixel z cost is added: avg
  17513->17901us (+2.2%), p95 29664->30816us (+3.9%) on the E1M1 mesh bench.
- **Verify limitation:** the artifact is viewpoint-dependent and the frozen BENCH_MARK
  frames (every 128) do not land on a viewpoint that exhibits it prominently, so a clean
  static A/B can't show the specific before/after (the diff at markers is dominated by
  capture present-timing + sub-pixel edge sampling). What WAS confirmed from the captures:
  the wall-z build introduces no regression / z-fighting across the inspected frames. The
  fix is mechanically the correct one for the described class (per-pixel z-test prevents a
  far wall overwriting a near one). Final visual confirmation is the interactive/normal run.
- **Resolution:** FIXED (rdp_view.c `dl_wall_z` decoupled from the floors flag).

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
