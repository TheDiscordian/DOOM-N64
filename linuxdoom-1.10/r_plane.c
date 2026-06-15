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
//	Here is a core component: drawing the floors and ceilings,
//	 while maintaining a per column clipping list only.
//	Moreover, the sky areas have to be determined.
//
//-----------------------------------------------------------------------------


static const char
rcsid[] = "$Id: r_plane.c,v 1.4 1997/02/03 16:47:55 b1 Exp $";

#include <stdlib.h>

#include "i_system.h"
#include "z_zone.h"
#include "w_wad.h"

#include "doomdef.h"
#include "doomstat.h"

#include "r_local.h"
#include "r_sky.h"

#ifdef N64
#include "rdp_view.h"   // Stage-4 RDP plane span emit (DL_PlaneRouteOn/DL_EmitSpan)
#endif



planefunction_t		floorfunc;
planefunction_t		ceilingfunc;

//
// opening
//

// Here comes the obnoxious "visplane".
// Limit-removing: the visplane pool is allocated dynamically and grown on
// demand (see R_GrowVisplanes) instead of a fixed MAXVISPLANES array, so
// detailed PWAD maps (e.g. SIGIL) no longer trip "no more visplanes".
// MAXVISPLANES is just the initial capacity / growth quantum.
#define MAXVISPLANES	128
visplane_t*		visplanes;
int			numvisplanes;	// current allocated capacity
visplane_t*		lastvisplane;
visplane_t*		floorplane;
visplane_t*		ceilingplane;

// ?
#define MAXOPENINGS	SCREENWIDTH*64
short			openings[MAXOPENINGS];
short*			lastopening;


//
// Clip values are the solid pixel bounding the range.
//  floorclip starts out SCREENHEIGHT
//  ceilingclip starts out -1
//
short			floorclip[SCREENWIDTH];
short			ceilingclip[SCREENWIDTH];

//
// spanstart holds the start of a plane span
// initialized to 0 at start
//
int			spanstart[SCREENHEIGHT];
int			spanstop[SCREENHEIGHT];

//
// texture mapping
//
lighttable_t**		planezlight;
fixed_t			planeheight;

#ifdef N64
// Stage-4 RDP plane path: the resolved flat lump number for the visplane
// currently being filled (firstflat+flattranslation[picnum]), set in
// R_DrawPlanes alongside ds_source and read by R_MapPlane's emit dispatch so
// the span carries an animation-correct flat key. Sky visplanes never set this
// (they take the colfunc branch and continue before reaching R_MakeSpans).
static int		ds_flatlump;
#endif

fixed_t			yslope[SCREENHEIGHT];
fixed_t			distscale[SCREENWIDTH];
fixed_t			basexscale;
fixed_t			baseyscale;

// Per-scanline plane-mapping cache. Packed as array-of-structs (16 bytes ==
// one VR4300 cache line) so a single y indexes height/distance/xstep/ystep from
// the same line: the R_MapPlane cache-hit path then touches one line instead of
// the four separate lines a struct-of-arrays layout would scatter them across.
typedef struct
{
    fixed_t	height;
    fixed_t	distance;
    fixed_t	xstep;
    fixed_t	ystep;
} planecache_t;

planecache_t		planecache[SCREENHEIGHT];



//
// R_InitPlanes
// Only at game startup.
//
void R_InitPlanes (void)
{
    // Allocate the initial visplane pool. It is grown on demand by
    // R_GrowVisplanes, and persists across levels / browser relaunches
    // (so this only allocates once).
    if (!visplanes)
    {
	numvisplanes = MAXVISPLANES;
	visplanes = (visplane_t*) malloc (numvisplanes * sizeof(*visplanes));
	if (!visplanes)
	    I_Error ("R_InitPlanes: failed to allocate %d visplanes",
		     numvisplanes);
    }
}


//
// R_GrowVisplanes
// Doubles the visplane pool when it is exhausted mid-frame. Because the
// engine holds several live visplane_t* (lastvisplane, floorplane,
// ceilingplane), the array base can move on realloc, so those globals are
// re-based by the same element offset. Local visplane_t* held by callers
// must be re-based by the caller (see R_FindPlane / R_CheckPlane).
//
static void R_GrowVisplanes (void)
{
    int		lastoff;
    int		flooroff;
    int		ceiloff;
    int		newmax;
    visplane_t*	newplanes;

    lastoff  = (int)(lastvisplane - visplanes);
    flooroff = floorplane   ? (int)(floorplane   - visplanes) : -1;
    ceiloff  = ceilingplane ? (int)(ceilingplane - visplanes) : -1;

    newmax = numvisplanes ? numvisplanes * 2 : MAXVISPLANES;
    newplanes = (visplane_t*) realloc (visplanes, newmax * sizeof(*visplanes));
    if (!newplanes)
	I_Error ("R_GrowVisplanes: failed to grow to %d visplanes", newmax);

    visplanes = newplanes;
    numvisplanes = newmax;

    lastvisplane = visplanes + lastoff;
    floorplane   = (flooroff >= 0) ? (visplanes + flooroff) : NULL;
    ceilingplane = (ceiloff  >= 0) ? (visplanes + ceiloff)  : NULL;
}


//
// R_MapPlane
//
// Uses global vars:
//  planeheight
//  ds_source
//  basexscale
//  baseyscale
//  viewx
//  viewy
//
// BASIC PRIMITIVE
//
void
R_MapPlane
( int		y,
  int		x1,
  int		x2 )
{
    angle_t	angle;
    fixed_t	distance;
    fixed_t	length;
    unsigned	index;
	
#ifdef RANGECHECK
    if (x2 < x1
	|| x1<0
	|| x2>=viewwidth
	|| (unsigned)y>viewheight)
    {
	I_Error ("R_MapPlane: %i, %i at %i",x1,x2,y);
    }
#endif

    {
	register planecache_t* pc = &planecache[y];

	if (planeheight != pc->height)
	{
	    pc->height = planeheight;
	    distance = pc->distance = FixedMul (planeheight, yslope[y]);
	    ds_xstep = pc->xstep = FixedMul (distance,basexscale);
	    ds_ystep = pc->ystep = FixedMul (distance,baseyscale);
	}
	else
	{
	    distance = pc->distance;
	    ds_xstep = pc->xstep;
	    ds_ystep = pc->ystep;
	}
    }
	
    length = FixedMul (distance,distscale[x1]);
    angle = (viewangle + xtoviewangle[x1])>>ANGLETOFINESHIFT;
    ds_xfrac = viewx + FixedMul(finecosine[angle], length);
    ds_yfrac = -viewy - FixedMul(finesine[angle], length);

    if (fixedcolormap)
	ds_colormap = fixedcolormap;
    else
    {
	index = distance >> LIGHTZSHIFT;
	
	if (index >= MAXLIGHTZ )
	    index = MAXLIGHTZ-1;

	ds_colormap = planezlight[index];
    }
	
    ds_y = y;
    ds_x1 = x1;
    ds_x2 = x2;

#ifdef N64
    // Stage-4 dispatch: route this span to the RDP (emit one affine textured
    // primitive) when planes route, else run the CPU spanfunc exactly as
    // before. The visplane bookkeeping and all the affine ds_* math above are
    //100% unchanged -- only the leaf fill is replaced. ds_colormap is the
    // resolved per-span light table (planezlight[index] or fixedcolormap),
    // reduced to a PRIM level inside DL_EmitSpan.
    if (DL_PlaneRouteOn())
	DL_EmitSpan(ds_y, ds_x1, ds_x2,
		    ds_xfrac, ds_yfrac, ds_xstep, ds_ystep,
		    ds_flatlump, ds_colormap);
    else
	spanfunc ();
#else
    // high or low detail
    spanfunc ();
#endif
}


//
// R_ClearPlanes
// At begining of frame.
//
void R_ClearPlanes (void)
{
    int		i;
    angle_t	angle;
    
    // opening / clipping determination
    for (i=0 ; i<viewwidth ; i++)
    {
	floorclip[i] = viewheight;
	ceilingclip[i] = -1;
    }

    lastvisplane = visplanes;
    lastopening = openings;

    // texture calculation
    // Reset only the per-scanline height sentinels, exactly as the original
    // memset(cachedheight,...) did. distance/xstep/ystep are intentionally left
    // stale: R_MapPlane recomputes them whenever planeheight != height.
    for (i=0 ; i<SCREENHEIGHT ; i++)
	planecache[i].height = 0;

    // left to right mapping
    angle = (viewangle-ANG90)>>ANGLETOFINESHIFT;
	
    // scale will be unit scale at SCREENWIDTH/2 distance
    basexscale = FixedDiv (finecosine[angle],projection);
    baseyscale = -FixedDiv (finesine[angle],projection);
}




//
// R_FindPlane
//
visplane_t*
R_FindPlane
( fixed_t	height,
  int		picnum,
  int		lightlevel )
{
    visplane_t*	check;
	
    if (picnum == skyflatnum)
    {
	height = 0;			// all skys map together
	lightlevel = 0;
    }
	
    for (check=visplanes; check<lastvisplane; check++)
    {
	if (height == check->height
	    && picnum == check->picnum
	    && lightlevel == check->lightlevel)
	{
	    break;
	}
    }
    
			
    if (check < lastvisplane)
	return check;
		
    if (lastvisplane - visplanes == numvisplanes)
	R_GrowVisplanes ();	// limit-removing: grow instead of I_Error

    check = lastvisplane;	// re-base after a possible array move
    lastvisplane++;

    check->height = height;
    check->picnum = picnum;
    check->lightlevel = lightlevel;
    check->minx = SCREENWIDTH;
    check->maxx = -1;
    
    memset (check->top,0xff,sizeof(check->top));
		
    return check;
}


//
// R_CheckPlane
//
visplane_t*
R_CheckPlane
( visplane_t*	pl,
  int		start,
  int		stop )
{
    int		intrl;
    int		intrh;
    int		unionl;
    int		unionh;
    int		x;
	
    if (start < pl->minx)
    {
	intrl = pl->minx;
	unionl = start;
    }
    else
    {
	unionl = pl->minx;
	intrl = start;
    }
	
    if (stop > pl->maxx)
    {
	intrh = pl->maxx;
	unionh = stop;
    }
    else
    {
	unionh = pl->maxx;
	intrh = stop;
    }

    for (x=intrl ; x<= intrh ; x++)
	if (pl->top[x] != 0xff)
	    break;

    if (x > intrh)
    {
	pl->minx = unionl;
	pl->maxx = unionh;

	// use the same one
	return pl;		
    }
	
    // make a new visplane
    if (lastvisplane - visplanes == numvisplanes)
    {
	int ploff = (int)(pl - visplanes);
	R_GrowVisplanes ();	// limit-removing: grow instead of overflow
	pl = visplanes + ploff;	// re-base caller's plane after a move
    }

    lastvisplane->height = pl->height;
    lastvisplane->picnum = pl->picnum;
    lastvisplane->lightlevel = pl->lightlevel;
    
    pl = lastvisplane++;
    pl->minx = start;
    pl->maxx = stop;

    memset (pl->top,0xff,sizeof(pl->top));
		
    return pl;
}


//
// R_MakeSpans
//
void
R_MakeSpans
( int		x,
  int		t1,
  int		b1,
  int		t2,
  int		b2 )
{
    while (t1 < t2 && t1<=b1)
    {
	R_MapPlane (t1,spanstart[t1],x-1);
	t1++;
    }
    while (b1 > b2 && b1>=t1)
    {
	R_MapPlane (b1,spanstart[b1],x-1);
	b1--;
    }
	
    while (t2 < t1 && t2<=b2)
    {
	spanstart[t2] = x;
	t2++;
    }
    while (b2 > b1 && b2>=t2)
    {
	spanstart[b2] = x;
	b2--;
    }
}



//
// R_DrawPlanes
// At the end of each frame.
//
void R_DrawPlanes (void)
{
    visplane_t*		pl;
    int			light;
    int			x;
    int			stop;
    int			angle;
				
#ifdef RANGECHECK
    if (ds_p - drawsegs > MAXDRAWSEGS)
	I_Error ("R_DrawPlanes: drawsegs overflow (%i)",
		 ds_p - drawsegs);
    
    if (lastvisplane - visplanes > numvisplanes)
	I_Error ("R_DrawPlanes: visplane overflow (%i)",
		 lastvisplane - visplanes);
    
    if (lastopening - openings > MAXOPENINGS)
	I_Error ("R_DrawPlanes: opening overflow (%i)",
		 lastopening - openings);
#endif

    for (pl = visplanes ; pl < lastvisplane ; pl++)
    {
	if (pl->minx > pl->maxx)
	    continue;

	
	// sky flat
	if (pl->picnum == skyflatnum)
	{
	    // Vertical sky scale follows the focal length (1:1 texels at the
	    // fullscreen focal), not the pane width.
	    dc_iscale = FixedDiv ((SCREENWIDTH/2)<<FRACBITS, projectiony<<detailshift);
	    
	    // Sky is allways drawn full bright,
	    //  i.e. colormaps[0] is used.
	    // Because of this hack, sky is not affected
	    //  by INVUL inverse mapping.
	    dc_colormap = colormaps;
	    dc_texturemid = skytexturemid;
	    for (x=pl->minx ; x <= pl->maxx ; x++)
	    {
		dc_yl = pl->top[x];
		dc_yh = pl->bottom[x];

		if (dc_yl <= dc_yh)
		{
		    angle = (viewangle + xtoviewangle[x])>>ANGLETOSKYSHIFT;
		    dc_x = x;
		    dc_source = R_GetColumn(skytexture, angle);
		    colfunc ();
		}
	    }
	    continue;
	}
	
	// regular flat
	ds_source = W_CacheLumpNum(firstflat +
				   flattranslation[pl->picnum],
				   PU_STATIC);
#ifdef N64
	// Stage-4: remember the resolved flat lump for R_MapPlane's RDP emit
	// (animation-correct -- flattranslation advances per tic). The RDP path
	// re-caches the lump bytes itself in DL_FlatBlock at flush time; this
	// CPU cache stays exactly as software needs (spanfunc still reads it when
	// planes are on the CPU, and the Z_ChangeTag below releases it either way).
	ds_flatlump = firstflat + flattranslation[pl->picnum];
#endif

	planeheight = abs(pl->height-viewz);
	light = (pl->lightlevel >> LIGHTSEGSHIFT)+extralight;

	if (light >= LIGHTLEVELS)
	    light = LIGHTLEVELS-1;

	if (light < 0)
	    light = 0;

	planezlight = zlight[light];

	pl->top[pl->maxx+1] = 0xff;
	pl->top[pl->minx-1] = 0xff;
		
	stop = pl->maxx + 1;

	for (x=pl->minx ; x<= stop ; x++)
	{
	    R_MakeSpans(x,pl->top[x-1],
			pl->bottom[x-1],
			pl->top[x],
			pl->bottom[x]);
	}
	
	Z_ChangeTag (ds_source, PU_CACHE);
    }
}


#if defined(N64_BENCH) && defined(PLANETESS_COUNT)
//
// R_CountPlanePolyTris  --  DECISIVE go/no-go measurement for the future
// "visplanes as RDP polygons" feature. COUNT ONLY: no rendering, no UV, no RDP
// emit. It answers one question -- if each frame's visplanes were tessellated
// into TRAPEZOID STRIPS (one quad = 2 triangles per straight run, the run
// breaking wherever the top or bottom edge deviates from a straight
// interpolation by more than the split threshold), how many triangles per
// frame would that be? Mean + p95 over the bench decide viability before any
// real rebuild.
//
// The split predicate is REUSED from the wall path's DL_EmitRunPiece
// (rdp_view.c): the same Y-edge deviation logic and the SAME threshold
// DL_SPLIT_DEVY (~1.25 rows). The S (texture-column) split lives only in the
// textured wall path -- there is no UV here (count-only, pre-feature), so this
// pass applies exactly the geometric Y-edge half of that predicate:
//   * corners extrapolated half a pixel outward (the rasterizer's interpolation
//     then passes through the sampled edge values at the end columns' centers),
//   * top edge = top[x], bottom edge = bottom[x]+1 (mirrors the wall code's
//     t_yh[x]+1.0f -- bottom is the row PAST the last covered row),
//   * a run that interpolates within DL_SPLIT_DEVY of every captured column is
//     ONE trapezoid (2 tris); the first column exceeding it splits the run, and
//     each sub-piece re-verifies itself (recursion's fixed point at width<=2).
//
// Non-convex / disconnected planes: a visplane's top[]/bottom[] carry the 0xff
// sentinel in columns the plane does not cover (R_FindPlane memsets top to
// 0xff; a column is covered iff top[x] != 0xff && top[x] <= bottom[x]). The
// pass walks [minx..maxx] and tessellates each contiguous covered ISLAND
// independently, so a plane split by an occluder counts as the separate
// trapezoid strips it would really decompose into.
//
// DL_SPLIT_DEVY is duplicated here (not #included) to keep r_plane.c free of
// the RDP wall header's heavy dependencies; it is the same 1.25-row coverage
// threshold and is asserted to match by a build-time comment in rdp_view.c.
#define PLANETESS_SPLIT_DEVY  1.25f

// Recursive trapezoid-run counter for one covered island [xa..xb] of a single
// visplane, top[]/bottom[] sampled per column. Returns the number of trapezoid
// runs (each = 2 triangles) the island decomposes into. Direct adaptation of
// DL_EmitRunPiece's Y-deviation scan (rdp_view.c ~:1840), stripped to the
// geometric edges (no S/scale/T -- there is no texture in count-only mode).
static int
R_CountIslandRuns
( const byte*	top,
  const byte*	bottom,
  int		xa,
  int		xb,
  int		depth )
{
    float	ytl, ytr, ybl, ybr;	// corner Y edges (ybot is bottom+1)
    float	width = (float)(xb + 1 - xa);
    int		x;

    // Corner attributes, extrapolated half a pixel outward along each end
    // column's local per-column step -- identical construction to
    // DL_EmitRunPiece. A width-1 island gets the column's own constant edges.
    {
	float tl0 = (float)top[xa],            tr0 = (float)top[xb];
	float bl0 = (float)bottom[xa] + 1.0f,  br0 = (float)bottom[xb] + 1.0f;

	if (xb > xa)
	{
	    tl0 -= 0.5f * ((float)top[xa + 1] - tl0);
	    tr0 += 0.5f * (tr0 - (float)top[xb - 1]);
	    bl0 -= 0.5f * (((float)bottom[xa + 1] + 1.0f) - bl0);
	    br0 += 0.5f * (br0 - ((float)bottom[xb - 1] + 1.0f));
	}
	ytl = tl0;  ytr = tr0;
	ybl = bl0;  ybr = br0;
    }

    // Deviation scan: linear-interp each edge across the run and compare to the
    // captured per-column values; split at the FIRST column whose top or bottom
    // edge deviates beyond PLANETESS_SPLIT_DEVY. Width-1/2 islands are exact by
    // construction (the lerp through two extrapolated corners passes through
    // both column samples), so they skip the scan and terminate the recursion.
    if (xb > xa + 1)
    {
	for (x = xa; x <= xb; x++)
	{
	    float f  = ((float)x + 0.5f - (float)xa) / width;
	    float lt = ytl + (ytr - ytl) * f;
	    float lb = ybl + (ybr - ybl) * f;
	    float dT = lt - (float)top[x];
	    float dB = ((float)bottom[x] + 1.0f) - lb;
	    float aT = (dT < 0.0f) ? -dT : dT;
	    float aB = (dB < 0.0f) ? -dB : dB;

	    if ((aT > PLANETESS_SPLIT_DEVY || aB > PLANETESS_SPLIT_DEVY)
		&& depth < 10)
	    {
		// Split at the first deviating column; keep both halves
		// non-empty so the recursion always shrinks (exactly the
		// midpoint rule DL_EmitRunPiece uses).
		int xm = (x >= xb) ? (xb - 1) : ((x > xa) ? x : xa);
		return R_CountIslandRuns(top, bottom, xa, xm, depth + 1)
		     + R_CountIslandRuns(top, bottom, xm + 1, xb, depth + 1);
	    }
	}
    }

    // One trapezoid run covers this island span.
    return 1;
}

// Walk every visplane in the frame, split each into covered islands over
// [minx..maxx], count trapezoid runs per island, and return total TRIANGLES
// (runs * 2). Pure measurement -- does not touch the renderer, the visplane
// pool, or any latched count, so the geometry fingerprint is unperturbed.
int R_CountPlanePolyTris (void)
{
    visplane_t*	pl;
    int		total_runs = 0;

    for (pl = visplanes ; pl < lastvisplane ; pl++)
    {
	int x;

	if (pl->minx > pl->maxx)
	    continue;

	// Walk [minx..maxx], grouping contiguous covered columns into islands.
	// A column is covered iff its top sentinel is clear AND top <= bottom
	// (an empty/degenerate column reads top == 0xff from the R_FindPlane
	// memset, or a crossed pair from clip interplay -- either way no span).
	x = pl->minx;
	while (x <= pl->maxx)
	{
	    int xa, xb;

	    while (x <= pl->maxx
		   && (pl->top[x] == 0xff || pl->top[x] > pl->bottom[x]))
		x++;
	    if (x > pl->maxx)
		break;

	    xa = x;
	    while (x <= pl->maxx
		   && pl->top[x] != 0xff && pl->top[x] <= pl->bottom[x])
		x++;
	    xb = x - 1;

	    total_runs += R_CountIslandRuns(pl->top, pl->bottom, xa, xb, 0);
	}
    }

    return total_runs * 2;	// 2 triangles per trapezoid run
}
#endif	// N64_BENCH && PLANETESS_COUNT
