/* Goof Troop Recomp -- Settings > Video page.  SDL-FREE.
 * GOOF_ENHANCEMENTS_E3_HOST_SETTINGS_UI.
 *
 *   VIDEO   Display mode / Window scale / Pixel aspect / Fullscreen scaling
 *           VSync (E4) / Reset video defaults / Save / Cancel changes / Back
 *
 * GOOF_ENHANCEMENTS_E4: VSYNC OFF/ON only chooses whether a present waits for
 * the display's vertical blank.  It is previewed live like the other values
 * and never changes how fast the game runs (host/goof_frame_pacer.c owns the
 * guest cadence); there is deliberately no "game speed" or "FPS" item.
 *
 * Transaction: m->video_working is the page's working copy, taken from the
 * host's committed values (m->pages.video) when the overlay opens.  Every
 * change is previewed live (host preview_video: window / fullscreen /
 * geometry only -- the guest is paused and its framebuffer is not touched).
 * SAVE commits and persists; CANCEL, BACK-and-discard and closing the
 * overlay re-apply the committed values, so no unsaved preview survives. */
#include "host/ui/goof_video_menu.h"

#include <stdio.h>

enum { VI_DISPLAY, VI_SCALE, VI_ASPECT, VI_SCALING, VI_VSYNC, VI_FILTER, VI_RESET, VI_SAVE,
       VI_CANCEL, VI_BACK, VI_COUNT };
static const char *const kVideoItems[VI_COUNT] = {
  "DISPLAY MODE", "WINDOW SCALE", "PIXEL ASPECT", "FULLSCREEN SCALING",
  "VSYNC", "FILTER", "RESET VIDEO DEFAULTS", "SAVE", "CANCEL CHANGES", "BACK",
};
enum { VALUE_COL = 22 };

void goof_video_menu_on_open(GoofMenu *m) {
  if (!m->host_pages) return;
  m->video_working = *m->pages.video;
  m->video_previewed = false;
}

static bool preview(GoofMenu *m) {
  m->video_previewed = true;
  return !m->pages.preview_video ||
         m->pages.preview_video(m->pages.user, &m->video_working);
}

void goof_video_menu_revert(GoofMenu *m) {
  if (!m->host_pages) return;
  m->video_working = *m->pages.video;
  if (m->video_previewed && m->pages.preview_video)
    m->pages.preview_video(m->pages.user, m->pages.video);
  m->video_previewed = false;
}

static void change(GoofMenu *m, int item, int dir) {
  GoofVideoSettings *v = &m->video_working;
  GoofVideoSettings before = *v;
  switch (item) {
    case VI_DISPLAY:
      v->display = (GoofDisplayMode)((v->display + 1) % GOOF_DISPLAY_COUNT);
      break;
    case VI_SCALE: v->window_scale = goof_window_scale_step(v->window_scale, dir); break;
    case VI_ASPECT:
      v->aspect = (GoofPixelAspect)((v->aspect + 1) % GOOF_ASPECT_COUNT);
      break;
    case VI_SCALING:
      v->scaling = (GoofScalingMode)((v->scaling + 1) % GOOF_SCALING_COUNT);
      break;
    case VI_VSYNC: v->vsync = !v->vsync; break;
    case VI_FILTER:
      v->filter = (GoofFilterMode)((v->filter + (dir > 0 ? 1 : GOOF_FILTER_COUNT - 1)) % GOOF_FILTER_COUNT);
      break;
    default: return;
  }
  m->status[0] = '\0';
  if (!preview(m)) {
    /* The host refused (fullscreen unavailable): step back, keep the old
     * presentation, say so.  Nothing is written. */
    *v = before;
    preview(m);
    goof_menu_set_status(m, GOOF_UI_C_WARN, item == VI_VSYNC
                             ? "VSYNC CHANGE FAILED (SEE LOG)"
                             : "DISPLAY CHANGE FAILED (SEE LOG)");
  }
}

static unsigned video_save(GoofMenu *m) {
  if (!m->pages.save_video) {
    goof_menu_set_status(m, GOOF_UI_C_WARN, "SAVE UNAVAILABLE");
    return 0;
  }
  char msg[GOOF_UI_COLS + 1] = "";
  GoofSaveResult r = m->pages.save_video(m->pages.user, &m->video_working, msg, sizeof msg);
  bool ok = r != GOOF_SAVE_FAILED;
  if (ok) {
    m->save_count++;
    m->video_previewed = false;          /* live == committed now */
    m->video_working = *m->pages.video;  /* committed as stored by the host */
  }
  goof_menu_set_status(m, r == GOOF_SAVE_PERSISTED ? GOOF_UI_C_OK : GOOF_UI_C_WARN,
                       "%s", msg[0] ? msg : (ok ? "SAVED" : "SAVE FAILED"));
  return ok ? GOOF_MENU_SAVED : 0;
}

static void video_back(GoofMenu *m) {
  if (goof_menu_video_dirty(m)) goof_menu_ask(m, GOOF_CONFIRM_VIDEO_DISCARD_BACK);
  else { goof_video_menu_revert(m); goof_menu_pop(m); }
}

static int video_count(const GoofMenu *m) { (void)m; return VI_COUNT; }

static void video_activate(GoofMenu *m, int item, const GoofUiEvent *trigger) {
  (void)trigger;
  switch (item) {
    case VI_DISPLAY: case VI_SCALE: case VI_ASPECT: case VI_SCALING: case VI_VSYNC: case VI_FILTER:
      change(m, item, +1);
      break;
    case VI_RESET: goof_menu_ask(m, GOOF_CONFIRM_VIDEO_RESET); break;
    case VI_SAVE: video_save(m); break;
    case VI_CANCEL:
      goof_video_menu_revert(m);
      goof_menu_set_status(m, GOOF_UI_C_DIM, "VIDEO CHANGES CANCELLED");
      break;
    default: video_back(m); break;
  }
}

static void video_lateral(GoofMenu *m, int dir) {
  change(m, m->sel[GOOF_PAGE_VIDEO], dir);
}

const char *goof_video_menu_confirm_text(GoofConfirmId id) {
  switch (id) {
    case GOOF_CONFIRM_VIDEO_RESET:        return "RESET VIDEO TO DEFAULTS?";
    case GOOF_CONFIRM_VIDEO_DISCARD_BACK: return "DISCARD UNSAVED VIDEO CHANGES?";
    default:                              return "";
  }
}

void goof_video_menu_confirm(GoofMenu *m, GoofConfirmId id, bool yes) {
  if (id == GOOF_CONFIRM_VIDEO_RESET) {
    if (!yes) { goof_menu_set_status(m, GOOF_UI_C_DIM, "RESET CANCELLED"); return; }
    goof_video_defaults(&m->video_working);
    preview(m);
    if (goof_menu_video_dirty(m))
      goof_menu_set_status(m, GOOF_UI_C_OK, "VIDEO DEFAULTS LOADED - SAVE TO KEEP");
    else
      goof_menu_set_status(m, GOOF_UI_C_OK, "ALREADY USING THE VIDEO DEFAULTS");
  } else if (id == GOOF_CONFIRM_VIDEO_DISCARD_BACK) {
    if (!yes) return;
    goof_video_menu_revert(m);
    goof_menu_pop(m);
    goof_menu_set_status(m, GOOF_UI_C_DIM, "VIDEO CHANGES DISCARDED");
  }
}

static bool field_changed(const GoofMenu *m, int item) {
  const GoofVideoSettings *a = &m->video_working, *b = m->pages.video;
  switch (item) {
    case VI_DISPLAY: return a->display != b->display;
    case VI_SCALE:   return a->window_scale != b->window_scale;
    case VI_ASPECT:  return a->aspect != b->aspect;
    case VI_SCALING: return a->scaling != b->scaling;
    case VI_VSYNC:   return a->vsync != b->vsync;
    case VI_FILTER:  return a->filter != b->filter;
    default:         return false;
  }
}

static void video_draw(const GoofMenu *m, GoofUiDrawList *d) {
  int sel = m->sel[GOOF_PAGE_VIDEO];
  const GoofVideoSettings *v = &m->video_working;
  for (int i = 0; i < VI_COUNT; i++) {
    int row = 3 + i;
    bool s = i == sel;
    if (s) goof_ui_row_bar(d, row, GOOF_UI_C_SEL_BAR);
    GoofUiColor c = s ? GOOF_UI_C_SEL_TEXT : GOOF_UI_C_TEXT;
    goof_ui_text(d, 1, row, c, "%s%s", kVideoItems[i], field_changed(m, i) ? "*" : "");
    char value[16] = "";
    switch (i) {
      case VI_DISPLAY: snprintf(value, sizeof value, "%s", goof_display_label(v->display)); break;
      case VI_SCALE:   goof_window_scale_label(v->window_scale, value, sizeof value); break;
      case VI_ASPECT:  snprintf(value, sizeof value, "%s", goof_aspect_label(v->aspect)); break;
      case VI_SCALING: snprintf(value, sizeof value, "%s", goof_scaling_label(v->scaling)); break;
      case VI_VSYNC:   snprintf(value, sizeof value, "%s", goof_vsync_label(v->vsync)); break;
      case VI_FILTER:  snprintf(value, sizeof value, "%s", goof_filter_label(v->filter)); break;
      default: break;
    }
    if (value[0])
      goof_ui_text(d, VALUE_COL, row, c, "< %s >", value);
  }
  char geo[GOOF_UI_COLS + 1] = "";
  if (m->pages.describe_video)
    m->pages.describe_video(m->pages.user, v, geo, sizeof geo);
  if (geo[0]) goof_ui_text(d, 1, 13, GOOF_UI_C_DIM, "%s", geo);
  if (m->pages.video_note && m->pages.video_note[0])
    goof_ui_text(d, 1, 14, GOOF_UI_C_DIM, "%s", m->pages.video_note);
  goof_ui_text(d, 1, 15, GOOF_UI_C_DIM, "LEFT/RIGHT CHANGES A VALUE (LIVE PREVIEW)");
  goof_ui_text(d, 1, 16, GOOF_UI_C_DIM, "SAVE KEEPS IT - CANCEL RESTORES");
  goof_ui_text(d, 1, 17, GOOF_UI_C_DIM, "THE GAME IMAGE ITSELF STAYS 256X224");
  goof_ui_text(d, 1, 18, GOOF_UI_C_DIM, "VSYNC NEVER CHANGES THE GAME SPEED");
}

const GoofMenuPageDef goof_video_page_def = {
  "VIDEO", video_count, video_activate, video_back, video_lateral, video_draw,
};
