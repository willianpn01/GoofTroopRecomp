/* GOOF_ENHANCEMENTS_E4 -- [video] vsync key and the Settings > Video VSYNC item.
 * SDL-FREE (host/goof_config + host/ui/goof_menu with a fake frontend).
 *
 *   Y1  default OFF (= E3); an E3 file without the key loads OFF, no warning
 *   Y2  vsync=on/off (and the audio page's boolean spellings) parse; an
 *       invalid value warns once and keeps OFF; a duplicate warns
 *   Y3  serialisation: "vsync=off|on" right after fullscreen_scaling;
 *       round-trip for both values x every other video value
 *   Y4  the E3 writer contract still holds: an input-only save keeps the
 *       vsync byte; a vsync-only change leaves the input bytes untouched
 *   Y5  Video page: VSYNC item between FULLSCREEN SCALING and RESET, shows
 *       < OFF > / < ON >, Enter / Left / Right toggle it, preview is live
 *       (the host gets vsync=on) while the committed value and the file stay
 *   Y6  CANCEL / F2-close revert the preview; SAVE commits + persists;
 *       RESET VIDEO DEFAULTS -> OFF
 *   Y7  a host that refuses the vsync change: the page steps back and says
 *       "VSYNC CHANGE FAILED", nothing is written
 *   Y8  no simulation-rate / FPS / interpolation item exists on the page, and
 *       the hint "VSYNC NEVER CHANGES THE GAME SPEED" is drawn inside the grid */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host/goof_config.h"
#include "host/ui/goof_menu.h"

static int failures, case_failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; case_failures++; \
  printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void report(const char *name, const char *what) {
  printf("%s %s %s\n", name, case_failures ? "FAIL" : "PASS", what);
  case_failures = 0;
}

static char warnings[8192];
static int nwarn;
static void collect(void *u, const char *msg) {
  (void)u;
  nwarn++;
  strncat(warnings, msg, sizeof warnings - strlen(warnings) - 2);
  strcat(warnings, "\n");
}
static GoofConfigLoadStatus parse(const char *text, GoofConfig *c) {
  warnings[0] = '\0'; nwarn = 0;
  return goof_config_parse(text, strlen(text), c, collect, NULL);
}

/* ---- fake frontend (same shape as goof_menu_e3_test) ------------------- */
typedef struct {
  GoofBindings live;
  GoofVideoSettings video, shown;
  GoofAudioSettings audio;
  GoofConfig stored;
  char file[65536];
  int video_saves, previews_v;
  bool refuse_vsync;
} Front;
static Front F;
static GoofMenu m;

static GoofSaveResult save_input(void *u, const GoofBindings *b, char *msg, size_t cap) {
  (void)u; F.live = *b; F.stored.bindings = *b;
  goof_config_serialize(&F.stored, F.file, sizeof F.file);
  snprintf(msg, cap, "SAVED - ACTIVE NOW");
  return GOOF_SAVE_PERSISTED;
}
static bool preview_video(void *u, const GoofVideoSettings *v) {
  (void)u; F.previews_v++;
  if (v->vsync != F.shown.vsync && F.refuse_vsync) return false;   /* keep shown */
  F.shown = *v;
  return true;
}
static void preview_audio(void *u, const GoofAudioSettings *a) { (void)u; (void)a; }
static GoofSaveResult save_video(void *u, const GoofVideoSettings *v, char *msg, size_t cap) {
  (void)u; F.video = *v; F.shown = *v; F.stored.video = *v; F.video_saves++;
  goof_config_serialize(&F.stored, F.file, sizeof F.file);
  snprintf(msg, cap, "SAVED - ACTIVE NOW");
  return GOOF_SAVE_PERSISTED;
}
static GoofSaveResult save_audio(void *u, const GoofAudioSettings *a, char *msg, size_t cap) {
  (void)u; (void)a; snprintf(msg, cap, "SAVED"); return GOOF_SAVE_PERSISTED;
}
static void reset_all(void) {
  memset(&F, 0, sizeof F);
  goof_config_defaults(&F.stored);
  F.live = F.stored.bindings;
  F.video = F.shown = F.stored.video;
  F.audio = F.stored.audio;
  goof_config_serialize(&F.stored, F.file, sizeof F.file);
  goof_menu_init(&m, &F.live, (GoofSettingsHost){NULL, save_input, "~/.config/x"});
  goof_menu_attach_host_pages(&m, (GoofHostPages){
      .user = NULL, .video = &F.video, .audio = &F.audio,
      .preview_video = preview_video, .preview_audio = preview_audio,
      .save_video = save_video, .save_audio = save_audio,
      .describe_video = NULL, .video_note = ""});
}
static unsigned key(uint16_t code) {
  GoofUiEvent d = {GOOF_UI_KEY_DOWN, code, false, -1}, u = {GOOF_UI_KEY_UP, code, false, -1};
  return goof_menu_handle(&m, &d) | goof_menu_handle(&m, &u);
}
enum { R_INPUT, R_VIDEO, R_AUDIO, R_RESUME };
enum { VI_DISPLAY, VI_SCALE, VI_ASPECT, VI_SCALING, VI_VSYNC, VI_FILTER, VI_RESET, VI_SAVE, VI_CANCEL,
       VI_BACK, VI_COUNT };
static void open_video(void) {
  if (m.open && goof_menu_page(&m) != GOOF_PAGE_ROOT) key(GOOF_UI_SC_F2);
  if (!m.open) key(GOOF_UI_SC_F2);
  for (int g = 0; m.sel[GOOF_PAGE_ROOT] != R_VIDEO && g < 8; g++) key(GOOF_UI_SC_DOWN);
  key(GOOF_UI_SC_RETURN);
}
static void select_item(int item) {
  for (int g = 0; m.sel[GOOF_PAGE_VIDEO] != item && g < 32; g++) key(GOOF_UI_SC_DOWN);
}
static GoofUiDrawList dl;
static bool draw_has(const char *needle) {
  goof_menu_draw(&m, &dl);
  for (int i = 0; i < dl.ntext; i++)
    if (strstr(dl.text[i].text, needle)) return true;
  return false;
}
/* Row (grid y) of the first text containing `needle`, or -1. */
static int draw_row(const char *needle) {
  goof_menu_draw(&m, &dl);
  for (int i = 0; i < dl.ntext; i++)
    if (strstr(dl.text[i].text, needle)) return (dl.text[i].y - GOOF_UI_ORIGIN_Y) / GOOF_UI_CELL_H;
  return -1;
}
static bool draw_in_bounds(void) {
  goof_menu_draw(&m, &dl);
  for (int i = 0; i < dl.ntext; i++) {
    int col = (dl.text[i].x - GOOF_UI_ORIGIN_X) / GOOF_UI_CELL_W;
    if (col + (int)strlen(dl.text[i].text) > GOOF_UI_COLS) return false;
    if (dl.text[i].y + 7 > GOOF_UI_CANVAS_H) return false;
  }
  return dl.ntext < GOOF_UI_MAX_TEXTS;
}

static char buf[65536], buf2[65536];

int main(void) {
  GoofConfig c;
  /* Y1 */
  goof_config_defaults(&c);
  CHECK(!c.video.vsync, "default off");
  GoofVideoSettings vd; goof_video_defaults(&vd);
  CHECK(!vd.vsync, "goof_video_defaults: off");
  GoofConfigLoadStatus st = parse("[meta]\nschema=1\n[video]\ndisplay_mode=fullscreen\nwindow_scale=2\n"
                                  "pixel_aspect=8:7\nfullscreen_scaling=fit\n[audio]\nmute=on\n", &c);
  CHECK(st == GOOF_CONFIG_LOADED && nwarn == 0 && !c.video.vsync &&
        c.video.display == GOOF_DISPLAY_FULLSCREEN, "E3 file: loaded, vsync off, %d warnings", nwarn);
  report("Y1", "default OFF (= E3); an E3 file without the key loads OFF without a warning");

  /* Y2 */
  const char *on[] = {"on", "ON", "true", "yes", "1"}, *off[] = {"off", "Off", "false", "no", "0"};
  for (int i = 0; i < 5; i++) {
    char t[128];
    snprintf(t, sizeof t, "[meta]\nschema=1\n[video]\nvsync=%s\n", on[i]);
    CHECK(parse(t, &c) == GOOF_CONFIG_LOADED && c.video.vsync && nwarn == 0, "on: %s", on[i]);
    snprintf(t, sizeof t, "[meta]\nschema=1\n[video]\nvsync=%s\n", off[i]);
    CHECK(parse(t, &c) == GOOF_CONFIG_LOADED && !c.video.vsync && nwarn == 0, "off: %s", off[i]);
  }
  CHECK(parse("[meta]\nschema=1\n[video]\nvsync=adaptive\n", &c) == GOOF_CONFIG_LOADED &&
        !c.video.vsync && nwarn == 1 && strstr(warnings, "invalid [video] vsync 'adaptive'"),
        "invalid: %s", warnings);
  CHECK(parse("[meta]\nschema=1\n[video]\nvsync=on\nvsync=off\n", &c) == GOOF_CONFIG_LOADED &&
        c.video.vsync && nwarn == 1 && strstr(warnings, "duplicate 'vsync' in [video]"),
        "duplicate: %s", warnings);
  report("Y2", "vsync on/off parse (boolean spellings); invalid and duplicate warn once, default kept");

  /* Y3 */
  goof_config_defaults(&c);
  size_t n = goof_config_serialize(&c, buf, sizeof buf);
  CHECK(n && strstr(buf, "fullscreen_scaling=integer\nvsync=off\nfilter=nearest\n\n[audio]"), "default tail");
  c.video.vsync = true;
  goof_config_serialize(&c, buf, sizeof buf);
  CHECK(strstr(buf, "\nvsync=on\n") != NULL, "on written");
  int combos = 0;
  for (int d = 0; d < GOOF_DISPLAY_COUNT; d++)
    for (int s = 0; s <= GOOF_WINDOW_SCALE_MAX; s++)
      for (int a = 0; a < GOOF_ASPECT_COUNT; a++)
        for (int f = 0; f < GOOF_SCALING_COUNT; f++)
          for (int v = 0; v < 2; v++) {
            GoofConfig x, y;
            goof_config_defaults(&x);
            x.video = (GoofVideoSettings){(GoofDisplayMode)d, s, (GoofPixelAspect)a,
                                          (GoofScalingMode)f, v != 0};
            size_t n1 = goof_config_serialize(&x, buf, sizeof buf);
            warnings[0] = '\0'; nwarn = 0;
            GoofConfigLoadStatus s2 = goof_config_parse(buf, n1, &y, collect, NULL);
            size_t n2 = goof_config_serialize(&y, buf2, sizeof buf2);
            combos++;
            CHECK(s2 == GOOF_CONFIG_LOADED && nwarn == 0 && n1 == n2 && !memcmp(buf, buf2, n1) &&
                  goof_video_equal(&x.video, &y.video), "combo %d", combos);
          }
  printf("  %d video combinations (incl. vsync) round-tripped\n", combos);
  report("Y3", "vsync serialised after fullscreen_scaling; deterministic round-trip");

  /* Y4 */
  goof_config_defaults(&c);
  goof_config_serialize(&c, buf, sizeof buf);
  c.video.vsync = true;
  goof_config_serialize(&c, buf2, sizeof buf2);
  const char *v1 = strstr(buf, "\n[video]\n"), *v2 = strstr(buf2, "\n[video]\n");
  CHECK(v1 && v2 && (v1 - buf) == (v2 - buf2) && !memcmp(buf, buf2, (size_t)(v1 - buf)),
        "vsync change leaves the input bytes identical");
  goof_bindlist_set_single(&c.bindings.player[0].list[GOOF_BIND_KEY][GOOF_ACT_B], 20);
  goof_config_serialize(&c, buf, sizeof buf);
  CHECK(!strcmp(strstr(buf, "\n[video]\n"), v2), "input change keeps [video] (incl. vsync) bytes");
  report("Y4", "one writer: vsync and input sections isolate");

  /* Y5 */
  reset_all();
  open_video();
  CHECK(goof_menu_page(&m) == GOOF_PAGE_VIDEO, "video page");
  int r_scaling = draw_row("FULLSCREEN SCALING"), r_vsync = draw_row("VSYNC"),
      r_reset = draw_row("RESET VIDEO DEFAULTS");
  CHECK(r_scaling >= 0 && r_vsync == r_scaling + 1 && r_reset == r_vsync + 2,
        "VSYNC between FULLSCREEN SCALING (%d) and RESET (%d): %d", r_scaling, r_reset, r_vsync);
  CHECK(draw_has("< OFF >"), "shows OFF");
  select_item(VI_VSYNC);
  CHECK(m.sel[GOOF_PAGE_VIDEO] == VI_VSYNC, "selected");
  char before[65536];
  memcpy(before, F.file, sizeof before);
  key(GOOF_UI_SC_RETURN);
  CHECK(m.video_working.vsync && F.shown.vsync && !F.video.vsync, "Enter toggles + previews live");
  CHECK(draw_has("VSYNC*") && draw_has("< ON >"), "dirty star + ON");
  CHECK(!strcmp(before, F.file) && F.video_saves == 0, "file untouched by the preview");
  key(GOOF_UI_SC_LEFT);
  CHECK(!m.video_working.vsync && !F.shown.vsync, "Left toggles back");
  key(GOOF_UI_SC_RIGHT);
  CHECK(m.video_working.vsync && F.shown.vsync, "Right toggles");
  report("Y5", "VSYNC item: placed after FULLSCREEN SCALING, toggles, previews live, file untouched");

  /* Y6 */
  select_item(VI_CANCEL);
  key(GOOF_UI_SC_RETURN);
  CHECK(!m.video_working.vsync && !F.shown.vsync && F.video_saves == 0, "CANCEL reverts");
  select_item(VI_VSYNC);
  key(GOOF_UI_SC_RETURN);
  CHECK(F.shown.vsync, "previewed again");
  key(GOOF_UI_SC_F2);
  CHECK(!m.open && !F.shown.vsync && F.video_saves == 0, "F2 close reverts the preview");
  open_video();
  select_item(VI_VSYNC);
  key(GOOF_UI_SC_RETURN);
  select_item(VI_SAVE);
  key(GOOF_UI_SC_RETURN);
  CHECK(F.video.vsync && F.shown.vsync && F.video_saves == 1 && strstr(F.file, "\nvsync=on\n"),
        "SAVE commits and persists");
  CHECK(!draw_has("VSYNC*"), "clean after save");
  select_item(VI_RESET);
  key(GOOF_UI_SC_RETURN);
  key(GOOF_UI_SC_LEFT);          /* default NO -> YES */
  key(GOOF_UI_SC_RETURN);
  CHECK(!m.video_working.vsync && !F.shown.vsync, "reset defaults -> OFF (previewed)");
  select_item(VI_SAVE);
  key(GOOF_UI_SC_RETURN);
  CHECK(!F.video.vsync && strstr(F.file, "\nvsync=off\n"), "reset saved");
  report("Y6", "CANCEL / F2 revert; SAVE persists; RESET VIDEO DEFAULTS -> OFF");

  /* Y7 */
  reset_all();
  F.refuse_vsync = true;
  open_video();
  select_item(VI_VSYNC);
  memcpy(before, F.file, sizeof before);
  key(GOOF_UI_SC_RETURN);
  CHECK(!m.video_working.vsync && !F.shown.vsync, "stepped back to OFF");
  CHECK(draw_has("VSYNC CHANGE FAILED"), "status says so");
  CHECK(!strcmp(before, F.file) && F.video_saves == 0, "nothing written");
  report("Y7", "renderer refuses the change: page steps back, VSYNC CHANGE FAILED, nothing written");

  /* Y8 */
  reset_all();
  open_video();
  CHECK(!draw_has("SPEED ") || draw_has("VSYNC NEVER CHANGES THE GAME SPEED"), "hint");
  CHECK(draw_has("VSYNC NEVER CHANGES THE GAME SPEED"), "hint drawn");
  CHECK(!draw_has("FPS") && !draw_has("INTERPOL") && !draw_has("SIMULATION") &&
        !draw_has("GAME RATE"), "no simulation-rate / FPS / interpolation item");
  int items = 0;
  for (int g = 0; g < VI_COUNT + 2; g++) { key(GOOF_UI_SC_DOWN); if (m.sel[GOOF_PAGE_VIDEO] == 0) { items = g + 1; break; } }
  CHECK(items == VI_COUNT, "the page has exactly %d items (%d)", VI_COUNT, items);
  CHECK(draw_in_bounds(), "inside the 42x22 grid");
  report("Y8", "no game-speed / FPS / interpolation option; hint drawn; page inside the grid");

  printf("GOOF_VSYNC_SETTINGS_TEST %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
