// r_bake.c -- static world-space mesh bake (GPU port). See r_bake.h / Docs/GPU_PORT_PLAN.md.
#include <libdragon.h>          // debugf
#include "doomdef.h"
#include "z_zone.h"
#include "r_state.h"     // lines, sides, sectors, numlines
#include "r_bake.h"
#include <string.h>      // memset (visibility gate)

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
