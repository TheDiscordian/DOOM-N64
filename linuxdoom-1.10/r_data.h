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
// DESCRIPTION:
//  Refresh module, data I/O, caching, retrieval of graphics
//  by name.
//
//-----------------------------------------------------------------------------


#ifndef __R_DATA__
#define __R_DATA__

#include "r_defs.h"
#include "r_state.h"

#ifdef __GNUG__
#pragma interface
#endif

// Retrieve column data for span blitting.
byte*
R_GetColumn
( int		tex,
  int		col );

// Composite generator (definition in r_data.c) -- referenced by the inline
// column accessor below for the lazy-composite fallback path.
void R_GenerateComposite (int texnum);

#include "w_wad.h"	// W_CacheLumpNum, used by R_GetColumnIn below
#include "z_zone.h"	// PU_CACHE, used by R_GetColumnIn below

// Per-texture column-lookup tables (definitions in r_data.c). Exported here so
// the inline accessors below -- and the per-column wall raster that uses them --
// can index them directly with the per-tex resolution hoisted out of the loop.
extern int*		texturewidthmask;
extern short**		texturecolumnlump;
extern unsigned short**	texturecolumnofs;
extern byte**		texturecomposite;

// The 8-char texture name for a texnum (NOT NUL-terminated; print with %.8s). For
// diagnostics that only see texnums (e.g. the DLBUILD_TRACE first-touch logger in
// rdp_view.c) where the `textures` array isn't in scope. Returns "" if out of range.
const char* R_TextureNameForNum(int texnum);

// Per-tex column-lookup state resolved ONCE per seg/tier, so the per-column wall
// raster (r_segs.c) avoids re-doing R_GetColumn's per-tex work every column:
// the function call, the two scattered Z_Malloc array indexes
// (texturecolumnlump[tex] / texturecolumnofs[tex]), and the width-mask lookup.
typedef struct
{
    const short*		lump;	// texturecolumnlump[tex]
    const unsigned short*	ofs;	// texturecolumnofs[tex]
    int				mask;	// texturewidthmask[tex]
    int				tex;	// for the rare lazy-composite fallback
} texcol_t;

// Resolve the per-tex column tables once (call OUTSIDE the column loop).
static inline texcol_t R_GetColumnTex (int tex)
{
    texcol_t	tc;
    tc.lump = texturecolumnlump[tex];
    tc.ofs  = texturecolumnofs[tex];
    tc.mask = texturewidthmask[tex];
    tc.tex  = tex;
    return tc;
}

// Per-column accessor -- byte-identical addresses to R_GetColumn(tc->tex, col),
// just with the per-tex resolution hoisted into *tc. Call INSIDE the column loop.
static inline byte* R_GetColumnIn (const texcol_t* tc, int col)
{
    int		c   = col & tc->mask;
    int		lump = tc->lump[c];
    int		ofs  = tc->ofs[c];

    if (lump > 0)
	return (byte *)W_CacheLumpNum(lump,PU_CACHE)+ofs;

    if (!texturecomposite[tc->tex])
	R_GenerateComposite (tc->tex);

    return texturecomposite[tc->tex] + ofs;
}


// I/O, setting up the stuff.
void R_InitData (void);
void R_PrecacheLevel (void);


// Retrieval.
// Floor/ceiling opaque texture tiles,
// lookup by name. For animation?
int R_FlatNumForName (char* name);


// Called by P_Ticker for switches and animations,
// returns the texture number for the texture name.
int R_TextureNumForName (char *name);
int R_CheckTextureNumForName (char *name);

#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
