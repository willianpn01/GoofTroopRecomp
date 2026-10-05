/* Goof Troop Recomp -- host video / presentation settings.  SDL-FREE.
 * See goof_present_settings.h.  GOOF_ENHANCEMENTS_E3_HOST_SETTINGS_UI. */
#include "host/goof_present_settings.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void goof_video_defaults(GoofVideoSettings *v) {
  v->display = GOOF_DISPLAY_WINDOWED;
  v->window_scale = GOOF_WINDOW_SCALE_DEFAULT;
  v->aspect = GOOF_ASPECT_SQUARE;
  v->scaling = GOOF_SCALING_INTEGER;
  v->vsync = false;
  v->filter = GOOF_FILTER_NEAREST;
}

bool goof_video_equal(const GoofVideoSettings *a, const GoofVideoSettings *b) {
  return a->display == b->display && a->window_scale == b->window_scale &&
         a->aspect == b->aspect && a->scaling == b->scaling &&
         a->vsync == b->vsync && a->filter == b->filter;
}

void goof_video_sanitize(GoofVideoSettings *v) {
  if ((unsigned)v->filter >= GOOF_FILTER_COUNT) v->filter = GOOF_FILTER_NEAREST;
  if ((unsigned)v->display >= GOOF_DISPLAY_COUNT) v->display = GOOF_DISPLAY_WINDOWED;
  if ((unsigned)v->aspect >= GOOF_ASPECT_COUNT) v->aspect = GOOF_ASPECT_SQUARE;
  if ((unsigned)v->scaling >= GOOF_SCALING_COUNT) v->scaling = GOOF_SCALING_INTEGER;
  if (v->window_scale < GOOF_WINDOW_SCALE_AUTO ||
      v->window_scale > GOOF_WINDOW_SCALE_CLI_MAX)
    v->window_scale = GOOF_WINDOW_SCALE_DEFAULT;
}

/* ---- tokens ----------------------------------------------------------- */

static bool token_eq(const char *a, const char *b) {
  for (; *a && *b; a++, b++)
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
  return *a == *b;
}

static const char *const kDisplayTok[GOOF_DISPLAY_COUNT] = {"windowed", "fullscreen"};
static const char *const kAspectTok[GOOF_ASPECT_COUNT] = {"square", "8:7"};
static const char *const kScalingTok[GOOF_SCALING_COUNT] = {"integer", "fit"};
static const char *const kDisplayLbl[GOOF_DISPLAY_COUNT] = {"WINDOWED", "FULLSCREEN"};
static const char *const kAspectLbl[GOOF_ASPECT_COUNT] = {"SQUARE", "SNES 8:7"};
static const char *const kScalingLbl[GOOF_SCALING_COUNT] = {"INTEGER", "FIT"};

static const char *const kFilterTok[GOOF_FILTER_COUNT] = {"nearest", "bilinear", "scanlines"};
static const char *const kFilterLbl[GOOF_FILTER_COUNT] = {"NEAREST", "SMOOTH", "SCANLINES"};
const char *goof_filter_token(GoofFilterMode mode) {
  return kFilterTok[(unsigned)mode < GOOF_FILTER_COUNT ? mode : GOOF_FILTER_NEAREST];
}
const char *goof_filter_label(GoofFilterMode mode) {
  return kFilterLbl[(unsigned)mode < GOOF_FILTER_COUNT ? mode : GOOF_FILTER_NEAREST];
}
bool goof_filter_from_token(const char *text, GoofFilterMode *out) {
  for (int i = 0; i < GOOF_FILTER_COUNT; i++)
    if (token_eq(text, kFilterTok[i])) { *out = (GoofFilterMode)i; return true; }
  return false;
}

const char *goof_display_token(GoofDisplayMode m) {
  return (unsigned)m < GOOF_DISPLAY_COUNT ? kDisplayTok[m] : kDisplayTok[0];
}
const char *goof_aspect_token(GoofPixelAspect a) {
  return (unsigned)a < GOOF_ASPECT_COUNT ? kAspectTok[a] : kAspectTok[0];
}
const char *goof_scaling_token(GoofScalingMode s) {
  return (unsigned)s < GOOF_SCALING_COUNT ? kScalingTok[s] : kScalingTok[0];
}
const char *goof_display_label(GoofDisplayMode m) {
  return (unsigned)m < GOOF_DISPLAY_COUNT ? kDisplayLbl[m] : "?";
}
const char *goof_aspect_label(GoofPixelAspect a) {
  return (unsigned)a < GOOF_ASPECT_COUNT ? kAspectLbl[a] : "?";
}
const char *goof_scaling_label(GoofScalingMode s) {
  return (unsigned)s < GOOF_SCALING_COUNT ? kScalingLbl[s] : "?";
}
const char *goof_vsync_token(bool vsync) { return vsync ? "on" : "off"; }
const char *goof_vsync_label(bool vsync) { return vsync ? "ON" : "OFF"; }

bool goof_display_from_token(const char *t, GoofDisplayMode *out) {
  for (int i = 0; i < GOOF_DISPLAY_COUNT; i++)
    if (token_eq(t, kDisplayTok[i])) { *out = (GoofDisplayMode)i; return true; }
  return false;
}
bool goof_aspect_from_token(const char *t, GoofPixelAspect *out) {
  for (int i = 0; i < GOOF_ASPECT_COUNT; i++)
    if (token_eq(t, kAspectTok[i])) { *out = (GoofPixelAspect)i; return true; }
  return false;
}
bool goof_scaling_from_token(const char *t, GoofScalingMode *out) {
  for (int i = 0; i < GOOF_SCALING_COUNT; i++)
    if (token_eq(t, kScalingTok[i])) { *out = (GoofScalingMode)i; return true; }
  return false;
}

bool goof_window_scale_from_token(const char *t, int *out) {
  if (token_eq(t, "auto")) { *out = GOOF_WINDOW_SCALE_AUTO; return true; }
  if (t[0] >= '1' && t[0] <= '0' + GOOF_WINDOW_SCALE_MAX && t[1] == '\0') {
    *out = t[0] - '0';
    return true;
  }
  return false;
}

void goof_window_scale_token(int scale, char *out, size_t cap) {
  if (scale == GOOF_WINDOW_SCALE_AUTO) snprintf(out, cap, "auto");
  else snprintf(out, cap, "%d", scale);
}

void goof_window_scale_label(int scale, char *out, size_t cap) {
  if (scale == GOOF_WINDOW_SCALE_AUTO) snprintf(out, cap, "AUTO");
  else snprintf(out, cap, "%dX", scale);
}

int goof_window_scale_step(int scale, int dir) {
  /* Menu cycle: AUTO(0), 1, 2, 3, 4, wrapping. */
  enum { N = GOOF_WINDOW_SCALE_MAX + 1 };
  if (scale > GOOF_WINDOW_SCALE_MAX)            /* CLI-only value (--scale 6) */
    return dir > 0 ? GOOF_WINDOW_SCALE_AUTO : GOOF_WINDOW_SCALE_MAX;
  if (scale < 0) scale = GOOF_WINDOW_SCALE_AUTO;
  return (scale + (dir > 0 ? 1 : N - 1)) % N;
}

/* ---- geometry --------------------------------------------------------- */

long goof_video_base_width_milli(GoofPixelAspect a) {
  /* 256 * 8 / 7 = 292.571428...  Rounded to the nearest 1/1000 pixel. */
  return a == GOOF_ASPECT_8_7 ? 292571L : 256000L;
}

static int round_milli(long long milli) { return (int)((milli + 500) / 1000); }

void goof_video_window_size(GoofPixelAspect a, int k, int *w, int *h) {
  if (k < 1) k = 1;
  if (a == GOOF_ASPECT_8_7)
    *w = (int)((2048LL * k + 3) / 7);           /* round(256 * 8 * k / 7) */
  else
    *w = GOOF_FB_W * k;
  *h = GOOF_FB_H * k;
}

int goof_video_auto_scale(GoofPixelAspect a, int avail_w, int avail_h, int reserve_h) {
  int best = 1;
  for (int k = 1; k <= GOOF_WINDOW_SCALE_CLI_MAX; k++) {
    int w, h;
    goof_video_window_size(a, k, &w, &h);
    if (w <= avail_w && h <= avail_h - reserve_h) best = k;
    else break;
  }
  return best;
}

enum { GOOF_TITLE_BAR_RESERVE = 48 };

int goof_video_resolve_scale(const GoofVideoSettings *v, int avail_w, int avail_h) {
  if (v->window_scale >= 1) return v->window_scale;
  if (avail_w <= 0 || avail_h <= 0) return GOOF_WINDOW_SCALE_DEFAULT;
  return goof_video_auto_scale(v->aspect, avail_w, avail_h, GOOF_TITLE_BAR_RESERVE);
}

GoofRect goof_video_dest_rect(GoofPixelAspect a, GoofScalingMode s, int out_w,
                              int out_h) {
  GoofRect r = {0, 0, out_w, out_h};
  if (out_w <= 0 || out_h <= 0) return r;
  long base = goof_video_base_width_milli(a);
  int w, h;
  if (s == GOOF_SCALING_FIT) {
    /* Largest size of aspect base:224 inside the output. */
    long long w_from_h = (long long)out_h * base / GOOF_FB_H;   /* milli px */
    if (w_from_h <= (long long)out_w * 1000) {
      h = out_h;
      w = round_milli(w_from_h);
    } else {
      w = out_w;
      h = (int)(((long long)out_w * 1000 * GOOF_FB_H + base / 2) / base);
    }
  } else {
    /* Same rounded widths as goof_video_window_size, so a window sized for
     * scale k presents at exactly scale k (8:7 widths are rounded). */
    int k = out_h / GOOF_FB_H;
    if (k < 1) k = 1;
    goof_video_window_size(a, k, &w, &h);
    while (k > 1 && w > out_w) goof_video_window_size(a, --k, &w, &h);
    if (w > out_w) w = out_w;                  /* output smaller than 1x */
    if (h > out_h) h = out_h;
  }
  r.w = w;
  r.h = h;
  r.x = (out_w - w) / 2;
  r.y = (out_h - h) / 2;
  return r;
}
