// N64 system layer for timing, memory, and fatal errors.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libdragon.h>

#include "doomdef.h"
#include "m_misc.h"
#include "i_video.h"
#include "i_sound.h"
#include "d_net.h"
#include "g_game.h"
#include "i_main_n64.h"
#include "i_system.h"
#include "n64_debug.h"
#include "z_zone.h"

int mb_used = 4;

static byte* zone_base;

ticcmd_t emptycmd;

void I_N64LogMemoryStats(const char* tag)
{
#if DOOM_N64_DEBUG
    heap_stats_t heap;
    int zone_free;
    const char* label;

    label = (tag && tag[0]) ? tag : "mem";

    sys_get_heap_stats(&heap);

    if (Z_IsInitialized())
    {
        zone_free = Z_FreeMemory();
        N64_DEBUGF("MEM[%s]: heap=%d/%d KiB zone_free=%d KiB\n",
                   label,
                   heap.used / 1024,
                   heap.total / 1024,
                   zone_free / 1024);
    }
    else
    {
        N64_DEBUGF("MEM[%s]: heap=%d/%d KiB zone_free=n/a\n",
                   label,
                   heap.used / 1024,
                   heap.total / 1024);
    }
#else
    (void)tag;
#endif
}

void
I_Tactile
( int on,
  int off,
  int total )
{
    on = off = total = 0;
}

ticcmd_t* I_BaseTiccmd(void)
{
    return &emptycmd;
}

byte* I_ZoneBase(int* size)
{
    if (is_memory_expanded())
        mb_used = 4;
    else
        mb_used = 2;

    *size = mb_used * 1024 * 1024;

    if (!zone_base)
    {
        zone_base = (byte*)malloc(*size);
        if (!zone_base)
            I_Error("I_ZoneBase: failed to allocate %d bytes", *size);
    }

    return zone_base;
}

int I_GetTime(void)
{
    static uint64_t basetime_ms;
    uint64_t now_ms;

    now_ms = get_ticks_ms();
    if (!basetime_ms)
        basetime_ms = now_ms;

    return (int)(((now_ms - basetime_ms) * TICRATE) / 1000);
}

// Microsecond wall clock for sub-tic interpolation (uncapped framerate).
uint64_t I_GetTimeUS(void)
{
    return get_ticks_us();
}

// --- Settings persistence to cartridge EEPROM (game cart, not Controller Pak) ---
// Game saves use the Controller Pak; settings live in the cart's EEPROM so they
// persist without an accessory. Requires N64_ROM_SAVETYPE=eeprom4k in the build.

extern int alwaysRun, controlScheme, frame_interpolation;
extern int showMessages, detailLevel;
extern int snd_SfxVolume, snd_MusicVolume, mouseSensitivity;

#define N64_SETTINGS_MAGIC   0x444E3631u	/* 'DN61' */
#define N64_SETTINGS_VERSION 1

typedef struct
{
    uint32_t	magic;
    uint8_t	version;
    int8_t	always_run;
    int8_t	control_scheme;
    int8_t	frame_interp;
    int8_t	show_messages;
    int8_t	detail_level;
    int8_t	sfx_volume;
    int8_t	music_volume;
    int8_t	mouse_sens;
    uint8_t	reserved[3];
} n64_settings_t;

void I_N64LoadSettings(void)
{
    n64_settings_t s;

    if (eeprom_present() == EEPROM_NONE)
        return;

    eeprom_read_bytes(&s, 0, sizeof(s));
    if (s.magic != N64_SETTINGS_MAGIC || s.version != N64_SETTINGS_VERSION)
        return;			// nothing saved yet; keep compiled defaults

    alwaysRun           = s.always_run;
    controlScheme       = s.control_scheme;
    frame_interpolation = s.frame_interp;
    showMessages        = s.show_messages;
    detailLevel         = s.detail_level;
    snd_SfxVolume       = s.sfx_volume;
    snd_MusicVolume     = s.music_volume;
    mouseSensitivity    = s.mouse_sens;
}

void I_N64SaveSettings(void)
{
    n64_settings_t s;

    if (eeprom_present() == EEPROM_NONE)
        return;

    memset(&s, 0, sizeof(s));
    s.magic          = N64_SETTINGS_MAGIC;
    s.version        = N64_SETTINGS_VERSION;
    s.always_run     = (int8_t)alwaysRun;
    s.control_scheme = (int8_t)controlScheme;
    s.frame_interp   = (int8_t)frame_interpolation;
    s.show_messages  = (int8_t)showMessages;
    s.detail_level   = (int8_t)detailLevel;
    s.sfx_volume     = (int8_t)snd_SfxVolume;
    s.music_volume   = (int8_t)snd_MusicVolume;
    s.mouse_sens     = (int8_t)mouseSensitivity;

    eeprom_write_bytes(&s, 0, sizeof(s));
}

void I_Init(void)
{
    I_InitSound();
}

void I_Quit(void)
{
    D_QuitNetGame();
    I_ShutdownSound();
    I_ShutdownMusic();
    M_SaveDefaults();
    I_ShutdownGraphics();
    I_N64ReturnToBrowser();
}

void I_WaitVBL(int count)
{
    if (count > 0)
        wait_ms(((unsigned long)count * 1000UL) / 70UL);
}

void I_BeginRead(void)
{
}

void I_EndRead(void)
{
}

byte* I_AllocLow(int length)
{
    byte* mem;

    mem = (byte*)malloc(length);
    if (!mem)
        I_Error("I_AllocLow: failed to allocate %d bytes", length);

    memset(mem, 0, length);
    return mem;
}

extern boolean demorecording;

void I_Error(char *error, ...)
{
    char msg[512];
    va_list argptr;

    va_start(argptr, error);
    vsnprintf(msg, sizeof(msg), error, argptr);
    va_end(argptr);

    N64_DEBUGF("Error: %s\n", msg);

    if (demorecording)
        G_CheckDemoStatus();

    D_QuitNetGame();
    I_ShutdownGraphics();

    assertf(false, "%s", msg);
    abort();
}
