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

// ---- PLANE_UV_TRACE: debug-gated floor-poly texel self-trace ----------------
// Diagnostic ONLY (no rendering change). When -DPLANE_UV_TRACE=1 (Makefile
// PLANE_UV_TRACE=1; default 0 -> compiled out of normal/timing builds), dump for
// the first few floor trapezoid polys of an early frame:
//   * per corner: screen (x,y), the EMITTED (u,v,invw) R_PlaneCornerAttr made,
//     and the EXPECTED masked texel (s,t) R_MapPlane's per-pixel integer math
//     yields at that corner's screen pixel;
//   * at the poly CENTER pixel: the RDP's RECONSTRUCTED texel (bilinear-interp
//     the corner u*invw,v*invw,invw to centre, then divide) vs R_MapPlane's
//     EXPECTED (s,t) at that same pixel;
//   * the frame constants (planeheight, viewx, viewy, projectiony, centery).
// The orchestrator compares these to localise the garbage-floor bug. Cheap and
// fully compiled out unless the flag is set. Uses debugf -> ISViewer, so it
// pulls libdragon in only under the flag.
#ifndef PLANE_UV_TRACE
#define PLANE_UV_TRACE 0
#endif
#if PLANE_UV_TRACE
#include <math.h>               // floorf (trace float formatting)
#include <libdragon.h>          // debugf -> ISViewer (flag-gated only)
#include "tables.h"             // finecosine/finesine, ANGLETOFINESHIFT
#ifndef PLANE_UV_TRACE_FRAME
#define PLANE_UV_TRACE_FRAME 8  // dump on this R_DrawPlanes call (post-warmup)
#endif
#ifndef PLANE_UV_TRACE_POLYS
#define PLANE_UV_TRACE_POLYS 3  // dump at most this many polys that frame
#endif
extern int framecount;          // r_main.c per-view frame counter (cross-build)
static int plane_uv_trace_polys_left = -1;  // armed each matching frame
// debugf has no float support here, so print floats as int + signed milli-frac.
// IFLOORF: float -> floor as int (toward -inf, matches texel-cell selection).
// MILLIFRAC: the |fractional part| in thousandths (0..999) for "%d.%03d".
#define IFLOORF(f)   ((int)floorf((float)(f)))
#define MILLIFRAC(f) ((int)(( (float)(f) - floorf((float)(f)) ) * 1000.0f))
#endif

// ---- PLANE_GEOM_TRACE: debug-gated floor-poly COVERAGE/geometry trace --------
// Diagnostic ONLY (no rendering change). When -DPLANE_GEOM_TRACE=1 (Makefile
// PLANE_GEOM_TRACE=1; default 0 -> compiled out of normal/timing builds), dump,
// for the first few floor trapezoid runs of an early frame, the numbers that pin
// the COVERAGE/SHAPE bug (NOT the texel bug -- that is PLANE_UV_TRACE's job):
//   1. per emitted run: screen x1,x2 (column span) and the FOUR corner screen
//      (x, ytop, ybot) -- the poly's actual screen coverage;
//   2. the visplane's TRUE coverage at those columns: top[x]/bottom[x] for
//      x = x1, mid, x2 -- the per-column floor extent software would fill, plus
//      the emitted poly's interpolated [ytop,ybot] AT those same columns so the
//      poly-vs-visplane per-column row delta is directly readable;
//   3. consecutive-run boundary: run N's right edge (x2, ytr/ybr at x2+1) vs run
//      N+1's left edge (x1, ytl/ybl at x1) within the SAME island -> OVERLAP /
//      GAP / clean-meet, reported in px;
//   4. the island [minx,maxx] and how the half-pixel run-end extrapolation placed
//      the run-edge corners (raw top[xa]/top[xb] vs extrapolated ytl/ytr...).
// Uses debugf -> ISViewer (libdragon pulled in only under the flag). Mirrors the
// PLANE_UV_TRACE gating exactly: independent flag, default OFF, fully compiled
// out of normal/timing builds. The orchestrator analyses + fixes from the dump.
#ifndef PLANE_GEOM_TRACE
#define PLANE_GEOM_TRACE 0
#endif
#if PLANE_GEOM_TRACE
#include <math.h>               // floorf (trace float formatting)
#include <libdragon.h>          // debugf -> ISViewer (flag-gated only)
#include "n64_bench.h"          // N64Bench_FrameNo (marker-frame pairing)
// Which BENCH marker frame(s) to dump. Keyed to N64Bench_FrameNo() exactly like
// rdp_view.c's PLANE_UV_TRACE: the render whose COMMIT produces marker N sees
// N64Bench_FrameNo() == N-1, so we dump when the counter is FRAME-1 to pair the
// lines with `BENCH_MARK frame=FRAME`. 0 disables a slot.
#ifndef PLANE_GEOM_TRACE_FRAME
#define PLANE_GEOM_TRACE_FRAME 3200 // primary defect frame (red ceiling triangle)
#endif
#ifndef PLANE_GEOM_TRACE_FRAME2
#define PLANE_GEOM_TRACE_FRAME2 3328 // second defect frame (nukage smear); 0=off
#endif
#ifndef PLANE_GEOM_TRACE_POLYS
#define PLANE_GEOM_TRACE_POLYS 64 // dump at most this many runs that frame (high
                                  // enough to reach CEILING polys -- floors emit
                                  // first, ceilings later)
#endif
// True on the render that pairs with BENCH_MARK frame=FRAME (counter == FRAME-1).
#define PGT_FRAME_HIT() \
    ( (N64Bench_FrameNo() == (unsigned long)(PLANE_GEOM_TRACE_FRAME) - 1UL) \
   || (PLANE_GEOM_TRACE_FRAME2 != 0 \
       && N64Bench_FrameNo() == (unsigned long)(PLANE_GEOM_TRACE_FRAME2) - 1UL) )
// Float -> int + signed milli-frac, same convention PLANE_UV_TRACE uses. Define
// independently so the two traces compile in isolation (either flag alone).
#ifndef IFLOORF
#define IFLOORF(f)   ((int)floorf((float)(f)))
#endif
#ifndef MILLIFRAC
#define MILLIFRAC(f) ((int)(( (float)(f) - floorf((float)(f)) ) * 1000.0f))
#endif
static int   pgt_runs_left = -1;        // runs still to dump this matching frame
static int   pgt_island_minx = -1;      // current island [minx,maxx] (set by the
static int   pgt_island_maxx = -1;      //   R_EmitPlanePolys island loop)
static int   pgt_island_seq  = 0;       // 0,1,2... islands seen this dumped frame
static int   pgt_run_in_isl  = -1;      // run index within the current island
// Previous emitted run's RIGHT edge, for the adjacent-run boundary check. Valid
// only while pgt_prev_valid and we are still inside the same island (the island
// loop clears pgt_prev_valid at each new island). Because R_EmitIslandRuns emits
// strictly left-to-right, the immediately preceding R_EmitRunPoly in the same
// island IS the spatially adjacent run to the left.
static int   pgt_prev_valid  = 0;
static int   pgt_prev_x2     = 0;       // previous run's last column
static float pgt_prev_ytr    = 0.0f;    // ... its right-edge top Y (at x2+1)
static float pgt_prev_ybr    = 0.0f;    // ... its right-edge bottom Y (at x2+1)
#endif // PLANE_GEOM_TRACE

// ---- DPLANES_PROBE: sub-bracket the RDP plane work (diagnostic, no render -----
// change). When -DDPLANES_PROBE=1 (Makefile DPLANES_PROBE=1; default 0 -> compiled
// out), R_DrawPlanes accumulates RAW CP0 ticks (get_ticks()) into three per-frame
// counters splitting the `planes` BPH bracket:
//   * dpp_lump_tk   -- per-visplane W_CacheLumpNum + the trailing Z_ChangeTag;
//   * dpp_emit_tk   -- the whole R_EmitPlanePolys run-fitter (it CALLS the
//                      un-projection, so the pure fitter cost is dpp_emit_tk minus
//                      dpp_unproj_tk -- subtracted at the latch);
//   * dpp_unproj_tk -- the R_PlaneCornerAttr clusters in R_EmitRunPoly /
//                      R_EmitPlaneBand (timed around the 4-corner call group, one
//                      get_ticks pair per emitted quad to keep overhead low).
// Latched once per frame via N64Bench_SetDPlanes at the R_DrawPlanes return. The
// emit body runs UNCHANGED; only get_ticks() reads are added, so the geometry
// fingerprint is unperturbed (verify BENCH_RESULT stays identical).
#ifndef DPLANES_PROBE
#define DPLANES_PROBE 0
#endif
#if DPLANES_PROBE
#include <libdragon.h>          // get_ticks (flag-gated only)
#include "n64_bench.h"          // N64Bench_SetDPlanes
static uint32_t dpp_lump_tk;    // ticks: lump cache (per frame)
static uint32_t dpp_emit_tk;    // ticks: R_EmitPlanePolys total (per frame)
static uint32_t dpp_unproj_tk;  // ticks: R_PlaneCornerAttr clusters (per frame)
static uint32_t dpp_scan_cols;  // total deviation-scan column iterations (per frame)
static uint32_t dpp_nodes;      // R_EmitIslandRuns invocations (per frame)
#define DPP_T0()  uint64_t _dpp_t0 = get_ticks()
#define DPP_ACC(acc)  do { (acc) += (uint32_t)(get_ticks() - _dpp_t0); } while (0)
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



#ifdef N64
//
// --- Stage-4b: visplanes as RDP POLYGONS (trapezoid strips) ----------------
//
// Replaces DOOM's per-span software fill with RDP trapezoid quads. This is the
// EMIT half of the count-only R_CountPlanePolyTris probe: the SAME island walk
// and the SAME DL_SPLIT_DEVY (~1.25-row) run-break predicate, but where the
// counter returned "1 run" each terminal now computes the run's four corner
// attributes and emits one quad (2 tris) via DL_EmitPlanePoly.
//
// Per-corner U/V are R_MapPlane's un-projection evaluated AT THE CORNER (not per
// span): for a corner at screen (xc, yc),
//   distance = planeheight * yslope(yc)         [yslope(yc)=projectiony/|yc-cy+.5|]
//   length   = distance * distscale[xc]
//   angle    = (viewangle + xtoviewangle[xc]) >> ANGLETOFINESHIFT
//   u_texel  = (viewx + cos(angle)*length) / FRACUNIT     (mod 64 in HW)
//   v_texel  = (-viewy - sin(angle)*length) / FRACUNIT
// -- the exact float form of R_MapPlane's ds_xfrac/ds_yfrac, in texel units (the
// flat sampler reads (xfrac>>16)&63 / (yfrac>>16)&63, so >>16 == texels). The
// columns xtoviewangle/distscale are integer-indexed; the run's end SCREEN edges
// are at columns xa and xb+1, so the right edge samples column xb's angle/scale
// extrapolated half a column outward, mirroring the geometry extrapolation.
//
// Per-corner INV_W = (yc - centery + 0.5)/planeheight (screen-Y-linear, distance
// proportional to 1/(yc-centery) via yslope). The absolute scale cancels in the
// RDP's hyperbolic divide; we keep planeheight in the denominator so the divide
// reconstructs distance. With INV_W ~ (yc-cy) and u = viewx + cos*dist*distscale
// (dist ~ 1/(yc-cy)), both u*INV_W and v*INV_W are screen-AFFINE (the (yc-cy)
// cancels the 1/(yc-cy) in dist, leaving an x-only + y-only sum), so perspective
// interpolation reproduces the software affine flat map EXACTLY.
//
// Light: R_MapPlane picks per-distance light (planezlight[distance>>LIGHTZSHIFT]).
// A poly spans depths, so the run resolves ONE light at its representative (mid)
// distance -- the count-only probe's poly granularity; banding is per-run, a
// close match to software's per-row banding for the shallow floor runs the walk
// produces, and exact under fixedcolormap (invuln/light-amp).

#include "rdp_view.h"
#include "tables.h"     // finecosine/finesine, ANGLETOFINESHIFT

// Y-edge run-break threshold (~1.25 rows): the SAME DL_SPLIT_DEVY the wall path
// and the count-only probe use, duplicated here (r_plane.c stays free of the RDP
// wall header's heavy deps; asserted to match by a build-time comment in
// rdp_view.c). A run that interpolates within this of every captured column is
// one trapezoid; the first column exceeding it splits the run.
#ifndef PLANETESS_SPLIT_DEVY
#define PLANETESS_SPLIT_DEVY  1.25f
#endif

// Per-poly far/near INV_W (depth) ratio ceiling -- the floor-garbage fix.
// R_PlaneCornerAttr's INV_W is the corner's row offset from the horizon (dyrows);
// the RDP reconstructs each pixel's texel as (S*INV_W)/INV_W in fixed point. When
// a single emitted quad spans a large depth range its top/bottom INV_W differ by
// a big ratio, and that hyperbolic divide loses precision / the per-pixel S*INV_W
// setup slope overflows the RDP's fixed point -> the view-dependent floor "garbage
// + stretches forever" (the trace measured 7-11x far/near INV_W on garbage frames,
// ~1x on the clean ones). A tall run is therefore split into horizontal depth
// bands so NO emitted quad's far/near INV_W ratio exceeds PLANE_INVW_RATIO -- each
// band stays in the RDP's exact range. The split is GEOMETRIC in INV_W (equal-ratio
// bands), so a deep run needs only ceil(log_ratio(R)) bands; PLANE_MAX_BANDS caps
// the worst case. Both run-fitter twins (emit + count) compute the SAME band count
// from the SAME corner Ys, so the bench tessellation count tracks the emitted polys.
#ifndef PLANE_INVW_RATIO
#define PLANE_INVW_RATIO  4.0f
#endif
#ifndef PLANE_MAX_BANDS
#define PLANE_MAX_BANDS   6
#endif

// One corner's screen->texel un-projection + INV_W. xc/yc are the corner's
// screen column/row (fractional). Fills *u/*v (texel coords) and *invw.
//
// The texel un-projection runs R_MapPlane's EXACT integer FixedMul chain (the
// same one R_PlaneExpectedTexel mirrors), then expresses the final fixed
// xfrac/yfrac in TEXELS as float (xfrac/65536, yfrac/65536). The earlier float
// port carried yslope/distance/length as 16.16-FIXED magnitudes but only shed
// ONE FRACUNIT on the way to *u/*v, leaving the length term un-divided: it
// emitted u,v of ~+-700,000 texels (a whole uncancelled <<16) where R_MapPlane's
// true texel is ~700-1200. Running the integer chain and dividing the final
// fixed xfrac/yfrac by FRACUNIT removes that ambiguity by construction -- the
// emitted texel is then identical (modulo the masking the trace applies) to the
// software floor's ds_xfrac>>16 / ds_yfrac>>16 at that corner pixel. Full
// precision, UN-masked: the perspective interp and the whole-64 period bias
// downstream need the continuous texel, so we do NOT &63 here.
static void
R_PlaneCornerAttr
( float		xc,
  float		yc,
  float*	u,
  float*	v,
  float*	invw )
{
    float	dyrows = yc - (float)centery + 0.5f;
    int		xci, yci;
    angle_t	ang;
    fixed_t	distance, length, xfrac, yfrac;

    if (dyrows < 0.0f) dyrows = -dyrows;
    // Horizon guard. A corner closer than ~1 row to centery would yield a
    // near-infinite distance (yslope -> inf); software never maps a floor row
    // that close to the horizon (its visplanes are bounded away from it), so the
    // only way a corner reaches it is the half-pixel run-end extrapolation. Clamp
    // dyrows to >= 1 row: the distance stays finite + sane, and the downstream
    // texel coords + IFLOOR/s10.5 casts can never overflow (which trapped before).
    if (dyrows < 1.0f) dyrows = 1.0f;

    // Row-indexed yslope and column-indexed distscale/xtoviewangle, the exact
    // integer tables R_MapPlane samples. yci is the corner row offset from
    // centery (yslope[] is symmetric about centery: yslope[y] depends only on
    // |y-centery+.5|). Clamp both screen indices into range -- the right edge
    // xb+1 can reach viewwidth and the half-pixel extrapolation can push a corner
    // a row past the visplane; sample the last valid column/row (matches
    // R_PlaneExpectedTexel's clamps).
    xci = (int)(xc + 0.5f);
    if (xci < 0)            xci = 0;
    if (xci >= viewwidth)   xci = viewwidth - 1;

    yci = (int)((float)centery + dyrows - 0.5f + 0.5f);   // round(centery+dyrows)
    if (yci < 0)            yci = 0;
    if (yci >= viewheight)  yci = viewheight - 1;

    // R_MapPlane's exact integer chain (== R_PlaneExpectedTexel):
    //   distance = FixedMul(planeheight, yslope[y])
    //   length   = FixedMul(distance, distscale[x])
    //   xfrac    =  viewx + FixedMul(finecosine[ang], length)
    //   yfrac    = -viewy - FixedMul(finesine[ang],   length)
    distance = FixedMul(planeheight, yslope[yci]);
    length   = FixedMul(distance, distscale[xci]);
    ang      = (viewangle + xtoviewangle[xci]) >> ANGLETOFINESHIFT;
    xfrac    =  viewx + FixedMul(finecosine[ang], length);
    yfrac    = -viewy - FixedMul(finesine[ang],   length);

    // u/v in TEXELS = the fixed xfrac/yfrac >> 16, full precision (not masked):
    // xfrac/65536, yfrac/65536 -- exactly R_MapPlane's ds_xfrac/ds_yfrac in texel
    // units. FRACUNIT == 65536; the fixed magnitude carries the whole <<16, so
    // the single /FRACUNIT here lands on the true texel position.
    *u = (float)xfrac * (1.0f / (float)FRACUNIT);
    *v = (float)yfrac * (1.0f / (float)FRACUNIT);

    // INV_W = (yc - centery + 0.5) -- screen-Y-linear, the ROW offset itself.
    // distance ~ planeheight/dyrows (yslope), so 1/W ~ dyrows up to a constant;
    // the ABSOLUTE scale of INV_W cancels in the RDP's texture divide (u =
    // (u*INV_W)/INV_W), and planeheight cancels independently because the texel
    // coords already carry the full distance (length ~ planeheight/dyrows, so
    // u*INV_W is x-only + y-only -- screen-affine -- for ANY INV_W scale). Using
    // bare dyrows keeps 1/INV_W = 1/dyrows in [~1/168, ~32], well inside
    // rdpq_triangle's s16.16 W = 1/INV_W cast (the planeheight-scaled form
    // overflowed it on deep floors near the horizon: 1/INV_W = planeheight/dyrows).
    *invw = dyrows;     // dyrows >= 0.03125 (guarded above) -> 1/INV_W bounded
}

// ---------------------------------------------------------------------------
// SHARED-EDGE top-corner construction (the coverage/seam fix).
//
// A run [xa..xb] lives inside a covered island [islx_lo..islx_hi]. Its top-edge
// corners sit at the pixel EDGES xa and xb+1 (half a column outside the end
// column centres). Three cases per side:
//
//   * INTERNAL boundary, GENTLE slope -- a covered column exists outward
//     (xa > islx_lo on the left, xb < islx_hi on the right) and the top[] step
//     ACROSS that boundary is <= PLANETESS_SPLIT_DEVY. The boundary is SHARED
//     with the neighbouring run, so the corner Y MUST be a value both runs
//     compute identically: the visplane edge AT the pixel boundary, the
//     midpoint of the two columns straddling it, 0.5*(top[c]+top[c+1]). Run N's
//     right corner 0.5*(top[xb]+top[xb+1]) == run N+1's left corner
//     0.5*(top[xa-1]+top[xa]) when xa == xb+1 -> the two quads meet EXACTLY,
//     no +-0.5-row seam. (The OLD code extrapolated each boundary corner half
//     a pixel PAST the run independently, so adjacent corners never coincided.)
//
//   * INTERNAL boundary, STEEP step -- a covered column exists outward but the
//     top[] step across the boundary EXCEEDS PLANETESS_SPLIT_DEVY. This is a
//     real height DISCONTINUITY (a wall rises between two floor heights), not a
//     smooth edge: a shared midpoint there sits ~half the step from BOTH
//     columns' true top (the +-12-row over-shoot -- floor short on one side,
//     slicing UP into the wall on the other). So the corner is pinned to THIS
//     column's OWN top[] -- a vertical step edge. Each side independently
//     evaluates the same |top[c+1]-top[c]| predicate, so they agree it is a
//     step and each owns its corner; coverage is then exact (dtop == 0) and the
//     "seam" is the correct vertical floor-height discontinuity, not a bleed.
//
//   * TRUE island edge -- no covered column outward (xa == islx_lo / xb ==
//     islx_hi). Keep the half-pixel outward extrapolation along the end
//     column's local step (so the rasterizer's interpolation passes through
//     the sampled value at the end column's centre), but CLAMP its outward
//     reach to <= PLANETESS_SPLIT_DEVY rows past top[] at that column, so the
//     extrapolation can never over-shoot a steep edge by tens of rows.
//
// The bottom edge is left to the caller's original construction (it runs to
// screen bottom; its extrapolation is benign and unchanged).
static void
R_PlaneRunTopCorners
( const byte*	top,
  int		xa,
  int		xb,
  int		islx_lo,
  int		islx_hi,
  float*	ytl,
  float*	ytr )
{
    float	tl0, tr0;

    // -- left-top corner at column xa --
    if (xa > islx_lo)
    {
	// internal boundary shared with the previous run's RIGHT corner.
	float step = (float)top[xa] - (float)top[xa - 1];
	if (step < 0.0f) step = -step;
	if (step > PLANETESS_SPLIT_DEVY)
	    tl0 = (float)top[xa];			// steep step: own column (vertical edge)
	else
	    tl0 = 0.5f * ((float)top[xa - 1] + (float)top[xa]);	// gentle: shared midpoint
    }
    else if (xb > xa)
    {
	// true island left edge: half-pixel outward extrapolation, clamped so
	// the outward reach past top[xa] never exceeds the split tolerance.
	float lim = (float)top[xa];
	tl0 = (float)top[xa] - 0.5f * ((float)top[xa + 1] - (float)top[xa]);
	if (tl0 < lim - PLANETESS_SPLIT_DEVY) tl0 = lim - PLANETESS_SPLIT_DEVY;
	if (tl0 > lim + PLANETESS_SPLIT_DEVY) tl0 = lim + PLANETESS_SPLIT_DEVY;
    }
    else
    {
	tl0 = (float)top[xa];		// width-1 island: column's own edge
    }

    // -- right-top corner at column xb --
    if (xb < islx_hi)
    {
	// internal boundary shared with the next run's LEFT corner.
	float step = (float)top[xb + 1] - (float)top[xb];
	if (step < 0.0f) step = -step;
	if (step > PLANETESS_SPLIT_DEVY)
	    tr0 = (float)top[xb];			// steep step: own column (vertical edge)
	else
	    tr0 = 0.5f * ((float)top[xb] + (float)top[xb + 1]);	// gentle: shared midpoint
    }
    else if (xb > xa)
    {
	float lim = (float)top[xb];
	tr0 = (float)top[xb] + 0.5f * ((float)top[xb] - (float)top[xb - 1]);
	if (tr0 < lim - PLANETESS_SPLIT_DEVY) tr0 = lim - PLANETESS_SPLIT_DEVY;
	if (tr0 > lim + PLANETESS_SPLIT_DEVY) tr0 = lim + PLANETESS_SPLIT_DEVY;
    }
    else
    {
	tr0 = (float)top[xb];
    }

    *ytl = tl0;
    *ytr = tr0;
}

// Does run [xa..xb]'s TOP edge step too steeply to be ONE trapezoid? The
// per-column deviation scan only runs for width >= 3 (xb > xa+1); a width-2 run
// is normally taken as exact, but if top[] jumps hard across its two columns the
// single lerped top edge slices through the wall on one side and leaves floor
// short on the other (the +-12-row over-shoot at a steep step). This forces the
// split-scan to engage for such width-2 runs too, so each emitted piece tracks
// the true visplane within tolerance. Returns nonzero => take the split scan.
static int
R_PlaneTopStepSteep
( const byte*	top,
  int		xa,
  int		xb )
{
    float d = (float)top[xb] - (float)top[xa];
    if (d < 0.0f) d = -d;
    return d > PLANETESS_SPLIT_DEVY;
}

#if PLANE_UV_TRACE
// GROUND TRUTH for the self-trace: R_MapPlane's EXACT per-pixel integer math at
// an ARBITRARY screen pixel (xs,ys). Returns the masked texel (s,t)&63 the
// software floor would sample there -- the same chain R_MapPlane runs:
//   distance = FixedMul(planeheight, yslope[ys])
//   length   = FixedMul(distance, distscale[xs])
//   ds_xfrac = viewx + FixedMul(finecosine[ang], length)   ; s = (xfrac>>16)&63
//   ds_yfrac = -viewy - FixedMul(finesine[ang], length)    ; t = (yfrac>>16)&63
// (ang = (viewangle+xtoviewangle[xs])>>ANGLETOFINESHIFT). Integer-faithful, so
// it is the literal reference the emitted/reconstructed texels must reproduce.
// Also returns the raw (unmasked) fixed_t xfrac/yfrac so the trace can show the
// pre-wrap texel position too.
static void
R_PlaneExpectedTexel
( int       xs,
  int       ys,
  int*      s_out,
  int*      t_out,
  fixed_t*  xfrac_out,
  fixed_t*  yfrac_out )
{
    angle_t ang;
    fixed_t distance, length, xfrac, yfrac;

    if (xs < 0) xs = 0; else if (xs >= viewwidth)  xs = viewwidth  - 1;
    if (ys < 0) ys = 0; else if (ys >= viewheight) ys = viewheight - 1;

    distance = FixedMul(planeheight, yslope[ys]);
    length   = FixedMul(distance, distscale[xs]);
    ang      = (viewangle + xtoviewangle[xs]) >> ANGLETOFINESHIFT;
    xfrac    =  viewx + FixedMul(finecosine[ang], length);
    yfrac    = -viewy - FixedMul(finesine[ang],   length);

    *xfrac_out = xfrac;
    *yfrac_out = yfrac;
    *s_out = (int)((xfrac >> 16) & 63);
    *t_out = (int)((yfrac >> 16) & 63);
}
#endif // PLANE_UV_TRACE

// Depth-band count for a run with corner Ys [ytl,ytr,ybl,ybr]. INV_W at a corner
// is its |row offset from the horizon| (R_PlaneCornerAttr's dyrows, clamped >=1);
// the left edge sizes the split (the run-fitter holds top/bottom edge deviation
// within PLANETESS_SPLIT_DEVY ~1.25 rows, so left and right ratios track). Returns
// ceil(log_PLANE_INVW_RATIO(R)) bands, R = far/near INV_W ratio, capped at
// PLANE_MAX_BANDS. The ybl horizon/degenerate clamp mirrors R_EmitRunPoly so the
// emit and count twins derive an IDENTICAL count from the same inputs.
static int
R_PlaneRunBands
( float	ytl,
  float	ytr,
  float	ybl,
  float	ybr )
{
    float	cyf = (float)centery - 0.5f;
    float	wtop, wbot, lo, hi, R, acc;
    int		N;

    (void)ytr; (void)ybr;                   // left edge sizes the split

    if (ybl < ytl + 0.05f) ybl = ytl + 0.05f;   // mirror R_EmitRunPoly's clamp

    wtop = ytl - cyf;  if (wtop < 0.0f) wtop = -wtop;  if (wtop < 1.0f) wtop = 1.0f;
    wbot = ybl - cyf;  if (wbot < 0.0f) wbot = -wbot;  if (wbot < 1.0f) wbot = 1.0f;

    lo = (wtop < wbot) ? wtop : wbot;
    hi = (wtop < wbot) ? wbot : wtop;
    R  = hi / lo;

    N = 1;  acc = PLANE_INVW_RATIO;
    while (acc < R && N < PLANE_MAX_BANDS) { acc *= PLANE_INVW_RATIO; N++; }
    return N;
}

// Resolve the light COLORMAP LEVEL at a single CORNER's own screen row, mirroring
// R_MapPlane's exact per-distance light chain (distance = planeheight*yslope[row];
// index = distance>>LIGHTZSHIFT; cm = planezlight[index]). Software shades floors
// per ROW; the RDP plane path picked ONE flat colormap per quad (R_PlaneRunColormap,
// or per-band in 31fd2f1) and applied it across the whole quad as a single PRIM, so
// a deep run rendered hard horizontal brightness STAIRS where software is a smooth
// gradient -- glaring under the red death-flash (the band step over-darkens the far
// corner toward black). Resolving the level at EACH of the quad's four corners lets
// the plane flush feed dl_prim_lut[level] as a per-vertex SHADE, which the RDP
// GOURAUD-interpolates corner->corner -- a continuous depth gradient that converges
// to software's brightness at the corners (keeps 31fd2f1's deep-floor win) with NO
// per-band step. fixedcolormap (invuln/light-amp) overrides per-distance light
// exactly as R_MapPlane does, so a corner under it gets that flat level. Must run at
// EMIT time -- planeheight/planezlight/fixedcolormap are this visplane's live state
// then. Returns the colormap pointer; DL_PlaneLightLevel reduces it to the level.
static const void*
R_PlaneCornerColormap
( float		corner_row )
{
    int		yrow;
    fixed_t	distance;
    unsigned	index;

    if (fixedcolormap)
	return fixedcolormap;

    yrow = (int)(corner_row + 0.5f);
    if (yrow < 0) yrow = 0;
    if (yrow >= viewheight) yrow = viewheight - 1;

    distance = FixedMul(planeheight, yslope[yrow]);
    index = distance >> LIGHTZSHIFT;
    if (index >= MAXLIGHTZ)
	index = MAXLIGHTZ - 1;
    return planezlight[index];
}

// Fill a poly's 4 per-corner light levels from its 4 corner screen rows, via the
// exact R_MapPlane distance->planezlight chain (or fixedcolormap). The plane flush
// reads these as per-vertex SHADE. ytl/ytr are the top corners (left/right edge),
// ybl/ybr the bottom corners. Levels are reduced through DL_PlaneLightLevel so they
// key the SAME dl_prim_lut[] the walls and the legacy flat-PRIM path use.
static void
R_FillPlaneCornerLights
( rdp_ppoly_t*	q,
  float		ytl,
  float		ytr,
  float		ybl,
  float		ybr )
{
    q->light_tl = DL_PlaneLightLevel(R_PlaneCornerColormap(ytl));
    q->light_tr = DL_PlaneLightLevel(R_PlaneCornerColormap(ytr));
    q->light_bl = DL_PlaneLightLevel(R_PlaneCornerColormap(ybl));
    q->light_br = DL_PlaneLightLevel(R_PlaneCornerColormap(ybr));
}

// Emit ONE depth band as a quad: four corner U/V/INV_W at screen (xl|xr, yt*|yb*)
// + flat + light, exactly as R_EmitRunPoly builds its single quad. x1/x2 are the
// run's column span (shared by every band); only the Y edges + corner attrs differ.
static void
R_EmitPlaneBand
( int16_t	x1,
  int16_t	x2,
  float		xl,
  float		xr,
  float		ytl,
  float		ytr,
  float		ybl,
  float		ybr,
  uint16_t	flatlump,
  const void*	cm )
{
    rdp_ppoly_t	q;

    q.x1 = x1;  q.x2 = x2;
    q.ytop_l = ytl;  q.ybot_l = ybl;
    q.ytop_r = ytr;  q.ybot_r = ybr;
#if DPLANES_PROBE
    { DPP_T0();
#endif
    R_PlaneCornerAttr(xl, ytl, &q.u_tl, &q.v_tl, &q.invw_tl);
    R_PlaneCornerAttr(xr, ytr, &q.u_tr, &q.v_tr, &q.invw_tr);
    R_PlaneCornerAttr(xl, ybl, &q.u_bl, &q.v_bl, &q.invw_bl);
    R_PlaneCornerAttr(xr, ybr, &q.u_br, &q.v_br, &q.invw_br);
#if DPLANES_PROBE
    DPP_ACC(dpp_unproj_tk); }
#endif
    q.flatlump = flatlump;
    q.light    = 0;     // overwritten by DL_EmitPlanePoly from cm (legacy run PRIM)
    // PER-CORNER light for the GOURAUD-interpolated SHADE: resolve each corner's
    // own depth-row level (R_MapPlane chain) so this band's light interpolates
    // smoothly within itself AND continues into the adjacent band (shared corner
    // rows => matching levels => seamless gradient across the whole run).
    R_FillPlaneCornerLights(&q, ytl, ytr, ybl, ybr);
    DL_EmitPlanePoly(&q, cm);
}

// Emit ONE trapezoid run [xa..xb] of a covered island as a quad. top[]/bottom[]
// are the visplane's per-column edges; the corner Y geometry uses the EXACT same
// half-pixel extrapolation R_CountIslandRuns builds (so the emitted quad's screen
// shape matches what the probe counted). flatlump/cm are the run's flat + light.
static void
R_EmitRunPoly
( const byte*	top,
  const byte*	bottom,
  int		xa,
  int		xb,
  int		islx_lo,
  int		islx_hi,
  int		flatlump,
  const void*	cm )
{
    rdp_ppoly_t	p;
    float	ytl, ytr, ybl, ybr;
    float	xl = (float)xa;
    float	xr = (float)xb + 1.0f;      // right SCREEN edge

    // TOP-edge corners: shared-boundary construction (internal split boundaries
    // get the SHARED visplane midpoint so adjacent runs coincide -- no seam --
    // and true island edges use the clamped half-pixel extrapolation). BOTTOM
    // edge keeps its original outward extrapolation (it runs to screen bottom).
    R_PlaneRunTopCorners(top, xa, xb, islx_lo, islx_hi, &ytl, &ytr);
    {
	float bl0 = (float)bottom[xa] + 1.0f,  br0 = (float)bottom[xb] + 1.0f;

	if (xb > xa)
	{
	    bl0 -= 0.5f * (((float)bottom[xa + 1] + 1.0f) - bl0);
	    br0 += 0.5f * (br0 - ((float)bottom[xb - 1] + 1.0f));
	}
	ybl = bl0;  ybr = br0;
    }
    // Keep each edge's screen span strictly positive (degenerate clip steps).
    if (ybl < ytl + 0.05f) ybl = ytl + 0.05f;
    if (ybr < ytr + 0.05f) ybr = ytr + 0.05f;

    p.x1 = (int16_t)xa;
    p.x2 = (int16_t)xb;
    p.ytop_l = ytl;  p.ybot_l = ybl;
    p.ytop_r = ytr;  p.ybot_r = ybr;

    // Four corner texel U/V + INV_W (left edge at column xa, right at xb+1).
#if DPLANES_PROBE
    { DPP_T0();
#endif
    R_PlaneCornerAttr(xl, ytl, &p.u_tl, &p.v_tl, &p.invw_tl);
    R_PlaneCornerAttr(xr, ytr, &p.u_tr, &p.v_tr, &p.invw_tr);
    R_PlaneCornerAttr(xl, ybl, &p.u_bl, &p.v_bl, &p.invw_bl);
    R_PlaneCornerAttr(xr, ybr, &p.u_br, &p.v_br, &p.invw_br);
#if DPLANES_PROBE
    DPP_ACC(dpp_unproj_tk); }
#endif

    p.flatlump = (uint16_t)flatlump;
    p.light    = 0;     // overwritten by DL_EmitPlanePoly from cm

    // Depth-band split: if this run's far/near INV_W ratio is too wide for the
    // RDP's fixed-point perspective divide, slice it into PLANE_INVW_RATIO-bounded
    // horizontal bands (geometric in INV_W) so each emitted quad stays in range.
    // N==1 emits the single quad p unchanged (identical to the pre-split path).
    {
	int N = R_PlaneRunBands(ytl, ytr, ybl, ybr);

	if (N <= 1)
	{
	    // Single quad: 4 corner light levels at the run's own corner rows for
	    // the GOURAUD SHADE (smooth depth gradient across the quad).
	    R_FillPlaneCornerLights(&p, ytl, ytr, ybl, ybr);
	    DL_EmitPlanePoly(&p, cm);
	}
	else
	{
	    float	cyf  = (float)centery - 0.5f;
	    float	wtop = ytl - cyf, wbot = ybl - cyf;
	    float	f, denom, bk;
	    int		k;

	    if (wtop < 0.0f) wtop = -wtop;
	    if (wtop < 1.0f) wtop = 1.0f;
	    if (wbot < 0.0f) wbot = -wbot;
	    if (wbot < 1.0f) wbot = 1.0f;

	    // March band boundaries geometrically from the near edge's INV_W toward
	    // the far edge's; t = (invw-wtop)/(wbot-wtop) is the edge parameter (INV_W
	    // is linear in screen Y, so t doubles as the screen-Y lerp factor). denom
	    // is safe: N>1 => the far/near ratio exceeds PLANE_INVW_RATIO, so wbot!=wtop.
	    f     = (wbot >= wtop) ? PLANE_INVW_RATIO : (1.0f / PLANE_INVW_RATIO);
	    denom = wbot - wtop;
	    bk    = wtop;

	    for (k = 0; k < N; k++)
	    {
		float		bk1 = (k + 1 == N) ? wbot : (bk * f);
		float		t0, t1, btl, btr, bbl, bbr;

		if (f > 1.0f) { if (bk1 > wbot) bk1 = wbot; }
		else          { if (bk1 < wbot) bk1 = wbot; }

		t0 = (bk  - wtop) / denom;
		t1 = (bk1 - wtop) / denom;
		btl = ytl + t0 * (ybl - ytl);  btr = ytr + t0 * (ybr - ytr);
		bbl = ytl + t1 * (ybl - ytl);  bbr = ytr + t1 * (ybr - ytr);

		// LIGHT is now PER-CORNER (R_EmitPlaneBand fills the band's four corner
		// levels from their own depth rows) and the RDP GOURAUD-interpolates it
		// as SHADE -- so a deep multi-band run is a CONTINUOUS depth gradient,
		// not flat-per-band steps. The band split here is purely the INV_W
		// perspective-precision slice; it no longer carries a flat band light.
		// cm stays the run's representative colormap (legacy run PRIM/fallback).
		R_EmitPlaneBand(p.x1, p.x2, xl, xr, btl, btr, bbl, bbr,
				p.flatlump, cm);
		bk = bk1;
	    }
	}
    }

#if PLANE_GEOM_TRACE
    // ---- floor-poly COVERAGE/geometry trace (diagnostic, no render effect) ----
    // Fires for the first PLANE_GEOM_TRACE_POLYS emitted runs of frame
    // PLANE_GEOM_TRACE_FRAME only. Dumps the run's screen coverage, the visplane's
    // true per-column extent at the run's columns (poly-vs-visplane delta), and the
    // boundary against the immediately-preceding run in the same island.
    if (PGT_FRAME_HIT())
    {
        if (pgt_runs_left < 0)          // first run seen this frame: arm + header
        {
            pgt_runs_left = PLANE_GEOM_TRACE_POLYS;
            debugf("PGT_FRAME frame=%lu centery=%d viewwidth=%d viewheight=%d "
                   "planeheight=%d\n",
                   N64Bench_FrameNo() + 1UL, (int)centery, viewwidth, viewheight,
                   (int)planeheight);
        }

        if (pgt_runs_left > 0)
        {
            int   xm = (xa + xb) >> 1;          // mid column of the run
            float width = (float)(xb + 1 - xa);
            // Emitted poly's interpolated [ytop,ybot] AT a given column center xs,
            // so the per-column poly-vs-visplane delta is directly comparable.
            // (lerp of the corner edges, exactly what the rasterizer interpolates.)
            #define PGT_LERP(a,b,xs) \
                ((a) + ((b) - (a)) * (((float)(xs) + 0.5f - (float)xa) / width))
            float poly_top_xa = ytl,                         poly_bot_xa = ybl;
            float poly_top_xm = PGT_LERP(ytl, ytr, xm),      poly_bot_xm = PGT_LERP(ybl, ybr, xm);
            float poly_top_xb = ytr,                         poly_bot_xb = ybr;

            // ---- 1+4: run screen coverage + island context + corner Ys ----------
            // Raw column samples (top[xa], top[xb], bottom[xa]+1, bottom[xb]+1) vs
            // the extrapolated corner Ys (ytl/ytr/ybl/ybr) show exactly how the
            // half-pixel run-end extrapolation displaced the run-edge corners.
            debugf("PGT_RUN n=%d isl=%d run=%d islx=%d..%d x1=%d x2=%d "
                   "xl=%d xr=%d ytl=%d.%03d ytr=%d.%03d ybl=%d.%03d ybr=%d.%03d "
                   "raw_top_xa=%d raw_top_xb=%d raw_bot_xa=%d raw_bot_xb=%d\n",
                   PLANE_GEOM_TRACE_POLYS - pgt_runs_left,
                   pgt_island_seq, pgt_run_in_isl,
                   pgt_island_minx, pgt_island_maxx,
                   xa, xb, (int)xl, (int)xr,
                   IFLOORF(ytl), MILLIFRAC(ytl), IFLOORF(ytr), MILLIFRAC(ytr),
                   IFLOORF(ybl), MILLIFRAC(ybl), IFLOORF(ybr), MILLIFRAC(ybr),
                   (int)top[xa], (int)top[xb],
                   (int)bottom[xa] + 1, (int)bottom[xb] + 1);

            // ---- 2: visplane TRUE coverage vs emitted poly, per column ----------
            // For x = xa, xm, xb: the software-fill extent [top[x], bottom[x]+1]
            // vs the poly's interpolated [polytop, polybot] at that column. dtop /
            // dbot are the poly-minus-visplane row deltas; nonzero => the poly clips
            // wrong (into a wall if poly extent < visplane, or short if > ). The
            // half-pixel extrapolation makes a HALF-row delta expected at the END
            // columns (xa,xb); a delta >> 0.5 row, or any nonzero at the MID column,
            // is the real coverage bug.
            debugf("PGT_COV x=%d vp_top=%d vp_bot=%d poly_top=%d.%03d "
                   "poly_bot=%d.%03d dtop=%d.%03d dbot=%d.%03d\n",
                   xa, (int)top[xa], (int)bottom[xa] + 1,
                   IFLOORF(poly_top_xa), MILLIFRAC(poly_top_xa),
                   IFLOORF(poly_bot_xa), MILLIFRAC(poly_bot_xa),
                   IFLOORF(poly_top_xa - (float)top[xa]),
                   MILLIFRAC(poly_top_xa - (float)top[xa]),
                   IFLOORF(poly_bot_xa - ((float)bottom[xa] + 1.0f)),
                   MILLIFRAC(poly_bot_xa - ((float)bottom[xa] + 1.0f)));
            debugf("PGT_COV x=%d vp_top=%d vp_bot=%d poly_top=%d.%03d "
                   "poly_bot=%d.%03d dtop=%d.%03d dbot=%d.%03d\n",
                   xm, (int)top[xm], (int)bottom[xm] + 1,
                   IFLOORF(poly_top_xm), MILLIFRAC(poly_top_xm),
                   IFLOORF(poly_bot_xm), MILLIFRAC(poly_bot_xm),
                   IFLOORF(poly_top_xm - (float)top[xm]),
                   MILLIFRAC(poly_top_xm - (float)top[xm]),
                   IFLOORF(poly_bot_xm - ((float)bottom[xm] + 1.0f)),
                   MILLIFRAC(poly_bot_xm - ((float)bottom[xm] + 1.0f)));
            debugf("PGT_COV x=%d vp_top=%d vp_bot=%d poly_top=%d.%03d "
                   "poly_bot=%d.%03d dtop=%d.%03d dbot=%d.%03d\n",
                   xb, (int)top[xb], (int)bottom[xb] + 1,
                   IFLOORF(poly_top_xb), MILLIFRAC(poly_top_xb),
                   IFLOORF(poly_bot_xb), MILLIFRAC(poly_bot_xb),
                   IFLOORF(poly_top_xb - (float)top[xb]),
                   MILLIFRAC(poly_top_xb - (float)top[xb]),
                   IFLOORF(poly_bot_xb - ((float)bottom[xb] + 1.0f)),
                   MILLIFRAC(poly_bot_xb - ((float)bottom[xb] + 1.0f)));

            // ---- 3: adjacent-run boundary (prev run RIGHT edge vs this LEFT) -----
            // prev run's right SCREEN edge is column pgt_prev_x2+1; this run's left
            // SCREEN edge is column xa. gap_cols = xa - (pgt_prev_x2+1): 0 = they
            // abut, >0 = uncovered GAP columns, <0 = OVERLAP columns (bleed). dtop/
            // dbot = this run's left corner Y minus prev run's right corner Y at the
            // shared boundary; nonzero with gap_cols==0 means a seam (the two quads
            // meet at the column but at different rows -> a visible notch/bleed).
            if (pgt_prev_valid)
            {
                int gap_cols = xa - (pgt_prev_x2 + 1);
                debugf("PGT_BND prev_x2=%d prev_xr=%d prev_ytr=%d.%03d "
                       "prev_ybr=%d.%03d this_x1=%d this_xl=%d this_ytl=%d.%03d "
                       "this_ybl=%d.%03d gap_cols=%d dtop=%d.%03d dbot=%d.%03d\n",
                       pgt_prev_x2, pgt_prev_x2 + 1,
                       IFLOORF(pgt_prev_ytr), MILLIFRAC(pgt_prev_ytr),
                       IFLOORF(pgt_prev_ybr), MILLIFRAC(pgt_prev_ybr),
                       xa, (int)xl,
                       IFLOORF(ytl), MILLIFRAC(ytl),
                       IFLOORF(ybl), MILLIFRAC(ybl),
                       gap_cols,
                       IFLOORF(ytl - pgt_prev_ytr), MILLIFRAC(ytl - pgt_prev_ytr),
                       IFLOORF(ybl - pgt_prev_ybr), MILLIFRAC(ybl - pgt_prev_ybr));
            }
            #undef PGT_LERP

            pgt_runs_left--;
        }

        // Record THIS run's right edge as the "previous" for the next adjacent run.
        // R_EmitPlanePolys clears pgt_prev_valid at each new island, so this only
        // pairs runs that are spatially adjacent within one island.
        pgt_prev_valid = 1;
        pgt_prev_x2    = xb;
        pgt_prev_ytr   = ytr;
        pgt_prev_ybr   = ybr;
        pgt_run_in_isl++;
    }
    else if (pgt_runs_left >= 0)
    {
        pgt_runs_left  = -1;            // re-arm for a future matching frame
        pgt_prev_valid = 0;
    }
#endif // PLANE_GEOM_TRACE

#if PLANE_UV_TRACE
    // ---- floor-poly texel self-trace (diagnostic, no render effect) ----------
    // Fires for the first PLANE_UV_TRACE_POLYS polys of frame PLANE_UV_TRACE_FRAME
    // only. Dumps, per corner: screen(x,y), EMITTED(u,v,invw), and the EXPECTED
    // masked texel (s,t)&63 R_MapPlane would sample at that corner pixel. Then at
    // the poly's parametric-center pixel: the RDP-RECONSTRUCTED texel (bilinear
    // u*invw,v*invw,invw -> divide -> &63) vs R_MapPlane's EXPECTED there. If the
    // corners already disagree -> the corner-attr derivation is wrong. If corners
    // agree but the center reconstruction diverges -> the INV_W / screen-affine
    // assumption is the bug.
    if (framecount == PLANE_UV_TRACE_FRAME)
    {
        if (plane_uv_trace_polys_left < 0)      // first poly seen this frame: arm
        {
            plane_uv_trace_polys_left = PLANE_UV_TRACE_POLYS;
            debugf("PUVT_FRAME frame=%d planeheight=%d viewx=%d viewy=%d "
                   "projectiony=%d centery=%d viewwidth=%d viewheight=%d\n",
                   framecount, (int)planeheight, (int)viewx, (int)viewy,
                   (int)projectiony, (int)centery, viewwidth, viewheight);
        }

        if (plane_uv_trace_polys_left > 0)
        {
            int   i;
            // Corner screen coords + emitted attrs in a 4-entry array
            // (order: TL, TR, BL, BR), matching p.* and the xl/xr/yt*/yb* above.
            float cx[4]   = { xl,        xr,        xl,        xr        };
            float cy[4]   = { ytl,       ytr,       ybl,       ybr       };
            float cu[4]   = { p.u_tl,    p.u_tr,    p.u_bl,    p.u_br    };
            float cv[4]   = { p.v_tl,    p.v_tr,    p.v_bl,    p.v_br    };
            float ciw[4]  = { p.invw_tl, p.invw_tr, p.invw_bl, p.invw_br };
            const char* nm[4] = { "TL", "TR", "BL", "BR" };

            debugf("PUVT_POLY n=%d xrun=%d..%d ytl=%d ytr=%d ybl=%d ybr=%d\n",
                   PLANE_UV_TRACE_POLYS - plane_uv_trace_polys_left,
                   xa, xb, (int)ytl, (int)ytr, (int)ybl, (int)ybr);

            for (i = 0; i < 4; i++)
            {
                int     xs = (int)(cx[i] + 0.5f);
                int     ys = (int)(cy[i] + 0.5f);
                int     es, et;
                fixed_t exf, eyf;
                // Emitted u,v are raw texels; mask to the 64 period for the
                // apples-to-apples compare against R_MapPlane's masked s,t.
                int     emu = ((int)IFLOORF(cu[i])) & 63;
                int     emv = ((int)IFLOORF(cv[i])) & 63;

                R_PlaneExpectedTexel(xs, ys, &es, &et, &exf, &eyf);

                // Print emitted u,v in milli-texels (x1000) to keep it integer.
                debugf("PUVT_C %s sx=%d sy=%d emit_u=%d.%03d emit_v=%d.%03d "
                       "invw=%d.%03d emit_s=%d emit_t=%d exp_s=%d exp_t=%d "
                       "exp_xfrac=%d exp_yfrac=%d\n",
                       nm[i], xs, ys,
                       IFLOORF(cu[i]), MILLIFRAC(cu[i]),
                       IFLOORF(cv[i]), MILLIFRAC(cv[i]),
                       IFLOORF(ciw[i]), MILLIFRAC(ciw[i]),
                       emu, emv, es, et, (int)exf, (int)eyf);
            }

            // ---- CENTER pixel: RDP reconstruction vs R_MapPlane expected ------
            // Bilinear-interp the corner (u*invw), (v*invw), invw to the
            // parametric center (fx=fy=0.5), then divide -> the texel the RDP's
            // perspective-correct rasterizer reconstructs at the poly's middle.
            {
                float uw_tl = cu[0]*ciw[0], uw_tr = cu[1]*ciw[1];
                float uw_bl = cu[2]*ciw[2], uw_br = cu[3]*ciw[3];
                float vw_tl = cv[0]*ciw[0], vw_tr = cv[1]*ciw[1];
                float vw_bl = cv[2]*ciw[2], vw_br = cv[3]*ciw[3];

                // bilinear at fx=fy=0.5 == simple average of the 4 corners.
                float uw_c = 0.25f*(uw_tl + uw_tr + uw_bl + uw_br);
                float vw_c = 0.25f*(vw_tl + vw_tr + vw_bl + vw_br);
                float iw_c = 0.25f*(ciw[0]+ciw[1]+ciw[2]+ciw[3]);

                float s_recon_f = (iw_c != 0.0f) ? (uw_c / iw_c) : 0.0f;
                float t_recon_f = (iw_c != 0.0f) ? (vw_c / iw_c) : 0.0f;
                int   s_recon   = ((int)IFLOORF(s_recon_f)) & 63;
                int   t_recon   = ((int)IFLOORF(t_recon_f)) & 63;

                // Center SCREEN pixel: mid column, mid row at that column.
                int   xc_s = (int)((xl + xr) * 0.5f);
                int   yc_s = (int)(((ytl+ytr)*0.5f + (ybl+ybr)*0.5f) * 0.5f);
                int   es, et;
                fixed_t exf, eyf;

                R_PlaneExpectedTexel(xc_s, yc_s, &es, &et, &exf, &eyf);

                debugf("PUVT_CTR sx=%d sy=%d recon_s=%d recon_t=%d "
                       "recon_uf=%d.%03d recon_vf=%d.%03d "
                       "exp_s=%d exp_t=%d exp_xfrac=%d exp_yfrac=%d\n",
                       xc_s, yc_s, s_recon, t_recon,
                       IFLOORF(s_recon_f), MILLIFRAC(s_recon_f),
                       IFLOORF(t_recon_f), MILLIFRAC(t_recon_f),
                       es, et, (int)exf, (int)eyf);
            }

            plane_uv_trace_polys_left--;
        }
    }
    else if (plane_uv_trace_polys_left >= 0)
    {
        plane_uv_trace_polys_left = -1;         // re-arm for a future match
    }
#endif // PLANE_UV_TRACE
}

// Recursive trapezoid-run walk for one covered island [xa..xb] -- the EMIT twin
// of R_CountIslandRuns. Identical corner extrapolation + identical DL_SPLIT_DEVY
// deviation predicate + identical split rule; only the terminal differs (emit a
// quad instead of returning a count). flatlump/cm carry the run's flat + light.
static void
R_EmitIslandRuns
( const byte*	top,
  const byte*	bottom,
  int		xa,
  int		xb,
  int		islx_lo,
  int		islx_hi,
  int		depth,
  int		flatlump,
  const void*	cm )
{
    float	ytl, ytr, ybl, ybr;
    float	width = (float)(xb + 1 - xa);
    int		x;

#if DPLANES_PROBE
    dpp_nodes++;
#endif

    // Shared-boundary TOP corners (seam fix) + original BOTTOM extrapolation.
    R_PlaneRunTopCorners(top, xa, xb, islx_lo, islx_hi, &ytl, &ytr);
    {
	float bl0 = (float)bottom[xa] + 1.0f,  br0 = (float)bottom[xb] + 1.0f;

	if (xb > xa)
	{
	    bl0 -= 0.5f * (((float)bottom[xa + 1] + 1.0f) - bl0);
	    br0 += 0.5f * (br0 - ((float)bottom[xb - 1] + 1.0f));
	}
	ybl = bl0;  ybr = br0;
    }

    // Run the per-column deviation scan for width >= 3 OR for a width-2 run
    // whose top edge steps too steeply to be one trapezoid (the +-12-row
    // over-shoot fix): a hard step across a width-2 run must be split per column.
    //
    // MAX-DEVIATION SPLIT (Ramer-Douglas-Peucker): split at the column of
    // GREATEST edge deviation, not the first deviating column. The old
    // first-deviation rule peeled one narrow piece off the left per recursion
    // level, so a wide island with a curved (non-monotonic) top/bottom edge
    // needed O(width) levels -- it hit the depth<10 cap and emitted the wide
    // remainder as ONE trapezoid whose straight edges missed the real visplane
    // by 14-24 rows (the stray-triangle / floor-smear / wall-pull defects on
    // frames 3200/3328/4096). Bisecting at the worst-error column converges in
    // O(log width) levels, so the cap is never the limiter for these runs.
    if (xb > xa + 1 || (xb > xa && R_PlaneTopStepSteep(top, xa, xb)))
    {
	float	maxdev = 0.0f;
	int	xmax   = -1;
	// Strength-reduce the per-column f = (x+0.5-xa)/width DIVIDE (the run-
	// fitter's hottest op -- this scan dominated the `planes` bracket) to ONE
	// reciprocal per node + a multiply per column. width > 0 always (xb >= xa).
	// f is recomputed FROM SCRATCH each column (no running accumulation), so the
	// only arithmetic change vs the divide is reciprocal-then-multiply for the
	// single f term -- the deviation values, the argmax column (xmax) and the
	// split test are otherwise the SAME algebra. The emit twin (here) and the
	// count twin (R_CountIslandRuns) apply the IDENTICAL reduction so the
	// tessellation stays in lockstep, and the emitted geometry is verified
	// pixel-stable against the canonical software reference.
	float	inv_w = 1.0f / width;

#if DPLANES_PROBE
	dpp_scan_cols += (uint32_t)(xb - xa + 1);
#endif
	for (x = xa; x <= xb; x++)
	{
	    float f  = ((float)x + 0.5f - (float)xa) * inv_w;
	    float lt = ytl + (ytr - ytl) * f;
	    float lb = ybl + (ybr - ybl) * f;
	    float dT = lt - (float)top[x];
	    float dB = ((float)bottom[x] + 1.0f) - lb;
	    float aT = (dT < 0.0f) ? -dT : dT;
	    float aB = (dB < 0.0f) ? -dB : dB;
	    float a  = (aT > aB) ? aT : aB;

	    if (a > maxdev) { maxdev = a; xmax = x; }
	}

	if (maxdev > PLANETESS_SPLIT_DEVY && depth < 10)
	{
	    // Split at the worst-error column; clamp so both halves are non-empty
	    // (a max at xa would otherwise make the left half empty).
	    int xm = (xmax >= xb) ? (xb - 1) : ((xmax > xa) ? xmax : xa);
	    R_EmitIslandRuns(top, bottom, xa, xm, islx_lo, islx_hi,
			     depth + 1, flatlump, cm);
	    R_EmitIslandRuns(top, bottom, xm + 1, xb, islx_lo, islx_hi,
			     depth + 1, flatlump, cm);
	    return;
	}
    }

    R_EmitRunPoly(top, bottom, xa, xb, islx_lo, islx_hi, flatlump, cm);
}

// Resolve the run's light table at its representative (mid) distance -- the same
// planezlight index R_MapPlane would compute for that depth (or fixedcolormap).
// Returns the colormap pointer DL_EmitPlanePoly reduces to a PRIM level.
static const void*
R_PlaneRunColormap
( const byte*	top,
  const byte*	bottom,
  int		xa,
  int		xb )
{
    int		xm = (xa + xb) >> 1;
    int		ymid;
    fixed_t	distance;
    unsigned	index;

    if (fixedcolormap)
	return fixedcolormap;

    // Representative row: the vertical mid of the run at its mid column.
    ymid = ((int)top[xm] + (int)bottom[xm] + 1) >> 1;
    if (ymid < 0) ymid = 0;
    if (ymid >= viewheight) ymid = viewheight - 1;

    distance = FixedMul(planeheight, yslope[ymid]);
    index = distance >> LIGHTZSHIFT;
    if (index >= MAXLIGHTZ)
	index = MAXLIGHTZ - 1;
    return planezlight[index];
}

// Walk every NON-SKY visplane and emit its covered islands as trapezoid quads.
// Mirrors R_CountPlanePolyTris's island grouping EXACTLY, but emits. Called from
// R_DrawPlanes when DL_PlanePolyOn(); the per-visplane planeheight/planezlight/
// ds_source bookkeeping is set up identically to the software path first.
static void R_EmitPlanePolys (visplane_t* pl)
{
    int		x;
    int		flatlump = firstflat + flattranslation[pl->picnum];

    x = pl->minx;
    while (x <= pl->maxx)
    {
	int		xa, xb;
	const void*	cm;

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

#if PLANE_GEOM_TRACE
	// New island: publish its [minx,maxx] for the run trace and break the
	// adjacent-run chain so runs of DIFFERENT islands are never paired.
	pgt_island_minx = xa;
	pgt_island_maxx = xb;
	pgt_prev_valid  = 0;
	pgt_run_in_isl  = 0;
	if (PGT_FRAME_HIT())
	    pgt_island_seq++;
#endif

	cm = R_PlaneRunColormap(pl->top, pl->bottom, xa, xb);
	R_EmitIslandRuns(pl->top, pl->bottom, xa, xb, xa, xb, 0, flatlump, cm);
    }
}
#endif // N64


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

#if DPLANES_PROBE
    dpp_lump_tk = dpp_emit_tk = dpp_unproj_tk = 0;
    dpp_scan_cols = dpp_nodes = 0;
#endif

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

#ifdef N64
	// GPU port Phase 3: the baked mesh leaves (DL_DrawMeshLeaves) draw the FLOORS
	// when mesh-floors are on, so suppress the existing floor-visplane emit -- the
	// mesh floors become the sole floor source (and the per-frame visplane
	// tessellation they replace is the CPU cost Phase 3 targets). A floor visplane
	// sits below the eye (height < viewz); ceilings (height >= viewz) + sky stay on
	// the existing path (mesh ceilings are a later slice). NOTE slice-1: a raised
	// floor above the eye is mis-classified as a ceiling here -- a known edge case.
	if (n64_rdp_mesh && pl->picnum != skyflatnum && pl->height < viewz)
	    continue;
#endif


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
#if DPLANES_PROBE
	{ DPP_T0();
#endif
	ds_source = W_CacheLumpNum(firstflat +
				   flattranslation[pl->picnum],
				   PU_STATIC);
#if DPLANES_PROBE
	DPP_ACC(dpp_lump_tk); }
#endif
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

#ifdef N64
	// Stage-4b: emit this visplane as RDP trapezoid POLYGONS instead of the
	// software per-span fill (the count-only probe's EMIT twin). planeheight/
	// planezlight/ds_flatlump are already set up above exactly as software
	// needs, so the corner un-projection reads the same camera state. The CPU
	// span loop is SKIPPED -- the RDP fills these rows, and the keyed present
	// blit shows the RDP floor through the CI8 overlay (DL_FlushPlanePolys).
	if (DL_PlanePolyOn())
	{
#if DPLANES_PROBE
	    { DPP_T0();
	      R_EmitPlanePolys(pl);
	      DPP_ACC(dpp_emit_tk); }
	    { DPP_T0();
	      Z_ChangeTag (ds_source, PU_CACHE);
	      DPP_ACC(dpp_lump_tk); }
#else
	    R_EmitPlanePolys(pl);
	    Z_ChangeTag (ds_source, PU_CACHE);
#endif
	    continue;
	}
#endif

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

#if DPLANES_PROBE
    // Latch the three sub-bracket tick accumulators for this frame. The run-fitter
    // (R_EmitPlanePolys) CALLS the un-projection, so report the pure fitter cost as
    // emit-minus-unproj; n64_bench.c converts to us and reports mean + p95 per part.
    N64Bench_SetDPlanes(dpp_lump_tk,
			(dpp_emit_tk > dpp_unproj_tk) ? (dpp_emit_tk - dpp_unproj_tk) : 0,
			dpp_unproj_tk, dpp_scan_cols, dpp_nodes);
#endif
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
// (The N64 emit block above may have already defined this same value; guard it.)
#ifndef PLANETESS_SPLIT_DEVY
#define PLANETESS_SPLIT_DEVY  1.25f
#endif

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
  int		islx_lo,
  int		islx_hi,
  int		depth )
{
    float	ytl, ytr, ybl, ybr;	// corner Y edges (ybot is bottom+1)
    float	width = (float)(xb + 1 - xa);
    int		x;

    // Corner Y edges -- MUST match R_EmitIslandRuns/R_EmitRunPoly exactly so the
    // count equals what is emitted (fingerprint/tessellation stays consistent).
    // TOP edge: shared-boundary construction (seam fix); BOTTOM: original half-
    // pixel outward extrapolation. A width-1 island gets the column's own edges.
    R_PlaneRunTopCorners(top, xa, xb, islx_lo, islx_hi, &ytl, &ytr);
    {
	float bl0 = (float)bottom[xa] + 1.0f,  br0 = (float)bottom[xb] + 1.0f;

	if (xb > xa)
	{
	    bl0 -= 0.5f * (((float)bottom[xa + 1] + 1.0f) - bl0);
	    br0 += 0.5f * (br0 - ((float)bottom[xb - 1] + 1.0f));
	}
	ybl = bl0;  ybr = br0;
    }

    // Deviation scan: linear-interp each edge across the run and compare to the
    // captured per-column values. MUST match R_EmitIslandRuns EXACTLY (same
    // max-deviation split) so the count equals the emitted poly count. Split at
    // the column of GREATEST top/bottom deviation (Ramer-Douglas-Peucker), not
    // the first deviating column -- the old first-deviation rule needed O(width)
    // recursion levels for a curved edge and hit the depth<10 cap, emitting a
    // wide degenerate trapezoid (see R_EmitIslandRuns). Width-1/2 islands are
    // normally exact by construction, but a width-2 run whose TOP edge steps too
    // steeply must ALSO be split (the +-12-row over-shoot fix).
    if (xb > xa + 1 || (xb > xa && R_PlaneTopStepSteep(top, xa, xb)))
    {
	float	maxdev = 0.0f;
	int	xmax   = -1;
	// IDENTICAL strength-reduction to R_EmitIslandRuns (the emit twin): the
	// per-column f = (x+0.5-xa)/width divide becomes one reciprocal + a multiply,
	// f recomputed from scratch each column. The two twins MUST run the same
	// arithmetic so the counted tessellation equals the emitted poly count (the
	// bench fingerprint); keep this in lockstep with R_EmitIslandRuns.
	float	inv_w = 1.0f / width;

	for (x = xa; x <= xb; x++)
	{
	    float f  = ((float)x + 0.5f - (float)xa) * inv_w;
	    float lt = ytl + (ytr - ytl) * f;
	    float lb = ybl + (ybr - ybl) * f;
	    float dT = lt - (float)top[x];
	    float dB = ((float)bottom[x] + 1.0f) - lb;
	    float aT = (dT < 0.0f) ? -dT : dT;
	    float aB = (dB < 0.0f) ? -dB : dB;
	    float a  = (aT > aB) ? aT : aB;

	    if (a > maxdev) { maxdev = a; xmax = x; }
	}

	if (maxdev > PLANETESS_SPLIT_DEVY && depth < 10)
	{
	    // Split at the worst-error column; keep both halves non-empty so the
	    // recursion always shrinks (matches R_EmitIslandRuns exactly).
	    int xm = (xmax >= xb) ? (xb - 1) : ((xmax > xa) ? xmax : xa);
	    return R_CountIslandRuns(top, bottom, xa, xm,
				     islx_lo, islx_hi, depth + 1)
		 + R_CountIslandRuns(top, bottom, xm + 1, xb,
				     islx_lo, islx_hi, depth + 1);
	}
    }

    // One trapezoid run covers this island span -- but a deep run is emitted as
    // R_PlaneRunBands() depth bands (the INV_W-ratio floor-garbage fix), so the
    // count must equal the emitted band count, not 1. Same corner Ys as emit.
    return R_PlaneRunBands(ytl, ytr, ybl, ybr);
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

	    total_runs += R_CountIslandRuns(pl->top, pl->bottom, xa, xb,
					    xa, xb, 0);
	}
    }

    return total_runs * 2;	// 2 triangles per trapezoid run
}
#endif	// N64_BENCH && PLANETESS_COUNT
