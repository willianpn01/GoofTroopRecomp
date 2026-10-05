/* GOOF_ENHANCEMENTS_E3 -- host video / presentation settings, SDL-free.
 *
 *   P1  defaults reproduce E2: windowed, 3x, square pixels, integer
 *   P2  window size per scale: square 256k x 224k; SNES 8:7 round(2048k/7) x 224k
 *   P3  default windowed destination == the whole window (E2's full copy)
 *   P4  fullscreen destination rectangles (integer / fit, square / 8:7)
 *   P5  AUTO window scale fits the usable desktop (title-bar reserve), >= 1
 *   P6  menu scale cycle AUTO,1..4 wraps both ways; a CLI-only value steps in
 *   P7  config tokens round-trip, case-insensitive; invalid tokens refused
 *   P8  sanitize clamps out-of-range fields to safe values
 *   P9  sweep: every destination lies inside the output, is centred, keeps
 *       the requested aspect, and INTEGER keeps an integer vertical scale
 *   P10 the guest framebuffer size is a constant 256x224 in every mode */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/goof_present_settings.h"

static int failures, case_failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; case_failures++; \
  printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void report(const char *name, const char *what) {
  printf("%s %s %s\n", name, case_failures ? "FAIL" : "PASS", what);
  case_failures = 0;
}

static bool rect_is(GoofRect r, int x, int y, int w, int h) {
  return r.x == x && r.y == y && r.w == w && r.h == h;
}

int main(void) {
  GoofVideoSettings v;
  goof_video_defaults(&v);
  CHECK(v.display == GOOF_DISPLAY_WINDOWED && v.window_scale == 3 &&
        v.aspect == GOOF_ASPECT_SQUARE && v.scaling == GOOF_SCALING_INTEGER,
        "defaults");
  int w, h;
  goof_video_window_size(v.aspect, goof_video_resolve_scale(&v, 1920, 1080), &w, &h);
  CHECK(w == 768 && h == 672, "default window 768x672 (E2 --scale 3), got %dx%d", w, h);
  report("P1", "defaults reproduce E2 (windowed, 3x = 768x672, square, integer)");

  static const int k87[5] = {0, 293, 585, 878, 1170};
  for (int k = 1; k <= 4; k++) {
    goof_video_window_size(GOOF_ASPECT_SQUARE, k, &w, &h);
    CHECK(w == 256 * k && h == 224 * k, "square %d: %dx%d", k, w, h);
    goof_video_window_size(GOOF_ASPECT_8_7, k, &w, &h);
    CHECK(w == k87[k] && h == 224 * k, "8:7 %d: %dx%d", k, w, h);
  }
  report("P2", "window sizes: square 256k x 224k, 8:7 293/585/878/1170 x 224k");

  CHECK(rect_is(goof_video_dest_rect(GOOF_ASPECT_SQUARE, GOOF_SCALING_INTEGER, 768, 672),
                0, 0, 768, 672), "default 3x window: full copy");
  for (int k = 1; k <= 8; k++) {
    goof_video_window_size(GOOF_ASPECT_SQUARE, k, &w, &h);
    CHECK(rect_is(goof_video_dest_rect(GOOF_ASPECT_SQUARE, GOOF_SCALING_INTEGER, w, h),
                  0, 0, w, h), "square window %d: full copy", k);
    goof_video_window_size(GOOF_ASPECT_8_7, k, &w, &h);
    GoofRect r = goof_video_dest_rect(GOOF_ASPECT_8_7, GOOF_SCALING_INTEGER, w, h);
    CHECK(rect_is(r, 0, 0, w, h), "8:7 window %d: full copy (%d,%d %dx%d)", k, r.x, r.y, r.w, r.h);
  }
  report("P3", "a window sized for scale k presents at exactly scale k (full window, E2 copy)");

  GoofRect r;
  r = goof_video_dest_rect(GOOF_ASPECT_SQUARE, GOOF_SCALING_INTEGER, 1920, 1080);
  CHECK(rect_is(r, 448, 92, 1024, 896), "1080p square integer %d,%d %dx%d", r.x, r.y, r.w, r.h);
  r = goof_video_dest_rect(GOOF_ASPECT_8_7, GOOF_SCALING_INTEGER, 1920, 1080);
  CHECK(rect_is(r, 375, 92, 1170, 896), "1080p 8:7 integer %d,%d %dx%d", r.x, r.y, r.w, r.h);
  r = goof_video_dest_rect(GOOF_ASPECT_SQUARE, GOOF_SCALING_FIT, 1920, 1080);
  CHECK(rect_is(r, 343, 0, 1234, 1080), "1080p square fit %d,%d %dx%d", r.x, r.y, r.w, r.h);
  r = goof_video_dest_rect(GOOF_ASPECT_8_7, GOOF_SCALING_FIT, 1920, 1080);
  CHECK(rect_is(r, 254, 0, 1411, 1080), "1080p 8:7 fit %d,%d %dx%d", r.x, r.y, r.w, r.h);
  r = goof_video_dest_rect(GOOF_ASPECT_SQUARE, GOOF_SCALING_INTEGER, 2560, 1440);
  CHECK(rect_is(r, 512, 48, 1536, 1344), "1440p square integer %d,%d %dx%d", r.x, r.y, r.w, r.h);
  r = goof_video_dest_rect(GOOF_ASPECT_SQUARE, GOOF_SCALING_INTEGER, 3840, 2160);
  CHECK(rect_is(r, 768, 72, 2304, 2016), "4K square integer %d,%d %dx%d", r.x, r.y, r.w, r.h);
  r = goof_video_dest_rect(GOOF_ASPECT_SQUARE, GOOF_SCALING_FIT, 1000, 400);   /* tall-limited */
  CHECK(rect_is(r, 271, 0, 457, 400), "wide output fit %d,%d %dx%d", r.x, r.y, r.w, r.h);
  r = goof_video_dest_rect(GOOF_ASPECT_SQUARE, GOOF_SCALING_FIT, 512, 1000);   /* width-limited */
  CHECK(rect_is(r, 0, 276, 512, 448), "tall output fit %d,%d %dx%d", r.x, r.y, r.w, r.h);
  report("P4", "fullscreen: 1080p integer 1024x896 / 8:7 1170x896, fit 1234x1080 / 1411x1080");

  CHECK(goof_video_auto_scale(GOOF_ASPECT_SQUARE, 1920, 1050, 48) == 4, "1080p usable -> 4");
  CHECK(goof_video_auto_scale(GOOF_ASPECT_8_7, 1920, 1050, 48) == 4, "1080p usable 8:7 -> 4");
  CHECK(goof_video_auto_scale(GOOF_ASPECT_SQUARE, 1366, 728, 48) == 3, "1366x768 -> 3");
  CHECK(goof_video_auto_scale(GOOF_ASPECT_SQUARE, 800, 600, 48) == 2, "800x600 -> 2");
  CHECK(goof_video_auto_scale(GOOF_ASPECT_8_7, 1024, 740, 48) == 3, "1024x768 8:7 -> 3 (878 wide)");
  CHECK(goof_video_auto_scale(GOOF_ASPECT_8_7, 870, 2000, 48) == 2, "width-limited 8:7 -> 2");
  CHECK(goof_video_auto_scale(GOOF_ASPECT_SQUARE, 200, 100, 48) == 1, "nothing fits -> 1");
  CHECK(goof_video_auto_scale(GOOF_ASPECT_SQUARE, 3840, 2100, 48) == 9, "4K -> 9");
  GoofVideoSettings a = v;
  a.window_scale = GOOF_WINDOW_SCALE_AUTO;
  CHECK(goof_video_resolve_scale(&a, 1920, 1050) == 4, "resolve auto");
  CHECK(goof_video_resolve_scale(&a, 0, 0) == 3, "resolve auto without desktop -> 3");
  a.window_scale = 2;
  CHECK(goof_video_resolve_scale(&a, 200, 100) == 2, "explicit scale is not clamped");
  for (int aw = 300; aw <= 4000; aw += 37)
    for (int ah = 300; ah <= 2200; ah += 41)
      for (int asp = 0; asp < GOOF_ASPECT_COUNT; asp++) {
        int k = goof_video_auto_scale((GoofPixelAspect)asp, aw, ah, 48);
        goof_video_window_size((GoofPixelAspect)asp, k, &w, &h);
        CHECK(k == 1 || (w <= aw && h <= ah - 48), "auto %dx%d exceeds %dx%d", w, h, aw, ah);
      }
  report("P5", "AUTO = largest integer window inside the usable desktop (48 px title reserve)");

  int s = GOOF_WINDOW_SCALE_AUTO;
  int seq[6];
  for (int i = 0; i < 6; i++) { seq[i] = s; s = goof_window_scale_step(s, +1); }
  CHECK(seq[0] == 0 && seq[1] == 1 && seq[2] == 2 && seq[3] == 3 && seq[4] == 4 && seq[5] == 0,
        "right cycle");
  CHECK(goof_window_scale_step(0, -1) == 4 && goof_window_scale_step(1, -1) == 0, "left cycle");
  CHECK(goof_window_scale_step(6, +1) == 0 && goof_window_scale_step(6, -1) == 4, "CLI 6 steps in");
  char lbl[16];
  goof_window_scale_label(0, lbl, sizeof lbl);
  CHECK(strcmp(lbl, "AUTO") == 0, "AUTO label");
  goof_window_scale_label(3, lbl, sizeof lbl);
  CHECK(strcmp(lbl, "3X") == 0, "3X label");
  report("P6", "scale cycle AUTO,1X,2X,3X,4X wraps; --scale 6 steps to AUTO / 4X");

  GoofDisplayMode dm; GoofPixelAspect pa; GoofScalingMode sm; int sc;
  CHECK(goof_display_from_token("FullScreen", &dm) && dm == GOOF_DISPLAY_FULLSCREEN, "display");
  CHECK(goof_display_from_token("windowed", &dm) && dm == GOOF_DISPLAY_WINDOWED, "windowed");
  CHECK(!goof_display_from_token("exclusive", &dm), "exclusive refused");
  CHECK(goof_aspect_from_token("8:7", &pa) && pa == GOOF_ASPECT_8_7, "8:7");
  CHECK(goof_aspect_from_token("SQUARE", &pa) && pa == GOOF_ASPECT_SQUARE, "square");
  CHECK(!goof_aspect_from_token("16:9", &pa), "16:9 refused");
  CHECK(goof_scaling_from_token("Fit", &sm) && sm == GOOF_SCALING_FIT, "fit");
  CHECK(!goof_scaling_from_token("stretch", &sm), "stretch refused");
  CHECK(goof_window_scale_from_token("AUTO", &sc) && sc == 0, "auto");
  CHECK(goof_window_scale_from_token("4", &sc) && sc == 4, "4");
  CHECK(!goof_window_scale_from_token("0", &sc) && !goof_window_scale_from_token("5", &sc) &&
        !goof_window_scale_from_token("3x", &sc) && !goof_window_scale_from_token("", &sc) &&
        !goof_window_scale_from_token("12", &sc), "bad scales refused");
  for (int i = 0; i < GOOF_DISPLAY_COUNT; i++)
    CHECK(goof_display_from_token(goof_display_token((GoofDisplayMode)i), &dm) && (int)dm == i, "rt");
  for (int i = 0; i < GOOF_ASPECT_COUNT; i++)
    CHECK(goof_aspect_from_token(goof_aspect_token((GoofPixelAspect)i), &pa) && (int)pa == i, "rt");
  for (int i = 0; i < GOOF_SCALING_COUNT; i++)
    CHECK(goof_scaling_from_token(goof_scaling_token((GoofScalingMode)i), &sm) && (int)sm == i, "rt");
  for (int i = 0; i <= 4; i++) {
    char t[8];
    goof_window_scale_token(i, t, sizeof t);
    CHECK(goof_window_scale_from_token(t, &sc) && sc == i, "scale rt %d", i);
  }
  report("P7", "tokens round-trip, case-insensitive; exclusive/16:9/stretch/0/5/3x refused");

  GoofVideoSettings bad = {(GoofDisplayMode)9, 99, (GoofPixelAspect)7, (GoofScalingMode)-1};
  goof_video_sanitize(&bad);
  CHECK(bad.display == GOOF_DISPLAY_WINDOWED && bad.window_scale == 3 &&
        bad.aspect == GOOF_ASPECT_SQUARE && bad.scaling == GOOF_SCALING_INTEGER, "sanitize");
  GoofVideoSettings cli = v;
  cli.window_scale = 8;
  goof_video_sanitize(&cli);
  CHECK(cli.window_scale == 8, "--scale 8 survives sanitize");
  report("P8", "sanitize: out-of-range -> safe defaults, CLI scales kept");

  int checked = 0;
  for (int ow = 64; ow <= 4000; ow += 29)
    for (int oh = 64; oh <= 2400; oh += 31)
      for (int asp = 0; asp < GOOF_ASPECT_COUNT; asp++)
        for (int sm2 = 0; sm2 < GOOF_SCALING_COUNT; sm2++) {
          r = goof_video_dest_rect((GoofPixelAspect)asp, (GoofScalingMode)sm2, ow, oh);
          checked++;
          CHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= ow && r.y + r.h <= oh && r.w > 0 && r.h > 0,
                "inside %dx%d", ow, oh);
          CHECK(abs((ow - r.w) - 2 * r.x) <= 1 && abs((oh - r.h) - 2 * r.y) <= 1, "centred");
          if (ow >= 293 && oh >= 224) {
            double want = asp == GOOF_ASPECT_8_7 ? 256.0 * 8 / 7 / 224 : 256.0 / 224;
            double got = (double)r.w / r.h;
            CHECK(got > want * (1 - 2.0 / r.h) - 0.01 && got < want * (1 + 2.0 / r.h) + 0.01,
                  "aspect %dx%d in %dx%d", r.w, r.h, ow, oh);
            if (sm2 == GOOF_SCALING_INTEGER) CHECK(r.h % 224 == 0, "integer h %d", r.h);
          }
          if (failures > 20) break;
        }
  printf("  %d destination rectangles checked\n", checked);
  report("P9", "every destination inside the output, centred, aspect kept, integer = k*224");

  CHECK(GOOF_FB_W == 256 && GOOF_FB_H == 224, "framebuffer constants");
  report("P10", "framebuffer stays 256x224 (geometry is a destination rectangle only)");

  printf("GOOF_PRESENT_SETTINGS_TEST %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
