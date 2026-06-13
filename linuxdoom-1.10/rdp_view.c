// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// RDP renderer -- frame display-list emit + flush (Stage 3).
//
// DESCRIPTION:
//   Stage 3 of the RDP renderer (Docs/RDP_RENDERER_DESIGN.md staging plan).
//   Routes ALL solid wall tiers through the new RDP wall path, behind the
//   n64_use_rdp_renderer flag: every single-sided (midtexture) seg AND the
//   upper/lower textures of two-sided segs. Masked mid-textures and sky/planes
//   stay CPU (Stages 4/5).
//
//   This file owns: the per-frame emit arena + per-texture buckets, the
//   rdp_wall_t record type, DL_BeginFrame/DL_EmitWallTier/DL_Flush, the free-W
//   proportionality constant k (Q1), the on-demand column-major->row-major CI8
//   transpose cache (Q10), and the 32-entry PRIM light LUT baked from the
//   colormap ramp (Q8).
//
//   DL_Flush walks the touched textures one at a time: ONE rdpq_tex_upload per
//   texture per band-set, then all that texture's quads, then the next texture
//   -- collapsing autosync TMEM/pipe thrash from per-seg (Stage 2) to
//   per-texture (Q6). On arena overflow it falls back to an un-bucketed direct
//   draw of the overflow records (slower, correct, never crashes).
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
// Defaults to the RDP path so Stage 3 exercises the new wall pipeline. The task
// allows defaulting this to CPU (0) if walls render wrong and cannot be fixed
// within the iteration budget.
int n64_rdp_wall_ab = 1;

// --- emit arena + per-texture buckets --------------------------------------
// Stage 3 routes ALL solid wall tiers. Each tier splits into a record per
// light-level run AND per S-span run (the saturation split in DL_RouteEmit), so
// the whole frame's walls can produce hundreds of records. Size for the
// documented tail (drawsegs=47) x ~3 tiers x a few runs each, with margin. On
// overflow DL_EmitWallTier flags the arena full; DL_Flush still draws every
// queued record (the overflow records simply were never queued -- their columns
// stay key-index inside the keyed box, a punched hole over stale fb, the worst
// artifact class), so the arena should be effectively unfillable in practice.
//
// 512 records x sizeof(rdp_wall_t) (~64 B) = ~32 KB static -- comfortable.
#define DL_WALL_ARENA   512

static rdp_wall_t   dl_walls[DL_WALL_ARENA];
static int          dl_wall_count;
static int          dl_arena_overflow;     // 1 if a record was dropped this frame

// Per-texture buckets (Q6). Each touched texnum keeps a singly-linked chain of
// its records THROUGH the arena (rdp_wall_t.bucket_next), head/tail indices in
// dl_bucket_head/tail. DL_Flush walks the touched-texture list, and for each
// texture uploads its tile ONCE per band-set then draws every record in the
// chain -- so a texture used by N segs uploads once, not N times. The
// touched-texture list (dl_touched[]) is the sparse set the design's
// tex_bucket_t array stands for: we keep the heads dense in a numtextures-sized
// array (cleared lazily via a per-frame generation stamp so BeginFrame stays
// O(touched), not O(numtextures)).
static int32_t*     dl_bucket_head;         // [numtextures] first record idx, -1 none
static uint32_t*    dl_bucket_gen;          // [numtextures] frame stamp of head
static uint32_t     dl_frame_gen;           // bumped each DL_BeginFrame
static int          dl_buckets_inited;
static uint16_t     dl_touched[DL_WALL_ARENA]; // texnums touched this frame (deduped)
static int          dl_touched_count;

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
    uint32_t lastuse;   // present generation of the last DL_MarkInFlight
    uint8_t  pinned;    // currently PU_STATIC for an in-flight window
} dl_rowmajor_t;

static dl_rowmajor_t* dl_rowmajor;      // [numtextures]
static int            dl_rowmajor_inited;

// Present generation counter for the in-flight pin/demote schedule (see the
// in-flight block tracking section below). Declared here because fresh
// transposes in DL_RowMajorBlock stamp their slot with it.
static uint32_t       dl_present_gen;

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
// into it. Blocks marked in-flight by DL_Flush carry a per-slot present-
// generation stamp (see DL_MarkInFlight / DL_PresentEnd).
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
    // The raw LOAD_TILE path addresses source rows in texture-period-relative
    // coordinates, whose fixed-point encoding caps at 1024 texels
    // (rdpq_load_tile); no stock DOOM texture is taller than 128, so this
    // refusal is theoretical (custom-WAD guard).
    if (th > 1023)
        return NULL;

    // The transpose width is the texture's power-of-two sampling period
    // (R_GetColumn masks with texturewidthmask), NOT a fixed 64: see BUG-D note
    // above. Wider periods than 512 can't be addressed by LOAD_TILE (S
    // coordinate caps at 1024) and tile at <= 4 TMEM rows anyway; narrower
    // than 8 break band alignment -- both refuse to route (CPU keeps the seg).
    tw = texturewidthmask[texnum] + 1;
    if (tw < DL_MIN_TEX_W || tw > 512)
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
    // Fresh blocks are born PU_STATIC: record that as a pin in the current
    // present generation so DL_PresentEnd's sweep demotes them on the same
    // schedule as re-pinned blocks (see the in-flight tracking below).
    slot->pinned = 1;
    slot->lastuse = dl_present_gen;
    if (out_h) *out_h = th;
    if (out_w) *out_w = tw;
    return block;
}

// --- in-flight block tracking (PU_CACHE async-read race) -------------------
// Per-slot generation stamps replace the former fixed 2x16-entry pending/
// previous lists (Stage-3 insurance: the lists overflowed silently past 16
// distinct textures per present, leaving overflow blocks pinned PU_STATIC
// FOREVER -- harmless under Stage 2's one-seg routing, a permanent zone leak
// once Stage 3 routes arbitrarily many textures). dl_present_gen counts
// presents (declared above the transpose cache, which also stamps it);
// DL_MarkInFlight stamps the slot with the current generation and pins it;
// DL_PresentEnd -- called AFTER the present's buffer-flip spin, which proves
// the PREVIOUS present's RDP work fully drained -- demotes every pinned
// block whose stamp is older than the present just enqueued (i.e. last used
// by a present that has provably drained), then advances the generation.
// Semantics are identical to the old list rotation, with no capacity limit:
// O(numtextures) == 125 slot reads per present, negligible.

static void DL_MarkInFlight(int texnum)
{
    dl_rowmajor_t* slot;

    if (!dl_rowmajor)
        return;
    slot = &dl_rowmajor[texnum];
    if (!slot->raw)
        return;
    if (!slot->pinned)
    {
        // (Re-)pin: a previously demoted block goes back to PU_STATIC for
        // the duration of its in-flight window.
        Z_ChangeTag(slot->raw, PU_STATIC);
        slot->pinned = 1;
    }
    slot->lastuse = dl_present_gen;
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

// Allocate the per-texture bucket arrays once numtextures is known. Lazy (the
// transpose cache uses the same trigger) so it survives a level load that grows
// numtextures, and is a no-op if numtextures is not yet set.
static void DL_InitBuckets(void)
{
    if (dl_buckets_inited)
        return;
    if (numtextures <= 0)
        return;
    dl_bucket_head = (int32_t*)Z_Malloc(numtextures * sizeof(int32_t),
                                        PU_STATIC, 0);
    dl_bucket_gen  = (uint32_t*)Z_Malloc(numtextures * sizeof(uint32_t),
                                         PU_STATIC, 0);
    memset(dl_bucket_gen, 0, numtextures * sizeof(uint32_t));
    dl_frame_gen = 1;   // 0 is the cleared-stamp sentinel; start at 1
    dl_buckets_inited = 1;
}

void DL_BeginFrame(void)
{
    dl_wall_count = 0;
    dl_arena_overflow = 0;
    dl_touched_count = 0;

    // Per-frame generation bump invalidates every bucket head/tail in O(1):
    // a bucket whose gen stamp != dl_frame_gen is treated as empty, so the
    // numtextures-sized head/tail arrays never need a per-frame memset (only
    // the small dl_touched[] list is walked at flush). Wrap is benign: gen 0
    // is the sentinel, so on the rare 2^32 wrap we re-base to 1 and clear once.
    DL_InitBuckets();
    if (++dl_frame_gen == 0)
    {
        if (dl_bucket_gen)
            memset(dl_bucket_gen, 0, numtextures * sizeof(uint32_t));
        dl_frame_gen = 1;
    }

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

// One-entry memo on the resolved colormap pointer. The per-column light level
// is a pointer-difference DIVIDE (cm - colormaps)/256; within a seg the scale
// (hence index, hence cm) is monotonic and changes only at LIGHTSCALESHIFT
// quantization boundaries, so adjacent drawn columns repeatedly resolve to the
// SAME cm. Caching the last (cm -> level) skips the divide for every column in
// a light-level run -- byte-identical output (the cached level is exactly what
// the divide would recompute), and the cache is keyed on cm so a colormap or
// walllights change (NULL cm, palette reload) misses and recomputes. Reset on
// numtextures setup is unnecessary: cm pointers are stable for the level's
// lifetime and a stale-but-equal pointer would yield the same level anyway.
static const lighttable_t* dl_lll_cm    = NULL;
static uint8_t             dl_lll_level = 0;

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

    if (cm == dl_lll_cm)
        return dl_lll_level;

    // walllights[index] == colormaps + level*256.
    level = (cm - colormaps) / 256;
    if (level < 0)
        level = 0;
    if (level >= NUMCOLORMAPS)
        level = NUMCOLORMAPS - 1;

    dl_lll_cm    = cm;
    dl_lll_level = (uint8_t)level;
    return (uint8_t)level;
}

#if DL_DEBUG_TRACE
static int dl_drop_count;       // records dropped on arena overflow this frame
static int dl_present_no;       // diagnostic present counter
#endif

int DL_EmitWallTier(const rdp_wall_t* w)
{
    int         idx;
    int         tex;
    rdp_wall_t* rec;

    if (!w)
        return 0;
    if (dl_wall_count >= DL_WALL_ARENA)
    {
        dl_arena_overflow = 1;      // DL_Flush's overflow note + correctness rely on this
#if DL_DEBUG_TRACE
        dl_drop_count++;
#endif
        return 0;       // arena full: drop (correct -- columns stay key-index)
    }

    idx = dl_wall_count++;
    rec = &dl_walls[idx];
    *rec = *w;
    rec->bucket_next = -1;

    // Append to the texture's bucket (Q6). If the bucket arrays could not be
    // allocated (numtextures not yet known), the record is still in the arena
    // and DL_Flush's bucket walk simply won't reach it -- but that only happens
    // before any level is loaded, where no seg routes, so it is unreachable in
    // practice. Guard anyway.
    tex = w->texid;
    if (dl_bucket_head && tex >= 0 && tex < numtextures)
    {
        if (dl_bucket_gen[tex] != dl_frame_gen)
        {
            // First record for this texture this frame: open the bucket and
            // record it in the sparse touched-texture list (deduped by the gen
            // stamp, so each texnum appears in dl_touched[] at most once).
            dl_bucket_gen[tex]  = dl_frame_gen;
            dl_bucket_head[tex] = idx;
            if (dl_touched_count < DL_WALL_ARENA)
                dl_touched[dl_touched_count++] = (uint16_t)tex;
        }
        else
        {
            // HEAD-insert (back-to-front draw order). The BSP walk emits
            // front-to-back; head insertion makes DL_Flush draw each
            // texture's records in REVERSE emit order, i.e. farthest first.
            // Records never overlap on the columns software assigned them,
            // but a record's quad over-covers its captured spans by a couple
            // of rows (coverage bias) -- where that over-coverage lands on
            // ANOTHER tier's key-revealed span, painter order decides which
            // texels survive. Back-to-front resolves those overlaps to the
            // NEARER record, matching occlusion (the stairs/pillars
            // stacked-tier scene is the worst case: far risers stomping near
            // ones reads as vanished geometry).
            rec->bucket_next    = dl_bucket_head[tex];
            dl_bucket_head[tex] = idx;
        }
    }
    return 1;
}

// --- routed per-tier per-column capture ------------------------------------
// All routed capture lives here (not in R_RenderSegLoop) so the flag-OFF
// seg-loop translation unit stays at the pre-RDP baseline .text size. The seg
// loop feeds each drawn wall-tier column to DL_RouteCapture (tagged by tier)
// and calls DL_RouteEmit once per tier after the column loop -- but ONLY when
// the kill-switch is on, so none of this is reachable with the flag off.
//
// THREE tier streams (mid/top/bottom). A two-sided seg draws its top and bottom
// tiers across overlapping column ranges with DIFFERENT screen Y spans and
// DIFFERENT texturemids, so each tier needs its own per-column capture; a
// single-sided seg uses only the mid stream. scale/texcol/light are physically
// shared across tiers in a column, but storing them per tier keeps the emit
// walk a single uniform routine reading one tier's arrays.
static short         dl_rt_yl   [DL_TIER_COUNT][SCREENWIDTH];
static short         dl_rt_yh   [DL_TIER_COUNT][SCREENWIDTH];
static fixed_t       dl_rt_scale[DL_TIER_COUNT][SCREENWIDTH];
static fixed_t       dl_rt_scol [DL_TIER_COUNT][SCREENWIDTH];   // texturecolumn
static unsigned char dl_rt_lit  [DL_TIER_COUNT][SCREENWIDTH];   // colormap level
static unsigned char dl_rt_drawn[DL_TIER_COUNT][SCREENWIDTH];   // 1 if drawn here
static int           dl_rt_first[DL_TIER_COUNT];                // first drawn col (-1)
static int           dl_rt_last [DL_TIER_COUNT];                // last drawn col

// SEG_RASTER capture-bookkeeping tax cut: clear only the PREVIOUS seg's drawn
// span per tier, not the full ~960 bytes/tier every seg. The only cells that
// can be stale are those a prior DL_RouteCapture set to 1, and capture sets
// drawn=1 strictly inside that seg's recorded [first..last]; clearing that span
// is equivalent to the old full-width memset (every other cell is already 0).
void DL_RouteBeginSeg(void)
{
    int t;
    for (t = 0; t < DL_TIER_COUNT; t++)
    {
        // Clear only the PREVIOUS seg's drawn span for this tier. A column the
        // upcoming seg skips (yl > yh, never captured) would otherwise keep a
        // STALE drawn=1 (and stale yl/yh/scale/texturecolumn) from an earlier
        // seg -- the emit walk reads those stale cells for run-break decisions
        // and as run endpoints, producing warped/misplaced quads. Only cells a
        // prior DL_RouteCapture set to 1 can be stale, and capture sets drawn=1
        // strictly inside that seg's recorded [first..last], so clearing that
        // span is equivalent to the old full-width memset (every other cell is
        // already 0) at a fraction of the cost. Per-seg (not per-frame) so runs
        // from segs drawn earlier this frame never bleed into a later seg.
        // The prior seg's drawn span for this tier is exactly its [first..last]
        // (capture advances last only on drawn=1 columns). Clear that, then arm
        // the precondition for the upcoming seg.
        if (dl_rt_first[t] >= 0)
        {
            int span = dl_rt_last[t] - dl_rt_first[t] + 1;
            memset(&dl_rt_drawn[t][dl_rt_first[t]], 0, (size_t)span);
        }
        dl_rt_first[t] = -1;
        dl_rt_last[t]  = -1;
    }
}

void DL_RouteCapture(int tier, int x, int yl, int yh, fixed_t scale,
                     fixed_t texcol, const void* const* walllights)
{
    if ((unsigned)tier >= DL_TIER_COUNT)
        return;
    if (x < 0 || x >= SCREENWIDTH)
        return;
    if (yl <= yh)
    {
        dl_rt_yl[tier][x]    = (short)yl;
        dl_rt_yh[tier][x]    = (short)yh;
        dl_rt_scale[tier][x] = scale;
        dl_rt_scol[tier][x]  = texcol;
        dl_rt_lit[tier][x]   = DL_WallLightLevel(walllights,
                                  (unsigned)(scale >> LIGHTSCALESHIFT));
        dl_rt_drawn[tier][x] = 1;
        if (dl_rt_first[tier] < 0) dl_rt_first[tier] = x;
        dl_rt_last[tier] = x;
    }
    else
    {
        dl_rt_drawn[tier][x] = 0;
    }
}

// Coalesce the captured columns into rdp_wall_t records, splitting at columns
// where the colormap light level changes (per light-level run -- preserves
// vanilla's per-column light banding as long contiguous runs; Q8 mitigation).
//
// COVERAGE RULE (the wall/plane junction-gap fix): every suppressed CPU pixel
// -- exactly the per-column [yl..yh] spans, with software's own >>HEIGHTBITS
// truncation -- MUST be covered by an emitted quad, or it stays key-index and
// the keyed present blit punches a hole at the junction (green/stale gap rows
// between the wall and its CPU-drawn ceiling/floor). OVER-coverage outside
// the suppressed spans is repainted by the blit wherever the CI8 holds
// non-key art -- but where it lands on ANOTHER record's key-revealed span,
// painter order decides whose texels survive, so over-coverage must be SMALL.
// UNDER-coverage is the only sin; large over-coverage is the second sin (the
// Stage-3 bounding-RECTANGLE emit over-covered by the full per-run yl/yh
// variation -- tens of rows on stacked stair/pillar micro-tiers -- and its
// far-record stomps read as vanished geometry).
//
// DL_EmitRunPiece (below) emits each run as a TRAPEZOID through the run's
// endpoint-column samples (corners extrapolated half a pixel outward so the
// rasterizer's interpolation passes through the sampled values at the end
// columns' centers), VERIFIES per column that the quad's interpolation tracks
// the captured yl/yh (rows) and texturecolumn (S, projectively), SPLITS the
// run where it deviates (clip-deformed spans, scale-clamp plateaus on
// glancing walls), and biases the final edges by the measured deviation so
// coverage of every captured span is guaranteed with over-coverage bounded by
// the split thresholds (~1-2 rows).
//
// T is computed PER CORNER from that corner's own 1/scale (T at a shared
// screen row differs between edges when scale differs). With per-corner T and
// INV_W = scale*k, T/W and 1/W are affine in screen space, so the perspective
// interpolation reproduces software's per-column T = mid + (y-cy)/scale(x)
// exactly (DOOM itself lerps scale linearly per column); corners above/below
// a column's drawn span just extend the same projective map, so texel rows
// stay aligned with software wherever pixels survive the blit. T is float
// end-to-end (texel units), which also removes the 32-bit (y-cy)*iscale
// overflow class the old fixed-point corner math inherited from vanilla.

// Split thresholds. Y: deviation beyond ~a row means the lerped edge cannot
// represent the truncated/clipped span shape -- split. S: the floor admits
// the sub-texel residue of projective math; the 0.51*step term admits the
// irreducible half-column anchor uncertainty at glancing minification (an S
// error under half the local per-column S step shifts sampling by less than
// software's own column quantization).
#define DL_SPLIT_DEVY   1.25f
#define DL_SPLIT_DEVS   1.5f

static void DL_EmitRunPiece(int tier, int xa, int xb, fixed_t mid, int texnum,
                            int cy, float k, int depth)
{
    const short*   t_yl    = dl_rt_yl[tier];
    const short*   t_yh    = dl_rt_yh[tier];
    const fixed_t* t_scale = dl_rt_scale[tier];
    const fixed_t* t_scol  = dl_rt_scol[tier];

    float s_l, s_r;             // corner S (texels)
    float invw_l, invw_r;       // corner 1/w
    float ytl, ytr, ybl, ybr;   // corner Y edges, pre-bias (ybot is yh+1)
    float width = (float)(xb + 1 - xa);
    float devtop = 0.0f;        // max signed under-coverage at the top edge
    float devbot = 0.0f;        // max signed under-coverage at the bottom edge
    rdp_wall_t w;

    // Corner attributes. Per-column samples apply at pixel CENTERS (x+0.5);
    // the quad corners sit at the pixel EDGES xa and xb+1, half a pixel
    // outward, so extrapolate each corner along its end column's local
    // per-column step. A width-1 run gets the column's own constant values --
    // exactly software's single source column.
    {
        float sl0 = (float)t_scol[xa],          sr0 = (float)t_scol[xb];
        float wl0 = (float)t_scale[xa] * k,     wr0 = (float)t_scale[xb] * k;
        float tl0 = (float)t_yl[xa],            tr0 = (float)t_yl[xb];
        float bl0 = (float)t_yh[xa] + 1.0f,     br0 = (float)t_yh[xb] + 1.0f;

        if (xb > xa)
        {
            sl0 -= 0.5f * ((float)t_scol[xa + 1] - sl0);
            sr0 += 0.5f * (sr0 - (float)t_scol[xb - 1]);
            wl0 -= 0.5f * ((float)t_scale[xa + 1] * k - wl0);
            wr0 += 0.5f * (wr0 - (float)t_scale[xb - 1] * k);
            tl0 -= 0.5f * ((float)t_yl[xa + 1] - tl0);
            tr0 += 0.5f * (tr0 - (float)t_yl[xb - 1]);
            bl0 -= 0.5f * (((float)t_yh[xa + 1] + 1.0f) - bl0);
            br0 += 0.5f * (br0 - ((float)t_yh[xb - 1] + 1.0f));
        }
        if (wl0 < 1e-6f) wl0 = 1e-6f;
        if (wr0 < 1e-6f) wr0 = 1e-6f;
        s_l = sl0;  s_r = sr0;
        invw_l = wl0;  invw_r = wr0;
        ytl = tl0;  ytr = tr0;
        ybl = bl0;  ybr = br0;
    }

    // Deviation scan: compare the quad's interpolation (Y edges linear, S
    // projective via s/w over 1/w -- exactly what the rasterizer computes) at
    // every pixel center against the captured per-column values. Width-1/2
    // pieces are exact by construction (the lerp through two extrapolated
    // corners passes through both column samples), so they skip the scan --
    // and they are the recursion's fixed point. The scan EARLY-EXITS at the
    // first threshold-crossing column and splits there (sub-pieces re-verify
    // themselves), so failing pieces pay only a partial scan; the S split
    // threshold needs the run's max per-column S step, which a cheap integer
    // prepass provides.
    if (xb > xa + 1)
    {
        float swl = s_l * invw_l, swr = s_r * invw_r;
        float sthresh;
        int   x;

        {
            // S split tolerance scales with the run's max per-column S step
            // (0.51*maxstep), floored at DL_SPLIT_DEVS. RATIONALE (orchestrator
            // pixel A/B, post-c1849d6): tightening this to a FIXED sub-texel
            // tolerance (c1849d6) shattered glancing pieces to width-1 to chase
            // a trace metric (S residual 18->1.5 texel) but produced ZERO
            // visible change on the glancing-wall smear -- pre/post-ON A/B of
            // the SAME scene+marker (frame-4096, bottom-right 3x crop) measured
            // 0.53/255 mean abs diff, i.e. the optimized metric did not
            // correspond to the visible defect. c1849d6 cost +1091us and +3.6%
            // emit volume for no benefit, so the adaptive threshold is restored
            // (visually free per the A/B, recovers the emit/dlbuild cost it
            // added). The smear remains an OPEN S-domain item independent of
            // this threshold; this change does not make it worse.
            fixed_t maxstep = 1;
            for (x = xa; x < xb; x++)
            {
                fixed_t st = t_scol[x + 1] - t_scol[x];
                if (st < 0) st = -st;
                if (st > maxstep) maxstep = st;
            }
            sthresh = 0.51f * (float)maxstep;
            if (sthresh < DL_SPLIT_DEVS)
                sthresh = DL_SPLIT_DEVS;
        }

        for (x = xa; x <= xb; x++)
        {
            float f  = ((float)x + 0.5f - (float)xa) / width;
            float wm = invw_l + (invw_r - invw_l) * f;
            float sp = (swl + (swr - swl) * f) / wm;
            float lt = ytl + (ytr - ytl) * f;
            float lb = ybl + (ybr - ybl) * f;
            float dS = sp - (float)t_scol[x];
            float dT = lt - (float)t_yl[x];                 // >0: edge too low
            float dB = ((float)t_yh[x] + 1.0f) - lb;        // >0: edge too high
            float aS = (dS < 0.0f) ? -dS : dS;
            float aT = (dT < 0.0f) ? -dT : dT;
            float aB = (dB < 0.0f) ? -dB : dB;

            if ((aT > DL_SPLIT_DEVY || aB > DL_SPLIT_DEVY || aS > sthresh)
                && depth < 10)
            {
                // Split at the first deviating column. Keep both halves
                // non-empty so the recursion always shrinks.
                int xm = (x >= xb) ? (xb - 1) : ((x > xa) ? x : xa);
                DL_EmitRunPiece(tier, xa, xm, mid, texnum, cy, k, depth + 1);
                DL_EmitRunPiece(tier, xm + 1, xb, mid, texnum, cy, k,
                                depth + 1);
                return;
            }
            if (dT > devtop) devtop = dT;
            if (dB > devbot) devbot = dB;
        }
    }
    else if (xb > xa)
    {
        // Width-2: the corner extrapolation makes the edge lerps pass through
        // both column samples exactly for Y; only the half-row sampling margin
        // remains, which the 0.49 coverage slack absorbs.
        float dT = ytl + (ytr - ytl) * 0.25f - (float)t_yl[xa];
        float dB = ((float)t_yh[xa] + 1.0f) - (ybl + (ybr - ybl) * 0.25f);
        float dT2 = ytl + (ytr - ytl) * 0.75f - (float)t_yl[xb];
        float dB2 = ((float)t_yh[xb] + 1.0f) - (ybl + (ybr - ybl) * 0.75f);
        if (dT > devtop) devtop = dT;
        if (dT2 > devtop) devtop = dT2;
        if (dB > devbot) devbot = dB;
        if (dB2 > devbot) devbot = dB2;
        // (c1849d6's width-2 S-split removed with the adaptive-threshold
        // restore above: it shattered glancing width-2 pieces to two width-1
        // pieces for the same trace metric that showed no visible change.)
    }

    // Coverage bias: raise/lower the whole edge by the measured worst
    // under-coverage so every captured span row is rasterized (pixel-center
    // rule: top edge <= yl+0.5, bottom edge > yh+0.5). Over-coverage stays
    // bounded by the split thresholds (~1-2 rows) -- small enough that a
    // neighbouring tier's key span loses at most an edge row to painter
    // order (drawn back-to-front, the nearer record wins).
    {
        float bias_top = devtop - 0.49f;
        float bias_bot = devbot - 0.49f;

        if (bias_top < 0.0f) bias_top = 0.0f;
        if (bias_bot < 0.0f) bias_bot = 0.0f;
        ytl -= bias_top;
        ytr -= bias_top;
        ybl += bias_bot;
        ybr += bias_bot;
    }
    // Degenerate-edge guard (wild clip steps at a piece boundary could fold
    // an extrapolated edge): keep each edge's T span strictly positive for
    // the per-edge T division and the band-slice lerps.
    if (ybl < ytl + 0.05f) ybl = ytl + 0.05f;
    if (ybr < ytr + 0.05f) ybr = ytr + 0.05f;

    w.x1 = (int16_t)xa;
    w.x2 = (int16_t)xb;
    w.ytop_l = ytl;
    w.ybot_l = ybl;
    w.ytop_r = ytr;
    w.ybot_r = ybr;
    w.s_l = s_l;
    w.s_r = s_r;
    w.invw_l = invw_l;
    w.invw_r = invw_r;

    // Per-corner T through each edge's own 1/scale, float end-to-end in texel
    // units (65536/scale = texels per screen row; no 32-bit overflow).
    {
        float midf = (float)mid * (1.0f / (float)FRACUNIT);
        float il = 65536.0f / (float)t_scale[xa];
        float ir = 65536.0f / (float)t_scale[xb];

        w.t_top_l = midf + (ytl - (float)cy) * il;
        w.t_bot_l = midf + (ybl - (float)cy) * il;
        w.t_top_r = midf + (ytr - (float)cy) * ir;
        w.t_bot_r = midf + (ybr - (float)cy) * ir;
    }

    w.texid = (uint16_t)texnum;
    w.light = dl_rt_lit[tier][xa];

    DL_EmitWallTier(&w);
}
void DL_RouteEmit(int tier, fixed_t mid, int texnum, int centery)
{
    const float     k = dl_invw_k;
    const int       cy = centery;
    int             run0 = -1;
    int             x;

    // This tier's per-column capture arrays (run-break inputs only; the
    // geometry arrays are read by DL_EmitRunPiece). A two-sided seg emits its
    // top and bottom tiers from independent streams; a single-sided seg only
    // its mid.
    const fixed_t*       t_scol  = dl_rt_scol[tier];
    const unsigned char* t_lit   = dl_rt_lit[tier];
    const unsigned char* t_drawn = dl_rt_drawn[tier];
    const int            first   = dl_rt_first[tier];
    const int            last    = dl_rt_last[tier];

    if ((unsigned)tier >= DL_TIER_COUNT)
        return;
    if (first < 0)
        return;

    for (x = first; x <= last + 1; x++)
    {
        int drawn = (x <= last) ? t_drawn[x] : 0;
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
        else if (t_lit[x] != t_lit[run0])
            breakrun = 1;
        // ... or when the S (texturecolumn) span exceeds the fixed-point safe
        // range. rdpq_triangle's S/T attribute setup saturates at |S| >= 1024
        // texels (s*32 through float_to_s16_16, rdpq_tri.c) -- a saturated S
        // smears the texture across the whole quad. DL_Flush biases the run's
        // S endpoints down by a shared multiple of the texture period (so the
        // smaller endpoint lands in [0, width)), which bounds |S| by
        // width + span; with span capped at 700 and width <= 256 that is < 1024
        // always. Long offset segs simply split into more quads.
        else if (t_scol[x] - t_scol[run0] > 700
                 || t_scol[run0] - t_scol[x] > 700)
            breakrun = 1;

        if (breakrun)
        {
            // Emit the run [run0 .. x-1] as one or more verified trapezoids
            // (DL_EmitRunPiece: deviation-checked, split where the captured
            // spans deviate, coverage-biased -- see the COVERAGE RULE above).
            DL_EmitRunPiece(tier, run0, x - 1, mid, texnum, cy, k, 0);

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

int DL_WallRouteOn(void)
{
    if (!n64_use_rdp_renderer)
        return 0;
    if (!n64_rdp_wall_ab)
        return 0;       // A/B toggle: keep walls on the CPU
    return 1;
}

// --- per-record draw (the validated Stage-2 band/T/S machinery) -------------
// Draw ONE wall record: PRIM from the light LUT, then the texel-T band walk
// (tile uploads + 2 triangles per band). Factored out of DL_Flush so the
// Stage-3 per-texture bucket walk can call it for every record in a bucket
// after the bucket's first upload, keeping the band/S/T math byte-identical to
// Stage 2. Returns nonzero if the record drew (had a usable transpose block).
//
// block/blkh/blkw are the record's transpose block (already fetched + pinned by
// the caller). The caller owns rdpq mode/combiner/TLUT/persp setup.
//
// UPLOAD DEDUP (Stage-3 autosync collapse): the band walk uploads a tile-local
// CI8 strip per band. Within a texture's bucket many records sit at similar
// depth/T and resolve to the SAME source band (same block ptr + first row +
// row count), so the previous band already loaded into TMEM is reusable. We
// cache the last upload's signature and skip the redundant rdpq_tex_upload (and
// its AUTOSYNC_TILE/LOAD) when it matches -- the single largest DL_BUILD cost.
// The cache is reset at the top of every DL_Flush (dl_last_up_block=NULL) so it
// never carries a stale TMEM assumption across present seams.
static const byte* dl_last_up_block;
static int         dl_last_up_lo;
static int         dl_last_up_rows;
static int         dl_last_up_c0;      // S window origin of the resident band
// Tile-descriptor state (ADAPTIVE S WINDOW): the tile pitch/clamp now vary per
// record (see the window note in DL_DrawRecord), so SET_TILE moved from
// per-texture (DL_Flush) into the band path, deduped on (width, wrap). Reset
// alongside the upload dedup at every flush and at every texture switch (the
// wrap mask depends on the texture period).
static int         dl_last_tile_lw;
static int         dl_last_tile_wrap;
// PRIM-color dedup. PRIM = the colormap-level brightness (TEX0*PRIM in 1-cycle).
// Consecutive records in a texture bucket frequently share a light level (the
// emit splits runs AT light boundaries, so a multi-record run that split for S
// or deviation reasons keeps one light), so the per-record rdpq_set_prim_color
// is usually redundant. Cache the last packed PRIM and skip the set when
// unchanged -- one fewer RDP command + its command-gen per record, sampling-
// identical. 0 = "no prim set yet this flush" (a real prim is never 0: the LUT
// entries are colormap brightness and the unlit fallback is 0xFFFFFFFF).
static uint32_t    dl_last_prim;

static int DL_DrawRecord(const rdp_wall_t* w, byte* block, int blkh, int blkw)
{
    int     cap;            // max tile rows fitting the lower TMEM half
    int     lw;             // adaptive S window width (texels; pow2 <= blkw)
    int     c0;             // S window origin within the period (0 if full)
    int     wrap;           // 1: full-period tile, hardware S wrap
    uint32_t prim;
    float   xl, xr;
    float   s_l, s_r;       // S endpoints, period-bias-reduced (BUG D)
    float   tl0, tl1;       // left-edge texel-T range (top..bottom)
    float   tr0, tr1;       // right-edge texel-T range (top..bottom)
    float   t0, t1;         // union texel-T range covered by the quad

    if (blkw < 1)
        return 0;

    // PRIM = colormap-level brightness (Q8). TEX0*PRIM in 1-cycle. Deduped:
    // skip the set when this record's light matches the resident PRIM.
    prim = (w->light < NUMCOLORMAPS) ? dl_prim_lut[w->light] : 0xFFFFFFFFu;
    if (prim != dl_last_prim)
    {
        rdpq_set_prim_color(color_from_packed32(prim));
        dl_last_prim = prim;
    }

    xl = (float)w->x1;
    xr = (float)w->x2 + 1.0f;

    // S bias reduction (BUG D, saturation guard): texturecolumn is unbounded
    // (rw_offset + tangent term), but rdpq_triangle's attribute fixed point
    // saturates at |S| >= 1024 texels, smearing the texture. The tile wraps S
    // with mask log2(blkw), so subtracting a SHARED whole number of periods from
    // both endpoints is sampling-identical; after the bias the smaller endpoint
    // lies in [0, blkw) and the emit-side run split caps |s_r - s_l| at 700, so
    // |S| < blkw + 700 < 1024.
    {
        float smin = (w->s_l < w->s_r) ? w->s_l : w->s_r;
        float bias = floorf(smin / (float)blkw) * (float)blkw;
        s_l = w->s_l - bias;
        s_r = w->s_r - bias;
    }

    // ADAPTIVE S WINDOW (band/triangle collapse). The TMEM band cap is
    // 2048/tile_width rows, so wide textures band-split aggressively (a
    // 128-wide texture caps at 16 rows: a full-height wall = 8 bands = 16
    // triangles PER RECORD), and the triangle/band volume -- not the upload
    // helper -- is what keeps DL_BUILD heavy. Most records sample a NARROW S
    // range (run width in columns ~ texels at normal angles), so when the
    // biased S range [smin,smax] fits inside one texture period, load only a
    // power-of-two S window [c0, c0+lw) of the block instead of full rows:
    // cap doubles per halving of lw, collapsing the band count (typical
    // 30-px run on a 128-wide texture: 8 bands -> 2). Sampling is identical:
    // the window covers [floor(smin)-1, ceil(smax)+1] (a margin texel each
    // side), the tile CLAMPS S at the window edges it never reaches, and
    // triangle S values are unchanged (LOAD_TILE keeps original coords).
    // Records whose S range crosses a period seam need the hardware wrap and
    // keep the full-width path (mask = log2(blkw)).
    {
        float smin = (s_l < s_r) ? s_l : s_r;
        float smax = (s_l < s_r) ? s_r : s_l;
        int   ilo = (int)floorf(smin) - 1;
        int   ihi = (int)ceilf(smax) + 1;

        lw = blkw;
        c0 = 0;
        wrap = 1;
        if (ilo < 0)
            ilo = 0;
        if (ihi < blkw)
        {
            int need = ihi - ilo + 1;
            int w2 = 8;
            while (w2 < need)
                w2 <<= 1;
            if (w2 < blkw)
            {
                lw = w2;
                wrap = 0;
                c0 = ilo;
                if (c0 + lw > blkw)
                    c0 = blkw - lw;
            }
        }
    }

    // Per-width TMEM tile cap: lw bytes/row (CI8) against the lower 2 KB TMEM
    // half beside the resident TLUT (64-wide: 32 rows, 128-wide: 16,
    // 256-wide: 8 -- Q7; the adaptive window above raises it further).
    cap = DL_TMEM_HALF / lw;
    if (cap < 1)
        return 0;

    // Configure TILE0 for this record's window geometry (deduped: most
    // consecutive records of a texture share lw/wrap). Pitch = lw bytes
    // (pow2 >= 8). Wrap mode masks S with the texture period; window mode
    // clamps (mask 0 forces clamp). T always clamps at the loaded band rows.
    // A tile change invalidates the resident band (TMEM layout changed).
    if (lw != dl_last_tile_lw || wrap != dl_last_tile_wrap)
    {
        rdpq_tileparms_t tp;

        memset(&tp, 0, sizeof(tp));
        if (wrap)
        {
            int maskbits = 0;
            int wbit;
            for (wbit = blkw; wbit > 1; wbit >>= 1)
                maskbits++;
            tp.s.mask = maskbits;
        }
        else
            tp.s.clamp = true;
        tp.t.clamp = true;
        rdpq_set_tile(TILE0, FMT_CI8, 0, lw, &tp);
        dl_last_tile_lw   = lw;
        dl_last_tile_wrap = wrap;
        dl_last_up_block  = NULL;
    }

    // Per-edge T ranges (texel rows at the rect's top/bottom screen rows,
    // through each edge's own 1/scale -- see DL_RouteEmit). Both increase
    // down-screen. The band walk marches their UNION; a band's slice is clamped
    // per edge, so adjacent slices share their boundary chord exactly (no gap,
    // no overlap) and the union tiles the full rect.
    tl0 = w->t_top_l;
    tl1 = w->t_bot_l;
    tr0 = w->t_top_r;
    tr1 = w->t_bot_r;
    t0 = (tl0 < tr0) ? tl0 : tr0;   // union range start
    t1 = (tl1 > tr1) ? tl1 : tr1;   // union range end

    // Texel-T band walk (BUG B fix). DOOM's column drawer WRAPS the source
    // vertically (`source[(frac>>FRACBITS)&127]`, r_draw.c:150) -- the texture
    // TILES over a wall taller than itself; it is NOT clamped. We march the FULL
    // [t0,t1] range, splitting sub-bands at (a) texture-period boundaries
    // (floor-division multiples of blkh, negatives included) and (b) the TMEM
    // row cap, and give the triangles PERIOD-RELATIVE, band-local T.
    //
    // Documented divergence from vanilla: software wraps mod 128 ALWAYS, so a
    // sub-128-tall texture on an over-tall wall shows vanilla's tutti-frutti. We
    // wrap mod blkh (the texture tiles cleanly) -- acceptable, deliberate.
    //
    // TMEM RULE: each band's source rows are uploaded tile-LOCAL starting at
    // TMEM row 0 (surface pointed at the band's first row), never at their
    // original T address -- an original-T upload past the cap would overrun the
    // lower 2 KB TMEM half into the resident TLUT.
    {
    const float span = t1 - t0;
    float cur;
    int   guard;

    if (span <= 0.0f)
        return 1;

    cur = t0;
    for (guard = 0; cur < t1 - (1.0f / 1024.0f) && guard < 256; guard++)
    {
        int     period_base;    // floor(cur/blkh)*blkh, texels
        int     src_lo;         // band's first source row, in [0,blkh)
        int     src_cap;        // band row ceiling (TMEM cap / period edge)
        int     src_hi;         // one past the band's last drawn source row
        int     rows;           // drawn rows in this band
        int     rows_up;        // uploaded rows (rows + optional overlap)
        float   band_end;       // T where this band stops (texels)
        float   a_l, b_l;       // band T clamped into the left edge range
        float   a_r, b_r;       // band T clamped into the right edge range
        float   f0, f1;         // per-edge screen-Y lerp factors
        float   base;           // band-local T origin (texels)
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

        // Band load via the RAW tile API (the Stage-3 DL_BUILD collapse).
        // rdpq_tex_upload costs ~50 us of CPU per call (tex-loader setup); at
        // the measured ~150 band uploads/frame that alone was ~7.5 ms of the
        // ~9.6 ms DL_BUILD wall. DL_Flush points the RDP at the FULL
        // row-major block once per texture (rdpq_set_texture_image); the
        // TILE0 descriptor is configured above per record-window (deduped),
        // and each band is a single LOAD_TILE of the band's source rows and
        // the record's S window (~1 us CPU): the tile-size registers recorded
        // by LOAD_TILE give the same band-edge T clamp the old per-band
        // surface upload provided, in PERIOD-RELATIVE coordinates -- so the
        // triangles below use base=period_base (not band-local rows) and the
        // sampled texels are unchanged.
        // Skip the load (and its autosync) when the identical band is already
        // resident from the previous record's draw (UPLOAD DEDUP above).
        bandsrc = block + (src_lo * blkw);
        if (bandsrc != dl_last_up_block
            || src_lo != dl_last_up_lo
            || rows_up != dl_last_up_rows
            || c0 != dl_last_up_c0)
        {
            rdpq_load_tile(TILE0, c0, src_lo, c0 + lw, src_lo + rows_up);
            dl_last_up_block = bandsrc;
            dl_last_up_lo    = src_lo;
            dl_last_up_rows  = rows_up;
            dl_last_up_c0    = c0;
        }

        // Slice corners. T iso-lines of the wall's projective map are straight
        // lines in screen space, so the slice between T=cur and T=band_end is
        // bounded by two chords; each chord's endpoint on an edge sits at that
        // EDGE's own T position, clamped into the edge's range. Adjacent bands
        // clamp the SAME chord identically, so slices share edges exactly. Y is
        // affine in T along each vertical edge, so the corner Y is a per-edge
        // lerp.
        a_l = cur;
        if (a_l < tl0) a_l = tl0;
        if (a_l > tl1) a_l = tl1;
        b_l = band_end;
        if (b_l < tl0) b_l = tl0;
        if (b_l > tl1) b_l = tl1;
        a_r = cur;
        if (a_r < tr0) a_r = tr0;
        if (a_r > tr1) a_r = tr1;
        b_r = band_end;
        if (b_r < tr0) b_r = tr0;
        if (b_r > tr1) b_r = tr1;

        f0 = (a_l - tl0) / (tl1 - tl0);
        f1 = (b_l - tl0) / (tl1 - tl0);
        ytl = w->ytop_l + (w->ybot_l - w->ytop_l) * f0;
        ybl = w->ytop_l + (w->ybot_l - w->ytop_l) * f1;
        f0 = (a_r - tr0) / (tr1 - tr0);
        f1 = (b_r - tr0) / (tr1 - tr0);
        ytr = w->ytop_r + (w->ybot_r - w->ytop_r) * f0;
        ybr = w->ytop_r + (w->ybot_r - w->ytop_r) * f1;

        // Skip a slice that degenerated on BOTH edges (band entirely outside
        // both edge ranges -- possible at the union's extremes).
        if (b_l <= a_l && b_r <= a_r)
        {
            cur = band_end;
            continue;
        }

        // Period-relative, fraction-preserved T per corner (LOAD_TILE records
        // the band's tile size in period-relative rows, so T is offset by the
        // period base only -- NOT by src_lo as the old band-local upload was).
        base = (float)period_base;

        {
            float tl[5] = { xl, ytl, s_l, a_l - base, w->invw_l };
            float tr[5] = { xr, ytr, s_r, a_r - base, w->invw_r };
            float bl[5] = { xl, ybl, s_l, b_l - base, w->invw_l };
            float br[5] = { xr, ybr, s_r, b_r - base, w->invw_r };

            rdpq_triangle(&TRIFMT_TEX, tl, tr, bl);
            rdpq_triangle(&TRIFMT_TEX, tr, br, bl);
        }

        cur = band_end;
    }
    }
    return 1;
}

// Drain the emitted wall records into the attached display fb. Stage 3: a
// per-TEXTURE bucket walk -- for each touched texture, fetch + pin its transpose
// block ONCE, then draw every record in that texture's bucket (DL_DrawRecord)
// before moving to the next texture. This collapses autosync TMEM/pipe thrash
// from per-seg (Stage 2, textures interleaved) to per-texture (Q6). Overflow
// records that never made it into a bucket (arena full) are drawn afterward in
// arena order as the documented un-bucketed fallback (slower, correct, never
// crashes -- design risk table "DL arena overflow on tail frames").
//
// Must run inside the present seam after rdpq_attach + the view scissor, before
// the overlay blit (same rspq stream). Caller owns the mode/combiner/TLUT/persp
// setup (DESIGN section 4): rdpq_set_mode_standard + RDPQ_COMBINER_TEX_FLAT +
// rdpq_mode_tlut(TLUT_RGBA16) + rdpq_mode_persp(true).
void DL_Flush(void)
{
    int ti;

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

    // Reset the upload-dedup cache: no band is resident in TMEM at the start of
    // a flush (the caller just set up the world mode/combiner/tile state).
    dl_last_up_block = NULL;
    dl_last_up_lo    = -1;
    dl_last_up_rows  = -1;
    dl_last_up_c0    = -1;
    dl_last_tile_lw   = -1;
    dl_last_tile_wrap = -1;
    dl_last_prim      = 0;      // no PRIM resident (caller's setup left it unset)

    // Per-texture bucket walk (Q6, the Stage-3 autosync collapse). For each
    // texnum touched this frame, fetch + pin its transpose block ONCE, then draw
    // every record chained in that texture's bucket. All of a texture's uploads
    // (the band tiles of its records) are issued consecutively, so the tile
    // descriptor never thrashes between unrelated textures across the frame --
    // the per-seg interleaving Stage 2 produced is gone, and consecutive records
    // sharing a band skip the re-upload. Walking dl_touched[] (the sparse set of
    // textures used) keeps this O(records), not O(numtextures).
    for (ti = 0; ti < dl_touched_count; ti++)
    {
        int     tex = dl_touched[ti];
        int     ridx;
        byte*   block;
        int     blkh = 0;       // texture height = vertical wrap period
        int     blkw = 0;       // pow2 sampling width (texturewidthmask+1)

        block = DL_RowMajorBlock(tex, &blkh, &blkw);
        if (!block || blkh < 1 || blkw < 1)
        {
#if DL_DEBUG_TRACE
            debugf("DL_TRACE p=%d tex=%d NOBLOCK h=%d w=%d\n",
                   dl_present_no, tex, blkh, blkw);
#endif
            continue;       // no usable transpose: this texture's records stay key
        }

        // Pin the block against zone eviction while the RDP may read it
        // asynchronously (see DL_RowMajorBlock lifetime note / DL_PresentEnd).
        // Once per texture, not once per record (Stage 2 over-pinned per seg).
        DL_MarkInFlight(tex);

        // Point the RDP at the FULL row-major block ONCE per texture (the
        // raw-tile DL_BUILD collapse; see the band-load note in
        // DL_DrawRecord). The TILE0 descriptor itself is configured inside
        // the band path -- its pitch and S clamp/wrap depend on the record's
        // adaptive S window -- and deduped there; invalidate that state here
        // because the wrap mask is per-texture-period.
        {
            surface_t texsurf;

            texsurf = surface_make_linear(block, FMT_CI8, blkw, blkh);
            rdpq_set_texture_image(&texsurf);
            dl_last_tile_lw   = -1;
            dl_last_tile_wrap = -1;
            dl_last_up_block  = NULL;
        }

        for (ridx = dl_bucket_head[tex]; ridx >= 0;
             ridx = dl_walls[ridx].bucket_next)
        {
            const rdp_wall_t* w = &dl_walls[ridx];
#if DL_DEBUG_TRACE
            debugf("DL_TRACE p=%d rec=%d tex=%d w=%d h=%d x=%d..%d "
                   "y=%d.%d/%d.%d tl=%d..%d tr=%d..%d s=%d..%d lit=%d\n",
                   dl_present_no, ridx, tex, blkw, blkh,
                   (int)w->x1, (int)w->x2,
                   (int)w->ytop_l, (int)w->ytop_r, (int)w->ybot_l, (int)w->ybot_r,
                   (int)w->t_top_l, (int)w->t_bot_l,
                   (int)w->t_top_r, (int)w->t_bot_r,
                   (int)w->s_l, (int)w->s_r, (int)w->light);
#endif
            DL_DrawRecord(w, block, blkh, blkw);
        }
    }

    // Overflow fallback (design risk table "DL arena overflow on tail frames").
    // If the arena filled, records past DL_WALL_ARENA were never queued (their
    // columns stay key-index, a punched hole -- but they were dropped, not
    // mis-drawn). Nothing else to do: the bucket walk already drew every record
    // that DID fit. The flag exists so a future tail-frame can be detected and
    // the arena grown if it ever bites; with DL_WALL_ARENA=512 it should not.
    (void)dl_arena_overflow;
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
    int i;

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

    // Demote every pinned block this present did NOT re-use: its last-using
    // present is at latest the previous one, which the buffer-flip spin has
    // just proven fully drained. Blocks stamped with the CURRENT generation
    // were enqueued by the present we just detached -- still potentially in
    // flight, keep pinned. A slot whose raw was reclaimed (zone NULLed the
    // back-reference after an earlier demotion) just has its stale pin flag
    // cleared. No capacity limit: this replaces the former 16-entry lists,
    // whose silent overflow left blocks PU_STATIC forever.
    if (dl_rowmajor)
    {
        for (i = 0; i < numtextures; i++)
        {
            dl_rowmajor_t* slot = &dl_rowmajor[i];

            if (!slot->pinned)
                continue;
            if (!slot->raw)
            {
                slot->pinned = 0;
                continue;
            }
            if (slot->lastuse != dl_present_gen)
            {
                Z_ChangeTag(slot->raw, PU_CACHE);
                slot->pinned = 0;
            }
        }
    }
    dl_present_gen++;

    dl_wall_count = 0;      // consumed: never re-flush stale records
}

#endif // N64
