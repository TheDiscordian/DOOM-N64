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

#include <stdint.h>     // uint64_t in the virtual-clock API below
#include "d_ticcmd.h"

// Wired into D_DoomMain: forces autostart of the bench scenario and selects
// the uncapped single-player render path. Returns the map to warp to.
void N64Bench_Init(void);

// True while the bench is collecting samples (D_DoomLoop forces uncapped path).
int  N64Bench_Active(void);

// Deterministic virtual tic clock. N64Bench_VirtualTick() is called once per
// D_DoomLoop iteration (before the tic-production pass); I_GetTime() reads
// N64Bench_VirtualTimeMs() while the bench is active so the tic cadence is host-
// independent and the scripted playthrough is byte-identical run to run.
void     N64Bench_VirtualTick(void);
uint64_t N64Bench_VirtualTimeMs(void);

// Called from D_DoomLoop around D_Display to time one rendered frame.
void N64Bench_FrameBegin(void);
void N64Bench_FrameEnd(void);

// --- per-phase profiling --------------------------------------------------
// All phase boundaries are CP0-cheap (get_ticks()); kept out of inner loops.
// Each Begin/End pair brackets one phase and accumulates ticks into the
// current frame's slot. Counts are latched at FrameEnd (or explicit setters).
//
// Phase indices for the per-frame breakdown.
//
// The old BPH_BSP lumped two distinct costs: the CPU BSP walk / 1-D occlusion
// clip / scale math (KEPT on the CPU forever) and the per-column wall fill
// inside R_RenderSegLoop (the rasterization the RDP renderer offloads). They
// are split into BSP_WALK and SEG_RASTER so the offload target is measured
// directly. PLANE_EMIT/MASKED_EMIT/DL_BUILD/RDP_BUSY are the RDP renderer's
// future phases (display-list emit, list build/flush, and the async RDP busy
// window); they measure ~0 until the RDP path lands the work in them.
typedef enum
{
    BPH_GAMETIC = 0,   // TryRunTics (sim) in the uncapped path
    BPH_BSP_WALK,      // R_RenderBSPNode minus the per-column wall fill (walk/clip/scale)
    BPH_SEG_RASTER,    // R_RenderSegLoop column-fill loop (the wall raster the RDP offloads)
    BPH_PLANES,        // R_DrawPlanes (software span fill)
    BPH_MASKED,        // R_DrawMasked (sprite sort + sprites + masked segs + psprites)
    BPH_PLANE_EMIT,    // RDP renderer: plane span emit (~0 until planes move to RDP)
    BPH_MASKED_EMIT,   // RDP renderer: sprite/masked emit (~0 until sprites move to RDP)
    BPH_DL_BUILD,      // RDP renderer: DL_Flush list build/upload (~0 until DL exists)
    BPH_RDP_BUSY,      // RDP renderer: async RDP busy window read at frame top (~0 until RDP draws the world)
    BPH_KEY_CLEAR,     // RDP renderer: temporary view-window key-clear (~0 unless the flag is on)
    BPH_HUD,           // D_Display work outside the 3D view (status bar/HUD/border/menu)
    BPH_PRESENT,       // I_FinishUpdate (page flip / buffer-busy spin)
    BPH_AUDIO,         // S_UpdateSounds + I_SubmitSound (post-display)
    BPH_COUNT
} bench_phase_id_t;

// Brackets the whole D_DoomLoop iteration (sim + audio + display). The
// per-phase breakdown's "frame total" is this loop-wall time; "leftover" is
// the wall minus all accounted phases. Kept distinct from FrameBegin/FrameEnd,
// which still bracket only D_Display for the unchanged BENCH_RESULT line.
void N64Bench_LoopBegin(void);
void N64Bench_LoopEnd(void);

// Bracket a phase. End attributes (now - the matching Begin) to phase `id`.
void N64Bench_PhaseBegin(int id);
void N64Bench_PhaseEnd(int id);

// One get_ticks() read that ends `end_id` and begins `begin_id` -- used at
// back-to-back phase boundaries (e.g. bsp->planes->masked, separated only by a
// single-player no-op NetUpdate) so the hot render path takes one CP0 read per
// boundary instead of two.
void N64Bench_PhaseSwitch(int end_id, int begin_id);

// Brackets the whole D_Display call. DisplayEnd derives BPH_HUD as the display
// wall time minus the in-view render phases (bsp/planes/masked) and present,
// i.e. status bar + HUD + border + menu + render setup/clears -- everything in
// D_Display that is not the 3D draw or the page flip.
void N64Bench_DisplayBegin(void);
void N64Bench_DisplayEnd(void);

// Record how many gametics TryRunTics advanced this frame (0 or more).
void N64Bench_SetTicsRan(int tics);

// Latch the renderer's per-frame work counts (vissprites/drawsegs/visplanes).
// Called from R_RenderPlayerView after R_DrawMasked, when the pools are full.
void N64Bench_SetCounts(int vissprites, int drawsegs, int visplanes);

#ifdef PLANETESS_COUNT
// Latch the count-only go/no-go measurement for "visplanes as RDP polygons":
// the triangle count this frame's visplanes WOULD tessellate into as RDP
// trapezoid strips (R_CountPlanePolyTris). Same call site as SetCounts.
void N64Bench_SetPlanePolyTris(int polytris);
#endif

#ifdef RDPWAIT_PROBE
// Async RDP/RDRAM stall attribution (where the CPU blocks on the GPU).
// N64Bench_NoteAsyncStall is called FROM the RDP-completion interrupt
// (I_N64BufferDone, fired via rdpq_detach_cb on DP SYNC_FULL) with the wall-clock
// ticks that interrupt spent; it charges them to whichever BPH_* render bracket is
// open at fire time. NoteDispGet/NoteRdpBusySpins split the two present-seam waits
// (free-framebuffer acquire, buffer-flip spin) out of PRESENT/RDP_BUSY. All three
// are TIME/COUNT-ONLY -- no bracket boundary moves, no rendering changes.
void N64Bench_NoteAsyncStall(uint64_t ticks);
void N64Bench_NoteDispGet(uint64_t ticks);
void N64Bench_NoteRdpBusySpins(uint32_t spins);
#endif

#ifdef PVS_PROBE
// Latch the count-only PVS/occlusion-bake go/no-go measurement: subsectors
// VISITED this frame (= sscount) and how many of those the existing REJECT
// matrix would have culled from the view sector (a free LOWER-bound stand-in
// for a true subsector PVS). Accumulated in r_bsp.c R_Subsector; latched at the
// SetCounts call site (r_main.c). COUNT-ONLY -- perturbs no geometry.
void N64Bench_SetPvsCounts(int visited, int cullable);
#endif

#ifdef BAKEFAN_PROBE
// Latch the count-only baked-leaf-fan go/no-go: the leaf-fan triangle total this
// frame's drawn subsector floors/ceilings WOULD emit under a native offline bake
// ((numsegs - 2) clamp >=1 per visible plane). Accumulated in r_bsp.c R_Subsector;
// latched at the SetCounts call site (r_main.c). COUNT-ONLY -- perturbs no geometry.
void N64Bench_SetBakefanTris(int tris);
#endif

#ifdef DPLANES_PROBE
// Sub-bracket the RDP plane work (the ~1646us `planes` BPH bracket) into its
// three constituents so the optimizer attacks the real cost, not a guess:
//   (a) lump   -- per-visplane W_CacheLumpNum + Z_ChangeTag (the flat lump cache);
//   (b) fitter -- the recursive R_EmitIslandRuns run-fitter (island walk +
//                 deviation scan + split), MINUS the corner un-projection it calls;
//   (c) unproj -- the R_PlaneCornerAttr float un-projections (the 4-corner clusters
//                 in R_EmitRunPoly / R_EmitPlaneBand).
// r_plane.c accumulates RAW CP0 ticks per frame into three counters and calls this
// once per frame (at the R_DrawPlanes return) to latch them; n64_bench.c converts
// to us and reports mean + EXACT p95 per sub-part on the BENCH_DPLANES line. The
// emit-path body is timed in place but NOT changed -- the geometry fingerprint is
// unperturbed, so it is OFF by default (pass DPLANES_PROBE=1 on the make line).
void N64Bench_SetDPlanes(uint32_t lump_tk, uint32_t fitter_tk, uint32_t unproj_tk,
                         uint32_t scan_cols, uint32_t nodes);
#endif

// Called once per gametic from G_Ticker to advance scenario timing/phases.
void N64Bench_TicHook(void);

// Computes results and freezes the overlay; called when the window completes.
void N64Bench_Finish(void);

// Called at the end of G_BuildTiccmd to overwrite real input with the script.
void N64Bench_FillTiccmd(ticcmd_t* cmd);

// MP bench: scripted movement fields for one local player (consistancy is
// left untouched). Player 0 follows the standard table; higher players run
// it phase-shifted (distinct phase per player) so each pane sees different
// geometry.
void N64Bench_FillTiccmdMP(ticcmd_t* cmd, int playernum);

// One-time log that the interpolated render path engaged.
void N64Bench_NoteInterp(int local_players);

// Drawn at the end of D_Display (over everything) once the bench finishes,
// holding the final numbers on screen indefinitely for screenshot capture.
void N64Bench_DrawOverlay(void);

// Retained-frame counter (the SAME counter the BENCH_MARK frame=N markers key
// off: marker N fires when this count reaches N at commit). Read DURING a render
// (e.g. plane diagnostics) it returns the frames COMMITTED SO FAR, so the render
// whose commit produces marker N sees this == N-1. Diagnostics-only accessor;
// 0 before the bench reaches BENCH_RUNNING / outside bench builds.
unsigned long N64Bench_FrameNo(void);

#endif // N64_BENCH
#endif // N64_BENCH_H
