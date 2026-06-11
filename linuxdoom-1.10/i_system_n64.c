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
