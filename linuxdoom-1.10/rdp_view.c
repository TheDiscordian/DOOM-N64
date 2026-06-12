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

#include <libdragon.h>

#include "doomdef.h"
#include "doomstat.h"
#include "m_fixed.h"
#include "r_defs.h"
#include "r_main.h"
#include "z_zone.h"
#include "w_wad.h"
#include "i_video.h"

#include "rdp_view.h"

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
// Stage 2 emits at most one record (the single routed seg may split into a few
// light-level runs, each its own record). Size generously for the documented
// tail (Q + risk table) so a future stage's per-seg light-run split never
// overflows here; on overflow DL_EmitWallTier just drops the record (correct,
// the CPU path already drew nothing for the routed seg's columns -- they stay
// key-index and reveal the empty fb, which the A/B toggle would catch).
#define DL_WALL_ARENA   64

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
// first time a texture is routed, cache the FULL-height row-major block (64 x
// textureheight) in PU_CACHE zone memory keyed by texnum, and
// data_cache_hit_writeback it before the RDP reads it (section 4).
//
// A 64-wide CI8 tile is capped at DL_TILE_H rows by TMEM (a 64xH CI8 tile is
// 64*H bytes; the lower TMEM half is 2 KB, so H<=32 -- Q7's hard boundary). The
// full block is therefore uploaded in 32-row bands by DL_Flush; a wall taller
// than 32 texels draws as stacked bands with a 1-texel overlap + T-clamp at the
// seam (the design's tall-wall seam rule, section 4 / risk table). Width stays
// 64 (R_GetColumn masks/wraps narrower textures to match the software sampler).
#define DL_TILE_W   64
#define DL_TILE_H   32

typedef struct
{
    void* raw;      // the actual Z_Malloc'd allocation (back-referenced by zone)
    byte* block;    // 8-byte-aligned row-major 64 x height CI8 view into raw
    int   height;   // texture height in texels (rows in the block)
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
// Column-major DOOM posts -> row-major: for each of the 64 columns, R_GetColumn
// gives that column's texel run (textureheight texels); scatter it across the
// rows of the row-major block. Stored in PU_CACHE so the zone LRU may evict it
// (the back-reference pointer NULLs the slot on reclaim, re-transposed on next
// touch). Returns the block (and sets *out_h to its row count) or NULL.
static byte* DL_RowMajorBlock(int texnum, int* out_h)
{
    dl_rowmajor_t*  slot;
    byte*           block;
    int             th;
    int             col, row;

    if (texnum < 0 || texnum >= numtextures)
        return NULL;

    DL_InitCaches();
    if (!dl_rowmajor)
        return NULL;

    slot = &dl_rowmajor[texnum];
    // The zone LRU may reclaim a PU_CACHE block; it NULLs slot->raw (the
    // back-referenced user ptr) when it does, so re-derive slot->block from raw
    // each touch and re-transpose if raw is gone.
    if (slot->raw && slot->block)
    {
        if (out_h) *out_h = slot->height;
        return slot->block;
    }
    slot->block = NULL;

    th = (textureheight[texnum] >> FRACBITS);
    if (th < 1)
        th = 1;

    // PU_CACHE; user ptr back-references the cache slot so a zone reclaim NULLs
    // slot->raw and the next touch re-transposes. Over-allocate by 7 bytes so
    // the row-major view can be 8-byte aligned -- the RDP DMA that loads this
    // block into TMEM (rdpq_tex_upload) requires an 8-byte-aligned source, and
    // Z_Malloc only guarantees 4-byte alignment (size rounded to 4, z_zone.c
    // :195). A misaligned source silently corrupts the tile (garbled wall).
    slot->raw = Z_Malloc(DL_TILE_W * th + 7, PU_CACHE, (void**)&slot->raw);
    if (!slot->raw)
        return NULL;
    block = (byte*)(((uintptr_t)slot->raw + 7) & ~(uintptr_t)7);

    for (col = 0; col < DL_TILE_W; col++)
    {
        // R_GetColumn masks the column into the texture width; a narrower
        // texture wraps, matching the software sampler's wrap.
        const byte* src = R_GetColumn(texnum, col);
        for (row = 0; row < th; row++)
            block[row * DL_TILE_W + col] = src[row];
    }

    data_cache_hit_writeback(block, DL_TILE_W * th);
    slot->block = block;
    slot->height = th;
    if (out_h) *out_h = th;
    return block;
}

// --- PRIM light LUT (Q8) ---------------------------------------------------
// 32 entries (one per colormap level 0..NUMCOLORMAPS-1). The PRIM colour is the
// colormap's darkening of a near-white reference index, read back through the
// RGB palette (PLAYPAL) -- so TEX0*PRIM reproduces the actual stepped colormap
// falloff curve, not a linear assumption (section 4 caveat). Baked once.
static uint32_t     dl_prim_lut[NUMCOLORMAPS];
static int          dl_prim_inited;

// A bright greyscale reference near the top of the DOOM grey ramp (indices
// 0..31 are the light->dark greys). Index 0 is the brightest light grey; using
// it makes the LUT track the colormap's full darkening range.
#define DL_PRIM_REF 0

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

int DL_EmitWallTier(const rdp_wall_t* w)
{
    if (!w)
        return 0;
    if (dl_wall_count >= DL_WALL_ARENA)
        return 0;       // arena full: drop (correct -- columns stay key-index)
    dl_walls[dl_wall_count++] = *w;
    return 1;
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

    for (i = 0; i < dl_wall_count; i++)
    {
        const rdp_wall_t* w = &dl_walls[i];
        byte*   block;
        int     blkh = 0;
        uint32_t prim;
        float   xl, xr;
        float   t0, t1;     // texel-T range covered by the quad
        int     band_t0;

        block = DL_RowMajorBlock(w->texid, &blkh);
        if (!block || blkh < 1)
            continue;

        // PRIM = colormap-level brightness (Q8). TEX0*PRIM in 1-cycle.
        prim = (w->light < NUMCOLORMAPS) ? dl_prim_lut[w->light]
                                         : 0xFFFFFFFFu;
        rdpq_set_prim_color(color_from_packed32(prim));

        xl = (float)w->x1;
        xr = (float)w->x2 + 1.0f;
        t0 = w->t_top;          // texel row at the wall's TOP screen edge
        t1 = w->t_bot;          // texel row at the wall's BOTTOM screen edge

        // Texel-T span the quad actually covers (order-independent: a bottom-
        // pegged wall can have t0 > t1). Clamp into the block's row range so a
        // band upload never reads past the transposed data.
        {
        float tlo = (t0 < t1) ? t0 : t1;
        float thi = (t0 < t1) ? t1 : t0;
        int   tlo_i, thi_i;

        if (tlo < 0.0f) tlo = 0.0f;
        if (thi > (float)blkh) thi = (float)blkh;
        if (thi <= tlo)
            continue;           // degenerate (no vertical extent) -- skip

        tlo_i = (int)tlo;                       // first texel row touched
        thi_i = (int)(thi + 0.999f);            // one past last texel row
        if (thi_i > blkh) thi_i = blkh;

        // Walk the covered texel rows in DL_TILE_H-row bands. For each band,
        // upload exactly [band_t0, band_t1) rows of the row-major block (<=
        // DL_TILE_H rows => a 64xH CI8 tile never exceeds the 2 KB lower-TMEM
        // half, Q7's hard 64x32 boundary) and draw the slice of the quad whose
        // T falls in that band. The vast majority of single-sided walls cover
        // <=32 texel rows, so this loop runs ONCE in the common Stage-2 case.
        //
        // CRITICAL: rdpq_tex_upload_sub places the loaded rows at their
        // ORIGINAL texture-space addresses in the tile (rdpq_tex.h:128-146 --
        // "draw using texture coordinates contained within the loaded ones"),
        // so the triangle T must be the ORIGINAL texel row (band_t0..band_t1),
        // NOT a band-relative value. (The previous code subtracted band_t0,
        // which sampled the wrong rows on any wall taller than one band and
        // produced missing/garbled wall textures.)
        for (band_t0 = tlo_i; band_t0 < thi_i; band_t0 += DL_TILE_H)
        {
            surface_t       surf;
            rdpq_texparms_t parms;
            int             band_t1 = band_t0 + DL_TILE_H;
            float           bt0, bt1;       // band texel range clamped to quad
            float           f0, f1;         // fractional position along t0..t1
            float           ytl, ytr, ybl, ybr;

            if (band_t1 > thi_i)
                band_t1 = thi_i;

            bt0 = (float)band_t0;
            bt1 = (float)band_t1;
            if (bt0 < tlo) bt0 = tlo;
            if (bt1 > thi) bt1 = thi;
            if (bt1 <= bt0)
                continue;

            surf = surface_make_linear(block, FMT_CI8, DL_TILE_W, blkh);
            memset(&parms, 0, sizeof(parms));
            parms.s.repeats = REPEAT_INFINITE;  // wrap S (texture column)
            parms.t.repeats = 1;                // clamp T (vertical, no bleed)
            rdpq_tex_upload_sub(TILE0, &surf, &parms,
                                0, band_t0, DL_TILE_W, band_t1);

            // Fractional position of this band's [bt0,bt1] texel sub-range along
            // the quad's full t0..t1, so screen Y interpolates to match. f maps
            // a texel row to its 0..1 position from the TOP screen edge (t0).
            {
                float span = t1 - t0;
                if (span > -0.001f && span < 0.001f) span = (span < 0) ? -1.0f : 1.0f;
                f0 = (bt0 - t0) / span;
                f1 = (bt1 - t0) / span;
                // f is the screen-Y blend factor; the texel nearer t0 is the
                // top of this slice on screen regardless of T direction.
                if (f0 > f1) { float s = f0; f0 = f1; f1 = s; }
            }

            // Screen Y at both edges for this band's slice (Y is affine in the
            // texel range for a vertical wall).
            ytl = w->ytop_l + (w->ybot_l - w->ytop_l) * f0;
            ybl = w->ytop_l + (w->ybot_l - w->ytop_l) * f1;
            ytr = w->ytop_r + (w->ybot_r - w->ytop_r) * f0;
            ybr = w->ytop_r + (w->ybot_r - w->ytop_r) * f1;

            // T at the screen-top / screen-bottom of this slice, in ORIGINAL
            // texel coordinates (the band loaded rows live at their own T).
            {
                float t_screen_top = (t0 <= t1) ? bt0 : bt1;
                float t_screen_bot = (t0 <= t1) ? bt1 : bt0;

                float tl[5] = { xl, ytl, w->s_l, t_screen_top, w->invw_l };
                float tr[5] = { xr, ytr, w->s_r, t_screen_top, w->invw_r };
                float bl[5] = { xl, ybl, w->s_l, t_screen_bot, w->invw_l };
                float br[5] = { xr, ybr, w->s_r, t_screen_bot, w->invw_r };

                rdpq_triangle(&TRIFMT_TEX, tl, tr, bl);
                rdpq_triangle(&TRIFMT_TEX, tr, br, bl);
            }
        }
        }
    }
}

#endif // N64
