#ifndef GOOF_BINDINGS_H
#define GOOF_BINDINGS_H

/* Goof Troop Recomp -- configurable per-player bindings.
 * GOOF_ENHANCEMENTS_E2_BUTTON_REMAPPING_CONFIG.
 *
 * SDL-FREE and guest-free.  This is the policy E2 inserts between the E1
 * normalised host state and the guest seam:
 *
 *   GoofHostInputFrame (goof_host_input.h: keys by USB HID usage, pad buttons
 *                       by physical position, pad in slot P1/P2)
 *     -> GoofBindings   (this header: per player, per SNES action, a short
 *                        list of keyboard keys and a short list of pad inputs)
 *       -> GoofInputSample (goof_input.h, unchanged guest currency)
 *
 * A player's mask is the OR of every bound physical input that is held, so
 * keyboard and pad (and several keys) may drive the same SNES action.  The
 * defaults reproduce the E1 tables exactly (goof_bindings_test M10 proves
 * goof_bindings_sample(defaults) == goof_host_default_sample over the whole
 * input space).  Nothing here is linked into goof_app_headless or any gate.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "goof_input.h"
#include "host/goof_host_input.h"

/* The 12 SNES actions, in display / serialisation order. */
typedef enum {
  GOOF_ACT_UP = 0, GOOF_ACT_DOWN, GOOF_ACT_LEFT, GOOF_ACT_RIGHT,
  GOOF_ACT_B, GOOF_ACT_A, GOOF_ACT_Y, GOOF_ACT_X,
  GOOF_ACT_L, GOOF_ACT_R, GOOF_ACT_START, GOOF_ACT_SELECT,
  GOOF_ACT_COUNT
} GoofSnesAction;

/* Bindable pad inputs.  0..14 are GoofHostButton (by position); the four
 * left-stick directions keep E1's digital stick (|axis| > 16384, strict). */
typedef enum {
  GOOF_PAD_LSTICK_UP = GOOF_HOST_BTN_COUNT,
  GOOF_PAD_LSTICK_DOWN,
  GOOF_PAD_LSTICK_LEFT,
  GOOF_PAD_LSTICK_RIGHT,
  GOOF_PAD_INPUT_COUNT
} GoofPadInputExtra;

typedef enum { GOOF_BIND_KEY = 0, GOOF_BIND_PAD = 1, GOOF_BIND_KINDS } GoofBindKind;

/* Keyboard codes are USB HID usages (== SDL_Scancode), 1..511. */
enum { GOOF_KEY_CODE_LIMIT = 512 };
enum { GOOF_BIND_MAX_PER_KIND = 4 };

typedef struct {
  uint8_t count;
  uint16_t code[GOOF_BIND_MAX_PER_KIND];
} GoofBindList;

typedef struct {
  GoofBindList list[GOOF_BIND_KINDS][GOOF_ACT_COUNT];
} GoofPlayerBindings;

typedef struct {
  GoofPlayerBindings player[GOOF_HOST_PLAYERS];
} GoofBindings;

/* ---- names ------------------------------------------------------------ */
uint16_t goof_action_bit(GoofSnesAction a);           /* GOOF_BTN_*        */
const char *goof_action_token(GoofSnesAction a);      /* "up", "b", ...    */
const char *goof_action_label(GoofSnesAction a);      /* "UP", "B", ...    */
bool goof_action_from_token(const char *s, GoofSnesAction *out);

/* Stable serialisation tokens and short display names.  Keyboard tokens are
 * names of the physical key POSITION on a US layout ("Z", "RETURN",
 * "KP_ENTER"); a usage without a name is "HID_<n>".  Pad tokens name the
 * normalised control ("bottom", "left_shoulder", "dpad_up", ...).  Token
 * lookup is case-insensitive; output is always the canonical spelling. */
bool goof_key_token(uint16_t code, char *out, size_t cap);
bool goof_key_from_token(const char *s, uint16_t *out);
bool goof_key_label(uint16_t code, char *out, size_t cap);
const char *goof_pad_token(uint16_t code);            /* NULL if invalid   */
bool goof_pad_from_token(const char *s, uint16_t *out);
const char *goof_pad_label(uint16_t code);

/* Host keys that can never be a guest binding: F2 (Settings), ESCAPE (quit /
 * back) and P (pause).  Architecture section 6: host actions and SNES bits
 * never share a physical input. */
bool goof_key_is_reserved(uint16_t code);
bool goof_key_is_valid(uint16_t code);          /* 1..511 and not reserved */
bool goof_pad_is_valid(uint16_t code);

/* ---- editing ---------------------------------------------------------- */
void goof_bindings_defaults(GoofBindings *b);
void goof_player_bindings_defaults(GoofPlayerBindings *p, int player);
bool goof_bindings_equal(const GoofBindings *a, const GoofBindings *b);
/* Adds `code` unless already present or the list is full; true if present
 * afterwards. */
bool goof_bindlist_add(GoofBindList *l, uint16_t code);
void goof_bindlist_set_single(GoofBindList *l, uint16_t code);
void goof_bindlist_clear(GoofBindList *l);
bool goof_bindlist_contains(const GoofBindList *l, uint16_t code);
bool goof_bindlist_remove(GoofBindList *l, uint16_t code);

/* ---- evaluation ------------------------------------------------------- */
/* True when pad input `code` is active in `pad` (buttons, or the left stick
 * beyond the E1 threshold). */
bool goof_pad_input_held(const GoofHostPadState *pad, uint16_t code);
bool goof_key_held(const GoofHostInputFrame *f, uint16_t code);

/* Post-Settings input guard.  Controls that were held when the overlay
 * closed stay invisible to the guest until they are released once, so the
 * Enter/confirm that closed the menu can never arrive as SNES Start.  Pure
 * host state; an empty guard changes nothing. */
typedef struct {
  bool active;
  uint8_t key[GOOF_KEY_CODE_LIMIT / 8];
  uint32_t pad[GOOF_HOST_PLAYERS];              /* bit n = pad input n */
} GoofInputGuard;

void goof_input_guard_arm(GoofInputGuard *g, const GoofHostInputFrame *f);
/* Forgets every guarded control that is no longer held.  Call once per tick
 * before sampling. */
void goof_input_guard_update(GoofInputGuard *g, const GoofHostInputFrame *f);

/* P1 = P1 keyboard bindings | P1 pad bindings on the pad in slot P1; P2 the
 * same on its own slot.  The players never read each other's pad.  `guard`
 * may be NULL.  Not normalised (the caller runs goof_input_normalize, as in
 * E1). */
GoofInputSample goof_bindings_sample(const GoofBindings *b,
                                     const GoofHostInputFrame *f,
                                     const GoofInputGuard *guard);

/* ---- conflicts -------------------------------------------------------- */
/* A conflict is ONE physical input bound to more than one SNES action:
 *   - the same key or pad input on two actions of the same player;
 *   - the same KEY on any action of both players (one keyboard feeds both).
 * The same pad input for P1 and P2 is not a conflict (different pads), and
 * several inputs on one action are not either. */
typedef struct {
  GoofBindKind kind;
  uint16_t code;
  uint8_t player_a, player_b;
  GoofSnesAction action_a, action_b;
} GoofBindConflict;

size_t goof_bindings_conflicts(const GoofBindings *b, GoofBindConflict *out,
                               size_t cap);
bool goof_bindings_action_conflicted(const GoofBindings *b, int player,
                                     GoofBindKind kind, GoofSnesAction a);

#endif
