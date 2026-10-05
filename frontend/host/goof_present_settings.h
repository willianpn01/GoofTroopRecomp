#ifndef GOOF_PRESENT_SETTINGS_H
#define GOOF_PRESENT_SETTINGS_H

/* Goof Troop Recomp -- host video / presentation settings.
 * GOOF_ENHANCEMENTS_E3_HOST_SETTINGS_UI.
 *
 * SDL-FREE and guest-free.  These settings describe only how the finished
 * 256x224 guest framebuffer is put on the screen: window or fullscreen,
 * window size, pixel aspect and fullscreen scaling.  The framebuffer itself
 * (the object every oracle hashes) is never resized, filtered or written;
 * the geometry below is a destination rectangle for SDL_RenderCopy.
 *
 *   guest framebuffer 256x224 (unchanged)
 *     -> presentation base size  256x224 (SQUARE) or 292.57x224 (8:7)
 *     -> integer multiple (window, fullscreen INTEGER) or best fit (FIT)
 *
 * Defaults reproduce E2 exactly: windowed, 3x, square pixels, integer.
 * Widescreen is not here (E5/E6).
 *
 * GOOF_ENHANCEMENTS_E4 adds ONE presentation-pacing field, `vsync`: whether
 * SDL_RenderPresent waits for the display's vertical blank.  It is not
 * geometry (no function below reads it) and it is not a clock: the guest's
 * cadence is owned by host/goof_frame_pacer.c and cannot follow the display
 * whatever this says.  Default OFF (= E3). */

#include <stdbool.h>
#include <stddef.h>

enum { GOOF_FB_W = 256, GOOF_FB_H = 224 };

typedef enum {
  GOOF_DISPLAY_WINDOWED = 0,
  GOOF_DISPLAY_FULLSCREEN,         /* SDL "fullscreen desktop" (borderless)  */
  GOOF_DISPLAY_COUNT
} GoofDisplayMode;

typedef enum {
  GOOF_ASPECT_SQUARE = 0,          /* 1:1 pixels (E2 appearance, default)    */
  GOOF_ASPECT_8_7,                 /* SNES 8:7 pixel aspect, 4:3-ish image   */
  GOOF_ASPECT_COUNT
} GoofPixelAspect;

typedef enum {
  GOOF_SCALING_INTEGER = 0,        /* whole-number multiple, centred         */
  GOOF_SCALING_FIT,                /* largest size that fits, keeps aspect   */
  GOOF_SCALING_COUNT
} GoofScalingMode;

enum {
  GOOF_WINDOW_SCALE_AUTO = 0,      /* largest integer window that fits       */
  GOOF_WINDOW_SCALE_MIN = 1,
  GOOF_WINDOW_SCALE_MAX = 4,       /* largest value the file / menu offers   */
  GOOF_WINDOW_SCALE_DEFAULT = 3,   /* E2 default (--scale 3)                 */
  GOOF_WINDOW_SCALE_CLI_MAX = 64,  /* --scale N may go beyond the menu set   */
};

typedef struct {
  GoofDisplayMode display;
  int window_scale;                /* GOOF_WINDOW_SCALE_AUTO or 1.. (see above) */
  GoofPixelAspect aspect;
  GoofScalingMode scaling;         /* used in fullscreen; a window is always
                                    * exactly an integer size                */
  bool vsync;                      /* E4: present on vblank (pacing only)    */
  enum GoofFilterMode {
    GOOF_FILTER_NEAREST = 0,
    GOOF_FILTER_BILINEAR,
    GOOF_FILTER_SCANLINES,
    GOOF_FILTER_COUNT
  } filter;                        /* E5: downstream of the guest oracle */
} GoofVideoSettings;

typedef enum GoofFilterMode GoofFilterMode;
const char *goof_filter_token(GoofFilterMode mode);
const char *goof_filter_label(GoofFilterMode mode);
bool goof_filter_from_token(const char *text, GoofFilterMode *out);

void goof_video_defaults(GoofVideoSettings *v);
bool goof_video_equal(const GoofVideoSettings *a, const GoofVideoSettings *b);
/* Clamps every field into its valid range (used after loading). */
void goof_video_sanitize(GoofVideoSettings *v);

/* ---- config tokens (exact lower case; parse is case-insensitive) ------ */
const char *goof_display_token(GoofDisplayMode m);          /* windowed|fullscreen */
const char *goof_aspect_token(GoofPixelAspect a);           /* square|8:7          */
const char *goof_scaling_token(GoofScalingMode s);          /* integer|fit         */
bool goof_display_from_token(const char *t, GoofDisplayMode *out);
bool goof_aspect_from_token(const char *t, GoofPixelAspect *out);
bool goof_scaling_from_token(const char *t, GoofScalingMode *out);
/* "auto" or 1..GOOF_WINDOW_SCALE_MAX. */
bool goof_window_scale_from_token(const char *t, int *out);
void goof_window_scale_token(int scale, char *out, size_t cap);

/* ---- menu labels (upper case, bitmap-font friendly) ------------------- */
const char *goof_display_label(GoofDisplayMode m);          /* WINDOWED / FULLSCREEN */
const char *goof_aspect_label(GoofPixelAspect a);           /* SQUARE / SNES 8:7     */
const char *goof_scaling_label(GoofScalingMode s);          /* INTEGER / FIT         */
void goof_window_scale_label(int scale, char *out, size_t cap);   /* AUTO / 3X */
/* E4: vsync token (off|on; parsed with goof_bool_from_token) and label. */
const char *goof_vsync_token(bool vsync);                    /* off|on       */
const char *goof_vsync_label(bool vsync);                    /* OFF / ON     */

/* Next value in the menu cycle AUTO,1X..4X (wrapping); a CLI-only value
 * outside the set (e.g. --scale 6) steps to its nearest neighbour. */
int goof_window_scale_step(int scale, int dir);

/* ---- geometry --------------------------------------------------------- */
typedef struct { int x, y, w, h; } GoofRect;

/* Presentation base width for one integer scale step, in 1/1000 pixel:
 * 256000 (square) or 292571 (8:7 = 256 * 8 / 7). */
long goof_video_base_width_milli(GoofPixelAspect a);

/* Window client size for an integer scale k >= 1:
 * width = round(256 * par * k), height = 224 * k. */
void goof_video_window_size(GoofPixelAspect a, int k, int *w, int *h);

/* Largest k >= 1 whose window fits (avail_w, avail_h) after reserving
 * `reserve_h` pixels for the title bar; 1 when nothing fits. */
int goof_video_auto_scale(GoofPixelAspect a, int avail_w, int avail_h, int reserve_h);

/* The integer window scale a setting resolves to (AUTO uses the desktop
 * size given; a value < 1 is treated as AUTO). */
int goof_video_resolve_scale(const GoofVideoSettings *v, int avail_w, int avail_h);

/* Destination rectangle of the 256x224 image inside an output of
 * out_w x out_h, centred.  INTEGER: the largest integer vertical multiple
 * that fits (>= 1), width from the pixel aspect.  FIT: the largest size of
 * the same aspect that fits.  Windowed presentation passes INTEGER, and the
 * window is sized so that the rectangle is exactly the whole output. */
GoofRect goof_video_dest_rect(GoofPixelAspect a, GoofScalingMode s, int out_w,
                              int out_h);

#endif
