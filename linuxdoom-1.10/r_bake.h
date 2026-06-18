// r_bake.h -- static world-space mesh bake (GPU port). See Docs/GPU_PORT_PLAN.md.
//
// Builds a static world-space polygon mesh from the level's geometry lumps ONCE at
// level load (P_SetupLevel), so the per-frame renderer can cull + transform it
// instead of re-deriving screen geometry from the BSP walk every frame. The mesh is
// in ABSOLUTE world map-unit coordinates (NOT viewz-relative -- the per-frame
// transform subtracts the eye). Phase 1: single-sided wall quads only; two-sided
// walls and floor/ceiling leaf fans come in later phases.
#ifndef __R_BAKE_H__
#define __R_BAKE_H__

#include "doomtype.h"
#include "m_fixed.h"

// One baked wall quad. DOOM map coords are on-disk shorts (<<FRACBITS at load), so
// the integer map units fit int16 -- range-validated at bake.
typedef struct
{
    fixed_t x1, y1;     // wall start vertex (linedef v1)
    fixed_t x2, y2;     // wall end vertex   (linedef v2)
    fixed_t zbot;       // bottom edge height (sector floorheight)
    fixed_t ztop;       // top edge height    (sector ceilingheight)
    short   texture;    // wall texture index (>0; 0 = '-' sentinel, not baked)
    short   light;      // owning sector lightlevel (SHADE through the colormap)
} bake_wall_t;

extern bake_wall_t* bake_walls;     // PU_LEVEL; rebuilt each P_SetupLevel
extern int          bake_numwalls;

// Build the static world mesh for the current level. Call AFTER P_GroupLines().
void P_BakeWorldMesh (void);

#endif
