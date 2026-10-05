/* GOOF_ENHANCEMENTS_E3 -- config.ini compatibility and [video] / [audio]
 * sections, SDL-free.
 *
 *   K1  real E2-written files (made by the E2 build, prototypes/e3/
 *       config_compat.sh) load under E3: LOADED, no warning, every binding
 *       identical (E3's input part re-serialises to E2's exact bytes),
 *       video / audio = defaults
 *   K2  the hand-written E2 bizarre fixture loads with the same warnings E2
 *       gave and video / audio defaults
 *   K3  missing [video] -> video defaults; missing [audio] -> audio defaults;
 *       a partial section keeps the defaults of the keys it omits
 *   K4  a full E3 file loads every value (case-insensitive values)
 *   K5  invalid values / unknown keys / duplicates in [video] / [audio]:
 *       one warning each, that key keeps its default, the rest is used
 *   K6  deterministic round-trip over every video / audio combination
 *   K7  one writer: save -> reload keeps bindings, video and audio together;
 *       changing one section and saving leaves the other two byte-identical
 *   K8  per-section defaults are isolated (resetting video leaves input and
 *       audio; resetting audio leaves input and video; resetting input
 *       leaves video and audio)
 *   K9  E3 output: every [video] / [audio] line is a known key with a
 *       canonical value; no device identity anywhere
 *   K10 schema stays 1; schema 2 still rejected with everything at defaults
 *   K11 writes SCRATCH/e3_full.ini for the E2-reads-E3 check
 *
 * usage: goof_config_e3_test FIXTURE_DIR SCRATCH_DIR */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/goof_config.h"

static int failures, case_failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; case_failures++; \
  printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void report(const char *name, const char *what) {
  printf("%s %s %s\n", name, case_failures ? "FAIL" : "PASS", what);
  case_failures = 0;
}

static char warnings[16384];
static int nwarn;
static void collect(void *user, const char *msg) {
  (void)user;
  nwarn++;
  size_t n = strlen(warnings);
  snprintf(warnings + n, sizeof warnings - n, "%s\n", msg);
}
static void clear_warn(void) { warnings[0] = '\0'; nwarn = 0; }

static GoofConfigLoadStatus parse(const char *text, GoofConfig *c) {
  clear_warn();
  return goof_config_parse(text, strlen(text), c, collect, NULL);
}

static size_t read_file(const char *path, char *buf, size_t cap) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  size_t n = fread(buf, 1, cap - 1, f);
  buf[n] = '\0';
  fclose(f);
  return n;
}

/* The text from "[meta]" up to (not including) the blank line before
 * "[video]" -- or to the end when there is no [video]. */
static void input_part(const char *text, char *out, size_t cap) {
  const char *a = strstr(text, "[meta]");
  if (!a) { out[0] = '\0'; return; }
  const char *b = strstr(a, "\n\n[video]");
  size_t n = b ? (size_t)(b - a) + 1 : strlen(a);
  if (n >= cap) n = cap - 1;
  memcpy(out, a, n);
  out[n] = '\0';
}

static bool video_is_default(const GoofVideoSettings *v) {
  GoofVideoSettings d;
  goof_video_defaults(&d);
  return goof_video_equal(v, &d);
}
static bool audio_is_default(const GoofAudioSettings *a) {
  GoofAudioSettings d;
  goof_audio_defaults(&d);
  return goof_audio_equal(a, &d);
}

static char buf[65536], buf2[65536], part1[65536], part2[65536];

int main(int argc, char **argv) {
  if (argc != 3) { printf("usage: goof_config_e3_test FIXTURE_DIR SCRATCH_DIR\n"); return 2; }
  const char *fix = argv[1], *scratch = argv[2];
  char path[1024];
  GoofConfig c, def;
  goof_config_defaults(&def);
  GoofBindings e1;
  goof_bindings_defaults(&e1);

  /* K1 */
  static const char *const kE2Files[] = {"e2_default.ini", "e2_custom.ini"};
  for (int i = 0; i < 2; i++) {
    snprintf(path, sizeof path, "%s/%s", fix, kE2Files[i]);
    size_t n = read_file(path, buf, sizeof buf);
    CHECK(n > 0, "fixture %s missing", path);
    clear_warn();
    GoofConfigLoadStatus st = goof_config_load_file(path, &c, collect, NULL);
    CHECK(st == GOOF_CONFIG_LOADED && nwarn == 0, "%s: status %d, %d warnings:\n%s",
          kE2Files[i], (int)st, nwarn, warnings);
    CHECK(video_is_default(&c.video) && audio_is_default(&c.audio), "%s: video/audio defaults",
          kE2Files[i]);
    CHECK(!strstr(buf, "[video]") && !strstr(buf, "[audio]"), "fixture is E2-only");
    size_t m = goof_config_serialize(&c, buf2, sizeof buf2);
    input_part(buf, part1, sizeof part1);
    input_part(buf2, part2, sizeof part2);
    CHECK(m && strlen(part1) > 100 && strcmp(part1, part2) == 0,
          "%s: E3 input part != E2 bytes\n--E2--\n%s\n--E3--\n%s", kE2Files[i], part1, part2);
    if (i == 0) CHECK(goof_bindings_equal(&c.bindings, &e1), "E2 default file == E1 bindings");
    else CHECK(!goof_bindings_equal(&c.bindings, &e1) &&
               c.bindings.player[0].list[GOOF_BIND_KEY][GOOF_ACT_B].count == 2 &&
               c.bindings.player[1].list[GOOF_BIND_KEY][GOOF_ACT_SELECT].count == 0,
               "custom remap present");
  }
  report("K1", "E2-written files load under E3: no warning, bindings byte-identical, video/audio defaults");

  /* K2 */
  snprintf(path, sizeof path, "%s/e2_bizarre.ini", fix);
  clear_warn();
  GoofConfigLoadStatus st = goof_config_load_file(path, &c, collect, NULL);
  CHECK(st == GOOF_CONFIG_LOADED, "bizarre loads");
  CHECK(strstr(warnings, "unknown key 'future_meta_key' in [meta]") &&
        strstr(warnings, "unknown section [future.section]") &&
        strstr(warnings, "unknown key 'turbo'") && strstr(warnings, "'not_a_button'"),
        "E2 warnings kept:\n%s", warnings);
  CHECK(!strstr(warnings, "[video]") && !strstr(warnings, "[audio]"), "no video/audio warnings");
  CHECK(c.bindings.player[0].list[GOOF_BIND_KEY][GOOF_ACT_UP].code[0] == 22 /* S */,
        "bizarre P1 up = S");
  CHECK(video_is_default(&c.video) && audio_is_default(&c.audio), "bizarre: video/audio defaults");
  report("K2", "E2 bizarre fixture: same E2 warnings, bindings used, video/audio defaults");

  /* K3 */
  st = parse("[meta]\nschema=1\n[input.player1.keyboard]\nb=Q\n", &c);
  CHECK(st == GOOF_CONFIG_LOADED && nwarn == 0 && video_is_default(&c.video) &&
        audio_is_default(&c.audio), "input only -> defaults");
  st = parse("[meta]\nschema=1\n[video]\npixel_aspect=8:7\n", &c);
  CHECK(st == GOOF_CONFIG_LOADED && nwarn == 0 && c.video.aspect == GOOF_ASPECT_8_7 &&
        c.video.display == GOOF_DISPLAY_WINDOWED && c.video.window_scale == 3 &&
        c.video.scaling == GOOF_SCALING_INTEGER && audio_is_default(&c.audio) &&
        goof_bindings_equal(&c.bindings, &e1), "partial video");
  st = parse("[meta]\nschema=1\n[audio]\nmute=on\n", &c);
  CHECK(st == GOOF_CONFIG_LOADED && nwarn == 0 && c.audio.mute && c.audio.master_volume == 100 &&
        !c.audio.mute_when_unfocused && video_is_default(&c.video), "partial audio");
  st = parse("[meta]\nschema=1\n[video]\n[audio]\n", &c);
  CHECK(st == GOOF_CONFIG_LOADED && nwarn == 0 && video_is_default(&c.video) &&
        audio_is_default(&c.audio), "empty sections");
  report("K3", "missing / partial [video] / [audio] -> defaults for what is missing");

  /* K4 */
  st = parse("[meta]\nschema=1\n"
             "[audio]\nmaster_volume=35\nmute=ON\nmute_when_unfocused=yes\n"
             "[video]\ndisplay_mode=FullScreen\nwindow_scale=AUTO\npixel_aspect=8:7\n"
             "fullscreen_scaling=Fit\n"
             "[input.player2.keyboard]\nb=Q\n", &c);
  CHECK(st == GOOF_CONFIG_LOADED && nwarn == 0, "full: %d warnings\n%s", nwarn, warnings);
  CHECK(c.video.display == GOOF_DISPLAY_FULLSCREEN && c.video.window_scale == 0 &&
        c.video.aspect == GOOF_ASPECT_8_7 && c.video.scaling == GOOF_SCALING_FIT, "video values");
  CHECK(c.audio.master_volume == 35 && c.audio.mute && c.audio.mute_when_unfocused, "audio values");
  CHECK(c.bindings.player[1].list[GOOF_BIND_KEY][GOOF_ACT_B].code[0] == 20, "P2 b = Q");
  report("K4", "full E3 file loads all values, any section order, case-insensitive values");

  /* K5 */
  st = parse("[meta]\nschema=1\n"
             "[video]\ndisplay_mode=exclusive\nwindow_scale=7\npixel_aspect=8:7\n"
             "pixel_aspect=square\nrefresh_rate=144\nfullscreen_scaling=\n"
             "[audio]\nmaster_volume=150\nmute=maybe\nmute_when_unfocused=on\ndevice=hdmi\n"
             "[input.player1.keyboard]\nb=Q\n", &c);
  CHECK(st == GOOF_CONFIG_LOADED, "loaded");
  CHECK(c.video.display == GOOF_DISPLAY_WINDOWED && c.video.window_scale == 3 &&
        c.video.aspect == GOOF_ASPECT_8_7 && c.video.scaling == GOOF_SCALING_INTEGER,
        "bad video values keep defaults, good one used, first duplicate wins");
  CHECK(c.audio.master_volume == 100 && !c.audio.mute && c.audio.mute_when_unfocused,
        "bad audio values keep defaults, good one used");
  CHECK(c.bindings.player[0].list[GOOF_BIND_KEY][GOOF_ACT_B].code[0] == 20, "bindings still used");
  CHECK(strstr(warnings, "invalid [video] display_mode 'exclusive'") &&
        strstr(warnings, "invalid [video] window_scale '7'") &&
        strstr(warnings, "duplicate 'pixel_aspect' in [video]") &&
        /* E4: 'vsync' became a known key, so an unknown one stands in */
        strstr(warnings, "unknown key 'refresh_rate' in [video]") &&
        strstr(warnings, "invalid [video] fullscreen_scaling ''") &&
        strstr(warnings, "invalid [audio] master_volume '150'") &&
        strstr(warnings, "invalid [audio] mute 'maybe'") &&
        strstr(warnings, "unknown key 'device' in [audio]"), "warnings:\n%s", warnings);
  CHECK(nwarn == 8, "exactly one warning per problem (%d)", nwarn);
  report("K5", "invalid / unknown / duplicate video+audio keys: one warning each, default kept");

  /* K6 */
  int combos = 0;
  for (int d = 0; d < GOOF_DISPLAY_COUNT; d++)
    for (int s = 0; s <= GOOF_WINDOW_SCALE_MAX; s++)
      for (int a = 0; a < GOOF_ASPECT_COUNT; a++)
        for (int f = 0; f < GOOF_SCALING_COUNT; f++)
          for (int v = 0; v <= 100; v += 25)
            for (int mu = 0; mu < 4; mu++) {
              GoofConfig x;
              goof_config_defaults(&x);
              x.video = (GoofVideoSettings){(GoofDisplayMode)d, s, (GoofPixelAspect)a,
                                            (GoofScalingMode)f, false};
              x.audio = (GoofAudioSettings){v, (mu & 1) != 0, (mu & 2) != 0};
              size_t n1 = goof_config_serialize(&x, buf, sizeof buf);
              GoofConfig y;
              clear_warn();
              GoofConfigLoadStatus s2 = goof_config_parse(buf, n1, &y, collect, NULL);
              size_t n2 = goof_config_serialize(&y, buf2, sizeof buf2);
              combos++;
              CHECK(s2 == GOOF_CONFIG_LOADED && nwarn == 0 && n1 == n2 &&
                    memcmp(buf, buf2, n1) == 0 && goof_video_equal(&x.video, &y.video) &&
                    goof_audio_equal(&x.audio, &y.audio) &&
                    goof_bindings_equal(&x.bindings, &y.bindings), "combo %d", combos);
              if (failures > 10) goto k6_done;
            }
k6_done:
  printf("  %d video/audio combinations round-tripped\n", combos);
  report("K6", "deterministic round-trip for every video/audio combination");

  /* K7 */
  GoofConfig all;
  goof_config_defaults(&all);
  goof_bindlist_set_single(&all.bindings.player[0].list[GOOF_BIND_KEY][GOOF_ACT_B], 20);
  all.video = (GoofVideoSettings){GOOF_DISPLAY_FULLSCREEN, 2, GOOF_ASPECT_8_7, GOOF_SCALING_FIT,
                                  false};
  all.audio = (GoofAudioSettings){40, true, true};
  snprintf(path, sizeof path, "%s/k7/config.ini", scratch);
  char err[256];
  CHECK(goof_config_save_atomic(path, &all, err, sizeof err), "save: %s", err);
  GoofConfig back;
  CHECK(goof_config_load_file(path, &back, NULL, NULL) == GOOF_CONFIG_LOADED &&
        goof_bindings_equal(&back.bindings, &all.bindings) &&
        goof_video_equal(&back.video, &all.video) && goof_audio_equal(&back.audio, &all.audio),
        "all three survive save/reload");
  read_file(path, buf, sizeof buf);
  /* Video-only change + save: the input part and the audio part unchanged. */
  back.video.window_scale = 4;
  CHECK(goof_config_save_atomic(path, &back, err, sizeof err), "save 2");
  read_file(path, buf2, sizeof buf2);
  input_part(buf, part1, sizeof part1);
  input_part(buf2, part2, sizeof part2);
  CHECK(strcmp(part1, part2) == 0, "video save keeps the input bytes");
  CHECK(strcmp(strstr(buf, "\n[audio]\n"), strstr(buf2, "\n[audio]\n")) == 0, "video save keeps audio");
  CHECK(strstr(buf2, "window_scale=4\n") != NULL, "video change written");
  /* Input-only change + save: video + audio bytes unchanged. */
  memcpy(buf, buf2, sizeof buf);
  goof_bindlist_set_single(&back.bindings.player[1].list[GOOF_BIND_KEY][GOOF_ACT_A], 21);
  CHECK(goof_config_save_atomic(path, &back, err, sizeof err), "save 3");
  read_file(path, buf2, sizeof buf2);
  CHECK(strcmp(strstr(buf, "\n[video]"), strstr(buf2, "\n[video]")) == 0,
        "input save keeps video + audio bytes");
  report("K7", "one writer: bindings + video + audio persist together; single-section saves isolate");

  /* K8 */
  GoofConfig r = all;
  goof_video_defaults(&r.video);
  CHECK(goof_bindings_equal(&r.bindings, &all.bindings) && goof_audio_equal(&r.audio, &all.audio) &&
        video_is_default(&r.video), "video reset isolated");
  r = all;
  goof_audio_defaults(&r.audio);
  CHECK(goof_bindings_equal(&r.bindings, &all.bindings) && goof_video_equal(&r.video, &all.video) &&
        audio_is_default(&r.audio), "audio reset isolated");
  r = all;
  goof_bindings_defaults(&r.bindings);
  CHECK(goof_video_equal(&r.video, &all.video) && goof_audio_equal(&r.audio, &all.audio) &&
        goof_bindings_equal(&r.bindings, &e1), "input reset isolated");
  report("K8", "per-section defaults are isolated at the model level");

  /* K9 */
  size_t n = goof_config_serialize(&all, buf, sizeof buf);
  /* Values only: the ';' header comment is prose. */
  part1[0] = '\0';
  for (const char *l = buf; *l;) {
    const char *e = strchr(l, '\n');
    size_t len = e ? (size_t)(e - l) + 1 : strlen(l);
    if (*l != ';') strncat(part1, l, len);
    l += len;
  }
  CHECK(n && !strstr(part1, "instance") && !strstr(part1, "guid") && !strstr(part1, "which") &&
        !strstr(part1, "device"), "no device identity");
  const char *vs = strstr(buf, "\n[video]\n");
  CHECK(vs && strcmp(vs, "\n[video]\ndisplay_mode=fullscreen\nwindow_scale=2\npixel_aspect=8:7\n"
                         "fullscreen_scaling=fit\nvsync=off\nfilter=nearest\n\n[audio]\nmaster_volume=40\nmute=on\n"
                         "mute_when_unfocused=on\n") == 0, "canonical tail:\n%s", vs ? vs : "(none)");
  n = goof_config_serialize(&def, buf, sizeof buf);
  vs = strstr(buf, "\n[video]\n");
  CHECK(vs && strcmp(vs, "\n[video]\ndisplay_mode=windowed\nwindow_scale=3\npixel_aspect=square\n"
                         "fullscreen_scaling=integer\nvsync=off\nfilter=nearest\n\n[audio]\nmaster_volume=100\nmute=off\n"
                         "mute_when_unfocused=off\n") == 0, "default tail:\n%s", vs ? vs : "(none)");
  report("K9", "canonical [video]/[audio] output, known keys only, no device identity");

  /* K10 */
  CHECK(strstr(buf, "[meta]\nschema=1\n") != NULL, "still schema 1");
  st = parse("[meta]\nschema=2\n[video]\ndisplay_mode=fullscreen\n[audio]\nmute=on\n", &c);
  CHECK(st == GOOF_CONFIG_REJECTED && video_is_default(&c.video) && audio_is_default(&c.audio) &&
        goof_bindings_equal(&c.bindings, &e1), "schema 2 rejected, all defaults");
  report("K10", "schema stays 1; a future schema is still rejected (defaults, file untouched)");

  /* K11 */
  snprintf(path, sizeof path, "%s/e3_full.ini", scratch);
  CHECK(goof_config_save_atomic(path, &all, err, sizeof err), "e3_full: %s", err);
  report("K11", "wrote e3_full.ini for the E2-build forward-compatibility check");

  printf("GOOF_CONFIG_E3_TEST %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
