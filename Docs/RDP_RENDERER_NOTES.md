# RDP Renderer — Architecture Notes 🦴

Merged survey findings for the DOOM-N64 RDP renderer effort.
Repo: `/home/discordian/Programming/DOOM-N64` · branch `perf/rdp-renderer` · HEAD `6405a1b`.

**Status quo in one line**: this is `linuxdoom-1.10` keeping the classic 8bpp 320×200 **software** renderer. The R4300 CPU rasterizes *every* 3D-view pixel into a CI8 byte array; the RDP is used **only** for the final present blit (`i_video_n64.c:752-769`). Verified: `grep rdpq_/t3d_/tiny3d/triangle` across `r_segs.c r_plane.c r_things.c r_draw.c r_bsp.c r_main.c` returns nothing — RDP calls live only in `i_video_n64.c`.

Platform bottleneck (cited everywhere, treat as the binding constraint): **250 MB/s RDRAM, ~640 ns single-access latency, 8 KB L1, no L2**. TMEM is **4 KB**. The N64 is bandwidth-bound, not setup-bound.

Vendored, **MUST NOT modify**: `libdragon/` and `tiny3d/` (both present as dirs; `AGENTS.md`). All changes live in `linuxdoom-1.10/` and the root `Makefile`.

---

## 1. Current renderer data flow

CPU stages per frame (uncapped, every vsync — `d_main.c:919-926`; camera/sprite/weapon lerp via `fractionaltic` — `r_main.c:936`, `r_things.c:496`, `r_things.c:698`):

```
TryRunTics (sim)                          d_main.c:946        [BPH_GAMETIC]
  └─ deterministic 35Hz gametic, single-player only (NetUpdate gated on netgame
     r_main.c:1010,1020)

D_Display  (d_main.c:568-836)
  ├─ HU_Erase
  ├─ buffered drawers switch: AM_Drawer / ST_Drawer / WI_Drawer / F_Drawer / D_PageDrawer
  ├─ I_UpdateNoBlit (no-op, i_video_n64.c:692)
  ├─ R_RenderPlayerView  (single, or split loop d_main.c:686-728)
  │    │
  │    ├─ R_RenderBSPNode                  r_main.c:1001       [BPH_BSP]  ← DOMINANT
  │    │    front-to-back BSP walk + 1D occlusion clip (solidsegs[32] r_bsp.c:80-92;
  │    │    R_CheckBBox subtree reject r_bsp.c:381-487; NO z-buffer).
  │    │    R_StoreWallRange (r_segs.c:433) → R_RenderSegLoop (r_segs.c:206):
  │    │      *** WALLS RASTERIZED INLINE IN THE BSP WALK ***
  │    │      per-column colfunc() write AND plane top[]/bottom[] write in the SAME
  │    │      loop (r_segs.c:284-285, 302-303). Constant-z column:
  │    │      dc_iscale = 0xffffffff/rw_scale (r_segs.c:322);
  │    │      texturecolumn = rw_offset - finetangent[angle]*rw_distance (r_segs.c:311).
  │    │    drawseg_t (r_defs.h:322-347, drawsegs[256] r_bsp.c:53) is KEPT only for
  │    │      sprite occlusion + masked-texture deferral.
  │    │
  │    ├─ R_DrawPlanes                      r_main.c:1014       [BPH_PLANES]
  │    │    visplane_t (r_defs.h:463-483; realloc pool r_plane.c:139): per-column
  │    │    top[]/bottom[] spans (0xff = no pixel). R_MapPlane (r_plane.c:200-220)
  │    │    affine 6.10 u/v. Flats 64×64 row-major (r_draw.c:733) — already RDP-native.
  │    │
  │    └─ R_DrawMasked                      r_main.c:1024       [BPH_MASKED]
  │         vissprite_t (r_defs.h:375-412, vissprites[128] r_things.h:31). Sprite
  │         occlusion has NO z-buffer: R_DrawSprite (r_things.c:898) scans drawsegs
  │         back-to-front using silhouette arrays; sort is O(n²) (r_things.c:838).
  │
  ├─ HU_Drawer, border (R_FillBackScreen/R_DrawViewBorder), pause pic, M_Drawer  [BPH_HUD]
  └─ I_FinishUpdate                         d_main.c:808        [BPH_PRESENT]

S_UpdateSounds + I_SubmitSound              d_main.c:1003,1024  [BPH_AUDIO]
```

**Where pixels actually happen — the framebuffer model.** DOOM's `screens[0]` (`v_video.c:44`, `byte* screens[5]`) is the linear 8-bit palette-index array. On N64 it is aliased to one of **two** uncached CI8 surfaces:

- `N64_CI8_BUFFERS = 2` (`i_video_n64.c:22`); `doom_screen8[0..1]` are `surface_alloc(FMT_CI8, 320, 200)` (`i_video_n64.c:854`), which calls `malloc_uncached_aligned(64,…)` — **every CI8 write is uncached straight-to-RDRAM** (`libdragon/src/surface.c:47`).
- `screens[0]` is repointed to `doom_screen8[draw_idx].buffer` at present (`i_video_n64.c:702`, `I_N64PointScreen`). `screens[1..4]` are **cached** malloc scratch/background caches, **not** presented (`i_video_n64.c:876-901`; `st_stuff.c` BG cache = `screens[4]`).
- 3D fill targets `ylookup[i] = screens[0] + (i+viewwindowy)*SCREENWIDTH` (`r_draw.c:992,1024,1035`); `R_DrawColumn` dest `= ylookup[dc_yl]+columnofs[dc_x]` (`r_draw.c:130`). `R_DrawSpan` does **8 texels/aligned uint64** stores (`r_draw.c:731-784`) explicitly because the target is uncached RDRAM; `R_DrawColumn` does 1 byte/row (`r_draw.c:146-156`). Low-detail packs pairs (`r_draw.c:260-281`); `detailshift` swaps `colfunc`/`spanfunc` (`r_main.c:781-794`).

**Present (`I_FinishUpdate`, `i_video_n64.c:728-794`)** — the only existing RDP use, and the seam the new renderer plugs into:

```
rdpq_attach(disp, NULL)                       :752   (disp = 16bpp display fb)
rdpq_set_mode_copy(false)                     :753   COPY mode (~4× fill rate)
rdpq_mode_tlut(TLUT_RGBA16)                   :754
if (n64_palette_dirty):                       :758   re-upload only when changed
   memcpy master → per-buffer slot doom_tlut_up[draw_idx][256]
   data_cache_hit_writeback(slot,…)           :763
   rdpq_tex_upload_tlut(slot, 0, 256)         :764   TLUT → TMEM UPPER half (persists)
rdpq_tex_blit(&doom_screen8[draw_idx], 0,0, NULL)  :769   CI8→RGBA5551 1:1, NO scaling
rdpq_detach_cb(I_N64BufferDone, draw_idx)     :777   NON-blocking RDP-completion fence
next_idx = draw_idx ^ 1                        :782
while (doom_screen8_rdp_busy[next_idx]) ;      :783   spin (normally zero iterations)
[optional] memcpy next←cur if copy_forward     :786   (melt wipe coherency)
I_N64PointScreen(next_idx)                     :790   flip CPU draw target
```

Display: `display_init(N64_DISPLAY_RESOLUTION, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE)` (`i_video_n64.c:845`) → 16bpp output, **triple-buffered** (3), bilinear resample. `N64_DISPLAY_RESOLUTION` = 320×200, aspect 4:3, overscan `VI_CRT_MARGIN` (`i_video.h:57-62`). The VI does the 4:3 output scaling; the blit itself is 1:1. Resolution is fixed: `SCREENWIDTH 320` (`doomdef.h:110`), `SCREENHEIGHT 200` (`doomdef.h:112`), `INV_ASPECT_RATIO 0.625` (`doomdef.h:105`).

**The completion fence is the entire double-buffer safety model**: `rdpq_detach_cb → I_N64BufferDone` (`i_video_n64.c:719-726`) fires under RDP-done interrupt, calls `display_show` and clears `doom_screen8_rdp_busy`. There is **no** global `rspq_wait` in steady state — CPU and RDP overlap. The only stall is the busy-flag spin (`:783-784`).

---

## 2. CPU vs RDP work split

**STAYS ON CPU (cannot move — visibility/sim/topology):**

| Work | Why it stays | Code |
|---|---|---|
| Game sim / gametics | deterministic 35Hz, single-player | `d_main.c:946` |
| BSP traversal (`R_RenderBSPNode`) | RDP cannot do visibility; emits drawsegs/visplanes | `r_main.c:1001`, `r_bsp.c` |
| 1D occlusion clip (solidsegs, `R_CheckBBox`) | front-to-back ordering replaces a z-buffer | `r_bsp.c:80-92,381-487` |
| Vissprite project + O(n²) sort | back-to-front sprite order | `r_things.c:838,898` |
| Silhouette generation | sprite-vs-wall clipping arrays | `r_things.c`, drawseg `r_defs.h:322-347` |
| Patch/UI compositing logic | 70+ call sites assume the CI8 model (see §6) | `v_video.c` etc. |

**MOVES TO RDP (the ~68% of frame that is per-pixel fill):**

| Work | Maps to | Notes |
|---|---|---|
| Wall column fill (`R_RenderSegLoop`) | 2 RDP triangles per seg (screen-space quad) | drawseg `scale1/scale2/scalestep` = endpoint depths; camera-upright invariant ⇒ vertical screen edges. **Hot path** (52.5% of frame). |
| Flat/visplane span fill (`R_DrawPlanes`) | triangle fan/strip or textured rects | flats already 64×64 row-major (RDP-native). |
| Sprite fill (`R_DrawMasked`) | textured rect / 2-tri billboard quad + TLUT | cheap avg, but tail spikes (see §7). |
| Lighting per-pixel colormap lookup | RDP combiner (`TEX × SHADE` or prim color) | drops the CPU colormap remap in `r_draw.c:150`. |

**RSP role (open, see §8):** RSP can do per-column/per-span affine texcoord setup and feed the RDP triangle-setup, OR be skipped entirely if the CPU feeds `rdpq_triangle` directly. Do **NOT** move BSP/occlusion/sort onto the RSP.

**Re-instrumentation required first**: `BPH_BSP` currently lumps BSP-walk (kept) with seg-fill (offloaded) — they must be split (e.g. `BSP_WALK` vs `SEG_RASTER`) plus an `RDP_BUSY` phase, by adding enum entries to `bench_phase_id_t` (`n64_bench.h:33-43`) and brackets via `N64Bench_PhaseSwitch/PhaseBegin` (`n64_bench.c:219-246`). No harness rewrite.

---

## 3. RDP / rdpq capability cheat-sheet

Only the facts that shape this design. API names verified against the vendored headers.

**Triangles (the core of the renderer):**
- `rdpq_triangle(const rdpq_trifmt_t *fmt, const float *v1, const float *v2, const float *v3)` — `libdragon/include/rdpq_tri.h:248`. Runs the RSP triangle-setup ucode (`rspq_triangle.inc`) — edge-walk / slope / coverage setup — but does **NO** model transform, clip, or cull. The CPU supplies already-projected screen-space coordinates.
- `rdpq_trifmt_t` (`rdpq_tri.h:100`) — configurable per-vertex offsets for pos / shade / tex(S,T,W) / z. Predefined formats (verified present): `TRIFMT_FILL`, `TRIFMT_SHADE`, `TRIFMT_TEX`, `TRIFMT_SHADE_TEX`, `TRIFMT_ZBUF`, `TRIFMT_ZBUF_SHADE`, and (per survey) `TRIFMT_ZBUF_TEX`/`TRIFMT_ZBUF_SHADE_TEX` (`rdpq_tri.h:162-169`).
- Perspective-correct texturing needs per-vertex **S,T,W** + `rdpq_mode_persp` (`rdpq_tri.h:204-207`). **Caveat**: DOOM's `r_segs` math produces *affine* per-column texturing, not the projective W the triangle setup expects — see §8.

**Modes / combiner / TLUT (verified):**
- `rdpq_set_mode_copy(false)` — COPY mode, ~4× fill of 1-cycle, 16bpp only (`rdpq_mode.h`).
- `rdpq_mode_tlut(TLUT_RGBA16)` — `TLUT_RGBA16 = 2` (`rdpq_mode.h:229`); also `TLUT_NONE`, `TLUT_IA16` (`rdpq_mode.h:807`).
- `rdpq_tex_upload_tlut(uint16_t *tlut, int color_idx, int num_colors)` — `rdpq_tex_upload_tlut(slot,0,256)` (`rdpq_tex.h:218`). (`rdpq_tex_load_tlut` is deprecated → forwards to this, `rdpq_tex.h:431`.)
- Combiner macros: `TEX_FLAT` / `TEX_SHADE` (`rdpq_macros.h:531/540`); shade × prim available (`rdpq_macros.h:105-140`). `TEX_FLAT + prim_color` = free 1-cycle light.
- `rdpq_set_mode_fill` + `rdpq_fill_rectangle` — solid rects (used by the wad browser UI).
- `rdpq_tex_blit(surface, x, y, NULL)` — the present blit; handles CI8→RGBA5551 with active TLUT.

**Display lists / overlap:**
- `rspq_block_begin/end` + `rspq_block_run` — zero-CPU static command lists (`rspq.h:815`). Record per-frame static batches; CPU runs next-frame BSP while RSP/RDP drain.
- `rdpq_detach_cb(cb, arg)` — non-blocking per-target RDP-completion fence (already proven in present, `i_video_n64.c:777`). Generalize this instead of `rspq_wait` for the new draw work.

**Autosync cost (the real 60fps gate):** RDP must autosync TMEM/tile/pipe whenever material/texture changes (`__rdpq_autosync_use AUTOSYNC_PIPE|TILES|TMEM`, `t3d.c:412,433,454`). DOOM draws many small segs with frequent texture switches → frequent TMEM reloads + pipe syncs. **Design must batch by texture.**

**Reference implementations:** `i_wad_browser_n64.c` already uses the native-RDP UI toolkit (`rdpq_font_load_builtin`, `rdpq_font_style`, `rdpq_paragraph_build/render` with layout cache, `rdpq_fill_rectangle`, `rdpq_set_mode_fill`) — same `display_init` params, `display_set_fps_limit(60.0f)`. It uses the **blocking** `rdpq_attach_clear`/`rdpq_detach_show`, unlike the game's overlapped `detach_cb`. libdragon `rdpqdemo`/`gldemo` and tiny3d `00_quad` are the other models.

---

## 4. tiny3d (T3D) assessment — **BYPASS** (candidate b)

**What T3D is:** HailToDodongo's from-scratch RSP 3D microcode + C library for libdragon (vendored `tiny3d/`, MIT). Its RSP ucode (`rsp_tiny3d.rspl`) does full per-vertex T&L: object→clip-space MVP transform, per-vertex lighting (ambient + up to 7 lights, `LIGHT_COUNT 7`), guard-band clip + trivial reject, perspective W/inv-W, then emits RDP triangle commands through the **same** shared `rspq_triangle.inc` setup ucode that `rdpq_triangle` uses.

**Hard facts:**
- Vertex cache = **70** transformed verts in DMEM (`T3D_VERTEX_CACHE_SIZE 70`, verified `tiny3d/src/t3d/t3d.h:17`). Input verts interleaved 2-per-32B (`T3DVertPacked`, `t3d.h:42-53`); transformed verts 36B (`TRI_SIZE`, `rsp_tiny3d.rspl:45`).
- DMEM is at **~100% usage** (`modelOpt.md:108`). Enabling `RSPQ_PROFILE` pushes it over and forces lights down to 2 / breaks vertex-FX (testscene `Readme.md:22-24`). **Cannot grow the vertex cache without removing features.**
- T3D enables z-buffer by default (`t3d_frame_start` → `rdpq_mode_zbuf(true,true)`, `t3d.c:170-184`; depth cleared 0xFFFC).
- Interops freely with raw rdpq in one frame, but **must** call `t3d_tri_sync()` before switching to other RDP-emitting code (`t3d.h:414-421`).

**Why bypass:** T3D's entire value is the RSP transform + lighting + clip — but DOOM's BSP/`r_segs`/`r_plane` **already** produce clipped screen-space span coordinates on the CPU (`R_RenderSegLoop`, `R_MapPlane`). Feeding T3D 3D world geometry re-does work DOOM has already done, and consumes the shared RSP cycle budget. Candidate (b) — CPU computes screen-space XY + texcoords, feeds `rdpq_triangle()` directly — gets full hardware RDP rasterization (texturing, z, shade) via the same triangle-setup ucode, skipping T&L entirely.

**Supporting evidence the bandwidth limit, not transform, is the wall:** T3D's own `22_bigtex` example abandons direct RDP texturing for a deferred CPU texel-lookup scheme and reaches 60 FPS only **skybox-only** (`main.cpp:22-31`), because direct RDP texturing of large textures is RDRAM-bandwidth-bound — exactly DOOM's constraint. So for DOOM the win is **paletted/CI formats + small TMEM tiles + texture batching**, not deferred shading and not T&L offload.

**Reusable T3D insights even on the direct-feed path:** batch by texture (T3D sorts by material, `t3dmodel.c:363`); strips/batching cut command count ~90%+ and RDRAM traffic ~45% vs per-tri draws (`modelOpt.md:252-281`). Note: modifying the vendored tree is forbidden; a fork would be a project-owned copy.

---

## 5. Texture / TMEM strategy facts

**TMEM = 4 KB total. TLUT reserves the upper 2 KB** (256 RGBA16 entries), persists across frames (only the present blit writes it; CI8 texels load the lower half — `i_video_n64.c:755-764`, `rdpq_tex.h:206-218`). So a CI8 tile is at most **2048 texels** in the lower 2 KB → a **64×64 flat does not fit alongside the TLUT** and must be split or use CI4.

**Formats / sizes:**
- Keep **CI8 + TLUT end-to-end** (preserves the 8-bit look and the 8-bit RDRAM footprint), OR pre-expand to RGBA16 in TMEM. CI8 is the strong default.
- Flats: 64×64 **row-major** (`r_draw.c:716-735`) — already RDP-native; split into tiles or store as CI4 to share TMEM with the TLUT.
- Walls: **column-major posts** (`r_defs.h:285-292`; `R_GetColumn` per-column ptr `r_data.c:382`). RDP wants row-major ⇒ **transpose / composite-on-load is mandatory**, done at `R_GenerateComposite` (`r_data.c:228`) or `R_PrecacheLevel` (`r_data.c:743-845`). 64×128 walls must be tiled.
- Asset scale (DOOM1.WAD shareware): 125 textures, 54 flats, 483 sprites. As CI8 walls ≈ **1245 KiB**, as CI4 ≈ **622 KiB** (`assets` survey). The full texture cache will not fit in the 2–4 MB zone (`i_system_n64.c:73-90`) — use a working set or CI4 (CI4 halves the cache).

**Light levels:** **16 light levels** (`LIGHTLEVELS 16`, verified `r_main.h:70`), **32 colormaps** (`NUMCOLORMAPS 32`, verified `r_main.h:88`), `MAXLIGHTSCALE 48`. Lighting today is a baked 256-byte colormap pointer per column/span: walls `walllights[rw_scale>>LIGHTSCALESHIFT]` (`r_segs.c:315`, `LIGHTSCALESHIFT 12`), planes `planezlight[distance>>LIGHTZSHIFT]` (`r_plane.c:226`, `LIGHTZSHIFT 20`). It is quantized (16×48 scale / 16×128 z), **not per-pixel**. RDP options: combiner shade × one TLUT prim = light per drawseg (`rdpq_macros.h:105-140`), or 16 CI4 TLUTs. RDP shade must quantize or accept smooth gradients.

**Transparency:** CI has no alpha channel; transparency is done via the **TLUT key + alpha-compare** (`i_video_n64.c:818` sets alpha bit = 1 on every entry). Sprite transparency on RDP = TLUT key alpha-compare. (Verified: the present `I_SetPalette` packs RGBA5551 as `(r>>3)<<11 | (g>>3)<<6 | (b>>3)<<1 | 1` — R@11, G@6, B@1, alpha@0=1.)

**Damage/item palette flashes** (`ST_doPaletteStuff → I_SetPalette`) are today free via a 256-entry TLUT swap. An RDP pipeline must reproduce them: either keep the TLUT-blit overlay model (one 256-entry upload, already implemented) or apply a global combiner/blend tint over the rendered view (`rdpq_set_prim_color`).

---

## 6. Non-3D drawers inventory

~70+ CPU call sites write 8-bit indices directly into `screens[0]`. Each must become an rdpq draw or stay on a CPU-composited overlay surface that is itself RDP-blitted. **Recommended baseline strategy** (lowest rewrite risk): RDP renders the 3D view directly into the RGBA16 display fb; the remaining CPU overlays stay on a **separate CI8 overlay surface** blitted on top with TLUT — preserving all `V_DrawPatch`/AM/ST code behind one extra blit. The present path already proves CI8+TLUT blit works.

| Drawer | What it does today | Story |
|---|---|---|
| **`V_DrawPatch` family** (`v_video.c`) | column-by-column 8bpp index writes: `V_DrawPatch` (239-262), `V_DrawPatchFlipped` (368-391), `V_DrawPatchStretch` nearest-neighbour scaled + optional remap (270-327), `V_DrawPatchDirect` (407). `V_CopyRect` (157-196) / `V_DrawBlock` (468-500) memcpy block ops. | Either keep on CI8 overlay surface (cheap), or convert to rdpq sprite draws: upload patches once as CI8/I8 textures, `rdpq_tex_blit` with active TLUT, cache per-lump. |
| **Status bar** (`st_lib.c`, `st_stuff.c`) | diff-draw with **per-CI8-buffer** old-value state indexed by `I_N64DrawBufferIndex()` (`st_lib.c:46,224,278`): `oldinum[idx]`/`oldval[idx]`. `ST_refreshBackground` draws sbar into BG=`screens[4]` then `V_CopyRect`→FG=`screens[0]` (`st_stuff.c:505-510`). `ST_Drawer` forces refresh into BOTH buffers via `st_n64_refresh_left=2` (`st_stuff.c:1119-1129`). | **Load-bearing dependency on exactly 2 ping-pong buffers + per-buffer widget state.** Any change to buffer count or compositing order breaks incremental redraw *unless overlays are fully redrawn each frame*. Easiest: full-redraw onto overlay surface, drop the diff-draw. |
| **Automap** (`am_map.c`) | caches `fb=screens[0]` (`:464`, rebased `AM_N64RebaseScreen:1339-1342`), `AM_clearFB` memset (`:833-835`), Bresenham `PUTDOT fb[yy*f_w+xx]=cc` (`:1008`) in `AM_drawFline`/`AM_drawMline` (`:978-1067`), marks via `V_DrawPatch(...,FB,...)` FB=0 (`:88,1324`). | Lines map cleanly to RDP primitives: `rdpq_fill_rectangle` for 1px spans, or batch into RDP filled triangles/thick lines — moves all automap geometry off CPU. Or keep on overlay surface. |
| **Melt wipe** (`f_wipe.c`) | writes melted CI8 incrementally into `wipe_scr=screens[0]` (`:304`); start/end captured into `screens[2]`/`screens[3]` (`:257,269`); `doMelt` short* column copies (`:214-230`). Sets `n64_present_copy_forward=true` (`:307`) so both ping-pong buffers stay in sync; rebased `F_N64WipeRebaseScreen` (`:56-60`). Runs its OWN blocking present loop in `D_Display` (`d_main.c:822-835`). | Transient + perf-insensitive: keep CPU-side on a small CI8 scratch, composite via overlay blit. OR reimplement as per-column RDP textured-rect copies with animated source-Y from the existing `y[]` table (`f_wipe.c:152-238`). Cannot trivially become one RDP blit (it reads its own prior output). |
| **Finale** (`f_finale.c`) | `F_TextWrite` memcpy-tiles a flat into `screens[0]` full-screen (`:322-337`) then `V_DrawPatch` glyphs; `F_DrawPatchCol` column writes `desttop=screens[0]+x` (`:670-682`); `F_CastDrawer`/`F_BunnyScroll` use `V_DrawPatch`/`Flipped` (`:636-767`). | Full-screen flat backgrounds → single tiled RDP texture fill (kills large CPU memcpy into uncached RDRAM). Glyphs → overlay or rdpq sprites. |
| **Intermission** (`wi_stuff.c`) | background via `memcpy(screens[0], screens[1], 320*200)` (`:408`) then counters/level pics with `V_DrawPatch(...,FB=0,...)` (`:426-1042`). | Background → tiled RDP fill; overlays → overlay surface or rdpq sprites. |
| **Menu / HUD text** (`m_menu.c`, `hu_lib.c`) | `M_WriteText` (`:1437-1478`) / `M_WriteTextScaled→V_DrawPatchStretch` (`:1484-1528`); `HUlib_drawTextLine→V_DrawPatchDirect` (`hu_lib.c:122,137`). `HU_Erase→HUlib_eraseTextLine→R_VideoErase memcpy(screens[0],screens[1])` (`hu_lib.c:161-165`, `r_draw.c:1144`). | Native-RDP text path proven in `i_wad_browser_n64.c` (`rdpq_font` + paragraph layout cache). Or keep on overlay surface. |
| **View border** | `R_FillBackScreen` builds pattern into `screens[1]` BG cache (`r_draw.c:1073`); `R_DrawViewBorder`/`R_VideoErase` copy `screens[1]→screens[0]` (`r_draw.c:1144,1160-1192`). | Disappears entirely if RDP renders the 3D view into a scissored window and the border is a static RDP-blitted texture under it. |
| **Split-screen** (1–4p) | `I_N64SplitScreenBeginFrame` memsets `screens[0]` each frame (`i_video_n64.c:482`); each player's `R_RenderPlayerView` into a sub-rect (`d_main.c:709-714`); `I_N64SplitScreenEndFrame` CPU dividers via memset/byte-poke (`i_video_n64.c:491-525`). `splitOrientation` 0=horiz/1=vert (`g_game.c:337`). `viewangleoffset` (`r_main.c:66`) suppresses psprites in side panes (`r_things.c:1040`). | Natural with RDP scissoring: render each player into a `rdpq_set_scissor` sub-rect of the shared target; the per-frame memset + byte-poke dividers become RDP fill rects. |

**Overlay-surface constraint:** any new CPU-composited overlay must remain uncached (or be explicitly `data_cache_hit_writeback`'d) before the RDP reads it, mirroring the TLUT writeback (`i_video_n64.c:763`).

**Widescreen note:** the `widescreen` var (`g_game.c:338`, 0=4:3 / 1=16:9 Hor+) only changes 3D projection/FOV (`r_main.c:778,814`) and sprite anamorphic step (`r_things.c:593`). It does **NOT** change the display (stays 4:3 320×200) or the present blit.

**Menu present pacing:** `I_FinishUpdate` early-returns if `menuactive && GS_LEVEL` and <16 ms since last present (`i_video_n64.c:741-746`) — back-to-back menu presents don't swap; the next frame keeps drawing into the same buffer.

---

## 7. Perf baseline & bench workflow

**60 FPS budget = 16.67 ms/frame.** Combat avg is 17.4 ms (over) and p95 27.3 ms (well over) → the software rasterizer must be replaced.

**Current round-2 SP** (`/tmp/bench-sp-regression`, frames=2839, mean_total 20765 µs):

| Phase | µs | % | Note |
|---|---|---|---|
| gametic | 1001 | 4.8 | CPU sim, stays |
| **bsp_segs** | **10907** | **52.5** | **DOMINANT** — offload target |
| **planes** | **3286** | **15.8** | offload target |
| masked | 1321 | 6.3 | offload (tail spikes to 11.8 ms) |
| hud | 1129 | 5.4 | CPU, stays (or overlay) |
| present | 806 | 3.8 | already RDP |
| audio | 2276 | 10.9 | CPU, stays |

`BENCH_RESULT`: avg_us=17444 p95_us=27296 max_us=36319 min_us=12400 avg_fps=57.3 p95_fps=36.6.
**bsp_segs + planes = ~68% of the frame** = exactly what an RDP renderer offloads. Offloading them reclaims ~14.2 ms of the 17.4 ms avg; CPU keeps gametic(1.0)+BSP-traversal+masked-setup+hud(1.1)+audio(2.3) ≈ well under 16.67 ms, with RDP rasterization overlapping the next-frame BSP walk via the existing async fence.

**Work counts are LOW** (mean vissprites=3, drawsegs=7, visplanes=5; tail ds=29, vp=15, viss=10) → cost is **per-pixel fill (RDRAM bandwidth)**, not per-primitive setup. This is why RDP rasterization (native per-pixel) is the win.

**Tail (worst 5%) shifts to planes:** bsp_segs 14020 µs/38.4%, planes 10034 µs/27.5%, masked 3317 µs/9.1%, audio 4706 µs/12.9%.

**Reference points:**
- README baseline (frozen `bench/bench-baseline.z64`, unmodified perf/renderer, verified present): frames=2564 avg_us=19603 p95_us=31072 max_us=44306 avg_fps=51.0 p95_fps=32.1. A/B reference, **not** round-2 state.
- Pre-opt (`/tmp/bench-unopt`, frames=2165): avg_us=23277 p95_us=33440 avg_fps=42.9.
- Two opt rounds so far: round-1 cut renderer −25/−27%; round-2 (`d599f7b`) cut full-frame −12/−14/−18% tail (the 64-bit-store batching).

**Success criterion: `avg_us < 16670` AND `p95_us < 16670`.**

**Phase→code map** (`n64_bench.h:33-43`, verified): `BPH_GAMETIC`=TryRunTics (`d_main.c:946`); `BPH_BSP`=R_RenderBSPNode walk+seg fill (`r_main.c:1001`); `BPH_PLANES`=R_DrawPlanes (`r_main.c:1014`); `BPH_MASKED`=R_DrawMasked (`r_main.c:1024`); `BPH_HUD`=display work outside the 3D view; `BPH_PRESENT`=I_FinishUpdate (`d_main.c:808`); `BPH_AUDIO`=S_UpdateSounds+I_SubmitSound (`d_main.c:1003,1024`).

**Bench harness details:** times each frame in VR4300 CP0 ticks via `get_ticks()`, `TICKS_TO_US` keyed on 46.875 MHz (`n64_bench.c:5-7,187`) — **deterministic, byte-identical run-to-run** (`bench/README.md:82-89`). Scenario = 10-step table looping every 20 gametics (`n64_bench.c:111-135`): forward-walk + turns + `BT_ATTACK` + `BT_USE`, injected via `N64Bench_FillTiccmd`; warm-up 35 gametics discarded, collect 2100 gametics (`n64_bench.c:25-27`). Outliers ≥200 ms (`n64_bench.c:45`, level reloads) counted separately and **excluded** from tail/p95 (`n64_bench.c:302-308`) — so `BENCH_RESULT max_us` can show ~1.1M µs while worst phase frame is ~45–50 ms.

**Workflow (exact commands):**
```bash
# Build BENCH ROM + run A/B (Docker doom-n64:tc build, ares run, prints BENCH_RESULT):
cd /home/discordian/Programming/DOOM-N64 && bench/run-bench.sh <label>

# A/B vs frozen baseline ROM (skip build):
ROM=bench/bench-baseline.z64 bench/run-bench.sh baseline

# Per-phase profile from preserved log:
grep ^BENCH_PHASE /tmp/bench-<label>-ares.log   # mean / pct / p95
grep ^BENCH_TAIL  /tmp/bench-<label>-ares.log
grep ^BENCH_WORST /tmp/bench-<label>-ares.log
```
Build is `rm -rf filesystem build && make BENCH=1 -j4` inside `DOCKER_IMAGE` (default `doom-n64:tc`, `run-bench.sh:37,74-77`); ROM at `$WORKDIR/bench.z64`. Overrides: `ROM=path`, `KEEP_ROM=path`, `TIMEOUT`, `ARES`, `DOCKER_IMAGE` (`run-bench.sh:23-37`). Log copied to `/tmp/bench-<label>-ares.log` (`run-bench.sh:61`).

**Footguns:**
- `-DN64_BENCH=1` gate: with `BENCH` unset, `n64_bench.c` does not compile. **`make BENCH=1` leaves `n64_bench.o` in `build/`; a non-BENCH build that doesn't `rm -rf build` silently links bench code** (`bench/README.md:95-101,114`).
- ares disables ISViewer (the primary result sink) for ROMs >64 MB; shareware ROM ~10 MB is fine, but pushing the ROM over 64 MB forces a screenshot fallback (`bench/README.md:35-39`).
- Determinism is load-bearing: the A/B method depends on byte-identical CP0-tick runs. An async-RDP renderer must still be measured by the same CPU-wall `LoopBegin`/`LoopEnd` brackets — OR wait for RDP completion inside the timed window.

---

## 8. Open questions for the design phase

1. **Affine vs perspective texturing.** DOOM's `r_segs` produces *affine* per-column texcoords (`dc_iscale`, `r_segs.c:322`); `rdpq_triangle` perspective texturing wants per-vertex **S,T,W** + `rdpq_mode_persp` (`rdpq_tri.h:204-207`). Does the CPU synthesize correct W/inv-W per screen-space vertex (and does that match DOOM's look), or does the renderer accept affine-only triangles (visible texture warping on near walls)? This is the single biggest unknown.

2. **No z-buffer vs RDP z-buffer.** Today: BSP front-to-back + solidseg clip + sprite silhouettes, no depth buffer. Add an RDP z-buffer (128 KB, fights the 250 MB/s bandwidth that is the actual bottleneck) — or preserve the CPU clipping and feed the RDP in strict back-to-front/front-to-back order? T3D defaults z-buffer **on**.

3. **Overlay-blit model vs native-RDP UI.** §6 baseline (CI8 overlay surface, one extra blit, all `V_DrawPatch`/AM/ST unchanged) minimizes rewrite risk but adds a full-screen 320×200 CI8 blit per frame. Native-RDP UI (rdpq sprites/font, `i_wad_browser_n64.c` model) removes the overlay blit but rewrites ~12 drawer sites. Which, and does the overlay blit's bandwidth cost matter at the new frame budget?

4. **Status-bar diff-draw fate.** It is hard-coupled to exactly 2 ping-pong buffers + per-buffer widget state + the `st_n64_refresh_left=2` trick (`st_stuff.c:1123`). Keep the 2-buffer model, or drop diff-draw entirely and full-redraw the status bar onto the overlay each frame?

5. **RSP role.** Skip the RSP (CPU feeds `rdpq_triangle` directly), or use the RSP for per-span affine texcoord setup / flat sector-polygon transform? T3D transform buys nothing for already-projected geometry and competes for the shared RSP cycle budget.

6. **Texture batching / TMEM residency.** Walls switch textures frequently → autosync pipe/TMEM thrash. How are spans sorted/batched by texture (the modelOpt insight, ~90% command + ~45% bandwidth reduction)? Working-set eviction policy when the full CI8 cache exceeds the 2–4 MB zone?

7. **CI8 vs CI4 vs RGBA16 for textures.** CI4 halves the cache (622 vs 1245 KiB) and fits 64×64 alongside the TLUT in the 4 KB TMEM, but needs 16 sub-palettes / restricted color. Per-format choice for walls vs flats vs sprites?

8. **Lighting on RDP.** Per-drawseg shade prim (`TEX_FLAT + prim_color`, quantized to 16 levels) vs 16 CI4 TLUTs vs a global combiner tint. Must still reproduce the per-frame damage/item palette flashes that are free today via TLUT swap.

9. **Determinism inside the timed window.** With async RDP, does the bench bracket the CPU wall (RDP overlapping into next frame, not counted) or force an RDP fence inside `LoopEnd` (serializing, but byte-identical)? The chosen answer changes what the numbers mean.

10. **Wall texture transpose timing.** Column-major→row-major transpose is mandatory (`r_data.c:228` / `R_PrecacheLevel` `r_data.c:743-845`). Done at level load (memory cost) or on-demand at TMEM upload (bandwidth cost)?
