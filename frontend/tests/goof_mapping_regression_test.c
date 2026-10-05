/* GOOF_ENHANCEMENTS_E1 -- mapping regression: the E1 host bindings produce
 * exactly the masks the pre-E1 frontend produced.
 *
 * The block between the VERBATIM markers is copied byte-for-byte from
 * the pre-E1 frontend/main_sdl.c, lines 263-337 and
 * 405-429 (prototypes/e1/report.py re-checks it against work/baseline).
 *
 *   K1  every single scancode, every pair, 200000 random key sets, both
 *       players: goof_host_keyboard_default_mask == baseline input_read_keys
 *   K2  combined sample: keyboard P1 | pad P1 and keyboard P2 | pad P2
 *   M1  on an SDL virtual game controller: all 2^15 combinations of the 15
 *       standard buttons, and a sweep of left-stick values around the
 *       threshold -- goof_sdl_pads_read + goof_host_pad_default_mask ==
 *       baseline input_read_pad on the SAME SDL_GameController.
 */
#include <SDL.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "goof_input.h"
#include "host/goof_host_input.h"
#include "sdl_pads.h"

/* ---- BEGIN VERBATIM baseline main_sdl.c:263-337 ---- */
typedef struct {
  SDL_Scancode key;
  uint16_t button;
} HostKeyBind;

static const HostKeyBind kP1Keys[] = {
  {SDL_SCANCODE_UP,       GOOF_BTN_UP},
  {SDL_SCANCODE_DOWN,     GOOF_BTN_DOWN},
  {SDL_SCANCODE_LEFT,     GOOF_BTN_LEFT},
  {SDL_SCANCODE_RIGHT,    GOOF_BTN_RIGHT},
  {SDL_SCANCODE_RETURN,   GOOF_BTN_START},
  {SDL_SCANCODE_KP_ENTER, GOOF_BTN_START},
  {SDL_SCANCODE_RSHIFT,   GOOF_BTN_SELECT},
  {SDL_SCANCODE_Z,        GOOF_BTN_B},
  {SDL_SCANCODE_A,        GOOF_BTN_Y},
  {SDL_SCANCODE_X,        GOOF_BTN_A},
  {SDL_SCANCODE_S,        GOOF_BTN_X},
  {SDL_SCANCODE_C,        GOOF_BTN_L},
  {SDL_SCANCODE_V,        GOOF_BTN_R},
};

/* PLAYER 2 KEYBOARD MAP.  Goof Troop is a two-player co-op game and the core
 * model carries P2 from the start (FramePlan.joy2 -> $421A/$421B), so leaving
 * P2 reachable from one keyboard costs one table.  Chosen to sit clear of the
 * P1 cluster and of P/ESC:
 *
 *   D-pad  I J K L       Start  RCtrl        Select  RAlt
 *   B  N     Y  B        A  M     X  H       L  U      R  O
 *
 * A second keyboard player is cramped by nature; a gamepad in slot 2 is the
 * intended path and is wired below. */
static const HostKeyBind kP2Keys[] = {
  {SDL_SCANCODE_I,      GOOF_BTN_UP},
  {SDL_SCANCODE_K,      GOOF_BTN_DOWN},
  {SDL_SCANCODE_J,      GOOF_BTN_LEFT},
  {SDL_SCANCODE_L,      GOOF_BTN_RIGHT},
  {SDL_SCANCODE_RCTRL,  GOOF_BTN_START},
  {SDL_SCANCODE_RALT,   GOOF_BTN_SELECT},
  {SDL_SCANCODE_N,      GOOF_BTN_B},
  {SDL_SCANCODE_B,      GOOF_BTN_Y},
  {SDL_SCANCODE_M,      GOOF_BTN_A},
  {SDL_SCANCODE_H,      GOOF_BTN_X},
  {SDL_SCANCODE_U,      GOOF_BTN_L},
  {SDL_SCANCODE_O,      GOOF_BTN_R},
};

/* GAMEPAD MAP.  SDL_GameController is already a normalised abstraction, so
 * this is a plain table and no per-device work is needed.  Priority order of
 * section 17: the deterministic model first, keyboard P1 second, this third --
 * so it is opened at startup and on ADDED, released on REMOVED, and nothing
 * about it can fail in a way that stops the guest. */
typedef struct {
  SDL_GameControllerButton pad;
  uint16_t button;
} HostPadBind;

static const HostPadBind kPadButtons[] = {
  {SDL_CONTROLLER_BUTTON_DPAD_UP,       GOOF_BTN_UP},
  {SDL_CONTROLLER_BUTTON_DPAD_DOWN,     GOOF_BTN_DOWN},
  {SDL_CONTROLLER_BUTTON_DPAD_LEFT,     GOOF_BTN_LEFT},
  {SDL_CONTROLLER_BUTTON_DPAD_RIGHT,    GOOF_BTN_RIGHT},
  {SDL_CONTROLLER_BUTTON_START,         GOOF_BTN_START},
  {SDL_CONTROLLER_BUTTON_BACK,          GOOF_BTN_SELECT},
  /* SNES face layout on an SDL pad: SDL A/B (bottom/right) are the SNES
   * B/A row, SDL X/Y (left/top) are the SNES Y/X row.  This is the physical
   * position match, which is what a player's thumb expects. */
  {SDL_CONTROLLER_BUTTON_A,             GOOF_BTN_B},
  {SDL_CONTROLLER_BUTTON_B,             GOOF_BTN_A},
  {SDL_CONTROLLER_BUTTON_X,             GOOF_BTN_Y},
  {SDL_CONTROLLER_BUTTON_Y,             GOOF_BTN_X},
  {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  GOOF_BTN_L},
  {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, GOOF_BTN_R},
};

enum { GOOF_PAD_SLOTS = 2, GOOF_PAD_AXIS_DEADZONE = 16384 };
/* ---- END VERBATIM baseline main_sdl.c:263-337 ---- */

/* ---- BEGIN VERBATIM baseline main_sdl.c:405-429 ---- */
static uint16_t input_read_keys(const uint8_t *keys, const HostKeyBind *map,
                                size_t count) {
  uint16_t mask = 0;
  for (size_t i = 0; i < count; i++)
    if (keys[map[i].key]) mask |= map[i].button;
  return mask;
}

static uint16_t input_read_pad(SDL_GameController *c) {
  if (!c) return 0;
  uint16_t mask = 0;
  for (size_t i = 0; i < sizeof kPadButtons / sizeof kPadButtons[0]; i++)
    if (SDL_GameControllerGetButton(c, kPadButtons[i].pad))
      mask |= kPadButtons[i].button;
  /* Left stick as a digital D-pad.  A stick is analogue host state; the SNES
   * pad has no analogue axis, so the only faithful thing to do is threshold
   * it, and the threshold is a host constant, not a guest one. */
  int x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
  int y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
  if (x < -GOOF_PAD_AXIS_DEADZONE) mask |= GOOF_BTN_LEFT;
  if (x >  GOOF_PAD_AXIS_DEADZONE) mask |= GOOF_BTN_RIGHT;
  if (y < -GOOF_PAD_AXIS_DEADZONE) mask |= GOOF_BTN_UP;
  if (y >  GOOF_PAD_AXIS_DEADZONE) mask |= GOOF_BTN_DOWN;
  return mask;
}
/* ---- END VERBATIM baseline main_sdl.c:405-429 ---- */

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

/* Test isolation: only SDL virtual pads may be visible.  The vendor filter
 * is enough on Linux; on Windows (and Wine) XInput/RawInput/WGI/HIDAPI pads
 * bypass it, so those hardware backends are switched off as well. */
static void hide_real_pads(void) {
  SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x0000/0x0000");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
  SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
  SDL_SetHint(SDL_HINT_DIRECTINPUT_ENABLED, "0");
}


static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
  if (failures < 20) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t next_rand(void) {
  rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
  return (uint32_t)(rng >> 16);
}

static void check_keys(const uint8_t *keys, uint64_t *cases) {
  uint16_t b1 = input_read_keys(keys, kP1Keys, COUNT_OF(kP1Keys));
  uint16_t b2 = input_read_keys(keys, kP2Keys, COUNT_OF(kP2Keys));
  uint16_t n1 = goof_host_keyboard_default_mask(0, keys, SDL_NUM_SCANCODES);
  uint16_t n2 = goof_host_keyboard_default_mask(1, keys, SDL_NUM_SCANCODES);
  CHECK(b1 == n1 && b2 == n2, "keys: baseline %03x/%03x new %03x/%03x", b1, b2, n1, n2);
  (*cases)++;
}

static void test_keyboard(void) {
  static uint8_t keys[SDL_NUM_SCANCODES];
  uint64_t cases = 0;
  int before = failures;
  memset(keys, 0, sizeof keys);
  check_keys(keys, &cases);
  for (int a = 0; a < SDL_NUM_SCANCODES; a++) {
    keys[a] = 1;
    check_keys(keys, &cases);
    for (int b = a + 1; b < SDL_NUM_SCANCODES; b++) {
      keys[b] = 1; check_keys(keys, &cases); keys[b] = 0;
    }
    keys[a] = 0;
  }
  /* Random sets drawn from the 25 bound keys plus noise keys. */
  int bound[32]; int nb = 0;
  for (size_t i = 0; i < COUNT_OF(kP1Keys); i++) bound[nb++] = kP1Keys[i].key;
  for (size_t i = 0; i < COUNT_OF(kP2Keys); i++) bound[nb++] = kP2Keys[i].key;
  for (int r = 0; r < 200000; r++) {
    memset(keys, 0, sizeof keys);
    int n = (int)(next_rand() % 12);
    for (int i = 0; i < n; i++) keys[bound[next_rand() % (unsigned)nb]] = 1;
    for (int i = 0; i < 3; i++) keys[next_rand() % SDL_NUM_SCANCODES] = 1;
    check_keys(keys, &cases);
  }
  /* Nonzero byte values other than 1 are "down" in both. */
  memset(keys, 0, sizeof keys);
  keys[SDL_SCANCODE_Z] = 0xFF; keys[SDL_SCANCODE_N] = 2;
  check_keys(keys, &cases);
  printf("K1 %s keyboard_cases=%llu single+pairs+random both_players\n",
         failures == before ? "PASS" : "FAIL", (unsigned long long)cases);
}

static void test_combined(void) {
  static uint8_t keys[SDL_NUM_SCANCODES];
  int before = failures;
  uint64_t cases = 0;
  for (int r = 0; r < 100000; r++) {
    memset(keys, 0, sizeof keys);
    int n = (int)(next_rand() % 6);
    for (int i = 0; i < n; i++) keys[next_rand() % SDL_NUM_SCANCODES] = 1;
    GoofHostInputFrame f;
    memset(&f, 0, sizeof f);
    f.keys = keys; f.key_count = SDL_NUM_SCANCODES;
    for (int k = 0; k < 2; k++) {
      f.pad_present[k] = (next_rand() & 1) != 0;
      f.pad[k].buttons = next_rand() & 0x7FFF;
      for (int a = 0; a < GOOF_HOST_AXIS_COUNT; a++)
        f.pad[k].axes[a] = (int16_t)(next_rand() & 0xFFFF);
    }
    GoofInputSample s = goof_host_default_sample(&f);
    /* The pre-E1 combination: keys(P1) | pad(slot 1), keys(P2) | pad(slot 2). */
    uint16_t e1 = input_read_keys(keys, kP1Keys, COUNT_OF(kP1Keys));
    uint16_t e2 = input_read_keys(keys, kP2Keys, COUNT_OF(kP2Keys));
    if (f.pad_present[0]) e1 |= goof_host_pad_default_mask(&f.pad[0]);
    if (f.pad_present[1]) e2 |= goof_host_pad_default_mask(&f.pad[1]);
    CHECK(s.p1 == e1 && s.p2 == e2, "combined %03x/%03x vs %03x/%03x", s.p1, s.p2, e1, e2);
    cases++;
  }
  printf("K2 %s combined_cases=%llu p1=kbd1|pad1 p2=kbd2|pad2\n",
         failures == before ? "PASS" : "FAIL", (unsigned long long)cases);
}

static int attach_pad(const char *name) {
  SDL_VirtualJoystickDesc d;
  memset(&d, 0, sizeof d);
  d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
  d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
  d.naxes = SDL_CONTROLLER_AXIS_MAX;
  d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
  d.name = name;
  return SDL_JoystickAttachVirtualEx(&d);
}

static void test_pad(void) {
  int before = failures;
  GoofSdlPads pads;
  if (!goof_sdl_pads_open(&pads, NULL)) { CHECK(0, "pads open"); return; }
  int index = attach_pad("E1 mapping pad");
  CHECK(index >= 0, "attach: %s", SDL_GetError());
  SDL_PumpEvents();
  goof_sdl_pads_scan(&pads);
  const GoofControllerDevice *d = goof_controllers_slot(&pads.slots, 0);
  CHECK(d != NULL, "virtual pad not in P1");
  if (!d) return;
  SDL_GameController *c = (SDL_GameController *)d->handle;
  SDL_Joystick *j = SDL_GameControllerGetJoystick(c);
  uint64_t cases = 0;
  for (uint32_t combo = 0; combo < (1u << 15); combo++) {
    for (int b = 0; b < 15; b++) SDL_JoystickSetVirtualButton(j, b, (combo >> b) & 1);
    SDL_GameControllerUpdate();
    GoofHostPadState st;
    goof_sdl_pads_read(&pads, 0, &st);
    CHECK(st.buttons == combo, "virtual button readback %04x vs %04x", st.buttons, combo);
    uint16_t base = input_read_pad(c);
    uint16_t now = goof_host_pad_default_mask(&st);
    CHECK(base == now, "pad combo %04x: baseline %03x new %03x", combo, base, now);
    cases++;
  }
  for (int b = 0; b < 15; b++) SDL_JoystickSetVirtualButton(j, b, 0);
  static const int kAxis[] = {-32768, -32767, -16385, -16384, -16383, -1, 0, 1,
                              16383, 16384, 16385, 32767};
  for (size_t xi = 0; xi < COUNT_OF(kAxis); xi++) {
    for (size_t yi = 0; yi < COUNT_OF(kAxis); yi++) {
      SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_LEFTX, (Sint16)kAxis[xi]);
      SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_LEFTY, (Sint16)kAxis[yi]);
      SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_RIGHTX, (Sint16)kAxis[yi]);
      SDL_GameControllerUpdate();
      GoofHostPadState st;
      goof_sdl_pads_read(&pads, 0, &st);
      uint16_t base = input_read_pad(c);
      uint16_t now = goof_host_pad_default_mask(&st);
      CHECK(base == now, "stick %d,%d: baseline %03x new %03x", kAxis[xi], kAxis[yi], base, now);
      cases++;
    }
  }
  printf("M1 %s pad_cases=%llu button_combos=32768 stick_points=%zu"
         " (SDL virtual game controller, baseline input_read_pad vs E1)\n",
         failures == before ? "PASS" : "FAIL", (unsigned long long)cases,
         COUNT_OF(kAxis) * COUNT_OF(kAxis));
  goof_sdl_pads_close(&pads);
}

/* argc/argv: on Windows SDL.h renames main to SDL_main(int, char **). */
int main(int argc, char **argv) {
  (void)argc; (void)argv;
  hide_real_pads();
  if (SDL_Init(SDL_INIT_EVENTS) != 0) { printf("SDL_Init: %s\n", SDL_GetError()); return 2; }
  test_keyboard();
  test_combined();
  test_pad();
  SDL_Quit();
  printf("GOOF_MAPPING_REGRESSION %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
