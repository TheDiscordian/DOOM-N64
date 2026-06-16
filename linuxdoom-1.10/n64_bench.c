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
        f->tics_ran   = (uint8_t)cur_tics_ran;
        f->is_outlier = 0;
    }

#if N64_BENCH_MARKS
    // Frame-keyed visual-capture markers (BENCH_MARKS=1 builds only): the
    // host capture loop greps these off the live ISViewer log and screenshots
    // on each one. Frame N is the same game state on every build (virtual tic
    // clock), so captures pair exactly across flag-on/flag-off ROMs. Never
    // enabled in timing builds -- the debugf cost would skew the numbers.
    // Every 128 retained frames (32 points/run): Ryan observed texture
    // artifacting falling between the original 256-frame samples.
    if ((bench_frame_count & 127) == 0)
    {
        debugf("BENCH_MARK frame=%lu\n", bench_frame_count);

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
#endif
}

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
#ifdef PVS_PROBE
    unsigned long long pvs_visited_sum = 0, pvs_cullable_sum = 0;
#endif
    unsigned long tics_frames = 0;

    unsigned long tail_thresh, tail_target, tail_n = 0;
    unsigned long long t_phase_sum[BPH_COUNT];
    unsigned long long t_total_sum = 0, t_leftover_sum = 0;
    unsigned long long t_viss_sum = 0, t_ds_sum = 0, t_vp_sum = 0;
    unsigned long t_tics_frames = 0;

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
#ifdef PVS_PROBE
        pvs_visited_sum  += f->pvs_visited;
        pvs_cullable_sum += f->pvs_cullable;
#endif
        if (f->tics_ran) tics_frames++;

        // tail accumulation
        if (f->total_us >= tail_thresh)
        {
            tail_n++;
            t_total_sum += f->total_us;
            for (p = 0; p < BPH_COUNT; p++) t_phase_sum[p] += f->phase_us[p];
            t_leftover_sum += leftover;
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
