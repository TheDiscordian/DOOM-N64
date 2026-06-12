# RDP Renderer — Final Design 🦴

The shippable architecture for a fully RDP-rasterized DOOM-N64 renderer targeting 60 FPS
(16.67 ms/frame) on the combat bench. Companion to `RDP_RENDERER_NOTES.md` (the verified
factual ground). Repo: `/home/discordian/Programming/DOOM-N64` · branch `perf/rdp-renderer`.

**Constraints (non-negotiable, carried verbatim):** visual fidelity stays recognisably DOOM
(lighting falloff, palette damage/pickup flashes, masked mid-textures, sprite-vs-wall clipping);
the uncapped/interpolated path and 2–4p split-screen keep working; the deterministic bench
(`make BENCH=1`) stays meaningful; `libdragon/` and `tiny3d/` are never modified; the new
renderer sits behind a runtime toggle so the software path stays selectable.

---

## 0. Decision summary

**Winning lens: bandwidth-first** ("RDP-Direct Renderer", candidate `bandwidth`). The N64's
250 MB/s RDRAM bus is the binding constraint, and a typical combat frame moves only ~280–400 KiB
(≤10% of the 4.17 MB/frame budget) once you (a) keep CI8 textures + the master TLUT end-to-end,
(b) refuse a z-buffer because BSP front-to-back ordering already sorts geometry (a depth buffer
would add ~250 KiB/frame of read-modify-write traffic for zero benefit), and (c) batch RDP draws
by texture so each tile loads RDRAM→TMEM ~once per frame. It won on the perf lens (score 9 — the
only design whose fill arithmetic uses the **correct** RDP mode per target) and the feasibility
lens (score 9 — the most debuggable single-seg-first staging), and it is *not* flagged as
likely-broken on fidelity (score 7 — "less faithful lighting than incremental, but not broken";
its TLUT-resident flashes are *more* faithful than the ceiling design's blend-rect).

The `incremental` design scored higher on pure fidelity (9) but its load-bearing perf claim —
rendering the 3D view into the 8bpp `screens[0]` surface **in COPY mode** for a "4× / 215 µs"
fill — is the hardware-crash path: `rdpq_set_mode_copy` "only works with 16-bpp framebuffers …
will trigger a hardware crash (!)" (`libdragon/include/rdpq_mode.h:335-336`) and is restricted to
texture-rectangles, not the triangles/affine-X spans the design needs (`:325`). Resolving that
arithmetic against the header voids its central advantage, so it cannot win. The `ceiling` design
is architecturally the same correct shape as `bandwidth` but the fidelity judge scored it 6 and
flagged its `TEX0*PRIM` lighting **and** its full-view blend-rect flashes + arithmetic-negate
invuln as likely-divergent approximations — disqualifying under "a design the fidelity judge flags
as likely-broken cannot win" for those specific mechanisms, so we take its sound parts and reject
its flash/invuln mechanism.

**Grafted in (cross-compatible ideas the judges recommended):**

1. **From `incremental` — CI8 `screens[0]` containment for the *non-3D* layers only.** All ~70
   `V_DrawPatch`/AM/ST/menu/wipe/finale drawers keep writing 8-bit indices into a CI8 overlay
   surface byte-for-byte unchanged. This is the single largest reduction in fidelity-regression
   surface and is fully compatible with rendering the *3D view* into the 16bpp fb. (We do **not**
   adopt its render-into-CI8-via-COPY path — that is the crash path.)
2. **From `incremental` — keep damage/pickup palette flashes in the master TLUT (free).** Because
   the world is CI8 sampling the same 256-entry TLUT, the existing `I_SetPalette` swap tints the
   whole rendered view for free, with zero extra per-frame traffic. We reject `ceiling`'s
   full-view blend-rect for the common flashes (it re-touches ~54 KiB when active and is a new
   effect path that must be tuned to match) and keep the blend-rect **only** for invuln/inverse,
   where a TLUT swap also works but a combiner path is the documented escape hatch.
3. **From `ceiling`/`bandwidth` (kept) — perspective-correct walls via free INV_W.** `scale =
   FixedDiv(projection, z)` (verified `r_main.c:495`), so `INV_W ∝ scale1/scale2` already lives in
   every drawseg; perspective texturing matches DOOM's per-column look with no warping and zero
   extra CPU cost.
4. **From `incremental` — per-seg A/B toggle for the wall path** (RDP trapezoid vs CPU column) so
   the free-W proportionality constant can be pixel-diffed on a near wall before trusting the whole
   scene. De-risks Q1, "the single biggest unknown".
5. **From `ceiling` — per-texture format choice by colour histogram** (CI8 for >16-colour textures,
   CI4 otherwise) to bound CI4 banding on gradient flats (NUKAGE-type).
6. **From `ceiling` — INV_W trapezoid subdivision** as the documented fallback if the
   proportionality constant can't be tuned to sub-pixel accuracy.
7. **From `incremental`/`ceiling`/`bandwidth` (consensus) — translated (player-colour) columns
   stay on the CPU overlay path initially.** `R_DrawTranslatedColumn` remaps per-pixel and does not
   map to a simple textured primitive. **Fuzz/spectre does NOT stay on CPU** — `R_DrawFuzzColumn`
   reads its *own framebuffer output* (`dest[fuzzoffset[pos]]`, `r_draw.c:391`), and under the
   transparent-key overlay model the view-window CI8 holds key-index pixels, not world colours, so a
   CPU fuzz read returns garbage. Fuzz moves to an **RDP blender pass** that reads the world the RDP
   just drew into the 16bpp fb (`MEMORY_RGB`); see Q8 and the staging plan.
8. **From `bandwidth` (kept, hardened) — the explicit per-frame RDRAM byte-budget table** as the
   standing accounting artifact, extended with the two costs all three under-tracked: the
   on-first-touch column-major→row-major transpose + CI4/colormap-convert CPU cost (lands in
   `BSP_WALK`), and the worst-case tail-frame per-texture TMEM upload count × bytes.

**One-paragraph architecture statement.** DOOM's "software renderer" is two fused stages: a cheap
CPU *geometry/visibility* stage (BSP walk, 1-D solidseg occlusion, scale-from-angle, sprite sort,
silhouette clipping) and an expensive CPU *rasterization* stage (the ~68%-of-frame per-column /
per-span byte fill into uncached RDRAM). We keep stage 1 verbatim and replace stage 2 with a
per-frame, **texture-sorted display list** of screen-space primitives that the RDP rasterizes
**into the 16bpp display framebuffer** in standard 1-cycle mode: walls as vertical-edged
perspective-correct quads (INV_W from `scale`, free), planes and sprites as affine textured
rects/quads (constant-z by construction), all in CI8/CI4 sampling the master TLUT, lit per-drawseg
by a quantized `PRIM`-colour multiply that reproduces DOOM's already-stepped colormap falloff,
with damage/pickup flashes staying free in the TLUT. The ~70 non-3D drawers keep writing CI8 into a
separate overlay surface that is **composited via a transparent-key alpha-compare blit** (one
reserved palette index keyed out so the RDP-drawn world shows through the view window) on top each
present. No z-buffer (BSP already orders), no T3D/RSP transform (geometry is already projected), no
per-byte CPU fill — the CPU races into the next frame's BSP walk behind the existing non-blocking
`rdpq_detach_cb` fence while the RDP drains.

---

## 1. Pipeline architecture

### CPU stages (per `R_RenderPlayerView`; visibility unchanged, fill replaced by emit)

```
TryRunTics (sim)                       d_main.c:946   [GAMETIC]  unchanged
R_SetupFrame (interp camera)           r_main.c:936   unchanged (fractionaltic lerp kept)
DL_BeginFrame()                                       NEW: reset emit arena + per-texture buckets
── for each player (split loop d_main.c:686-728) ──
  R_RenderBSPNode                      r_main.c:1001  [BSP_WALK]  KEPT verbatim (visibility/sim)
    R_StoreWallRange  r_segs.c:433     KEPT clip/scale math
      R_RenderSegLoop r_segs.c:206:
        - KEEP per-column visplane top[]/bottom[] writes (r_segs.c:284-285,302-303) — planes need them
        - KEEP per-column ceilingclip/floorclip + silhouette bookkeeping
        - REPLACE the l_colfunc() pixel writes (r_segs.c:333,355,…) with DL_EmitWallTier():
            one wall_cmd_t per (tier × contiguous-clip-run [× light-level run]) into the per-texture
            bucket (x1/x2; topfrac/bottomfrac→screen Y at both edges; scale1/scale2→INV_W;
             texturecolumn→S; dc_texturemid+iscale→T; rw_scale>>LIGHTSCALESHIFT→light index).
            The optional per-light-level-run split (break the seg at columns where the light index
            changes) preserves vanilla's per-column light banding; without it the sub-seg gets one
            PRIM light level — see Q8 "per-column → per-seg light" divergence
  R_DrawPlanes                         r_main.c:1014  [PLANE_EMIT]
    R_MakeSpans / R_MapPlane r_plane.c:200  KEEP affine span construction (ds_xfrac/ystep)
      - REPLACE spanfunc() with DL_EmitSpan(): one flat-textured rect per run into per-flat bucket
      - sky visplanes flagged → fullbright (colormaps[0], PRIM=white)
  R_DrawMasked                         r_main.c:1024  [MASKED_EMIT]
    KEEP O(n²) sort (r_things.c:838) + R_DrawSprite silhouette scan (r_things.c:898)
      - REPLACE R_DrawMaskedColumn/R_DrawVisSprite inner loops with DL_EmitSprite():
        per-sprite clipped textured quad(s), coalesced from clipbot[]/cliptop[] into
        vertical scissor sub-rects, ordered back-to-front; transparency = TLUT-key alpha-compare via
        `rdpq_mode_alphacompare(threshold)` (`rdpq_mode.h:710`) in 1-cycle STANDARD mode (NOT the
        COPY-mode `transparency=true` flag — that applies only to the present blit; sprites draw in
        standard mode, so they need the explicit alphacompare call against the key entry's alpha=0)
      - fuzz/spectre: RDP blender pass (MEM-blend darken, Q8); translated columns: CPU overlay (graft #7)
── end player loop ──
DL_Flush()                             [DL_BUILD]  NEW: walk buckets in texture order →
  per texture: rdpq_tex_upload(tile) once, then all its rdpq_triangle/rect cmds, then next texture
HU/ST/AM/menu/border overlays          [HUD]  CPU, write CI8 into the OVERLAY surface (unchanged drawers)
I_FinishUpdate                         d_main.c:808  [PRESENT]  RDP blits overlay over the view + detach_cb
S_UpdateSounds + I_SubmitSound         d_main.c:1003,1024  [AUDIO]  unchanged
```

### RDP stages (drain order — painter, no z-buffer)

The RDP attaches the **16bpp display fb**, scissored to the 3D view window, in
`rdpq_set_mode_standard()` + `rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT)` +
`rdpq_mode_tlut(TLUT_RGBA16)` + `rdpq_mode_persp(true)`, then draws in DOOM's exact order:

1. **Sky** visplanes — textured, fullbright (`colormaps[0]`, PRIM=white).
2. **Flats** (floor/ceiling) — one CI4 tile per flat, all spans of that flat before evicting.
3. **Walls** — one CI8 tile per wall texture, all tiers/segs using it before switching. Front-to-
   back within a texture is fine: solidseg clip guarantees each screen column is covered ~once per
   tier, so no overdraw inside a texture batch.
4. **Masked mid-textures + sprites** — last, alpha-compare via TLUT key, clipped to the CPU's
   silhouette arrays, drawn strictly back-to-front.
5. **Invuln/inverse tint** (when `fixedcolormap` is the inverse map) — single full-view blend rect.
6. **Overlay blit** — `rdpq_set_mode_copy(true)` (valid: the *display fb is 16bpp*; **`true`
   enables transparent-key alpha-compare** — see below) + `rdpq_mode_tlut(TLUT_RGBA16)` +
   `rdpq_tex_blit(&overlay,0,0,NULL)`, then `rdpq_detach_cb(I_N64BufferDone, idx)` — the existing
   non-blocking fence. The overlay's view-window pixels carry the reserved key index (alpha=0 in the
   TLUT), so the alpha-compare in COPY mode discards them and the world the RDP drew in steps 1–5
   shows through; every other overlay pixel (HUD/menu/border) overwrites the fb as today.

**Transparent-key overlay model (the compositing mechanism).** The RDP world pass (steps 1–5) draws
**into the 16bpp fb before** the overlay blit (step 6) in the *same* rspq stream. One palette index
is reserved as the **transparency key**: its TLUT entry is written with **alpha=0** (every other
entry keeps alpha=1, `i_video_n64.c:815-818`), so the present blit's alpha-compare keys it out. Any
view-window pixel the CPU drawers leave as the key index becomes transparent and reveals the world
underneath; the same key index doubles as the sprite/masked-texture transparent-gap index. This
model is established **before any world geometry moves to the RDP** (a dedicated early stage,
software-only) so the split is de-risked while the view is still 100% software-rendered. See §5 and
the staging plan.

Because BSP + solidseg already eliminate hidden walls/planes (each visible screen column written
~once per tier) and sprites are drawn back-to-front after the opaque world, painter ordering needs
no depth test; overdraw stays ~1.0–1.6× (sprites are the only source).

### Frame timeline (CPU / RDP overlap)

```
 |<--------------------------- 16.67 ms frame budget --------------------------->|
 Frame N CPU: [GAMETIC 1.0][BSP_WALK+emit ~2.5][PLANE/MASK emit ~1.5][DL_BUILD ~1.5][overlay HUD ~1.1][AUDIO 2.3]  ≈ 9–10 ms
 RDP:                                                              (idle)  [drain N: sky→flats→walls→sprites ~1.2][overlay blit 0.26][present]  ≈ 1.5 ms
                                                                                                                  └ rdpq_detach_cb (non-blocking)
 Frame N+1 CPU: [GAMETIC][BSP_WALK N+1 …]  ← runs concurrently with RDP draining N
                  spin on doom_screen8_rdp_busy[next] only if RDP fell >1 frame behind (≈ never)
```

The RDP busy window (~1.5 ms typical, ~1.9 ms tail) is far smaller than the CPU residual (~9–10
ms), so the RDP always finishes inside the same frame and the CPU never stalls on the buffer-flip
busy-spin (`i_video_n64.c:783`).

**Two distinct buffer pools — do not conflate them (Q4):** (1) the **16bpp display fb is
triple-buffered** by libdragon (`display_init(...,3,...)`, `i_video_n64.c:845`); `display_get`
hands out the next free fb and `display_show` (fired by `detach_cb`) queues it — this is the world
target and the one frame of *display* slack lives here. (2) The **CI8 ping-pong is only 2 buffers**
(`N64_CI8_BUFFERS 2`, `i_video_n64.c:22`; `doom_screen8[2]`) guarded by the `doom_screen8_rdp_busy`
flag (`:783`) — this is the overlay/source pool the present blit reads. The "one frame of slack"
belongs to pool (1); the busy-spin stall point is pool (2). They are independent, so the slack of
the 3-deep display fb does **not** by itself make the 2-deep overlay coherent — see the overlay
buffering rule in §5. The uncapped/interpolated path is untouched (it
just calls `D_Display` more often; each present is now cheaper). Split-screen: each pane is one
`rdpq_set_scissor` sub-rect; the per-frame `memset` + byte-poke dividers (`i_video_n64.c:491-525`)
become RDP fill rects.

---

## 2. Data structures and key code touchpoints

All new code lives in `linuxdoom-1.10/` (e.g. a new `rdp_view.c`/`rdp_view.h`); `libdragon/` and
`tiny3d/` are never touched.

### New data structures

```c
// One emitted wall tier (top/bottom/mid). Vertical screen edges ⇒ quad = 2 triangles.
typedef struct {
    int16_t  x1, x2;                 // screen column span (inclusive); vertical edges
    float    ytop_l, ybot_l;         // top/bottom screen Y at left edge  (from topfrac/bottomfrac)
    float    ytop_r, ybot_r;         // at right edge
    float    s_l, s_r;               // texture S at each edge (texturecolumn, r_segs.c:312)
    float    invw_l, invw_r;         // = scale1*k, scale2*k  (FREE W, r_main.c:495)
    float    t_top;                  // T origin (dc_texturemid mapping)
    uint16_t texid;                  // bucket key (texnum)
    uint8_t  light;                  // 0..15 quantised PRIM index (rw_scale>>LIGHTSCALESHIFT).
                                     // One value per emitted sub-seg ⇒ vanilla's PER-COLUMN light
                                     // step is flattened to per-sub-seg unless the emit splits per
                                     // light-level run (Q8 divergence + mitigation).
    uint8_t  tier;                   // top/bottom/mid + sky flag
} rdp_wall_t;

// Flat span run (R_MakeSpans already yields horizontal runs).
typedef struct {
    int16_t  y, x1, x2;
    float    u0, v0, ustep, vstep;   // ds_xfrac/yfrac/xstep/ystep (r_plane.c:218-220)
    uint16_t flatid;
    uint8_t  light;                  // distance>>LIGHTZSHIFT (r_plane.c:226)
} rdp_span_t;

// Sprite/masked billboard. Per-column clip → coalesced vertical scissor sub-rects.
typedef struct {
    int16_t  x1, x2;
    float    ytop, ybot, s0, sstep;
    uint16_t patchid;
    uint8_t  light;                  // vis->colormap level
    int8_t   mode;                   // opaque / masked / (CPU-fallback: fuzz/translated)
} rdp_sprite_t;

// Per-texture bucket: index range into the frame arena (sparse, only touched ones flushed).
typedef struct { int head, tail, count; } tex_bucket_t;
tex_bucket_t wall_buckets[NUMTEXTURES];   // sized at level init
tex_bucket_t flat_buckets[NUMFLATS];
// Arena: bump-allocated rdp_wall_t/rdp_span_t/rdp_sprite_t, reset each frame. Append-only
// sequential writes (cache-friendly); only DL_Flush reads non-sequentially, in texture order,
// touching ≤ a few hundred ~48-byte records — inside the 8 KB L1 working set per batch.
```

### Integration sites (file:line of every touchpoint)

| Site | File:line | Change |
|---|---|---|
| Bench enum | `n64_bench.h:33-43` | Split `BPH_BSP` → `BSP_WALK`/`SEG_RASTER`; add `PLANE_EMIT`, `MASKED_EMIT`, `DL_BUILD`, `RDP_BUSY`, `KEY_CLEAR` |
| Bench brackets | `n64_bench.c:219-246` | `PhaseSwitch`/`PhaseBegin` for the new phases; non-serializing RDP-done timestamp read |
| Wall fill | `r_segs.c:333,355,…` (the `l_colfunc()` sites in `R_RenderSegLoop`) | Replace each with `DL_EmitWallTier()`; KEEP all clip/visplane-marking math (`r_segs.c:262-432`) |
| Wall scale/S/T inputs | `r_segs.c:312` (texturecolumn), `:315` (light index), `:322` (iscale) | Read as emit inputs, not fill inputs |
| Free-W constant | `r_main.c:495` (`scale=FixedDiv(projection,z)`), `r_main.c:776` (`projection=basefocal`) | Derive `k` once in `R_SetupFrame` from `projection`/`centerxfrac` |
| Plane fill | `r_plane.c` `R_MapPlane` → `spanfunc()` site | Replace with `DL_EmitSpan()`; KEEP `R_MakeSpans` + `ds_*` setup (`r_plane.c:200-235`) |
| Flat format | `r_draw.c:716-735` (64×64 row-major) | Source for CI4 down-convert (on-demand cache) |
| Sprite fill | `r_things.c:355-389` (`R_DrawMaskedColumn`), `R_DrawVisSprite` | Replace inner loop with `DL_EmitSprite()`; KEEP post structure semantics for the alpha-key |
| Silhouette clip | `r_things.c:898-1006` (`R_DrawSprite`, `clipbot[]`/`cliptop[]`) | KEEP; coalesce to scissor sub-rects at emit |
| Drawseg endpoints | `r_defs.h:322-347` (`scale1/scale2/scalestep`, silhouette arrays) | Consumed by wall emit + sprite clip; unchanged |
| Wall transpose/convert | `r_data.c:228` (`R_GenerateComposite`), `r_data.c:383` (`R_GetColumn`), `r_data.c:743` (`R_PrecacheLevel`) | Extend to lazily produce + cache row-major CI8 tiles in PU_CACHE zone |
| Present seam | `i_video_n64.c:728-794` (`I_FinishUpdate`) | Insert view-render BEFORE the overlay blit (same rspq stream); flip the blit to `rdpq_set_mode_copy(true)` for transparent-key alpha-compare; reuse `detach_cb`/busy-flag verbatim. **The view-render + flush MUST sit AFTER the menu-pacing early-return (`:741-746`), not before it** — see "Menu present-pacing seam" below |
| Menu present-pacing seam | `i_video_n64.c:741-746` (`menuactive && GS_LEVEL`, <16 ms paced early-return) | **The early-return skips `rdpq_attach` entirely**, so the world emit must NOT be drained before it (a paced menu frame would reset the emit arena, never draw the world, and the rotated display buffer would show a stale/garbage 3D view — a pause/options menu over a level is the common case). Fix: the world emit/flush is part of the present (after the early-return), so a paced frame simply does not re-present — but because the world now lives only in the transient display list, the display buffer it last drew into must be **re-presented unchanged**, not re-rendered. Concretely: on the paced path, skip emit AND keep `display_show`-ing the last completed display buffer (do not rotate). The emit arena is reset at `DL_BeginFrame` only on frames that actually reach the present, so a paced frame leaves the previous frame's drained buffer intact on screen |
| Palette/flashes + key | `i_video_n64.c:805-823` (`I_SetPalette`), `:758-765` (TLUT upload) | Per-frame flash swaps UNCHANGED (free via TLUT swap). **NEW:** the per-entry pack at `:815-818` forces `alpha=0` for the reserved key index in EVERY uploaded TLUT variant, so damage/pickup swaps preserve the key |
| Transparency-key reserve | startup, after PLAYPAL load (key-scan helper near `I_SetPalette`) | Scan UI/status-bar/font/menu patch lumps drawn outside the 3D view, pick a palette index none of them use; assert if none free |
| View-window key-clear | `I_FinishUpdate` / view-render entry (when `n64_use_rdp_renderer` ON) | Batched 64-bit key-fill of the 3D-view region of the CI8 surface (~54 KB). **TEMPORARY SCAFFOLDING** — removed in the final stage when view-window CI8 writes become event-driven erase-to-key |
| Split render loop | `d_main.c:686-728` | Per-pane `rdpq_set_scissor(viewrect)`; KEEP loop structure |
| Split dividers | `i_video_n64.c:482,491-525` | Replace `memset`/byte-poke with RDP fill rects (or keep on overlay) |
| Renderer toggle | `r_main.c` (renderer dispatch) + `m_menu.c` (option) | `n64_use_rdp_renderer` flag selects RDP vs software `colfunc`/`spanfunc` path |

---

## 3. The 10 open questions — final answers

**Q1 — Affine vs perspective texturing → PERSPECTIVE for walls (free W), AFFINE for planes/sprites.**
DOOM's `R_ScaleFromGlobalAngle` documents `scale = FixedDiv(projection, z)` (`r_main.c:495`), so
`rw_scale` (and the drawseg `scale1`/`scale2` endpoints, `r_defs.h:328-329`) is exactly
`projection/z` ∝ 1/W. We emit per-vertex `INV_W = scale * k` (the constant `k = projection`
folds into the tile S/T scale) plus `S = texturecolumn` (`r_segs.c:312`) and `T =
dc_texturemid+(y-centery)*dc_iscale`, then `rdpq_mode_persp(true)` + `TRIFMT_TEX`. The RDP's
hyperbolic S/W,T/W ÷ (1/W) interpolation reproduces DOOM's exact per-column
`finetangent*rw_distance` hyperbola at column centres and stays correct between them — **matches
the vanilla look, no near-wall warping, zero extra CPU cost.** Planes and sprites stay affine:
floor/ceiling spans are constant-z by construction (that is why DOOM uses spans) and sprites are
billboards at constant z, so affine is *exact* for them. *Rationale:* affine-only walls were
rejected because near walls would visibly warp and break fidelity; the free-W path is the only one
that is both faithful and free. *De-risk (graft #4/#6):* ship behind a per-seg A/B toggle
(RDP-trapezoid vs CPU-column) to pixel-diff `k` on a near wall before trusting the scene, with
trapezoid subdivision as the fallback if sub-pixel accuracy can't be tuned.

**Q2 — Z-buffer vs painter ordering → NO z-buffer.** Preserve the CPU BSP front-to-back walk +
solidseg 1-D clip (`r_bsp.c:80-92,381-487`) + sprite silhouettes (`r_things.c:898`). A z-buffer
costs 128 KB resident and ~250 KiB/frame of read-modify-write traffic on the bus that *is* the
bottleneck, and buys nothing because BSP already produces correctly-ordered, mutually
non-overlapping geometry per screen column. Draw order: sky → flats → walls (per-texture batches)
→ masked/sprites (back-to-front, TLUT-key alpha). *Rationale:* this is the single biggest correct
bandwidth call; it quantifiably avoids the +250 KiB the depth buffer would add. T3D defaults
z-buffer *on* (`t3d.c:170`) — another reason to bypass T3D, not adopt it.

**Q3 — Overlay-blit vs native-RDP UI → CI8 transparent-key overlay-blit (graft #1).** Render the 3D
view into the 16bpp fb; keep a **separate uncached CI8 overlay surface** that all ~70 `V_DrawPatch`/
AM/ST/menu/finale/intermission sites (§6 of the notes) write into byte-for-byte unchanged, composited
on top each present in **COPY mode with transparent-key alpha-compare** (`rdpq_set_mode_copy(true)`).
**Verified COPY-mode alpha-compare:** `rdpq_set_mode_copy(bool transparency)` — when `true`, "pixels
with alpha set to 0 can optionally be discarded during blit, so that the target buffer contents is
not overwritten for those pixels. This is implemented using alpha compare" (`rdpq_mode.h:328-330`),
and COPY mode "only works with 16-bpp framebuffers" (`rdpq_mode.h:335`). The present target **is** the
16bpp display fb, so alpha-compare in COPY mode is valid here — **no 1-cycle fallback needed**, the
~256 µs / 192 KB COPY cost line stands. The reserved key index has alpha=0 in the TLUT, so its
view-window pixels are discarded and the RDP world shows through; all other overlay pixels overwrite.
Cost: 320×200 CI8 read (64 KB) + RGBA5551 write (128 KB) = 192 KB ≈ 4.6% of the bus budget and
~0.26 ms COPY fill — affordable. The overlay is `data_cache_hit_writeback`'d before the RDP reads it
(graft, mirroring `i_video_n64.c:763`). *Rationale:* lowest rewrite risk and lowest fidelity surface;
native-RDP UI (`rdpq_font`, `i_wad_browser_n64.c` model) rewrites ~12 sites and is **not** on the
critical path to 60 FPS — deferred to an optional later stage if the blit ever hurts (it doesn't, per
§6 budget).

**Q4 — Status-bar diff-draw → DROP it; full-redraw the status bar onto the overlay each frame.**
The diff-draw is hard-coupled to exactly 2 ping-pong buffers + per-buffer `oldval[idx]` widget
state + the `st_n64_refresh_left=2` trick (`st_stuff.c:1123`) — fragile, and it was a hack to avoid
redrawing into *uncached* RDRAM. A full status-bar redraw is 320×32 = 10 KiB of CPU writes into
the overlay surface — negligible. *Rationale:* dropping it decouples the renderer from the
2-buffer assumption entirely, de-risking the overlay surface's buffer count, and full-redraw
*costs less* CPU here than maintaining diff state.

**Overlay buffering / coherency (specified, not assumed independent).** The overlay is **re-drawn
in full every frame** (no diff state, no per-buffer widget state) and **re-blitted onto the current
display fb every present** (step 6), so HUD staleness across the triple-buffered display fb is a
non-issue — each of the 3 display buffers receives a freshly-composited overlay the frame it is
shown; there is no "draw once into buffer N, show stale in N+1/N+2" hazard. What the overlay *does*
need is its own **double buffering matched to the present pipeline**: the present is non-blocking
(`detach_cb`, the RDP reads the overlay *after* `I_FinishUpdate` returns), so the CPU must not
overwrite the overlay the RDP is still reading. The overlay therefore **reuses the existing 2-deep
CI8 ping-pong** (`doom_screen8[2]` + `doom_screen8_rdp_busy`, `i_video_n64.c:22,783`) — the same
pool, same busy-flag interlock that already gates the present source today; the CPU draws frame N+1's
overlay into the *other* CI8 buffer while the RDP reads frame N's. So the overlay is **not** a third
independent surface with an unspecified count: it **is** the CI8 pool (2 buffers), and the
triple-buffered *display* fb (pool 1) is a separate concern (§1 timeline). This keeps the overlay
coherent without per-buffer redraw tricks and without growing the buffer count.

**Q5 — RSP role → SKIP transform/T&L; use only the stock rdpq triangle-setup ucode.** The CPU
feeds `rdpq_triangle`/`rdpq_texture_rectangle` directly with already-projected screen-space XY +
S/T/INV_W; the RSP runs only the shared `rspq_triangle.inc` setup ucode that those calls already
invoke, plus rspq command dispatch. Do **not** add custom RSP code, do **not** use T3D transform
(it re-does the CPU's already-solved projection and competes for the shared RSP cycle budget; DMEM
is at ~100%, `modelOpt.md:108`). *Optional later:* record per-frame-static sub-sequences (view
border, split dividers, sky) as `rspq_block` for zero-CPU replay — a libdragon block, not custom
ucode, deferred.

**Q6 — Texture batching / TMEM residency → BATCH BY TEXTURE id.** During the BSP/plane/masked
passes, bucket every emitted primitive by `texnum`/`flatnum` into sparse `tex_bucket_t` arrays.
`DL_Flush` walks one texture at a time: one `rdpq_tex_upload`, then all its primitives, then move
on — so each tile loads RDRAM→TMEM ~once per frame (the `modelOpt.md:252-281` insight: ~90% command
/ ~45% bandwidth cut). This collapses autosync TMEM/pipe thrash from per-seg to per-texture (combat
scene: ~7 wall textures + ~5 flats + sprites = ~15–20 uploads, not hundreds). *Residency:* TMEM
lower 2 KB holds exactly one tile at a time (TLUT owns the upper 2 KB), so "residency" is just "the
current batch"; the full RDRAM texture cache uses DOOM's existing `PU_CACHE` zone-memory LRU
(`R_GetColumn`/`W_CacheLumpNum`), which already manages the working set under the 2–4 MB zone — no
new eviction policy needed. *Rationale:* autosync, not bandwidth, is the real 60-FPS gate (notes
§3); batching by texture is the direct mitigation and is measured via `RDP_BUSY`.

**Q7 — CI8 vs CI4 vs RGBA16 → walls/sprites = CI8, flats = CI4 (graft #5 for overrides).** CI8 +
the master TLUT preserves DOOM's exact 8-bit look, the 8-bit RDRAM footprint, and reuses the
already-uploaded 256-entry palette. A 64×32 CI8 wall tile = 2 KB fits the lower TMEM half beside the
TLUT — **at exactly 2048 bytes, zero margin** (`rdpq_tex.c:188` caps CI/RGBA32/YUV tiles to 2048;
`tmem_pitch = ROUND_UP(64,8) = 64`, ×32 rows = 2048). This is a **hard boundary, not a comfortable
fit**: any CI8 tile wider than 64 texels exceeds 2048 at 32 rows. DOOM textures are 64-wide so this
holds, but the 64-wide tile width is load-bearing — a wider tile would have to drop to 24 rows or
split. Tall 64×128 walls draw as stacked 64×32 tiles (the seg quad is split/scissored vertically with
a 1-texel overlap + T-clamp at the seam). **Flats specifically use CI4** (64×64 = 2 KB, fits beside
the 2 KB TLUT) — a 64×64 CI8 flat is 4 KB and does *not* fit; CI4 also halves the flat cache, and
flats are low-frequency mostly-flat-shaded surfaces where a 16-entry sub-palette is visually safe.
RGBA16 is used **nowhere** (doubles both TMEM and RDRAM for no fidelity gain on paletted art).
*Graft #5:* the per-flat (and per-wall, if ever forced to CI4) format is chosen from that lump's
actual distinct-colour count at convert time — CI8-tile the >16-colour ones, CI4 the rest — to
avoid banding on gradient flats (NUKAGE). *Rationale:* CI8-walls (vs the ceiling design's
CI4-walls) keeps the wall look bit-exact and removes the CI4 banding risk on the dominant surface.

**Q8 — Lighting → per-drawseg PRIM-colour shade-multiply; flashes stay in the TLUT (graft #2).**
Use `RDPQ_COMBINER_TEX_FLAT` (`TEX0*PRIM`, verified `rdpq_macros.h:531`, free in 1-cycle) with a
16-entry PRIM-brightness LUT built once at level/light init from the colormap darkening ramp.
**LUT reference index (Stage-2 fix round, load-bearing):** the ramp is sampled by feeding a
**pure-white** PLAYPAL index through each colormap level — `colormaps[level*256 + 4]` (PLAYPAL
index 4 = (255,255,255)) — and reading the mapped entry's RGB back through PLAYPAL. PLAYPAL
**index 0 is pure black (0,0,0), not a bright grey**: a LUT referenced on index 0 maps black→black
at every level, making PRIM (and therefore every `TEX0*PRIM` wall pixel) solid black. This exact
bug shipped in the first Stage-2 cut and blinded all texture-path validation. Emit
`rdpq_set_prim_color(light_lut[index])` using the **same index DOOM computes today** —
`rw_scale>>LIGHTSCALESHIFT` for walls (`r_segs.c:315`), `distance>>LIGHTZSHIFT` for planes
(`r_plane.c:226`). DOOM's lighting is *already* quantized to 16 levels (`LIGHTLEVELS 16`,
`r_main.h:70`) and is **not** smoothly per-pixel today.

**Known fidelity divergence — per-column → per-seg light (accepted, not "faithful").** DOOM
recomputes the wall light index **per screen column**: `l_rw_scale += l_rw_scalestep` every column
and `index = l_rw_scale>>LIGHTSCALESHIFT` (`r_segs.c:315`,`:411`), so a single seg receding in depth
**steps through several of the 16 levels across its screen width** (visible light banding on angled
near walls in vanilla). But `PRIM` is a *register*, not a vertex attribute, and `TRIFMT_TEX` is
`{X,Y,S,T,INV_W}` with **no per-vertex shade** (`rdpq_tri.h:128`) — so one
`rdpq_set_prim_color(light_lut[index])` per emitted sub-seg gives the **whole sub-seg a single
light level**. This is a real divergence from vanilla, *not* identity: it flattens the per-column
falloff banding within a sub-seg to one step. It is bounded (still one of DOOM's own 16 levels, just
chosen once per sub-seg rather than per column) and is **not** a build blocker, but on the dominant
surface (walls, 52.5% of frame) angled near walls will lose their depth-light steps. **Mitigations,
in order of cost:** (1) **emit per light-level run** — split a seg at the columns where
`rw_scale>>LIGHTSCALESHIFT` changes (extend the emit's "per (tier × contiguous-clip-run)" split,
§1, to "per (tier × clip-run × light-level run)"); a receding seg crossing N of the 16 levels
becomes ≤N sub-segs, each with its own `PRIM` — exact per-column-quantized parity at a modest
command-count increase, since adjacent columns share a level so runs are long. (2) Accept the
per-seg step if (1)'s extra sub-segs hurt the autosync budget — a documented, measurable fidelity
trade tracked by the Stage-3 A/B screenshot diff. Sky/fullbright and short/distant segs (which span
≤1 level) are unaffected either way. **Damage/pickup palette flashes stay FREE (graft #2):** because the CI8 world samples
the master TLUT, the existing `ST_doPaletteStuff → I_SetPalette` swap (`i_video_n64.c:805-823`)
tints the whole rendered view via the single 256-entry TLUT upload (`:758-765`) — *zero* renderer
involvement, *zero* extra per-frame traffic. We **reject** the ceiling design's full-view blend-rect
for the common flashes (it re-touches ~54 KiB when active and is a new path that must match
`ST_doPaletteStuff`). **Invuln / `fixedcolormap` inverse:** the inverse map is a hue-inverting LUT,
not an arithmetic negate — so reproduce it by swapping to the inverse colormap's pre-built PRIM
ramp (level-0 identity for fullbright; the inverse ramp for invuln), or as the documented escape
hatch a single full-view blend rect; both are bounded and faithful. Fullbright sprites/sky set
PRIM=white (`colormaps[0]`). *Caveat acknowledged:* `TEX0*PRIM` is a linear RGB multiply while
DOOM's colormap is a non-linear palette remap, so per-level brightness is matched by *sampling the
colormap ramp into the LUT* rather than assuming a uniform scale — the LUT bakes the actual
darkening curve, keeping the stepped falloff recognisably DOOM. Translated (player-colour) columns
stay on the CPU overlay path (graft #7).

**Fuzz/spectre → RDP blender pass (PRIMARY); translucent-dark alpha-keyed sprite (FALLBACK).** Fuzz
**cannot** stay on CPU under the overlay model: `R_DrawFuzzColumn` reads its own framebuffer output
(`*dest = fuzzmap[dest[fuzzoffset[pos]]]`, `r_draw.c:391`; `fuzzoffset` = ±`SCREENWIDTH`,
`r_draw.c:289`; `fuzzmap = colormaps+6*256`, `r_draw.c:382`), but the view-window CI8 now holds
key-index pixels, not world colours, so a CPU read returns garbage. **Primary — RDP blender-based
fuzz:** draw the spectre as a textured/alpha-keyed quad whose blender **multiplies/darkens the
framebuffer** it just rendered into. The blender can read `MEMORY_RGB` — "current contents of the
framebuffer, where the current pixel will be drawn" (`rdpq_macros.h:802-805`) — and combine it as
`(P*A) + (Q*B)` (`rdpq_macros.h:767-778`); the formula `RDPQ_BLENDER((BLEND_RGB, IN_ALPHA,
MEMORY_RGB, INV_MUX_ALPHA))` darkens the world toward a constant dark `BLEND_RGB` by the sprite's
keyed alpha. (The stock `RDPQ_BLENDER_MULTIPLY` (`rdpq_mode.h:515`) is **NOT** equivalent: it is
`RDPQ_BLENDER((IN_RGB, IN_ALPHA, MEMORY_RGB, INV_MUX_ALPHA))`, so its `P` is `IN_RGB` — the spectre
texture's *own* colour via the combiner — which mixes the fb toward the spectre's texels, not toward
a constant dark colour; it does **not** achieve "darken toward constant dark", so the custom
`BLEND_RGB` formula above is required.) On the 16bpp fb, `P`/`Q` carry 5-bit
precision and `A`/`B` 5-bit (`rdpq_macros.h:840-847`) — coarse but acceptable for a shimmer. This is
**distortion-free** (no ±column displacement) — a dark-shimmer approximation rather than DOOM's exact
pixel-smear, but it reads the *correct* world the RDP drew, which the CPU path no longer can. Note
the blender reads the framebuffer at the draw position only (no neighbour offset), so the lateral
"swim" is lost; the darkening + masked sprite silhouette preserves the spectre read. **Fallback —
translucent-dark sprite draw:** draw the spectre's own alpha-keyed sprite texture (the masked sprite
already emitted in Stage 5) with the blender in MEM-mix toward dark (`RDPQ_BLENDER_MULTIPLY_CONST`,
`rdpq_mode.h:537`, with a fixed dark `FOG_RGB`), giving a flat translucent-shadow silhouette — less
faithful (no per-pixel framebuffer modulation) but trivially correct and reusing the masked path.
**Rejected — CPU fuzz from the previous frame's 16bpp fb converted back to indices:** requires an
RGBA5551→index reverse-lookup per pixel every frame on the bandwidth-bound CPU, re-introducing the
exact per-pixel uncached fill this design exists to remove; cost/complexity disqualify it.

**Q9 — Determinism with async RDP → BRACKET THE CPU WALL; report RDP_BUSY separately.** The bench
brackets the CPU wall only (`LoopBegin`/`LoopEnd`), letting the RDP overlap into the next frame
**uncounted** — this is the honest model of the shippable async pipeline (the player sees the
overlapped throughput) and stays byte-identical run-to-run because CP0 ticks are deterministic
(`n64_bench.c:5-7,187`): the CPU emit path (record append + rdpq command queueing) is identical
every run regardless of when the RDP drains. Add a dedicated **`RDP_BUSY`** phase that captures the
RDP-done timestamp via the `detach_cb` (read non-serializing at the next `LoopBegin`) so a too-slow
RDP surfaces as rising counted busy-spin time rather than hidden async work — *plus* the
buffer-flip spin (`i_video_n64.c:783`) is itself inside the wall and counted, so an RDP that can't
keep up shows up directly. Provide a debug `BENCH_SYNC=1` variant that forces an `rspq_wait` inside
`LoopEnd` for a fully-serialized byte-identical A/B when validating the RDP cost itself. Split
`BPH_BSP` → `BSP_WALK`/`SEG_RASTER` and add `PLANE_EMIT`/`MASKED_EMIT`/`DL_BUILD`/`RDP_BUSY`/
`KEY_CLEAR` (`n64_bench.h:33-43`). Success criterion unchanged: `avg_us < 16670` AND `p95_us < 16670` on the
CPU wall; `RDP_BUSY` is the diagnostic guardrail. Keep the `I_ShutdownGraphics` `rspq_wait`
(`i_video_n64.c:554`) out of the timed loop. *Rationale:* preserves the load-bearing
byte-identical A/B method while making the async pipeline measurable.

**Q10 — Wall texture transpose timing → ON-DEMAND at first TMEM upload, cached across frames.** Walls
are column-major posts (`r_defs.h:285-292`); the RDP wants row-major. Transpose + CI8-tile lazily
the first time a texture is needed — extend `R_GetColumn`/`R_GenerateComposite` (`r_data.c:228,383`)
to also produce a row-major block cached in `PU_CACHE` zone memory keyed by `texnum`. This pays the
transpose **once per texture per level-residency** (not per frame, not for textures never seen),
amortized to ~0 in steady state, and lets the existing zone-memory LRU evict cold blocks under
pressure. **Not at level load:** `R_PrecacheLevel` transposing all 125 textures up front costs
~1.2 MiB resident (CI8) and converts textures that may never appear — wasteful of the scarce 2–4 MB
zone, against the bandwidth-first principle. Flats are already row-major (`r_draw.c:716-735`) so
they need only on-demand CI4 down-convert, also cached. *Accounting (graft #8):* the first-touch
transpose/convert CPU cost lands in `BSP_WALK`/`DL_BUILD` and is tracked there; the bench's
deterministic scenario warms the cache within 1–2 frames and will surface any sustained hitch.

---

## 4. Texture system

**Formats (Q7):** walls **CI8** (column-major posts transposed to row-major, 64×32 tiles, stacked
for tall walls), flats **CI4** (64×64 row-major, 16-entry sub-palette per flat, fits 2 KB beside
the TLUT), sprites **CI8** (full palette + alpha-key). Per-lump CI8 override for any flat/wall whose
distinct-colour count exceeds 16 (graft #5). RGBA16 nowhere.

**Conversion timing (Q10):** on-demand at first TMEM upload, cached in `PU_CACHE` zone memory keyed
by `texnum`/`flatnum`, evicted by the existing zone LRU. Transpose (walls) + CI4 down-convert
(flats) paid once per residency, amortized to ~0 steady-state. Every converted/transposed block is
`data_cache_hit_writeback`'d before the RDP DMA reads it (graft #1, mirroring `i_video_n64.c:763`).

**TMEM residency (Q6):** 4 KB total; **TLUT permanently owns the upper 2 KB** (256 RGBA16 entries,
written only by the present blit, persists across frames — `i_video_n64.c:755-764`). The lower 2 KB
holds exactly one tile per batch. `DL_Flush` uploads one tile, draws all its primitives, then loads
the next — so ~15–20 uploads/frame (per visible texture), not hundreds (per column). This is the
autosync mitigation; `RDP_BUSY` measures whether it lands.

**Lighting / TLUT strategy (Q8):** lighting is a per-drawseg/per-span `PRIM`-colour multiply via
`RDPQ_COMBINER_TEX_FLAT`, indexed by DOOM's existing 16-level quantization, with the PRIM LUT baked
from the actual colormap darkening ramp (not a linear assumption). The **master TLUT remains the
palette of record**: damage/pickup flashes are free via the existing `I_SetPalette` swap (CI8 world
samples the swapped TLUT); invuln uses the inverse-map PRIM ramp (or a bounded full-view tint rect).
Sky/fullbright = PRIM white. This keeps flashes free, the falloff stepped-and-faithful, and avoids
both 16 live TLUTs and any per-pixel colormap lookup.

---

## 5. Non-3D drawer story

**Model:** the 3D view renders into the **16bpp display fb** (scissored to the view window); all
non-3D drawers keep writing 8-bit indices into a **separate uncached CI8 overlay surface** that is
composited on top each present with the TLUT via **transparent-key alpha-compare**
(`rdpq_set_mode_copy(true)`, graft #1). This preserves ~70 `V_DrawPatch`/AM/ST/menu call sites
byte-for-byte.

**Transparent-key compositing (established early, before any world geometry moves to RDP):** one
palette index is reserved as the **transparency key**. At startup, after PLAYPAL load, scan the
UI/status-bar/font/menu patch lumps actually drawn *outside* the 3D view and pick an index none of
them use (**assert if none free**; likely candidates sit near the end of PLAYPAL — the high indices
that DOOM's UI art rarely touches). The TLUT builder (`I_SetPalette`, the per-entry RGBA5551 pack at
`i_video_n64.c:815-818`) sets **alpha=0** for the key index in EVERY uploaded palette variant — so
damage/pickup/invuln palette swaps preserve the key — while every other entry keeps alpha=1. The
present blit runs `rdpq_set_mode_copy(true)`, whose alpha-compare discards the alpha-0 key pixels
(`rdpq_mode.h:328-330`), so any view-window pixel left as the key index reveals the RDP world drawn
underneath in the same rspq stream. The **same index doubles** as the sprite/masked-texture
transparent-gap index: alpha-compare ON for masked draws keys out the gaps, OFF for opaque
walls/flats — so opaque world art may legitimately *contain* the key index without being keyed out.
**Temporary scaffolding:** while the view-window CPU drawers are being removed stage-by-stage, the
frame begins (when `n64_use_rdp_renderer` is ON) with a batched 64-bit key-clear of the 3D-view
region of the CI8 surface (~54 KB, the 320×~168 window) so any pixel the CPU drawers no longer write
becomes the key and reveals the world. Cost basis: a full-region uncached CI8 fill is the same class
of write the round-2 "Narrow visplane top[] clear to spanned columns" commit (`0d2ea28`) reduced —
~54 KB of uncached RDRAM ≈ **0.3–0.6 ms**. This clear is **removed in the final stage** when
view-window CI8 writes become event-driven erase-to-key. The key-clear lands in a **named bench
phase** so its cost is visible.

- **UI/menus/HUD** (`V_DrawPatch` family `v_video.c:239-407`, `m_menu.c:1437-1528`,
  `hu_lib.c:122-165`): unchanged; write into the overlay surface. Native-RDP text is an optional
  later cleanup, not required for 60 FPS.
- **Status bar** (`st_lib.c`, `st_stuff.c:505-510,1119-1129`): **drop diff-draw** (Q4), full-redraw
  onto the overlay each frame (320×32 cached writes, negligible). Removes the 2-buffer coupling.
- **Automap** (`am_map.c:464,833-1067`): keep on the overlay surface initially (Bresenham `PUTDOT`
  into overlay CI8); optionally later move lines to `rdpq_fill_rectangle`/filled triangles. Marks
  via `V_DrawPatch` unchanged.
- **Melt wipe** (`f_wipe.c:152-307`): transient + perf-insensitive — **but the start/end scenes it
  melts no longer exist in CI8 after the split.** `wipe_StartScreen` captures via
  `I_ReadScreen(wipe_scr_start)` into `screens[2]` (`f_wipe.c:257-258`) and `wipe_EndScreen` on N64
  does `memcpy(wipe_scr_end, screens[0], …)` into `screens[3]` (`f_wipe.c:269-273`). Under the overlay
  model the CI8 surface holds only the overlay (key-index view window), not the 3D scene, so naïvely
  there is nothing real to capture. **The start and end captures fail for DIFFERENT reasons and the
  toggle alone fixes only the end one:**
  - **End screen — fixed by a same-frame toggle-off.** `wipe_EndScreen` reads `screens[0]` *after*
    that frame's render, so rendering the end frame with `n64_use_rdp_renderer` **off** lands a real
    3D scene in CI8 and the capture is correct.
  - **Start screen — needs the toggle-off ONE FRAME EARLIER.** `wipe_StartScreen` fires at the TOP of
    `D_Display` (`d_main.c:614-617`, `if (gamestate != wipegamestate)`) **before** this frame's
    `R_RenderPlayerView`, and `I_ReadScreen` on N64 reads the **previously-presented** buffer
    (`n64_draw_idx ^ 1`, `i_video_n64.c:801`), i.e. the buffer rendered **last** frame. If last frame
    rendered under the RDP path, its CI8 holds only the key-index view window — toggling off on the
    *current* frame cannot retroactively fix a buffer drawn last frame, so the start image would be
    key-index garbage and the (most common) level-exit wipe would melt from a corrupt source.
  - **Fix:** drive the per-frame toggle-off from a **one-frame-ahead wipe-pending predictor**, not
    from the `wipe_StartScreen` trigger itself. A wipe is imminent whenever a gamestate transition is
    queued (`gamestate != wipegamestate` is about to become true — e.g. level exit/`G_DoCompleted`,
    finale/intermission entry); the frame *before* that transition must already have rendered through
    the **software path** so the buffer `wipe_StartScreen` later reads holds a real CI8 scene. The
    simplest robust implementation: render the world via software (toggle behaviour off) on **every**
    frame where `gamestate != wipegamestate` *or* a state change is pending **and** on the frame that
    actually performs the capture — and, because the predictor can miss an unanticipated transition,
    also detect a missed start capture (the captured `screens[2]` view region is all key index) and
    force a one-frame software re-render + re-present before starting the melt. Wipes are level
    transitions — **perf-irrelevant** — so software-rendering one or two extra frames around a
    transition is free.
  The wipe's own blocking present loop (`d_main.c:822-835`) runs the **unchanged CI8 present** and is
  compatible as is. `n64_present_copy_forward` (`f_wipe.c:307`) coherency is preserved because the
  wipe frames stay fully on the CI8 software path.
- **Finale / intermission** (`f_finale.c:322-767`, `wi_stuff.c:408-1042`): full-screen flat
  backgrounds become a single tiled RDP texture fill (kills the large CPU `memcpy` into uncached
  RDRAM); glyphs/pics stay on the overlay.
- **View border** (`r_draw.c:1073-1192`): disappears — the RDP renders the 3D view into a scissored
  window and the border becomes a static RDP-blitted texture under it (or stays on the overlay).
- **Split-screen** (1–4p, `d_main.c:686-728`, `i_video_n64.c:482-525`): each player's
  `R_RenderPlayerView` emits into a `rdpq_set_scissor(viewrect)` sub-rect of the shared 16bpp fb;
  the per-frame `memset` + byte-poke dividers become RDP fill rects. `viewangleoffset` psprite
  suppression in side panes (`r_things.c:1040`) and `splitOrientation` (`g_game.c:337`) are
  unchanged. The interpolated/uncapped path (`fractionaltic` lerp, CPU) is untouched.

**Coherency rule (graft #1):** the overlay surface and every converted/transposed texture block
must be `data_cache_hit_writeback`'d before the RDP reads them (mirrors the TLUT writeback,
`i_video_n64.c:763`).

---

## 6. Per-frame byte / time budget (typical combat scene)

Bus budget = 250 MB/s × (1/60 s) = **4,166,667 B/frame ≈ 4.17 MB**. Typical combat means (notes §7):
7 drawsegs, 5 visplanes, 3 vissprites; 3D view 320×~168 ≈ 53,760 px, overlay 320×200 = 64,000 px.

### RDRAM traffic (the binding constraint)

| Traffic class | Typical (KiB) | Tail/p95 (KiB) | Note |
|---|---|---|---|
| 3D-view FB write (16bpp, ~1.15× overdraw typical / 1.6× tail) | 144 | 235 | RGBA5551, 2 B/px; painter order keeps overdraw low |
| Texel loads RDRAM→TMEM (batched ~1 load/texture) | 39 | 113 | ~10 wall CI8 tiles ×2 KB + ~5 flat CI4 ×2 KB + sprites |
| UI overlay blit (CI8 read 64 KB + RGBA5551 write 128 KB) | 188 | 188 | full-screen; dirty-rect (38) is an optional later win |
| DL commands (RSP reads) | 25 | 58 | ~400 prims × ~64 B; static parts via `rspq_block` |
| **First-touch transpose/CI4-convert** (graft #8, cold tiles only) | ~0 (warm) | ≤~30 (cold) | amortized to ~0 steady-state; lands in `BSP_WALK` |
| **Temporary view-window key-clear** (`KEY_CLEAR`, Stages 1–6 only) | 54 | 54 | ~54 KB uncached CI8 fill ≈ 0.3–0.6 ms; **removed in Stage 7** (event-driven erase-to-key) |
| **Z-buffer (REJECTED, Q2)** | **0 (+250 avoided)** | **0 (+250 avoided)** | BSP already orders |
| **TOTAL (Stages 1–6, with key-clear)** | **~450 (10.8%)** | **~678 (16.3%)** | drops to ~396 / ~624 after Stage 7 removes the key-clear |

### RDP fill time (62.5 MHz; standard 1-cycle textured = ~1 px/cycle; COPY = ~4 px/cycle, 16bpp only)

- World (walls+planes+sprites, standard 1-cycle): 53,760 × 1.15 ≈ 61,800 px → **~990 µs**.
- Sprites overdraw included above; tail (vp=15, viss=10, 1.6×): ≈ 86,000 px → ~1.38 ms.
- Overlay blit (COPY mode, valid on the 16bpp display fb, ~4 px/cycle): 64,000 / 4 = 16,000 cyc →
  **~256 µs**.
- **RDP busy ≈ 990 + 256 ≈ 1.25 ms typical; ~1.6 ms tail.** Comfortably < 16.67 ms, ≥ a full frame
  of slack (triple-buffered).

### CPU wall (counted in bench window; overlaps the RDP)

`GAMETIC 1.0 + BSP_WALK ~2.5 (walk only, fill removed) + PLANE/MASKED emit ~1.5 + DL_BUILD ~1.5 +
overlay HUD ~1.1 + AUDIO 2.3 + RDP_BUSY ~0` ≈ **~9.9 ms typical**. The ~1.25 ms RDP busy drains
entirely inside this window, so the CPU never spins on `doom_screen8_rdp_busy`.

### Why this hits 60 FPS

Today the frame is 19.6 ms avg / 31.1 ms p95, of which `bsp_segs` (10.9 ms) + `planes` (3.3 ms) +
`masked` (1.3 ms) ≈ 15.5 ms is per-pixel CPU fill writing the CI8 view byte-by-byte into uncached
640-ns-latency RDRAM (`R_DrawColumn` 1 B/row, `r_draw.c:130-156`). Moving that fill to the RDP —
where the same ~54K-pixel fill costs ~1 ms against a bus that is only ~10% utilized — leaves a CPU
residual of ~9.9 ms that runs concurrently with RDP rasterization. **Both processors finish well
under 16.67 ms**, and the current p95 tail (dominated by plane+masked fill) collapses because that
fill is exactly what moved to the RDP. The honest expectation: shipping **avg ≈ 10–13 ms (≈ 75–100
FPS)** and **p95 < 16.67 ms gated on the texture-batching (Stage 3/4) landing the autosync
collapse** — the one residual risk, measured directly by `RDP_BUSY`.

---

## 7. Staging plan

Each stage is independently buildable and benchable. Bench with `bench/run-bench.sh <label>`
against the frozen `bench/bench-baseline.z64` (`ROM=bench/bench-baseline.z64 bench/run-bench.sh
baseline` for the A/B reference). `BENCH_RESULT` success: `avg_us < 16670` AND `p95_us < 16670`.
Each later stage A/Bs against the *previous* stage and the frozen baseline.

**Split-screen scope across the staging plan (explicit gate).** The transparent-key + `KEY_CLEAR` +
view-render established in Stage 1 is sized for a **single** 320×~168 view window (~54 KB key-clear,
§5/§6). **Split-screen (2–4p) is deliberately deferred to Stage 6** — its 2–4 view sub-rects
(`D_N64GetSplitRect` → `R_SetViewWindow`, `d_main.c:709-712`), per-pane key-clears, multi-scissor
view-render, and the byte-poke/`memset` dividers (`i_video_n64.c:491-525`) are NOT handled in Stages
1–5. **Therefore split-screen is expected to be broken/untested with the flag ON across Stages 1–5;
the kill-switch (flag OFF → unchanged software split) is the supported split path until Stage 6.**
Single-player is the only flag-ON configuration validated in Stages 1–5; the bench scenario is
single-player so this does not affect the gates, but split-screen must not be shipped flag-ON before
Stage 6 lands the per-pane key-clear + multi-scissor.

1. **Stage 0 — Instrumentation (no behaviour change).** Split `BPH_BSP` → `BSP_WALK`/`SEG_RASTER`
   and add `PLANE_EMIT`/`MASKED_EMIT`/`DL_BUILD`/`RDP_BUSY`/`KEY_CLEAR` enum entries + brackets
   (`n64_bench.h:33-43`, `n64_bench.c:219-246`). Add the `n64_use_rdp_renderer` toggle skeleton
   (defaults off). **Expected effect:** 0% perf change. **Bench gate:** `SEG_RASTER` ≈ matches the
   52.5% `bsp_segs` share and `PLANE_EMIT` ≈ matches 15.8% — confirms the split is correct and
   gives the per-stage A/B baseline.

2. **Stage 1 — Transparent-key overlay model + framebuffer/overlay split (view still 100%
   software-rendered).** Establish the compositing mechanism *before any world geometry moves to the
   RDP*, so the split is de-risked while the scene still renders in software exactly as today. (a)
   Reserve the **transparency key** index: at startup, after PLAYPAL load, scan the
   UI/status-bar/font/menu patch lumps drawn outside the 3D view and pick an index none of them use
   (assert if none free; likely candidates near the end of PLAYPAL). (b) The TLUT builder
   (`I_SetPalette` per-entry pack, `i_video_n64.c:815-818`) sets **alpha=0** for the key index in
   EVERY uploaded palette variant (so damage/pickup/invuln swaps preserve the key). (c) Flip the
   present blit to `rdpq_set_mode_copy(true)` so its alpha-compare keys out the alpha-0 index
   (verified COPY-mode alpha-compare on the 16bpp display fb, `rdpq_mode.h:328-330,335` — no 1-cycle
   fallback needed). (d) Introduce the **CI8 overlay surface**; redraw HUD/ST/AM/menu/border onto it
   (full status-bar redraw, diff-draw dropped — Q4); present = software 3D view (still in CI8) +
   overlay blit + `detach_cb` (`i_video_n64.c:728-794`). (e) When `n64_use_rdp_renderer` is ON, the
   frame begins with the **temporary scaffolding key-clear** — a batched 64-bit key-fill of the
   3D-view region of the CI8 surface (~54 KB) in the named `KEY_CLEAR` phase — so any pixel the CPU
   drawers no longer write becomes transparent; at this stage the software renderer still writes the
   whole view so nothing shows through yet (the clear is overwritten), proving the mechanism is
   harmless before walls move. (f) **Wire the menu present-pacing seam (`i_video_n64.c:741-746`)
   before any world geometry moves to the RDP:** the world emit/flush lives *after* the paced
   early-return, and on a paced menu frame the present skips emit AND re-shows the last completed
   display buffer without rotating (§1 / risk table) — established now, while software still fills
   the view, so the pause/options-over-level case is correct before Stage 3. Verify melt wipe:
   wipe-capture frames render with `n64_use_rdp_renderer` **off** (software path) so start/end scenes
   land in CI8 (`wipe_StartScreen`/`wipe_EndScreen`, `f_wipe.c:257-273`); **and the toggle-off must
   latch one frame EARLY** — `wipe_StartScreen` fires at the TOP of `D_Display` (`d_main.c:614-617`)
   *before* this frame's render and `I_ReadScreen` captures the **previously-presented** buffer
   (`n64_draw_idx ^ 1`, `i_video_n64.c:801`), so the frame *before* the wipe must already have been
   software-rendered into CI8 (see §5 wipe fix); the wipe's blocking present loop
   (`d_main.c:822-835`) runs the unchanged CI8 present. **Expected effect:** neutral-to-slight (+overlay blit ~0.26 ms / 188 KiB + `KEY_CLEAR`
   ~0.3–0.6 ms, both affordable per §6). **Bench gate:** `present`/`hud` phase shape changes,
   `KEY_CLEAR` appears with the expected cost; total holds < 16.67 ms; HUD + menu + wipe + flashes
   visually identical to baseline (the alpha-key + key-clear are invisible while software still fills
   the view).

3. **Stage 2 — Single wall seg through the RDP path, behind the flag (graft from `bandwidth`
   feasibility #1).** Keep the CPU rasterizing everything **except** route one single-sided
   (midtexture) wall seg through `DL_EmitWallTier` → `DL_Flush` → `rdpq_triangle` (perspective
   INV_W, `TEX_FLAT`, CI8, on-demand transpose), rendered into the fb **before** the overlay blit in
   the same rspq stream. That seg's CPU `colfunc` writes are suppressed so its view-window columns
   hold the key index and the RDP fill shows through; one texture upload per seg (no batching yet) to
   validate geometry / free-W / lighting on **one** seg, pixel-diffable against the software frame
   via the per-seg A/B toggle (graft #4). **Expected effect:** small net regression (autosync
   thrash, no batching) — honest and falsifiable; proves the wall pipeline + the proportionality
   constant `k` + the transparent-key compositing on a live RDP pixel. **Bench gate:** `SEG_RASTER`
   drops slightly for that seg, `RDP_BUSY` appears and stays < frame; screenshot diff on a near wall
   is sub-pixel-clean.

4. **Stage 3 — Walls fully on RDP + texture batching (the dominant 52.5%).** Replace all three
   `l_colfunc()` sites in `R_RenderSegLoop` (`r_segs.c:333,355,390`) with `DL_EmitWallTier`; add the
   per-texture bucket arena and texture-sorted `DL_Flush`; extend to two-sided top/bottom tiers and
   tall-wall vertical tiling (1-texel overlap + T-clamp). KEEP visplane `top[]/bottom[]` marking on
   CPU. Wall `colfunc` writes stop entirely, so those view columns are left as the key index (cleared
   by `KEY_CLEAR`) and the RDP walls show through; **opaque** walls draw with alpha-compare OFF, so
   wall art may legitimately contain the key index. **Expected effect:** `bsp_segs` fill (~10.9 ms) →
   RDP-overlapped; CPU wall drops to BSP traversal + emit. Autosync collapses to per-texture.
   **Bench gate:** `SEG_RASTER` → ~0 (the gate holds because wall colfunc writes cease completely);
   `RDP_BUSY` rises but stays < frame; avg should approach or pass 16.67 ms; full A/B screenshot diff
   vs software matches.

5. **Stage 4 — Planes on RDP (15.8%).** Replace `R_MapPlane`'s `spanfunc()` with `DL_EmitSpan`;
   emit per-flat CI4 textured rects (on-demand down-convert, per-flat CI8 override by colour
   histogram, graft #5); sky as fullbright textured columns. Reuses the residency manager + PRIM
   light table from Stage 3. Plane `spanfunc` writes stop, leaving those columns as the key index for
   the RDP flats to show through. **Expected effect:** `planes` (~3.3 ms) → RDP-overlapped; targets
   the tail directly (the current p95 tail is plane-dominated). **Bench gate:** `PLANE_EMIT` → ~0;
   avg comfortably < 16.67 ms; p95 drops sharply; no flat banding (histogram override caught any
   gradient flat).

6. **Stage 5 — Sprites / masked mid-textures on RDP.** Replace `R_DrawMaskedColumn`/
   `R_DrawVisSprite` inner loops with `DL_EmitSprite`: per-sprite clipped quads, silhouette
   `clipbot[]/cliptop[]` coalesced into vertical scissor sub-rects, **TLUT-key alpha-compare ON** for
   transparent gaps (the same reserved key index), back-to-front order. Masked mid-textures via the
   same path. Translated (player-colour) columns stay on the CPU overlay (graft #7). **Expected
   effect:** `masked` (~1.3 ms avg, tail spikes to 11.8 ms) → RDP; kills the worst p95 spikes (last
   big tail lever). **Bench gate:** `MASKED_EMIT` → ~0; p95 < 16670; sprite-vs-wall clipping correct
   in scenes with sprites behind/in-front of pillars and through masked grates (screenshot diff vs
   software).

7. **Stage 6 — Fuzz/spectre on RDP + lighting flashes + invuln + split-screen + interp validation.**
   Implement fuzz as the **RDP blender pass** (primary, Q8): a textured/alpha-keyed spectre quad
   whose blender reads `MEMORY_RGB` and darkens the world the RDP just drew (distortion-free
   dark-shimmer); **fallback** is the translucent-dark alpha-keyed sprite draw (Q8). This replaces
   the CPU `R_DrawFuzzColumn` (`r_draw.c:314-404`), which cannot run under the overlay model (it reads
   the view-window framebuffer, now key-index — `r_draw.c:391`). Confirm damage/pickup flashes via
   TLUT swap (graft #2, free), invuln via inverse PRIM ramp / tint rect (Q8). Add per-pane
   `rdpq_set_scissor` and divider RDP fill rects (`d_main.c:686-728`, `i_video_n64.c:491-525`); verify
   1–4p split + the uncapped/interpolated path. **Expected effect:** visual-fidelity completion +
   final overlap headroom; perf-neutral hardening. **Bench gate:** fuzz/flashes/invuln match vanilla
   (fuzz as faithful dark-shimmer); no split regression (each pane is a scissored sub-emit); final
   A/B `avg_us < 16670` AND `p95_us < 16670`.

8. **Stage 7 — Remove the temporary key-clear scaffolding (event-driven erase-to-key).** With every
   view-window CPU drawer now removed (walls Stage 3, planes Stage 4, sprites/masked Stage 5, fuzz
   Stage 6), the full-region `KEY_CLEAR` is no longer needed each frame: make view-window CI8 writes
   **event-driven erase-to-key** (only the few overlay drawers that still intrude on the view region —
   e.g. psprites if kept on CPU, the split dividers — erase their own footprint to the key on change),
   dropping the ~0.3–0.6 ms `KEY_CLEAR` phase. **Expected effect:** −~54 KB/frame, `KEY_CLEAR` → ~0.
   **Bench gate:** `KEY_CLEAR` disappears; no stale view pixels leak through; A/B holds < 16.67 ms.

9. **Stage 8 (optional, only if a stage shows a bus regression) — Dirty-rect overlay.** Replace the
   full-screen overlay blit with a status-bar + HUD dirty-rect blit (38 vs 188 KiB). **Expected
   effect:** −150 KiB/frame. **Deferred:** the §6 budget shows the full blit already fits; do it
   only if the tail needs the headroom.

10. **Stage 9 (optional follow-on) — CI4 walls / RDP automap / native-RDP text.** Pull CI4 for walls
    only if the texture cache blows the 2–4 MB zone (halves the cache, costs 16-colour
    restriction, per-texture histogram override); move automap lines and finale/intermission
    backgrounds to RDP if profiling shows the CPU paths hurt. **Bench gate:** each A/B'd
    independently; ship only if it doesn't regress fidelity or the budget.

---

## 8. Risks and kill-switches

**Kill-switch (mandatory):** the entire RDP renderer sits behind a runtime toggle
`n64_use_rdp_renderer` (menu option in `m_menu.c`, dispatched in `r_main.c`). With it off, the
software `colfunc`/`spanfunc` path (`r_main.c:781-794`) and the CI8 `screens[0]` present
(`i_video_n64.c:728-794`) run **byte-for-byte unchanged** — the software path stays selectable for
every stage, every release. Each new stage is additionally gated so an individual sub-path (e.g.
walls) can fall back to CPU per-seg (the Stage-2 A/B toggle). **The toggle is also reused per-frame
by the melt wipe:** the renderer drops to software behaviour so the 3D scene lands in CI8 for
`wipe_StartScreen`/`wipe_EndScreen` to capture (`f_wipe.c:257-273`). **The end-screen capture reads
the current frame** (toggle-off this frame suffices), **but the start-screen capture reads the
PREVIOUSLY-presented buffer** (`I_ReadScreen` → `n64_draw_idx ^ 1`, `i_video_n64.c:801`) because
`wipe_StartScreen` fires at the TOP of `D_Display` (`d_main.c:614-617`) before this frame renders —
so the software toggle-off must latch **one frame early**, on the frame *before* the gamestate
transition, with a missed-capture fallback that re-renders software and re-presents if the captured
view region is still key-index (§5 wipe fix). Wipes are level transitions and perf-irrelevant, so
the one/two-frame software fallback is free and the wipe's blocking CI8 present loop
(`d_main.c:822-835`) is unchanged.

| Risk | Why | Mitigation / kill-switch |
|---|---|---|
| **Autosync / pipe-sync, not bandwidth, is the real 60-FPS gate** | many small triangles with material/PRIM changes force `AUTOSYNC_PIPE` between draws (`t3d.c:412-454`) | Batch by texture (Stage 3/4); sort within a batch to group PRIM changes; `RDP_BUSY` measures it every stage. Stage 2 is *expected* to regress before Stage 3 wins — falsifiable. |
| **Free-W proportionality constant** (`INV_W = scale*k`, not literally 1/W) | if `k` is wrong, textures scale wrong with depth / near walls warp ("single biggest unknown", notes §8.1) | Derive `k` analytically from `projection`/`centerxfrac` in `R_SetupFrame` (`r_main.c:776`); validate Stage 2 pixel-vs-pixel on a near wall via the per-seg toggle. **Fallback:** trapezoid subdivision (graft #6) or affine (visible warp but trivially correct). |
| **`TEX0*PRIM` lighting divergence** (linear RGB multiply vs non-linear colormap) | the fidelity judge's flag on the ceiling design | PRIM LUT baked from the *actual* colormap darkening ramp, not a uniform scale (Q8). A/B screenshot diff vs software per stage. **Fallback:** per-light pre-shaded CI8 source tiles (the `incremental` index-remap) if the multiply ever bands visibly. |
| **Per-column → per-seg wall light flattening** (PRIM is a register, `TRIFMT_TEX` has no per-vertex shade, `rdpq_tri.h:128`) | vanilla recomputes the light index per column (`l_rw_scale += l_rw_scalestep`, `r_segs.c:411`,`:315`) so a receding seg steps through several of the 16 levels across its width; one `PRIM` per sub-seg loses that banding | **Primary:** split the emit at light-index-change columns (per light-level run, §1) so each sub-seg carries the correct quantized level — exact per-column-quantized parity (runs are long, modest command cost). **Fallback:** accept the per-seg step as a bounded, documented fidelity trade if the extra sub-segs hurt the autosync budget; tracked by the Stage-3 A/B screenshot diff. Not a build blocker. |
| **CI4 flat banding** | 64×64 flats forced to 16-entry sub-palettes may band on gradients (NUKAGE) | Per-flat sub-palette from the flat's colour histogram; CI8-tile override for >16-colour flats (graft #5). |
| **Tall-wall vertical-tile seams** | stacking 64×32 CI8 tiles risks a 1-px seam | 1-texel overlap between vertical tiles + T-clamp at the seam (known libdragon pattern). |
| **RDP falls a frame behind under tail load** (the 60-FPS gate, not avg) | even with CPU < 16.67 ms, if `RDP_BUSY` > 16.67 ms on worst frames the pipeline stalls on the buffer-flip spin (`i_video_n64.c:783`) | The **triple-buffered display fb** (pool 1) gives one frame of *display* slack; the stall point is the **2-deep CI8 busy-spin** (pool 2) — distinct pools (§1 timeline, Q4), so the slack masks display latency, not the CI8 source contention. `RDP_BUSY` makes it observable per stage; pull Stage 3/4 batching (and Stage 9 CI4) harder if it rises. |
| **Menu present-pacing early-return drops the world** (`i_video_n64.c:741-746`) | the paced `menuactive && GS_LEVEL` early-return skips `rdpq_attach` and the whole present; under the RDP model the world is a transient display list reset each frame, so a paced frame that still drained the emit would show a stale/garbage 3D view (pause/options over a level is common) | Emit/flush lives **after** the early-return; on the paced path skip emit AND skip the buffer rotate so libdragon keeps showing the last completed display fb unchanged (§1 integration table). Established in Stage 1 before walls move. |
| **Transparency-key index exhaustion** | the reserved key index must be unused by every UI/HUD/font/menu patch drawn *outside* the 3D view, or those pixels vanish | Stage 1 scans the actual non-view patch lumps and asserts if no index is free (likely candidates near the end of PLAYPAL); opaque world art may *contain* the key (alpha-compare OFF for walls/flats), only masked draws key it out. |
| **Key index leaks through opaque world art** | if a wall/flat texel legitimately uses the key index, alpha-compare would wrongly punch a hole | alpha-compare is **OFF** for opaque walls/flats (they write every covered pixel into the fb), **ON** only for masked/sprite draws — so opaque art carrying the key index renders normally. |
| **Sprite silhouette → scissor-run mis-clip** | coalescing per-column `clipbot/cliptop` into scissor sub-rects could mis-clip vs walls ("sprite clipping vs walls must work") | Stage 5 keeps the exact silhouette scan (`r_things.c:898-1006`); only the leaf draw changes. Validate behind/in-front-of-pillar + masked-grate scenes. **Fallback:** per-column quads (more commands, exact). |
| **Masked-gap transparency** | sparse posts (`r_things.c:355-389`) must reproduce DOOM's transparent gaps via TLUT-key alpha-compare | source texels store the reserved key index in gaps; the key's alpha=0 TLUT entry (`i_video_n64.c:815-818`) + alpha-compare ON keys them out. Screenshot-diff masked mid-textures. **Fallback:** keep masked mids on CPU. |
| **Fuzz/spectre under the overlay model** | `R_DrawFuzzColumn` reads the view-window framebuffer (`dest[fuzzoffset[pos]]`, `r_draw.c:391`), which now holds key-index pixels not world colours → CPU fuzz produces garbage | Move fuzz to the **RDP blender pass** (Stage 6, primary): a spectre quad whose blender reads `MEMORY_RGB` (`rdpq_macros.h:802-805`) and darkens the RDP-drawn world — distortion-free dark-shimmer. **Fallback:** translucent-dark alpha-keyed sprite draw (`RDPQ_BLENDER_MULTIPLY_CONST`). The lateral "swim" is lost in both. |
| **Overlay / wipe buffer coherency** | overlay must be `data_cache_hit_writeback`'d before RDP read; wipe captures need a real CI8 scene | explicit writeback (graft #1, `i_video_n64.c:763` precedent); wipe-capture frames render via the **software path** (toggle off per-frame). **End capture: this-frame toggle-off. Start capture: ONE-frame-early toggle-off** — `wipe_StartScreen` (`d_main.c:614-617`) reads the previously-presented buffer (`i_video_n64.c:801`), so the frame *before* the transition must be software-rendered, with a missed-capture fallback (re-render+re-present if the captured view region is key-index — §5). `copy_forward` (`f_wipe.c:307`) coherency preserved on the CI8 path. |
| **Temporary key-clear scaffolding lingers** | the ~54 KB/frame `KEY_CLEAR` is a crutch while CPU view drawers are removed; left in, it wastes ~0.3–0.6 ms forever | Stage 7 removes it once all view-window CPU writes are gone, replacing with event-driven erase-to-key; tracked in the named `KEY_CLEAR` bench phase so the cost stays visible until removed. |
| **Bench determinism under async RDP** | a serializing fence could drift CP0 ticks | headline brackets the CPU wall only (deterministic); `RDP_BUSY` read non-serializing; `BENCH_SYNC=1` debug variant for serialized validation (Q9). |
| **DL arena overflow on tail frames** (ds=29, vp=15) | emit arena / buckets must handle worst case | size for documented tail ×2; on overflow, fall back to un-bucketed `rdpq_triangle` (slower, correct), mirroring how `drawsegs[256]`/visplanes grow today. |

---

## 9. Explicit non-goals

- **No T3D / custom RSP microcode.** DOOM already produces clipped screen-space geometry; T&L
  offload re-does solved work and competes for the ~100%-full DMEM / shared RSP cycle budget
  (notes §4). `tiny3d/` is never modified.
- **No z-buffer.** BSP front-to-back + solidseg + sprite silhouettes already order geometry; a
  depth buffer would add ~250 KiB/frame against the binding bus for zero benefit (Q2).
- **No perspective-correct floors/ceilings or sprites.** They are constant-z by construction;
  affine is exact and cheaper. Perspective is used only where it is free and needed (walls).
- **No resolution / aspect change.** Stays 320×200, 4:3, `INV_ASPECT_RATIO 0.625`; the VI does the
  output scaling; the present blit stays 1:1. Widescreen (`g_game.c:338`) keeps only changing
  3D projection/FOV, not the display.
- **No RGBA16 textures.** CI8/CI4 + master TLUT end-to-end preserves the 8-bit look and footprint.
- **No removal of the software renderer.** It stays behind the toggle, selectable forever (§8), and
  is reused per-frame by the melt wipe to land start/end scenes in CI8 for capture (§5).
- **No native-RDP UI rewrite as a ship requirement.** The overlay-blit model ships; `rdpq_font`/
  RDP automap are optional follow-ons (Stage 9) only if profiling demands them.
- **No change to gametic determinism, the 35 Hz sim, or the bench scenario / tic injection.** The
  CPU sim, NetUpdate gating, and the deterministic bench harness are untouched.
- **No modification of `libdragon/` or `tiny3d/`.** All changes live in `linuxdoom-1.10/` and the
  root `Makefile`.
```
