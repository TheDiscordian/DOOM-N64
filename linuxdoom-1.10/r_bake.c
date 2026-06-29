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

bake_cell_t*    bake_cells        = NULL;    // grid-tessellated floor/ceiling cells (PU_LEVEL)
int             bake_numcells     = 0;
fixed_t       (*bake_cell_verts)[2] = NULL;  // shared cell vertex pool (map x,y)
int             bake_numcellverts = 0;

#define BAKE_LEAF_MAXV  48      // clipped-leaf vertex cap (BSP depth + the 4 bound edges)
#define BAKE_MAXDEPTH   64      // BSP recursion guard
// Cell edge in map units. Each cell's texel span is <= BAKE_CELL_SIZE, which MUST stay
// under the rsp_rdpq_tri per-edge s10.5 derivative limit (~1024 texels on S/world-x;
// the observed V/world-y drop threshold sits below ~938) or the engine drops triangles
// and the plane flickers to void. 512 keeps ~2x margin while halving the cell count vs
// 256 (fewer triangles -> lower P95). Multiple of 64 so cell edges fall on flat-period
// boundaries. Tunable: raise toward the limit for fewer triangles, lower for safety.
#define BAKE_CELL_SIZE  512
#define BAKE_CELL_MAXV  64      // a convex leaf (<=48v) clipped to a box gains <=4 verts

typedef struct { double x, y; } bdpt_t;

static int      bake_leafvert_cap = 0;
static int      bake_cellvert_cap = 0;
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

// ===========================================================================
// Floor/ceiling CELL tessellation -- clip each leaf's convex polygon to a fixed
// world-aligned grid (BAKE_CELL_SIZE map units) so every emitted triangle's texel
// span stays under the rsp_rdpq_tri edge-derivative limit. Built OFFLINE (this pass)
// so the runtime no longer needs its per-frame, view-dependent depth-band split.
// ===========================================================================

// Clip convex poly (n verts, map units) to ONE axis-aligned half-plane.
// axis 0 == x, 1 == y. keepLE: keep points whose coord <= bound (1) or >= bound (0).
static int bake_clip_axis (const bdpt_t* in, int n, int axis, double bound,
                           int keepLE, bdpt_t* out)
{
    int i, m = 0;
    for (i = 0; i < n; i++)
    {
        const bdpt_t* A = &in[i];
        const bdpt_t* B = &in[(i + 1) % n];
        double ca = axis ? A->y : A->x;
        double cb = axis ? B->y : B->x;
        double fA = keepLE ? (ca - bound) : (bound - ca);   // <=0 == inside
        double fB = keepLE ? (cb - bound) : (bound - cb);
        int    inA = (fA <= 0.0), inB = (fB <= 0.0);
        if (inA && m < BAKE_CELL_MAXV)
            out[m++] = *A;
        if (inA != inB && m < BAKE_CELL_MAXV)
        {
            double t = fA / (fA - fB);
            out[m].x = A->x + t * (B->x - A->x);
            out[m].y = A->y + t * (B->y - A->y);
            m++;
        }
    }
    return m;
}

// Commit one tessellated cell of leaf lf (subsector ss) into the cell table + pool.
// Two-pass: when bake_cells is NULL this is the COUNT pass (advance counters only);
// when allocated it stores. Both passes run the identical clip/cull, so the counts
// match exactly and the store pass cannot overflow the sized pool.
static void bake_store_cell (int ss, const bake_leaf_t* lf, const bdpt_t* poly, int n)
{
    int k;
    if (n < 3) return;
    if (bake_cells)                                         // STORE pass
    {
        bake_cell_t* c;
        fixed_t      umin = 0x7fffffff, vmin = 0x7fffffff;
        c = &bake_cells[bake_numcells];
        c->firstvert  = bake_numcellverts;
        c->numverts   = (short)n;
        c->subsector  = (short)ss;
        c->sector     = lf->sector;
        c->floorpic   = lf->floorpic;
        c->ceilingpic = lf->ceilingpic;
        for (k = 0; k < n; k++)
        {
            fixed_t fx = (fixed_t)(poly[k].x * 65536.0);
            fixed_t fy = (fixed_t)(poly[k].y * 65536.0);
            fixed_t u  = fx >> FRACBITS, v = fy >> FRACBITS;
            if (u < umin) umin = u;
            if (v < vmin) vmin = v;
            bake_cell_verts[bake_numcellverts][0] = fx;
            bake_cell_verts[bake_numcellverts][1] = fy;
            bake_numcellverts++;
        }
        c->ubias = (int)(umin & ~63);                       // floor to 64-texel flat period
        c->vbias = (int)(vmin & ~63);
    }
    else                                                    // COUNT pass
    {
        bake_numcellverts += n;
    }
    bake_numcells++;
}

// Tight axis-aligned bbox (map units) of a sector, over all its linedef vertices.
// Bounds the real floor footprint: a peripheral BSP leaf's convex region can keep
// edges of the map-bounds quad (overhang past the sector's walls -- pure overdraw the
// runtime hides under nearer geometry + the z-buffer), and tessellating that raw emits
// thousands of dead cells. The sector AABB provably contains every pixel of that
// sector's floor (the floor is the interior of the sector's linedefs), so clipping a
// leaf to it before gridding trims ONLY dead overhang. Returns 0 if the sector has no
// lines (leave the leaf untrimmed).
static int bake_sector_bbox (int sec, double* xlo, double* ylo, double* xhi, double* yhi)
{
    const sector_t* s;
    double lo_x = 1e30, lo_y = 1e30, hi_x = -1e30, hi_y = -1e30;
    int    i;
    if ((unsigned)sec >= (unsigned)numsectors) return 0;
    s = &sectors[sec];
    if (s->linecount <= 0 || !s->lines) return 0;
    for (i = 0; i < s->linecount; i++)
    {
        const line_t* ln = s->lines[i];
        double vx[2], vy[2];
        int    k;
        vx[0] = ln->v1->x / 65536.0; vy[0] = ln->v1->y / 65536.0;
        vx[1] = ln->v2->x / 65536.0; vy[1] = ln->v2->y / 65536.0;
        for (k = 0; k < 2; k++)
        {
            if (vx[k] < lo_x) lo_x = vx[k];
            if (vx[k] > hi_x) hi_x = vx[k];
            if (vy[k] < lo_y) lo_y = vy[k];
            if (vy[k] > hi_y) hi_y = vy[k];
        }
    }
    *xlo = lo_x; *ylo = lo_y; *xhi = hi_x; *yhi = hi_y;
    return 1;
}

// Tessellate one leaf's convex polygon into world-grid cells. Cells are aligned to a
// global grid (floor(x / BAKE_CELL_SIZE)) so neighbouring leaves share cell seams and
// each cell edge falls on a 64-texel flat-period boundary.
static void bake_tessellate_leaf (int ss)
{
    const bake_leaf_t* lf = &bake_leaves[ss];
    bdpt_t src[BAKE_CELL_MAXV], a[BAKE_CELL_MAXV], b[BAKE_CELL_MAXV];
    double xmin = 1e30, xmax = -1e30, ymin = 1e30, ymax = -1e30;
    double sxlo, sylo, sxhi, syhi;
    int    n, i, gx0, gx1, gy0, gy1, gx, gy;
    int    produced = bake_numcells;     // diagnostic: cells this leaf emits

    n = lf->numverts;
    if (n < 3) return;
    if (n > BAKE_CELL_MAXV) n = BAKE_CELL_MAXV;             // leaf maxv 48; guard anyway
    for (i = 0; i < n; i++)
    {
        src[i].x = bake_leaf_verts[lf->firstvert + i][0] / 65536.0;
        src[i].y = bake_leaf_verts[lf->firstvert + i][1] / 65536.0;
    }

    // Trim the leaf to its sector's real footprint FIRST -- kills the map-bounds
    // overhang that peripheral BSP leaves carry. Normal leaves sit inside their sector
    // AABB, so this is a no-op for them; a degenerate clip (<3 verts) leaves src as-is.
    if (bake_sector_bbox (lf->sector, &sxlo, &sylo, &sxhi, &syhi))
    {
        int cn;
        cn = bake_clip_axis (src, n, 0, sxlo, 0, a);                  // x >= sxlo
        cn = (cn >= 3) ? bake_clip_axis (a, cn, 0, sxhi, 1, b) : 0;   // x <= sxhi
        cn = (cn >= 3) ? bake_clip_axis (b, cn, 1, sylo, 0, a) : 0;   // y >= sylo
        cn = (cn >= 3) ? bake_clip_axis (a, cn, 1, syhi, 1, b) : 0;   // y <= syhi
        if (cn >= 3) { memcpy (src, b, (size_t)cn * sizeof(bdpt_t)); n = cn; }
    }

    for (i = 0; i < n; i++)
    {
        double x = src[i].x, y = src[i].y;
        if (x < xmin) xmin = x;
        if (x > xmax) xmax = x;
        if (y < ymin) ymin = y;
        if (y > ymax) ymax = y;
    }
    gx0 = (int)floor(xmin / BAKE_CELL_SIZE);
    gx1 = (int)floor((xmax - 1e-4) / BAKE_CELL_SIZE);
    gy0 = (int)floor(ymin / BAKE_CELL_SIZE);
    gy1 = (int)floor((ymax - 1e-4) / BAKE_CELL_SIZE);
    if (gx1 < gx0) gx1 = gx0;
    if (gy1 < gy0) gy1 = gy0;
    if (gx0 == gx1 && gy0 == gy1)                           // already within one cell
    {
        bake_store_cell (ss, lf, src, n);
        return;
    }
    for (gy = gy0; gy <= gy1; gy++)
    {
        double ylo = (double)gy * BAKE_CELL_SIZE, yhi = ylo + BAKE_CELL_SIZE;
        for (gx = gx0; gx <= gx1; gx++)
        {
            double xlo = (double)gx * BAKE_CELL_SIZE, xhi = xlo + BAKE_CELL_SIZE;
            int m;
            m = bake_clip_axis (src, n, 0, xlo, 0, a);      // x >= xlo
            if (m < 3) continue;
            m = bake_clip_axis (a, m, 0, xhi, 1, b);        // x <= xhi
            if (m < 3) continue;
            m = bake_clip_axis (b, m, 1, ylo, 0, a);        // y >= ylo
            if (m < 3) continue;
            m = bake_clip_axis (a, m, 1, yhi, 1, b);        // y <= yhi
            if (m < 3) continue;
            bake_store_cell (ss, lf, b, m);
        }
    }
    produced = bake_numcells - produced;
    if (bake_cells && produced > 256)                      // STORE pass only; flag the outliers
        debugf ("  leaf ss=%d sec=%d nv=%d bbox=%dx%d units -> %d cells\n",
                ss, lf->sector, n, (int)(xmax - xmin), (int)(ymax - ymin), produced);
}

// Allocate the cell table + vertex pool exactly (sizes from the count pass). PU_LEVEL.
static void bake_cell_cap_alloc (int ncells, int nverts)
{
    int nc = (ncells > 0) ? ncells : 1;
    int nv = (nverts > 0) ? nverts : 1;
    bake_cellvert_cap = nv;
    bake_cells      = Z_Malloc (sizeof(bake_cell_t) * nc, PU_LEVEL, NULL);
    bake_cell_verts = Z_Malloc (sizeof(fixed_t) * 2 * nv, PU_LEVEL, NULL);
}

//
// P_BakeLeafCells -- grid-tessellate every filled leaf into the cell pool. Two passes:
// pass 1 counts cells+verts with the pool NULL, pass 2 stores into the exactly-sized
// PU_LEVEL pool. Leaves bake_leaves / bake_leaf_verts and the whole runtime untouched;
// nothing consumes the cells yet (STEP 1). Reports the baked counts for verification.
//
static void P_BakeLeafCells (void)
{
    int ss, cells_per_leaf_max = 0, maxv = 0;

    bake_cells = NULL; bake_numcells = 0;
    bake_cell_verts = NULL; bake_numcellverts = 0; bake_cellvert_cap = 0;
    if (numsubsectors <= 0 || !bake_leaves)
        return;

    // Pass 1: count (pool NULL).
    for (ss = 0; ss < numsubsectors; ss++)
    {
        int before = bake_numcells;
        if (bake_leaves[ss].numverts < 3) continue;
        bake_tessellate_leaf (ss);
        if (bake_numcells - before > cells_per_leaf_max)
            cells_per_leaf_max = bake_numcells - before;
    }
    if (bake_numcells <= 0)
    {
        debugf ("P_BakeLeafCells: 0 cells (no filled leaves)\n");
        return;
    }

    // Allocate exactly, then re-run to store.
    bake_cell_cap_alloc (bake_numcells, bake_numcellverts);
    {
        int want_cells = bake_numcells, want_verts = bake_numcellverts;
        bake_numcells = 0; bake_numcellverts = 0;
        for (ss = 0; ss < numsubsectors; ss++)
        {
            if (bake_leaves[ss].numverts < 3) continue;
            bake_tessellate_leaf (ss);
        }
        for (ss = 0; ss < bake_numcells; ss++)
            if (bake_cells[ss].numverts > maxv) maxv = bake_cells[ss].numverts;
        debugf ("P_BakeLeafCells: %d cells, %d verts (cellsize=%d), maxv=%d, "
                "max-cells/leaf=%d (want %d/%d)\n",
                bake_numcells, bake_numcellverts, BAKE_CELL_SIZE, maxv,
                cells_per_leaf_max, want_cells, want_verts);
    }
}

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

    // STEP 1: grid-tessellate the leaves into the cell pool. Nothing consumes the cells
    // yet -- the runtime still draws from bake_leaves -- so this cannot regress rendering.
    P_BakeLeafCells ();
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
void R_MeshResetLeafVis (void)
{
    if (bake_leafvis)
        memset (bake_leafvis, 0, numsubsectors);
}

void R_MeshMarkSubsector (int ssidx)
{
    if (bake_leafvis && (unsigned)ssidx < (unsigned)numsubsectors)
        bake_leafvis[ssidx] = 1;
}
