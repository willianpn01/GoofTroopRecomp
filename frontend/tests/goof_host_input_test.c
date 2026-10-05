/* GOOF_ENHANCEMENTS_E1 -- default mapping tests, SDL-free.
 *
 *   B1  each pad control -> its SNES bit, for P1 and for P2 independently
 *       (D-pad, position-true face buttons, L/R, Start, Select)
 *   B2  unbound controls (Guide, stick clicks, triggers, right stick) -> 0;
 *       no button -> 0
 *   B3  several buttons -> the OR of their bits
 *   B4  left stick threshold: |v| must exceed 16384
 *   B5  isolation: P1's pad never reaches p2 and P2's never reaches p1; an
 *       absent slot contributes nothing even with stale state
 *   B6  keyboard: every bound key per player, each player's keys isolated,
 *       keyboard ORed with the same player's pad, out-of-range keys ignored */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "goof_input.h"
#include "host/goof_host_input.h"

static int failures;
static int case_failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; case_failures++; \
  printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void report(const char *name, const char *what) {
  printf("%s %s %s\n", name, case_failures ? "FAIL" : "PASS", what);
  case_failures = 0;
}

typedef struct { GoofHostButton host; uint16_t snes; const char *label; } Expect;

/* The E1 required default (position-true, decision D4). */
static const Expect kExpect[] = {
  {GOOF_HOST_BTN_DPAD_UP,        GOOF_BTN_UP,     "dpad up -> Up"},
  {GOOF_HOST_BTN_DPAD_DOWN,      GOOF_BTN_DOWN,   "dpad down -> Down"},
  {GOOF_HOST_BTN_DPAD_LEFT,      GOOF_BTN_LEFT,   "dpad left -> Left"},
  {GOOF_HOST_BTN_DPAD_RIGHT,     GOOF_BTN_RIGHT,  "dpad right -> Right"},
  {GOOF_HOST_BTN_SOUTH,          GOOF_BTN_B,      "bottom face -> B"},
  {GOOF_HOST_BTN_EAST,           GOOF_BTN_A,      "right face -> A"},
  {GOOF_HOST_BTN_WEST,           GOOF_BTN_Y,      "left face -> Y"},
  {GOOF_HOST_BTN_NORTH,          GOOF_BTN_X,      "top face -> X"},
  {GOOF_HOST_BTN_LEFT_SHOULDER,  GOOF_BTN_L,      "left shoulder -> L"},
  {GOOF_HOST_BTN_RIGHT_SHOULDER, GOOF_BTN_R,      "right shoulder -> R"},
  {GOOF_HOST_BTN_START,          GOOF_BTN_START,  "start -> Start"},
  {GOOF_HOST_BTN_BACK,           GOOF_BTN_SELECT, "back/select/view -> Select"},
};
#define N_EXPECT (sizeof kExpect / sizeof kExpect[0])

static GoofInputSample sample_pad(int player, const GoofHostPadState *pad) {
  GoofHostInputFrame f;
  memset(&f, 0, sizeof f);
  f.pad_present[player] = true;
  f.pad[player] = *pad;
  return goof_host_default_sample(&f);
}

int main(void) {
  /* B1 */
  for (int player = 0; player < 2; player++) {
    for (size_t i = 0; i < N_EXPECT; i++) {
      GoofHostPadState pad;
      memset(&pad, 0, sizeof pad);
      pad.buttons = 1u << kExpect[i].host;
      GoofInputSample s = sample_pad(player, &pad);
      uint16_t mine = player == 0 ? s.p1 : s.p2;
      uint16_t other = player == 0 ? s.p2 : s.p1;
      CHECK(mine == kExpect[i].snes, "P%d %s: got %03x want %03x", player + 1,
            kExpect[i].label, mine, kExpect[i].snes);
      CHECK(other == 0, "P%d %s leaked into the other player (%03x)", player + 1,
            kExpect[i].label, other);
      printf("  P%d %-28s -> %03x\n", player + 1, kExpect[i].label, mine);
    }
  }
  report("B1", "every bound pad control maps to its SNES bit, P1 and P2 independently");

  /* B2 */
  {
    GoofHostPadState pad;
    memset(&pad, 0, sizeof pad);
    CHECK(goof_host_pad_default_mask(&pad) == 0, "no button is not 0");
    const GoofHostButton unbound[] = {GOOF_HOST_BTN_GUIDE, GOOF_HOST_BTN_LEFT_STICK,
                                      GOOF_HOST_BTN_RIGHT_STICK};
    for (size_t i = 0; i < 3; i++) {
      pad.buttons = 1u << unbound[i];
      CHECK(goof_host_pad_default_mask(&pad) == 0, "unbound button %d mapped", unbound[i]);
    }
    pad.buttons = 0;
    pad.axes[GOOF_HOST_AXIS_RIGHT_X] = 32767; pad.axes[GOOF_HOST_AXIS_RIGHT_Y] = -32768;
    pad.axes[GOOF_HOST_AXIS_TRIGGER_LEFT] = 32767; pad.axes[GOOF_HOST_AXIS_TRIGGER_RIGHT] = 32767;
    CHECK(goof_host_pad_default_mask(&pad) == 0, "right stick / triggers mapped");
    CHECK(goof_host_pad_default_mask(NULL) == 0, "NULL pad not 0");
  }
  report("B2", "no button -> 0; Guide, stick clicks, right stick, triggers -> 0");

  /* B3: every subset of the 12 bound controls ORs exactly. */
  {
    long cases = 0;
    for (uint32_t subset = 0; subset < (1u << N_EXPECT); subset++) {
      GoofHostPadState pad;
      memset(&pad, 0, sizeof pad);
      uint16_t want = 0;
      for (size_t i = 0; i < N_EXPECT; i++)
        if (subset & (1u << i)) { pad.buttons |= 1u << kExpect[i].host; want |= kExpect[i].snes; }
      CHECK(goof_host_pad_default_mask(&pad) == want, "subset %03x", subset);
      cases++;
    }
    printf("  subsets=%ld\n", cases);
  }
  report("B3", "multiple buttons -> exact OR mask (all 4096 subsets)");

  /* B4 */
  {
    struct { int x, y; uint16_t want; } pts[] = {
      {0, 0, 0}, {16384, 0, 0}, {-16384, 0, 0}, {0, 16384, 0}, {0, -16384, 0},
      {16385, 0, GOOF_BTN_RIGHT}, {-16385, 0, GOOF_BTN_LEFT},
      {0, 16385, GOOF_BTN_DOWN}, {0, -16385, GOOF_BTN_UP},
      {32767, -32768, GOOF_BTN_RIGHT | GOOF_BTN_UP},
      {-32768, 32767, GOOF_BTN_LEFT | GOOF_BTN_DOWN},
    };
    for (size_t i = 0; i < sizeof pts / sizeof pts[0]; i++) {
      GoofHostPadState pad;
      memset(&pad, 0, sizeof pad);
      pad.axes[GOOF_HOST_AXIS_LEFT_X] = (int16_t)pts[i].x;
      pad.axes[GOOF_HOST_AXIS_LEFT_Y] = (int16_t)pts[i].y;
      CHECK(goof_host_pad_default_mask(&pad) == pts[i].want, "stick %d,%d", pts[i].x, pts[i].y);
    }
  }
  report("B4", "left stick -> D-pad only beyond +/-16384 (strict)");

  /* B5 */
  {
    GoofHostInputFrame f;
    memset(&f, 0, sizeof f);
    f.pad_present[0] = true;  f.pad[0].buttons = 0x7FFF;
    f.pad[0].axes[GOOF_HOST_AXIS_LEFT_X] = 32767;
    GoofInputSample s = goof_host_default_sample(&f);
    CHECK(s.p1 != 0 && s.p2 == 0, "P1 pad reached p2 (%03x)", s.p2);
    memset(&f, 0, sizeof f);
    f.pad_present[1] = true;  f.pad[1].buttons = 0x7FFF;
    s = goof_host_default_sample(&f);
    CHECK(s.p2 != 0 && s.p1 == 0, "P2 pad reached p1 (%03x)", s.p1);
    memset(&f, 0, sizeof f);
    f.pad_present[0] = false; f.pad[0].buttons = 0x7FFF;   /* stale, absent */
    f.pad_present[1] = true;  f.pad[1].buttons = 1u << GOOF_HOST_BTN_START;
    s = goof_host_default_sample(&f);
    CHECK(s.p1 == 0 && s.p2 == GOOF_BTN_START, "absent slot contributed (%03x/%03x)", s.p1, s.p2);
    f.pad_present[0] = true;  f.pad[0].buttons = 1u << GOOF_HOST_BTN_SOUTH;
    s = goof_host_default_sample(&f);
    CHECK(s.p1 == GOOF_BTN_B && s.p2 == GOOF_BTN_START, "simultaneous %03x/%03x", s.p1, s.p2);
  }
  report("B5", "P1 pad never reaches p2, P2 pad never reaches p1, simultaneous input independent");

  /* B6 */
  {
    static const struct { int player; GoofHostKey key; uint16_t snes; } keys[] = {
      {0, GOOF_HOST_KEY_UP, GOOF_BTN_UP}, {0, GOOF_HOST_KEY_DOWN, GOOF_BTN_DOWN},
      {0, GOOF_HOST_KEY_LEFT, GOOF_BTN_LEFT}, {0, GOOF_HOST_KEY_RIGHT, GOOF_BTN_RIGHT},
      {0, GOOF_HOST_KEY_RETURN, GOOF_BTN_START}, {0, GOOF_HOST_KEY_KP_ENTER, GOOF_BTN_START},
      {0, GOOF_HOST_KEY_RSHIFT, GOOF_BTN_SELECT}, {0, GOOF_HOST_KEY_Z, GOOF_BTN_B},
      {0, GOOF_HOST_KEY_A, GOOF_BTN_Y}, {0, GOOF_HOST_KEY_X, GOOF_BTN_A},
      {0, GOOF_HOST_KEY_S, GOOF_BTN_X}, {0, GOOF_HOST_KEY_C, GOOF_BTN_L},
      {0, GOOF_HOST_KEY_V, GOOF_BTN_R},
      {1, GOOF_HOST_KEY_I, GOOF_BTN_UP}, {1, GOOF_HOST_KEY_K, GOOF_BTN_DOWN},
      {1, GOOF_HOST_KEY_J, GOOF_BTN_LEFT}, {1, GOOF_HOST_KEY_L, GOOF_BTN_RIGHT},
      {1, GOOF_HOST_KEY_RCTRL, GOOF_BTN_START}, {1, GOOF_HOST_KEY_RALT, GOOF_BTN_SELECT},
      {1, GOOF_HOST_KEY_N, GOOF_BTN_B}, {1, GOOF_HOST_KEY_B, GOOF_BTN_Y},
      {1, GOOF_HOST_KEY_M, GOOF_BTN_A}, {1, GOOF_HOST_KEY_H, GOOF_BTN_X},
      {1, GOOF_HOST_KEY_U, GOOF_BTN_L}, {1, GOOF_HOST_KEY_O, GOOF_BTN_R},
    };
    static uint8_t state[512];
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
      memset(state, 0, sizeof state);
      state[keys[i].key] = 1;
      GoofHostInputFrame f;
      memset(&f, 0, sizeof f);
      f.keys = state; f.key_count = sizeof state;
      GoofInputSample s = goof_host_default_sample(&f);
      uint16_t mine = keys[i].player == 0 ? s.p1 : s.p2;
      uint16_t other = keys[i].player == 0 ? s.p2 : s.p1;
      CHECK(mine == keys[i].snes && other == 0, "key %d player %d: %03x/%03x",
            keys[i].key, keys[i].player + 1, s.p1, s.p2);
    }
    /* Keyboard ORs with the same player's pad, and only that player's. */
    memset(state, 0, sizeof state);
    state[GOOF_HOST_KEY_Z] = 1;                         /* P1 B */
    GoofHostInputFrame f;
    memset(&f, 0, sizeof f);
    f.keys = state; f.key_count = sizeof state;
    f.pad_present[0] = true; f.pad[0].buttons = 1u << GOOF_HOST_BTN_START;
    f.pad_present[1] = true; f.pad[1].buttons = 1u << GOOF_HOST_BTN_NORTH;
    GoofInputSample s = goof_host_default_sample(&f);
    CHECK(s.p1 == (GOOF_BTN_B | GOOF_BTN_START) && s.p2 == GOOF_BTN_X,
          "kbd|pad combine %03x/%03x", s.p1, s.p2);
    /* key_count bounds the array; NULL keys = no keyboard. */
    CHECK(goof_host_keyboard_default_mask(0, state, GOOF_HOST_KEY_Z) == 0, "key beyond count read");
    CHECK(goof_host_keyboard_default_mask(0, NULL, 512) == 0, "NULL keys");
    CHECK(goof_host_keyboard_default_mask(2, state, 512) == 0, "player 3");
  }
  report("B6", "keyboard P1/P2 bindings isolated; keyboard ORs with the same player's pad");

  printf("GOOF_HOST_INPUT_TEST %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
