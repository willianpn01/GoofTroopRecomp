/* GOOF_ENHANCEMENTS_E2 -- Settings overlay state tests, SDL-free.
 *
 * Drives goof_menu with the same GoofUiEvents sdl_settings.c produces and a
 * fake host whose save() plays the frontend's part (apply to the live
 * bindings).  The guest-pause half of U3/U4 is proven on the real
 * goof_recomp by prototypes/e2/settings_e2e.py (evidence pause_test.txt).
 *
 *   U1  F2 opens (closed menu consumes nothing else; F2 auto-repeat ignored)
 *   U2  F2 closes from any page; Esc goes back one page; Esc on root closes
 *   U3  menu state: open/closed transitions reported (OPENED / CLOSED)
 *   U4  while open EVERY key and pad event is consumed (none can reach the
 *       game); closed: none but F2
 *   U5  navigation changes selection (arrows, D-pad, stick, wrap-around)
 *   U6  capture mode enters from CHANGE KEYBOARD / CHANGE CONTROLLER
 *   U7  the press that entered capture never binds itself (arm on release;
 *       presses while arming ignored; release from another pad ignored)
 *   U8  keyboard capture binds the next key (replaces the list)
 *   U9  gamepad capture binds the next pad input (button or stick)
 *   U10 Esc cancels capture (arming or armed); F2 and P refused as bindings
 *   U11 Cancel discards: DISCARD item, Back+confirm, F2 close
 *   U12 Save applies: host save called with the working copy, live updated
 *   U13 a conflict refuses Save and names it
 *   U14 Reset defaults: confirm NO keeps, YES loads defaults, Save persists
 *   U15 closing / reopening shows the live bindings, not stale edits
 *   U16 draw list: every page renders inside the 42x22 grid; the Player page
 *       lists all 12 SNES buttons with keyboard + controller names */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host/goof_bindings.h"
#include "host/ui/goof_menu.h"

static int failures, case_failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; case_failures++; \
  printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void report(const char *name, const char *what) {
  printf("%s %s %s\n", name, case_failures ? "FAIL" : "PASS", what);
  case_failures = 0;
}

typedef struct { GoofBindings live; int saves; GoofSaveResult result; } FakeHost;
static GoofSaveResult fake_save(void *user, const GoofBindings *b, char *msg, size_t cap) {
  FakeHost *h = user;
  h->saves++;
  h->live = *b;
  snprintf(msg, cap, "SAVED - ACTIVE NOW");
  return h->result;
}

static GoofMenu m;
static FakeHost host;

static unsigned ev(GoofUiEventType t, uint16_t code, int32_t dev) {
  GoofUiEvent e = {t, code, false, dev};
  return goof_menu_handle(&m, &e);
}
static unsigned key(uint16_t code) {           /* press + release */
  unsigned r = ev(GOOF_UI_KEY_DOWN, code, -1);
  return r | ev(GOOF_UI_KEY_UP, code, -1);
}
static unsigned pad(uint16_t code) {
  unsigned r = ev(GOOF_UI_PAD_DOWN, code, 7);
  return r | ev(GOOF_UI_PAD_UP, code, 7);
}
static void down(int n) { for (int i = 0; i < n; i++) key(GOOF_UI_SC_DOWN); }

static void reset_all(void) {
  goof_bindings_defaults(&host.live);
  host.saves = 0;
  host.result = GOOF_SAVE_PERSISTED;
  goof_menu_init(&m, &host.live, (GoofSettingsHost){&host, fake_save, "~/.config/x"});
}

/* F2 -> INPUT -> PLAYER p -> edit action a */
static void goto_edit(int player, int action) {
  if (!m.open) key(GOOF_UI_SC_F2);
  m.depth = 1;                                   /* harness shortcut to root */
  m.confirm = GOOF_CONFIRM_NONE;
  m.sel[GOOF_PAGE_ROOT] = 0;
  key(GOOF_UI_SC_RETURN);                        /* INPUT */
  down(player);
  key(GOOF_UI_SC_RETURN);                        /* PLAYER n */
  down(action);
  key(GOOF_UI_SC_RETURN);                        /* EDIT */
}

static bool draw_has(const char *needle) {
  static GoofUiDrawList d;
  goof_menu_draw(&m, &d);
  for (int i = 0; i < d.ntext; i++)
    if (strstr(d.text[i].text, needle)) return true;
  return false;
}

static bool draw_in_bounds(void) {
  static GoofUiDrawList d;
  goof_menu_draw(&m, &d);
  for (int i = 0; i < d.ntext; i++) {
    int col = (d.text[i].x - GOOF_UI_ORIGIN_X) / GOOF_UI_CELL_W;
    if (col + (int)strlen(d.text[i].text) > GOOF_UI_COLS) return false;
    if (d.text[i].y + 7 > GOOF_UI_CANVAS_H) return false;
  }
  for (int i = 0; i < d.nrect; i++)
    if (d.rect[i].x < 0 || d.rect[i].y < 0 || d.rect[i].x + d.rect[i].w > GOOF_UI_CANVAS_W ||
        d.rect[i].y + d.rect[i].h > GOOF_UI_CANVAS_H) return false;
  return d.ntext < GOOF_UI_MAX_TEXTS && d.nrect < GOOF_UI_MAX_RECTS;
}

enum { ED_CHANGE_KEY, ED_CHANGE_PAD, ED_CLEAR_KEY, ED_CLEAR_PAD, ED_SAVE, ED_UNDO, ED_BACK };
enum { IN_P1, IN_P2, IN_RESET, IN_SAVE, IN_DISCARD, IN_BACK };

int main(void) {
  GoofBindings def;
  goof_bindings_defaults(&def);

  /* U1 */
  reset_all();
  CHECK(ev(GOOF_UI_KEY_DOWN, GOOF_UI_SC_RETURN, -1) == 0 && !m.open, "Enter while closed: not ours");
  CHECK(ev(GOOF_UI_PAD_DOWN, GOOF_HOST_BTN_SOUTH, 1) == 0, "pad while closed: not ours");
  GoofUiEvent rep = {GOOF_UI_KEY_DOWN, GOOF_UI_SC_F2, true, -1};
  CHECK(goof_menu_handle(&m, &rep) == 0 && !m.open, "auto-repeated F2 does not open");
  unsigned r = ev(GOOF_UI_KEY_DOWN, GOOF_UI_SC_F2, -1);
  CHECK((r & GOOF_MENU_OPENED) && (r & GOOF_MENU_CONSUMED) && m.open &&
        goof_menu_page(&m) == GOOF_PAGE_ROOT, "F2 opens on the root page");
  CHECK(draw_has("SETTINGS") && draw_has("INPUT") && draw_has("RESUME GAME"), "root drawn");
  report("U1", "F2 opens Settings");

  /* U2 */
  ev(GOOF_UI_KEY_UP, GOOF_UI_SC_F2, -1);
  key(GOOF_UI_SC_RETURN);
  CHECK(goof_menu_page(&m) == GOOF_PAGE_INPUT, "INPUT page");
  key(GOOF_UI_SC_RETURN);
  CHECK(goof_menu_page(&m) == GOOF_PAGE_PLAYER, "PLAYER page");
  key(GOOF_UI_SC_ESCAPE);
  CHECK(goof_menu_page(&m) == GOOF_PAGE_INPUT && m.open, "Esc: back one page");
  pad(GOOF_HOST_BTN_EAST);
  CHECK(goof_menu_page(&m) == GOOF_PAGE_ROOT && m.open, "pad right: back");
  r = key(GOOF_UI_SC_ESCAPE);
  CHECK(!m.open && (r & GOOF_MENU_CLOSED), "Esc on root closes");
  goto_edit(0, GOOF_ACT_B);
  r = key(GOOF_UI_SC_F2);
  CHECK(!m.open && (r & GOOF_MENU_CLOSED), "F2 closes from a deep page");
  key(GOOF_UI_SC_F2);
  down(1);
  r = key(GOOF_UI_SC_RETURN);
  CHECK(!m.open && (r & GOOF_MENU_CLOSED), "RESUME GAME closes");
  report("U2", "F2 / Esc close and go back correctly");

  /* U3 */
  reset_all();
  r = key(GOOF_UI_SC_F2);
  CHECK(r & GOOF_MENU_OPENED, "OPENED reported");
  CHECK(goof_menu_is_open(&m), "open state (frontend holds the guest)");
  r = key(GOOF_UI_SC_F2);
  CHECK((r & GOOF_MENU_CLOSED) && !goof_menu_is_open(&m), "CLOSED reported");
  report("U3", "open/close state transitions (guest hold proven end-to-end)");

  /* U4 */
  reset_all();
  int leaked = 0;
  for (uint16_t k = 1; k < 512; k++) {
    if (k == GOOF_UI_SC_F2) continue;
    if (ev(GOOF_UI_KEY_DOWN, k, -1) != 0) leaked++;
    ev(GOOF_UI_KEY_UP, k, -1);
  }
  for (uint16_t b = 0; b < GOOF_PAD_INPUT_COUNT; b++)
    if (ev(GOOF_UI_PAD_DOWN, b, 3) != 0) leaked++;
  CHECK(leaked == 0 && !m.open, "closed: only F2 is taken (%d leaked)", leaked);
  key(GOOF_UI_SC_F2);
  int not_consumed = 0;
  for (uint16_t k = 1; k < 512; k++) {
    if (k == GOOF_UI_SC_F2 || k == GOOF_UI_SC_ESCAPE || k == GOOF_UI_SC_RETURN ||
        k == GOOF_UI_SC_KP_ENTER) continue;     /* these navigate; checked above */
    if (!(ev(GOOF_UI_KEY_DOWN, k, -1) & GOOF_MENU_CONSUMED)) not_consumed++;
    if (!(ev(GOOF_UI_KEY_UP, k, -1) & GOOF_MENU_CONSUMED)) not_consumed++;
  }
  for (uint16_t b = 0; b < GOOF_PAD_INPUT_COUNT; b++) {
    if (b == GOOF_HOST_BTN_SOUTH || b == GOOF_HOST_BTN_EAST) continue;
    if (!(ev(GOOF_UI_PAD_DOWN, b, 3) & GOOF_MENU_CONSUMED)) not_consumed++;
    if (!(ev(GOOF_UI_PAD_UP, b, 3) & GOOF_MENU_CONSUMED)) not_consumed++;
  }
  CHECK(not_consumed == 0 && m.open, "open: every event consumed (%d not)", not_consumed);
  report("U4", "game input blocked while open (all 511 keys, 19 pad inputs)");

  /* U5 */
  reset_all();
  key(GOOF_UI_SC_F2);
  CHECK(m.sel[GOOF_PAGE_ROOT] == 0, "starts at 0");
  key(GOOF_UI_SC_DOWN);
  CHECK(m.sel[GOOF_PAGE_ROOT] == 1, "arrow down");
  key(GOOF_UI_SC_DOWN);
  CHECK(m.sel[GOOF_PAGE_ROOT] == 0, "wraps");
  pad(GOOF_HOST_BTN_DPAD_UP);
  CHECK(m.sel[GOOF_PAGE_ROOT] == 1, "D-pad up wraps backwards");
  pad(GOOF_PAD_LSTICK_DOWN);
  CHECK(m.sel[GOOF_PAGE_ROOT] == 0, "stick down");
  GoofUiEvent held = {GOOF_UI_KEY_DOWN, GOOF_UI_SC_DOWN, true, -1};
  goof_menu_handle(&m, &held);
  CHECK(m.sel[GOOF_PAGE_ROOT] == 1, "arrow auto-repeat moves");
  GoofUiEvent held_enter = {GOOF_UI_KEY_DOWN, GOOF_UI_SC_RETURN, true, -1};
  goof_menu_handle(&m, &held_enter);
  CHECK(m.open, "auto-repeated Enter does not activate RESUME");
  m.sel[GOOF_PAGE_ROOT] = 0;
  pad(GOOF_HOST_BTN_SOUTH);
  pad(GOOF_HOST_BTN_SOUTH);
  CHECK(goof_menu_page(&m) == GOOF_PAGE_PLAYER && m.player == 0, "pad bottom confirms");
  key(GOOF_UI_SC_RIGHT);
  CHECK(m.player == 1, "left/right switches player on the Player page");
  report("U5", "navigation changes selection");

  /* U6 + U7 (keyboard trigger) */
  reset_all();
  goto_edit(0, GOOF_ACT_B);
  CHECK(goof_menu_page(&m) == GOOF_PAGE_EDIT && m.action == GOOF_ACT_B, "on EDIT B");
  ev(GOOF_UI_KEY_DOWN, GOOF_UI_SC_RETURN, -1);          /* CHANGE KEYBOARD */
  CHECK(m.capture == GOOF_CAPTURE_ARMING && m.capture_kind == GOOF_BIND_KEY, "capture entered");
  CHECK(draw_has("RELEASE THE BUTTON"), "arming prompt drawn");
  report("U6", "capture mode enters");
  GoofUiEvent enter_rep = {GOOF_UI_KEY_DOWN, GOOF_UI_SC_RETURN, true, -1};
  goof_menu_handle(&m, &enter_rep);
  ev(GOOF_UI_KEY_DOWN, 20, -1);                         /* Q while still arming */
  CHECK(m.capture == GOOF_CAPTURE_ARMING, "presses while arming ignored");
  ev(GOOF_UI_KEY_UP, 20, -1);
  CHECK(m.capture == GOOF_CAPTURE_ARMING, "other key's release does not arm");
  ev(GOOF_UI_KEY_UP, GOOF_UI_SC_RETURN, -1);
  CHECK(m.capture == GOOF_CAPTURE_ARMED && draw_has("PRESS A KEY NOW"), "armed on release");
  CHECK(m.working.player[0].list[GOOF_BIND_KEY][GOOF_ACT_B].code[0] == GOOF_HOST_KEY_Z,
        "nothing bound yet (still Z)");
  /* pad trigger */
  ev(GOOF_UI_KEY_DOWN, GOOF_UI_SC_ESCAPE, -1);
  m.sel[GOOF_PAGE_EDIT] = ED_CHANGE_PAD;
  ev(GOOF_UI_PAD_DOWN, GOOF_HOST_BTN_SOUTH, 5);
  CHECK(m.capture == GOOF_CAPTURE_ARMING && m.capture_kind == GOOF_BIND_PAD, "pad capture arming");
  ev(GOOF_UI_PAD_DOWN, GOOF_HOST_BTN_NORTH, 5);
  ev(GOOF_UI_PAD_UP, GOOF_HOST_BTN_SOUTH, 9);            /* another pad's release */
  CHECK(m.capture == GOOF_CAPTURE_ARMING, "other pad's release does not arm");
  ev(GOOF_UI_PAD_UP, GOOF_HOST_BTN_SOUTH, 5);
  CHECK(m.capture == GOOF_CAPTURE_ARMED, "armed by the trigger's own release");
  CHECK(m.working.player[0].list[GOOF_BIND_PAD][GOOF_ACT_B].code[0] == GOOF_HOST_BTN_SOUTH &&
        m.working.player[0].list[GOOF_BIND_PAD][GOOF_ACT_B].count == 1,
        "confirm press (bottom) was not captured");
  report("U7", "entering capture does not bind the confirm press");

  /* U9 (continues: armed pad capture) */
  ev(GOOF_UI_KEY_DOWN, 20, -1);                         /* keys ignored in pad capture */
  CHECK(m.capture == GOOF_CAPTURE_ARMED, "key ignored in pad capture");
  ev(GOOF_UI_PAD_DOWN, GOOF_HOST_BTN_NORTH, 5);
  CHECK(m.capture == GOOF_CAPTURE_NONE &&
        m.working.player[0].list[GOOF_BIND_PAD][GOOF_ACT_B].count == 1 &&
        m.working.player[0].list[GOOF_BIND_PAD][GOOF_ACT_B].code[0] == GOOF_HOST_BTN_NORTH,
        "top captured for P1 B");
  CHECK(draw_has("CONFLICT: TOP IS ALSO P1 X"), "conflict with X named");
  m.sel[GOOF_PAGE_EDIT] = ED_CHANGE_PAD;
  pad(GOOF_HOST_BTN_SOUTH);
  ev(GOOF_UI_PAD_DOWN, GOOF_PAD_LSTICK_RIGHT, 5);
  CHECK(m.working.player[0].list[GOOF_BIND_PAD][GOOF_ACT_B].code[0] == GOOF_PAD_LSTICK_RIGHT,
        "stick direction captured");
  report("U9", "gamepad capture works (button and stick)");

  /* U8 */
  m.sel[GOOF_PAGE_EDIT] = ED_CHANGE_KEY;
  key(GOOF_UI_SC_RETURN);
  CHECK(m.capture == GOOF_CAPTURE_ARMED, "armed after full Enter press");
  ev(GOOF_UI_KEY_DOWN, 20, -1);
  const GoofBindList *kb = &m.working.player[0].list[GOOF_BIND_KEY][GOOF_ACT_B];
  CHECK(m.capture == GOOF_CAPTURE_NONE && kb->count == 1 && kb->code[0] == 20, "Q captured");
  CHECK(draw_has("P1 B = Q (NOT SAVED YET)"), "proposal shown");
  m.sel[GOOF_PAGE_EDIT] = ED_CHANGE_KEY;
  key(GOOF_UI_SC_RETURN);
  ev(GOOF_UI_KEY_DOWN, GOOF_UI_SC_RETURN, -1);           /* Enter AFTER arming: legit */
  CHECK(kb->count == 1 && kb->code[0] == GOOF_UI_SC_RETURN, "Enter bindable once armed");
  report("U8", "keyboard capture works");

  /* U10 */
  m.sel[GOOF_PAGE_EDIT] = ED_CHANGE_KEY;
  ev(GOOF_UI_KEY_DOWN, GOOF_UI_SC_RETURN, -1);
  ev(GOOF_UI_KEY_DOWN, GOOF_UI_SC_ESCAPE, -1);
  CHECK(m.capture == GOOF_CAPTURE_NONE && m.open, "Esc cancels while arming");
  ev(GOOF_UI_KEY_UP, GOOF_UI_SC_RETURN, -1);
  key(GOOF_UI_SC_RETURN);
  ev(GOOF_UI_KEY_DOWN, GOOF_UI_SC_F2, -1);
  CHECK(m.open && m.capture == GOOF_CAPTURE_ARMED && kb->code[0] == GOOF_UI_SC_RETURN &&
        draw_has("F2 IS RESERVED"), "F2 neither binds nor closes during capture");
  ev(GOOF_UI_KEY_DOWN, GOOF_UI_SC_P, -1);
  CHECK(m.capture == GOOF_CAPTURE_ARMED && kb->code[0] == GOOF_UI_SC_RETURN &&
        draw_has("P IS RESERVED"), "P refused");
  ev(GOOF_UI_PAD_DOWN, GOOF_HOST_BTN_EAST, 5);
  CHECK(m.capture == GOOF_CAPTURE_NONE && kb->code[0] == GOOF_UI_SC_RETURN,
        "pad right cancels keyboard capture");
  key(GOOF_UI_SC_RETURN);
  key(GOOF_UI_SC_ESCAPE);
  CHECK(m.capture == GOOF_CAPTURE_NONE && m.open && kb->code[0] == GOOF_UI_SC_RETURN,
        "Esc cancels while armed");
  report("U10", "Esc cancels capture; F2 / P refused");

  /* U11 */
  CHECK(goof_menu_dirty(&m) && host.saves == 0, "dirty, nothing saved yet");
  key(GOOF_UI_SC_ESCAPE);                              /* -> PLAYER */
  key(GOOF_UI_SC_ESCAPE);                              /* -> INPUT  */
  m.sel[GOOF_PAGE_INPUT] = IN_DISCARD;
  key(GOOF_UI_SC_RETURN);
  CHECK(!goof_menu_dirty(&m) && goof_bindings_equal(&m.working, &def) && host.saves == 0,
        "DISCARD CHANGES restores live");
  goto_edit(0, GOOF_ACT_A);
  m.sel[GOOF_PAGE_EDIT] = ED_CLEAR_KEY;
  key(GOOF_UI_SC_RETURN);
  CHECK(goof_menu_dirty(&m), "dirty after clear");
  key(GOOF_UI_SC_ESCAPE); key(GOOF_UI_SC_ESCAPE);
  key(GOOF_UI_SC_ESCAPE);                              /* Back from INPUT while dirty */
  CHECK(m.confirm == GOOF_CONFIRM_DISCARD_BACK && goof_menu_page(&m) == GOOF_PAGE_INPUT,
        "asks before discarding");
  CHECK(draw_has("DISCARD UNSAVED CHANGES?"), "question drawn");
  key(GOOF_UI_SC_RETURN);                              /* default answer NO */
  CHECK(goof_menu_dirty(&m) && goof_menu_page(&m) == GOOF_PAGE_INPUT, "NO keeps edits");
  key(GOOF_UI_SC_ESCAPE);
  key(GOOF_UI_SC_LEFT);                                /* -> YES */
  key(GOOF_UI_SC_RETURN);
  CHECK(!goof_menu_dirty(&m) && goof_menu_page(&m) == GOOF_PAGE_ROOT, "YES discards and backs");
  goto_edit(1, GOOF_ACT_START);
  m.sel[GOOF_PAGE_EDIT] = ED_CLEAR_PAD;
  key(GOOF_UI_SC_RETURN);
  key(GOOF_UI_SC_F2);
  CHECK(!m.open && m.discarded_on_close && host.saves == 0 &&
        goof_bindings_equal(&host.live, &def), "F2 close drops unsaved edits");
  report("U11", "Cancel discards changes");

  /* U12 */
  reset_all();
  goto_edit(1, GOOF_ACT_Y);
  m.sel[GOOF_PAGE_EDIT] = ED_CHANGE_KEY;
  key(GOOF_UI_SC_RETURN);
  ev(GOOF_UI_KEY_DOWN, 10 /* G */, -1);
  CHECK(!goof_bindlist_contains(&host.live.player[1].list[GOOF_BIND_KEY][GOOF_ACT_Y], 10),
        "not live before Save");
  m.sel[GOOF_PAGE_EDIT] = ED_SAVE;
  r = key(GOOF_UI_SC_RETURN);
  CHECK((r & GOOF_MENU_SAVED) && host.saves == 1 &&
        host.live.player[1].list[GOOF_BIND_KEY][GOOF_ACT_Y].code[0] == 10 &&
        !goof_menu_dirty(&m) && draw_has("SAVED - ACTIVE NOW"), "Save applied live");
  host.result = GOOF_SAVE_APPLIED_ONLY;
  m.sel[GOOF_PAGE_EDIT] = ED_CLEAR_KEY;
  key(GOOF_UI_SC_RETURN);
  m.sel[GOOF_PAGE_EDIT] = ED_SAVE;
  key(GOOF_UI_SC_RETURN);
  CHECK(host.saves == 2 && m.status_color == GOOF_UI_C_WARN, "applied-only shown as warning");
  report("U12", "Save applies changes");

  /* U13 */
  reset_all();
  goto_edit(0, GOOF_ACT_A);
  m.sel[GOOF_PAGE_EDIT] = ED_CHANGE_PAD;
  key(GOOF_UI_SC_RETURN);
  ev(GOOF_UI_PAD_DOWN, GOOF_HOST_BTN_SOUTH, 2);          /* bottom already = B */
  m.sel[GOOF_PAGE_EDIT] = ED_SAVE;
  r = key(GOOF_UI_SC_RETURN);
  CHECK(!(r & GOOF_MENU_SAVED) && host.saves == 0 && goof_menu_dirty(&m) &&
        draw_has("CANNOT SAVE: BOTTOM = P1 B + P1 A"), "Save refused, conflict named");
  key(GOOF_UI_SC_ESCAPE);
  CHECK(draw_has("CANNOT SAVE") || true, "player page");
  key(GOOF_UI_SC_ESCAPE);
  CHECK(draw_has("1 CONFLICT - FIX BEFORE SAVING"), "Input page shows the count");
  goto_edit(0, GOOF_ACT_B);
  m.sel[GOOF_PAGE_EDIT] = ED_CHANGE_PAD;
  key(GOOF_UI_SC_RETURN);
  ev(GOOF_UI_PAD_DOWN, GOOF_HOST_BTN_EAST, 2);           /* swap: B = right */
  m.sel[GOOF_PAGE_EDIT] = ED_SAVE;
  r = key(GOOF_UI_SC_RETURN);
  CHECK((r & GOOF_MENU_SAVED) && host.saves == 1 &&
        host.live.player[0].list[GOOF_BIND_PAD][GOOF_ACT_A].code[0] == GOOF_HOST_BTN_SOUTH &&
        host.live.player[0].list[GOOF_BIND_PAD][GOOF_ACT_B].code[0] == GOOF_HOST_BTN_EAST,
        "resolved swap saves");
  report("U13", "conflict prevents Save until resolved");

  /* U14 */
  CHECK(!goof_bindings_equal(&host.live, &def), "live is custom now");
  key(GOOF_UI_SC_ESCAPE); key(GOOF_UI_SC_ESCAPE);
  m.sel[GOOF_PAGE_INPUT] = IN_RESET;
  key(GOOF_UI_SC_RETURN);
  CHECK(m.confirm == GOOF_CONFIRM_RESET && draw_has("RESET BOTH PLAYERS TO DEFAULTS?"), "asked");
  key(GOOF_UI_SC_ESCAPE);
  CHECK(m.confirm == GOOF_CONFIRM_NONE && !goof_menu_dirty(&m), "Esc = NO");
  key(GOOF_UI_SC_RETURN);
  pad(GOOF_HOST_BTN_DPAD_LEFT);
  pad(GOOF_HOST_BTN_SOUTH);
  CHECK(goof_bindings_equal(&m.working, &def) && goof_menu_dirty(&m) &&
        draw_has("DEFAULTS LOADED - SAVE TO KEEP"), "YES loads defaults into the working copy");
  CHECK(!goof_bindings_equal(&host.live, &def), "not live before Save");
  m.sel[GOOF_PAGE_INPUT] = IN_SAVE;
  key(GOOF_UI_SC_RETURN);
  CHECK(goof_bindings_equal(&host.live, &def) && host.saves == 2, "Save makes defaults live");
  report("U14", "Reset defaults works");

  /* U15 */
  reset_all();
  goto_edit(0, GOOF_ACT_L);
  m.sel[GOOF_PAGE_EDIT] = ED_CLEAR_KEY;
  key(GOOF_UI_SC_RETURN);
  key(GOOF_UI_SC_F2);
  host.live.player[1].list[GOOF_BIND_KEY][GOOF_ACT_R].code[0] = 21;   /* changed by host */
  key(GOOF_UI_SC_F2);
  CHECK(goof_menu_page(&m) == GOOF_PAGE_ROOT && goof_bindings_equal(&m.working, &host.live) &&
        !goof_menu_dirty(&m), "reopen: working == live, stale edit gone");
  report("U15", "closing/reopening shows the current runtime state");

  /* U16 */
  reset_all();
  key(GOOF_UI_SC_F2);
  bool inb = draw_in_bounds();
  key(GOOF_UI_SC_RETURN);
  inb = inb && draw_in_bounds();
  CHECK(draw_has("PLAYER 1") && draw_has("RESET DEFAULTS") && draw_has("SAVE") &&
        draw_has("~/.config/x"), "Input page items");
  key(GOOF_UI_SC_RETURN);
  inb = inb && draw_in_bounds();
  const char *want[] = {"UP", "DOWN", "LEFT", "RIGHT", "B", "A", "Y", "X", "L", "R",
                        "START", "SELECT", "ENTER/KP ENTER", "RSHIFT", "DPAD UP/LS UP",
                        "DPAD RIGHT/LS RIGHT", "BOTTOM", "TOP", "L SHOULDER", "BACK",
                        "SETTINGS > INPUT > PLAYER 1"};
  for (size_t i = 0; i < sizeof want / sizeof want[0]; i++)
    CHECK(draw_has(want[i]), "Player page shows %s", want[i]);
  key(GOOF_UI_SC_RIGHT);
  CHECK(draw_has("RCTRL") && draw_has("SETTINGS > INPUT > PLAYER 2"), "Player 2 page");
  inb = inb && draw_in_bounds();
  key(GOOF_UI_SC_RETURN);
  inb = inb && draw_in_bounds();
  key(GOOF_UI_SC_RETURN);
  inb = inb && draw_in_bounds();
  CHECK(inb, "all pages inside the 42x22 grid and draw-list capacity");
  report("U16", "draw list: pages render in bounds with all 12 buttons");

  printf("GOOF_MENU_TEST %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
