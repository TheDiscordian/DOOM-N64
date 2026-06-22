// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//	All the clipping: columns, horizontal spans, sky columns.
//
//-----------------------------------------------------------------------------


static const char
rcsid[] = "$Id: r_segs.c,v 1.3 1997/01/29 20:10:19 b1 Exp $";





#include <stdlib.h>

#include "i_system.h"

#include "doomdef.h"
#include "doomstat.h"

#include "r_local.h"
#include "r_sky.h"

#ifdef N64_BENCH
#include "n64_bench.h"
#endif

#ifdef N64
#include "rdp_view.h"
#include "r_bake.h"

#if defined(DL_DEBUG_TRACE) && DL_DEBUG_TRACE
#include <libdragon.h>      // debugf for the diagnostic seg-claim trace
#endif
#endif


// OPTIMIZE: closed two sided lines as single sided

// True if any of the segs textures might be visible.
boolean		segtextured;	

// False if the back side is the same plane.
boolean		markfloor;	
boolean		markceiling;

boolean		maskedtexture;
int		toptexture;
int		bottomtexture;
int		midtexture;


angle_t		rw_normalangle;
// angle to line origin
int		rw_angle1;	

//
// regular wall
//
int		rw_x;
int		rw_stopx;
angle_t		rw_centerangle;
fixed_t		rw_offset;
fixed_t		rw_distance;
fixed_t		rw_scale;
fixed_t		rw_scalestep;
fixed_t		rw_midtexturemid;
fixed_t		rw_toptexturemid;
fixed_t		rw_bottomtexturemid;

int		worldtop;
int		worldbottom;
int		worldhigh;
int		worldlow;

fixed_t		pixhigh;
fixed_t		pixlow;
fixed_t		pixhighstep;
fixed_t		pixlowstep;

fixed_t		topfrac;
fixed_t		topstep;

fixed_t		bottomfrac;
fixed_t		bottomstep;


lighttable_t**	walllights;

short*		maskedtexturecol;



//
// R_RenderMaskedSegRange
//
void
R_RenderMaskedSegRange
( drawseg_t*	ds,
  int		x1,
  int		x2 )
{
    unsigned	index;
    column_t*	col;
    int		lightnum;
    int		texnum;
    
    // Calculate light table.
    // Use different light tables
    //   for horizontal / vertical / diagonal. Diagonal?
    // OPTIMIZE: get rid of LIGHTSEGSHIFT globally
    curline = ds->curline;
    frontsector = curline->frontsector;
    backsector = curline->backsector;
    texnum = texturetranslation[curline->sidedef->midtexture];
	
    lightnum = (frontsector->lightlevel >> LIGHTSEGSHIFT)+extralight;

    if (curline->v1->y == curline->v2->y)
	lightnum--;
    else if (curline->v1->x == curline->v2->x)
	lightnum++;

    if (lightnum < 0)		
	walllights = scalelight[0];
    else if (lightnum >= LIGHTLEVELS)
	walllights = scalelight[LIGHTLEVELS-1];
    else
	walllights = scalelight[lightnum];

    maskedtexturecol = ds->maskedtexturecol;

    rw_scalestep = ds->scalestep;		
    spryscale = ds->scale1 + (x1 - ds->x1)*rw_scalestep;
    mfloorclip = ds->sprbottomclip;
    mceilingclip = ds->sprtopclip;
    
    // find positioning
    if (curline->linedef->flags & ML_DONTPEGBOTTOM)
    {
	dc_texturemid = frontsector->floorheight > backsector->floorheight
	    ? frontsector->floorheight : backsector->floorheight;
	dc_texturemid = dc_texturemid + textureheight[texnum] - viewz;
    }
    else
    {
	dc_texturemid =frontsector->ceilingheight<backsector->ceilingheight
	    ? frontsector->ceilingheight : backsector->ceilingheight;
	dc_texturemid = dc_texturemid - viewz;
    }
    dc_texturemid += curline->sidedef->rowoffset;
			
    if (fixedcolormap)
	dc_colormap = fixedcolormap;

    // texnum is constant across the column loop -- resolve its column tables
    // once instead of paying R_GetColumn's per-tex work every column.
    const texcol_t	tc_masked = R_GetColumnTex(texnum);

    // draw the columns
    for (dc_x = x1 ; dc_x <= x2 ; dc_x++)
    {
	// calculate lighting
	if (maskedtexturecol[dc_x] != MAXSHORT)
	{
	    if (!fixedcolormap)
	    {
		index = spryscale>>LIGHTSCALESHIFT;

		if (index >=  MAXLIGHTSCALE )
		    index = MAXLIGHTSCALE-1;

		dc_colormap = walllights[index];
	    }
			
	    sprtopscreen = centeryfrac - FixedMul(dc_texturemid, spryscale);
	    dc_iscale = 0xffffffffu / (unsigned)spryscale;
	    
	    // draw the texture
	    col = (column_t *)(
		(byte *)R_GetColumnIn(&tc_masked,maskedtexturecol[dc_x]) -3);
			
	    R_DrawMaskedColumn (col);
	    maskedtexturecol[dc_x] = MAXSHORT;
	}
	spryscale += rw_scalestep;
    }
	
}




//
// R_RenderSegLoop
// Draws zero, one, or two textures (and possibly a masked
//  texture) for walls.
// Can draw or mark the starting pixel of floor and ceiling
//  textures.
// CALLED: CORE LOOPING ROUTINE.
//
#define HEIGHTBITS		12
#define HEIGHTUNIT		(1<<HEIGHTBITS)

void R_RenderSegLoop (void)
{
    angle_t		angle;
    unsigned		index;
    int			yl;
    int			yh;
    int			mid;
	fixed_t		texturecolumn = 0;
    int			top;
    int			bottom;

    // Hoist loop invariants into locals. The wall tiers write visplane
    // top[]/bottom[], which are byte arrays: a store through a char/byte
    // pointer aliases every object under GCC's TBAA, forcing a reload of
    // each cached global on every column. R_GetColumn/colfunc calls clobber
    // globals likewise. Copy the read-only state to locals (whose address is
    // never taken, so the byte stores and calls cannot touch them) and write
    // back the few mutated accumulators after the loop.
    const int		l_rw_stopx = rw_stopx;
    const int		l_markceiling = markceiling;
    const int		l_markfloor = markfloor;
    const int		l_midtexture = midtexture;
    const int		l_toptexture = toptexture;
    const int		l_bottomtexture = bottomtexture;
    // Each tier's texture is constant across the whole column loop, so resolve
    // its column-lookup tables ONCE here (texturecolumnlump[tex] /
    // texturecolumnofs[tex] / texturewidthmask[tex]) instead of paying
    // R_GetColumn's per-tex work -- the call, the two scattered Z_Malloc array
    // indexes, and the width-mask lookup -- on every column. Only active tiers
    // are resolved (inactive tex is 0 and never sampled), matching the original
    // R_GetColumn call guard exactly.
    texcol_t		tc_mid, tc_top, tc_bot;
    if (l_midtexture)	tc_mid = R_GetColumnTex(l_midtexture);
    if (l_toptexture)	tc_top = R_GetColumnTex(l_toptexture);
    if (l_bottomtexture) tc_bot = R_GetColumnTex(l_bottomtexture);
    const int		l_maskedtexture = maskedtexture;
    const int		l_segtextured = segtextured;
    const int		l_viewheight = viewheight;
    visplane_t* const	l_ceilingplane = ceilingplane;
    visplane_t* const	l_floorplane = floorplane;
    short* const	l_ceilingclip = ceilingclip;
    short* const	l_floorclip = floorclip;
    lighttable_t** const l_walllights = walllights;
    short* const	l_maskedtexturecol = maskedtexturecol;
    const angle_t	l_rw_centerangle = rw_centerangle;
    const fixed_t	l_rw_offset = rw_offset;
    const fixed_t	l_rw_distance = rw_distance;
    const fixed_t	l_rw_midtexturemid = rw_midtexturemid;
    const fixed_t	l_rw_toptexturemid = rw_toptexturemid;
    const fixed_t	l_rw_bottomtexturemid = rw_bottomtexturemid;
    const fixed_t	l_rw_scalestep = rw_scalestep;
    const fixed_t	l_topstep = topstep;
    const fixed_t	l_bottomstep = bottomstep;
    const fixed_t	l_pixhighstep = pixhighstep;
    const fixed_t	l_pixlowstep = pixlowstep;
    // During wall rendering colfunc always holds basecolfunc (the base column
    // drawer); the fuzz/translated variants are only swapped in for sprites.
    // Cache it so each column's call doesn't reload the global.
    void (* const l_colfunc)(void) = colfunc;

    int			l_rw_x = rw_x;
    fixed_t		l_rw_scale = rw_scale;
    fixed_t		l_topfrac = topfrac;
    fixed_t		l_bottomfrac = bottomfrac;
    fixed_t		l_pixhigh = pixhigh;
    fixed_t		l_pixlow = pixlow;

#ifdef N64
    // RDP renderer (Stage 3): route ALL solid wall tiers through the RDP wall
    // path -- every single-sided (midtexture) seg AND the upper/lower textures
    // of two-sided segs. For a routed tier the per-column colfunc pixel write is
    // suppressed (those view columns keep the transparency-key index, and the
    // RDP fill drawn at the present seam shows through), while ALL clip /
    // visplane bookkeeping stays on the CPU exactly as the software path does.
    // Per-column screen Y / scale / texturecolumn / light are captured per tier
    // and turned into rdp_wall_t records (per light-level run) after the loop.
    // Masked mid-textures stay CPU (Stage 5); sky is a visplane drawn by
    // R_DrawPlanes (Stage 4), so the seg loop's wall tiers never carry sky.
    int			rdp_route = 0;
    // GPU port: mesh_route suppresses the CPU wall fill WITHOUT the per-column RDP
    // capture -- the static world mesh (DL_MeshDrawWalls, after the BSP walk) draws
    // these walls on the RDP through the Z-buffer instead. Suppressed columns keep
    // the key index (I_N64KeyClearView) so the mesh shows through the keyed present;
    // ALL clip/visplane bookkeeping stays on the CPU exactly as the software path,
    // since floors + sprites still consume it. Mutually exclusive with rdp_route.
    int			mesh_route = 0;

    // Kill-switch FIRST, inline, before any cross-TU call. With the flag OFF
    // this short-circuits to the pre-RDP software seg loop with ZERO added
    // function calls (DL_WallRouteOn is never reached), so the flag-OFF hot path
    // is byte-identical to the Stage-1 baseline. (Gate criterion 2: flag-OFF
    // byte-identical to baseline.) ALL routed capture state + the post-loop emit
    // live in rdp_view.c so this hot translation unit's .text stays at the
    // pre-RDP size when the flag is off (structural flag-OFF drift minimised).
    if (n64_use_rdp_renderer && DL_WallRouteOn())
    {
	rdp_route = 1;
	DL_RouteBeginSeg();
    }
    else if (n64_use_rdp_renderer && DL_MeshRouteOn()
	     && bake_line_meshed
	     && bake_line_meshed[(int)(curline->linedef - lines)])
    {
	// Suppress the CPU fill ONLY for lines actually baked into the static mesh.
	// Door/lift/mover lines are excluded from the bake (P_BakeWorldMesh), so
	// bake_line_meshed is 0 for them and they keep rendering through the
	// software path here -- correct live texture, no mesh ghost/black.
	mesh_route = 1;     // mesh draws these walls; suppress the CPU fill (no capture)
    }
#if defined(DL_DEBUG_TRACE) && DL_DEBUG_TRACE
    // Diagnostic builds only (DL_TRACE=1): log every seg-loop invocation with
    // its tier textures, column range, and whether it routed.
    if (n64_use_rdp_renderer)
	debugf("DL_SEG mid=%d top=%d bot=%d x=%d..%d route=%d\n",
	       l_midtexture, l_toptexture, l_bottomtexture,
	       l_rw_x, l_rw_stopx - 1, rdp_route);
#endif
#endif

#ifdef N64_BENCH
    // SEG_RASTER attributes the per-column wall fill below (the rasterization
    // the RDP renderer offloads) separately from the BSP walk/clip/scale math
    // that surrounds it. Bracketed once per seg around the whole column loop
    // (not per column) so the two CP0 reads stay negligible. The enclosing
    // phase is BSP_WALK; switch to SEG_RASTER and back so it nests cleanly.
    N64Bench_PhaseSwitch(BPH_BSP_WALK, BPH_SEG_RASTER);
#endif

    for ( ; l_rw_x < l_rw_stopx ; l_rw_x++)
    {
	int		cc = l_ceilingclip[l_rw_x];
	int		fc = l_floorclip[l_rw_x];

	// mark floor / ceiling areas
	yl = (l_topfrac+HEIGHTUNIT-1)>>HEIGHTBITS;

	// no space above wall?
	if (yl < cc+1)
	    yl = cc+1;

	if (l_markceiling)
	{
	    top = cc+1;
	    bottom = yl-1;

	    if (bottom >= fc)
		bottom = fc-1;

	    if (top <= bottom)
	    {
		l_ceilingplane->top[l_rw_x] = top;
		l_ceilingplane->bottom[l_rw_x] = bottom;
	    }
	}

	yh = l_bottomfrac>>HEIGHTBITS;

	if (yh >= fc)
	    yh = fc-1;

	if (l_markfloor)
	{
	    top = yh+1;
	    bottom = fc-1;
	    if (top <= cc)
		top = cc+1;
	    if (top <= bottom)
	    {
		l_floorplane->top[l_rw_x] = top;
		l_floorplane->bottom[l_rw_x] = bottom;
	    }
	}

	// texturecolumn and lighting are independent of wall tiers
	if (l_segtextured)
	{
	    // calculate texture offset
	    angle = (l_rw_centerangle + xtoviewangle[l_rw_x])>>ANGLETOFINESHIFT;
	    texturecolumn = l_rw_offset-FixedMul(finetangent[angle],l_rw_distance);
	    texturecolumn >>= FRACBITS;
#ifdef N64
	    // RDP-routed seg: every tier's colfunc is suppressed, so the
	    // colfunc-only inputs are never read -- skip them, most notably
	    // the dc_iscale division (~70 VR4300 cycles per column).
	    // DL_RouteCapture derives the light level from the captured scale
	    // itself; the masked path (R_RenderMaskedSegRange) computes its own
	    // iscale/colormap later from maskedtexturecol. The zero-pixel
	    // colfunc calls a routed mid tier can still make (yl > yh) return
	    // at their count check before reading any of these. mesh_route also
	    // suppresses the fill, so its columns need none of this either.
	    if (!rdp_route && !mesh_route)
#endif
	    {
	    // calculate lighting
	    index = l_rw_scale>>LIGHTSCALESHIFT;

	    if (index >=  MAXLIGHTSCALE )
		index = MAXLIGHTSCALE-1;

	    dc_colormap = l_walllights[index];
	    dc_x = l_rw_x;
	    dc_iscale = 0xffffffffu / (unsigned)l_rw_scale;
	    }
	}

	// draw the wall tiers
	if (l_midtexture)
	{
	    // single sided line
	    dc_yl = yl;
	    dc_yh = yh;
	    dc_texturemid = l_rw_midtexturemid;
#ifdef N64
	    // RDP-routed mid tier: suppress the CPU pixel write ONLY for columns
	    // the RDP will actually fill (yl <= yh). Those columns keep the key
	    // index (the batched I_N64KeyClearView wrote it before any drawer
	    // ran -- Stage-3 replacement for the per-column R_FillColumnKey)
	    // and the RDP quad shows through via the full-view keyed box.
	    // Columns with yl > yh fall through to the normal path: l_colfunc
	    // draws zero pixels there (yl>yh), byte-identical to vanilla. KEEP
	    // the ceiling/floor clip writes below as the CPU path does.
	    if (rdp_route && yl <= yh)
	    {
		DL_RouteCapture(DL_TIER_MID, l_rw_x, yl, yh, l_rw_scale,
				texturecolumn, (const void* const*)l_walllights);
	    }
	    else if (mesh_route && yl <= yh)
	    {
		// mesh draws this column on the RDP; suppress the CPU fill so the
		// column keeps the key index and the mesh shows through the present.
	    }
	    else
#endif
	    {
	    dc_source = R_GetColumnIn(&tc_mid,texturecolumn);
	    l_colfunc ();
	    }
	    l_ceilingclip[l_rw_x] = l_viewheight;
	    l_floorclip[l_rw_x] = -1;
	}
	else
	{
	    // two sided line
	    if (l_toptexture)
	    {
		// top wall
		mid = l_pixhigh>>HEIGHTBITS;
		l_pixhigh += l_pixhighstep;

		if (mid >= fc)
		    mid = fc-1;

		if (mid >= yl)
		{
		    dc_yl = yl;
		    dc_yh = mid;
		    dc_texturemid = l_rw_toptexturemid;
#ifdef N64
		    // RDP-routed top tier (two-sided upper texture). Suppress the
		    // CPU fill, capture the span (dc_yl<=dc_yh guaranteed by
		    // mid>=yl); the span already holds the key from the batched
		    // view clear (I_N64KeyClearView). KEEP the ceilingclip write
		    // below.
		    if (rdp_route)
		    {
			DL_RouteCapture(DL_TIER_TOP, l_rw_x, yl, mid, l_rw_scale,
					texturecolumn,
					(const void* const*)l_walllights);
		    }
		    else if (mesh_route)
		    {
		        // mesh draws this top tier; suppress the CPU fill.
		    }
		    else
#endif
		    {
		    dc_source = R_GetColumnIn(&tc_top,texturecolumn);
		    l_colfunc ();
		    }
		    l_ceilingclip[l_rw_x] = mid;
		}
		else
		    l_ceilingclip[l_rw_x] = yl-1;
	    }
	    else
	    {
		// no top wall
		if (l_markceiling)
		    l_ceilingclip[l_rw_x] = yl-1;
	    }

	    if (l_bottomtexture)
	    {
		// bottom wall
		mid = (l_pixlow+HEIGHTUNIT-1)>>HEIGHTBITS;
		l_pixlow += l_pixlowstep;

		// no space above wall? Re-read ceilingclip live: the top-wall
		// branch above may have written ceilingclip[rw_x] this same
		// iteration, and the original reads that updated value here.
		{
		    int cc_now = l_ceilingclip[l_rw_x];
		    if (mid <= cc_now)
			mid = cc_now+1;
		}

		if (mid <= yh)
		{
		    dc_yl = mid;
		    dc_yh = yh;
		    dc_texturemid = l_rw_bottomtexturemid;
#ifdef N64
		    // RDP-routed bottom tier (two-sided lower texture). Suppress
		    // the CPU fill, capture the span (dc_yl<=dc_yh guaranteed by
		    // mid<=yh); the span already holds the key from the batched
		    // view clear (I_N64KeyClearView). KEEP the floorclip write
		    // below.
		    if (rdp_route)
		    {
			DL_RouteCapture(DL_TIER_BOT, l_rw_x, mid, yh, l_rw_scale,
					texturecolumn,
					(const void* const*)l_walllights);
		    }
		    else if (mesh_route)
		    {
		        // mesh draws this bottom tier; suppress the CPU fill.
		    }
		    else
#endif
		    {
		    dc_source = R_GetColumnIn(&tc_bot,
					    texturecolumn);
		    l_colfunc ();
		    }
		    l_floorclip[l_rw_x] = mid;
		}
		else
		    l_floorclip[l_rw_x] = yh+1;
	    }
	    else
	    {
		// no bottom wall
		if (l_markfloor)
		    l_floorclip[l_rw_x] = yh+1;
	    }

	    if (l_maskedtexture)
	    {
		// save texturecol
		//  for backdrawing of masked mid texture
		l_maskedtexturecol[l_rw_x] = texturecolumn;
	    }
	}

	l_rw_scale += l_rw_scalestep;
	l_topfrac += l_topstep;
	l_bottomfrac += l_bottomstep;
    }

#ifdef N64_BENCH
    // Close SEG_RASTER, reopen BSP_WALK for the rest of the walk.
    N64Bench_PhaseSwitch(BPH_SEG_RASTER, BPH_BSP_WALK);
#endif

#ifdef N64
    // RDP-routed seg: turn each tier's captured per-column spans into rdp_wall_t
    // records (run-coalescing + the per-run quad math live in rdp_view.c so this
    // hot TU stays at the pre-RDP baseline size when the flag is off). Each
    // DL_RouteEmit is a no-op if that tier drew nothing this seg. A single-sided
    // seg only fed the MID stream; a two-sided seg fed TOP and/or BOT. The tier
    // texturemid + texture select which records map to which texture bucket.
    if (rdp_route)
    {
#ifdef N64_BENCH
	// ACCOUNTING ONLY (zero runtime effect): bracket the wall-emit
	// (run-coalesce + DL_EmitRunPiece deviation scan + DL_EmitWallTier)
	// into the otherwise-unused PLANE_EMIT slot, so the bench reports
	// wall-emit cost SEPARATELY from BSP traversal instead of hiding it
	// inside BSP_WALK. The diagnosis measured this hidden emit at
	// ~1800-2046us; without this split a future round cannot see emit
	// vs traversal. PLANE_EMIT is unused for walls (it was reserved for a
	// future plane-to-RDP move that has not landed), so reusing it here
	// is free and unambiguous.
	N64Bench_PhaseSwitch(BPH_BSP_WALK, BPH_PLANE_EMIT);
#endif
	DL_RouteEmit(DL_TIER_MID, l_rw_midtexturemid,    l_midtexture,    centery);
	DL_RouteEmit(DL_TIER_TOP, l_rw_toptexturemid,    l_toptexture,    centery);
	DL_RouteEmit(DL_TIER_BOT, l_rw_bottomtexturemid, l_bottomtexture, centery);
#ifdef N64_BENCH
	N64Bench_PhaseSwitch(BPH_PLANE_EMIT, BPH_BSP_WALK);
#endif
    }
#endif

    // write back the accumulators the caller / next seg reads
    rw_x = l_rw_x;
    rw_scale = l_rw_scale;
    topfrac = l_topfrac;
    bottomfrac = l_bottomfrac;
    pixhigh = l_pixhigh;
    pixlow = l_pixlow;
}




//
// R_StoreWallRange
// A wall segment will be drawn
//  between start and stop pixels (inclusive).
//
void
R_StoreWallRange
( int	start,
  int	stop )
{
    fixed_t		hyp;
    fixed_t		sineval;
    angle_t		distangle, offsetangle;
    fixed_t		vtop;
    int			lightnum;

    // don't overflow and crash
    if (ds_p == &drawsegs[MAXDRAWSEGS])
	return;		
		
#ifdef RANGECHECK
    if (start >=viewwidth || start > stop)
	I_Error ("Bad R_RenderWallRange: %i to %i", start , stop);
#endif
    
    sidedef = curline->sidedef;
    linedef = curline->linedef;

    // mark the segment as visible for auto map
    linedef->flags |= ML_MAPPED;

    // GPU port: flag this wall visible (single- or two-sided) so DL_MeshDrawWalls
    // emits only occlusion-surviving walls (R_StoreWallRange runs only for visible
    // segs); the back-face cull in the transform picks the right side's step quads.
    if (n64_rdp_mesh)
        R_MeshMarkLine ((int)(linedef - lines));

    // calculate rw_distance for scale calculation
    rw_normalangle = curline->angle + ANG90;
    offsetangle = abs(rw_normalangle-rw_angle1);
    
    if (offsetangle > ANG90)
	offsetangle = ANG90;

    distangle = ANG90 - offsetangle;
    hyp = R_PointToDist (curline->v1->x, curline->v1->y);
    sineval = finesine[distangle>>ANGLETOFINESHIFT];
    rw_distance = FixedMul (hyp, sineval);
		
	
    ds_p->x1 = rw_x = start;
    ds_p->x2 = stop;
    ds_p->curline = curline;
    rw_stopx = stop+1;
    
    // calculate scale at both ends and step
    ds_p->scale1 = rw_scale = 
	R_ScaleFromGlobalAngle (viewangle + xtoviewangle[start]);
    
    if (stop > start )
    {
	ds_p->scale2 = R_ScaleFromGlobalAngle (viewangle + xtoviewangle[stop]);
	ds_p->scalestep = rw_scalestep = 
	    (ds_p->scale2 - rw_scale) / (stop-start);
    }
    else
    {
	// UNUSED: try to fix the stretched line bug
#if 0
	if (rw_distance < FRACUNIT/2)
	{
	    fixed_t		trx,try;
	    fixed_t		gxt,gyt;

	    trx = curline->v1->x - viewx;
	    try = curline->v1->y - viewy;
			
	    gxt = FixedMul(trx,viewcos); 
	    gyt = -FixedMul(try,viewsin); 
	    ds_p->scale1 = FixedDiv(projection, gxt-gyt)<<detailshift;
	}
#endif
	ds_p->scale2 = ds_p->scale1;
    }
    
    // calculate texture boundaries
    //  and decide if floor / ceiling marks are needed
    worldtop = frontsector->ceilingheight - viewz;
    worldbottom = frontsector->floorheight - viewz;
	
    midtexture = toptexture = bottomtexture = maskedtexture = 0;
    ds_p->maskedtexturecol = NULL;
	
    if (!backsector)
    {
	// single sided line
	midtexture = texturetranslation[sidedef->midtexture];
	// a single sided line is terminal, so it must mark ends
	markfloor = markceiling = true;
	if (linedef->flags & ML_DONTPEGBOTTOM)
	{
	    vtop = frontsector->floorheight +
		textureheight[sidedef->midtexture];
	    // bottom of texture at bottom
	    rw_midtexturemid = vtop - viewz;	
	}
	else
	{
	    // top of texture at top
	    rw_midtexturemid = worldtop;
	}
	rw_midtexturemid += sidedef->rowoffset;

	ds_p->silhouette = SIL_BOTH;
	ds_p->sprtopclip = screenheightarray;
	ds_p->sprbottomclip = negonearray;
	ds_p->bsilheight = MAXINT;
	ds_p->tsilheight = MININT;
    }
    else
    {
	// two sided line
	ds_p->sprtopclip = ds_p->sprbottomclip = NULL;
	ds_p->silhouette = 0;
	
	if (frontsector->floorheight > backsector->floorheight)
	{
	    ds_p->silhouette = SIL_BOTTOM;
	    ds_p->bsilheight = frontsector->floorheight;
	}
	else if (backsector->floorheight > viewz)
	{
	    ds_p->silhouette = SIL_BOTTOM;
	    ds_p->bsilheight = MAXINT;
	    // ds_p->sprbottomclip = negonearray;
	}
	
	if (frontsector->ceilingheight < backsector->ceilingheight)
	{
	    ds_p->silhouette |= SIL_TOP;
	    ds_p->tsilheight = frontsector->ceilingheight;
	}
	else if (backsector->ceilingheight < viewz)
	{
	    ds_p->silhouette |= SIL_TOP;
	    ds_p->tsilheight = MININT;
	    // ds_p->sprtopclip = screenheightarray;
	}
		
	if (backsector->ceilingheight <= frontsector->floorheight)
	{
	    ds_p->sprbottomclip = negonearray;
	    ds_p->bsilheight = MAXINT;
	    ds_p->silhouette |= SIL_BOTTOM;
	}
	
	if (backsector->floorheight >= frontsector->ceilingheight)
	{
	    ds_p->sprtopclip = screenheightarray;
	    ds_p->tsilheight = MININT;
	    ds_p->silhouette |= SIL_TOP;
	}
	
	worldhigh = backsector->ceilingheight - viewz;
	worldlow = backsector->floorheight - viewz;
		
	// hack to allow height changes in outdoor areas
	if (frontsector->ceilingpic == skyflatnum 
	    && backsector->ceilingpic == skyflatnum)
	{
	    worldtop = worldhigh;
	}
	
			
	if (worldlow != worldbottom 
	    || backsector->floorpic != frontsector->floorpic
	    || backsector->lightlevel != frontsector->lightlevel)
	{
	    markfloor = true;
	}
	else
	{
	    // same plane on both sides
	    markfloor = false;
	}
	
			
	if (worldhigh != worldtop 
	    || backsector->ceilingpic != frontsector->ceilingpic
	    || backsector->lightlevel != frontsector->lightlevel)
	{
	    markceiling = true;
	}
	else
	{
	    // same plane on both sides
	    markceiling = false;
	}
	
	if (backsector->ceilingheight <= frontsector->floorheight
	    || backsector->floorheight >= frontsector->ceilingheight)
	{
	    // closed door
	    markceiling = markfloor = true;
	}
	

	if (worldhigh < worldtop)
	{
	    // top texture
	    toptexture = texturetranslation[sidedef->toptexture];
	    if (linedef->flags & ML_DONTPEGTOP)
	    {
		// top of texture at top
		rw_toptexturemid = worldtop;
	    }
	    else
	    {
		vtop =
		    backsector->ceilingheight
		    + textureheight[sidedef->toptexture];
		
		// bottom of texture
		rw_toptexturemid = vtop - viewz;	
	    }
	}
	if (worldlow > worldbottom)
	{
	    // bottom texture
	    bottomtexture = texturetranslation[sidedef->bottomtexture];

	    if (linedef->flags & ML_DONTPEGBOTTOM )
	    {
		// bottom of texture at bottom
		// top of texture at top
		rw_bottomtexturemid = worldtop;
	    }
	    else	// top of texture at top
		rw_bottomtexturemid = worldlow;
	}
	rw_toptexturemid += sidedef->rowoffset;
	rw_bottomtexturemid += sidedef->rowoffset;
	
	// allocate space for masked texture tables
	if (sidedef->midtexture)
	{
	    // masked midtexture
	    maskedtexture = true;
	    ds_p->maskedtexturecol = maskedtexturecol = lastopening - rw_x;
	    lastopening += rw_stopx - rw_x;
	}
    }
    
    // calculate rw_offset (only needed for textured lines)
    segtextured = midtexture | toptexture | bottomtexture | maskedtexture;

    if (segtextured)
    {
	offsetangle = rw_normalangle-rw_angle1;
	
	if (offsetangle > ANG180)
	    offsetangle = -offsetangle;

	if (offsetangle > ANG90)
	    offsetangle = ANG90;

	sineval = finesine[offsetangle >>ANGLETOFINESHIFT];
	rw_offset = FixedMul (hyp, sineval);

	if (rw_normalangle-rw_angle1 < ANG180)
	    rw_offset = -rw_offset;

	rw_offset += sidedef->textureoffset + curline->offset;
	rw_centerangle = ANG90 + viewangle - rw_normalangle;
	
	// calculate light table
	//  use different light tables
	//  for horizontal / vertical / diagonal
	// OPTIMIZE: get rid of LIGHTSEGSHIFT globally
	if (!fixedcolormap)
	{
	    lightnum = (frontsector->lightlevel >> LIGHTSEGSHIFT)+extralight;

	    if (curline->v1->y == curline->v2->y)
		lightnum--;
	    else if (curline->v1->x == curline->v2->x)
		lightnum++;

	    if (lightnum < 0)		
		walllights = scalelight[0];
	    else if (lightnum >= LIGHTLEVELS)
		walllights = scalelight[LIGHTLEVELS-1];
	    else
		walllights = scalelight[lightnum];
	}
    }
    
    // if a floor / ceiling plane is on the wrong side
    //  of the view plane, it is definitely invisible
    //  and doesn't need to be marked.
    
  
    if (frontsector->floorheight >= viewz)
    {
	// above view plane
	markfloor = false;
    }
    
    if (frontsector->ceilingheight <= viewz 
	&& frontsector->ceilingpic != skyflatnum)
    {
	// below view plane
	markceiling = false;
    }

    
    // calculate incremental stepping values for texture edges
    worldtop >>= 4;
    worldbottom >>= 4;

    // hoist shared subexpressions out of the four edge calcs below
    {
    const fixed_t	centery4 = centeryfrac>>4;
    const fixed_t	scalestep = rw_scalestep;
    const fixed_t	scale = rw_scale;

    topstep = -FixedMul (scalestep, worldtop);
    topfrac = centery4 - FixedMul (worldtop, scale);

    bottomstep = -FixedMul (scalestep,worldbottom);
    bottomfrac = centery4 - FixedMul (worldbottom, scale);

    if (backsector)
    {
	worldhigh >>= 4;
	worldlow >>= 4;

	if (worldhigh < worldtop)
	{
	    pixhigh = centery4 - FixedMul (worldhigh, scale);
	    pixhighstep = -FixedMul (scalestep,worldhigh);
	}

	if (worldlow > worldbottom)
	{
	    pixlow = centery4 - FixedMul (worldlow, scale);
	    pixlowstep = -FixedMul (scalestep,worldlow);
	}
    }
    }
    
    // render it
    if (markceiling)
	ceilingplane = R_CheckPlane (ceilingplane, rw_x, rw_stopx-1);
    
    if (markfloor)
	floorplane = R_CheckPlane (floorplane, rw_x, rw_stopx-1);

    R_RenderSegLoop ();

    
    // save sprite clipping info
    if ( ((ds_p->silhouette & SIL_TOP) || maskedtexture)
	 && !ds_p->sprtopclip)
    {
	memcpy (lastopening, ceilingclip+start, 2*(rw_stopx-start));
	ds_p->sprtopclip = lastopening - start;
	lastopening += rw_stopx - start;
    }
    
    if ( ((ds_p->silhouette & SIL_BOTTOM) || maskedtexture)
	 && !ds_p->sprbottomclip)
    {
	memcpy (lastopening, floorclip+start, 2*(rw_stopx-start));
	ds_p->sprbottomclip = lastopening - start;
	lastopening += rw_stopx - start;	
    }

    if (maskedtexture && !(ds_p->silhouette&SIL_TOP))
    {
	ds_p->silhouette |= SIL_TOP;
	ds_p->tsilheight = MININT;
    }
    if (maskedtexture && !(ds_p->silhouette&SIL_BOTTOM))
    {
	ds_p->silhouette |= SIL_BOTTOM;
	ds_p->bsilheight = MAXINT;
    }
    ds_p++;
}

