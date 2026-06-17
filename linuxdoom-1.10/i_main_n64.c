// N64 entrypoint for the DOOM port.

#include <setjmp.h>
#include <stdlib.h>

#include <libdragon.h>

#include "doomdef.h"
#include "m_argv.h"
#include "d_main.h"
#include "i_wad_browser_n64.h"
#include "i_main_n64.h"

static char* n64_argv[] = { "doom", NULL };
static jmp_buf n64_browser_return;
static int n64_browser_return_armed;

void I_N64ReturnToBrowser(void)
{
    if (n64_browser_return_armed)
        longjmp(n64_browser_return, 1);

    // Fallback behavior if called before main loop is armed.
    exit(0);
}

int main(void)
{
    debug_init_isviewer();
    debug_init_usblog();
    timer_init();

    assertf(is_memory_expanded(), "Expansion Pak (8MB) required");

    dfs_init(DFS_DEFAULT_LOCATION);
    joypad_init();

    switch (joypad_get_accessory_type(JOYPAD_PORT_1))
    {
        case JOYPAD_ACCESSORY_TYPE_CONTROLLER_PAK:
            debugf("N64 startup: Controller Pak detected on port 1\n");
            break;
        case JOYPAD_ACCESSORY_TYPE_NONE:
            debugf("N64 startup: no accessory on port 1\n");
            break;
        default:
            debugf("N64 startup: non-pak accessory on port 1\n");
            break;
    }

    for (;;)
    {
        (void)setjmp(n64_browser_return);
        n64_browser_return_armed = 1;

#ifdef N64_BENCH
        // Skip the WAD browser; auto-load the shareware IWAD for the bench.
        I_N64ForceSelectedWad("rom:/DOOM1.WAD");
#else
        I_N64RunWadBrowser();
#endif

        myargc = 1;
        myargv = n64_argv;

        D_DoomMain();
    }

    return 0;
}
