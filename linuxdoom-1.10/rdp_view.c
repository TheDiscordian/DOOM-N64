// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// RDP renderer -- frame display-list emit + flush (Stage 2).
//
// DESCRIPTION:
//   Stage 2 of the RDP renderer (Docs/RDP_RENDERER_DESIGN.md staging plan).
//   Routes exactly ONE single-sided (midtexture) wall seg per frame through the
//   new RDP wall path, behind the n64_use_rdp_renderer flag; everything else
//   stays CPU-rendered.
//
//   This file owns: the per-frame emit arena, the rdp_wall_t record type,
//   DL_BeginFrame/DL_EmitWallTier/DL_Flush, the free-W proportionality constant
//   k (Q1), the on-demand column-major->row-major CI8 transpose cache (Q10), and
//   the 32-entry PRIM light LUT baked from the colormap ramp (Q8).
//
//   Stage 2 is deliberately a trivial sequential flush (no per-texture bucket
//   sort -- that is Stage 3). It exists to validate the wall pipeline, the
//   free-W constant, the on-demand transpose, the PRIM light path, and the
//   transparent-key compositing on ONE live RDP pixel run.
//
//-----------------------------------------------------------------------------

#ifdef N64

#include <stdint.h>
#include <string.h>
#include <math.h>

#include <libdragon.h>

#include "doomdef.h"
#include "doomstat.h"
#include "m_fixed.h"
#include "r_defs.h"
#include "r_main.h"
#include "z_zone.h"
#include "w_wad.h"
#include "i_system.h"
#include "i_video.h"

#include "rdp_view.h"

// Compile-time diagnostic trace (ISViewer debugf) for the flush/emit path.
// MUST stay 0 in all normal builds -- it adds per-present log traffic that
// would skew timing runs and spam capture logs. Flip to 1 only for a
// one-off diagnostic build.
#ifndef DL_DEBUG_TRACE
#define DL_DEBUG_TRACE 0
#endif

#if DL_DEBUG_TRACE
#include <stdio.h>
#include "v_video.h"    // screens[] for the CI8 ground-truth sampler
extern int viewwindowy; // r_draw.c
#endif

// Texture system globals (r_data.c).
extern int          numtextures;
extern int*         texturewidthmask;
extern fixed_t*     textureheight;
extern lighttable_t* colormaps;

byte* R_GetColumn(int tex, int col);

// --- per-seg A/B toggle (graft #4) -----------------------------------------
// Defaults to the RDP path so Stage 2 exercises the new wall pipeline. The task
// allows defaulting this to CPU (0) if the seg renders wrong and cannot be
// fixed within the iteration budget.
int n64_rdp_wall_ab = 1;

// --- emit arena ------------------------------------------------------------
// Stage 2 emits at most one routed seg, but that seg splits into a record per
// light-level run AND per S-span run (the saturation split in DL_RouteEmit), so
// a long glancing wall can produce dozens of records. Size generously: on
// overflow DL_EmitWallTier drops the record and the dropped columns stay
// key-index inside the keyed box -- a punched hole over stale fb content, the
// worst Stage-2 artifact class -- so the arena should be effectively
// unfillable for a single seg (<= 320 columns => <= 320 runs hard ceiling;
// realistic worst case is far below 128).
#define DL_WALL_ARENA   128

static rdp_wall_t   dl_walls[DL_WALL_ARENA];
static int          dl_wall_count;

// Per-frame routed-seg latch (the first eligible seg claims it).
static int          dl_seg_claimed;

// Free-W proportionality constant for this frame (Q1). INV_W = rw_scale * k.
// The absolute scale cancels in the RDP's hyperbolic divide, so any positive k
// proportional to 1/projection is correct; we pick k = 1/projection so INV_W is
// the dimensionless FRACUNIT/z, well-scaled for the triangle setup's fixed
// point. Derived once per frame from projection (r_main.c:779) in DL_BeginFrame.
static float        dl_invw_k = 1.0f;

// --- on-demand row-major CI8 transpose cache (Q10) -------------------------
// Walls are column-major posts; the RDP wants row-major. Transpose lazily the
// first time a texture is routed, cache the FULL-height row-major block
// (width x textureheight) in zone memory keyed by texnum, and
// data_cache_hit_writeback it before the RDP reads it (section 4).
//
// WIDTH IS PER-TEXTURE (BUG-D fix). The design's original premise "DOOM
// textures are 64-wide" is FALSE: 44 of DOOM1's 125 TEXTURE1 entries are wider
// (36 x 128-wide, 8 x 256-wide). A fixed 64-wide transpose sampled `col mod 64`
// where software samples `col mod width` -- wrong columns across half of every
// wide wall. The transpose width is texturewidthmask[texnum]+1: the EXACT
// power-of-two period R_GetColumn samples with (`col &= texturewidthmask`,
// r_data.c:390), so non-power-of-two declared widths (e.g. the one 24-wide
// texture, effective period 16) match the software sampler bit-for-bit.
//
// TMEM tile sizing is per-width: a CI8 tile must fit the lower 2 KB TMEM half
// (the TLUT owns the upper half), so DL_Flush uploads the block in bands of at
// most DL_TMEM_HALF/width rows (64-wide: 32 rows; 128-wide: 16; 256-wide: 8).
#define DL_TMEM_HALF    2048
// Widths below 8 would break the 8-byte DMA/TMEM-line alignment of per-band
// row offsets; no stock DOOM texture is that narrow -- refuse to route them.
#define DL_MIN_TEX_W    8

typedef struct
{
    void* raw;      // the actual Z_Malloc'd allocation (back-referenced by zone)
    byte* block;    // 8-byte-aligned row-major width x height CI8 view into raw
    int   height;   // texture height in texels (rows in the block)
    int   width;    // transpose width = texturewidthmask+1 (pow2 sample period)
} dl_rowmajor_t;

static dl_rowmajor_t* dl_rowmajor;      // [numtextures]
static int            dl_rowmajor_inited;

static void DL_InitCaches(void)
{
    if (dl_rowmajor_inited)
        return;
    if (numtextures <= 0)
        return;
    dl_rowmajor = (dl_rowmajor_t*)Z_Malloc(numtextures * sizeof(dl_rowmajor_t),
                                           PU_STATIC, 0);
    memset(dl_rowmajor, 0, numtextures * sizeof(dl_rowmajor_t));
    dl_rowmajor_inited = 1;
}

// Produce (or fetch) the full-height row-major CI8 block for texnum.
// Column-major DOOM posts -> row-major: for each of the width columns,
// R_GetColumn gives that column's texel run (textureheight texels); scatter it
// across the rows of the row-major block. Returns the block (and sets *out_h /
// *out_w to its row count / width) or NULL.
//
// LIFETIME (PU_CACHE async-read race -- deliberate decision, documented):
// the RDP reads this block ASYNCHRONOUSLY, after rdpq_detach_cb returns and
// while the CPU is already building the next frame. A plain PU_CACHE block can
// be evicted (and its memory reused) by any Z_Malloc in that window, so the RDP
// would DMA garbage into TMEM. The block is therefore allocated PU_STATIC
// (un-evictable) and only DEMOTED to PU_CACHE by DL_PresentEnd once the present
// AFTER the one that used it has confirmed (via the buffer-flip busy spin,
// i_video_n64.c) that the RDP fully drained the using present's command stream.
// Allocating PU_STATIC up front also closes a second window: the transpose loop
// itself calls R_GetColumn -> W_CacheLumpNum (Z_Malloc traffic) and a fresh
// PU_CACHE block could in principle be purged out from under the loop writing
// into it. Blocks marked in-flight by DL_Flush are tracked in a 2-deep
// pending/previous list (see DL_MarkInFlight / DL_PresentEnd).
static byte* DL_RowMajorBlock(int texnum, int* out_h, int* out_w)
{
    dl_rowmajor_t*  slot;
    byte*           block;
    int             th, tw;
    int             col, row;

    if (texnum < 0 || texnum >= numtextures)
        return NULL;

    DL_InitCaches();
    if (!dl_rowmajor)
        return NULL;

    slot = &dl_rowmajor[texnum];
    // A demoted (PU_CACHE) block may be reclaimed by the zone LRU; it NULLs
    // slot->raw (the back-referenced user ptr) when it does, so re-derive
    // slot->block from raw each touch and re-transpose if raw is gone.
    if (slot->raw && slot->block)
    {
        if (out_h) *out_h = slot->height;
        if (out_w) *out_w = slot->width;
        return slot->block;
    }
    slot->block = NULL;

    th = (textureheight[texnum] >> FRACBITS);
    if (th < 1)
        th = 1;

    // The transpose width is the texture's power-of-two sampling period
    // (R_GetColumn masks with texturewidthmask), NOT a fixed 64: see BUG-D note
    // above. Wider periods than 2048 can't tile at even 1 TMEM row; narrower
    // than 8 break band alignment -- both refuse to route (CPU keeps the seg).
    tw = texturewidthmask[texnum] + 1;
    if (tw < DL_MIN_TEX_W || tw > DL_TMEM_HALF)
        return NULL;

    // PU_STATIC until DL_PresentEnd demotes it (see lifetime note above); user
    // ptr back-references the cache slot so a zone reclaim of the demoted block
    // NULLs slot->raw and the next touch re-transposes. Over-allocate by 7
    // bytes so the row-major view can be 8-byte aligned -- the RDP DMA that
    // loads this block into TMEM (rdpq_tex_upload) requires an 8-byte-aligned
    // source, and Z_Malloc only guarantees 4-byte alignment (size rounded to 4,
    // z_zone.c:195). A misaligned source silently corrupts the tile.
    slot->raw = Z_Malloc(tw * th + 7, PU_STATIC, (void**)&slot->raw);
    if (!slot->raw)
        return NULL;
    block = (byte*)(((uintptr_t)slot->raw + 7) & ~(uintptr_t)7);

    for (col = 0; col < tw; col++)
    {
        // R_GetColumn masks the column into the texture's pow2 period, so col
        // 0..tw-1 enumerates exactly the columns software can ever sample.
        const byte* src = R_GetColumn(texnum, col);
        for (row = 0; row < th; row++)
            block[row * tw + col] = src[row];
    }

    data_cache_hit_writeback(block, tw * th);
    slot->block = block;
    slot->height = th;
    slot->width = tw;
    if (out_h) *out_h = th;
    if (out_w) *out_w = tw;
    return block;
}

// --- in-flight block tracking (PU_CACHE async-read race) -------------------
// Textures whose blocks were enqueued for RDP reads this present (pending) and
// last present (prev). DL_PresentEnd -- called AFTER the present's buffer-flip
// spin, which proves the PREVIOUS present's RDP work fully drained -- demotes
// last present's blocks back to PU_CACHE unless this present re-used them, then
// rotates pending->prev. A list overflow simply leaves the block PU_STATIC
// forever: safe (never dangling), merely un-evictable.
#define DL_INFLIGHT_MAX 16
static int dl_if_pend[DL_INFLIGHT_MAX];
static int dl_if_pend_n;
static int dl_if_prev[DL_INFLIGHT_MAX];
static int dl_if_prev_n;

static void DL_MarkInFlight(int texnum)
{
    int i;

    if (!dl_rowmajor || !dl_rowmajor[texnum].raw)
        return;
    for (i = 0; i < dl_if_pend_n; i++)
        if (dl_if_pend[i] == texnum)
            return;
    // (Re-)pin: a previously demoted block goes back to PU_STATIC for the
    // duration of its in-flight window.
    Z_ChangeTag(dl_rowmajor[texnum].raw, PU_STATIC);
    if (dl_if_pend_n < DL_INFLIGHT_MAX)
        dl_if_pend[dl_if_pend_n++] = texnum;
}

// --- PRIM light LUT (Q8) ---------------------------------------------------
// 32 entries (one per colormap level 0..NUMCOLORMAPS-1). The PRIM colour is the
// colormap's darkening of a near-white reference index, read back through the
// RGB palette (PLAYPAL) -- so TEX0*PRIM reproduces the actual stepped colormap
// falloff curve, not a linear assumption (section 4 caveat). Baked once.
static uint32_t     dl_prim_lut[NUMCOLORMAPS];
static int          dl_prim_inited;

// The brightness reference index fed through the colormap. It MUST be a bright
// (ideally pure-white) palette entry so colormaps[level*256 + ref] traces the
// colormap's darkening curve as a grey ramp; TEX0*PRIM then reproduces DOOM's
// stepped falloff on the textured wall.
//
// DOOM's PLAYPAL index 0 is PURE BLACK (0,0,0) -- NOT a grey-ramp top. Feeding 0
// here made the colormap map black->black at every level, so dl_prim_lut was
// black for all 32 levels and every routed wall rendered TEX0*0 = solid black
// (visually a flat dark/uncleared-fb hole). DOOM's white is index 4
// (255,255,255); colormaps[level*256 + 4] gives the canonical 255,223,191,...,35
// brightness ramp, which is exactly the per-level shade we want as PRIM.
#define DL_PRIM_REF 4

static void DL_BuildPrimLUT(void)
{
    const byte* playpal;
    int         level;

    if (dl_prim_inited)
        return;
    if (!colormaps)
        return;

    playpal = (const byte*)W_CacheLumpName("PLAYPAL", PU_CACHE);
    if (!playpal)
        return;

    for (level = 0; level < NUMCOLORMAPS; level++)
    {
        // The colormap remaps the reference index toward a darker palette
        // entry; read that entry's RGB as the PRIM brightness for this level.
        int mapped = colormaps[level * 256 + DL_PRIM_REF];
        int r = playpal[mapped * 3 + 0];
        int g = playpal[mapped * 3 + 1];
        int b = playpal[mapped * 3 + 2];
        dl_prim_lut[level] = (uint32_t)((r << 24) | (g << 16) | (b << 8) | 0xFF);
    }
    dl_prim_inited = 1;
}

// --- public API ------------------------------------------------------------

void DL_BeginFrame(void)
{
    dl_wall_count = 0;
    dl_seg_claimed = 0;

    // Free-W constant (Q1). projection is the focal length in fixed point
    // (r_main.c:779); rw_scale = projection*FRACUNIT/z (R_ScaleFromGlobalAngle,
    // r_main.c:495). INV_W = rw_scale * k with k = 1/projection gives INV_W =
    // FRACUNIT/z (dimensionless, well-scaled). projection==0 cannot happen on a
    // live frame but guard anyway.
    if (projection != 0)
        dl_invw_k = 1.0f / (float)projection;
    else
        dl_invw_k = 1.0f;

    DL_BuildPrimLUT();
}

float DL_InvWScale(void)
{
    return dl_invw_k;
}

uint8_t DL_WallLightLevel(const void* const* walllights, unsigned index)
{
    const lighttable_t* cm;
    long level;

    if (index >= MAXLIGHTSCALE)
        index = MAXLIGHTSCALE - 1;

    if (!walllights || !colormaps)
        return 0;

    cm = (const lighttable_t*)walllights[index];
    if (!cm)
        return 0;

    // walllights[index] == colormaps + level*256.
    level = (cm - colormaps) / 256;
    if (level < 0)
        level = 0;
    if (level >= NUMCOLORMAPS)
        level = NUMCOLORMAPS - 1;
    return (uint8_t)level;
}

#if DL_DEBUG_TRACE
static int dl_drop_count;       // records dropped on arena overflow this frame
static int dl_present_no;       // diagnostic present counter
#endif

int DL_EmitWallTier(const rdp_wall_t* w)
{
    if (!w)
        return 0;
    if (dl_wall_count >= DL_WALL_ARENA)
    {
#if DL_DEBUG_TRACE
        dl_drop_count++;
#endif
        return 0;       // arena full: drop (correct -- columns stay key-index)
    }
    dl_walls[dl_wall_count++] = *w;
    return 1;
}

// --- routed-seg per-column capture (Stage 2) -------------------------------
// All routed-seg capture lives here (not in R_RenderSegLoop) so the flag-OFF
// seg-loop translation unit stays at the pre-RDP baseline .text size. The seg
// loop feeds drawn columns to DL_RouteCapture and calls DL_RouteEmit once after
// the loop -- but ONLY when it has claimed the routed seg this frame, so none of
// this is reachable with the kill-switch off.
static short         dl_rt_yl[SCREENWIDTH];
static short         dl_rt_yh[SCREENWIDTH];
static fixed_t       dl_rt_scale[SCREENWIDTH];
static fixed_t       dl_rt_scol[SCREENWIDTH];   // texturecolumn
static unsigned char dl_rt_lit[SCREENWIDTH];    // colormap level
static unsigned char dl_rt_drawn[SCREENWIDTH];  // 1 if a span was emitted here
static int           dl_rt_first;               // first drawn column (-1 none)
static int           dl_rt_last;                // last drawn column

void DL_RouteBeginSeg(void)
{
    dl_rt_first = -1;
    dl_rt_last  = -1;
    // Clear the drawn flags for the whole width. The seg loop only calls
    // DL_RouteCapture for columns that actually draw (yl <= yh), so a column
    // skipped this seg would otherwise keep a STALE drawn=1 (and stale
    // yl/yh/scale/texturecolumn) from an earlier frame's routed seg. The emit
    // walk reads those stale cells for run-break decisions and (worse) as run
    // endpoints, producing quads at last-frame's screen coordinates -- visible
    // as warped / misplaced wall pieces. The pre-refactor seg loop zeroed the
    // flag inline in its else-branch; this restores that invariant in one
    // place. 320 bytes once per claimed seg (once per frame), flag-on only.
    memset(dl_rt_drawn, 0, sizeof(dl_rt_drawn));
}

void DL_RouteCapture(int x, int yl, int yh, fixed_t scale, fixed_t texcol,
                     const void* const* walllights)
{
    if (x < 0 || x >= SCREENWIDTH)
        return;
    if (yl <= yh)
    {
        dl_rt_yl[x]    = (short)yl;
        dl_rt_yh[x]    = (short)yh;
        dl_rt_scale[x] = scale;
        dl_rt_scol[x]  = texcol;
        dl_rt_lit[x]   = DL_WallLightLevel(walllights,
                            (unsigned)(scale >> LIGHTSCALESHIFT));
        dl_rt_drawn[x] = 1;
        if (dl_rt_first < 0) dl_rt_first = x;
        dl_rt_last = x;
    }
    else
    {
        dl_rt_drawn[x] = 0;
    }
}

// Coalesce the captured columns into rdp_wall_t records, splitting at columns
// where the colormap light level changes (per light-level run -- preserves
// vanilla's per-column light banding as long contiguous runs; Q8 mitigation).
// The quad's screen-space top/bottom edges and S are sampled at each run's
// left/right columns (top/bottom/scale step linearly, so a per-run quad is
// geometrically exact between its endpoints). T_top/T_bot are the texel rows at
// the wall's top/bottom screen edges -- a constant for the wall thanks to the
// free-W perspective property (Q1), so taken at the run's left column.
void DL_RouteEmit(fixed_t mid, int texnum, int centery)
{
    const float     k = dl_invw_k;
    const int       cy = centery;
    int             run0 = -1;
    int             x;

    if (dl_rt_first < 0)
        return;

    for (x = dl_rt_first; x <= dl_rt_last + 1; x++)
    {
        int drawn = (x <= dl_rt_last) ? dl_rt_drawn[x] : 0;
        int breakrun = 0;

        if (run0 < 0)
        {
            if (drawn)
                run0 = x;
            continue;
        }

        // Break the run at a gap (undrawn column) or a light-level change.
        if (!drawn)
            breakrun = 1;
        else if (dl_rt_lit[x] != dl_rt_lit[run0])
            breakrun = 1;
        // ... or when the S (texturecolumn) span exceeds the fixed-point safe
        // range. rdpq_triangle's S/T attribute setup saturates at |S| >= 1024
        // texels (s*32 through float_to_s16_16, rdpq_tri.c) -- a saturated S
        // smears the texture across the whole quad. DL_Flush biases the run's
        // S endpoints down by a shared multiple of the texture period (so the
        // smaller endpoint lands in [0, width)), which bounds |S| by
        // width + span; with span capped at 700 and width <= 256 that is < 1024
        // always. Long offset segs simply split into more quads.
        else if (dl_rt_scol[x] - dl_rt_scol[run0] > 700
                 || dl_rt_scol[run0] - dl_rt_scol[x] > 700)
            breakrun = 1;

        if (breakrun)
        {
            int         xa = run0;
            int         xb = x - 1;      // inclusive last column of the run
            fixed_t     sca = dl_rt_scale[xa];
            fixed_t     scb = dl_rt_scale[xb];
            fixed_t     isca = 0xffffffffu / (unsigned)sca;
            rdp_wall_t  w;

            w.x1 = (int16_t)xa;
            w.x2 = (int16_t)xb;
            w.ytop_l = (float)dl_rt_yl[xa];
            w.ybot_l = (float)(dl_rt_yh[xa] + 1);   // span is inclusive
            w.ytop_r = (float)dl_rt_yl[xb];
            w.ybot_r = (float)(dl_rt_yh[xb] + 1);
            w.s_l = (float)dl_rt_scol[xa];
            w.s_r = (float)dl_rt_scol[xb];
            w.invw_l = (float)sca * k;
            w.invw_r = (float)scb * k;
            // T texel rows at the wall's top/bottom screen edges (left col).
            // Keep the 16.16 FRACTION (divide, don't >>FRACBITS-truncate): the
            // flush slices the quad into TMEM bands and truncating here shifted
            // every band by up to a texel, mis-seating the band seams. The
            // fixed-point sum itself keeps software's wrap-around semantics.
            w.t_top = (float)(mid + (dl_rt_yl[xa] - cy) * isca)
                      * (1.0f / (float)FRACUNIT);
            w.t_bot = (float)(mid + ((dl_rt_yh[xa] + 1) - cy) * isca)
                      * (1.0f / (float)FRACUNIT);
            w.texid = (uint16_t)texnum;
            w.light = dl_rt_lit[xa];

            DL_EmitWallTier(&w);

            // Start a new run at the current column if it still draws.
            run0 = drawn ? x : -1;
        }
    }
}

int DL_Count(void)
{
    return dl_wall_count;
}

int DL_KeyedSpan(int* x0, int* y0, int* x1, int* y1)
{
    int i;
    int xlo = SCREENWIDTH;
    int ylo = SCREENHEIGHT;
    int xhi = -1;
    int yhi = -1;

    if (dl_wall_count <= 0)
        return 0;

    // The routed seg's emitted records cover a screen-space bounding box; that
    // box is exactly the region whose CPU colfunc was suppressed (it holds the
    // key index and must be keyed out so the RDP fill shows through). Every
    // pixel OUTSIDE the box holds real software-rendered world art, which must
    // NOT be subjected to alpha-compare -- opaque art may legitimately contain
    // the key index without being keyed (DESIGN sec5 / risk table "Key index
    // leaks through opaque world art"). The present blit scissors the keyed
    // COPY pass to this box so software world art elsewhere is blitted opaque.
    //
    // Tight bounding box (not full-screen columns): the routed seg is a single-
    // sided midtexture wall that fills its own [yl,yh] span solidly -- floors
    // and ceilings are marked at rows ABOVE/BELOW the wall, i.e. outside the
    // record's screen Y -- so the box's keyed surface is the wall itself, not
    // arbitrary world fill. Y edges step linearly per record; min(ytop)/
    // max(ybot) over all records bound them.
    for (i = 0; i < dl_wall_count; i++)
    {
        const rdp_wall_t* w = &dl_walls[i];
        int a = w->x1;
        int b = w->x2;
        float yt = (w->ytop_l < w->ytop_r) ? w->ytop_l : w->ytop_r;
        float yb = (w->ybot_l > w->ybot_r) ? w->ybot_l : w->ybot_r;
        int ti = (int)yt;               // floor toward the top edge
        int bi = (int)(yb + 0.999f);    // ceil toward the bottom edge

        if (a < xlo) xlo = a;
        if (b > xhi) xhi = b;
        if (ti < ylo) ylo = ti;
        if (bi > yhi) yhi = bi;
    }

    if (xhi < xlo || yhi < ylo)
        return 0;

    if (xlo < 0) xlo = 0;
    if (ylo < 0) ylo = 0;
    if (xhi > SCREENWIDTH - 1)  xhi = SCREENWIDTH - 1;
    if (yhi > SCREENHEIGHT - 1) yhi = SCREENHEIGHT - 1;

    *x0 = xlo;
    *y0 = ylo;
    *x1 = xhi;
    *y1 = yhi;
    return 1;
}

int DL_WallSegAvailable(void)
{
    if (!n64_use_rdp_renderer)
        return 0;
    if (!n64_rdp_wall_ab)
        return 0;       // A/B toggle: keep the routed seg on the CPU
    return !dl_seg_claimed;
}

int DL_ClaimWallSeg(void)
{
    if (dl_seg_claimed)
        return 0;
    dl_seg_claimed = 1;
    return 1;
}

// Drain the emitted wall records into the attached display fb. Stage 2: one
// texture upload + 2 triangles per record (TRIFMT_TEX), PRIM from the light
// LUT. Must run inside the present seam after rdpq_attach + the view scissor,
// before the overlay blit (same rspq stream). Caller owns the mode/combiner/
// TLUT/persp setup (DESIGN section 4): rdpq_set_mode_standard +
// RDPQ_COMBINER_TEX_FLAT + rdpq_mode_tlut(TLUT_RGBA16) + rdpq_mode_persp(true).
void DL_Flush(void)
{
    int i;

#if DL_DEBUG_TRACE
    dl_present_no++;
    // Stamp the present number into the CI8 bottom row as a 2px-wide binary
    // barcode (white=1 black=0, LSB left) BEFORE the present blit is enqueued,
    // so a host-side screenshot decodes to the exact present it shows.
    if (screens[0])
    {
        // 4px-wide bits over 2 rows so the VI resample can't blur them away.
        int b, r;
        for (r = 198; r <= 199; r++)
        {
            unsigned char* tag = (unsigned char*)screens[0] + r * SCREENWIDTH;
            for (b = 0; b < 16; b++)
            {
                unsigned char v = (unsigned char)(((dl_present_no >> b) & 1) ? 4 : 0);
                tag[b * 4 + 0] = v;
                tag[b * 4 + 1] = v;
                tag[b * 4 + 2] = v;
                tag[b * 4 + 3] = v;
            }
        }
        // screens[0] is the uncached CI8 present buffer (see i_video_n64.c);
        // no cache writeback needed.
    }
    // Sentinel underlay: fill the keyed box with SOLID GREEN in the display fb
    // BEFORE the quads draw. Any keyed-blit punch that the quads fail to cover
    // then shows green instead of stale fb history -- separating "quad did not
    // rasterize here" (green) from "blit layer painted this" (any other colour).
    if (dl_wall_count > 0)
    {
        int bx0, by0, bx1, by1;
        if (DL_KeyedSpan(&bx0, &by0, &bx1, &by1))
        {
            rdpq_mode_push();
            rdpq_set_mode_fill(color_from_packed32(0x00FF00FFu));
            rdpq_fill_rectangle(bx0, by0, bx1 + 1, by1 + 1);
            rdpq_mode_pop();
        }
    }
    if (dl_wall_count > 0)
    {
        int bx0, by0, bx1, by1;
        int have = DL_KeyedSpan(&bx0, &by0, &bx1, &by1);
        debugf("DL_TRACE p=%d count=%d drops=%d box=%d,%d..%d,%d have=%d buf=%d\n",
               dl_present_no, dl_wall_count, dl_drop_count,
               have ? bx0 : -1, have ? by0 : -1,
               have ? bx1 : -1, have ? by1 : -1, have,
               I_N64DrawBufferIndex());
    }
    dl_drop_count = 0;
#endif

    for (i = 0; i < dl_wall_count; i++)
    {
        const rdp_wall_t* w = &dl_walls[i];
        byte*   block;
        int     blkh = 0;       // texture height = vertical wrap period
        int     blkw = 0;       // pow2 sampling width (texturewidthmask+1)
        int     cap;            // max tile rows fitting the lower TMEM half
        uint32_t prim;
        float   xl, xr;
        float   s_l, s_r;       // S endpoints, period-bias-reduced (BUG D)
        float   t0, t1;         // texel-T range covered by the quad

        block = DL_RowMajorBlock(w->texid, &blkh, &blkw);
        if (!block || blkh < 1 || blkw < 1)
        {
#if DL_DEBUG_TRACE
            debugf("DL_TRACE p=%d rec=%d tex=%d NOBLOCK h=%d w=%d\n",
                   dl_present_no, i, w->texid, blkh, blkw);
#endif
            continue;
        }

#if DL_DEBUG_TRACE
        debugf("DL_TRACE p=%d rec=%d tex=%d w=%d h=%d x=%d..%d "
               "y=%d.%d/%d.%d t=%d.%d..%d.%d s=%d..%d lit=%d\n",
               dl_present_no, i, w->texid, blkw, blkh,
               (int)w->x1, (int)w->x2,
               (int)w->ytop_l, (int)w->ytop_r, (int)w->ybot_l, (int)w->ybot_r,
               (int)w->t_top, (int)((w->t_top - (int)w->t_top) * 100),
               (int)w->t_bot, (int)((w->t_bot - (int)w->t_bot) * 100),
               (int)w->s_l, (int)w->s_r, (int)w->light);
#endif

        // Pin the block against zone eviction while the RDP may read it
        // asynchronously (see DL_RowMajorBlock lifetime note / DL_PresentEnd).
        DL_MarkInFlight(w->texid);

        // Per-width TMEM tile cap: a CI8 tile is blkw bytes/row and must fit
        // the lower 2 KB TMEM half beside the resident TLUT (64-wide: 32 rows,
        // 128-wide: 16, 256-wide: 8 -- Q7, corrected for wide textures).
        cap = DL_TMEM_HALF / blkw;
        if (cap < 1)
            continue;

        // PRIM = colormap-level brightness (Q8). TEX0*PRIM in 1-cycle.
        prim = (w->light < NUMCOLORMAPS) ? dl_prim_lut[w->light]
                                         : 0xFFFFFFFFu;
        rdpq_set_prim_color(color_from_packed32(prim));

        xl = (float)w->x1;
        xr = (float)w->x2 + 1.0f;

        // S bias reduction (BUG D, saturation guard): texturecolumn is
        // unbounded (rw_offset + tangent term), but rdpq_triangle's attribute
        // fixed point saturates at |S| >= 1024 texels, smearing the texture.
        // The tile wraps S with mask log2(blkw), so subtracting a SHARED whole
        // number of periods from both endpoints is sampling-identical; after
        // the bias the smaller endpoint lies in [0, blkw) and the emit-side
        // run split caps |s_r - s_l| at 700, so |S| < blkw + 700 < 1024.
        {
            float smin = (w->s_l < w->s_r) ? w->s_l : w->s_r;
            float bias = floorf(smin / (float)blkw) * (float)blkw;
            s_l = w->s_l - bias;
            s_r = w->s_r - bias;
        }

        t0 = w->t_top;          // texel row at the wall's TOP screen edge
        t1 = w->t_bot;          // texel row at the wall's BOTTOM screen edge

        // Texel-T band walk (BUG B fix). DOOM's column drawer WRAPS the source
        // vertically (`source[(frac>>FRACBITS)&127]`, r_draw.c:150) -- the
        // texture TILES over a wall taller than itself; it is NOT clamped. The
        // committed code clamped [t0,t1] into [0,blkh) and silently dropped the
        // wall's other periods: those screen rows kept the key index, the keyed
        // present blit punched them, and 3-presents-stale fb content showed
        // through (the "blue smear"). We march the FULL [t0,t1] range instead,
        // splitting sub-bands at (a) texture-period boundaries (floor-division
        // multiples of blkh, negatives included) and (b) the TMEM row cap, and
        // give the triangles PERIOD-RELATIVE, band-local T.
        //
        // Documented divergence from vanilla: software wraps mod 128 ALWAYS, so
        // a sub-128-tall texture on an over-tall wall shows vanilla's
        // tutti-frutti (rows past the texture sample adjacent zone memory). We
        // wrap mod blkh (the texture tiles cleanly) -- acceptable, deliberate.
        //
        // TMEM RULE: each band's source rows are uploaded tile-LOCAL starting
        // at TMEM row 0 (surface pointed at the band's first row), never at
        // their original T address -- an original-T upload past the cap would
        // overrun the lower 2 KB TMEM half into the resident TLUT.
        //
        // Band seams (design section 4 seam rule): bands abut in screen space
        // (shared edge => each seam pixel rasterizes in exactly one band) and
        // each band's T is clamped to its tile (t.repeats=1), so the exact
        // bottom-edge sample T==rows clamps to the last loaded row instead of
        // reading past the tile. Where a row of headroom exists (cap not hit,
        // period edge not hit) the next source row is uploaded too -- the
        // literal 1-texel overlap -- so interpolation jitter at the seam stays
        // in-band; at the hard 2 KB boundary the T-clamp alone closes the seam.
        {
        const float span = t1 - t0;
        float cur;
        int   guard;

        // T increases down-screen for walls (dc_iscale > 0, yh+1 > yl), so
        // span >= ~1/64 texel for any captured column; <= 0 is pure paranoia.
        if (span <= 0.0f)
            continue;

        cur = t0;
        for (guard = 0; cur < t1 - (1.0f / 1024.0f) && guard < 256; guard++)
        {
            surface_t       surf;
            rdpq_texparms_t parms;
            int     period_base;    // floor(cur/blkh)*blkh, texels
            int     src_lo;         // band's first source row, in [0,blkh)
            int     src_cap;        // band row ceiling (TMEM cap / period edge)
            int     src_hi;         // one past the band's last drawn source row
            int     rows;           // drawn rows in this band
            int     rows_up;        // uploaded rows (rows + optional overlap)
            float   band_end;       // T where this band stops (texels)
            float   f0, f1;         // screen-Y lerp factors for the slice
            float   bt0, bt1;       // band-local T at slice top/bottom
            float   ytl, ytr, ybl, ybr;
            byte*   bandsrc;

            // Period base via floor division (handles negative T).
            period_base = (int)floorf(cur / (float)blkh) * blkh;
            src_lo = (int)floorf(cur) - period_base;
            if (src_lo < 0) src_lo = 0;             // float-edge paranoia
            if (src_lo >= blkh) src_lo = blkh - 1;

            src_cap = src_lo + cap;
            if (src_cap > blkh)
                src_cap = blkh;                     // split at the period edge

            band_end = (float)(period_base + src_cap);
            if (band_end > t1)
                band_end = t1;

            src_hi = (int)ceilf(band_end - (float)period_base);
            if (src_hi > src_cap) src_hi = src_cap;
            if (src_hi <= src_lo) src_hi = src_lo + 1;
            rows = src_hi - src_lo;

            // 1-texel seam overlap where a row of headroom exists (see above).
            rows_up = rows;
            if (rows_up < cap && src_lo + rows_up < blkh)
                rows_up++;

            // Band-local upload at TMEM row 0; S wraps with the texture's pow2
            // period (mask = log2(blkw), matching R_GetColumn's `& mask`).
            bandsrc = block + (src_lo * blkw);
            surf = surface_make_linear(bandsrc, FMT_CI8, blkw, rows_up);
            memset(&parms, 0, sizeof(parms));
            parms.s.repeats = REPEAT_INFINITE;  // wrap S (texture column)
            parms.t.repeats = 1;                // clamp T at the band edges
            rdpq_tex_upload(TILE0, &surf, &parms);

            // Screen-Y for this slice: Y is affine in T along the wall (free-W,
            // Q1), so lerp both edges by the slice's fractional T position.
            f0 = (cur - t0) / span;
            f1 = (band_end - t0) / span;
            ytl = w->ytop_l + (w->ybot_l - w->ytop_l) * f0;
            ybl = w->ytop_l + (w->ybot_l - w->ytop_l) * f1;
            ytr = w->ytop_r + (w->ybot_r - w->ytop_r) * f0;
            ybr = w->ytop_r + (w->ybot_r - w->ytop_r) * f1;

            // Band-local T, fraction preserved (the slice's top usually starts
            // mid-texel after a period or cap split).
            bt0 = cur - (float)(period_base + src_lo);
            bt1 = band_end - (float)(period_base + src_lo);

            {
                float tl[5] = { xl, ytl, s_l, bt0, w->invw_l };
                float tr[5] = { xr, ytr, s_r, bt0, w->invw_r };
                float bl[5] = { xl, ybl, s_l, bt1, w->invw_l };
                float br[5] = { xr, ybr, s_r, bt1, w->invw_r };

                rdpq_triangle(&TRIFMT_TEX, tl, tr, bl);
                rdpq_triangle(&TRIFMT_TEX, tr, br, bl);
            }

            cur = band_end;
        }
        }
    }
}

// Retire per-present RDP world state. MUST be called at the end of the present
// seam, AFTER the buffer-flip busy spin (i_video_n64.c) -- that spin proves the
// PREVIOUS present's RDP commands (including its texture-block DMAs) fully
// drained, so last present's pinned blocks can be demoted back to PU_CACHE
// unless this present re-used them. Also clears the emit arena: DL_BeginFrame
// only runs on world-rendered frames (d_main.c gates it on GS_LEVEL &&
// !automapactive), so without this clear an automap or wipe present would
// re-flush the LAST world frame's quads and re-punch its keyed box over
// non-world content (the user-visible stale-rectangle / duplicated-gun class
// of defect).
void DL_PresentEnd(void)
{
    int i, j;

#if DL_DEBUG_TRACE
    // Presents that ran with NO world records (automap/wipe/menu-paced/no
    // eligible seg) are invisible to the flush trace; log them so capture
    // frames can be correlated against opaque-blit presents too.
    if (dl_wall_count == 0)
    {
        dl_present_no++;
        debugf("DL_TRACE p=%d EMPTY\n", dl_present_no);
    }
    // Ground truth on the CI8 content the present just blitted: full-view scan
    // for key-index pixels OUTSIDE the keyed box (those get blitted OPAQUE as
    // the key's TLUT colour -- the magenta/salmon artifact). screens[0] still
    // points at the just-presented buffer here (the flip happens after
    // DL_PresentEnd), and the RDP blit reads exactly this content.
    if (screens[0])
    {
        extern int n64_rdp_key_index;
        int bx0 = 0, by0 = 0, bx1 = -1, by1 = -1;
        int inbox = 0, outbox = 0;
        int ox0 = 9999, oy0 = 9999, ox1 = -1, oy1 = -1;
        int x, y;
        int key = n64_rdp_key_index;
        int have = (dl_wall_count > 0) && DL_KeyedSpan(&bx0, &by0, &bx1, &by1);

        for (y = 0; y < 168; y++)
        {
            const unsigned char* row = (const unsigned char*)screens[0]
                                       + (viewwindowy + y) * SCREENWIDTH;
            for (x = 0; x < SCREENWIDTH; x++)
            {
                if (row[x] != key)
                    continue;
                if (have && x >= bx0 && x <= bx1 && y >= by0 && y <= by1)
                    inbox++;
                else
                {
                    outbox++;
                    if (x < ox0) ox0 = x;
                    if (x > ox1) ox1 = x;
                    if (y < oy0) oy0 = y;
                    if (y > oy1) oy1 = y;
                }
            }
        }
        if (outbox > 0)
            debugf("DL_KEYSCAN p=%d in=%d OUT=%d outbox=%d,%d..%d,%d "
                   "box=%d,%d..%d,%d have=%d\n",
                   dl_present_no, inbox, outbox, ox0, oy0, ox1, oy1,
                   bx0, by0, bx1, by1, have);
    }
#endif

    for (i = 0; i < dl_if_prev_n; i++)
    {
        int tex = dl_if_prev[i];
        int live = 0;

        for (j = 0; j < dl_if_pend_n; j++)
        {
            if (dl_if_pend[j] == tex)
            {
                live = 1;
                break;
            }
        }
        if (!live && dl_rowmajor && dl_rowmajor[tex].raw)
            Z_ChangeTag(dl_rowmajor[tex].raw, PU_CACHE);
    }

    for (i = 0; i < dl_if_pend_n; i++)
        dl_if_prev[i] = dl_if_pend[i];
    dl_if_prev_n = dl_if_pend_n;
    dl_if_pend_n = 0;

    dl_wall_count = 0;      // consumed: never re-flush stale records
}

#endif // N64
