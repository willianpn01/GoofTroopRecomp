#ifndef GOOF_AUDIO_SETTINGS_H
#define GOOF_AUDIO_SETTINGS_H

/* Goof Troop Recomp -- host audio output settings.
 * GOOF_ENHANCEMENTS_E3_HOST_SETTINGS_UI.
 *
 * SDL-FREE and guest-free.  Volume and mute act on the HOST copy of the PCM,
 * after the guest produced it and after the canonical native-PCM digest was
 * taken (AUDIO1, at the DSP producer):
 *
 *   DSP -> [AUDIO1 digest] -> core ring -> host FIFO -> stretch
 *       -> goof_audio_apply_gain (HERE) -> SDL_QueueAudio -> device
 *
 * Muting queues silence instead of stopping the device, so queue occupancy,
 * the stretch servo and the guest are exactly as they are unmuted.  At 100 %
 * and unmuted the gain stage is skipped (bit-exact passthrough, E2 output).
 *
 * Output device and latency are NOT settings in E3 (see
 * architecture/E3_HOST_SETTINGS_UI.md section 9: deferred). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  GOOF_VOLUME_MIN = 0, GOOF_VOLUME_MAX = 100, GOOF_VOLUME_DEFAULT = 100,
  GOOF_VOLUME_STEP = 5,
  GOOF_GAIN_UNITY = 65536,         /* Q16 */
};

typedef struct {
  int master_volume;               /* percent, 0..100                        */
  bool mute;
  bool mute_when_unfocused;        /* silence while the window lacks focus   */
} GoofAudioSettings;

void goof_audio_defaults(GoofAudioSettings *a);
bool goof_audio_equal(const GoofAudioSettings *x, const GoofAudioSettings *y);
void goof_audio_sanitize(GoofAudioSettings *a);

/* Linear amplitude gain in Q16 for the current state: 0 when muted (or
 * unfocused with mute_when_unfocused), GOOF_GAIN_UNITY at 100 %. */
int32_t goof_audio_gain_q16(const GoofAudioSettings *a, bool focused);

/* In place, interleaved S16: s = s * gain / 65536 (truncating toward zero,
 * no clipping possible since gain <= unity).  Unity returns without
 * touching the samples; zero writes silence. */
void goof_audio_apply_gain(int16_t *samples, size_t count, int32_t gain_q16);

/* One volume step (+-GOOF_VOLUME_STEP), clamped to 0..100. */
int goof_audio_volume_step(int volume, int dir);

/* Config tokens: on/off (also true/false, yes/no, 1/0 on input). */
bool goof_bool_from_token(const char *t, bool *out);
/* 0..100, optional trailing '%'. */
bool goof_volume_from_token(const char *t, int *out);

#endif
