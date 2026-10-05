/* goof_app_headless -- the application coordinator with no presentation
 * layer at all.
 *
 * This target exists to make gate V11 a link-level fact rather than a
 * convention: it links the same goof_app_core archive the interactive player
 * links, it is built even when GOOF_BUILD_FRONTEND is OFF, and it must show
 * no SDL / X11 / GL dependency under ldd.  It is also the subject of the
 * headless build validations V3 and V4, which configure and build with no
 * SDL development package reachable at all.
 *
 * It drives the guest only through goof_app / goof_compare.  There is no
 * second loop here.
 *
 * usage: goof_app_headless ROM [--epochs N] [--record PATH] [--quiet]
 *                             [--input-script PATH] [--discover]
 *
 * --discover reports the neutral checkpoint table instead of asserting it
 * (the E1000 gate's discovery mode), for runs across a chronology change.
 *
 * --input-script is the SDL-free half of GOOF_INPUT_V1: it feeds the same
 * goof_app_step input parameter the interactive player feeds from a real
 * keyboard, which is what makes gate INPUT7 (headless == frontend for one
 * script) a statement about the coordinator rather than about SDL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "goof_app.h"
#include "goof_compare.h"
#include "goof_input.h"
#include "audio_trace.h"

static void usage(void) {
  fprintf(stderr,
          "usage: goof_app_headless ROM [--epochs N] [--record PATH] "
          "[--quiet]\n"
          "                             [--input-script PATH] [--discover]\n");
  exit(2);
}

int main(int argc, char **argv) {
  if (argc < 2 || argv[1][0] == '-') usage();
  setvbuf(stdout, NULL, _IOLBF, 0);

  uint64_t epochs = goof_compare_last_checkpoint_epoch();
  const char *record_path = NULL;
  const char *script_path = NULL;
  bool quiet = false;
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--quiet") == 0) { quiet = true; continue; }
    if (strcmp(argv[i], "--audio-pins-advisory") == 0) {
      goof_compare_set_audio_pins_advisory(true); continue;
    }
    if (strcmp(argv[i], "--discover") == 0) {
      goof_compare_set_discover(true); continue;
    }
    if (i + 1 >= argc) usage();
    if (strcmp(argv[i], "--epochs") == 0) {
      char *end;
      unsigned long long v = strtoull(argv[++i], &end, 10);
      if (*end != '\0' || v == 0) usage();
      epochs = (uint64_t)v;
    } else if (strcmp(argv[i], "--record") == 0) {
      record_path = argv[++i];
    } else if (strcmp(argv[i], "--input-script") == 0) {
      script_path = argv[++i];
    } else {
      usage();
    }
  }

  GoofInputScript script = {0};
  if (script_path) {
    char err[256];
    if (!goof_input_script_load(script_path, &script, err, sizeof err)) {
      fprintf(stderr, "goof_app_headless: input script: %s\n", err);
      return 2;
    }
  }

  FILE *record = NULL;
  if (record_path && !(record = fopen(record_path, "wb"))) {
    fprintf(stderr, "goof_app_headless: cannot write %s\n", record_path);
    goof_input_script_free(&script);
    return 2;
  }

  GoofAppStatus status;
  GoofAppConfig cfg = {.rom_path = argv[1]};
  GoofApp *app = goof_app_create(&cfg, &status);
  if (!app) {
    fprintf(stderr, "goof_app_headless: %s\n", goof_app_status_text(status));
    if (record) fclose(record);
    goof_input_script_free(&script);
    return status == GOOF_APP_ERR_ROM ? 3 : 4;
  }

  /* Dispatched explicitly rather than by passing a possibly-NULL script: with
   * no --input-script this is the historical neutral entry point, unchanged,
   * and neutrality is a different CALL rather than a NULL argument. */
  int rc = script_path
      ? goof_compare_run_scripted(app, epochs, quiet, record, &script)
      : goof_compare_run(app, epochs, quiet, record);
  /* AUDIO1/AUDIO8: the canonical native-PCM digest, taken at the producer
   * (every (L,R) pair dsp_cycle emits, before the output ring's overflow
   * check) and therefore upstream of any host queue, resampler or device.
   * Printed here so the headless gate and the frontend can be compared on
   * the SAME number while neither has an audio device. */
  { AudioTraceStats _ats; audio_trace_get_stats(&_ats);
    printf("AUDIO1_NATIVE_PCM hash=%016" PRIx64 " frames=%" PRIu64 "\n",
           _ats.pcm_hash, _ats.pcm_frames); }
  goof_app_destroy(app);
  goof_input_script_free(&script);
  if (record && fclose(record) != 0) rc = 1;
  return rc;
}
