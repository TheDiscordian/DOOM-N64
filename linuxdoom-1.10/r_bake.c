// r_bake.c -- static world-space mesh bake (GPU port). See r_bake.h / Docs/GPU_PORT_PLAN.md.
#include <libdragon.h>          // debugf
#include "doomdef.h"
#include "z_zone.h"
#include "r_state.h"     // lines, sides, sectors, numlines
#include "r_bake.h"

bake_wall_t*    bake_walls   = NULL;
int             bake_numwalls = 0;

//
// P_BakeWorldMesh
// Phase 1: bake the single-sided walls (a one-sided linedef has sidenum[1] == -1)
// into world-space quads. Two-sided walls (top/bottom steps + see-through midtex)
// and floor/ceiling leaf fans come in later phases. The mesh is PU_LEVEL, so it is
// freed and rebuilt by the next P_SetupLevel.
//
void P_BakeWorldMesh (void)
{
    int             i;
    int             count;
    bake_wall_t*    w;

    bake_walls    = NULL;
    bake_numwalls = 0;

    // Pass 1: count the single-sided wall quads we will emit.
    count = 0;
    for (i = 0; i < numlines; i++)
    {
        line_t* ld = &lines[i];
        side_t* sd;
        if (ld->sidenum[1] != -1)       // two-sided -- deferred to a later phase
            continue;
        if (ld->sidenum[0] < 0)
            continue;
        sd = &sides[ld->sidenum[0]];
        if (sd->midtexture <= 0)        // '-' / missing texture: no quad
            continue;
        count++;
    }

    if (count == 0)
        return;

    bake_walls = Z_Malloc (sizeof(bake_wall_t) * count, PU_LEVEL, NULL);

    // Pass 2: fill them, in ABSOLUTE world heights.
    count = 0;
    for (i = 0; i < numlines; i++)
    {
        line_t*   ld = &lines[i];
        side_t*   sd;
        sector_t* sec;
        if (ld->sidenum[1] != -1)
            continue;
        if (ld->sidenum[0] < 0)
            continue;
        sd = &sides[ld->sidenum[0]];
        if (sd->midtexture <= 0)
            continue;
        sec = sd->sector;

        w = &bake_walls[count++];
        w->x1      = ld->v1->x;
        w->y1      = ld->v1->y;
        w->x2      = ld->v2->x;
        w->y2      = ld->v2->y;
        w->zbot    = sec->floorheight;
        w->ztop    = sec->ceilingheight;
        w->texture = sd->midtexture;
        w->light   = sec->lightlevel;
    }
    bake_numwalls = count;

    // int16 map-coord range check: the per-frame transform stores posA as int16, and
    // DOOM map units are on-disk shorts, so the integer part fits -- assert it once
    // so an out-of-range custom WAD is caught at bake, not as silent garbage.
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

    debugf ("P_BakeWorldMesh: baked %d single-sided wall quads (of %d linedefs)\n",
            bake_numwalls, numlines);
}
