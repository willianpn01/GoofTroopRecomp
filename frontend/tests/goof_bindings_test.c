/* GOOF_ENHANCEMENTS_E2 -- binding model / mapping tests, SDL-free.
 *
 *   M1   custom keyboard binding sets the right SNES bit (and only it)
 *   M2   custom gamepad binding sets the right SNES bit
 *   M3   several sources on one action OR together (key + key + pad)
 *   M4   a P1 custom mapping does not affect P2
 *   M5   a P2 custom mapping does not affect P1
 *   M6   a cleared binding never activates
 *   M7   conflicts: same input on two actions detected; two inputs on one
 *        action and the same pad input on P1 and P2 are not conflicts;
 *        one key on both players is
 *   M8   reset restores the E1 default mapping
 *   M9   config save -> reload preserves custom mappings (bit-exact masks)
 *   M10  defaults == E1 goof_host_default_sample over the whole input space:
 *        every key (0..511) alone, every key pair of the E1 tables, all
 *        32768 pad button sets x 9 stick points per slot, 200000 random
 *        combined frames
 *   M11  input guard: held-at-close controls are hidden until released
 *   M12  names: every key/pad token round-trips; reserved keys refused */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "goof_input.h"
#include "host/goof_bindings.h"
#include "host/goof_config.h"
#include "host/goof_host_input.h"

static int failures, case_failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; case_failures++; \
  if (case_failures < 8) { printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
  printf(__VA_ARGS__); printf("\n"); } } } while (0)

static void report(const char *name, const char *what) {
  printf("%s %s %s\n", name, case_failures ? "FAIL" : "PASS", what);
  case_failures = 0;
}

static uint8_t keys[512];
static GoofHostInputFrame frame(void) {
  GoofHostInputFrame f;
  memset(&f, 0, sizeof f);
  f.keys = keys;
  f.key_count = sizeof keys;
  return f;
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next_rand(void) {
  rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
  return rng;
}

static bool same(GoofInputSample a, GoofInputSample b) { return a.p1 == b.p1 && a.p2 == b.p2; }

int main(void) {
  GoofBindings def, b;
  goof_bindings_defaults(&def);

  /* M1 */
  b = def;
  goof_bindlist_set_single(&b.player[0].list[GOOF_BIND_KEY][GOOF_ACT_B], 20 /* Q */);
  memset(keys, 0, sizeof keys);
  GoofHostInputFrame f = frame();
  keys[20] = 1;
  GoofInputSample s = goof_bindings_sample(&b, &f, NULL);
  CHECK(s.p1 == GOOF_BTN_B && s.p2 == 0, "Q -> P1 B, got %03X %03X", s.p1, s.p2);
  keys[20] = 0; keys[GOOF_HOST_KEY_Z] = 1;             /* old key no longer B */
  s = goof_bindings_sample(&b, &f, NULL);
  CHECK(s.p1 == 0 && s.p2 == 0, "Z unbound after remap, got %03X", s.p1);
  report("M1", "custom keyboard binding -> correct SNES bit");

  /* M2 */
  b = def;
  goof_bindlist_set_single(&b.player[0].list[GOOF_BIND_PAD][GOOF_ACT_B], GOOF_HOST_BTN_NORTH);
  memset(keys, 0, sizeof keys);
  f = frame();
  f.pad_present[0] = true;
  f.pad[0].buttons = 1u << GOOF_HOST_BTN_NORTH;
  s = goof_bindings_sample(&b, &f, NULL);
  CHECK(s.p1 == (GOOF_BTN_B | GOOF_BTN_X), "top -> B (and still X by default), got %03X", s.p1);
  f.pad[0].buttons = 1u << GOOF_HOST_BTN_SOUTH;
  s = goof_bindings_sample(&b, &f, NULL);
  CHECK(s.p1 == 0, "bottom no longer B, got %03X", s.p1);
  report("M2", "custom gamepad binding -> correct SNES bit");

  /* M3 */
  b = def;
  goof_bindlist_add(&b.player[0].list[GOOF_BIND_KEY][GOOF_ACT_START], 44 /* SPACE */);
  const uint16_t start_keys[3] = {GOOF_HOST_KEY_RETURN, GOOF_HOST_KEY_KP_ENTER, 44};
  for (int i = 0; i < 3; i++) {
    memset(keys, 0, sizeof keys);
    f = frame();
    keys[start_keys[i]] = 1;
    s = goof_bindings_sample(&b, &f, NULL);
    CHECK(s.p1 == GOOF_BTN_START, "source %d -> Start, got %03X", i, s.p1);
  }
  memset(keys, 0, sizeof keys);
  f = frame();
  f.pad_present[0] = true;
  f.pad[0].buttons = 1u << GOOF_HOST_BTN_START;
  keys[GOOF_HOST_KEY_RETURN] = 1; keys[44] = 1;
  s = goof_bindings_sample(&b, &f, NULL);
  CHECK(s.p1 == GOOF_BTN_START, "all sources together -> Start, got %03X", s.p1);
  report("M3", "multiple sources on one action OR together");

  /* M4 / M5 */
  for (int who = 0; who < 2; who++) {
    b = def;
    int other = who ^ 1;
    goof_bindlist_set_single(&b.player[who].list[GOOF_BIND_PAD][GOOF_ACT_A],
                             GOOF_HOST_BTN_LEFT_SHOULDER);
    goof_bindlist_set_single(&b.player[who].list[GOOF_BIND_KEY][GOOF_ACT_A], 20 /* Q */);
    goof_bindlist_clear(&b.player[who].list[GOOF_BIND_PAD][GOOF_ACT_L]);
    CHECK(memcmp(&b.player[other], &def.player[other], sizeof def.player[other]) == 0,
          "other player's table untouched");
    for (uint32_t btn = 0; btn < (1u << GOOF_HOST_BTN_COUNT); btn += 7) {
      memset(keys, 0, sizeof keys);
      f = frame();
      f.pad_present[0] = f.pad_present[1] = true;
      f.pad[0].buttons = f.pad[1].buttons = btn;
      GoofInputSample want = goof_host_default_sample(&f);
      s = goof_bindings_sample(&b, &f, NULL);
      uint16_t got_other = other == 0 ? s.p1 : s.p2;
      uint16_t want_other = other == 0 ? want.p1 : want.p2;
      CHECK(got_other == want_other, "P%d unaffected by P%d remap (btn %05X)",
            other + 1, who + 1, btn);
    }
    memset(keys, 0, sizeof keys);
    f = frame();
    f.pad_present[who] = true;
    f.pad[who].buttons = 1u << GOOF_HOST_BTN_LEFT_SHOULDER;
    s = goof_bindings_sample(&b, &f, NULL);
    CHECK((who == 0 ? s.p1 : s.p2) == GOOF_BTN_A && (who == 0 ? s.p2 : s.p1) == 0,
          "P%d left shoulder -> A only", who + 1);
    report(who == 0 ? "M4" : "M5", who == 0 ? "P1 custom mapping does not affect P2"
                                            : "P2 custom mapping does not affect P1");
  }

  /* M6 */
  b = def;
  goof_bindlist_clear(&b.player[0].list[GOOF_BIND_KEY][GOOF_ACT_SELECT]);
  goof_bindlist_clear(&b.player[0].list[GOOF_BIND_PAD][GOOF_ACT_SELECT]);
  memset(keys, 0, sizeof keys);
  f = frame();
  keys[GOOF_HOST_KEY_RSHIFT] = 1;
  f.pad_present[0] = true;
  f.pad[0].buttons = 1u << GOOF_HOST_BTN_BACK;
  s = goof_bindings_sample(&b, &f, NULL);
  CHECK(s.p1 == 0, "cleared Select never fires, got %03X", s.p1);
  report("M6", "cleared binding does not activate");

  /* M7 */
  GoofBindConflict c[8];
  CHECK(goof_bindings_conflicts(&def, c, 8) == 0, "defaults are conflict-free");
  b = def;
  goof_bindlist_add(&b.player[0].list[GOOF_BIND_PAD][GOOF_ACT_A], GOOF_HOST_BTN_SOUTH);
  size_t n = goof_bindings_conflicts(&b, c, 8);
  CHECK(n == 1 && c[0].kind == GOOF_BIND_PAD && c[0].code == GOOF_HOST_BTN_SOUTH &&
        c[0].action_a == GOOF_ACT_B && c[0].action_b == GOOF_ACT_A,
        "bottom -> B and A is one conflict (n=%zu)", n);
  CHECK(goof_bindings_action_conflicted(&b, 0, GOOF_BIND_PAD, GOOF_ACT_A) &&
        goof_bindings_action_conflicted(&b, 0, GOOF_BIND_PAD, GOOF_ACT_B) &&
        !goof_bindings_action_conflicted(&b, 1, GOOF_BIND_PAD, GOOF_ACT_B) &&
        !goof_bindings_action_conflicted(&b, 0, GOOF_BIND_KEY, GOOF_ACT_B),
        "conflict flags per row");
  b = def;
  goof_bindlist_add(&b.player[0].list[GOOF_BIND_KEY][GOOF_ACT_B], 27 /* X is A */);
  CHECK(goof_bindings_conflicts(&b, NULL, 0) == 1, "one key on B and A: conflict");
  b = def;
  goof_bindlist_add(&b.player[0].list[GOOF_BIND_KEY][GOOF_ACT_B], 20);
  goof_bindlist_add(&b.player[0].list[GOOF_BIND_KEY][GOOF_ACT_B], 21);
  CHECK(goof_bindings_conflicts(&b, NULL, 0) == 0, "two keys on one action: valid");
  CHECK(goof_bindings_conflicts(&def, NULL, 0) == 0 &&
        goof_bindlist_contains(&def.player[1].list[GOOF_BIND_PAD][GOOF_ACT_B],
                               GOOF_HOST_BTN_SOUTH),
        "same pad input for P1 and P2 (different pads): valid");
  b = def;
  goof_bindlist_add(&b.player[1].list[GOOF_BIND_KEY][GOOF_ACT_B], GOOF_HOST_KEY_Z);
  n = goof_bindings_conflicts(&b, c, 8);
  CHECK(n == 1 && c[0].player_a == 0 && c[0].player_b == 1, "Z on P1 B and P2 B: conflict");
  report("M7", "conflict detection (one input -> two actions; keyboard shared)");

  /* M8 */
  b = def;
  for (int p = 0; p < 2; p++)
    for (int k = 0; k < 2; k++)
      for (int a = 0; a < GOOF_ACT_COUNT; a++)
        goof_bindlist_set_single(&b.player[p].list[k][a], (uint16_t)(k ? 5 : 30 + a));
  goof_bindings_defaults(&b);
  CHECK(goof_bindings_equal(&b, &def), "reset == defaults");
  report("M8", "reset restores the E1 default mapping");

  /* M9 */
  GoofConfig cfg;
  goof_config_defaults(&cfg);
  goof_bindlist_set_single(&cfg.bindings.player[0].list[GOOF_BIND_KEY][GOOF_ACT_B], 20);
  goof_bindlist_set_single(&cfg.bindings.player[1].list[GOOF_BIND_PAD][GOOF_ACT_A],
                           GOOF_HOST_BTN_NORTH);
  goof_bindlist_clear(&cfg.bindings.player[1].list[GOOF_BIND_PAD][GOOF_ACT_X]);
  goof_bindlist_add(&cfg.bindings.player[0].list[GOOF_BIND_PAD][GOOF_ACT_L],
                    GOOF_HOST_BTN_LEFT_STICK);
  static char text[GOOF_CONFIG_MAX_BYTES];
  size_t len = goof_config_serialize(&cfg, text, sizeof text);
  GoofConfig back;
  GoofConfigLoadStatus st = goof_config_parse(text, len, &back, NULL, NULL);
  CHECK(st == GOOF_CONFIG_LOADED && goof_bindings_equal(&back.bindings, &cfg.bindings),
        "reparsed bindings equal");
  for (int i = 0; i < 20000; i++) {
    for (size_t k = 0; k < sizeof keys; k++) keys[k] = (next_rand() & 31) == 0;
    f = frame();
    for (int p = 0; p < 2; p++) {
      f.pad_present[p] = next_rand() & 1;
      f.pad[p].buttons = (uint32_t)next_rand() & 0x7fff;
      f.pad[p].axes[0] = (int16_t)next_rand();
      f.pad[p].axes[1] = (int16_t)next_rand();
    }
    CHECK(same(goof_bindings_sample(&cfg.bindings, &f, NULL),
               goof_bindings_sample(&back.bindings, &f, NULL)), "masks after reload");
  }
  report("M9", "save -> reload preserves mappings (20000 random frames)");

  /* M10 */
  memset(keys, 0, sizeof keys);
  for (int k = 0; k < 512; k++) {
    memset(keys, 0, sizeof keys);
    keys[k] = 1;
    f = frame();
    CHECK(same(goof_bindings_sample(&def, &f, NULL), goof_host_default_sample(&f)),
          "single key %d", k);
  }
  static const uint16_t kTableKeys[] = {
    GOOF_HOST_KEY_A, GOOF_HOST_KEY_B, GOOF_HOST_KEY_C, GOOF_HOST_KEY_H, GOOF_HOST_KEY_I,
    GOOF_HOST_KEY_J, GOOF_HOST_KEY_K, GOOF_HOST_KEY_L, GOOF_HOST_KEY_M, GOOF_HOST_KEY_N,
    GOOF_HOST_KEY_O, GOOF_HOST_KEY_S, GOOF_HOST_KEY_U, GOOF_HOST_KEY_V, GOOF_HOST_KEY_X,
    GOOF_HOST_KEY_Z, GOOF_HOST_KEY_RETURN, GOOF_HOST_KEY_RIGHT, GOOF_HOST_KEY_LEFT,
    GOOF_HOST_KEY_DOWN, GOOF_HOST_KEY_UP, GOOF_HOST_KEY_KP_ENTER, GOOF_HOST_KEY_RCTRL,
    GOOF_HOST_KEY_RSHIFT, GOOF_HOST_KEY_RALT,
  };
  size_t nk = sizeof kTableKeys / sizeof kTableKeys[0];
  unsigned long cases = 512;
  for (size_t i = 0; i < nk; i++)
    for (size_t j = i + 1; j < nk; j++) {
      memset(keys, 0, sizeof keys);
      keys[kTableKeys[i]] = keys[kTableKeys[j]] = 1;
      f = frame();
      CHECK(same(goof_bindings_sample(&def, &f, NULL), goof_host_default_sample(&f)),
            "key pair %u %u", kTableKeys[i], kTableKeys[j]);
      cases++;
    }
  static const int16_t kAxis[] = {-32768, -16385, -16384, 0, 16384, 16385, 32767, -1, 1};
  memset(keys, 0, sizeof keys);
  for (int slot = 0; slot < 2; slot++)
    for (uint32_t btn = 0; btn < (1u << GOOF_HOST_BTN_COUNT); btn++)
      for (int ax = 0; ax < 9; ax++) {
        f = frame();
        f.pad_present[slot] = true;
        f.pad[slot].buttons = btn;
        f.pad[slot].axes[GOOF_HOST_AXIS_LEFT_X] = kAxis[ax];
        f.pad[slot].axes[GOOF_HOST_AXIS_LEFT_Y] = kAxis[(ax * 5 + (int)btn) % 9];
        f.pad[slot].axes[GOOF_HOST_AXIS_RIGHT_X] = kAxis[(ax + 3) % 9];
        f.pad[slot].axes[GOOF_HOST_AXIS_TRIGGER_LEFT] = 32767;
        CHECK(same(goof_bindings_sample(&def, &f, NULL), goof_host_default_sample(&f)),
              "slot %d buttons %05X axis %d", slot, btn, ax);
        cases++;
      }
  for (int i = 0; i < 200000; i++) {
    for (size_t k = 0; k < sizeof keys; k++) keys[k] = (next_rand() % 40) == 0;
    f = frame();
    f.key_count = (size_t)(next_rand() % 513);
    for (int p = 0; p < 2; p++) {
      f.pad_present[p] = next_rand() & 1;
      f.pad[p].buttons = (uint32_t)next_rand();       /* incl. bits >= 15 */
      for (int a = 0; a < GOOF_HOST_AXIS_COUNT; a++) f.pad[p].axes[a] = (int16_t)next_rand();
    }
    GoofInputSample x = goof_bindings_sample(&def, &f, NULL);
    GoofInputSample y = goof_host_default_sample(&f);
    CHECK(same(x, y), "random %d: %03X %03X vs %03X %03X", i, x.p1, x.p2, y.p1, y.p2);
    cases++;
  }
  f = frame();
  f.keys = NULL;
  CHECK(same(goof_bindings_sample(&def, &f, NULL), goof_host_default_sample(&f)), "NULL keys");
  printf("  M10 cases=%lu\n", cases + 1);
  report("M10", "config absent (defaults) == E1 goof_host_default_sample, exhaustive");

  /* M11 */
  GoofInputGuard g;
  memset(keys, 0, sizeof keys);
  keys[GOOF_HOST_KEY_RETURN] = 1;
  f = frame();
  f.pad_present[0] = true;
  f.pad[0].buttons = 1u << GOOF_HOST_BTN_SOUTH;
  goof_input_guard_arm(&g, &f);
  CHECK(g.active, "guard armed");
  goof_input_guard_update(&g, &f);
  s = goof_bindings_sample(&def, &f, &g);
  CHECK(s.p1 == 0, "held Enter + bottom hidden, got %03X", s.p1);
  keys[GOOF_HOST_KEY_Z] = 1;                          /* new press passes */
  goof_input_guard_update(&g, &f);
  s = goof_bindings_sample(&def, &f, &g);
  CHECK(s.p1 == GOOF_BTN_B, "new press visible, got %03X", s.p1);
  keys[GOOF_HOST_KEY_RETURN] = 0;
  goof_input_guard_update(&g, &f);
  keys[GOOF_HOST_KEY_RETURN] = 1;                     /* re-pressed: visible */
  goof_input_guard_update(&g, &f);
  s = goof_bindings_sample(&def, &f, &g);
  CHECK(s.p1 == (GOOF_BTN_B | GOOF_BTN_START), "Enter after release visible, got %03X", s.p1);
  f.pad[0].buttons = 0;
  goof_input_guard_update(&g, &f);
  CHECK(!g.active, "guard empties once everything was released");
  f.pad[0].buttons = 1u << GOOF_HOST_BTN_SOUTH;
  s = goof_bindings_sample(&def, &f, &g);
  CHECK(s.p1 == (GOOF_BTN_B | GOOF_BTN_START), "bottom after release visible");
  memset(&g, 0, sizeof g);
  CHECK(same(goof_bindings_sample(&def, &f, &g), goof_bindings_sample(&def, &f, NULL)),
        "empty guard == no guard");
  report("M11", "post-Settings guard hides held controls until released");

  /* M12 */
  char tok[32];
  for (uint16_t k = 1; k < 512; k++) {
    uint16_t back_code = 0;
    CHECK(goof_key_token(k, tok, sizeof tok) && goof_key_from_token(tok, &back_code) &&
          back_code == k, "key %u token %s", k, tok);
  }
  for (uint16_t p = 0; p < GOOF_PAD_INPUT_COUNT; p++) {
    uint16_t back_code = 0xffff;
    CHECK(goof_pad_from_token(goof_pad_token(p), &back_code) && back_code == p,
          "pad %u", p);
  }
  uint16_t code;
  CHECK(goof_key_from_token("kp_enter", &code) && code == 88, "case-insensitive tokens");
  CHECK(!goof_key_from_token("HID_29", &code), "named key has one spelling (Z, not HID_29)");
  CHECK(goof_key_is_reserved(59) && goof_key_is_reserved(41) && goof_key_is_reserved(19) &&
        !goof_key_is_valid(59), "F2, ESCAPE, P reserved");
  report("M12", "token round-trip for 511 keys and 19 pad inputs; reserved keys");

  printf("GOOF_BINDINGS_TEST %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
