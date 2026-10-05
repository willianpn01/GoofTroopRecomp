#ifndef GOOF_HOST_INPUT_H
#define GOOF_HOST_INPUT_H

/* Goof Troop Recomp -- normalised HOST input and the default SNES mapping.
 * GOOF_ENHANCEMENTS_E1_MODERN_CONTROLLER.
 *
 * SDL-FREE.  This module knows what a modern pad and a keyboard LOOK like
 * (buttons by physical position, keys by USB HID usage), and how the default
 * bindings turn them into the 12-bit SNES masks of goof_input.h.  It does not
 * know which device is plugged in (goof_controller.h), how SDL reports it
 * (sdl_pads.c), or anything about the guest beyond the GoofInputSample bit
 * layout.  Nothing here is linked into goof_app_headless or any phase4 gate.
 *
 *   physical device (SDL)            sdl_pads.c / main_sdl.c
 *     -> GoofHostPadState / key array   (this header: normalised host state)
 *       -> default mapping              (this header)
 *         -> GoofInputSample            (goof_input.h, the guest seam)
 *
 * E2 replaces the default tables with user bindings; the types stay.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "goof_input.h"

/* Modern pad buttons named by PHYSICAL POSITION.  The numbering is the SDL2
 * SDL_GameControllerButton numbering (0..14), and sdl_pads.c static-asserts
 * that, so the adapter copies a button index without a translation table.
 * SDL reports face buttons by position ("as though on an Xbox controller")
 * only while SDL_GAMECONTROLLER_USE_BUTTON_LABELS is "0"; sdl_pads.c sets it. */
typedef enum {
  GOOF_HOST_BTN_SOUTH = 0,      /* SDL A: bottom face button               */
  GOOF_HOST_BTN_EAST,           /* SDL B: right face button                */
  GOOF_HOST_BTN_WEST,           /* SDL X: left face button                 */
  GOOF_HOST_BTN_NORTH,          /* SDL Y: top face button                  */
  GOOF_HOST_BTN_BACK,           /* Back / Select / View / Share / Minus    */
  GOOF_HOST_BTN_GUIDE,
  GOOF_HOST_BTN_START,          /* Start / Menu / Options / Plus           */
  GOOF_HOST_BTN_LEFT_STICK,
  GOOF_HOST_BTN_RIGHT_STICK,
  GOOF_HOST_BTN_LEFT_SHOULDER,
  GOOF_HOST_BTN_RIGHT_SHOULDER,
  GOOF_HOST_BTN_DPAD_UP,
  GOOF_HOST_BTN_DPAD_DOWN,
  GOOF_HOST_BTN_DPAD_LEFT,
  GOOF_HOST_BTN_DPAD_RIGHT,
  GOOF_HOST_BTN_COUNT
} GoofHostButton;

/* Same numbering as SDL_GameControllerAxis (static-asserted in sdl_pads.c). */
typedef enum {
  GOOF_HOST_AXIS_LEFT_X = 0,
  GOOF_HOST_AXIS_LEFT_Y,
  GOOF_HOST_AXIS_RIGHT_X,
  GOOF_HOST_AXIS_RIGHT_Y,
  GOOF_HOST_AXIS_TRIGGER_LEFT,
  GOOF_HOST_AXIS_TRIGGER_RIGHT,
  GOOF_HOST_AXIS_COUNT
} GoofHostAxis;

/* One pad's physical state, read once per presentation tick. */
typedef struct {
  uint32_t buttons;                       /* bit n = GoofHostButton n held */
  int16_t axes[GOOF_HOST_AXIS_COUNT];     /* SDL range, -32768..32767      */
} GoofHostPadState;

/* Left stick as a digital D-pad: |axis| must EXCEED this (strictly).  The
 * value and the strict comparison are the pre-E1 frontend's. */
enum { GOOF_HOST_STICK_THRESHOLD = 16384 };

/* Keyboard keys by USB HID usage ID (keyboard page 0x07).  SDL_Scancode is
 * defined as this same numbering; main_sdl.c static-asserts every key used. */
typedef enum {
  GOOF_HOST_KEY_A = 4, GOOF_HOST_KEY_B = 5, GOOF_HOST_KEY_C = 6,
  GOOF_HOST_KEY_H = 11, GOOF_HOST_KEY_I = 12, GOOF_HOST_KEY_J = 13,
  GOOF_HOST_KEY_K = 14, GOOF_HOST_KEY_L = 15, GOOF_HOST_KEY_M = 16,
  GOOF_HOST_KEY_N = 17, GOOF_HOST_KEY_O = 18, GOOF_HOST_KEY_S = 22,
  GOOF_HOST_KEY_U = 24, GOOF_HOST_KEY_V = 25, GOOF_HOST_KEY_X = 27,
  GOOF_HOST_KEY_Z = 29,
  GOOF_HOST_KEY_RETURN = 40,
  GOOF_HOST_KEY_RIGHT = 79, GOOF_HOST_KEY_LEFT = 80,
  GOOF_HOST_KEY_DOWN = 81, GOOF_HOST_KEY_UP = 82,
  GOOF_HOST_KEY_KP_ENTER = 88,
  GOOF_HOST_KEY_RCTRL = 228, GOOF_HOST_KEY_RSHIFT = 229,
  GOOF_HOST_KEY_RALT = 230
} GoofHostKey;

enum { GOOF_HOST_PLAYERS = 2 };

/* Everything the host read this tick, for both players.  `keys` is a level
 * array indexed by GoofHostKey (NULL = no keyboard state); pad[k] is used
 * only when pad_present[k]. */
typedef struct {
  const uint8_t *keys;
  size_t key_count;
  bool pad_present[GOOF_HOST_PLAYERS];
  GoofHostPadState pad[GOOF_HOST_PLAYERS];
} GoofHostInputFrame;

/* Default POSITION-TRUE pad mapping (decision D4): bottom -> SNES B,
 * right -> A, left -> Y, top -> X; shoulders -> L/R; D-pad and left stick ->
 * directions; Start -> Start; Back -> Select.  Returns an unnormalised mask. */
uint16_t goof_host_pad_default_mask(const GoofHostPadState *pad);

/* Default keyboard mapping for player 0 (P1) or 1 (P2); the pre-E1 layout
 * unchanged.  Keys at or beyond key_count read as released. */
uint16_t goof_host_keyboard_default_mask(int player, const uint8_t *keys,
                                         size_t key_count);

/* P1 = keyboard P1 | pad in slot P1; P2 = keyboard P2 | pad in slot P2.
 * The two players never read each other's devices.  The result is NOT
 * normalised: the caller runs goof_input_normalize, as before E1. */
GoofInputSample goof_host_default_sample(const GoofHostInputFrame *frame);

#endif
