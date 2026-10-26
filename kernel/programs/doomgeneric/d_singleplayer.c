//
// Copyright(C) 1993-1996 Id Software, Inc.
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
//	Single-player game loop callbacks.
//

#include "d_main.h"
#include "m_menu.h"
#include "g_game.h"
#include "doomdef.h"
#include "doomstat.h"
#include "d_loop.h"

ticcmd_t *current_ticcmds;

static void RunTic(ticcmd_t *cmds, boolean *ingame)
{
	int i;

	// Pass the local tic command and player state to the game ticker.
	current_ticcmds = cmds;
	for (i = 0; i < MAXPLAYERS; ++i)
	{
	    playeringame[i] = ingame[i];
	}

	// D_StartTitle requests the first demo sequence asynchronously. Advance
	// it here, before G_Ticker reaches D_PageTicker / D_PageDrawer.
	if (advancedemo)
	{
	    D_DoAdvanceDemo();
	}

	G_Ticker();
}

static loop_interface_t doom_loop_interface = {
    D_ProcessEvents,
    G_BuildTiccmd,
    RunTic,
    M_Ticker
};


void D_InitSinglePlayerLoop(void)
{
	// Set up the callbacks and timing needed by the offline game loop.
	D_RegisterLoopCallbacks(&doom_loop_interface);
	ticdup = 1;
	consoleplayer = 0;
	playeringame[0] = true;
}

