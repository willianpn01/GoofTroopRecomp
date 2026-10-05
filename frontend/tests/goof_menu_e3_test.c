/* GOOF_ENHANCEMENTS_E3 -- expanded Settings menu (Input + Video + Audio),
 * SDL-free.
 *
 * Drives goof_menu with the same GoofUiEvents sdl_settings.c produces and a
 * fake host that plays the frontend's part: committed effective values, a
 * live "presentation" that previews change, and ONE stored config that every
 * Save serialises whole (like main_sdl.c's settings_persist).  The guest
 * pause / no-leak half of S7 / S8 is proven on the real goof_recomp by
 * prototypes/e3/settings_e3_e2e.py.
 *
 *   S1  F2 opens Settings: INPUT, VIDEO, AUDIO, RESUME GAME (an E2-style menu
 *       without attached host pages still shows only INPUT + RESUME GAME)
 *   S2  Input opens and the full E2 flow works through the larger root
 *       (capture arm/release, bind, Save applies live)
 *   S3  Video opens, shows the committed values
 *   S4  Audio opens, shows the committed values
 *   S5  Esc / Back returns one level from every page; Esc on root closes
 *   S6  controller navigation: D-pad moves, D-pad left/right changes values,
 *       bottom confirms, right goes back
 *   S7  every key / pad event is consumed while any page is open
 *   S8  values change only the working copy + live preview; the committed
 *       values (what Save writes) stay until Save
 *   S9  RESUME GAME (last item, also Up-wrap from INPUT) closes
 *   S10 unsaved page: Back asks; NO stays; YES restores the live preview;
 *       CANCEL CHANGES restores; F2 close restores + reports the discard
 *   S11 Save commits (host save called with the working copy), persists,
 *       reports SAVED; reopening shows the saved values
 *   S12 per-page reset (Video / Audio / Input): confirm NO keeps, YES loads
 *       only that page's defaults; the other sections are untouched
 *   S13 Input bindings survive Video and Audio saves (stored file bytes)
 *   S14 Video survives an Input save
 *   S15 Audio survives an Input save
 *   S16 preview refused (fullscreen unavailable): display steps back,
 *       warning shown, nothing saved
 *   S17 value controls: volume 5 % steps clamped 0..100, Enter on volume
 *       changes nothing; toggles; scale cycle AUTO..4X; --scale note shown
 *   S18 draw list: Video / Audio pages inside the 42x22 grid with values */
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

/* ---- fake frontend ---------------------------------------------------- */
typedef struct {
  GoofBindings live;              /* live input bindings                    */
  GoofVideoSettings video;        /* committed effective                    */
  GoofAudioSettings audio;
  GoofVideoSettings shown;        /* what the "window" shows right now      */
  GoofAudioSettings heard;        /* what the host gain uses right now      */
  GoofConfig stored;              /* the one stored model                   */
  char file[65536];               /* last serialisation ("the file")        */
  int saves, video_saves, audio_saves, input_saves, previews_v, previews_a;
  bool refuse_fullscreen;
} Front;

static Front F;

static void persist(void) {
  goof_config_serialize(&F.stored, F.file, sizeof F.file);
  F.saves++;
}
static GoofSaveResult save_input(void *u, const GoofBindings *b, char *msg, size_t cap) {
  (void)u;
  F.live = *b; F.stored.bindings = *b; F.input_saves++;
  persist();
  snprintf(msg, cap, "SAVED - ACTIVE NOW");
  return GOOF_SAVE_PERSISTED;
}
static bool preview_video(void *u, const GoofVideoSettings *v) {
  (void)u;
  F.previews_v++;
  if (v->display == GOOF_DISPLAY_FULLSCREEN && F.refuse_fullscreen) {
    F.shown = *v; F.shown.display = GOOF_DISPLAY_WINDOWED;
    return false;
  }
  F.shown = *v;
  return true;
}
static void preview_audio(void *u, const GoofAudioSettings *a) {
  (void)u; F.previews_a++; F.heard = *a;
}
static GoofSaveResult save_video(void *u, const GoofVideoSettings *v, char *msg, size_t cap) {
  (void)u;
  F.video = *v; F.shown = *v; F.stored.video = *v; F.video_saves++;
  persist();
  snprintf(msg, cap, "SAVED - ACTIVE NOW");
  return GOOF_SAVE_PERSISTED;
}
static GoofSaveResult save_audio(void *u, const GoofAudioSettings *a, char *msg, size_t cap) {
  (void)u;
  F.audio = *a; F.heard = *a; F.stored.audio = *a; F.audio_saves++;
  persist();
  snprintf(msg, cap, "SAVED - ACTIVE NOW");
  return GOOF_SAVE_PERSISTED;
}
static void describe(void *u, const GoofVideoSettings *v, char *out, size_t cap) {
  (void)u;
  int w, h;
  goof_video_window_size(v->aspect, v->window_scale ? v->window_scale : 4, &w, &h);
  snprintf(out, cap, "WINDOW %dX%d", w, h);
}

static GoofMenu m;
static const char *note = "";

static void reset_all(bool attach) {
  memset(&F, 0, sizeof F);
  goof_config_defaults(&F.stored);
  F.live = F.stored.bindings;
  F.video = F.shown = F.stored.video;
  F.audio = F.heard = F.stored.audio;
  goof_menu_init(&m, &F.live, (GoofSettingsHost){NULL, save_input, "~/.config/x"});
  if (attach)
    goof_menu_attach_host_pages(&m, (GoofHostPages){
        .user = NULL, .video = &F.video, .audio = &F.audio,
        .preview_video = preview_video, .preview_audio = preview_audio,
        .save_video = save_video, .save_audio = save_audio,
        .describe_video = describe, .video_note = note});
}

static unsigned ev(GoofUiEventType t, uint16_t code, int32_t dev) {
  GoofUiEvent e = {t, code, false, dev};
  return goof_menu_handle(&m, &e);
}
static unsigned key(uint16_t code) {
  unsigned r = ev(GOOF_UI_KEY_DOWN, code, -1);
  return r | ev(GOOF_UI_KEY_UP, code, -1);
}
static unsigned pad(uint16_t code) {
  unsigned r = ev(GOOF_UI_PAD_DOWN, code, 7);
  return r | ev(GOOF_UI_PAD_UP, code, 7);
}
static void down(int n) { for (int i = 0; i < n; i++) key(GOOF_UI_SC_DOWN); }
static void enter(void) { key(GOOF_UI_SC_RETURN); }
static void yes(void) { key(GOOF_UI_SC_LEFT); enter(); }   /* default NO -> YES */

enum { R_INPUT, R_VIDEO, R_AUDIO, R_RESUME };
/* GOOF_ENHANCEMENTS_E4 inserted VSYNC after FULLSCREEN SCALING (the only E4
 * changes to this test: this enum and one extra Up in S10). */
enum { VI_DISPLAY, VI_SCALE, VI_ASPECT, VI_SCALING, VI_VSYNC, VI_FILTER, VI_RESET, VI_SAVE, VI_CANCEL,
       VI_BACK };
enum { AU_VOLUME, AU_MUTE, AU_MUTE_UNFOCUSED, AU_RESET, AU_SAVE, AU_CANCEL, AU_BACK };
enum { IN_P1, IN_P2, IN_RESET, IN_SAVE, IN_DISCARD, IN_BACK };
enum { ED_CHANGE_KEY, ED_CHANGE_PAD, ED_CLEAR_KEY, ED_CLEAR_PAD, ED_SAVE, ED_UNDO, ED_BACK };

/* Opens Settings and enters a root section by real navigation. */
static void open_section(int item) {
  /* Not on the root page: close (F2 drops nothing that matters here --
   * every caller saved or wants the discard) and reopen. */
  if (m.open && goof_menu_page(&m) != GOOF_PAGE_ROOT) key(GOOF_UI_SC_F2);
  if (!m.open) key(GOOF_UI_SC_F2);
  for (int guard = 0; m.sel[GOOF_PAGE_ROOT] != item && guard < 8; guard++) key(GOOF_UI_SC_DOWN);
  enter();
}
static void select_item(GoofMenuPageId page, int item) {
  for (int guard = 0; m.sel[page] != item && guard < 32; guard++) key(GOOF_UI_SC_DOWN);
  if (m.sel[page] != item) { failures++; printf("  FAIL cannot select item %d\n", item); }
}

static GoofUiDrawList dl;
static bool draw_has(const char *needle) {
  goof_menu_draw(&m, &dl);
  for (int i = 0; i < dl.ntext; i++)
    if (strstr(dl.text[i].text, needle)) return true;
  return false;
}
static bool draw_in_bounds(void) {
  goof_menu_draw(&m, &dl);
  for (int i = 0; i < dl.ntext; i++) {
    int col = (dl.text[i].x - GOOF_UI_ORIGIN_X) / GOOF_UI_CELL_W;
    if (col + (int)strlen(dl.text[i].text) > GOOF_UI_COLS) return false;
    if (dl.text[i].y + 7 > GOOF_UI_CANVAS_H) return false;
  }
  for (int i = 0; i < dl.nrect; i++)
    if (dl.rect[i].x < 0 || dl.rect[i].y < 0 || dl.rect[i].x + dl.rect[i].w > GOOF_UI_CANVAS_W ||
        dl.rect[i].y + dl.rect[i].h > GOOF_UI_CANVAS_H) return false;
  return dl.ntext < GOOF_UI_MAX_TEXTS && dl.nrect < GOOF_UI_MAX_RECTS;
}

/* The stored file's text from "[video]" on / up to "[video]". */
static const char *video_tail(const char *file) { const char *p = strstr(file, "\n[video]\n"); return p ? p : ""; }
static void input_head(const char *file, char *out, size_t cap) {
  const char *a = strstr(file, "[meta]"), *b = strstr(file, "\n[video]\n");
  size_t n = (a && b) ? (size_t)(b - a) : 0;
  if (n >= cap) n = cap - 1;
  memcpy(out, a ? a : "", n);
  out[n] = '\0';
}

static char head1[65536], head2[65536], tail1[4096];

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  GoofVideoSettings vdef; goof_video_defaults(&vdef);
  GoofAudioSettings adef; goof_audio_defaults(&adef);

  /* S1 */
  reset_all(true);
  unsigned r = key(GOOF_UI_SC_F2);
  CHECK((r & GOOF_MENU_OPENED) && goof_menu_page(&m) == GOOF_PAGE_ROOT, "F2 opens root");
  CHECK(draw_has("INPUT") && draw_has("VIDEO") && draw_has("AUDIO") && draw_has("RESUME GAME") &&
        draw_has("THE GAME IS PAUSED"), "root items");
  CHECK(!draw_has("ENHANCEMENTS"), "no empty Enhancements page");
  down(4);
  CHECK(m.sel[GOOF_PAGE_ROOT] == 0, "4 root items (wraps after 4)");
  reset_all(false);
  key(GOOF_UI_SC_F2);
  CHECK(draw_has("INPUT") && !draw_has("VIDEO") && !draw_has("AUDIO"), "E2-style menu unchanged");
  down(2);
  CHECK(m.sel[GOOF_PAGE_ROOT] == 0, "E2-style: 2 root items");
  report("S1", "F2 opens Settings: INPUT / VIDEO / AUDIO / RESUME GAME (E2-style init unchanged)");

  /* S2 */
  reset_all(true);
  open_section(R_INPUT);
  CHECK(goof_menu_page(&m) == GOOF_PAGE_INPUT, "INPUT page");
  enter();                                        /* PLAYER 1 */
  down(GOOF_ACT_B);
  enter();                                        /* EDIT B */
  CHECK(goof_menu_page(&m) == GOOF_PAGE_EDIT && m.action == GOOF_ACT_B, "EDIT B");
  ev(GOOF_UI_KEY_DOWN, GOOF_UI_SC_RETURN, -1);    /* CHANGE KEYBOARD (press) */
  CHECK(m.capture == GOOF_CAPTURE_ARMING, "arming");
  ev(GOOF_UI_KEY_DOWN, 20, -1);                   /* Q while arming: ignored */
  CHECK(m.capture == GOOF_CAPTURE_ARMING, "press while arming ignored");
  ev(GOOF_UI_KEY_UP, GOOF_UI_SC_RETURN, -1);
  CHECK(m.capture == GOOF_CAPTURE_ARMED, "armed on release");
  key(20);                                        /* Q */
  CHECK(m.working.player[0].list[GOOF_BIND_KEY][GOOF_ACT_B].code[0] == 20 &&
        F.live.player[0].list[GOOF_BIND_KEY][GOOF_ACT_B].code[0] != 20, "working only");
  select_item(GOOF_PAGE_EDIT, ED_SAVE);
  r = key(GOOF_UI_SC_RETURN);
  CHECK((r & GOOF_MENU_SAVED) && F.input_saves == 1 &&
        F.live.player[0].list[GOOF_BIND_KEY][GOOF_ACT_B].code[0] == 20, "Save applies live");
  CHECK(strstr(F.file, "[input.player1.keyboard]\nup=UP\ndown=DOWN\nleft=LEFT\nright=RIGHT\nb=Q\n") != NULL,
        "persisted");
  report("S2", "Input opens; E2 capture (arm on release) + Save work through the larger root");

  /* S3 */
  reset_all(true);
  F.video = F.shown = F.stored.video = (GoofVideoSettings){GOOF_DISPLAY_WINDOWED, 2, GOOF_ASPECT_8_7,
                                                           GOOF_SCALING_FIT, false};
  open_section(R_VIDEO);
  CHECK(goof_menu_page(&m) == GOOF_PAGE_VIDEO, "VIDEO page");
  CHECK(draw_has("SETTINGS > VIDEO") && draw_has("DISPLAY MODE") && draw_has("< WINDOWED >") &&
        draw_has("WINDOW SCALE") && draw_has("< 2X >") && draw_has("PIXEL ASPECT") &&
        draw_has("< SNES 8:7 >") && draw_has("FULLSCREEN SCALING") && draw_has("< FIT >") &&
        draw_has("RESET VIDEO DEFAULTS") && draw_has("SAVE") && draw_has("CANCEL CHANGES") &&
        draw_has("WINDOW 585X448"), "video items + committed values");
  CHECK(F.previews_v == 0, "opening previews nothing");
  report("S3", "Video opens with the committed values and a geometry line");

  /* S4 */
  reset_all(true);
  F.audio = F.heard = F.stored.audio = (GoofAudioSettings){65, false, true};
  open_section(R_AUDIO);
  CHECK(goof_menu_page(&m) == GOOF_PAGE_AUDIO, "AUDIO page");
  CHECK(draw_has("SETTINGS > AUDIO") && draw_has("MASTER VOLUME") && draw_has("< 65% >") &&
        draw_has("MUTE") && draw_has("< OFF >") && draw_has("MUTE WHEN UNFOCUSED") &&
        draw_has("< ON >") && draw_has("RESET AUDIO DEFAULTS") && draw_has("SAVE, THEN RESUME"),
        "audio items + committed values");
  report("S4", "Audio opens with the committed values");

  /* S5 */
  reset_all(true);
  for (int sec = R_INPUT; sec <= R_AUDIO; sec++) {
    open_section(sec);
    CHECK(goof_menu_page(&m) != GOOF_PAGE_ROOT, "in section %d", sec);
    key(GOOF_UI_SC_ESCAPE);
    CHECK(goof_menu_page(&m) == GOOF_PAGE_ROOT && m.open, "Esc back from section %d", sec);
    open_section(sec);
    select_item(goof_menu_page(&m), sec == R_INPUT ? IN_BACK : (sec == R_VIDEO ? VI_BACK : AU_BACK));
    enter();
    CHECK(goof_menu_page(&m) == GOOF_PAGE_ROOT && m.open, "BACK item from section %d", sec);
  }
  r = key(GOOF_UI_SC_ESCAPE);
  CHECK(!m.open && (r & GOOF_MENU_CLOSED), "Esc on root closes");
  report("S5", "Esc / BACK return one level from Input, Video, Audio; Esc on root closes");

  /* S6 */
  reset_all(true);
  key(GOOF_UI_SC_F2);
  pad(GOOF_HOST_BTN_DPAD_DOWN);
  CHECK(m.sel[GOOF_PAGE_ROOT] == R_VIDEO, "D-pad down");
  pad(GOOF_HOST_BTN_SOUTH);
  CHECK(goof_menu_page(&m) == GOOF_PAGE_VIDEO, "bottom confirms -> VIDEO");
  pad(GOOF_HOST_BTN_DPAD_DOWN);                   /* WINDOW SCALE */
  pad(GOOF_HOST_BTN_DPAD_RIGHT);
  CHECK(m.video_working.window_scale == 4 && F.shown.window_scale == 4, "D-pad right: 3X -> 4X");
  pad(GOOF_PAD_LSTICK_LEFT);
  CHECK(m.video_working.window_scale == 3, "stick left: back to 3X");
  pad(GOOF_HOST_BTN_EAST);                        /* back (not dirty now) */
  CHECK(goof_menu_page(&m) == GOOF_PAGE_ROOT, "right goes back");
  pad(GOOF_HOST_BTN_DPAD_DOWN);
  pad(GOOF_HOST_BTN_SOUTH);
  CHECK(goof_menu_page(&m) == GOOF_PAGE_AUDIO, "AUDIO by pad");
  pad(GOOF_HOST_BTN_DPAD_LEFT);
  CHECK(m.audio_working.master_volume == 95 && F.heard.master_volume == 95, "D-pad left: -5%%");
  pad(GOOF_HOST_BTN_EAST);
  CHECK(m.confirm == GOOF_CONFIRM_AUDIO_DISCARD_BACK, "dirty back asks");
  pad(GOOF_HOST_BTN_DPAD_LEFT);                   /* -> YES */
  pad(GOOF_HOST_BTN_SOUTH);
  CHECK(goof_menu_page(&m) == GOOF_PAGE_ROOT && F.heard.master_volume == 100, "pad YES discards");
  report("S6", "controller: D-pad/stick move + change values, bottom confirms, right backs");

  /* S7 */
  reset_all(true);
  bool all = true;
  for (int sec = R_VIDEO; sec <= R_AUDIO; sec++) {
    open_section(sec);
    static const uint16_t keys[] = {4, 29, 27, 44, GOOF_UI_SC_P, 225, 228, 40};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
      unsigned f = ev(GOOF_UI_KEY_DOWN, keys[i], -1) & ev(GOOF_UI_KEY_UP, keys[i], -1);
      all = all && (f & GOOF_MENU_CONSUMED);
      if (m.confirm != GOOF_CONFIRM_NONE) key(GOOF_UI_SC_ESCAPE);
      if (!m.open || goof_menu_page(&m) == GOOF_PAGE_ROOT) open_section(sec);
    }
    for (uint16_t b = 0; b < GOOF_HOST_BTN_COUNT; b++) {
      if (b == GOOF_HOST_BTN_SOUTH || b == GOOF_HOST_BTN_EAST) continue;
      unsigned f = ev(GOOF_UI_PAD_DOWN, b, 3) & ev(GOOF_UI_PAD_UP, b, 3);
      all = all && (f & GOOF_MENU_CONSUMED);
    }
  }
  CHECK(all, "every event consumed on Video / Audio");
  report("S7", "every key and pad event is consumed while Video / Audio are open");

  /* S8 */
  reset_all(true);
  open_section(R_VIDEO);
  key(GOOF_UI_SC_RIGHT);                          /* DISPLAY -> FULLSCREEN */
  CHECK(m.video_working.display == GOOF_DISPLAY_FULLSCREEN &&
        F.shown.display == GOOF_DISPLAY_FULLSCREEN && F.video.display == GOOF_DISPLAY_WINDOWED &&
        F.saves == 0, "live preview, committed unchanged, nothing written");
  CHECK(draw_has("DISPLAY MODE*") && draw_has("UNSAVED CHANGES"), "changed marker + unsaved");
  select_item(GOOF_PAGE_VIDEO, VI_ASPECT);
  key(GOOF_UI_SC_RIGHT);
  CHECK(F.shown.aspect == GOOF_ASPECT_8_7 && F.video.aspect == GOOF_ASPECT_SQUARE, "aspect preview");
  report("S8", "changes preview live; committed values and the file stay until Save");

  /* S9 */
  reset_all(true);
  key(GOOF_UI_SC_F2);
  key(GOOF_UI_SC_UP);
  CHECK(m.sel[GOOF_PAGE_ROOT] == R_RESUME, "Up wraps to RESUME");
  r = key(GOOF_UI_SC_RETURN);
  CHECK(!m.open && (r & GOOF_MENU_CLOSED), "RESUME GAME closes");
  report("S9", "RESUME GAME (4th item, Up-wrap) closes the overlay");

  /* S10 */
  reset_all(true);
  open_section(R_VIDEO);
  key(GOOF_UI_SC_RIGHT);                          /* fullscreen preview */
  key(GOOF_UI_SC_ESCAPE);
  CHECK(m.confirm == GOOF_CONFIRM_VIDEO_DISCARD_BACK && draw_has("DISCARD UNSAVED VIDEO CHANGES?"),
        "Back asks");
  enter();                                        /* NO (default) */
  CHECK(goof_menu_page(&m) == GOOF_PAGE_VIDEO && F.shown.display == GOOF_DISPLAY_FULLSCREEN,
        "NO keeps the preview and the page");
  key(GOOF_UI_SC_ESCAPE);
  yes();
  CHECK(goof_menu_page(&m) == GOOF_PAGE_ROOT && F.shown.display == GOOF_DISPLAY_WINDOWED &&
        goof_video_equal(&F.shown, &F.video) && F.saves == 0, "YES restores the committed values");
  open_section(R_VIDEO);
  select_item(GOOF_PAGE_VIDEO, VI_SCALE);
  key(GOOF_UI_SC_LEFT);                           /* 3X -> 2X */
  CHECK(F.shown.window_scale == 2, "scale preview");
  select_item(GOOF_PAGE_VIDEO, VI_CANCEL);
  enter();
  CHECK(F.shown.window_scale == 3 && m.video_working.window_scale == 3 &&
        draw_has("VIDEO CHANGES CANCELLED") && goof_menu_page(&m) == GOOF_PAGE_VIDEO, "CANCEL restores");
  key(GOOF_UI_SC_UP); key(GOOF_UI_SC_UP); key(GOOF_UI_SC_UP); key(GOOF_UI_SC_UP);
  key(GOOF_UI_SC_UP);
  key(GOOF_UI_SC_UP);                             /* CANCEL -> WINDOW SCALE (E4: +1, VSYNC) */
  key(GOOF_UI_SC_UP);                             /* E5: FILTER */
  CHECK(m.sel[GOOF_PAGE_VIDEO] == VI_SCALE, "at scale");
  key(GOOF_UI_SC_RIGHT);
  r = key(GOOF_UI_SC_F2);
  CHECK(!m.open && (r & GOOF_MENU_CLOSED) && m.discarded_on_close &&
        F.shown.window_scale == 3 && F.saves == 0, "F2 close restores + reports discard");
  open_section(R_AUDIO);
  key(GOOF_UI_SC_LEFT); key(GOOF_UI_SC_LEFT);     /* 90% */
  select_item(GOOF_PAGE_AUDIO, AU_MUTE);
  enter();
  CHECK(F.heard.master_volume == 90 && F.heard.mute, "audio preview");
  select_item(GOOF_PAGE_AUDIO, AU_CANCEL);
  enter();
  CHECK(goof_audio_equal(&F.heard, &adef) && goof_audio_equal(&m.audio_working, &adef),
        "audio CANCEL restores");
  key(GOOF_UI_SC_UP); key(GOOF_UI_SC_UP); key(GOOF_UI_SC_UP); key(GOOF_UI_SC_UP);
  key(GOOF_UI_SC_UP);                             /* MASTER VOLUME */
  key(GOOF_UI_SC_LEFT);
  key(GOOF_UI_SC_F2);
  CHECK(goof_audio_equal(&F.heard, &adef) && m.discarded_on_close && F.saves == 0,
        "audio F2 close restores");
  report("S10", "unsaved page: Back asks (NO stays / YES restores), CANCEL restores, F2 close restores");

  /* S11 */
  reset_all(true);
  open_section(R_VIDEO);
  key(GOOF_UI_SC_RIGHT);                          /* fullscreen */
  select_item(GOOF_PAGE_VIDEO, VI_ASPECT);
  key(GOOF_UI_SC_RIGHT);                          /* 8:7 */
  select_item(GOOF_PAGE_VIDEO, VI_SAVE);
  r = key(GOOF_UI_SC_RETURN);
  CHECK((r & GOOF_MENU_SAVED) && F.video_saves == 1 && F.video.display == GOOF_DISPLAY_FULLSCREEN &&
        F.video.aspect == GOOF_ASPECT_8_7 && !goof_menu_video_dirty(&m) && draw_has("SAVED"),
        "video Save commits");
  CHECK(strstr(F.file, "display_mode=fullscreen\n") && strstr(F.file, "pixel_aspect=8:7\n"),
        "video persisted");
  key(GOOF_UI_SC_F2);
  CHECK(!m.discarded_on_close && F.shown.display == GOOF_DISPLAY_FULLSCREEN, "close keeps saved");
  open_section(R_VIDEO);
  CHECK(draw_has("< FULLSCREEN >") && draw_has("< SNES 8:7 >"), "reopen shows saved");
  key(GOOF_UI_SC_ESCAPE);
  open_section(R_AUDIO);
  key(GOOF_UI_SC_LEFT);                           /* 95 */
  select_item(GOOF_PAGE_AUDIO, AU_MUTE_UNFOCUSED);
  key(GOOF_UI_SC_RIGHT);
  select_item(GOOF_PAGE_AUDIO, AU_SAVE);
  r = key(GOOF_UI_SC_RETURN);
  CHECK((r & GOOF_MENU_SAVED) && F.audio_saves == 1 && F.audio.master_volume == 95 &&
        F.audio.mute_when_unfocused && strstr(F.file, "master_volume=95\n") &&
        strstr(F.file, "mute_when_unfocused=on\n"), "audio Save commits + persists");
  report("S11", "Save commits + persists Video and Audio; reopening shows saved values");

  /* S12 */
  reset_all(true);
  F.video = F.shown = F.stored.video = (GoofVideoSettings){GOOF_DISPLAY_FULLSCREEN, 1, GOOF_ASPECT_8_7,
                                                           GOOF_SCALING_FIT, false, GOOF_FILTER_SCANLINES};
  F.audio = F.heard = F.stored.audio = (GoofAudioSettings){30, true, true};
  goof_bindlist_set_single(&F.live.player[0].list[GOOF_BIND_KEY][GOOF_ACT_B], 20);
  F.stored.bindings = F.live;
  GoofBindings custom = F.live;
  GoofAudioSettings custom_a = F.audio;
  GoofVideoSettings custom_v = F.video;
  open_section(R_VIDEO);
  select_item(GOOF_PAGE_VIDEO, VI_RESET);
  enter();
  CHECK(m.confirm == GOOF_CONFIRM_VIDEO_RESET && draw_has("RESET VIDEO TO DEFAULTS?"), "asks");
  enter();                                        /* NO */
  CHECK(goof_video_equal(&m.video_working, &custom_v), "NO keeps");
  enter(); yes();
  CHECK(goof_video_equal(&m.video_working, &vdef) && goof_video_equal(&F.shown, &vdef) &&
        draw_has("VIDEO DEFAULTS LOADED"), "YES loads video defaults (previewed)");
  CHECK(goof_bindings_equal(&m.working, &custom) && goof_audio_equal(&m.audio_working, &custom_a) &&
        goof_audio_equal(&F.heard, &custom_a), "video reset leaves input + audio");
  select_item(GOOF_PAGE_VIDEO, VI_SAVE);
  enter();
  CHECK(goof_video_equal(&F.stored.video, &vdef) && goof_bindings_equal(&F.stored.bindings, &custom) &&
        goof_audio_equal(&F.stored.audio, &custom_a), "saved: only video reset");
  /* E5: exercise Audio reset with a non-default committed filter. */
  F.video = F.shown = F.stored.video = custom_v;
  key(GOOF_UI_SC_ESCAPE);
  open_section(R_AUDIO);
  select_item(GOOF_PAGE_AUDIO, AU_RESET);
  enter(); yes();
  CHECK(goof_audio_equal(&m.audio_working, &adef) && draw_has("AUDIO DEFAULTS LOADED"), "audio reset");
  select_item(GOOF_PAGE_AUDIO, AU_SAVE);
  enter();
  CHECK(goof_audio_equal(&F.stored.audio, &adef) && goof_bindings_equal(&F.stored.bindings, &custom) &&
        goof_video_equal(&F.stored.video, &custom_v), "saved: only audio reset (filter kept)");
  /* Input reset leaves video + audio. */
  F.video = F.shown = F.stored.video = custom_v;
  F.audio = F.heard = F.stored.audio = custom_a;
  key(GOOF_UI_SC_F2);
  open_section(R_INPUT);
  select_item(GOOF_PAGE_INPUT, IN_RESET);
  enter(); yes();
  select_item(GOOF_PAGE_INPUT, IN_SAVE);
  enter();
  GoofBindings e1; goof_bindings_defaults(&e1);
  CHECK(goof_bindings_equal(&F.stored.bindings, &e1) && goof_video_equal(&F.stored.video, &custom_v) &&
        goof_audio_equal(&F.stored.audio, &custom_a), "input reset leaves video + audio");
  report("S12", "per-page reset (Video / Audio / Input) changes only its own section");

  /* S13 / S14 / S15 */
  reset_all(true);
  open_section(R_INPUT);
  enter(); down(GOOF_ACT_Y); enter();             /* EDIT Y */
  select_item(GOOF_PAGE_EDIT, ED_CLEAR_PAD);
  enter();
  select_item(GOOF_PAGE_EDIT, ED_SAVE);
  enter();
  input_head(F.file, head1, sizeof head1);
  CHECK(strstr(head1, "[input.player1.gamepad]") && strstr(head1, "\ny=none\n"), "input saved");
  key(GOOF_UI_SC_F2);
  open_section(R_VIDEO);
  select_item(GOOF_PAGE_VIDEO, VI_SCALING);
  key(GOOF_UI_SC_RIGHT);
  select_item(GOOF_PAGE_VIDEO, VI_SAVE);
  enter();
  input_head(F.file, head2, sizeof head2);
  CHECK(strcmp(head1, head2) == 0 && strstr(F.file, "fullscreen_scaling=fit\n"), "S13 video save");
  key(GOOF_UI_SC_ESCAPE);
  open_section(R_AUDIO);
  select_item(GOOF_PAGE_AUDIO, AU_MUTE);
  enter();
  select_item(GOOF_PAGE_AUDIO, AU_SAVE);
  enter();
  input_head(F.file, head2, sizeof head2);
  CHECK(strcmp(head1, head2) == 0 && strstr(F.file, "\nmute=on\n"), "S13 audio save");
  report("S13", "Input bindings byte-identical in the file after Video and Audio saves");
  snprintf(tail1, sizeof tail1, "%s", video_tail(F.file));
  key(GOOF_UI_SC_F2);
  open_section(R_INPUT);
  enter(); down(GOOF_ACT_A); enter();             /* EDIT A */
  select_item(GOOF_PAGE_EDIT, ED_CLEAR_KEY);
  enter();
  select_item(GOOF_PAGE_EDIT, ED_SAVE);
  enter();
  CHECK(F.input_saves == 2 && strcmp(tail1, video_tail(F.file)) == 0 &&
        strstr(tail1, "fullscreen_scaling=fit\n"), "S14 video after input save");
  report("S14", "Video settings byte-identical in the file after an Input save");
  CHECK(strstr(video_tail(F.file), "\n[audio]\nmaster_volume=100\nmute=on\n") != NULL,
        "S15 audio after input save");
  report("S15", "Audio settings byte-identical in the file after an Input save");

  /* S16 */
  reset_all(true);
  F.refuse_fullscreen = true;
  open_section(R_VIDEO);
  key(GOOF_UI_SC_RIGHT);
  CHECK(m.video_working.display == GOOF_DISPLAY_WINDOWED && F.shown.display == GOOF_DISPLAY_WINDOWED &&
        draw_has("DISPLAY CHANGE FAILED") && F.saves == 0, "refused fullscreen steps back");
  report("S16", "fullscreen refused by the host: stays windowed, warning, nothing written");

  /* S17 */
  reset_all(true);
  open_section(R_AUDIO);
  enter();                                        /* Enter on volume */
  CHECK(m.audio_working.master_volume == 100 && draw_has("USE LEFT/RIGHT"), "Enter on volume");
  key(GOOF_UI_SC_RIGHT);
  CHECK(m.audio_working.master_volume == 100, "clamped at 100");
  for (int i = 0; i < 25; i++) key(GOOF_UI_SC_LEFT);
  CHECK(m.audio_working.master_volume == 0 && draw_has("< 0% >"), "clamped at 0");
  key(GOOF_UI_SC_RIGHT);
  CHECK(m.audio_working.master_volume == 5, "5%% step");
  key(GOOF_UI_SC_F2);
  open_section(R_VIDEO);
  select_item(GOOF_PAGE_VIDEO, VI_SCALE);
  int seen[6];
  for (int i = 0; i < 6; i++) { seen[i] = m.video_working.window_scale; enter(); }
  CHECK(seen[0] == 3 && seen[1] == 4 && seen[2] == 0 && seen[3] == 1 && seen[4] == 2 && seen[5] == 3,
        "Enter cycles 3,4,AUTO,1,2,3");
  key(GOOF_UI_SC_F2);
  note = "WINDOW SCALE FROM --SCALE 6 (SESSION)";
  reset_all(true);
  F.video.window_scale = 6;
  goof_menu_attach_host_pages(&m, m.pages);
  open_section(R_VIDEO);
  CHECK(draw_has("< 6X >") && draw_has("FROM --SCALE 6"), "CLI scale + note shown");
  note = "";
  report("S17", "volume 5% steps clamped 0..100; Enter on volume no-op; scale cycle; --scale note");

  /* S18 */
  reset_all(true);
  bool inb = true;
  key(GOOF_UI_SC_F2);
  inb = inb && draw_in_bounds();
  open_section(R_VIDEO);
  inb = inb && draw_in_bounds();
  key(GOOF_UI_SC_RIGHT);
  key(GOOF_UI_SC_ESCAPE);                         /* confirm modal */
  inb = inb && draw_in_bounds();
  key(GOOF_UI_SC_ESCAPE);
  open_section(R_AUDIO);
  inb = inb && draw_in_bounds();
  CHECK(inb, "pages + modal inside the 42x22 grid and draw-list capacity");
  report("S18", "Video / Audio pages and modals render inside the grid");

  printf("GOOF_MENU_E3_TEST %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
