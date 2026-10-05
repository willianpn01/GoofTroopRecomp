/* Goof Troop Recomp -- Settings > Audio page.  SDL-FREE.
 * GOOF_ENHANCEMENTS_E3_HOST_SETTINGS_UI.
 *
 *   AUDIO   Master volume / Mute / Mute when unfocused
 *           Reset audio defaults / Save / Cancel changes / Back
 *
 * Same transaction as Video (goof_video_menu.c).  Host audio is paused with
 * the guest while the overlay is open (E2 strategy A), so a preview is
 * applied to the host gain but is only heard after SAVE + resume; an unsaved
 * change is put back when the overlay closes. */
#include "host/ui/goof_audio_menu.h"

#include <stdio.h>

enum { AU_VOLUME, AU_MUTE, AU_MUTE_UNFOCUSED, AU_RESET, AU_SAVE, AU_CANCEL,
       AU_BACK, AU_COUNT };
static const char *const kAudioItems[AU_COUNT] = {
  "MASTER VOLUME", "MUTE", "MUTE WHEN UNFOCUSED", "RESET AUDIO DEFAULTS",
  "SAVE", "CANCEL CHANGES", "BACK",
};
enum { VALUE_COL = 22 };

void goof_audio_menu_on_open(GoofMenu *m) {
  if (!m->host_pages) return;
  m->audio_working = *m->pages.audio;
  m->audio_previewed = false;
}

static void preview(GoofMenu *m) {
  if (m->pages.preview_audio) m->pages.preview_audio(m->pages.user, &m->audio_working);
  m->audio_previewed = true;
}

void goof_audio_menu_revert(GoofMenu *m) {
  if (!m->host_pages) return;
  m->audio_working = *m->pages.audio;
  if (m->audio_previewed && m->pages.preview_audio)
    m->pages.preview_audio(m->pages.user, m->pages.audio);
  m->audio_previewed = false;
}

static void change(GoofMenu *m, int item, int dir) {
  GoofAudioSettings *a = &m->audio_working;
  switch (item) {
    case AU_VOLUME:
      if (dir == 0) {                       /* Enter: say how, change nothing */
        goof_menu_set_status(m, GOOF_UI_C_DIM, "USE LEFT/RIGHT TO CHANGE THE VOLUME");
        return;
      }
      a->master_volume = goof_audio_volume_step(a->master_volume, dir);
      break;
    case AU_MUTE: a->mute = !a->mute; break;
    case AU_MUTE_UNFOCUSED: a->mute_when_unfocused = !a->mute_when_unfocused; break;
    default: return;
  }
  m->status[0] = '\0';
  preview(m);
}

static unsigned audio_save(GoofMenu *m) {
  if (!m->pages.save_audio) {
    goof_menu_set_status(m, GOOF_UI_C_WARN, "SAVE UNAVAILABLE");
    return 0;
  }
  char msg[GOOF_UI_COLS + 1] = "";
  GoofSaveResult r = m->pages.save_audio(m->pages.user, &m->audio_working, msg, sizeof msg);
  bool ok = r != GOOF_SAVE_FAILED;
  if (ok) {
    m->save_count++;
    m->audio_previewed = false;
    m->audio_working = *m->pages.audio;
  }
  goof_menu_set_status(m, r == GOOF_SAVE_PERSISTED ? GOOF_UI_C_OK : GOOF_UI_C_WARN,
                       "%s", msg[0] ? msg : (ok ? "SAVED" : "SAVE FAILED"));
  return ok ? GOOF_MENU_SAVED : 0;
}

static void audio_back(GoofMenu *m) {
  if (goof_menu_audio_dirty(m)) goof_menu_ask(m, GOOF_CONFIRM_AUDIO_DISCARD_BACK);
  else { goof_audio_menu_revert(m); goof_menu_pop(m); }
}

static int audio_count(const GoofMenu *m) { (void)m; return AU_COUNT; }

static void audio_activate(GoofMenu *m, int item, const GoofUiEvent *trigger) {
  (void)trigger;
  switch (item) {
    case AU_VOLUME: change(m, item, 0); break;
    case AU_MUTE: case AU_MUTE_UNFOCUSED: change(m, item, +1); break;
    case AU_RESET: goof_menu_ask(m, GOOF_CONFIRM_AUDIO_RESET); break;
    case AU_SAVE: audio_save(m); break;
    case AU_CANCEL:
      goof_audio_menu_revert(m);
      goof_menu_set_status(m, GOOF_UI_C_DIM, "AUDIO CHANGES CANCELLED");
      break;
    default: audio_back(m); break;
  }
}

static void audio_lateral(GoofMenu *m, int dir) {
  change(m, m->sel[GOOF_PAGE_AUDIO], dir);
}

const char *goof_audio_menu_confirm_text(GoofConfirmId id) {
  switch (id) {
    case GOOF_CONFIRM_AUDIO_RESET:        return "RESET AUDIO TO DEFAULTS?";
    case GOOF_CONFIRM_AUDIO_DISCARD_BACK: return "DISCARD UNSAVED AUDIO CHANGES?";
    default:                              return "";
  }
}

void goof_audio_menu_confirm(GoofMenu *m, GoofConfirmId id, bool yes) {
  if (id == GOOF_CONFIRM_AUDIO_RESET) {
    if (!yes) { goof_menu_set_status(m, GOOF_UI_C_DIM, "RESET CANCELLED"); return; }
    goof_audio_defaults(&m->audio_working);
    preview(m);
    if (goof_menu_audio_dirty(m))
      goof_menu_set_status(m, GOOF_UI_C_OK, "AUDIO DEFAULTS LOADED - SAVE TO KEEP");
    else
      goof_menu_set_status(m, GOOF_UI_C_OK, "ALREADY USING THE AUDIO DEFAULTS");
  } else if (id == GOOF_CONFIRM_AUDIO_DISCARD_BACK) {
    if (!yes) return;
    goof_audio_menu_revert(m);
    goof_menu_pop(m);
    goof_menu_set_status(m, GOOF_UI_C_DIM, "AUDIO CHANGES DISCARDED");
  }
}

static bool field_changed(const GoofMenu *m, int item) {
  const GoofAudioSettings *a = &m->audio_working, *b = m->pages.audio;
  switch (item) {
    case AU_VOLUME:         return a->master_volume != b->master_volume;
    case AU_MUTE:           return a->mute != b->mute;
    case AU_MUTE_UNFOCUSED: return a->mute_when_unfocused != b->mute_when_unfocused;
    default:                return false;
  }
}

static void audio_draw(const GoofMenu *m, GoofUiDrawList *d) {
  int sel = m->sel[GOOF_PAGE_AUDIO];
  const GoofAudioSettings *a = &m->audio_working;
  for (int i = 0; i < AU_COUNT; i++) {
    int row = 3 + i;
    bool s = i == sel;
    if (s) goof_ui_row_bar(d, row, GOOF_UI_C_SEL_BAR);
    GoofUiColor c = s ? GOOF_UI_C_SEL_TEXT : GOOF_UI_C_TEXT;
    goof_ui_text(d, 1, row, c, "%s%s", kAudioItems[i], field_changed(m, i) ? "*" : "");
    switch (i) {
      case AU_VOLUME: goof_ui_text(d, VALUE_COL, row, c, "< %d%% >", a->master_volume); break;
      case AU_MUTE: goof_ui_text(d, VALUE_COL, row, c, "< %s >", a->mute ? "ON" : "OFF"); break;
      case AU_MUTE_UNFOCUSED:
        goof_ui_text(d, VALUE_COL, row, c, "< %s >", a->mute_when_unfocused ? "ON" : "OFF");
        break;
      default: break;
    }
  }
  goof_ui_text(d, 1, 12, GOOF_UI_C_DIM, "LEFT/RIGHT CHANGES A VALUE (VOLUME 5%%)");
  goof_ui_text(d, 1, 13, GOOF_UI_C_DIM, "SOUND IS PAUSED WHILE THIS MENU IS OPEN:");
  goof_ui_text(d, 1, 14, GOOF_UI_C_DIM, "SAVE, THEN RESUME THE GAME TO HEAR IT.");
  goof_ui_text(d, 1, 15, GOOF_UI_C_DIM, "AFFECTS YOUR SPEAKERS ONLY, NOT THE GAME.");
}

const GoofMenuPageDef goof_audio_page_def = {
  "AUDIO", audio_count, audio_activate, audio_back, audio_lateral, audio_draw,
};
