/* GOOF_ENHANCEMENTS_E2 -- SDL half of the Settings overlay.
 *
 *   S1  SDL event translation: keys by scancode (+ repeat flag), controller
 *       buttons by position with the instance id, left stick as digital
 *       directions with the E1 threshold (press / release / side switch)
 *   S2  routing: closed -> only F2 consumed; open -> every key / pad event
 *       consumed; SDL_QUIT, window and controller ADDED/REMOVED never
 *   S3  keycode-P / keycode-ESC keys on other layouts refused as captures
 *   S4  offscreen render (software renderer, no window): the overlay draws
 *       only inside the canvas, draws text, and leaves the "game" pixels
 *       outside the panel region of a larger target untouched; BMP
 *       snapshots of every page are written for human review
 *
 * usage: goof_settings_sdl_test OUT_DIR */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "host/goof_bindings.h"
#include "sdl_settings.h"

static int failures, case_failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; case_failures++; \
  printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void report(const char *name, const char *what) {
  printf("%s %s %s\n", name, case_failures ? "FAIL" : "PASS", what);
  case_failures = 0;
}

static GoofBindings live;
static GoofSaveResult save_cb(void *u, const GoofBindings *b, char *msg, size_t cap) {
  (void)u; live = *b; snprintf(msg, cap, "SAVED - ACTIVE NOW");
  return GOOF_SAVE_PERSISTED;
}

static SDL_Event key_ev(Uint32 type, SDL_Scancode sc, SDL_Keycode sym, int repeat) {
  SDL_Event e;
  memset(&e, 0, sizeof e);
  e.type = type;
  e.key.keysym.scancode = sc;
  e.key.keysym.sym = sym;
  e.key.repeat = (Uint8)repeat;
  return e;
}
static SDL_Event btn_ev(Uint32 type, int button, SDL_JoystickID id) {
  SDL_Event e;
  memset(&e, 0, sizeof e);
  e.type = type;
  e.cbutton.button = (Uint8)button;
  e.cbutton.which = id;
  return e;
}
static SDL_Event axis_ev(int axis, int value, SDL_JoystickID id) {
  SDL_Event e;
  memset(&e, 0, sizeof e);
  e.type = SDL_CONTROLLERAXISMOTION;
  e.caxis.axis = (Uint8)axis;
  e.caxis.value = (Sint16)value;
  e.caxis.which = id;
  return e;
}
static unsigned tap(GoofSdlSettings *s, SDL_Scancode sc, SDL_Keycode sym) {
  SDL_Event d = key_ev(SDL_KEYDOWN, sc, sym, 0), u = key_ev(SDL_KEYUP, sc, sym, 0);
  return goof_sdl_settings_event(s, &d) | goof_sdl_settings_event(s, &u);
}

static int snap_count;
static void snapshot(GoofSdlSettings *s, SDL_Surface *surf, SDL_Renderer *r,
                     const char *dir, const char *name) {
  /* Fake "game frame": a checker of two mid colours, like a scene. */
  for (int y = 0; y < surf->h; y++)
    for (int x = 0; x < surf->w; x++)
      ((Uint32 *)surf->pixels)[y * (surf->pitch / 4) + x] =
          ((x / 48 + y / 48) & 1) ? 0x00407040u : 0x00a08040u;
  goof_sdl_settings_render(s, r);
  SDL_RenderPresent(r);
  char path[1024];
  snprintf(path, sizeof path, "%s/overlay_%02d_%s.bmp", dir, ++snap_count, name);
  if (SDL_SaveBMP(surf, path) != 0) { CHECK(0, "save %s: %s", path, SDL_GetError()); }
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: goof_settings_sdl_test OUT_DIR\n"); return 2; }
  goof_bindings_defaults(&live);
  GoofSdlSettings s;
  goof_sdl_settings_init(&s, &live, (GoofSettingsHost){NULL, save_cb, "~/.config/GoofTroopRecomp/config.ini"});

  /* S1 */
  GoofUiEvent out[2];
  SDL_Event e = key_ev(SDL_KEYDOWN, SDL_SCANCODE_Q, SDLK_q, 1);
  int n = goof_sdl_settings_translate(&s, &e, out);
  CHECK(n == 1 && out[0].type == GOOF_UI_KEY_DOWN && out[0].code == 20 && out[0].repeat &&
        out[0].device == -1, "key Q");
  e = btn_ev(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_Y, 42);
  n = goof_sdl_settings_translate(&s, &e, out);
  CHECK(n == 1 && out[0].type == GOOF_UI_PAD_UP && out[0].code == GOOF_HOST_BTN_NORTH &&
        out[0].device == 42, "button Y (top) up");
  e = axis_ev(SDL_CONTROLLER_AXIS_LEFTX, 16384, 3);
  CHECK(goof_sdl_settings_translate(&s, &e, out) == 0, "16384 is not beyond the threshold");
  e = axis_ev(SDL_CONTROLLER_AXIS_LEFTX, 16385, 3);
  n = goof_sdl_settings_translate(&s, &e, out);
  CHECK(n == 1 && out[0].type == GOOF_UI_PAD_DOWN && out[0].code == GOOF_PAD_LSTICK_RIGHT, "right");
  e = axis_ev(SDL_CONTROLLER_AXIS_LEFTX, 20000, 3);
  CHECK(goof_sdl_settings_translate(&s, &e, out) == 0, "held: no repeat");
  e = axis_ev(SDL_CONTROLLER_AXIS_LEFTX, -30000, 3);
  n = goof_sdl_settings_translate(&s, &e, out);
  CHECK(n == 2 && out[0].type == GOOF_UI_PAD_DOWN && out[0].code == GOOF_PAD_LSTICK_LEFT &&
        out[1].type == GOOF_UI_PAD_UP && out[1].code == GOOF_PAD_LSTICK_RIGHT,
        "side switch: left down, right up (n=%d)", n);
  e = axis_ev(SDL_CONTROLLER_AXIS_LEFTY, -30000, 4);            /* other pad */
  n = goof_sdl_settings_translate(&s, &e, out);
  CHECK(n == 1 && out[0].code == GOOF_PAD_LSTICK_UP && out[0].device == 4, "pad 4 up");
  e = axis_ev(SDL_CONTROLLER_AXIS_LEFTX, 0, 3);
  n = goof_sdl_settings_translate(&s, &e, out);
  CHECK(n == 1 && out[0].type == GOOF_UI_PAD_UP && out[0].code == GOOF_PAD_LSTICK_LEFT, "centre");
  e = axis_ev(SDL_CONTROLLER_AXIS_RIGHTX, 30000, 3);
  CHECK(goof_sdl_settings_translate(&s, &e, out) == 0, "right stick ignored");
  e = axis_ev(SDL_CONTROLLER_AXIS_LEFTY, 0, 4);
  goof_sdl_settings_translate(&s, &e, out);
  report("S1", "SDL event translation");

  /* S2 */
  SDL_Event quit; memset(&quit, 0, sizeof quit); quit.type = SDL_QUIT;
  SDL_Event win; memset(&win, 0, sizeof win); win.type = SDL_WINDOWEVENT;
  SDL_Event add; memset(&add, 0, sizeof add); add.type = SDL_CONTROLLERDEVICEADDED;
  SDL_Event rem; memset(&rem, 0, sizeof rem); rem.type = SDL_CONTROLLERDEVICEREMOVED;
  e = key_ev(SDL_KEYDOWN, SDL_SCANCODE_P, SDLK_p, 0);
  CHECK(goof_sdl_settings_event(&s, &e) == 0, "closed: P is the pause key, not ours");
  e = key_ev(SDL_KEYDOWN, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, 0);
  CHECK(goof_sdl_settings_event(&s, &e) == 0, "closed: ESC quits as before");
  e = btn_ev(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_START, 1);
  CHECK(goof_sdl_settings_event(&s, &e) == 0, "closed: pad not ours");
  unsigned f = tap(&s, SDL_SCANCODE_F2, SDLK_F2);
  CHECK((f & GOOF_MENU_OPENED) && s.menu.open, "F2 opens");
  CHECK(goof_sdl_settings_event(&s, &quit) == 0 && goof_sdl_settings_event(&s, &win) == 0 &&
        goof_sdl_settings_event(&s, &add) == 0 && goof_sdl_settings_event(&s, &rem) == 0,
        "quit / window / hot-plug pass through while open");
  e = key_ev(SDL_KEYDOWN, SDL_SCANCODE_P, SDLK_p, 0);
  CHECK(goof_sdl_settings_event(&s, &e) & GOOF_MENU_CONSUMED, "open: P consumed (no pause)");
  e = key_ev(SDL_KEYUP, SDL_SCANCODE_Z, SDLK_z, 0);
  CHECK(goof_sdl_settings_event(&s, &e) & GOOF_MENU_CONSUMED, "open: key up consumed");
  e = btn_ev(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_GUIDE, 1);
  CHECK(goof_sdl_settings_event(&s, &e) & GOOF_MENU_CONSUMED, "open: pad consumed");
  e = axis_ev(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 30000, 1);
  CHECK(goof_sdl_settings_event(&s, &e) & GOOF_MENU_CONSUMED, "open: axis consumed");
  f = tap(&s, SDL_SCANCODE_F2, SDLK_F2);
  CHECK((f & GOOF_MENU_CLOSED) && (f & GOOF_MENU_CONSUMED) && !s.menu.open, "F2 closes");
  report("S2", "event routing");

  /* S3: on AZERTY-like layouts the pause key (keycode p) may sit on another
   * scancode; it must not become a binding either. */
  tap(&s, SDL_SCANCODE_F2, SDLK_F2);
  tap(&s, SDL_SCANCODE_RETURN, SDLK_RETURN);       /* INPUT */
  tap(&s, SDL_SCANCODE_RETURN, SDLK_RETURN);       /* PLAYER 1 */
  for (int i = 0; i < GOOF_ACT_B; i++) tap(&s, SDL_SCANCODE_DOWN, SDLK_DOWN);
  tap(&s, SDL_SCANCODE_RETURN, SDLK_RETURN);       /* EDIT B */
  tap(&s, SDL_SCANCODE_RETURN, SDLK_RETURN);       /* CHANGE KEYBOARD, armed */
  CHECK(s.menu.capture == GOOF_CAPTURE_ARMED, "armed");
  e = key_ev(SDL_KEYDOWN, SDL_SCANCODE_SEMICOLON, SDLK_p, 0);
  goof_sdl_settings_event(&s, &e);
  CHECK(s.menu.capture == GOOF_CAPTURE_ARMED &&
        s.menu.working.player[0].list[GOOF_BIND_KEY][GOOF_ACT_B].code[0] == SDL_SCANCODE_Z,
        "keycode p refused");
  e = key_ev(SDL_KEYDOWN, SDL_SCANCODE_Q, SDLK_a, 0);  /* AZERTY: Q position types 'a' */
  goof_sdl_settings_event(&s, &e);
  CHECK(s.menu.capture == GOOF_CAPTURE_NONE &&
        s.menu.working.player[0].list[GOOF_BIND_KEY][GOOF_ACT_B].code[0] == SDL_SCANCODE_Q,
        "binding stores the scancode (position), not the layout letter");
  report("S3", "layout-P / layout-ESC refused; scancode stored");

  /* S4 */
  goof_sdl_settings_destroy(&s);
  goof_bindings_defaults(&live);
  goof_sdl_settings_init(&s, &live, (GoofSettingsHost){NULL, save_cb, "~/.config/GoofTroopRecomp/config.ini"});
  const char *dir = argv[1];
  /* 768x672 = the default --scale 3 window. */
  SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, 768, 672, 32, SDL_PIXELFORMAT_XRGB8888);
  SDL_Renderer *r = surf ? SDL_CreateSoftwareRenderer(surf) : NULL;
  CHECK(surf && r, "software renderer: %s", SDL_GetError());
  if (surf && r) {
    goof_sdl_settings_render(&s, r);
    CHECK(((Uint32 *)surf->pixels)[0] == 0, "closed: draws nothing");
    tap(&s, SDL_SCANCODE_F2, SDLK_F2);
    snapshot(&s, surf, r, dir, "root");
    tap(&s, SDL_SCANCODE_RETURN, SDLK_RETURN);
    snapshot(&s, surf, r, dir, "input");
    tap(&s, SDL_SCANCODE_RETURN, SDLK_RETURN);
    snapshot(&s, surf, r, dir, "player1");
    for (int i = 0; i < GOOF_ACT_B; i++) tap(&s, SDL_SCANCODE_DOWN, SDLK_DOWN);
    tap(&s, SDL_SCANCODE_RETURN, SDLK_RETURN);
    tap(&s, SDL_SCANCODE_DOWN, SDLK_DOWN);         /* CHANGE CONTROLLER */
    snapshot(&s, surf, r, dir, "edit_b");
    SDL_Event d = key_ev(SDL_KEYDOWN, SDL_SCANCODE_RETURN, SDLK_RETURN, 0);
    goof_sdl_settings_event(&s, &d);
    snapshot(&s, surf, r, dir, "capture_arming");
    d = key_ev(SDL_KEYUP, SDL_SCANCODE_RETURN, SDLK_RETURN, 0);
    goof_sdl_settings_event(&s, &d);
    snapshot(&s, surf, r, dir, "capture_armed");
    e = btn_ev(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_Y, 1);
    goof_sdl_settings_event(&s, &e);
    snapshot(&s, surf, r, dir, "captured_conflict");
    tap(&s, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    snapshot(&s, surf, r, dir, "player1_conflict");
    tap(&s, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    tap(&s, SDL_SCANCODE_DOWN, SDLK_DOWN);
    tap(&s, SDL_SCANCODE_DOWN, SDLK_DOWN);
    tap(&s, SDL_SCANCODE_RETURN, SDLK_RETURN);     /* RESET DEFAULTS */
    snapshot(&s, surf, r, dir, "reset_confirm");
    /* Pixel checks on the last snapshot: text pixels exist in the title
     * row, and the whole 768x672 canvas is covered by the translucent panel
     * (the game image is dimmed, not replaced). */
    Uint32 *px = (Uint32 *)surf->pixels;
    int bright = 0;
    for (int y = 3 * 3; y < 3 * 10; y++)
      for (int x = 0; x < 768; x++)
        if (((px[y * (surf->pitch / 4) + x] >> 16) & 0xff) > 200) bright++;
    CHECK(bright > 100, "title text drawn (%d bright px)", bright);
    Uint32 corner = px[671 * (surf->pitch / 4) + 767];
    CHECK(corner != 0x00407040u && corner != 0x00a08040u, "panel covers the frame");
    /* A larger output (e.g. 800x700) is centred with integer scaling: the
     * border outside 768x672 keeps the frame pixels. */
    SDL_DestroyRenderer(r);
    SDL_FreeSurface(surf);
    surf = SDL_CreateRGBSurfaceWithFormat(0, 800, 700, 32, SDL_PIXELFORMAT_XRGB8888);
    r = SDL_CreateSoftwareRenderer(surf);
    snapshot(&s, surf, r, dir, "reset_confirm_800x700");
    px = (Uint32 *)surf->pixels;
    CHECK(px[0] == 0x00a08040u && px[5 * (surf->pitch / 4) + 5] == 0x00a08040u,
          "outside the centred canvas untouched (%08X)", px[0]);
    goof_sdl_settings_destroy(&s);
    SDL_DestroyRenderer(r);
    SDL_FreeSurface(surf);
    printf("  S4 snapshots=%d in %s\n", snap_count, dir);
  }
  report("S4", "offscreen overlay render + snapshots");

  printf("GOOF_SETTINGS_SDL_TEST %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
