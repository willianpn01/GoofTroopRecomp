#ifndef GOOF_FRAME_PACER_H
#define GOOF_FRAME_PACER_H

/* Goof Troop Recomp -- host SIMULATION CLOCK and pacing statistics.
 * GOOF_ENHANCEMENTS_E4_REFRESH_RATE_INDEPENDENT_PRESENTATION.
 *
 * SDL-FREE, guest-free, and clock-free: every function takes the current
 * host time as an argument (integer ticks of a monotonic counter whose
 * frequency is given at init), so the whole policy runs unchanged against a
 * fake clock in tests/goof_frame_pacer_test.c.  In goof_recomp the ticks are
 * SDL_GetPerformanceCounter() (CLOCK_MONOTONIC on Linux, QPC on Windows).
 *
 * Three clocks, and only this one decides when the guest runs:
 *
 *   SIMULATION    this pacer.  One guest step (goof_app_step = one logical
 *                 epoch) is due when host time reaches `deadline`; after the
 *                 step, the deadline moves by the PHYSICAL periods that epoch
 *                 crossed, times the canonical period.  Nothing here reads a
 *                 display, a vsync, a present, or the audio queue.
 *   PRESENTATION  main_sdl.c.  Shows the latest completed framebuffer when a
 *                 new one exists (or the UI needs a redraw).  A present may
 *                 block (vsync); it can only DELAY the next due check, and
 *                 the bounded catch-up below restores the canonical cadence.
 *   AUDIO         main_sdl.c push model.  Consumes the PCM the guest already
 *                 produced; its occupancy never reaches this pacer.
 *
 * Canonical cadence: 1 physical period = 357368 NTSC master clocks
 * (1364 x 262) at 236250000/11 Hz (21.477272... MHz), i.e. exactly
 * 491381/29531250 s = 16.639356... ms, 60.098478 Hz.  The period is kept as
 * an exact rational: the deadline advances by an integer number of ticks
 * plus a remainder carried in 1/den units, so no rounding ever accumulates
 * (deadline after N periods == anchor + floor(N * freq * num / den)).
 *
 * Catch-up policy (bounded):
 *   - a step is due whenever now >= deadline, so a host that is late by less
 *     than GOOF_PACER_RESYNC_LAG_PERIODS periods runs the missed steps back to
 *     back and the guest's long-term rate is exactly canonical;
 *   - at most GOOF_PACER_MAX_STEPS_PER_ITERATION steps run between two
 *     presentation opportunities (spiral-of-death clamp);
 *   - if the host is later than GOOF_PACER_RESYNC_LAG_PERIODS periods (a
 *     window drag, a debugger, a fullscreen switch, a long compositor
 *     stall), that debt is DROPPED: the deadline is re-anchored to now and
 *     the guest simply did not run during the stall (no burst afterwards);
 *   - hold/resume (pause, Settings overlay) always re-anchors: zero
 *     catch-up after any host-initiated pause, however long. */

#include <stdbool.h>
#include <stdint.h>

enum {
  GOOF_MASTER_PER_PERIOD = 357368,           /* 1364 x 262, engine constant */
  GOOF_PACER_MAX_STEPS_PER_ITERATION = 4,
  GOOF_PACER_RESYNC_LAG_PERIODS = 4,         /* ~66.6 ms at the canonical rate */
  GOOF_PACER_MAX_EPOCH_PERIODS = 240,        /* safety bound on one epoch's hold */
};

/* Canonical period in seconds, as an exact reduced fraction num/den. */
void goof_pacer_canonical_period(uint64_t *num, uint64_t *den);
/* Period for an explicit rate (--present-hz override, tests): 1/hz as a
 * fraction with microhertz resolution.  hz must be > 0. */
void goof_pacer_period_for_hz(double hz, uint64_t *num, uint64_t *den);

/* Welford running statistics over a series of values (seconds). */
typedef struct {
  uint64_t n;
  double mean, m2, min, max, sum;
} GoofSeries;
void goof_series_add(GoofSeries *s, double v);
double goof_series_sd(const GoofSeries *s);

typedef struct {
  uint64_t steps;             /* guest steps (logical epochs) run           */
  uint64_t periods;           /* canonical periods those steps advanced     */
  uint64_t catchup_steps;     /* steps that ran as the 2nd..Nth of an iteration */
  uint64_t max_steps_in_iteration;
  uint64_t budget_deferrals;  /* iterations that hit the per-iteration clamp */
  uint64_t resyncs;           /* lag > resync bound: debt dropped           */
  uint64_t boot_anchor_ticks;  /* first step's compute, not guest time       */
  uint64_t resync_dropped_ticks;
  uint64_t reanchors;         /* hold/resume re-anchors (pause, Settings)   */
  uint64_t held_ticks;        /* host time spent held                       */
  uint64_t capped_epochs;     /* epochs whose periods hit the safety bound  */
  uint64_t max_lag_ticks;     /* worst lateness seen at a due check         */
  GoofSeries lag;             /* lateness of each step vs its deadline (s)  */
  GoofSeries step_interval;   /* host time between consecutive step starts,
                               * per period, same segment only (s)          */
  /* Least-squares fit of step start time against the cumulative period
   * index over the CURRENT segment (reset at every resync / hold): its slope
   * is the measured seconds-per-period, i.e. the effective guest rate. */
  uint64_t seg_n, segments;
  double seg_sx, seg_sy, seg_sxx, seg_sxy;
  double seg_x;               /* period index of the next step in segment  */
} GoofPacerStats;

typedef struct {
  uint64_t freq;              /* host ticks per second                      */
  uint64_t num, den;          /* period, seconds = num / den                */
  uint64_t q, r;              /* ticks per period = q + r / den (exact)     */
  uint64_t deadline;          /* host tick at which the next step is due    */
  uint64_t frac;              /* remainder of the deadline, in 1/den ticks  */
  uint64_t origin;            /* start of the current segment               */
  uint64_t last_step_at;      /* start of the previous step (same segment)  */
  uint64_t last_step_periods;
  uint64_t held_since;
  unsigned max_steps;         /* per-iteration clamp                        */
  uint64_t resync_lag;        /* ticks                                      */
  unsigned iter_steps;
  uint64_t last_lag;          /* lateness of the most recent due step       */
  bool held;
  bool have_last_step;
  GoofPacerStats st;
} GoofPacer;

/* The first step is due at `now`. */
void goof_pacer_init(GoofPacer *p, uint64_t freq, uint64_t num, uint64_t den,
                     uint64_t now);
/* Nominal period in ticks (rounded; for waits and UI cadence only). */
uint64_t goof_pacer_period_ticks(const GoofPacer *p);
double goof_pacer_rate_hz(const GoofPacer *p);

/* One main-loop iteration = one presentation opportunity. */
void goof_pacer_begin_iteration(GoofPacer *p);
/* True when a guest step must run now.  Applies the resync bound (dropping
 * a debt larger than GOOF_PACER_RESYNC_LAG_PERIODS) and the per-iteration
 * clamp.  Always false while held. */
bool goof_pacer_step_due(GoofPacer *p, uint64_t now);
/* The step started at `started` (the `now` given to step_due) has run and
 * crossed `periods` physical periods (0 is treated as 1, larger values are
 * clamped to GOOF_PACER_MAX_EPOCH_PERIODS). */
void goof_pacer_stepped(GoofPacer *p, uint64_t started, uint64_t periods);
/* The same step finished at `finished`.  Only the FIRST step of the session
 * does anything: the simulation clock is anchored at the moment the first
 * guest epoch exists, i.e. that step's compute time is not part of guest time.
 * That first epoch is the cold-boot loader (121 periods computed in ~0.3 s);
 * anchoring before it would start the audio device -- which can only play
 * PCM once it exists -- that far behind the guest for the whole session.
 * Later steps are never shifted: a slow one-period step is a lag the resync
 * rule handles, and a multi-period step computes inside its own hold. */
void goof_pacer_step_finished(GoofPacer *p, uint64_t started, uint64_t finished);
/* Tick at which the next step is due. */
uint64_t goof_pacer_deadline(const GoofPacer *p);

/* Host-initiated hold (P pause, Settings overlay) and resume.  Resume
 * re-anchors: the next step is due at `now`, with no catch-up. */
void goof_pacer_hold(GoofPacer *p, uint64_t now);
void goof_pacer_resume(GoofPacer *p, uint64_t now);
bool goof_pacer_is_held(const GoofPacer *p);

/* Measured guest rate over the current segment (Hz; 0 if < 3 steps) and the
 * relative error against the canonical rate in parts per million. */
double goof_pacer_measured_hz(const GoofPacer *p);
double goof_pacer_rate_error_ppm(const GoofPacer *p);

/* The ideal number of periods for `running_ticks` of un-held, un-dropped
 * host time, and the signed difference actual - ideal (periods).  With
 * exact deadlines the difference stays within [-1, max epoch hold]. */
double goof_pacer_expected_periods(const GoofPacer *p, uint64_t running_ticks);

#endif
