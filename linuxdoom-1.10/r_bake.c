// r_bake.c -- static world-space mesh bake (GPU port). See r_bake.h / Docs/GPU_PORT_PLAN.md.
#include <libdragon.h>          // debugf
#include "doomdef.h"
#include "doomdata.h"    // NF_SUBSECTOR
#include "z_zone.h"
#include "r_state.h"     // lines, sides, sectors, nodes, subsectors, segs, num*
#include "r_bake.h"
#include <string.h>      // memset (visibility gate)
#include <math.h>        // sqrt (leaf-bake validation probe)

bake_wall_t*    bake_walls    = NULL;
int             bake_numwalls = 0;
byte*           bake_linevis  = NULL;    // per-linedef visibility (PU_LEVEL)
int             bake_numlines = 0;
byte*           bake_line_meshed = NULL; // per-linedef: 1 if baked into the mesh (PU_LEVEL)

//
// bake_quad -- append one wall quad, if it has a texture and a positive height.
// Winding (x1,y1)->(x2,y2) sets the facing; the per-frame transform back-face-culls.
//
// Live height of a sector edge: floorheight (ceil==0) or ceilingheight (ceil==1).
static fixed_t bake_secz (int secidx, int ceil)
{
    sector_t* s = &sectors[secidx];
    return ceil ? s->ceilingheight : s->floorheight;
}

static void bake_quad (bake_wall_t* arr, int* n,
                       fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2,
                       int zbot_sec, int zbot_ceil, int ztop_sec, int ztop_ceil,
                       int tex, int lightsec, int line,
                       int peg_sec, int peg_ceil, int peg_addth,
                       fixed_t rowoffset, fixed_t textureoffset)
{
    bake_wall_t* w;
    // Bake-time heights only decide whether the step is exposed NOW (doors start
    // closed -> their upper step IS exposed at level load, so it bakes; it then
    // SHRINKS at runtime as the door opens via the live resolve in DL_MeshDrawWalls).
    if (tex <= 0 || bake_secz(ztop_sec, ztop_ceil) <= bake_secz(zbot_sec, zbot_ceil))
        return;
    w = &arr[(*n)++];
    w->x1 = x1; w->y1 = y1; w->x2 = x2; w->y2 = y2;
    w->zbot_sec = (int16_t)zbot_sec; w->zbot_ceil = (uint8_t)zbot_ceil;
    w->ztop_sec = (int16_t)ztop_sec; w->ztop_ceil = (uint8_t)ztop_ceil;
    w->texture = (short)tex; w->line = (short)line;
    // Store the light SECTOR index so the emit can read its LIVE lightlevel (flicker/strobe/
    // glow specials mutate sectors[].lightlevel each tic, like the door/lift heights already
    // resolved live). w->light keeps the bake-time value as a static fallback.
    w->lightsec = (int16_t)lightsec;
    w->light    = (short)sectors[lightsec].lightlevel;
    // Texture pegging anchor (resolved live in DL_MeshDrawWalls).
    w->peg_sec = (int16_t)peg_sec; w->peg_ceil = (uint8_t)peg_ceil;
    w->peg_addth = (uint8_t)peg_addth;
    w->rowoffset = rowoffset; w->textureoffset = textureoffset;
    // Edge length in map units (== texels v1->v2), STATIC. Baked once here so the
    // per-frame RSP S/T pack avoids the sqrt. dxf/dyf in map units (float) then back
    // to 16.16. Matches DL_MeshDrawWalls' sLen = sqrtf(dxf*dxf + dyf*dyf).
    {
        double dxf = (double)(x2 - x1) / 65536.0;
        double dyf = (double)(y2 - y1) / 65536.0;
        double len = sqrt(dxf * dxf + dyf * dyf);
        w->slen = (fixed_t)(len * 65536.0);
    }
}

// ===========================================================================
// Floor/ceiling LEAF bake -- one convex polygon per BSP subsector.
//
// Vanilla nodes have no minisegs, so a subsector's segs only cover its WALL edges;
// the boundary that runs along a BSP partition line has NO seg. A seg/centroid fan
// would therefore gap along every partition edge. Instead, build each leaf's true
// convex polygon by descending the BSP tree from the root and Sutherland-Hodgman
// clipping a map-bounds quad by the partition half-plane of each branch taken; the
// polygon that reaches a leaf IS that subsector's convex region.
// ===========================================================================
bake_leaf_t*    bake_leaves       = NULL;
int             bake_numleaves    = 0;
fixed_t       (*bake_leaf_verts)[2] = NULL;
int             bake_numleafverts = 0;
byte*           bake_leafvis      = NULL;    // per-subsector visibility (PU_LEVEL)

#define BAKE_LEAF_MAXV  48      // clipped-leaf vertex cap (BSP depth + the 4 bound edges)
#define BAKE_MAXDEPTH   64      // BSP recursion guard

typedef struct { double x, y; } bdpt_t;

static int      bake_leafvert_cap = 0;
static bdpt_t   bake_polypool[BAKE_MAXDEPTH][BAKE_LEAF_MAXV];  // per-depth scratch (flat stack)

// Clip convex poly (n verts, map units) to ONE node partition half-plane.
// R_PointOnSide: f(P) = (Py-ny)*ndx - (Px-nx)*ndy ; the front side (child 0) is f<0.
static int bake_clip (const bdpt_t* in, int n, const node_t* nd, int keepFront, bdpt_t* out)
{
    double nx  = (double)nd->x  * (1.0 / 65536.0), ny  = (double)nd->y  * (1.0 / 65536.0);
    double ndx = (double)nd->dx * (1.0 / 65536.0), ndy = (double)nd->dy * (1.0 / 65536.0);
    int    i, m = 0;
    for (i = 0; i < n; i++)
    {
        const bdpt_t* A = &in[i];
        const bdpt_t* B = &in[(i + 1) % n];
        double fA = (A->y - ny) * ndx - (A->x - nx) * ndy;
        double fB = (B->y - ny) * ndx - (B->x - nx) * ndy;
        int    inA = keepFront ? (fA <= 0.0) : (fA >= 0.0);
        int    inB = keepFront ? (fB <= 0.0) : (fB >= 0.0);
        if (inA && m < BAKE_LEAF_MAXV)
            out[m++] = *A;
        if (inA != inB && m < BAKE_LEAF_MAXV)
        {
            double t = fA / (fA - fB);
            out[m].x = A->x + t * (B->x - A->x);
            out[m].y = A->y + t * (B->y - A->y);
            m++;
        }
    }
    return m;
}

// Commit the clipped polygon of subsector ss into the leaf table + vert pool.
static void bake_store_leaf (int ss, const bdpt_t* poly, int n)
{
    bake_leaf_t* lf;
    sector_t*    sec;
    int          k;

    if ((unsigned)ss >= (unsigned)numsubsectors) return;
    lf = &bake_leaves[ss];
    if (lf->numverts != 0) return;                  // each leaf is reached once; guard anyway
    if (n < 3 || subsectors[ss].numlines <= 0) return;
    sec = subsectors[ss].sector;
    if (!sec) sec = segs[subsectors[ss].firstline].frontsector;
    if (!sec) return;
    if (bake_numleafverts + n > bake_leafvert_cap)
    {
        debugf ("P_BakeLeafFans: WARN leaf-vert pool full (cap %d) at ss %d\n",
                bake_leafvert_cap, ss);
        return;
    }
    lf->firstvert  = bake_numleafverts;
    lf->numverts   = (short)n;
    lf->sector     = (short)(sec - sectors);
    lf->floorpic   = (short)sec->floorpic;
    lf->ceilingpic = (short)sec->ceilingpic;
    {
        fixed_t umin = 0x7fffffff, vmin = 0x7fffffff;       // STATIC S/T period bias (>>FRACBITS
        for (k = 0; k < n; k++)                             // world texel, min over verts; the
        {                                                   // no-readback RSP emit never sees u/v
            fixed_t fx = (fixed_t)(poly[k].x * 65536.0);    // so it cannot derive umin/vmin live)
            fixed_t fy = (fixed_t)(poly[k].y * 65536.0);
            fixed_t u  = fx >> FRACBITS, v = fy >> FRACBITS;
            if (u < umin) umin = u;
            if (v < vmin) vmin = v;
            bake_leaf_verts[bake_numleafverts][0] = fx;
            bake_leaf_verts[bake_numleafverts][1] = fy;
            bake_numleafverts++;
        }
        lf->ubias = (int)(umin & ~63);                      // floor to 64-period (mask = floor
        lf->vbias = (int)(vmin & ~63);                      // for both signs, two's complement)
    }
}

// Recurse the BSP tree, clipping the running convex polygon by each branch's
// partition. front-clip uses bake_polypool[depth]; after the front subtree returns
// that scratch is free, so the back-clip reuses it (the parent `poly` is untouched).
static void bake_leaf_walk (int bspnum, const bdpt_t* poly, int n, int depth)
{
    const node_t* nd;
    bdpt_t*       buf;
    int           m;

    if (bspnum & NF_SUBSECTOR)
    {
        bake_store_leaf ((bspnum == -1) ? 0 : (bspnum & ~NF_SUBSECTOR), poly, n);
        return;
    }
    if (bspnum < 0 || bspnum >= numnodes || depth >= BAKE_MAXDEPTH) return;
    nd  = &nodes[bspnum];
    buf = bake_polypool[depth];

    m = bake_clip (poly, n, nd, 1, buf);            // FRONT half-space -> child 0
    if (m >= 3) bake_leaf_walk (nd->children[0], buf, m, depth + 1);
    m = bake_clip (poly, n, nd, 0, buf);            // BACK half-space  -> child 1
    if (m >= 3) bake_leaf_walk (nd->children[1], buf, m, depth + 1);
}

// Validation helpers (capture-independent correctness check; map units).
static int bake_leaf_convex_ok (const bake_leaf_t* lf)
{
    int i, n = lf->numverts, base = lf->firstvert, sign = 0;
    for (i = 0; i < n; i++)
    {
        double ax = bake_leaf_verts[base + i][0] / 65536.0;
        double ay = bake_leaf_verts[base + i][1] / 65536.0;
        double bx = bake_leaf_verts[base + (i + 1) % n][0] / 65536.0;
        double by = bake_leaf_verts[base + (i + 1) % n][1] / 65536.0;
        double cx = bake_leaf_verts[base + (i + 2) % n][0] / 65536.0;
        double cy = bake_leaf_verts[base + (i + 2) % n][1] / 65536.0;
        double cr = (bx - ax) * (cy - by) - (by - ay) * (cx - bx);
        if (cr >  0.5) { if (sign < 0) return 0; sign =  1; }
        else if (cr < -0.5) { if (sign > 0) return 0; sign = -1; }
    }
    return 1;
}

// How far (map units) a point lies OUTSIDE a convex leaf -- max perpendicular
// distance past any edge, 0 if inside. Winding sign taken from the polygon area, so
// it works for CW or CCW. Used to tell nodebuilder rounding (a unit or two) from a
// real clip error (segs far outside = wrong-side clip).
static double bake_pt_outside_dist (const bake_leaf_t* lf, double px, double py)
{
    int    i, n = lf->numverts, base = lf->firstvert, ccw;
    double area2 = 0.0, worst = 0.0;
    for (i = 0; i < n; i++)
    {
        double ax = bake_leaf_verts[base + i][0] / 65536.0;
        double ay = bake_leaf_verts[base + i][1] / 65536.0;
        double bx = bake_leaf_verts[base + (i + 1) % n][0] / 65536.0;
        double by = bake_leaf_verts[base + (i + 1) % n][1] / 65536.0;
        area2 += ax * by - bx * ay;
    }
    ccw = (area2 > 0.0) ? 1 : -1;
    for (i = 0; i < n; i++)
    {
        double ax = bake_leaf_verts[base + i][0] / 65536.0;
        double ay = bake_leaf_verts[base + i][1] / 65536.0;
        double bx = bake_leaf_verts[base + (i + 1) % n][0] / 65536.0;
        double by = bake_leaf_verts[base + (i + 1) % n][1] / 65536.0;
        double ex = bx - ax, ey = by - ay;
        double len = sqrt (ex * ex + ey * ey);
        double cr, sdist;
        if (len < 1e-6) continue;
        cr    = ex * (py - ay) - ey * (px - ax);    // >0 == left of edge
        sdist = (cr / len) * ccw;                    // >0 == inside side
        if (-sdist > worst) worst = -sdist;          // outside amount past this edge
    }
    return worst;
}

static void P_BakeWeldedPlanes (void);   // THE GOAL: welded plane mesh (defined below)

//
// P_BakeLeafFans -- build the convex floor/ceiling polygon for every subsector and
// validate it numerically (convexity + every seg endpoint inside-or-on its leaf, which
// proves the clip kept the RIGHT side and the non-seg partition edges closed the leaf).
// Z is not stored: per-frame transform reads live sector heights. PU_LEVEL.
//
static void P_BakeLeafFans (void)
{
    bdpt_t quad[4];
    int    root, ss;
    int    filled = 0, empty = 0, nonconvex = 0, segout = 0, maxv = 0;
    double worstseg = 0.0;

    bake_leaves = NULL; bake_numleaves = 0;
    bake_leaf_verts = NULL; bake_numleafverts = 0; bake_leafvert_cap = 0;
    bake_leafvis = NULL;
    if (numsubsectors <= 0)
        return;

    bake_leaves = Z_Malloc (sizeof(bake_leaf_t) * numsubsectors, PU_LEVEL, NULL);
    memset (bake_leaves, 0, sizeof(bake_leaf_t) * numsubsectors);   // numverts=0 => empty slot
    bake_leafvert_cap = numsubsectors * 16;                          // generous avg; overflow guarded
    bake_leaf_verts   = Z_Malloc (sizeof(fixed_t) * 2 * bake_leafvert_cap, PU_LEVEL, NULL);
    bake_leafvis      = Z_Malloc (numsubsectors, PU_LEVEL, NULL);
    memset (bake_leafvis, 0, numsubsectors);

    // Map-bounds quad (int16 map range) -- the root polygon the partitions carve down.
    quad[0].x = -32768.0; quad[0].y = -32768.0;
    quad[1].x =  32767.0; quad[1].y = -32768.0;
    quad[2].x =  32767.0; quad[2].y =  32767.0;
    quad[3].x = -32768.0; quad[3].y =  32767.0;

    root = (numnodes > 0) ? (numnodes - 1) : -1;    // -1 == single-subsector map
    bake_leaf_walk (root, quad, 4, 0);
    bake_numleaves = numsubsectors;                 // slot count; some may be empty

    for (ss = 0; ss < numsubsectors; ss++)
    {
        const bake_leaf_t* lf  = &bake_leaves[ss];
        const subsector_t* sub = &subsectors[ss];
        int s;
        if (lf->numverts == 0) { empty++; continue; }
        filled++;
        if (lf->numverts > maxv) maxv = lf->numverts;
        if (!bake_leaf_convex_ok (lf)) nonconvex++;
        for (s = 0; s < sub->numlines; s++)
        {
            const seg_t* sg = &segs[sub->firstline + s];
            double d1 = bake_pt_outside_dist (lf, sg->v1->x / 65536.0, sg->v1->y / 65536.0);
            double d2 = bake_pt_outside_dist (lf, sg->v2->x / 65536.0, sg->v2->y / 65536.0);
            double d  = (d1 > d2) ? d1 : d2;
            if (d > worstseg) worstseg = d;
            if (d > 1.0) { segout++; break; }
        }
    }
    debugf ("P_BakeLeafFans: %d subsectors, %d filled, %d empty, maxverts=%d, "
            "nonconvex=%d, seg-outside(>1u)=%d worst=%dunits, vpool %d/%d\n",
            numsubsectors, filled, empty, maxv, nonconvex, segout,
            (int)(worstseg + 0.5), bake_numleafverts, bake_leafvert_cap);

    // THE GOAL (Docs/GPU_PORT_PLAN.md): finish the plane geometry HERE, at level
    // load -- welded, grid-cut, self-checked. Renders nothing yet (Phase-1 pattern).
    P_BakeWeldedPlanes ();
}

// ===========================================================================
// WELDED plane mesh (Docs/GPU_PORT_PLAN.md §THE GOAL). The geometry is FINISHED
// here, at level load. Two defects condemned every previous mesh-plane attempt:
// T-junction cracks (leaf polygons clipped independently never share border
// vertices) and per-frame cutting seams (view-dependent side/near/band clipping
// produces edges that differ between neighbours and between frames). Both are
// killed structurally:
//   1. WELD -- any leaf-pool vertex lying on another polygon's edge is inserted
//      into that edge with its EXACT coordinates, so adjacent polygons share
//      border vertices bit-for-bit.
//   2. GRID CUT -- each welded polygon is cut ONCE on the fixed BAKE_PM_GRID
//      world grid. Intersection arithmetic is direction-canonicalized (the
//      smaller endpoint leads regardless of winding), so the two polygons
//      sharing an edge compute the bit-identical cut vertex, and the cut
//      coordinate on the clip axis is snapped exactly onto the grid line.
// Every piece's texel span is bounded by the grid size via its STATIC 64-aligned
// ubias/vbias. The runtime must NEVER cut these polygons. Self-checks print at
// load: T-junction count MUST be 0 -- do not draw from a pool that fails.
// ===========================================================================
bake_pmpiece_t* bake_pmpieces     = NULL;
int             bake_numpmpieces  = 0;
fixed_t       (*bake_pm_verts)[2] = NULL;
int             bake_numpmverts   = 0;

#define BAKE_PM_GRID      512           /* cell edge, multiple of 64 (flat period) */
#define BAKE_PM_MAXV      96            /* welded-polygon vertex cap */
#define BAKE_PM_WELD_EPS  (1.0 / 16.0)  /* on-edge distance (map units) for a weld */
#define BAKE_PM_CHK_EPS   (1.0 / 256.0) /* post-bake T-junction detector tolerance */

// Sorted-by-x vertex index (weld + T-junction check acceleration): binary-search
// the edge's x window, scan only those candidates. Rebuilt per pool it indexes.
static int* pm_xsort   = NULL;
static int  pm_xsort_n = 0;

static void pm_xsort_build (fixed_t (*pool)[2], int n)
{
    int gap, i, j;
    pm_xsort_n = n;
    for (i = 0; i < n; i++) pm_xsort[i] = i;
    for (gap = n / 2; gap > 0; gap /= 2)            // shell sort: small pools, no libc
        for (i = gap; i < n; i++)
            for (j = i - gap; j >= 0
                 && pool[pm_xsort[j]][0] > pool[pm_xsort[j + gap]][0]; j -= gap)
            {
                int t = pm_xsort[j];
                pm_xsort[j] = pm_xsort[j + gap];
                pm_xsort[j + gap] = t;
            }
}

static int pm_xsort_lower (fixed_t (*pool)[2], fixed_t x0)
{
    int lo = 0, hi = pm_xsort_n;
    while (lo < hi)
    {
        int mid = (lo + hi) >> 1;
        if (pool[pm_xsort[mid]][0] < x0) lo = mid + 1; else hi = mid;
    }
    return lo;
}

// Tight AABB (fixed map units) of a sector over its linedef vertices. The floor's
// real footprint is inside it; a peripheral BSP leaf's polygon can extend far past
// it (up to the +/-32768 map bounds, where partition edges have no segs). That
// overhang is void-side geometry -- trimming to the bbox BEFORE welding drops it
// (and the thousands of junk grid pieces + the 2^31 grid-bound overflow it caused).
// Trim lines live in the void or under the map's outer walls, so a trim cut never
// borders a drawn piece. Returns 0 if the sector has no lines (leave untrimmed).
static int pm_sector_bbox (int sec, fixed_t* xlo, fixed_t* ylo, fixed_t* xhi, fixed_t* yhi)
{
    const sector_t* s;
    fixed_t lx = 0x7FFFFFFF, ly = 0x7FFFFFFF, hx = -0x7FFFFFFF, hy = -0x7FFFFFFF;
    int     i, k;
    if ((unsigned)sec >= (unsigned)numsectors) return 0;
    s = &sectors[sec];
    if (s->linecount <= 0 || !s->lines) return 0;
    for (i = 0; i < s->linecount; i++)
    {
        const line_t* ln = s->lines[i];
        fixed_t vx[2], vy[2];
        vx[0] = ln->v1->x; vy[0] = ln->v1->y;
        vx[1] = ln->v2->x; vy[1] = ln->v2->y;
        for (k = 0; k < 2; k++)
        {
            if (vx[k] < lx) lx = vx[k];
            if (vx[k] > hx) hx = vx[k];
            if (vy[k] < ly) ly = vy[k];
            if (vy[k] > hy) hy = vy[k];
        }
    }
    *xlo = lx; *ylo = ly; *xhi = hx; *yhi = hy;
    return 1;
}

// WELD one edge (A,B) of a leaf: append A, then every foreign pool vertex lying
// strictly inside the segment (within BAKE_PM_WELD_EPS of the line, at least
// WELD_EPS from both endpoints), in order along the edge, using the vertex's EXACT
// pool coordinates. B is appended by the next edge's call. Returns the new count.
static int pm_weld_edge (fixed_t (*pool)[2], int selfstart, int selfend,
                         fixed_t ax, fixed_t ay, fixed_t bx, fixed_t by,
                         fixed_t (*w)[2], int wn, int wcap, int* inserted)
{
    static int    ins_idx[BAKE_PM_MAXV];
    static double ins_t[BAKE_PM_MAXV];
    double  axd = (double)ax, ayd = (double)ay;          // doubles carry FIXED values
    double  exd = (double)bx - axd, eyd = (double)by - ayd;
    double  elen2 = exd * exd + eyd * eyd;
    double  elen  = sqrt (elen2);
    double  weld_fx = BAKE_PM_WELD_EPS * 65536.0;        // eps in fixed units
    fixed_t xlo = (ax < bx ? ax : bx) - (fixed_t)(weld_fx + 1.0);
    fixed_t xhi = (ax > bx ? ax : bx) + (fixed_t)(weld_fx + 1.0);
    fixed_t ylo = (ay < by ? ay : by) - (fixed_t)(weld_fx + 1.0);
    fixed_t yhi = (ay > by ? ay : by) + (fixed_t)(weld_fx + 1.0);
    int     k, q, nins = 0;

    if (wn < wcap) { w[wn][0] = ax; w[wn][1] = ay; wn++; }
    if (elen2 < 1.0)
        return wn;                                        // degenerate edge

    for (k = pm_xsort_lower (pool, xlo); k < pm_xsort_n; k++)
    {
        int     vi = pm_xsort[k];
        fixed_t vx = pool[vi][0], vy = pool[vi][1];
        double  t, ddx, ddy, perp2, along;
        if (vx > xhi) break;
        if (vi >= selfstart && vi < selfend) continue;    // own vertex
        if (vy < ylo || vy > yhi) continue;
        if ((vx == ax && vy == ay) || (vx == bx && vy == by)) continue;  // endpoint
        ddx = (double)vx - axd;
        ddy = (double)vy - ayd;
        t   = (ddx * exd + ddy * eyd) / elen2;
        along = t * elen;                                 // fixed-unit distance from A
        if (along < weld_fx || (elen - along) < weld_fx) continue;   // hugs an endpoint
        {
            double px = axd + t * exd - (double)vx;
            double py = ayd + t * eyd - (double)vy;
            perp2 = px * px + py * py;
        }
        if (perp2 > weld_fx * weld_fx) continue;          // not on the edge
        for (q = 0; q < nins; q++)                        // dedup identical coords
            if (pool[ins_idx[q]][0] == vx && pool[ins_idx[q]][1] == vy) break;
        if (q < nins) continue;
        if (nins < BAKE_PM_MAXV) { ins_idx[nins] = vi; ins_t[nins] = t; nins++; }
    }
    for (k = 1; k < nins; k++)                            // insertion sort by t
    {
        int    ii = ins_idx[k];
        double tt = ins_t[k];
        for (q = k - 1; q >= 0 && ins_t[q] > tt; q--)
        {
            ins_idx[q + 1] = ins_idx[q];
            ins_t[q + 1]   = ins_t[q];
        }
        ins_idx[q + 1] = ii;
        ins_t[q + 1]   = tt;
    }
    for (k = 0; k < nins; k++)
        if (wn < wcap)
        {
            w[wn][0] = pool[ins_idx[k]][0];
            w[wn][1] = pool[ins_idx[k]][1];
            wn++;
            (*inserted)++;
        }
    return wn;
}

// Clip a convex fixed-coord polygon to one axis-aligned half-plane (axis 0 = x,
// 1 = y; keepLE keeps coord <= bound, else >= bound). The intersection is computed
// in double from the CANONICALIZED edge (smaller (x,y) endpoint leads, so both
// windings of a shared edge produce the bit-identical cut vertex), and the clip-axis
// coordinate is snapped exactly onto the grid line.
static int pm_clip_axis (fixed_t (*in)[2], int n, int axis, double bound,
                         int keepLE, fixed_t (*out)[2])
{
    // Clamp before converting: a peripheral leaf keeps the +/-32768 map-bounds
    // corners, so the outermost grid cell's far edge is exactly 2^31 in fixed --
    // unrepresentable, and the VR4300 FPU TRAPS on the out-of-range double->int
    // conversion (this was a boot crash). Both sides of a shared edge clamp to
    // the identical value, so determinism holds.
    double  bc = bound;
    fixed_t bfx;
    int     i, m = 0;
    if (bc >  2147483647.0) bc =  2147483647.0;
    if (bc < -2147483648.0) bc = -2147483648.0;
    bfx = (fixed_t)bc;
    for (i = 0; i < n; i++)
    {
        fixed_t ax = in[i][0],           ay = in[i][1];
        fixed_t bx = in[(i + 1) % n][0], by = in[(i + 1) % n][1];
        fixed_t ca = axis ? ay : ax,     cb = axis ? by : bx;
        int inA = keepLE ? (ca <= bfx) : (ca >= bfx);
        int inB = keepLE ? (cb <= bfx) : (cb >= bfx);
        if (inA && m < BAKE_PM_MAXV + 6) { out[m][0] = ax; out[m][1] = ay; m++; }
        if (inA != inB && m < BAKE_PM_MAXV + 6)
        {
            fixed_t px = ax, py = ay, qx = bx, qy = by;
            double  t, oxd, oyd;
            if (qx < px || (qx == px && qy < py))
            { px = bx; py = by; qx = ax; qy = ay; }       // canonical direction
            {
                double p0 = axis ? (double)py : (double)px;
                double q0 = axis ? (double)qy : (double)qx;
                t = (bc - p0) / (q0 - p0);
            }
            oxd = (double)px + t * ((double)qx - (double)px);
            oyd = (double)py + t * ((double)qy - (double)py);
            if (axis) oyd = bc; else oxd = bc;            // exactly on the clip line
            out[m][0] = (fixed_t)oxd;
            out[m][1] = (fixed_t)oyd;
            m++;
        }
    }
    return m;
}

// Commit one piece. Two-pass: with bake_pmpieces NULL this is the COUNT pass; both
// passes run identical geometry so the counts match exactly. Slivers (area below
// ~1/64 map-unit^2, e.g. on-line keeps along a grid border) are dropped.
static void pm_store_piece (int ss, const bake_leaf_t* lf, fixed_t (*poly)[2], int n)
{
    double area2 = 0.0;
    int    k;
    if (n < 3 || n > BAKE_PM_MAXV) return;
    for (k = 0; k < n; k++)
    {
        int j = (k + 1) % n;
        area2 += (double)poly[k][0] * (double)poly[j][1]
               - (double)poly[j][0] * (double)poly[k][1];
    }
    if (area2 < 0.0) area2 = -area2;                      // 2*area in fixed^2
    if (area2 < 65536.0 * 65536.0 / 32.0) return;         // < 1/64 unit^2 -> sliver
    if (bake_pmpieces)
    {
        bake_pmpiece_t* p = &bake_pmpieces[bake_numpmpieces];
        int umin = 0x7FFFFFFF, vmin = 0x7FFFFFFF;
        p->firstvert  = bake_numpmverts;
        p->numverts   = (short)n;
        p->subsector  = (short)ss;
        p->sector     = lf->sector;
        p->floorpic   = lf->floorpic;
        p->ceilingpic = lf->ceilingpic;
        p->pad        = 0;
        for (k = 0; k < n; k++)
        {
            int u = poly[k][0] >> FRACBITS, v = poly[k][1] >> FRACBITS;
            if (u < umin) umin = u;
            if (v < vmin) vmin = v;
            bake_pm_verts[bake_numpmverts][0] = poly[k][0];
            bake_pm_verts[bake_numpmverts][1] = poly[k][1];
            bake_numpmverts++;
        }
        p->ubias = umin & ~63;
        p->vbias = vmin & ~63;
    }
    else
        bake_numpmverts += n;
    bake_numpmpieces++;
}

static void P_BakeWeldedPlanes (void)
{
    fixed_t (*tpool)[2] = NULL;                           // footprint-trimmed polys (temp)
    fixed_t (*wpool)[2] = NULL;                           // welded polygons (temp)
    int*      tstart    = NULL;
    int*      tcount    = NULL;
    int*      wstart    = NULL;
    int*      wcount    = NULL;
    void*     xsort_raw = NULL;
    int       tcap, tn = 0, wcap, wn = 0, ss, pass, e;
    int       inserted = 0, weld_overflow = 0, maxwv = 0;
    int       want_pieces = 0, want_verts = 0;

    bake_pmpieces = NULL; bake_numpmpieces = 0;
    bake_pm_verts = NULL; bake_numpmverts  = 0;
    if (numsubsectors <= 0 || !bake_leaves || bake_numleafverts <= 0)
        return;

    // ---- temp pools (PU_STATIC, freed at the end) ----
    tcap      = bake_numleafverts + numsubsectors * 8 + 64;
    wcap      = tcap * 4 + 1024;
    tpool     = Z_Malloc (sizeof(fixed_t) * 2 * tcap, PU_STATIC, NULL);
    tstart    = Z_Malloc (sizeof(int) * numsubsectors, PU_STATIC, NULL);
    tcount    = Z_Malloc (sizeof(int) * numsubsectors, PU_STATIC, NULL);
    wpool     = Z_Malloc (sizeof(fixed_t) * 2 * wcap, PU_STATIC, NULL);
    wstart    = Z_Malloc (sizeof(int) * numsubsectors, PU_STATIC, NULL);
    wcount    = Z_Malloc (sizeof(int) * numsubsectors, PU_STATIC, NULL);
    xsort_raw = Z_Malloc (sizeof(int) * (tcap > 4 ? tcap : 4), PU_STATIC, NULL);
    pm_xsort  = (int*)xsort_raw;

    // ---- 0. TRIM each leaf polygon to its sector's real footprint (kill the
    //         map-bounds overhang of peripheral leaves before it reaches the weld) ----
    for (ss = 0; ss < numsubsectors; ss++)
    {
        static fixed_t ta[BAKE_PM_MAXV + 6][2], tb[BAKE_PM_MAXV + 6][2];
        const bake_leaf_t* lf = &bake_leaves[ss];
        fixed_t bxlo, bylo, bxhi, byhi;
        int n = lf->numverts, k, m;
        tstart[ss] = tn; tcount[ss] = 0;
        if (n < 3 || n > BAKE_PM_MAXV) continue;
        for (k = 0; k < n; k++)
        {
            ta[k][0] = bake_leaf_verts[lf->firstvert + k][0];
            ta[k][1] = bake_leaf_verts[lf->firstvert + k][1];
        }
        m = n;
        if (pm_sector_bbox (lf->sector, &bxlo, &bylo, &bxhi, &byhi))
        {
            m = pm_clip_axis (ta, m, 0, (double)bxlo, 0, tb);              // x >= xlo
            m = (m >= 3) ? pm_clip_axis (tb, m, 0, (double)bxhi, 1, ta) : 0; // x <= xhi
            m = (m >= 3) ? pm_clip_axis (ta, m, 1, (double)bylo, 0, tb) : 0; // y >= ylo
            m = (m >= 3) ? pm_clip_axis (tb, m, 1, (double)byhi, 1, ta) : 0; // y <= yhi
        }
        if (m < 3) continue;
        for (k = 0; k < m && tn < tcap; k++)
        {
            tpool[tn][0] = ta[k][0];
            tpool[tn][1] = ta[k][1];
            tn++;
        }
        tcount[ss] = tn - tstart[ss];
    }
    pm_xsort_build (tpool, tn);

    // ---- 1. WELD every trimmed polygon against the whole trimmed vertex pool ----
    for (ss = 0; ss < numsubsectors; ss++)
    {
        int n = tcount[ss], base = wn;
        wstart[ss] = wn; wcount[ss] = 0;
        if (n < 3) continue;
        for (e = 0; e < n; e++)
        {
            fixed_t ax = tpool[tstart[ss] + e][0];
            fixed_t ay = tpool[tstart[ss] + e][1];
            fixed_t bx = tpool[tstart[ss] + (e + 1) % n][0];
            fixed_t by = tpool[tstart[ss] + (e + 1) % n][1];
            wn = pm_weld_edge (tpool, tstart[ss], tstart[ss] + n,
                               ax, ay, bx, by, wpool, wn,
                               (base + BAKE_PM_MAXV < wcap) ? base + BAKE_PM_MAXV : wcap,
                               &inserted);
        }
        wcount[ss] = wn - base;
        if (wcount[ss] > maxwv) maxwv = wcount[ss];
        if (wcount[ss] >= BAKE_PM_MAXV) weld_overflow++;
    }

    // ---- 2. GRID CUT each welded polygon; count then store (exact PU_LEVEL) ----
    for (pass = 0; pass < 2; pass++)
    {
        bake_numpmpieces = 0;
        bake_numpmverts  = 0;
        for (ss = 0; ss < numsubsectors; ss++)
        {
            static fixed_t pa[BAKE_PM_MAXV + 6][2], pb[BAKE_PM_MAXV + 6][2];
            static fixed_t pc[BAKE_PM_MAXV + 6][2];
            const bake_leaf_t* lf = &bake_leaves[ss];
            fixed_t (*src)[2] = &wpool[wstart[ss]];
            int  n = wcount[ss];
            int  k, gx, gy, gx0, gx1, gy0, gy1;
            int  uminu, umaxu, vminu, vmaxu;
            if (n < 3 || n > BAKE_PM_MAXV) continue;
            uminu = vminu = 0x7FFFFFFF; umaxu = vmaxu = -0x7FFFFFFF;
            for (k = 0; k < n; k++)
            {
                int u = src[k][0] >> FRACBITS, v = src[k][1] >> FRACBITS;
                if (u < uminu) uminu = u;
                if (u > umaxu) umaxu = u;
                if (v < vminu) vminu = v;
                if (v > vmaxu) vmaxu = v;
            }
            gx0 = uminu >> 9; gx1 = umaxu >> 9;           // >>9 == floor div 512
            gy0 = vminu >> 9; gy1 = vmaxu >> 9;
            if (gx0 == gx1 && gy0 == gy1)
            {
                pm_store_piece (ss, lf, src, n);          // already inside one cell
                continue;
            }
            for (gy = gy0; gy <= gy1; gy++)
            {
                double ylo = (double)gy * BAKE_PM_GRID * 65536.0;
                double yhi = ylo + BAKE_PM_GRID * 65536.0;
                for (gx = gx0; gx <= gx1; gx++)
                {
                    double xlo = (double)gx * BAKE_PM_GRID * 65536.0;
                    double xhi = xlo + BAKE_PM_GRID * 65536.0;
                    int m;
                    m = pm_clip_axis (src, n, 0, xlo, 0, pa);   // x >= xlo
                    if (m < 3) continue;
                    m = pm_clip_axis (pa, m, 0, xhi, 1, pb);    // x <= xhi
                    if (m < 3) continue;
                    m = pm_clip_axis (pb, m, 1, ylo, 0, pc);    // y >= ylo
                    if (m < 3) continue;
                    m = pm_clip_axis (pc, m, 1, yhi, 1, pa);    // y <= yhi
                    if (m < 3) continue;
                    pm_store_piece (ss, lf, pa, m);
                }
            }
        }
        if (pass == 0)
        {
            want_pieces = bake_numpmpieces;
            want_verts  = bake_numpmverts;
            if (want_pieces <= 0) break;
            bake_pmpieces = Z_Malloc (sizeof(bake_pmpiece_t) * want_pieces, PU_LEVEL, NULL);
            bake_pm_verts = Z_Malloc (sizeof(fixed_t) * 2 * want_verts, PU_LEVEL, NULL);
        }
    }

    // ---- 3. SELF-CHECKS on the final pools (printed every load; TJUNC must be 0) ----
    {
        int tjunc = 0, spanmax = 0, maxpv = 0, pi;
        double chk_fx = BAKE_PM_CHK_EPS * 65536.0;
        if (bake_numpmverts > 0)
        {
            if (bake_numpmverts > tcap)
            {
                Z_Free (xsort_raw);
                xsort_raw = Z_Malloc (sizeof(int) * bake_numpmverts, PU_STATIC, NULL);
                pm_xsort  = (int*)xsort_raw;
            }
            pm_xsort_build (bake_pm_verts, bake_numpmverts);
        }
        for (pi = 0; pi < bake_numpmpieces; pi++)
        {
            const bake_pmpiece_t* p = &bake_pmpieces[pi];
            int k;
            if (p->numverts > maxpv) maxpv = p->numverts;
            for (k = 0; k < p->numverts; k++)
            {
                int span_u = (bake_pm_verts[p->firstvert + k][0] >> FRACBITS) - p->ubias;
                int span_v = (bake_pm_verts[p->firstvert + k][1] >> FRACBITS) - p->vbias;
                if (span_u > spanmax) spanmax = span_u;
                if (span_v > spanmax) spanmax = span_v;
            }
            for (k = 0; k < p->numverts; k++)
            {
                fixed_t ax = bake_pm_verts[p->firstvert + k][0];
                fixed_t ay = bake_pm_verts[p->firstvert + k][1];
                fixed_t bx = bake_pm_verts[p->firstvert + (k + 1) % p->numverts][0];
                fixed_t by = bake_pm_verts[p->firstvert + (k + 1) % p->numverts][1];
                double  axd = (double)ax, ayd = (double)ay;
                double  exd = (double)bx - axd, eyd = (double)by - ayd;
                double  elen2 = exd * exd + eyd * eyd, elen = sqrt (elen2);
                fixed_t xlo = (ax < bx ? ax : bx) - (fixed_t)(chk_fx + 1.0);
                fixed_t xhi = (ax > bx ? ax : bx) + (fixed_t)(chk_fx + 1.0);
                fixed_t ylo = (ay < by ? ay : by) - (fixed_t)(chk_fx + 1.0);
                fixed_t yhi = (ay > by ? ay : by) + (fixed_t)(chk_fx + 1.0);
                int     s;
                if (elen2 < 1.0) continue;
                for (s = pm_xsort_lower (bake_pm_verts, xlo); s < pm_xsort_n; s++)
                {
                    int     vi = pm_xsort[s];
                    fixed_t vx = bake_pm_verts[vi][0], vy = bake_pm_verts[vi][1];
                    double  t, ddx, ddy, px, py, along;
                    if (vx > xhi) break;
                    if (vy < ylo || vy > yhi) continue;
                    if (vi >= p->firstvert && vi < p->firstvert + p->numverts) continue;
                    if ((vx == ax && vy == ay) || (vx == bx && vy == by)) continue;
                    ddx = (double)vx - axd; ddy = (double)vy - ayd;
                    t   = (ddx * exd + ddy * eyd) / elen2;
                    along = t * elen;
                    if (along < chk_fx || (elen - along) < chk_fx) continue;
                    px = axd + t * exd - (double)vx;
                    py = ayd + t * eyd - (double)vy;
                    if (px * px + py * py <= chk_fx * chk_fx) tjunc++;
                }
            }
        }
        debugf ("P_BakeWeldedPlanes: pieces=%d verts=%d (want %d/%d) maxv=%d "
                "weld_ins=%d weld_maxv=%d weld_ovf=%d TJUNC=%d spanmax=%d/%d\n",
                bake_numpmpieces, bake_numpmverts, want_pieces, want_verts, maxpv,
                inserted, maxwv, weld_overflow, tjunc, spanmax,
                BAKE_PM_GRID + 63);
        if (tjunc != 0)
            debugf ("P_BakeWeldedPlanes: *** TJUNC=%d NONZERO -- the weld failed; "
                    "DO NOT draw from this pool ***\n", tjunc);
    }

    Z_Free (xsort_raw);
    Z_Free (wcount);
    Z_Free (wstart);
    Z_Free (wpool);
    Z_Free (tcount);
    Z_Free (tstart);
    Z_Free (tpool);
    pm_xsort = NULL;
    pm_xsort_n = 0;
}

//
// P_BakeWorldMesh
// Bake the level's walls into static world-space quads: single-sided faces get one
// midtexture quad (front floor..ceiling); two-sided lines get up to a top + bottom
// STEP quad on EACH side, bounded by the front/back sector heights (the see-through
// midtexture and the floor/ceiling leaf fans come in later phases). PU_LEVEL --
// freed and rebuilt by the next P_SetupLevel.
//
void P_BakeWorldMesh (void)
{
    int     i, count = 0, cap;
    byte*   bake_sector_movable = NULL;  // local: door/lift sectors, freed at end

    bake_walls    = NULL;
    bake_numwalls = 0;
    bake_linevis  = NULL;
    bake_numlines = 0;
    bake_line_meshed = NULL;
    if (numlines <= 0)
        return;

    // <= 4 quads per linedef (two sides x top/bottom step); single-sided uses 1.
    cap = numlines * 4;
    bake_walls = Z_Malloc (sizeof(bake_wall_t) * cap, PU_LEVEL, NULL);

    // Per-linedef "is this line in the static mesh" flag (r_segs.c reads it every
    // seg loop to decide whether to suppress the CPU wall fill). A line excluded
    // below stays 0, so it keeps rendering through the software path.
    bake_line_meshed = Z_Malloc (numlines, PU_LEVEL, NULL);
    memset (bake_line_meshed, 0, numlines);

    // DOOR / LIFT / MOVER EXCLUSION. Walls whose sector can be displaced by a
    // special must NOT go into the static mesh: the mesh stores world-space quads
    // and (a) when the sector moves the wall ghosts/blacks in the keyed present,
    // (b) a baked-but-skipped wall leaves a black hole. Keep them on the SOFTWARE
    // path, which derives screen geometry live every frame and renders the correct
    // texture with no ghost. Build a per-sector "movable" flag, then skip any wall
    // touching a movable sector.
    //
    // Generous by design (no special-number table): a sector targeted by ANY
    // special line is treated as movable. A manual (tag 0) use-special moves the
    // line's BACK sector (DR doors, local lifts); a tagged special moves every
    // sector carrying that tag (remote doors, switched lifts, raised floors). A
    // false positive (e.g. a light-only trigger) only renders a few extra walls in
    // software -- safe; a MISSED mover would ghost, so err toward exclusion.
    // WALL-Z MODE re-includes doors/movers: with the wall Z-buffer on, the mesh
    // occludes the moving walls correctly via depth and the per-frame colour-clear
    // (ace12f2) kills the ghost, so the movable walls go on the RDP too -- their
    // software fill (seg_rast) comes off the CPU for NO extra Z payment (the Z is
    // already cleared every mesh frame). Door motion otherwise spikes seg_rast: the
    // door's own faces + track sides render software, and an opening door reveals more
    // geometry. dl_wall_z is now on for EVERY mesh build (4c6424d, not just floors), so
    // gate on n64_rdp_mesh -- only the no-Z / non-mesh path still needs the exclusion.
    extern int n64_rdp_mesh_floors;
    extern int n64_rdp_mesh;
    (void)n64_rdp_mesh_floors;
    bake_sector_movable = Z_Malloc (numsectors, PU_STATIC, NULL);
    memset (bake_sector_movable, 0, numsectors);
    if (!n64_rdp_mesh)
    for (i = 0; i < numlines; i++)
    {
        line_t* ld = &lines[i];
        if (!ld->special)
            continue;
        if (ld->tag == 0)
        {
            if (ld->sidenum[1] != -1)
                bake_sector_movable[(int)(sides[ld->sidenum[1]].sector - sectors)] = 1;
        }
        else
        {
            int s;
            for (s = 0; s < numsectors; s++)
                if (sectors[s].tag == ld->tag)
                    bake_sector_movable[s] = 1;
        }
    }

    for (i = 0; i < numlines; i++)
    {
        line_t*   ld = &lines[i];
        side_t*   fs;
        side_t*   bs;
        sector_t* fsec;
        sector_t* bsec;

        if (ld->sidenum[0] < 0)
            continue;
        fs = &sides[ld->sidenum[0]];

        // Skip walls touching a movable (door/lift/mover) sector -- leave them on
        // the software path (bake_line_meshed[i] stays 0 -> r_segs keeps the CPU
        // fill). Catches both the door's own face and the doorway/track sides.
        {
            int excl = bake_sector_movable[(int)(fs->sector - sectors)];
            if (ld->sidenum[1] != -1)
                excl |= bake_sector_movable[(int)(sides[ld->sidenum[1]].sector - sectors)];
            if (excl)
                continue;
        }

        if (ld->sidenum[1] == -1)
        {
            // Single-sided: one midtexture quad, front floor..ceiling.
            fsec = fs->sector;
            {
                int fsi = (int)(fsec - sectors);
                // Pegging (r_segs.c:722-734): DONTPEGBOTTOM anchors texture row 0
                // at front.floor + textureheight (bottom-of-texture-at-bottom);
                // otherwise row 0 at front.ceiling (worldtop). +rowoffset.
                int pegbot = (ld->flags & ML_DONTPEGBOTTOM) != 0;
                int before = count;
                bake_quad (bake_walls, &count, ld->v1->x, ld->v1->y, ld->v2->x, ld->v2->y,
                           fsi, 0, fsi, 1,              // front floor .. front ceiling
                           fs->midtexture, fsi, i,
                           fsi,                          // peg_sec: front sector
                           pegbot ? 0 : 1,               // peg_ceil: floor if DONTPEGBOTTOM else ceiling
                           pegbot ? 1 : 0,               // peg_addth: +textureheight on DONTPEGBOTTOM
                           fs->rowoffset, fs->textureoffset);
                if (count > before)
                    bake_line_meshed[i] = 1;
            }
            continue;
        }

        // Two-sided: top + bottom STEP quads on each side (bounded by the other
        // sector). FRONT side keeps the linedef winding (v1->v2); BACK side reverses
        // it (v2->v1) so it faces the back sector. bake_quad drops a step that is not
        // exposed (ztop <= zbot) or has no texture.
        bs   = &sides[ld->sidenum[1]];
        fsec = fs->sector;
        bsec = bs->sector;
        {
            int fsi = (int)(fsec - sectors), bsi = (int)(bsec - sectors);
            // Pegging flags are PER LINEDEF (same flags for both sides); the
            // anchor SECTOR differs per side (each side's own "worldtop" =
            // ITS frontsector ceiling). Mirrors R_StoreWallRange r_segs.c:
            //   TOP tier (worldhigh<worldtop, lines 830-847): DONTPEGTOP ->
            //     row0 at worldtop (the side's frontsector ceiling = the upper
            //     step's TOP edge); else row0 at backsector.ceil + textureheight
            //     (the upper step's BOTTOM edge + th).
            //   BOT tier (worldlow>worldbottom, lines 849-861): DONTPEGBOTTOM ->
            //     row0 at worldtop (the side's frontsector ceiling -- NOT a quad
            //     edge, the door-track case); else row0 at worldlow (backsector
            //     floor = the lower step's TOP edge).
            // Both tiers then += sidedef->rowoffset (r_segs.c:863-864).
            int pegtop = (ld->flags & ML_DONTPEGTOP)    != 0;
            int pegbot = (ld->flags & ML_DONTPEGBOTTOM) != 0;
            int before = count;

            // FRONT upper: back.ceil .. front.ceil. Side frontsector = fsec.
            bake_quad (bake_walls, &count, ld->v1->x, ld->v1->y, ld->v2->x, ld->v2->y,
                       bsi, 1, fsi, 1,                  // front upper: back.ceil .. front.ceil
                       fs->toptexture, fsi, i,
                       pegtop ? fsi : bsi,              // peg_sec: front.ceil (worldtop) else back.ceil
                       1,                               // peg_ceil: both branches reference a ceiling
                       pegtop ? 0 : 1,                  // peg_addth: +th when NOT DONTPEGTOP
                       fs->rowoffset, fs->textureoffset);
            // FRONT lower: front.floor .. back.floor. Side frontsector = fsec.
            bake_quad (bake_walls, &count, ld->v1->x, ld->v1->y, ld->v2->x, ld->v2->y,
                       fsi, 0, bsi, 0,                  // front lower: front.floor .. back.floor
                       fs->bottomtexture, fsi, i,
                       pegbot ? fsi : bsi,              // peg_sec: DONTPEGBOTTOM->front.ceil(worldtop) else back.floor(worldlow)
                       pegbot ? 1 : 0,                  // peg_ceil: ceiling on DONTPEGBOTTOM else floor
                       0,                               // peg_addth: neither bottom-tier branch adds textureheight
                       fs->rowoffset, fs->textureoffset);
            // BACK upper: front.ceil .. back.ceil. Side frontsector = bsec.
            bake_quad (bake_walls, &count, ld->v2->x, ld->v2->y, ld->v1->x, ld->v1->y,
                       fsi, 1, bsi, 1,                  // back upper: front.ceil .. back.ceil
                       bs->toptexture, bsi, i,
                       pegtop ? bsi : fsi,              // peg_sec: back.ceil (worldtop) else front.ceil
                       1,                               // peg_ceil: ceiling
                       pegtop ? 0 : 1,                  // peg_addth: +th when NOT DONTPEGTOP
                       bs->rowoffset, bs->textureoffset);
            // BACK lower: back.floor .. front.floor. Side frontsector = bsec.
            bake_quad (bake_walls, &count, ld->v2->x, ld->v2->y, ld->v1->x, ld->v1->y,
                       bsi, 0, fsi, 0,                  // back lower: back.floor .. front.floor
                       bs->bottomtexture, bsi, i,
                       pegbot ? bsi : fsi,              // peg_sec: DONTPEGBOTTOM->back.ceil(worldtop) else front.floor(worldlow)
                       pegbot ? 1 : 0,                  // peg_ceil: ceiling on DONTPEGBOTTOM else floor
                       0,                               // peg_addth: neither bottom-tier branch adds textureheight
                       bs->rowoffset, bs->textureoffset);
            if (count > before)
                bake_line_meshed[i] = 1;
        }
    }
    bake_numwalls = count;
    Z_Free (bake_sector_movable);

    // Per-linedef visibility gate: reset each frame, set by the BSP walk
    // (R_StoreWallRange marks occlusion-surviving walls visible).
    bake_numlines = numlines;
    bake_linevis  = Z_Malloc (numlines, PU_LEVEL, NULL);
    memset (bake_linevis, 0, numlines);

    // int16 map-coord range check: the per-frame transform stores posA as int16, and
    // DOOM map units are on-disk shorts, so the integer part fits -- warn once so an
    // out-of-range custom WAD is caught at bake, not as silent garbage.
    for (i = 0; i < bake_numwalls; i++)
    {
        int mx1 = bake_walls[i].x1 >> FRACBITS, my1 = bake_walls[i].y1 >> FRACBITS;
        int mx2 = bake_walls[i].x2 >> FRACBITS, my2 = bake_walls[i].y2 >> FRACBITS;
        if (mx1 < -32768 || mx1 > 32767 || my1 < -32768 || my1 > 32767 ||
            mx2 < -32768 || mx2 > 32767 || my2 < -32768 || my2 > 32767)
        {
            debugf ("P_BakeWorldMesh: WARN wall %d outside int16 map range\n", i);
            break;
        }
    }

    debugf ("P_BakeWorldMesh: baked %d wall quads (of %d linedefs)\n",
            bake_numwalls, numlines);

    // Floor/ceiling convex leaf polygons (Phase 3 foundation; render wiring is next).
    P_BakeLeafFans ();
}

//
// R_MeshResetVis -- clear the per-linedef visibility flags. Call once per frame
// BEFORE the BSP walk (R_RenderPlayerView). No-op until a level is baked.
//
void R_MeshResetVis (void)
{
    if (bake_linevis)
        memset (bake_linevis, 0, bake_numlines);
}

//
// R_MeshMarkLine -- flag a linedef visible. Called from R_StoreWallRange for each
// occlusion-surviving single-sided wall, so DL_MeshDrawWalls emits only those.
//
void R_MeshMarkLine (int lineidx)
{
    if (bake_linevis && (unsigned)lineidx < (unsigned)bake_numlines)
        bake_linevis[lineidx] = 1;
}

//
// R_MeshResetLeafVis / R_MeshMarkSubsector -- per-subsector visibility for the floor/
// ceiling leaf fans, mirroring the wall line-vis. Reset before the BSP walk; R_Subsector
// marks each leaf it reaches so DL_MeshDrawLeaves transforms only visible leaves.
//
int bake_leafvis_count = 0;     // marks this frame (welded-plane render-gate term)

void R_MeshResetLeafVis (void)
{
    if (bake_leafvis)
        memset (bake_leafvis, 0, numsubsectors);
    bake_leafvis_count = 0;
}

void R_MeshMarkSubsector (int ssidx)
{
    if (bake_leafvis && (unsigned)ssidx < (unsigned)numsubsectors)
    {
        if (!bake_leafvis[ssidx])
            bake_leafvis_count++;
        bake_leafvis[ssidx] = 1;
    }
}
