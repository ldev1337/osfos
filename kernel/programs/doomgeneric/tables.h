//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 1993-2008 Raven Software
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Lookup tables.
//	Do not try to look them up :-).
//	In the order of appearance: 
//
//	int finetangent[4096]	- Tangens LUT.
//	 Should work with BAM fairly well (12 of 16bit,
//      effectively, by shifting).
//
//	int finesine[10240]		- Sine lookup.
//	 Guess what, serves as cosine, too.
//	 Remarkable thing is, how to use BAMs with this? 
//
//	int tantoangle[2049]	- ArcTan LUT,
//	  maps tan(angle) to angle fast. Gotta search.	
//    

#ifndef __TABLES__
#define __TABLES__

#include "doomtype.h"
#include "m_fixed.h"

#define FINEANGLES		8192
#define FINEMASK		(FINEANGLES-1)

#define ANGLETOFINESHIFT	19

#define ANG45			(0x20000000)
#define ANG90			(0x40000000)
#define ANG180			(0x80000000)
#define ANG270			(0xC0000000)

#define SLOPERANGE		2048
#define SLOPEBITS		11
#define DBITS			(FRACBITS-SLOPEBITS)

typedef unsigned angle_t;

// Non-const declarations since tables are populated at startup
extern fixed_t finesine[5 * FINEANGLES / 4];
extern const fixed_t *finecosine;
extern fixed_t finetangent[FINEANGLES / 2];
extern byte gammatable[5][256];
extern angle_t tantoangle[SLOPERANGE + 1];

int SlopeDiv(unsigned int num, unsigned int den);
void R_InitTables(void);

#endif