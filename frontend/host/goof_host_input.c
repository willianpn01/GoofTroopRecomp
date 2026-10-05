/* Goof Troop Recomp -- default host -> SNES mapping.  SDL-FREE.
 * See goof_host_input.h.  The tables are the pre-E1 main_sdl.c tables
 * (kP1Keys, kP2Keys, kPadButtons) re-expressed in SDL-free identifiers;
 * goof_mapping_regression_test.c proves the masks are identical. */
#include "goof_host_input.h"

typedef struct {
  uint8_t host;       /* GoofHostKey or GoofHostButton */
  uint16_t snes;      /* GOOF_BTN_* */
} GoofHostBind;

/* PLAYER 1 KEYBOARD (snesrecomp desktop layout):
 *   D-pad arrows   Start Enter/KP-Enter   Select RShift
 *   B Z   Y A   A X   X S   L C   R V */
static const GoofHostBind kP1Keys[] = {
  {GOOF_HOST_KEY_UP,       GOOF_BTN_UP},
  {GOOF_HOST_KEY_DOWN,     GOOF_BTN_DOWN},
  {GOOF_HOST_KEY_LEFT,     GOOF_BTN_LEFT},
  {GOOF_HOST_KEY_RIGHT,    GOOF_BTN_RIGHT},
  {GOOF_HOST_KEY_RETURN,   GOOF_BTN_START},
  {GOOF_HOST_KEY_KP_ENTER, GOOF_BTN_START},
  {GOOF_HOST_KEY_RSHIFT,   GOOF_BTN_SELECT},
  {GOOF_HOST_KEY_Z,        GOOF_BTN_B},
  {GOOF_HOST_KEY_A,        GOOF_BTN_Y},
  {GOOF_HOST_KEY_X,        GOOF_BTN_A},
  {GOOF_HOST_KEY_S,        GOOF_BTN_X},
  {GOOF_HOST_KEY_C,        GOOF_BTN_L},
  {GOOF_HOST_KEY_V,        GOOF_BTN_R},
};

/* PLAYER 2 KEYBOARD:
 *   D-pad I J K L   Start RCtrl   Select RAlt
 *   B N   Y B   A M   X H   L U   R O */
static const GoofHostBind kP2Keys[] = {
  {GOOF_HOST_KEY_I,     GOOF_BTN_UP},
  {GOOF_HOST_KEY_K,     GOOF_BTN_DOWN},
  {GOOF_HOST_KEY_J,     GOOF_BTN_LEFT},
  {GOOF_HOST_KEY_L,     GOOF_BTN_RIGHT},
  {GOOF_HOST_KEY_RCTRL, GOOF_BTN_START},
  {GOOF_HOST_KEY_RALT,  GOOF_BTN_SELECT},
  {GOOF_HOST_KEY_N,     GOOF_BTN_B},
  {GOOF_HOST_KEY_B,     GOOF_BTN_Y},
  {GOOF_HOST_KEY_M,     GOOF_BTN_A},
  {GOOF_HOST_KEY_H,     GOOF_BTN_X},
  {GOOF_HOST_KEY_U,     GOOF_BTN_L},
  {GOOF_HOST_KEY_O,     GOOF_BTN_R},
};

/* PAD, position-true (D4).  The SNES face diamond is B bottom, A right,
 * Y left, X top, so each modern button maps to the SNES button that sits in
 * the same place, whatever letter is printed on it. */
static const GoofHostBind kPadButtons[] = {
  {GOOF_HOST_BTN_DPAD_UP,        GOOF_BTN_UP},
  {GOOF_HOST_BTN_DPAD_DOWN,      GOOF_BTN_DOWN},
  {GOOF_HOST_BTN_DPAD_LEFT,      GOOF_BTN_LEFT},
  {GOOF_HOST_BTN_DPAD_RIGHT,     GOOF_BTN_RIGHT},
  {GOOF_HOST_BTN_START,          GOOF_BTN_START},
  {GOOF_HOST_BTN_BACK,           GOOF_BTN_SELECT},
  {GOOF_HOST_BTN_SOUTH,          GOOF_BTN_B},
  {GOOF_HOST_BTN_EAST,           GOOF_BTN_A},
  {GOOF_HOST_BTN_WEST,           GOOF_BTN_Y},
  {GOOF_HOST_BTN_NORTH,          GOOF_BTN_X},
  {GOOF_HOST_BTN_LEFT_SHOULDER,  GOOF_BTN_L},
  {GOOF_HOST_BTN_RIGHT_SHOULDER, GOOF_BTN_R},
};

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

uint16_t goof_host_pad_default_mask(const GoofHostPadState *pad) {
  if (!pad) return 0;
  uint16_t mask = 0;
  for (size_t i = 0; i < COUNT_OF(kPadButtons); i++)
    if (pad->buttons & (1u << kPadButtons[i].host)) mask |= kPadButtons[i].snes;
  int x = pad->axes[GOOF_HOST_AXIS_LEFT_X];
  int y = pad->axes[GOOF_HOST_AXIS_LEFT_Y];
  if (x < -GOOF_HOST_STICK_THRESHOLD) mask |= GOOF_BTN_LEFT;
  if (x >  GOOF_HOST_STICK_THRESHOLD) mask |= GOOF_BTN_RIGHT;
  if (y < -GOOF_HOST_STICK_THRESHOLD) mask |= GOOF_BTN_UP;
  if (y >  GOOF_HOST_STICK_THRESHOLD) mask |= GOOF_BTN_DOWN;
  return mask;
}

uint16_t goof_host_keyboard_default_mask(int player, const uint8_t *keys,
                                         size_t key_count) {
  if (!keys || (player != 0 && player != 1)) return 0;
  const GoofHostBind *map = player == 0 ? kP1Keys : kP2Keys;
  size_t count = player == 0 ? COUNT_OF(kP1Keys) : COUNT_OF(kP2Keys);
  uint16_t mask = 0;
  for (size_t i = 0; i < count; i++)
    if (map[i].host < key_count && keys[map[i].host]) mask |= map[i].snes;
  return mask;
}

GoofInputSample goof_host_default_sample(const GoofHostInputFrame *frame) {
  GoofInputSample s = {0, 0};
  if (!frame) return s;
  s.p1 = goof_host_keyboard_default_mask(0, frame->keys, frame->key_count);
  s.p2 = goof_host_keyboard_default_mask(1, frame->keys, frame->key_count);
  if (frame->pad_present[0]) s.p1 |= goof_host_pad_default_mask(&frame->pad[0]);
  if (frame->pad_present[1]) s.p2 |= goof_host_pad_default_mask(&frame->pad[1]);
  return s;
}
