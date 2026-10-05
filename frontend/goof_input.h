#ifndef GOOF_INPUT_H
#define GOOF_INPUT_H

/* Goof Troop Recomp -- the deterministic guest input model.  GOOF_INPUT_V1.
 *
 * THIS MODULE IS SDL-FREE, PLATFORM-FREE AND HOST-CLOCK-FREE, for the same
 * reason goof_app.c is (gate V11): it is the seam at which HOST state becomes
 * GUEST-VISIBLE state, and nothing downstream of it may learn that a keyboard,
 * a window or a wall clock exists.  What crosses this seam is a pair of 12-bit
 * button masks and nothing else -- no key code, no SDL event, no timestamp.
 *
 * DIRECTION, normative:
 *
 *   host keyboard / gamepad            (main_sdl.c, host state)
 *     -> GoofInputSample                (this header, host->guest currency)
 *       -> FramePlan.joy1/joy2          (one immutable sample per epoch)
 *         -> Snes.input{1,2}_currentState  (latched at every hardware event)
 *           -> $4218/$4219/$421A/$421B     (engine, unchanged)
 *             -> guest
 *
 * The guest is never written to directly, and no host event timing ever
 * reaches the guest master clock.  See goof_p2_input_implementation.md.
 *
 * --- BIT LAYOUT (normative) ----------------------------------------------
 *
 * These bits are the engine's OWN `Snes.input1_currentState` layout, not a
 * frontend invention and not the SNES wire order.  snes.c derives the wire
 * order from it with SwapInputBits() + the $4218..$421B byte split, so this
 * is the one layout a host mask may be written in:
 *
 *   bit  0  B        bit  4  Up        bit  8  A
 *   bit  1  Y        bit  5  Down      bit  9  X
 *   bit  2  Select   bit  6  Left      bit 10  L
 *   bit  3  Start    bit  7  Right     bit 11  R
 *
 * Bits 12..15 MUST be zero.  They are not padding: SwapInputBits maps them
 * onto $4218 bits 3..0, which is the controller-type signature, and Goof
 * Troop's own NMI input routine at $00:8318 discards the WHOLE pad when that
 * nibble is nonzero (`AND #$000F / BEQ / LDX #$0000`).  A 16-bit host mask is
 * therefore masked to 12 bits before it can reach the guest.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Canonical SNES button bits in the engine's input{1,2}_currentState order. */
enum {
  GOOF_BTN_B      = 1u << 0,
  GOOF_BTN_Y      = 1u << 1,
  GOOF_BTN_SELECT = 1u << 2,
  GOOF_BTN_START  = 1u << 3,
  GOOF_BTN_UP     = 1u << 4,
  GOOF_BTN_DOWN   = 1u << 5,
  GOOF_BTN_LEFT   = 1u << 6,
  GOOF_BTN_RIGHT  = 1u << 7,
  GOOF_BTN_A      = 1u << 8,
  GOOF_BTN_X      = 1u << 9,
  GOOF_BTN_L      = 1u << 10,
  GOOF_BTN_R      = 1u << 11,
  GOOF_BTN_MASK   = 0x0fffu,          /* the only bits a pad may carry      */
  GOOF_BTN_VERTICAL   = GOOF_BTN_UP   | GOOF_BTN_DOWN,
  GOOF_BTN_HORIZONTAL = GOOF_BTN_LEFT | GOOF_BTN_RIGHT
};

/* One guest-visible input sample: the held state of both pads.
 *
 * `p1`/`p2` are LEVEL state (held), never edges and never host key-repeat
 * pulses.  Goof Troop derives its own press edges from two consecutive
 * samples in its NMI handler ($00:833D `EOR $46 / AND $44`), so an edge
 * invented here would be a second, contradicting edge detector. */
typedef struct {
  uint16_t p1;
  uint16_t p2;
} GoofInputSample;

#define GOOF_INPUT_NEUTRAL ((GoofInputSample){0, 0})

/* Masks to 12 bits and cancels opposing directions on both pads.
 *
 * Opposing-direction cancellation is ENGINE PRECEDENT, not a gameplay filter
 * invented here: snesrecomp's own host entry point does exactly this before
 * writing input1_currentState (runner/src/common_rtl.c:453 --
 * `if ((inputs & 0x30) == 0x30) inputs ^= 0x30;` on the same bit layout).
 * Applying it here, in the SDL-free core, is what makes the live path and the
 * scripted path share one definition of a legal pad state.
 *
 * Idempotent.  Safe with `s == NULL` (no-op). */
void goof_input_normalize(GoofInputSample *s);

bool goof_input_is_neutral(const GoofInputSample *s);

/* "Start+Right", or "-" when neutral.  `out` must hold GOOF_INPUT_TEXT_MAX. */
enum { GOOF_INPUT_TEXT_MAX = 64 };
void goof_input_mask_text(uint16_t mask, char *out, size_t out_size);

/* Parses one button spec: "-" / "0" / "none" => 0, "0x1a8" => hex literal,
 * or '+'-joined button names ("Start", "Up", "b", ...), case-insensitive.
 * Returns false and leaves *out untouched on any unknown token. */
bool goof_input_parse_mask(const char *text, uint16_t *out);

/* --- Scripted deterministic input ---------------------------------------
 *
 * A script is a LEVEL timeline over logical epochs, which is the same shape
 * as the thing it drives: each entry states the held state from its epoch
 * onwards, until the next entry.  Text format, one entry per line:
 *
 *     # comment; blank lines ignored
 *     <epoch> <p1-spec> [<p2-spec>]
 *
 * e.g.
 *
 *     520  Start   -      # press Start on the title screen
 *     523  -       -      # release it
 *     700  Right   -      # hold Right
 *
 * Epochs must be strictly increasing and >= 1.  Before the first entry the
 * sample is neutral, so a script is exactly as deterministic as the epoch
 * numbering, which is guest state.  No host timestamp appears anywhere: an
 * SDL event time is host presentation metadata and is never input identity.
 *
 * This is the SAME currency the live SDL path produces, and it is consumed
 * through the SAME goof_app_step parameter -- there is no test-only path
 * into guest RAM. */
typedef struct {
  uint64_t epoch;
  GoofInputSample sample;
} GoofInputScriptEntry;

typedef struct {
  GoofInputScriptEntry *entries;
  size_t count;
  uint64_t digest;          /* FNV-1a 64 over the parsed entries           */
} GoofInputScript;

/* Loads and validates `path`.  On failure returns false, writes a
 * human-readable reason into `err` (may be NULL) and leaves *script zeroed.
 * On success the caller owns *script and must goof_input_script_free it. */
bool goof_input_script_load(const char *path, GoofInputScript *script,
                            char *err, size_t err_size);

/* The held state for `epoch` (1-based): the last entry at or before it, or
 * neutral when the script is empty / NULL or the epoch precedes entry 0. */
GoofInputSample goof_input_script_sample(const GoofInputScript *script,
                                         uint64_t epoch);

void goof_input_script_free(GoofInputScript *script);

/* --- Guest-visible input digest -----------------------------------------
 *
 * GOOF_INPUT_DIGEST_V1.  One update per guest-visible LATCH, folding in the
 * latch ordinal, the guest master time of the latch and both masks.  Guest
 * time is included deliberately: it is what makes the digest prove that
 * 30/60/120 Hz presentation does not move WHEN the guest sees input, not
 * merely that it sees the same bits. */
typedef struct {
  uint64_t hash;
  uint64_t latches;
} GoofInputDigest;

void goof_input_digest_reset(GoofInputDigest *d);
void goof_input_digest_update(GoofInputDigest *d, uint64_t guest_master_cycles,
                              uint16_t p1, uint16_t p2);

#endif
