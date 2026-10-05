/* Goof Troop Recomp -- Settings > Input pages.  SDL-FREE.
 * GOOF_ENHANCEMENTS_E2_BUTTON_REMAPPING_CONFIG.
 *
 *   INPUT    Player 1 / Player 2 / Reset defaults / Save / Discard / Back
 *   PLAYER   the 12 SNES buttons with their keyboard and controller inputs
 *   EDIT     one button: change or clear keyboard / controller, save, undo
 *
 * Every edit goes to m->working.  Nothing reaches the live bindings until
 * Save, and Save refuses while any conflict exists (goof_bindings.h). */
#include "host/ui/goof_input_menu.h"

#include <stdio.h>
#include <string.h>

static const char *kind_label(GoofBindKind k) {
  return k == GOOF_BIND_KEY ? "KEYBOARD" : "CONTROLLER";
}

void goof_input_menu_list_label(const GoofBindList *l, GoofBindKind kind,
                                char *out, size_t cap, size_t width) {
  if (!cap) return;
  out[0] = '\0';
  if (width + 1 > cap) width = cap - 1;
  if (l->count == 0) { snprintf(out, cap, "-"); return; }
  char buf[128] = "";
  for (int i = 0; i < l->count; i++) {
    char name[24];
    if (kind == GOOF_BIND_KEY) goof_key_label(l->code[i], name, sizeof name);
    else snprintf(name, sizeof name, "%s", goof_pad_label(l->code[i]));
    size_t n = strlen(buf);
    snprintf(buf + n, sizeof buf - n, "%s%s", i ? "/" : "", name);
  }
  if (strlen(buf) > width) {           /* truncated: mark with a trailing + */
    buf[width - 1] = '+';
    buf[width] = '\0';
  }
  snprintf(out, cap, "%s", buf);
}

static void code_label(GoofBindKind kind, uint16_t code, char *out, size_t cap) {
  if (kind == GOOF_BIND_KEY) goof_key_label(code, out, cap);
  else snprintf(out, cap, "%s", goof_pad_label(code));
}

/* Another (player, action) using `code` in a conflicting way, if any. */
static bool other_use(const GoofBindings *b, int player, GoofBindKind kind,
                      GoofSnesAction a, uint16_t code, int *op, int *oa) {
  for (int p = 0; p < GOOF_HOST_PLAYERS; p++) {
    if (p != player && kind == GOOF_BIND_PAD) continue;
    for (int x = 0; x < GOOF_ACT_COUNT; x++) {
      if (p == player && x == (int)a) continue;
      if (goof_bindlist_contains(&b->player[p].list[kind][x], code)) {
        *op = p; *oa = x;
        return true;
      }
    }
  }
  return false;
}

static GoofBindList *working_list(GoofMenu *m, GoofBindKind k) {
  return &m->working.player[m->player].list[k][m->action];
}

void goof_input_menu_on_open(GoofMenu *m) {
  if (m->runtime) m->working = *m->runtime;
  else goof_bindings_defaults(&m->working);
  m->player = 0;
  m->action = GOOF_ACT_UP;
}

/* ---- save / confirm --------------------------------------------------- */

unsigned goof_input_menu_save(GoofMenu *m) {
  GoofBindConflict c;
  size_t n = goof_bindings_conflicts(&m->working, &c, 1);
  if (n) {
    char name[24];
    code_label(c.kind, c.code, name, sizeof name);
    goof_menu_set_status(m, GOOF_UI_C_WARN, "CANNOT SAVE: %s = P%d %s + P%d %s",
                         name, c.player_a + 1, goof_action_label(c.action_a),
                         c.player_b + 1, goof_action_label(c.action_b));
    return 0;
  }
  if (!m->host.save) {
    goof_menu_set_status(m, GOOF_UI_C_WARN, "SAVE UNAVAILABLE");
    return 0;
  }
  char msg[GOOF_UI_COLS + 1] = "";
  GoofSaveResult r = m->host.save(m->host.user, &m->working, msg, sizeof msg);
  bool ok = r != GOOF_SAVE_FAILED;
  if (ok) m->save_count++;
  goof_menu_set_status(m, r == GOOF_SAVE_PERSISTED ? GOOF_UI_C_OK : GOOF_UI_C_WARN,
                       "%s", msg[0] ? msg : (ok ? "SAVED" : "SAVE FAILED"));
  return ok ? GOOF_MENU_SAVED : 0;
}

const char *goof_input_menu_confirm_text(GoofConfirmId id) {
  switch (id) {
    case GOOF_CONFIRM_RESET:        return "RESET BOTH PLAYERS TO DEFAULTS?";
    case GOOF_CONFIRM_DISCARD_BACK: return "DISCARD UNSAVED CHANGES?";
    default:                        return "";
  }
}

void goof_input_menu_confirm(GoofMenu *m, GoofConfirmId id, bool yes) {
  if (id == GOOF_CONFIRM_RESET) {
    if (!yes) { goof_menu_set_status(m, GOOF_UI_C_DIM, "RESET CANCELLED"); return; }
    goof_bindings_defaults(&m->working);
    if (goof_menu_dirty(m))
      goof_menu_set_status(m, GOOF_UI_C_OK, "DEFAULTS LOADED - SAVE TO KEEP");
    else
      goof_menu_set_status(m, GOOF_UI_C_OK, "ALREADY USING THE DEFAULTS");
  } else if (id == GOOF_CONFIRM_DISCARD_BACK) {
    if (!yes) return;
    if (m->runtime) m->working = *m->runtime;
    goof_menu_pop(m);
    goof_menu_set_status(m, GOOF_UI_C_DIM, "CHANGES DISCARDED");
  }
}

/* ---- INPUT page ------------------------------------------------------- */

enum { IN_P1, IN_P2, IN_RESET, IN_SAVE, IN_DISCARD, IN_BACK, IN_COUNT };
static const char *const kInputItems[IN_COUNT] = {
  "PLAYER 1", "PLAYER 2", "RESET DEFAULTS", "SAVE", "DISCARD CHANGES", "BACK",
};

static int input_count(const GoofMenu *m) { (void)m; return IN_COUNT; }

static void input_back(GoofMenu *m) {
  if (goof_menu_dirty(m)) goof_menu_ask(m, GOOF_CONFIRM_DISCARD_BACK);
  else goof_menu_pop(m);
}

static void input_activate(GoofMenu *m, int item, const GoofUiEvent *trigger) {
  (void)trigger;
  switch (item) {
    case IN_P1: case IN_P2:
      m->player = item == IN_P1 ? 0 : 1;
      goof_menu_push(m, GOOF_PAGE_PLAYER);
      break;
    case IN_RESET: goof_menu_ask(m, GOOF_CONFIRM_RESET); break;
    case IN_SAVE: goof_input_menu_save(m); break;
    case IN_DISCARD:
      if (m->runtime) m->working = *m->runtime;
      goof_menu_set_status(m, GOOF_UI_C_DIM, "CHANGES DISCARDED");
      break;
    default: input_back(m); break;
  }
}

static void input_draw(const GoofMenu *m, GoofUiDrawList *d) {
  int sel = m->sel[GOOF_PAGE_INPUT];
  for (int i = 0; i < IN_COUNT; i++) {
    int row = 3 + i;
    if (i == sel) goof_ui_row_bar(d, row, GOOF_UI_C_SEL_BAR);
    goof_ui_text(d, 1, row, i == sel ? GOOF_UI_C_SEL_TEXT : GOOF_UI_C_TEXT, "%s",
                 kInputItems[i]);
  }
  size_t conflicts = goof_bindings_conflicts(&m->working, NULL, 0);
  if (conflicts)
    goof_ui_text(d, 1, 10, GOOF_UI_C_WARN, "%zu CONFLICT%s - FIX BEFORE SAVING",
                 conflicts, conflicts == 1 ? "" : "S");
  goof_ui_text(d, 1, 12, GOOF_UI_C_DIM, "SAVE APPLIES NOW AND WRITES TO:");
  goof_ui_text(d, 1, 13, GOOF_UI_C_DIM, "%s",
               m->host.save_target ? m->host.save_target : "-");
  goof_ui_text(d, 1, 15, GOOF_UI_C_DIM, "DEFAULT CONTROLLER LAYOUT IS BY");
  goof_ui_text(d, 1, 16, GOOF_UI_C_DIM, "POSITION: BOTTOM=B RIGHT=A LEFT=Y TOP=X");
}

const GoofMenuPageDef goof_input_page_def = {
  "INPUT", input_count, input_activate, input_back, NULL, input_draw,
};

/* ---- PLAYER page ------------------------------------------------------ */

enum { PL_SAVE = GOOF_ACT_COUNT, PL_BACK, PL_COUNT };

static int player_count(const GoofMenu *m) { (void)m; return PL_COUNT; }

static void player_activate(GoofMenu *m, int item, const GoofUiEvent *trigger) {
  (void)trigger;
  if (item < GOOF_ACT_COUNT) {
    m->action = (GoofSnesAction)item;
    goof_menu_push(m, GOOF_PAGE_EDIT);
  } else if (item == PL_SAVE) {
    goof_input_menu_save(m);
  } else {
    goof_menu_pop(m);
  }
}

static void player_lateral(GoofMenu *m, int dir) {
  (void)dir;
  m->player ^= 1;                        /* two players: either way flips */
  m->status[0] = '\0';
}

static bool action_changed(const GoofMenu *m, int player, int a) {
  if (!m->runtime) return false;
  for (int k = 0; k < GOOF_BIND_KINDS; k++) {
    const GoofBindList *x = &m->working.player[player].list[k][a];
    const GoofBindList *y = &m->runtime->player[player].list[k][a];
    if (x->count != y->count || memcmp(x->code, y->code, sizeof x->code) != 0)
      return true;
  }
  return false;
}

static void player_draw(const GoofMenu *m, GoofUiDrawList *d) {
  int sel = m->sel[GOOF_PAGE_PLAYER];
  goof_ui_text(d, 0, 2, GOOF_UI_C_DIM, "BUTTON KEYBOARD        CONTROLLER");
  for (int i = 0; i < PL_COUNT; i++) {
    int row = 3 + i;
    bool s = i == sel;
    if (s) goof_ui_row_bar(d, row, GOOF_UI_C_SEL_BAR);
    GoofUiColor base = s ? GOOF_UI_C_SEL_TEXT : GOOF_UI_C_TEXT;
    if (i >= GOOF_ACT_COUNT) {
      goof_ui_text(d, 0, row, base, "%s", i == PL_SAVE ? "SAVE" : "BACK");
      continue;
    }
    GoofSnesAction a = (GoofSnesAction)i;
    goof_ui_text(d, 0, row, base, "%s%s", goof_action_label(a),
                 action_changed(m, m->player, i) ? "*" : "");
    for (int k = 0; k < GOOF_BIND_KINDS; k++) {
      const GoofBindList *l = &m->working.player[m->player].list[k][a];
      char text[32];
      goof_input_menu_list_label(l, (GoofBindKind)k, text, sizeof text,
                                 k == GOOF_BIND_KEY ? 15 : 19);
      GoofUiColor c = base;
      if (goof_bindings_action_conflicted(&m->working, m->player, (GoofBindKind)k, a))
        c = GOOF_UI_C_WARN;
      else if (l->count == 0 && !s)
        c = GOOF_UI_C_DIM;
      goof_ui_text(d, k == GOOF_BIND_KEY ? 7 : 23, row, c, "%s", text);
    }
  }
  goof_ui_text(d, 0, 17, GOOF_UI_C_DIM, "LEFT/RIGHT: OTHER PLAYER   * = CHANGED");
}

const GoofMenuPageDef goof_player_page_def = {
  "PLAYER", player_count, player_activate, NULL, player_lateral, player_draw,
};

/* ---- EDIT page -------------------------------------------------------- */

enum { ED_CHANGE_KEY, ED_CHANGE_PAD, ED_CLEAR_KEY, ED_CLEAR_PAD, ED_SAVE,
       ED_UNDO, ED_BACK, ED_COUNT };
static const char *const kEditItems[ED_COUNT] = {
  "CHANGE KEYBOARD", "CHANGE CONTROLLER", "CLEAR KEYBOARD", "CLEAR CONTROLLER",
  "SAVE", "UNDO CHANGES TO THIS BUTTON", "BACK",
};

static int edit_count(const GoofMenu *m) { (void)m; return ED_COUNT; }

static void start_capture(GoofMenu *m, GoofBindKind kind, const GoofUiEvent *trigger) {
  m->capture_kind = kind;
  m->status[0] = '\0';
  /* ARM/RELEASE BOUNDARY: the press that chose "Change" must be released
   * before anything is captured, so it can never bind itself.  Only an
   * unknown trigger (not a press) skips straight to ARMED. */
  if (trigger && (trigger->type == GOOF_UI_KEY_DOWN || trigger->type == GOOF_UI_PAD_DOWN)) {
    m->capture_trigger = *trigger;
    m->capture = GOOF_CAPTURE_ARMING;
  } else {
    m->capture = GOOF_CAPTURE_ARMED;
  }
}

static void report_unbound(GoofMenu *m) {
  const GoofPlayerBindings *p = &m->working.player[m->player];
  if (p->list[GOOF_BIND_KEY][m->action].count == 0 &&
      p->list[GOOF_BIND_PAD][m->action].count == 0)
    goof_menu_set_status(m, GOOF_UI_C_WARN, "P%d %s IS NOW UNBOUND (NOT SAVED)",
                         m->player + 1, goof_action_label(m->action));
}

static void edit_activate(GoofMenu *m, int item, const GoofUiEvent *trigger) {
  switch (item) {
    case ED_CHANGE_KEY: start_capture(m, GOOF_BIND_KEY, trigger); break;
    case ED_CHANGE_PAD: start_capture(m, GOOF_BIND_PAD, trigger); break;
    case ED_CLEAR_KEY: case ED_CLEAR_PAD: {
      GoofBindKind k = item == ED_CLEAR_KEY ? GOOF_BIND_KEY : GOOF_BIND_PAD;
      goof_bindlist_clear(working_list(m, k));
      goof_menu_set_status(m, GOOF_UI_C_OK, "P%d %s %s CLEARED (NOT SAVED)",
                           m->player + 1, goof_action_label(m->action), kind_label(k));
      report_unbound(m);
      break;
    }
    case ED_SAVE: goof_input_menu_save(m); break;
    case ED_UNDO:
      if (m->runtime)
        for (int k = 0; k < GOOF_BIND_KINDS; k++)
          *working_list(m, (GoofBindKind)k) =
              m->runtime->player[m->player].list[k][m->action];
      goof_menu_set_status(m, GOOF_UI_C_DIM, "P%d %s RESTORED TO SAVED VALUE",
                           m->player + 1, goof_action_label(m->action));
      break;
    default: goof_menu_pop(m); break;
  }
}

static void edit_draw(const GoofMenu *m, GoofUiDrawList *d) {
  goof_ui_text(d, 0, 2, GOOF_UI_C_TITLE, "PLAYER %d  SNES %s", m->player + 1,
               goof_action_label(m->action));
  for (int k = 0; k < GOOF_BIND_KINDS; k++) {
    const GoofBindList *l = &m->working.player[m->player].list[k][m->action];
    char text[40];
    goof_input_menu_list_label(l, (GoofBindKind)k, text, sizeof text, 30);
    bool conflicted = goof_bindings_action_conflicted(&m->working, m->player,
                                                      (GoofBindKind)k, m->action);
    goof_ui_text(d, 0, 3 + k, GOOF_UI_C_DIM, "%s", kind_label((GoofBindKind)k));
    goof_ui_text(d, 11, 3 + k, conflicted ? GOOF_UI_C_WARN : GOOF_UI_C_TEXT, "%s", text);
  }
  int sel = m->sel[GOOF_PAGE_EDIT];
  for (int i = 0; i < ED_COUNT; i++) {
    int row = 6 + i;
    if (i == sel) goof_ui_row_bar(d, row, GOOF_UI_C_SEL_BAR);
    goof_ui_text(d, 1, row, i == sel ? GOOF_UI_C_SEL_TEXT : GOOF_UI_C_TEXT, "%s",
                 kEditItems[i]);
  }
  /* Name the first conflict of this button, if any. */
  int line = 14;
  for (int k = 0; k < GOOF_BIND_KINDS && line < 16; k++) {
    const GoofBindList *l = &m->working.player[m->player].list[k][m->action];
    for (int i = 0; i < l->count && line < 16; i++) {
      int op, oa;
      if (!other_use(&m->working, m->player, (GoofBindKind)k, m->action,
                     l->code[i], &op, &oa))
        continue;
      char name[24];
      code_label((GoofBindKind)k, l->code[i], name, sizeof name);
      goof_ui_text(d, 0, line++, GOOF_UI_C_WARN, "CONFLICT: %s IS ALSO P%d %s", name,
                   op + 1, goof_action_label((GoofSnesAction)oa));
    }
  }
  if (line == 14)
    goof_ui_text(d, 0, 15, GOOF_UI_C_DIM, "CHANGE REPLACES THIS BINDING");
}

const GoofMenuPageDef goof_edit_page_def = {
  "EDIT", edit_count, edit_activate, NULL, NULL, edit_draw,
};

/* ---- capture ---------------------------------------------------------- */

static void capture_apply(GoofMenu *m, uint16_t code) {
  GoofBindKind k = m->capture_kind;
  goof_bindlist_set_single(working_list(m, k), code);
  m->capture = GOOF_CAPTURE_NONE;
  char name[24];
  code_label(k, code, name, sizeof name);
  int op, oa;
  if (other_use(&m->working, m->player, k, m->action, code, &op, &oa))
    goof_menu_set_status(m, GOOF_UI_C_WARN, "P%d %s = %s - FIX CONFLICT TO SAVE",
                         m->player + 1, goof_action_label(m->action), name);
  else
    goof_menu_set_status(m, GOOF_UI_C_OK, "P%d %s = %s (NOT SAVED YET)",
                         m->player + 1, goof_action_label(m->action), name);
}

static void capture_cancel(GoofMenu *m) {
  m->capture = GOOF_CAPTURE_NONE;
  goof_menu_set_status(m, GOOF_UI_C_DIM, "CAPTURE CANCELLED");
}

unsigned goof_input_menu_capture_event(GoofMenu *m, const GoofUiEvent *ev) {
  bool key_press = ev->type == GOOF_UI_KEY_DOWN && !ev->repeat;
  if (key_press && ev->code == GOOF_UI_SC_ESCAPE) { capture_cancel(m); return 0; }

  if (m->capture == GOOF_CAPTURE_ARMING) {
    const GoofUiEvent *t = &m->capture_trigger;
    bool released =
        (t->type == GOOF_UI_KEY_DOWN && ev->type == GOOF_UI_KEY_UP && ev->code == t->code) ||
        (t->type == GOOF_UI_PAD_DOWN && ev->type == GOOF_UI_PAD_UP &&
         ev->code == t->code && ev->device == t->device);
    if (released) m->capture = GOOF_CAPTURE_ARMED;
    return 0;                        /* nothing is captured while arming */
  }

  if (key_press && ev->code == GOOF_UI_SC_F2) {
    goof_menu_set_status(m, GOOF_UI_C_WARN, "F2 IS RESERVED FOR SETTINGS");
    return 0;
  }
  if (m->capture_kind == GOOF_BIND_KEY) {
    if (key_press) {
      if (goof_key_is_reserved(ev->code))
        goof_menu_set_status(m, GOOF_UI_C_WARN, "P IS RESERVED FOR PAUSE");
      else if (goof_key_is_valid(ev->code))
        capture_apply(m, ev->code);
    } else if (ev->type == GOOF_UI_PAD_DOWN && ev->code == GOOF_HOST_BTN_EAST) {
      capture_cancel(m);             /* pad users can back out of key capture */
    }
  } else if (ev->type == GOOF_UI_PAD_DOWN && goof_pad_is_valid(ev->code)) {
    capture_apply(m, ev->code);
  }
  return 0;
}

void goof_input_menu_draw_capture(const GoofMenu *m, GoofUiDrawList *d) {
  int x = GOOF_UI_ORIGIN_X + 2 * GOOF_UI_CELL_W, y = GOOF_UI_ORIGIN_Y + 7 * GOOF_UI_CELL_H;
  goof_ui_rect(d, x - 4, y - 4, 38 * GOOF_UI_CELL_W + 8, 5 * GOOF_UI_CELL_H + 6,
               GOOF_UI_C_BOX);
  bool key = m->capture_kind == GOOF_BIND_KEY;
  goof_ui_text(d, 3, 7, GOOF_UI_C_TITLE, "PLAYER %d SNES %s", m->player + 1,
               goof_action_label(m->action));
  if (m->capture == GOOF_CAPTURE_ARMING)
    goof_ui_text(d, 3, 9, GOOF_UI_C_TEXT, "RELEASE THE BUTTON YOU PRESSED...");
  else
    goof_ui_text(d, 3, 9, GOOF_UI_C_OK, key ? "PRESS A KEY NOW" : "PRESS A CONTROLLER BUTTON NOW");
  goof_ui_text(d, 3, 11, GOOF_UI_C_DIM, key ? "ESC OR PAD RIGHT: CANCEL" : "ESC: CANCEL");
}
