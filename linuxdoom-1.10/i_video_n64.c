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
static byte* n64_aux_screens[3];
static boolean n64_aux_screen_owned[3];
// I_SetPalette writes the CPU-side master; the present path copies it into an
// upload slot keyed by the CI8 buffer being presented. A buffer's previous
// TLUT load is ordered before its previous blit in the command stream, so once
// the buffer is free for CPU reuse its slot is free to rewrite -- a queued
// LOAD_TLUT can never see a rewrite, however fast the palette churns.
static uint16_t doom_tlut_master[256];
static uint16_t doom_tlut_up[2][256] __attribute__((aligned(16)));
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

// Fired (under RDP interrupt) when the RDP has finished reading a CI8 buffer.
// Marks it free and shows the framebuffer it was blitted into -- the same
// display_show that rdpq_detach_show would have scheduled.
static void I_N64BufferDone(void* arg)
{
    int idx = (int)(intptr_t)arg;

    if (doom_screen8_disp[idx])
        display_show(doom_screen8_disp[idx]);
    doom_screen8_rdp_busy[idx] = false;
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

    patch = (patch_t*)W_CacheLumpNum(lumpnum, PU_CACHE);
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

// Pick the transparency-key palette index (RDP renderer, Stage 1+). Scans every
// UI/status-bar/font/menu patch lump drawn OUTSIDE the 3D view (the ST*, M_*,
// and WI* graphic families), marks the palette indices they use, and reserves
// the highest index none of them touch. Reserving a HIGH index matches the
// design's expectation that UI art rarely touches the top of PLAYPAL. Asserts
// if every index is in use. Called once at startup, after the WAD is loaded.
void I_N64ScanTransparencyKey(void)
{
    static const char* const ui_prefixes[] = { "ST", "M_", "WI" };
    boolean used[256];
    int lump;
    int p;
    int idx;

    if (n64_rdp_key_index >= 0)
        return;                             // already chosen

    memset(used, 0, sizeof(used));

    for (lump = 0; lump < numlumps; lump++)
    {
        const char* name = lumpinfo[lump].name;

        for (p = 0; p < (int)(sizeof(ui_prefixes) / sizeof(ui_prefixes[0])); p++)
        {
            size_t plen = strlen(ui_prefixes[p]);
            if (strncmp(name, ui_prefixes[p], plen) == 0)
            {
                I_N64MarkPatchIndices(lump, used);
                break;
            }
        }
    }

    // Prefer the highest free index (UI art rarely uses the top of PLAYPAL).
    for (idx = 255; idx >= 0; idx--)
    {
        if (!used[idx])
        {
            n64_rdp_key_index = idx;
            N64_DEBUGF("I_N64ScanTransparencyKey: reserved key index %d\n", idx);
            // If the flag is already on at boot (persisted in EEPROM), the
            // master TLUT may have been packed before the key was known, so
            // apply the key's alpha=0 now and mark dirty for the next present.
            I_N64MarkPaletteDirty();
            return;
        }
    }

    // Fallback: every UI patch uses every palette index (effectively
    // impossible for DOOM art). Reserve 255 anyway so the renderer stays
    // functional; the assert documents the invariant.
    I_Error("I_N64ScanTransparencyKey: no free palette index for transparency key");
}

// Temporary Stage-1 scaffolding: fill the 3D-view region of the CI8 draw buffer
// with the transparency-key index, in batched 64-bit stores. With the flag on,
// the software renderer then overwrites every view pixel, so nothing is keyed
// out yet -- this only proves the mechanism is harmless before world geometry
// moves to the RDP. Removed in the final stage (event-driven erase-to-key).
void I_N64KeyClearView(void)
{
    byte* base;
    int key;
    uint64_t pattern;
    int x0, x1, y0, y1;
    int y;

    if (n64_rdp_key_index < 0)
        return;
    if (!screens[0])
        return;

    key = n64_rdp_key_index;
    pattern = (uint64_t)((uint8_t)key);
    pattern |= pattern << 8;
    pattern |= pattern << 16;
    pattern |= pattern << 32;

    // 3D-view window in CI8 screen coordinates (see R_InitBuffer).
    x0 = viewwindowx;
    y0 = viewwindowy;
    x1 = viewwindowx + scaledviewwidth;
    y1 = viewwindowy + viewheight;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCREENWIDTH)  x1 = SCREENWIDTH;
    if (y1 > SCREENHEIGHT) y1 = SCREENHEIGHT;
    if (x1 <= x0 || y1 <= y0)
        return;

    base = screens[0] + y0 * SCREENWIDTH;

    for (y = y0; y < y1; y++)
    {
        byte* row = base + x0;
        byte* end = base + x1;

        // Align the run to 8 bytes, then store 64 bits at a time (the same
        // uncached-RDRAM batching the span renderer uses, r_draw.c:731-784).
        while (((uintptr_t)row & 7) && row < end)
            *row++ = (byte)key;

        while (row + 8 <= end)
        {
            *(uint64_t*)row = pattern;
            row += 8;
        }

        while (row < end)
            *row++ = (byte)key;

        base += SCREENWIDTH;
    }
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

    disp = display_get();
    if (!disp)
        return;

    rdp_on = (n64_use_rdp_renderer != 0);

    rdpq_attach(disp, NULL);

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
    if (rdp_on && DL_Count() > 0)
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

            // Standard 1-cycle textured: TEX0*PRIM (free light), CI8 via TLUT,
            // perspective-correct (free INV_W). Scissor to the view window so
            // the quads can't spill outside the 3D view (DESIGN section 4).
            rdpq_set_mode_standard();
            rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
            rdpq_mode_tlut(TLUT_RGBA16);
            rdpq_mode_persp(true);
            rdpq_set_scissor(vx0, vy0, vx1, vy1);

#ifdef N64_BENCH
            N64Bench_PhaseBegin(BPH_DL_BUILD);
#endif
            DL_Flush();
#ifdef N64_BENCH
            N64Bench_PhaseEnd(BPH_DL_BUILD);
#endif

            // Restore full-screen scissor + persp off for the overlay COPY blit.
            rdpq_mode_persp(false);
            rdpq_set_scissor(0, 0, SCREENWIDTH, SCREENHEIGHT);
            world_drawn = true;
        }
    }

    // Present blit. COPY-mode alpha-compare (transparency=true) keys out the
    // alpha-0 reserved index so the RDP-drawn world shows through the view
    // window. Enabled ONLY when the RDP actually drew world geometry this frame
    // (Stage 2: the routed seg). Then the routed seg's suppressed-colfunc
    // columns hold the key index (from KEY_CLEAR), get keyed out, and reveal the
    // RDP fill drawn underneath; every other view pixel is still real software-
    // rendered colour and is blitted normally. When no world geometry was drawn
    // (flag off, or no eligible seg, or a paced menu frame), alpha-compare stays
    // OFF so the present is byte-identical to the software path -- opaque art
    // that happens to contain the key index is never punched out.
    // COPY-mode alpha-compare is valid on the 16bpp display fb
    // (rdpq_mode.h:328-330,335).
    rdpq_set_mode_copy(world_drawn ? true : false);
    rdpq_mode_tlut(TLUT_RGBA16);
    // Palette area of TMEM (upper half) is only ever written by this blit path
    // (or the world pass above), and a CI8 blit only loads texels into the lower
    // half, so the TLUT persists across frames and is re-uploaded only when it
    // changed.
    if (n64_palette_dirty)
    {
        uint16_t* slot = doom_tlut_up[n64_draw_idx];

        memcpy(slot, doom_tlut_master, sizeof(doom_tlut_master));
        data_cache_hit_writeback(slot, sizeof(doom_tlut_master));
        rdpq_tex_upload_tlut(slot, 0, 256);
        n64_palette_dirty = false;
    }
    // The CI8 source covers the entire 320x200 display, so attach (no clear)
    // is sufficient -- the blit overwrites every pixel (except, with the flag
    // on and world geometry drawn, the alpha-0 key pixels keyed out by
    // alpha-compare).
    rdpq_tex_blit(&doom_screen8[n64_draw_idx], 0, 0, NULL);
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
    N64Bench_PhaseBegin(BPH_RDP_BUSY);
#endif
    next_idx = n64_draw_idx ^ 1;
    while (doom_screen8_rdp_busy[next_idx])
        ;
#ifdef N64_BENCH
    N64Bench_PhaseEnd(BPH_RDP_BUSY);
#endif

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
