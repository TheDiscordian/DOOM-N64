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
//	Rendering main loop and setup functions,
//	 utility functions (BSP, geometry, trigonometry).
//	See tables.c, too.
//
//-----------------------------------------------------------------------------


static const char rcsid[] = "$Id: r_main.c,v 1.5 1997/02/03 22:45:12 b1 Exp $";



#include <stdlib.h>
#include <math.h>


#include "doomdef.h"
#include "d_net.h"

#include "m_bbox.h"

#include "r_local.h"
#include "r_sky.h"

#include "doomstat.h"
#ifdef N64
#include "i_video.h"
#endif

#ifdef N64_BENCH
#include "n64_bench.h"
// visplane pool pointers are file-static in r_plane.c; pull them in for the
// per-frame visplane count (realloc-grown pool: count = lastvisplane-visplanes).
extern visplane_t*	visplanes;
extern visplane_t*	lastvisplane;
#endif




// Fineangles in the SCREENWIDTH wide window.
#define FIELDOFVIEW		2048

// Horizontal projection factor for 16:9 output: the framebuffer is stretched
// 4:3 -> 16:9 by the display, so render compressed to 3/4 width (Hor+, the
// vertical FOV is unchanged and the horizontal FOV widens to ~106 degrees).
#define WIDESCREEN_HSCALE	(FRACUNIT*3/4)



int			viewangleoffset;

// increment every time a check is made
int			validcount = 1;		


lighttable_t*		fixedcolormap;
extern lighttable_t**	walllights;

int			centerx;
int			centery;

fixed_t			centerxfrac;
fixed_t			centeryfrac;
fixed_t			projection;
fixed_t			projectiony;

// just for profiling purposes
int			framecount;	

int			sscount;
int			linecount;
int			loopcount;

fixed_t			viewx;
fixed_t			viewy;
fixed_t			viewz;

// Uncapped framerate: when r_interpolate is set, the view and sprites are
// lerped between the previous and current tic by fractionaltic [0, FRACUNIT].
boolean			r_interpolate = false;
fixed_t			fractionaltic = 0;

angle_t			viewangle;

fixed_t			viewcos;
fixed_t			viewsin;

player_t*		viewplayer;

// 0 = high, 1 = low
int			detailshift;	

//
// precalculated math tables
//
angle_t			clipangle;

// The viewangletox[viewangle + FINEANGLES/4] lookup
// maps the visible view angles to screen X coordinates,
// flattening the arc to a flat projection plane.
// There will be many angles mapped to the same X. 
int			viewangletox[FINEANGLES/2];

// The xtoviewangleangle[] table maps a screen pixel
// to the lowest viewangle that maps back to x ranges
// from clipangle to -clipangle.
angle_t			xtoviewangle[SCREENWIDTH+1];


// UNUSED.
// The finetangentgent[angle+FINEANGLES/4] table
// holds the fixed_t tangent values for view angles,
// ranging from MININT to 0 to MAXINT.
// fixed_t		finetangent[FINEANGLES/2];

// fixed_t		finesine[5*FINEANGLES/4];
fixed_t*		finecosine = &finesine[FINEANGLES/4];


lighttable_t*		scalelight[LIGHTLEVELS][MAXLIGHTSCALE];
lighttable_t*		scalelightfixed[MAXLIGHTSCALE];
lighttable_t*		zlight[LIGHTLEVELS][MAXLIGHTZ];

// bumped light from gun blasts
int			extralight;			



void (*colfunc) (void);
void (*basecolfunc) (void);
void (*fuzzcolfunc) (void);
void (*transcolfunc) (void);
void (*spanfunc) (void);



//
// R_AddPointToBox
// Expand a given bbox
// so that it encloses a given point.
//
void
R_AddPointToBox
( int		x,
  int		y,
  fixed_t*	box )
{
    if (x< box[BOXLEFT])
	box[BOXLEFT] = x;
    if (x> box[BOXRIGHT])
	box[BOXRIGHT] = x;
    if (y< box[BOXBOTTOM])
	box[BOXBOTTOM] = y;
    if (y> box[BOXTOP])
	box[BOXTOP] = y;
}


//
// R_PointOnSide
// Traverse BSP (sub) tree,
//  check point against partition plane.
// Returns side 0 (front) or 1 (back).
//
int
R_PointOnSide
( fixed_t	x,
  fixed_t	y,
  node_t*	node )
{
    fixed_t	dx;
    fixed_t	dy;
    fixed_t	left;
    fixed_t	right;
	
    if (!node->dx)
    {
	if (x <= node->x)
	    return node->dy > 0;
	
	return node->dy < 0;
    }
    if (!node->dy)
    {
	if (y <= node->y)
	    return node->dx < 0;
	
	return node->dx > 0;
    }
	
    dx = (x - node->x);
    dy = (y - node->y);
	
    // Try to quickly decide by looking at sign bits.
    if ( (node->dy ^ node->dx ^ dx ^ dy)&0x80000000 )
    {
	if  ( (node->dy ^ dx) & 0x80000000 )
	{
	    // (left is negative)
	    return 1;
	}
	return 0;
    }

    left = FixedMul ( node->dy>>FRACBITS , dx );
    right = FixedMul ( dy , node->dx>>FRACBITS );
	
    if (right < left)
    {
	// front side
	return 0;
    }
    // back side
    return 1;			
}


int
R_PointOnSegSide
( fixed_t	x,
  fixed_t	y,
  seg_t*	line )
{
    fixed_t	lx;
    fixed_t	ly;
    fixed_t	ldx;
    fixed_t	ldy;
    fixed_t	dx;
    fixed_t	dy;
    fixed_t	left;
    fixed_t	right;
	
    lx = line->v1->x;
    ly = line->v1->y;
	
    ldx = line->v2->x - lx;
    ldy = line->v2->y - ly;
	
    if (!ldx)
    {
	if (x <= lx)
	    return ldy > 0;
	
	return ldy < 0;
    }
    if (!ldy)
    {
	if (y <= ly)
	    return ldx < 0;
	
	return ldx > 0;
    }
	
    dx = (x - lx);
    dy = (y - ly);
	
    // Try to quickly decide by looking at sign bits.
    if ( (ldy ^ ldx ^ dx ^ dy)&0x80000000 )
    {
	if  ( (ldy ^ dx) & 0x80000000 )
	{
	    // (left is negative)
	    return 1;
	}
	return 0;
    }

    left = FixedMul ( ldy>>FRACBITS , dx );
    right = FixedMul ( dy , ldx>>FRACBITS );
	
    if (right < left)
    {
	// front side
	return 0;
    }
    // back side
    return 1;			
}


//
// R_PointToAngle
// To get a global angle from cartesian coordinates,
//  the coordinates are flipped until they are in
//  the first octant of the coordinate system, then
//  the y (<=x) is scaled and divided by x to get a
//  tangent (slope) value which is looked up in the
//  tantoangle[] table.

//




angle_t
R_PointToAngle
( fixed_t	x,
  fixed_t	y )
{	
    x -= viewx;
    y -= viewy;
    
    if ( (!x) && (!y) )
	return 0;

    if (x>= 0)
    {
	// x >=0
	if (y>= 0)
	{
	    // y>= 0

	    if (x>y)
	    {
		// octant 0
		return tantoangle[ SlopeDiv(y,x)];
	    }
	    else
	    {
		// octant 1
		return ANG90-1-tantoangle[ SlopeDiv(x,y)];
	    }
	}
	else
	{
	    // y<0
	    y = -y;

	    if (x>y)
	    {
		// octant 8
		return -tantoangle[SlopeDiv(y,x)];
	    }
	    else
	    {
		// octant 7
		return ANG270+tantoangle[ SlopeDiv(x,y)];
	    }
	}
    }
    else
    {
	// x<0
	x = -x;

	if (y>= 0)
	{
	    // y>= 0
	    if (x>y)
	    {
		// octant 3
		return ANG180-1-tantoangle[ SlopeDiv(y,x)];
	    }
	    else
	    {
		// octant 2
		return ANG90+ tantoangle[ SlopeDiv(x,y)];
	    }
	}
	else
	{
	    // y<0
	    y = -y;

	    if (x>y)
	    {
		// octant 4
		return ANG180+tantoangle[ SlopeDiv(y,x)];
	    }
	    else
	    {
		 // octant 5
		return ANG270-1-tantoangle[ SlopeDiv(x,y)];
	    }
	}
    }
    return 0;
}


angle_t
R_PointToAngle2
( fixed_t	x1,
  fixed_t	y1,
  fixed_t	x2,
  fixed_t	y2 )
{	
    viewx = x1;
    viewy = y1;
    
    return R_PointToAngle (x2, y2);
}


fixed_t
R_PointToDist
( fixed_t	x,
  fixed_t	y )
{
    int		angle;
    fixed_t	dx;
    fixed_t	dy;
    fixed_t	temp;
    fixed_t	dist;
	
    dx = abs(x - viewx);
    dy = abs(y - viewy);
	
    if (dy>dx)
    {
	temp = dx;
	dx = dy;
	dy = temp;
    }
	
    angle = (tantoangle[ FixedDiv(dy,dx)>>DBITS ]+ANG90) >> ANGLETOFINESHIFT;

    // use as cosine
    dist = FixedDiv (dx, finesine[angle] );	
	
    return dist;
}




//
// R_InitPointToAngle
//
void R_InitPointToAngle (void)
{
    // UNUSED - now getting from tables.c
#if 0
    int	i;
    long	t;
    float	f;
//
// slope (tangent) to angle lookup
//
    for (i=0 ; i<=SLOPERANGE ; i++)
    {
	f = atan( (float)i/SLOPERANGE )/(3.141592657*2);
	t = 0xffffffff*f;
	tantoangle[i] = t;
    }
#endif
}


//
// R_ScaleFromGlobalAngle
// Returns the texture mapping scale
//  for the current line (horizontal span)
//  at the given angle.
// rw_distance must be calculated first.
//
fixed_t R_ScaleFromGlobalAngle (angle_t visangle)
{
    fixed_t		scale;
    int			anglea;
    int			angleb;
    int			sinea;
    int			sineb;
    fixed_t		num;
    int			den;

    // UNUSED
#if 0
{
    fixed_t		dist;
    fixed_t		z;
    fixed_t		sinv;
    fixed_t		cosv;
	
    sinv = finesine[(visangle-rw_normalangle)>>ANGLETOFINESHIFT];	
    dist = FixedDiv (rw_distance, sinv);
    cosv = finecosine[(viewangle-visangle)>>ANGLETOFINESHIFT];
    z = abs(FixedMul (dist, cosv));
    scale = FixedDiv(projection, z);
    return scale;
}
#endif

    anglea = ANG90 + (visangle-viewangle);
    angleb = ANG90 + (visangle-rw_normalangle);

    // both sines are allways positive
    sinea = finesine[anglea>>ANGLETOFINESHIFT];	
    sineb = finesine[angleb>>ANGLETOFINESHIFT];
    num = FixedMul(projectiony,sineb)<<detailshift;
    den = FixedMul(rw_distance,sinea);

    if (den > num>>16)
    {
	scale = FixedDiv (num, den);

	if (scale > 64*FRACUNIT)
	    scale = 64*FRACUNIT;
	else if (scale < 256)
	    scale = 256;
    }
    else
	scale = 64*FRACUNIT;
	
    return scale;
}



//
// R_InitTables
//
void R_InitTables (void)
{
    // UNUSED: now getting from tables.c
#if 0
    int		i;
    float	a;
    float	fv;
    int		t;
    
    // viewangle tangent table
    for (i=0 ; i<FINEANGLES/2 ; i++)
    {
	a = (i-FINEANGLES/4+0.5)*PI*2/FINEANGLES;
	fv = FRACUNIT*tan (a);
	t = fv;
	finetangent[i] = t;
    }
    
    // finesine table
    for (i=0 ; i<5*FINEANGLES/4 ; i++)
    {
	// OPTIMIZE: mirror...
	a = (i+0.5)*PI*2/FINEANGLES;
	t = FRACUNIT*sin (a);
	finesine[i] = t;
    }
#endif

}



//
// R_InitTextureMapping
//
void R_InitTextureMapping (void)
{
    int			i;
    int			x;
    int			t;
    fixed_t		focallength;
    
    // Use tangent table to generate viewangletox:
    //  viewangletox will give the next greatest x
    //  after the view angle.
    //
    // Calc focallength
    //  so FIELDOFVIEW angles covers SCREENWIDTH.
    focallength = FixedDiv (projection,
			    finetangent[FINEANGLES/4+FIELDOFVIEW/2] );
	
    for (i=0 ; i<FINEANGLES/2 ; i++)
    {
	if (finetangent[i] > FRACUNIT*2)
	    t = -1;
	else if (finetangent[i] < -FRACUNIT*2)
	    t = viewwidth+1;
	else
	{
	    t = FixedMul (finetangent[i], focallength);
	    t = (centerxfrac - t+FRACUNIT-1)>>FRACBITS;

	    if (t < -1)
		t = -1;
	    else if (t>viewwidth+1)
		t = viewwidth+1;
	}
	viewangletox[i] = t;
    }
    
    // Scan viewangletox[] to generate xtoviewangle[]:
    //  xtoviewangle will give the smallest view angle
    //  that maps to x.	
    for (x=0;x<=viewwidth;x++)
    {
	i = 0;
	while (viewangletox[i]>x)
	    i++;
	xtoviewangle[x] = (i<<ANGLETOFINESHIFT)-ANG90;
    }
    
    // Take out the fencepost cases from viewangletox.
    for (i=0 ; i<FINEANGLES/2 ; i++)
    {
	t = FixedMul (finetangent[i], focallength);
	t = centerx - t;
	
	if (viewangletox[i] == -1)
	    viewangletox[i] = 0;
	else if (viewangletox[i] == viewwidth+1)
	    viewangletox[i]  = viewwidth;
    }
	
    clipangle = xtoviewangle[0];
}



//
// R_InitLightTables
// Only inits the zlight table,
//  because the scalelight table changes with view size.
//
#define DISTMAP		2

void R_InitLightTables (void)
{
    int		i;
    int		j;
    int		level;
    int		startmap; 	
    int		scale;
    
    // Calculate the light levels to use
    //  for each level / distance combination.
    for (i=0 ; i< LIGHTLEVELS ; i++)
    {
	startmap = ((LIGHTLEVELS-1-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;
	for (j=0 ; j<MAXLIGHTZ ; j++)
	{
	    scale = FixedDiv ((SCREENWIDTH/2*FRACUNIT), (j+1)<<LIGHTZSHIFT);
	    scale >>= LIGHTSCALESHIFT;
	    level = startmap - scale/DISTMAP;
	    
	    if (level < 0)
		level = 0;

	    if (level >= NUMCOLORMAPS)
		level = NUMCOLORMAPS-1;

	    zlight[i][j] = colormaps + level*256;
	}
    }
}



//
// R_SetViewSize
// Do not really change anything here,
//  because it might be in the middle of a refresh.
// The change will take effect next refresh.
//
boolean		setsizeneeded;
int		setblocks;
int		setdetail;
#ifdef N64
static boolean	n64_viewsize_override;
static int	n64_override_width;
static int	n64_override_height;
#endif


void
R_SetViewSize
( int		blocks,
  int		detail )
{
    setsizeneeded = true;
    setblocks = blocks;
    setdetail = detail;
}

#ifdef N64
void R_SetN64ViewSize(int width, int height, int detail)
{
    if (width < 1 || height < 1)
        return;

    if (width > SCREENWIDTH)
        width = SCREENWIDTH;

    if (height > SCREENHEIGHT)
        height = SCREENHEIGHT;

    n64_viewsize_override = true;
    n64_override_width = width;
    n64_override_height = height;
    setdetail = detail;
    setsizeneeded = true;
}

void R_ClearN64ViewSizeOverride(void)
{
    n64_viewsize_override = false;
}
#endif


//
// R_ExecuteSetViewSize
//
void R_ExecuteSetViewSize (void)
{
    fixed_t	cosadj;
    fixed_t	dy;
    fixed_t	basefocal;
    int		i;
    int		j;
    int		level;
    int		startmap; 	

    setsizeneeded = false;

#ifdef N64
    if (n64_viewsize_override)
    {
	scaledviewwidth = n64_override_width;
	viewheight = n64_override_height;
    }
    else
#endif
    if (setblocks == 11)
    {
	scaledviewwidth = SCREENWIDTH;
	viewheight = SCREENHEIGHT;
    }
    else
    {
	scaledviewwidth = setblocks*32;
	viewheight = (setblocks*168/10)&~7;
    }
    
    detailshift = setdetail;
    viewwidth = scaledviewwidth>>detailshift;
	
    centery = viewheight/2;
    centerx = viewwidth/2;
    centerxfrac = centerx<<FRACBITS;
    centeryfrac = centery<<FRACBITS;

    // Base focal length. Normal views get the vanilla 90-degree FOV (focal =
    // half the view width). Split panes are the 1p camera scaled by the
    // pane's larger relative dimension, cropped on the other axis: a
    // same-aspect pane (4p quadrant) is an exact miniature of 1p, a
    // half-width full-height pane (vertical 2p) shows half the tangent
    // range instead of 90 degrees squeezed into it.
    basefocal = centerxfrac;
#ifdef N64
    if (n64_viewsize_override)
    {
	fixed_t ratio = FRACUNIT*scaledviewwidth/SCREENWIDTH;
	if (ratio < FRACUNIT*viewheight/SCREENHEIGHT)
	    ratio = FRACUNIT*viewheight/SCREENHEIGHT;
	basefocal = FixedMul(((SCREENWIDTH/2)<<FRACBITS)>>detailshift, ratio);
    }
#endif
    projection = basefocal;
    projectiony = basefocal;
    if (widescreen)
	projection = FixedMul(basefocal, WIDESCREEN_HSCALE);

    if (!detailshift)
    {
	colfunc = basecolfunc = R_DrawColumn;
	fuzzcolfunc = R_DrawFuzzColumn;
	transcolfunc = R_DrawTranslatedColumn;
	spanfunc = R_DrawSpan;
    }
    else
    {
	colfunc = basecolfunc = R_DrawColumnLow;
    fuzzcolfunc = R_DrawFuzzColumnLow;
    transcolfunc = R_DrawTranslatedColumnLow;
	spanfunc = R_DrawSpanLow;
    }

    R_InitBuffer (scaledviewwidth, viewheight);
	
    R_InitTextureMapping ();
    
    // psprite scales
    pspritescale = FRACUNIT*viewwidth/SCREENWIDTH;
    pspriteiscale = FRACUNIT*SCREENWIDTH/viewwidth;

    // Weapon draw scale: the larger axis ratio, so a tall pane (vertical
    // split) draws a full-size weapon instead of a width-keyed half-size
    // one. Uniform on both axes; the sky keeps using pspriteiscale.
    pspritedrawscale = (FRACUNIT*viewheight/SCREENHEIGHT) >> detailshift;
    if (pspritedrawscale < pspritescale)
	pspritedrawscale = pspritescale;
    pspritedrawiscale = FixedDiv(FRACUNIT, pspritedrawscale);

    // Weapon horizontal placement/step, compressed like the world in 16:9.
    pspritedrawxscale = pspritedrawscale;
    if (widescreen)
	pspritedrawxscale = FixedMul(pspritedrawscale, WIDESCREEN_HSCALE);
    pspritedrawixscale = FixedDiv(FRACUNIT, pspritedrawxscale);
    
    // thing clipping
    for (i=0 ; i<viewwidth ; i++)
	screenheightarray[i] = viewheight;
    
    // planes
    for (i=0 ; i<viewheight ; i++)
    {
	dy = ((i-viewheight/2)<<FRACBITS)+FRACUNIT/2;
	dy = abs(dy);
	yslope[i] = FixedDiv (projectiony<<detailshift, dy);
    }
	
    for (i=0 ; i<viewwidth ; i++)
    {
	cosadj = abs(finecosine[xtoviewangle[i]>>ANGLETOFINESHIFT]);
	distscale[i] = FixedDiv (FRACUNIT,cosadj);
    }
    
    // Calculate the light levels to use
    //  for each level / scale combination.
    for (i=0 ; i< LIGHTLEVELS ; i++)
    {
	startmap = ((LIGHTLEVELS-1-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;
	for (j=0 ; j<MAXLIGHTSCALE ; j++)
	{
	    level = startmap - j*SCREENWIDTH/(viewwidth<<detailshift)/DISTMAP;
	    
	    if (level < 0)
		level = 0;

	    if (level >= NUMCOLORMAPS)
		level = NUMCOLORMAPS-1;

	    scalelight[i][j] = colormaps + level*256;
	}
    }
}



//
// R_Init
//
extern int	detailLevel;
extern int	screenblocks;



void R_Init (void)
{
    R_InitData ();
    printf ("\nR_InitData");
    R_InitPointToAngle ();
    printf ("\nR_InitPointToAngle");
    R_InitTables ();
    // viewwidth / viewheight / detailLevel are set by the defaults
    printf ("\nR_InitTables");

    R_SetViewSize (screenblocks, detailLevel);
    R_InitPlanes ();
    printf ("\nR_InitPlanes");
    R_InitLightTables ();
    printf ("\nR_InitLightTables");
    R_InitSkyMap ();
    printf ("\nR_InitSkyMap");
    R_InitTranslationTables ();
    printf ("\nR_InitTranslationsTables");
	
    framecount = 0;
}


//
// R_PointInSubsector
//
subsector_t*
R_PointInSubsector
( fixed_t	x,
  fixed_t	y )
{
    node_t*	node;
    int		side;
    int		nodenum;

    // single subsector is a special case
    if (!numnodes)				
	return subsectors;
		
    nodenum = numnodes-1;

    while (! (nodenum & NF_SUBSECTOR) )
    {
	node = &nodes[nodenum];
	side = R_PointOnSide (x, y, node);
	nodenum = node->children[side];
    }
	
    return &subsectors[nodenum & ~NF_SUBSECTOR];
}



//
// R_SetupFrame
//
void R_SetupFrame (player_t* player)
{		
    int		i;
    
    viewplayer = player;

    // Interpolate the camera between tics so the view moves at display rate
    // instead of snapping to 35 Hz tic boundaries.
    if (r_interpolate)
    {
	mobj_t*	mo = player->mo;
	int	adiff;

	viewx = mo->oldx + FixedMul(mo->x - mo->oldx, fractionaltic);
	viewy = mo->oldy + FixedMul(mo->y - mo->oldy, fractionaltic);
	viewz = player->oldviewz
	      + FixedMul(player->viewz - player->oldviewz, fractionaltic);

	// Short-arc angle delta via unsigned-sub + signed cast; multiply in
	// 64 bits since a 180 deg delta << fractionaltic overflows 32 bits.
	adiff = (int)(mo->angle - mo->oldangle);
	viewangle = mo->oldangle
		  + (angle_t)(((long long)adiff * fractionaltic) >> FRACBITS)
		  + viewangleoffset;
    }
    else
    {
	viewx = player->mo->x;
	viewy = player->mo->y;
	viewangle = player->mo->angle + viewangleoffset;
	viewz = player->viewz;
    }

    extralight = player->extralight;

    viewsin = finesine[viewangle>>ANGLETOFINESHIFT];
    viewcos = finecosine[viewangle>>ANGLETOFINESHIFT];
	
    sscount = 0;
	
    if (player->fixedcolormap)
    {
	fixedcolormap =
	    colormaps
	    + player->fixedcolormap*256*sizeof(lighttable_t);
	
	walllights = scalelightfixed;

	for (i=0 ; i<MAXLIGHTSCALE ; i++)
	    scalelightfixed[i] = fixedcolormap;
    }
    else
	fixedcolormap = 0;
		
    framecount++;
    validcount++;
}



//
// R_RenderView
//
void R_RenderPlayerView (player_t* player)
{
    R_SetupFrame (player);

    // RDP renderer dispatch (kill-switch). n64_use_rdp_renderer selects the
    // RDP-rasterized path over the software colfunc/spanfunc fill. The world
    // pass is built up stage by stage; in this stage the view is still 100%
    // software-rendered into the CI8 buffer, so the visible result is identical
    // regardless of the flag.
    //
    // Erase-to-key is EVENT-DRIVEN (Stage-2 stale-key fix): the seg loop
    // writes the key index into exactly the routed seg's suppressed column
    // spans (R_FillColumnKey, r_segs.c), so key pixels can only exist where
    // an emitted record covers them and the keyed present blit punches them
    // out. The Stage-1 full-view pre-clear (I_N64KeyClearView) is retired:
    // it relied on "software covers every view pixel", which vanilla does
    // not guarantee -- rare per-column coverage gaps kept key pixels OUTSIDE
    // the keyed box and the present blitted them opaque as the key colour
    // (the 28-px stale-key sparkle, trace DL_KEYSCAN p=1659). Gap pixels now
    // keep stale buffer content, byte-identical to the flag-off software
    // path. The KEY_CLEAR bench phase stays in the enum (reads 0).

    // Clear buffers.
    R_ClearClipSegs ();
    R_ClearDrawSegs ();
    R_ClearPlanes ();
    R_ClearSprites ();
    
    // check for new console commands.
    NetUpdate ();

    // The head node is the last node output. The phase open across the BSP
    // walk is BSP_WALK; R_RenderSegLoop switches to SEG_RASTER around its
    // per-column fill and back, so the wall raster cost is attributed
    // separately (the RDP renderer offloads SEG_RASTER, keeps BSP_WALK).
#ifdef N64_BENCH
    N64Bench_PhaseBegin(BPH_BSP_WALK);
    R_RenderBSPNode (numnodes-1);
#else
    R_RenderBSPNode (numnodes-1);
#endif

    // Mid-render NetUpdate keeps the net serviced during a long frame. In
    // 1p there is no net to service, so it only burns I_GetTime + joypad
    // polling; gate on netgame (true for local split-screen MP too).
    if (netgame)
	NetUpdate ();

#ifdef N64_BENCH
    N64Bench_PhaseSwitch(BPH_BSP_WALK, BPH_PLANES);   // one read closes bsp walk, opens planes
    R_DrawPlanes ();
    // PLANE_EMIT brackets the RDP renderer's plane-span emit, which will live
    // inside R_DrawPlanes once planes move to the RDP. ~0 in this stage.
    N64Bench_PhaseBegin(BPH_PLANE_EMIT);
    N64Bench_PhaseEnd(BPH_PLANE_EMIT);
#else
    R_DrawPlanes ();
#endif

    if (netgame)
	NetUpdate ();

#ifdef N64_BENCH
    N64Bench_PhaseSwitch(BPH_PLANES, BPH_MASKED);
    R_DrawMasked ();
    N64Bench_PhaseEnd(BPH_MASKED);
    // MASKED_EMIT brackets the RDP renderer's sprite/masked emit, which will
    // live inside R_DrawMasked once sprites move to the RDP. ~0 in this stage.
    N64Bench_PhaseBegin(BPH_MASKED_EMIT);
    N64Bench_PhaseEnd(BPH_MASKED_EMIT);
    // Latch per-frame work counts while the pools are still full (before the
    // next frame's R_Clear* resets them). vissprites/drawsegs are end-pointer
    // minus base; visplanes is the realloc-grown pool's used span.
    N64Bench_SetCounts((int)(vissprite_p - vissprites),
		       (int)(ds_p - drawsegs),
		       (int)(lastvisplane - visplanes));
#else
    R_DrawMasked ();
#endif

    // Check for new console commands.
    NetUpdate ();
}
