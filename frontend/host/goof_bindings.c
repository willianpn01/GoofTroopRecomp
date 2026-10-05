/* Goof Troop Recomp -- configurable per-player bindings.  SDL-FREE.
 * See goof_bindings.h.  GOOF_ENHANCEMENTS_E2_BUTTON_REMAPPING_CONFIG. */
#include "host/goof_bindings.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

/* ---- actions ---------------------------------------------------------- */

static const struct {
  const char *token, *label;
  uint16_t bit;
} kActions[GOOF_ACT_COUNT] = {
  {"up", "UP", GOOF_BTN_UP},         {"down", "DOWN", GOOF_BTN_DOWN},
  {"left", "LEFT", GOOF_BTN_LEFT},   {"right", "RIGHT", GOOF_BTN_RIGHT},
  {"b", "B", GOOF_BTN_B},            {"a", "A", GOOF_BTN_A},
  {"y", "Y", GOOF_BTN_Y},            {"x", "X", GOOF_BTN_X},
  {"l", "L", GOOF_BTN_L},            {"r", "R", GOOF_BTN_R},
  {"start", "START", GOOF_BTN_START}, {"select", "SELECT", GOOF_BTN_SELECT},
};

uint16_t goof_action_bit(GoofSnesAction a) {
  return (unsigned)a < GOOF_ACT_COUNT ? kActions[a].bit : 0;
}
const char *goof_action_token(GoofSnesAction a) {
  return (unsigned)a < GOOF_ACT_COUNT ? kActions[a].token : "?";
}
const char *goof_action_label(GoofSnesAction a) {
  return (unsigned)a < GOOF_ACT_COUNT ? kActions[a].label : "?";
}
bool goof_action_from_token(const char *s, GoofSnesAction *out) {
  for (int i = 0; i < GOOF_ACT_COUNT; i++)
    if (strcmp(s, kActions[i].token) == 0) { *out = (GoofSnesAction)i; return true; }
  return false;
}

static bool eq_nocase(const char *a, const char *b) {
  for (; *a && *b; a++, b++)
    if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
  return *a == *b;
}

/* ---- keyboard names --------------------------------------------------- */
/* USB HID keyboard page usages (== SDL_Scancode).  Token = config spelling,
 * label = what the Settings overlay shows.  Both name the key POSITION on a
 * US layout, because the binding is the position (scancode), not the
 * character a localised layout prints on it. */
typedef struct { uint16_t code; const char *token, *label; } KeyName;

static const KeyName kKeyNames[] = {
  {4, "A", "A"}, {5, "B", "B"}, {6, "C", "C"}, {7, "D", "D"}, {8, "E", "E"},
  {9, "F", "F"}, {10, "G", "G"}, {11, "H", "H"}, {12, "I", "I"},
  {13, "J", "J"}, {14, "K", "K"}, {15, "L", "L"}, {16, "M", "M"},
  {17, "N", "N"}, {18, "O", "O"}, {19, "P", "P"}, {20, "Q", "Q"},
  {21, "R", "R"}, {22, "S", "S"}, {23, "T", "T"}, {24, "U", "U"},
  {25, "V", "V"}, {26, "W", "W"}, {27, "X", "X"}, {28, "Y", "Y"},
  {29, "Z", "Z"},
  {30, "1", "1"}, {31, "2", "2"}, {32, "3", "3"}, {33, "4", "4"},
  {34, "5", "5"}, {35, "6", "6"}, {36, "7", "7"}, {37, "8", "8"},
  {38, "9", "9"}, {39, "0", "0"},
  {40, "RETURN", "ENTER"}, {41, "ESCAPE", "ESC"}, {42, "BACKSPACE", "BKSP"},
  {43, "TAB", "TAB"}, {44, "SPACE", "SPACE"}, {45, "MINUS", "-"},
  {46, "EQUALS", "="}, {47, "LEFTBRACKET", "["}, {48, "RIGHTBRACKET", "]"},
  {49, "BACKSLASH", "\\"}, {50, "NONUSHASH", "#"}, {51, "SEMICOLON", ";"},
  {52, "APOSTROPHE", "'"}, {53, "GRAVE", "`"}, {54, "COMMA", ","},
  {55, "PERIOD", "."}, {56, "SLASH", "/"}, {57, "CAPSLOCK", "CAPS"},
  {58, "F1", "F1"}, {59, "F2", "F2"}, {60, "F3", "F3"}, {61, "F4", "F4"},
  {62, "F5", "F5"}, {63, "F6", "F6"}, {64, "F7", "F7"}, {65, "F8", "F8"},
  {66, "F9", "F9"}, {67, "F10", "F10"}, {68, "F11", "F11"},
  {69, "F12", "F12"},
  {70, "PRINTSCREEN", "PRTSC"}, {71, "SCROLLLOCK", "SCRLK"},
  {72, "PAUSE", "PAUSE"}, {73, "INSERT", "INS"}, {74, "HOME", "HOME"},
  {75, "PAGEUP", "PGUP"}, {76, "DELETE", "DEL"}, {77, "END", "END"},
  {78, "PAGEDOWN", "PGDN"}, {79, "RIGHT", "RIGHT"}, {80, "LEFT", "LEFT"},
  {81, "DOWN", "DOWN"}, {82, "UP", "UP"}, {83, "NUMLOCK", "NUMLK"},
  {84, "KP_DIVIDE", "KP /"}, {85, "KP_MULTIPLY", "KP *"},
  {86, "KP_MINUS", "KP -"}, {87, "KP_PLUS", "KP +"},
  {88, "KP_ENTER", "KP ENTER"}, {89, "KP_1", "KP 1"}, {90, "KP_2", "KP 2"},
  {91, "KP_3", "KP 3"}, {92, "KP_4", "KP 4"}, {93, "KP_5", "KP 5"},
  {94, "KP_6", "KP 6"}, {95, "KP_7", "KP 7"}, {96, "KP_8", "KP 8"},
  {97, "KP_9", "KP 9"}, {98, "KP_0", "KP 0"}, {99, "KP_PERIOD", "KP ."},
  {100, "NONUSBACKSLASH", "<>"}, {101, "APPLICATION", "MENU"},
  {103, "KP_EQUALS", "KP ="},
  {224, "LCTRL", "LCTRL"}, {225, "LSHIFT", "LSHIFT"}, {226, "LALT", "LALT"},
  {227, "LGUI", "LGUI"}, {228, "RCTRL", "RCTRL"}, {229, "RSHIFT", "RSHIFT"},
  {230, "RALT", "RALT"}, {231, "RGUI", "RGUI"},
};

static const KeyName *key_name(uint16_t code) {
  for (size_t i = 0; i < COUNT_OF(kKeyNames); i++)
    if (kKeyNames[i].code == code) return &kKeyNames[i];
  return NULL;
}

bool goof_key_token(uint16_t code, char *out, size_t cap) {
  if (code == 0 || code >= GOOF_KEY_CODE_LIMIT || !cap) return false;
  const KeyName *k = key_name(code);
  int n = k ? snprintf(out, cap, "%s", k->token)
            : snprintf(out, cap, "HID_%u", (unsigned)code);
  return n > 0 && (size_t)n < cap;
}

bool goof_key_label(uint16_t code, char *out, size_t cap) {
  if (code == 0 || code >= GOOF_KEY_CODE_LIMIT || !cap) return false;
  const KeyName *k = key_name(code);
  int n = k ? snprintf(out, cap, "%s", k->label)
            : snprintf(out, cap, "KEY %u", (unsigned)code);
  return n > 0 && (size_t)n < cap;
}

bool goof_key_from_token(const char *s, uint16_t *out) {
  for (size_t i = 0; i < COUNT_OF(kKeyNames); i++)
    if (eq_nocase(s, kKeyNames[i].token)) { *out = kKeyNames[i].code; return true; }
  /* HID_<decimal>: only for usages that have no name, so every key has
   * exactly one canonical spelling. */
  if ((s[0] == 'H' || s[0] == 'h') && (s[1] == 'I' || s[1] == 'i') &&
      (s[2] == 'D' || s[2] == 'd') && s[3] == '_' && s[4]) {
    unsigned v = 0;
    for (const char *p = s + 4; *p; p++) {
      if (!isdigit((unsigned char)*p) || v > GOOF_KEY_CODE_LIMIT) return false;
      v = v * 10 + (unsigned)(*p - '0');
    }
    if (s[4] == '0' || v == 0 || v >= GOOF_KEY_CODE_LIMIT || key_name((uint16_t)v))
      return false;
    *out = (uint16_t)v;
    return true;
  }
  return false;
}

enum { KEY_F2 = 59, KEY_ESCAPE = 41, KEY_P = 19 };

bool goof_key_is_reserved(uint16_t code) {
  return code == KEY_F2 || code == KEY_ESCAPE || code == KEY_P;
}
bool goof_key_is_valid(uint16_t code) {
  return code > 0 && code < GOOF_KEY_CODE_LIMIT && !goof_key_is_reserved(code);
}

/* ---- pad names -------------------------------------------------------- */

static const struct { const char *token, *label; } kPadNames[GOOF_PAD_INPUT_COUNT] = {
  [GOOF_HOST_BTN_SOUTH] = {"bottom", "BOTTOM"},
  [GOOF_HOST_BTN_EAST] = {"right", "RIGHT"},
  [GOOF_HOST_BTN_WEST] = {"left", "LEFT"},
  [GOOF_HOST_BTN_NORTH] = {"top", "TOP"},
  [GOOF_HOST_BTN_BACK] = {"back", "BACK"},
  [GOOF_HOST_BTN_GUIDE] = {"guide", "GUIDE"},
  [GOOF_HOST_BTN_START] = {"start", "START"},
  [GOOF_HOST_BTN_LEFT_STICK] = {"left_stick_button", "LS CLICK"},
  [GOOF_HOST_BTN_RIGHT_STICK] = {"right_stick_button", "RS CLICK"},
  [GOOF_HOST_BTN_LEFT_SHOULDER] = {"left_shoulder", "L SHOULDER"},
  [GOOF_HOST_BTN_RIGHT_SHOULDER] = {"right_shoulder", "R SHOULDER"},
  [GOOF_HOST_BTN_DPAD_UP] = {"dpad_up", "DPAD UP"},
  [GOOF_HOST_BTN_DPAD_DOWN] = {"dpad_down", "DPAD DOWN"},
  [GOOF_HOST_BTN_DPAD_LEFT] = {"dpad_left", "DPAD LEFT"},
  [GOOF_HOST_BTN_DPAD_RIGHT] = {"dpad_right", "DPAD RIGHT"},
  [GOOF_PAD_LSTICK_UP] = {"left_stick_up", "LS UP"},
  [GOOF_PAD_LSTICK_DOWN] = {"left_stick_down", "LS DOWN"},
  [GOOF_PAD_LSTICK_LEFT] = {"left_stick_left", "LS LEFT"},
  [GOOF_PAD_LSTICK_RIGHT] = {"left_stick_right", "LS RIGHT"},
};

bool goof_pad_is_valid(uint16_t code) { return code < GOOF_PAD_INPUT_COUNT; }
const char *goof_pad_token(uint16_t code) {
  return goof_pad_is_valid(code) ? kPadNames[code].token : NULL;
}
const char *goof_pad_label(uint16_t code) {
  return goof_pad_is_valid(code) ? kPadNames[code].label : "?";
}
bool goof_pad_from_token(const char *s, uint16_t *out) {
  for (uint16_t i = 0; i < GOOF_PAD_INPUT_COUNT; i++)
    if (eq_nocase(s, kPadNames[i].token)) { *out = i; return true; }
  return false;
}

/* ---- defaults --------------------------------------------------------- */
/* The E1 tables (host/goof_host_input.c), unchanged, one list per action.
 * P1 keyboard: arrows, Enter + KP-Enter Start, RShift Select, Z B, X A, A Y,
 * S X, C L, V R.  P2: I/K/J/L, RCtrl Start, RAlt Select, N B, M A, B Y, H X,
 * U L, O R.  Pad: position-true (D4) plus the left stick as a D-pad. */
static const uint16_t kDefaultKeys[GOOF_HOST_PLAYERS][GOOF_ACT_COUNT][2] = {
  { [GOOF_ACT_UP] = {GOOF_HOST_KEY_UP}, [GOOF_ACT_DOWN] = {GOOF_HOST_KEY_DOWN},
    [GOOF_ACT_LEFT] = {GOOF_HOST_KEY_LEFT},
    [GOOF_ACT_RIGHT] = {GOOF_HOST_KEY_RIGHT},
    [GOOF_ACT_B] = {GOOF_HOST_KEY_Z}, [GOOF_ACT_A] = {GOOF_HOST_KEY_X},
    [GOOF_ACT_Y] = {GOOF_HOST_KEY_A}, [GOOF_ACT_X] = {GOOF_HOST_KEY_S},
    [GOOF_ACT_L] = {GOOF_HOST_KEY_C}, [GOOF_ACT_R] = {GOOF_HOST_KEY_V},
    [GOOF_ACT_START] = {GOOF_HOST_KEY_RETURN, GOOF_HOST_KEY_KP_ENTER},
    [GOOF_ACT_SELECT] = {GOOF_HOST_KEY_RSHIFT} },
  { [GOOF_ACT_UP] = {GOOF_HOST_KEY_I}, [GOOF_ACT_DOWN] = {GOOF_HOST_KEY_K},
    [GOOF_ACT_LEFT] = {GOOF_HOST_KEY_J}, [GOOF_ACT_RIGHT] = {GOOF_HOST_KEY_L},
    [GOOF_ACT_B] = {GOOF_HOST_KEY_N}, [GOOF_ACT_A] = {GOOF_HOST_KEY_M},
    [GOOF_ACT_Y] = {GOOF_HOST_KEY_B}, [GOOF_ACT_X] = {GOOF_HOST_KEY_H},
    [GOOF_ACT_L] = {GOOF_HOST_KEY_U}, [GOOF_ACT_R] = {GOOF_HOST_KEY_O},
    [GOOF_ACT_START] = {GOOF_HOST_KEY_RCTRL},
    [GOOF_ACT_SELECT] = {GOOF_HOST_KEY_RALT} },
};

static const uint16_t kDefaultPad[GOOF_ACT_COUNT][2] = {
  [GOOF_ACT_UP] = {GOOF_HOST_BTN_DPAD_UP, GOOF_PAD_LSTICK_UP},
  [GOOF_ACT_DOWN] = {GOOF_HOST_BTN_DPAD_DOWN, GOOF_PAD_LSTICK_DOWN},
  [GOOF_ACT_LEFT] = {GOOF_HOST_BTN_DPAD_LEFT, GOOF_PAD_LSTICK_LEFT},
  [GOOF_ACT_RIGHT] = {GOOF_HOST_BTN_DPAD_RIGHT, GOOF_PAD_LSTICK_RIGHT},
  [GOOF_ACT_B] = {GOOF_HOST_BTN_SOUTH},
  [GOOF_ACT_A] = {GOOF_HOST_BTN_EAST},
  [GOOF_ACT_Y] = {GOOF_HOST_BTN_WEST},
  [GOOF_ACT_X] = {GOOF_HOST_BTN_NORTH},
  [GOOF_ACT_L] = {GOOF_HOST_BTN_LEFT_SHOULDER},
  [GOOF_ACT_R] = {GOOF_HOST_BTN_RIGHT_SHOULDER},
  [GOOF_ACT_START] = {GOOF_HOST_BTN_START},
  [GOOF_ACT_SELECT] = {GOOF_HOST_BTN_BACK},
};

void goof_player_bindings_defaults(GoofPlayerBindings *p, int player) {
  memset(p, 0, sizeof *p);
  if (player != 0 && player != 1) return;
  for (int a = 0; a < GOOF_ACT_COUNT; a++) {
    for (int i = 0; i < 2; i++)
      if (kDefaultKeys[player][a][i])
        goof_bindlist_add(&p->list[GOOF_BIND_KEY][a], kDefaultKeys[player][a][i]);
    goof_bindlist_add(&p->list[GOOF_BIND_PAD][a], kDefaultPad[a][0]);
    if (kDefaultPad[a][1])
      goof_bindlist_add(&p->list[GOOF_BIND_PAD][a], kDefaultPad[a][1]);
  }
}

void goof_bindings_defaults(GoofBindings *b) {
  for (int p = 0; p < GOOF_HOST_PLAYERS; p++)
    goof_player_bindings_defaults(&b->player[p], p);
}

bool goof_bindings_equal(const GoofBindings *a, const GoofBindings *b) {
  for (int p = 0; p < GOOF_HOST_PLAYERS; p++)
    for (int k = 0; k < GOOF_BIND_KINDS; k++)
      for (int act = 0; act < GOOF_ACT_COUNT; act++) {
        const GoofBindList *x = &a->player[p].list[k][act];
        const GoofBindList *y = &b->player[p].list[k][act];
        if (x->count != y->count) return false;
        for (int i = 0; i < x->count; i++)
          if (x->code[i] != y->code[i]) return false;
      }
  return true;
}

bool goof_bindlist_contains(const GoofBindList *l, uint16_t code) {
  for (int i = 0; i < l->count; i++)
    if (l->code[i] == code) return true;
  return false;
}

bool goof_bindlist_add(GoofBindList *l, uint16_t code) {
  if (goof_bindlist_contains(l, code)) return true;
  if (l->count >= GOOF_BIND_MAX_PER_KIND) return false;
  l->code[l->count++] = code;
  return true;
}

void goof_bindlist_set_single(GoofBindList *l, uint16_t code) {
  goof_bindlist_clear(l);
  l->code[0] = code;
  l->count = 1;
}

void goof_bindlist_clear(GoofBindList *l) { memset(l, 0, sizeof *l); }

bool goof_bindlist_remove(GoofBindList *l, uint16_t code) {
  for (int i = 0; i < l->count; i++) {
    if (l->code[i] != code) continue;
    for (int j = i + 1; j < l->count; j++) l->code[j - 1] = l->code[j];
    l->code[--l->count] = 0;
    return true;
  }
  return false;
}

/* ---- evaluation ------------------------------------------------------- */

bool goof_pad_input_held(const GoofHostPadState *pad, uint16_t code) {
  if (!pad) return false;
  if (code < GOOF_HOST_BTN_COUNT) return (pad->buttons >> code) & 1u;
  int x = pad->axes[GOOF_HOST_AXIS_LEFT_X];
  int y = pad->axes[GOOF_HOST_AXIS_LEFT_Y];
  switch (code) {   /* E1 threshold, strict comparison */
    case GOOF_PAD_LSTICK_UP:    return y < -GOOF_HOST_STICK_THRESHOLD;
    case GOOF_PAD_LSTICK_DOWN:  return y >  GOOF_HOST_STICK_THRESHOLD;
    case GOOF_PAD_LSTICK_LEFT:  return x < -GOOF_HOST_STICK_THRESHOLD;
    case GOOF_PAD_LSTICK_RIGHT: return x >  GOOF_HOST_STICK_THRESHOLD;
    default: return false;
  }
}

bool goof_key_held(const GoofHostInputFrame *f, uint16_t code) {
  return f && f->keys && code < f->key_count && f->keys[code];
}

static bool guard_key(const GoofInputGuard *g, uint16_t code) {
  return g && g->active && code < GOOF_KEY_CODE_LIMIT &&
         ((g->key[code >> 3] >> (code & 7)) & 1u);
}
static bool guard_pad(const GoofInputGuard *g, int slot, uint16_t code) {
  return g && g->active && code < 32 && ((g->pad[slot] >> code) & 1u);
}

void goof_input_guard_arm(GoofInputGuard *g, const GoofHostInputFrame *f) {
  memset(g, 0, sizeof *g);
  if (!f) return;
  for (uint16_t c = 1; c < GOOF_KEY_CODE_LIMIT; c++)
    if (goof_key_held(f, c)) { g->key[c >> 3] |= (uint8_t)(1u << (c & 7)); g->active = true; }
  for (int s = 0; s < GOOF_HOST_PLAYERS; s++) {
    if (!f->pad_present[s]) continue;
    for (uint16_t c = 0; c < GOOF_PAD_INPUT_COUNT; c++)
      if (goof_pad_input_held(&f->pad[s], c)) { g->pad[s] |= 1u << c; g->active = true; }
  }
}

void goof_input_guard_update(GoofInputGuard *g, const GoofHostInputFrame *f) {
  if (!g->active) return;
  bool any = false;
  for (uint16_t c = 1; c < GOOF_KEY_CODE_LIMIT; c++) {
    if (!guard_key(g, c)) continue;
    if (goof_key_held(f, c)) any = true;
    else g->key[c >> 3] &= (uint8_t)~(1u << (c & 7));
  }
  for (int s = 0; s < GOOF_HOST_PLAYERS; s++) {
    for (uint16_t c = 0; c < GOOF_PAD_INPUT_COUNT; c++) {
      if (!guard_pad(g, s, c)) continue;
      if (f && f->pad_present[s] && goof_pad_input_held(&f->pad[s], c)) any = true;
      else g->pad[s] &= ~(1u << c);
    }
  }
  g->active = any;
}

static uint16_t player_mask(const GoofPlayerBindings *p, int slot,
                            const GoofHostInputFrame *f,
                            const GoofInputGuard *g) {
  uint16_t mask = 0;
  for (int a = 0; a < GOOF_ACT_COUNT; a++) {
    const GoofBindList *k = &p->list[GOOF_BIND_KEY][a];
    for (int i = 0; i < k->count; i++)
      if (goof_key_held(f, k->code[i]) && !guard_key(g, k->code[i]))
        mask |= kActions[a].bit;
    if (!f->pad_present[slot]) continue;
    const GoofBindList *q = &p->list[GOOF_BIND_PAD][a];
    for (int i = 0; i < q->count; i++)
      if (goof_pad_input_held(&f->pad[slot], q->code[i]) &&
          !guard_pad(g, slot, q->code[i]))
        mask |= kActions[a].bit;
  }
  return mask;
}

GoofInputSample goof_bindings_sample(const GoofBindings *b,
                                     const GoofHostInputFrame *f,
                                     const GoofInputGuard *guard) {
  GoofInputSample s = {0, 0};
  if (!b || !f) return s;
  s.p1 = player_mask(&b->player[0], 0, f, guard);
  s.p2 = player_mask(&b->player[1], 1, f, guard);
  return s;
}

/* ---- conflicts -------------------------------------------------------- */

size_t goof_bindings_conflicts(const GoofBindings *b, GoofBindConflict *out,
                               size_t cap) {
  size_t n = 0;
  /* Enumerate every (player, kind, action, code) once, in canonical order,
   * and pair it with every LATER occurrence of the same physical input that
   * is a conflict by the header's rule. */
  for (int pa = 0; pa < GOOF_HOST_PLAYERS; pa++)
    for (int k = 0; k < GOOF_BIND_KINDS; k++)
      for (int aa = 0; aa < GOOF_ACT_COUNT; aa++) {
        const GoofBindList *la = &b->player[pa].list[k][aa];
        for (int i = 0; i < la->count; i++) {
          uint16_t code = la->code[i];
          for (int pb = pa; pb < GOOF_HOST_PLAYERS; pb++) {
            if (pb != pa && k == GOOF_BIND_PAD) continue;  /* different pads */
            for (int ab = (pb == pa ? aa + 1 : 0); ab < GOOF_ACT_COUNT; ab++) {
              if (!goof_bindlist_contains(&b->player[pb].list[k][ab], code)) continue;
              if (out && n < cap)
                out[n] = (GoofBindConflict){(GoofBindKind)k, code, (uint8_t)pa,
                                            (uint8_t)pb, (GoofSnesAction)aa,
                                            (GoofSnesAction)ab};
              n++;
            }
          }
        }
      }
  return n;
}

bool goof_bindings_action_conflicted(const GoofBindings *b, int player,
                                     GoofBindKind kind, GoofSnesAction a) {
  if (player < 0 || player >= GOOF_HOST_PLAYERS || (unsigned)a >= GOOF_ACT_COUNT)
    return false;
  const GoofBindList *l = &b->player[player].list[kind][a];
  for (int i = 0; i < l->count; i++)
    for (int p = 0; p < GOOF_HOST_PLAYERS; p++) {
      if (p != player && kind == GOOF_BIND_PAD) continue;
      for (int other = 0; other < GOOF_ACT_COUNT; other++) {
        if (p == player && other == (int)a) continue;
        if (goof_bindlist_contains(&b->player[p].list[kind][other], l->code[i]))
          return true;
      }
    }
  return false;
}
