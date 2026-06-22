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

//
// bake_quad -- append one wall quad, if it has a texture and a positive height.
// Winding (x1,y1)->(x2,y2) sets the facing; the per-frame transform back-face-culls.
//
static void bake_quad (bake_wall_t* arr, int* n,
                       fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2,
                       fixed_t zbot, fixed_t ztop,
                       int tex, int light, int line)
{
    bake_wall_t* w;
    if (tex <= 0 || ztop <= zbot)       // '-'/missing texture, or no exposed step
        return;
    w = &arr[(*n)++];
    w->x1 = x1; w->y1 = y1; w->x2 = x2; w->y2 = y2;
    w->zbot = zbot; w->ztop = ztop;
    w->texture = (short)tex; w->light = (short)light; w->line = (short)line;
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
    for (k = 0; k < n; k++)
    {
        bake_leaf_verts[bake_numleafverts][0] = (fixed_t)(poly[k].x * 65536.0);
        bake_leaf_verts[bake_numleafverts][1] = (fixed_t)(poly[k].y * 65536.0);
        bake_numleafverts++;
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

    bake_walls    = NULL;
    bake_numwalls = 0;
    bake_linevis  = NULL;
    bake_numlines = 0;
    if (numlines <= 0)
        return;

    // <= 4 quads per linedef (two sides x top/bottom step); single-sided uses 1.
    cap = numlines * 4;
    bake_walls = Z_Malloc (sizeof(bake_wall_t) * cap, PU_LEVEL, NULL);

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

        if (ld->sidenum[1] == -1)
        {
            // Single-sided: one midtexture quad, front floor..ceiling.
            fsec = fs->sector;
            bake_quad (bake_walls, &count, ld->v1->x, ld->v1->y, ld->v2->x, ld->v2->y,
                       fsec->floorheight, fsec->ceilingheight,
                       fs->midtexture, fsec->lightlevel, i);
            continue;
        }

        // Two-sided: top + bottom STEP quads on each side (bounded by the other
        // sector). FRONT side keeps the linedef winding (v1->v2); BACK side reverses
        // it (v2->v1) so it faces the back sector. bake_quad drops a step that is not
        // exposed (ztop <= zbot) or has no texture.
        bs   = &sides[ld->sidenum[1]];
        fsec = fs->sector;
        bsec = bs->sector;

        bake_quad (bake_walls, &count, ld->v1->x, ld->v1->y, ld->v2->x, ld->v2->y,
                   bsec->ceilingheight, fsec->ceilingheight,        // front upper step
                   fs->toptexture, fsec->lightlevel, i);
        bake_quad (bake_walls, &count, ld->v1->x, ld->v1->y, ld->v2->x, ld->v2->y,
                   fsec->floorheight, bsec->floorheight,            // front lower step
                   fs->bottomtexture, fsec->lightlevel, i);
        bake_quad (bake_walls, &count, ld->v2->x, ld->v2->y, ld->v1->x, ld->v1->y,
                   fsec->ceilingheight, bsec->ceilingheight,        // back upper step
                   bs->toptexture, bsec->lightlevel, i);
        bake_quad (bake_walls, &count, ld->v2->x, ld->v2->y, ld->v1->x, ld->v1->y,
                   bsec->floorheight, fsec->floorheight,            // back lower step
                   bs->bottomtexture, bsec->lightlevel, i);
    }
    bake_numwalls = count;

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
