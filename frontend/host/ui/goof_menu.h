#ifndef GOOF_MENU_H
#define GOOF_MENU_H

/* Goof Troop Recomp -- host Settings overlay: state machine and draw list.
 * GOOF_ENHANCEMENTS_E2_BUTTON_REMAPPING_CONFIG (foundation for E3).
 *
 * SDL-FREE and guest-free.  The overlay is a HOST feature: it reads and
 * edits host bindings only, and the frontend stops stepping the guest while
 * it is open.  This module never draws; it produces a GoofUiDrawList in a
 * 256x224 canvas that sdl_settings.c scales and composites AFTER the guest
 * framebuffer has been copied to the window, so the hashed 256x224 oracle
 * framebuffer is never touched.
 *
 * Input arrives as GoofUiEvents (key / pad presses and releases in the E1
 * normalised vocabulary), translated here -- not in SDL code -- into menu
 * navigation, so the whole flow is unit-testable (goof_menu_test.c):
 *
 *   keyboard  F2 open/close   arrows move   Enter/KP-Enter confirm   Esc back
 *   pad       D-pad / left stick move   bottom confirm   right back
 *
 * Pages form a stack.  E2 registers the Input section; E3 adds sections to
 * kRootSections (goof_menu.c) and pages to the page table, nothing else.
 *
 * GOOF_ENHANCEMENTS_E3_HOST_SETTINGS_UI: VIDEO and AUDIO sections.  They
 * appear only when the frontend attaches them (goof_menu_attach_host_pages),
 * so a menu initialised the E2 way is exactly the E2 menu.  Each page edits
 * a working copy; changes are previewed live through the host (the guest
 * stays paused), SAVE commits + persists, CANCEL / BACK-and-discard / closing
 * the overlay restore the committed values.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "host/goof_audio_settings.h"
#include "host/goof_bindings.h"
#include "host/goof_present_settings.h"

/* ---- draw list -------------------------------------------------------- */
enum {
  GOOF_UI_CANVAS_W = 256, GOOF_UI_CANVAS_H = 224,
  GOOF_UI_CELL_W = 6, GOOF_UI_CELL_H = 10,
  GOOF_UI_COLS = 42, GOOF_UI_ROWS = 22,
  GOOF_UI_ORIGIN_X = 2, GOOF_UI_ORIGIN_Y = 2,
  GOOF_UI_MAX_RECTS = 48, GOOF_UI_MAX_TEXTS = 64,
};

typedef enum {
  GOOF_UI_C_PANEL = 0,     /* translucent dark backdrop       */
  GOOF_UI_C_TEXT,
  GOOF_UI_C_TITLE,
  GOOF_UI_C_DIM,
  GOOF_UI_C_SEL_BAR,       /* selected-row bar                */
  GOOF_UI_C_SEL_TEXT,
  GOOF_UI_C_WARN,
  GOOF_UI_C_OK,
  GOOF_UI_C_BOX,           /* modal box (capture / confirm)   */
  GOOF_UI_C_RULE,
  GOOF_UI_C_COUNT
} GoofUiColor;

typedef struct { int16_t x, y, w, h; uint8_t color, layer; } GoofUiRect;
typedef struct { int16_t x, y; uint8_t color, layer; char text[GOOF_UI_COLS + 1]; } GoofUiText;

/* Two layers: 0 = page, 1 = modal (capture prompt, yes/no).  Renderers draw
 * layer 0 rects, layer 0 text, then layer 1 rects, layer 1 text. */
enum { GOOF_UI_LAYERS = 2 };

typedef struct {
  GoofUiRect rect[GOOF_UI_MAX_RECTS];
  int nrect;
  GoofUiText text[GOOF_UI_MAX_TEXTS];
  int ntext;
  uint8_t layer;                 /* layer new items are added to */
} GoofUiDrawList;

/* ---- events ----------------------------------------------------------- */
typedef enum {
  GOOF_UI_KEY_DOWN, GOOF_UI_KEY_UP, GOOF_UI_PAD_DOWN, GOOF_UI_PAD_UP
} GoofUiEventType;

typedef struct {
  GoofUiEventType type;
  uint16_t code;      /* key: USB HID usage; pad: GoofHostButton / stick dir */
  bool repeat;        /* keyboard auto-repeat                              */
  int32_t device;     /* pad instance id; -1 for the keyboard              */
} GoofUiEvent;

enum {
  GOOF_MENU_CONSUMED = 1u << 0,   /* the event belongs to the host        */
  GOOF_MENU_OPENED = 1u << 1,
  GOOF_MENU_CLOSED = 1u << 2,
  GOOF_MENU_SAVED = 1u << 3,
};

/* ---- host interface --------------------------------------------------- */
typedef enum {
  GOOF_SAVE_FAILED = 0,        /* nothing applied                         */
  GOOF_SAVE_APPLIED_ONLY,      /* live now, but not written to a file      */
  GOOF_SAVE_PERSISTED,         /* live now and written atomically         */
} GoofSaveResult;

typedef struct {
  void *user;
  /* Apply `b` to the live input path and persist it.  Writes a short
   * user-visible result into msg. */
  GoofSaveResult (*save)(void *user, const GoofBindings *b, char *msg, size_t cap);
  const char *save_target;   /* short label: where Save writes            */
} GoofSettingsHost;

/* E3: host video / audio pages.  `video` / `audio` point at the host's
 * COMMITTED effective values (what is live after the last Save, including
 * command-line overrides).  preview_* applies a working copy live without
 * committing it (called on every change and, with the committed values, on
 * Cancel); save_* commits it (live + persisted, like the Input save).
 * preview_video returns false when the host could not apply it (e.g.
 * fullscreen refused); the page then steps the display mode back.
 * describe_video writes a one-line geometry summary (may be NULL). */
typedef struct {
  void *user;
  const GoofVideoSettings *video;
  const GoofAudioSettings *audio;
  bool (*preview_video)(void *user, const GoofVideoSettings *v);
  void (*preview_audio)(void *user, const GoofAudioSettings *a);
  GoofSaveResult (*save_video)(void *user, const GoofVideoSettings *v, char *msg, size_t cap);
  GoofSaveResult (*save_audio)(void *user, const GoofAudioSettings *a, char *msg, size_t cap);
  void (*describe_video)(void *user, const GoofVideoSettings *v, char *out, size_t cap);
  const char *video_note;          /* e.g. "WINDOW SCALE FROM --SCALE"   */
} GoofHostPages;

typedef enum {
  GOOF_PAGE_ROOT = 0, GOOF_PAGE_INPUT, GOOF_PAGE_PLAYER, GOOF_PAGE_EDIT,
  GOOF_PAGE_VIDEO, GOOF_PAGE_AUDIO,                          /* E3 */
  GOOF_PAGE_COUNT
} GoofMenuPageId;

typedef enum { GOOF_CAPTURE_NONE, GOOF_CAPTURE_ARMING, GOOF_CAPTURE_ARMED } GoofCaptureState;

typedef enum {
  GOOF_CONFIRM_NONE, GOOF_CONFIRM_RESET, GOOF_CONFIRM_DISCARD_BACK,
  /* E3 */
  GOOF_CONFIRM_VIDEO_RESET, GOOF_CONFIRM_VIDEO_DISCARD_BACK,
  GOOF_CONFIRM_AUDIO_RESET, GOOF_CONFIRM_AUDIO_DISCARD_BACK,
} GoofConfirmId;

typedef struct GoofMenu {
  bool open;
  GoofMenuPageId stack[8];
  int depth;
  int sel[GOOF_PAGE_COUNT];
  /* modal yes/no */
  GoofConfirmId confirm;
  int confirm_yes;                 /* 1 = YES highlighted */
  /* status line */
  char status[GOOF_UI_COLS + 1];
  uint8_t status_color;
  /* Input section */
  const GoofBindings *runtime;     /* live bindings, owned by the host */
  GoofBindings working;            /* edited copy; Save applies it     */
  int player;
  GoofSnesAction action;
  GoofCaptureState capture;
  GoofBindKind capture_kind;
  GoofUiEvent capture_trigger;
  GoofSettingsHost host;
  bool discarded_on_close;         /* last close dropped unsaved edits */
  unsigned save_count;             /* successful Saves since init      */
  /* E3 Video / Audio sections (inactive unless attached) */
  bool host_pages;
  GoofHostPages pages;
  GoofVideoSettings video_working;
  GoofAudioSettings audio_working;
  bool video_previewed, audio_previewed;   /* live state != committed */
} GoofMenu;

void goof_menu_init(GoofMenu *m, const GoofBindings *runtime, GoofSettingsHost host);
/* E3: adds VIDEO and AUDIO to the Settings root (call once, after init). */
void goof_menu_attach_host_pages(GoofMenu *m, GoofHostPages pages);
void goof_menu_open(GoofMenu *m);
void goof_menu_close(GoofMenu *m);
bool goof_menu_is_open(const GoofMenu *m);
bool goof_menu_dirty(const GoofMenu *m);          /* Input working copy   */
bool goof_menu_video_dirty(const GoofMenu *m);    /* E3                   */
bool goof_menu_audio_dirty(const GoofMenu *m);    /* E3                   */
GoofMenuPageId goof_menu_page(const GoofMenu *m);

/* Feeds one event.  Closed menu: only F2 is consumed (it opens).  Open
 * menu: every event is consumed. */
unsigned goof_menu_handle(GoofMenu *m, const GoofUiEvent *ev);

void goof_menu_draw(const GoofMenu *m, GoofUiDrawList *out);

/* Draw-list helpers shared by the pages. */
void goof_ui_text(GoofUiDrawList *d, int col, int row, GoofUiColor c,
                  const char *fmt, ...);
void goof_ui_rect(GoofUiDrawList *d, int x, int y, int w, int h, GoofUiColor c);
void goof_ui_row_bar(GoofUiDrawList *d, int row, GoofUiColor c);
void goof_menu_set_status(GoofMenu *m, GoofUiColor c, const char *fmt, ...);

/* Navigation vocabulary derived from events (exposed for the tests). */
typedef enum {
  GOOF_NAV_NONE, GOOF_NAV_UP, GOOF_NAV_DOWN, GOOF_NAV_LEFT, GOOF_NAV_RIGHT,
  GOOF_NAV_CONFIRM, GOOF_NAV_BACK, GOOF_NAV_TOGGLE
} GoofNav;
GoofNav goof_menu_nav_of(const GoofUiEvent *ev);

/* Keys the menu itself uses (USB HID usages). */
enum {
  GOOF_UI_SC_F2 = 59, GOOF_UI_SC_ESCAPE = 41, GOOF_UI_SC_RETURN = 40,
  GOOF_UI_SC_KP_ENTER = 88, GOOF_UI_SC_P = 19,
  GOOF_UI_SC_RIGHT = 79, GOOF_UI_SC_LEFT = 80, GOOF_UI_SC_DOWN = 81,
  GOOF_UI_SC_UP = 82,
};

#endif
