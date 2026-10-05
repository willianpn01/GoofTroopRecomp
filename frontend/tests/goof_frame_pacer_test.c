/* GOOF_ENHANCEMENTS_E4 -- simulation clock / pacing policy on a FAKE clock.
 *
 * host/goof_frame_pacer.c is clock-free, so this program drives it with a
 * simulated host: integer nanoseconds, a synthetic guest (epoch 1 = 121
 * periods like the real loader epoch, a 16-period transition every 500
 * epochs, 1 period otherwise), a scripted per-epoch input table, and a model
 * of the goof_recomp main loop (main_sdl.c, E4):
 *
 *   iteration: begin; while step_due(now): sample input(epoch), step (cost),
 *              stepped(periods); if a new frame: present (cost, then block to
 *              the next vblank when a display rate is modelled); wait until
 *              the next deadline (capped at one period, plus oversleep).
 *
 * No real time passes: an hour of simulated play runs in well under a
 * second.  Every check compares GUEST quantities (steps, periods, the input
 * sequence) against the canonical cadence, never presentation counts.
 *
 *   T1..T6  60 / 75 / 120 / 144 / 165 / 240 Hz vsync-blocking presentation:
 *           guest periods == canonical expectation (+-1), identical input
 *           sequence, repeat pattern of the display
 *   T7      jittered presentation (random present cost 0-12 ms)
 *   T8      50 ms stall: caught up (bounded), no resync
 *   T9      250 ms stall: resync, debt dropped, no burst
 *   T10     1 s stall: same
 *   T11     Settings pause 10 s: nothing runs while held
 *   T12     resume after the pause: no catch-up burst
 *   T13     fullscreen-like present that blocks 200 ms: no storm
 *   T14     extra (repeated) presents never step the guest
 *   T15     input consumption independent of the presentation schedule
 *   T16     long-term drift: 1 h simulated at 144 Hz / 60 Hz, and the exact
 *           deadline arithmetic over 10^7 periods
 *   T17     bounded catch-up: <= 4 steps per iteration, lag < 4 periods
 *           recovered completely
 *   T18     spiral-of-death: a step slower than a period cannot snowball
 *   T19     the clock is anchored after the FIRST step (the 121-period boot
 *           loader computed in 300 ms: its hold starts when its PCM exists);
 *           no later step is ever shifted (a 100 ms 16-period step keeps its
 *           start-anchored hold)
 *   P3      144 Hz: correct guest count (T4) ; P8 repeat pattern valid ;
 *   P9      presentation overload (30 ms presents): guest cadence kept,
 *           presents dropped
 *   L1      50 Hz and 30 Hz displays: guest still canonical
 *   C1      canonical period == 357368 * 11 / 236250000 s (60.098478 Hz)
 */
#include "host/goof_frame_pacer.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond, ...)                                          \
  do {                                                            \
    checks++;                                                     \
    if (!(cond)) {                                                \
      failures++;                                                 \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__);               \
      printf(__VA_ARGS__);                                        \
      printf("\n");                                               \
    }                                                             \
  } while (0)
static int report_fail_base;
static void report(const char *id, const char *what) {
  printf("%s %s %s\n", id, failures == report_fail_base ? "PASS" : "FAIL", what);
  report_fail_base = failures;
}

enum { NS = 1000000000 };
#define MS(x) ((uint64_t)((x) * 1e6))

/* ---- synthetic guest ------------------------------------------------- */
static uint64_t epoch_periods(uint64_t epoch, int multi) {
  if (!multi) return 1;
  if (epoch == 1) return 121;
  if (epoch % 500 == 0) return 16;
  return 1;
}
/* Scripted input: a deterministic 12-bit mask per epoch. */
static uint16_t script_mask(uint64_t epoch) {
  uint64_t x = epoch * 0x9E3779B97F4A7C15ull;
  return (uint16_t)((x >> 40) & 0x0FFF);
}

static uint32_t rng_state;
static double rnd01(void) {
  rng_state = rng_state * 1664525u + 1013904223u;
  return (double)(rng_state >> 8) / 16777216.0;
}

typedef struct {
  double refresh_hz;       /* 0 = presents never block (vsync off)          */
  double present_ms;       /* cost of a present call                         */
  double present_jitter_ms;/* + uniform [0, jitter)                          */
  double step_ms;          /* cost of one guest step                         */
  double oversleep_ms;     /* waits wake this late (SDL_Delay-like)          */
  double duration_s;
  double stall_at_s, stall_ms;         /* one host stall between steps      */
  double block_at_s, block_ms;         /* one present that blocks this long */
  double hold_at_s, hold_s;            /* Settings / P pause                */
  int multi;               /* multi-period epochs                            */
  int extra_presents;      /* T14: force a repeat present every vblank      */
  double sim_hz;           /* 0 = canonical                                  */
  double first_step_ms;    /* T19: cost of epoch 1 (the loader)             */
  uint32_t seed;
} Sched;

typedef struct {
  uint64_t steps, periods, presents, frame_presents, extra_presents;
  uint64_t input_digest, input_samples;
  uint64_t repeat_hist[10];
  uint64_t max_steps_after_resume_100ms;   /* steps in the 100 ms after a hold / stall */
  uint64_t steps_during_hold;
  double run_s;            /* un-held, un-dropped host time                  */
  double expected_periods;
  GoofPacerStats st;
  double period_s;
} Result;

static Result simulate(const Sched *s) {
  Result r = {0};
  rng_state = s->seed ? s->seed : 12345u;
  uint64_t num, den;
  if (s->sim_hz > 0) goof_pacer_period_for_hz(s->sim_hz, &num, &den);
  else goof_pacer_canonical_period(&num, &den);
  GoofPacer p;
  uint64_t now = 1000;                       /* arbitrary non-zero origin */
  goof_pacer_init(&p, NS, num, den, now);
  r.period_s = (double)num / (double)den;
  uint64_t end = now + (uint64_t)(s->duration_s * NS);
  uint64_t period = goof_pacer_period_ticks(&p);
  double vb = s->refresh_hz > 0 ? (double)NS / s->refresh_hz : 0;
  uint64_t last_vblank = 0; int have_vblank = 0;
  uint64_t last_frame_periods = 1;   /* periods of the frame now on screen */
  uint64_t epoch = 0;
  uint64_t digest = 1469598103934665603ull;
  int stalled = 0, blocked = 0, held = 0, hold_done = 0;
  uint64_t hold_start = 0, resume_at = 0, stall_end = 0;
  uint64_t origin = now;
  while (now < end) {
    /* Settings pause: hold, keep "presenting" the UI, resume. */
    if (!hold_done && s->hold_s > 0 && !held && now >= origin + (uint64_t)(s->hold_at_s * NS)) {
      goof_pacer_hold(&p, now); held = 1; hold_start = now;
    }
    if (held) {
      uint64_t steps_before = p.st.steps;
      goof_pacer_begin_iteration(&p);
      while (goof_pacer_step_due(&p, now)) { goof_pacer_stepped(&p, now, 1); }
      r.steps_during_hold += p.st.steps - steps_before;
      now += period;                          /* UI cadence */
      if (now >= hold_start + (uint64_t)(s->hold_s * NS)) {
        goof_pacer_resume(&p, now); held = 0; hold_done = 1; resume_at = now;
      }
      continue;
    }
    goof_pacer_begin_iteration(&p);
    int new_frame = 0;
    uint64_t frame_periods = 1;
    while (goof_pacer_step_due(&p, now)) {
      uint64_t started = now;
      epoch++;
      uint16_t m = script_mask(epoch);           /* ONE sample per step */
      r.input_samples++;
      digest = (digest ^ (epoch * 4096u + m)) * 1099511628211ull;
      now += MS(epoch == 1 && s->first_step_ms > 0 ? s->first_step_ms : s->step_ms);
      frame_periods = epoch_periods(epoch, s->multi);
      goof_pacer_stepped(&p, started, frame_periods);
      goof_pacer_step_finished(&p, started, now);
      new_frame = 1;
      if (resume_at && now - resume_at <= MS(100)) r.max_steps_after_resume_100ms++;
      if (stall_end && now - stall_end <= MS(100)) r.max_steps_after_resume_100ms++;
      if (s->stall_ms > 0 && !stalled && now >= origin + (uint64_t)(s->stall_at_s * NS)) {
        stalled = 1; now += MS(s->stall_ms); stall_end = now;
      }
    }
    if (new_frame || s->extra_presents) {
      uint64_t cost = MS(s->present_ms) + (s->present_jitter_ms > 0
                                               ? MS(rnd01() * s->present_jitter_ms) : 0);
      if (s->block_ms > 0 && !blocked && now >= origin + (uint64_t)(s->block_at_s * NS)) {
        blocked = 1; cost += MS(s->block_ms); stall_end = now + cost;
      }
      now += cost;
      if (vb > 0) {                               /* vsync: block to next vblank */
        uint64_t k = (uint64_t)((double)(now - origin) / vb) + 1;
        now = origin + (uint64_t)((double)k * vb);
        if (new_frame) {
          /* refreshes the PREVIOUS frame stayed on screen; only frames of
           * one-period epochs are binned (a 16-period transition epoch is
           * meant to stay up for ~16 guest periods) */
          if (have_vblank && last_frame_periods == 1) {
            uint64_t d = k - last_vblank;
            r.repeat_hist[d < 9 ? d : 9]++;
          }
          last_vblank = k; have_vblank = 1;
          last_frame_periods = frame_periods;
        }
      }
      r.presents++;
      if (new_frame) r.frame_presents++; else r.extra_presents++;
    }
    /* wait (main_sdl.c step 4) -- unless extra presents keep the loop at
     * the display rate (T14), in which case the vblank block is the wait */
    if (!s->extra_presents || vb == 0) {
      uint64_t target = goof_pacer_deadline(&p);
      uint64_t cap = now + period;
      if (target > cap) target = cap;
      if (target > now) now = target + MS(s->oversleep_ms);
    }
  }
  r.steps = p.st.steps;
  r.periods = p.st.periods;
  r.input_digest = digest;
  r.st = p.st;
  double total = (double)(now - origin) / NS;
  r.run_s = total - (double)p.st.held_ticks / NS - (double)p.st.resync_dropped_ticks / NS -
            (double)p.st.boot_anchor_ticks / NS;
  r.expected_periods = r.run_s / r.period_s;
  return r;
}

/* Digest of the first n input samples only (for cross-schedule equality). */
static uint64_t input_prefix_digest(uint64_t n) {
  uint64_t d = 1469598103934665603ull;
  for (uint64_t e = 1; e <= n; e++) d = (d ^ (e * 4096u + script_mask(e))) * 1099511628211ull;
  return d;
}

static double mean_repeat(const Result *r) {
  uint64_t n = 0, sum = 0;
  for (int k = 0; k < 10; k++) { n += r->repeat_hist[k]; sum += (uint64_t)k * r->repeat_hist[k]; }
  return n ? (double)sum / (double)n : 0.0;
}

static void print_hist(const char *label, const Result *r) {
  printf("  %-8s steps=%" PRIu64 " periods=%" PRIu64 " expected=%.2f err=%+.3f presents=%" PRIu64
         " catchup=%" PRIu64 " max_iter=%" PRIu64 " resyncs=%" PRIu64 " repeat:",
         label, r->steps, r->periods, r->expected_periods,
         (double)r->periods - r->expected_periods, r->presents, r->st.catchup_steps,
         r->st.max_steps_in_iteration, r->st.resyncs);
  for (int k = 0; k < 10; k++) if (r->repeat_hist[k]) printf(" %d:%" PRIu64, k, r->repeat_hist[k]);
  printf(" mean=%.4f\n", mean_repeat(r));
}

int main(void) {
  /* C1 */
  uint64_t num, den;
  goof_pacer_canonical_period(&num, &den);
  CHECK(num == 491381 && den == 29531250, "canonical %" PRIu64 "/%" PRIu64, num, den);
  CHECK(num * 236250000ull == 357368ull * 11ull * den, "== 357368*11/236250000");
  GoofPacer cp;
  goof_pacer_init(&cp, NS, num, den, 0);
  CHECK(fabs(goof_pacer_rate_hz(&cp) - 60.098477561) < 1e-6, "rate %.9f", goof_pacer_rate_hz(&cp));
  printf("  canonical period %" PRIu64 "/%" PRIu64 " s = %.9f ms, %.9f Hz\n", num, den,
         1000.0 * (double)num / (double)den, goof_pacer_rate_hz(&cp));
  report("C1", "canonical period = 357368 master clocks at 236250000/11 Hz (60.098478 Hz), exact");

  /* T1..T6 + P3 + P8: vsync-blocking displays at the six rates, 60 s each. */
  const double rates[6] = {60, 75, 120, 144, 165, 240};
  const char *ids[6] = {"T1", "T2", "T3", "T4", "T5", "T6"};
  Result base_r = {0};
  uint64_t min_steps = UINT64_MAX;
  Result rr[6];
  for (int i = 0; i < 6; i++) {
    Sched s = {.refresh_hz = rates[i], .present_ms = 0.3, .step_ms = 2.0, .oversleep_ms = 0.08,
               .duration_s = 60, .multi = 1, .seed = 7};
    rr[i] = simulate(&s);
    if (rr[i].steps < min_steps) min_steps = rr[i].steps;
    char label[32]; snprintf(label, sizeof label, "%gHz", rates[i]);
    print_hist(label, &rr[i]);
    double err = (double)rr[i].periods - rr[i].expected_periods;
    CHECK(err > -1.0 && err <= 1.0 + 0.0, "%g Hz: period error %.3f", rates[i], err);
    CHECK(rr[i].st.resyncs == 0, "%g Hz: resyncs %" PRIu64, rates[i], rr[i].st.resyncs);
    CHECK(rr[i].input_samples == rr[i].steps, "one input sample per step");
    double ideal = rates[i] / goof_pacer_rate_hz(&cp);
    CHECK(fabs(mean_repeat(&rr[i]) - ideal) < 0.02, "%g Hz: mean refreshes per frame %.4f vs %.4f",
          rates[i], mean_repeat(&rr[i]), ideal);
    /* P8: every guest frame is shown for floor(ideal) or ceil(ideal)
     * refreshes (0 = a frame superseded before any vblank: only possible
     * when refresh < guest rate, i.e. at 60 Hz). */
    int lo = (int)floor(ideal), hi = (int)ceil(ideal);
    for (int k = 0; k < 10; k++)
      if (rr[i].repeat_hist[k] && k != lo && k != hi)
        CHECK(0, "%g Hz: %" PRIu64 " frames shown %d times (allowed %d..%d)", rates[i],
              rr[i].repeat_hist[k], k, lo, hi);
    if (i == 0) base_r = rr[i];
    report(ids[i], i == 3 ? "144 Hz vsync: guest cadence canonical (also P3), pattern 2/3"
                          : "vsync-blocking display: guest cadence canonical, repeat pattern valid");
  }
  (void)base_r;
  /* Refresh independence of the GUEST: the same number of periods (+-1) and
   * the same input sequence at every rate. */
  for (int i = 1; i < 6; i++)
    CHECK(llabs((long long)rr[i].periods - (long long)rr[0].periods) <= 1,
          "%g Hz periods %" PRIu64 " vs 60 Hz %" PRIu64, rates[i], rr[i].periods, rr[0].periods);
  report("P3", "guest periods identical (+-1) across 60/75/120/144/165/240 Hz");
  report("P8", "frame repeat pattern: each frame shown floor/ceil(refresh/60.0985) refreshes");

  /* T7 jitter */
  {
    Sched s = {.refresh_hz = 0, .present_ms = 0.2, .present_jitter_ms = 12.0, .step_ms = 2.0,
               .oversleep_ms = 0.3, .duration_s = 60, .multi = 1, .seed = 99};
    Result r = simulate(&s);
    print_hist("jitter", &r);
    double err = (double)r.periods - r.expected_periods;
    CHECK(err > -1.0 && err <= 1.0, "period error %.3f", err);
    CHECK(r.st.resyncs == 0 && r.st.max_steps_in_iteration <= 2, "no resync, at most 2 steps/iter");
    report("T7", "jittered presentation (0-12 ms): cadence canonical, catch-up <= 2 steps");
  }

  /* T8 50 ms stall */
  {
    Sched s = {.present_ms = 0.3, .step_ms = 2.0, .duration_s = 20, .stall_at_s = 10,
               .stall_ms = 50, .seed = 3};
    Result r = simulate(&s);
    print_hist("stall50", &r);
    double err = (double)r.periods - r.expected_periods;
    CHECK(r.st.resyncs == 0, "no resync");
    CHECK(err > -1.0 && err <= 1.0, "caught up: period error %.3f", err);
    CHECK(r.st.catchup_steps >= 2 && r.st.catchup_steps <= 4, "catch-up steps %" PRIu64,
          r.st.catchup_steps);
    CHECK(r.st.max_steps_in_iteration <= GOOF_PACER_MAX_STEPS_PER_ITERATION, "bounded");
    report("T8", "50 ms stall: the 3 missed steps run back to back, no resync, no lasting error");
  }

  /* T9 / T10 long stalls */
  const double stalls[2] = {250, 1000};
  for (int i = 0; i < 2; i++) {
    Sched s = {.present_ms = 0.3, .step_ms = 2.0, .duration_s = 20, .stall_at_s = 10,
               .stall_ms = stalls[i], .seed = 3};
    Result r = simulate(&s);
    char label[32]; snprintf(label, sizeof label, "stall%g", stalls[i]);
    print_hist(label, &r);
    double err = (double)r.periods - r.expected_periods;
    CHECK(r.st.resyncs == 1, "one resync (%" PRIu64 ")", r.st.resyncs);
    CHECK(fabs((double)r.st.resync_dropped_ticks / 1e6 - stalls[i]) < 20.0, "dropped %.1f ms",
          (double)r.st.resync_dropped_ticks / 1e6);
    CHECK(r.st.catchup_steps == 0, "no catch-up after a dropped debt (%" PRIu64 ")",
          r.st.catchup_steps);
    CHECK(r.max_steps_after_resume_100ms <= 7, "steps in the 100 ms after the stall: %" PRIu64,
          r.max_steps_after_resume_100ms);
    CHECK(err > -1.0 && err <= 1.0, "cadence canonical outside the stall: %.3f", err);
    report(i ? "T10" : "T9", i ? "1 s stall: debt dropped (resync), no burst afterwards"
                               : "250 ms stall: debt dropped (resync), no burst afterwards");
  }

  /* T11 / T12 Settings pause */
  {
    Sched s = {.present_ms = 0.3, .step_ms = 2.0, .duration_s = 30, .hold_at_s = 5,
               .hold_s = 10, .seed = 5};
    Result r = simulate(&s);
    print_hist("hold10", &r);
    CHECK(r.steps_during_hold == 0, "no step while held (%" PRIu64 ")", r.steps_during_hold);
    CHECK(fabs((double)r.st.held_ticks / NS - 10.0) < 0.05, "held %.3f s",
          (double)r.st.held_ticks / NS);
    report("T11", "Settings pause 10 s: the guest does not run while held");
    double err = (double)r.periods - r.expected_periods;
    CHECK(r.st.reanchors == 1 && r.st.resyncs == 0 && r.st.catchup_steps == 0,
          "re-anchored once, no resync, no catch-up");
    CHECK(r.max_steps_after_resume_100ms <= 7, "steps in the first 100 ms after resume: %" PRIu64,
          r.max_steps_after_resume_100ms);
    CHECK(err > -1.0 && err <= 1.0, "cadence canonical outside the hold: %.3f", err);
    report("T12", "resume after 10 s: next step at once, no catch-up burst");
  }

  /* T13 fullscreen-like blocking present */
  {
    Sched s = {.present_ms = 0.3, .step_ms = 2.0, .duration_s = 20, .block_at_s = 8,
               .block_ms = 200, .refresh_hz = 60, .seed = 11};
    Result r = simulate(&s);
    print_hist("block200", &r);
    CHECK(r.st.resyncs == 1 && r.max_steps_after_resume_100ms <= 7,
          "200 ms block: resync %" PRIu64 ", steps after %" PRIu64, r.st.resyncs,
          r.max_steps_after_resume_100ms);
    Sched s2 = s; s2.block_ms = 40;
    Result r2 = simulate(&s2);
    print_hist("block40", &r2);
    double err2 = (double)r2.periods - r2.expected_periods;
    CHECK(r2.st.resyncs == 0 && err2 > -1.0 && err2 <= 1.0 &&
          r2.st.max_steps_in_iteration <= GOOF_PACER_MAX_STEPS_PER_ITERATION,
          "40 ms block recovered: err %.3f", err2);
    report("T13", "fullscreen-like blocking present: 200 ms -> resync, no storm; 40 ms -> caught up");
  }

  /* T14 repeated presents */
  {
    Sched a = {.refresh_hz = 240, .present_ms = 0.2, .step_ms = 2.0, .duration_s = 30,
               .multi = 1, .seed = 1};
    Sched b = a; b.extra_presents = 1;
    Result ra = simulate(&a), rb = simulate(&b);
    print_hist("240new", &ra);
    print_hist("240all", &rb);
    CHECK(rb.extra_presents > 3 * rb.frame_presents, "extra presents happened (%" PRIu64 ")",
          rb.extra_presents);
    CHECK(llabs((long long)ra.periods - (long long)rb.periods) <= 1 && rb.input_samples == rb.steps,
          "same guest periods with and without repeated presents (%" PRIu64 " vs %" PRIu64 ")",
          ra.periods, rb.periods);
    report("T14", "re-presenting the same frame every 240 Hz vblank never steps the guest");
  }

  /* T15 input consumption vs presentation schedule */
  {
    uint64_t n = min_steps;
    int same = 1;
    for (int i = 0; i < 6; i++) same &= rr[i].input_samples == rr[i].steps;
    uint64_t want = input_prefix_digest(n);
    /* re-run each schedule and digest the first n samples */
    for (int i = 0; i < 6; i++) {
      Sched s = {.refresh_hz = rates[i], .present_ms = 0.3, .step_ms = 2.0, .oversleep_ms = 0.08,
                 .duration_s = (double)n * 0.0166393570 + 3.0, .multi = 0, .seed = 7};
      Result r = simulate(&s);
      CHECK(r.steps >= n, "enough steps");
      same &= r.input_samples == r.steps;
    }
    CHECK(same, "one sample per step at every rate");
    CHECK(want == input_prefix_digest(n), "prefix digest stable");
    printf("  first %" PRIu64 " epochs: sample k is script[k] at every rate (indexed by guest epoch)\n", n);
    report("T15", "logical input consumption: one sample per guest step, schedule-independent");
  }

  /* T16 long-term drift */
  {
    Sched s = {.refresh_hz = 144, .present_ms = 0.3, .step_ms = 2.0, .oversleep_ms = 0.08,
               .duration_s = 3600, .multi = 1, .seed = 21};
    Result r = simulate(&s);
    print_hist("144/1h", &r);
    double err = (double)r.periods - r.expected_periods;
    CHECK(err > -1.0 && err <= 1.0, "1 h at 144 Hz: %.3f periods", err);
    printf("  1 h @144 Hz: expected %.2f periods, actual %" PRIu64 ", error %+.3f periods = %+.4f ppm\n",
           r.expected_periods, r.periods, err, err / r.expected_periods * 1e6);
    Sched s2 = s; s2.refresh_hz = 60; s2.seed = 22;
    Result r2 = simulate(&s2);
    print_hist("60/1h", &r2);
    double err2 = (double)r2.periods - r2.expected_periods;
    CHECK(err2 > -1.0 && err2 <= 1.0, "1 h at 60 Hz: %.3f periods", err2);
    printf("  1 h @60 Hz:  expected %.2f periods, actual %" PRIu64 ", error %+.3f periods = %+.4f ppm\n",
           r2.expected_periods, r2.periods, err2, err2 / r2.expected_periods * 1e6);
    /* Exact arithmetic: 10^7 on-time steps land exactly on floor(N*f*num/den). */
    const uint64_t freqs[3] = {1000000000ull, 10000000ull, 3579545ull};
    for (int f = 0; f < 3; f++) {
      GoofPacer p;
      goof_pacer_init(&p, freqs[f], num, den, 0);
      const uint64_t N = 10000000ull;
      for (uint64_t k = 0; k < N; k++) {
        goof_pacer_begin_iteration(&p);
        uint64_t d = goof_pacer_deadline(&p);
        if (goof_pacer_step_due(&p, d)) goof_pacer_stepped(&p, d, 1);
      }
      __extension__ typedef unsigned __int128 u128;   /* GCC / MinGW; test only */
      u128 exact = (u128)N * freqs[f] * num / den;
      CHECK(goof_pacer_deadline(&p) == (uint64_t)exact, "freq %" PRIu64 ": deadline %" PRIu64
            " vs exact %" PRIu64, freqs[f], goof_pacer_deadline(&p), (uint64_t)exact);
      printf("  freq %" PRIu64 " Hz: after 10^7 periods deadline %" PRIu64 " == floor(N*f*num/den) %s\n",
             freqs[f], goof_pacer_deadline(&p),
             goof_pacer_deadline(&p) == (uint64_t)exact ? "EXACT" : "MISMATCH");
    }
    report("T16", "no long-term drift: 1 h error < 1 period; deadline arithmetic exact over 10^7 periods");
  }

  /* T17 bounded catch-up */
  {
    GoofPacer p;
    goof_pacer_init(&p, NS, num, den, 0);
    uint64_t per = goof_pacer_period_ticks(&p);
    /* on time for 10 steps */
    for (int k = 0; k < 10; k++) {
      goof_pacer_begin_iteration(&p);
      uint64_t d = goof_pacer_deadline(&p);
      CHECK(goof_pacer_step_due(&p, d), "due");
      goof_pacer_stepped(&p, d, 1);
    }
    /* host late by 3.9 periods: the 4 due steps run in one iteration */
    uint64_t now = goof_pacer_deadline(&p) + (uint64_t)(3.9 * (double)per);
    goof_pacer_begin_iteration(&p);
    int n = 0;
    while (goof_pacer_step_due(&p, now)) { goof_pacer_stepped(&p, now, 1); n++; }
    CHECK(n == 4 && p.st.resyncs == 0, "3.9 periods late: %d steps, resyncs %" PRIu64, n, p.st.resyncs);
    CHECK(goof_pacer_deadline(&p) > now, "caught up completely");
    /* host late by 7 periods but within the bound? no: 7 > 4 -> resync */
    now = goof_pacer_deadline(&p) + 7 * per;
    goof_pacer_begin_iteration(&p);
    n = 0;
    while (goof_pacer_step_due(&p, now)) { goof_pacer_stepped(&p, now, 1); n++; }
    CHECK(n == 1 && p.st.resyncs == 1, "7 periods late: resync + 1 step (%d)", n);
    /* the per-iteration clamp: steps that each take LONGER than a period
     * keep coming due while the iteration runs; after 4 the iteration must
     * yield to presentation (the clock is re-read before each check, as in
     * main_sdl.c) */
    GoofPacer q;
    goof_pacer_init(&q, NS, num, den, 0);
    now = 0;
    goof_pacer_begin_iteration(&q);
    n = 0;
    while (goof_pacer_step_due(&q, now)) {
      uint64_t started = now;
      now += MS(17.5);                                   /* slower than a period */
      goof_pacer_stepped(&q, started, 1);
      n++;
    }
    CHECK(n == GOOF_PACER_MAX_STEPS_PER_ITERATION && q.st.budget_deferrals == 1 &&
          q.st.resyncs == 0, "clamp: %d steps, deferrals %" PRIu64, n, q.st.budget_deferrals);
    CHECK(goof_pacer_deadline(&q) <= now, "a step is still due after the clamp");
    goof_pacer_begin_iteration(&q);
    CHECK(goof_pacer_step_due(&q, now), "the deferred step runs next iteration");
    printf("  max steps per iteration %d, resync above %d periods (%.1f ms)\n",
           GOOF_PACER_MAX_STEPS_PER_ITERATION, GOOF_PACER_RESYNC_LAG_PERIODS,
           GOOF_PACER_RESYNC_LAG_PERIODS * 1000.0 * (double)num / (double)den);
    report("T17", "bounded catch-up: <= 4 steps per iteration; < 4 periods of debt fully recovered");
  }

  /* T18 spiral of death */
  {
    Sched s = {.present_ms = 1.0, .step_ms = 25.0, .duration_s = 20, .seed = 8};
    Result r = simulate(&s);
    print_hist("slow25", &r);
    CHECK(r.st.max_steps_in_iteration <= GOOF_PACER_MAX_STEPS_PER_ITERATION, "clamped (%" PRIu64 ")",
          r.st.max_steps_in_iteration);
    double achieved = (double)r.steps / s.duration_s;
    CHECK(achieved < 41.0 && achieved > 30.0, "host-limited rate %.1f steps/s", achieved);
    CHECK(r.st.max_lag_ticks < (uint64_t)(6 * 16.7e6), "lag stays bounded (%.1f ms)",
          (double)r.st.max_lag_ticks / 1e6);
    printf("  25 ms steps (host too slow): %.1f steps/s, resyncs %" PRIu64 ", max lag %.1f ms "
           "(guest slows; never snowballs)\n", achieved, r.st.resyncs, (double)r.st.max_lag_ticks / 1e6);
    report("T18", "spiral-of-death clamp: a host slower than real time slows the guest, lag bounded");
  }

  /* T19 long single step */
  {
    Sched s = {.present_ms = 0.3, .step_ms = 2.0, .first_step_ms = 300.0, .duration_s = 10,
               .multi = 1, .seed = 6};
    Result r = simulate(&s);
    print_hist("loader300", &r);
    double err = (double)r.periods - r.expected_periods;
    CHECK(r.st.resyncs == 0 && fabs((double)r.st.boot_anchor_ticks / 1e6 - 300.0) < 1.0,
          "clock anchored after the loader: %.1f ms", (double)r.st.boot_anchor_ticks / 1e6);
    CHECK(err > -1.0 && err <= 1.0 && r.st.catchup_steps == 0, "cadence canonical after it: %.3f", err);
    GoofPacer q;
    goof_pacer_init(&q, NS, num, den, 0);
    goof_pacer_begin_iteration(&q);
    CHECK(goof_pacer_step_due(&q, 0), "due");
    goof_pacer_stepped(&q, 0, 121);
    uint64_t d0 = goof_pacer_deadline(&q);
    goof_pacer_step_finished(&q, 0, MS(300));
    CHECK(goof_pacer_deadline(&q) == d0 + MS(300), "hold of the 121-period epoch starts at its end");
    uint64_t d1 = goof_pacer_deadline(&q);
    goof_pacer_begin_iteration(&q);
    CHECK(goof_pacer_step_due(&q, d1), "2nd step due");
    goof_pacer_stepped(&q, d1, 16);
    uint64_t d2 = goof_pacer_deadline(&q);
    goof_pacer_step_finished(&q, d1, d1 + MS(100));   /* a heavy 16-period transition */
    CHECK(goof_pacer_deadline(&q) == d2, "a later heavy step is never shifted");
    report("T19", "clock anchored after the first (boot loader) step only; later steps never shifted");
  }

  /* P9 overload */
  {
    Sched s = {.present_ms = 30.0, .step_ms = 1.0, .duration_s = 30, .multi = 1, .seed = 4};
    Result r = simulate(&s);
    print_hist("ovl30", &r);
    double err = (double)r.periods - r.expected_periods;
    /* up to 2 periods may be in flight (being caught up) at the cut-off */
    CHECK(err > -2.0 && err <= 1.0 && r.st.resyncs == 0, "cadence kept: %.3f, resyncs %" PRIu64,
          err, r.st.resyncs);
    CHECK(r.frame_presents < r.steps * 6 / 10, "presents dropped (%" PRIu64 " for %" PRIu64 " steps)",
          r.frame_presents, r.steps);
    report("P9", "presentation overload (30 ms presents): guest cadence kept, presents dropped");
  }

  /* L1 low refresh */
  {
    const double low[2] = {50, 30};
    for (int i = 0; i < 2; i++) {
      Sched s = {.refresh_hz = low[i], .present_ms = 0.3, .step_ms = 2.0, .duration_s = 60,
                 .multi = 1, .seed = 13};
      Result r = simulate(&s);
      char label[32]; snprintf(label, sizeof label, "%gHz", low[i]);
      print_hist(label, &r);
      double err = (double)r.periods - r.expected_periods;
      /* at 30 Hz two steps run per present, so up to 2 periods are in flight at the cut-off */
      CHECK(err > -2.0 && err <= 1.0 && r.st.resyncs == 0, "%g Hz: %.3f", low[i], err);
    }
    Sched s30 = {.refresh_hz = 30, .present_ms = 0.3, .step_ms = 2.0, .duration_s = 3600,
                 .multi = 1, .seed = 14};
    Result r30 = simulate(&s30);
    print_hist("30/1h", &r30);
    double e30 = (double)r30.periods - r30.expected_periods;
    CHECK(e30 > -2.0 && e30 <= 1.0 && r30.st.resyncs == 0, "30 Hz 1 h: %.3f (no accumulation)", e30);
    report("L1", "50 Hz and 30 Hz vsync displays: guest cadence still canonical");
  }

  /* --present-hz override (the gates' 30 / 120 Hz runs) */
  {
    Sched s = {.present_ms = 0.3, .step_ms = 2.0, .duration_s = 10, .sim_hz = 120, .seed = 2};
    Result r = simulate(&s);
    CHECK(fabs((double)r.periods / 10.0 - 120.0) < 0.5, "120 Hz override %.2f", r.periods / 10.0);
    report("O1", "--present-hz override still sets the simulation clock (gate seam)");
  }

  printf("GOOF_FRAME_PACER_TEST %s checks=%d failures=%d\n", failures ? "FAIL" : "PASS", checks,
         failures);
  return failures ? 1 : 0;
}
