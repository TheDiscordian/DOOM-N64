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
// DESCRIPTION:  none
//
//-----------------------------------------------------------------------------


static const char
rcsid[] = "$Id: g_game.c,v 1.8 1997/02/03 22:45:09 b1 Exp $";

#include <string.h>
#include <stdlib.h>

#ifdef N64
#include <errno.h>
#include <stdio.h>
#include <libdragon.h>
#include "lzfx.h"
#endif

#include "doomdef.h" 
#include "doomstat.h"

#include "z_zone.h"
#include "f_finale.h"
#include "m_argv.h"
#include "m_misc.h"
#include "m_menu.h"
#include "m_random.h"
#include "i_system.h"
#include "i_video.h"

#include "p_setup.h"
#include "p_saveg.h"
#include "p_tick.h"

#include "d_main.h"
#include "d_net.h"

#include "wi_stuff.h"
#include "hu_stuff.h"
#include "st_stuff.h"
#include "am_map.h"

// Needs access to LFB.
#include "v_video.h"

#include "w_wad.h"

#include "p_local.h" 

#include "s_sound.h"

// Data.
#include "dstrings.h"
#include "sounds.h"

// SKY handling - still the wrong place.
#include "r_data.h"
#include "r_sky.h"



#include "g_game.h"

#ifdef N64_BENCH
#include "n64_bench.h"
#endif


#define SAVEGAMESIZE	0x2c000
#define SAVESTRINGSIZE	24



boolean	G_CheckDemoStatus (void); 
void	G_ReadDemoTiccmd (ticcmd_t* cmd); 
void	G_WriteDemoTiccmd (ticcmd_t* cmd); 
void	G_PlayerReborn (int player); 
void	G_InitNew (skill_t skill, int episode, int map); 
 
void	G_DoReborn (int playernum); 
 
void	G_DoLoadLevel (void); 
void	G_DoNewGame (void); 
void	G_DoLoadGame (void); 
void	G_DoPlayDemo (void); 
void	G_DoCompleted (void); 
void	G_DoVictory (void); 
void	G_DoWorldDone (void); 
void	G_DoSaveGame (void); 

static int G_EpisodeSkyTexture (int episode);
 
 
gameaction_t    gameaction; 
gamestate_t     gamestate; 
skill_t         gameskill; 
boolean		respawnmonsters;
int             gameepisode; 
int             gamemap; 
 
boolean         paused; 
boolean         sendpause;             	// send a pause event next tic 
boolean         sendsave;             	// send a save event next tic 
boolean         usergame;               // ok to save / end game 
 
boolean         timingdemo;             // if true, exit with report on completion 
boolean         nodrawers;              // for comparative timing purposes 
boolean         noblit;                 // for comparative timing purposes 
int             starttime;          	// for comparative timing purposes  	 
 
boolean         viewactive; 
 
boolean         deathmatch;           	// only if started as net death 
boolean         netgame;                // only true if packets are broadcast 
boolean         playeringame[MAXPLAYERS]; 
player_t        players[MAXPLAYERS]; 
 
int             consoleplayer;          // player taking events and displaying 
int             displayplayer;          // view being displayed 
int             gametic; 
int             levelstarttic;          // gametic at level start 
int             totalkills, totalitems, totalsecret;    // for intermission 
 
char            demoname[32]; 
boolean         demorecording; 
boolean         demoplayback; 
boolean		netdemo; 
byte*		demobuffer;
byte*		demo_p;
byte*		demoend; 
boolean         singledemo;            	// quit after playing a demo from cmdline 
 
boolean         precache = true;        // if true, load all graphics at start 
 
wbstartstruct_t wminfo;               	// parms for world map / intermission 
 
short		consistancy[MAXPLAYERS][BACKUPTICS]; 
 
byte*		savebuffer;

#ifdef N64
#define N64_CPAK_PREFIX         "cpak:/"
#define N64_CPAK_GAME_PUB       "DOOM.64"
#define N64_CPAK_NOTE_EXT       "SAV"
#define N64_CPAK_PAYLOAD_MAX    (256 * 123)
#define N64_CPAK_HEADER_SIZE    4
#define N64_CPAK_COMPRESSED_MAX (N64_CPAK_PAYLOAD_MAX - N64_CPAK_HEADER_SIZE)

static byte    n64_savebuffer[SAVEGAMESIZE] __attribute__((aligned(8)));
static uint8_t n64_cpak_payload[N64_CPAK_PAYLOAD_MAX] __attribute__((aligned(8)));
static char    n64_save_status[96];
static char    n64_save_note_path[64];

static void G_N64SetStatus(const char* text)
{
    if (!text)
        text = "";

    snprintf(n64_save_status, sizeof(n64_save_status), "%s", text);
}

static joypad_port_t G_N64ResolveSavePort(void)
{
    int active_port;

    active_port = I_N64GetActiveGameplayPort();
    if (active_port >= (int)JOYPAD_PORT_1 && active_port <= (int)JOYPAD_PORT_4)
    {
        joypad_port_t port;

        port = (joypad_port_t)active_port;
        if (joypad_is_connected(port))
            return port;
    }

    JOYPAD_PORT_FOREACH(port)
    {
        if (joypad_is_connected(port))
            return port;
    }

    return JOYPAD_PORT_1;
}

static void G_N64BuildSaveNotePath(char* out_path, size_t out_size)
{
    const char* save_key;
    char key[17];
    size_t i;
    size_t out;

    save_key = D_N64GetSaveKey();
    if (!save_key || !save_key[0])
        save_key = "DOOM";

    memset(key, 0, sizeof(key));
    out = 0;

    for (i = 0; save_key[i] && out < sizeof(key) - 1; i++)
    {
        unsigned char ch;

        ch = (unsigned char)save_key[i];
        if (ch >= 'a' && ch <= 'z')
            ch = (unsigned char)(ch - ('a' - 'A'));

        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
            key[out++] = (char)ch;
    }

    if (!out)
        strcpy(key, "DOOM");

    snprintf(out_path,
             out_size,
             N64_CPAK_PREFIX N64_CPAK_GAME_PUB "-%s." N64_CPAK_NOTE_EXT,
             key);
}

static uint32_t G_N64ReadU32(const uint8_t* src)
{
    uint32_t value;

    memcpy(&value, src, sizeof(value));
    return value;
}

static void G_N64WriteU32(uint8_t* dst, uint32_t value)
{
    memcpy(dst, &value, sizeof(value));
}

static int G_N64MountPak(joypad_port_t port, boolean allow_format)
{
    int err;
    int saved_errno;

    if (joypad_get_accessory_type(port) != JOYPAD_ACCESSORY_TYPE_CONTROLLER_PAK)
    {
        G_N64SetStatus("Controller Pak not detected.");
        return -1;
    }

    cpakfs_unmount(port);

    errno = 0;
    err = cpakfs_mount(port, N64_CPAK_PREFIX);
    if (!err)
        return 0;

    saved_errno = errno;
    if (allow_format && saved_errno == ENODEV)
    {
        if (cpakfs_format(port, false) == 0)
        {
            errno = 0;
            if (cpakfs_mount(port, N64_CPAK_PREFIX) == 0)
                return 0;

            saved_errno = errno;
        }
    }

    if (!saved_errno)
        saved_errno = EIO;

    snprintf(n64_save_status,
             sizeof(n64_save_status),
             "Pak mount failed (%d:%s)",
             saved_errno,
             strerror(saved_errno));
    return -1;
}

static void G_N64UnmountPak(joypad_port_t port)
{
    cpakfs_unmount(port);
}
#endif
 
 
// 
// controls (have defaults) 
// 
int             key_right;
int		key_left;

int		key_up;
int		key_down; 
int             key_strafeleft;
int		key_straferight; 
int             key_fire;
int		key_use;
int		key_strafe;
int		key_speed; 
 
int             mousebfire; 
int             mousebstrafe; 
int             mousebforward; 
 
int             joybfire; 
int             joybstrafe; 
int             joybuse; 
int             joybspeed; 
 
 
 
#define MAXPLMOVE		(forwardmove[1]) 
#define JOY_ANALOG_MAX	80
 
#define TURBOTHRESHOLD	0x32

fixed_t		forwardmove[2] = {0x19, 0x32}; 
fixed_t		sidemove[2] = {0x18, 0x28};
fixed_t		angleturn[3] = {640, 1280, 320};	// + slow turn
int		alwaysRun = 0;		// default movement: 1 = run, 0 = walk
int		controlScheme = 0;	// N64 controls: 0 = original, 1 = alt
int		frame_interpolation = 1;	// 1 = uncapped/smooth, 0 = capped
int		splitOrientation = 0;		// 2p split: 0 = horizontal, 1 = vertical
int		widescreen = 0;		// aspect: 0 = 4:3, 1 = 16:9 (Hor+)

#define SLOWTURNTICS	6 
 
#define NUMKEYS		256 

boolean         gamekeydown[NUMKEYS]; 
int             turnheld;				// for accelerative turning 
 
boolean		mousearray[4]; 
boolean*	mousebuttons = &mousearray[1];		// allow [-1]

// mouse values are used once 
int             mousex;
int		mousey;         

int             dclicktime;
int		dclickstate;
int		dclicks; 
int             dclicktime2;
int		dclickstate2;
int		dclicks2;

// joystick values are repeated 
int             joyxmove;
int		joyymove;
boolean         joyarray[5]; 
boolean*	joybuttons = &joyarray[1];		// allow [-1] 

#ifdef N64
static int      n64_turnheld[MAXPLAYERS];
#endif
 
int		savegameslot; 
char		savedescription[32]; 
 
 
#define	BODYQUESIZE	32

mobj_t*		bodyque[BODYQUESIZE]; 
int		bodyqueslot; 
 
void*		statcopy;				// for statistics driver
 
 
 
int G_CmdChecksum (ticcmd_t* cmd) 
{ 
    int		i;
    int		sum = 0; 
	 
    for (i=0 ; i< sizeof(*cmd)/4 - 1 ; i++) 
	sum += ((int *)cmd)[i]; 
		 
    return sum; 
} 

static int G_ScaleJoyAnalog(int value, int full_scale)
{
    int magnitude;
    int scaled;

    if (!value)
        return 0;

    magnitude = (value < 0) ? -value : value;
    if (magnitude > JOY_ANALOG_MAX)
        magnitude = JOY_ANALOG_MAX;

    scaled = (full_scale * magnitude + (JOY_ANALOG_MAX / 2)) / JOY_ANALOG_MAX;
    if (!scaled)
        scaled = 1;

    return scaled;
}
 

//
// G_BuildTiccmd
// Builds a ticcmd from all of the available inputs
// or reads it from the demo buffer. 
// If recording a demo, write it out 
// 
void G_BuildTiccmd (ticcmd_t* cmd) 
{ 
    int		i; 
    boolean	strafe;
    boolean	bstrafe; 
    int		speed;
    int		tspeed; 
    int		forward;
    int		side;
    
    ticcmd_t*	base;

    base = I_BaseTiccmd ();		// empty, or external driver
    memcpy (cmd,base,sizeof(*cmd)); 
	
    cmd->consistancy = 
	consistancy[consoleplayer][maketic%BACKUPTICS]; 

 
    strafe = gamekeydown[key_strafe] || mousebuttons[mousebstrafe] 
	|| joybuttons[joybstrafe]; 
    speed = (gamekeydown[key_speed] || joybuttons[joybspeed]) ^ (alwaysRun & 1);
 
    forward = side = 0;
    
    // use two stage accelerative turning
    // on the keyboard and joystick
    if (joyxmove < 0
	|| joyxmove > 0  
	|| gamekeydown[key_right]
	|| gamekeydown[key_left]) 
	turnheld += ticdup; 
    else 
	turnheld = 0; 

    if (turnheld < SLOWTURNTICS) 
	tspeed = 2;             // slow turn 
    else 
	tspeed = speed;
    
    // let movement keys cancel each other out
    if (strafe) 
    { 
	if (gamekeydown[key_right]) 
	{
	    // fprintf(stderr, "strafe right\n");
	    side += sidemove[speed]; 
	}
	if (gamekeydown[key_left]) 
	{
	    //	fprintf(stderr, "strafe left\n");
	    side -= sidemove[speed]; 
	}
	if (joyxmove > 0) 
        side += G_ScaleJoyAnalog(joyxmove, sidemove[speed]); 
	if (joyxmove < 0) 
        side -= G_ScaleJoyAnalog(joyxmove, sidemove[speed]); 
 
    } 
    else 
    { 
	if (gamekeydown[key_right]) 
	    cmd->angleturn -= angleturn[tspeed]; 
	if (gamekeydown[key_left]) 
	    cmd->angleturn += angleturn[tspeed]; 
	if (joyxmove > 0) 
        cmd->angleturn -= G_ScaleJoyAnalog(joyxmove, angleturn[tspeed]); 
	if (joyxmove < 0) 
        cmd->angleturn += G_ScaleJoyAnalog(joyxmove, angleturn[tspeed]); 
    } 
 
    if (gamekeydown[key_up]) 
    {
	// fprintf(stderr, "up\n");
	forward += forwardmove[speed]; 
    }
    if (gamekeydown[key_down]) 
    {
	// fprintf(stderr, "down\n");
	forward -= forwardmove[speed]; 
    }
    if (joyymove < 0) 
    forward += G_ScaleJoyAnalog(joyymove, forwardmove[speed]); 
    if (joyymove > 0) 
    forward -= G_ScaleJoyAnalog(joyymove, forwardmove[speed]); 
    if (gamekeydown[key_straferight]) 
	side += sidemove[speed]; 
    if (gamekeydown[key_strafeleft]) 
	side -= sidemove[speed];
    
    // buttons
    cmd->chatchar = HU_dequeueChatChar(); 
 
    if (gamekeydown[key_fire] || mousebuttons[mousebfire] 
	|| joybuttons[joybfire]) 
	cmd->buttons |= BT_ATTACK; 
 
    if (gamekeydown[key_use] || joybuttons[joybuse] ) 
    { 
	cmd->buttons |= BT_USE;
	// clear double clicks if hit use button 
	dclicks = 0;                   
    } 

    // chainsaw overrides 
    for (i=0 ; i<NUMWEAPONS-1 ; i++)        
	if (gamekeydown['1'+i]) 
	{ 
	    cmd->buttons |= BT_CHANGE; 
	    cmd->buttons |= i<<BT_WEAPONSHIFT; 
	    break; 
	}
    
    // mouse
    if (mousebuttons[mousebforward]) 
	forward += forwardmove[speed];
    
    // forward double click
    if (mousebuttons[mousebforward] != dclickstate && dclicktime > 1 ) 
    { 
	dclickstate = mousebuttons[mousebforward]; 
	if (dclickstate) 
	    dclicks++; 
	if (dclicks == 2) 
	{ 
	    cmd->buttons |= BT_USE; 
	    dclicks = 0; 
	} 
	else 
	    dclicktime = 0; 
    } 
    else 
    { 
	dclicktime += ticdup; 
	if (dclicktime > 20) 
	{ 
	    dclicks = 0; 
	    dclickstate = 0; 
	} 
    }
    
    // strafe double click
    bstrafe =
	mousebuttons[mousebstrafe] 
	|| joybuttons[joybstrafe]; 
    if (bstrafe != dclickstate2 && dclicktime2 > 1 ) 
    { 
	dclickstate2 = bstrafe; 
	if (dclickstate2) 
	    dclicks2++; 
	if (dclicks2 == 2) 
	{ 
	    cmd->buttons |= BT_USE; 
	    dclicks2 = 0; 
	} 
	else 
	    dclicktime2 = 0; 
    } 
    else 
    { 
	dclicktime2 += ticdup; 
	if (dclicktime2 > 20) 
	{ 
	    dclicks2 = 0; 
	    dclickstate2 = 0; 
	} 
    } 
 
    forward += mousey; 
    if (strafe) 
	side += mousex*2; 
    else 
	cmd->angleturn -= mousex*0x8; 

    mousex = mousey = 0; 
	 
    if (forward > MAXPLMOVE) 
	forward = MAXPLMOVE; 
    else if (forward < -MAXPLMOVE) 
	forward = -MAXPLMOVE; 
    if (side > MAXPLMOVE) 
	side = MAXPLMOVE; 
    else if (side < -MAXPLMOVE) 
	side = -MAXPLMOVE; 
 
    cmd->forwardmove += forward; 
    cmd->sidemove += side;
    
    // special buttons
    if (sendpause) 
    { 
	sendpause = false; 
	cmd->buttons = BT_SPECIAL | BTS_PAUSE; 
    } 
 
    if (sendsave)
    {
	sendsave = false;
	cmd->buttons = BT_SPECIAL | BTS_SAVEGAME | (savegameslot<<BTS_SAVESHIFT);
    }
#ifdef N64_BENCH
    // Bench: replace real input with the deterministic scripted sequence so the
    // scenario is identical every run, independent of any host-side input.
    N64Bench_FillTiccmd(cmd);
#endif
}

#ifdef N64
void G_BuildTiccmdN64Local(ticcmd_t* cmd, int playernum, const n64_local_input_t* input)
{
    int speed;
    int tspeed;
    int forward;
    int side;
    int joyx;
    int joyy;
    ticcmd_t* base;

    if (!cmd)
        return;

    if (playernum < 0 || playernum >= MAXPLAYERS)
        playernum = 0;

    base = I_BaseTiccmd();
    memcpy(cmd, base, sizeof(*cmd));

    cmd->consistancy = consistancy[playernum][maketic % BACKUPTICS];

    speed = (((input && input->speed) ? 1 : 0) ^ (alwaysRun & 1));
    joyx = input ? input->joy_x : 0;
    joyy = input ? input->joy_y : 0;

    if (joyx)
        n64_turnheld[playernum] += ticdup;
    else
        n64_turnheld[playernum] = 0;

    if (n64_turnheld[playernum] < SLOWTURNTICS)
        tspeed = 2;
    else
        tspeed = speed;

    forward = 0;
    side = 0;

    if (joyx > 0)
        cmd->angleturn -= G_ScaleJoyAnalog(joyx, angleturn[tspeed]);
    if (joyx < 0)
        cmd->angleturn += G_ScaleJoyAnalog(joyx, angleturn[tspeed]);

    if (joyy < 0)
        forward += G_ScaleJoyAnalog(joyy, forwardmove[speed]);
    if (joyy > 0)
        forward -= G_ScaleJoyAnalog(joyy, forwardmove[speed]);

    if (input && input->strafe_right)
        side += sidemove[speed];
    if (input && input->strafe_left)
        side -= sidemove[speed];

    if (input && input->fire)
        cmd->buttons |= BT_ATTACK;

    if (input && input->use)
        cmd->buttons |= BT_USE;

    if (input && input->weapon_key >= '1' && input->weapon_key <= '7')
    {
        cmd->buttons |= BT_CHANGE;
        cmd->buttons |= (input->weapon_key - '1') << BT_WEAPONSHIFT;
    }

    if (forward > MAXPLMOVE)
        forward = MAXPLMOVE;
    else if (forward < -MAXPLMOVE)
        forward = -MAXPLMOVE;

    if (side > MAXPLMOVE)
        side = MAXPLMOVE;
    else if (side < -MAXPLMOVE)
        side = -MAXPLMOVE;

    cmd->forwardmove += forward;
    cmd->sidemove += side;

    if (playernum == consoleplayer)
    {
        if (sendpause)
        {
            sendpause = false;
            cmd->buttons = BT_SPECIAL | BTS_PAUSE;
        }

        if (sendsave)
        {
            sendsave = false;
            cmd->buttons = BT_SPECIAL | BTS_SAVEGAME | (savegameslot << BTS_SAVESHIFT);
        }
    }
}
#endif
 

//
// G_DoLoadLevel 
//
extern  gamestate_t     wipegamestate; 
 
void G_DoLoadLevel (void) 
{ 
    int             i; 

    // Set the sky map.
    // First thing, we have a dummy sky texture name,
    //  a flat. The data is in the WAD only because
    //  we look for an actual index, instead of simply
    //  setting one.
    skyflatnum = R_FlatNumForName ( SKYFLATNAME );

    // DOOM determines the sky texture to be used
    // depending on the current episode, and the game version.
    if ( (gamemode == commercial)
     || ( gamemission == pack_tnt )
     || ( gamemission == pack_plut ) )
    {
	skytexture = R_TextureNumForName ("SKY3");
	if (gamemap < 12)
	    skytexture = R_TextureNumForName ("SKY1");
	else
	    if (gamemap < 21)
		skytexture = R_TextureNumForName ("SKY2");
    }
    else
    {
	// DOOM 1 style episodes (covers extra episodes such as SIGIL's
	// episode 5 -> SKY5); also fixes sky on save-game loads which do
	// not pass through G_InitNew.
	skytexture = G_EpisodeSkyTexture (gameepisode);
    }

    levelstarttic = gametic;        // for time calculation
    
    if (wipegamestate == GS_LEVEL) 
	wipegamestate = -1;             // force a wipe 

    gamestate = GS_LEVEL; 

    for (i=0 ; i<MAXPLAYERS ; i++) 
    { 
	if (playeringame[i] && players[i].playerstate == PST_DEAD) 
	    players[i].playerstate = PST_REBORN; 
	memset (players[i].frags,0,sizeof(players[i].frags)); 
    } 
		 
    P_SetupLevel (gameepisode, gamemap, 0, gameskill);    
    displayplayer = consoleplayer;		// view the guy you are playing    
    starttime = I_GetTime (); 
    gameaction = ga_nothing; 
    Z_CheckHeap ();
    
    // clear cmd building stuff
    memset (gamekeydown, 0, sizeof(gamekeydown)); 
    joyxmove = joyymove = 0; 
    mousex = mousey = 0; 
    sendpause = sendsave = paused = false; 
    memset (mousearray, 0, sizeof(mousearray)); 
    memset (joyarray, 0, sizeof(joyarray)); 

#ifdef N64
    memset(n64_turnheld, 0, sizeof(n64_turnheld));
#endif
} 
 
 
//
// G_Responder  
// Get info needed to make ticcmd_ts for the players.
// 
boolean G_Responder (event_t* ev) 
{ 
    // allow spy mode changes even during the demo
    if (gamestate == GS_LEVEL && ev->type == ev_keydown 
	&& ev->data1 == KEY_F12 && (singledemo || !deathmatch) )
    {
	// spy mode 
	do 
	{ 
	    displayplayer++; 
	    if (displayplayer == MAXPLAYERS) 
		displayplayer = 0; 
	} while (!playeringame[displayplayer] && displayplayer != consoleplayer); 
	return true; 
    }
    
    // any other key pops up menu if in demos
    if (gameaction == ga_nothing && !singledemo && 
	(demoplayback || gamestate == GS_DEMOSCREEN) 
	) 
    { 
	if (ev->type == ev_keydown ||  
	    (ev->type == ev_mouse && ev->data1) || 
	    (ev->type == ev_joystick && ev->data1) ) 
	{ 
	    M_StartControlPanel (); 
	    return true; 
	} 
	return false; 
    } 
 
    if (gamestate == GS_LEVEL) 
    { 
#if 0 
	if (devparm && ev->type == ev_keydown && ev->data1 == ';') 
	{ 
	    G_DeathMatchSpawnPlayer (0); 
	    return true; 
	} 
#endif 
	if (HU_Responder (ev)) 
	    return true;	// chat ate the event 
	if (ST_Responder (ev)) 
	    return true;	// status window ate it 
	if (AM_Responder (ev)) 
	    return true;	// automap ate it 
    } 
	 
    if (gamestate == GS_FINALE) 
    { 
	if (F_Responder (ev)) 
	    return true;	// finale ate the event 
    } 
	 
    switch (ev->type) 
    { 
      case ev_keydown: 
	if (ev->data1 == KEY_PAUSE) 
	{ 
	    sendpause = true; 
	    return true; 
	} 
	if (ev->data1 <NUMKEYS) 
	    gamekeydown[ev->data1] = true; 
	return true;    // eat key down events 
 
      case ev_keyup: 
	if (ev->data1 <NUMKEYS) 
	    gamekeydown[ev->data1] = false; 
	return false;   // always let key up events filter down 
		 
      case ev_mouse: 
	mousebuttons[0] = ev->data1 & 1; 
	mousebuttons[1] = ev->data1 & 2; 
	mousebuttons[2] = ev->data1 & 4; 
	mousex = ev->data2*(mouseSensitivity+5)/10; 
	mousey = ev->data3*(mouseSensitivity+5)/10; 
	return true;    // eat events 
 
      case ev_joystick: 
	joybuttons[0] = ev->data1 & 1; 
	joybuttons[1] = ev->data1 & 2; 
	joybuttons[2] = ev->data1 & 4; 
	joybuttons[3] = ev->data1 & 8; 
	joyxmove = ev->data2; 
	joyymove = ev->data3; 
	return true;    // eat events 
 
      default: 
	break; 
    } 
 
    return false; 
} 
 
 
 
//
// G_Ticker
// Make ticcmd_ts for the players.
//
void G_Ticker (void) 
{ 
    int		i;
    int		buf; 
    ticcmd_t*	cmd;
    
    // do player reborns if needed
    for (i=0 ; i<MAXPLAYERS ; i++) 
	if (playeringame[i] && players[i].playerstate == PST_REBORN) 
	    G_DoReborn (i);
    
    // do things to change the game state
    while (gameaction != ga_nothing) 
    { 
	switch (gameaction) 
	{ 
	  case ga_loadlevel: 
	    G_DoLoadLevel (); 
	    break; 
	  case ga_newgame: 
	    G_DoNewGame (); 
	    break; 
	  case ga_loadgame: 
	    G_DoLoadGame (); 
	    break; 
	  case ga_savegame: 
	    G_DoSaveGame (); 
	    break; 
	  case ga_playdemo: 
	    G_DoPlayDemo (); 
	    break; 
	  case ga_completed: 
	    G_DoCompleted (); 
	    break; 
	  case ga_victory: 
	    F_StartFinale (); 
	    break; 
	  case ga_worlddone: 
	    G_DoWorldDone (); 
	    break; 
	  case ga_screenshot: 
	    M_ScreenShot (); 
	    gameaction = ga_nothing; 
	    break; 
	  case ga_nothing: 
	    break; 
	} 
    }
    
    // get commands, check consistancy,
    // and build new consistancy check
    buf = (gametic/ticdup)%BACKUPTICS; 
 
    for (i=0 ; i<MAXPLAYERS ; i++)
    {
	if (playeringame[i]) 
	{ 
	    cmd = &players[i].cmd; 
 
	    memcpy (cmd, &netcmds[i][buf], sizeof(ticcmd_t)); 
 
	    if (demoplayback) 
		G_ReadDemoTiccmd (cmd); 
	    if (demorecording) 
		G_WriteDemoTiccmd (cmd);
	    
	    // check for turbo cheats
	    if (cmd->forwardmove > TURBOTHRESHOLD 
		&& !(gametic&31) && ((gametic>>5)&3) == i )
	    {
		static char turbomessage[80];
		extern char *player_names[4];
		sprintf (turbomessage, "%s is turbo!",player_names[i]);
		players[consoleplayer].message = turbomessage;
	    }
			
	    if (netgame && !netdemo && !(gametic%ticdup) ) 
	    { 
		if (!D_LocalMultiplayerEnabled()
		    && gametic > BACKUPTICS
		    && consistancy[i][buf] != cmd->consistancy)
		{
		    I_Error ("consistency failure (%i should be %i)",
			     cmd->consistancy, consistancy[i][buf]); 
		}
		if (players[i].mo) 
		    consistancy[i][buf] = players[i].mo->x; 
		else 
		    consistancy[i][buf] = rndindex; 
	    } 
	}
    }
    
    // check for special buttons
    for (i=0 ; i<MAXPLAYERS ; i++)
    {
	if (playeringame[i]) 
	{ 
	    if (players[i].cmd.buttons & BT_SPECIAL) 
	    { 
		switch (players[i].cmd.buttons & BT_SPECIALMASK) 
		{ 
		  case BTS_PAUSE: 
		    paused ^= 1; 
		    if (paused) 
			S_PauseSound (); 
		    else 
			S_ResumeSound (); 
		    break; 
					 
		  case BTS_SAVEGAME: 
		    if (!savedescription[0]) 
			strcpy (savedescription, "NET GAME"); 
		    savegameslot =  
			(players[i].cmd.buttons & BTS_SAVEMASK)>>BTS_SAVESHIFT; 
		    gameaction = ga_savegame; 
		    break; 
		} 
	    } 
	}
    }
    
    // do main actions
    switch (gamestate) 
    { 
      case GS_LEVEL: 
	P_Ticker (); 
	ST_Ticker (); 
	AM_Ticker (); 
	HU_Ticker ();            
	break; 
	 
      case GS_INTERMISSION: 
	WI_Ticker (); 
	break; 
			 
      case GS_FINALE: 
	F_Ticker (); 
	break; 
 
      case GS_DEMOSCREEN:
	D_PageTicker ();
	break;
    }
#ifdef N64_BENCH
    // Once per gametic: advance scenario timing (warm-up / collect / finish).
    N64Bench_TicHook();
#endif
}
 
 
//
// PLAYER STRUCTURE FUNCTIONS
// also see P_SpawnPlayer in P_Things
//

//
// G_InitPlayer 
// Called at the start.
// Called by the game initialization functions.
//
void G_InitPlayer (int player) 
{ 
    player_t*	p; 
 
    // set up the saved info         
    p = &players[player]; 
	 
    // clear everything else to defaults 
    G_PlayerReborn (player); 
	 
} 
 
 

//
// G_PlayerFinishLevel
// Can when a player completes a level.
//
void G_PlayerFinishLevel (int player) 
{ 
    player_t*	p; 
	 
    p = &players[player]; 
	 
    memset (p->powers, 0, sizeof (p->powers)); 
    memset (p->cards, 0, sizeof (p->cards)); 
    p->mo->flags &= ~MF_SHADOW;		// cancel invisibility 
    p->extralight = 0;			// cancel gun flashes 
    p->fixedcolormap = 0;		// cancel ir gogles 
    p->damagecount = 0;			// no palette changes 
    p->bonuscount = 0; 
} 
 

//
// G_PlayerReborn
// Called after a player dies 
// almost everything is cleared and initialized 
//
void G_PlayerReborn (int player) 
{ 
    player_t*	p; 
    int		i; 
    int		frags[MAXPLAYERS]; 
    int		killcount;
    int		itemcount;
    int		secretcount; 
	 
    memcpy (frags,players[player].frags,sizeof(frags)); 
    killcount = players[player].killcount; 
    itemcount = players[player].itemcount; 
    secretcount = players[player].secretcount; 
	 
    p = &players[player]; 
    memset (p, 0, sizeof(*p)); 
 
    memcpy (players[player].frags, frags, sizeof(players[player].frags)); 
    players[player].killcount = killcount; 
    players[player].itemcount = itemcount; 
    players[player].secretcount = secretcount; 
 
    p->usedown = p->attackdown = true;	// don't do anything immediately 
    p->playerstate = PST_LIVE;       
    p->health = MAXHEALTH; 
    p->readyweapon = p->pendingweapon = wp_pistol; 
    p->weaponowned[wp_fist] = true; 
    p->weaponowned[wp_pistol] = true; 
    p->ammo[am_clip] = 50; 
	 
    for (i=0 ; i<NUMAMMO ; i++) 
	p->maxammo[i] = maxammo[i]; 
		 
}

//
// G_CheckSpot  
// Returns false if the player cannot be respawned
// at the given mapthing_t spot  
// because something is occupying it 
//
void P_SpawnPlayer (mapthing_t* mthing); 
 
boolean
G_CheckSpot
( int		playernum,
  mapthing_t*	mthing ) 
{ 
    fixed_t		x;
    fixed_t		y; 
    subsector_t*	ss; 
    unsigned		an; 
    mobj_t*		mo; 
    int			i;
	
    if (!players[playernum].mo)
    {
	// first spawn of level, before corpses
	for (i=0 ; i<playernum ; i++)
	    if (players[i].mo->x == mthing->x << FRACBITS
		&& players[i].mo->y == mthing->y << FRACBITS)
		return false;	
	return true;
    }
		
    x = mthing->x << FRACBITS; 
    y = mthing->y << FRACBITS; 
	 
    if (!P_CheckPosition (players[playernum].mo, x, y) ) 
	return false; 
 
    // flush an old corpse if needed 
    if (bodyqueslot >= BODYQUESIZE) 
	P_RemoveMobj (bodyque[bodyqueslot%BODYQUESIZE]); 
    bodyque[bodyqueslot%BODYQUESIZE] = players[playernum].mo; 
    bodyqueslot++; 
	
    // spawn a teleport fog 
    ss = R_PointInSubsector (x,y); 
    an = ( ANG45 * (mthing->angle/45) ) >> ANGLETOFINESHIFT; 
 
    mo = P_SpawnMobj (x+20*finecosine[an], y+20*finesine[an] 
		      , ss->sector->floorheight 
		      , MT_TFOG); 
	 
    if (players[consoleplayer].viewz != 1) 
	S_StartSound (mo, sfx_telept);	// don't start sound on first frame 
 
    return true; 
} 


//
// G_DeathMatchSpawnPlayer 
// Spawns a player at one of the random death match spots 
// called at level load and each death 
//
void G_DeathMatchSpawnPlayer (int playernum) 
{ 
    int             i,j; 
    int				selections; 
	 
    selections = deathmatch_p - deathmatchstarts; 
    if (selections < 4) 
	I_Error ("Only %i deathmatch spots, 4 required", selections); 
 
    for (j=0 ; j<20 ; j++) 
    { 
	i = P_Random() % selections; 
	if (G_CheckSpot (playernum, &deathmatchstarts[i]) ) 
	{ 
	    deathmatchstarts[i].type = playernum+1; 
	    P_SpawnPlayer (&deathmatchstarts[i]); 
	    return; 
	} 
    } 
 
    // no good spot, so the player will probably get stuck 
    P_SpawnPlayer (&playerstarts[playernum]); 
} 

//
// G_DoReborn 
// 
void G_DoReborn (int playernum) 
{ 
    int                             i; 
	 
    if (!netgame)
    {
	// reload the level from scratch
	gameaction = ga_loadlevel;  
    }
    else 
    {
	// respawn at the start

	// first dissasociate the corpse 
	players[playernum].mo->player = NULL;   
		 
	// spawn at random spot if in death match 
	if (deathmatch) 
	{ 
	    G_DeathMatchSpawnPlayer (playernum); 
	    return; 
	} 
		 
	if (G_CheckSpot (playernum, &playerstarts[playernum]) ) 
	{ 
	    P_SpawnPlayer (&playerstarts[playernum]); 
	    return; 
	}
	
	// try to spawn at one of the other players spots 
	for (i=0 ; i<MAXPLAYERS ; i++)
	{
	    if (G_CheckSpot (playernum, &playerstarts[i]) ) 
	    { 
		playerstarts[i].type = playernum+1;	// fake as other player 
		P_SpawnPlayer (&playerstarts[i]); 
		playerstarts[i].type = i+1;		// restore 
		return; 
	    }	    
	    // he's going to be inside something.  Too bad.
	}
	P_SpawnPlayer (&playerstarts[playernum]); 
    } 
} 
 
 
void G_ScreenShot (void) 
{ 
    gameaction = ga_screenshot; 
} 
 


// DOOM Par Times
int pars[4][10] = 
{ 
    {0}, 
    {0,30,75,120,90,165,180,180,30,165}, 
    {0,90,90,90,120,90,360,240,30,170}, 
    {0,90,45,90,150,90,90,165,30,135} 
}; 

// DOOM II Par Times
int cpars[32] =
{
    30,90,120,120,90,150,120,120,270,90,	//  1-10
    210,150,150,150,210,150,420,150,210,150,	// 11-20
    240,150,180,150,150,300,330,420,300,180,	// 21-30
    120,30					// 31-32
};
 

//
// G_DoCompleted 
//
boolean		secretexit; 
extern char*	pagename; 
 
void G_ExitLevel (void) 
{ 
    secretexit = false; 
    gameaction = ga_completed; 
} 

// Here's for the german edition.
void G_SecretExitLevel (void) 
{ 
    // IF NO WOLF3D LEVELS, NO SECRET EXIT!
    if ( (gamemode == commercial)
      && (W_CheckNumForName("map31")<0))
	secretexit = false;
    else
	secretexit = true; 
    gameaction = ga_completed; 
} 
 
void G_DoCompleted (void) 
{ 
    int             i; 
	 
    gameaction = ga_nothing; 
 
    for (i=0 ; i<MAXPLAYERS ; i++) 
	if (playeringame[i]) 
	    G_PlayerFinishLevel (i);        // take away cards and stuff 
	 
    if (automapactive) 
	AM_Stop (); 
	
    if ( gamemode != commercial)
	switch(gamemap)
	{
	  case 8:
	    gameaction = ga_victory;
	    return;
	  case 9: 
	    for (i=0 ; i<MAXPLAYERS ; i++) 
		players[i].didsecret = true; 
	    break;
	}
		
//#if 0  Hmmm - why?
    if ( (gamemap == 8)
	 && (gamemode != commercial) ) 
    {
	// victory 
	gameaction = ga_victory; 
	return; 
    } 
	 
    if ( (gamemap == 9)
	 && (gamemode != commercial) ) 
    {
	// exit secret level 
	for (i=0 ; i<MAXPLAYERS ; i++) 
	    players[i].didsecret = true; 
    } 
//#endif
    
	 
    wminfo.didsecret = players[consoleplayer].didsecret; 
    wminfo.epsd = gameepisode -1; 
    wminfo.last = gamemap -1;
    
    // wminfo.next is 0 biased, unlike gamemap
    if ( gamemode == commercial)
    {
	if (secretexit)
	    switch(gamemap)
	    {
	      case 15: wminfo.next = 30; break;
	      case 31: wminfo.next = 31; break;
	    }
	else
	    switch(gamemap)
	    {
	      case 31:
	      case 32: wminfo.next = 15; break;
	      default: wminfo.next = gamemap;
	    }
    }
    else
    {
	if (secretexit) 
	    wminfo.next = 8; 	// go to secret level 
	else if (gamemap == 9) 
	{
	    // returning from secret level 
	    switch (gameepisode) 
	    { 
	      case 1: 
		wminfo.next = 3; 
		break; 
	      case 2: 
		wminfo.next = 5; 
		break; 
	      case 3: 
		wminfo.next = 6; 
		break; 
	      case 4:
		wminfo.next = 2;
		break;
	      case 5:
		// SIGIL: secret level E5M9 returns to E5M7.
		wminfo.next = 6;
		break;
	    }                
	} 
	else 
	    wminfo.next = gamemap;          // go to next level 
    }
		 
    wminfo.maxkills = totalkills; 
    wminfo.maxitems = totalitems; 
    wminfo.maxsecret = totalsecret; 
    wminfo.maxfrags = 0; 
    if ( gamemode == commercial )
	wminfo.partime = 35*cpars[gamemap-1]; 
    else if (gameepisode >= 1 && gameepisode <= 3
	     && gamemap >= 1 && gamemap <= 9)
	wminfo.partime = 35*pars[gameepisode][gamemap]; 
    else
	// No par time defined for this episode/map (e.g. episode 4+,
	// SIGIL's episode 5); pars[][] only covers episodes 1-3.
	wminfo.partime = 0;
    wminfo.pnum = consoleplayer; 
 
    for (i=0 ; i<MAXPLAYERS ; i++) 
    { 
	wminfo.plyr[i].in = playeringame[i]; 
	wminfo.plyr[i].skills = players[i].killcount; 
	wminfo.plyr[i].sitems = players[i].itemcount; 
	wminfo.plyr[i].ssecret = players[i].secretcount; 
	wminfo.plyr[i].stime = leveltime; 
	memcpy (wminfo.plyr[i].frags, players[i].frags 
		, sizeof(wminfo.plyr[i].frags)); 
    } 
 
    gamestate = GS_INTERMISSION; 
    viewactive = false; 
    automapactive = false; 
 
    if (statcopy)
	memcpy (statcopy, &wminfo, sizeof(wminfo));
	
    WI_Start (&wminfo); 
} 


//
// G_WorldDone 
//
void G_WorldDone (void) 
{ 
    gameaction = ga_worlddone; 

    if (secretexit) 
	players[consoleplayer].didsecret = true; 

    if ( gamemode == commercial )
    {
	switch (gamemap)
	{
	  case 15:
	  case 31:
	    if (!secretexit)
		break;
	  case 6:
	  case 11:
	  case 20:
	  case 30:
	    F_StartFinale ();
	    break;
	}
    }
} 
 
void G_DoWorldDone (void) 
{        
    gamestate = GS_LEVEL; 
    gamemap = wminfo.next+1; 
    G_DoLoadLevel (); 
    gameaction = ga_nothing; 
    viewactive = true; 
} 
 


//
// G_InitFromSavegame
// Can be called by the startup code or the menu task. 
//
extern boolean setsizeneeded;
void R_ExecuteSetViewSize (void);

char	savename[256];

void G_LoadGame (char* name) 
{ 
    strcpy (savename, name); 
    gameaction = ga_loadgame; 
} 
 
#define VERSIONSIZE		16 


void G_DoLoadGame (void) 
{ 
#ifdef N64
    int		i;
    int		a,b,c;
    char	vcheck[VERSIONSIZE];
    FILE	*fp;
    long	note_size;
    size_t	read_bytes;
    uint32_t	compressed_size;
    unsigned int uncompressed_size;
    int		rv;
    joypad_port_t save_port;
    boolean	loaded;

    loaded = false;
    gameaction = ga_nothing;
    G_N64SetStatus("");

    save_port = G_N64ResolveSavePort();
    if (G_N64MountPak(save_port, true) < 0)
        goto done;

    G_N64BuildSaveNotePath(n64_save_note_path, sizeof(n64_save_note_path));

    fp = fopen(n64_save_note_path, "rb");
    if (!fp)
    {
        if (errno == ENOENT)
            G_N64SetStatus("Must save a game to load!");
        else
            snprintf(n64_save_status,
                     sizeof(n64_save_status),
                     "Load failed (%d:%s)",
                     errno,
                     strerror(errno));
        goto done_unmount;
    }

    if (fseek(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        G_N64SetStatus("Load failed while seeking save note.");
        goto done_unmount;
    }

    note_size = ftell(fp);
    if (note_size < (long)N64_CPAK_HEADER_SIZE || note_size > (long)N64_CPAK_PAYLOAD_MAX)
    {
        fclose(fp);
        G_N64SetStatus("Save note size invalid.");
        goto done_unmount;
    }

    if (fseek(fp, 0, SEEK_SET) != 0)
    {
        fclose(fp);
        G_N64SetStatus("Load failed while rewinding save note.");
        goto done_unmount;
    }

    read_bytes = fread(n64_cpak_payload, 1, (size_t)note_size, fp);
    fclose(fp);
    if (read_bytes != (size_t)note_size)
    {
        G_N64SetStatus("Load failed while reading save note.");
        goto done_unmount;
    }

    compressed_size = G_N64ReadU32(n64_cpak_payload);
    if (compressed_size < 1
        || compressed_size > N64_CPAK_COMPRESSED_MAX
        || compressed_size > (uint32_t)(note_size - N64_CPAK_HEADER_SIZE))
    {
        G_N64SetStatus("Save note data invalid.");
        goto done_unmount;
    }

    uncompressed_size = SAVEGAMESIZE;
    rv = lzfx_decompress(&n64_cpak_payload[N64_CPAK_HEADER_SIZE],
                         compressed_size,
                         n64_savebuffer,
                         &uncompressed_size);
    if (rv < 0)
    {
        G_N64SetStatus("Save decompression failed.");
        goto done_unmount;
    }

    savebuffer = n64_savebuffer;
    save_p = savebuffer + SAVESTRINGSIZE;

    memset(vcheck,0,sizeof(vcheck));
    sprintf(vcheck,"version %i",VERSION);
    if (strcmp ((char*)save_p, vcheck))
    {
        G_N64SetStatus("Save version mismatch.");
        goto done_unmount;
    }
    save_p += VERSIONSIZE;

    gameskill = *save_p++;
    gameepisode = *save_p++;
    gamemap = *save_p++;
    for (i=0 ; i<MAXPLAYERS ; i++)
        playeringame[i] = *save_p++;

    G_InitNew(gameskill, gameepisode, gamemap);

    a = *save_p++;
    b = *save_p++;
    c = *save_p++;
    leveltime = (a<<16) + (b<<8) + c;

    P_UnArchivePlayers();
    P_UnArchiveWorld();
    P_UnArchiveThinkers();
    P_UnArchiveSpecials();

    if (*save_p != 0x1d)
    {
        G_N64SetStatus("Save data corrupt.");
        goto done_unmount;
    }

    loaded = true;
    G_N64SetStatus("Loaded game from pak.");

done_unmount:
    G_N64UnmountPak(save_port);

done:
    players[consoleplayer].message = n64_save_status;

    if (loaded)
    {
        if (setsizeneeded)
            R_ExecuteSetViewSize();

        R_FillBackScreen();
    }
    return;
#else
    int		length; 
    int		i; 
    int		a,b,c; 
    char	vcheck[VERSIONSIZE]; 
	 
    gameaction = ga_nothing; 
	 
    length = M_ReadFile (savename, &savebuffer); 
    save_p = savebuffer + SAVESTRINGSIZE;
    
    // skip the description field 
    memset (vcheck,0,sizeof(vcheck)); 
    sprintf (vcheck,"version %i",VERSION); 
    if (strcmp ((char*)save_p, vcheck)) 
	return;				// bad version 
    save_p += VERSIONSIZE; 
			 
    gameskill = *save_p++; 
    gameepisode = *save_p++; 
    gamemap = *save_p++; 
    for (i=0 ; i<MAXPLAYERS ; i++) 
	playeringame[i] = *save_p++; 

    // load a base level 
    G_InitNew (gameskill, gameepisode, gamemap); 
 
    // get the times 
    a = *save_p++; 
    b = *save_p++; 
    c = *save_p++; 
    leveltime = (a<<16) + (b<<8) + c; 
	 
    // dearchive all the modifications
    P_UnArchivePlayers (); 
    P_UnArchiveWorld (); 
    P_UnArchiveThinkers (); 
    P_UnArchiveSpecials (); 
 
    if (*save_p != 0x1d) 
	I_Error ("Bad savegame");
    
    // done 
    Z_Free (savebuffer); 
 
    if (setsizeneeded)
	R_ExecuteSetViewSize ();
    
    // draw the pattern into the back screen
    R_FillBackScreen ();   
#endif
} 
 

//
// G_SaveGame
// Called by the menu task.
// Description is a 24 byte text string 
//
void
G_SaveGame
( int	slot,
  char*	description ) 
{ 
    savegameslot = slot; 
    strcpy (savedescription, description); 
    sendsave = true; 
} 
 
void G_DoSaveGame (void) 
{ 
#ifdef N64
    char	name2[VERSIONSIZE];
    char	*description;
    int		length;
    int		i;
    unsigned int compressed_size;
    uint32_t payload_size;
    int		rv;
    FILE	*fp;
    size_t	written;
    joypad_port_t save_port;

    gameaction = ga_nothing;
    G_N64SetStatus("");

    description = savedescription;

    memset(n64_savebuffer, 0, sizeof(n64_savebuffer));
    savebuffer = n64_savebuffer;
    save_p = savebuffer;

    memcpy(save_p, description, SAVESTRINGSIZE);
    save_p += SAVESTRINGSIZE;
    memset(name2,0,sizeof(name2));
    sprintf(name2,"version %i",VERSION);
    memcpy(save_p, name2, VERSIONSIZE);
    save_p += VERSIONSIZE;

    *save_p++ = gameskill;
    *save_p++ = gameepisode;
    *save_p++ = gamemap;
    for (i=0 ; i<MAXPLAYERS ; i++)
        *save_p++ = playeringame[i];
    *save_p++ = leveltime>>16;
    *save_p++ = leveltime>>8;
    *save_p++ = leveltime;

    P_ArchivePlayers();
    P_ArchiveWorld();
    P_ArchiveThinkers();
    P_ArchiveSpecials();

    *save_p++ = 0x1d;

    length = save_p - savebuffer;
    if (length > SAVEGAMESIZE)
        I_Error("Savegame buffer overrun");

    compressed_size = N64_CPAK_COMPRESSED_MAX;
    rv = lzfx_compress(savebuffer,
                       (unsigned int)length,
                       &n64_cpak_payload[N64_CPAK_HEADER_SIZE],
                       &compressed_size);
    if (rv < 0)
    {
        G_N64SetStatus("Save too large for Controller Pak.");
        goto done;
    }

    payload_size = compressed_size + N64_CPAK_HEADER_SIZE;
    G_N64WriteU32(n64_cpak_payload, compressed_size);

    save_port = G_N64ResolveSavePort();
    if (G_N64MountPak(save_port, true) < 0)
        goto done;

    G_N64BuildSaveNotePath(n64_save_note_path, sizeof(n64_save_note_path));
    fp = fopen(n64_save_note_path, "wb");
    if (!fp)
    {
        if (errno == ENOSPC)
            G_N64SetStatus("Mempak full. Save failed.");
        else if (errno == EMFILE)
            G_N64SetStatus("Mempak note table full.");
        else
            snprintf(n64_save_status,
                     sizeof(n64_save_status),
                     "Save failed (%d:%s)",
                     errno,
                     strerror(errno));
        goto done_unmount;
    }

    written = fwrite(n64_cpak_payload, 1, payload_size, fp);
    if (fclose(fp) != 0 || written != payload_size)
    {
        if (errno == ENOSPC)
            G_N64SetStatus("Mempak full. Save failed.");
        else
            G_N64SetStatus("Save write failed.");
        goto done_unmount;
    }

    G_N64SetStatus("Saved game to pak.");

done_unmount:
    G_N64UnmountPak(save_port);

done:
    savedescription[0] = 0;
    players[consoleplayer].message = n64_save_status;
    R_FillBackScreen();
    return;
#else
    char	name[100]; 
    char	name2[VERSIONSIZE]; 
    char*	description; 
    int		length; 
    int		i; 
	
    if (M_CheckParm("-cdrom"))
	sprintf(name,"c:\\doomdata\\"SAVEGAMENAME"%d.dsg",savegameslot);
    else
	sprintf (name,SAVEGAMENAME"%d.dsg",savegameslot); 
    description = savedescription; 
	 
    save_p = savebuffer = screens[1]+0x4000; 
	 
    memcpy (save_p, description, SAVESTRINGSIZE); 
    save_p += SAVESTRINGSIZE; 
    memset (name2,0,sizeof(name2)); 
    sprintf (name2,"version %i",VERSION); 
    memcpy (save_p, name2, VERSIONSIZE); 
    save_p += VERSIONSIZE; 
	 
    *save_p++ = gameskill; 
    *save_p++ = gameepisode; 
    *save_p++ = gamemap; 
    for (i=0 ; i<MAXPLAYERS ; i++) 
	*save_p++ = playeringame[i]; 
    *save_p++ = leveltime>>16; 
    *save_p++ = leveltime>>8; 
    *save_p++ = leveltime; 
 
    P_ArchivePlayers (); 
    P_ArchiveWorld (); 
    P_ArchiveThinkers (); 
    P_ArchiveSpecials (); 
	 
    *save_p++ = 0x1d;		// consistancy marker 
	 
    length = save_p - savebuffer; 
    if (length > SAVEGAMESIZE) 
	I_Error ("Savegame buffer overrun"); 
    M_WriteFile (name, savebuffer, length); 
    gameaction = ga_nothing; 
    savedescription[0] = 0;		 
	 
    players[consoleplayer].message = GGSAVED; 

    // draw the pattern into the back screen
    R_FillBackScreen ();	
#endif
} 
 

//
// G_InitNew
// Can be called by the startup code or the menu task,
// consoleplayer, displayplayer, playeringame[] should be set. 
//
skill_t	d_skill; 
int     d_episode; 
int     d_map; 
 
void
G_DeferedInitNew
( skill_t	skill,
  int		episode,
  int		map) 
{ 
    d_skill = skill; 
    d_episode = episode; 
    d_map = map; 
    gameaction = ga_newgame; 
} 


void G_DoNewGame (void) 
{
    int i;

    demoplayback = false; 
    netdemo = false;

    if (D_LocalMultiplayerEnabled())
    {
        netgame = true;
        if (!M_CheckParm("-deathmatch") && !M_CheckParm("-altdeath"))
            deathmatch = false;
        for (i = 0; i < MAXPLAYERS; i++)
            playeringame[i] = (i < D_GetLocalPlayerCount());

        // Expand doomcom/nodeingame to match the chosen local player count so
        // NetUpdate iterates every local player.
        if (doomcom)
        {
            extern boolean nodeingame[MAXNETNODES];
            int np = D_GetLocalPlayerCount();
            doomcom->numplayers = np;
            doomcom->numnodes = np;
            for (i = 0; i < np && i < MAXNETNODES; i++)
                nodeingame[i] = true;
        }
    }
    else
    {
        netgame = false;
        deathmatch = false;
        playeringame[1] = playeringame[2] = playeringame[3] = 0;
    }

    respawnparm = false;
    fastparm = false;
    nomonsters = false;
    consoleplayer = 0;
    G_InitNew (d_skill, d_episode, d_map); 
    gameaction = ga_nothing; 
} 

// The sky texture to be used instead of the F_SKY1 dummy.
extern  int	skytexture; 


//
// G_EpisodeSkyTexture
// Resolve the sky texture name for a DOOM 1 style episode. Episodes 1-4
// use the stock SKY1-SKY4; extra episodes (e.g. SIGIL's episode 5 uses
// SKY5) resolve SKY<episode> when present, falling back to SKY1.
//
static int G_EpisodeSkyTexture (int episode)
{
    char	skyname[9];
    int		tex;

    switch (episode)
    {
      case 1: return R_TextureNumForName ("SKY1");
      case 2: return R_TextureNumForName ("SKY2");
      case 3: return R_TextureNumForName ("SKY3");
      case 4: return R_TextureNumForName ("SKY4");
      default:
	break;
    }

    if (episode >= 5 && episode <= 9)
    {
	skyname[0] = 'S';
	skyname[1] = 'K';
	skyname[2] = 'Y';
	skyname[3] = (char)('0' + episode);
	skyname[4] = 0;

	tex = R_CheckTextureNumForName (skyname);
	if (tex >= 0)
	    return tex;
    }

    return R_TextureNumForName ("SKY1");
}


void
G_InitNew
( skill_t	skill,
  int		episode,
  int		map ) 
{ 
    int             i; 
	 
    if (paused) 
    { 
	paused = false; 
	S_ResumeSound (); 
    } 
	

    if (skill > sk_nightmare) 
	skill = sk_nightmare;


    // This was quite messy with SPECIAL and commented parts.
    // Supposedly hacks to make the latest edition work.
    // It might not work properly.
    if (episode < 1)
      episode = 1; 

    if ( gamemode == retail )
    {
      // Allow extra episodes detected in the loaded WAD set (e.g. SIGIL
      // adds a 5th episode); fall back to the stock four otherwise.
      int maxepisode = (d_episodes > 4) ? d_episodes : 4;
      if (episode > maxepisode)
	episode = maxepisode;
    }
    else if ( gamemode == shareware )
    {
      if (episode > 1) 
	   episode = 1;	// only start episode 1 on shareware
    }  
    else
    {
      if (episode > 3)
	episode = 3;
    }
    

  
    if (map < 1) 
	map = 1;
    
    if ( (map > 9)
	 && ( gamemode != commercial) )
      map = 9; 
		 
    M_ClearRandom (); 
	 
    if (skill == sk_nightmare || respawnparm )
	respawnmonsters = true;
    else
	respawnmonsters = false;
		
    if (fastparm || (skill == sk_nightmare && gameskill != sk_nightmare) )
    { 
	for (i=S_SARG_RUN1 ; i<=S_SARG_PAIN2 ; i++) 
	    states[i].tics >>= 1; 
	mobjinfo[MT_BRUISERSHOT].speed = 20*FRACUNIT; 
	mobjinfo[MT_HEADSHOT].speed = 20*FRACUNIT; 
	mobjinfo[MT_TROOPSHOT].speed = 20*FRACUNIT; 
    } 
    else if (skill != sk_nightmare && gameskill == sk_nightmare) 
    { 
	for (i=S_SARG_RUN1 ; i<=S_SARG_PAIN2 ; i++) 
	    states[i].tics <<= 1; 
	mobjinfo[MT_BRUISERSHOT].speed = 15*FRACUNIT; 
	mobjinfo[MT_HEADSHOT].speed = 10*FRACUNIT; 
	mobjinfo[MT_TROOPSHOT].speed = 10*FRACUNIT; 
    } 
	 
			 
    // force players to be initialized upon first level load         
    for (i=0 ; i<MAXPLAYERS ; i++) 
	players[i].playerstate = PST_REBORN; 
 
    usergame = true;                // will be set false if a demo 
    paused = false; 
    demoplayback = false; 
    automapactive = false; 
    viewactive = true; 
    gameepisode = episode; 
    gamemap = map; 
    gameskill = skill; 
 
    viewactive = true;
    
    // set the sky map for the episode
    if ( gamemode == commercial)
    {
	skytexture = R_TextureNumForName ("SKY3");
	if (gamemap < 12)
	    skytexture = R_TextureNumForName ("SKY1");
	else
	    if (gamemap < 21)
		skytexture = R_TextureNumForName ("SKY2");
    }
    else
	switch (episode) 
	{ 
	  case 1: 
	    skytexture = R_TextureNumForName ("SKY1"); 
	    break; 
	  case 2: 
	    skytexture = R_TextureNumForName ("SKY2"); 
	    break; 
	  case 3: 
	    skytexture = R_TextureNumForName ("SKY3"); 
	    break; 
	  case 4:	// Special Edition sky
	    skytexture = R_TextureNumForName ("SKY4");
	    break;
	  default:	// extra episodes (e.g. SIGIL E5 uses SKY5)
	    skytexture = G_EpisodeSkyTexture (episode);
	    break;
	} 
 
    G_DoLoadLevel (); 
} 
 

//
// DEMO RECORDING 
// 
#define DEMOMARKER		0x80


void G_ReadDemoTiccmd (ticcmd_t* cmd) 
{ 
    if (*demo_p == DEMOMARKER) 
    {
	// end of demo data stream 
	G_CheckDemoStatus (); 
	return; 
    } 
    cmd->forwardmove = ((signed char)*demo_p++); 
    cmd->sidemove = ((signed char)*demo_p++); 
    cmd->angleturn = ((unsigned char)*demo_p++)<<8; 
    cmd->buttons = (unsigned char)*demo_p++; 
} 


void G_WriteDemoTiccmd (ticcmd_t* cmd) 
{ 
    if (gamekeydown['q'])           // press q to end demo recording 
	G_CheckDemoStatus (); 
    *demo_p++ = cmd->forwardmove; 
    *demo_p++ = cmd->sidemove; 
    *demo_p++ = (cmd->angleturn+128)>>8; 
    *demo_p++ = cmd->buttons; 
    demo_p -= 4; 
    if (demo_p > demoend - 16)
    {
	// no more space 
	G_CheckDemoStatus (); 
	return; 
    } 
	
    G_ReadDemoTiccmd (cmd);         // make SURE it is exactly the same 
} 
 
 
 
//
// G_RecordDemo 
// 
void G_RecordDemo (char* name) 
{ 
    int             i; 
    int				maxsize;
	
    usergame = false; 
    strcpy (demoname, name); 
    strcat (demoname, ".lmp"); 
    maxsize = 0x20000;
    i = M_CheckParm ("-maxdemo");
    if (i && i<myargc-1)
	maxsize = atoi(myargv[i+1])*1024;
    demobuffer = Z_Malloc (maxsize,PU_STATIC,NULL); 
    demoend = demobuffer + maxsize;
	
    demorecording = true; 
} 
 
 
void G_BeginRecording (void) 
{ 
    int             i; 
		
    demo_p = demobuffer;
	
    *demo_p++ = VERSION;
    *demo_p++ = gameskill; 
    *demo_p++ = gameepisode; 
    *demo_p++ = gamemap; 
    *demo_p++ = deathmatch; 
    *demo_p++ = respawnparm;
    *demo_p++ = fastparm;
    *demo_p++ = nomonsters;
    *demo_p++ = consoleplayer;
	 
    for (i=0 ; i<MAXPLAYERS ; i++) 
	*demo_p++ = playeringame[i]; 		 
} 
 

//
// G_PlayDemo 
//

char*	defdemoname; 
 
void G_DeferedPlayDemo (char* name) 
{ 
    defdemoname = name; 
    gameaction = ga_playdemo; 
} 
 
void G_DoPlayDemo (void) 
{ 
    skill_t skill; 
    int             i, episode, map; 
	 
    gameaction = ga_nothing; 
    demobuffer = demo_p = W_CacheLumpName (defdemoname, PU_STATIC); 
    if ( *demo_p++ != VERSION)
    {
      fprintf( stderr, "Demo is from a different game version!\n");
      gameaction = ga_nothing;
      return;
    }
    
    skill = *demo_p++; 
    episode = *demo_p++; 
    map = *demo_p++; 
    deathmatch = *demo_p++;
    respawnparm = *demo_p++;
    fastparm = *demo_p++;
    nomonsters = *demo_p++;
    consoleplayer = *demo_p++;
	
    for (i=0 ; i<MAXPLAYERS ; i++) 
	playeringame[i] = *demo_p++; 
    if (playeringame[1]) 
    { 
	netgame = true; 
	netdemo = true; 
    }

    // don't spend a lot of time in loadlevel 
    precache = false;
    G_InitNew (skill, episode, map); 
    precache = true; 

    usergame = false; 
    demoplayback = true; 
} 

//
// G_TimeDemo 
//
void G_TimeDemo (char* name) 
{ 	 
    nodrawers = M_CheckParm ("-nodraw"); 
    noblit = M_CheckParm ("-noblit"); 
    timingdemo = true; 
    singletics = true; 

    defdemoname = name; 
    gameaction = ga_playdemo; 
} 
 
 
/* 
=================== 
= 
= G_CheckDemoStatus 
= 
= Called after a death or level completion to allow demos to be cleaned up 
= Returns true if a new demo loop action will take place 
=================== 
*/ 
 
boolean G_CheckDemoStatus (void) 
{ 
    int             endtime; 
	 
    if (timingdemo) 
    { 
	endtime = I_GetTime (); 
	I_Error ("timed %i gametics in %i realtics",gametic 
		 , endtime-starttime); 
    } 
	 
    if (demoplayback) 
    { 
	if (singledemo) 
	    I_Quit (); 
			 
	Z_ChangeTag (demobuffer, PU_CACHE); 
	demoplayback = false; 
	netdemo = false;
	netgame = false;
	deathmatch = false;
	playeringame[1] = playeringame[2] = playeringame[3] = 0;
	respawnparm = false;
	fastparm = false;
	nomonsters = false;
	consoleplayer = 0;
	D_AdvanceDemo (); 
	return true; 
    } 
 
    if (demorecording) 
    { 
	*demo_p++ = DEMOMARKER; 
	M_WriteFile (demoname, demobuffer, demo_p - demobuffer); 
	Z_Free (demobuffer); 
	demorecording = false; 
	I_Error ("Demo %s recorded",demoname); 
    } 
	 
    return false; 
} 
 
 
 
