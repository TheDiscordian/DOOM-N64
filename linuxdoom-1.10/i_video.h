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
//	System specific interface stuff.
//
//-----------------------------------------------------------------------------


#ifndef __I_VIDEO__
#define __I_VIDEO__


#include "doomtype.h"

#ifdef __GNUG__
#pragma interface
#endif


// Called by D_DoomMain,
// determines the hardware configuration
// and sets up the video mode
void I_InitGraphics (void);


void I_ShutdownGraphics(void);

// Takes full 8 bit values.
void I_SetPalette (byte* palette);

void I_UpdateNoBlit (void);
void I_FinishUpdate (void);

// Wait for vertical retrace or pause a bit.
void I_WaitVBL(int count);

void I_ReadScreen (byte* scr);

void I_BeginRead (void);
void I_EndRead (void);

#ifdef N64
#define N64_DISPLAY_RESOLUTION ((resolution_t){ \
	.width = 320, \
	.height = 200, \
	.aspect_ratio = 4.0f / 3.0f, \
	.overscan_margin = VI_CRT_MARGIN \
})

typedef struct n64_local_input_s
{
	int joy_x;
	int joy_y;
	boolean strafe_left;
	boolean strafe_right;
	boolean speed;
	boolean fire;
	boolean use;
	int weapon_key;
} n64_local_input_t;

void I_N64GetLocalInputState(int player_index, n64_local_input_t* out_state);
void I_N64SplitScreenBeginFrame(int player_count);
void I_N64SplitScreenEndFrame(void);
int I_N64GetActiveGameplayPort(void);

// RDP renderer (Stage 1+). Reserve the transparency-key palette index by
// scanning the UI/status-bar/font/menu patch lumps; call once at startup after
// the WAD is loaded. n64_rdp_key_index holds the result (-1 until scanned).
void I_N64ScanTransparencyKey(void);
extern int n64_rdp_key_index;

// Force a TLUT re-upload on the next present (used by the renderer toggle,
// which changes the key index's alpha bit).
void I_N64MarkPaletteDirty(void);

// Force the MASTER 256-entry TLUT to be re-uploaded before the present blit
// (does NOT touch the key alpha -- the master is already correct). The CI4 wall
// pass overwrites the 256-entry TLUT region with per-texture 16-colour sub-
// palettes; this re-asserts the master so the present blit + CI8 sprites/HUD
// sample the right colours. Called by DL_Flush at the end of the wall pass.
void I_N64ForceTLUTReupload(void);

// SYNCHRONOUS master-TLUT re-upload: re-load the master 256-entry TLUT into TMEM
// NOW (in the current rspq stream), not just arm the present-blit dirty flag.
// DL_Flush calls this between the CI4 wall pass and the CI8 plane pass when walls
// drew, so the plane flats (which sample the master TLUT) do not read the CI4 sub-
// palettes the wall pass left in the 256-entry TLUT region. Caller must be in a
// TLUT-addressable mode (the world TLUT_RGBA16 textured mode satisfies this).
void I_N64UploadMasterTLUT(void);

// Scrub transparency-key pixels out of a wipe-captured CI8 screen (replace
// each with the pixel above; top row falls back to palette 0). The wipe
// captures recycle buffers that contain the routed seg's key-suppressed
// pixels; unscrubbed, melt presents repaint them outside any keyed box and
// they show as opaque key colour (stale-key sparkle). No-op flag-off.
void I_N64WipeScrubKey(byte* scr);

// Batched key-fill of the 3D-view window of the CI8 draw buffer (Stage-3
// seg_rast collapse): called at view-render entry (KEY_CLEAR phase) when the
// RDP wall route is active, REPLACING the per-column R_FillColumnKey writes.
// Also arms the present's full-view keyed box. No-op flag-off / route-off.
void I_N64KeyClearView(void);
#endif



#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
