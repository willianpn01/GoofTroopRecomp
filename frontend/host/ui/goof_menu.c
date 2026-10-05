/* Goof Troop Recomp -- host Settings overlay core.  SDL-FREE.
 * See goof_menu.h.  GOOF_ENHANCEMENTS_E2; VIDEO / AUDIO sections E3. */
#include "host/ui/goof_menu.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "host/ui/goof_audio_menu.h"
#include "host/ui/goof_input_menu.h"
#include "host/ui/goof_video_menu.h"

/* ---- draw helpers ----------------------------------------------------- */

void goof_ui_rect(GoofUiDrawList *d, int x, int y, int w, int h, GoofUiColor c) {
  if (d->nrect >= GOOF_UI_MAX_RECTS) return;
  d->rect[d->nrect++] = (GoofUiRect){(int16_t)x, (int16_t)y, (int16_t)w,
                                     (int16_t)h, (uint8_t)c, d->layer};
}

void goof_ui_text(GoofUiDrawList *d, int col, int row, GoofUiColor c,
                  const char *fmt, ...) {
  if (d->ntext >= GOOF_UI_MAX_TEXTS || col >= GOOF_UI_COLS) return;
  GoofUiText *t = &d->text[d->ntext++];
  t->x = (int16_t)(GOOF_UI_ORIGIN_X + col * GOOF_UI_CELL_W);
  t->y = (int16_t)(GOOF_UI_ORIGIN_Y + row * GOOF_UI_CELL_H + 1);
  t->color = (uint8_t)c;
  t->layer = d->layer;
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  size_t room = (size_t)(GOOF_UI_COLS - col);      /* clip at the panel edge */
  size_t n = strlen(buf);
  if (n > room) n = room;
  memcpy(t->text, buf, n);
  t->text[n] = '\0';
}

void goof_ui_row_bar(GoofUiDrawList *d, int row, GoofUiColor c) {
  goof_ui_rect(d, GOOF_UI_ORIGIN_X - 2, GOOF_UI_ORIGIN_Y + row * GOOF_UI_CELL_H - 1,
               GOOF_UI_COLS * GOOF_UI_CELL_W + 2, GOOF_UI_CELL_H, c);
}

void goof_menu_set_status(GoofMenu *m, GoofUiColor c, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(m->status, sizeof m->status, fmt, ap);
  va_end(ap);
  m->status_color = (uint8_t)c;
}

/* ---- root page -------------------------------------------------------- */
/* Sections of the Settings root.  E3 appends Video / Audio here; they are
 * listed only when the frontend attached them (goof_menu_attach_host_pages),
 * so an E2-style menu keeps exactly INPUT + RESUME GAME. */
typedef struct { const char *label; GoofMenuPageId page; bool host_page; } RootSection;
static const RootSection kRootSections[] = {
  {"INPUT", GOOF_PAGE_INPUT, false},
  {"VIDEO", GOOF_PAGE_VIDEO, true},
  {"AUDIO", GOOF_PAGE_AUDIO, true},
};
enum { ROOT_SECTIONS = (int)(sizeof kRootSections / sizeof kRootSections[0]) };

/* Visible sections in order; returns how many. */
static int root_sections(const GoofMenu *m, const RootSection **out) {
  int n = 0;
  for (int i = 0; i < ROOT_SECTIONS; i++)
    if (!kRootSections[i].host_page || m->host_pages) out[n++] = &kRootSections[i];
  return n;
}

static int root_count(const GoofMenu *m) {
  const RootSection *v[ROOT_SECTIONS];
  return root_sections(m, v) + 1;
}

static void root_activate(GoofMenu *m, int item, const GoofUiEvent *trigger) {
  (void)trigger;
  const RootSection *v[ROOT_SECTIONS];
  int n = root_sections(m, v);
  if (item < n) goof_menu_push(m, v[item]->page);
  else goof_menu_close(m);                               /* RESUME GAME */
}

static void root_back(GoofMenu *m) { goof_menu_close(m); }

static void root_draw(const GoofMenu *m, GoofUiDrawList *d) {
  int sel = m->sel[GOOF_PAGE_ROOT];
  const RootSection *v[ROOT_SECTIONS];
  int n = root_sections(m, v);
  for (int i = 0; i <= n; i++) {
    int row = 3 + i;
    const char *label = i < n ? v[i]->label : "RESUME GAME";
    if (i == sel) goof_ui_row_bar(d, row, GOOF_UI_C_SEL_BAR);
    goof_ui_text(d, 1, row, i == sel ? GOOF_UI_C_SEL_TEXT : GOOF_UI_C_TEXT,
                 "%s", label);
  }
  int note = 3 + n + 2 > 7 ? 3 + n + 2 : 7;      /* E2 position when it fits */
  goof_ui_text(d, 1, note, GOOF_UI_C_DIM, "THE GAME IS PAUSED WHILE THIS");
  goof_ui_text(d, 1, note + 1, GOOF_UI_C_DIM, "MENU IS OPEN.");
}

static const GoofMenuPageDef kRootPage = {
  "SETTINGS", root_count, root_activate, root_back, NULL, root_draw,
};

static const GoofMenuPageDef *page_def(GoofMenuPageId id) {
  switch (id) {
    case GOOF_PAGE_INPUT:  return &goof_input_page_def;
    case GOOF_PAGE_PLAYER: return &goof_player_page_def;
    case GOOF_PAGE_EDIT:   return &goof_edit_page_def;
    case GOOF_PAGE_VIDEO:  return &goof_video_page_def;
    case GOOF_PAGE_AUDIO:  return &goof_audio_page_def;
    default:               return &kRootPage;
  }
}

/* ---- state ------------------------------------------------------------ */

void goof_menu_init(GoofMenu *m, const GoofBindings *runtime, GoofSettingsHost host) {
  memset(m, 0, sizeof *m);
  m->runtime = runtime;
  m->host = host;
}

void goof_menu_attach_host_pages(GoofMenu *m, GoofHostPages pages) {
  m->pages = pages;
  m->host_pages = pages.video && pages.audio;
  if (pages.video) m->video_working = *pages.video;
  if (pages.audio) m->audio_working = *pages.audio;
}

bool goof_menu_is_open(const GoofMenu *m) { return m->open; }

bool goof_menu_video_dirty(const GoofMenu *m) {
  return m->host_pages && !goof_video_equal(&m->video_working, m->pages.video);
}

bool goof_menu_audio_dirty(const GoofMenu *m) {
  return m->host_pages && !goof_audio_equal(&m->audio_working, m->pages.audio);
}

bool goof_menu_dirty(const GoofMenu *m) {
  return m->runtime && !goof_bindings_equal(&m->working, m->runtime);
}

GoofMenuPageId goof_menu_page(const GoofMenu *m) {
  return m->depth > 0 ? m->stack[m->depth - 1] : GOOF_PAGE_ROOT;
}

void goof_menu_push(GoofMenu *m, GoofMenuPageId page) {
  if (m->depth >= (int)(sizeof m->stack / sizeof m->stack[0])) return;
  m->stack[m->depth++] = page;
  m->sel[page] = 0;
  m->status[0] = '\0';
}

void goof_menu_pop(GoofMenu *m) {
  if (m->depth > 1) m->depth--;
  else goof_menu_close(m);
}

void goof_menu_ask(GoofMenu *m, GoofConfirmId id) {
  m->confirm = id;
  m->confirm_yes = 0;                      /* default answer: NO */
}

void goof_menu_open(GoofMenu *m) {
  /* Every open starts from the LIVE bindings (U15). */
  m->open = true;
  m->depth = 0;
  memset(m->sel, 0, sizeof m->sel);
  m->confirm = GOOF_CONFIRM_NONE;
  m->capture = GOOF_CAPTURE_NONE;
  m->status[0] = '\0';
  m->discarded_on_close = false;
  goof_menu_push(m, GOOF_PAGE_ROOT);
  goof_input_menu_on_open(m);
  goof_video_menu_on_open(m);
  goof_audio_menu_on_open(m);
}

void goof_menu_close(GoofMenu *m) {
  /* Closing never applies anything: unsaved edits are dropped, and a live
   * Video / Audio preview is put back to the committed values. */
  m->discarded_on_close = goof_menu_dirty(m) || goof_menu_video_dirty(m) ||
                          goof_menu_audio_dirty(m);
  if (m->runtime) m->working = *m->runtime;
  goof_video_menu_revert(m);
  goof_audio_menu_revert(m);
  m->open = false;
  m->depth = 0;
  m->confirm = GOOF_CONFIRM_NONE;
  m->capture = GOOF_CAPTURE_NONE;
}

GoofNav goof_menu_nav_of(const GoofUiEvent *ev) {
  if (ev->type == GOOF_UI_KEY_DOWN) {
    switch (ev->code) {
      case GOOF_UI_SC_UP:    return GOOF_NAV_UP;
      case GOOF_UI_SC_DOWN:  return GOOF_NAV_DOWN;
      case GOOF_UI_SC_LEFT:  return GOOF_NAV_LEFT;
      case GOOF_UI_SC_RIGHT: return GOOF_NAV_RIGHT;
      default: break;
    }
    if (ev->repeat) return GOOF_NAV_NONE;      /* only movement auto-repeats */
    switch (ev->code) {
      case GOOF_UI_SC_RETURN:
      case GOOF_UI_SC_KP_ENTER: return GOOF_NAV_CONFIRM;
      case GOOF_UI_SC_ESCAPE:   return GOOF_NAV_BACK;
      case GOOF_UI_SC_F2:       return GOOF_NAV_TOGGLE;
      default: return GOOF_NAV_NONE;
    }
  }
  if (ev->type == GOOF_UI_PAD_DOWN) {
    /* Physical positions, as everywhere since E1: bottom confirms, right
     * goes back, whatever letters the pad prints. */
    switch (ev->code) {
      case GOOF_HOST_BTN_DPAD_UP:    case GOOF_PAD_LSTICK_UP:    return GOOF_NAV_UP;
      case GOOF_HOST_BTN_DPAD_DOWN:  case GOOF_PAD_LSTICK_DOWN:  return GOOF_NAV_DOWN;
      case GOOF_HOST_BTN_DPAD_LEFT:  case GOOF_PAD_LSTICK_LEFT:  return GOOF_NAV_LEFT;
      case GOOF_HOST_BTN_DPAD_RIGHT: case GOOF_PAD_LSTICK_RIGHT: return GOOF_NAV_RIGHT;
      case GOOF_HOST_BTN_SOUTH: return GOOF_NAV_CONFIRM;
      case GOOF_HOST_BTN_EAST:  return GOOF_NAV_BACK;
      default: return GOOF_NAV_NONE;
    }
  }
  return GOOF_NAV_NONE;
}

static unsigned handle_confirm(GoofMenu *m, GoofNav nav) {
  switch (nav) {
    case GOOF_NAV_LEFT: case GOOF_NAV_RIGHT: case GOOF_NAV_UP: case GOOF_NAV_DOWN:
      m->confirm_yes = !m->confirm_yes;
      break;
    case GOOF_NAV_CONFIRM: case GOOF_NAV_BACK: {
      GoofConfirmId id = m->confirm;
      bool yes = nav == GOOF_NAV_CONFIRM && m->confirm_yes;
      m->confirm = GOOF_CONFIRM_NONE;
      if (id == GOOF_CONFIRM_VIDEO_RESET || id == GOOF_CONFIRM_VIDEO_DISCARD_BACK)
        goof_video_menu_confirm(m, id, yes);
      else if (id == GOOF_CONFIRM_AUDIO_RESET || id == GOOF_CONFIRM_AUDIO_DISCARD_BACK)
        goof_audio_menu_confirm(m, id, yes);
      else
        goof_input_menu_confirm(m, id, yes);
      break;
    }
    default: break;
  }
  return GOOF_MENU_CONSUMED;
}

unsigned goof_menu_handle(GoofMenu *m, const GoofUiEvent *ev) {
  GoofNav nav = goof_menu_nav_of(ev);
  if (!m->open) {
    if (nav != GOOF_NAV_TOGGLE) return 0;       /* not ours: game input */
    goof_menu_open(m);
    return GOOF_MENU_CONSUMED | GOOF_MENU_OPENED;
  }
  /* Open: every event is the host's. */
  if (m->capture != GOOF_CAPTURE_NONE)
    return GOOF_MENU_CONSUMED | goof_input_menu_capture_event(m, ev);
  if (nav == GOOF_NAV_TOGGLE) {
    goof_menu_close(m);
    return GOOF_MENU_CONSUMED | GOOF_MENU_CLOSED;
  }
  if (m->confirm != GOOF_CONFIRM_NONE) return handle_confirm(m, nav);

  const GoofMenuPageDef *p = page_def(goof_menu_page(m));
  GoofMenuPageId id = goof_menu_page(m);
  int count = p->count(m);
  unsigned r = GOOF_MENU_CONSUMED;
  switch (nav) {
    case GOOF_NAV_UP:   m->sel[id] = (m->sel[id] + count - 1) % count; break;
    case GOOF_NAV_DOWN: m->sel[id] = (m->sel[id] + 1) % count; break;
    case GOOF_NAV_LEFT: case GOOF_NAV_RIGHT:
      if (p->lateral) p->lateral(m, nav == GOOF_NAV_LEFT ? -1 : 1);
      break;
    case GOOF_NAV_CONFIRM: {
      unsigned saves = m->save_count;
      p->activate(m, m->sel[id], ev);
      if (m->save_count != saves) r |= GOOF_MENU_SAVED;
      break;
    }
    case GOOF_NAV_BACK:
      if (p->back) p->back(m); else goof_menu_pop(m);
      break;
    default: break;
  }
  if (!m->open) r |= GOOF_MENU_CLOSED;
  return r;
}

/* ---- drawing ---------------------------------------------------------- */

void goof_menu_draw(const GoofMenu *m, GoofUiDrawList *d) {
  memset(d, 0, sizeof *d);
  if (!m->open) return;
  goof_ui_rect(d, 0, 0, GOOF_UI_CANVAS_W, GOOF_UI_CANVAS_H, GOOF_UI_C_PANEL);

  /* Breadcrumb title. */
  char title[96] = "";
  for (int i = 0; i < m->depth; i++) {
    const GoofMenuPageDef *p = page_def(m->stack[i]);
    char part[32];
    if (m->stack[i] == GOOF_PAGE_PLAYER)
      snprintf(part, sizeof part, "PLAYER %d", m->player + 1);
    else if (m->stack[i] == GOOF_PAGE_EDIT)
      snprintf(part, sizeof part, "%s", goof_action_label(m->action));
    else
      snprintf(part, sizeof part, "%s", p->title);
    size_t n = strlen(title);
    snprintf(title + n, sizeof title - n, "%s%s", i ? " > " : "", part);
  }
  if (strlen(title) > 32) {                      /* keep the tail visible */
    char tail[40];
    snprintf(tail, sizeof tail, "..%s", title + strlen(title) - 30);
    snprintf(title, sizeof title, "%s", tail);
  }
  goof_ui_text(d, 0, 0, GOOF_UI_C_TITLE, "%s", title);
  goof_ui_text(d, 34, 0, GOOF_UI_C_DIM, "F2 CLOSE");
  goof_ui_rect(d, GOOF_UI_ORIGIN_X, GOOF_UI_ORIGIN_Y + GOOF_UI_CELL_H + 1,
               GOOF_UI_COLS * GOOF_UI_CELL_W, 1, GOOF_UI_C_RULE);

  page_def(goof_menu_page(m))->draw(m, d);

  GoofMenuPageId page = goof_menu_page(m);
  bool dirty = goof_menu_dirty(m) ||
               (page == GOOF_PAGE_VIDEO && goof_menu_video_dirty(m)) ||
               (page == GOOF_PAGE_AUDIO && goof_menu_audio_dirty(m));
  if (m->status[0])
    goof_ui_text(d, 0, 18, (GoofUiColor)m->status_color, "%s", m->status);
  else if (dirty)
    goof_ui_text(d, 0, 18, GOOF_UI_C_WARN, "UNSAVED CHANGES");

  goof_ui_rect(d, GOOF_UI_ORIGIN_X, GOOF_UI_ORIGIN_Y + 20 * GOOF_UI_CELL_H - 2,
               GOOF_UI_COLS * GOOF_UI_CELL_W, 1, GOOF_UI_C_RULE);
  goof_ui_text(d, 0, 20, GOOF_UI_C_DIM, "KEYS: ARROWS MOVE  ENTER OK  ESC BACK");
  goof_ui_text(d, 0, 21, GOOF_UI_C_DIM, "PAD:  DPAD MOVE  BOTTOM OK  RIGHT BACK");

  d->layer = 1;                                  /* modals above the page */
  if (m->confirm != GOOF_CONFIRM_NONE) {
    int x = GOOF_UI_ORIGIN_X + 3 * GOOF_UI_CELL_W, y = GOOF_UI_ORIGIN_Y + 7 * GOOF_UI_CELL_H;
    goof_ui_rect(d, x - 4, y - 4, 36 * GOOF_UI_CELL_W + 8, 5 * GOOF_UI_CELL_H + 6,
                 GOOF_UI_C_BOX);
    const char *q = goof_video_menu_confirm_text(m->confirm);
    if (!q[0]) q = goof_audio_menu_confirm_text(m->confirm);
    if (!q[0]) q = goof_input_menu_confirm_text(m->confirm);
    goof_ui_text(d, 4, 7, GOOF_UI_C_TITLE, "%s", q);
    goof_ui_text(d, 4, 8, GOOF_UI_C_DIM, "(NOTHING IS WRITTEN UNTIL SAVE)");
    int yes_col = 10, no_col = 24;
    int sel_col = m->confirm_yes ? yes_col : no_col;
    goof_ui_rect(d, GOOF_UI_ORIGIN_X + (sel_col - 1) * GOOF_UI_CELL_W - 1,
                 GOOF_UI_ORIGIN_Y + 10 * GOOF_UI_CELL_H - 1, 7 * GOOF_UI_CELL_W,
                 GOOF_UI_CELL_H, GOOF_UI_C_SEL_BAR);
    goof_ui_text(d, yes_col, 10, m->confirm_yes ? GOOF_UI_C_SEL_TEXT : GOOF_UI_C_TEXT, "YES");
    goof_ui_text(d, no_col, 10, m->confirm_yes ? GOOF_UI_C_TEXT : GOOF_UI_C_SEL_TEXT, "NO");
  }
  if (m->capture != GOOF_CAPTURE_NONE) goof_input_menu_draw_capture(m, d);
}
