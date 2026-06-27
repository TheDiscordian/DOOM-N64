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

#include <stdint.h>
#include "doomtype.h"
#include "m_fixed.h"

// One baked wall quad. DOOM map coords are on-disk shorts (<<FRACBITS at load), so
// the integer map units fit int16 -- range-validated at bake.
typedef struct
{
    fixed_t x1, y1;     // wall start vertex (linedef v1)
    fixed_t x2, y2;     // wall end vertex   (linedef v2)
    // The top/bottom edge heights are NOT frozen at bake -- they track a sector's live
    // floor/ceiling so doors/lifts/crushers (moving sectors) follow the geometry
    // instead of leaving a ghost. zbot_sec/ztop_sec index sectors[]; zbot_ceil/ztop_ceil
    // pick floorheight (0) or ceilingheight (1) of that sector. Resolved each frame in
    // DL_MeshDrawWalls.
    int16_t zbot_sec, ztop_sec;
    uint8_t zbot_ceil, ztop_ceil;
    short   texture;    // wall texture index (>0; 0 = '-' sentinel, not baked)
    short   light;      // owning sector lightlevel AT BAKE (static fallback; the LIVE value is
                        // sectors[lightsec].lightlevel -- use that so flickering/strobe light
                        // specials are honoured, like software and the per-frame edge heights)
    int16_t lightsec;   // owning sector index for the LIVE lightlevel (flicker/glow/strobe)
    short   line;       // owning linedef index (for the per-frame visibility gate)
    // --- texture pegging / offset (mirror of R_StoreWallRange, r_segs.c) ----
    // texturemid_world (the absolute world height that maps to texture row 0) is
    // NOT frozen at bake -- like the edge heights it is resolved per-frame from a
    // LIVE sector reference, so a moving sector (door/lift) pegs to the live height.
    // texturemid_world = sectors[peg_sec].(ceil|floor) [+ textureheight] + rowoffset.
    // The per-frame transform then derives texel-T at any edge height z as
    // (texturemid_world - z), matching software's dc_texturemid -> column-T chain.
    int16_t peg_sec;    // sector whose live floor/ceiling height anchors texture row 0
    uint8_t peg_ceil;   // 0 = that sector's floorheight, 1 = its ceilingheight
    uint8_t peg_addth;  // 1 = add textureheight[texture] (DONTPEGBOTTOM/non-DONTPEGTOP)
    fixed_t rowoffset;     // sidedef->rowoffset    (16.16 map units -> vertical texel shift)
    fixed_t textureoffset; // sidedef->textureoffset (16.16; S at v1, +1 texel/map-unit to v2)
    fixed_t slen;          // wall length in map units (16.16) = texels v1->v2; STATIC, baked
                           // once (sqrt of the edge) so the per-frame S/T pack needs no sqrt.
                           // S at corner B = textureoffset + slen (pre near-clip).
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
    int     ubias;      // floor(min world-texel x / 64)*64 -- STATIC S period bias (no-readback emit)
    int     vbias;      // floor(min world-texel y / 64)*64 -- STATIC T period bias
} bake_leaf_t;

extern bake_leaf_t*   bake_leaves;        // PU_LEVEL, one per subsector (sky leaves too)
extern int            bake_numleaves;
extern fixed_t      (*bake_leaf_verts)[2]; // shared convex-polygon vertex pool (map x,y)
extern int            bake_numleafverts;

// Per-subsector visibility, set during the BSP walk (R_Subsector marks each leaf it
// reaches) and consumed by DL_MeshDrawLeaves so only visible leaves transform/draw.
// PU_LEVEL, sized numsubsectors. Reset each frame (R_MeshResetLeafVis).
extern byte*          bake_leafvis;

// Per-linedef visibility, set by the BSP walk (R_StoreWallRange marks a line whose
// seg survives the solidsegs occlusion) and consumed by DL_MeshDrawWalls so only
// occlusion-surviving walls emit. PU_LEVEL, sized numlines. Reset each frame.
extern byte*        bake_linevis;
extern int          bake_numlines;

// Per-linedef: 1 if the line was baked into the static mesh, 0 if excluded (a
// door/lift/mover wall kept on the software path). r_segs.c gates the CPU-fill
// suppression on this so excluded lines render normally. PU_LEVEL, sized numlines.
extern byte*        bake_line_meshed;

// Build the static world mesh for the current level. Call AFTER P_GroupLines().
void P_BakeWorldMesh (void);

// Per-frame visibility gate (GPU port). R_MeshResetVis clears the flags (call once
// before the BSP walk); R_MeshMarkLine flags a linedef visible (called from the walk).
void R_MeshResetVis (void);
void R_MeshMarkLine (int lineidx);

// Per-subsector visibility gate for the floor/ceiling leaf fans (Phase 3).
void R_MeshResetLeafVis (void);
void R_MeshMarkSubsector (int ssidx);

#endif
