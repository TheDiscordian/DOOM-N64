// Deterministic A/B benchmark harness. See n64_bench.h.
//
// Frame cost is measured in VR4300 ticks via get_ticks() (CP0 cycle counter),
// which advances with emulated CPU cycles and is therefore independent of host
// emulation speed. Ticks are converted to microseconds with TICKS_TO_US (the
// libdragon macro keyed on TICKS_PER_SECOND = CPU_FREQUENCY/2). A retail N64
// runs CPU_FREQUENCY = 93.75 MHz, so TICKS_PER_SECOND = 46.875 MHz.

#ifdef N64_BENCH

#include <libdragon.h>
#include <string.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_ticcmd.h"
#include "g_game.h"
#include "n64_bench.h"
#include "n64_debug.h"

// m_menu.c font helpers (external linkage, not in m_menu.h).
extern int  M_StringWidth(char* string);
extern void M_WriteTextScaled(int x, int y, char* string, int num, int den);

// Scenario length in gametics (35 Hz). 35*60 = 60 emulated seconds.
#define BENCH_GAMETICS      (35 * 60)
// Discard the first second of rendered frames (level load / cache warm-up).
#define BENCH_WARMUP_GAMETICS (35 * 1)

// Deterministic virtual tic clock. The shipping tic clock (I_GetTime ->
// get_ticks_ms) is host wall-clock, so NetUpdate's newtics = elapsed-since-last
// is lumpy under host jitter: a run that hitches produces tics in 0/2 bursts
// instead of 1/1, which phase-shifts the scripted input vs the simulation that
// consumes it and forks the playthrough (a different player path -> a death and
// level reload in one build, none in another). That is exactly what made
// drawsegs jump 8->11 and inserted a ~1.2s reload frame between two builds that
// are byte-identical in the flag-off path. Fix: while the bench is active,
// I_GetTime() reads this virtual clock instead. It advances a FIXED amount per
// render-loop iteration so the sim consumes exactly one tic every
// BENCH_FRAMES_PER_TIC rendered frames, identical on any host. The uncapped+
// interpolated render path is still fully exercised (multiple rendered frames
// per tic); only the *tic cadence* is pinned. Sub-tic interpolation phase still
// reads the wall clock (I_GetTimeUS) -- that only reshuffles which interpolated
// camera each render frame samples (cosmetic mean-drawseg jitter), never the
// simulation, so it cannot fork the playthrough.
#define BENCH_FRAMES_PER_TIC  2
// Virtual ms per render-loop iteration: TICRATE*ms/1000 must clear one tic every
// BENCH_FRAMES_PER_TIC iterations. ms_per_tic = 1000/TICRATE; per iter = that /
// BENCH_FRAMES_PER_TIC. Computed in N64Bench_VirtualTimeMs from an iteration
// counter so it is exact integer-deterministic (no float, no host clock).
static uint64_t         bench_virtual_iter;   // render-loop iterations elapsed

// Per-frame cost histogram for a RAM-cheap p95. 64 us per bucket * 4096 =
// 0..262 ms range, which comfortably brackets N64 frame costs (~14-50 ms).
#define BENCH_HIST_BUCKETS  4096
#define BENCH_HIST_US_SHIFT 6      // 64 us per bucket
#define BENCH_HIST_OVERFLOW (BENCH_HIST_BUCKETS - 1)

// Per-frame profiling record ring. The scenario runs BENCH_GAMETICS at 35 Hz
// and renders uncapped (~55 fps), so frame count tops out near 3300. Size the
// ring generously; frames past the cap are still counted in the histogram/avg
// but not retained for the tail report (never reached in practice).
#define BENCH_MAX_FRAMES    4096

// us above which a frame is treated as a level-load outlier (death/respawn
// reload), not a render frame: excluded from tail stats, reported separately.
// The combat scenario's only such event measures ~1.07 s; render frames are
// well under 60 ms. 200 ms is a safe gap.
#define BENCH_OUTLIER_US    200000UL

// Worst-N individual frames listed in the tail report.
#define BENCH_WORST_N       10
// Tail = worst this-percent of (non-outlier) frames.
#define BENCH_TAIL_PCT      5

typedef struct
{
    uint32_t total_us;
    uint32_t phase_us[BPH_COUNT];
    uint16_t vissprites;
    uint16_t drawsegs;
    uint16_t visplanes;
    uint16_t tile_loads;    // RDP wall band LOAD_TILEs this frame (CI4 diagnostic)
    uint16_t recs;          // RDP wall records drawn this present (A/B lever)
    uint16_t uploads;       // RDP wall upload calls this present (A/B lever)
    uint16_t tris;          // RDP wall triangles emitted this present (A/B lever)
#ifdef PLANETESS_COUNT
    uint16_t plane_polytris; // count-only: tris this frame's visplanes WOULD
                             // tessellate into as RDP trapezoid strips (go/no-go
                             // for the "visplanes as RDP polygons" feature; no
                             // render, no UV -- R_CountPlanePolyTris in r_plane.c)
#endif
#ifdef PVS_PROBE
    uint16_t pvs_visited;    // count-only: subsectors VISITED this frame (= sscount;
                             // the BSP-walk-survivors AFTER R_CheckBBox node prune).
    uint16_t pvs_cullable;   // count-only: of those visited, how many the existing
                             // REJECT matrix (view sector vs subsector frontsector)
                             // says are NOT visible -- a free LOWER-bound stand-in
                             // for what a real PVS would cull. go/no-go for the
                             // PVS/occlusion bake. (r_bsp.c R_Subsector)
#endif
#ifdef BAKEFAN_PROBE
    uint16_t bakefan_tris;   // count-only: leaf-fan tris this frame's DRAWN
                             // subsector floors/ceilings WOULD emit under a native
                             // offline bake ((numsegs-2) clamp >=1 per visible
                             // plane). A/B vs plane_polytris (the runtime trapezoid
                             // tessellation the bake replaces). r_bsp.c R_Subsector.
#endif
#ifdef DPLANES_PROBE
    uint32_t dpl_lump_us;    // sub-bracket of `planes`: per-visplane W_CacheLumpNum
    uint32_t dpl_fitter_us;  //   + Z_ChangeTag / the run-fitter (minus un-projection)
    uint32_t dpl_unproj_us;  //   / the R_PlaneCornerAttr float un-projections.
    uint32_t dpl_scan_cols;  // deviation-scan column iterations this frame
    uint32_t dpl_nodes;      // R_EmitIslandRuns invocations this frame
#endif
#ifdef BSPWALK_PROBE
    // Per-frame us for the bsp_walk sub-brackets, for the TAIL breakdown ONLY (uint16_t
    // to keep the bench_frames[] BSS bloat to ~32KB; the MEAN comes from running sums).
    uint16_t bspw_addline_net_us;
    uint16_t bspw_checkbbox_us;
    uint16_t bspw_sprite_us;
    uint16_t bspw_mesh_us;
#endif
#ifdef RDPWAIT_PROBE
    uint32_t async_us[BPH_COUNT];  // async RDP-completion interrupt us charged to
                                   // each open phase this frame (where the DP
                                   // SYNC_FULL stall actually lands)
    uint16_t async_fires;          // DP-completion interrupts serviced this frame
    uint16_t dispget_us;           // display_get() wall us (PRESENT sub-bracket:
                                   // the vsync-coupled free-framebuffer wait)
    uint16_t rdpbusy_spins;        // buffer-flip spin iterations this frame
#endif
    uint8_t  tics_ran;
    uint8_t  is_outlier;
} bench_frame_t;

typedef enum
{
    BENCH_WARMUP,
    BENCH_RUNNING,
    BENCH_DONE
} bench_phase_t;

static bench_phase_t    bench_phase = BENCH_WARMUP;
static int              bench_started;

static uint64_t         frame_start_ticks;
static int              frame_open;        // FrameBegin seen, FrameEnd pending

static unsigned long    rendered_frames;   // frames counted into stats
static unsigned long long sum_us;
static unsigned long    min_us = 0xFFFFFFFFUL;
static unsigned long    max_us;
static unsigned long    hist[BENCH_HIST_BUCKETS];

static char             result_line1[32];
static char             result_line2[32];
static char             result_line3[32];

// --- per-phase profiling state -------------------------------------------
static bench_frame_t    bench_frames[BENCH_MAX_FRAMES];
static unsigned long    bench_frame_count;     // retained records (capped)

static uint32_t         cur_phase_tk[BPH_COUNT];   // accumulates over a frame
static uint64_t         phase_begin_ticks[BPH_COUNT];
static uint32_t         phase_open_mask;           // bit n: phase n bracket open
static uint64_t         loop_start_ticks;          // whole-iteration wall clock
static int              loop_open;
static uint64_t         display_start_ticks;       // D_Display wall clock
static int              cur_tics_ran;
static uint16_t         cur_vissprites;
static uint16_t         cur_drawsegs;
static uint16_t         cur_visplanes;
#ifdef PLANETESS_COUNT
static uint16_t         cur_plane_polytris;	// count-only plane-poly tris this frame
#endif
#ifdef PVS_PROBE
static uint16_t         cur_pvs_visited;	// count-only subsectors visited this frame
static uint16_t         cur_pvs_cullable;	// count-only REJECT-cullable of those visited
#endif
#ifdef BAKEFAN_PROBE
static uint16_t         cur_bakefan_tris;	// count-only baked-leaf-fan tris this frame
#endif
#ifdef DPLANES_PROBE
// Sub-bracket of the `planes` BPH bracket. r_plane.c accumulates RAW CP0 ticks
// into its own per-frame counters during R_DrawPlanes and passes them here via
// N64Bench_SetDPlanes (called at the R_DrawPlanes return). Stored as us per frame.
static uint32_t         cur_dpl_lump_us;
static uint32_t         cur_dpl_fitter_us;
static uint32_t         cur_dpl_unproj_us;
static uint32_t         cur_dpl_scan_cols;
static uint32_t         cur_dpl_nodes;
#endif
#ifdef BSPWALK_PROBE
// Sub-bracket of the `bsp_walk` BPH bracket (per-frame us, latched by N64Bench_SetBspWalk).
static uint32_t         cur_bspw_addline_net_us;
static uint32_t         cur_bspw_segloop_us;
static uint32_t         cur_bspw_checkbbox_us;
static uint32_t         cur_bspw_sprite_us;
static uint32_t         cur_bspw_mesh_us;
static uint32_t         cur_bspw_rspwait_us;
static uint32_t         cur_bspw_addline_calls;
// Running totals across the measured frames. MEAN-ONLY (no per-frame array): adding 6
// per-frame fields to bench_frames[BENCH_MAX_FRAMES] bloats BSS ~98KB and starves the
// heap (surface/scratch malloc fails at I_InitGraphics -- same OOM RDPWAIT_PROBE hits).
// The mean breakdown answers the headline (each sub-part's SHARE of bsp_walk).
static unsigned long long bspw_addline_sum, bspw_segloop_sum, bspw_checkbbox_sum;
static unsigned long long bspw_sprite_sum,  bspw_mesh_sum,    bspw_calls_sum;
static unsigned long long bspw_rspwait_sum;
static unsigned long      bspw_nframes;
#endif

#ifdef RDPWAIT_PROBE
// Async RDP/RDRAM stall attribution. The RDP-completion callback (I_N64BufferDone,
// fired from the DP interrupt handler via rdpq_detach_cb) runs in INTERRUPT
// context: its wall-clock service time is charged to whatever BPH_* bracket is
// open at the instant the DP raises SYNC_FULL. This probe attributes that
// interrupt-service time per open phase, proving WHERE the "phantom" async stall
// (e.g. the ~1644us planes residual that survives an R_DrawPlanes body no-op)
// actually lands -- a CP0-completion interrupt, not CPU work in that phase.
//
// Accumulated per FRAME into cur_async_tk[phase] (interrupt adds raw ticks to the
// open phase) and committed to the frame record at LoopEnd, alongside fire counts.
// COUNT/TIME-ONLY -- changes no rendering and no phase bracket boundary.
static uint32_t         cur_async_tk[BPH_COUNT];   // async interrupt ticks per open phase this frame
static uint32_t         cur_async_fires;           // DP-completion interrupts this frame
static uint32_t         cur_dispget_tk;            // display_get() wall ticks (PRESENT sub-bracket)
static uint32_t         cur_rdpbusy_spins;         // buffer-flip spin iterations this frame
#endif

// Outlier (level-reload) frames: counted separately, kept out of tail stats.
static unsigned long    outlier_frames;
static unsigned long    outlier_max_us;

// --- scripted input -------------------------------------------------------
// A fixed forward-walk-with-turns pattern from the E1M1 spawn. Deterministic:
// identical every run, no demo lumps, no RNG. Tuned to keep the player moving
// through varied geometry (open rooms, the toxin maze) so the renderer sees a
// representative mix of wall/sprite/visplane load rather than a static view.
//
// One pattern step spans STEP_TICS gametics; the table loops.
#define BENCH_STEP_TICS     20

typedef struct
{
    signed char forward;   // *2048 movement
    signed char side;
    short       turn;      // <<16 angle delta per tic
    byte        buttons;   // held for the whole step; USE retriggers because
                           // adjacent steps release it
} bench_step_t;

static const bench_step_t bench_script[] =
{
    {  50,   0,      0, 0         },   // forward
    {  50,   0,   -640, BT_ATTACK },   // forward + turn left, firing (noise wakes monsters)
    {  50,   0,      0, BT_USE    },   // forward, try doors
    {  25,   0,    768, 0         },   // forward + turn right (wide)
    {   0,   0,    768, BT_USE    },   // pivot in place, try doors
    {  50,   0,      0, 0         },   // forward
    { -25,   0,   -512, BT_ATTACK },   // back-pedal + turn, firing
    {  50,  25,      0, BT_USE    },   // forward + strafe, try doors
    {  50,   0,    384, 0         },   // forward + slow turn
    {   0,   0,  -1024, BT_ATTACK },   // fast pivot, firing
};
#define BENCH_SCRIPT_STEPS  (sizeof(bench_script) / sizeof(bench_script[0]))

static int bench_tic_index;   // counts gametics fed to the script

// -------------------------------------------------------------------------

void N64Bench_Init(void)
{
    bench_started = 1;
    bench_phase   = BENCH_WARMUP;
    bench_tic_index = 0;
    rendered_frames = 0;
    sum_us = 0;
    min_us = 0xFFFFFFFFUL;
    max_us = 0;
    memset(hist, 0, sizeof(hist));

    bench_frame_count = 0;
    memset(cur_phase_tk, 0, sizeof(cur_phase_tk));
    cur_tics_ran = 0;
    cur_vissprites = cur_drawsegs = cur_visplanes = 0;
#ifdef PLANETESS_COUNT
    cur_plane_polytris = 0;
#endif
#ifdef PVS_PROBE
    cur_pvs_visited = cur_pvs_cullable = 0;
#endif
#ifdef BAKEFAN_PROBE
    cur_bakefan_tris = 0;
#endif
    outlier_frames = 0;
    outlier_max_us = 0;
    bench_virtual_iter = 0;

    debugf("BENCH: init, scenario=E1M1 uncapped, target=%d gametics, "
           "frames_per_tic=%d (deterministic virtual tic clock)\n",
           BENCH_GAMETICS, BENCH_FRAMES_PER_TIC);
}

int N64Bench_Active(void)
{
    // Forces the uncapped single-player render path in D_DoomLoop. Stays true
    // through DONE so input keeps flowing (idle) and the overlay holds.
    return bench_started;
}

// Retained-frame counter, the SAME one BENCH_MARK frame=N keys off (the marker
// fires in N64Bench_CommitFrame when (bench_frame_count & 127) == 0, i.e. at
// 128, 256, 384...). Read DURING a render (before that frame's commit) it is the
// count of frames already committed, so the render whose commit makes the count
// reach N sees this == N-1. Diagnostics only (PLANE_UV_TRACE pairs its dump to a
// marker frame via this); 0 before BENCH_RUNNING.
unsigned long N64Bench_FrameNo(void)
{
    return bench_frame_count;
}

// Per-render-frame heartbeat: advance the deterministic virtual clock by one
// "frame" worth of time. Called once per D_DoomLoop iteration AND once per inner
// present (screen wipe), so any loop that spins waiting on I_GetTime() to
// advance still terminates -- the clock is driven by frames presented, never by
// the host wall clock.
void N64Bench_VirtualTick(void)
{
    if (bench_started)
        bench_virtual_iter++;
}

// Deterministic replacement for the wall-clock ms used by I_GetTime() while the
// bench is active. The virtual clock advances ONE frame per N64Bench_VirtualTick
// and BENCH_FRAMES_PER_TIC frames map to one 35 Hz tic, so NetUpdate's newtics
// has a fixed host-independent cadence and the scripted playthrough is identical
// run to run (no spurious death/level-reload fork). Integer math only: no host
// clock, no float.
//
// Crucially this must ALSO be strictly monotonic across the inner spin loops
// (the screen-wipe melt busy-waits on I_GetTime advancing): every present bumps
// the frame counter via N64Bench_VirtualTick, so the wipe always makes progress
// and the level transition can never deadlock the bench.
uint64_t N64Bench_VirtualTimeMs(void)
{
    // ms = frames * 1000 / (TICRATE * FRAMES_PER_TIC). I_GetTime() recomputes
    // tic = ms*TICRATE/1000 = frames / FRAMES_PER_TIC.
    return (bench_virtual_iter * 1000ULL) /
           ((uint64_t)TICRATE * BENCH_FRAMES_PER_TIC);
}

void N64Bench_FrameBegin(void)
{
    if (!bench_started || bench_phase == BENCH_DONE)
        return;
    frame_start_ticks = get_ticks();
    frame_open = 1;
}

void N64Bench_FrameEnd(void)
{
    uint64_t elapsed;
    unsigned long us;

    if (!frame_open || bench_phase == BENCH_DONE)
        return;
    frame_open = 0;

    elapsed = get_ticks() - frame_start_ticks;
    us = (unsigned long)TICKS_TO_US(elapsed);

    if (bench_phase != BENCH_RUNNING)
        return;                 // warm-up frames not counted

    // Level-reload (death/respawn) frames are not render frames. The per-phase
    // path (LoopEnd) already excludes them via BENCH_OUTLIER_US, but the
    // BENCH_RESULT avg_us/min_us/max_us/histogram are a SEPARATE accumulator
    // that did not -- so one ~1.2 s reload frame alone inflated avg_us by
    // ~400 us (+2.3 pts of the bogus "+8%") and pinned max_us at ~1.1 s.
    // Apply the same exclusion here so BENCH_RESULT and the phase report agree
    // on which frames are real render frames.
    if (us >= BENCH_OUTLIER_US)
        return;

    rendered_frames++;
    sum_us += us;
    if (us < min_us) min_us = us;
    if (us > max_us) max_us = us;
    {
        unsigned long bucket = us >> BENCH_HIST_US_SHIFT;
        if (bucket >= BENCH_HIST_BUCKETS)
            bucket = BENCH_HIST_OVERFLOW;
        hist[bucket]++;
    }
}

// --- per-phase profiling --------------------------------------------------

void N64Bench_LoopBegin(void)
{
    if (!bench_started || bench_phase == BENCH_DONE)
        return;
    // Fresh per-frame accumulators for this whole-iteration profile.
    memset(cur_phase_tk, 0, sizeof(cur_phase_tk));
    cur_tics_ran = 0;
    cur_vissprites = cur_drawsegs = cur_visplanes = 0;
#ifdef PLANETESS_COUNT
    cur_plane_polytris = 0;
#endif
#ifdef PVS_PROBE
    cur_pvs_visited = cur_pvs_cullable = 0;
#endif
#ifdef BAKEFAN_PROBE
    cur_bakefan_tris = 0;
#endif
#ifdef DPLANES_PROBE
    cur_dpl_lump_us = cur_dpl_fitter_us = cur_dpl_unproj_us = 0;
    cur_dpl_scan_cols = cur_dpl_nodes = 0;
#endif
#ifdef BSPWALK_PROBE
    cur_bspw_addline_net_us = cur_bspw_segloop_us = cur_bspw_checkbbox_us = 0;
    cur_bspw_sprite_us = cur_bspw_mesh_us = cur_bspw_addline_calls = 0;
    cur_bspw_rspwait_us = 0;
#endif
#ifdef RDPWAIT_PROBE
    memset(cur_async_tk, 0, sizeof(cur_async_tk));
    cur_async_fires = 0;
    cur_dispget_tk = 0;
    cur_rdpbusy_spins = 0;
#endif
    loop_start_ticks = get_ticks();
    loop_open = 1;
}

void N64Bench_PhaseBegin(int id)
{
    if (!loop_open || id < 0 || id >= BPH_COUNT)
        return;
    phase_begin_ticks[id] = get_ticks();
    phase_open_mask |= (1u << id);
}

void N64Bench_PhaseEnd(int id)
{
    if (!loop_open || id < 0 || id >= BPH_COUNT)
        return;
    // Accumulate RAW ticks (no divide): TICKS_TO_US is deferred to commit so
    // the per-phase hot path is only a CP0 read plus an add. Audio brackets
    // fire twice per frame, hence +=. The open-mask guard makes every bracket
    // safe against unpaired calls: I_FinishUpdate carries PhaseSwitch brackets
    // that pause/resume PRESENT around DL_BUILD/RDP_BUSY, but the wipe melt
    // loop calls I_FinishUpdate OUTSIDE any PRESENT bracket -- closing a phase
    // that was never opened would accumulate a garbage delta from a stale
    // begin tick.
    if (!(phase_open_mask & (1u << id)))
        return;
    phase_open_mask &= ~(1u << id);
    cur_phase_tk[id] += (uint32_t)(get_ticks() - phase_begin_ticks[id]);
}

void N64Bench_PhaseSwitch(int end_id, int begin_id)
{
    uint64_t now;
    if (!loop_open)
        return;
    now = get_ticks();   // single read marks both the close and the open
    if (end_id >= 0 && end_id < BPH_COUNT
        && (phase_open_mask & (1u << end_id)))
    {
        phase_open_mask &= ~(1u << end_id);
        cur_phase_tk[end_id] += (uint32_t)(now - phase_begin_ticks[end_id]);
    }
    if (begin_id >= 0 && begin_id < BPH_COUNT)
    {
        phase_begin_ticks[begin_id] = now;
        phase_open_mask |= (1u << begin_id);
    }
}

#ifdef RDPWAIT_PROBE
// Called from the RDP-completion interrupt (I_N64BufferDone, via rdpq_detach_cb)
// with the wall-clock ticks the interrupt handler spent. Attributes that time to
// whichever render phase bracket is OPEN right now -- the phase the async DP
// SYNC_FULL interrupt happened to land in. The lowest open render-phase bit is
// the innermost active bracket. COUNT/TIME-ONLY: no bracket boundary moves.
//
// loop_open / the phase mask are plain statics touched by the main render thread
// AND this interrupt; the interrupt is brief and the main thread only reads these
// fields right after this returns (at the next get_ticks boundary), so a torn read
// would at worst misattribute one fire by one phase -- acceptable for a probe.
void N64Bench_NoteAsyncStall(uint64_t ticks)
{
    int p;
    if (!loop_open)
        return;
    cur_async_fires++;
    // Attribute to the innermost open render phase (skip GAMETIC, which is sim,
    // and the derived HUD slot). If nothing render-ish is open, charge GAMETIC.
    for (p = BPH_BSP_WALK; p < BPH_COUNT; p++)
    {
        if (phase_open_mask & (1u << p))
        {
            cur_async_tk[p] += (uint32_t)ticks;
            return;
        }
    }
    cur_async_tk[BPH_GAMETIC] += (uint32_t)ticks;
}

// Sub-bracket helpers for the present-seam waits (display_get + buffer-flip spin).
// Called from i_video_n64.c around the two specific blocking points so their cost
// is split out of the PRESENT/RDP_BUSY brackets. TIME/COUNT-ONLY.
void N64Bench_NoteDispGet(uint64_t ticks)
{
    if (!loop_open)
        return;
    cur_dispget_tk += (uint32_t)ticks;
}

void N64Bench_NoteRdpBusySpins(uint32_t spins)
{
    if (!loop_open)
        return;
    cur_rdpbusy_spins += spins;
}
#endif

void N64Bench_SetTicsRan(int tics)
{
    if (!loop_open)
        return;
    cur_tics_ran = tics;
}

void N64Bench_SetCounts(int vissprites, int drawsegs, int visplanes)
{
    if (!loop_open)
        return;
    cur_vissprites = (uint16_t)vissprites;
    cur_drawsegs   = (uint16_t)drawsegs;
    cur_visplanes  = (uint16_t)visplanes;
}

#ifdef PLANETESS_COUNT
// Count-only latch for the "visplanes as RDP polygons" go/no-go measurement:
// the number of triangles this frame's visplanes WOULD tessellate into as RDP
// trapezoid strips (R_CountPlanePolyTris in r_plane.c). Separate setter so the
// SetCounts call site (r_main.c) stays byte-identical when the flag is off.
void N64Bench_SetPlanePolyTris(int polytris)
{
    if (!loop_open)
        return;
    cur_plane_polytris = (uint16_t)polytris;
}
#endif

#ifdef PVS_PROBE
// Count-only latch for the PVS/occlusion-bake go/no-go measurement: how many
// subsectors this frame's BSP walk VISITED (= sscount, the survivors after the
// R_CheckBBox node prune) and how many of those the existing REJECT matrix would
// have culled as not-visible from the view sector. The REJECT cull rate is a
// conservative LOWER bound on what a true subsector PVS could cull. Accumulated
// per-frame in r_bsp.c R_Subsector; latched here at the SetCounts call site so
// the SetCounts path stays byte-identical when the flag is off.
void N64Bench_SetPvsCounts(int visited, int cullable)
{
    if (!loop_open)
        return;
    cur_pvs_visited  = (uint16_t)visited;
    cur_pvs_cullable = (uint16_t)cullable;
}
#endif

#ifdef BAKEFAN_PROBE
// Count-only latch for the native baked-leaf-fan go/no-go: the leaf-fan triangle
// total this frame's DRAWN subsector floors/ceilings WOULD emit under an offline
// bake ((numsegs - 2) clamp >=1 per visible plane), accumulated per-frame in
// r_bsp.c R_Subsector. Directly A/B-able against the runtime trapezoid tessellation
// (cur_plane_polytris). Latched at the SetCounts call site (r_main.c); separate
// setter so the SetCounts path stays byte-identical when the flag is off.
void N64Bench_SetBakefanTris(int tris)
{
    if (!loop_open)
        return;
    cur_bakefan_tris = (uint16_t)tris;
}
#endif

#ifdef DPLANES_PROBE
// Latch the three `planes` sub-bracket tick accumulators r_plane.c builds during
// R_DrawPlanes (lump-cache / run-fitter / un-projection), converting RAW CP0 ticks
// to us per frame. Called once per frame at the R_DrawPlanes return. The probe
// times the emit body in place but changes nothing it does, so the geometry
// fingerprint is unperturbed (verify the BENCH_RESULT line stays identical).
void N64Bench_SetDPlanes(uint32_t lump_tk, uint32_t fitter_tk, uint32_t unproj_tk,
                         uint32_t scan_cols, uint32_t nodes)
{
    if (!loop_open)
        return;
    cur_dpl_lump_us   = (uint32_t)TICKS_TO_US(lump_tk);
    cur_dpl_fitter_us = (uint32_t)TICKS_TO_US(fitter_tk);
    cur_dpl_unproj_us = (uint32_t)TICKS_TO_US(unproj_tk);
    cur_dpl_scan_cols = scan_cols;
    cur_dpl_nodes     = nodes;
}
#endif

#ifdef BSPWALK_PROBE
// Latch the `bsp_walk` sub-bracket tick accumulators (call-site CP0 brackets in
// r_bsp.c/r_segs.c/r_main.c), converting RAW CP0 ticks to us per frame. Called once
// per frame at the SetCounts call site (r_main.c). addline_net = R_AddLine MINUS the
// R_RenderSegLoop nested inside it (the SEG_RASTER column loop, already attributed to
// seg_rast -- subtract so this is the per-seg bsp_walk work, not the raster). Clamp >=0.
void N64Bench_SetBspWalk(uint32_t addline_tk, uint32_t segloop_tk, uint32_t checkbbox_tk,
                         uint32_t sprite_tk, uint32_t mesh_tk, uint32_t rspwait_tk,
                         uint32_t addline_calls)
{
    uint32_t addline_net = (addline_tk > segloop_tk) ? (addline_tk - segloop_tk) : 0;
    if (!loop_open)
        return;
    cur_bspw_addline_net_us = (uint32_t)TICKS_TO_US(addline_net);
    cur_bspw_segloop_us     = (uint32_t)TICKS_TO_US(segloop_tk);
    cur_bspw_checkbbox_us   = (uint32_t)TICKS_TO_US(checkbbox_tk);
    cur_bspw_sprite_us      = (uint32_t)TICKS_TO_US(sprite_tk);
    cur_bspw_mesh_us        = (uint32_t)TICKS_TO_US(mesh_tk);
    cur_bspw_rspwait_us     = (uint32_t)TICKS_TO_US(rspwait_tk);
    cur_bspw_addline_calls  = addline_calls;
}
#endif

void N64Bench_DisplayBegin(void)
{
    if (!loop_open)
        return;
    display_start_ticks = get_ticks();
}

void N64Bench_DisplayEnd(void)
{
    uint32_t display_tk, inside_tk;
    if (!loop_open)
        return;
    // All in raw ticks (no divide); HUD = display wall minus the in-view
    // render phases, the key-clear, and the present. BSP_WALK+SEG_RASTER
    // together replace the old single BSP phase. KEY_CLEAR is a standalone
    // bracket at the view-render entry (not nested in another subtracted
    // phase), so it is subtracted here -- otherwise its cost would leak into
    // HUD. PLANE_EMIT now carries the RDP wall-emit (run-coalesce +
    // DL_EmitRunPiece), bracketed in r_segs.c by SWITCHING OUT of BSP_WALK --
    // so it is DISJOINT from BSP_WALK (and does not run inside PLANES), and MUST
    // be subtracted here or the emit cost would leak into HUD. MASKED_EMIT stays
    // unused (sprites are not RDP-routed) and reads 0. DL_BUILD and RDP_BUSY are
    // DISJOINT from PRESENT: I_FinishUpdate PAUSES the PRESENT bracket around
    // DL_Flush and around the buffer-flip busy spin via PhaseSwitch (the Stage-3
    // attribution fix -- previously PRESENT contained DL_BUILD and the phase
    // table double-counted ~10 ms), so both must be subtracted here too.
    display_tk = (uint32_t)(get_ticks() - display_start_ticks);
    inside_tk = cur_phase_tk[BPH_BSP_WALK] + cur_phase_tk[BPH_SEG_RASTER]
              + cur_phase_tk[BPH_PLANES] + cur_phase_tk[BPH_MASKED]
              + cur_phase_tk[BPH_PLANE_EMIT] + cur_phase_tk[BPH_MASKED_EMIT]
              + cur_phase_tk[BPH_KEY_CLEAR] + cur_phase_tk[BPH_PRESENT]
              + cur_phase_tk[BPH_DL_BUILD] + cur_phase_tk[BPH_RDP_BUSY];
    cur_phase_tk[BPH_HUD] = (display_tk > inside_tk) ? (display_tk - inside_tk) : 0;
}

void N64Bench_LoopEnd(void)
{
    uint64_t elapsed;
    unsigned long total_us;
    int i;

    if (!loop_open || bench_phase == BENCH_DONE)
        return;
    loop_open = 0;

    elapsed  = get_ticks() - loop_start_ticks;
    total_us = (unsigned long)TICKS_TO_US(elapsed);

    if (bench_phase != BENCH_RUNNING)
        return;                 // warm-up frames not retained

    // Level-reload (death/respawn) frames are not render frames: count them
    // separately and keep them out of the per-phase/tail stats entirely.
    if (total_us >= BENCH_OUTLIER_US)
    {
        outlier_frames++;
        if (total_us > outlier_max_us)
            outlier_max_us = total_us;
        return;
    }

    if (bench_frame_count >= BENCH_MAX_FRAMES)
        return;                 // ring full (never reached for this scenario)

    {
        bench_frame_t* f = &bench_frames[bench_frame_count++];
        f->total_us = (uint32_t)total_us;
        // ticks -> us once per phase, here at commit (off the hot path).
        for (i = 0; i < BPH_COUNT; i++)
            f->phase_us[i] = (uint32_t)TICKS_TO_US(cur_phase_tk[i]);
        f->vissprites = cur_vissprites;
        f->drawsegs   = cur_drawsegs;
        f->visplanes  = cur_visplanes;
        {
            // RDP wall band LOAD_TILE count for this frame's present (CI4 lever
            // diagnostic). Weakly referenced so non-RDP builds link clean.
            extern uint32_t DL_TileLoadCount(void) __attribute__((weak));
            f->tile_loads = DL_TileLoadCount ?
                            (uint16_t)DL_TileLoadCount() : 0;
            // Sibling per-present primitive counters (records/uploads/tris).
            // Same weak-ref pattern so non-RDP builds link clean.
            extern uint32_t DL_RecCount(void)    __attribute__((weak));
            extern uint32_t DL_UploadCount(void) __attribute__((weak));
            extern uint32_t DL_TriCount(void)    __attribute__((weak));
            f->recs    = DL_RecCount    ? (uint16_t)DL_RecCount()    : 0;
            f->uploads = DL_UploadCount ? (uint16_t)DL_UploadCount() : 0;
            f->tris    = DL_TriCount    ? (uint16_t)DL_TriCount()    : 0;
        }
#ifdef PLANETESS_COUNT
        f->plane_polytris = cur_plane_polytris;
#endif
#ifdef PVS_PROBE
        f->pvs_visited  = cur_pvs_visited;
        f->pvs_cullable = cur_pvs_cullable;
#endif
#ifdef BAKEFAN_PROBE
        f->bakefan_tris = cur_bakefan_tris;
#endif
#ifdef DPLANES_PROBE
        f->dpl_lump_us   = cur_dpl_lump_us;
        f->dpl_fitter_us = cur_dpl_fitter_us;
        f->dpl_unproj_us = cur_dpl_unproj_us;
        f->dpl_scan_cols = cur_dpl_scan_cols;
        f->dpl_nodes     = cur_dpl_nodes;
#endif
#ifdef BSPWALK_PROBE
        // MEAN-ONLY running totals (no per-frame array -- see the bspw_*_sum note). Same
        // frame set as the official bench_frames commit (this block), so bspw_nframes
        // tracks bench_frame_count.
        bspw_addline_sum  += cur_bspw_addline_net_us;
        bspw_segloop_sum  += cur_bspw_segloop_us;
        bspw_checkbbox_sum+= cur_bspw_checkbbox_us;
        bspw_sprite_sum   += cur_bspw_sprite_us;
        bspw_mesh_sum     += cur_bspw_mesh_us;
        bspw_rspwait_sum  += cur_bspw_rspwait_us;
        bspw_calls_sum    += cur_bspw_addline_calls;
        bspw_nframes++;
        // Per-frame copy for the TAIL breakdown (the report's tail loop sums these).
        f->bspw_addline_net_us = (uint16_t)cur_bspw_addline_net_us;
        f->bspw_checkbbox_us   = (uint16_t)cur_bspw_checkbbox_us;
        f->bspw_sprite_us      = (uint16_t)cur_bspw_sprite_us;
        f->bspw_mesh_us        = (uint16_t)cur_bspw_mesh_us;
#endif
#ifdef RDPWAIT_PROBE
        for (i = 0; i < BPH_COUNT; i++)
            f->async_us[i] = (uint32_t)TICKS_TO_US(cur_async_tk[i]);
        f->async_fires   = (uint16_t)cur_async_fires;
        f->dispget_us    = (uint16_t)TICKS_TO_US(cur_dispget_tk);
        f->rdpbusy_spins = (uint16_t)cur_rdpbusy_spins;
#endif
        f->tics_ran   = (uint8_t)cur_tics_ran;
        f->is_outlier = 0;
    }

#if N64_BENCH_MARKS
    // Frame-keyed visual-capture markers (BENCH_MARKS=1 builds only): the
    // host capture loop greps these off the live ISViewer log and screenshots
    // on each one. Frame N is the same game state on every build (virtual tic
    // clock), so captures pair exactly across flag-on/flag-off ROMs. Never
    // enabled in timing builds -- the debugf cost would skew the numbers.
    // Every 128 retained frames (32 points/run): the user observed texture
    // artifacting falling between the original 256-frame samples.
    //
    // BENCH_MARK_FLASH: opt-in EXTRA markers on the off-grid death-flash detail
    // frames (3150/3160) so the red damage-flash band-fix A/B can pair them. Does
    // not alter the canonical 128-grid (those still fire), so a flash-marks ROM is
    // a strict superset of the canonical capture. Bench-only; no renderer effect.
    {
        int marker_hit = ((bench_frame_count & 127) == 0);
#if defined(BENCH_MARK_FLASH) && BENCH_MARK_FLASH
        if (bench_frame_count == 3150UL || bench_frame_count == 3160UL)
            marker_hit = 1;
#endif
#if defined(BENCH_MARK_VOID) && BENCH_MARK_VOID
        // OFF-GRID void diagnosis: the ~316 near-total-black frames found by the
        // full-demo counter are ALL off the 128-grid (doom-n64-capture-pitfalls #7).
        // Mark a few known-black ones so a capture can confirm the void on the
        // current build + pair RDP-walls/mesh/plane control builds at the SAME state.
        if (bench_frame_count == 837UL  || bench_frame_count == 1274UL ||
            bench_frame_count == 3482UL || bench_frame_count == 3213UL)
            marker_hit = 1;     // 3213 = the death->respawn melt-wipe (mesh-build black)
#endif
    if (marker_hit)
    {
        debugf("BENCH_MARK frame=%lu\n", bench_frame_count);

#if defined(BENCH_MARK_FBSCAN) && BENCH_MARK_FBSCAN
        // Displayed-framebuffer bbox scan (BENCH_MARK_FBSCAN=1): wait for the
        // VI to flip to the just-queued mark frame, then read the buffer the
        // VI is actually showing (VI_ORIGIN) and log the bounding box of
        // warm-bright pixels (fireball/explosion colours). Host-independent
        // stand-in for the screenshot bbox measurement -- works when no
        // display capture is possible. RGBA16 only (this port's display mode).
        {
            uint64_t settle = get_ticks() + TICKS_PER_SECOND / 5;
            while (get_ticks() < settle)
                ;   // let the VI flip; same no-VirtualTick freeze as the hold
        }
        {
            // Four equal-height bands, bbox+count each: a union bbox is
            // dominated by the status bar's warm numerals; banding isolates
            // view-area defects (e.g. a truncated explosion sprite).
            uint32_t origin = (*(volatile uint32_t*)0xA4400004u) & 0x00FFFFFFu;
            volatile uint16_t* fb = (volatile uint16_t*)(0xA0000000u | origin);
            int vw = (int)display_get_width(), vh = (int)display_get_height();
            int x, y, band;
            for (band = 0; band < 4; band++)
            {
                int yb0 = band * vh / 4, yb1 = (band + 1) * vh / 4;
                int n = 0, x0 = 9999, x1 = -1, y0 = 9999, y1 = -1;
                for (y = yb0; y < yb1; y++)
                    for (x = 0; x < vw; x++)
                    {
                        uint16_t px = fb[y * vw + x];
                        int r = (px >> 11) & 31, g = (px >> 6) & 31, b = (px >> 1) & 31;
                        if (r >= 23 && g >= 10 && b < 17)
                        {
                            n++;
                            if (x < x0) x0 = x;
                            if (x > x1) x1 = x;
                            if (y < y0) y0 = y;
                            if (y > y1) y1 = y;
                        }
                    }
                debugf("BENCH_FBSCAN frame=%lu band=%d n=%d x=%d..%d y=%d..%d\n",
                       bench_frame_count, band, n, x0, x1, y0, y1);
            }
        }
        {
            // Raw z16 probe (same flag): two rows across the view centre. At a
            // truncated-sprite pixel the value IS the occluder's depth; compared
            // against a neighbouring drawn pixel it identifies what z-killed it.
            extern byte* i_n64_zbuf_base;
            extern int   i_n64_zbuf_w;
            if (i_n64_zbuf_base)
            {
                volatile uint16_t* zb = (volatile uint16_t*)
                    (0xA0000000u | ((uint32_t)(uintptr_t)i_n64_zbuf_base & 0x1FFFFFFFu));
                static const int rows[2] = { 60, 80 };
                int ri, x;
                for (ri = 0; ri < 2; ri++)
                {
                    char lbuf[200];
                    int  o = sprintf(lbuf, "BENCH_ZPROBE frame=%lu y=%d z=",
                                     bench_frame_count, rows[ri]);
                    for (x = 144; x <= 208; x += 4)
                        o += sprintf(lbuf + o, "%04x,",
                                     zb[rows[ri] * i_n64_zbuf_w + x]);
                    debugf("%s\n", lbuf);
                }
            }
        }
#endif

        // FREEZE-AT-MARKER: hold ~2 wall-clock seconds before returning to
        // the loop, so the screen keeps showing EXACTLY the marker frame
        // while the host capture loop (grep marker -> grim) fires. Without
        // the hold, captures land frames late with host-side jitter, so
        // paired "frame N" stills were only tic-approximate -- the capture-
        // methodology hole that produced false texture-defect evidence.
        // Determinism is untouched: the virtual tic clock advances on
        // N64Bench_VirtualTick (per render-loop iteration / per present),
        // never on wall time, so stalling here freezes the game state with
        // the presented frame. Marks-build timing is already meaningless
        // (the debugf traffic skews it), so the stall is free. get_ticks()
        // is emulated CP0 time; ares runs the bench at full speed, so 2
        // emulated seconds == ~2 host seconds for the capture window.
        {
            uint64_t hold_until = get_ticks()
                                + (uint64_t)TICKS_PER_SECOND * 2;
            while (get_ticks() < hold_until)
                ;   // spin: no VirtualTick, no present, state frozen
        }
    }
    }
#endif
}

#if defined(BENCH_WIPE_FREEZE)
// Capture hook for the death->respawn MELT WIPE. The wipe runs in d_main.c's melt
// loop (D_Display), which bypasses N64Bench_CommitFrame -- so the normal frame
// markers never reach it and a frozen-marker capture lands on the pre-wipe frame,
// not the melt. WipeStart resets the per-wipe step counter; WipeFreezeMaybe is
// called once per melt present and, on a few chosen melt steps, emits a BENCH_MARK
// SENTINEL (9208/9216/9224 = early/mid/late melt) then holds ~2 wall-clock seconds
// so scan-marks captures the held mid-melt frame. The demo's death-respawn wipe is
// the LAST wipe, so its frames overwrite the earlier demo-start wipe's same
// sentinels -> frame-92NN.png is the death-respawn melt. No VirtualTick during the
// hold, so the melt state is frozen (same mechanism as the marker freeze above).
static int bench_wf_iter = 0;
void N64Bench_WipeStart(void) { bench_wf_iter = 0; }
void N64Bench_WipeFreezeMaybe(void)
{
    bench_wf_iter++;
    if (bench_wf_iter == 8 || bench_wf_iter == 16 || bench_wf_iter == 24)
    {
        uint64_t hold_until;
        debugf("BENCH_MARK frame=%d\n", 9200 + bench_wf_iter);
        hold_until = get_ticks() + (uint64_t)TICKS_PER_SECOND * 2;
        while (get_ticks() < hold_until)
            ;   // spin: no VirtualTick, no melt advance, present frozen
    }
}
#endif

void N64Bench_FillTiccmd(ticcmd_t* cmd)
{
    const bench_step_t* step;

    if (!bench_started)
        return;

    memset(cmd, 0, sizeof(*cmd));

    if (bench_phase == BENCH_DONE)
        return;                 // hold still after the run completes

    step = &bench_script[(bench_tic_index / BENCH_STEP_TICS) % BENCH_SCRIPT_STEPS];
    cmd->forwardmove = step->forward;
    cmd->sidemove    = step->side;
    cmd->angleturn   = step->turn;
    cmd->buttons     = step->buttons;
}

// MP variant: movement fields only, so the caller's consistancy assignment
// stays intact. Phase-shifting the table per player keeps the panes on
// different geometry while staying deterministic; the step offset of 3 is
// coprime with the 10-step table, giving players 0-3 distinct phases
// (0, 3, 6, 9).
void N64Bench_FillTiccmdMP(ticcmd_t* cmd, int playernum)
{
    const bench_step_t* step;

    if (!bench_started)
        return;

    cmd->forwardmove = 0;
    cmd->sidemove    = 0;
    cmd->angleturn   = 0;
    cmd->buttons     = 0;

    if (bench_phase == BENCH_DONE)
        return;                 // hold still after the run completes

    step = &bench_script[(bench_tic_index / BENCH_STEP_TICS
                          + (unsigned)playernum * 3)
                         % BENCH_SCRIPT_STEPS];
    cmd->forwardmove = step->forward;
    cmd->sidemove    = step->side;
    cmd->angleturn   = step->turn;
    cmd->buttons     = step->buttons;
}

void N64Bench_NoteInterp(int local_players)
{
    static int noted;

    if (noted || !bench_started)
        return;
    noted = 1;
    debugf("BENCH: interpolation ACTIVE, local_players=%d\n", local_players);
}

// Called once per gametic from G_Ticker (via the script counter in FillTiccmd
// advancing) -- we drive phase transitions off the gametic count so timing is
// in emulated game time, not host or rendered-frame time.
static void N64Bench_AdvancePhase(void)
{
    bench_tic_index++;

    if (bench_phase == BENCH_WARMUP && bench_tic_index >= BENCH_WARMUP_GAMETICS)
    {
        bench_phase = BENCH_RUNNING;
        debugf("BENCH: warmup done, collecting at gametic %d\n", bench_tic_index);
    }
    else if (bench_phase == BENCH_RUNNING && bench_tic_index >= BENCH_GAMETICS)
    {
        N64Bench_Finish();
    }
}

static unsigned long N64Bench_Percentile(int pct)
{
    unsigned long target;
    unsigned long cum;
    int i;

    if (!rendered_frames)
        return 0;
    target = (rendered_frames * (unsigned long)pct + 99) / 100;
    cum = 0;
    for (i = 0; i < BENCH_HIST_BUCKETS; i++)
    {
        cum += hist[i];
        if (cum >= target)
            return ((unsigned long)i << BENCH_HIST_US_SHIFT)
                   + (1UL << (BENCH_HIST_US_SHIFT - 1));   // bucket midpoint
    }
    return (unsigned long)BENCH_HIST_OVERFLOW << BENCH_HIST_US_SHIFT;
}

static const char* const bench_phase_name[BPH_COUNT] =
{
    "gametic", "bsp_walk", "seg_rast", "planes", "masked",
    "plnemit", "mskemit", "dlbuild", "rdpbusy", "keyclr",
    "hud", "present", "audio"
};

// p95 of a uint32 field over the retained (non-outlier) frames, via the same
// 64-us-bucket histogram trick used for the frame total. `phase` < 0 selects
// the frame total; otherwise it selects phase_us[phase].
static unsigned long N64Bench_FieldP95(int phase)
{
    static unsigned long fhist[BENCH_HIST_BUCKETS];
    unsigned long i, target, cum;

    if (!bench_frame_count)
        return 0;
    memset(fhist, 0, sizeof(fhist));
    for (i = 0; i < bench_frame_count; i++)
    {
        unsigned long v = (phase < 0) ? bench_frames[i].total_us
                                      : bench_frames[i].phase_us[phase];
        unsigned long b = v >> BENCH_HIST_US_SHIFT;
        if (b >= BENCH_HIST_BUCKETS) b = BENCH_HIST_OVERFLOW;
        fhist[b]++;
    }
    target = (bench_frame_count * 95UL + 99) / 100;
    cum = 0;
    for (i = 0; i < BENCH_HIST_BUCKETS; i++)
    {
        cum += fhist[i];
        if (cum >= target)
            return (i << BENCH_HIST_US_SHIFT) + (1UL << (BENCH_HIST_US_SHIFT - 1));
    }
    return (unsigned long)BENCH_HIST_OVERFLOW << BENCH_HIST_US_SHIFT;
}

#ifdef DPLANES_PROBE
// p95 of one `planes` sub-bracket us field over the retained frames, same 64-us
// histogram trick as N64Bench_FieldP95. sel: 0=lump, 1=fitter, 2=unproj.
static unsigned long N64Bench_DPlanesP95(int sel)
{
    static unsigned long fhist[BENCH_HIST_BUCKETS];
    unsigned long i, target, cum;

    if (!bench_frame_count)
        return 0;
    memset(fhist, 0, sizeof(fhist));
    for (i = 0; i < bench_frame_count; i++)
    {
        unsigned long v = (sel == 0) ? bench_frames[i].dpl_lump_us
                        : (sel == 1) ? bench_frames[i].dpl_fitter_us
                                     : bench_frames[i].dpl_unproj_us;
        unsigned long b = v >> BENCH_HIST_US_SHIFT;
        if (b >= BENCH_HIST_BUCKETS) b = BENCH_HIST_OVERFLOW;
        fhist[b]++;
    }
    target = (bench_frame_count * 95UL + 99) / 100;
    cum = 0;
    for (i = 0; i < BENCH_HIST_BUCKETS; i++)
    {
        cum += fhist[i];
        if (cum >= target)
            return (i << BENCH_HIST_US_SHIFT) + (1UL << (BENCH_HIST_US_SHIFT - 1));
    }
    return (unsigned long)BENCH_HIST_OVERFLOW << BENCH_HIST_US_SHIFT;
}
#endif

#ifdef RDPWAIT_PROBE
// p95 of the async RDP-completion-interrupt us, same 64us-bucket histogram trick
// as N64Bench_FieldP95. `phase` < 0 selects the per-frame async TOTAL (sum over
// all phases); otherwise async_us[phase] (the interrupt time charged to that
// phase). 32us return (one bucket midpoint) means the interrupt never landed in
// that phase across the run.
static unsigned long N64Bench_AsyncFieldP95(int phase)
{
    static unsigned long fhist[BENCH_HIST_BUCKETS];
    unsigned long i, target, cum;

    if (!bench_frame_count)
        return 0;
    memset(fhist, 0, sizeof(fhist));
    for (i = 0; i < bench_frame_count; i++)
    {
        unsigned long v;
        if (phase < 0)
        {
            int pp; v = 0;
            for (pp = 0; pp < BPH_COUNT; pp++) v += bench_frames[i].async_us[pp];
        }
        else
            v = bench_frames[i].async_us[phase];
        {
            unsigned long b = v >> BENCH_HIST_US_SHIFT;
            if (b >= BENCH_HIST_BUCKETS) b = BENCH_HIST_OVERFLOW;
            fhist[b]++;
        }
    }
    target = (bench_frame_count * 95UL + 99) / 100;
    cum = 0;
    for (i = 0; i < BENCH_HIST_BUCKETS; i++)
    {
        cum += fhist[i];
        if (cum >= target)
            return (i << BENCH_HIST_US_SHIFT) + (1UL << (BENCH_HIST_US_SHIFT - 1));
    }
    return (unsigned long)BENCH_HIST_OVERFLOW << BENCH_HIST_US_SHIFT;
}
#endif

#ifdef PLANETESS_COUNT
// p95 of the per-frame plane-poly-tri count (count-only go/no-go measurement).
// Triangle counts are small integers, so this buckets the value DIRECTLY (one
// bucket per tri count -- no quantization), giving an EXACT p95 rather than the
// 64us-bucketed estimate N64Bench_FieldP95 uses for microsecond timings. Counts
// far beyond BENCH_HIST_BUCKETS (4096) clamp to the overflow bucket, which the
// E1M1 plane geometry never approaches.
static unsigned long N64Bench_PlanePolyTrisP95(void)
{
    static unsigned long fhist[BENCH_HIST_BUCKETS];
    unsigned long i, target, cum;

    if (!bench_frame_count)
        return 0;
    memset(fhist, 0, sizeof(fhist));
    for (i = 0; i < bench_frame_count; i++)
    {
        unsigned long b = bench_frames[i].plane_polytris;
        if (b >= BENCH_HIST_BUCKETS) b = BENCH_HIST_OVERFLOW;
        fhist[b]++;
    }
    target = (bench_frame_count * 95UL + 99) / 100;
    cum = 0;
    for (i = 0; i < BENCH_HIST_BUCKETS; i++)
    {
        cum += fhist[i];
        if (cum >= target)
            return i;       // exact tri count at the 95th percentile frame
    }
    return BENCH_HIST_OVERFLOW;
}
#endif

#ifdef BAKEFAN_PROBE
// EXACT p95 of the per-frame baked-leaf-fan triangle count (count-only go/no-go
// for the native offline-baked RDP renderer). Mirrors N64Bench_PlanePolyTrisP95:
// counts are small integers, so bucket the value DIRECTLY (one bucket per tri
// count, no quantization) for an exact p95. Counts beyond BENCH_HIST_BUCKETS
// (4096) clamp to the overflow bucket -- the E1M1 subsector fan totals never
// approach it.
static unsigned long N64Bench_BakefanTrisP95(void)
{
    static unsigned long fhist[BENCH_HIST_BUCKETS];
    unsigned long i, target, cum;

    if (!bench_frame_count)
        return 0;
    memset(fhist, 0, sizeof(fhist));
    for (i = 0; i < bench_frame_count; i++)
    {
        unsigned long b = bench_frames[i].bakefan_tris;
        if (b >= BENCH_HIST_BUCKETS) b = BENCH_HIST_OVERFLOW;
        fhist[b]++;
    }
    target = (bench_frame_count * 95UL + 99) / 100;
    cum = 0;
    for (i = 0; i < BENCH_HIST_BUCKETS; i++)
    {
        cum += fhist[i];
        if (cum >= target)
            return i;       // exact fan-tri count at the 95th percentile frame
    }
    return BENCH_HIST_OVERFLOW;
}
#endif

#ifdef PVS_PROBE
// EXACT p95 of the per-frame REJECT-cullable subsector count (count-only
// PVS/occlusion go/no-go). Mirrors N64Bench_PlanePolyTrisP95: counts are small
// integers, so bucket the value directly (no quantization) for an exact p95.
static unsigned long N64Bench_PvsCullableP95(void)
{
    static unsigned long fhist[BENCH_HIST_BUCKETS];
    unsigned long i, target, cum;

    if (!bench_frame_count)
        return 0;
    memset(fhist, 0, sizeof(fhist));
    for (i = 0; i < bench_frame_count; i++)
    {
        unsigned long b = bench_frames[i].pvs_cullable;
        if (b >= BENCH_HIST_BUCKETS) b = BENCH_HIST_OVERFLOW;
        fhist[b]++;
    }
    target = (bench_frame_count * 95UL + 99) / 100;
    cum = 0;
    for (i = 0; i < BENCH_HIST_BUCKETS; i++)
    {
        cum += fhist[i];
        if (cum >= target)
            return i;       // exact cullable count at the 95th percentile frame
    }
    return BENCH_HIST_OVERFLOW;
}

// p95-TAIL cull PERCENTAGE: the highest-cull-rate frames are the ones a PVS bake
// would help most, so this scans the per-frame cull-pct (cullable*1000/visited,
// tenths of a percent) histogram from the TOP and returns the worst 5% threshold
// -- i.e. the cull rate at/above which the best-5% of frames cull. This is the
// number the go/no-go gate reads (>250 tenths = >25% -> headroom; <100 = NO-GO).
// Returns tenths of a percent. Direct integer buckets (0..1000), no quantization.
static unsigned long N64Bench_PvsCullPctP95Tail(void)
{
    static unsigned long fhist[1001];   // 0..1000 tenths of a percent
    unsigned long i, target, cum;

    if (!bench_frame_count)
        return 0;
    memset(fhist, 0, sizeof(fhist));
    for (i = 0; i < bench_frame_count; i++)
    {
        unsigned long v = bench_frames[i].pvs_visited;
        unsigned long pct10 = v ?
            ((unsigned long)bench_frames[i].pvs_cullable * 1000UL) / v : 0;
        if (pct10 > 1000) pct10 = 1000;
        fhist[pct10]++;
    }
    // worst 5% of frames by cull-pct: cumulative from the top.
    target = (bench_frame_count * 5UL + 99) / 100;
    cum = 0;
    for (i = 1001; i-- > 0; )
    {
        cum += fhist[i];
        if (cum >= target)
            return i;       // tenths-of-percent threshold for the best-culling 5%
    }
    return 0;
}
#endif

// us threshold at/above which a frame is in the worst BENCH_TAIL_PCT, found by
// scanning the frame-total histogram from the top.
static unsigned long N64Bench_TailThreshold(unsigned long* out_tail_target)
{
    static unsigned long fhist[BENCH_HIST_BUCKETS];
    unsigned long i, target, cum;

    memset(fhist, 0, sizeof(fhist));
    for (i = 0; i < bench_frame_count; i++)
    {
        unsigned long b = bench_frames[i].total_us >> BENCH_HIST_US_SHIFT;
        if (b >= BENCH_HIST_BUCKETS) b = BENCH_HIST_OVERFLOW;
        fhist[b]++;
    }
    // Want the worst TAIL_PCT% of frames: cumulative from the top.
    target = (bench_frame_count * (unsigned long)BENCH_TAIL_PCT + 99) / 100;
    if (out_tail_target) *out_tail_target = target;
    cum = 0;
    for (i = BENCH_HIST_BUCKETS; i-- > 0; )
    {
        cum += fhist[i];
        if (cum >= target)
            return (i << BENCH_HIST_US_SHIFT);   // bucket floor (inclusive)
    }
    return 0;
}

static void N64Bench_ReportPhases(void)
{
    unsigned long i;
    int p;
    unsigned long long phase_sum[BPH_COUNT];
    unsigned long long total_sum = 0;
    unsigned long long leftover_sum = 0;
    unsigned long long vissprite_sum = 0, drawseg_sum = 0, visplane_sum = 0;
    unsigned long long tileload_sum = 0;
    unsigned long long rec_sum = 0, upload_sum = 0, tri_sum = 0;
#ifdef PLANETESS_COUNT
    unsigned long long plane_polytris_sum = 0;
#endif
#ifdef BAKEFAN_PROBE
    unsigned long long bakefan_tris_sum = 0;
#endif
#ifdef PVS_PROBE
    unsigned long long pvs_visited_sum = 0, pvs_cullable_sum = 0;
#endif
#ifdef DPLANES_PROBE
    unsigned long long dpl_lump_sum = 0, dpl_fitter_sum = 0, dpl_unproj_sum = 0;
    unsigned long long dpl_scan_cols_sum = 0, dpl_nodes_sum = 0;
#endif
    unsigned long tics_frames = 0;

    unsigned long tail_thresh, tail_target, tail_n = 0;
    unsigned long long t_phase_sum[BPH_COUNT];
    unsigned long long t_total_sum = 0, t_leftover_sum = 0;
    unsigned long long t_viss_sum = 0, t_ds_sum = 0, t_vp_sum = 0;
    unsigned long t_tics_frames = 0;
#ifdef BSPWALK_PROBE
    unsigned long long t_bspw_addline = 0, t_bspw_checkbbox = 0, t_bspw_sprite = 0, t_bspw_mesh = 0;
#endif

    // worst-N frames by total (indices), simple insertion sort, descending.
    unsigned long worst_idx[BENCH_WORST_N];
    unsigned long worst_us[BENCH_WORST_N];
    int worst_count = 0;

    debugf("BENCH_REPORT_BEGIN frames=%lu\n", bench_frame_count);

    if (!bench_frame_count)
    {
        debugf("BENCH_PHASE: no retained frames\n");
        return;
    }

    for (p = 0; p < BPH_COUNT; p++) { phase_sum[p] = 0; t_phase_sum[p] = 0; }

    tail_thresh = N64Bench_TailThreshold(&tail_target);

    for (i = 0; i < bench_frame_count; i++)
    {
        bench_frame_t* f = &bench_frames[i];
        unsigned long long acc = 0;
        unsigned long long leftover;

        total_sum += f->total_us;
        for (p = 0; p < BPH_COUNT; p++)
        {
            phase_sum[p] += f->phase_us[p];
            acc += f->phase_us[p];
        }
        leftover = (f->total_us > acc) ? (f->total_us - acc) : 0;
        leftover_sum += leftover;
        vissprite_sum += f->vissprites;
        drawseg_sum   += f->drawsegs;
        visplane_sum  += f->visplanes;
        tileload_sum  += f->tile_loads;
        rec_sum       += f->recs;
        upload_sum    += f->uploads;
        tri_sum       += f->tris;
#ifdef PLANETESS_COUNT
        plane_polytris_sum += f->plane_polytris;
#endif
#ifdef BAKEFAN_PROBE
        bakefan_tris_sum += f->bakefan_tris;
#endif
#ifdef PVS_PROBE
        pvs_visited_sum  += f->pvs_visited;
        pvs_cullable_sum += f->pvs_cullable;
#endif
#ifdef DPLANES_PROBE
        dpl_lump_sum   += f->dpl_lump_us;
        dpl_fitter_sum += f->dpl_fitter_us;
        dpl_unproj_sum += f->dpl_unproj_us;
        dpl_scan_cols_sum += f->dpl_scan_cols;
        dpl_nodes_sum     += f->dpl_nodes;
#endif
        if (f->tics_ran) tics_frames++;

        // tail accumulation
        if (f->total_us >= tail_thresh)
        {
            tail_n++;
            t_total_sum += f->total_us;
            for (p = 0; p < BPH_COUNT; p++) t_phase_sum[p] += f->phase_us[p];
            t_leftover_sum += leftover;
#ifdef BSPWALK_PROBE
            t_bspw_addline  += f->bspw_addline_net_us;
            t_bspw_checkbbox+= f->bspw_checkbbox_us;
            t_bspw_sprite   += f->bspw_sprite_us;
            t_bspw_mesh     += f->bspw_mesh_us;
#endif
            t_viss_sum += f->vissprites;
            t_ds_sum   += f->drawsegs;
            t_vp_sum   += f->visplanes;
            if (f->tics_ran) t_tics_frames++;
        }

        // worst-N by total
        if (worst_count < BENCH_WORST_N || f->total_us > worst_us[worst_count-1])
        {
            int j = (worst_count < BENCH_WORST_N) ? worst_count++ : (BENCH_WORST_N - 1);
            while (j > 0 && worst_us[j-1] < f->total_us)
            {
                worst_us[j]  = worst_us[j-1];
                worst_idx[j] = worst_idx[j-1];
                j--;
            }
            worst_us[j]  = f->total_us;
            worst_idx[j] = i;
        }
    }

    // --- overall per-phase table -----------------------------------------
    debugf("BENCH_PHASE_HDR frames=%lu mean_total_us=%lu p95_total_us=%lu "
           "tic_frames=%lu (out of %lu)\n",
           bench_frame_count,
           (unsigned long)(total_sum / bench_frame_count),
           N64Bench_FieldP95(-1),
           tics_frames, bench_frame_count);
    for (p = 0; p < BPH_COUNT; p++)
    {
        unsigned long mean = (unsigned long)(phase_sum[p] / bench_frame_count);
        unsigned long pct10 = total_sum ?
            (unsigned long)((phase_sum[p] * 1000ULL) / total_sum) : 0;
        debugf("BENCH_PHASE name=%-8s mean_us=%lu pct=%lu.%lu p95_us=%lu\n",
               bench_phase_name[p], mean, pct10 / 10, pct10 % 10,
               N64Bench_FieldP95(p));
    }
    {
        unsigned long mean = (unsigned long)(leftover_sum / bench_frame_count);
        unsigned long pct10 = total_sum ?
            (unsigned long)((leftover_sum * 1000ULL) / total_sum) : 0;
        debugf("BENCH_PHASE name=%-8s mean_us=%lu pct=%lu.%lu\n",
               "leftover", mean, pct10 / 10, pct10 % 10);
    }
    debugf("BENCH_COUNTS mean_vissprites=%lu mean_drawsegs=%lu mean_visplanes=%lu "
           "mean_recs=%lu mean_uploads=%lu mean_tris=%lu mean_tile_loads=%lu\n",
           (unsigned long)(vissprite_sum / bench_frame_count),
           (unsigned long)(drawseg_sum / bench_frame_count),
           (unsigned long)(visplane_sum / bench_frame_count),
           (unsigned long)(rec_sum / bench_frame_count),
           (unsigned long)(upload_sum / bench_frame_count),
           (unsigned long)(tri_sum / bench_frame_count),
           (unsigned long)(tileload_sum / bench_frame_count));
#ifdef PLANETESS_COUNT
    // DECISIVE go/no-go line for "visplanes as RDP polygons": mean + EXACT p95
    // of the per-frame triangle count the frame's visplanes WOULD tessellate
    // into as RDP trapezoid strips (count-only; reuses the wall split predicate
    // DL_SPLIT_DEVY ~= 1.25 rows). Verdict gate: mean < ~120 and p95 < ~250 = GO;
    // p95 > ~300 = NO-GO.
    debugf("BENCH_PLANETESS mean_plane_polytris=%lu p95_plane_polytris=%lu\n",
           (unsigned long)(plane_polytris_sum / bench_frame_count),
           N64Bench_PlanePolyTrisP95());
#endif
#ifdef DPLANES_PROBE
    // Sub-bracket of the `planes` BPH bracket: mean + EXACT p95 us of the three
    // constituents (lump-cache / run-fitter / corner un-projection). The three
    // means sum to ~the `planes` mean (modulo the get_ticks read overhead the probe
    // adds). Read it directly against the BENCH_PHASE name=planes line to decide
    // which sub-part to attack. fitter is the run-fitter MINUS the un-projection it
    // calls (un-projection is timed and subtracted in r_plane.c).
    debugf("BENCH_DPLANES lump_mean=%lu lump_p95=%lu fitter_mean=%lu fitter_p95=%lu "
           "unproj_mean=%lu unproj_p95=%lu scan_cols_mean=%lu nodes_mean=%lu\n",
           (unsigned long)(dpl_lump_sum   / bench_frame_count), N64Bench_DPlanesP95(0),
           (unsigned long)(dpl_fitter_sum / bench_frame_count), N64Bench_DPlanesP95(1),
           (unsigned long)(dpl_unproj_sum / bench_frame_count), N64Bench_DPlanesP95(2),
           (unsigned long)(dpl_scan_cols_sum / bench_frame_count),
           (unsigned long)(dpl_nodes_sum     / bench_frame_count));
#endif
#ifdef BSPWALK_PROBE
    // Sub-bracket of the `bsp_walk` BPH bracket: mean + EXACT p95 us per constituent.
    // addline_net (the per-seg BSP-walk-as-visibility + seg setup, EXCLUDING the
    // R_RenderSegLoop raster nested inside it) + checkbbox + sprite + mesh ~= the
    // BENCH_PHASE name=bsp_walk mean (residual = R_FindPlane + recursion glue +
    // get_ticks overhead). segloop is shown for validation (it is attributed to
    // seg_rast, NOT bsp_walk). addline_calls = R_AddLine entries (reject-ratio vs
    // drawsegs). Read directly against the bsp_walk BENCH_PHASE/BENCH_TAIL lines.
    if (bspw_nframes)
    debugf("BENCH_BSPWALK addline_mean=%lu checkbbox_mean=%lu sprite_mean=%lu mesh_mean=%lu "
           "mesh_rspwait_mean=%lu segloop_mean=%lu addline_calls_mean=%lu nframes=%lu\n",
           (unsigned long)(bspw_addline_sum  / bspw_nframes),
           (unsigned long)(bspw_checkbbox_sum / bspw_nframes),
           (unsigned long)(bspw_sprite_sum   / bspw_nframes),
           (unsigned long)(bspw_mesh_sum     / bspw_nframes),
           (unsigned long)(bspw_rspwait_sum  / bspw_nframes),
           (unsigned long)(bspw_segloop_sum  / bspw_nframes),
           (unsigned long)(bspw_calls_sum    / bspw_nframes),
           bspw_nframes);
    // TAIL breakdown (worst-5% frames by total) -- the proportions that actually matter,
    // since the mean is vsync-bound. mesh's RSP-wait is ~constant; addline/checkbbox/sprite
    // scale with the geometry that grows in the tail, so the split flips vs the mean.
    if (tail_n)
    debugf("BENCH_BSPWALK_TAIL addline=%lu checkbbox=%lu sprite=%lu mesh=%lu tail_n=%lu\n",
           (unsigned long)(t_bspw_addline  / tail_n),
           (unsigned long)(t_bspw_checkbbox / tail_n),
           (unsigned long)(t_bspw_sprite   / tail_n),
           (unsigned long)(t_bspw_mesh     / tail_n),
           tail_n);
#endif
#ifdef BAKEFAN_PROBE
    // DECISIVE go/no-go for the native offline-baked RDP renderer (per-subsector
    // floor/ceiling LEAF FANS vs runtime visplane trapezoid tessellation): mean +
    // EXACT p95 of the per-frame leaf-fan triangle count a bake WOULD emit
    // ((numsegs-2) clamp >=1 per DRAWN floor/ceiling). Read it directly against
    // the BENCH_PLANETESS line above (the runtime trapezoid-run tris the bake
    // replaces): FEWER here -> the bake's RSP triangle-setup VOLUME case holds;
    // MORE -> per-subsector fans cost more setup than the merged trapezoid runs.
    debugf("BENCH_BAKEFAN mean_tris=%lu p95_tris=%lu\n",
           (unsigned long)(bakefan_tris_sum / bench_frame_count),
           N64Bench_BakefanTrisP95());
#endif
#ifdef PVS_PROBE
    // DECISIVE go/no-go for the PVS/occlusion bake: of the subsectors the BSP
    // walk VISITS (survivors of the existing R_CheckBBox node prune + 1-D
    // solidsegs occlusion), what fraction would the existing sector-granular
    // REJECT matrix have culled from the view sector. REJECT is coarser than a
    // true subsector PVS, so this cull rate is a conservative LOWER bound on PVS
    // headroom. cull_pct_mean = whole-run cullable/visited; cull_pct_p95tail =
    // the cull rate at the best-culling 5% of frames (tenths of a percent).
    // Gate: p95tail > 25.0% -> real headroom, build the bake; < 10.0% -> NO-GO.
    {
        unsigned long mean_vis = (unsigned long)(pvs_visited_sum / bench_frame_count);
        unsigned long mean_cul = (unsigned long)(pvs_cullable_sum / bench_frame_count);
        unsigned long pct_mean10 = pvs_visited_sum ?
            (unsigned long)((pvs_cullable_sum * 1000ULL) / pvs_visited_sum) : 0;
        unsigned long pct_tail10 = N64Bench_PvsCullPctP95Tail();
        debugf("BENCH_PVS mean_visited=%lu mean_cullable=%lu p95_cullable=%lu "
               "cull_pct_mean=%lu.%lu cull_pct_p95tail=%lu.%lu\n",
               mean_vis, mean_cul, N64Bench_PvsCullableP95(),
               pct_mean10 / 10, pct_mean10 % 10,
               pct_tail10 / 10, pct_tail10 % 10);
    }
#endif
#ifdef RDPWAIT_PROBE
    // DECISIVE async-stall attribution: where the RDP-completion interrupt
    // (I_N64BufferDone, fired from the DP SYNC_FULL interrupt via rdpq_detach_cb)
    // charges its wall-clock service time. The interrupt runs in whichever BPH_*
    // bracket is open at fire time. If the per-phase async_us here accounts for a
    // phase's "phantom" cost (a bracket time that survives a body no-op -- e.g.
    // the ~1644us planes residual), that phase is NOT CPU work: it is the CPU
    // stalled in an async DP-completion interrupt charged to whatever was open.
    //
    // dispget_us: display_get() wall time -- the vsync-coupled wait for a free
    // framebuffer (a HARD serialization if it blocks; a SOFT ~0 if buffers free).
    // rdpbusy_spins: iterations of the buffer-flip RDP-busy spin (the only CPU
    // spin-on-RDP-completion); ~0 confirms the CPU never locks on the RDP there.
    {
        unsigned long async_sum[BPH_COUNT];
        unsigned long fires_sum = 0, dispget_sum = 0, spins_sum = 0;
        unsigned long async_total = 0;
        unsigned long ii;
        memset(async_sum, 0, sizeof(async_sum));
        for (ii = 0; ii < bench_frame_count; ii++)
        {
            bench_frame_t* f = &bench_frames[ii];
            int pp;
            for (pp = 0; pp < BPH_COUNT; pp++)
            {
                async_sum[pp] += f->async_us[pp];
                async_total   += f->async_us[pp];
            }
            fires_sum   += f->async_fires;
            dispget_sum += f->dispget_us;
            spins_sum   += f->rdpbusy_spins;
        }
        debugf("BENCH_ASYNC_HDR frames=%lu total_async_us_mean=%lu fires_mean=%lu.%lu "
               "dispget_us_mean=%lu p95=%lu spins_mean=%lu.%lu\n",
               bench_frame_count,
               (unsigned long)(async_total / bench_frame_count),
               (fires_sum * 10UL / bench_frame_count) / 10,
               (fires_sum * 10UL / bench_frame_count) % 10,
               (unsigned long)(dispget_sum / bench_frame_count),
               N64Bench_AsyncFieldP95(-1),
               (spins_sum * 10UL / bench_frame_count) / 10,
               (spins_sum * 10UL / bench_frame_count) % 10);
        for (p = 0; p < BPH_COUNT; p++)
        {
            unsigned long mean = (unsigned long)(async_sum[p] / bench_frame_count);
            if (mean == 0 && N64Bench_AsyncFieldP95(p) == 32)
                continue;   // skip phases the interrupt never lands in
            debugf("BENCH_ASYNC name=%-8s mean_us=%lu p95_us=%lu\n",
                   bench_phase_name[p], mean, N64Bench_AsyncFieldP95(p));
        }
    }
#endif

    // --- tail report (worst BENCH_TAIL_PCT%) ------------------------------
    if (tail_n)
    {
        debugf("BENCH_TAIL_HDR tail_pct=%d frames=%lu thresh_us=%lu "
               "mean_total_us=%lu tic_frames=%lu\n",
               BENCH_TAIL_PCT, tail_n, tail_thresh,
               (unsigned long)(t_total_sum / tail_n), t_tics_frames);
        for (p = 0; p < BPH_COUNT; p++)
        {
            unsigned long mean = (unsigned long)(t_phase_sum[p] / tail_n);
            unsigned long pct10 = t_total_sum ?
                (unsigned long)((t_phase_sum[p] * 1000ULL) / t_total_sum) : 0;
            debugf("BENCH_TAIL name=%-8s mean_us=%lu pct=%lu.%lu\n",
                   bench_phase_name[p], mean, pct10 / 10, pct10 % 10);
        }
        debugf("BENCH_TAIL name=%-8s mean_us=%lu pct=%lu.%lu\n", "leftover",
               (unsigned long)(t_leftover_sum / tail_n),
               t_total_sum ? (unsigned long)((t_leftover_sum * 1000ULL) / t_total_sum) / 10 : 0,
               t_total_sum ? (unsigned long)((t_leftover_sum * 1000ULL) / t_total_sum) % 10 : 0);
        debugf("BENCH_TAIL_COUNTS mean_vissprites=%lu mean_drawsegs=%lu "
               "mean_visplanes=%lu\n",
               (unsigned long)(t_viss_sum / tail_n),
               (unsigned long)(t_ds_sum / tail_n),
               (unsigned long)(t_vp_sum / tail_n));
    }

    // --- absolute worst-N individual frames -------------------------------
    for (i = 0; i < (unsigned long)worst_count; i++)
    {
        bench_frame_t* f = &bench_frames[worst_idx[i]];
        // Sum every phase for the leftover (mirrors the per-frame loop above);
        // the RDP renderer's emit phases are ~0 in this stage so they do not
        // perturb the figure.
        unsigned long acc = 0;
        for (p = 0; p < BPH_COUNT; p++) acc += f->phase_us[p];
        debugf("BENCH_WORST rank=%lu frame=%lu total_us=%lu "
               "gametic=%lu bsp_walk=%lu seg_rast=%lu planes=%lu masked=%lu "
               "plnemit=%lu mskemit=%lu dlbuild=%lu rdpbusy=%lu keyclr=%lu "
               "hud=%lu present=%lu audio=%lu leftover=%lu "
               "viss=%u ds=%u vp=%u tic=%u\n",
               i + 1, worst_idx[i], (unsigned long)f->total_us,
               (unsigned long)f->phase_us[BPH_GAMETIC],
               (unsigned long)f->phase_us[BPH_BSP_WALK],
               (unsigned long)f->phase_us[BPH_SEG_RASTER],
               (unsigned long)f->phase_us[BPH_PLANES],
               (unsigned long)f->phase_us[BPH_MASKED],
               (unsigned long)f->phase_us[BPH_PLANE_EMIT],
               (unsigned long)f->phase_us[BPH_MASKED_EMIT],
               (unsigned long)f->phase_us[BPH_DL_BUILD],
               (unsigned long)f->phase_us[BPH_RDP_BUSY],
               (unsigned long)f->phase_us[BPH_KEY_CLEAR],
               (unsigned long)f->phase_us[BPH_HUD],
               (unsigned long)f->phase_us[BPH_PRESENT],
               (unsigned long)f->phase_us[BPH_AUDIO],
               (unsigned long)(f->total_us > acc ? f->total_us - acc : 0),
               f->vissprites, f->drawsegs, f->visplanes, f->tics_ran);
    }

    debugf("BENCH_OUTLIER level_reload_frames=%lu max_us=%lu (excluded from tail)\n",
           outlier_frames, outlier_max_us);
    N64_ReportAudioUnderruns();   // transparency proof for the audio-pump cadence
    debugf("BENCH_REPORT_END\n");
}

void N64Bench_Finish(void)
{
    unsigned long avg_us;
    unsigned long p95_us;
    unsigned long avg_fps_x10;
    unsigned long min_fps_x10;   // derived from the WORST (max) frame
    unsigned long p95_fps_x10;

    if (bench_phase == BENCH_DONE)
        return;
    bench_phase = BENCH_DONE;

    if (!rendered_frames)
        rendered_frames = 1;     // guard

    avg_us = (unsigned long)(sum_us / rendered_frames);
    p95_us = N64Bench_Percentile(95);
    if (!avg_us) avg_us = 1;
    if (!max_us) max_us = 1;
    if (!p95_us) p95_us = 1;

    avg_fps_x10 = 10000000UL / avg_us;
    min_fps_x10 = 10000000UL / max_us;     // worst frame -> lowest fps
    p95_fps_x10 = 10000000UL / p95_us;

    // Machine-readable, emitted over ISViewer (captured if the sink works).
    debugf("BENCH_RESULT frames=%lu avg_us=%lu p95_us=%lu max_us=%lu "
           "min_us=%lu avg_fps=%lu.%lu min_fps=%lu.%lu p95_fps=%lu.%lu\n",
           rendered_frames, avg_us, p95_us, max_us, min_us,
           avg_fps_x10 / 10, avg_fps_x10 % 10,
           min_fps_x10 / 10, min_fps_x10 % 10,
           p95_fps_x10 / 10, p95_fps_x10 % 10);

    // On-screen lines (drawn large + frozen for screenshot capture).
    sprintf(result_line1, "AVG %lu.%lu FPS", avg_fps_x10 / 10, avg_fps_x10 % 10);
    sprintf(result_line2, "P95 %lu.%lu MIN %lu.%lu",
            p95_fps_x10 / 10, p95_fps_x10 % 10,
            min_fps_x10 / 10, min_fps_x10 % 10);
    sprintf(result_line3, "AVG %luUS N%lu", avg_us, rendered_frames);

    // Per-phase breakdown + tail report (emitted once, at end of run).
    N64Bench_ReportPhases();
}

// Hook from G_Ticker: advance the bench gametic counter. Declared here, called
// via the #ifdef hook in g_game.c.
void N64Bench_TicHook(void)
{
    if (bench_started && bench_phase != BENCH_DONE)
        N64Bench_AdvancePhase();
}

void N64Bench_DrawOverlay(void)
{
    if (bench_phase != BENCH_DONE)
        return;

    // Re-emit the phase report periodically while frozen: the ISViewer->ares
    // stdout path can drop a burst printed right at finish, so a later capture
    // window eventually sees a complete copy (stats are frozen -- identical).
    {
        static int report_refresh;

        if (++report_refresh >= 600)
        {
            report_refresh = 0;
            N64Bench_ReportPhases();
        }
    }

    // Big, stable digits centred on screen. num/den = 3/1 (triple size).
    {
        int num = 3, den = 1;
        int w1 = (M_StringWidth(result_line1) * num) / den;
        int w2 = (M_StringWidth(result_line2) * num) / den;
        int w3 = (M_StringWidth(result_line3) * num) / den;
        int x1 = (SCREENWIDTH - w1) / 2;  if (x1 < 0) x1 = 0;
        int x2 = (SCREENWIDTH - w2) / 2;  if (x2 < 0) x2 = 0;
        int x3 = (SCREENWIDTH - w3) / 2;  if (x3 < 0) x3 = 0;

        M_WriteTextScaled(x1, 50,  result_line1, num, den);
        M_WriteTextScaled(x2, 95,  result_line2, num, den);
        M_WriteTextScaled(x3, 140, result_line3, num, den);
    }
}

#endif // N64_BENCH
