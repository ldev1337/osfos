//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// DESCRIPTION:
//     Single-player game loop and tic timing.
//

#include <string.h>

#include "doomdef.h"
#include "d_event.h"
#include "d_loop.h"
#include "d_ticcmd.h"
#include "i_system.h"
#include "i_timer.h"
#include "i_video.h"

// A local tic contains one player's command and active-player mask.
typedef struct
{
    ticcmd_t cmds[MAXPLAYERS];
    boolean ingame[MAXPLAYERS];
} ticcmd_set_t;

static ticcmd_set_t ticdata[BACKUPTICS];
static int maketic;
int gametic;
int ticdup = 1;
boolean singletics = false;
static int localplayer;
static int lasttime;
static loop_interface_t *loop_interface;

static int GetGameTime(void)
{
    return (I_GetTimeMS() * TICRATE) / 1000;
}

static boolean BuildNewTic(void)
{
    int gameticdiv = gametic / ticdup;
    ticcmd_t cmd;
    ticcmd_set_t *set;

    I_StartTic();
    loop_interface->ProcessEvents();
    loop_interface->RunMenu();

    // Keep a short input queue in single-player mode.
    if (maketic - gameticdiv > 2)
    {
        return false;
    }

    memset(&cmd, 0, sizeof(cmd));
    loop_interface->BuildTiccmd(&cmd, maketic);

    set = &ticdata[maketic % BACKUPTICS];
    memset(set, 0, sizeof(*set));
    set->cmds[localplayer] = cmd;
    set->ingame[localplayer] = true;
    ++maketic;

    return true;
}

void BuildSinglePlayerTics(void)
{
    int nowtime;
    int newtics;

    if (singletics)
    {
        return;
    }

    nowtime = GetGameTime();
    newtics = nowtime - lasttime;
    lasttime = nowtime;

    while (newtics-- > 0)
    {
        if (!BuildNewTic())
        {
            break;
        }
    }
}

void D_StartGameLoop(void)
{
    lasttime = GetGameTime();
}

void TryRunTics(void)
{
    int entertic = GetGameTime() / ticdup;
    int counts;
    int lowtic;

    if (singletics)
    {
        BuildNewTic();
    }
    else
    {
        BuildSinglePlayerTics();
    }

    lowtic = maketic;
    counts = lowtic - gametic / ticdup;
    if (counts < 1)
    {
        counts = 1;
    }

    while (lowtic < gametic / ticdup + counts)
    {
        BuildSinglePlayerTics();
        lowtic = maketic;

        // Return to the caller once a game tic has elapsed; the next frame
        // can update the display and try again.
        if (GetGameTime() / ticdup > entertic)
        {
            return;
        }

        I_Sleep(1);
    }

    while (counts-- > 0)
    {
        ticcmd_set_t *set = &ticdata[(gametic / ticdup) % BACKUPTICS];

        loop_interface->RunTic(set->cmds, set->ingame);
        ++gametic;
    }
}

void D_RegisterLoopCallbacks(loop_interface_t *callbacks)
{
    loop_interface = callbacks;
}
