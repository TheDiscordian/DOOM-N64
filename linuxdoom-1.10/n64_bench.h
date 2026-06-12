// Deterministic A/B benchmark harness for the N64 DOOM renderer.
// Compiled in only when N64_BENCH is defined (make BENCH=1).
//
// Measures per-frame render cost in VR4300 ticks (get_ticks(), the CP0 cycle
// counter), which is deterministic under emulation and host-speed independent.
// Drives a fixed scripted ticcmd through the SHIPPING uncapped/interpolated
// render path, then freezes a large on-screen result the host can screenshot.

#ifndef N64_BENCH_H
#define N64_BENCH_H

#ifdef N64_BENCH

#include "d_ticcmd.h"

// Wired into D_DoomMain: forces autostart of the bench scenario and selects
// the uncapped single-player render path. Returns the map to warp to.
void N64Bench_Init(void);

// True while the bench is collecting samples (D_DoomLoop forces uncapped path).
int  N64Bench_Active(void);

// Called from D_DoomLoop around D_Display to time one rendered frame.
void N64Bench_FrameBegin(void);
void N64Bench_FrameEnd(void);

// Called once per gametic from G_Ticker to advance scenario timing/phases.
void N64Bench_TicHook(void);

// Computes results and freezes the overlay; called when the window completes.
void N64Bench_Finish(void);

// Called at the end of G_BuildTiccmd to overwrite real input with the script.
void N64Bench_FillTiccmd(ticcmd_t* cmd);

// Drawn at the end of D_Display (over everything) once the bench finishes,
// holding the final numbers on screen indefinitely for screenshot capture.
void N64Bench_DrawOverlay(void);

#endif // N64_BENCH
#endif // N64_BENCH_H
