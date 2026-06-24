// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// RDP renderer -- frame display-list emit + flush (Stage 3 scaffolding).
//
// DESCRIPTION:
//   The new RDP-rasterized world path emits screen-space primitives into a
//   per-frame bump arena during the (unchanged) CPU geometry/visibility walk,
//   then DL_Flush() drains them through rdpq into the 16bpp display fb in the
//   present seam, BEFORE the overlay blit and in the same rspq stream
//   (Docs/RDP_RENDERER_DESIGN.md sections 1-4).
//
//   Stage 3 routes ALL solid wall tiers through this path: every single-sided
//   (midtexture) seg AND the upper/lower textures of two-sided segs. Masked
//   mid-textures stay CPU (Stage 5); sky visplanes stay CPU (Stage 4 -- the
//   wall tiers themselves carry ordinary textures, so the seg loop never routes
//   sky here). Records are bucketed by texnum during the walk and DL_Flush
//   walks texture-by-texture: ONE rdpq_tex_upload per texture per band-set,
//   then all its quads -- collapsing autosync thrash from per-seg to per-texture.
//
//-----------------------------------------------------------------------------

#ifndef __RDP_VIEW_H__
#define __RDP_VIEW_H__

#ifdef N64

#include <stdint.h>

// Wall tier identity (the seg loop's three texture branches). Carried through
// capture/emit so the per-tier texturemid + texture are kept distinct.
enum
{
    DL_TIER_MID = 0,    // single-sided midtexture
    DL_TIER_TOP,        // two-sided upper texture
    DL_TIER_BOT,        // two-sided lower texture
    DL_TIER_COUNT
};

// One emitted wall tier (mid/top/bottom). Vertical screen edges => the quad is
// two triangles. Per Docs/RDP_RENDERER_DESIGN.md section 2.
//
// S is the texture column (texels) and is perspective-correct across the seg
// (free INV_W from rw_scale, Q1). T is affine in screen-y per column with
// per-column slope 1/scale, so T at a shared screen row DIFFERS between the
// left and right edges whenever the scale differs -- the record carries T per
// corner. With per-corner T (and INV_W = scale*k), T/W and 1/W are affine in
// screen space, so the RDP's perspective interpolation reproduces software's
// per-column T = mid + (y - centery)/scale(x) exactly (scale itself stepping
// linearly per column, as DOOM lerps it).
//
// bucket_next chains records sharing a texnum into a per-texture bucket so
// DL_Flush uploads each texture once and draws all its quads before evicting
// the tile (the Stage-3 autosync collapse). -1 terminates the chain.
typedef struct
{
    int16_t  x1, x2;            // screen column span (inclusive); vertical edges
    float    ytop_l, ybot_l;    // top/bottom screen Y at the left edge
    float    ytop_r, ybot_r;    // top/bottom screen Y at the right edge
    float    s_l, s_r;          // texture S (texels) at each edge
    float    t_top_l, t_bot_l;  // texture T (texels) at the LEFT edge's Y span
    float    t_top_r, t_bot_r;  // texture T (texels) at the RIGHT edge's Y span
    float    invw_l, invw_r;    // INV_W = rw_scale * k (free W, Q1)
    int32_t  bucket_next;       // next record index in this texnum's bucket (-1 end)
    uint16_t texid;             // texnum (the wall texture)
    uint8_t  light;             // colormap level 0..NUMCOLORMAPS-1 (PRIM index)
} rdp_wall_t;

// One emitted plane span (Stage 4). A floor/ceiling span is ONE horizontal run
// at screen row y, columns [x1..x2], drawn as a SINGLE affine textured primitive
// (one tri-pair, NOT a band-split stack -- a span samples one thin V-slice of
// the 64x64 flat, so there is no per-row band multiplication, which is the trap
// that floored walls). Flats are 64x64 row-major CI8 sampling the master TLUT
// (NOT CI4 sub-palettes -- they share the same 2 KB TMEM region as the master).
//
// UV is AFFINE and EXACT for planes (constant-z by construction; DOOM uses spans
// precisely because affine is exact). The DOOM span sampler is
//   spot = ((yfrac>>10)&(63*64)) + ((xfrac>>16)&63)   (r_draw.c)
// i.e. U(column) = (xfrac>>16)&63, V(row) = (yfrac>>16)&63, flat[V*64+U]. So
// xfrac drives U, yfrac drives V. We carry the 16.16 fixed-point origin + step
// straight from R_MapPlane (ds_xfrac/ds_yfrac at x1, ds_xstep/ds_ystep per
// column) and convert to texel-space at draw time. No perspective, no per-corner
// T -- one rect covers the whole run.
typedef struct
{
    int16_t  y;                 // screen row
    int16_t  x1, x2;            // screen column span (inclusive)
    int32_t  u0, v0;            // 16.16 texel U/V at column x1 (ds_xfrac/ds_yfrac)
    int32_t  ustep, vstep;      // 16.16 per-column U/V step (ds_xstep/ds_ystep)
    int32_t  bucket_next;       // next span idx in this flat's bucket (-1 end)
    uint16_t flatlump;          // flat lump number (firstflat+flattranslation)
    uint8_t  light;             // colormap level 0..NUMCOLORMAPS-1 (PRIM index)
} rdp_span_t;

// Per-frame emit reset. Called from R_SetupFrame after the camera is set up;
// also derives the free-W proportionality constant k for this frame (Q1).
void DL_BeginFrame(void);

// Emit one wall tier into the frame arena + its per-texture bucket. Inputs are
// already in screen space (computed by the unchanged R_RenderSegLoop math).
// No-op if the arena is full. Returns nonzero if the record was emitted.
int DL_EmitWallTier(const rdp_wall_t* w);

// Emit one plane span (Stage 4). Called from R_MapPlane's leaf in place of
// spanfunc() when DL_PlaneRouteOn(). y/x1/x2 are the span's screen row + column
// range; xfrac/yfrac are ds_xfrac/ds_yfrac (16.16 texel U/V at x1); xstep/ystep
// are ds_xstep/ds_ystep (16.16 per-column step). flatlump is the resolved flat
// lump number (firstflat+flattranslation[picnum]); colormap is ds_colormap (the
// span's resolved light table, reduced to a PRIM level here). No-op if the arena
// is full (the span stays key-index -- a punched hole over stale fb, the bounded
// overflow class). Buckets the span by flat lump so DL_FlushSpans uploads each
// flat once then draws all its spans.
void DL_EmitSpan(int y, int x1, int x2,
                 fixed_t xfrac, fixed_t yfrac, fixed_t xstep, fixed_t ystep,
                 int flatlump, const void* colormap);

// How many plane spans are queued this frame (0 if no plane routed). The present
// world-flush gate (i_video_n64.c) ORs this with DL_Count() so a planes-only
// frame (0 walls, N spans) still enters the flush.
int DL_SpanCount(void);

// --- Stage-4b plane POLYGON path (trapezoid strips) ------------------------
// The visplanes-as-RDP-polygons feature (replacing the per-span Stage-4 fill).
// One rdp_ppoly_t is a single trapezoid RUN over a visplane island: a quad whose
// left/right edges sit at screen columns x1 and x2+1, each edge spanning its own
// [ytop..ybot] screen rows. Per-corner U/V are the flat texel coords (R_MapPlane's
// un-projection evaluated at the corner) and per-edge INV_W ~ (y-centery)/
// planeheight, so the RDP's perspective divide reproduces software's affine flat
// map EXACTLY (u*INV_W and v*INV_W are screen-affine for a constant-z plane).
// Drawn with persp ON (a tall floor poly spans many z-depths -- affine would warp
// it), one tri-pair per run. Bucketed by flat lump like the spans so each flat
// uploads once. r_plane.c emits these per trapezoid run (the same island walk the
// count-only R_CountPlanePolyTris uses, made to emit instead of count).
typedef struct
{
    int16_t  x1, x2;            // screen column span (inclusive); edges at x1, x2+1
    float    ytop_l, ybot_l;    // left-edge top/bottom screen rows (x1 edge)
    float    ytop_r, ybot_r;    // right-edge top/bottom screen rows (x2+1 edge)
    float    u_tl, v_tl;        // texel U/V at the four CORNERS (flat coords, texels)
    float    u_tr, v_tr;
    float    u_bl, v_bl;
    float    u_br, v_br;
    float    invw_tl, invw_bl;  // per-corner 1/W (left edge top/bottom)
    float    invw_tr, invw_br;  // per-corner 1/W (right edge top/bottom)
    int32_t  bucket_next;       // next poly idx in this flat's bucket (-1 end)
    uint16_t flatlump;          // flat lump number (firstflat+flattranslation)
    uint8_t  light;             // run colormap level 0..NUMCOLORMAPS-1 (legacy PRIM)
    // PER-CORNER colormap levels (0..NUMCOLORMAPS-1) resolved at EMIT time at each
    // corner's own screen row -- the same distance>>LIGHTZSHIFT chain software runs
    // per row. The plane flush feeds dl_prim_lut[level] as a per-vertex SHADE so the
    // RDP GOURAUD-interpolates depth-light corner->corner (smooth, like software's
    // per-row falloff) instead of one flat PRIM step per quad. Computed at emit, not
    // flush, because planeheight/planezlight/fixedcolormap are this visplane's live
    // state then -- by flush time they hold the LAST visplane's values.
    uint8_t  light_tl, light_tr, light_bl, light_br;
} rdp_ppoly_t;

// Emit one plane-polygon trapezoid run (Stage-4b). Called from r_plane.c's island
// walk in place of the per-span path when the polygon plane route is on. Corners
// carry the flat texel U/V (R_MapPlane un-projection at each corner) and per-corner
// INV_W. flatlump is the resolved flat lump (firstflat+flattranslation[picnum]);
// colormap is the run's resolved light table (reduced to a PRIM level here). No-op
// if the arena is full (the run stays key-index -- a punched hole over stale fb).
void DL_EmitPlanePoly(const rdp_ppoly_t* p, const void* colormap);

// How many plane polygons are queued this frame (0 if none). ORed into the present
// world-flush gate alongside DL_Count()/DL_SpanCount() so a poly-planes-only frame
// still enters DL_Flush.
int DL_PolyCount(void);

// True if the RDP plane path should emit POLYGONS (trapezoid strips) rather than
// per-span affine rects. On == plane route on AND the polygon sub-path selected.
// r_plane.c's R_DrawPlanes consults this to pick the poly emit + suppress the CPU
// span loop; OFF falls back to the per-span DL_EmitSpan path (Stage-4) or the
// software spanfunc (flag fully off).
int DL_PlanePolyOn(void);

// Convert a plane's resolved colormap pointer (ds_colormap) to the colormap
// level used as the PRIM index, shared with the walls' baked dl_prim_lut. Kept
// here so the R_MapPlane emit site stays a pure data feed.
uint8_t DL_PlaneLightLevel(const void* colormap);

// --- routed-seg per-column capture -----------------------------------------
// All routed capture state + the post-loop run-coalescing emit live HERE, out
// of the R_RenderSegLoop hot translation unit, so the flag-OFF seg loop
// compiles to (near) the pre-RDP baseline .text layout. The seg loop, ONLY when
// the kill-switch is on, feeds each drawn wall-tier column to DL_RouteCapture
// (tagged by tier) and then calls DL_RouteEmit once per drawn tier after the
// column loop.

// Reset the per-column capture for a freshly-entered seg (clears all three tier
// drawn-runs). Called once at the top of R_RenderSegLoop when the flag is on.
void DL_RouteBeginSeg(void);

// Capture one drawn column of a wall tier. tier is DL_TIER_MID/TOP/BOT; scale =
// rw_scale at this column, texcol = texturecolumn, walllights = the seg's light
// table. yl/yh are the tier's clipped screen span (caller guarantees yl<=yh).
void DL_RouteCapture(int tier, int x, int yl, int yh, fixed_t scale,
                     fixed_t texcol, const void* const* walllights);

// After the column loop, coalesce one tier's captured columns into rdp_wall_t
// records (per light-level run) and emit them. mid = the tier's texturemid,
// texnum = the tier's texture, centery = the global centery. No-op if the tier
// drew nothing.
void DL_RouteEmit(int tier, fixed_t mid, int texnum, int centery);

// Convert a DOOM wall light index (rw_scale>>LIGHTSCALESHIFT, clamped) plus the
// seg's walllights table into the colormap level used as the PRIM index. Kept
// here so the emit site stays a pure data feed.
uint8_t DL_WallLightLevel(const void* const* walllights, unsigned index);

// The per-frame free-W constant k (INV_W = rw_scale * k). Valid after
// DL_BeginFrame. Exposed so the emit site can fold it without recomputing.
float DL_InvWScale(void);

// Drain the frame's emitted records into the attached 16bpp display fb. Must be
// called inside the present seam AFTER rdpq_attach and the view scissor is set,
// BEFORE the overlay blit (same rspq stream). Stage 3: per-texture bucket walk
// -- one rdpq_tex_upload per texture per band-set, then all its quads. Stage 4:
// after the walls, DL_Flush drains plane spans too (per-flat bucket walk, persp
// OFF for the affine flat pass), so a planes-only frame still fills its floors.
void DL_Flush(void);

// How many WALL records are queued this frame (0 if no wall tier routed). Lets
// the present seam skip the flush plumbing when empty. The world-flush gate ORs
// this with DL_SpanCount() so a planes-only frame still enters DL_Flush.
int DL_Count(void);

// Retire per-present RDP world state. Call at the END of the present seam,
// AFTER the buffer-flip busy spin (which proves the PREVIOUS present's RDP
// stream fully drained). Demotes texture blocks pinned for the previous
// present's async RDP reads back to PU_CACHE, and clears the emit arena so a
// present that skipped the world render (automap, wipe, menu-paced) can never
// re-flush the last world frame's quads or re-punch its keyed box.
void DL_PresentEnd(void);

// The inclusive screen-space bounding box [*x0,*y0]..[*x1,*y1] covered by this
// frame's emitted world records (the routed walls' suppressed-colfunc pixels).
// Returns nonzero and fills the box when at least one record was emitted, else
// 0. The present seam keys out (alpha-compare) ONLY this box in the overlay
// COPY blit, so software-rendered world art outside it (planes/sprites) is
// never subjected to alpha-compare -- opaque art may legitimately contain the
// key index without being punched out (DESIGN sec5 / risk table "Key index
// leaks through opaque world art").
int DL_KeyedSpan(int* x0, int* y0, int* x1, int* y1);

// --- kill-switch / A/B toggle ----------------------------------------------
// Stage 3 routes ALL eligible wall tiers (no per-frame single-seg latch). The
// seg loop consults DL_WallRouteOn() once per seg to decide whether to route.

// True if the RDP wall path is enabled (flag on + wall A/B toggle on). The seg
// loop calls this at the top of each seg to decide whether to capture/route its
// tiers.
int DL_WallRouteOn(void);

// GPU port (Docs/GPU_PORT_PLAN.md): the static-world-mesh wall route. DL_MeshDrawWalls
// transforms the baked mesh (r_bake.c) into screen records each frame; DL_MeshRouteOn
// gates it (n64_rdp_mesh, set by BENCH_FORCE_MESH).
extern int n64_rdp_mesh;
extern int n64_rdp_mesh_cull;   // BENCH_FORCE_MESH_CULL: mesh's own frustum wall vis (vs BSP occlusion)
extern int n64_rdp_mesh_floors; // Phase 3 floor leaves (BENCH_FORCE_MESH_FLOORS); OFF = perf loss
#ifdef BENCH_FORCE_MESH_RSP
extern int n64_rdp_mesh_rsp;    // RSP port (BENCH_FORCE_MESH_RSP); Phase 0 = DMA loopback probe only
#endif
extern int dl_zbuf_attached;    // set by i_video each frame (1 = z-image attached)
int  DL_MeshRouteOn(void);
void DL_MeshDrawWalls(void);

// Pre-build one wall texture's CI4 block + sub-palette at LEVEL LOAD (called from
// R_PrecacheLevel) so the per-frame render never pays the first-touch median-cut
// quantisation burst. No-op unless the RDP wall path is the active renderer.
void DL_PrequantTexture(int texnum);

// True if the RDP plane path is enabled (flag on + plane A/B toggle on). Mirror
// of DL_WallRouteOn for the Stage-4 floor/ceiling span path; R_MapPlane calls it
// at its leaf to decide whether to emit a span (RDP) or run spanfunc() (CPU).
int DL_PlaneRouteOn(void);

// True if ANY RDP world pass is active this frame (walls OR planes). Used at the
// isolation sites that mean "is the RDP drawing the world": the key-clear arming
// (r_main.c) and its internal guard (i_video_n64.c), so a planes-only or
// walls-only config both arm the full-view key-clear + keyed present correctly.
int DL_AnyRouteOn(void);

// Per-seg A/B debug toggle (graft #4): flip ALL routed walls between the RDP
// path and the CPU column path for pixel comparison. Runtime so a bench/ares
// session can be reasoned about; defaults to RDP (1). Set to 0 to keep walls on
// the CPU (the safe fallback the task allows if walls render wrong).
extern int n64_rdp_wall_ab;     // 1 = route walls through RDP, 0 = keep on CPU

// Plane A/B toggle (symmetry-completion of n64_rdp_wall_ab). 1 = route floors/
// ceilings through the RDP span path, 0 = keep them on the CPU spanfunc(). The
// Stage-4 ISOLATION config is n64_rdp_wall_ab=0 && n64_rdp_plane_ab=1 (SW walls
// + RDP planes), selected at build time via BENCH_FORCE_PLANES_ONLY.
extern int n64_rdp_plane_ab;    // 1 = route planes through RDP, 0 = keep on CPU

// Stage-4b sub-path selector. 1 = emit visplanes as RDP POLYGONS (trapezoid
// strips, perspective-correct -- the low-primitive floor path); 0 = the legacy
// per-span affine path. Only meaningful when the plane route is on.
extern int n64_rdp_plane_poly;

#endif // N64
#endif // __RDP_VIEW_H__
