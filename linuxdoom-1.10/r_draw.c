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
//	The actual span/column drawing functions.
//	Here find the main potential for optimization,
//	 e.g. inline assembly, different algorithms.
//
//-----------------------------------------------------------------------------


static const char
rcsid[] = "$Id: r_draw.c,v 1.4 1997/02/03 16:47:55 b1 Exp $";


#include <stdint.h>

#include "doomdef.h"

#include "i_system.h"
#include "z_zone.h"
#include "w_wad.h"

#include "r_local.h"

// Needs access to LFB (guess what).
#include "v_video.h"

// State.
#include "doomstat.h"


// ?
#define MAXWIDTH			1120
#define MAXHEIGHT			832

// status bar height at bottom of screen
#define SBARHEIGHT		32

//
// All drawing to the view buffer is accomplished in this file.
// The other refresh files only know about ccordinates,
//  not the architecture of the frame buffer.
// Conveniently, the frame buffer is a linear one,
//  and we need only the base address,
//  and the total size == width*height*depth/8.,
//


byte*		viewimage; 
int		viewwidth;
int		scaledviewwidth;
int		viewheight;
int		viewwindowx;
int		viewwindowy; 
byte*		ylookup[MAXHEIGHT]; 
int		columnofs[MAXWIDTH]; 

// Color tables for different players,
//  translate a limited part to another
//  (color ramps used for  suit colors).
//
byte		translations[3][256];	
 
 


//
// R_DrawColumn
// Source is the top of the column to scale.
//
lighttable_t*		dc_colormap; 
int			dc_x; 
int			dc_yl; 
int			dc_yh; 
fixed_t			dc_iscale; 
fixed_t			dc_texturemid;

// first pixel in a column (possibly virtual) 
byte*			dc_source;		

// just for profiling 
int			dccount;

//
// A column is a vertical slice/span from a wall texture that,
//  given the DOOM style restrictions on the view orientation,
//  will always have constant z depth.
// Thus a special case loop for very fast rendering can
//  be used. It has also been used with Wolfenstein 3D.
// 
void R_DrawColumn (void) 
{ 
    int			count; 
    byte*		dest; 
    fixed_t		frac;
    fixed_t		fracstep;	 
 
    count = dc_yh - dc_yl; 

    // Zero length, column does not exceed a pixel.
    if (count < 0) 
	return; 
				 
#ifdef RANGECHECK 
    if ((unsigned)dc_x >= SCREENWIDTH
	|| dc_yl < 0
	|| dc_yh >= SCREENHEIGHT) 
	I_Error ("R_DrawColumn: %i to %i at %i", dc_yl, dc_yh, dc_x); 
#endif 

    // Framebuffer destination address.
    // Use ylookup LUT to avoid multiply with ScreenWidth.
    // Use columnofs LUT for subwindows?
    dest = ylookup[dc_yl] + columnofs[dc_x];

    // Determine scaling,
    //  which is the only mapping to be done.
    fracstep = dc_iscale;
    frac = dc_texturemid + (dc_yl-centery)*fracstep;

    // Cache globals in locals so the per-pixel store (through a byte pointer,
    //  which aliases everything) cannot force a reload of these each iteration.
    {
	register byte*			source = dc_source;
	register lighttable_t*		colormap = dc_colormap;

	// Inner loop that does the actual texture mapping,
	//  e.g. a DDA-lile scaling.
	// This is as fast as it gets.
	do
	{
	    // Re-map color indices from wall texture column
	    //  using a lighting/special effects LUT.
	    *dest = colormap[source[(frac>>FRACBITS)&127]];

	    dest += SCREENWIDTH;
	    frac += fracstep;

	} while (count--);
    }
}



// UNUSED.
// Loop unrolled.
#if 0
void R_DrawColumn (void) 
{ 
    int			count; 
    byte*		source;
    byte*		dest;
    byte*		colormap;
    
    unsigned		frac;
    unsigned		fracstep;
    unsigned		fracstep2;
    unsigned		fracstep3;
    unsigned		fracstep4;	 
 
    count = dc_yh - dc_yl + 1; 

    source = dc_source;
    colormap = dc_colormap;		 
    dest = ylookup[dc_yl] + columnofs[dc_x];  
	 
    fracstep = dc_iscale<<9; 
    frac = (dc_texturemid + (dc_yl-centery)*dc_iscale)<<9; 
 
    fracstep2 = fracstep+fracstep;
    fracstep3 = fracstep2+fracstep;
    fracstep4 = fracstep3+fracstep;
	
    while (count >= 8) 
    { 
	dest[0] = colormap[source[frac>>25]]; 
	dest[SCREENWIDTH] = colormap[source[(frac+fracstep)>>25]]; 
	dest[SCREENWIDTH*2] = colormap[source[(frac+fracstep2)>>25]]; 
	dest[SCREENWIDTH*3] = colormap[source[(frac+fracstep3)>>25]];
	
	frac += fracstep4; 

	dest[SCREENWIDTH*4] = colormap[source[frac>>25]]; 
	dest[SCREENWIDTH*5] = colormap[source[(frac+fracstep)>>25]]; 
	dest[SCREENWIDTH*6] = colormap[source[(frac+fracstep2)>>25]]; 
	dest[SCREENWIDTH*7] = colormap[source[(frac+fracstep3)>>25]]; 

	frac += fracstep4; 
	dest += SCREENWIDTH*8; 
	count -= 8;
    } 
	
    while (count > 0)
    { 
	*dest = colormap[source[frac>>25]]; 
	dest += SCREENWIDTH; 
	frac += fracstep; 
	count--;
    } 
}
#endif


void R_DrawColumnLow (void) 
{ 
    int			count; 
    byte*		dest; 
    byte*		dest2;
    fixed_t		frac;
    fixed_t		fracstep;	 
 
    count = dc_yh - dc_yl; 

    // Zero length.
    if (count < 0) 
	return; 
				 
#ifdef RANGECHECK 
    if ((unsigned)dc_x >= SCREENWIDTH
	|| dc_yl < 0
	|| dc_yh >= SCREENHEIGHT)
    {
	
	I_Error ("R_DrawColumn: %i to %i at %i", dc_yl, dc_yh, dc_x);
    }
    //	dccount++; 
#endif 
    // Blocky mode, need to multiply by 2.
    dc_x <<= 1;
    
    dest = ylookup[dc_yl] + columnofs[dc_x];
    dest2 = ylookup[dc_yl] + columnofs[dc_x+1];

    fracstep = dc_iscale;
    frac = dc_texturemid + (dc_yl-centery)*fracstep;

    {
	register byte*			source = dc_source;
	register lighttable_t*		colormap = dc_colormap;

	// dest2 == dest+1 (adjacent framebuffer columns). When the pair is
	// 2-byte aligned, write both with one halfword store; otherwise an
	// unaligned `sh` would address-error, so fall back to two byte stores.
	if (!(((uintptr_t)dest) & 1))
	{
	    do
	    {
		register byte texel = colormap[source[(frac>>FRACBITS)&127]];
		*(uint16_t*)dest = (texel << 8) | texel;
		dest += SCREENWIDTH;
		frac += fracstep;
	    } while (count--);
	}
	else
	{
	    do
	    {
		// Hack. Does not work corretly.
		*dest2 = *dest = colormap[source[(frac>>FRACBITS)&127]];
		dest += SCREENWIDTH;
		dest2 += SCREENWIDTH;
		frac += fracstep;
	    } while (count--);
	}
    }
}


//
// Spectre/Invisibility.
//
#define FUZZTABLE		50 
#define FUZZOFF	(SCREENWIDTH)


int	fuzzoffset[FUZZTABLE] =
{
    FUZZOFF,-FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
    FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
    FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,
    FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
    FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,
    FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,
    FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF 
}; 

int	fuzzpos = 0; 


//
// Framebuffer postprocessing.
// Creates a fuzzy image by copying pixels
//  from adjacent ones to left and right.
// Used with an all black colormap, this
//  could create the SHADOW effect,
//  i.e. spectres and invisible players.
//
void R_DrawFuzzColumn (void) 
{ 
    int			count; 
    byte*		dest; 
    fixed_t		frac;
    fixed_t		fracstep;	 

    // Adjust borders. Low... 
    if (!dc_yl) 
	dc_yl = 1;

    // .. and high.
    if (dc_yh == viewheight-1) 
	dc_yh = viewheight - 2; 
		 
    count = dc_yh - dc_yl; 

    // Zero length.
    if (count < 0) 
	return; 

    
#ifdef RANGECHECK 
    if ((unsigned)dc_x >= SCREENWIDTH
	|| dc_yl < 0 || dc_yh >= SCREENHEIGHT)
    {
	I_Error ("R_DrawFuzzColumn: %i to %i at %i",
		 dc_yl, dc_yh, dc_x);
    }
#endif


    // Keep till detailshift bug in blocky mode fixed,
    //  or blocky mode removed.
    /* WATCOM code 
    if (detailshift)
    {
	if (dc_x & 1)
	{
	    outpw (GC_INDEX,GC_READMAP+(2<<8) ); 
	    outp (SC_INDEX+1,12); 
	}
	else
	{
	    outpw (GC_INDEX,GC_READMAP); 
	    outp (SC_INDEX+1,3); 
	}
	dest = destview + dc_yl*80 + (dc_x>>1); 
    }
    else
    {
	outpw (GC_INDEX,GC_READMAP+((dc_x&3)<<8) ); 
	outp (SC_INDEX+1,1<<(dc_x&3)); 
	dest = destview + dc_yl*80 + (dc_x>>2); 
    }*/

    
    // Does not work with blocky mode.
    dest = ylookup[dc_yl] + columnofs[dc_x];

    // Looks familiar.
    fracstep = dc_iscale; 
    frac = dc_texturemid + (dc_yl-centery)*fracstep; 

    // Looks like an attempt at dithering,
    //  using the colormap #6 (of 0-31, a bit
    //  brighter than average).
    {
	register lighttable_t*	fuzzmap = colormaps + 6*256;
	register int		pos = fuzzpos;

	do
	{
	    // Lookup framebuffer, and retrieve
	    //  a pixel that is either one column
	    //  left or right of the current one.
	    // Add index from colormap to index.
	    *dest = fuzzmap[dest[fuzzoffset[pos]]];

	    // Clamp table lookup index.
	    if (++pos == FUZZTABLE)
		pos = 0;

	    dest += SCREENWIDTH;

	    frac += fracstep;
	} while (count--);

	fuzzpos = pos;
    }
}

void R_DrawFuzzColumnLow (void)
{
    int			count;
    byte*		dest;
    byte*		dest2;
    fixed_t		frac;
    fixed_t		fracstep;
    byte		fuzzed;

    if (!dc_yl)
	dc_yl = 1;

    if (dc_yh == viewheight-1)
	dc_yh = viewheight - 2;

    count = dc_yh - dc_yl;

    if (count < 0)
	return;

#ifdef RANGECHECK
    if ((unsigned)dc_x >= SCREENWIDTH
	|| dc_yl < 0 || dc_yh >= SCREENHEIGHT)
    {
	I_Error ("R_DrawFuzzColumn: %i to %i at %i",
		 dc_yl, dc_yh, dc_x);
    }
#endif

    // Blocky mode duplicates each logical column to two framebuffer columns.
    dc_x <<= 1;

    dest = ylookup[dc_yl] + columnofs[dc_x];
    dest2 = ylookup[dc_yl] + columnofs[dc_x+1];

    // Keep frac stepping identical to full-detail fuzz behavior.
    fracstep = dc_iscale;
    frac = dc_texturemid + (dc_yl-centery)*fracstep;

    {
	register lighttable_t*	fuzzmap = colormaps + 6*256;
	register int		pos = fuzzpos;

	do
	{
	    fuzzed = fuzzmap[dest[fuzzoffset[pos]]];
	    *dest = fuzzed;
	    *dest2 = fuzzed;

	    if (++pos == FUZZTABLE)
		pos = 0;

	    dest += SCREENWIDTH;
	    dest2 += SCREENWIDTH;

	    frac += fracstep;
	} while (count--);

	fuzzpos = pos;
    }
}
 
  
 

//
// R_DrawTranslatedColumn
// Used to draw player sprites
//  with the green colorramp mapped to others.
// Could be used with different translation
//  tables, e.g. the lighter colored version
//  of the BaronOfHell, the HellKnight, uses
//  identical sprites, kinda brightened up.
//
byte*	dc_translation;
byte*	translationtables;

void R_DrawTranslatedColumn (void) 
{ 
    int			count; 
    byte*		dest; 
    fixed_t		frac;
    fixed_t		fracstep;	 
 
    count = dc_yh - dc_yl; 
    if (count < 0) 
	return; 
				 
#ifdef RANGECHECK 
    if ((unsigned)dc_x >= SCREENWIDTH
	|| dc_yl < 0
	|| dc_yh >= SCREENHEIGHT)
    {
	I_Error ( "R_DrawColumn: %i to %i at %i",
		  dc_yl, dc_yh, dc_x);
    }
    
#endif 


    // WATCOM VGA specific.
    /* Keep for fixing.
    if (detailshift)
    {
	if (dc_x & 1)
	    outp (SC_INDEX+1,12); 
	else
	    outp (SC_INDEX+1,3);
	
	dest = destview + dc_yl*80 + (dc_x>>1); 
    }
    else
    {
	outp (SC_INDEX+1,1<<(dc_x&3)); 

	dest = destview + dc_yl*80 + (dc_x>>2); 
    }*/

    
    // FIXME. As above.
    dest = ylookup[dc_yl] + columnofs[dc_x]; 

    // Looks familiar.
    fracstep = dc_iscale; 
    frac = dc_texturemid + (dc_yl-centery)*fracstep; 

    // Here we do an additional index re-mapping.
    {
	register byte*			source = dc_source;
	register byte*			translation = dc_translation;
	register lighttable_t*		colormap = dc_colormap;

	do
	{
	    // Translation tables are used
	    //  to map certain colorramps to other ones,
	    //  used with PLAY sprites.
	    // Thus the "green" ramp of the player 0 sprite
	    //  is mapped to gray, red, black/indigo.
	    *dest = colormap[translation[source[frac>>FRACBITS]]];
	    dest += SCREENWIDTH;

	    frac += fracstep;
	} while (count--);
    }
}

void R_DrawTranslatedColumnLow (void)
{
    int			count;
    byte*		dest;
    byte*		dest2;
    fixed_t		frac;
    fixed_t		fracstep;
    byte		translated;

    count = dc_yh - dc_yl;
    if (count < 0)
	return;

#ifdef RANGECHECK
    if ((unsigned)dc_x >= SCREENWIDTH
	|| dc_yl < 0
	|| dc_yh >= SCREENHEIGHT)
    {
	I_Error ( "R_DrawColumn: %i to %i at %i",
		  dc_yl, dc_yh, dc_x);
    }
#endif

    // Blocky mode duplicates each logical column to two framebuffer columns.
    dc_x <<= 1;

    dest = ylookup[dc_yl] + columnofs[dc_x];
    dest2 = ylookup[dc_yl] + columnofs[dc_x+1];

    fracstep = dc_iscale;
    frac = dc_texturemid + (dc_yl-centery)*fracstep;

    {
	register byte*			source = dc_source;
	register byte*			translation = dc_translation;
	register lighttable_t*		colormap = dc_colormap;

	// dest2 == dest+1: collapse the pair into one halfword store when the
	// destination is 2-byte aligned, else keep two byte stores.
	if (!(((uintptr_t)dest) & 1))
	{
	    do
	    {
		translated = colormap[translation[source[frac>>FRACBITS]]];
		*(uint16_t*)dest = (translated << 8) | translated;
		dest += SCREENWIDTH;
		frac += fracstep;
	    } while (count--);
	}
	else
	{
	    do
	    {
		translated = colormap[translation[source[frac>>FRACBITS]]];
		*dest = translated;
		*dest2 = translated;

		dest += SCREENWIDTH;
		dest2 += SCREENWIDTH;

		frac += fracstep;
	    } while (count--);
	}
    }
}




//
// R_InitTranslationTables
// Creates the translation tables to map
//  the green color ramp to gray, brown, red.
// Assumes a given structure of the PLAYPAL.
// Could be read from a lump instead.
//
void R_InitTranslationTables (void)
{
    int		i;
	
    translationtables = Z_Malloc (256*3+255, PU_STATIC, 0);
    translationtables = (byte *)(( (int)translationtables + 255 )& ~255);
    
    // translate just the 16 green colors
    for (i=0 ; i<256 ; i++)
    {
	if (i >= 0x70 && i<= 0x7f)
	{
	    // map green ramp to gray, brown, red
	    translationtables[i] = 0x60 + (i&0xf);
	    translationtables [i+256] = 0x40 + (i&0xf);
	    translationtables [i+512] = 0x20 + (i&0xf);
	}
	else
	{
	    // Keep all other colors as is.
	    translationtables[i] = translationtables[i+256] 
		= translationtables[i+512] = i;
	}
    }
}




//
// R_DrawSpan 
// With DOOM style restrictions on view orientation,
//  the floors and ceilings consist of horizontal slices
//  or spans with constant z depth.
// However, rotation around the world z axis is possible,
//  thus this mapping, while simpler and faster than
//  perspective correct texture mapping, has to traverse
//  the texture at an angle in all but a few cases.
// In consequence, flats are not stored by column (like walls),
//  and the inner loop has to step in texture space u and v.
//
int			ds_y; 
int			ds_x1; 
int			ds_x2;

lighttable_t*		ds_colormap; 

fixed_t			ds_xfrac; 
fixed_t			ds_yfrac; 
fixed_t			ds_xstep; 
fixed_t			ds_ystep;

// start of a 64*64 tile image 
byte*			ds_source;	

// just for profiling
int			dscount;


//
// Draws the actual span.
void R_DrawSpan (void)
{
    fixed_t		xfrac;
    fixed_t		yfrac;
    byte*		dest;
    int			count;

#ifdef RANGECHECK
    if (ds_x2 < ds_x1
	|| ds_x1<0
	|| ds_x2>=SCREENWIDTH  
	|| (unsigned)ds_y>SCREENHEIGHT)
    {
	I_Error( "R_DrawSpan: %i to %i at %i",
		 ds_x1,ds_x2,ds_y);
    }
//	dscount++; 
#endif 

    
    xfrac = ds_xfrac;
    yfrac = ds_yfrac;

    dest = ylookup[ds_y] + columnofs[ds_x1];

    // We do not check for zero spans here?
    count = ds_x2 - ds_x1;

    {
	// Cache loop-invariant globals; the byte stores below would otherwise
	// force a reload of each on every pixel (char aliases everything).
	register byte*			source = ds_source;
	register lighttable_t*		colormap = ds_colormap;
	register fixed_t		xstep = ds_xstep;
	register fixed_t		ystep = ds_ystep;
	register int			n = count + 1;
	register int			spot;

	// screens[0] is uncached RDRAM: one aligned 64-bit store is a single
	// transaction for 8 pixels vs 8 separate byte transactions. Head singles
	// bring dest to 8-byte alignment, then write packed octets, then tail.
	while (n > 0 && (((uintptr_t)dest) & 7))
	{
	    spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
	    *dest++ = colormap[source[spot]];
	    xfrac += xstep;
	    yfrac += ystep;
	    n--;
	}

	while (n >= 8)
	{
	    register uint64_t packed;

	    // Pixel at the lowest address is the most significant byte
	    //  (big-endian target), so pack p0..p7 from bit 56 down to 0.
	    spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
	    packed = (uint64_t)colormap[source[spot]] << 56;
	    xfrac += xstep; yfrac += ystep;
	    spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
	    packed |= (uint64_t)colormap[source[spot]] << 48;
	    xfrac += xstep; yfrac += ystep;
	    spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
	    packed |= (uint64_t)colormap[source[spot]] << 40;
	    xfrac += xstep; yfrac += ystep;
	    spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
	    packed |= (uint64_t)colormap[source[spot]] << 32;
	    xfrac += xstep; yfrac += ystep;
	    spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
	    packed |= (uint64_t)colormap[source[spot]] << 24;
	    xfrac += xstep; yfrac += ystep;
	    spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
	    packed |= (uint64_t)colormap[source[spot]] << 16;
	    xfrac += xstep; yfrac += ystep;
	    spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
	    packed |= (uint64_t)colormap[source[spot]] << 8;
	    xfrac += xstep; yfrac += ystep;
	    spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
	    packed |= (uint64_t)colormap[source[spot]];
	    xfrac += xstep; yfrac += ystep;

	    *(uint64_t*)dest = packed;
	    dest += 8;
	    n -= 8;
	}

	while (n > 0)
	{
	    spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
	    *dest++ = colormap[source[spot]];
	    xfrac += xstep;
	    yfrac += ystep;
	    n--;
	}
    }
}



// UNUSED.
// Loop unrolled by 4.
#if 0
void R_DrawSpan (void) 
{ 
    unsigned	position, step;

    byte*	source;
    byte*	colormap;
    byte*	dest;
    
    unsigned	count;
    usingned	spot; 
    unsigned	value;
    unsigned	temp;
    unsigned	xtemp;
    unsigned	ytemp;
		
    position = ((ds_xfrac<<10)&0xffff0000) | ((ds_yfrac>>6)&0xffff);
    step = ((ds_xstep<<10)&0xffff0000) | ((ds_ystep>>6)&0xffff);
		
    source = ds_source;
    colormap = ds_colormap;
    dest = ylookup[ds_y] + columnofs[ds_x1];	 
    count = ds_x2 - ds_x1 + 1; 
	
    while (count >= 4) 
    { 
	ytemp = position>>4;
	ytemp = ytemp & 4032;
	xtemp = position>>26;
	spot = xtemp | ytemp;
	position += step;
	dest[0] = colormap[source[spot]]; 

	ytemp = position>>4;
	ytemp = ytemp & 4032;
	xtemp = position>>26;
	spot = xtemp | ytemp;
	position += step;
	dest[1] = colormap[source[spot]];
	
	ytemp = position>>4;
	ytemp = ytemp & 4032;
	xtemp = position>>26;
	spot = xtemp | ytemp;
	position += step;
	dest[2] = colormap[source[spot]];
	
	ytemp = position>>4;
	ytemp = ytemp & 4032;
	xtemp = position>>26;
	spot = xtemp | ytemp;
	position += step;
	dest[3] = colormap[source[spot]]; 
		
	count -= 4;
	dest += 4;
    } 
    while (count > 0) 
    { 
	ytemp = position>>4;
	ytemp = ytemp & 4032;
	xtemp = position>>26;
	spot = xtemp | ytemp;
	position += step;
	*dest++ = colormap[source[spot]]; 
	count--;
    } 
} 
#endif


//
// Again..
//
void R_DrawSpanLow (void)
{
    fixed_t		xfrac;
    fixed_t		yfrac;
    byte*		dest;
    int			count;

#ifdef RANGECHECK
    if (ds_x2 < ds_x1
	|| ds_x1<0
	|| ds_x2>=SCREENWIDTH  
	|| (unsigned)ds_y>SCREENHEIGHT)
    {
	I_Error( "R_DrawSpan: %i to %i at %i",
		 ds_x1,ds_x2,ds_y);
    }
//	dscount++; 
#endif 
	 
    xfrac = ds_xfrac; 
    yfrac = ds_yfrac; 

    // Span endpoints arrive in logical (viewwidth) coordinates. In blocky mode
    // each logical column maps to two framebuffer columns, so keep `count` in
    // logical space and write two screen pixels per iteration. (Original DOOM
    // 1.10 doubled count along with the endpoints, making every span overdraw
    // ~2x its width — the dormant bug that caused 3-4P split-screen artifacts
    // when low detail was forced.)
    count = ds_x2 - ds_x1;
    ds_x1 <<= 1;
    dest = ylookup[ds_y] + columnofs[ds_x1];

    {
	register byte*			source = ds_source;
	register lighttable_t*		colormap = ds_colormap;
	register fixed_t		xstep = ds_xstep;
	register fixed_t		ystep = ds_ystep;
	register int			n = count + 1;
	register int			spot;
	register byte			texel;

	// Each logical pixel writes two adjacent framebuffer bytes of the same
	// value. Head singles bring dest to 8-byte alignment, then pack 4 logical
	// pixels (8 bytes) into one 64-bit store, then tail singles.
	while (n > 0 && (((uintptr_t)dest) & 7))
	{
	    spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
	    texel = colormap[source[spot]];
	    *dest++ = texel;
	    *dest++ = texel;
	    xfrac += xstep;
	    yfrac += ystep;
	    n--;
	}

	while (n >= 4)
	{
	    register uint64_t packed;

	    // Lowest address = most significant byte (big-endian). Each logical
	    //  pixel occupies two adjacent bytes of the same value.
	    spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
	    texel = colormap[source[spot]];
	    packed = (uint64_t)texel << 56 | (uint64_t)texel << 48;
	    xfrac += xstep; yfrac += ystep;
	    spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
	    texel = colormap[source[spot]];
	    packed |= (uint64_t)texel << 40 | (uint64_t)texel << 32;
	    xfrac += xstep; yfrac += ystep;
	    spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
	    texel = colormap[source[spot]];
	    packed |= (uint64_t)texel << 24 | (uint64_t)texel << 16;
	    xfrac += xstep; yfrac += ystep;
	    spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
	    texel = colormap[source[spot]];
	    packed |= (uint64_t)texel << 8 | (uint64_t)texel;
	    xfrac += xstep; yfrac += ystep;

	    *(uint64_t*)dest = packed;
	    dest += 8;
	    n -= 4;
	}

	while (n > 0)
	{
	    spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
	    texel = colormap[source[spot]];
	    *dest++ = texel;
	    *dest++ = texel;
	    xfrac += xstep;
	    yfrac += ystep;
	    n--;
	}
    }
}

//
// R_InitBuffer 
// Creats lookup tables that avoid
//  multiplies and other hazzles
//  for getting the framebuffer address
//  of a pixel to draw.
//
void
R_InitBuffer
( int		width,
  int		height ) 
{ 
    int		i; 

    // Handle resize,
    //  e.g. smaller view windows
    //  with border and/or status bar.
    viewwindowx = (SCREENWIDTH-width) >> 1; 

    // Column offset. For windows.
    for (i=0 ; i<width ; i++) 
	columnofs[i] = viewwindowx + i;

    // Samw with base row offset.
    if (width == SCREENWIDTH) 
	viewwindowy = 0; 
    else 
	viewwindowy = (SCREENHEIGHT-SBARHEIGHT-height) >> 1; 

    // Preclaculate all row offsets.
    for (i=0 ; i<height ; i++) 
	ylookup[i] = screens[0] + (i+viewwindowy)*SCREENWIDTH; 
} 

void
R_SetViewWindow
( int		x,
  int		y )
{
    int		i;

    if (x < 0)
	x = 0;
    if (y < 0)
	y = 0;

    if (x + scaledviewwidth > SCREENWIDTH)
	x = SCREENWIDTH - scaledviewwidth;
    if (y + viewheight > SCREENHEIGHT)
	y = SCREENHEIGHT - viewheight;

    if (x < 0)
	x = 0;
    if (y < 0)
	y = 0;

    viewwindowx = x;
    viewwindowy = y;

    for (i=0 ; i<scaledviewwidth ; i++)
	columnofs[i] = viewwindowx + i;

    for (i=0 ; i<viewheight ; i++)
	ylookup[i] = screens[0] + (i+viewwindowy)*SCREENWIDTH;
}

#ifdef N64
// The N64 layer ping-pongs screens[0] each present; ylookup caches that base,
// so rebase it to the active draw buffer using the current view geometry.
void R_N64RebaseScreen(void)
{
    int i;

    for (i=0 ; i<viewheight ; i++)
	ylookup[i] = screens[0] + (i+viewwindowy)*SCREENWIDTH;
}
#endif




//
// R_FillBackScreen
// Fills the back screen with a pattern
//  for variable screen sizes
// Also draws a beveled edge.
//
void R_FillBackScreen (void) 
{ 
    byte*	src;
    byte*	dest; 
    int		x;
    int		y; 
    patch_t*	patch;

    // DOOM border patch.
    char	name1[] = "FLOOR7_2";

    // DOOM II border patch.
    char	name2[] = "GRNROCK";	

    char*	name;
	
    if (scaledviewwidth == 320)
	return;
	
    if ( gamemode == commercial)
	name = name2;
    else
	name = name1;
    
    src = W_CacheLumpName (name, PU_CACHE); 
    dest = screens[1]; 
	 
    for (y=0 ; y<SCREENHEIGHT-SBARHEIGHT ; y++) 
    { 
	for (x=0 ; x<SCREENWIDTH/64 ; x++) 
	{ 
	    memcpy (dest, src+((y&63)<<6), 64); 
	    dest += 64; 
	} 

	if (SCREENWIDTH&63) 
	{ 
	    memcpy (dest, src+((y&63)<<6), SCREENWIDTH&63); 
	    dest += (SCREENWIDTH&63); 
	} 
    } 
	
    patch = W_CacheLumpName ("brdr_t",PU_CACHE);

    for (x=0 ; x<scaledviewwidth ; x+=8)
	V_DrawPatch (viewwindowx+x,viewwindowy-8,1,patch);
    patch = W_CacheLumpName ("brdr_b",PU_CACHE);

    for (x=0 ; x<scaledviewwidth ; x+=8)
	V_DrawPatch (viewwindowx+x,viewwindowy+viewheight,1,patch);
    patch = W_CacheLumpName ("brdr_l",PU_CACHE);

    for (y=0 ; y<viewheight ; y+=8)
	V_DrawPatch (viewwindowx-8,viewwindowy+y,1,patch);
    patch = W_CacheLumpName ("brdr_r",PU_CACHE);

    for (y=0 ; y<viewheight ; y+=8)
	V_DrawPatch (viewwindowx+scaledviewwidth,viewwindowy+y,1,patch);


    // Draw beveled edge. 
    V_DrawPatch (viewwindowx-8,
		 viewwindowy-8,
		 1,
		 W_CacheLumpName ("brdr_tl",PU_CACHE));
    
    V_DrawPatch (viewwindowx+scaledviewwidth,
		 viewwindowy-8,
		 1,
		 W_CacheLumpName ("brdr_tr",PU_CACHE));
    
    V_DrawPatch (viewwindowx-8,
		 viewwindowy+viewheight,
		 1,
		 W_CacheLumpName ("brdr_bl",PU_CACHE));
    
    V_DrawPatch (viewwindowx+scaledviewwidth,
		 viewwindowy+viewheight,
		 1,
		 W_CacheLumpName ("brdr_br",PU_CACHE));
} 
 

//
// Copy a screen buffer.
//
void
R_VideoErase
( unsigned	ofs,
  int		count ) 
{ 
  // LFB copy.
  // This might not be a good idea if memcpy
  //  is not optiomal, e.g. byte by byte on
  //  a 32bit CPU, as GNU GCC/Linux libc did
  //  at one point.
    memcpy (screens[0]+ofs, screens[1]+ofs, count); 
} 


//
// R_DrawViewBorder
// Draws the border around the view
//  for different size windows?
//
void
V_MarkRect
( int		x,
  int		y,
  int		width,
  int		height ); 
 
void R_DrawViewBorder (void) 
{ 
    int		top;
    int		side;
    int		ofs;
    int		i; 
 
    if (scaledviewwidth == SCREENWIDTH) 
	return; 
  
    top = ((SCREENHEIGHT-SBARHEIGHT)-viewheight)/2; 
    side = (SCREENWIDTH-scaledviewwidth)/2; 
 
    // copy top and one line of left side 
    R_VideoErase (0, top*SCREENWIDTH+side); 
 
    // copy one line of right side and bottom 
    ofs = (viewheight+top)*SCREENWIDTH-side; 
    R_VideoErase (ofs, top*SCREENWIDTH+side); 
 
    // copy sides using wraparound 
    ofs = top*SCREENWIDTH + SCREENWIDTH-side; 
    side <<= 1;
    
    for (i=1 ; i<viewheight ; i++) 
    { 
	R_VideoErase (ofs, side); 
	ofs += SCREENWIDTH; 
    } 

    // ? 
    V_MarkRect (0,0,SCREENWIDTH, SCREENHEIGHT-SBARHEIGHT); 
} 
 
 
