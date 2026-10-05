/* GOOF_ENHANCEMENTS_E3 -- host audio output settings, SDL-free.
 *
 *   H1  defaults == E2 output: 100 %, not muted, mute-when-unfocused off
 *   H2  100 % unmuted is unity: the gain stage leaves the samples
 *       bit-identical (and is skipped)
 *   H3  volume scales every sample by exactly v/100 (Q16, truncation)
 *   H4  mute and 0 % write silence (same sample count: timing unchanged)
 *   H5  mute when unfocused: silent only while unfocused and only if enabled;
 *       focus back restores the previous gain (mute / volume kept)
 *   H6  volume steps of 5, clamped to 0..100
 *   H7  tokens: on/off/true/false/yes/no/1/0; volume 0..100 with optional %
 *   H8  sanitize clamps the volume */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host/goof_audio_settings.h"

static int failures, case_failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; case_failures++; \
  printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void report(const char *name, const char *what) {
  printf("%s %s %s\n", name, case_failures ? "FAIL" : "PASS", what);
  case_failures = 0;
}

enum { N = 4096 };
static int16_t src[N], buf[N];

int main(void) {
  uint32_t x = 12345;
  for (int i = 0; i < N; i++) { x = x * 1103515245u + 12345u; src[i] = (int16_t)(x >> 16); }
  src[0] = 32767; src[1] = -32768; src[2] = 0; src[3] = -1;

  GoofAudioSettings a;
  goof_audio_defaults(&a);
  CHECK(a.master_volume == 100 && !a.mute && !a.mute_when_unfocused, "defaults");
  CHECK(goof_audio_gain_q16(&a, true) == GOOF_GAIN_UNITY &&
        goof_audio_gain_q16(&a, false) == GOOF_GAIN_UNITY, "unity focused and unfocused");
  report("H1", "defaults = E2 output (100%, unmuted, keeps playing unfocused)");

  memcpy(buf, src, sizeof buf);
  goof_audio_apply_gain(buf, N, goof_audio_gain_q16(&a, true));
  CHECK(memcmp(buf, src, sizeof buf) == 0, "unity is bit-exact");
  report("H2", "100% unmuted leaves every sample bit-identical");

  int vols[] = {95, 75, 50, 25, 5, 1};
  for (size_t v = 0; v < sizeof vols / sizeof vols[0]; v++) {
    a.master_volume = vols[v];
    int32_t g = goof_audio_gain_q16(&a, true);
    CHECK(g == (int32_t)(((int64_t)vols[v] * 65536 + 50) / 100), "gain %d", vols[v]);
    memcpy(buf, src, sizeof buf);
    goof_audio_apply_gain(buf, N, g);
    int bad = 0;
    for (int i = 0; i < N; i++)
      if (buf[i] != (int16_t)(((int32_t)src[i] * g) / 65536)) bad++;
    CHECK(bad == 0, "volume %d: %d samples wrong", vols[v], bad);
    int32_t peak = 0;
    for (int i = 0; i < N; i++) { int32_t m = buf[i] < 0 ? -buf[i] : buf[i]; if (m > peak) peak = m; }
    CHECK(peak <= (32768 * vols[v]) / 100 + 1, "volume %d peak %d", vols[v], (int)peak);
  }
  a.master_volume = 50;
  memcpy(buf, src, sizeof buf);
  goof_audio_apply_gain(buf, N, goof_audio_gain_q16(&a, true));
  CHECK(buf[0] == 16383 && buf[1] == -16384 && buf[2] == 0 && buf[3] == 0, "50%% edges %d %d %d %d",
        buf[0], buf[1], buf[2], buf[3]);
  report("H3", "volume v scales each sample by v/100 exactly (Q16), no clipping");

  goof_audio_defaults(&a);
  a.mute = true;
  CHECK(goof_audio_gain_q16(&a, true) == 0, "mute gain 0");
  memcpy(buf, src, sizeof buf);
  goof_audio_apply_gain(buf, N, 0);
  int nz = 0;
  for (int i = 0; i < N; i++) nz += buf[i] != 0;
  CHECK(nz == 0, "mute writes silence (%d non-zero)", nz);
  a.mute = false; a.master_volume = 0;
  CHECK(goof_audio_gain_q16(&a, true) == 0, "0%% gain 0");
  a.master_volume = 70; a.mute = true;
  CHECK(goof_audio_gain_q16(&a, true) == 0, "mute wins over volume");
  a.mute = false;
  CHECK(goof_audio_gain_q16(&a, true) == goof_audio_gain_q16(&(GoofAudioSettings){70, false, false}, true),
        "unmute restores 70%%");
  report("H4", "mute / 0% = silence of the same length; unmute restores the volume");

  goof_audio_defaults(&a);
  CHECK(goof_audio_gain_q16(&a, false) == GOOF_GAIN_UNITY, "option off: unfocused plays");
  a.mute_when_unfocused = true;
  a.master_volume = 60;
  int32_t g60 = goof_audio_gain_q16(&a, true);
  CHECK(g60 > 0 && goof_audio_gain_q16(&a, false) == 0, "option on: unfocused silent");
  CHECK(goof_audio_gain_q16(&a, true) == g60, "focus back restores 60%%");
  a.mute = true;
  CHECK(goof_audio_gain_q16(&a, true) == 0, "focus back keeps an explicit mute");
  report("H5", "mute-when-unfocused: silent only unfocused, focus restores previous state");

  CHECK(goof_audio_volume_step(100, +1) == 100 && goof_audio_volume_step(100, -1) == 95 &&
        goof_audio_volume_step(3, -1) == 0 && goof_audio_volume_step(0, +1) == 5 &&
        goof_audio_volume_step(97, +1) == 100, "steps");
  int v = 100;
  for (int i = 0; i < 25; i++) v = goof_audio_volume_step(v, -1);
  CHECK(v == 0, "down to 0");
  report("H6", "volume steps of 5%, clamped 0..100");

  bool b;
  int vol;
  CHECK(goof_bool_from_token("ON", &b) && b && goof_bool_from_token("off", &b) && !b &&
        goof_bool_from_token("True", &b) && b && goof_bool_from_token("no", &b) && !b &&
        goof_bool_from_token("1", &b) && b && goof_bool_from_token("0", &b) && !b, "bools");
  CHECK(!goof_bool_from_token("maybe", &b) && !goof_bool_from_token("", &b), "bad bools");
  CHECK(goof_volume_from_token("0", &vol) && vol == 0 && goof_volume_from_token("100", &vol) &&
        vol == 100 && goof_volume_from_token("35%", &vol) && vol == 35, "volumes");
  CHECK(!goof_volume_from_token("101", &vol) && !goof_volume_from_token("-5", &vol) &&
        !goof_volume_from_token("", &vol) && !goof_volume_from_token("50 %", &vol) &&
        !goof_volume_from_token("1000", &vol) && !goof_volume_from_token("loud", &vol), "bad volumes");
  report("H7", "tokens: on/off (+true/false/yes/no/1/0), volume 0..100 with optional %");

  GoofAudioSettings s = {250, false, false};
  goof_audio_sanitize(&s);
  CHECK(s.master_volume == 100, "clamp high");
  s.master_volume = -3;
  goof_audio_sanitize(&s);
  CHECK(s.master_volume == 0, "clamp low");
  report("H8", "sanitize clamps the volume");

  printf("GOOF_AUDIO_SETTINGS_TEST %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
