/* Goof Troop Recomp -- host simulation clock.  SDL-FREE.
 * GOOF_ENHANCEMENTS_E4_REFRESH_RATE_INDEPENDENT_PRESENTATION.
 * See goof_frame_pacer.h for the policy; this file is only arithmetic. */
#include "host/goof_frame_pacer.h"

#include <math.h>

/* NTSC master clock: 315/88 MHz x 6 = 236250000/11 Hz. */
enum { NTSC_MASTER_NUM = 236250000, NTSC_MASTER_DEN = 11 };

static uint64_t gcd_u64(uint64_t a, uint64_t b) {
  while (b) { uint64_t t = a % b; a = b; b = t; }
  return a;
}

static void reduce(uint64_t *num, uint64_t *den) {
  uint64_t g = gcd_u64(*num, *den);
  if (g > 1) { *num /= g; *den /= g; }
}

void goof_pacer_canonical_period(uint64_t *num, uint64_t *den) {
  /* seconds = 357368 / (236250000 / 11) = 357368 * 11 / 236250000 */
  *num = (uint64_t)GOOF_MASTER_PER_PERIOD * NTSC_MASTER_DEN;
  *den = NTSC_MASTER_NUM;
  reduce(num, den);
}

void goof_pacer_period_for_hz(double hz, uint64_t *num, uint64_t *den) {
  double micro = hz * 1e6 + 0.5;
  *num = 1000000u;
  *den = micro < 1.0 ? 1u : (uint64_t)micro;
  reduce(num, den);
}

void goof_series_add(GoofSeries *s, double v) {
  s->n++;
  s->sum += v;
  if (s->n == 1) { s->min = s->max = v; }
  if (v < s->min) s->min = v;
  if (v > s->max) s->max = v;
  double d = v - s->mean;
  s->mean += d / (double)s->n;
  s->m2 += d * (v - s->mean);
}

double goof_series_sd(const GoofSeries *s) {
  return s->n > 1 ? sqrt(s->m2 / (double)s->n) : 0.0;
}

static void new_segment(GoofPacer *p, uint64_t now) {
  p->origin = now;
  p->have_last_step = false;
  p->st.seg_n = 0;
  p->st.seg_sx = p->st.seg_sy = p->st.seg_sxx = p->st.seg_sxy = 0.0;
  p->st.seg_x = 0.0;
  p->st.segments++;
}

void goof_pacer_init(GoofPacer *p, uint64_t freq, uint64_t num, uint64_t den,
                     uint64_t now) {
  *p = (GoofPacer){0};
  p->freq = freq ? freq : 1;
  p->num = num ? num : 1;
  p->den = den ? den : 1;
  /* freq * num stays far below 2^64 for every real counter (1e9 * 1e6). */
  uint64_t fn = p->freq * p->num;
  p->q = fn / p->den;
  p->r = fn % p->den;
  p->deadline = now;
  p->max_steps = GOOF_PACER_MAX_STEPS_PER_ITERATION;
  p->resync_lag = p->q * GOOF_PACER_RESYNC_LAG_PERIODS +
                  (p->r * GOOF_PACER_RESYNC_LAG_PERIODS) / p->den;
  new_segment(p, now);
}

uint64_t goof_pacer_period_ticks(const GoofPacer *p) {
  return p->q + (2 * p->r >= p->den ? 1 : 0);
}

double goof_pacer_rate_hz(const GoofPacer *p) {
  return (double)p->den / (double)p->num;
}

void goof_pacer_begin_iteration(GoofPacer *p) { p->iter_steps = 0; }

bool goof_pacer_step_due(GoofPacer *p, uint64_t now) {
  if (p->held || now < p->deadline) return false;
  if (p->iter_steps >= p->max_steps) {
    if (p->iter_steps == p->max_steps) {       /* count the iteration once */
      p->st.budget_deferrals++;
      p->iter_steps++;
    }
    return false;
  }
  uint64_t lag = now - p->deadline;
  bool resynced = false;
  if (lag > p->st.max_lag_ticks) p->st.max_lag_ticks = lag;
  if (lag > p->resync_lag) {
    /* Too late to catch up without a visible burst: drop the debt.  The
     * guest did not run during the stall and will not make up for it. */
    p->st.resyncs++;
    p->st.resync_dropped_ticks += lag;
    p->deadline = now;
    p->frac = 0;
    new_segment(p, now);
    lag = 0;
    resynced = true;
  }
  p->last_lag = lag;
  goof_series_add(&p->st.lag, (double)lag / (double)p->freq);
  /* A catch-up step is a 2nd..Nth step of one iteration that pays back a
   * debt; the first step after a resync is an ordinary on-time step. */
  if (p->iter_steps > 0 && !resynced) p->st.catchup_steps++;
  return true;
}

void goof_pacer_stepped(GoofPacer *p, uint64_t started, uint64_t periods) {
  if (periods == 0) periods = 1;
  if (periods > GOOF_PACER_MAX_EPOCH_PERIODS) {
    periods = GOOF_PACER_MAX_EPOCH_PERIODS;
    p->st.capped_epochs++;
  }
  GoofPacerStats *s = &p->st;
  if (p->have_last_step && started >= p->last_step_at)
    goof_series_add(&s->step_interval,
                    (double)(started - p->last_step_at) / (double)p->freq /
                        (double)p->last_step_periods);
  p->last_step_at = started;
  p->last_step_periods = periods;
  p->have_last_step = true;

  double x = s->seg_x, y = (double)(started - p->origin) / (double)p->freq;
  s->seg_n++;
  s->seg_sx += x; s->seg_sy += y; s->seg_sxx += x * x; s->seg_sxy += x * y;
  s->seg_x += (double)periods;

  p->iter_steps++;
  if (p->iter_steps > s->max_steps_in_iteration) s->max_steps_in_iteration = p->iter_steps;
  s->steps++;
  s->periods += periods;

  /* deadline += periods * (q + r/den), exactly. */
  p->deadline += periods * p->q;
  p->frac += periods * p->r;
  p->deadline += p->frac / p->den;
  p->frac %= p->den;
}

void goof_pacer_step_finished(GoofPacer *p, uint64_t started, uint64_t finished) {
  if (p->st.steps != 1 || finished <= started) return;
  uint64_t took = finished - started;
  p->st.boot_anchor_ticks = took;
  p->deadline += took;            /* the first hold starts when the step ended */
  new_segment(p, finished);
}

uint64_t goof_pacer_deadline(const GoofPacer *p) { return p->deadline; }

void goof_pacer_hold(GoofPacer *p, uint64_t now) {
  if (p->held) return;
  p->held = true;
  p->held_since = now;
}

void goof_pacer_resume(GoofPacer *p, uint64_t now) {
  if (!p->held) return;
  p->held = false;
  if (now > p->held_since) p->st.held_ticks += now - p->held_since;
  p->deadline = now;
  p->frac = 0;
  p->st.reanchors++;
  new_segment(p, now);
}

bool goof_pacer_is_held(const GoofPacer *p) { return p->held; }

double goof_pacer_measured_hz(const GoofPacer *p) {
  const GoofPacerStats *s = &p->st;
  if (s->seg_n < 3) return 0.0;
  double n = (double)s->seg_n;
  double den = n * s->seg_sxx - s->seg_sx * s->seg_sx;
  if (den <= 0.0) return 0.0;
  double slope = (n * s->seg_sxy - s->seg_sx * s->seg_sy) / den;
  return slope > 0.0 ? 1.0 / slope : 0.0;
}

double goof_pacer_rate_error_ppm(const GoofPacer *p) {
  double m = goof_pacer_measured_hz(p);
  return m > 0.0 ? (m / goof_pacer_rate_hz(p) - 1.0) * 1e6 : 0.0;
}

double goof_pacer_expected_periods(const GoofPacer *p, uint64_t running_ticks) {
  return (double)running_ticks * (double)p->den /
         ((double)p->freq * (double)p->num);
}
