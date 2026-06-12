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
//	Fixed point arithemtics, implementation.
//
//-----------------------------------------------------------------------------


#ifndef __M_FIXED__
#define __M_FIXED__


#ifdef __GNUG__
#pragma interface
#endif

#include <stdlib.h>

#include "doomtype.h"


//
// Fixed point, 32bit as 16.16.
//
#define FRACBITS		16
#define FRACUNIT		(1<<FRACBITS)

typedef int fixed_t;

static inline fixed_t FixedMul (fixed_t a, fixed_t b)
{
    return ((long long) a * (long long) b) >> FRACBITS;
}

static inline fixed_t FixedDiv2 (fixed_t a, fixed_t b)
{
    // exact 64-bit integer divide; FP path removed (VR4300 flushes denormals)
    if (b == 0)
	return (a^b)<0 ? MININT : MAXINT;
    return (fixed_t)(((long long)a<<16) / (long long)b);
}

static inline fixed_t FixedDiv (fixed_t a, fixed_t b)
{
    if ( (abs(a)>>14) >= abs(b))
	return (a^b)<0 ? MININT : MAXINT;
    return FixedDiv2 (a,b);
}



#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
