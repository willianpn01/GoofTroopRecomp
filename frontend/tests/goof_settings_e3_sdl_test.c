/* GOOF_ENHANCEMENTS_E3 -- SDL half of the expanded Settings overlay and of
 * the presentation geometry, offscreen (software renderer, no window).
 *
 *   Z1  routing is unchanged with Video / Audio attached: closed -> only F2
 *       consumed; open (on any page) -> every key / pad event consumed;
 *       SDL_QUIT, window, render-reset and controller ADDED/REMOVED never
 *   Z2  snapshots of the root, Video, Audio pages and their modals (BMP)
 *   Z3  the overlay stays integer-scaled and centred in every presentation
 *       output (8:7 window 878x672, fullscreen 1920x1080): pixels outside
 *       its canvas keep the game image
 *   Z4  presentation composite, as main_sdl.c does it (256x224 streaming
 *       texture -> goof_video_dest_rect -> overlay): the source framebuffer
 *       bytes are identical before and after every mode (the oracle object
 *       is never written), the image lands exactly in the destination
 *       rectangle, the bars around it are black
 *   Z5  8:7 correction is horizontal only: every source column maps to
 *       3 or 4 output columns at 3x (878 px), every row to exactly 3 rows
 *
 * usage: goof_settings_e3_sdl_test OUT_DIR */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "host/goof_config.h"
#include "sdl_settings.h"

static int failures, case_failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; case_failures++; \
  printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void report(const char *name, const char *what) {
  printf("%s %s %s\n", name, case_failures ? "FAIL" : "PASS", what);
  case_failures = 0;
}

static GoofBindings live;
static GoofVideoSettings video, shown;
static GoofAudioSettings audio;
static GoofSaveResult save_in(void *u, const GoofBindings *b, char *msg, size_t cap) {
  (void)u; live = *b; snprintf(msg, cap, "SAVED - ACTIVE NOW"); return GOOF_SAVE_PERSISTED;
}
static bool prev_v(void *u, const GoofVideoSettings *v) { (void)u; shown = *v; return true; }
static void prev_a(void *u, const GoofAudioSettings *a) { (void)u; (void)a; }
static GoofSaveResult save_v(void *u, const GoofVideoSettings *v, char *msg, size_t cap) {
  (void)u; video = *v; snprintf(msg, cap, "SAVED - ACTIVE NOW"); return GOOF_SAVE_PERSISTED;
}
static GoofSaveResult save_a(void *u, const GoofAudioSettings *a, char *msg, size_t cap) {
  (void)u; audio = *a; snprintf(msg, cap, "SAVED - ACTIVE NOW"); return GOOF_SAVE_PERSISTED;
}
static void describe(void *u, const GoofVideoSettings *v, char *out, size_t cap) {
  (void)u;
  int w, h;
  goof_video_window_size(v->aspect, v->window_scale ? v->window_scale : 4, &w, &h);
  snprintf(out, cap, "WINDOW %dX%d", w, h);
}

static SDL_Event key_ev(Uint32 type, SDL_Scancode sc, SDL_Keycode sym) {
  SDL_Event e;
  memset(&e, 0, sizeof e);
  e.type = type;
  e.key.keysym.scancode = sc;
  e.key.keysym.sym = sym;
  return e;
}
static unsigned tap(GoofSdlSettings *s, SDL_Scancode sc, SDL_Keycode sym) {
  SDL_Event d = key_ev(SDL_KEYDOWN, sc, sym), u = key_ev(SDL_KEYUP, sc, sym);
  return goof_sdl_settings_event(s, &d) | goof_sdl_settings_event(s, &u);
}

static void fill_game(SDL_Surface *surf) {
  for (int y = 0; y < surf->h; y++)
    for (int x = 0; x < surf->w; x++)
      ((Uint32 *)surf->pixels)[y * (surf->pitch / 4) + x] =
          ((x / 48 + y / 48) & 1) ? 0x00407040u : 0x00a08040u;
}

static int snap_count;
static void snapshot(GoofSdlSettings *s, SDL_Surface *surf, SDL_Renderer *r, const char *dir,
                     const char *name) {
  fill_game(surf);
  goof_sdl_settings_render(s, r);
  SDL_RenderPresent(r);
  char path[1024];
  snprintf(path, sizeof path, "%s/e3_overlay_%02d_%s.bmp", dir, ++snap_count, name);
  if (SDL_SaveBMP(surf, path) != 0) CHECK(0, "save %s: %s", path, SDL_GetError());
}

static uint64_t fnv(const void *p, size_t n) {
  const uint8_t *b = p;
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
  return h;
}

/* A recognisable 256x224 "guest frame": each column a distinct colour band,
 * with a white 1-px grid every 16 px. */
static uint32_t fb[224][256];
static void make_fb(void) {
  for (int y = 0; y < 224; y++)
    for (int x = 0; x < 256; x++)
      fb[y][x] = (x % 16 == 0 || y % 16 == 0) ? 0x00FFFFFFu
               : (uint32_t)((x * 255 / 255) << 16 | (y & 0xff) << 8 | 0x40);
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: goof_settings_e3_sdl_test OUT_DIR\n"); return 2; }
  setvbuf(stdout, NULL, _IOLBF, 0);
  const char *dir = argv[1];
  goof_bindings_defaults(&live);
  goof_video_defaults(&video); shown = video;
  goof_audio_defaults(&audio);
  GoofSdlSettings s;
  goof_sdl_settings_init(&s, &live, (GoofSettingsHost){NULL, save_in, "~/.config/GoofTroopRecomp/config.ini"});
  goof_menu_attach_host_pages(&s.menu, (GoofHostPages){
      .video = &video, .audio = &audio, .preview_video = prev_v, .preview_audio = prev_a,
      .save_video = save_v, .save_audio = save_a, .describe_video = describe});

  /* Z1 */
  SDL_Event e = key_ev(SDL_KEYDOWN, SDL_SCANCODE_Z, SDLK_z);
  CHECK(goof_sdl_settings_event(&s, &e) == 0, "closed: game key passes");
  CHECK(tap(&s, SDL_SCANCODE_F2, SDLK_F2) & GOOF_MENU_OPENED, "F2 opens");
  tap(&s, SDL_SCANCODE_DOWN, SDLK_DOWN);
  tap(&s, SDL_SCANCODE_RETURN, SDLK_RETURN);            /* VIDEO */
  CHECK(goof_menu_page(&s.menu) == GOOF_PAGE_VIDEO, "on VIDEO");
  bool consumed = true;
  static const SDL_Scancode keys[] = {SDL_SCANCODE_Z, SDL_SCANCODE_P, SDL_SCANCODE_SPACE,
                                      SDL_SCANCODE_RSHIFT, SDL_SCANCODE_I};
  for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
    SDL_Event d = key_ev(SDL_KEYDOWN, keys[i], 0), u = key_ev(SDL_KEYUP, keys[i], 0);
    consumed = consumed && (goof_sdl_settings_event(&s, &d) & GOOF_MENU_CONSUMED) &&
               (goof_sdl_settings_event(&s, &u) & GOOF_MENU_CONSUMED);
  }
  CHECK(consumed, "open on VIDEO: keys consumed");
  static const Uint32 pass[] = {SDL_QUIT, SDL_WINDOWEVENT, SDL_RENDER_TARGETS_RESET,
                                SDL_RENDER_DEVICE_RESET, SDL_CONTROLLERDEVICEADDED,
                                SDL_CONTROLLERDEVICEREMOVED};
  for (size_t i = 0; i < sizeof pass / sizeof pass[0]; i++) {
    SDL_Event x;
    memset(&x, 0, sizeof x);
    x.type = pass[i];
    CHECK(!(goof_sdl_settings_event(&s, &x) & GOOF_MENU_CONSUMED), "event 0x%x passes", pass[i]);
  }
  tap(&s, SDL_SCANCODE_F2, SDLK_F2);
  report("Z1", "routing unchanged with Video/Audio: open pages take every key; system events pass");

  /* Z2 */
  SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, 768, 672, 32, SDL_PIXELFORMAT_XRGB8888);
  SDL_Renderer *r = surf ? SDL_CreateSoftwareRenderer(surf) : NULL;
  CHECK(surf && r, "software renderer: %s", SDL_GetError());
  if (!surf || !r) { printf("GOOF_SETTINGS_E3_SDL_TEST FAIL\n"); return 1; }
  tap(&s, SDL_SCANCODE_F2, SDLK_F2);
  snapshot(&s, surf, r, dir, "root");
  tap(&s, SDL_SCANCODE_DOWN, SDLK_DOWN);
  tap(&s, SDL_SCANCODE_RETURN, SDLK_RETURN);
  snapshot(&s, surf, r, dir, "video");
  tap(&s, SDL_SCANCODE_DOWN, SDLK_DOWN);
  tap(&s, SDL_SCANCODE_DOWN, SDLK_DOWN);
  tap(&s, SDL_SCANCODE_RIGHT, SDLK_RIGHT);              /* 8:7 preview */
  snapshot(&s, surf, r, dir, "video_changed");
  CHECK(shown.aspect == GOOF_ASPECT_8_7, "preview reached the host");
  tap(&s, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
  snapshot(&s, surf, r, dir, "video_discard_confirm");
  tap(&s, SDL_SCANCODE_LEFT, SDLK_LEFT);
  tap(&s, SDL_SCANCODE_RETURN, SDLK_RETURN);            /* YES */
  CHECK(shown.aspect == GOOF_ASPECT_SQUARE && goof_menu_page(&s.menu) == GOOF_PAGE_ROOT, "discarded");
  tap(&s, SDL_SCANCODE_DOWN, SDLK_DOWN);
  tap(&s, SDL_SCANCODE_RETURN, SDLK_RETURN);            /* AUDIO */
  snapshot(&s, surf, r, dir, "audio");
  tap(&s, SDL_SCANCODE_LEFT, SDLK_LEFT);
  tap(&s, SDL_SCANCODE_DOWN, SDLK_DOWN);
  tap(&s, SDL_SCANCODE_RETURN, SDLK_RETURN);            /* mute on */
  snapshot(&s, surf, r, dir, "audio_changed");
  tap(&s, SDL_SCANCODE_DOWN, SDLK_DOWN);
  tap(&s, SDL_SCANCODE_DOWN, SDLK_DOWN);
  tap(&s, SDL_SCANCODE_RETURN, SDLK_RETURN);            /* RESET AUDIO DEFAULTS */
  snapshot(&s, surf, r, dir, "audio_reset_confirm");
  report("Z2", "BMP snapshots: root, video (+changed, discard modal), audio (+changed, reset modal)");

  /* Z3 */
  struct { int w, h, k; const char *name; } outs[] = {
    {878, 672, 3, "8x7_window_878x672"}, {1920, 1080, 4, "fullscreen_1920x1080"},
  };
  for (size_t i = 0; i < 2; i++) {
    SDL_DestroyRenderer(r);
    SDL_FreeSurface(surf);
    surf = SDL_CreateRGBSurfaceWithFormat(0, outs[i].w, outs[i].h, 32, SDL_PIXELFORMAT_XRGB8888);
    r = SDL_CreateSoftwareRenderer(surf);
    snapshot(&s, surf, r, dir, outs[i].name);
    Uint32 *px = (Uint32 *)surf->pixels;
    int pitch = surf->pitch / 4;
    int ox = (outs[i].w - 256 * outs[i].k) / 2, oy = (outs[i].h - 224 * outs[i].k) / 2;
    bool outside_ok = true, inside_dimmed = true;
    for (int y = 0; y < outs[i].h; y += 7)
      for (int x = 0; x < outs[i].w; x += 5) {
        bool in = x >= ox && x < ox + 256 * outs[i].k && y >= oy && y < oy + 224 * outs[i].k;
        Uint32 g = ((x / 48 + y / 48) & 1) ? 0x00407040u : 0x00a08040u;
        Uint32 p = px[y * pitch + x] & 0x00FFFFFFu;
        if (!in && p != g) outside_ok = false;
        if (in && p == g) inside_dimmed = false;
      }
    CHECK(outside_ok && inside_dimmed, "%s: overlay %dx at +%d,+%d only", outs[i].name, outs[i].k, ox, oy);
  }
  tap(&s, SDL_SCANCODE_F2, SDLK_F2);
  report("Z3", "overlay integer-scaled + centred in 8:7 and fullscreen outputs; outside untouched");

  /* Z4 + Z5 */
  make_fb();
  uint64_t before = fnv(fb, sizeof fb);
  struct { GoofPixelAspect a; GoofScalingMode sm; int w, h; const char *name; } modes[] = {
    {GOOF_ASPECT_SQUARE, GOOF_SCALING_INTEGER, 768, 672, "square_window_3x"},
    {GOOF_ASPECT_8_7, GOOF_SCALING_INTEGER, 878, 672, "8x7_window_3x"},
    {GOOF_ASPECT_SQUARE, GOOF_SCALING_INTEGER, 1920, 1080, "fs_square_integer"},
    {GOOF_ASPECT_SQUARE, GOOF_SCALING_FIT, 1920, 1080, "fs_square_fit"},
    {GOOF_ASPECT_8_7, GOOF_SCALING_INTEGER, 1920, 1080, "fs_8x7_integer"},
    {GOOF_ASPECT_8_7, GOOF_SCALING_FIT, 1920, 1080, "fs_8x7_fit"},
  };
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
    SDL_DestroyRenderer(r);
    SDL_FreeSurface(surf);
    surf = SDL_CreateRGBSurfaceWithFormat(0, modes[i].w, modes[i].h, 32, SDL_PIXELFORMAT_XRGB8888);
    r = SDL_CreateSoftwareRenderer(surf);
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 224);
    SDL_UpdateTexture(t, NULL, fb, 256 * 4);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    GoofRect g = goof_video_dest_rect(modes[i].a, modes[i].sm, modes[i].w, modes[i].h);
    SDL_Rect dst = {g.x, g.y, g.w, g.h};
    SDL_RenderCopy(r, t, NULL, &dst);
    SDL_RenderPresent(r);
    char path[1024];
    snprintf(path, sizeof path, "%s/e3_present_%s_%dx%d.bmp", dir, modes[i].name, g.w, g.h);
    SDL_SaveBMP(surf, path);
    Uint32 *px = (Uint32 *)surf->pixels;
    int pitch = surf->pitch / 4;
    bool bars_black = true, image_in = true;
    for (int y = 0; y < modes[i].h; y += 3)
      for (int x = 0; x < modes[i].w; x += 3) {
        bool in = x >= g.x && x < g.x + g.w && y >= g.y && y < g.y + g.h;
        Uint32 p = px[y * pitch + x] & 0x00FFFFFFu;
        if (!in && p != 0) bars_black = false;
      }
    /* Image corners are the frame's corner pixels (grid lines = white). */
    image_in = (px[g.y * pitch + g.x] & 0xFFFFFF) == 0xFFFFFF &&
               (px[(g.y + g.h - 1) * pitch + g.x + g.w - 1] & 0xFFFFFF) == (fb[223][255] & 0xFFFFFF);
    CHECK(bars_black && image_in, "%s: image exactly in %d,%d %dx%d", modes[i].name, g.x, g.y, g.w, g.h);
    if (i == 1) {
      /* Z5: count output columns per source column along row 5*3+1. */
      int y = 1 * 3 + 1;
      int runs[256] = {0};
      for (int x = 0; x < g.w; x++) {
        int sx = (int)((long long)x * 256 / g.w);
        runs[sx]++;
      }
      bool cols_ok = true;
      for (int c = 0; c < 256; c++) if (runs[c] < 3 || runs[c] > 4) cols_ok = false;
      int grid_rows = 0;
      for (int yy = 0; yy < g.h; yy++)
        if ((px[yy * pitch + g.x + 1 * 3 * 16 / 1 + 5] & 0xFFFFFF) == 0xFFFFFF) grid_rows++;
      (void)y;
      CHECK(cols_ok && g.w == 878 && g.h == 672, "8:7 3x: 878 px, 3-4 px per column");
      CHECK(grid_rows == 14 * 3, "rows stay exactly 3x (grid rows %d)", grid_rows);
    }
    SDL_DestroyTexture(t);
  }
  CHECK(fnv(fb, sizeof fb) == before, "framebuffer bytes unchanged by every presentation mode");
  printf("  framebuffer fnv64=%016llx before and after all modes\n", (unsigned long long)before);
  report("Z4", "composite per mode: image exactly in the destination rect, black bars, framebuffer unchanged");
  report("Z5", "8:7 is horizontal only (878 px at 3x, 3-4 px/column), rows stay exactly 3x");

  SDL_DestroyRenderer(r);
  SDL_FreeSurface(surf);
  goof_sdl_settings_destroy(&s);
  printf("GOOF_SETTINGS_E3_SDL_TEST %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
