// N64 video and input layer using libdragon + RDPQ.

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <libdragon.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_main.h"
#include "i_system.h"
#include "i_video.h"
#include "v_video.h"
#include "r_defs.h"
#include "r_state.h"
#include "m_swap.h"
#include "w_wad.h"
#include "z_zone.h"
#include "n64_debug.h"
#include "rdp_view.h"
#ifdef N64_BENCH
#include "n64_bench.h"
#endif

// Rebase hooks for code that caches a screens[0]-derived pointer across frames.
void R_N64RebaseScreen(void);
void AM_N64RebaseScreen(void);
void F_N64WipeRebaseScreen(void);

#define N64_CI8_BUFFERS 2

static boolean video_initialized;
static surface_t doom_screen8[N64_CI8_BUFFERS];
// Set true when a buffer is handed to the RDP, cleared (under interrupt) when
// the RDP has finished reading it. The CPU must not overwrite a buffer that is
// still busy. Updated from an interrupt callback, so volatile.
static volatile boolean doom_screen8_rdp_busy[N64_CI8_BUFFERS];
static surface_t* doom_screen8_disp[N64_CI8_BUFFERS]; // framebuffer to show per buffer
static int n64_draw_idx;                 // CI8 buffer the CPU draws into (== screens[0])
static boolean n64_palette_dirty;        // TLUT needs re-upload

// Transparent-key overlay model (RDP renderer, Stage 1+). One palette index is
// reserved as the transparency key: its TLUT entry is forced to alpha=0 (every
// other entry stays alpha=1), so the present blit's COPY-mode alpha-compare
// discards key-index pixels in the 3D-view window and reveals the RDP-drawn
// world underneath. The index is chosen at startup so no UI/HUD/status/font/
// menu patch drawn outside the 3D view uses it (I_N64ScanTransparencyKey). -1
// until the scan runs; the scan asserts if no index is free.
int n64_rdp_key_index = -1;
boolean n64_present_copy_forward;        // wipe: copy presented frame into next draw buffer
// RDP melt-wipe (mesh fix). The CI8 wipe melts screens[], which on the RDP path hold
// only HUD + key-clear (the world is in the 16bpp fb), so it dissolves garbage. The
// present (I_FinishUpdate) instead melts the COMPOSITED 16bpp frames captured here:
// _start = the old frame (last shown), _end = the new frame (this present's composite).
// The per-column drip uses the CI8 wipe's own offsets (F_N64WipeMeltY) so no extra
// M_Random is consumed (demo determinism). 320*200*2 = 128 KB each -- Z_Malloc'd from
// DOOM's zone (NOT static BSS / surface_alloc: that heap is exhausted at startup, the
// same trap the mesh z-buffer hit, and 256 KB of BSS starved the CI8 surface_alloc).
static uint16_t* n64_wipe16_start;
static uint16_t* n64_wipe16_end;
static int       n64_wipe16_state;       // 0 = idle, 1 = capturing/running
static surface_t* n64_last_shown_disp;   // the 16bpp fb most recently display_show'd
extern int* F_N64WipeMeltY(void);        // f_wipe.c: live per-column melt offsets, or NULL
static byte* n64_aux_screens[3];
static boolean n64_aux_screen_owned[3];
// I_SetPalette writes the CPU-side master; the present path copies it into an
// upload slot keyed by the CI8 buffer being presented. A buffer's previous
// TLUT load is ordered before its previous blit in the command stream, so once
// the buffer is free for CPU reuse its slot is free to rewrite -- a queued
// LOAD_TLUT can never see a rewrite, however fast the palette churns.
static uint16_t doom_tlut_master[256];

// Palette GENERATION: bumped ONLY when I_SetPalette actually rewrites the master
// (a real palette/flash change -- damage red, pickup, radsuit green, invuln). The
// CI4 wall pass reads this to re-tint its sub-palettes when the flash changes, so
// walls track the world flash (they sample their own 16-entry sub-palettes built
// from the BASE palette, not the master, so without this they would stay un-
// flashed). NOT bumped by I_N64ForceTLUTReupload / I_N64MarkPaletteDirty (those
// only re-arm the existing master's upload, no colour change), so the wall re-tint
// fires once per flash, not every frame.
static uint32_t doom_palette_gen;
uint32_t I_N64PaletteGen(void) { return doom_palette_gen; }

// The UN-FLASHED base palette (PLAYPAL palette 0), packed RGBA5551 exactly like
// the master (same gamma + key alpha). Captured the first time I_SetPalette is
// handed the base palette (st_palette transitions through 0 on every flash
// fade-out, and the level-load path sets it before any flash) and never tracks a
// flash afterward. The RDP plane pass uploads THIS instead of the flashed master
// so its CI8 floor texels carry the un-flashed colours; the screen-space flash
// overlay (DL_FlushPlanePolys) then washes them uniformly, matching software's
// single palette remap. Without un-flashing TEX0 the overlay would double-tint
// (flashed-TEX0 multiply + overlay). doom_base_valid gates the whole overlay
// path -- until a base palette has been seen the planes keep sampling the master.
static uint16_t doom_tlut_base[256];
static boolean  doom_base_valid;
// 24-bit RGB of the active uniform flash tint and its 0..255 strength, recovered
// from the flashed master vs the base (see I_N64SetFlashFromPalette). 0 strength
// == no flash (palette 0) -> the plane pass draws NO overlay. Updated every
// I_SetPalette so it always reflects the live master.
static uint8_t  doom_flash_r, doom_flash_g, doom_flash_b;
static int      doom_flash_a;    // 0..255; 0 = no overlay
static void     I_N64SetFlashFromPalette(void);   // defined below; used in I_SetPalette

// The live (flashed) master TLUT (RGBA5551, 256 entries). The CI4 wall pass
// re-derives each sub-palette entry's colour from the master at its fixed PLAYPAL
// index so wall colours track palette flashes. Returned as a pointer so the wall
// re-tint indexes it directly (no per-entry cross-TU call on flash frames).
const uint16_t* I_N64MasterTLUT(void) { return doom_tlut_master; }
static uint16_t doom_tlut_up[2][256] __attribute__((aligned(16)));
// DEDICATED scratch for the fixedcolormap-composed plane TLUT -- must NOT share
// doom_tlut_up: rdpq_tex_upload_tlut records the source's PHYSICAL address into the
// rspq stream and the LOAD_TLUT DMAs it at RDP-execution time (async). The plane pass
// re-asserts the master into doom_tlut_up AFTER the plane draws; if the composed TLUT
// lived in doom_tlut_up, that re-assert would overwrite the buffer before the RDP ran
// the plane's deferred LOAD_TLUT, so the planes would sample the master (the floors
// came out un-inverted under invuln). Its own buffer keeps the composed source intact
// until the RDP consumes it (same reasoning as the CI4 walls' per-slot sub-palettes).
static uint16_t doom_tlut_fcm[2][256] __attribute__((aligned(16)));
static uint64_t last_menu_present_ms;
static boolean n64_split_active;
static int n64_split_player_count;
static int n64_split_prev_count;
static n64_local_input_t n64_local_inputs[MAXPLAYERS];
static joypad_port_t n64_last_active_port = JOYPAD_PORT_1;
static boolean n64_local_weapon_cycle_down[MAXPLAYERS];
static boolean n64_local_weapon_prev_down[MAXPLAYERS];
static boolean n64_local_weapon_next_down[MAXPLAYERS];
static int n64_local_next_weapon_cycle_key[MAXPLAYERS];

#define STICK_ANALOG_MAX 80
#define STICK_ANALOG_DEADZONE 6
#define STICK_MENU_THRESHOLD 16

enum
{
    KEYIDX_MENU,
    KEYIDX_MAP_TOGGLE,
    KEYIDX_PAUSE,
    KEYIDX_A_MENU_ENTER,
    KEYIDX_A_USE,
    KEYIDX_B_MENU_BACK,
    KEYIDX_MENU_CONFIRM_Y,
    KEYIDX_MENU_CONFIRM_N,
    KEYIDX_FIRE,
    KEYIDX_STRAFE_LEFT,
    KEYIDX_STRAFE_RIGHT,
    KEYIDX_SPEED,
    KEYIDX_COUNT
};

static boolean key_state[KEYIDX_COUNT];
static boolean weapon_cycle_down;
static boolean weapon_prev_down;
static boolean weapon_next_down;
static int weapon_cycle_key;
static int weapon_prev_key;
static int weapon_next_key;
static int next_weapon_cycle_key = '1';

static boolean I_IsWeaponKeySelectable(player_t* player, int key)
{
    int newweapon;

    if (key < '1' || key > '7')
        return false;

    newweapon = key - '1';

    if (newweapon == wp_fist
        && player->weaponowned[wp_chainsaw]
        && !(player->readyweapon == wp_chainsaw && player->powers[pw_strength]))
    {
        newweapon = wp_chainsaw;
    }

    if (gamemode == commercial
        && newweapon == wp_shotgun
        && player->weaponowned[wp_supershotgun]
        && player->readyweapon != wp_supershotgun)
    {
        newweapon = wp_supershotgun;
    }

    if (!player->weaponowned[newweapon] || newweapon == player->readyweapon)
        return false;

    if ((newweapon == wp_plasma || newweapon == wp_bfg) && gamemode == shareware)
        return false;

    return true;
}

static int I_GetNextSelectableWeaponKeyForPlayer(int playernum, int* next_cycle_key)
{
    int i;
    int key;
    player_t* player;

    if (!next_cycle_key)
        return 0;

    if (playernum < 0 || playernum >= MAXPLAYERS || !playeringame[playernum])
        return 0;

    player = &players[playernum];
    key = *next_cycle_key;
    if (key < '1' || key > '7')
        key = '1';

    for (i = 0; i < 7; i++)
    {
        if (I_IsWeaponKeySelectable(player, key))
            return key;

        key++;
        if (key > '7')
            key = '1';
    }

    return 0;
}

static int I_WeaponToKey(weapontype_t weapon)
{
    switch (weapon)
    {
        case wp_fist:
        case wp_chainsaw:
            return '1';
        case wp_pistol:
            return '2';
        case wp_shotgun:
        case wp_supershotgun:
            return '3';
        case wp_chaingun:
            return '4';
        case wp_missile:
            return '5';
        case wp_plasma:
            return '6';
        case wp_bfg:
            return '7';
        default:
            return '2';
    }
}

static int I_GetDirectionalWeaponKeyForPlayer(int playernum, boolean next)
{
    int key;
    int current_key;
    player_t* player;
    weapontype_t current_weapon;

    if (playernum < 0 || playernum >= MAXPLAYERS || !playeringame[playernum])
        return 0;

    player = &players[playernum];
    current_weapon = player->readyweapon;
    if (player->pendingweapon != wp_nochange)
        current_weapon = player->pendingweapon;

    current_key = I_WeaponToKey(current_weapon);

    if (next)
    {
        for (key = current_key + 1; key <= '7'; key++)
            if (I_IsWeaponKeySelectable(player, key))
                return key;
    }
    else
    {
        for (key = current_key - 1; key >= '1'; key--)
            if (I_IsWeaponKeySelectable(player, key))
                return key;
    }

    return 0;
}

static void I_PostKeyEvent(evtype_t type, int key)
{
    event_t event;

    event.type = type;
    event.data1 = key;
    event.data2 = 0;
    event.data3 = 0;

    D_PostEvent(&event);
}

static void I_PostJoystickEvent(int buttons, int x, int y)
{
    event_t event;

    event.type = ev_joystick;
    event.data1 = buttons;
    event.data2 = x;
    event.data3 = y;

    D_PostEvent(&event);
}

static int I_NormalizeStickAxis(int value)
{
    int sign;
    int magnitude;
    int range;

    if (!value)
        return 0;

    sign = (value < 0) ? -1 : 1;
    magnitude = (value < 0) ? -value : value;

    if (magnitude <= STICK_ANALOG_DEADZONE)
        return 0;

    if (magnitude > STICK_ANALOG_MAX)
        magnitude = STICK_ANALOG_MAX;

    range = STICK_ANALOG_MAX - STICK_ANALOG_DEADZONE;
    magnitude = ((magnitude - STICK_ANALOG_DEADZONE) * STICK_ANALOG_MAX + (range / 2)) / range;

    if (magnitude > STICK_ANALOG_MAX)
        magnitude = STICK_ANALOG_MAX;

    return sign * magnitude;
}

static int I_MenuAxisStep(int value)
{
    if (value > STICK_MENU_THRESHOLD)
        return 1;
    if (value < -STICK_MENU_THRESHOLD)
        return -1;
    return 0;
}

static int I_PortHasActivity(joypad_port_t port)
{
    joypad_buttons_t pressed;
    joypad_inputs_t inputs;

    if (!joypad_is_connected(port))
        return 0;

    pressed = joypad_get_buttons_pressed(port);
    if (pressed.raw)
        return 1;

    inputs = joypad_get_inputs(port);
    return (inputs.stick_x > 48 || inputs.stick_x < -48
        || inputs.stick_y > 48 || inputs.stick_y < -48);
}

static void I_UpdateKeyState(boolean down, int state_index, int keycode)
{
    if (down && !key_state[state_index])
        I_PostKeyEvent(ev_keydown, keycode);
    else if (!down && key_state[state_index])
        I_PostKeyEvent(ev_keyup, keycode);

    key_state[state_index] = down;
}

static void I_UpdateWeaponCycle(boolean down)
{
    int next_key;

    if (down && !weapon_cycle_down)
    {
        next_key = I_GetNextSelectableWeaponKeyForPlayer(consoleplayer, &next_weapon_cycle_key);
        weapon_cycle_key = next_key;

        if (next_key)
        {
            I_PostKeyEvent(ev_keydown, weapon_cycle_key);

            next_weapon_cycle_key = weapon_cycle_key + 1;
            if (next_weapon_cycle_key > '7')
                next_weapon_cycle_key = '1';
        }
    }
    else if (!down && weapon_cycle_down && weapon_cycle_key)
    {
        I_PostKeyEvent(ev_keyup, weapon_cycle_key);
    }

    weapon_cycle_down = down;
}

static void I_UpdateWeaponSelect(boolean down, boolean next, boolean* select_down, int* select_key)
{
    int key;

    if (down && !*select_down)
    {
        key = I_GetDirectionalWeaponKeyForPlayer(consoleplayer, next);
        *select_key = key;
        if (key)
            I_PostKeyEvent(ev_keydown, key);
    }
    else if (!down && *select_down && *select_key)
    {
        I_PostKeyEvent(ev_keyup, *select_key);
    }

    *select_down = down;
}

static void I_UpdateLocalWeaponInput(int playernum,
                                     joypad_buttons_t buttons,
                                     n64_local_input_t* state)
{
    int key;

    if (playernum == 0 && menuactive)
    {
        state->weapon_key = 0;
        n64_local_weapon_cycle_down[playernum] = false;
        n64_local_weapon_prev_down[playernum] = false;
        n64_local_weapon_next_down[playernum] = false;
        return;
    }

    key = 0;

    // Original: A = prev, B = next.  Alt: R = prev, A = next (B is freed for use).
    boolean prev_btn = (controlScheme == 1) ? buttons.r : buttons.a;
    boolean next_btn = (controlScheme == 1) ? buttons.a : buttons.b;

    if (prev_btn && !n64_local_weapon_prev_down[playernum])
    {
        key = I_GetDirectionalWeaponKeyForPlayer(playernum, false);
    }
    else if (next_btn && !n64_local_weapon_next_down[playernum])
    {
        key = I_GetDirectionalWeaponKeyForPlayer(playernum, true);
    }

    state->weapon_key = key;

    n64_local_weapon_cycle_down[playernum] = false;
    n64_local_weapon_prev_down[playernum] = prev_btn;
    n64_local_weapon_next_down[playernum] = next_btn;
}

static void I_UpdateLocalPlayerInput(int playernum, joypad_port_t port)
{
    joypad_inputs_t inputs;
    joypad_buttons_t buttons;
    n64_local_input_t* state;

    state = &n64_local_inputs[playernum];
    memset(state, 0, sizeof(*state));

    if (!joypad_is_connected(port))
    {
        n64_local_weapon_cycle_down[playernum] = false;
        n64_local_weapon_prev_down[playernum] = false;
        n64_local_weapon_next_down[playernum] = false;
        return;
    }

    inputs = joypad_get_inputs(port);
    buttons = inputs.btn;

    state->joy_x = I_NormalizeStickAxis(inputs.stick_x);
    state->joy_y = -I_NormalizeStickAxis(inputs.stick_y);

    if (buttons.d_left)
        state->joy_x = -STICK_ANALOG_MAX;
    else if (buttons.d_right)
        state->joy_x = STICK_ANALOG_MAX;

    if (buttons.d_up)
        state->joy_y = -STICK_ANALOG_MAX;
    else if (buttons.d_down)
        state->joy_y = STICK_ANALOG_MAX;

    // Alt scheme: C-Up/C-Down drive forward/back. Override the forward axis
    // (same as the d-pad) so combining with the stick can't exceed full speed.
    if (controlScheme == 1)
    {
        if (buttons.c_up)
            state->joy_y = -STICK_ANALOG_MAX;
        else if (buttons.c_down)
            state->joy_y = STICK_ANALOG_MAX;
    }

    if (playernum == 0 && menuactive)
    {
        state->joy_x = 0;
        state->joy_y = 0;
    }

    state->strafe_left = buttons.c_left;
    state->strafe_right = buttons.c_right;
    // Alt has no run/walk button; speed follows the alwaysRun default (XOR in g_game).
    state->speed = (controlScheme == 1) ? false : buttons.r;

    if (playernum == 0 && menuactive)
    {
        state->fire = false;
        state->use = false;
    }
    else if (controlScheme == 1)
    {
        state->fire = buttons.z;	// L is automap in alt
        state->use = buttons.b;
    }
    else
    {
        state->fire = (buttons.z || buttons.l);
        state->use = buttons.c_down;
    }

    I_UpdateLocalWeaponInput(playernum, buttons, state);
}

void I_N64GetLocalInputState(int player_index, n64_local_input_t* out_state)
{
    if (!out_state)
        return;

    if (player_index < 0 || player_index >= MAXPLAYERS)
    {
        memset(out_state, 0, sizeof(*out_state));
        return;
    }

    *out_state = n64_local_inputs[player_index];
}

int I_N64GetActiveGameplayPort(void)
{
    return (int)n64_last_active_port;
}

void I_N64SplitScreenBeginFrame(int player_count)
{
    if (player_count < 1)
        player_count = 1;
    else if (player_count > MAXPLAYERS)
        player_count = MAXPLAYERS;

    n64_split_player_count = player_count;
    n64_split_active = (screens[0] != NULL) && (player_count > 1);

    // The CI8 buffer is ping-ponged at present time, so the draw buffer holds
    // the frame from two presents ago -- the clear-on-layout-change shortcut
    // would leave that stale content behind. Clear every frame for all split
    // layouts so no quadrant keeps another buffer's pixels.
    if (n64_split_active)
    {
        memset(screens[0], 0, SCREENWIDTH * SCREENHEIGHT);
        n64_split_prev_count = player_count;
    }
    else
    {
        n64_split_prev_count = 0;
    }
}

void I_N64SplitScreenEndFrame(void)
{
    int x;
    int y;
    int vertical_divider_height;

    if (!n64_split_active || !screens[0])
        return;

    if (n64_split_player_count <= 2)
    {
        if (splitOrientation == 1)
        {
            // vertical split: divider down the middle
            for (y = 0; y < SCREENHEIGHT; y++)
                for (x = SCREENWIDTH / 2 - 1; x <= SCREENWIDTH / 2; x++)
                    screens[0][y * SCREENWIDTH + x] = 0;
        }
        else
        {
            // horizontal split: divider across the middle
            memset(screens[0] + (SCREENHEIGHT / 2 - 1) * SCREENWIDTH, 0, SCREENWIDTH);
        }
    }
    else
    {
        memset(screens[0] + (SCREENHEIGHT / 2 - 1) * SCREENWIDTH, 0, SCREENWIDTH);
        vertical_divider_height = SCREENHEIGHT;
        for (y = 0; y < vertical_divider_height; y++)
        {
            for (x = SCREENWIDTH / 2 - 1; x <= SCREENWIDTH / 2; x++)
                screens[0][y * SCREENWIDTH + x] = 0;
        }
    }
}

void I_ShutdownGraphics(void)
{
    int i;

    if (!video_initialized)
        return;

    I_N64LogMemoryStats("i_video:before_shutdown");

    for (i = 0; i < 3; i++)
    {
        if (n64_aux_screen_owned[i] && n64_aux_screens[i])
        {
            free(n64_aux_screens[i]);
        }

        n64_aux_screens[i] = NULL;
        n64_aux_screen_owned[i] = false;
        screens[i + 1] = NULL;
    }

    n64_split_active = false;
    n64_split_player_count = 1;
    n64_last_active_port = JOYPAD_PORT_1;
    memset(n64_local_inputs, 0, sizeof(n64_local_inputs));

    // Ensure the RDP has finished reading either CI8 buffer before freeing.
    rspq_wait();
    for (i = 0; i < N64_CI8_BUFFERS; i++)
    {
        surface_free(&doom_screen8[i]);
        doom_screen8_rdp_busy[i] = false;
        doom_screen8_disp[i] = NULL;
    }
    n64_draw_idx = 0;
    n64_palette_dirty = false;
    n64_present_copy_forward = false;
    screens[0] = NULL;
    rdpq_close();

    video_initialized = false;
    I_N64LogMemoryStats("i_video:after_shutdown");
}

void I_StartFrame(void)
{
}

void I_StartTic(void)
{
    joypad_inputs_t inputs;
    joypad_buttons_t buttons;
    joypad_buttons_t pressed;
    joypad_port_t active_port;
    int have_connected;
    joypad_port_t port;
    int playernum;
    int stick_x;
    int stick_y;
    int joy_x;
    int joy_y;

    joypad_poll();

    active_port = JOYPAD_PORT_1;
    have_connected = 0;
    JOYPAD_PORT_FOREACH(port)
    {
        if (!joypad_is_connected(port))
            continue;

        if (!have_connected)
        {
            active_port = port;
            have_connected = 1;
        }

        if (I_PortHasActivity(port))
        {
            active_port = port;
            break;
        }
    }

    if (have_connected)
        n64_last_active_port = active_port;
    else
        n64_last_active_port = JOYPAD_PORT_1;

    inputs = joypad_get_inputs(JOYPAD_PORT_1);
    buttons = inputs.btn;
    pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);

    stick_x = I_NormalizeStickAxis(inputs.stick_x);
    stick_y = I_NormalizeStickAxis(inputs.stick_y);

    if (buttons.d_left)
        stick_x = -STICK_ANALOG_MAX;
    else if (buttons.d_right)
        stick_x = STICK_ANALOG_MAX;

    if (buttons.d_up)
        stick_y = STICK_ANALOG_MAX;
    else if (buttons.d_down)
        stick_y = -STICK_ANALOG_MAX;

    // Alt scheme: C-Up/C-Down drive forward/back (single-player path).
    // Override the forward axis so stick + C-Up can't exceed full speed.
    if (controlScheme == 1 && !menuactive)
    {
        if (buttons.c_up)
            stick_y = STICK_ANALOG_MAX;
        else if (buttons.c_down)
            stick_y = -STICK_ANALOG_MAX;
    }

    if (menuactive)
    {
        joy_x = I_MenuAxisStep(stick_x);
        joy_y = I_MenuAxisStep(-stick_y);
    }
    else
    {
        joy_x = stick_x;
        joy_y = -stick_y;
    }

    I_PostJoystickEvent(0, joy_x, joy_y);

    I_UpdateKeyState(false, KEYIDX_PAUSE, KEY_PAUSE);
    I_UpdateWeaponCycle(false);
    // Weapon cycle: original A=prev / B=next; alt R=prev / A=next (B is Use in alt).
    boolean wprev = (controlScheme == 1) ? buttons.r : buttons.a;
    boolean wnext = (controlScheme == 1) ? buttons.a : buttons.b;
    I_UpdateWeaponSelect((wprev && !menuactive), false, &weapon_prev_down, &weapon_prev_key);
    I_UpdateWeaponSelect((wnext && !menuactive), true, &weapon_next_down, &weapon_next_key);

    I_UpdateKeyState(buttons.start, KEYIDX_MENU, KEY_ESCAPE);
    // Automap: C-Up on original, L on alt (where C-Up is forward movement).
    I_UpdateKeyState((controlScheme == 1) ? buttons.l : buttons.c_up,
                     KEYIDX_MAP_TOGGLE, KEY_TAB);
    // Menu buttons should fire on fresh presses to avoid carry-over from held gameplay inputs.
    I_UpdateKeyState((pressed.a && menuactive), KEYIDX_A_MENU_ENTER, KEY_ENTER);
    // Use: original C-Down, alt B.
    I_UpdateKeyState((((controlScheme == 1) ? buttons.b : buttons.c_down) && !menuactive),
                     KEYIDX_A_USE, ' ');
    I_UpdateKeyState((pressed.b && menuactive), KEYIDX_B_MENU_BACK, KEY_BACKSPACE);
    I_UpdateKeyState((menuactive && (pressed.c_down || pressed.r || pressed.y)), KEYIDX_MENU_CONFIRM_Y, 'y');
    I_UpdateKeyState((menuactive && (pressed.z || pressed.x)), KEYIDX_MENU_CONFIRM_N, 'n');
    // Fire: original Z or L; alt Z only (L is automap in alt).
    I_UpdateKeyState((((controlScheme == 1) ? buttons.z : (buttons.z || buttons.l)) && !menuactive),
                     KEYIDX_FIRE, KEY_RCTRL);

    I_UpdateKeyState(buttons.c_left, KEYIDX_STRAFE_LEFT, ',');
    I_UpdateKeyState(buttons.c_right, KEYIDX_STRAFE_RIGHT, '.');
    // Speed (run): original R; alt has no run button (speed follows the alwaysRun default).
    I_UpdateKeyState(((controlScheme != 1) && buttons.r), KEYIDX_SPEED, KEY_RSHIFT);

    for (playernum = 0; playernum < MAXPLAYERS; playernum++)
    {
        port = (joypad_port_t)(JOYPAD_PORT_1 + playernum);
        I_UpdateLocalPlayerInput(playernum, port);
    }
}

void I_UpdateNoBlit(void)
{
}

// Point screens[0] (and everything derived from it) at one of the two CI8
// buffers. Called at present time to flip the CPU's draw target away from the
// buffer the RDP is reading.
static void I_N64PointScreen(int idx)
{
    n64_draw_idx = idx;
    screens[0] = (byte*)doom_screen8[idx].buffer;

    // Rebase everything that caches a screens[0]-derived pointer across frames.
    R_N64RebaseScreen();    // ylookup[] (column renderer destination)
    AM_N64RebaseScreen();   // automap fb
    F_N64WipeRebaseScreen();
}

// CI8 draw-buffer index; st_lib keeps widget diff state per buffer.
int I_N64DrawBufferIndex(void)
{
    return n64_draw_idx;
}

// Copy `rows` rows starting at screen y=`y0` from the CURRENT CI8 draw buffer
// to the OTHER CI8 buffer, forcing them byte-identical there.
//
// Used to resync the status bar after a level reload. The status bar is
// rebuilt per-buffer only when a widget's value CHANGES (st_lib tracks
// oldinum[idx] per buffer); a full ST_refreshBackground redraw is rebuilt
// from a transient scratch (screens[BG]) whose holes hold frame-dependent
// content, so the two buffers' bars can freeze in slightly different states
// after a reborn (post-respawn HUD shimmer, Docs/PAST_BUGS.md). One buffer is
// always correctly drawn; mirroring it onto the other guarantees they match,
// and static widgets never redraw so they stay matched.
void I_N64SyncRegionToOtherBuffer(int y0, int rows)
{
    int other = n64_draw_idx ^ 1;
    int y1 = y0 + rows;
    byte* src;
    byte* dst;
    if (y0 < 0) y0 = 0;
    if (y1 > SCREENHEIGHT) y1 = SCREENHEIGHT;
    if (y1 <= y0) return;
    if (other < 0 || other >= N64_CI8_BUFFERS) return;
    if (!doom_screen8[n64_draw_idx].buffer || !doom_screen8[other].buffer) return;
    src = (byte*)doom_screen8[n64_draw_idx].buffer + y0 * SCREENWIDTH;
    dst = (byte*)doom_screen8[other].buffer       + y0 * SCREENWIDTH;
    memcpy(dst, src, (size_t)(y1 - y0) * SCREENWIDTH);
}

// Fired (under RDP interrupt) when the RDP has finished reading a CI8 buffer.
// Marks it free and shows the framebuffer it was blitted into -- the same
// display_show that rdpq_detach_show would have scheduled.
static void I_N64BufferDone(void* arg)
{
    int idx = (int)(intptr_t)arg;
#if defined(N64_BENCH) && defined(RDPWAIT_PROBE)
    // This runs in the DP SYNC_FULL interrupt. Time its wall-clock service and
    // charge it to whichever BPH_* render bracket is open right now -- proving
    // WHERE the async RDP-completion stall lands (the "phantom" bracket cost that
    // survives a body no-op). Probe-only; compiled out of timing/ship builds.
    uint64_t isr_t0 = get_ticks();
#endif

    if (doom_screen8_disp[idx])
    {
        display_show(doom_screen8_disp[idx]);
        n64_last_shown_disp = doom_screen8_disp[idx];   // old frame for the RDP melt-wipe
    }
    doom_screen8_rdp_busy[idx] = false;

#if defined(N64_BENCH) && defined(RDPWAIT_PROBE)
    N64Bench_NoteAsyncStall(get_ticks() - isr_t0);
#endif
}

// Read a lump's bytes for the transparency-key scan WITHOUT disturbing an
// already-resident copy's purge tag. W_CacheLumpNum(lump, PU_CACHE) calls
// Z_ChangeTag on a cached lump (w_wad.c), so scanning a font that ST_Init/
// HU_Init/M_Init already pinned PU_STATIC -- the status-bar STTNUM* and the
// menu STCFN*/M_* patches -- would DOWNGRADE it to purgeable. The zone then
// reuses the block and the font draws from garbage: STlib_drawNum reads a bogus
// patch width and V_CopyRect I_Errors when a new game starts, and the menu font
// goes garbage so the Options page renders blank. The scan only READS pixels,
// so it must never alter a lump's lifetime -- return a resident lump's pointer
// as-is, and only PU_CACHE-load lumps not yet present (transient by design).
extern void** lumpcache;            // w_wad.c
static const void* I_N64ScanReadLump(int lumpnum)
{
    if (lumpnum < 0 || lumpnum >= numlumps)
        return NULL;
    if (lumpcache[lumpnum])
        return lumpcache[lumpnum];          // resident -- do NOT retag it
    return W_CacheLumpNum(lumpnum, PU_CACHE);   // absent -- transient scan cache
}

// Mark every palette index a single patch lump touches. The lump is decoded as
// a patch (column posts); a malformed lump (bogus width/height/offset) is
// skipped rather than trusted, so a non-patch lump that happens to match a UI
// name prefix can never corrupt the scan.
static void I_N64MarkPatchIndices(int lumpnum, boolean used[256])
{
    patch_t* patch;
    int lumplen;
    int w;
    int col;

    lumplen = W_LumpLength(lumpnum);
    if (lumplen < 8)
        return;

    patch = (patch_t*)I_N64ScanReadLump(lumpnum);
    if (!patch)
        return;

    w = SHORT(patch->width);
    if (w <= 0 || w > 4096 || SHORT(patch->height) <= 0 || SHORT(patch->height) > 4096)
        return;
    // columnofs[] must fit inside the lump (header is 8 bytes + w longs).
    if ((size_t)lumplen < 8 + (size_t)w * 4)
        return;

    for (col = 0; col < w; col++)
    {
        int ofs = LONG(patch->columnofs[col]);
        column_t* column;

        if (ofs < 0 || ofs >= lumplen)
            return;                         // corrupt: abandon this lump

        column = (column_t*)((byte*)patch + ofs);

        while (column->topdelta != 0xff)
        {
            byte* source = (byte*)column + 3;
            int count = column->length;
            int i;

            // Post body must stay inside the lump.
            if ((byte*)source + count > (byte*)patch + lumplen)
                return;

            for (i = 0; i < count; i++)
                used[source[i]] = true;

            column = (column_t*)((byte*)column + column->length + 4);
            if ((byte*)column >= (byte*)patch + lumplen)
                return;
        }
    }
}

// Mark every palette index a single raw flat lump touches. A flat is a bare
// 64x64 byte array of palette indices (no patch header) -- so every byte in a
// 4096-byte lump is a used index. Lumps that are not exactly flat-sized are
// skipped (animated-flat markers, oversized custom flats are rare in the
// shareware set and erring toward "skip" only ever frees MORE indices).
static void I_N64MarkFlatIndices(int lumpnum, boolean used[256])
{
    const byte* data;
    int len;
    int i;

    len = W_LumpLength(lumpnum);
    if (len != 64 * 64)
        return;

    data = (const byte*)I_N64ScanReadLump(lumpnum);
    if (!data)
        return;

    for (i = 0; i < len; i++)
        used[data[i]] = true;
}

// Mark the indices used by all WORLD art -- wall-texture patches (the lumps
// PNAMES references), sprites, and flats. Used so the reserved transparency key
// can be chosen PROVABLY absent from opaque world art, which closes the design's
// "Key index leaks through opaque world art" hazard (DESIGN sec5 / risk table):
// with the key absent from every world texel, the present blit's alpha-compare
// can never punch a hole in software-rendered world pixels even if it touched
// them. Belt-and-suspenders with the present seam's bbox-scissored keying.
static void I_N64MarkWorldArtIndices(boolean used[256])
{
    int flat_start, flat_end;
    int lump;

    // Wall-texture patches: every lump named in PNAMES (patch format).
    {
        const byte* names = (const byte*)I_N64ScanReadLump(W_CheckNumForName("PNAMES"));
        if (names)
        {
            int nummappatches = LONG(*((const int*)names));
            const char* name_p = (const char*)names + 4;
            int i;
            char nm[9];
            nm[8] = 0;
            for (i = 0; i < nummappatches; i++)
            {
                int pl;
                strncpy(nm, name_p + i * 8, 8);
                pl = W_CheckNumForName(nm);
                if (pl >= 0)
                    I_N64MarkPatchIndices(pl, used);
            }
        }
    }

    // Sprites: patch format, [firstspritelump, lastspritelump].
    for (lump = firstspritelump; lump <= lastspritelump; lump++)
        I_N64MarkPatchIndices(lump, used);

    // Flats: raw 64x64, the lumps between F_START and F_END.
    flat_start = W_CheckNumForName("F_START");
    flat_end   = W_CheckNumForName("F_END");
    if (flat_start >= 0 && flat_end > flat_start)
    {
        for (lump = flat_start + 1; lump < flat_end; lump++)
            I_N64MarkFlatIndices(lump, used);
    }
}

// Expand a raw world-art index set into the set of bytes those indices can
// actually put ON SCREEN. Every world pixel write goes through a colormap
// (`*dest = colormap[source[...]]`, r_draw.c) -- walls, flats, sprites AND the
// CPU-drawn psprites -- so the framebuffer byte is COLORMAP[level][raw], for
// any of the lump's maps (32 light levels + the invuln inverse map + the spare
// 34th). The raw lump byte itself never reaches the framebuffer. A key chosen
// "absent from raw world art" can therefore still collide on screen: e.g. raw
// index 16 (bright red, blood/fireballs/imp art) maps to 255 at light levels
// 11..13, so a key of 255 punched holes in software-drawn sprites inside the
// keyed box. The key must be absent from the colormap OUTPUT closure.
static void I_N64MarkColormapOutputs(const boolean raw_used[256],
                                     boolean out_used[256])
{
    const byte* cmap;
    int lumpnum, nmaps, l, i;

    lumpnum = W_CheckNumForName("COLORMAP");
    cmap = (const byte*)I_N64ScanReadLump(lumpnum);
    if (!cmap)
    {
        // No COLORMAP (cannot happen for a valid IWAD): conservatively treat
        // every raw index as its own output so the caller still gets a set.
        for (i = 0; i < 256; i++)
            if (raw_used[i])
                out_used[i] = true;
        return;
    }

    nmaps = W_LumpLength(lumpnum) / 256;
    for (i = 0; i < 256; i++)
    {
        if (!raw_used[i])
            continue;
        for (l = 0; l < nmaps; l++)
            out_used[cmap[l * 256 + i]] = true;
    }
}

// Pick the transparency-key palette index (RDP renderer, Stage 1+). Scans every
// UI/status-bar/font/menu patch lump drawn OUTSIDE the 3D view (the ST*, M_*,
// and WI* graphic families) AND all world art (wall patches, sprites, flats),
// and reserves the highest index absent from BOTH (a) the raw UI bytes (UI is
// drawn un-colormapped, so its lump bytes ARE its screen bytes) and (b) the
// COLORMAP-OUTPUT closure of the world-art bytes (world pixels reach the screen
// only through a colormap -- see I_N64MarkColormapOutputs; scanning raw world
// bytes alone is provably wrong, the gate-round key 255 collided with the
// colormapped red ramp and punched sprite pixels). With the key absent from
// every byte the screen can hold inside the view, the present blit's
// alpha-compare can never punch a hole in software-rendered pixels.
// Belt-and-suspenders with the present seam's bbox-scissored keying. If no
// index survives the closure (palette-saturated WAD), fall back to raw-world +
// UI, then UI-only, in that order -- functional but with documented residual
// risk confined to the keyed box. Reserving a HIGH index matches the design's
// expectation that UI art rarely touches the top of PLAYPAL. Asserts only if
// even the UI-only set leaves no index free. Called once at startup, after the
// WAD is loaded.
void I_N64ScanTransparencyKey(void)
{
    static const char* const ui_prefixes[] = { "ST", "M_", "WI" };
    boolean ui_used[256];
    boolean world_raw[256];
    boolean used[256];
    int lump;
    int p;
    int idx;

    if (n64_rdp_key_index >= 0)
        return;                             // already chosen

    memset(ui_used, 0, sizeof(ui_used));

    for (lump = 0; lump < numlumps; lump++)
    {
        const char* name = lumpinfo[lump].name;

        for (p = 0; p < (int)(sizeof(ui_prefixes) / sizeof(ui_prefixes[0])); p++)
        {
            size_t plen = strlen(ui_prefixes[p]);
            if (strncmp(name, ui_prefixes[p], plen) == 0)
            {
                I_N64MarkPatchIndices(lump, ui_used);
                break;
            }
        }
    }

    memset(world_raw, 0, sizeof(world_raw));
    I_N64MarkWorldArtIndices(world_raw);

    // used = raw UI bytes + colormap-output closure of raw world bytes: the
    // complete set of bytes a level-play screen can contain.
    memcpy(used, ui_used, sizeof(used));
    I_N64MarkColormapOutputs(world_raw, used);

    for (idx = 255; idx >= 0; idx--)
    {
        if (!used[idx])
        {
            n64_rdp_key_index = idx;
            N64_DEBUGF("I_N64ScanTransparencyKey: reserved key index %d "
                       "(UI + world colormap-output free)\n", idx);
            I_N64MarkPaletteDirty();
            return;
        }
    }

    // Fallback 1: raw world + UI (the pre-closure criterion). Reachable only
    // on a WAD whose art saturates the colormap output space.
    memcpy(used, ui_used, sizeof(used));
    for (idx = 0; idx < 256; idx++)
        if (world_raw[idx])
            used[idx] = true;
    for (idx = 255; idx >= 0; idx--)
    {
        if (!used[idx])
        {
            n64_rdp_key_index = idx;
            N64_DEBUGF("I_N64ScanTransparencyKey: reserved key index %d "
                       "(raw-world+UI free only; colormap outputs saturate -- "
                       "bbox keying confines residual exposure)\n", idx);
            I_N64MarkPaletteDirty();
            return;
        }
    }

    for (idx = 255; idx >= 0; idx--)
    {
        if (!ui_used[idx])
        {
            n64_rdp_key_index = idx;
            N64_DEBUGF("I_N64ScanTransparencyKey: reserved key index %d "
                       "(UI-free; world art saturates the palette -- present "
                       "seam bbox-keying confines residual exposure)\n", idx);
            // If the flag is already on at boot (persisted in EEPROM), the
            // master TLUT may have been packed before the key was known, so
            // apply the key's alpha=0 now and mark dirty for the next present.
            I_N64MarkPaletteDirty();
            return;
        }
    }

    // Fallback: every UI patch uses every palette index (effectively
    // impossible for DOOM art). The assert documents the invariant.
    I_Error("I_N64ScanTransparencyKey: no free palette index for transparency key");
}

// (Erase-to-key history: Stage 1 used a full-view pre-clear; Stage 2 retired
// it for the event-driven per-column R_FillColumnKey because key pixels
// outside the DYNAMIC keyed box blitted opaque (stale-key sparkle); Stage 3
// resurrects the batched full-view clear -- I_N64KeyClearView below -- now
// paired with a FULL-VIEW keyed box so no view-window key pixel is ever
// opaque-blitted. See R_RenderPlayerView and the clear's comment block.)

// Scrub transparency-key pixels out of a wipe-captured CI8 screen. The wipe
// captures recycle presented/draw-buffer content that contains the routed
// seg's key-suppressed pixels (the RDP fill behind them lives only in the
// 16bpp display fb, which the CI8 melt never reads). Melt presents then
// repaint those key pixels OUTSIDE any keyed box -- with no world records the
// present is a single opaque blit -- so they rendered as bright key-colour
// blotches for the duration of the melt (the 606-px stale-key sparkle:
// trace WIPE_TRACE start keypx=606 == DL_KEYSCAN p=3310 OUT=606). Replace
// each key pixel with the pixel above it (top row falls back to palette 0):
// the suppressed wall region melts as a smear of the art above it instead of
// the key colour. Safe by construction: the key index was reserved from the
// UI raw bytes + the colormap-output closure of world art
// (I_N64ScanTransparencyKey), so during level play no legitimate screen byte
// can equal it -- only suppression holes are ever touched. Flag-off: no-op.
void I_N64WipeScrubKey(byte* scr)
{
    int x, y;
    int key;

    if (n64_use_rdp_renderer == 0 || n64_rdp_key_index < 0 || !scr)
        return;

    key = n64_rdp_key_index;

    for (x = 0; x < SCREENWIDTH; x++)
        if (scr[x] == key)
            scr[x] = 0;

    for (y = 1; y < SCREENHEIGHT; y++)
    {
        byte* row  = scr + y * SCREENWIDTH;
        byte* prev = row - SCREENWIDTH;

        for (x = 0; x < SCREENWIDTH; x++)
            if (row[x] == key)
                row[x] = prev[x];
    }
}

// --- Stage-3 view key-clear + full-view keyed box ---------------------------
// With ALL wall tiers routed (Stage 3), the per-column event-driven erase
// (R_FillColumnKey at every suppressed span) wrote ~1 uncached byte per wall
// pixel per frame -- the same store count as the colfunc it replaced, ~3.5-4 ms
// of the 5.2 ms flag-ON seg_rast wall. It is replaced by ONE batched 64-bit
// key-fill of the whole view window at view-render entry (~54 KB / ~6.7k
// stores, KEY_CLEAR phase): every view pixel starts key, the CPU planes/
// sprites/psprite/HU overwrite theirs, and the routed wall spans stay key for
// the RDP fill to show through.
//
// The full-view clear's Stage-2 failure mode (the stale-key sparkle: key
// pixels OUTSIDE the dynamic keyed box blitted OPAQUE as the key colour) is
// closed STRUCTURALLY by pairing it with a FULL-VIEW keyed box: when the
// clear armed this frame, the present alpha-compares the ENTIRE view window,
// so a key pixel is never opaque-blitted -- a vanilla per-column coverage gap
// reveals the display fb (3-presents-old composite) instead, the same stale-
// content artifact class as vanilla's own unwritten-pixel behaviour. Opaque
// world art containing the key index cannot exist INSIDE the view window
// while the route is on (walls never CPU-draw; the key is reserved out of the
// colormap-output closure for everything else), so the design rule
// "alpha-compare never runs over opaque world art" still holds in substance.
static boolean n64_ci8_view_keyed;       // this draw buffer's view window was
                                         // key-cleared this frame (arms the
                                         // present's full-view keyed box)

void I_N64KeyClearView(void)
{
    byte*    scr = screens[0];
    int      x0, y0, w, h, y;
    byte     key;
    uint64_t pat;

    if (n64_use_rdp_renderer == 0 || n64_rdp_key_index < 0 || !scr)
        return;
    if (!DL_AnyRouteOn())
        return;     // neither walls nor planes route: nothing is suppressed,
                    // keep CI8 key-free (full-software composite path)

    x0 = viewwindowx;
    y0 = viewwindowy;
    w  = scaledviewwidth;
    h  = viewheight;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > SCREENWIDTH)  w = SCREENWIDTH - x0;
    if (y0 + h > SCREENHEIGHT) h = SCREENHEIGHT - y0;
    if (w <= 0 || h <= 0)
        return;

    key = (byte)n64_rdp_key_index;
    pat = (uint64_t)key * 0x0101010101010101ull;

    for (y = y0; y < y0 + h; y++)
    {
        byte* row = scr + y * SCREENWIDTH + x0;
        byte* end = row + w;

        while (((uintptr_t)row & 7) && row < end)
            *row++ = key;
        while (row + 8 <= end)
        {
            *(uint64_t*)row = pat;
            row += 8;
        }
        while (row < end)
            *row++ = key;
    }

    n64_ci8_view_keyed = true;
}

void I_FinishUpdate(void)
{
    surface_t* disp;
    uint64_t now_ms;
    int next_idx;
    boolean rdp_on;

    if (!video_initialized)
        return;

    // Menu overlays can trigger back-to-back presents while logic catches up.
    // Pace these to display rate to avoid transient tearing/partial updates.
    // No swap happens on this path, so the next frame keeps drawing into the
    // same buffer.
    if (menuactive && gamestate == GS_LEVEL)
    {
        now_ms = get_ticks_ms();
        if (last_menu_present_ms && (now_ms - last_menu_present_ms) < 16)
            return;
    }

#if defined(N64_BENCH) && defined(RDPWAIT_PROBE)
    // Time the free-framebuffer acquire separately from the rest of PRESENT: this
    // is the vsync-coupled wait (display_get blocks when no display buffer is free)
    // -- a HARD serialization if it spins, ~0 if buffers are available. Probe-only.
    {
        uint64_t dg_t0 = get_ticks();
        disp = display_get();
        N64Bench_NoteDispGet(get_ticks() - dg_t0);
    }
#else
    disp = display_get();
#endif
    if (!disp)
        return;

    rdp_on = (n64_use_rdp_renderer != 0);

    // GPU port: attach a Z-buffer for the mesh wall pass (correct opaque depth
    // occlusion). Allocated once, lazily, sized to the ACTUAL display surface. z-mode
    // (DL_Flush) is enabled ONLY when dl_zbuf_attached -- never against a NULL z-image
    // (which froze the CPU on a cart-space write). Non-mesh builds attach NULL.
    dl_zbuf_attached = 0;
    if (n64_rdp_mesh && disp)
    {
        static surface_t dl_zbuf;
        static int       dl_zbuf_ready = 0;
        if (!dl_zbuf_ready)
        {
            // surface_alloc draws from libdragon's heap, which DOOM's Z_Init zone
            // exhausts at startup -> a 320x200x2 (128KB) RGBA16 z-buffer FAILS every
            // frame (the GPU-port z-buffer was silently inert; walls occluded via
            // painter's order only). Allocate from DOOM's zone instead (it owns the
            // RAM) and 64-byte align for the RDP z-image. RDP-only buffer (never
            // CPU-read), so a cached zone pointer is fine; PU_STATIC = whole run.
            int      zw = (int)disp->width, zh = (int)disp->height;
            uint32_t bytes = (uint32_t)zw * 2u * (uint32_t)zh;
            byte*    raw = Z_Malloc((int)bytes + 64, PU_STATIC, NULL);
            if (raw)
            {
                byte* al = (byte*)(((uint32_t)(uintptr_t)raw + 63u) & ~63u);
                dl_zbuf = surface_make_linear(al, FMT_RGBA16, zw, zh);
            }
            dl_zbuf_ready = (raw != NULL);
            debugf("GPU-PORT zbuf via Z_Malloc %dx%d -> %s\n", zw, zh,
                   dl_zbuf_ready ? "ok" : "FAILED");
        }
        if (dl_zbuf_ready)
        {
            rdpq_attach(disp, &dl_zbuf);
            rdpq_clear_z(0xFFFF);       // far; nearer walls (smaller Z) overwrite
            dl_zbuf_attached = 1;
        }
        else
        {
            rdpq_attach(disp, NULL);
        }
    }
    else
    {
        rdpq_attach(disp, NULL);
    }

    // Whether the RDP world pass actually drew geometry this frame (Stage 2:
    // the single routed midtexture seg, if one was eligible). Drives both the
    // world flush below and the present blit's alpha-compare key-out.
    {
    boolean world_drawn = false;

    // World-render seam (RDP renderer). When the flag is on, the world pass
    // draws the 3D view directly into the 16bpp display fb, scissored to the
    // view window, BEFORE the overlay blit and in the SAME rspq stream. It sits
    // AFTER the menu present-pacing early-return above so a paced menu frame
    // never drains a half-built world list. Stage 2 routes ONE single-sided
    // (midtexture) seg through DL_Flush; everything else is still software-
    // rendered into the CI8 buffer that the present blit reads.
    // DL_RSPEmitPending(): RSP-emit walls live in dl_rspemit_pending, NOT dl_wall_count
    // (DL_Count). Without it, an RSP-emit-ONLY frame (up-close facing a static mesh wall,
    // no door/movable wall and no routed plane in view) reads 0 here and SKIPS both the
    // colour-clear below and DL_Flush -> the 3-frames-ago 16bpp fb shows through the keyed
    // present -> whole-view motion ghost. 0 in non-RSP-emit builds (gate byte-identical).
    if (rdp_on && (DL_Count() + DL_SpanCount() + DL_PolyCount() + DL_RSPEmitPending()) > 0)
    {
        int vx0 = viewwindowx;
        int vy0 = viewwindowy;
        int vx1 = viewwindowx + scaledviewwidth;
        int vy1 = viewwindowy + viewheight;

        if (vx0 < 0) vx0 = 0;
        if (vy0 < 0) vy0 = 0;
        if (vx1 > SCREENWIDTH)  vx1 = SCREENWIDTH;
        if (vy1 > SCREENHEIGHT) vy1 = SCREENHEIGHT;

        if (vx1 > vx0 && vy1 > vy0)
        {
            // The world pass samples the master TLUT from the upper TMEM half;
            // upload it BEFORE the flush (the present blit below reuses the same
            // persistent TLUT, so this also satisfies that path and clears the
            // dirty flag once). A CI8 tile upload only touches the lower half,
            // so the TLUT survives the flush.
            if (n64_palette_dirty)
            {
                uint16_t* slot = doom_tlut_up[n64_draw_idx];

                memcpy(slot, doom_tlut_master, sizeof(doom_tlut_master));
                data_cache_hit_writeback(slot, sizeof(doom_tlut_master));
                rdpq_tex_upload_tlut(slot, 0, 256);
                n64_palette_dirty = false;
            }

            // GHOST FIX (moving walls): clear the RDP colour buffer in the view
            // region BEFORE drawing the mesh walls. The N64 rotates 3 hardware
            // framebuffers (display_init(...,3,...)), so `disp` holds the image
            // from 3 frames ago. A STATIC wall redraws in place every frame and
            // self-covers, but a MOVING wall (door/lift) draws at a NEW screen
            // position each frame and leaves its OLD position holding the
            // 3-frames-ago pixels -- a trailing ghost of past door positions in
            // the keyed-through region. The non-mesh full-RDP path never shows
            // this because its RDP planes repaint the whole view every frame; the
            // mesh path draws only walls, so the vacated pixels are never
            // overwritten. A per-frame fill of the view rect retires the stale
            // image so only THIS frame's walls survive. Mesh-only (the non-mesh
            // path is unperturbed); cheap (one RDP fill rect, fill mode).
            if (n64_rdp_mesh)
            {
                rdpq_set_scissor(vx0, vy0, vx1, vy1);
                rdpq_set_mode_fill(RGBA32(0, 0, 0, 255));
                rdpq_fill_rectangle(vx0, vy0, vx1, vy1);
            }

            // Standard 1-cycle textured: TEX0*PRIM (free light), CI8 via TLUT,
            // perspective-correct (free INV_W). Scissor to the view window so
            // the quads can't spill outside the 3D view (DESIGN section 4).
            rdpq_set_mode_standard();
            rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
            rdpq_mode_tlut(TLUT_RGBA16);
            rdpq_mode_persp(true);
            rdpq_set_scissor(vx0, vy0, vx1, vy1);

#ifdef N64_BENCH
            // PAUSE the enclosing PRESENT bracket (d_main.c wraps the whole
            // I_FinishUpdate) while DL_Flush runs, so DL_BUILD and PRESENT are
            // DISJOINT accumulators. Bracketing DL_BUILD inside an open
            // PRESENT (the previous code) double-counted the entire flush in
            // both phases -- the Stage-3 phase table showed present=11.9ms of
            // which ~10ms was DL_BUILD again. The wipe melt loop calls
            // I_FinishUpdate with NO open PRESENT bracket; the open-mask guard
            // in N64Bench_PhaseSwitch makes the pause a safe no-op there.
            N64Bench_PhaseSwitch(BPH_PRESENT, BPH_DL_BUILD);
#endif
            DL_Flush();
#ifdef N64_BENCH
            N64Bench_PhaseSwitch(BPH_DL_BUILD, BPH_PRESENT);
#endif

            // Restore full-screen scissor + persp off for the overlay COPY blit.
            rdpq_mode_persp(false);
            rdpq_set_scissor(0, 0, SCREENWIDTH, SCREENHEIGHT);
            world_drawn = true;
        }
    }

    // Present blit. The CI8 overlay covers the entire 320x200 display and is
    // blitted over the RDP-drawn world in COPY mode (~4x fill, valid on the
    // 16bpp display fb).
    //
    // CRITICAL (gate-round fix): alpha-compare (transparency=true) keys out the
    // alpha-0 reserved index. It MUST run ONLY over the routed seg's pixels --
    // the suppressed-colfunc region that holds the key index (from KEY_CLEAR)
    // and must be discarded so the RDP fill shows through. Running it over the
    // WHOLE screen (the previous Stage-2 code) subjected every software-rendered
    // world pixel to alpha-compare, so any opaque world texel that legitimately
    // equals the key index (high PLAYPAL reds in fire/blood/explosions are
    // plausible) would be wrongly discarded -> a flickering hole revealing the
    // uncleared 16bpp fb. The design forbids alpha-compare over opaque world art
    // (DESIGN sec5 / risk table "Key index leaks through opaque world art":
    // "alpha-compare is OFF for opaque walls/flats, ON only for masked draws").
    //
    // Fix: when world geometry drew, blit the routed seg's screen-space bounding
    // box with alpha-compare ON (keyed) and the rest of the screen with
    // alpha-compare OFF (opaque), so software world art outside the box is never
    // keyed. The opaque remainder is the up-to-4 rectangular bands around the
    // box. When NOTHING drew (flag off / no eligible seg / paced menu frame) the
    // whole screen is a single opaque blit, byte-identical to the software path.
    if (n64_palette_dirty)
    {
        uint16_t* slot = doom_tlut_up[n64_draw_idx];

        memcpy(slot, doom_tlut_master, sizeof(doom_tlut_master));
        data_cache_hit_writeback(slot, sizeof(doom_tlut_master));
        // The TLUT load must happen inside a mode where the upper TMEM half is
        // addressable; set COPY mode + TLUT first, then upload.
        rdpq_set_mode_copy(false);
        rdpq_mode_tlut(TLUT_RGBA16);
        rdpq_tex_upload_tlut(slot, 0, 256);
        n64_palette_dirty = false;
    }

    {
    int kx0 = 0, ky0 = 0, kx1 = -1, ky1 = -1;
    boolean keyed = false;

    // FULL-VIEW keyed box (Stage-3): whenever this draw buffer's view window
    // was key-cleared this frame (I_N64KeyClearView armed it at view-render
    // entry), the WHOLE view window is alpha-compare keyed -- key pixels can
    // exist anywhere in it (cleared wall spans, vanilla coverage gaps), and a
    // key pixel must never be opaque-blitted (that is the stale-key sparkle).
    // Non-key view pixels (CPU planes/sprites/psprite/HU/menu) blit opaque
    // through the alpha-compare unchanged. The record-bbox box (DL_KeyedSpan)
    // is retired from box duty: with all wall tiers routed it under-covers
    // the key population the full clear creates. world_drawn is NOT the gate
    // -- a 0-record view frame (all-sky scene) still has a key-cleared window
    // that must be keyed, not opaque-blitted.
    if (rdp_on && n64_ci8_view_keyed)
    {
        kx0 = viewwindowx;
        ky0 = viewwindowy;
        kx1 = viewwindowx + scaledviewwidth - 1;
        ky1 = viewwindowy + viewheight - 1;
        if (kx0 < 0) kx0 = 0;
        if (ky0 < 0) ky0 = 0;
        if (kx1 > SCREENWIDTH - 1)  kx1 = SCREENWIDTH - 1;
        if (ky1 > SCREENHEIGHT - 1) ky1 = SCREENHEIGHT - 1;
        keyed = (kx1 >= kx0 && ky1 >= ky0);
    }
    (void)world_drawn;

    if (!keyed)
    {
        // No keyed region: one opaque full-screen blit (software-path identical
        // when the flag is off). The CI8 source covers the whole display, so
        // attach-without-clear is sufficient -- every pixel is overwritten.
        rdpq_set_mode_copy(false);
        rdpq_mode_tlut(TLUT_RGBA16);
        rdpq_set_scissor(0, 0, SCREENWIDTH, SCREENHEIGHT);
        rdpq_tex_blit(&doom_screen8[n64_draw_idx], 0, 0, NULL);
    }
    else
    {
        // exclusive box edges for the opaque bands
        int bx1 = kx1 + 1;      // one past the box's right column
        int by1 = ky1 + 1;      // one past the box's bottom row
        rdpq_blitparms_t bp;

        // CARVING RULE (LOAD-BEARING): each region is blitted as an EXPLICIT
        // SOURCE SUB-RECT (rdpq_blitparms_t s0/t0/width/height) drawn at its
        // own screen position -- NEVER as a full-surface blit carved by the
        // scissor. A COPY-mode texture rectangle that crosses the scissor's
        // left/top edge does NOT get its texture coordinates compensated for
        // the clipped-off part: the rectangle's S restarts at the scissor
        // edge, so the blit painted SCREEN-LEFT content at the region's left
        // edge. That one displacement produced the whole duplication defect
        // family: the view's left half mirrored into the right half (box
        // shifted by kx0), the gun/HU text duplicated "on the opposite side"
        // (bands shifted by bx1), and solid key-colour polygons (a band's
        // shifted sampling window covering the suppressed key columns gets
        // painted with alpha-compare OFF -- a translated opaque copy of the
        // keyed wall region). Sub-rect blits derive the texrect S/T and
        // screen X/Y together, so no clipping is ever needed.
        //
        // (1) Keyed box: alpha-compare ON discards the key-index pixels so the
        //     RDP fill drawn earlier survives there; non-key overlay pixels in
        //     the box (e.g. HUD intruding into these columns -- scanned safe)
        //     blit normally.
        //
        //     LOAD-BEARING: rdpq_set_mode_copy(true) raises
        //     SOM_ALPHACOMPARE_THRESHOLD, which discards a texel only when
        //     texel_alpha < BLEND-COLOUR alpha -- it does NOT imply a nonzero
        //     threshold by itself, and neither libdragon init nor any other
        //     code in this port ever sets the blend colour. With the power-on
        //     blend alpha of 0 the compare passes EVERYTHING (0 < 0 is false),
        //     so the "keyed" box was blitted fully opaque and the suppressed
        //     key-index columns rendered as solid key-colour rectangles
        //     (salmon under key 255, magenta under key 251) instead of being
        //     punched out. Threshold alpha=1 discards exactly the alpha=0 key
        //     entry: an RGBA16 TLUT texel expands its 1-bit alpha to 0 or 255,
        //     so key (0 < 1) is discarded and opaque art (255 < 1 false) is
        //     kept, bit-exact.
        rdpq_set_blend_color(RGBA32(0, 0, 0, 1));
        rdpq_set_mode_copy(true);
        rdpq_mode_tlut(TLUT_RGBA16);
        memset(&bp, 0, sizeof(bp));
        bp.s0 = kx0;  bp.t0 = ky0;
        bp.width = bx1 - kx0;  bp.height = by1 - ky0;
        rdpq_tex_blit(&doom_screen8[n64_draw_idx], kx0, ky0, &bp);

        // (2) Opaque remainder: the up-to-4 bands around the box, alpha-compare
        //     OFF, so software-rendered world art outside the box is blitted
        //     unconditionally (never keyed). Together the bands + box tile the
        //     whole 320x200 surface exactly once with no overlap.
        rdpq_set_mode_copy(false);
        rdpq_mode_tlut(TLUT_RGBA16);

        // top band: full width, rows [0, ky0)
        if (ky0 > 0)
        {
            memset(&bp, 0, sizeof(bp));
            bp.s0 = 0;  bp.t0 = 0;
            bp.width = SCREENWIDTH;  bp.height = ky0;
            rdpq_tex_blit(&doom_screen8[n64_draw_idx], 0, 0, &bp);
        }
        // bottom band: full width, rows [by1, SCREENHEIGHT)
        if (by1 < SCREENHEIGHT)
        {
            memset(&bp, 0, sizeof(bp));
            bp.s0 = 0;  bp.t0 = by1;
            bp.width = SCREENWIDTH;  bp.height = SCREENHEIGHT - by1;
            rdpq_tex_blit(&doom_screen8[n64_draw_idx], 0, by1, &bp);
        }
        // left band: columns [0, kx0), box rows only
        if (kx0 > 0)
        {
            memset(&bp, 0, sizeof(bp));
            bp.s0 = 0;  bp.t0 = ky0;
            bp.width = kx0;  bp.height = by1 - ky0;
            rdpq_tex_blit(&doom_screen8[n64_draw_idx], 0, ky0, &bp);
        }
        // right band: columns [bx1, SCREENWIDTH), box rows only
        if (bx1 < SCREENWIDTH)
        {
            memset(&bp, 0, sizeof(bp));
            bp.s0 = bx1;  bp.t0 = ky0;
            bp.width = SCREENWIDTH - bx1;  bp.height = by1 - ky0;
            rdpq_tex_blit(&doom_screen8[n64_draw_idx], bx1, ky0, &bp);
        }
    }
    }
    }

    // Disarm the keyed-box flag: it described THIS draw buffer's content and
    // the present just consumed it. The next view render re-arms it (a paced
    // menu frame that skipped this present keeps it armed for the present
    // that eventually runs -- the early-return above sits before this point).
    n64_ci8_view_keyed = false;

#ifdef BENCH_VOID_SCAN
    // DEMO-WIDE VOID DETECTOR. The off-grid near-total-BLACK voids (doom-n64-capture-
    // pitfalls #7: ~316 frames, all off the 128-frame marker grid) cannot be sampled by
    // a marker capture. Here, after the present blit is queued, drain the RDP and count
    // PURE-BLACK pixels in the view region of the composited 16bpp fb -- a void leaves the
    // per-frame black colour-clear showing through the keyed present, so the view reads
    // black. Logs any frame >= 40% black (frame + %), across ALL ~4117 demo frames, so
    // both full and partial voids surface. Serialises on the RDP (rspq_wait) -> timing is
    // meaningless in this build; correctness-diagnostic only.
    {
        extern unsigned long N64Bench_FrameNo(void);
        const unsigned short* fb = UncachedUShortAddr(disp->buffer);
        int s16 = (int)(disp->stride >> 1);
        int x, y, tot = 0, blk = 0;
        rspq_wait();
        for (y = viewwindowy; y < viewwindowy + viewheight; y += 4)
            for (x = viewwindowx; x < viewwindowx + scaledviewwidth; x += 4) {
                unsigned short px = fb[y * s16 + x];
                tot++;
                if (((px >> 1) & 0x7FFF) == 0) blk++;   // RGB (15 bits) all zero = pure black
            }
        if (tot > 0 && (blk * 100 / tot) >= 40)
            debugf("BENCH_VOID frame=%lu black=%d%%\n",
                   N64Bench_FrameNo(), blk * 100 / tot);
    }
#endif

    // RDP MELT-WIPE (mesh fix). During a wipe the CI8 path melts screens[], which on
    // the RDP renderer hold only the HUD + key-clear (the world lives in this 16bpp fb),
    // so it dissolves garbage ("mangled rotated HUD"). Instead, melt the COMPOSITED 16bpp
    // frames: snapshot the old (last-shown) and new (this present's composite) frames once,
    // then each present dissolve old->new using the CI8 wipe's own per-column drip offsets
    // (F_N64WipeMeltY) -- so no extra M_Random is consumed (demo-deterministic) and the
    // 16bpp melt stays exactly in step with the CI8 melt that drives D_Display's loop.
    // Only the PRESENTED pixels change. Off the wipe this whole block is a no-op.
    if (!n64_wipe16_start)           // lazily reserve the two snapshots from DOOM's zone
    {                                // (once; PU_STATIC). NULL if the zone is full -> the
        n64_wipe16_start = (uint16_t*)Z_Malloc(SCREENWIDTH * SCREENHEIGHT * 2, PU_STATIC, 0);
        n64_wipe16_end   = (uint16_t*)Z_Malloc(SCREENWIDTH * SCREENHEIGHT * 2, PU_STATIC, 0);
    }
    if (rdp_on && n64_wipe16_start && n64_wipe16_end &&
        (n64_present_copy_forward || n64_wipe16_state))
    {
        int*       my  = F_N64WipeMeltY();           // CI8 melt offsets (NULL once ended)
        int        s16 = (int)(disp->stride >> 1);
        uint16_t*  d   = (uint16_t*)disp->buffer;    // cached write; one writeback below
        int        x, yy;

        rspq_wait();   // the composite (DL_Flush + keyed blit) into disp must be done
                       // before the CPU reads/overwrites it; the wipe is not perf-path.

        if (n64_present_copy_forward && n64_wipe16_state == 0)
        {
            // First wipe present: snapshot the new composite (disp) as the END frame and
            // the previously shown fb as the START (old) frame. Read uncached: the RDP
            // wrote these, the CPU's cached view may be stale.
            const uint16_t* s = UncachedUShortAddr(disp->buffer);
            for (yy = 0; yy < SCREENHEIGHT; yy++)
                for (x = 0; x < SCREENWIDTH; x++)
                    n64_wipe16_end[yy * SCREENWIDTH + x] = s[yy * s16 + x];
            if (n64_last_shown_disp && n64_last_shown_disp->buffer)
            {
                const uint16_t* o = UncachedUShortAddr(n64_last_shown_disp->buffer);
                int os16 = (int)(n64_last_shown_disp->stride >> 1);
                for (yy = 0; yy < SCREENHEIGHT; yy++)
                    for (x = 0; x < SCREENWIDTH; x++)
                        n64_wipe16_start[yy * SCREENWIDTH + x] = o[yy * os16 + x];
            }
            else
                memcpy(n64_wipe16_start, n64_wipe16_end, SCREENWIDTH * SCREENHEIGHT * 2);
            n64_wipe16_state = 1;
        }

        // Compose the melt into disp: per column the top `off` rows show the NEW frame
        // (revealed), the rest show the OLD frame scrolled DOWN by `off` (the drip). `off`
        // is the CI8 wipe's offset for the 2px short-column this pixel belongs to. Once the
        // wipe ends (copy_forward cleared, my == NULL) every column shows the full NEW frame.
        for (x = 0; x < SCREENWIDTH; x++)
        {
            int off = my ? my[x >> 1] : SCREENHEIGHT;
            if (off < 0) off = 0;
            if (off > SCREENHEIGHT) off = SCREENHEIGHT;
            for (yy = 0; yy < off; yy++)
                d[yy * s16 + x] = n64_wipe16_end[yy * SCREENWIDTH + x];
            for (yy = off; yy < SCREENHEIGHT; yy++)
                d[yy * s16 + x] = n64_wipe16_start[(yy - off) * SCREENWIDTH + x];
        }
        data_cache_hit_writeback(disp->buffer, (uint32_t)disp->stride * SCREENHEIGHT);

        if (!n64_present_copy_forward)
            n64_wipe16_state = 0;   // wipe finished this present; disarm for the next one
    }

    // Detach with a completion callback instead of a global rspq_wait(): the
    // CPU can render the next frame while the RDP reads this buffer. The
    // callback (RDP-done, not just RSP-done) marks the buffer free and shows
    // the framebuffer, exactly as rdpq_detach_show would.
    doom_screen8_disp[n64_draw_idx] = disp;
    doom_screen8_rdp_busy[n64_draw_idx] = true;
    rdpq_detach_cb(I_N64BufferDone, (void*)(intptr_t)n64_draw_idx);

    // Flip the CPU's draw target to the other buffer. Wait only if the RDP is
    // still reading it from an earlier frame; normally it finished long ago, so
    // this spins zero times and the CPU and RDP overlap. Bracketed as RDP_BUSY
    // (Q9): this buffer-flip spin is the point where an RDP that fell behind --
    // because the new world pass made it slower -- surfaces as counted wall
    // time, non-serializing (we never force an rspq_wait here). It stays ~0 as
    // long as the RDP drains inside the CPU residual.
#ifdef N64_BENCH
    // Pause PRESENT around the spin so RDP_BUSY is disjoint from it (same
    // attribution rule as the DL_BUILD pause above; safe no-op when the wipe
    // loop calls I_FinishUpdate outside a PRESENT bracket).
    N64Bench_PhaseSwitch(BPH_PRESENT, BPH_RDP_BUSY);
#endif
    next_idx = n64_draw_idx ^ 1;

#if defined(N64_BENCH) && defined(RDPWAIT_PROBE)
    {
        // Count spin iterations to prove the buffer-flip RDP-busy wait is the ~0
        // it's claimed to be (the only explicit CPU spin-on-RDP-completion). The
        // volatile read keeps the loop semantics byte-identical to the ship spin.
        uint32_t spins = 0;
        while (doom_screen8_rdp_busy[next_idx])
            spins++;
        N64Bench_NoteRdpBusySpins(spins);
    }
#else
    while (doom_screen8_rdp_busy[next_idx])
        ;
#endif
#ifdef N64_BENCH
    N64Bench_PhaseSwitch(BPH_RDP_BUSY, BPH_PRESENT);
#endif

    // Retire the RDP world state for this present. Sits AFTER the busy spin
    // (the spin proves the PREVIOUS present's RDP stream -- including its
    // texture-block DMAs -- fully drained, so DL_PresentEnd may demote that
    // present's pinned transpose blocks back to PU_CACHE) and clears the emit
    // arena so a later present that skipped the world render (automap, wipe,
    // menu-paced) can never re-flush this frame's quads or re-punch its keyed
    // box over non-world content. Gated on the kill-switch: with the flag off
    // the arena is empty and no block is ever pinned, so skipping the call
    // keeps the flag-off present seam unchanged.
    if (rdp_on)
        DL_PresentEnd();

    if (n64_present_copy_forward)
        memcpy(doom_screen8[next_idx].buffer, doom_screen8[n64_draw_idx].buffer,
               SCREENWIDTH * SCREENHEIGHT);

    I_N64PointScreen(next_idx);

    if (menuactive && gamestate == GS_LEVEL)
        last_menu_present_ms = get_ticks_ms();
}

void I_ReadScreen(byte* scr)
{
    // screens[0] is the current draw buffer; after a present that is the back
    // buffer, while the visible frame is the other one. The wipe start/end
    // capture wants the most recently presented (visible) image, so read that.
    int src_idx = video_initialized ? (n64_draw_idx ^ 1) : 0;
    memcpy(scr, doom_screen8[src_idx].buffer, SCREENWIDTH * SCREENHEIGHT);
}

void I_SetPalette(byte* palette)
{
    int i;
    int key = (n64_use_rdp_renderer != 0) ? n64_rdp_key_index : -1;

    for (i = 0; i < 256; i++)
    {
        uint8_t r = gammatable[usegamma][palette[0]];
        uint8_t g = gammatable[usegamma][palette[1]];
        uint8_t b = gammatable[usegamma][palette[2]];

        // Alpha bit is 1 for every entry (opaque) so the present blit writes it.
        // EXCEPTION: with the RDP renderer on, the reserved transparency-key
        // index gets alpha=0 in EVERY palette variant (damage/pickup/invuln
        // swaps preserve the key), so the present's COPY-mode alpha-compare
        // discards it and the world drawn in the 16bpp fb shows through. With
        // the flag off, every entry keeps alpha=1 -- byte-identical to before.
        uint16_t alpha = (i == key) ? 0 : 1;

        doom_tlut_master[i] = (uint16_t)(((r >> 3) << 11) |
                                         ((g >> 3) << 6) |
                                         ((b >> 3) << 1) |
                                         alpha);

        palette += 3;
    }

    n64_palette_dirty = true;
    doom_palette_gen++;     // real colour change: the CI4 wall re-tint tracks this

    // Plane damage-flash overlay support. Capture the UN-FLASHED base TLUT the
    // first time we see a palette (the engine's first I_SetPalette is always the
    // base PLAYPAL -- d_main.c:771 / st_stuff.c reset -- and ST_doPaletteStuff
    // re-passes palette 0 every time a flash fades out). Heuristic for "this is the
    // base": no flash is currently recovered yet (first call) OR the new master
    // resolves to ~0 flash strength against the existing base (a fade-out frame).
    // Capturing on every base-return keeps the base tracking gamma/menu changes.
    if (!doom_base_valid)
    {
        memcpy(doom_tlut_base, doom_tlut_master, sizeof(doom_tlut_base));
        doom_base_valid = true;
        doom_flash_a = 0;       // first palette is the base -> no overlay
    }
    else
    {
        I_N64SetFlashFromPalette();
        // A recovered zero-strength flash means this palette IS the base again
        // (flash cleared); refresh the captured base so it tracks any gamma/menu
        // recolour and the next flash is measured against the live base.
        if (doom_flash_a <= 0)
            memcpy(doom_tlut_base, doom_tlut_master, sizeof(doom_tlut_base));
    }
}

// The renderer toggle changes the key index's TLUT alpha bit (see I_SetPalette),
// so the menu must re-upload the TLUT after flipping the flag. I_SetPalette is
// not necessarily called again before the next present (a static scene with no
// flash), so patch the key entry's alpha directly in the already-packed master
// here, then mark dirty so the present re-uploads it. Cheap: one entry + one
// 256-entry upload.
void I_N64MarkPaletteDirty(void)
{
    if (n64_rdp_key_index >= 0)
    {
        uint16_t* e = &doom_tlut_master[n64_rdp_key_index];
        if (n64_use_rdp_renderer != 0)
            *e &= ~(uint16_t)1;     // key: alpha = 0 (keyed out by alpha-compare)
        else
            *e |= (uint16_t)1;      // restore opaque (byte-identical to before)
    }
    n64_palette_dirty = true;
}

// The CI4 wall pass (DL_Flush) overwrites the 256-entry TLUT region with up to
// 16 per-texture 16-colour sub-palettes. Force the master 256-TLUT to re-upload
// before the present blit so sprites/HUD/overlay (CI8 on the master) and the
// keyed COPY blit sample the correct colours again. The master already carries
// the right key alpha, so this does NOT touch any entry -- it just re-arms the
// upload. Negligible: one 512-byte writeback + one LOAD_TLUT per frame.
void I_N64ForceTLUTReupload(void)
{
    n64_palette_dirty = true;
}

// SYNCHRONOUS variant: actually re-upload the master 256-TLUT into TMEM NOW,
// in the current rspq stream, not just arm the dirty flag for the later present
// blit. The CI4 wall pass (DL_Flush bucket walk) overwrites the 256-entry TLUT
// region with up to 16 per-texture 16-colour CI4 sub-palettes (rdpq_tex_upload_-
// tlut at slot*16). When BOTH wall and plane routes are on, the CI8 plane pass
// drains INSIDE the same DL_Flush, AFTER the wall pass, but BEFORE I_FinishUpdate
// re-uploads the master TLUT for the present blit -- so the planes would sample
// the corrupted (CI4-sub-palette) TLUT and render garbage colours. Re-uploading
// here, between the wall and plane passes, restores the master 256-TLUT in TMEM
// so the CI8 flats sample the right colours. The world textured mode (TLUT_-
// RGBA16) the caller already set keeps the upper TMEM half addressable for the
// LOAD_TLUT. Mirrors the world-pass upload in I_FinishUpdate (no entry changes,
// the master already carries the key alpha). One 512 B writeback + one LOAD_TLUT.
void I_N64UploadMasterTLUT(void)
{
    uint16_t* slot = doom_tlut_up[n64_draw_idx];

    memcpy(slot, doom_tlut_master, sizeof(doom_tlut_master));
    data_cache_hit_writeback(slot, sizeof(doom_tlut_master));
    rdpq_tex_upload_tlut(slot, 0, 256);
    // The present-blit path still re-uploads under its own COPY-mode TLUT; leave
    // the dirty flag untouched so that path is unaffected (it idempotently re-
    // uploads the identical master TLUT).
}

// --- plane damage-flash overlay support ------------------------------------
// Synchronously upload the UN-FLASHED base TLUT (PLAYPAL palette 0) into TMEM so
// the RDP plane pass samples un-flashed CI8 floor texels. Caller (DL_FlushPlane-
// Polys) then draws ONE uniform translucent flash rect over the plane region and
// RE-ASSERTS the master via I_N64UploadMasterTLUT before returning, so the CI8
// sprites/HUD/present-blit still see the flashed palette. No-op (returns false)
// until a base palette has been captured -- the plane pass then keeps the master.
// Mirrors I_N64UploadMasterTLUT's mechanics (512 B writeback + one LOAD_TLUT); the
// caller's world textured mode (TLUT_RGBA16) keeps the upper TMEM half loadable.
boolean I_N64UploadBaseTLUT(void)
{
    uint16_t* slot;

    if (!doom_base_valid)
        return false;
    slot = doom_tlut_up[n64_draw_idx];
    memcpy(slot, doom_tlut_base, sizeof(doom_tlut_base));
    data_cache_hit_writeback(slot, sizeof(doom_tlut_base));
    rdpq_tex_upload_tlut(slot, 0, 256);
    return true;
}

// --- plane fixedcolormap (invuln / light-amp visor) support ----------------
// Synchronously upload a FIXEDCOLORMAP-COMPOSED 256-TLUT into TMEM so the RDP
// plane CI8 flats sample master[ colormap[level][texel] ] -- exactly software's
// whole-view colormap remap: the inverted grey-scale map under invuln (level 32),
// or the visor's near-fullbright level (1). The flash-overlay technique (uniform
// translucent rect) can only express a uniform TINT, not a per-index remap/inversion,
// so the planes need their TLUT composed through the colormap row instead. Composed
// from the CURRENT (flashed) master, so a simultaneous palette flash is already
// included and the caller SKIPS its uniform flash overlay. The caller re-asserts the
// plain master via I_N64UploadMasterTLUT before returning so the CI8 sprites/HUD/
// present-blit see the normal palette. Mirrors I_N64UploadBaseTLUT's mechanics (512 B
// writeback + one LOAD_TLUT). No-op (false) until the colormap lump is loaded.
extern lighttable_t* colormaps;     // r_data.c: 34-row colormap table (byte indices)
boolean I_N64UploadFixedColormapTLUT(int level)
{
    uint16_t* slot;
    const lighttable_t* cmap;
    int i;

    if (!colormaps)
        return false;
    cmap = colormaps + level * 256;
    slot = doom_tlut_fcm[n64_draw_idx];   // DEDICATED buffer (see decl) -- not doom_tlut_up
    for (i = 0; i < 256; i++)
        slot[i] = doom_tlut_master[cmap[i]];   // master[ colormap[L][i] ], software's chain
    data_cache_hit_writeback(slot, sizeof(doom_tlut_fcm[0]));
    rdpq_tex_upload_tlut(slot, 0, 256);
    return true;
}

// The active uniform flash tint as packed 0xRRGGBBAA, or 0 for no flash (palette
// 0). The plane pass blends this over the un-flashed planes (RDPQ_BLENDER_MULTIPLY,
// PRIM = this colour incl. alpha) to reproduce software's whole-screen palette
// wash uniformly. Recovered in I_N64SetFlashFromPalette below, so it always tracks
// the live master TLUT (every damage red 1-8, bonus gold, radsuit green, invuln).
uint32_t I_N64PlaneFlashARGB(void)
{
    if (!doom_base_valid || doom_flash_a <= 0)
        return 0;
    return ((uint32_t)doom_flash_r << 24) | ((uint32_t)doom_flash_g << 16) |
           ((uint32_t)doom_flash_b << 8) | (uint32_t)(doom_flash_a & 0xFF);
}

// Recover the uniform flash (tint RGB + strength alpha) by comparing the flashed
// master TLUT against the captured base. DOOM's flash is base[i]*(1-a)+target*a
// applied UNIFORMLY to every palette entry, so two reference entries pin (target,a)
// exactly: index 0 is pure black (base 0,0,0) -> master ~= target*a, and index 4 is
// pure white (base 255,255,255) -> master ~= target*a + 255*(1-a). The green
// channel of those two solves a; target then follows from the black entry. We work
// in the 5-bit TLUT space (the same colours the planes/HUD actually sample), so the
// overlay matches what the flash did to the floor, not an idealised PLAYPAL math.
// Called from I_SetPalette AFTER the master is packed. Cheap (a few entries).
static void I_N64SetFlashFromPalette(void)
{
    int wd, a256;
    // Unpack the relevant master/base entries from RGBA5551 (R:11..15 G:6..10
    // B:1..5) back to 0..255 (x<<3 | x>>2, the standard 5->8 expand).
#define EXP5(v) (((v) << 3) | ((v) >> 2))
    int mk_r = EXP5((doom_tlut_master[0] >> 11) & 0x1F);
    int mk_g = EXP5((doom_tlut_master[0] >>  6) & 0x1F);
    int mk_b = EXP5((doom_tlut_master[0] >>  1) & 0x1F);
    int mw_g = EXP5((doom_tlut_master[4] >>  6) & 0x1F);   // white entry, green
    int bw_g = EXP5((doom_tlut_base[4]   >>  6) & 0x1F);   // base white, green
#undef EXP5

    if (!doom_base_valid)
    {
        doom_flash_a = 0;
        return;
    }

    // Black index (base 0,0,0) flashes to master = target * a directly, so its RGB
    // IS target*a. White index (base ~255) flashes to target*a + base_white*(1-a).
    // Subtract -> base_white*(1-a) = white_master - black_master (per channel); use
    // green (the brightest, least quantized) to solve (1-a), hence a.
    wd = mw_g - mk_g;                 // ~= base_white_g * (1 - a)
    if (wd < 0) wd = 0;
    if (bw_g <= 0)
    {
        doom_flash_a = 0;
        return;
    }
    // a = 1 - wd/base_white_g  (Q8). Clamp to [0,255].
    a256 = 256 - (wd * 256) / bw_g;
    if (a256 < 0) a256 = 0;
    if (a256 > 255) a256 = 255;

    if (a256 <= 0)
    {
        doom_flash_a = 0;            // palette 0 / no flash -> no overlay
        return;
    }

    // target = black_master / a  (black_master == target*a). Q8 divide + clamp.
    {
        int tr = (mk_r * 256) / a256;
        int tg = (mk_g * 256) / a256;
        int tb = (mk_b * 256) / a256;
        if (tr > 255) tr = 255;
        if (tg > 255) tg = 255;
        if (tb > 255) tb = 255;
        doom_flash_r = (uint8_t)tr;
        doom_flash_g = (uint8_t)tg;
        doom_flash_b = (uint8_t)tb;
        doom_flash_a = a256;
    }
}

void I_InitGraphics(void)
{
    int i;
    size_t aux_size;

    if (video_initialized)
        return;

    I_N64LogMemoryStats("i_video:before_init");

    uint32_t existing_buffers = display_get_num_buffers();
    N64_DEBUGF("I_InitGraphics: display_get_num_buffers()=%u\n", (unsigned)existing_buffers);
    if (existing_buffers > 0)
    {
        N64_DEBUGF("I_InitGraphics: reusing existing display\n");
    }
    else
    {
        N64_DEBUGF("I_InitGraphics: calling display_init (fresh)\n");
        display_init(N64_DISPLAY_RESOLUTION, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE);
    }

    rdpq_init();

    // Two uncached CI8 source buffers, ping-ponged at present time so the CPU
    // renders the next frame while the RDP reads the previous one.
    for (i = 0; i < N64_CI8_BUFFERS; i++)
    {
        doom_screen8[i] = surface_alloc(FMT_CI8, SCREENWIDTH, SCREENHEIGHT);
        if (!doom_screen8[i].buffer)
            I_Error("I_InitGraphics: failed to allocate %dx%d CI8 surface",
                    SCREENWIDTH, SCREENHEIGHT);
        memset(doom_screen8[i].buffer, 0, SCREENWIDTH * SCREENHEIGHT);
        doom_screen8_rdp_busy[i] = false;
        doom_screen8_disp[i] = NULL;
    }

    n64_draw_idx = 0;
    n64_palette_dirty = true;
    n64_present_copy_forward = false;
    screens[0] = (byte*)doom_screen8[0].buffer;

    aux_size = (size_t)SCREENWIDTH * (size_t)SCREENHEIGHT;
    for (i = 0; i < 3; i++)
    {
        n64_aux_screens[i] = NULL;
        n64_aux_screen_owned[i] = false;
        screens[i + 1] = NULL;
    }

    n64_aux_screens[0] = (byte*)malloc(aux_size);
    if (!n64_aux_screens[0])
        I_Error("I_InitGraphics: failed to allocate scratch screen 1");

    n64_aux_screen_owned[0] = true;
    memset(n64_aux_screens[0], 0, aux_size);
    screens[1] = n64_aux_screens[0];

    for (i = 1; i < 3; i++)
    {
        n64_aux_screens[i] = (byte*)malloc(aux_size);
        if (!n64_aux_screens[i])
        {
            // Keep running in low-memory scenarios by aliasing to an existing scratch buffer.
            n64_aux_screens[i] = n64_aux_screens[i - 1];
            n64_aux_screen_owned[i] = false;
            N64_DEBUGF("I_InitGraphics: scratch screen %d fallback alias\n", i + 1);
        }
        else
        {
            n64_aux_screen_owned[i] = true;
            memset(n64_aux_screens[i], 0, aux_size);
        }

        screens[i + 1] = n64_aux_screens[i];
    }

    memset(doom_tlut_master, 0, sizeof(doom_tlut_master));
    memset(doom_tlut_up, 0, sizeof(doom_tlut_up));
    data_cache_hit_writeback(doom_tlut_up, sizeof(doom_tlut_up));
    memset(key_state, 0, sizeof(key_state));
    weapon_cycle_down = false;
    weapon_prev_down = false;
    weapon_next_down = false;
    weapon_cycle_key = 0;
    weapon_prev_key = 0;
    weapon_next_key = 0;
    next_weapon_cycle_key = '1';

    n64_split_active = false;
    n64_split_player_count = 1;
    n64_split_prev_count = 0;
    n64_last_active_port = JOYPAD_PORT_1;
    memset(n64_local_inputs, 0, sizeof(n64_local_inputs));
    memset(n64_local_weapon_cycle_down, 0, sizeof(n64_local_weapon_cycle_down));
    memset(n64_local_weapon_prev_down, 0, sizeof(n64_local_weapon_prev_down));
    memset(n64_local_weapon_next_down, 0, sizeof(n64_local_weapon_next_down));
    for (i = 0; i < MAXPLAYERS; i++)
        n64_local_next_weapon_cycle_key[i] = '1';

    video_initialized = true;
    I_N64LogMemoryStats("i_video:after_init");
}
