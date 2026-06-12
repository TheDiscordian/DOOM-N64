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

// m_menu.c font helpers (external linkage, not in m_menu.h).
extern int  M_StringWidth(char* string);
extern void M_WriteTextScaled(int x, int y, char* string, int num, int den);

// Scenario length in gametics (35 Hz). 35*60 = 60 emulated seconds.
#define BENCH_GAMETICS      (35 * 60)
// Discard the first second of rendered frames (level load / cache warm-up).
#define BENCH_WARMUP_GAMETICS (35 * 1)

// Per-frame cost histogram for a RAM-cheap p95. 64 us per bucket * 4096 =
// 0..262 ms range, which comfortably brackets N64 frame costs (~14-50 ms).
#define BENCH_HIST_BUCKETS  4096
#define BENCH_HIST_US_SHIFT 6      // 64 us per bucket
#define BENCH_HIST_OVERFLOW (BENCH_HIST_BUCKETS - 1)

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
} bench_step_t;

static const bench_step_t bench_script[] =
{
    {  50,   0,      0 },   // forward
    {  50,   0,   -640 },   // forward + turn left
    {  50,   0,      0 },   // forward
    {  25,   0,    768 },   // forward + turn right (wide)
    {   0,   0,    768 },   // pivot in place
    {  50,   0,      0 },   // forward
    { -25,   0,   -512 },   // back-pedal + turn
    {  50,  25,      0 },   // forward + strafe
    {  50,   0,    384 },   // forward + slow turn
    {   0,   0,  -1024 },   // fast pivot
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
    debugf("BENCH: init, scenario=E1M1 uncapped, target=%d gametics\n",
           BENCH_GAMETICS);
}

int N64Bench_Active(void)
{
    // Forces the uncapped single-player render path in D_DoomLoop. Stays true
    // through DONE so input keeps flowing (idle) and the overlay holds.
    return bench_started;
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
