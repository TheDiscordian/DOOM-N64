// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// RDP renderer -- frame display-list emit + flush (Stage 2 scaffolding).
//
// DESCRIPTION:
//   The new RDP-rasterized world path emits screen-space primitives into a
//   per-frame bump arena during the (unchanged) CPU geometry/visibility walk,
//   then DL_Flush() drains them through rdpq into the 16bpp display fb in the
//   present seam, BEFORE the overlay blit and in the same rspq stream
//   (Docs/RDP_RENDERER_DESIGN.md sections 1-4).
//
//   Stage 2 routes exactly ONE single-sided (midtexture) wall seg per frame
//   through this path; everything else stays CPU-rendered. The wall record
//   type + a trivial sequential flush (no per-texture bucket sort yet -- that
//   is Stage 3) are the only emit machinery present here.
//
//-----------------------------------------------------------------------------

#ifndef __RDP_VIEW_H__
#define __RDP_VIEW_H__

#ifdef N64

#include <stdint.h>

// One emitted wall tier (here: a midtexture sub-seg). Vertical screen edges =>
// the quad is two triangles. Per Docs/RDP_RENDERER_DESIGN.md section 2.
//
// S is the texture column (texels) and is perspective-correct across the seg
// (free INV_W from rw_scale, Q1). T is affine in screen-y per column with
// per-column slope 1/scale, so T at a shared screen row DIFFERS between the
// left and right edges whenever the scale differs -- the record carries T per
// corner. With per-corner T (and INV_W = scale*k), T/W and 1/W are affine in
// screen space, so the RDP's perspective interpolation reproduces software's
// per-column T = mid + (y - centery)/scale(x) exactly (scale itself stepping
// linearly per column, as DOOM lerps it).
typedef struct
{
    int16_t  x1, x2;            // screen column span (inclusive); vertical edges
    float    ytop_l, ybot_l;    // top/bottom screen Y at the left edge
    float    ytop_r, ybot_r;    // top/bottom screen Y at the right edge
    float    s_l, s_r;          // texture S (texels) at each edge
    float    t_top_l, t_bot_l;  // texture T (texels) at the LEFT edge's Y span
    float    t_top_r, t_bot_r;  // texture T (texels) at the RIGHT edge's Y span
    float    invw_l, invw_r;    // INV_W = rw_scale * k (free W, Q1)
    uint16_t texid;             // texnum (the wall texture)
    uint8_t  light;             // colormap level 0..NUMCOLORMAPS-1 (PRIM index)
} rdp_wall_t;

// Per-frame emit reset. Called from R_SetupFrame after the camera is set up;
// also derives the free-W proportionality constant k for this frame (Q1).
void DL_BeginFrame(void);

// Emit one midtexture wall tier into the frame arena. Inputs are already in
// screen space (computed by the unchanged R_RenderSegLoop math). No-op if the
// arena is full. Returns nonzero if the record was emitted.
int DL_EmitWallTier(const rdp_wall_t* w);

// --- routed-seg per-column capture (Stage 2) -------------------------------
// All routed-seg capture state + the post-loop run-coalescing emit live HERE,
// out of the R_RenderSegLoop hot translation unit, so the flag-OFF seg loop
// compiles to (near) the pre-RDP baseline .text layout. The seg loop, ONLY when
// it has actually claimed the routed seg, feeds each drawn midtexture column to
// DL_RouteCapture and then calls DL_RouteEmit once after the column loop.

// Reset the per-column capture for a freshly-claimed routed seg. Called once,
// right after DL_ClaimWallSeg() returns true.
void DL_RouteBeginSeg(void);

// Capture one drawn midtexture column of the routed seg. scale = rw_scale at
// this column, texcol = texturecolumn, walllights = the seg's light table.
void DL_RouteCapture(int x, int yl, int yh, fixed_t scale, fixed_t texcol,
                     const void* const* walllights);

// After the column loop, coalesce the captured columns into rdp_wall_t records
// (per light-level run) and emit them. mid = rw_midtexturemid, texnum =
// midtexture, centery = the global centery.
void DL_RouteEmit(fixed_t mid, int texnum, int centery);

// Convert a DOOM wall light index (rw_scale>>LIGHTSCALESHIFT, clamped) plus the
// seg's walllights table into the colormap level used as the PRIM index. Kept
// here so the emit site stays a pure data feed.
uint8_t DL_WallLightLevel(const void* const* walllights, unsigned index);

// The per-frame free-W constant k (INV_W = rw_scale * k). Valid after
// DL_BeginFrame. Exposed so the emit site can fold it without recomputing.
float DL_InvWScale(void);

// Drain the frame's emitted records into the attached 16bpp display fb. Must be
// called inside the present seam AFTER rdpq_attach and the view scissor is set,
// BEFORE the overlay blit (same rspq stream). Stage 2: a trivial sequential
// flush (one texture upload + 2 triangles per record). No bucket sort yet.
void DL_Flush(void);

// How many records are queued this frame (0 in Stage 2 unless the routed seg
// was found). Lets the present seam skip the flush plumbing when empty.
int DL_Count(void);

// Retire per-present RDP world state. Call at the END of the present seam,
// AFTER the buffer-flip busy spin (which proves the PREVIOUS present's RDP
// stream fully drained). Demotes texture blocks pinned for the previous
// present's async RDP reads back to PU_CACHE, and clears the emit arena so a
// present that skipped the world render (automap, wipe, menu-paced) can never
// re-flush the last world frame's quads or re-punch its keyed box.
void DL_PresentEnd(void);

// The inclusive screen-space bounding box [*x0,*y0]..[*x1,*y1] covered by this
// frame's emitted world records (the routed seg's suppressed-colfunc pixels).
// Returns nonzero and fills the box when at least one record was emitted, else
// 0. The present seam keys out (alpha-compare) ONLY this box in the overlay
// COPY blit, so software-rendered world art outside it is never subjected to
// alpha-compare -- opaque art may legitimately contain the key index without
// being punched out (DESIGN sec5 / risk table "Key index leaks through opaque
// world art").
int DL_KeyedSpan(int* x0, int* y0, int* x1, int* y1);

// --- Stage-2 single-seg selection / A/B toggle ----------------------------
// The routed seg is the FIRST single-sided seg with a midtexture encountered in
// R_RenderSegLoop each frame (deterministic under the virtual tic clock). The
// per-frame "have we taken one yet" latch is reset in DL_BeginFrame.

// True if the routed seg has NOT been claimed yet this frame AND the RDP wall
// path is enabled (flag on + A/B toggle on). The seg loop calls this at a
// single-sided midtexture column run to decide whether to route it.
int DL_WallSegAvailable(void);

// Claim the per-frame routed-seg slot (the first eligible seg takes it). After
// this returns nonzero, DL_WallSegAvailable() is false for the rest of the
// frame. Returns 0 if already claimed (caller renders on CPU as usual).
int DL_ClaimWallSeg(void);

// Per-seg A/B debug toggle (graft #4): flip the routed seg between the RDP path
// and the CPU column path for pixel comparison. Runtime so a bench/ares session
// can be reasoned about; defaults to RDP (1) so Stage 2 exercises the new path.
// Set to 0 to keep the routed seg on the CPU (the safe fallback default the
// task allows if the seg renders wrong).
extern int n64_rdp_wall_ab;     // 1 = route through RDP, 0 = keep on CPU

#endif // N64
#endif // __RDP_VIEW_H__
