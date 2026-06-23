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
#ifdef BENCH_FORCE_MESH_RSP
#include <stdlib.h>             // free (RSP loopback probe)
#include <malloc.h>             // memalign (cache-line aligned DMA buffers, RSP probe)
#endif
#include <math.h>

#include <libdragon.h>

#include "doomdef.h"
#include "doomstat.h"
#include "m_fixed.h"
#include "r_defs.h"
#include "r_main.h"
#include "r_state.h"            // viewx/viewangle/centerx/projectiony (mesh transform)
#include "tables.h"             // finesine/finecosine/ANGLETOFINESHIFT (mesh transform)
#include "r_bake.h"             // bake_walls / bake_numwalls (GPU port)
#include <math.h>               // sqrtf (mesh wall length)
#include "z_zone.h"
#include "w_wad.h"
#include "i_system.h"
#include "i_video.h"

#include "rdp_view.h"

// --- Integer floor/ceil for the wall emit path (perf-only refactor) --------
// IFLOOR/ICEIL compute floor()/ceil() of a float with integer/branch arithmetic
// instead of the libm floorf()/ceilf() calls. Truncation toward zero ((int)x)
// equals floor for x>=0 and equals ceil for x<=0; the correction term restores
// the toward-(-/+)inf rounding for the signed fractional case:
//   IFLOOR(x) = trunc(x) - (trunc(x) > x)   // -1 only when x<0 and fractional
//   ICEIL(x)  = trunc(x) + (trunc(x) < x)   // +1 only when x>0 and fractional
// These are PROVABLY identical to floorf/ceilf for every finite x whose true
// floor/ceil fits in int32. Every operand here is bounded well under 2^20 texels
// (traced: projective T, period-biased S, and 16.16 spot U/V), so the int cast
// never overflows -- the replacement is bit-identical to the float result.
// trunc() is evaluated ONCE (no double-eval of side effects; all call sites pass
// plain variables/divisions anyway).
static inline int dl_ifloor(float x) { int t = (int)x; return t - (t > x); }
static inline int dl_iceil (float x) { int t = (int)x; return t + (t < x); }
#define IFLOOR(x) dl_ifloor(x)
#define ICEIL(x)  dl_iceil(x)

// IFLOOR_ASSERT_ENABLE: BENCH-gated equivalence guard. Flip to 1 to compile in a
// per-site assertion that the integer result MATCHES the original floorf/ceilf
// on every real frame (any divergence calls I_Error and fail-fasts the bench).
// MUST be 0 for timing runs and for the committed tree -- the floorf/ceilf
// shadow calls and the compare defeat the whole point of the refactor.
#ifndef IFLOOR_ASSERT_ENABLE
#define IFLOOR_ASSERT_ENABLE 0
#endif
#if IFLOOR_ASSERT_ENABLE
#define IFLOOR_CHK(iv, x) do { \
    if ((iv) != (int)floorf(x)) \
        I_Error("IFLOOR ASSERT FAILED: x=%f int=%d floorf=%d\n", \
                (double)(x), (int)(iv), (int)floorf(x)); \
} while (0)
#define ICEIL_CHK(iv, x) do { \
    if ((iv) != (int)ceilf(x)) \
        I_Error("ICEIL ASSERT FAILED: x=%f int=%d ceilf=%d\n", \
                (double)(x), (int)(iv), (int)ceilf(x)); \
} while (0)
#else
#define IFLOOR_CHK(iv, x) ((void)0)
#define ICEIL_CHK(iv, x)  ((void)0)
#endif

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

// ---- PLANE_UV_TRACE: post-period-bias plane-poly S/T saturation self-trace ---
// Diagnostic ONLY (no rendering change). When -DPLANE_UV_TRACE=1 (Makefile
// PLANE_UV_TRACE=1; default 0 -> compiled out of normal/timing builds). This is
// the SECOND half of the floor/ceiling diagnostic: r_plane.c's PLANE_UV_TRACE
// dumps the EMITTED corner texels vs R_MapPlane EXPECTED; THIS one dumps, in
// DL_DrawPlanePoly, the FINAL u,v handed to rdpq_triangle -- i.e. AFTER the
// whole-64 period bias (ubias/vbias) and the V *0.5 decimation -- so the s10.5
// S/T fixed point (texel * 32 -> int16, saturates at |u*32| >= 32767, i.e.
// |u| >= ~1024 texels; see DL_Flush note) is computed on the EXACT values the
// RDP sees. Per sample poly it prints PUVT2_POLY: the 4 corners' post-bias u,v,
// each corner's u*32 / v*32 with a SAT flag, the texel SPREAD (umax-umin,
// vmax-vmin), the screen extent (x1,x2 + ytop range), and FLOOR/CEILING (by the
// poly's mean screen row vs centery). The hypothesis under test: glancing/deep
// views give one plane poly a huge texel spread that overruns s10.5 -> garbage/
// infinite-stretch; head-on frames have small spread -> clean. The dump is keyed
// to a BENCH marker frame (N64Bench_FrameNo()) so "frame 128/384" here is the
// SAME frame as BENCH_MARK frame=128/384. Two frames can be captured in one run
// (PLANE_UV_TRACE_FRAME + PLANE_UV_TRACE_FRAME2). Uses debugf -> ISViewer.
#ifndef PLANE_UV_TRACE
#define PLANE_UV_TRACE 0
#endif
#if PLANE_UV_TRACE
#include <math.h>               // floorf (trace float formatting)
#include "n64_bench.h"          // N64Bench_FrameNo (marker-frame pairing)
#include "r_main.h"             // centery (floor vs ceiling discriminator)
// Which BENCH marker frame(s) to dump. The render whose COMMIT produces marker N
// sees N64Bench_FrameNo() == N-1 (committed-so-far), so dump when the counter is
// FRAME-1 to pair the lines with `BENCH_MARK frame=FRAME`. 0 disables a slot.
#ifndef PLANE_UV_TRACE_FRAME
#define PLANE_UV_TRACE_FRAME  128   // garbage frame (Ryan)
#endif
#ifndef PLANE_UV_TRACE_FRAME2
#define PLANE_UV_TRACE_FRAME2 384   // clean frame (Ryan); 0 to dump one frame
#endif
#ifndef PLANE_UV_TRACE_POLYS
#define PLANE_UV_TRACE_POLYS  16    // dump at most this many polys per frame (high
                                    // enough to reach CEILING polys -- the per-flat
                                    // bucket walk emits floors first, ceilings later)
#endif
// Float -> int + signed milli-frac (debugf has no %f here). Match r_plane.c's.
#ifndef IFLOORF
#define IFLOORF(f)   ((int)floorf((float)(f)))
#endif
#ifndef MILLIFRAC
#define MILLIFRAC(f) ((int)(( (float)(f) - floorf((float)(f)) ) * 1000.0f))
#endif
// s10.5 S/T saturation: rdpq_triangle scales the per-vertex S/T by 32 and casts
// to int16; |S*32| >= 32767 saturates -> texture smears across the quad. Test on
// the FINAL (post-bias, post-decimation) u,v -- exactly what rdpq_triangle gets.
#define PUVT2_SAT_LIMIT 32767
#define PUVT2_SAT(uv)   ( (((float)(uv)) * 32.0f >=  (float)PUVT2_SAT_LIMIT) || \
                          (((float)(uv)) * 32.0f <= -(float)PUVT2_SAT_LIMIT) )
// Per-slot capture state. Each target frame fires ONCE: the first plane-poly
// render whose committed-frame-no reaches (FRAME-1) ARMS the slot and dumps up
// to PLANE_UV_TRACE_POLYS polys, all within that one frame (puvt2_arm_fno pins
// the frame so later frames can't re-trigger a fired slot). Latching on ">=
// FRAME-1" (not "== FRAME-1") makes the capture robust to the bench's excluded
// level-reload frames skewing which exact fno the target render lands on -- a
// skipped frame can't make the slot silently miss its window.
static int           puvt2_fired[2]   = { 0, 0 };          // slot already dumped?
static unsigned long puvt2_arm_fno[2] = { (unsigned long)-1, (unsigned long)-1 };
static int           puvt2_polys_left[2] = { -1, -1 };     // polys left this slot
#endif // PLANE_UV_TRACE

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

// --- per-seg wall A/B toggle -----------------------------------------------
// Defaults to CPU walls (0): the shipped "RENDERER: RDP" menu option is RDP
// floors/ceilings + SOFTWARE walls (the "planes-only" config). Benched 2026-06-16:
// planes-only beats software on BOTH avg and p95 (18814/30432 vs 19793/31648),
// with no wall glancing-smear and full COMPUTE2 fidelity. Full RDP (walls on, =1)
// ties on avg but loses the p95 tail (+12.6%) and carries the smear, so it is NOT
// shipped. BENCH_FORCE_WALLS_ONLY pins this to 1 for the isolation A/B (d_main.c).
int n64_rdp_wall_ab = 0;

// Plane A/B toggle. Defaults to the RDP path (1): combined with the CPU-wall
// default above, the shipped "RDP" toggle routes PLANES to the RDP and keeps
// walls on the CPU = the planes-only ship config. BENCH_FORCE_WALLS_ONLY pins
// this to 0; BENCH_FORCE_PLANES_ONLY pins wall=0 & plane=1 (the same config).
int n64_rdp_plane_ab = 1;

// Stage-4b sub-path selector: 1 = emit visplanes as RDP POLYGONS (trapezoid
// strips, perspective-correct) -- the new low-primitive floor path; 0 = the
// legacy per-span affine path (Stage-4 DL_EmitSpan). Only consulted when the
// plane route is on; OFF (plane route off) is byte-identical software either way.
// Defaults to the polygon path (the feature this stage adds).
int n64_rdp_plane_poly = 1;

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

// --- Stage-4b plane POLYGON arena + per-flat buckets -----------------------
// Visplanes-as-RDP-polygons (the trapezoid-strip path replacing the per-span
// Stage-4 fill). One rdp_ppoly_t per trapezoid RUN; the count-only probe found
// mean ~47 / p95 ~150 runs(*2 tris)/frame, well under the wall arena, so a
// 2048-entry arena is generous (p95 ~75 runs). Overflow drops a run (it stays
// key-index -- a punched hole over stale fb, the bounded class), same discipline
// as walls/spans. Buckets reuse the SAME per-flat head/gen arrays + dl_frame_gen
// stamp the span path uses (a frame runs EITHER the span path OR the poly path,
// never both -- DL_PlanePolyOn selects), so no extra per-flat allocation.
#define DL_PPOLY_ARENA   2048

static rdp_ppoly_t  dl_ppolys[DL_PPOLY_ARENA];
static int          dl_ppoly_count;
static int          dl_ppoly_overflow;      // 1 if a poly was dropped this frame

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
// the routed wall set (DL_DS_DIAG build). This gate governs SUB-256 textures only
// (>=256-wides are decided earlier by the protect boundary + dl_downsample2x_names,
// NOT by this threshold). The sub-256 detail Ryan named -- STARTAN3 (128, metallic
// striping), TEKWALL (circuit accents) -- scores above it (fine vertical structure
// destroyed by a column-pair merge); large flat/low-frequency sub-256 wides score
// below. NB: COMPUTE2 (256-wide) is INTENTIONALLY 2x-halved via the HALF list, not
// gated here -- earlier comments grouping it with the native sub-256 set were stale.
// When in doubt the texture lands native (the safe side).
#define DL_DS_ERR_THRESH  450

// =====================================================================
//  COLLAPSE/PROTECT BOUNDARY (the one knob that sets which wides downsample)
// =====================================================================
// The boundary is a single WIDTH threshold: a wall texture stays NATIVE iff its
// sampling period is >= DL_PROTECT_MIN_W. Everything narrower (the 128-wide
// tiling walls -- STARTAN3, TEKWALL1, COMPTILE, BROWN1, STARG3, ...) COLLAPSES
// to the one-quad 64x64 CI4 class (S 128->64 AND T 128->64); everything >= the
// threshold (COMPUTE2 256x56 and any other >=256-wide) stays at full resolution.
//
//  *** To re-tune the boundary, change THIS ONE LINE. ***
//  Raise to 512 to also collapse the 256-wides; the value is the smallest width
//  that is KEPT NATIVE. 256 protects COMPUTE2's 256-px computer-readout detail
//  (a 4x horizontal squish destroys it, and as a one-off non-tiling texture
//  hardware S-wrap can't recover the loads) while collapsing the 128 tiling set.
#define DL_PROTECT_MIN_W  256

// Per-texture NAME OVERRIDE list (fine-grained, on TOP of the width boundary).
// Use this to flip an INDIVIDUAL texture's verdict without moving the global
// boundary: list a name here to pin it NATIVE even though it is below the width
// threshold (e.g. a 128-wide whose detail the preview flags). EMPTY by default --
// the prompt's split is captured entirely by DL_PROTECT_MIN_W, so no 128-wide is
// named-protected (STARTAN3/TEKWALL1 are now COLLAPSE targets, deliberately). Add
// a name as a one-line edit to protect it; the list resolves to texnums once
// (R_CheckTextureNumForName) and is cached. -1 entries (absent in WAD) never hit.
extern int R_CheckTextureNumForName(char* name);
static const char* const dl_protect_names[] = {
    // (empty) -- e.g. add "STARTAN3", here to keep that one 128-wide native.
    0,  // sentinel so the array is never zero-length (ISO C); skipped at resolve.
};
#define DL_PROTECT_N (int)(sizeof(dl_protect_names)/sizeof(dl_protect_names[0]))
static int  dl_protect_tex[DL_PROTECT_N];
static int  dl_protect_resolved;

// A wide is PROTECTED (stays native) if it meets the width boundary OR is named
// in the override list above. tw is the texture's sampling period (store-width
// candidate); pass texturewidthmask+1 at the call site.
static int DL_TexIsProtectedWide(int texnum, int tw)
{
    int i;
    if (tw >= DL_PROTECT_MIN_W)             // global width boundary (the knob)
        return 1;
    if (!dl_protect_resolved)
    {
        for (i = 0; i < DL_PROTECT_N; i++)
            dl_protect_tex[i] = dl_protect_names[i]
                ? R_CheckTextureNumForName((char*)dl_protect_names[i])
                : -1;                       // sentinel slot -> never matches
        dl_protect_resolved = 1;
    }
    for (i = 0; i < DL_PROTECT_N; i++)
        if (dl_protect_tex[i] == texnum)    // (-1 absent/sentinel never match)
            return 1;
    return 0;
}

// =====================================================================
//  2x DOWNSAMPLE TIER (named >=256-wides: halve BOTH axes once, band-split)
// =====================================================================
// A THIRD per-texture verdict, between NATIVE (protected) and ONE-QUAD collapse.
// The width boundary (DL_PROTECT_MIN_W) keeps every >=256-wide NATIVE; this list
// carves a named subset back OUT of native into a 2x downsample: ds_shift=1 AND
// ts_shift=1 (halve width 256->128 AND halve a pow2 height 128->64 in ONE step
// each). A 128x64 CI4 tile is (128/2)*64 = 4096 B > DL_TMEM_HALF (2048), so it
// does NOT reach the one-quad fits_hw budget -- it BAND-SPLITS normally (2 S-bands
// of 64x64). So this tier still cuts the >=256-wide's upload/triangle volume (the
// RSP-command lever) WITHOUT the 4x horizontal squish a full collapse to 64 would
// inflict on these large-period faces.
//
//  *** This is the tunable boundary: ONE LINE PER TEXTURE. ***
// Listed >=256-wides downsample 2x; any >=256-wide NOT listed stays NATIVE. SKY1
// is deliberately ABSENT (it rides the sky special path, never route here). Drop
// a name to pull it back to native, add a name to 2x-downsample it; resolves to
// texnums once (R_CheckTextureNumForName), cached, -1 (absent in WAD) never hits.
static const char* const dl_downsample2x_names[] = {
    "PLANET1",
    "COMPUTE2",
    "COMPTALL",
    "GRAY7",
    "STONE",
    "PIPE2",
    0,  // sentinel so the array is never zero-length (ISO C); skipped at resolve.
};
#define DL_DS2X_N (int)(sizeof(dl_downsample2x_names)/sizeof(dl_downsample2x_names[0]))
static int  dl_ds2x_tex[DL_DS2X_N];
static int  dl_ds2x_resolved;

// Is this texture in the 2x-downsample set above? (Name match only -- the caller
// gates on width so a sub-256 name here would never reach this; SKY1 is excluded
// by absence.) Returns 1 to take the 2x (half) tier, 0 to leave the texture on
// whatever its width verdict was (native for >=256, collapse for narrower).
static int DL_Tex2xDownsample(int texnum)
{
    int i;
    if (!dl_ds2x_resolved)
    {
        for (i = 0; i < DL_DS2X_N; i++)
            dl_ds2x_tex[i] = dl_downsample2x_names[i]
                ? R_CheckTextureNumForName((char*)dl_downsample2x_names[i])
                : -1;                       // sentinel slot -> never matches
        dl_ds2x_resolved = 1;
    }
    for (i = 0; i < DL_DS2X_N; i++)
        if (dl_ds2x_tex[i] == texnum)       // (-1 absent/sentinel never match)
            return 1;
    return 0;
}

// Per-texture downsample verdict (3-way classifier on width + the named lists):
//   DL_TIER_NATIVE  -- full resolution (protected >=256-wide not in 2x list, or
//                      a width that fails the collapse gate). ds=ts=0.
//   DL_TIER_HALF    -- 2x downsample (named >=256-wide): ds_shift=1, ts_shift=1
//                      for pow2 height; band-splits (not one-quad).
//   DL_TIER_ONEQUAD -- collapse-to-64 (the 128-wide tiling walls): ds/ts toward 64.
// The HALF tier takes priority over NATIVE for a named >=256-wide; ONEQUAD is the
// existing sub-256 collapse path (gated by the box-filter error metric downstream).
#define DL_TIER_NATIVE   0
#define DL_TIER_HALF     1
#define DL_TIER_ONEQUAD  2
// WALL TEXTURE DOWNSAMPLE -- DISABLED: route every wall texture NATIVE.
// The HALF/ONEQUAD tiers existed to cut RSP load/command volume, but the texture
// pre-load (DL_PrequantTexture at R_PrecacheLevel) now builds every routed block
// ONCE at level load, so the per-frame saving is gone. Measured all-native avg
// 20036us vs the downsampled 21873us this session -- same-or-better -- because the
// RDP is idle (rdpbusy ~5us) and CPU emit is the bottleneck, not RSP volume. And the
// halved blocks visibly SMEARED detailed walls (the brown-ribbed faces): a downsample
// should never smear, so native is both crisp and not slower. The classifier below is
// retained (gated by a non-const flag, so it stays live) for an easy re-enable if the
// perf regime ever shifts back to RSP-volume-bound.
static int dl_wall_downsample_enabled = 0;
static int DL_TexDownsampleTier(int texnum, int tw, int th)
{
    if (!dl_wall_downsample_enabled)
        return DL_TIER_NATIVE;
    if (DL_TexIsProtectedWide(texnum, tw))      // >=256-wide (or name-pinned native)
        return DL_Tex2xDownsample(texnum) ? DL_TIER_HALF : DL_TIER_NATIVE;
    // Below the width boundary: the existing one-quad collapse, still gated by the
    // box-filter reconstruction-error metric (detailed wides stay native).
    if (tw > 64 && DL_DownsampleErr(texnum, tw, th) < DL_DS_ERR_THRESH)
        return DL_TIER_ONEQUAD;
    return DL_TIER_NATIVE;
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
    // (DL_DownsampleErr for sub-256; the protect boundary + dl_downsample2x_names for
    // >=256-wides), conservative -- when in doubt, native:
    //   - STARTAN3 (128, metallic striping), TEKWALL (circuit accents): high HF and
    //     sub-256, so the error gate keeps them NATIVE -- no resolution loss. Ryan
    //     rejected the blanket downsample (038c7f3/e94c39a) on exactly these.
    //   - COMPUTE2 / the other dl_downsample2x_names >=256-wides: INTENTIONALLY 2x
    //     (256->128, the HALF tier) -- a deliberate load/fidelity tradeoff agreed with
    //     Ryan. NOT native; earlier comments that called COMPUTE2 native were stale.
    //   - Large flat/low-frequency sub-256 wides: low HF -> halve S (the box-average
    //     lands between two near-equal columns, no visible smear), recovering loads.
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

        // SELECTIVE S+T DOWNSAMPLE DECISION (3-way: native / 2x half / one-quad).
        // DL_TexDownsampleTier classifies by width + the named lists:
        //   - >=256-wide in dl_downsample2x_names -> HALF (2x, band-split below).
        //   - >=256-wide NOT in that list (or name-pinned) -> NATIVE (full res).
        //   - sub-256, low box-filter error -> ONEQUAD collapse-to-64.
        //   - sub-256, high error (detailed) -> NATIVE.
        // The width boundary still pins the >=256-wides native by DEFAULT; the 2x
        // list carves a named subset back out. When a wide DOWNSAMPLES (HALF or
        // ONEQUAD) we take its axes down in ONE halving step each:
        //   - S 128->64 (or 256->128, capped at 64): halve the stored width so the
        //     CI4 row fits the TMEM half (the load-band lever).
        //   - T 128->64: halve the stored HEIGHT too, but ONLY for a pow2 height
        //     > 64. A 128x128 wall then stores 64x64 CI4 = 2 KB -> fits_hw -> the
        //     ONE-QUAD hardware S+T wrap path (1 LOAD_TILE + 1 tri-pair). Non-pow2
        //     or <=64-tall heights keep native T (the band walk handles them) so
        //     the hardware T-mask = log2(store_h) stays exact -- this is the
        //     consistency the prior blanket T-path also held; what got that path
        //     reverted was halving DETAILED wides indiscriminately, which the
        //     protect boundary + error gate now prevent.
        // store_h tracks the T downsample so blkh / T-mask / load-extent / T-scale
        // all reference the SAME (downsampled) height downstream -- no desync.
        // 3-WAY VERDICT: native (0), 2x half-downsample (1), one-quad collapse (2).
        // The 2x tier is a named >=256-wide carved out of native (see
        // DL_TexDownsampleTier / dl_downsample2x_names): it halves BOTH axes ONCE
        // (256->128 width, pow2 128->64 height) and stays >2 KB so it BAND-SPLITS,
        // NOT one-quad. The one-quad tier is the unchanged sub-256 collapse-to-64.
        int tier = DL_TexDownsampleTier(texnum, tw, th);
        if (tier == DL_TIER_ONEQUAD)
        {
            // --- S axis: halve width toward 64 (one step; cap at 64) ----------
            store_w  = tw >> 1;
            if (store_w < 64)               // never below 64 (one-quad target)
                store_w  = 64;
            {
                int s = 0, w2 = tw / store_w;
                while (w2 > 1) { w2 >>= 1; s++; }
                ds_shift = s;               // 1 for 128->64 (and 256->128)
            }

            // --- T axis: halve height 128->64 for pow2-tall wides only --------
            // Gate on a power-of-two height > 64 so the hardware T-mask
            // (log2(store_h)) wraps the downsampled block exactly; the common
            // wall height is 128 -> store_h 64. Heights <=64 (already one-quad-
            // tall) or non-pow2 (56/72/...) keep ts_shift 0 and stay native T.
            if (th > 64 && (th & (th - 1)) == 0)
            {
                store_h  = th >> 1;         // 128 -> 64 (one step)
                ts_shift = 1;
            }
        }
        else if (tier == DL_TIER_HALF)
        {
            // --- 2x TIER: one halving per axis, NOT capped at 64 --------------
            // S: 256 -> 128 (one step). ds_shift=1. Width stays 128 (above the
            // 64 one-quad target) so the 128x64 CI4 tile is 4 KB > DL_TMEM_HALF
            // and the fits_hw test below comes out 0 -> band-split (2 S-bands).
            store_w  = tw >> 1;             // 256 -> 128
            ds_shift = 1;

            // T: halve a pow2 height 128 -> 64 once (same exactness gate as the
            // one-quad path: hardware T-mask = log2(store_h) must wrap exactly).
            // Non-pow2 heights (COMPUTE2 56, etc.) keep native T -- they still
            // drop out of native via the S halving alone (the load-volume lever).
            if (th > 64 && (th & (th - 1)) == 0)
            {
                store_h  = th >> 1;         // 128 -> 64 (one step)
                ts_shift = 1;
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
           DL_TexIsProtectedWide(texnum, tw));
#endif
    if (out_h) *out_h = slot->height;   // STORED height (64 for T-downsampled)
    if (out_w) *out_w = slot->width;    // STORED width (64 for S-downsampled): the
                                        // pitch/cap/mask downstream MUST follow the
                                        // stored block, not the original period.
    return block;
}

// Pre-build the CI4 row-major block + sub-palette for ONE wall texture at LEVEL
// LOAD (from R_PrecacheLevel), so the per-frame render never pays the median-cut
// quantisation. Lazily quantising MANY new textures in a single frame is the
// dlbuild burst (measured 50-97k us) on area-transition frames; doing it up-front
// folds it into the loading pause instead. No-op unless the RDP wall path is the
// active renderer. Idempotent: DL_RowMajorBlock self-guards on slot->raw/block,
// and DL_InitCaches guards on dl_rowmajor_inited.
void DL_PrequantTexture(int texnum)
{
    if (!n64_use_rdp_renderer || !n64_rdp_wall_ab)
        return;
    if (texnum < 0 || texnum >= numtextures)
        return;
    DL_InitCaches();                        // ensure dl_rowmajor exists pre-frame
    (void)DL_RowMajorBlock(texnum, NULL, NULL);
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
// The source flat lump is 64x64 CI8 (4096 B). A 64-wide CI8 tile fits only
// DL_TMEM_HALF/64 = 32 rows in the lower TMEM half (the master TLUT owns the
// upper half), so the full 64-row flat does NOT load at once -- the old 32-row
// V-window left the unloaded half sampling garbage on tall floor polys. We now
// V-DECIMATE the flat to 64x32 (out[k] = in[2k]) so the WHOLE flat is 2 KB and
// fully TMEM-resident; the floor draw halves its per-vertex V (period 32 == the
// flat's 64-world period) so the decimated tile maps back exactly. Decimation
// (not box-filter) because CI8 is palette indices -- averaging indices is wrong.
#define DL_FLAT_SRC_W   64      // source lump width (CI8 texels per row)
#define DL_FLAT_SRC_H   64      // source lump height (rows)
#define DL_FLAT_SRC_BYTES (DL_FLAT_SRC_W * DL_FLAT_SRC_H)   // 4096
#define DL_FLAT_W       64      // stored block width  (unchanged)
#define DL_FLAT_H       32      // stored block height (V-decimated 64->32)
#define DL_FLAT_BYTES   (DL_FLAT_W * DL_FLAT_H)             // 2048 -> fits TMEM

typedef struct
{
    void*    raw;       // Z_Malloc'd allocation (back-referenced by zone)
    byte*    block;     // 8-byte-aligned 64x32 row-major CI8 view into raw
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
// V-DECIMATES the 64x64 source lump to a 64x32 block once (out row k <- src row
// 2k), caches PU_STATIC, returns the 8-byte-aligned block (always 64x32 = 2 KB,
// fully TMEM-resident). NULL on a bad index or alloc failure.
static byte* DL_FlatBlock(int flatidx)
{
    dl_flat_t*  slot;
    byte*       block;
    const byte* src;
    int         k;

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

    // V-decimate the 64x64 source lump (row-major CI8) into the 64x32 block:
    // out row k <- src row 2k (DL_FLAT_SRC_W bytes each). Index DECIMATION, not
    // box-filter -- CI8 bytes are palette indices, so averaging them is wrong.
    // Cache the source PU_CACHE during the copy; the block stays PU_STATIC
    // (slot->raw) so the copy source can be released immediately.
    src = (const byte*)W_CacheLumpNum(firstflat + flatidx, PU_CACHE);
    if (!src)
    {
        Z_Free(slot->raw);
        slot->raw = NULL;
        return NULL;
    }
    for (k = 0; k < DL_FLAT_H; k++)
        memcpy(block + k * DL_FLAT_W,
               src   + (2 * k) * DL_FLAT_SRC_W,
               DL_FLAT_W);
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

// --- PRIM/SHADE damage-flash: SHADE stays UN-flashed (plane palette-flash fix) -
// dl_prim_lut is the per-level depth-light RGB the RDP PLANE pass feeds as SHADE
// (DL_PlaneCornerShade), combined as TEX0*SHADE. TEX0 is the flat CI8 texel
// sampled through the RESIDENT master TLUT, and that TLUT is the FLASHED palette:
// I_FinishUpdate uploads doom_tlut_master right before DL_Flush (i_video_n64.c),
// and I_SetPalette writes the damage/pickup/radsuit colours into it. So under a
// flash TEX0 *already* carries the wash -- the flash reaches the floor through
// the texture side, exactly once.
//
// The earlier DL_RetintPrimLUT (26886ce) ALSO red-shifted SHADE from that same
// flashed master TLUT, so the combiner produced red(TEX0) * red(SHADE) -- the
// flash applied TWICE. Two sub-1.0 multiplies darken the result and, because a
// multiply preserves the texel's channel ratios instead of remapping the index
// like software's single palette swap, the flat's base green/brown hue bleeds
// through (Ryan's "RDP floor keeps its texture hue where software is a fuller,
// uniform red", frame 2048). Software's flashed floor is ONE remap:
// master_tlut[ colormap[level][texel] ] -- the wash applied a single time to the
// already-light-darkened index.
//
// FIX: SHADE must carry only the UN-flashed per-level darkening ramp (the base
// PLAYPAL bake from DL_BuildPrimLUT), so the single flash lives entirely in TEX0.
// That reproduces the validated off-flash structure (TEX0=base texel,
// SHADE=base darkening) with the flash riding the texture, matching software's
// single application. No per-flash work at all -- the base bake already holds the
// right SHADE for every palette -- so this is strictly perf-neutral (no LUT
// re-pack on flash transitions) and the gouraud de-banding (b4ac15b) and the
// non-flash case are untouched (dl_prim_lut never changes after the startup bake).
// The CI4 walls (DL_RetintSlot) still re-tint because their sub-palettes are NOT
// the master TLUT; the planes don't need to because their texture IS the master.
// (I_N64PaletteGen / I_N64MasterTLUT are still declared once near the top of the
// file for the wall re-tint path; this routine no longer needs them.)
static void DL_RetintPrimLUT(void)
{
    // Intentional no-op: see the block comment above. SHADE stays the un-flashed
    // base darkening ramp baked once in DL_BuildPrimLUT; the flash is carried by
    // TEX0 (the flat sampled through the flashed resident master TLUT) exactly
    // once, matching software's single palette remap. Kept as a named call site so
    // the plane-flash reasoning is documented where DL_FlushPlanePolys invokes it.
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

    // Stage-4b plane polygon arena reset (shares the per-flat buckets +
    // dl_flat_touched[] with the span path; only one of the two runs per frame).
    dl_ppoly_count = 0;
    dl_ppoly_overflow = 0;

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

// --- Stage-4b plane polygon emit -------------------------------------------
// Append ONE trapezoid run (rdp_ppoly_t) to the poly arena and chain it into the
// flat's bucket (the SAME per-flat bucket arrays the span path uses; a frame runs
// only one of the two paths). DL_FlushPlanePolys then uploads each flat once and
// draws all its runs. Caller (r_plane.c's island walk) has filled in geometry +
// per-corner U/V/INV_W; the colormap is reduced to a PRIM level here so the emit
// site stays a pure data feed. Overflow drops the run (stays key-index).
void DL_EmitPlanePoly(const rdp_ppoly_t* p, const void* colormap)
{
    int          idx;
    int          flatidx;
    rdp_ppoly_t* rec;

    if (!p)
        return;
    if (p->x2 < p->x1)
        return;
    if (dl_ppoly_count >= DL_PPOLY_ARENA)
    {
        dl_ppoly_overflow = 1;
        return;             // arena full: drop (run stays key-index)
    }

    idx = dl_ppoly_count++;
    rec = &dl_ppolys[idx];
    *rec = *p;
    rec->light       = DL_PlaneLightLevel(colormap);
    rec->bucket_next = -1;

    // Chain into the flat's bucket (indexed by flat index = lump-firstflat).
    // Order within a flat's bucket is irrelevant: a plane's runs tile disjoint
    // screen regions (each island/run owns its own columns+rows), so they never
    // overlap -- cheapest head-insert.
    flatidx = (int)p->flatlump - firstflat;
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
            rec->bucket_next = dl_flat_bucket_head[flatidx];
            dl_flat_bucket_head[flatidx] = idx;
        }
    }
}

int DL_PolyCount(void)
{
    return dl_ppoly_count;
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

// =====================================================================
//  GPU PORT -- static world-mesh wall draw (Phase 1b). See Docs/GPU_PORT_PLAN.md.
// =====================================================================
// Transform each baked world-space wall quad to a screen-space rdp_wall_t and hand
// it to the existing arena (DL_EmitWallTier) so DL_Flush draws it through the proven
// CI4 path. View-space divide projection -- DOOM's projection computed 4-corners-
// per-quad instead of per-column (that is where the seg_rast CPU cost goes). The
// invw / screen-Y conventions MATCH DL_EmitRunPiece: invw = 65536/depth (dl_invw_k =
// 1/projection), screen_y = centery - (z-viewz)*centerx/depth.
// Phase 1b scope: single-sided walls only, near-plane SKIP (no clip yet), NO cull.
int n64_rdp_mesh = 0;       // BENCH_FORCE_MESH gate (set in d_main.c)
int n64_rdp_mesh_floors = 0;// BENCH_FORCE_MESH_FLOORS gate -- Phase 3 floor leaves; OFF by
                            // default: measured a PERF LOSS (mesh floors slower than the
                            // already-coalesced visplane path). Separate flag so the
                            // default mesh build keeps the validated wall-only perf.
int dl_wall_z       = 0;    // mesh wall pass: draw with the Z-buffer (set in DL_Flush)
int dl_zbuf_attached = 0;   // set by i_video each frame: 1 iff a z-image is attached

// RSP wall-transform port (BENCH_FORCE_MESH_RSP gate; set in d_main.c). Phase 0 is a
// DMA-loopback PROBE only -- it does NOT change the DL_MeshDrawWalls render path. It
// de-risks overlay registration + 8-byte DMA alignment + cache coherency before any
// transform math is written. See Docs/RSP_PORT_PLAN.md.
//
// EVERYTHING RSP-specific below is gated on BENCH_FORCE_MESH_RSP so the default build
// neither links the rsp_dlwall overlay nor references its assembler symbols -- the
// default ROM stays byte-identical. The flag (Makefile) also adds the overlay .o to
// the ELF prereqs only when set.
// The whole block is compiled out without the flag: no global storage, no overlay
// reference -> the default ROM is byte-identical (verified by an A/B md5 of the
// no-flag build). Nothing references n64_rdp_mesh_rsp without the flag, so the
// header's extern is just an unused declaration there (no link symbol needed).
#ifdef BENCH_FORCE_MESH_RSP
int n64_rdp_mesh_rsp = 0;

// The Phase 0 loopback overlay (rsp/rsp_dlwall.S). DEFINE_RSP_UCODE makes the
// assembler-emitted rsp_dlwall_{text,data,meta}_* symbols available as an
// rsp_ucode_t named rsp_dlwall, ready for rspq_overlay_register.
DEFINE_RSP_UCODE(rsp_dlwall);

// Overlay ID assigned by rspq_overlay_register (preshifted by 28; 0 = unregistered).
static uint32_t rsp_dlwall_ovl_id = 0;

// rspq command indices (must match the RSPQ_DefineCommand order in rsp/rsp_dlwall.S).
#define DLWALL_CMD_LOOPBACK 0
#define DLWALL_CMD_XFORM    1
#define DLWALL_CMD_BATCH    2

// ---- Phase 1 transform DMA blocks. Layouts MUST match rsp_dlwall.S byte-for-byte
// (VIEWBLK / WALLIN / WALLOUT). All 32-bit signed words; no FPU on the RSP, so the
// output is fixed-point and the CPU converts on read.
typedef struct {
    int32_t viewx, viewy, viewz;   // 0x00,0x04,0x08  fixed_t
    int32_t vcos, vsin;            // 0x0C,0x10        fixed_t finecosine/finesine
    int32_t centerx, centery;      // 0x14,0x18        plain int
    int32_t pad0;                  // 0x1C
} rsp_view_blk_t;                  // 32 bytes

typedef struct {
    int32_t x1, y1, x2, y2;        // 0x00..0x0C  fixed_t wall corners
    int32_t ztop, zbot;            // 0x10,0x14   fixed_t snapshotted live heights
    int32_t pad0, pad1;            // 0x18,0x1C
} rsp_wall_in_t;                   // 32 bytes

typedef struct {
    int32_t dA, dB;                // 0x00,0x04  depth 16.16
    int32_t lA, lB;                // 0x08,0x0C  lateral 16.16
    int32_t invwA, invwB;          // 0x10,0x14  RAW 32-bit vrcp reciprocal of depth
    int32_t sxA, sxB;              // 0x18,0x1C  screen X 16.16
    int32_t ytA, ybA, ytB, ybB;    // 0x20..0x2C screen Y top/bot per corner 16.16
} rsp_wall_out_t;                  // 48 bytes

// ---- Phase 2 BATCH DMA blocks. Layouts MUST match rsp_dlwall.S BWALL_IN/BWALL_OUT.
// The CPU packs the WHOLE visible wall set; the RSP transforms it in DMEM-sized
// chunks. invw here is FixedDiv(65536,depth) in 16.16 (== invw_real * 65536), the
// same scale Phase 1's invwA/invwB use after FixedDivApply.
typedef struct {
    int32_t x1, y1, x2, y2;        // 0x00..0x0C  fixed_t wall corners
    int32_t ztop, zbot;            // 0x10,0x14   fixed_t snapshotted live heights
    int32_t vis;                   // 0x18        1 = bake_linevis[line] && ztop>zbot
    int32_t pad0;                  // 0x1C
} rsp_bwall_in_t;                  // 32 bytes

typedef struct {
    int32_t dA, dB;                // 0x00,0x04   depth 16.16 (post near-clip)
    int32_t lA, lB;                // 0x08,0x0C   lateral 16.16
    int32_t invwA, invwB;          // 0x10,0x14   FixedDiv(65536,depth) 16.16
    int32_t sxA, sxB;              // 0x18,0x1C   screen X 16.16
    int32_t ytA, ybA, ytB, ybB;    // 0x20..0x2C  screen Y top/bot per corner 16.16
    int32_t emit;                  // 0x30        1 = survived all skips, else 0
    int32_t pad0, pad1, pad2;      // 0x34,0x38,0x3C
} rsp_bwall_out_t;                 // 64 bytes
#endif

// Map a record's per-edge INV_W (== 1/depth in map units, see dl_invw_k) to a
// z-buffer depth in [0,1] (near = small). Z = depth / 32768 (the map extent),
// clamped. Used only on the mesh wall pass (dl_wall_z).
static inline float DL_WallZ(float invw)
{
    float z = (invw > 0.0f) ? (1.0f / (invw * 32768.0f)) : 1.0f;
    return z < 0.0f ? 0.0f : (z > 1.0f ? 1.0f : z);
}

int DL_MeshRouteOn(void)
{
    return n64_use_rdp_renderer && n64_rdp_mesh
        && bake_walls != NULL && bake_numwalls > 0;
}

// =====================================================================
//  RSP PORT -- PHASE 0: DMA loopback probe. See Docs/RSP_PORT_PLAN.md Sec 4.
// =====================================================================
// Proves the rsp_dlwall overlay registers and round-trips bake_wall_t records
// RDRAM->DMEM->RDRAM byte-identically. Runs ONCE (first DL_MeshDrawWalls call
// under n64_rdp_mesh_rsp). Does NOT touch the render path -- pure log probe.
//
// Coherency mirrors the production plan: the source buffer is cache-flushed
// (data_cache_hit_writeback) before the RSP reads it, and the destination is
// invalidated (data_cache_hit_invalidate) after rspq_wait before the CPU reads
// it back. Buffers are cache-line (16B) aligned so the writeback/invalidate hit
// whole lines and the DMA's 8-byte alignment requirement is met.
#ifdef BENCH_FORCE_MESH_RSP
static void DL_RSPLoopbackProbe(void)
{
    static int registered = 0;
    static int probed = 0;
    uint32_t   ovl_id;
    int        n, bytes;
    void      *src, *dst;

    if (probed)
        return;
    probed = 1;

    if (!bake_walls || bake_numwalls <= 0)
    {
        debugf("RSP-LOOPBACK: SKIP (no baked walls)\n");
        return;
    }

    // Register the overlay exactly once.
    if (!registered)
    {
        ovl_id = rspq_overlay_register(&rsp_dlwall);
        rsp_dlwall_ovl_id = ovl_id;
        registered = 1;
        debugf("RSP-LOOPBACK: overlay registered, id=0x%08lx\n",
               (unsigned long)ovl_id);
    }

    // Loopback the first N records. Cap the byte count so it fits the RSP's 1 KiB
    // DMEM scratch (LOOP_SCRATCH in rsp_dlwall.S) AND the 12-bit DMA width, and keep
    // it 8-byte aligned. 32 * 28B = 896B < 1024.
    n = bake_numwalls;
    if (n > 32) n = 32;
    bytes = n * (int)sizeof(bake_wall_t);
    bytes = (bytes + 7) & ~7;                 // round up to 8-byte DMA granule

    src = memalign(16, bytes);
    dst = memalign(16, bytes);
    if (!src || !dst)
    {
        if (src) free(src);
        if (dst) free(dst);
        debugf("RSP-LOOPBACK: SKIP (alloc failed)\n");
        return;
    }

    memcpy(src, bake_walls, n * sizeof(bake_wall_t));
    memset(dst, 0xA5, bytes);                 // poison so a no-op DMA can't pass

    // Flush src so the RSP DMA reads CPU-written bytes; flush dst's poison too so a
    // stale cached line can't masquerade as the round-tripped result on read-back.
    data_cache_hit_writeback(src, bytes);
    data_cache_hit_writeback(dst, bytes);

    // Issue the loopback: a0 low-24 = (bytes-1) DMA size word (height==1),
    // a1 = RDRAM src, a2 = RDRAM dst. Then block until the RSP finishes.
    rspq_write(rsp_dlwall_ovl_id, DLWALL_CMD_LOOPBACK,
               (uint32_t)(bytes - 1),
               PhysicalAddr(src),
               PhysicalAddr(dst));
    rspq_wait();

    // Invalidate dst so the CPU re-reads the RSP-written bytes, not stale cache.
    data_cache_hit_invalidate(dst, bytes);

    int diff = memcmp(src, dst, n * sizeof(bake_wall_t));
    debugf("RSP-LOOPBACK: N=%d bytes=%d memcmp=%d\n", n, bytes, diff ? diff : 0);

    free(src);
    free(dst);
}

// =====================================================================
//  RSP PORT -- PHASE 1: ONE-WALL TRANSFORM, bit-exact compare gate.
// =====================================================================
// The load-bearing precision gate (Docs/RSP_PORT_PLAN.md Sec 4/5). Picks the first
// baked wall that passes the near gate with BOTH corners past the near plane and on
// screen (so the float screen-edge-clip lerps DON'T run -- those are linear-in-float
// and not a hardware precision risk; isolating the two real risks, the FixedMul and
// the 65536/depth divide). Packs a fixed-point view block + the wall, runs the RSP
// transform overlay, reads back the fixed-point rsp_wall_out_t, recomputes every
// field CPU-side in IEEE32 float exactly as DL_MeshDrawWalls does, and logs the max
// per-field delta + a MATCH/DRIFT verdict.
//
// invw scale: the RSP returns the RAW 32-bit vrcp reciprocal of depth_fixed. vrcp
// produces ~ 2^31/X (with a documented 1-LSB right shift -> effectively 2^31 numerator
// for our normalized 16.16 inputs). The CPU bridges to the real 65536/depth by the
// known scale factor RSP_INVW_SCALE and reports the RELATIVE error -- a wrong power of
// two would show as a clean 2x in the log (self-diagnosing), the vrcp epsilon shows as
// a small relative delta (the divide-risk datum).
static void DL_RSPXformProbe(void)
{
    static int probed = 0;
    int i, pick = -1;
    fixed_t vcos, vsin;
    const fixed_t nearz = 4 << FRACBITS;

    rsp_view_blk_t *vb;
    rsp_wall_in_t  *wi;
    rsp_wall_out_t *wo;

    if (probed) return;

    if (!bake_walls || bake_numwalls <= 0) {
        debugf("RSP-XFORM: SKIP (no baked walls)\n");
        probed = 1;
        return;
    }
    if (rsp_dlwall_ovl_id == 0) {            // share the loopback probe's registration
        rsp_dlwall_ovl_id = rspq_overlay_register(&rsp_dlwall);
        debugf("RSP-XFORM: overlay registered, id=0x%08lx\n",
               (unsigned long)rsp_dlwall_ovl_id);
    }

    vcos = finecosine[viewangle >> ANGLETOFINESHIFT];
    vsin = finesine[viewangle >> ANGLETOFINESHIFT];

    // ---- choose an unclipped, on-screen, front-facing wall ----
    for (i = 0; i < bake_numwalls; i++) {
        const bake_wall_t* bw = &bake_walls[i];
        fixed_t txa = bw->x1 - viewx, tya = bw->y1 - viewy;
        fixed_t txb = bw->x2 - viewx, tyb = bw->y2 - viewy;
        fixed_t dA  = FixedMul(txa, vcos) + FixedMul(tya, vsin);
        fixed_t dB  = FixedMul(txb, vcos) + FixedMul(tyb, vsin);
        fixed_t lA, lB, ztopz, zbotz;
        float invwA, invwB, scA, scB, sxA, sxB;

        if (dA < nearz || dB < nearz) continue;     // BOTH past near -> no clip
        ztopz = bw->ztop_ceil ? sectors[bw->ztop_sec].ceilingheight
                              : sectors[bw->ztop_sec].floorheight;
        zbotz = bw->zbot_ceil ? sectors[bw->zbot_sec].ceilingheight
                              : sectors[bw->zbot_sec].floorheight;
        if (ztopz <= zbotz) continue;

        lA = FixedMul(tya, vcos) - FixedMul(txa, vsin);
        lB = FixedMul(tyb, vcos) - FixedMul(txb, vsin);
        invwA = 65536.0f / (float)dA;
        invwB = 65536.0f / (float)dB;
        scA = (float)centerx * invwA;
        scB = (float)centerx * invwB;
        sxA = (float)centerx - (float)lA * (1.0f/65536.0f) * scA;
        sxB = (float)centerx - (float)lB * (1.0f/65536.0f) * scB;
        if (sxB <= sxA) continue;
        if (sxA < 0.5f || sxB > (float)(SCREENWIDTH - 1) - 0.5f) continue; // fully on, no edge clip
        pick = i;
        break;
    }
    if (pick < 0) {
        debugf("RSP-XFORM: no unclipped on-screen wall this frame (retry)\n");
        return;     // try again next frame -- don't set probed; need a clean wall
    }
    probed = 1;

    {
        const bake_wall_t* bw = &bake_walls[pick];
        fixed_t ztopz = bw->ztop_ceil ? sectors[bw->ztop_sec].ceilingheight
                                      : sectors[bw->ztop_sec].floorheight;
        fixed_t zbotz = bw->zbot_ceil ? sectors[bw->zbot_sec].ceilingheight
                                      : sectors[bw->zbot_sec].floorheight;

        // ---- CPU reference (IEEE32 float, mirrors DL_MeshDrawWalls) ----
        fixed_t txa = bw->x1 - viewx, tya = bw->y1 - viewy;
        fixed_t txb = bw->x2 - viewx, tyb = bw->y2 - viewy;
        fixed_t dA  = FixedMul(txa, vcos) + FixedMul(tya, vsin);
        fixed_t dB  = FixedMul(txb, vcos) + FixedMul(tyb, vsin);
        fixed_t lA  = FixedMul(tya, vcos) - FixedMul(txa, vsin);
        fixed_t lB  = FixedMul(tyb, vcos) - FixedMul(txb, vsin);
        float viewzf = (float)viewz * (1.0f/65536.0f);
        float invwA  = 65536.0f / (float)dA;
        float invwB  = 65536.0f / (float)dB;
        float scA    = (float)centerx * invwA;
        float scB    = (float)centerx * invwB;
        float sxA    = (float)centerx - (float)lA * (1.0f/65536.0f) * scA;
        float sxB    = (float)centerx - (float)lB * (1.0f/65536.0f) * scB;
        float topf   = (float)ztopz * (1.0f/65536.0f) - viewzf;
        float botf   = (float)zbotz * (1.0f/65536.0f) - viewzf;
        float ytA = (float)centery - topf*scA, ybA = (float)centery - botf*scA;
        float ytB = (float)centery - topf*scB, ybB = (float)centery - botf*scB;

        // ---- pack DMA blocks (cache-line aligned) ----
        vb = memalign(16, sizeof *vb);
        wi = memalign(16, sizeof *wi);
        wo = memalign(16, sizeof *wo);
        if (!vb || !wi || !wo) {
            if (vb) free(vb); if (wi) free(wi); if (wo) free(wo);
            debugf("RSP-XFORM: SKIP (alloc failed)\n");
            return;
        }
        vb->viewx = viewx; vb->viewy = viewy; vb->viewz = viewz;
        vb->vcos = vcos;   vb->vsin = vsin;
        vb->centerx = centerx; vb->centery = centery; vb->pad0 = 0;
        wi->x1 = bw->x1; wi->y1 = bw->y1; wi->x2 = bw->x2; wi->y2 = bw->y2;
        wi->ztop = ztopz; wi->zbot = zbotz; wi->pad0 = wi->pad1 = 0;
        memset(wo, 0xA5, sizeof *wo);

        data_cache_hit_writeback(vb, sizeof *vb);
        data_cache_hit_writeback(wi, sizeof *wi);
        data_cache_hit_writeback(wo, sizeof *wo);  // flush poison so a no-write can't pass

        rspq_write(rsp_dlwall_ovl_id, DLWALL_CMD_XFORM,
                   PhysicalAddr(vb), PhysicalAddr(wi), PhysicalAddr(wo));
        rspq_wait();
        data_cache_hit_invalidate(wo, sizeof *wo);

        // ---- convert RSP fixed-point output to float ----
        float r_dA = (float)wo->dA * (1.0f/65536.0f);
        float r_dB = (float)wo->dB * (1.0f/65536.0f);
        float r_lA = (float)wo->lA * (1.0f/65536.0f);
        float r_lB = (float)wo->lB * (1.0f/65536.0f);
        // RSP invw = FixedDiv(65536, depth) = 2^32/depth_fixed in 16.16 == invw_real
        // in 16.16. Convert by /65536. (The FIXEDDIV_SH constant in the ucode lands the
        // 16.16 scale; the log's relative error reveals a wrong power-of-two as a clean
        // 2^n, vs the genuine vrcp epsilon.)
        double r_invwA = (double)(int32_t)wo->invwA / 65536.0;
        double r_invwB = (double)(int32_t)wo->invwB / 65536.0;
        float r_sxA = (float)wo->sxA * (1.0f/65536.0f);
        float r_sxB = (float)wo->sxB * (1.0f/65536.0f);
        float r_ytA = (float)wo->ytA * (1.0f/65536.0f);
        float r_ybA = (float)wo->ybA * (1.0f/65536.0f);
        float r_ytB = (float)wo->ytB * (1.0f/65536.0f);
        float r_ybB = (float)wo->ybB * (1.0f/65536.0f);

        // ---- max per-field deltas ----
        float d_d  = fmaxf(fabsf(r_dA - (float)dA*(1.0f/65536.0f)),
                           fabsf(r_dB - (float)dB*(1.0f/65536.0f)));
        float d_l  = fmaxf(fabsf(r_lA - (float)lA*(1.0f/65536.0f)),
                           fabsf(r_lB - (float)lB*(1.0f/65536.0f)));
        double e_iA = fabs((r_invwA - (double)invwA) / (double)invwA);
        double e_iB = fabs((r_invwB - (double)invwB) / (double)invwB);
        double d_invw = (e_iA > e_iB) ? e_iA : e_iB;
        float d_sx = fmaxf(fabsf(r_sxA - sxA), fabsf(r_sxB - sxB));
        float d_sy = fmaxf(fmaxf(fabsf(r_ytA - ytA), fabsf(r_ybA - ybA)),
                           fmaxf(fabsf(r_ytB - ytB), fabsf(r_ybB - ybB)));

        // screen coords within 0.5px, invw within a small relative epsilon
        int match = (d_sx <= 0.5f) && (d_sy <= 0.5f) && (d_invw <= 0.01);

        debugf("RSP-XFORM wall=%d pick_depthA=%d depthB=%d\n", pick, dA, dB);
        debugf("RSP-XFORM cpu  sxA=%d.%03d sxB=%d.%03d ytA=%d ybA=%d invwA=%d(e-6)\n",
               (int)sxA, (int)((sxA-(int)sxA)*1000), (int)sxB, (int)((sxB-(int)sxB)*1000),
               (int)ytA, (int)ybA, (int)(invwA*1e6f));
        debugf("RSP-XFORM rsp  sxA=%d.%03d sxB=%d.%03d ytA=%d ybA=%d invwA=%d(e-6)\n",
               (int)r_sxA, (int)((r_sxA-(int)r_sxA)*1000), (int)r_sxB,
               (int)((r_sxB-(int)r_sxB)*1000), (int)r_ytA, (int)r_ybA, (int)(r_invwA*1e6));
        debugf("RSP-XFORM wall=%d dmax_d=%d(e-3) dmax_l=%d(e-3) dmax_sx=%d(e-3) "
               "dmax_sy=%d(e-3) dmax_invw=%d(e-6) verdict=%s\n",
               pick, (int)(d_d*1000), (int)(d_l*1000), (int)(d_sx*1000),
               (int)(d_sy*1000), (int)(d_invw*1e6), match ? "MATCH" : "DRIFT");

        free(vb); free(wi); free(wo);
    }
}

// =====================================================================
//  RSP PORT -- PHASE 2: FULL-BATCH transform, CPU still authoritative.
// =====================================================================
// Transforms the ENTIRE visible wall set on the RSP each frame and compares the
// whole batch against the CPU's own DL_MeshDrawWalls computation. The render still
// uses the CPU records -- this phase only proves correctness AT SCALE before the
// Phase-3 cutover. See Docs/RSP_PORT_PLAN.md Sec 4.
//
// EMIT-DECISION ALIGNMENT (the load-bearing structural correctness): the RSP and
// the CPU must agree, slot-for-slot, on WHICH walls emit. We keep them aligned by
// processing one input slot per baked wall (no compaction): the CPU folds the two
// deterministic, non-HW gates (bake_linevis[line] and ztop>zbot) into a per-wall
// `vis` byte; the RSP then reproduces the HW-sensitive skips itself -- both-corners-
// behind-near, the near-plane corner slide, back-face (sxB<=sxA), and fully-off-
// screen -- and writes an `emit` flag per slot. The CPU reference below reproduces
// the SAME gates in float and compares the emit flags first (a structural mismatch),
// then the geometry fields only for walls both sides agree are emitted.
//
// SCOPE BOUNDARY: this matches DL_MeshDrawWalls up to and including the back-face +
// off-screen emit tests (the `continue`s BEFORE the screen-edge-clip block). The
// final per-attribute screen-edge clip + sub-pixel xa/xb rounding is float-linear
// render-stage refinement (Phase 1 classified the edge-clip lerps as non-HW-risk);
// it does not change the emit set here and is reproduced on the CPU at render time,
// not in this transform gate.
//
// Buffers are level-load-sized once (bake_numwalls is fixed per level) and reused.
// (Still inside the BENCH_FORCE_MESH_RSP block opened above for Phase 0/1.)
// DMA buffers in BSS, NOT the heap: the old memalign cost ~48KB of libdragon's tight
// heap (DOOM's zone is malloc'd from it and takes most of RAM), which starved the audio
// mixer -> mixer_ch_play "Out of memory" assert when a sound's sample buffer couldn't
// allocate late in the demo (the wall-only build, with no RSP buffers, never crashed).
// BSS is part of the program image, off the heap. Sized to the wall arena.
static rsp_bwall_in_t  batch_in_buf[DL_WALL_ARENA]  __attribute__((aligned(16)));
static rsp_bwall_out_t batch_out_buf[DL_WALL_ARENA] __attribute__((aligned(16)));
static rsp_bwall_in_t  *batch_in  = batch_in_buf;
static rsp_bwall_out_t *batch_out = batch_out_buf;

// Worst deltas observed across the whole run (for the end-of-demo report).
static float  batch_run_worst_sx   = 0.0f;
static float  batch_run_worst_sy   = 0.0f;
static double batch_run_worst_invw = 0.0;
static long   batch_run_total_mism = 0;
static unsigned batch_frame = 0;

static void DL_RSPBatchProbe(void)
{
    int i;
    fixed_t vcos, vsin;
    const fixed_t nearz = 4 << FRACBITS;
    float viewzf;
    rsp_view_blk_t *vb;
    int emitted = 0, mism = 0;
    float  worst_sx = 0.0f, worst_sy = 0.0f;
    double worst_invw = 0.0;
    int    emit_disagree = 0;
    int    cpu_emitted = 0, rsp_emitted = 0;   // independent CPU/RSP emit counts
    // context of the worst-sx wall this frame (for the residual-class log)
    int    wsx_i = -1, wsx_clip = 0; fixed_t wsx_dA = 0, wsx_dB = 0;
    int    n_clipped = 0, n_clip_mism = 0;
    // The per-wall CPU reference + compare below is DIAGNOSTIC only (batch_out is
    // already filled by the RSP). Off by default = the real offload: pack, dispatch,
    // DMA back, done. Set to 1 (rebuild) to re-confirm RSP-vs-CPU agreement.
    static int dl_rsp_verify = 0;

    if (!bake_walls || bake_numwalls <= 0)
        return;
    if (rsp_dlwall_ovl_id == 0) {
        rsp_dlwall_ovl_id = rspq_overlay_register(&rsp_dlwall);
        debugf("RSP-BATCH: overlay registered, id=0x%08lx\n",
               (unsigned long)rsp_dlwall_ovl_id);
    }

    // batch_in/out are static BSS arrays (off the heap) covering DL_WALL_ARENA walls.
    if (bake_numwalls > DL_WALL_ARENA) {
        debugf("RSP-BATCH: SKIP (walls %d > arena %d)\n", bake_numwalls, DL_WALL_ARENA);
        return;
    }

    vcos   = finecosine[viewangle >> ANGLETOFINESHIFT];
    vsin   = finesine[viewangle >> ANGLETOFINESHIFT];
    viewzf = (float)viewz * (1.0f / 65536.0f);

    // ---- pack the view block (ONE-TIME static alloc) ----
    // Was a per-frame memalign+free -> over the demo it churned/leaked the tight
    // libdragon heap (DOOM's zone takes most of RAM) until a monster's lazily-allocated
    // sound sample-buffer hit malloc_uncached==NULL -> the mixer "Out of memory" assert
    // (~frame 494, where a door wakes a monster). Alloc once, reuse, never free.
    {
        static rsp_view_blk_t* vb_cache = NULL;
        if (!vb_cache) vb_cache = memalign(16, sizeof *vb_cache);
        vb = vb_cache;
    }
    if (!vb) { debugf("RSP-BATCH: SKIP (vb alloc)\n"); return; }
    vb->viewx = viewx; vb->viewy = viewy; vb->viewz = viewz;
    vb->vcos = vcos;   vb->vsin = vsin;
    vb->centerx = centerx; vb->centery = centery; vb->pad0 = 0;

    // ---- pack every wall's input + the folded vis byte ----
    // Most baked walls fail the BSP-occlusion gate each frame (~30 of ~475 visible on
    // E1M1). The RSP checks BWI_vis BEFORE reading the heights (rsp_dlwall.S:584) and
    // zeroes a skipped slot, and DL_MeshDrawWalls re-gates on bake_linevis before
    // reading batch_out -- so a culled wall's geometry/height fields are never read.
    // Skip the two sector-height lookups (the per-wall cost) for culled walls.
    for (i = 0; i < bake_numwalls; i++) {
        const bake_wall_t* bw = &bake_walls[i];
        fixed_t ztopz = 0, zbotz = 0;
        int vis = (bake_linevis && !bake_linevis[bw->line]) ? 0 : 1;
        if (vis) {
            ztopz = bw->ztop_ceil ? sectors[bw->ztop_sec].ceilingheight
                                  : sectors[bw->ztop_sec].floorheight;
            zbotz = bw->zbot_ceil ? sectors[bw->zbot_sec].ceilingheight
                                  : sectors[bw->zbot_sec].floorheight;
            if (ztopz <= zbotz) vis = 0;
        }
        batch_in[i].x1 = bw->x1; batch_in[i].y1 = bw->y1;
        batch_in[i].x2 = bw->x2; batch_in[i].y2 = bw->y2;
        batch_in[i].ztop = ztopz; batch_in[i].zbot = zbotz;
        batch_in[i].vis = vis; batch_in[i].pad0 = 0;
    }

    // ---- coherency: flush inputs + view block, poison + flush outputs ----
    // The 0xA5 poison only exists so the verify-compare can spot cells the RSP failed
    // to write; it's a 30KB/frame memset that's pure waste in the real offload.
    if (dl_rsp_verify)
        memset(batch_out, 0xA5, (size_t)bake_numwalls * sizeof(rsp_bwall_out_t));
    data_cache_hit_writeback(vb, sizeof *vb);
    data_cache_hit_writeback(batch_in,  (uint32_t)((size_t)bake_numwalls * sizeof(rsp_bwall_in_t)));
    data_cache_hit_writeback(batch_out, (uint32_t)((size_t)bake_numwalls * sizeof(rsp_bwall_out_t)));

    rspq_write(rsp_dlwall_ovl_id, DLWALL_CMD_BATCH,
               PhysicalAddr(vb), PhysicalAddr(batch_in),
               PhysicalAddr(batch_out), (uint32_t)bake_numwalls);
    rspq_wait();
    data_cache_hit_invalidate(batch_out, (uint32_t)((size_t)bake_numwalls * sizeof(rsp_bwall_out_t)));

    // Real offload: batch_out is ready -- skip the CPU reference recompute below
    // (the per-frame double-work that masked the dlbuild win). Diagnostics only.
    if (!dl_rsp_verify) { batch_frame++; return; }

    // ---- per-wall CPU reference + compare ----
    for (i = 0; i < bake_numwalls; i++) {
        const bake_wall_t* bw = &bake_walls[i];
        const rsp_bwall_out_t* ro = &batch_out[i];
        fixed_t txa = bw->x1 - viewx, tya = bw->y1 - viewy;
        fixed_t txb = bw->x2 - viewx, tyb = bw->y2 - viewy;
        fixed_t dA  = FixedMul(txa, vcos) + FixedMul(tya, vsin);
        fixed_t dB  = FixedMul(txb, vcos) + FixedMul(tyb, vsin);
        fixed_t ztopz = batch_in[i].ztop, zbotz = batch_in[i].zbot;
        fixed_t dA_raw = dA, dB_raw = dB;
        int cpu_emit = 1, clipped = 0;
        float invwA, invwB, scA, scB, sxA, sxB, topf, botf;
        float ytA, ybA, ytB, ybB;

        // ---- CPU emit decision, mirroring DL_MeshDrawWalls up to the off-screen test ----
        if (!batch_in[i].vis) cpu_emit = 0;
        else if (dA < nearz && dB < nearz) cpu_emit = 0;

        if (cpu_emit) {
            // near-plane corner slide (same as DL_MeshDrawWalls)
            if (dA < nearz) {
                float u = (float)(nearz - dA) / (float)(dB - dA);
                txa += (fixed_t)(u * (float)(txb - txa));
                tya += (fixed_t)(u * (float)(tyb - tya));
                dA   = nearz; clipped = 1;
            } else if (dB < nearz) {
                float u = (float)(nearz - dB) / (float)(dA - dB);
                txb += (fixed_t)(u * (float)(txa - txb));
                tyb += (fixed_t)(u * (float)(tya - tyb));
                dB   = nearz; clipped = 1;
            }
            {
                fixed_t lA = FixedMul(tya, vcos) - FixedMul(txa, vsin);
                fixed_t lB = FixedMul(tyb, vcos) - FixedMul(txb, vsin);
                invwA = 65536.0f / (float)dA;
                invwB = 65536.0f / (float)dB;
                scA   = (float)centerx * invwA;
                scB   = (float)centerx * invwB;
                sxA = (float)centerx - (float)lA * (1.0f/65536.0f) * scA;
                sxB = (float)centerx - (float)lB * (1.0f/65536.0f) * scB;
                if (sxB <= sxA) cpu_emit = 0;
                else if (sxB <= 0.0f || sxA >= (float)(SCREENWIDTH - 1)) cpu_emit = 0;
                topf = (float)ztopz * (1.0f/65536.0f) - viewzf;
                botf = (float)zbotz * (1.0f/65536.0f) - viewzf;
                ytA = (float)centery - topf*scA; ybA = (float)centery - botf*scA;
                ytB = (float)centery - topf*scB; ybB = (float)centery - botf*scB;
            }
        } else {
            invwA = invwB = scA = scB = sxA = sxB = 0.0f;
            ytA = ybA = ytB = ybB = 0.0f;
        }

        if (cpu_emit) cpu_emitted++;
        if (ro->emit) rsp_emitted++;

        // ---- structural: emit flags must agree (the load-bearing slot alignment) ----
        if ((int)ro->emit != cpu_emit) {
            emit_disagree++;
            mism++;
            continue;   // geometry meaningless when emit sets disagree
        }
        if (!cpu_emit) continue;   // both agree culled -- nothing to compare
        emitted++;

        // ---- field compare (RSP fixed-point -> float) ----
        {
            float r_sxA = (float)ro->sxA * (1.0f/65536.0f);
            float r_sxB = (float)ro->sxB * (1.0f/65536.0f);
            float r_ytA = (float)ro->ytA * (1.0f/65536.0f);
            float r_ybA = (float)ro->ybA * (1.0f/65536.0f);
            float r_ytB = (float)ro->ytB * (1.0f/65536.0f);
            float r_ybB = (float)ro->ybB * (1.0f/65536.0f);
            double r_invwA = (double)(int32_t)ro->invwA / 65536.0;
            double r_invwB = (double)(int32_t)ro->invwB / 65536.0;
            float d_sx = fmaxf(fabsf(r_sxA - sxA), fabsf(r_sxB - sxB));
            float d_sy = fmaxf(fmaxf(fabsf(r_ytA - ytA), fabsf(r_ybA - ybA)),
                               fmaxf(fabsf(r_ytB - ytB), fabsf(r_ybB - ybB)));
            double e_iA = fabs((r_invwA - (double)invwA) / (double)invwA);
            double e_iB = fabs((r_invwB - (double)invwB) / (double)invwB);
            double d_invw = (e_iA > e_iB) ? e_iA : e_iB;

            if (d_sx   > worst_sx)   { worst_sx = d_sx; wsx_i = i; wsx_clip = clipped;
                                       wsx_dA = dA_raw; wsx_dB = dB_raw; }
            if (d_sy   > worst_sy)   worst_sy   = d_sy;
            if (d_invw > worst_invw) worst_invw = d_invw;

            if (clipped) n_clipped++;

            // mismatch: any field outside the Phase-1 epsilon (sx/sy >0.5px,
            // invw >1% rel). S/T are derived from these (S=si/invw, T spans
            // height) so the sx/sy/invw bounds subsume the >1-texel S/T bound.
            if (d_sx > 0.5f || d_sy > 0.5f || d_invw > 0.01) {
                mism++;
                if (clipped) n_clip_mism++;
            }
        }
    }

    // ---- accumulate run-wide worsts + total ----
    if (worst_sx   > batch_run_worst_sx)   batch_run_worst_sx   = worst_sx;
    if (worst_sy   > batch_run_worst_sy)   batch_run_worst_sy   = worst_sy;
    if (worst_invw > batch_run_worst_invw) batch_run_worst_invw = worst_invw;
    batch_run_total_mism += mism;

    debugf("RSP-BATCH frame=%u walls=%d cpu_emit=%d rsp_emit=%d mismatch=%d emit_disagree=%d "
           "worst_sx=%d(e-3) worst_sy=%d(e-3) worst_invw=%d(e-6) n_clip=%d clip_mism=%d\n",
           batch_frame, emitted, cpu_emitted, rsp_emitted, mism, emit_disagree,
           (int)(worst_sx*1000.0f), (int)(worst_sy*1000.0f),
           (int)(worst_invw*1e6), n_clipped, n_clip_mism);
    // When the worst sx is large, dump the offending wall's depth + clip status so a
    // pathological class (e.g. near-clipped deep wall) is identifiable in the log.
    if (worst_sx > 0.5f && wsx_i >= 0)
        debugf("RSP-BATCH WORST frame=%u i=%d clip=%d dA=%d dB=%d d_sx=%d(e-3)\n",
               batch_frame, wsx_i, wsx_clip, (int)wsx_dA, (int)wsx_dB,
               (int)(worst_sx*1000.0f));

    // Periodic run-wide rollup so a long demo's grand total is visible without
    // summing every per-frame line.
    if ((batch_frame % 256) == 0) {
        debugf("RSP-BATCH-RUN frame=%u total_mismatch=%ld run_worst_sx=%d(e-3) "
               "run_worst_sy=%d(e-3) run_worst_invw=%d(e-6)\n",
               batch_frame, batch_run_total_mism,
               (int)(batch_run_worst_sx*1000.0f), (int)(batch_run_worst_sy*1000.0f),
               (int)(batch_run_worst_invw*1e6));
    }
    batch_frame++;
    /* vb is a one-time static cache (vb_cache) -- never freed, so no per-frame heap churn */
}
#endif // BENCH_FORCE_MESH_RSP

void DL_MeshDrawWalls(void)
{
    int     i, nrec = 0;
    fixed_t vcos, vsin;
    float   viewzf;
    const fixed_t nearz = 4 << FRACBITS;
    // Collected this frame, then depth-sorted for painter's order. Static per-frame
    // scratch (kept off the stack); sized to the wall arena.
    static rdp_wall_t mrec[DL_WALL_ARENA];
    static fixed_t    mdepth[DL_WALL_ARENA];

    if (!DL_MeshRouteOn())
        return;

    // RSP port probes (logs only, no render effect): Phase 0 DMA loopback (once)
    // + Phase 1 one-wall transform compare gate (once) + Phase 2 full-batch
    // transform compare (EVERY frame, CPU still authoritative).
#ifdef BENCH_FORCE_MESH_RSP
    if (n64_rdp_mesh_rsp) {
        DL_RSPLoopbackProbe();
        DL_RSPXformProbe();
        DL_RSPBatchProbe();
    }
#endif

    vcos   = finecosine[viewangle >> ANGLETOFINESHIFT];
    vsin   = finesine[viewangle >> ANGLETOFINESHIFT];
    viewzf = (float)viewz * (1.0f / 65536.0f);

    for (i = 0; i < bake_numwalls; i++)
    {
        const bake_wall_t* bw = &bake_walls[i];
        fixed_t txa = bw->x1 - viewx, tya = bw->y1 - viewy;
        fixed_t txb = bw->x2 - viewx, tyb = bw->y2 - viewy;
        fixed_t dA  = FixedMul(txa, vcos) + FixedMul(tya, vsin);    // depth at v1
        fixed_t dB  = FixedMul(txb, vcos) + FixedMul(tyb, vsin);    // depth at v2
        fixed_t lA, lB;
        float   invwA, invwB, scA, scB, sxA, sxB, topf, botf, dxf, dyf;
        float   ytA, ybA, ytB, ybB;     // screen Y of top/bot edge at each corner
        float   sLen, sA, sB;
        int     xa, xb, lvl;
        fixed_t ztopz, zbotz;
        rdp_wall_t w;

        if (bake_linevis && !bake_linevis[bw->line]) continue;  // BSP-occlusion gate
        if (dA < nearz && dB < nearz) continue;     // both behind near plane

        // LIVE edge heights -- resolve the quad's sector references each frame so moving
        // sectors (doors/lifts/crushers) follow the geometry instead of leaving a ghost.
        // A step whose top has dropped to/below its bottom this frame (door fully open)
        // is not exposed -> skip it.
        ztopz = bw->ztop_ceil ? sectors[bw->ztop_sec].ceilingheight
                              : sectors[bw->ztop_sec].floorheight;
        zbotz = bw->zbot_ceil ? sectors[bw->zbot_sec].ceilingheight
                              : sectors[bw->zbot_sec].floorheight;
        if (ztopz <= zbotz) continue;

        // S runs sOff .. sOff+sLen (1 map unit = 1 texel) from corner A to corner B.
        // sOff = sidedef->textureoffset (texels): software's rw_offset starts at
        // sidedef->textureoffset + curline->offset (r_segs.c:895); the bake is the
        // WHOLE linedef so curline->offset (the seg start within the line) is 0 at
        // corner A. The bake winds corner A as each side's own seg-v1 (front v1->v2,
        // back v2->v1), so this single sOff is correct for both sides.
        dxf  = (float)(bw->x2 - bw->x1) * (1.0f / 65536.0f);
        dyf  = (float)(bw->y2 - bw->y1) * (1.0f / 65536.0f);
        sLen = sqrtf(dxf * dxf + dyf * dyf);
        {
            float sOff = (float)bw->textureoffset * (1.0f / 65536.0f);
            sA   = sOff;
            sB   = sOff + sLen;
        }

        // Near-plane CLIP the straddling corner: slide it along the edge to depth
        // == nearz instead of dropping the whole wall (a dropped straddler flickers
        // as a hole when the player hugs a wall). depth, the view-space XY, and S are
        // all linear in the edge parameter u, so interpolate them at the crossing.
        if (dA < nearz)
        {
            float u = (float)(nearz - dA) / (float)(dB - dA);
            txa += (fixed_t)(u * (float)(txb - txa));
            tya += (fixed_t)(u * (float)(tyb - tya));
            dA   = nearz;
            sA   = u * sLen;
        }
        else if (dB < nearz)
        {
            float u = (float)(nearz - dB) / (float)(dA - dB);
            txb += (fixed_t)(u * (float)(txa - txb));
            tyb += (fixed_t)(u * (float)(tya - tyb));
            dB   = nearz;
            sB   = sLen - u * sLen;
        }

#ifdef BENCH_FORCE_MESH_RSP
        // RSP CUTOVER (n64_rdp_mesh_rsp), Step B = the actual offload: when the RSP
        // transform is authoritative, SKIP the CPU projection entirely (the two
        // 65536/depth divides + the lat/sc/sx/screen-Y mults) and read the per-corner
        // geometry straight from batch_out[] (filled by DL_RSPBatchProbe at the top).
        // ro->emit is the authoritative draw flag (the RSP near-clips / back-face /
        // off-screen culls exactly as the CPU else-branch below). S stays CPU (sA/sB
        // above); dA/dB (the painter sort key) and ztopz/zbotz (downstream pegging) are
        // already resolved above the projection. 16.16 -> float via /65536. Guarded by
        // the flag (rsp_bwall_out_t/batch_out exist only in RSP builds).
        if (n64_rdp_mesh_rsp)
        {
            const rsp_bwall_out_t* ro = &batch_out[i];
            const float k = 1.0f / 65536.0f;
            if (!ro->emit) continue;
            invwA = (float)ro->invwA * k; invwB = (float)ro->invwB * k;
            sxA   = (float)ro->sxA   * k; sxB   = (float)ro->sxB   * k;
            ytA   = (float)ro->ytA   * k; ybA   = (float)ro->ybA   * k;
            ytB   = (float)ro->ytB   * k; ybB   = (float)ro->ybB   * k;
        }
        else
#endif
        {
            lA = FixedMul(tya, vcos) - FixedMul(txa, vsin);
            lB = FixedMul(tyb, vcos) - FixedMul(txb, vsin);

            invwA = 65536.0f / (float)dA;           // == 1/depth_mapunits (dl_invw_k)
            invwB = 65536.0f / (float)dB;
            scA   = (float)centerx * invwA;         // px per map-unit height at v1
            scB   = (float)centerx * invwB;

            sxA = (float)centerx - (float)lA * (1.0f / 65536.0f) * scA;
            sxB = (float)centerx - (float)lB * (1.0f / 65536.0f) * scB;
            if (sxB <= sxA) continue;               // back-facing / degenerate
            if (sxB <= 0.0f || sxA >= (float)(SCREENWIDTH - 1)) continue;  // off-screen

            topf = (float)ztopz * (1.0f / 65536.0f) - viewzf;
            botf = (float)zbotz * (1.0f / 65536.0f) - viewzf;

            // Screen Y of the top/bottom edge at each corner (CPU path).
            ytA = (float)centery - topf * scA; ybA = (float)centery - botf * scA;
            ytB = (float)centery - topf * scB; ybB = (float)centery - botf * scB;
        }

        // Perspective-correct SCREEN-EDGE clip. The naive version clamped screen x
        // (xa/xb) but kept the off-screen corner's S/invw/Y -> a wall spanning past a
        // screen edge had its texture SHEARED across the visible span (the close-wall
        // smear). invw, S*invw, and screen Y are ALL linear in screen x for a planar
        // wall quad, so clip each edge by lerping every attribute at the clip x.
        {
            float siA  = sA * invwA, siB = sB * invwB;      // S*invw at each (clipped) corner
            float dsx  = sxB - sxA;
            float tL   = (sxA < 0.0f) ? (0.0f - sxA) / dsx : 0.0f;
            float tR   = (sxB > (float)(SCREENWIDTH - 1))
                             ? ((float)(SCREENWIDTH - 1) - sxA) / dsx : 1.0f;
            float invw_l = invwA + tL * (invwB - invwA);
            float invw_r = invwA + tR * (invwB - invwA);
            float si_l   = siA + tL * (siB - siA);
            float si_r   = siA + tR * (siB - siA);
            float sx_l   = sxA + tL * dsx;
            float sx_r   = sxA + tR * dsx;

            xa = (int)(sx_l + 0.5f);
            xb = (int)(sx_r + 0.5f);
            if (xa < 0) xa = 0;
            if (xb > SCREENWIDTH - 1) xb = SCREENWIDTH - 1;
            if (xb <= xa) continue;
            w.x1 = (int16_t)xa;
            w.x2 = (int16_t)xb;

            w.ytop_l = ytA + tL * (ytB - ytA);
            w.ybot_l = ybA + tL * (ybB - ybA);
            w.ytop_r = ytA + tR * (ytB - ytA);
            w.ybot_r = ybA + tR * (ybB - ybA);

            // S = (S*invw)/invw at the (possibly clipped) endpoints.
            w.s_l = si_l / invw_l;
            w.s_r = si_r / invw_r;

            // VERTICAL PEGGING (mirror of R_StoreWallRange's rw_*texturemid, r_segs.c).
            // texturemid_world = the ABSOLUTE world height that maps to texture row 0:
            //   peg sector's live floor/ceiling [+ textureheight] + rowoffset.
            // This drops the "- viewz" from software's *texturemid (which is viewz-
            // relative) -- the mesh lives in world space, so T is viewz-independent.
            // T(z) = texturemid_world - z  in texels (1 map unit = 1 texel vertically),
            // matching software's column-T = dc_texturemid - (worldz - viewz). Heights
            // are LIVE (resolved from the peg sector this frame) so a moving door pegs
            // to the live height -- door tracks no longer slide as the sector moves.
            {
                fixed_t pegz = bw->peg_ceil ? sectors[bw->peg_sec].ceilingheight
                                            : sectors[bw->peg_sec].floorheight;
                float   midw = (float)pegz * (1.0f / 65536.0f);
                if (bw->peg_addth)
                    midw += (float)textureheight[bw->texture] * (1.0f / 65536.0f);
                midw += (float)bw->rowoffset * (1.0f / 65536.0f);  // sidedef rowoffset

                {
                    float ztopf = (float)ztopz * (1.0f / 65536.0f);
                    float zbotf = (float)zbotz * (1.0f / 65536.0f);
                    w.t_top_l = w.t_top_r = midw - ztopf;   // T at the top edge
                    w.t_bot_l = w.t_bot_r = midw - zbotf;   // T at the bottom edge
                }
            }
            w.invw_l = invw_l;
            w.invw_r = invw_r;
        }

        lvl = (255 - bw->light) >> 3;               // sector light -> colormap level
        if (lvl < 0) lvl = 0;
        if (lvl > NUMCOLORMAPS - 1) lvl = NUMCOLORMAPS - 1;
        w.light = (uint8_t)lvl;
        w.texid = (uint16_t)bw->texture;
        w.bucket_next = -1;

        if (nrec < DL_WALL_ARENA)
        {
            mrec[nrec]   = w;
            mdepth[nrec] = (dA < dB) ? dA : dB;     // nearest corner = sort key
            nrec++;
        }
    }

    // Painter's order (no Z-buffer): emit FAR-to-NEAR so a nearer wall overwrites a
    // farther one in shared columns. Insertion sort -- nrec is tiny (tens/frame).
    {
        int a, b;
        for (a = 1; a < nrec; a++)
        {
            rdp_wall_t tw = mrec[a];
            fixed_t    td = mdepth[a];
            for (b = a - 1; b >= 0 && mdepth[b] < td; b--)
            {
                mrec[b + 1]   = mrec[b];
                mdepth[b + 1] = mdepth[b];
            }
            mrec[b + 1]   = tw;
            mdepth[b + 1] = td;
        }
        for (a = 0; a < nrec; a++)
            DL_EmitWallTier(&mrec[a]);
    }

    {
        static unsigned mf = 0;
        if ((mf++ & 511) == 0)
            debugf("MESH: emitted %d / %d baked walls\n", nrec, bake_numwalls);
    }
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

int DL_PlanePolyOn(void)
{
    return DL_PlaneRouteOn() && n64_rdp_plane_poly;
}

int DL_AnyRouteOn(void)
{
    // Mesh walls also key-clear: their suppressed CPU columns must hold the key so
    // the RDP mesh shows through the present, even in a mesh-walls-only config (no
    // routed planes). Gating on walls/planes alone would leave the clear unarmed.
    return DL_WallRouteOn() || DL_PlaneRouteOn() || DL_MeshRouteOn();
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
        float bias = (float)(IFLOOR(smin / (float)blkw) * blkw);
        IFLOOR_CHK((int)bias / blkw, smin / (float)blkw);
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
        if (dl_wall_z)
        {
            float zl = DL_WallZ(w->invw_l), zr = DL_WallZ(w->invw_r);
            float tl[6] = { xl, ytl, zl, s_l, tl0, w->invw_l };
            float tr[6] = { xr, ytr, zr, s_r, tr0, w->invw_r };
            float bl[6] = { xl, ybl, zl, s_l, tl1, w->invw_l };
            float br[6] = { xr, ybr, zr, s_r, tr1, w->invw_r };
            rdpq_triangle(&TRIFMT_ZBUF_TEX, tl, tr, bl);
            rdpq_triangle(&TRIFMT_ZBUF_TEX, tr, br, bl);
            dl_tris += 2;
        }
        else
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
        period_base = IFLOOR(cur / (float)blkh) * blkh;
        IFLOOR_CHK(period_base / blkh, cur / (float)blkh);
        src_lo = IFLOOR(cur) - period_base;
        IFLOOR_CHK(src_lo + period_base, cur);
        if (src_lo < 0) src_lo = 0;             // float-edge paranoia
        if (src_lo >= blkh) src_lo = blkh - 1;

        src_cap = src_lo + cap;
        if (src_cap > blkh)
            src_cap = blkh;                     // split at the period edge

        band_end = (float)(period_base + src_cap);
        if (band_end > t1)
            band_end = t1;

        src_hi = ICEIL(band_end - (float)period_base);
        ICEIL_CHK(src_hi, band_end - (float)period_base);
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

        if (dl_wall_z)
        {
            float zl = DL_WallZ(w->invw_l), zr = DL_WallZ(w->invw_r);
            float tl[6] = { xl, ytl, zl, s_l, a_l - base, w->invw_l };
            float tr[6] = { xr, ytr, zr, s_r, a_r - base, w->invw_r };
            float bl[6] = { xl, ybl, zl, s_l, b_l - base, w->invw_l };
            float br[6] = { xr, ybr, zr, s_r, b_r - base, w->invw_r };
            rdpq_triangle(&TRIFMT_ZBUF_TEX, tl, tr, bl);
            rdpq_triangle(&TRIFMT_ZBUF_TEX, tr, br, bl);
            dl_tris += 2;
        }
        else
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

    // Period-bias V by a whole-64 multiple. The flat's WORLD V period is 64, so
    // subtracting a SHARED whole number of 64-periods from both V endpoints is
    // sampling-identical (keeps the fixed-point coords in range).
    {
        float vmin = (va < vb) ? va : vb;
        vbias = IFLOOR(vmin / 64.0f) * 64;
        IFLOOR_CHK(vbias / 64, vmin / 64.0f);
    }
    va -= (float)vbias;
    vb -= (float)vbias;
    // 64x32 DECIMATED FLAT: the stored block is V-decimated 64->32 (out[k]=src[2k]),
    // 2 KB and fully TMEM-resident, so there is no V window to compute -- the whole
    // flat loads once. HALVE the (already period-biased) V endpoints: a v_world that
    // mapped to texel v (period 64) now maps to v/2 (period 32), and the mask-5
    // (period-32) T-wrap addresses across the resident tile.
    va *= 0.5f;
    vb *= 0.5f;

    // U period bias (keep S endpoints in a sane fixed-point range; the tile
    // wraps S with mask 6, so a shared period subtraction is sampling-identical).
    ulo = IFLOOR((ua < ub) ? ua : ub);
    IFLOOR_CHK(ulo, (ua < ub) ? ua : ub);
    uhi = ICEIL((ua > ub) ? ua : ub);
    ICEIL_CHK(uhi, (ua > ub) ? ua : ub);
    ubias = (ulo / 64) * 64;
    if (ulo < 0) ubias = ((ulo - 63) / 64) * 64;    // floor toward -inf
    (void)uhi;
    ua -= (float)ubias;
    ub -= (float)ubias;

    // Tile descriptor: 64-period wrap on S (mask 6), 32-period on T (mask 5) --
    // the decimated flat is 64 wide x 32 tall. Deduped -- every flat span shares
    // the same tile geometry, so this SET_TILE is issued once per flush, not per
    // span.
    if (dl_last_tile_lw != 64 || dl_last_tile_wrap != 2)
    {
        rdpq_tileparms_t tp;
        memset(&tp, 0, sizeof(tp));
        tp.s.mask = 6;          // 64-texel S period
        tp.t.mask = 5;          // 32-texel T period (decimated flat)
        rdpq_set_tile(TILE0, FMT_CI8, 0, 64, &tp);
        dl_last_tile_lw   = 64;
        dl_last_tile_wrap = 2;   // sentinel != wall wrap values (0/1)
        dl_last_up_block  = NULL;
    }

    // Load the WHOLE 64x32 decimated flat in ONE call -- 2 KB, fully TMEM-resident,
    // so no per-span V window. The triangle T (halved + period-relative) addresses
    // within [0,32) and the mask-5 wrap handles the rest. Dedup on the source block
    // only -> the LOAD_TILE (and its autosync) is skipped across a flat's spans.
    if (block != dl_last_up_block)
    {
        rdpq_load_tile(TILE0, 0, 0, 64, 32);
        dl_last_up_block = block;
        dl_last_up_lo    = 0;
        dl_last_up_rows  = 32;
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

// --- Stage-4b plane POLYGON draw -------------------------------------------
// Draw ONE trapezoid run as a single PERSPECTIVE-correct textured tri-pair.
// Unlike the per-span affine path, a run spans many screen rows and many flat
// z-depths, so persp is ON (set once by the caller) and each corner carries its
// own INV_W ~ (y-centery)/planeheight -- the RDP's hyperbolic divide then
// reconstructs software's affine flat map exactly (u*INV_W and v*INV_W are
// screen-affine for a constant-z plane, verified in r_plane.c's emit).
//
// CI8 64x64 flat, mask-6 S/T HARDWARE wrap (64 period) on BOTH axes. S wraps in
// hardware so no S window is needed. T (V) is capped by TMEM: a 64-wide CI8 tile
// fits DL_TMEM_HALF/64 = 32 rows, so we load a 32-row, 16-aligned V window that
// covers the run's V extent and let the mask-6 T-wrap address within it. A run
// whose V extent exceeds 32 rows is a very near, steep floor (rare); the window
// clamps to 32 rows -- the over-extent texels wrap and re-sample within the
// loaded window, the SAME bounded near-floor minification the span path documents.
// The tile descriptor (64-wide, mask 6) and resident V window are deduped across
// a flat's runs (consecutive runs at a similar depth share a window -> skip the
// LOAD_TILE), the dominant DL_BUILD saving.
// Per-corner SHADE (R,G,B,A in 0..1) for one plane corner's colormap level. The
// level keys dl_prim_lut[] (the same colormap-darkening RGB the flat-PRIM path
// used); dividing by 255 puts it in rdpq_triangle's required 0..1 shade range. The
// RDP gouraud-interpolates these four corner shades across the quad -> a continuous
// depth-light gradient (smooth like software's per-row falloff), where the old
// flat PRIM gave one brightness step per quad (the visible bands under the flash).
static inline void DL_PlaneCornerShade(uint8_t level, float out[4])
{
    uint32_t c = (level < NUMCOLORMAPS) ? dl_prim_lut[level] : dl_unlit_prim;
    out[0] = (float)((c >> 24) & 0xFF) * (1.0f / 255.0f);   // R
    out[1] = (float)((c >> 16) & 0xFF) * (1.0f / 255.0f);   // G
    out[2] = (float)((c >>  8) & 0xFF) * (1.0f / 255.0f);   // B
    out[3] = 1.0f;                                          // A (opaque)
}

static void DL_DrawPlanePoly(const rdp_ppoly_t* p, byte* block)
{
    float vmin, vmax, umin;
    float u_tl, v_tl, u_tr, v_tr, u_bl, v_bl, u_br, v_br;
    int   vbias, ubias;
    float xl, xr;
    // Four corner SHADE colours (depth-light); the combiner is TEX0*SHADE (set by
    // DL_FlushPlanePolys), so light now interpolates corner->corner instead of one
    // flat PRIM per quad. No rdpq_set_prim_color here -- SHADE carries the light.
    float sh_tl[4], sh_tr[4], sh_bl[4], sh_br[4];
    DL_PlaneCornerShade(p->light_tl, sh_tl);
    DL_PlaneCornerShade(p->light_tr, sh_tr);
    DL_PlaneCornerShade(p->light_bl, sh_bl);
    DL_PlaneCornerShade(p->light_br, sh_br);

    // Per-corner texel U/V (texel units from the emit). PERSPECTIVE-SAFE PERIOD
    // BIAS: the raw flat texel coords can be huge (a far/steep run's u,v run to
    // thousands of texels), which overflows rdpq_triangle's s10.5 S/T cast. The
    // mask-6 S/T wrap (64 period) makes subtracting a WHOLE-64 multiple from a
    // texel coord sampling-identical, and under perspective a per-vertex constant
    // bias B shifts the reconstructed per-pixel S uniformly (RDP computes
    // S_px = interp(S*INV_W)/interp(INV_W) = S - B for S' = S-B), so a whole-64 B
    // wraps back to the same texel. Bias EACH axis by its min-corner whole-64
    // period so all four corners land near [0,64+spread). (The remaining within-
    // run spread is bounded by the Y-deviation run split; an extreme steep near-
    // floor whose spread still exceeds the s10.5 range is the documented bounded
    // near-floor artifact, the same class the span path's V clamp accepts.)
    u_tl = p->u_tl; v_tl = p->v_tl;
    u_tr = p->u_tr; v_tr = p->v_tr;
    u_bl = p->u_bl; v_bl = p->v_bl;
    u_br = p->u_br; v_br = p->v_br;

    // U/V extents across the four corners.
    umin = u_tl;
    if (u_tr < umin) umin = u_tr;
    if (u_bl < umin) umin = u_bl;
    if (u_br < umin) umin = u_br;
    vmin = v_tl; vmax = v_tl;
    if (v_tr < vmin) vmin = v_tr; if (v_tr > vmax) vmax = v_tr;
    if (v_bl < vmin) vmin = v_bl; if (v_bl > vmax) vmax = v_bl;
    if (v_br < vmin) vmin = v_br; if (v_br > vmax) vmax = v_br;

    // IFLOOR/int-cast SAFETY: the extents feed dl_ifloor((extent)/64), whose
    // (int) cast traps if |extent/64| >= 2^31. The corner horizon guard
    // (r_plane.c, dyrows>=1) already bounds these well under that, but clamp the
    // extents defensively to a generous +-1e8 texels so a degenerate camera frame
    // can never trap the floor's IFLOOR (the period bias below + HW mask-6 wrap
    // make the exact extent value irrelevant past whole-64 multiples anyway).
    if (umin >  1.0e8f) umin =  1.0e8f; else if (umin < -1.0e8f) umin = -1.0e8f;
    if (vmin >  1.0e8f) vmin =  1.0e8f; else if (vmin < -1.0e8f) vmin = -1.0e8f;
    if (vmax >  1.0e8f) vmax =  1.0e8f; else if (vmax < -1.0e8f) vmax = -1.0e8f;

    // Whole-64 period bias on BOTH axes (mask-6 wrap -> sampling-identical).
    ubias = IFLOOR(umin / 64.0f) * 64;
    IFLOOR_CHK(ubias / 64, umin / 64.0f);
    u_tl -= (float)ubias; u_tr -= (float)ubias;
    u_bl -= (float)ubias; u_br -= (float)ubias;

    vbias = IFLOOR(vmin / 64.0f) * 64;
    IFLOOR_CHK(vbias / 64, vmin / 64.0f);
    v_tl -= (float)vbias; v_tr -= (float)vbias;
    v_bl -= (float)vbias; v_br -= (float)vbias;
    vmin -= (float)vbias; vmax -= (float)vbias;

    // 64x32 DECIMATED TILE: the stored flat is V-decimated 64->32 (out[k]=src[2k]),
    // so the flat's 64-world-unit V period now lives in 32 texel rows. HALVE the
    // per-vertex V: a v_world that mapped to texel v (period 64) now maps to v/2
    // (period 32). The whole-64 vbias above is already period-relative, so halving
    // the biased coords keeps the min corner in [0, 32+spread/2) and the mask-5
    // (period-32) T-wrap addresses across the fully-resident tile. S/u unchanged.
    v_tl *= 0.5f; v_tr *= 0.5f;
    v_bl *= 0.5f; v_br *= 0.5f;

    // Tile descriptor: full 64-period wrap on S (mask 6), 32-period on T (mask 5)
    // -- the decimated flat is 64 wide x 32 tall. Deduped -- every flat poly shares
    // this geometry, so SET_TILE issues once per flush.
    if (dl_last_tile_lw != 64 || dl_last_tile_wrap != 2)
    {
        rdpq_tileparms_t tp;
        memset(&tp, 0, sizeof(tp));
        tp.s.mask = 6;          // 64-texel S period
        tp.t.mask = 5;          // 32-texel T period (decimated flat)
        rdpq_set_tile(TILE0, FMT_CI8, 0, 64, &tp);
        dl_last_tile_lw   = 64;
        dl_last_tile_wrap = 2;
        dl_last_up_block  = NULL;
    }

    // Load the WHOLE 64x32 decimated flat in ONE call -- it is 2 KB, fully TMEM-
    // resident, so no per-poly V window is needed. The triangle T (halved + period-
    // relative) addresses within [0,32) and the mask-5 wrap handles the rest. Dedup
    // on the source block only -> skip the LOAD_TILE + autosync across a flat's runs.
    if (block != dl_last_up_block)
    {
        rdpq_load_tile(TILE0, 0, 0, 64, 32);
        dl_last_up_block = block;
        dl_last_up_lo    = 0;
        dl_last_up_rows  = 32;
        dl_last_up_c0    = 0;
        dl_tile_loads++;
        dl_uploads++;
    }

    // NO per-vertex S/T clamp. The four corner texels are NOT independent: under
    // perspective the RDP reconstructs S_px = interp(S*INV_W)/interp(INV_W), which
    // is exact ONLY when (S*INV_W) is planar across the corners (it is -- u*INV_W
    // is screen-affine for a constant-z plane). Clamping a SUBSET of corners (a
    // far/near corner only) perturbs (S*INV_W) non-uniformly, breaks that
    // planarity, and the per-pixel divide then reconstructs garbage -> the
    // horizontal-streak white noise. The cast can't trap here regardless: the
    // uniform whole-64 bias above lands the min corner in [0,64), and libdragon's
    // perspective setup multiplies each S by its NORMALISED INV_W (invw*minw <= 1)
    // before the s16.16 cast -- a far corner's large raw S is scaled by its small
    // (far) INV_W, so S*INV_W stays bounded exactly as it does for wide walls. And
    // the vendored float_to_s16_16 SATURATES (>=32768 -> 0x7FFFFFFF) rather than
    // trapping, so even a degenerate corner can't fault the tri setup. (The only
    // genuine (int)x trunc.w.s trap risk is the V-window IFLOOR on huge extents,
    // already bounded by the +-1e8 extent clamp above -- those feed window
    // selection only, never the emitted vertex coords.)

    // Trapezoid quad: left edge at column x1, right edge at column x2+1; each
    // edge spans its own [ytop..ybot] screen rows. Per-corner S/T (texels) +
    // INV_W (persp ON). Tri-pair (tl,tr,bl),(tr,br,bl).
    xl = (float)p->x1;
    xr = (float)p->x2 + 1.0f;

#if PLANE_UV_TRACE
    // ---- FINAL S/T saturation self-trace (diagnostic, no render effect) -------
    // u_tl..v_br here are the EXACT post-period-bias (+V-decimation) coords about
    // to be handed to rdpq_triangle. Dump per sample poly on a selected BENCH
    // marker frame: the 4 corners, each corner's u*32/v*32 + SAT flag, the texel
    // SPREAD, the screen extent, and FLOOR/CEILING. Pairs with BENCH_MARK frame=N
    // (this fires when N64Bench_FrameNo()==N-1). See the header note for the
    // hypothesis (spread overruns s10.5 at glancing/deep views).
    {
        unsigned long fno = N64Bench_FrameNo();
        const unsigned long puvt2_target[2] = {
            (unsigned long)PLANE_UV_TRACE_FRAME,
            (unsigned long)PLANE_UV_TRACE_FRAME2     // 0 == slot disabled
        };
        int slot = -1;
        int s;
        // Pick the slot this draw belongs to. A slot that is ALREADY ARMED this
        // frame keeps draining (matches puvt2_arm_fno). Otherwise the FIRST
        // not-yet-fired enabled slot whose target has been reached (fno+1 >=
        // FRAME) arms NOW and latches to this fno. The ">=" (not "==") tolerates
        // the bench's excluded level-reload frames skewing the exact fno a target
        // render lands on, so a slot can never silently miss its window.
        for (s = 0; s < 2; s++)
        {
            if (puvt2_arm_fno[s] == fno) { slot = s; break; }   // still draining
        }
        if (slot < 0)
        {
            for (s = 0; s < 2; s++)
            {
                if (puvt2_target[s] == 0 || puvt2_fired[s]) continue;
                if (fno + 1 >= puvt2_target[s])
                {
                    slot = s;
                    puvt2_arm_fno[s]    = fno;
                    puvt2_fired[s]      = 1;
                    puvt2_polys_left[s] = PLANE_UV_TRACE_POLYS;
                    debugf("PUVT2_FRAME slot=%d marker=%lu fno=%lu centery=%d\n",
                           s, puvt2_target[s], fno, centery);
                    break;
                }
            }
        }

        if (slot >= 0)
        {
            if (puvt2_polys_left[slot] > 0)
            {
                // Post-bias texel extents (the SPREAD the s10.5 cast must hold).
                float umn = u_tl, umx = u_tl, vmn = v_tl, vmx = v_tl;
                if (u_tr < umn) umn = u_tr; if (u_tr > umx) umx = u_tr;
                if (u_bl < umn) umn = u_bl; if (u_bl > umx) umx = u_bl;
                if (u_br < umn) umn = u_br; if (u_br > umx) umx = u_br;
                if (v_tr < vmn) vmn = v_tr; if (v_tr > vmx) vmx = v_tr;
                if (v_bl < vmn) vmn = v_bl; if (v_bl > vmx) vmx = v_bl;
                if (v_br < vmn) vmn = v_br; if (v_br > vmx) vmx = v_br;
                {
                    float u_spread = umx - umn;
                    float v_spread = vmx - vmn;
                    // FLOOR vs CEILING: a ceiling poly's rows sit ABOVE the horizon
                    // (mean screen Y < centery); a floor's below. centery splits the
                    // two regardless of planeheight sign carried per-poly.
                    float ymean = 0.25f*(p->ytop_l + p->ytop_r + p->ybot_l + p->ybot_r);
                    const char* kind = (ymean < (float)centery) ? "CEIL" : "FLOOR";
                    // Per-corner s10.5 product + saturation flag.
                    float cu[4] = { u_tl, u_tr, u_bl, u_br };
                    float cv[4] = { v_tl, v_tr, v_bl, v_br };
                    const char* nm[4] = { "TL", "TR", "BL", "BR" };
                    int sat_any = 0;
                    int i;

                    debugf("PUVT2_POLY n=%d kind=%s x1=%d x2=%d "
                           "ytop=%d.%03d..%d.%03d ybot=%d.%03d..%d.%03d "
                           "u_spread=%d.%03d v_spread=%d.%03d\n",
                           PLANE_UV_TRACE_POLYS - puvt2_polys_left[slot], kind,
                           p->x1, p->x2,
                           IFLOORF(p->ytop_l), MILLIFRAC(p->ytop_l),
                           IFLOORF(p->ytop_r), MILLIFRAC(p->ytop_r),
                           IFLOORF(p->ybot_l), MILLIFRAC(p->ybot_l),
                           IFLOORF(p->ybot_r), MILLIFRAC(p->ybot_r),
                           IFLOORF(u_spread), MILLIFRAC(u_spread),
                           IFLOORF(v_spread), MILLIFRAC(v_spread));

                    for (i = 0; i < 4; i++)
                    {
                        // s10.5 product as an integer (round to nearest) + |.|>=lim.
                        int   us32 = (int)floorf(cu[i] * 32.0f + 0.5f);
                        int   vs32 = (int)floorf(cv[i] * 32.0f + 0.5f);
                        int   us_sat = PUVT2_SAT(cu[i]) ? 1 : 0;
                        int   vs_sat = PUVT2_SAT(cv[i]) ? 1 : 0;
                        sat_any |= us_sat | vs_sat;
                        debugf("PUVT2_C %s u=%d.%03d v=%d.%03d u32=%d v32=%d "
                               "u_sat=%d v_sat=%d invw=%d.%03d\n",
                               nm[i],
                               IFLOORF(cu[i]), MILLIFRAC(cu[i]),
                               IFLOORF(cv[i]), MILLIFRAC(cv[i]),
                               us32, vs32, us_sat, vs_sat,
                               IFLOORF((i==0)?p->invw_tl:(i==1)?p->invw_tr:
                                       (i==2)?p->invw_bl:p->invw_br),
                               MILLIFRAC((i==0)?p->invw_tl:(i==1)?p->invw_tr:
                                         (i==2)?p->invw_bl:p->invw_br));
                    }
                    debugf("PUVT2_SUM n=%d kind=%s u_spread=%d v_spread=%d "
                           "SAT=%d\n",
                           PLANE_UV_TRACE_POLYS - puvt2_polys_left[slot], kind,
                           (int)floorf(u_spread), (int)floorf(v_spread), sat_any);

                    puvt2_polys_left[slot]--;
                }
            }
            // Slot self-terminates: once puvt2_polys_left[slot] hits 0 the dump
            // stops, and puvt2_fired[slot] keeps the slot from re-arming on any
            // later frame. No cross-frame re-arm bookkeeping needed.
        }
    }
#endif // PLANE_UV_TRACE

    {
        // TRIFMT_SHADE_TEX vertex: {X, Y, R, G, B, A, S, T, INV_W} (9 floats). The
        // RGBA is this corner's depth-light SHADE; the RDP gouraud-interpolates it
        // across the quad and the TEX0*SHADE combiner multiplies it onto the flat
        // texel -- a smooth depth gradient replacing the old flat-per-quad PRIM.
        float tl[9] = { xl, p->ytop_l, sh_tl[0], sh_tl[1], sh_tl[2], sh_tl[3],
                        u_tl, v_tl, p->invw_tl };
        float tr[9] = { xr, p->ytop_r, sh_tr[0], sh_tr[1], sh_tr[2], sh_tr[3],
                        u_tr, v_tr, p->invw_tr };
        float bl[9] = { xl, p->ybot_l, sh_bl[0], sh_bl[1], sh_bl[2], sh_bl[3],
                        u_bl, v_bl, p->invw_bl };
        float br[9] = { xr, p->ybot_r, sh_br[0], sh_br[1], sh_br[2], sh_br[3],
                        u_br, v_br, p->invw_br };
        rdpq_triangle(&TRIFMT_SHADE_TEX, tl, tr, bl);
        rdpq_triangle(&TRIFMT_SHADE_TEX, tr, br, bl);
    }
    dl_tris += 2;
}

// Drain the frame's plane POLYGONS (trapezoid strips) into the attached display
// fb. Per-FLAT bucket walk mirroring DL_FlushSpans: for each flat used this
// frame, copy + pin its 64x64 CI8 block ONCE, point the RDP at it, then draw
// every trapezoid run in that flat's bucket. Persp is ON (a run spans many
// z-depths); the caller restores its own persp after. Shares the flat bucket
// arrays + dl_flat_touched[] with the span path -- only one of the two paths
// queued this frame, so the bucket walk is unambiguous.
// Plane damage-flash overlay (i_video_n64.c). On a flash the planes are drawn
// through the UN-FLASHED base TLUT (so their CI8 texels keep their normal hue,
// like software's pre-remap index) and then ONE uniform translucent rect is
// alpha-blended over the plane region, coloured + alpha'd from the active flash --
// reproducing software's whole-screen palette wash uniformly, instead of the per-
// pixel TEX0-multiply that bled the floor hue (the three diff spots on frame 2048).
extern boolean  I_N64UploadBaseTLUT(void);    // load un-flashed base TLUT into TMEM
extern void     I_N64UploadMasterTLUT(void);  // restore flashed master after the planes
extern uint32_t I_N64PlaneFlashARGB(void);    // active flash tint 0xRRGGBBAA, 0 = none
extern int      scaledviewwidth;              // r_state.h: view window width
extern int      viewwindowx;                  // r_main.h: view window x origin
extern int      viewheight;                   // r_main.h: view window height

static void DL_FlushPlanePolys(void)
{
    int fi;
    uint32_t flash;            // active uniform flash tint (0xRRGGBBAA), 0 = none
    boolean  unflashed;        // true once the base TLUT is resident for the planes

    if (dl_ppoly_count <= 0)
        return;

    // PLANE DAMAGE-FLASH CORRECTNESS (screen-space uniform tint, Ryan's design).
    // The plane CI8 TEX0 samples the RESIDENT master TLUT, which is the FLASHED
    // palette -- so under a flash the planes pick up the wash as a per-pixel
    // TEX0-multiply, which preserves each texel's channel ratios instead of doing
    // software's single index->palette remap (the floor hue bled: too-green near
    // floor + two too-red triangles, frame 2048). Fix: if a flash is active, swap
    // the master for the UN-FLASHED base TLUT so the planes draw their normal
    // colours, draw them, then lay ONE translucent rect over the plane region (the
    // view-window scissor the caller set) coloured+alpha'd to the flash -- a
    // uniform wash by construction. The flashed master is re-asserted before we
    // return so the CI8 sprites/HUD/present-blit are unaffected. Off-flash (flash
    // == 0) NOTHING changes: master stays resident, no overlay -> perf-neutral.
    flash = I_N64PlaneFlashARGB();
    unflashed = false;
    if (flash)
        unflashed = I_N64UploadBaseTLUT();   // false if base not captured yet -> skip

    // SHADE stays the un-flashed depth-light ramp (f35d6ce); now that TEX0 is also
    // un-flashed (base TLUT) the planes carry NO flash at all, and the overlay below
    // applies it once. Kept as a documented no-op call site.
    DL_RetintPrimLUT();

    // PER-VERTEX SHADE light: the plane polys now carry per-corner depth-light as
    // SHADE (RGBA), so switch the combiner from TEX0*PRIM (TEX_FLAT, the caller's
    // world mode) to TEX0*SHADE (TEX_SHADE). The RDP gouraud-interpolates SHADE
    // corner->corner -> a smooth depth gradient instead of one flat PRIM step per
    // quad (the visible bands). Restored to TEX_FLAT at the end so the wall path
    // (combined build) and the present-blit keep their assumed combiner.
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);

    // Floors span many z-depths -> perspective ON so S/T are perspective-correct
    // (affine would warp a tall floor poly). The walls left persp ON too.
    rdpq_mode_persp(true);

    // Reset the dedup caches: the flat tile geometry (64-wide CI8, mask-6 wrap)
    // differs from the wall tiles the wall flush left resident.
    dl_last_up_block  = NULL;
    dl_last_up_lo     = -1;
    dl_last_up_rows   = -1;
    dl_last_up_c0     = -1;
    dl_last_tile_lw   = -1;
    dl_last_tile_wrap = -1;

    for (fi = 0; fi < dl_flat_touched_count; fi++)
    {
        int   flatidx = dl_flat_touched[fi];
        int   pidx;
        byte* block;

        block = DL_FlatBlock(flatidx);
        if (!block)
            continue;       // no usable flat block: runs stay key

        DL_FlatMarkInFlight(flatidx);

        {
            surface_t fs = surface_make_linear(block, FMT_CI8, DL_FLAT_W, DL_FLAT_H);
            rdpq_set_texture_image(&fs);
            dl_last_up_block  = NULL;
            dl_last_tile_lw   = -1;
            dl_last_tile_wrap = -1;
        }

        for (pidx = dl_flat_bucket_head[flatidx]; pidx >= 0;
             pidx = dl_ppolys[pidx].bucket_next)
        {
            DL_DrawPlanePoly(&dl_ppolys[pidx], block);
        }
    }

    // UNIFORM FLASH OVERLAY. The planes above drew their un-flashed colours; now
    // wash the plane region with one translucent rect coloured+alpha'd to the
    // active flash. RDPQ_BLENDER_MULTIPLY computes PRIM_RGB*PRIM_A + MEMORY*(1-A)
    // per pixel with a CONSTANT PRIM -> the SAME wash on every plane pixel (uniform
    // by construction; kills the per-pixel-multiply non-uniformity). The rect spans
    // the whole view window -- it harmlessly also covers the wall/sky/sprite
    // regions of the 16bpp fb, but the present blit opaque-overwrites those from the
    // (already-flashed) CI8 buffer, so the overlay SURVIVES only where the RDP
    // planes drew (the key-cleared region) -- coverage-clipping for free via the
    // existing keyed present blit. The caller's view-window scissor still bounds it.
    if (flash)
    {
        // OVER-TINT CORRECTION. The recovered (target,alpha) is the EXACT linear
        // blend DOOM's palette math intends (FLASH_DBG confirmed: red flash target
        // (252,0,0) a=58/83/116, gold (216,180,70) a=58 -- spot-on). But software's
        // flash is a NON-LINEAR PLAYPAL index remap, while this overlay is a LINEAR
        // RDPQ_BLENDER_MULTIPLY. On bright, already-red-corner plane texels (E1M1
        // lava) the linear blend pulls every channel toward the pure-red target
        // proportionally and OVER-saturates, where the index remap of an already-red
        // colour shifts far less -- the frame-2048 floor went too-red at full alpha.
        // Knocking the rect alpha to 5/8 was MEASURED (two independent capture
        // geometries) to minimise the plane diff vs the frozen software reference:
        // frame-2048 floor |sw-rdp| 13.7 (full) -> 7.8 (5/8), below the pre-overlay
        // 9.9; ceiling 1.4 -> 0.7; horizon 7.9 -> 4.8. It leaves the subtle gold/
        // bonus flash visually uniform (that scene has little visible plane, so the
        // small under-tint is imperceptible) and is a pure no-op off-flash.
        {
            uint32_t fa = ((flash & 0xFF) * 5u) / 8u;
            if (fa > 0xFF) fa = 0xFF;
            flash = (flash & 0xFFFFFF00u) | fa;
        }
        rdpq_mode_combiner(RDPQ_COMBINER_FLAT);   // PRIM-only source for the blend
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY); // PRIM_RGB*PRIM_A + MEM*(1-PRIM_A)
        rdpq_set_prim_color(color_from_packed32(flash));
        {
            int vx0 = viewwindowx;
            int vy0 = viewwindowy;
            int vx1 = viewwindowx + scaledviewwidth;
            int vy1 = viewwindowy + viewheight;
            if (vx0 < 0) vx0 = 0;
            if (vy0 < 0) vy0 = 0;
            if (vx1 > SCREENWIDTH)  vx1 = SCREENWIDTH;
            if (vy1 > SCREENHEIGHT) vy1 = SCREENHEIGHT;
            if (vx1 > vx0 && vy1 > vy0)
                rdpq_fill_rectangle(vx0, vy0, vx1, vy1);
        }
        // Turn the blender back OFF: the wall path (combined build) and the present
        // COPY blit assume no blender. Re-assert the FLASHED master TLUT into TMEM
        // (we swapped it for the base above) so the CI8 sprites/HUD/present-blit
        // sample the flashed palette again. Only when we actually un-flashed.
        rdpq_mode_blender(0);
        if (unflashed)
            I_N64UploadMasterTLUT();
    }

    // Restore the world combiner (TEX0*PRIM) the caller/wall path assumes -- this
    // is the only place that switched to TEX_SHADE.
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
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

        // Point the RDP at this flat's 64x32 (V-decimated) row-major block once.
        // The TILE0 descriptor (64-wide, mask-6 S / mask-5 T wrap) is configured in
        // DL_DrawSpan and deduped; invalidate the resident band when the source
        // image changes.
        {
            surface_t fs = surface_make_linear(block, FMT_CI8, DL_FLAT_W, DL_FLAT_H);
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

// ---------------------------------------------------------------------------
// GPU port Phase 3: draw the baked floor leaf fans for visible subsectors.
// Mirrors DL_MeshDrawWalls's per-corner world->screen transform, but per LEAF with
// the LIVE sector floor height (doors/lifts move it). The flat texture is world-
// aligned (texel = world x/y, 64-period); the stored flat is V-decimated 64->32 so V
// is halved + mask-5 T-wrap, matching DL_DrawPlanePoly. Emits a triangle fan (corner
// 0 + edge i,i+1) with TRIFMT_ZBUF_TEX so the wall Z-buffer occludes floors. Called
// from DL_Flush after the wall bucket walk, WHILE Z is still enabled, with the world
// textured mode (RDPQ_COMBINER_TEX_FLAT, persp on) + master TLUT resident. Per-leaf
// flat upload (no bucketing yet -- correctness first). Slice 1: floors only, near-
// plane straddlers culled; ceilings/sky + the plane-suppression A/B come next.
#define DL_LEAF_MAXV 64
static int dl_leaf_tris = 0;
static void DL_DrawMeshLeaves(void)
{
    fixed_t vcos, vsin;
    float   viewzf;
    int     ss, drew = 0, surf;
    // Per-frame projection cache shared by the floor + ceiling passes (see below).
    static float*         leaf_proj = NULL;     // 6 floats/pool-vert: cx,sc,cu,cv,cw,cz
    static int            leaf_proj_cap = 0;
    static unsigned char* leaf_cull = NULL;     // [numsubsectors]: 1 = skip this frame
    static int            leaf_cull_cap = 0;

    if (!n64_rdp_mesh_floors || !DL_MeshRouteOn() || !bake_leaves || !bake_leafvis || !dl_wall_z)
        return;

    vcos   = finecosine[viewangle >> ANGLETOFINESHIFT];
    vsin   = finesine[viewangle >> ANGLETOFINESHIFT];
    viewzf = (float)viewz * (1.0f / 65536.0f);

    // Flat tile geometry: 64-wide x 32-tall decimated CI8, mask-6 S / mask-5 T wrap.
    {
        rdpq_tileparms_t tp;
        memset(&tp, 0, sizeof(tp));
        tp.s.mask = 6;
        tp.t.mask = 5;
        rdpq_set_tile(TILE0, FMT_CI8, 0, 64, &tp);
    }

    // Floor and ceiling share the SAME leaf polygon, so the per-vertex XY projection
    // (the 65536/depth divide, sc, screen-x, u/v, invw, z) is IDENTICAL for both --
    // only cy (the height term) differs. Transform each visible leaf's vertices ONCE
    // here into a per-frame cache keyed by leaf vertex-pool index, then both surface
    // passes below read it back + recompute the cheap cy. Halves the leaf transform
    // (the float divide especially) for every non-sky-ceiling subsector.
    {
        extern int bake_numleafverts;
        if (bake_numleafverts > leaf_proj_cap)
        {
            if (leaf_proj) Z_Free(leaf_proj);
            leaf_proj_cap = bake_numleafverts;
            leaf_proj = (float*)Z_Malloc(sizeof(float) * 6 * leaf_proj_cap, PU_STATIC, NULL);
        }
        if (numsubsectors > leaf_cull_cap)
        {
            if (leaf_cull) Z_Free(leaf_cull);
            leaf_cull_cap = numsubsectors;
            leaf_cull = (unsigned char*)Z_Malloc(numsubsectors, PU_STATIC, NULL);
        }
        if (!leaf_proj || !leaf_cull)
            return;                         // alloc failed -> skip the floor mesh this frame
    }

    for (ss = 0; ss < numsubsectors; ss++)
    {
        bake_leaf_t* lf = &bake_leaves[ss];
        int     n = lf->numverts, i, ubias, vbias, bad = 0;
        float   umin = 1.0e30f, vmin = 1.0e30f;

        leaf_cull[ss] = 1;                              // default: not drawable this frame
        if (!bake_leafvis[ss]) continue;
        if (n < 3 || n > DL_LEAF_MAXV) continue;
        if (lf->floorpic == skyflatnum && lf->ceilingpic == skyflatnum)
            continue;                                   // both surfaces sky -> never drawn

        for (i = 0; i < n; i++)
        {
            int     pv = lf->firstvert + i;
            fixed_t wx = bake_leaf_verts[pv][0];
            fixed_t wy = bake_leaf_verts[pv][1];
            fixed_t tx = wx - viewx, ty = wy - viewy;
            fixed_t depth = FixedMul(tx, vcos) + FixedMul(ty, vsin);
            fixed_t lat;
            float   invw, sc, sx, u, v, *pr;
            if (depth < (4 << FRACBITS)) { bad = 1; break; }    // near plane: cull
            lat  = FixedMul(ty, vcos) - FixedMul(tx, vsin);
            invw = 65536.0f / (float)depth;
            sc   = (float)centerx * invw;
            sx   = (float)centerx - (float)lat * (1.0f / 65536.0f) * sc;
            // X-only off-screen cull (cy is per-surface, checked in the draw pass).
            if (sx < -2048.0f || sx > (float)SCREENWIDTH + 2048.0f) { bad = 1; break; }
            u = (float)(wx >> FRACBITS);                // world-aligned flat texel (64-period)
            v = (float)(wy >> FRACBITS);
            pr = &leaf_proj[6 * pv];
            pr[0] = sx; pr[1] = sc; pr[2] = u; pr[3] = v;
            pr[4] = invw; pr[5] = DL_WallZ(invw);
            if (u < umin) umin = u;
            if (v < vmin) vmin = v;
        }
        if (bad) continue;

        // Whole-64 period bias (mask-6 S wrap = sampling-identical); V halved for the
        // 64->32 decimated tile. Both surfaces share these, so bake them in now.
        ubias = IFLOOR(umin / 64.0f) * 64;
        vbias = IFLOOR(vmin / 64.0f) * 64;
        for (i = 0; i < n; i++)
        {
            float* pr = &leaf_proj[6 * (lf->firstvert + i)];
            pr[2] -= (float)ubias;
            pr[3]  = (pr[3] - (float)vbias) * 0.5f;
        }
        leaf_cull[ss] = 0;                              // transformed OK -> drawable
    }

    // surf 0 = floor, surf 1 = ceiling. Same leaf polygon at the sector's floor/ceiling
    // height + flat. Both passes share the Z-buffer (already paid) so adding ceilings
    // costs only their tris, and lets us suppress the ceiling visplanes too (r_plane.c).
    for (surf = 0; surf < 2; surf++)
    {
    // Pass 1: collect visible non-sky leaves + their DISTINCT flats, so each flat is
    // uploaded to TMEM exactly ONCE this frame (the per-leaf upload spiked dlbuild to
    // 150ms). Pass 2 then walks per flat.
    {
        static int vis[2048];
        static int flats[256];
        int nvis = 0, nflat = 0, vi, fi;

        for (ss = 0; ss < numsubsectors; ss++)
        {
            bake_leaf_t* lf = &bake_leaves[ss];
            int fl, j, seen;
            int pic = surf ? lf->ceilingpic : lf->floorpic;
            if (leaf_cull[ss]) continue;                // not visible / culled in xform pass
            if (pic == skyflatnum) continue;            // sky stays CPU
            if (nvis >= 2048) break;
            vis[nvis++] = ss;
            fl = flattranslation[pic];
            seen = 0;
            for (j = 0; j < nflat; j++) if (flats[j] == fl) { seen = 1; break; }
            if (!seen && nflat < 256) flats[nflat++] = fl;
        }

        // Pass 2: per distinct flat, upload once, then draw every visible leaf on it.
        for (fi = 0; fi < nflat; fi++)
        {
            int   flatidx = flats[fi];
            byte* block   = DL_FlatBlock(flatidx);
            if (!block) continue;
            {
                surface_t fs = surface_make_linear(block, FMT_CI8, DL_FLAT_W, DL_FLAT_H);
                rdpq_set_texture_image(&fs);
            }

            for (vi = 0; vi < nvis; vi++)
            {
                bake_leaf_t* lf = &bake_leaves[vis[vi]];
                sector_t*    sec;
                float        hf, cy[DL_LEAF_MAXV];
                int          n, i, lvl, bad = 0;
                uint32_t     prim;

                if (flattranslation[surf ? lf->ceilingpic : lf->floorpic] != flatidx)
                    continue;                           // a different flat
                n   = lf->numverts;
                sec = &sectors[lf->sector];
                hf  = (float)(surf ? sec->ceilingheight : sec->floorheight)
                      * (1.0f / 65536.0f) - viewzf;

                // cy is the only per-surface term: recompute it (cheap) from the cached
                // sc, and cull a leaf whose height projects far off-screen (a degenerate
                // huge tri would stall the RDP -- the leaf path has no per-edge clip).
                for (i = 0; i < n; i++)
                {
                    float sc = leaf_proj[6 * (lf->firstvert + i) + 1];
                    cy[i] = (float)centery - hf * sc;
                    if (cy[i] < -2048.0f || cy[i] > (float)SCREENHEIGHT + 2048.0f)
                    { bad = 1; break; }
                }
                if (bad) continue;

                lvl = (255 - sec->lightlevel) >> 3;
                if (lvl < 0) lvl = 0;
                if (lvl > NUMCOLORMAPS - 1) lvl = NUMCOLORMAPS - 1;
                prim = dl_prim_lut[lvl];
                rdpq_set_prim_color(color_from_packed32(prim));

                for (i = 1; i < n - 1; i++)
                {
                    float* p0 = &leaf_proj[6 * (lf->firstvert + 0)];
                    float* pa = &leaf_proj[6 * (lf->firstvert + i)];
                    float* pb = &leaf_proj[6 * (lf->firstvert + i + 1)];
                    float t0[6] = { p0[0], cy[0],   p0[5], p0[2], p0[3], p0[4] };
                    float t1[6] = { pa[0], cy[i],   pa[5], pa[2], pa[3], pa[4] };
                    float t2[6] = { pb[0], cy[i+1], pb[5], pb[2], pb[3], pb[4] };
                    rdpq_triangle(&TRIFMT_ZBUF_TEX, t0, t1, t2);
                    drew++;
                }
            }
        }
    }
    }

    dl_leaf_tris = drew;
    {
        static unsigned lf_n = 0;
        if ((lf_n++ & 511) == 0)
            debugf("MESH: leaf tris=%d (floor+ceil)\n", dl_leaf_tris);
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

    // Walls need the Z-buffer for correct opaque occlusion: the painter's-order sort
    // in DL_MeshDrawWalls keys on each wall's NEAREST corner, which is only an
    // approximation of true depth. Two walls whose depth ranges overlap mis-order in
    // the columns where the far-by-nearest-corner wall is actually in front -- it gets
    // overwritten and the geometry behind it shows through (Ryan's "geometry in the
    // middle renders what's behind it"). Z-test fixes it per pixel. The z-image is
    // already attached + cleared every mesh frame (i_video_n64.c), and the RDP sits
    // idle (rdpbusy ~5us), so the per-pixel z cost barely touches the CPU-bound frame.
    dl_wall_z = n64_rdp_mesh && dl_zbuf_attached;

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

    // GPU port: the mesh wall pass uses a Z-buffer for correct opaque occlusion (the
    // z-image is cleared right after rdpq_attach in i_video). Enable depth test+write
    // for the walls only -- disabled again before the planes draw below.
    if (dl_wall_z)
        rdpq_mode_zbuf(true, true);

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

    // TLUT COLLISION FIX (combined full-RDP): the CI4 wall pass above overwrote
    // the 256-entry TLUT region with up to 16 per-texture sub-palettes. The CI8
    // plane pass below samples the MASTER 256-TLUT, and it runs in THIS rspq
    // stream BEFORE I_FinishUpdate's present-blit re-upload -- so without a re-
    // upload here it would read the wall sub-palettes and render garbage-coloured
    // planes (the combined-build plane corruption; planes-only never hits this
    // because dl_touched_count==0 leaves the master TLUT untouched). Re-load the
    // master TLUT into TMEM NOW, only when walls actually drew. The caller's world
    // textured mode (TLUT_RGBA16) keeps the upper TMEM half addressable for the
    // LOAD_TLUT. I_N64ForceTLUTReupload above still arms the present-blit path for
    // the CI8 sprites/HUD/COPY blit; this is the in-flush synchronous counterpart.
    if (dl_touched_count > 0)
        I_N64UploadMasterTLUT();

    // GPU port Phase 3: draw the baked floor leaf fans here -- WHILE the Z-buffer is
    // still enabled, so the walls just drawn occlude the floors correctly. Same world
    // textured mode (TEX0*PRIM, persp on). Self-gates on DL_MeshRouteOn + a z-image.
    DL_DrawMeshLeaves();

    // Walls done -- the planes/spans below draw WITHOUT the Z-buffer.
    if (dl_wall_z)
        rdpq_mode_zbuf(false, false);

    // Stage-4: drain the floors/ceilings after the walls, same rspq stream,
    // still scissored to the view window. Two mutually-exclusive paths (only one
    // queued this frame, per DL_PlanePolyOn): the Stage-4b POLYGON path (trapezoid
    // strips, persp ON) or the legacy per-span path (1px affine rects, persp OFF).
    // A planes-only frame (0 walls) enters DL_Flush via the DL_SpanCount()/
    // DL_PolyCount() OR in the present gate, runs the empty wall walk above
    // (dl_touched_count==0), and draws its floors here.
    if (dl_ppoly_count > 0)
    {
        DL_FlushPlanePolys();
        (void)dl_ppoly_overflow;
    }
    else
    {
        DL_FlushSpans();
        (void)dl_span_overflow;
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
    dl_ppoly_count = 0;     // Stage-4b: same for the polygon floor path
}

#endif // N64
