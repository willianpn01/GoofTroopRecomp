/* Goof Troop Recomp -- host audio output settings.  SDL-FREE.
 * See goof_audio_settings.h.  GOOF_ENHANCEMENTS_E3_HOST_SETTINGS_UI. */
#include "host/goof_audio_settings.h"

#include <ctype.h>
#include <string.h>

void goof_audio_defaults(GoofAudioSettings *a) {
  a->master_volume = GOOF_VOLUME_DEFAULT;
  a->mute = false;
  a->mute_when_unfocused = false;          /* E2 kept playing unfocused */
}

bool goof_audio_equal(const GoofAudioSettings *x, const GoofAudioSettings *y) {
  return x->master_volume == y->master_volume && x->mute == y->mute &&
         x->mute_when_unfocused == y->mute_when_unfocused;
}

void goof_audio_sanitize(GoofAudioSettings *a) {
  if (a->master_volume < GOOF_VOLUME_MIN) a->master_volume = GOOF_VOLUME_MIN;
  if (a->master_volume > GOOF_VOLUME_MAX) a->master_volume = GOOF_VOLUME_MAX;
}

int32_t goof_audio_gain_q16(const GoofAudioSettings *a, bool focused) {
  if (a->mute || (a->mute_when_unfocused && !focused)) return 0;
  int v = a->master_volume;
  if (v <= GOOF_VOLUME_MIN) return 0;
  if (v >= GOOF_VOLUME_MAX) return GOOF_GAIN_UNITY;
  return (int32_t)(((int64_t)v * GOOF_GAIN_UNITY + GOOF_VOLUME_MAX / 2) / GOOF_VOLUME_MAX);
}

void goof_audio_apply_gain(int16_t *samples, size_t count, int32_t gain_q16) {
  if (gain_q16 >= GOOF_GAIN_UNITY) return;
  if (gain_q16 <= 0) { memset(samples, 0, count * sizeof *samples); return; }
  for (size_t i = 0; i < count; i++)
    samples[i] = (int16_t)(((int32_t)samples[i] * gain_q16) / GOOF_GAIN_UNITY);
}

int goof_audio_volume_step(int volume, int dir) {
  int v = volume + (dir > 0 ? GOOF_VOLUME_STEP : -GOOF_VOLUME_STEP);
  if (v < GOOF_VOLUME_MIN) v = GOOF_VOLUME_MIN;
  if (v > GOOF_VOLUME_MAX) v = GOOF_VOLUME_MAX;
  return v;
}

static bool token_eq(const char *a, const char *b) {
  for (; *a && *b; a++, b++)
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
  return *a == *b;
}

bool goof_bool_from_token(const char *t, bool *out) {
  static const char *const kOn[] = {"on", "true", "yes", "1"};
  static const char *const kOff[] = {"off", "false", "no", "0"};
  for (size_t i = 0; i < sizeof kOn / sizeof kOn[0]; i++) {
    if (token_eq(t, kOn[i])) { *out = true; return true; }
    if (token_eq(t, kOff[i])) { *out = false; return true; }
  }
  return false;
}

bool goof_volume_from_token(const char *t, int *out) {
  int n = 0, digits = 0;
  const char *p = t;
  for (; isdigit((unsigned char)*p); p++) {
    if (++digits > 3) return false;
    n = n * 10 + (*p - '0');
  }
  if (!digits) return false;
  if (*p == '%') p++;
  if (*p != '\0' || n > GOOF_VOLUME_MAX) return false;
  *out = n;
  return true;
}
