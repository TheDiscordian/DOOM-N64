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
// One-shot per-texture downsample diagnostic (orig/store width, height). Temp.
#ifndef DL_DS_DIAG
#define DL_DS_DIAG 0
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

// Flat system globals (r_data.c) -- Stage 4 plane path. firstflat is the lump
// number of the first flat; numflats the count. A flat is a 64x64 row-major CI8
// lump (4096 bytes). The plane emit keys its cache by the resolved lump number
// (firstflat+flattranslation[picnum], animation-correct), reduced to a flat
// index (lump-firstflat) for the parallel [numflats] cache.
extern int          firstflat;
extern int          numflats;

byte* R_GetColumn(int tex, int col);

// --- per-seg A/B toggle (graft #4) -----------------------------------------
// Defaults to the RDP path so Stage 3 exercises the new wall pipeline. The task
// allows defaulting this to CPU (0) if walls render wrong and cannot be fixed
// within the iteration budget.
int n64_rdp_wall_ab = 1;

// Plane A/B toggle (Stage-4 symmetry-completion of the wall toggle). Defaults to
// the RDP path so a full RDP build routes both walls AND planes. The ISOLATION
// experiment (SW walls + RDP planes) is n64_rdp_wall_ab=0 && n64_rdp_plane_ab=1,
// pinned at build time by BENCH_FORCE_PLANES_ONLY (d_main.c).
int n64_rdp_plane_ab = 1;

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

// --- Stage-4 plane span arena + per-flat buckets ---------------------------
// Mirrors the wall arena/bucket machinery for floor/ceiling spans. Sized from
// real visplane/span counts (brief primitive_estimate): typical ~240 spans,
// tail ~950, worst ~1850. Design says size for tail x2 -> 4096 records. One
// rdp_span_t ~= 28 B, so 4096 x 28 ~= 112 KB static. Overflow drops a span (it
// stays key-index -- a punched hole over stale fb, the bounded class), same
// discipline as walls.
#define DL_SPAN_ARENA   4096

static rdp_span_t   dl_spans[DL_SPAN_ARENA];
static int          dl_span_count;
static int          dl_span_overflow;       // 1 if a span was dropped this frame

// Per-flat buckets: each flat lump used this frame keeps a chain of its spans
// through the arena (rdp_span_t.bucket_next). Heads kept dense in a [numflats]
// array, invalidated lazily by the SAME per-frame dl_frame_gen stamp the wall
// buckets use (so BeginFrame stays O(touched)). dl_flat_touched[] is the sparse
// set of flat indices used this frame, so DL_FlushSpans walks O(spans), not
// O(numflats). Indexed by FLAT INDEX (lump - firstflat), 0..numflats-1.
static int32_t*     dl_flat_bucket_head;    // [numflats] first span idx, -1 none
static uint32_t*    dl_flat_bucket_gen;     // [numflats] frame stamp of head
static uint16_t     dl_flat_touched[DL_SPAN_ARENA]; // flat indices touched (deduped)
static int          dl_flat_touched_count;

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
// wide wall. The transpose period is texturewidthmask[texnum]+1: the EXACT
// power-of-two period R_GetColumn samples with (`col &= texturewidthmask`,
// r_data.c:390), so non-power-of-two declared widths (e.g. the one 24-wide
// texture, effective period 16) match the software sampler bit-for-bit. The
// STORED block width can be narrower than that period: 128/256-wide textures are
// downsampled to a 64-wide CI4 block (DL_RowMajorBlock ds_shift; DO #1), and the
// emit's original-width S is right-shifted into the 64-wide space at draw time.
//
// TMEM tile sizing is per-STORED-width: a CI4 tile must fit the lower 2 KB TMEM
// half (the TLUT owns the upper half), so the band walk caps at
// DL_TMEM_HALF/(store_w/2) rows. With downsample every routed texture stores at
// <= 64-wide (32 B/row), so the cap is 64 rows -- a <=64-tall texture is ONE
// band (one LOAD_TILE, one tri-pair) regardless of its original 128/256 width.
#define DL_TMEM_HALF    2048
// Widths below 8 would break the 8-byte DMA/TMEM-line alignment of per-band
// row offsets; no stock DOOM texture is that narrow -- refuse to route them.
#define DL_MIN_TEX_W    8

// CI4 sub-palette (Stage-3 CI4 wall path). Each routed texture quantizes its
// distinct PLAYPAL colours down to <=16 (median-cut + nearest-PLAYPAL snap,
// matching /tmp/ci4-preview/quantize.py) ONCE at first touch. subpal[] holds
// the <=16 chosen colours as RGBA5551 (the TLUT format, alpha=1 opaque) for
// upload into the CI4 palette region; remap[] maps every PLAYPAL index 0..255
// to its 0..15 sub-palette slot so the transpose loop packs 2 CI4 nibbles/byte.
typedef struct
{
    void* raw;      // the actual Z_Malloc'd allocation (back-referenced by zone)
    byte* block;    // 8-byte-aligned row-major width x height CI4 view into raw
    int   height;   // STORED block height (rows in the block): texture height
                    // after the optional T downsample -- 64 for a downsampled
                    // 128-tall texture, else textureheight. T-mask/wrap follow it.
    int   width;    // STORED block width (texels/row in block): the sampling
                    // period after downsample -- 64 for downsampled 128/256-wide,
                    // else texturewidthmask+1. The S-mask/pitch follow this.
    uint8_t  ds_shift;  // S downsample shift: right-shift mapping the emit's
                        // original-width S into the stored width (0 for <=64-wide,
                        // 1 for 128->64, 2 for 256->64).
    uint8_t  ts_shift;  // T downsample shift: right-shift mapping the emit's
                        // original-height T into the stored height (0 for <=64-tall,
                        // 1 for 128->64). With both shifts the stored block caps at
                        // 64x64 CI4 = 2 KB -> fits the TMEM half -> ONE-QUAD via
                        // hardware S+T wrap (no band walk): the load-count lever.
    uint8_t  fits_hw;   // 1 if the stored block fits the lower TMEM half AND its
                        // stored W/H are pow2 -> the hardware T-wrap one-quad fast
                        // path applies (1 LOAD_TILE + 1 tri-pair, any wall height).
    uint32_t lastuse;   // present generation of the last DL_MarkInFlight
    uint8_t  pinned;    // currently PU_STATIC for an in-flight window
    uint8_t  subpal_n;  // count of valid sub-palette entries (1..16)
    uint8_t  subpal_inited; // 1 once the sub-palette + remap are built
    uint8_t  pal_slot;  // CI4 palette slot assigned for the CURRENT frame (0..15)
    uint16_t subpal[16];    // <=16 RGBA5551 TLUT entries (the quantized colours)
    uint8_t  subpal_r[16];  // gamma RGB of each sub-palette entry (downsample snap)
    uint8_t  subpal_g[16];
    uint8_t  subpal_b[16];
    uint8_t  subpal_idx[16];// PLAYPAL index each sub-palette entry resolved to.
                            // The slot SELECTION is fixed (built once from base
                            // PLAYPAL); on a palette flash the colours are re-
                            // derived from the CURRENT master TLUT at THESE indices
                            // so walls tint with the world (DL_RetintSubPalettes).
    uint32_t subpal_gen;    // palette generation the subpal[] COLOURS track (0 ==
                            // base PLAYPAL, as built). Re-tint when != current gen.
    uint8_t  remap[256];    // PLAYPAL index -> sub-palette slot (0..subpal_n-1)
} dl_rowmajor_t;

static dl_rowmajor_t* dl_rowmajor;      // [numtextures]
static int            dl_rowmajor_inited;

// Per-slot 8-byte-aligned scratch for CI4 sub-palette TLUT uploads. ONE buffer
// per palette slot (16), NOT a single shared one: rdpq_tex_upload_tlut records
// the source's PHYSICAL address into the rspq stream and the LOAD_TLUT reads it
// ASYNCHRONOUSLY when the RDP processes the command -- long after DL_Flush has
// returned. A single shared scratch overwritten per texture had every LOAD_TLUT
// read the SAME (last-written) buffer, so all walls sampled one texture's
// palette (the whole-frame colour-swap bug: grey STARTAN3 walls rendered brown,
// blue COMPTILE rendered green). Each slot keeps its own buffer alive for the
// frame's async window; 16 slots x 16 RGBA5551 = 512 B. 8-byte aligned so
// rdpq_tex_upload_tlut's init_offset is 0.
static uint16_t       dl_subpal_up[16][16] __attribute__((aligned(8)));

// Diagnostic: count LOAD_TILE band loads emitted this present (the headline
// CI4 lever -- CI8 band-split ran ~150/frame; CI4 should be far fewer). Summed
// across DL_DrawRecord, read by the bench report, reset each DL_BeginFrame.
static uint32_t       dl_tile_loads;
uint32_t DL_TileLoadCount(void) { return dl_tile_loads; }

// Sibling per-present primitive counters (bench A/B instrumentation). Mirror
// dl_tile_loads exactly: summed across DL_DrawRecord, read by the bench report,
// reset each DL_BeginFrame. dl_recs = records drawn, dl_uploads = LOAD_TILE
// upload calls (deduped, tracks dl_tile_loads), dl_tris = triangles emitted
// (2 per quad/band slice). For band-split walls every load also draws, so the
// identity tris = 2 * uploads holds; the one-quad fast path is 2 tris / 1 upload.
static uint32_t       dl_recs;
static uint32_t       dl_uploads;
static uint32_t       dl_tris;
uint32_t DL_RecCount(void)    { return dl_recs; }
uint32_t DL_UploadCount(void) { return dl_uploads; }
uint32_t DL_TriCount(void)    { return dl_tris; }

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

// --- CI4 sub-palette quantizer (Stage-3 CI4 wall path) ---------------------
// Build a texture's <=16-colour CI4 sub-palette ONCE, at first touch, into the
// rowmajor slot. The algorithm mirrors /tmp/ci4-preview/quantize.py (the
// fidelity target the previews were rendered from):
//   1. gather the texture's DISTINCT PLAYPAL indices + per-index pixel
//      frequency over every column software can sample (col & widthmask);
//   2. if <=16 distinct, take them verbatim (lossless -- BROWNGRN/COMPTILE/
//      SLADWALL all land here);
//   3. else frequency-weighted median-cut in RGB to 16 representatives, each
//      snapped to its nearest PLAYPAL entry (squared-RGB distance -- the C
//      path drops the Python preview's CIELab snap, perceptually a wash for
//      snapping to the source art's own palette);
//   4. remap[] maps every used PLAYPAL index to its nearest sub-palette slot
//      (squared-RGB), filling the 256-entry remap so the transpose can pack
//      any column's texels (unused indices map to slot 0, never sampled).
// The sub-palette colours are stored RGBA5551 (TLUT format, alpha=1, gamma via
// the SAME gammatable I_SetPalette uses so wall colour matches the master).
//
// Cost: once per routed texture, on its cold frame (excluded from steady-state
// timing). Median-cut over <=256 colours is a handful of partition passes.
extern int      usegamma;
extern byte     gammatable[5][256];

// One median-cut box: an index range into a working array of distinct colours,
// plus its frequency-weighted centroid (filled at extraction).
typedef struct { int lo, hi; } dl_mc_box_t;

static inline int DL_RGBdist2(int r0,int g0,int b0,int r1,int g1,int b1)
{
    int dr=r0-r1, dg=g0-g1, db=b0-b1;
    return dr*dr + dg*dg + db*db;
}

static void DL_BuildSubPalette(int texnum, dl_rowmajor_t* slot)
{
    const byte* playpal;
    int   th, tw, col, row, i, k;
    int   freq[256];
    int   nuniq = 0;
    // working arrays for median-cut (parallel, reordered together)
    uint8_t  wpix[256];     // PLAYPAL index of distinct colour
    int      wfreq[256];    // its frequency
    uint8_t  wr[256], wg[256], wb[256];     // gamma-applied RGB
    dl_mc_box_t boxes[16];
    int   nboxes;
    uint8_t  pal_r[16], pal_g[16], pal_b[16];   // final sub-palette RGB (gamma)
    uint8_t  pal_idx[16];                       // PLAYPAL index of each entry
                                                // (for per-flash re-tint)

    if (slot->subpal_inited)
        return;

    // Histogram the FULL-resolution texture (its complete colour set), NOT the
    // stored downsampled dims: slot->width/height may be the post-downsample 64x64
    // block, but the sub-palette must represent every colour the texture contains
    // so the box-filter averages snap to in-gamut entries. Use the original
    // sampling period / texture height directly.
    playpal = (const byte*)W_CacheLumpName("PLAYPAL", PU_CACHE);
    tw = texturewidthmask[texnum] + 1;
    th = textureheight[texnum] >> FRACBITS;
    if (!playpal || th < 1 || tw < 1)
    {
        // Degenerate: a single black entry, everything maps to it. Marks the
        // slot inited so we never retry a broken texture every frame.
        slot->subpal[0] = 1;        // black, opaque
        slot->subpal_n  = 1;
        memset(slot->remap, 0, sizeof(slot->remap));
        memset(slot->subpal_idx, 0, sizeof(slot->subpal_idx)); // PLAYPAL idx 0
        slot->subpal_inited = 1;
        return;
    }

    // Frequency histogram over exactly the texels the wall can sample.
    memset(freq, 0, sizeof(freq));
    for (col = 0; col < tw; col++)
    {
        const byte* src = R_GetColumn(texnum, col);
        for (row = 0; row < th; row++)
            freq[src[row]]++;
    }
    for (i = 0; i < 256; i++)
    {
        if (freq[i] > 0)
        {
            wpix[nuniq]  = (uint8_t)i;
            wfreq[nuniq] = freq[i];
            wr[nuniq] = gammatable[usegamma][playpal[i*3+0]];
            wg[nuniq] = gammatable[usegamma][playpal[i*3+1]];
            wb[nuniq] = gammatable[usegamma][playpal[i*3+2]];
            nuniq++;
        }
    }
    if (nuniq == 0)
    {
        slot->subpal[0] = 1;
        slot->subpal_n  = 1;
        memset(slot->remap, 0, sizeof(slot->remap));
        memset(slot->subpal_idx, 0, sizeof(slot->subpal_idx)); // PLAYPAL idx 0
        slot->subpal_inited = 1;
        return;
    }

    if (nuniq <= 16)
    {
        // Lossless: the distinct colours ARE the sub-palette.
        for (i = 0; i < nuniq; i++)
        {
            pal_r[i] = wr[i]; pal_g[i] = wg[i]; pal_b[i] = wb[i];
            pal_idx[i] = wpix[i];       // PLAYPAL index of this entry (re-tint)
        }
        slot->subpal_n = (uint8_t)nuniq;
    }
    else
    {
        // Frequency-weighted median-cut to 16 boxes. boxes hold index ranges
        // into the w* working arrays, which we reorder in place per split.
        boxes[0].lo = 0; boxes[0].hi = nuniq;    // [lo,hi)
        nboxes = 1;
        while (nboxes < 16)
        {
            int   best = -1;
            long  bestscore = -1;
            int   lo, hi, axis, mid;
            long  wsum, half, cum;
            int   rmin,rmax,gmin,gmax,bmin,bmax;
            // pick the box with the largest (longest-axis-range * weight)
            for (k = 0; k < nboxes; k++)
            {
                int n = boxes[k].hi - boxes[k].lo;
                long w = 0, rng;
                int j;
                if (n <= 1) continue;
                rmin=gmin=bmin=255; rmax=gmax=bmax=0;
                for (j = boxes[k].lo; j < boxes[k].hi; j++)
                {
                    if (wr[j]<rmin) rmin=wr[j];
                    if (wr[j]>rmax) rmax=wr[j];
                    if (wg[j]<gmin) gmin=wg[j];
                    if (wg[j]>gmax) gmax=wg[j];
                    if (wb[j]<bmin) bmin=wb[j];
                    if (wb[j]>bmax) bmax=wb[j];
                    w += wfreq[j];
                }
                rng = (rmax-rmin);
                if ((gmax-gmin) > rng) rng = (gmax-gmin);
                if ((bmax-bmin) > rng) rng = (bmax-bmin);
                {
                    long score = rng * w;
                    if (score > bestscore) { bestscore = score; best = k; }
                }
            }
            if (best < 0) break;     // no splittable box (all singletons)

            lo = boxes[best].lo; hi = boxes[best].hi;
            // longest axis of the chosen box
            rmin=gmin=bmin=255; rmax=gmax=bmax=0;
            for (i = lo; i < hi; i++)
            {
                if (wr[i]<rmin) rmin=wr[i];
                if (wr[i]>rmax) rmax=wr[i];
                if (wg[i]<gmin) gmin=wg[i];
                if (wg[i]>gmax) gmax=wg[i];
                if (wb[i]<bmin) bmin=wb[i];
                if (wb[i]>bmax) bmax=wb[i];
            }
            axis = 0;
            { int rr=rmax-rmin, gg=gmax-gmin, bb=bmax-bmin;
              if (gg>=rr && gg>=bb) axis=1; else if (bb>=rr && bb>=gg) axis=2; }
            // insertion-sort the box's slice by the chosen axis (n is small)
            for (i = lo+1; i < hi; i++)
            {
                int kr=wr[i],kg=wg[i],kb=wb[i],kp=wpix[i],kf=wfreq[i];
                int kv=(axis==0)?kr:(axis==1)?kg:kb;
                int j=i-1;
                while (j>=lo)
                {
                    int jv=(axis==0)?wr[j]:(axis==1)?wg[j]:wb[j];
                    if (jv<=kv) break;
                    wr[j+1]=wr[j]; wg[j+1]=wg[j]; wb[j+1]=wb[j];
                    wpix[j+1]=wpix[j]; wfreq[j+1]=wfreq[j];
                    j--;
                }
                wr[j+1]=kr; wg[j+1]=kg; wb[j+1]=kb; wpix[j+1]=kp; wfreq[j+1]=kf;
            }
            // split at the weighted median
            wsum = 0;
            for (i = lo; i < hi; i++) wsum += wfreq[i];
            half = wsum / 2; cum = 0; mid = lo;
            for (i = lo; i < hi; i++)
            {
                cum += wfreq[i];
                if (cum >= half) { mid = i+1; break; }
            }
            if (mid <= lo) mid = lo+1;
            if (mid >= hi) mid = hi-1;
            // replace best box with [lo,mid); append [mid,hi)
            boxes[best].hi = mid;
            boxes[nboxes].lo = mid; boxes[nboxes].hi = hi;
            nboxes++;
        }
        // Extract each box's frequency-weighted centroid, snap to PLAYPAL.
        for (k = 0; k < nboxes; k++)
        {
            long sr=0,sg=0,sb=0,sw=0;
            int  bestidx=0, bestd=0x7fffffff;
            int  cr,cg,cb;
            for (i = boxes[k].lo; i < boxes[k].hi; i++)
            {
                sr += (long)wr[i]*wfreq[i];
                sg += (long)wg[i]*wfreq[i];
                sb += (long)wb[i]*wfreq[i];
                sw += wfreq[i];
            }
            if (sw < 1) sw = 1;
            cr = (int)(sr/sw); cg = (int)(sg/sw); cb = (int)(sb/sw);
            // nearest PLAYPAL entry (gamma-applied, squared RGB)
            for (i = 0; i < 256; i++)
            {
                int pr=gammatable[usegamma][playpal[i*3+0]];
                int pg=gammatable[usegamma][playpal[i*3+1]];
                int pb=gammatable[usegamma][playpal[i*3+2]];
                int d=DL_RGBdist2(cr,cg,cb,pr,pg,pb);
                if (d<bestd) { bestd=d; bestidx=i; }
            }
            pal_r[k]=gammatable[usegamma][playpal[bestidx*3+0]];
            pal_g[k]=gammatable[usegamma][playpal[bestidx*3+1]];
            pal_b[k]=gammatable[usegamma][playpal[bestidx*3+2]];
            pal_idx[k]=(uint8_t)bestidx;    // PLAYPAL index of this entry (re-tint)
        }
        slot->subpal_n = (uint8_t)nboxes;
    }

    // Pack the final sub-palette as RGBA5551 (alpha=1, opaque -- walls are
    // never the transparency key; the key only matters for the CI8 overlay).
    // Also keep each entry's gamma RGB: the downsample transpose averages a
    // source column-group's gamma RGB per row and snaps to the nearest of these.
    for (k = 0; k < slot->subpal_n; k++)
    {
        slot->subpal[k] = (uint16_t)(((pal_r[k] >> 3) << 11) |
                                     ((pal_g[k] >> 3) << 6)  |
                                     ((pal_b[k] >> 3) << 1)  | 1);
        slot->subpal_r[k] = pal_r[k];
        slot->subpal_g[k] = pal_g[k];
        slot->subpal_b[k] = pal_b[k];
        slot->subpal_idx[k] = pal_idx[k];   // PLAYPAL index for per-flash re-tint
    }
    for (; k < 16; k++)
    {
        slot->subpal[k]   = slot->subpal[0];
        slot->subpal_r[k] = slot->subpal_r[0];
        slot->subpal_g[k] = slot->subpal_g[0];
        slot->subpal_b[k] = slot->subpal_b[0];
        slot->subpal_idx[k] = slot->subpal_idx[0];
    }

    // remap[]: every PLAYPAL index -> nearest sub-palette slot (squared RGB on
    // gamma-applied colours). Unused indices still get a valid slot (harmless;
    // they are never sampled). Used indices snap to the closest of the <=16.
    for (i = 0; i < 256; i++)
    {
        int pr=gammatable[usegamma][playpal[i*3+0]];
        int pg=gammatable[usegamma][playpal[i*3+1]];
        int pb=gammatable[usegamma][playpal[i*3+2]];
        int bestslot=0, bestd=0x7fffffff;
        for (k = 0; k < slot->subpal_n; k++)
        {
            int d=DL_RGBdist2(pr,pg,pb, pal_r[k],pal_g[k],pal_b[k]);
            if (d<bestd) { bestd=d; bestslot=k; }
        }
        slot->remap[i] = (uint8_t)bestslot;
    }

    slot->subpal_inited = 1;
}

// --- CI4 damage-flash re-tint (palette-flash correctness) -------------------
// Palette flashes (damage red / pickup gold / radsuit green / invuln) work by
// tinting the master 256-TLUT via I_SetPalette; CI8 sprites/planes/HUD sample
// the master so they flash. CI4 walls sample their OWN 16-entry sub-palettes,
// built ONCE from the BASE PLAYPAL and never re-tinted -> walls stayed un-flashed
// (Ryan: "the red that washes over everything doesn't hit the walls anymore").
//
// FIX: when the palette generation changes (a real flash, not the wall pass's own
// forced re-upload), re-derive each initialized sub-palette's 16 RGBA5551 entries
// from the CURRENT master TLUT at the entry's FIXED PLAYPAL index (subpal_idx[k]).
// The index SELECTION stays fixed (so the texture's quantization is unchanged);
// only the COLOURS track the flash, exactly like the CI8 world. Per-flash re-pack
// (~16 small palettes x <=16 entries), not per-frame -- gated on the generation.
// Alpha is forced opaque (walls are never the transparency key).
extern uint32_t I_N64PaletteGen(void);
extern const uint16_t* I_N64MasterTLUT(void);

// Re-derive ONE slot's 16 sub-palette entries from the current master TLUT at the
// entry's fixed PLAYPAL index (alpha forced opaque -- walls are never the key),
// and stamp the slot with the generation its colours now track. No-op if already
// current. The master is fetched once as a pointer and indexed directly (no
// per-entry cross-TU call). Used by the lazy per-texture upload path.
static void DL_RetintSlot(dl_rowmajor_t* slot, uint32_t gen)
{
    const uint16_t* master;
    int k;
    if (!slot->subpal_inited || slot->subpal_gen == gen)
        return;
    master = I_N64MasterTLUT();
    for (k = 0; k < 16; k++)
        slot->subpal[k] = (uint16_t)((master[slot->subpal_idx[k]]
                                      & ~(uint16_t)1) | 1);
    slot->subpal_gen = gen;
}

// Latch the current palette generation for this flush. The per-slot retint is
// LAZY: every texture that draws this frame passes the upload site, where
// DL_RetintSlot(slot, dl_retint_gen) re-derives its colours if they don't track
// the current flash. So the flush only needs to record the generation here --
// textures NOT drawn this frame are not visible and don't need retinting (they
// re-tint lazily whenever they next draw). Cost is O(touched) on a flash frame,
// not O(numtextures), and zero on un-flashed frames (DL_RetintSlot's gen check).
static uint32_t dl_retint_gen = 0xFFFFFFFFu;    // palette gen this flush tracks

static void DL_RetintSubPalettes(void)
{
    dl_retint_gen = I_N64PaletteGen();
}

// --- per-texture downsample-error metric (selective S-downsample gate) -------
// The load-collapse lever for WIDE walls (128/256-wide) is to halve their stored
// width so the CI4 row fits the TMEM half in fewer T-bands (and 128->64 reaches
// the one-quad fits_hw class). But halving width is a horizontal box-filter:
// SAFE on art whose merged column pairs were already near-identical, a visible
// SMEAR on art with fine vertical structure (COMPUTE2's computer bank, STARTAN3's
// metallic striping, TEKWALL's circuitry). Ryan accepted CI4 COLOUR quantization,
// NOT resolution loss on the detailed wides -- so the downsample is DECIDED PER
// TEXTURE from the texture's own loss under halving, conservative (when in doubt,
// native).
//
// Metric: SIMULATE the exact box-filter the store would apply (average each
// even/odd column PAIR that would merge), then measure how far the merged result
// sits from the two originals it replaces. This is the loss the player would see
// -- it directly answers "does halving smear THIS texture", unlike an
// adjacent-column-difference proxy (which misreads vertically-striped art like
// STARTAN3 as safe because neighbouring columns look similar while the merge of a
// stripe pair still destroys the stripe). Luma = (r*2+g*5+b)>>3 on the
// gamma-applied PLAYPAL (the colours the wall actually shows). Sampled on a row
// stride for speed. Returns the mean per-texel reconstruction error scaled x100.
static int DL_DownsampleErr(int texnum, int tw, int th)
{
    const byte* playpal = (const byte*)W_CacheLumpName("PLAYPAL", PU_CACHE);
    const byte* gt      = gammatable[usegamma];
    long        acc     = 0;
    long        nsamp   = 0;
    int         col, row;
    int         rstep   = (th >= 8) ? 4 : 1;

    if (!playpal || tw < 2 || th < 1)
        return 0;

    // Compare EVERY original column against the texel the player would actually
    // sample after halving: stored column (col>>1), which is the box-average of
    // originals 2*(col>>1) and 2*(col>>1)+1. Accumulate |orig - reconstruction|
    // over every sampled texel. This is the EXACT per-texel smear the halved wall
    // shows -- it is phase-correct (it scores the loss at the original column the
    // pixel lands on, not just the merged-pair representative), so vertically
    // striped art (STARTAN3, COMPUTE2) registers its full stripe contrast (a light
    // stripe column reconstructed from a dark+light average is half a stripe off)
    // instead of hiding behind a low merged-pair residual.
    for (col = 0; col < tw; col++)
    {
        int         e0   = (col & ~1);              // even partner of this pair
        const byte* orig = R_GetColumn(texnum, col);
        const byte* p0   = R_GetColumn(texnum, e0);
        const byte* p1   = R_GetColumn(texnum, (e0 + 1 < tw) ? e0 + 1 : e0);
        for (row = 0; row < th; row += rstep)
        {
            const byte* po = playpal + orig[row] * 3;
            const byte* pa = playpal + p0[row] * 3;
            const byte* pb = playpal + p1[row] * 3;
            int lo  = (gt[po[0]] * 2 + gt[po[1]] * 5 + gt[po[2]]) >> 3;
            int la  = (gt[pa[0]] * 2 + gt[pa[1]] * 5 + gt[pa[2]]) >> 3;
            int lb  = (gt[pb[0]] * 2 + gt[pb[1]] * 5 + gt[pb[2]]) >> 3;
            int rec = (la + lb) >> 1;               // halved reconstruction texel
            int e   = lo - rec; if (e < 0) e = -e;
            acc += e;
            nsamp++;
        }
    }
    if (nsamp < 1)
        return 0;
    return (int)((acc * 100) / nsamp);
}

// Downsample-error threshold (err x100 luma/texel). Below this the merged column
// pairs sit close enough to their originals that halving the stored width is
// visually safe; above it the wide stays NATIVE. Calibrated conservatively from
// the routed wall set (DL_DS_DIAG build): the detailed wides Ryan named --
// COMPUTE2 (256-wide computer bank), STARTAN3 (128, metallic striping), TEKWALL
// (circuit accents) -- all score above it (their fine vertical structure is
// destroyed by a column-pair merge); large flat/low-frequency wides score below.
// When in doubt the texture lands native (the safe side).
#define DL_DS_ERR_THRESH  450

// Named-detail PROTECT LIST (belt-and-suspenders over the metric). The re
// downsample-error metric is a luma reconstruction estimate; it can under-read
// art whose detail is chroma/structure the merge happens to preserve in luma
// (STARTAN3's metallic vertical striping and COMPUTE2's computer bank both score
// below the threshold on luma alone, yet Ryan named them as MUST-stay-native --
// CI4 ACCEPTANCE was COLOUR quantization, NOT resolution loss on these). So the
// prompt's protected wides are pinned NATIVE by NAME regardless of the metric;
// the metric then guards the REMAINING wides (catches detailed ones not named
// here). The list is resolved to texnums once (R_CheckTextureNumForName) and
// cached; -2 = "not yet resolved", -1 entries (absent in this WAD) never match.
extern int R_CheckTextureNumForName(char* name);
static const char* const dl_protect_names[] = {
    "STARTAN3",     // metallic vertical striping (high-traffic, named)
    "COMPUTE2",     // 256-wide computer bank (named)
    "COMPTALL",     // 256-wide computer bank sibling
    "TEKWALL1", "TEKWALL2", "TEKWALL3", "TEKWALL4",     // circuit accents (named)
    "COMPSPAN", "COMPWERD", "COMPUTE1", "COMPUTE3",     // computer-bank family
    "SILVER2", "SILVER3",                               // fine silver striping
};
#define DL_PROTECT_N (int)(sizeof(dl_protect_names)/sizeof(dl_protect_names[0]))
static int  dl_protect_tex[DL_PROTECT_N];
static int  dl_protect_resolved;

static int DL_TexIsProtectedWide(int texnum)
{
    int i;
    if (!dl_protect_resolved)
    {
        for (i = 0; i < DL_PROTECT_N; i++)
            dl_protect_tex[i] =
                R_CheckTextureNumForName((char*)dl_protect_names[i]);
        dl_protect_resolved = 1;
    }
    for (i = 0; i < DL_PROTECT_N; i++)
        if (dl_protect_tex[i] == texnum)        // (-1 absent entries never match)
            return 1;
    return 0;
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

    // The texture's power-of-two sampling period (R_GetColumn masks with
    // texturewidthmask), NOT a fixed 64: see BUG-D note above. Wider periods than
    // 512 can't be addressed by LOAD_TILE (S coordinate caps at 1024) and tile at
    // <= 4 TMEM rows anyway; narrower than 8 break band alignment -- both refuse
    // to route (CPU keeps the seg).
    tw = texturewidthmask[texnum] + 1;
    if (tw < DL_MIN_TEX_W || tw > 512)
        return NULL;

    // SELECTIVE CI4 STORE (fidelity-preserving load collapse). DETAILED wides stay
    // at FULL declared resolution; SAFE (low-horizontal-frequency) wides downsample
    // S (width) toward 64 so their CI4 row fits the TMEM half in fewer T-bands --
    // and 128->64 reaches the one-quad fits_hw class (1 LOAD_TILE + 1 tri-pair).
    // The split is PER TEXTURE, decided from the texture's own horizontal detail
    // (DL_HorizDetail), conservative -- when in doubt, native:
    //   - COMPUTE2 (256-wide computer bank), STARTAN3 (128, metallic striping),
    //     TEKWALL (circuit accents): high HF -> NATIVE, no resolution loss. Ryan
    //     rejected the blanket downsample (038c7f3/e94c39a) on exactly these.
    //   - Large flat/low-frequency wides: low HF -> halve S (the box-average lands
    //     between two near-equal columns, no visible smear), recovering their loads.
    //
    // T (height) is NEVER downsampled: the blanket path's ts_shift>0 carried a
    // functional T-stretch bug (DEFECTS "few pixels draw then stretched downwards"
    // -- mask_t desynced from the loaded T-extent), so this rework keeps native
    // height. S-downsample alone gives the load collapse: a 128-wide native CI4 is
    // 64 B/row (cap 32 rows -> a 128-tall wall is 4 T-bands); halved to 64-wide it
    // is 32 B/row (cap 64 -> 64x64 one-quad, or 64x128 = 2 bands). No T mapping
    // changes, so the stretch bug cannot recur.
    //
    // The 74->5 tile-load collapse otherwise comes WITHOUT throwing away pixels,
    // from BAND MATH + the one-quad fast path: CI4 packs 2 indices/byte (half the
    // CI8 row bytes -> double the TMEM rows), and a pow2 block fitting the lower
    // 2 KB half draws as a single LOAD_TILE + tri-pair with hardware S+T wrap.
    //
    // ds_shift is 0 for native textures and 1 (128/256->64) for safe-downsampled
    // wides; ts_shift stays 0. DL_DrawRecord's 1/2^ds_shift scale maps the emit's
    // original-width S into the stored width. The box-filter branch below builds
    // the downsampled block when ds_shift>0.
    {
        int store_w = tw;       // stored block width (full or downsampled period)
        int store_h = th;       // stored block height = full texture height
        int ds_shift = 0;       // S downsample shift (0 native, 1 = halve width)
        int ts_shift = 0;       // 0: native T (height never downsampled -- see above)

        // SELECTIVE S-DOWNSAMPLE DECISION. Only wides (>64) are candidates; a wide
        // halves only if it is NOT on the named-detail protect list AND its
        // reconstruction error is below threshold. The name guard pins the
        // prompt's must-stay-native wides (STARTAN3/COMPUTE2/TEKWALL...) regardless
        // of the luma metric; the metric guards the remaining wides. One halving
        // step only (128->64 reaches the one-quad class; 256->128 halves its
        // bands) -- a 2-step quarter would risk visible loss, so cap store at 64.
        if (tw > 64
            && !DL_TexIsProtectedWide(texnum)
            && DL_DownsampleErr(texnum, tw, th) < DL_DS_ERR_THRESH)
        {
            ds_shift = 1;                   // halve width (128->64, 256->128)
            store_w  = tw >> 1;
            if (store_w < 64)               // never below 64 (one-quad target)
            {
                store_w  = 64;
                ds_shift = 0;               // (unreachable for pow2 tw>64; guard)
            }
            // Recompute ds_shift from the actual stored width so the draw-time
            // 1/2^ds_shift S scale matches (256->128 is one step too; we only ever
            // take ONE step here, so ds_shift stays 1).
            {
                int s = 0, w2 = tw / store_w;
                while (w2 > 1) { w2 >>= 1; s++; }
                ds_shift = s;
            }
        }

        slot->width    = store_w;
        slot->height   = store_h;
        slot->ds_shift = (uint8_t)ds_shift;
        slot->ts_shift = (uint8_t)ts_shift;

    // CI4 row pitch is store_w/2 bytes (2 indices/byte), and the RDP requires
    // every tile pitch be a multiple of 8 bytes. store_w is a power of two, so
    // store_w/2 is a multiple of 8 for store_w >= 16; only store_w==8 (4 bytes)
    // is short. PAD the stored row to a 16-texel period (8 bytes) for store_w==8
    // -- the extra 8 texels are never sampled because the hardware S-wrap mask is
    // log2(store_w) (the REAL period), which folds every S into [0,store_w).
    // pad_w is the stored period (texels/row); the row pitch is pad_w/2 bytes.
    {
        int pad_w = (store_w < 16) ? 16 : store_w;
        int pitch = pad_w / 2;          // bytes/row (multiple of 8)

        // fits_hw: the stored block fits the lower TMEM half AND both stored
        // dimensions are pow2 -> the hardware T-wrap one-quad fast path (single
        // LOAD_TILE + single tri-pair, hardware S+T wrap) applies in DL_DrawRecord.
        {
            int is_pow2_w = (store_w & (store_w - 1)) == 0;
            int is_pow2_h = (store_h & (store_h - 1)) == 0;
            slot->fits_hw = (is_pow2_w && is_pow2_h &&
                             pitch * store_h <= DL_TMEM_HALF) ? 1 : 0;
        }

        // Build this texture's <=16-colour CI4 sub-palette + 256->slot remap
        // ONCE, at first touch (lossless if it already has <=16 distinct
        // colours). DL_BuildSubPalette uses textureheight/texturewidthmask (the
        // FULL-res columns), so downsample averaging snaps to the same palette.
        DL_BuildSubPalette(texnum, slot);

        // PU_STATIC until DL_PresentEnd demotes it (lifetime note above); user
        // ptr back-references the slot so a zone reclaim NULLs slot->raw and the
        // next touch re-transposes. The CI4 block is pitch*store_h bytes. Over-
        // allocate by 7 so the view is 8-byte aligned (the RDP DMA into TMEM needs
        // an 8-byte-aligned source; Z_Malloc guarantees only 4).
        {
            int ci4_bytes = pitch * store_h;
            slot->raw = Z_Malloc(ci4_bytes + 7, PU_STATIC, (void**)&slot->raw);
            if (!slot->raw)
                return NULL;
            block = (byte*)(((uintptr_t)slot->raw + 7) & ~(uintptr_t)7);
            memset(block, 0, ci4_bytes);    // pad texels = slot 0 (never sampled)

            if (ds_shift == 0 && ts_shift == 0)
            {
                // No downsample: transpose column-major posts -> row-major AND
                // pack to CI4 nibbles. Even column -> high nibble, odd column ->
                // low nibble (RDP CI4 byte order; matches mksprite's
                // (ix0<<4)|ix1). remap[] turns each PLAYPAL index into its 0..15
                // sub-palette slot.
                for (col = 0; col < tw; col++)
                {
                    const byte* src = R_GetColumn(texnum, col);
                    byte*       dstcol = block + (col >> 1);
                    int         shift  = (col & 1) ? 0 : 4;
                    for (row = 0; row < th; row++)
                    {
                        byte nib = slot->remap[src[row]] & 0x0F;
                        byte* d  = dstcol + row * pitch;
                        if (shift) *d = (byte)((*d & 0x0F) | (nib << 4));
                        else       *d = (byte)((*d & 0xF0) | nib);
                    }
                }
            }
            else
            {
                // Box-filter downsample (S by 1<<ds_shift, T by 1<<ts_shift): each
                // output texel averages its (sgroup x tgroup) source texels in
                // gamma RGB, then snaps to the nearest sub-palette slot. Averaging
                // gamma RGB (not palette indices, which is meaningless) is the
                // principled paletted box-filter; snapping among the sub-palette's
                // <=16 entries keeps the result inside the texture's own colours.
                const byte* playpal = (const byte*)W_CacheLumpName("PLAYPAL", PU_CACHE);
                const int   sgroup  = 1 << ds_shift;
                const int   tgroup  = 1 << ts_shift;
                const int   gshift  = ds_shift + ts_shift;  // log2(sgroup*tgroup)
                const byte* gt      = gammatable[usegamma];
                const byte* srccols[4];     // sgroup <= 4 (256 -> 64)
                int         g, tg;

                for (col = 0; col < store_w; col++)
                {
                    byte* dstcol = block + (col >> 1);
                    int   shift  = (col & 1) ? 0 : 4;
                    int   c0     = col << ds_shift;

                    for (g = 0; g < sgroup; g++)
                        srccols[g] = R_GetColumn(texnum, c0 + g);

                    for (row = 0; row < store_h; row++)
                    {
                        int sr = 0, sg = 0, sb = 0;
                        int bestslot = 0, bestd = 0x7fffffff;
                        int r0 = row << ts_shift;
                        byte* d;
                        byte  nib;

                        for (tg = 0; tg < tgroup; tg++)
                            for (g = 0; g < sgroup; g++)
                            {
                                const byte* p = playpal + srccols[g][r0 + tg] * 3;
                                sr += gt[p[0]];
                                sg += gt[p[1]];
                                sb += gt[p[2]];
                            }
                        sr >>= gshift; sg >>= gshift; sb >>= gshift;

                        for (g = 0; g < slot->subpal_n; g++)
                        {
                            int dd = DL_RGBdist2(sr, sg, sb,
                                                 slot->subpal_r[g],
                                                 slot->subpal_g[g],
                                                 slot->subpal_b[g]);
                            if (dd < bestd) { bestd = dd; bestslot = g; }
                        }
                        nib = (byte)(bestslot & 0x0F);
                        d   = dstcol + row * pitch;
                        if (shift) *d = (byte)((*d & 0x0F) | (nib << 4));
                        else       *d = (byte)((*d & 0xF0) | nib);
                    }
                }
            }
            data_cache_hit_writeback(block, ci4_bytes);
        }
    }
    }
    slot->block = block;
    // Fresh blocks are born PU_STATIC: record that as a pin in the current
    // present generation so DL_PresentEnd's sweep demotes them on the same
    // schedule as re-pinned blocks (see the in-flight tracking below).
    slot->pinned = 1;
    slot->lastuse = dl_present_gen;
#if DL_DS_DIAG
    debugf("DL_DS tex=%d orig_w=%d store_w=%d orig_h=%d store_h=%d ds=%d ts=%d "
           "fits_hw=%d subpal_n=%d err=%d prot=%d\n",
           texnum, tw, (int)slot->width, th, (int)slot->height,
           (int)slot->ds_shift, (int)slot->ts_shift, (int)slot->fits_hw,
           (int)slot->subpal_n, DL_DownsampleErr(texnum, tw, th),
           DL_TexIsProtectedWide(texnum));
#endif
    if (out_h) *out_h = slot->height;   // STORED height (64 for T-downsampled)
    if (out_w) *out_w = slot->width;    // STORED width (64 for S-downsampled): the
                                        // pitch/cap/mask downstream MUST follow the
                                        // stored block, not the original period.
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

// --- Stage-4 flat row-major CI8 cache --------------------------------------
// Flats are ALREADY 64x64 row-major CI8 (4096-byte lumps; r_draw.c spot formula
// confirms flat[V*64+U]), so unlike walls there is NO transpose -- the cache is
// a memcpy of the flat lump bytes into an 8-byte-aligned PU_STATIC block plus a
// cache writeback (cheaper than DL_RowMajorBlock's transpose loop). Keyed by
// FLAT INDEX (lump - firstflat), animation-correct because R_DrawPlanes resolves
// firstflat+flattranslation[picnum] before emitting.
//
// LIFETIME: identical to the wall transpose cache (DL_RowMajorBlock note) -- the
// RDP reads this block ASYNCHRONOUSLY after rdpq_detach_cb, so it is PU_STATIC
// until DL_PresentEnd demotes it once the using present provably drained. The
// reload-survival lesson (DEFECTS.md Stage-3) applies: a slot whose raw the zone
// reclaimed (NULLs the back-reference) re-copies on next touch, and DL_BeginFrame
// resets the generation stamps so nothing crosses a P_SetupLevel wrongly.
#define DL_FLAT_BYTES   4096    // 64x64 CI8

typedef struct
{
    void*    raw;       // Z_Malloc'd allocation (back-referenced by zone)
    byte*    block;     // 8-byte-aligned 64x64 row-major CI8 view into raw
    uint32_t lastuse;   // present generation of the last DL_FlatMarkInFlight
    uint8_t  pinned;    // currently PU_STATIC for an in-flight window
} dl_flat_t;

static dl_flat_t*   dl_flat;            // [numflats]
static int          dl_flat_inited;

static void DL_InitFlatCache(void)
{
    if (dl_flat_inited)
        return;
    if (numflats <= 0)
        return;
    dl_flat = (dl_flat_t*)Z_Malloc(numflats * sizeof(dl_flat_t), PU_STATIC, 0);
    memset(dl_flat, 0, numflats * sizeof(dl_flat_t));
    dl_flat_inited = 1;
}

// Produce (or fetch) the row-major CI8 block for a flat index (lump-firstflat).
// Copies the flat lump bytes once, caches PU_STATIC, returns the 8-byte-aligned
// block (always 64x64). NULL on a bad index or alloc failure.
static byte* DL_FlatBlock(int flatidx)
{
    dl_flat_t*  slot;
    byte*       block;
    const byte* src;

    if (flatidx < 0 || flatidx >= numflats)
        return NULL;

    DL_InitFlatCache();
    if (!dl_flat)
        return NULL;

    slot = &dl_flat[flatidx];
    // A demoted (PU_CACHE) block may be reclaimed by the zone LRU; it NULLs
    // slot->raw (the back-referenced user ptr) when it does, so re-derive
    // slot->block from raw each touch and re-copy if raw is gone.
    if (slot->raw && slot->block)
        return slot->block;
    slot->block = NULL;

    // Over-allocate by 7 bytes so the row-major view is 8-byte aligned (the RDP
    // DMA into TMEM requires an 8-byte-aligned source; Z_Malloc only guarantees
    // 4). User ptr back-references the slot so a zone reclaim NULLs slot->raw.
    slot->raw = Z_Malloc(DL_FLAT_BYTES + 7, PU_STATIC, (void**)&slot->raw);
    if (!slot->raw)
        return NULL;
    block = (byte*)(((uintptr_t)slot->raw + 7) & ~(uintptr_t)7);

    // Copy the flat lump bytes (already row-major CI8). Cache PU_CACHE during
    // the copy then writeback; the block stays PU_STATIC (slot->raw) so the
    // copy source can be released immediately.
    src = (const byte*)W_CacheLumpNum(firstflat + flatidx, PU_CACHE);
    if (!src)
    {
        Z_Free(slot->raw);
        slot->raw = NULL;
        return NULL;
    }
    memcpy(block, src, DL_FLAT_BYTES);
    data_cache_hit_writeback(block, DL_FLAT_BYTES);

    slot->block  = block;
    slot->pinned = 1;       // born PU_STATIC; demoted on the wall schedule
    slot->lastuse = dl_present_gen;
    return block;
}

static void DL_FlatMarkInFlight(int flatidx)
{
    dl_flat_t* slot;

    if (!dl_flat)
        return;
    slot = &dl_flat[flatidx];
    if (!slot->raw)
        return;
    if (!slot->pinned)
    {
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
    // Flat buckets (Stage 4) share the dl_frame_gen stamp; allocate alongside.
    if (numflats > 0)
    {
        dl_flat_bucket_head = (int32_t*)Z_Malloc(numflats * sizeof(int32_t),
                                                 PU_STATIC, 0);
        dl_flat_bucket_gen  = (uint32_t*)Z_Malloc(numflats * sizeof(uint32_t),
                                                  PU_STATIC, 0);
        memset(dl_flat_bucket_gen, 0, numflats * sizeof(uint32_t));
    }
    dl_frame_gen = 1;   // 0 is the cleared-stamp sentinel; start at 1
    dl_buckets_inited = 1;
}

void DL_BeginFrame(void)
{
    dl_wall_count = 0;
    dl_arena_overflow = 0;
    dl_touched_count = 0;

    // Stage-4 plane span arena reset.
    dl_span_count = 0;
    dl_span_overflow = 0;
    dl_flat_touched_count = 0;

    // Per-frame band-load counter reset (bench diagnostic).
    dl_tile_loads = 0;
    // Sibling per-present primitive counters reset (same lifecycle).
    dl_recs    = 0;
    dl_uploads = 0;
    dl_tris    = 0;

    // Per-frame generation bump invalidates every bucket head/tail in O(1):
    // a bucket whose gen stamp != dl_frame_gen is treated as empty, so the
    // numtextures-sized head/tail arrays never need a per-frame memset (only
    // the small dl_touched[] list is walked at flush). Wrap is benign: gen 0
    // is the sentinel, so on the rare 2^32 wrap we re-base to 1 and clear once.
    // The flat bucket gen array shares dl_frame_gen, so clear it on the same wrap.
    DL_InitBuckets();
    if (++dl_frame_gen == 0)
    {
        if (dl_bucket_gen)
            memset(dl_bucket_gen, 0, numtextures * sizeof(uint32_t));
        if (dl_flat_bucket_gen)
            memset(dl_flat_bucket_gen, 0, numflats * sizeof(uint32_t));
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

// Plane light level (Stage 4). R_MapPlane resolves ds_colormap = planezlight[
// index] (or fixedcolormap for invuln/light-amp), a pointer into the colormap
// ramp == colormaps + level*256. Reduce it to the same level index the walls'
// dl_prim_lut is keyed on, so flats and walls share ONE baked PRIM LUT. Own
// one-entry memo (adjacent spans of a visplane at the same distance share a cm).
static const lighttable_t* dl_pll_cm    = NULL;
static uint8_t             dl_pll_level = 0;

uint8_t DL_PlaneLightLevel(const void* colormap)
{
    const lighttable_t* cm = (const lighttable_t*)colormap;
    long level;

    if (!cm || !colormaps)
        return 0;
    if (cm == dl_pll_cm)
        return dl_pll_level;

    level = (cm - colormaps) / 256;
    if (level < 0)
        level = 0;
    if (level >= NUMCOLORMAPS)
        level = NUMCOLORMAPS - 1;

    dl_pll_cm    = cm;
    dl_pll_level = (uint8_t)level;
    return (uint8_t)level;
}

#if DL_DEBUG_TRACE
static int dl_drop_count;       // records dropped on arena overflow this frame
static int dl_span_drop_count;  // spans dropped on arena overflow this frame
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

// --- Stage-4 plane span emit -----------------------------------------------
// Called from R_MapPlane's leaf instead of spanfunc() when DL_PlaneRouteOn().
// Appends ONE rdp_span_t to the arena and chains it into the flat's bucket so
// DL_FlushSpans uploads each flat once then draws all its spans (the autosync
// collapse, mirrored from walls). LOW primitive count is the whole experiment:
// one record per span run, one tri-pair at draw time (no band multiplication --
// a span samples one thin V-slice of the 64x64 flat). Overflow drops the span
// (it stays key-index -- a punched hole over stale fb, the bounded class).
void DL_EmitSpan(int y, int x1, int x2,
                 fixed_t xfrac, fixed_t yfrac, fixed_t xstep, fixed_t ystep,
                 int flatlump, const void* colormap)
{
    int         idx;
    int         flatidx;
    rdp_span_t* sp;

    if (x2 < x1)
        return;
    if (dl_span_count >= DL_SPAN_ARENA)
    {
        dl_span_overflow = 1;
#if DL_DEBUG_TRACE
        dl_span_drop_count++;
#endif
        return;         // arena full: drop (span stays key-index)
    }

    idx = dl_span_count++;
    sp = &dl_spans[idx];
    sp->y     = (int16_t)y;
    sp->x1    = (int16_t)x1;
    sp->x2    = (int16_t)x2;
    sp->u0    = xfrac;
    sp->v0    = yfrac;
    sp->ustep = xstep;
    sp->vstep = ystep;
    sp->flatlump    = (uint16_t)flatlump;
    sp->light       = DL_PlaneLightLevel(colormap);
    sp->bucket_next = -1;

    // Chain into the flat's bucket (indexed by flat index = lump-firstflat).
    flatidx = flatlump - firstflat;
    if (dl_flat_bucket_head && flatidx >= 0 && flatidx < numflats)
    {
        if (dl_flat_bucket_gen[flatidx] != dl_frame_gen)
        {
            dl_flat_bucket_gen[flatidx]  = dl_frame_gen;
            dl_flat_bucket_head[flatidx] = idx;
            if (dl_flat_touched_count < DL_SPAN_ARENA)
                dl_flat_touched[dl_flat_touched_count++] = (uint16_t)flatidx;
        }
        else
        {
            // Head-insert: order within a flat's bucket does not matter for
            // planes (spans never overlap -- each owns its own screen row), so
            // the cheaper head-insert is used. Draw order is irrelevant.
            sp->bucket_next = dl_flat_bucket_head[flatidx];
            dl_flat_bucket_head[flatidx] = idx;
        }
    }
}

int DL_SpanCount(void)
{
    return dl_span_count;
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
// represent the truncated/clipped span shape -- split. This is COVERAGE
// (correctness: an undercovered row stays key-index = a junction gap), so it
// stays TIGHT at ~1 row -- NOT a perf lever.
//
// S: the floor admits the sub-texel residue of projective math; the adaptive
// term (DL_SPLIT_SCOEF * local per-column S step) admits the half-column anchor
// uncertainty at glancing minification. Ryan ACCEPTED the CI4 glancing look, so
// the floor is RAISED 1.5 -> 2.0 texels: a glancing wall whose projective S
// already tracks software within ~2 texels no longer shatters into extra
// sub-pieces (each split is +1 record = +2 triangles + dlbuild). The look comes
// from S compression itself, not the split count, so it is unchanged (glancing
// A/B confirms). MEASURED: net-neutral on avg in the bench scenario (few glancing
// segs there) with a small p95 improvement -- it is a tail/glancing-frame lever,
// not a hot-path one; pushing the floor/coefficient harder (2.75/1.0) measured a
// slight avg REGRESSION (larger runs -> wider T-span -> more band work), so 2.0
// is the net-neutral-or-better setting. Y coverage (DL_SPLIT_DEVY) stays TIGHT
// at ~1 row -- correctness (an undercovered row is a junction gap), not a lever.
#define DL_SPLIT_DEVY   1.25f
#define DL_SPLIT_DEVS   2.0f
#define DL_SPLIT_SCOEF  0.51f

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
            sthresh = DL_SPLIT_SCOEF * (float)maxstep;
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

int DL_PlaneRouteOn(void)
{
    if (!n64_use_rdp_renderer)
        return 0;
    if (!n64_rdp_plane_ab)
        return 0;       // A/B toggle: keep planes on the CPU
    return 1;
}

int DL_AnyRouteOn(void)
{
    return DL_WallRouteOn() || DL_PlaneRouteOn();
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

static int DL_DrawRecord(const rdp_wall_t* w, byte* block, int blkh, int blkw,
                         int pal_slot, int ds_shift, int ts_shift, int fits_hw)
{
    int     cap;            // max CI4 tile rows fitting the lower TMEM half
    int     pad_w;          // stored CI4 period (texels/row; >=16 for pitch)
    int     pitchb;         // CI4 row pitch in bytes (pad_w/2)
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

    // S SCALE: ds_shift is 0 at native resolution (no width downsample), so this
    // is an identity scale -- the emit's original-width S maps 1:1 into the stored
    // blkw-wide space. The 1/2^ds_shift form is retained so a future SELECTIVE
    // per-texture downsample (ds_shift>0) would still map original column c to
    // stored column c >> ds_shift before the period bias/mask.
    {
        float sscale = 1.0f / (float)(1 << ds_shift);
        float es_l = w->s_l * sscale;
        float es_r = w->s_r * sscale;

    // S bias reduction (BUG D, saturation guard): texturecolumn is unbounded
    // (rw_offset + tangent term), but rdpq_triangle's attribute fixed point
    // saturates at |S| >= 1024 texels, smearing the texture. The tile wraps S
    // with mask log2(blkw), so subtracting a SHARED whole number of periods from
    // both endpoints is sampling-identical; after the bias the smaller endpoint
    // lies in [0, blkw) and the emit-side run split caps |s_r - s_l| at 700, so
    // |S| < blkw + 700 < 1024.
        float smin = (es_l < es_r) ? es_l : es_r;
        float bias = floorf(smin / (float)blkw) * (float)blkw;
        s_l = es_l - bias;
        s_r = es_r - bias;
    }

    // CI4 HARDWARE S-WRAP (the headline one-quad-per-wall lever). The whole
    // texture width is one tile and the RDP S-mask (= log2(blkw)) wraps S in
    // HARDWARE -- no adaptive S window, no S-split bands. The CI4 row is half
    // the bytes of CI8, so the TMEM cap DOUBLES: 64-wide caps at 4096/64 = 64
    // rows (a 64x64 texture = ONE quad; a 64x128 = 2 T-bands vs CI8's 4),
    // 128-wide caps at 32, 256-wide at 16. pad_w pads tw<16 up to a 16-texel
    // (8-byte) row so the tile pitch is legal; the pad texels are never sampled
    // (S-mask folds into [0,blkw)).
    pad_w  = (blkw < 16) ? 16 : blkw;
    pitchb = pad_w / 2;     // bytes/row (multiple of 8)

    cap = DL_TMEM_HALF / pitchb;
    if (cap < 1)
        return 0;

    // Configure the CI4 draw tile (TILE0) + the internal I8 load tile (TILE1)
    // once per (texture period, palette slot). 4-bit textures cannot be
    // LOAD_TILE'd directly: the RDP loads them through a byte (I8) view at half
    // the S width (2 CI4 texels = 1 byte), then a separate CI4 tile descriptor
    // (with the palette) is used for DRAWING -- this is libdragon's own CI4
    // pattern (texload_tile_4bpp). TILE0 (CI4): pitch = pad_w/2 bytes, hardware
    // S-wrap (mask = log2(blkw)), T clamps at the loaded band rows (vertical
    // wrap is done by the T-band walk, which also handles non-pow2 heights -- so
    // hardware T-mask is NOT used), palette = the frame's assigned CI4 sub-
    // palette slot (0..15) in the 256-entry TLUT region. TILE1 (I8): same TMEM
    // addr + byte pitch, used only as the LOAD_TILE target. dl_last_tile_lw/wrap
    // reused as the dedup key: lw <- blkw (period), wrap <- pal_slot.
    if (blkw != dl_last_tile_lw || pal_slot != dl_last_tile_wrap)
    {
        rdpq_tileparms_t tp;
        int maskbits = 0;
        int wbit;

        memset(&tp, 0, sizeof(tp));
        for (wbit = blkw; wbit > 1; wbit >>= 1)
            maskbits++;
        tp.s.mask = maskbits;
        tp.t.clamp = true;
        tp.palette = pal_slot;
        rdpq_set_tile(TILE1, FMT_I8, 0, pitchb, NULL);      // internal load tile
        rdpq_set_tile(TILE0, FMT_CI4, 0, pitchb, &tp);      // draw tile
        dl_last_tile_lw   = blkw;
        dl_last_tile_wrap = pal_slot;
        dl_last_up_block  = NULL;
    }

    // Per-edge T ranges (texel rows at the rect's top/bottom screen rows,
    // through each edge's own 1/scale -- see DL_RouteEmit). Both increase
    // down-screen. The band walk marches their UNION; a band's slice is clamped
    // per edge, so adjacent slices share their boundary chord exactly (no gap,
    // no overlap) and the union tiles the full rect.
    //
    // T SCALE: ts_shift is 0 at native resolution (no height downsample), so this
    // is an identity scale -- the emit's original-height T maps 1:1 into the
    // stored blkh rows. blkh is the full texture height, the vertical wrap period
    // both paths honour (fast path via hardware T-mask = log2(blkh); band walk via
    // mod-blkh period split). The 1/2^ts_shift form is retained for a future
    // SELECTIVE per-texture downsample only.
    {
        float tscale = 1.0f / (float)(1 << ts_shift);
        tl0 = w->t_top_l * tscale;
        tl1 = w->t_bot_l * tscale;
        tr0 = w->t_top_r * tscale;
        tr1 = w->t_bot_r * tscale;
    }
    t0 = (tl0 < tr0) ? tl0 : tr0;   // union range start
    t1 = (tl1 > tr1) ? tl1 : tr1;   // union range end

    // HARDWARE T-WRAP ONE-QUAD FAST PATH (DO #3). When the whole stored block fits
    // the TMEM half and both stored dims are pow2 (fits_hw), load all blkh rows
    // ONCE with hardware T-mask = log2(blkh) and draw a SINGLE tri-pair: hardware
    // wraps BOTH S and T, so a wall of any height is one quad (1 LOAD_TILE deduped
    // per texture + 2 tris), never a band walk. This is DOOM 64's shipped wall
    // shape and the residual-load lever: 64x64-class (incl. T-downsampled 64x128)
    // textures stop contributing 2+ bands/record. The T attribute is the stored
    // texel-T directly (no period-relative base); the tile descriptor's t.mask
    // does the vertical tiling.
    if (fits_hw)
    {
        int   thbits = 0;
        int   hbit;
        float ytl, ytr, ybl, ybr;

        // (Re)configure the draw tile with hardware T-mask when the texture
        // period or palette changed (deduped on blkw/pal_slot like the band path).
        if (blkw != dl_last_tile_lw || pal_slot != dl_last_tile_wrap)
        {
            rdpq_tileparms_t tp;
            int maskbits = 0, wbit;

            memset(&tp, 0, sizeof(tp));
            for (wbit = blkw; wbit > 1; wbit >>= 1)
                maskbits++;
            for (hbit = blkh; hbit > 1; hbit >>= 1)
                thbits++;
            tp.s.mask = maskbits;
            tp.t.mask = thbits;             // hardware vertical wrap (pow2 blkh)
            tp.palette = pal_slot;
            rdpq_set_tile(TILE1, FMT_I8, 0, pitchb, NULL);  // internal load tile
            rdpq_set_tile(TILE0, FMT_CI4, 0, pitchb, &tp);  // draw tile
            dl_last_tile_lw   = blkw;
            dl_last_tile_wrap = pal_slot;
            dl_last_up_block  = NULL;
        }

        // Load all blkh rows once (deduped: same block, first row 0, blkh rows).
        if (block != dl_last_up_block || dl_last_up_lo != 0
            || dl_last_up_rows != blkh || dl_last_up_c0 != blkw)
        {
            rdpq_load_tile(TILE1, 0, 0, blkw / 2, blkh);
            rdpq_set_tile_size(TILE0, 0, 0, blkw, blkh);
            dl_tile_loads++;
            dl_uploads++;       // deduped upload, tracks dl_tile_loads
            dl_last_up_block = block;
            dl_last_up_lo    = 0;
            dl_last_up_rows  = blkh;
            dl_last_up_c0    = blkw;
        }

        // Single tri-pair over the full screen rect; per-edge T is the stored
        // texel-T (hardware wraps mod blkh). Y edges are the record's own edges.
        ytl = w->ytop_l; ybl = w->ybot_l;
        ytr = w->ytop_r; ybr = w->ybot_r;
        {
            float tl[5] = { xl, ytl, s_l, tl0, w->invw_l };
            float tr[5] = { xr, ytr, s_r, tr0, w->invw_r };
            float bl[5] = { xl, ybl, s_l, tl1, w->invw_l };
            float br[5] = { xr, ybr, s_r, tr1, w->invw_r };

            rdpq_triangle(&TRIFMT_TEX, tl, tr, bl);
            rdpq_triangle(&TRIFMT_TEX, tr, br, bl);
            dl_tris += 2;       // one-quad fast path: 2 tris / 1 upload
        }
        dl_recs++;              // one record drawn (one-quad path)
        return 1;
    }

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
        // resident from the previous record's draw (UPLOAD DEDUP above). The
        // band is the FULL texture width [0, blkw) loaded as CI4; the row pitch
        // in RDRAM is pitchb bytes (= pad_w/2). CI4 LOAD goes through the I8 view
        // (TILE1) at HALF the S width (2 CI4 texels/byte), then the CI4 draw tile
        // (TILE0) gets the full CI4 texel extents via SET_TILE_SIZE -- the same
        // two-step the libdragon tex-loader uses for 4-bit. The DRAW T is
        // period-relative (base=period_base) so set the tile T0 to src_lo (the
        // band's first source row), matching the loaded rows.
        bandsrc = block + (src_lo * pitchb);
        if (bandsrc != dl_last_up_block
            || src_lo != dl_last_up_lo
            || rows_up != dl_last_up_rows
            || blkw != dl_last_up_c0)
        {
            rdpq_load_tile(TILE1, 0, src_lo, blkw / 2, src_lo + rows_up);
            rdpq_set_tile_size(TILE0, 0, src_lo, blkw, src_lo + rows_up);
            dl_tile_loads++;
            dl_uploads++;       // deduped band upload, tracks dl_tile_loads
            dl_last_up_block = bandsrc;
            dl_last_up_lo    = src_lo;
            dl_last_up_rows  = rows_up;
            dl_last_up_c0    = blkw;
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
            dl_tris += 2;       // band slice: 2 tris (held to tris = 2 * uploads)
        }

        cur = band_end;
    }
    }
    dl_recs++;                  // one record drawn (band-walk path)
    return 1;
}

// --- Stage-4 plane span draw -----------------------------------------------
// Draw ONE plane span as a single affine textured tri-pair (persp OFF, set by
// the caller). The flat is 64x64 row-major CI8 sampling the resident master
// TLUT (DL_FlatBlock). A span is a 1px-tall screen rect [x1..x2+1] x [y..y+1];
// U/V are affine in column (DOOM constant-z floors), constant down the 1px
// height. U(column) <- xfrac, V(row) <- yfrac (r_draw.c spot formula), both wrap
// mod 64.
//
// TMEM: a 64-wide CI8 flat caps at DL_TMEM_HALF/64 = 32 rows in the lower TMEM
// half (the master TLUT owns the upper half), so the full 64-row flat does NOT
// fit at once. A single span samples a NARROW V slice (one distance-slice of the
// flat), so we load just the V-window the span touches -- bias both endpoints
// into the period and load [vlo, vlo+rows) with rows <= 32, then ONE tri-pair.
// This is the LOW-primitive shape that keeps planes off the RSP-triangle floor
// that sank walls (no per-row band multiplication: a span is one primitive).
//
// block is the flat's row-major block (already fetched + pinned by the caller).
// The PRIM/tile-dedup statics (dl_last_*) are shared with DL_DrawRecord and were
// reset at the top of DL_Flush; DL_FlushSpans resets the tile/upload dedup again
// at its head because the flat tile geometry differs from the wall tiles.
static const uint32_t dl_unlit_prim = 0xFFFFFFFFu;

static void DL_DrawSpan(const rdp_span_t* sp, byte* block)
{
    uint32_t prim;
    int   nx = sp->x2 - sp->x1 + 1;     // columns in the run
    float ua, ub, va, vb;               // texel U/V at the rect's left/right edge
    int   vlo, vhi, rows;               // V window (texel rows) to upload
    int   ulo, uhi;                     // U extent (for the period bias only)
    int   vbias, ubias;                 // shared period biases (texels)
    float xl, xr, yt, yb;

    if (nx < 1)
        return;

    // PRIM = colormap-level brightness (Q8), shared LUT with the walls. Deduped.
    prim = (sp->light < NUMCOLORMAPS) ? dl_prim_lut[sp->light] : dl_unlit_prim;
    if (prim != dl_last_prim)
    {
        rdpq_set_prim_color(color_from_packed32(prim));
        dl_last_prim = prim;
    }

    // Texel U/V at the rect's screen edges. The rect spans columns [x1, x2+1) in
    // screen space; the per-column affine step maps column -> texel. U/V at the
    // LEFT edge (x1) are u0/v0; at the RIGHT edge (x2+1) they are u0/v0 advanced
    // by nx steps. 16.16 fixed -> float texels.
    ua = (float)sp->u0 * (1.0f / 65536.0f);
    va = (float)sp->v0 * (1.0f / 65536.0f);
    ub = (float)(sp->u0 + sp->ustep * nx) * (1.0f / 65536.0f);
    vb = (float)(sp->v0 + sp->vstep * nx) * (1.0f / 65536.0f);

    // Period-bias V into a small window so the upload fits the 32-row cap. The
    // tile wraps T with mask 6 (64 period), so subtracting a SHARED whole number
    // of periods from both V endpoints is sampling-identical. After the bias the
    // smaller endpoint lands in [0,64) and the window is [floor(vmin), ceil(vmax)].
    {
        float vmin = (va < vb) ? va : vb;
        vbias = (int)floorf(vmin / 64.0f) * 64;
    }
    va -= (float)vbias;
    vb -= (float)vbias;
    {
        float vmin = (va < vb) ? va : vb;
        float vmax = (va < vb) ? vb : va;
        vlo = (int)floorf(vmin);
        vhi = (int)ceilf(vmax);
    }
    if (vlo < 0) vlo = 0;
    // Window must fit the TMEM 32-row cap. A span whose V extent exceeds 32
    // texel rows is a very near, steep floor (rare); clamp the window to 32 rows
    // -- the over-extent texels wrap and re-sample within the loaded window, a
    // bounded minification artifact on extreme near floors (accepted; the
    // overflow class is documented). Most spans are << 32 rows (one band).
    //
    // UPLOAD-DEDUP WIDENING (the perf lever -- per-span LOAD_TILE is the plane
    // path's DL_BUILD floor). Always load a FULL 32-row window (the TMEM cap),
    // and ALIGN its base to a 16-row grid that still contains the span's exact
    // [vlo,vhi] extent. Consecutive spans of a flat drift V slowly row-to-row,
    // so an aligned 32-row window is reused across many spans -- the resident-
    // window containment check below then skips the LOAD_TILE entirely. This
    // collapses the dominant per-span upload to ~once per distance band.
    {
        int ext = vhi - vlo + 1;            // exact span V extent
        if (ext > 32) ext = 32;             // near-floor clamp (as before)
        // Align the 32-row window base to a 16-grid that still covers [vlo,vhi].
        // Prefer the lowest 16-aligned base whose +32 window contains vhi.
        int base = (vlo / 16) * 16;
        if (base + 32 < vlo + ext)          // window too low to cover the span
            base = ((vhi - 31) / 16) * 16;  // raise to cover the top
        if (base < 0) base = 0;
        if (base + 32 > 64) base = 64 - 32; // keep inside the 64 period
        if (base < 0) base = 0;
        vlo  = base;
        rows = 32;
        if (vlo + rows > 64) rows = 64 - vlo;
        if (rows < 1) rows = 1;
    }
    vhi = vlo + rows - 1;

    // U period bias (keep S endpoints in a sane fixed-point range; the tile
    // wraps S with mask 6, so a shared period subtraction is sampling-identical).
    ulo = (int)floorf((ua < ub) ? ua : ub);
    uhi = (int)ceilf((ua > ub) ? ua : ub);
    ubias = (ulo / 64) * 64;
    if (ulo < 0) ubias = ((ulo - 63) / 64) * 64;    // floor toward -inf
    (void)uhi;
    ua -= (float)ubias;
    ub -= (float)ubias;

    // Tile descriptor: full 64-period wrap on BOTH S and T (mask 6). Deduped --
    // every flat span shares the same tile geometry (64-wide, mask 6), so this
    // SET_TILE is issued once per flush, not per span.
    if (dl_last_tile_lw != 64 || dl_last_tile_wrap != 2)
    {
        rdpq_tileparms_t tp;
        memset(&tp, 0, sizeof(tp));
        tp.s.mask = 6;          // 64-texel S period
        tp.t.mask = 6;          // 64-texel T period
        rdpq_set_tile(TILE0, FMT_CI8, 0, 64, &tp);
        dl_last_tile_lw   = 64;
        dl_last_tile_wrap = 2;   // sentinel != wall wrap values (0/1)
        dl_last_up_block  = NULL;
    }

    // Load the (16-aligned, 32-row) V window across the full 64-texel width.
    // Period-relative coords; the triangle T (period-relative too) addresses
    // within [vlo, vlo+rows). Dedup: the window base/rows are 16-aligned and
    // 32-wide, so a span whose exact V extent already lies inside the RESIDENT
    // window reuses it -- skip the LOAD_TILE (and its autosync). This is the
    // dominant DL_BUILD saving for the plane path.
    if (block != dl_last_up_block || vlo != dl_last_up_lo || rows != dl_last_up_rows)
    {
        rdpq_load_tile(TILE0, 0, vlo, 64, vlo + rows);
        dl_last_up_block = block;
        dl_last_up_lo    = vlo;
        dl_last_up_rows  = rows;
        dl_last_up_c0    = 0;
    }

    // Screen rect: 1px-tall row at y, columns [x1, x2+1). Affine S/T per corner
    // (persp OFF, INV_W ignored -> linear interpolation == DOOM's span stepping,
    // exact). Top and bottom corners of a column share the same texel (V constant
    // down the 1px height). Tri-pair: (tl,tr,bl),(tr,br,bl).
    xl = (float)sp->x1;
    xr = (float)sp->x2 + 1.0f;
    yt = (float)sp->y;
    yb = (float)sp->y + 1.0f;
    {
        float tl[5] = { xl, yt, ua, va, 1.0f };
        float tr[5] = { xr, yt, ub, vb, 1.0f };
        float bl[5] = { xl, yb, ua, va, 1.0f };
        float br[5] = { xr, yb, ub, vb, 1.0f };
        rdpq_triangle(&TRIFMT_TEX, tl, tr, bl);
        rdpq_triangle(&TRIFMT_TEX, tr, br, bl);
    }
}

// Drain the frame's plane spans into the attached display fb (Stage 4). A
// per-FLAT bucket walk mirroring the wall flush: for each flat used this frame,
// copy + pin its row-major block ONCE, point the RDP at it, then draw every
// span in that flat's bucket. Caller has set standard mode + TEX_FLAT combiner +
// TLUT; this function sets persp OFF (flats are affine) for its draws. The
// upload/tile/PRIM dedup statics are shared with the wall path; reset them here
// because the flat tile geometry (64-wide, mask-6 wrap) differs from wall tiles.
static void DL_FlushSpans(void)
{
    int fi;

    if (dl_span_count <= 0)
        return;

    // Flats are affine (constant-z); turn perspective OFF so S/T interpolate
    // linearly (== DOOM's span stepping, exact). The walls left persp ON.
    rdpq_mode_persp(false);

    // Reset the dedup caches: the flat tile geometry differs from the wall tiles
    // the wall flush left resident, so no flat band is loaded yet.
    dl_last_up_block  = NULL;
    dl_last_up_lo     = -1;
    dl_last_up_rows   = -1;
    dl_last_up_c0     = -1;
    dl_last_tile_lw   = -1;
    dl_last_tile_wrap = -1;

    for (fi = 0; fi < dl_flat_touched_count; fi++)
    {
        int   flatidx = dl_flat_touched[fi];
        int   sidx;
        byte* block;

        block = DL_FlatBlock(flatidx);
        if (!block)
            continue;       // no usable flat block: spans stay key

        DL_FlatMarkInFlight(flatidx);

        // Point the RDP at this flat's 64x64 row-major block once. The TILE0
        // descriptor (64-wide, mask-6 wrap) is configured in DL_DrawSpan and
        // deduped; invalidate the resident band when the source image changes.
        {
            surface_t fs = surface_make_linear(block, FMT_CI8, 64, 64);
            rdpq_set_texture_image(&fs);
            dl_last_up_block  = NULL;
            dl_last_tile_lw   = -1;
            dl_last_tile_wrap = -1;
        }

        for (sidx = dl_flat_bucket_head[flatidx]; sidx >= 0;
             sidx = dl_spans[sidx].bucket_next)
        {
            DL_DrawSpan(&dl_spans[sidx], block);
        }
    }
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

    // CI4 DAMAGE-FLASH CORRECTNESS. Latch the current palette generation; each
    // texture's sub-palette is then re-tinted LAZILY at its upload site below
    // (DL_RetintSlot) if its colours don't track the current flash. So walls tint
    // with the world on damage/pickup/radsuit flashes, at O(touched) cost on a
    // flash frame and zero on un-flashed frames.
    DL_RetintSubPalettes();

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

        // TMEM PALETTE STRATEGY (CI4, spec A+B). Assign this texture a CI4
        // palette slot in the 256-entry TLUT region and upload its 16-colour
        // sub-palette there. Slot = touch-order mod 16: the first <=16 textures
        // each get a distinct slot (one upload apiece, no mid-frame swap). On
        // tail frames with >16 distinct textures the slot RE-USES an earlier
        // one, and we re-upload that slot's sub-palette here, immediately before
        // drawing this texture's bucket (the bucket walk draws all of a
        // texture's records consecutively, so the slot's colours are valid for
        // exactly this texture's draws). Sub-palette = 16 RGBA5551 entries
        // (32 B) -> rdpq_tex_upload_tlut at slot*16. The master 256-TLUT is
        // re-asserted after the whole wall pass (I_N64ForceTLUTReupload).
        {
            dl_rowmajor_t* slot = &dl_rowmajor[tex];
            int pal_slot = ti & 15;
            surface_t texsurf;

            slot->pal_slot = (uint8_t)pal_slot;
            // First-touch-during-flash: DL_RowMajorBlock just built this slot from
            // base PLAYPAL (the flush-top sweep ran before it existed), so re-tint
            // it from the current master before its sub-palette is uploaded. No-op
            // (gen match) for slots the sweep already handled.
            DL_RetintSlot(slot, dl_retint_gen);
            // Copy this texture's sub-palette into ITS slot's persistent scratch
            // (the LOAD_TLUT reads it asynchronously; per-slot buffers keep each
            // alive through the frame's async window). Tail frames with >16
            // distinct textures reuse a slot: the prior owner's triangles must
            // have drained before the buffer is overwritten, so sync there.
            if (ti >= 16)
            {
                rdpq_sync_tile();
                rdpq_sync_load();
            }
            memcpy(dl_subpal_up[pal_slot], slot->subpal, sizeof(slot->subpal));
            data_cache_hit_writeback(dl_subpal_up[pal_slot], sizeof(slot->subpal));
            rdpq_tex_upload_tlut(dl_subpal_up[pal_slot], pal_slot * 16, 16);

            // Point the RDP at the FULL row-major CI4 block ONCE per texture,
            // interpreted as an I8 byte image (the CI4 LOAD goes through the I8
            // view at half the S width -- see DL_DrawRecord). Width = the CI4 row
            // pitch in bytes (pad_w/2); height = blkh. The TILE0/TILE1 descriptors
            // are configured + deduped inside the band path; invalidate that
            // state here because the period/palette is per-texture.
            {
                int pad_w  = (blkw < 16) ? 16 : blkw;
                int pitchb = pad_w / 2;
                texsurf = surface_make_linear(block, FMT_I8, pitchb, blkh);
                rdpq_set_texture_image(&texsurf);
            }
            dl_last_tile_lw   = -1;
            dl_last_tile_wrap = -1;
            dl_last_up_block  = NULL;

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
                DL_DrawRecord(w, block, blkh, blkw, pal_slot, slot->ds_shift,
                              slot->ts_shift, slot->fits_hw);
            }
        }
    }

    // The wall pass overwrote the 256-entry TLUT region with CI4 sub-palettes;
    // re-assert the master 256-TLUT before the present blit so CI8 sprites/HUD/
    // overlay sample the right colours (TMEM palette strategy B). Only when
    // walls actually drew -- a planes-only frame never touched the TLUT.
    if (dl_touched_count > 0)
        I_N64ForceTLUTReupload();

    // Overflow fallback (design risk table "DL arena overflow on tail frames").
    // If the arena filled, records past DL_WALL_ARENA were never queued (their
    // columns stay key-index, a punched hole -- but they were dropped, not
    // mis-drawn). Nothing else to do: the bucket walk already drew every record
    // that DID fit. The flag exists so a future tail-frame can be detected and
    // the arena grown if it ever bites; with DL_WALL_ARENA=512 it should not.
    (void)dl_arena_overflow;

    // Stage-4: drain plane spans (floors/ceilings) after the walls, same rspq
    // stream, still scissored to the view window. DL_FlushSpans sets persp OFF
    // (flats are affine) -- the caller's post-flush persp(false) for the blit is
    // then redundant but harmless. A planes-only frame (0 walls) enters DL_Flush
    // via the DL_SpanCount() OR in the present gate, runs the empty wall walk
    // above (dl_touched_count==0), and draws its floors here.
    DL_FlushSpans();
    (void)dl_span_overflow;
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

    // Stage-4: same demote sweep for the flat cache (shares dl_present_gen).
    if (dl_flat)
    {
        for (i = 0; i < numflats; i++)
        {
            dl_flat_t* slot = &dl_flat[i];

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
    dl_span_count = 0;      // Stage 4: same -- never re-flush stale spans (an
                            // automap/wipe present must not re-draw last world
                            // frame's floors; risk 7 stale-rectangle class)
}

#endif // N64
