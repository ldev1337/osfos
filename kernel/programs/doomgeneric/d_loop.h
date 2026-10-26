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
//	Single-player main loop.
//

#ifndef __D_LOOP__
#define __D_LOOP__

#include "d_ticcmd.h"

typedef struct
{
    // Read events from the event queue, and process them.

    void (*ProcessEvents)();

    // Given the current input state, fill in the fields of the specified
    // ticcmd_t structure with data for a new tic.

    void (*BuildTiccmd)(ticcmd_t *cmd, int maketic);

    // Advance the game forward one tic, using the specified player input.

    void (*RunTic)(ticcmd_t *cmds, boolean *ingame);

    // Run the menu (runs independently of the game).

    void (*RunMenu)();
} loop_interface_t;

// Register callbacks used by the single-player loop.
void D_RegisterLoopCallbacks(loop_interface_t *i);

// Run game simulation ticks from the local input queue.
void TryRunTics (void);
void BuildSinglePlayerTics(void);

// Called at start of game loop to initialize timers
void D_StartGameLoop(void);

extern boolean singletics;
extern int gametic, ticdup;

#endif

