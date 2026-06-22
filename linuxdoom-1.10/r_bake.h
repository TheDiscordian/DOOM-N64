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
    short   line;       // owning linedef index (for the per-frame visibility gate)
} bake_wall_t;

extern bake_wall_t* bake_walls;     // PU_LEVEL; rebuilt each P_SetupLevel
extern int          bake_numwalls;

// One baked floor/ceiling LEAF: the convex polygon of a BSP subsector, in absolute
// map coords. Vanilla nodes have no minisegs, so a subsector's segs cover only its
// WALL edges -- the boundary along a BSP partition line has no seg. So the polygon is
// built by clipping a map-bounds quad against the partition half-planes accumulated
// from the root to the leaf (NOT a seg fan, which gaps along partition edges). Z is
// NOT stored: floor/ceiling heights move at runtime (doors/lifts), so the per-frame
// transform reads sectors[sector].floorheight/ceilingheight live. Verts live in the
// shared bake_leaf_verts pool [firstvert .. firstvert+numverts), CCW or CW per BSP.
typedef struct
{
    int     firstvert;  // index into bake_leaf_verts
    short   numverts;   // convex polygon vertex count (>=3)
    short   sector;     // owning sector index (live floor/ceiling height + light)
    short   floorpic;   // flat lump for the floor   (skyflatnum => skip, stays CPU)
    short   ceilingpic; // flat lump for the ceiling (skyflatnum => skip, stays CPU)
} bake_leaf_t;

extern bake_leaf_t*   bake_leaves;        // PU_LEVEL, one per subsector (sky leaves too)
extern int            bake_numleaves;
extern fixed_t      (*bake_leaf_verts)[2]; // shared convex-polygon vertex pool (map x,y)
extern int            bake_numleafverts;

// Per-linedef visibility, set by the BSP walk (R_StoreWallRange marks a line whose
// seg survives the solidsegs occlusion) and consumed by DL_MeshDrawWalls so only
// occlusion-surviving walls emit. PU_LEVEL, sized numlines. Reset each frame.
extern byte*        bake_linevis;
extern int          bake_numlines;

// Build the static world mesh for the current level. Call AFTER P_GroupLines().
void P_BakeWorldMesh (void);

// Per-frame visibility gate (GPU port). R_MeshResetVis clears the flags (call once
// before the BSP walk); R_MeshMarkLine flags a linedef visible (called from the walk).
void R_MeshResetVis (void);
void R_MeshMarkLine (int lineidx);

#endif
