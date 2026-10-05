/* goof_recomp -- the permanent interactive video frontend of Goof Troop
 * Recomp.  This and sdl_pads.c (its game-controller adapter, linked only into
 * this executable) are the only translation units in the project that
 * include SDL.
 *
 * What lives here: the window, the renderer, the streaming texture, the SDL
 * event pump, the host presentation clock, pause and quit.  All of it is
 * host state.
 *
 * What does NOT live here: any guest advancement.  The guest is stepped only
 * by goof_app_step, one logical epoch per simulation step.
 *
 * GOOF_ENHANCEMENTS_E4 (refresh-rate independent presentation) separates the
 * three host clocks that used to be one presentation tick:
 *
 *   SIMULATION    host/goof_frame_pacer.c decides WHEN a step is due: at the
 *                 canonical physical-period cadence (357368 master clocks at
 *                 21.477 MHz = 60.0985 Hz), deadline-based and drift-free,
 *                 with a bounded catch-up (at most 4 steps per iteration; a
 *                 debt above 4 periods is dropped, never replayed as a
 *                 burst) and a re-anchor on every pause / Settings resume.
 *   PRESENTATION  this file shows the latest completed framebuffer once per
 *                 NEW guest frame (the display repeats it on its own at any
 *                 refresh rate), or at the UI cadence while the guest is
 *                 held.  VSync may block SDL_RenderPresent; it cannot change
 *                 the guest's cadence, only delay the next due check.
 *   AUDIO         the push-model device below, a consumer that never paces.
 *
 * None of the three can answer "how much guest runs" differently for the same
 * input: every guest-visible quantity is indexed by the guest's own epoch
 * number (rules R1/R3/R4; gate V6).  What E4 amends is R2: a host that is
 * late by less than the resync bound now runs the missed epochs back to back
 * (bounded), because otherwise a present that blocks on a 50 Hz or 60.000 Hz
 * vblank would make the monitor the guest's clock.  See
 * architecture/E4_REFRESH_RATE_INDEPENDENT_PRESENTATION.md.
 *
 * HOST INPUT lives here (GOOF_INPUT_V1), with two SDL-free helpers since
 * GOOF_ENHANCEMENTS_E1: host/goof_host_input.[ch] holds the default key and
 * pad bindings, host/goof_controller.[ch] decides which pad owns which player
 * slot, and sdl_pads.c does the SDL device work for it.  The rule it obeys:
 *
 *   this file owns HOST PHYSICAL STATE -- which keys and pad buttons are
 *   down right now, whether the window has focus, whether we are paused;
 *   goof_app_step owns the GUEST-VISIBLE SAMPLE.
 *
 * Exactly one value crosses between them, once per simulation step: a
 * GoofInputSample, which is a pair of 12-bit button masks in the engine's own
 * bit order.  No key code, no SDL event, no SDL event timestamp and no host
 * clock reading is ever passed down -- an SDL event time is host presentation
 * metadata and can never be guest input identity (rule R3 still holds).
 *
 * Held state is read from SDL_GetKeyboardState (a LEVEL array the event pump
 * maintains from SDL_KEYDOWN / SDL_KEYUP), never accumulated from the events
 * themselves.  That is the mature-port convention -- snesrecomp's own desktop
 * host does the same in runner/src/keybinds.c -- and it is what makes host
 * key-repeat structurally unable to become a phantom button press.
 *
 * P, ESC and the window close button are HOST controls and are deliberately
 * absent from the pad map, so they can never be ambiguous with guest input.
 * Single-threaded: no emulation, render or audio thread is created.
 *
 * GOOF_ENHANCEMENTS_E2: the bindings are host configuration now
 * (host/goof_bindings.[ch], defaults == the E1 tables; host/goof_config.[ch],
 * config.ini schema 1), and F2 opens a host Settings overlay
 * (host/ui/goof_menu.[ch], sdl_settings.c).  F2 is routed before anything
 * else and is never guest input; while the overlay is open the guest is not
 * stepped at all (same state as P), audio output is paused, and every key
 * and pad event belongs to the overlay.  The overlay is drawn on the window
 * after the guest texture, never into the hashed framebuffer.  None of this
 * runs for --headless-compare, --input-script or --no-input.
 *
 * HOST AUDIO PRESENTATION lives here too, and it obeys the same rule as the
 * presentation clock: it is a CONSUMER of what the guest has already
 * produced, and it is never a clock.  SDL_INIT_AUDIO is requested only when
 * audio presentation is enabled, the device is opened in PUSH mode
 * (callback == NULL + SDL_QueueAudio) so that no audio thread exists, and
 * RtlRenderAudio -- which in this engine pin still advances the SPC to cover
 * its own shortfall -- is never called.  Gates PRESENT1..PRESENT6.
 */
#include <SDL.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "goof_app.h"
#include "goof_compare.h"
#include "goof_input.h"
#include "audio_trace.h"
#include "host/goof_bindings.h"
#include "host/goof_config.h"
#include "host/goof_frame_pacer.h"
#include "host/goof_host_input.h"
#include "sdl_pads.h"
#include "sdl_settings.h"
#include "sdl_filter.h"

/* HOST SIMULATION POLICY.  One logical epoch per simulation step.  It is not
 * a claim that one logical epoch equals one physical SNES frame; see
 * interactive_presentation_frontend_design.md section 7.  (Before E4 this was
 * GOOF_EPOCHS_PER_PRESENT: the step and the present were the same tick.) */
enum { GOOF_EPOCHS_PER_STEP = 1 };

enum { GOOF_SCALE_MIN = 1, GOOF_SCALE_MAX = 8 };
enum { GOOF_EXIT_SDL = 2, GOOF_EXIT_ROM = 3, GOOF_EXIT_GUEST_INIT = 4,
       GOOF_EXIT_STEP = 5, GOOF_EXIT_RENDER = 6 };

typedef struct {
  const char *rom_path;
  const char *record_path;
  int scale;
  double present_hz;          /* E4: simulation-clock override; 0 = canonical */
  uint64_t epochs;            /* 0 = unlimited */
  bool debug;
  bool headless_compare;
  bool quiet;
  /* Deterministic validation seams.  They change only what the HOST does;
   * the guest path is byte-for-byte the same with or without them. */
  uint64_t suppress_from, suppress_to;   /* V8: presentation-suppressed range */
  uint64_t pause_at;                     /* V9: epoch after which to pause    */
  double pause_seconds;
  /* Host audio presentation.  All three change only what the HOST does. */
  bool no_audio;                         /* PRESENT1: mute path               */
  bool audio_no_servo;                   /* PRESENT: raw 1:1 push             */
  bool audio_fail_device;                /* PRESENT5: device-failure seam     */
  /* GOOF_AUDIO_TRANSITION_PACING_INVESTIGATION -- DIAGNOSTIC ONLY.
   * Both are pure observers: they read state audio_pump already computes
   * and tee the bytes audio_pump already queued.  No threshold, no policy
   * and no control-flow decision anywhere in this file depends on either. */
  const char *audio_trace_path;          /* per-tick TSV telemetry            */
  const char *audio_capture_path;        /* raw s16le stereo of QUEUED host
                                          * frames, exactly as pushed to SDL */
  const char *audio_native_wav_path;     /* canonical PCM ring, for the A/B  */
  /* GOOF_INPUT_V1.  --input-script replaces the live host pad with a
   * deterministic epoch timeline; --no-gamepad and --no-input are host-side
   * switches that only narrow what may reach the same one seam. */
  const char *input_script_path;
  bool no_gamepad;
  bool no_input;
  bool ignore_focus;
  /* GOOF_AUDIO_PHYSICAL_BOUNDARY_PUMP_V1 -- presentation pacing mode.
   *
   * HOST TIMING ONLY.  It selects how long the host WAITS after an epoch has
   * already finished executing; it changes no guest call, no plan, no input
   * chronology and no epoch count, which is why the guest record is
   * byte-identical in both modes and why R1 is untouched (epochs advanced
   * per presentation tick is still the constant GOOF_EPOCHS_PER_PRESENT).
   * E4: the hold is now the pacer's deadline advance (goof_frame_pacer.c),
   * at the canonical period; `epoch` still means one period per epoch.
   *
   *   epoch     legacy.  One epoch is held for ONE nominal frame interval,
   *             whatever guest time it contained.  A 16-period transition
   *             epoch is therefore presented 16x faster than real time.
   *   physical  one epoch is held for as many nominal frame intervals as it
   *             crossed PHYSICAL frame boundaries, so guest time and wall
   *             time advance together across a transition. */
  bool pace_epoch;                       /* --present-pacing epoch            */
  /* GOOF_ENHANCEMENTS_E2 -- which host config file (host bindings only).
   * Default: the per-user path (XDG / %APPDATA%). */
  const char *config_path;               /* --config PATH                     */
  bool no_config;                        /* --no-config: defaults, never write */
  bool portable;                         /* --portable: config.ini beside exe */
  /* GOOF_ENHANCEMENTS_E3: --scale given explicitly (a session override of
   * [video] window_scale that Save does not persist unless the user changes
   * it), and the fullscreen-refused test seam (host only). */
  bool scale_set;
  bool video_fail_fullscreen;
  /* GOOF_ENHANCEMENTS_E4.  --vsync is a session override of [video] vsync
   * (like --scale).  The other three are DIAGNOSTIC seams that change only
   * what the HOST does: an emulated blocking vblank after every present, a
   * one-off host stall after an epoch, and a per-step / per-present TSV. */
  bool vsync_set, vsync_on;
  bool filter_set;
  GoofFilterMode filter;
  double emulate_refresh_hz;             /* --present-emulate-refresh HZ     */
  uint64_t stall_at;                     /* --host-stall-at EPOCH:MS         */
  double stall_ms;
  const char *pacing_trace_path;         /* --pacing-trace PATH              */
} Options;

/* Defined with the host-input block below; used by usage() here. */
static void input_help(FILE *out);

static void usage(void) {
  fprintf(stderr,
      "usage: goof_recomp ROM [--scale N] [--vsync on|off] [--epochs N]\n"
      "                       [--debug] [--quiet] [--record PATH]\n"
      "                       [--headless-compare]\n"
      "                       [--suppress-present A:B] [--pause-at E:SECONDS]\n"
      "                       [--no-audio] [--audio-no-servo]\n"
      "                       [--audio-fail-device]\n"
      "                       [--input-script PATH] [--no-gamepad]\n"
      "                       [--no-input]\n"
      "                       [--present-pacing epoch|physical]\n"
      "                       [--config PATH | --no-config | --portable]\n"
      "                       [--present-hz F] [--present-emulate-refresh HZ]\n"
      "                       [--host-stall-at E:MS] [--pacing-trace PATH]\n"
      "\n"
      "  --epochs 0 (default) runs until quit.\n"
      "\n"
      "  The game runs at the SNES's own cadence (60.0985 Hz, 357368 master\n"
      "  clocks per frame) whatever the monitor's refresh rate; the display\n"
      "  repeats the latest frame as needed.  No interpolation.\n"
      "  --vsync on|off       wait for the display's vblank when presenting,\n"
      "                       this session (Settings > Video > VSync saves it).\n"
      "                       Never changes game speed.\n"
      "  --filter MODE        nearest|bilinear|scanlines (session only)\n"
      "  --present-hz F       TEST SEAM: run the simulation clock at F Hz\n"
      "                       instead of the canonical rate (the guest record is\n"
      "                       identical at any F; only wall time changes).\n"
      "  --present-emulate-refresh HZ  DIAGNOSTIC: block after every present\n"
      "                       until the next vblank of a synthetic HZ display.\n"
      "  --host-stall-at E:MS DIAGNOSTIC: stall the host MS ms after epoch E.\n"
      "  --pacing-trace PATH  DIAGNOSTIC: per-step / per-present TSV.\n"
      "\n"
      "  --present-pacing physical (default) holds each epoch for as many\n"
      "                       nominal frame intervals as it crossed physical\n"
      "                       frame boundaries, so a scene transition is\n"
      "                       presented in real time and its music is neither\n"
      "                       skipped nor delayed.  `epoch` restores the\n"
      "                       legacy one-interval-per-epoch pacing.  HOST\n"
      "                       TIMING ONLY: the guest record is identical.\n"
      "\n"
      "  --no-audio           no device, no drain: guest advances identically\n"
      "  --audio-no-servo     push native PCM 1:1, no occupancy stretch\n"
      "  --audio-fail-device  simulate an unavailable device (runs muted)\n"
      "  --audio-trace PATH   DIAGNOSTIC: per-presentation-tick audio TSV\n"
      "  --audio-capture PATH DIAGNOSTIC: raw s16le stereo of queued frames\n"
      "  --audio-dump-native PATH  DIAGNOSTIC: canonical native PCM as WAV\n"
      "  --input-script PATH  replay a deterministic epoch input timeline\n"
      "                       instead of the live pad (see goof_input.h)\n"
      "  --no-gamepad         keyboard only; do not open any game controller\n"
      "  --no-input           present neutral input to the guest, always\n"
      "  --ignore-focus       keep sending input even without keyboard focus\n"
      "  --config PATH        host config file (bindings, video, audio) instead of the\n"
      "                       per-user one ($XDG_CONFIG_HOME or ~/.config /\n"
      "                       %%APPDATA%%, GoofTroopRecomp/config.ini)\n"
      "  --no-config          built-in default bindings; nothing is read or\n"
      "                       written (Settings changes last this session)\n"
      "  --portable           config.ini next to the executable\n"
      "  --scale N            window scale for this session (overrides the\n"
      "                       Settings > Video value without saving it)\n");
  input_help(stderr);
  exit(2);
}

static double parse_positive(const char *text) {
  char *end;
  double v = strtod(text, &end);
  if (*text == '\0' || *end != '\0' || !(v > 0.0)) usage();
  return v;
}

static void parse_pair(const char *text, uint64_t *a, double *b, bool b_is_int) {
  char *end;
  unsigned long long first = strtoull(text, &end, 10);
  if (*end != ':') usage();
  double second = parse_positive(end + 1);
  if (b_is_int && second != (double)(unsigned long long)second) usage();
  *a = (uint64_t)first;
  *b = second;
}

static Options parse_options(int argc, char **argv) {
  Options o = {.scale = 3, .present_hz = 0.0, .epochs = 0};
  if (argc < 2 || argv[1][0] == '-') usage();
  o.rom_path = argv[1];
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--debug") == 0) { o.debug = true; continue; }
    if (strcmp(argv[i], "--quiet") == 0) { o.quiet = true; continue; }
    if (strcmp(argv[i], "--audio-pins-advisory") == 0) {
      goof_compare_set_audio_pins_advisory(true); continue;
    }
    if (strcmp(argv[i], "--discover") == 0) {
      goof_compare_set_discover(true); continue;
    }
    if (strcmp(argv[i], "--headless-compare") == 0) {
      o.headless_compare = true; continue;
    }
    if (strcmp(argv[i], "--no-audio") == 0) { o.no_audio = true; continue; }
    if (strcmp(argv[i], "--audio-no-servo") == 0) {
      o.audio_no_servo = true; continue;
    }
    if (strcmp(argv[i], "--audio-fail-device") == 0) {
      o.audio_fail_device = true; continue;
    }
    if (strcmp(argv[i], "--no-gamepad") == 0) { o.no_gamepad = true; continue; }
    if (strcmp(argv[i], "--no-input") == 0) { o.no_input = true; continue; }
    if (strcmp(argv[i], "--ignore-focus") == 0) {
      o.ignore_focus = true; continue;
    }
    if (strcmp(argv[i], "--no-config") == 0) { o.no_config = true; continue; }
    if (strcmp(argv[i], "--portable") == 0) { o.portable = true; continue; }
    if (strcmp(argv[i], "--video-fail-fullscreen") == 0) {
      o.video_fail_fullscreen = true; continue;
    }
    if (i + 1 >= argc) usage();
    if (strcmp(argv[i], "--scale") == 0) {
      double v = parse_positive(argv[++i]);
      if (v < GOOF_SCALE_MIN || v > GOOF_SCALE_MAX ||
          v != (double)(int)v) usage();
      o.scale = (int)v;
      o.scale_set = true;
    } else if (strcmp(argv[i], "--present-hz") == 0) {
      o.present_hz = parse_positive(argv[++i]);
    } else if (strcmp(argv[i], "--filter") == 0) {
      if (!goof_filter_from_token(argv[++i], &o.filter)) usage();
      o.filter_set = true;
    } else if (strcmp(argv[i], "--vsync") == 0) {
      const char *v = argv[++i];
      if (strcmp(v, "on") == 0) o.vsync_on = true;
      else if (strcmp(v, "off") == 0) o.vsync_on = false;
      else usage();
      o.vsync_set = true;
    } else if (strcmp(argv[i], "--present-emulate-refresh") == 0) {
      o.emulate_refresh_hz = parse_positive(argv[++i]);
    } else if (strcmp(argv[i], "--host-stall-at") == 0) {
      parse_pair(argv[++i], &o.stall_at, &o.stall_ms, false);
    } else if (strcmp(argv[i], "--pacing-trace") == 0) {
      o.pacing_trace_path = argv[++i];
    } else if (strcmp(argv[i], "--epochs") == 0) {
      char *end;
      unsigned long long v = strtoull(argv[++i], &end, 10);
      if (*end != '\0') usage();
      o.epochs = (uint64_t)v;
    } else if (strcmp(argv[i], "--record") == 0) {
      o.record_path = argv[++i];
    } else if (strcmp(argv[i], "--config") == 0) {
      o.config_path = argv[++i];
    } else if (strcmp(argv[i], "--input-script") == 0) {
      o.input_script_path = argv[++i];
    } else if (strcmp(argv[i], "--suppress-present") == 0) {
      double to;
      parse_pair(argv[++i], &o.suppress_from, &to, true);
      o.suppress_to = (uint64_t)to;
    } else if (strcmp(argv[i], "--pause-at") == 0) {
      parse_pair(argv[++i], &o.pause_at, &o.pause_seconds, false);
    } else if (strcmp(argv[i], "--audio-trace") == 0) {
      o.audio_trace_path = argv[++i];
    } else if (strcmp(argv[i], "--audio-capture") == 0) {
      o.audio_capture_path = argv[++i];
    } else if (strcmp(argv[i], "--audio-dump-native") == 0) {
      o.audio_native_wav_path = argv[++i];
    } else if (strcmp(argv[i], "--present-pacing") == 0) {
      const char *mode = argv[++i];
      if (!mode) usage();
      else if (strcmp(mode, "epoch") == 0) o.pace_epoch = true;
      else if (strcmp(mode, "physical") == 0) o.pace_epoch = false;
      else usage();
    } else {
      usage();
    }
  }
  /* One config source at most: the three choose the same thing. */
  if ((o.config_path != NULL) + o.no_config + o.portable > 1) usage();
  return o;
}

/* --------------------------------------------------------------------- */
/* HOST INPUT.  GOOF_INPUT_V1, host half.                                 */
/*                                                                        */
/* Everything in this block is HOST PHYSICAL STATE.  Its only output is a  */
/* GoofInputSample, and that sample is the only thing handed to            */
/* goof_app_step -- see the file header.                                   */
/* --------------------------------------------------------------------- */

/* KEYBOARD AND GAMEPAD BINDINGS live in host/goof_host_input.c since
 * GOOF_ENHANCEMENTS_E1, unchanged: P1 keyboard = arrows, Enter/KP-Enter
 * Start, RShift Select, Z B, A Y, X A, S X, C L, V R; P2 keyboard = I/K/J/L,
 * RCtrl Start, RAlt Select, N B, B Y, M A, H X, U L, O R; pads position-true.
 * That table is written in USB HID usage IDs, which is what SDL_Scancode
 * values are -- asserted here, key by key, so it cannot drift from SDL.
 * P and ESC are host controls and appear in no binding, so a host control can
 * never also be a guest button. */
_Static_assert((int)SDL_SCANCODE_A == (int)GOOF_HOST_KEY_A, "HID usage");
_Static_assert((int)SDL_SCANCODE_B == (int)GOOF_HOST_KEY_B, "HID usage");
_Static_assert((int)SDL_SCANCODE_C == (int)GOOF_HOST_KEY_C, "HID usage");
_Static_assert((int)SDL_SCANCODE_H == (int)GOOF_HOST_KEY_H, "HID usage");
_Static_assert((int)SDL_SCANCODE_I == (int)GOOF_HOST_KEY_I, "HID usage");
_Static_assert((int)SDL_SCANCODE_J == (int)GOOF_HOST_KEY_J, "HID usage");
_Static_assert((int)SDL_SCANCODE_K == (int)GOOF_HOST_KEY_K, "HID usage");
_Static_assert((int)SDL_SCANCODE_L == (int)GOOF_HOST_KEY_L, "HID usage");
_Static_assert((int)SDL_SCANCODE_M == (int)GOOF_HOST_KEY_M, "HID usage");
_Static_assert((int)SDL_SCANCODE_N == (int)GOOF_HOST_KEY_N, "HID usage");
_Static_assert((int)SDL_SCANCODE_O == (int)GOOF_HOST_KEY_O, "HID usage");
_Static_assert((int)SDL_SCANCODE_S == (int)GOOF_HOST_KEY_S, "HID usage");
_Static_assert((int)SDL_SCANCODE_U == (int)GOOF_HOST_KEY_U, "HID usage");
_Static_assert((int)SDL_SCANCODE_V == (int)GOOF_HOST_KEY_V, "HID usage");
_Static_assert((int)SDL_SCANCODE_X == (int)GOOF_HOST_KEY_X, "HID usage");
_Static_assert((int)SDL_SCANCODE_Z == (int)GOOF_HOST_KEY_Z, "HID usage");
_Static_assert((int)SDL_SCANCODE_RETURN == (int)GOOF_HOST_KEY_RETURN, "HID usage");
_Static_assert((int)SDL_SCANCODE_RIGHT == (int)GOOF_HOST_KEY_RIGHT, "HID usage");
_Static_assert((int)SDL_SCANCODE_LEFT == (int)GOOF_HOST_KEY_LEFT, "HID usage");
_Static_assert((int)SDL_SCANCODE_DOWN == (int)GOOF_HOST_KEY_DOWN, "HID usage");
_Static_assert((int)SDL_SCANCODE_UP == (int)GOOF_HOST_KEY_UP, "HID usage");
_Static_assert((int)SDL_SCANCODE_KP_ENTER == (int)GOOF_HOST_KEY_KP_ENTER, "HID usage");
_Static_assert((int)SDL_SCANCODE_RCTRL == (int)GOOF_HOST_KEY_RCTRL, "HID usage");
_Static_assert((int)SDL_SCANCODE_RSHIFT == (int)GOOF_HOST_KEY_RSHIFT, "HID usage");
_Static_assert((int)SDL_SCANCODE_RALT == (int)GOOF_HOST_KEY_RALT, "HID usage");

typedef struct {
  bool enabled;                 /* false => always present neutral          */
  bool use_gamepad;
  bool ignore_focus;            /* --ignore-focus: never gate on focus      */
  /* LIVE state, re-read from SDL_GetWindowFlags every tick -- NOT an event
   * history.  A missed or never-delivered FOCUS_GAINED under an unusual
   * window manager would otherwise leave the pad permanently dead, which is
   * a far worse failure than a stuck button.  The window flag is what SDL
   * itself believes right now, so it self-heals. */
  bool focused;
  /* Player-slot ownership, keyed by SDL instance id (sdl_pads.h): one
   * physical pad drives at most one player. */
  GoofSdlPads pads;
  /* GOOF_ENHANCEMENTS_E2: the live per-player bindings (defaults == E1
   * tables; replaced by config.ini and by Save in the Settings overlay), and
   * the guard that hides controls still held when the overlay closed. */
  GoofBindings bindings;
  GoofInputGuard guard;
  const GoofInputScript *script;   /* non-NULL => scripted, pad ignored     */
  /* Host-side observation only; never fed back into the sample. */
  uint64_t focus_clears;
  uint64_t nonneutral_epochs;
} HostInput;

static void input_help(FILE *out) {
  fprintf(out,
      "\nControls\n"
      "  Player 1 keyboard   Arrows = D-pad   Enter = Start   RShift = Select\n"
      "                      Z = B   A = Y    X = A   S = X    C = L   V = R\n"
      "  Player 2 keyboard   I/K/J/L = D-pad  RCtrl = Start   RAlt = Select\n"
      "                      N = B   B = Y    M = A   H = X    U = L   O = R\n"
      "  Gamepad             D-pad or left stick; face buttons by POSITION:\n"
      "                      bottom = B, right = A, left = Y, top = X;\n"
      "                      L/R shoulders; Start; Back/Select/View = Select.\n"
      "                      First pad -> Player 1, second -> Player 2; a pad\n"
      "                      keeps its player until unplugged, and a new pad\n"
      "                      takes the free player (see HOST_GAMEPAD lines).\n"
      "  Host only           P = pause (guest and audio stop)   ESC = quit\n"
      "                      F2 = Settings (remap keys / pad; game pauses)\n"
      "                      P, ESC and F2 are never sent to the game.\n"
      "  The layouts above are the defaults; Settings > Input can remap\n"
      "  both players, saved to config.ini.\n");
}

/* THE HOST -> GUEST CONVERSION.  Called once per simulation step (never per
 * present -- E4), for the epoch that is ABOUT to run, immediately before
 * goof_app_step.
 *
 * `next_epoch` is guest state (the count of completed epochs, plus one), not
 * a host tick count, which is why a scripted replay is identical at 30 Hz and
 * at 120 Hz: the script is indexed by the guest's own epoch numbering.
 *
 * Focus loss presents NEUTRAL rather than the stale held mask.  The keys are
 * not "released" -- SDL's state array may well still report them down while
 * another window owns the keyboard -- so a pad held at the moment focus went
 * away must not keep walking the character.  On focus return the live state is
 * read again on the very next tick, so nothing is sticky in either direction.
 *
 * Pause does not reach here at all: while paused no epoch runs, so no latch
 * happens.  On resume the next tick reads the CURRENT physical host state,
 * never the state from before the pause. */
/* The physical state right now: SDL's key level array and the pad in each
 * player slot. */
static void input_read_frame(HostInput *in, GoofHostInputFrame *frame) {
  memset(frame, 0, sizeof *frame);
  int key_count = 0;
  frame->keys = SDL_GetKeyboardState(&key_count);
  frame->key_count = frame->keys && key_count > 0 ? (size_t)key_count : 0;
  for (int k = 0; k < GOOF_HOST_PLAYERS; k++)
    frame->pad_present[k] = goof_sdl_pads_read(&in->pads, k, &frame->pad[k]);
}

static GoofInputSample input_sample(HostInput *in, uint64_t next_epoch) {
  GoofInputSample s = {0, 0};
  if (!in->enabled) return s;
  if (in->script) {
    s = goof_input_script_sample(in->script, next_epoch);
  } else if (in->focused || in->ignore_focus) {
    /* P1 = P1 keyboard bindings | P1 pad bindings on the pad in slot P1; P2
     * the same on its own slot (goof_bindings_sample).  With the default
     * bindings this is exactly E1's goof_host_default_sample. */
    GoofHostInputFrame frame;
    input_read_frame(in, &frame);
    goof_input_guard_update(&in->guard, &frame);
    s = goof_bindings_sample(&in->bindings, &frame, &in->guard);
  } else {
    in->focus_clears++;
  }
  /* Normalised again inside goof_app_step; doing it here too keeps the host
   * observation counter below counting the same masks the guest will see. */
  goof_input_normalize(&s);
  if (!goof_input_is_neutral(&s)) in->nonneutral_epochs++;
  return s;
}

/* --------------------------------------------------------------------- */
/* HOST WAITING (GOOF_ENHANCEMENTS_E4).  Nothing below this line is ever   */
/* read by, or passed to, anything under goof_app_step (rule R3).  WHEN a  */
/* guest step is due is decided by host/goof_frame_pacer.c; this block     */
/* only sleeps until a tick the pacer (or the held UI) asked for.          */
/* --------------------------------------------------------------------- */

static uint64_t host_now(void) { return SDL_GetPerformanceCounter(); }

/* Waits until `target` (performance-counter ticks) or until a host event is
 * pending, whichever comes first, and returns true in the second case.
 * Coarse part: SDL_WaitEventTimeout, which returns as soon as an event
 * arrives, so quit, F2 or a resize never wait for the next guest deadline.
 * Fine part (the last <= 1.5 ms): a short spin, because SDL_Delay oversleeps
 * by 0.05-0.8 ms on Linux (evidence/e4_refresh_rate/clock_sources.txt) --
 * the same sleep-then-spin split the pre-E4 clock used.  Guest cadence does
 * not depend on this precision: deadlines are absolute, so a late wake-up is
 * never carried into the next deadline. */
static bool host_wait_until(uint64_t target, uint64_t freq) {
  for (;;) {
    uint64_t now = host_now();
    if (now >= target) return false;
    double remaining_ms = (double)(target - now) * 1000.0 / (double)freq;
    if (remaining_ms > 1.5) {
      int ms = (int)(remaining_ms - 1.0);
      if (SDL_WaitEventTimeout(NULL, ms < 1 ? 1 : ms)) return true;
    }
    /* else: short busy wait, one iteration at a time */
  }
}

/* Blocking sleep with no event wake-up: DIAGNOSTIC seams only (emulated
 * vblank, injected host stall), which must block exactly like the thing
 * they emulate. */
static void host_sleep_until(uint64_t target, uint64_t freq) {
  for (;;) {
    uint64_t now = host_now();
    if (now >= target) return;
    double remaining_ms = (double)(target - now) * 1000.0 / (double)freq;
    if (remaining_ms > 1.5) SDL_Delay((Uint32)(remaining_ms - 1.0));
  }
}

/* Presentation-side observation.  HOST ONLY and read by no policy branch:
 * the pacer never sees any of it.  Printed as GOOF_PACING_PRESENT. */
enum { GOOF_REPEAT_BINS = 9 };   /* 0..7 vblanks, 8 = "8 or more"          */
typedef struct {
  uint64_t presents, frame_presents, ui_presents, hidden_skipped;
  uint64_t frames_superseded;    /* guest frames replaced before a present  */
  uint64_t last_frame_present;   /* end of the previous NEW-frame present   */
  GoofSeries frame_interval;     /* between NEW-frame presents (s)          */
  GoofSeries present_call;       /* SDL_RenderPresent duration (s)          */
  /* --present-emulate-refresh: a synthetic display.  After each present the
   * host blocks until the next vblank of the grid, like a vsynced present;
   * the vblank index of consecutive new-frame presents gives the number of
   * refreshes each guest frame stayed on screen (the repeat pattern). */
  double emulate_hz;
  uint64_t emulate_origin;
  uint64_t last_vblank;
  bool have_vblank;
  uint64_t last_frame_periods;   /* physical periods of the frame on screen */
  uint64_t repeat_hist[GOOF_REPEAT_BINS];  /* one-period frames only       */
  uint64_t multi_period_frames;  /* transition frames, held on purpose      */
  FILE *trace;
  uint64_t trace_origin;
} PresentObs;

static uint64_t emulate_vblank_wait(PresentObs *o, uint64_t freq) {
  /* vblank k is at origin + k * freq / hz (computed in double from an
   * integer index, so it does not drift). */
  uint64_t now = host_now();
  double per = (double)freq / o->emulate_hz;
  uint64_t k = (uint64_t)((double)(now - o->emulate_origin) / per) + 1;
  uint64_t at = o->emulate_origin + (uint64_t)((double)k * per);
  host_sleep_until(at, freq);
  return k;
}

/* --------------------------------------------------------------------- */
/* HOST AUDIO PRESENTATION.                                               */
/*                                                                        */
/* Everything in this block is HOST state.  It is downstream of the        */
/* canonical native-PCM digest (taken inside dsp_cycle, before the output  */
/* ring is written) and it is a pure consumer of what the guest has        */
/* already produced.                                                      */
/*                                                                        */
/*   guest master clock -> APU/SPC -> DSP -> native PCM ring              */
/*                      -> goof_app_audio_drain   (game thread)           */
/*                      -> host-only linear stretch                       */
/*                      -> SDL_QueueAudio -> device                       */
/*                                                                        */
/* The arrow never points back.  There is no audio callback, no audio      */
/* thread, and no path by which the device's schedule, the wall clock or   */
/* the queue's occupancy can reach goof_app_step.  A push model            */
/* (SDL_OpenAudioDevice with callback == NULL + SDL_QueueAudio) is what    */
/* makes that structural rather than conventional: the only code that      */
/* touches emulation state still runs on the one thread this process has.  */
/* Precedent: DKC1/DKC2/DKC3, which use the same push model over the same  */
/* 8192-native core ring.  See goof_audio_presentation_implementation.md.  */
/* --------------------------------------------------------------------- */

enum {
  /* Device block.  1024 frames is ~32 ms at the DSP's native rate; the
   * same value DKC2/DKC3 request, for the same reason. */
  GOOF_AUDIO_DEVICE_SAMPLES = 1024,
  /* Staging buffer: the engine's output ring capacity, so a drain can
   * always take everything the guest has produced in one call. */
  GOOF_AUDIO_STAGE_FRAMES = 8192,
  /* Stretch output headroom: at the maximum ratio (+0.5 %) the stretch
   * emits at most ceil(n / 0.995) frames.  256 covers 8192 with margin. */
  GOOF_AUDIO_OUT_FRAMES = GOOF_AUDIO_STAGE_FRAMES + 256,
  /* GOOF_AUDIO_PHYSICAL_BOUNDARY_PUMP_V1 -- host surplus FIFO capacity.
   *
   * Sized from measurement, not from taste.  The largest burst any epoch has
   * ever handed this host is the loader epoch E1: 143 physical periods x 534
   * native frames = 76 362 frames = 2383 ms.  The campaign's worst gameplay
   * transition, E957, is 16 periods = 8544 frames = 267 ms.  Capacity is
   * 131 072 frames -- 4092 ms, a power of two so the ring indexes by mask,
   * 1.72x the worst burst ever observed and 512 KiB of host memory.
   *
   * It is a BOUND, not a target.  Reaching it at all is the catastrophic
   * case (fifo_overflow_dropped), and the acceptance gate requires it to
   * stay 0; the steady state is an empty FIFO. */
  GOOF_AUDIO_FIFO_FRAMES = 131072,
};

/* --------------------------------------------------------------------- */
/* HOST SURPLUS FIFO.                                                      */
/*                                                                        */
/* Pure host storage between the guest's DSP ring and the SDL device       */
/* queue.  It exists because the two have different quanta: the guest      */
/* emits 534 native frames per PHYSICAL period and closes up to 143 of     */
/* them inside one logical epoch, while the device consumes at wall-clock  */
/* real time.  Before this FIFO the surplus had nowhere to go and the      */
/* intake cap deleted it oldest-first, which is what a listener heard as   */
/* the music jumping forward.                                             */
/*                                                                        */
/* ORDER-PRESERVING BY CONSTRUCTION.  Frames leave in the order they       */
/* entered; nothing here drops, reorders, duplicates, time-compresses or   */
/* time-expands.  The single exception is capacity exhaustion, which is    */
/* counted, reported, and required by the gate to never happen.           */
/* --------------------------------------------------------------------- */
typedef struct {
  int16_t *buf;             /* GOOF_AUDIO_FIFO_FRAMES * 2 interleaved     */
  uint32_t head;            /* next frame to pop                          */
  uint32_t count;           /* frames resident                            */
  uint32_t peak;            /* high-water mark over the session           */
  uint64_t pushed, popped;  /* lifetime totals, for the accounting sum    */
  uint64_t overflow_dropped;/* CATASTROPHIC ONLY; gate requires 0         */
  uint64_t overflow_events;
} HostFifo;

static bool fifo_init(HostFifo *f) {
  f->buf = malloc((size_t)GOOF_AUDIO_FIFO_FRAMES * 2 * sizeof(int16_t));
  if (!f->buf) return false;
  f->head = f->count = f->peak = 0;
  f->pushed = f->popped = f->overflow_dropped = f->overflow_events = 0;
  return true;
}

static void fifo_free(HostFifo *f) { free(f->buf); f->buf = NULL; }

/* Appends `n` frames.  If capacity would be exceeded the OLDEST resident
 * frames are released to make room -- the emergency semantics the previous
 * intake cap applied routinely, kept here only as a guard that must never
 * be reached.  Every such frame is counted. */
static void fifo_push(HostFifo *f, const int16_t *src, uint32_t n) {
  if (!f->buf || !n) return;
  if (n > GOOF_AUDIO_FIFO_FRAMES) {           /* cannot happen: n <= 8192 */
    uint32_t excess = n - GOOF_AUDIO_FIFO_FRAMES;
    src += (size_t)excess * 2;
    n = GOOF_AUDIO_FIFO_FRAMES;
    f->overflow_dropped += excess;
    f->overflow_events++;
  }
  if (f->count + n > GOOF_AUDIO_FIFO_FRAMES) {
    uint32_t release = f->count + n - GOOF_AUDIO_FIFO_FRAMES;
    f->head = (f->head + release) % GOOF_AUDIO_FIFO_FRAMES;
    f->count -= release;
    f->overflow_dropped += release;
    f->overflow_events++;
  }
  uint32_t tail = (f->head + f->count) % GOOF_AUDIO_FIFO_FRAMES;
  uint32_t first = GOOF_AUDIO_FIFO_FRAMES - tail;
  if (first > n) first = n;
  memcpy(&f->buf[(size_t)tail * 2], src, (size_t)first * 2 * sizeof(int16_t));
  if (n > first)
    memcpy(f->buf, src + (size_t)first * 2,
           (size_t)(n - first) * 2 * sizeof(int16_t));
  f->count += n;
  f->pushed += n;
  if (f->count > f->peak) f->peak = f->count;
}

/* Removes and copies out the `n` OLDEST frames (fewer if that is all there
 * is).  Returns how many were copied. */
static uint32_t fifo_pop(HostFifo *f, int16_t *dst, uint32_t n) {
  if (!f->buf || !n || !f->count) return 0;
  if (n > f->count) n = f->count;
  uint32_t first = GOOF_AUDIO_FIFO_FRAMES - f->head;
  if (first > n) first = n;
  memcpy(dst, &f->buf[(size_t)f->head * 2],
         (size_t)first * 2 * sizeof(int16_t));
  if (n > first)
    memcpy(dst + (size_t)first * 2, f->buf,
           (size_t)(n - first) * 2 * sizeof(int16_t));
  f->head = (f->head + n) % GOOF_AUDIO_FIFO_FRAMES;
  f->count -= n;
  f->popped += n;
  return n;
}

static void fifo_clear(HostFifo *f) { f->head = f->count = 0; }

/* Queue servo.  Host-only, and deliberately the ecosystem's numbers:
 * DKC2/DKC3 and the engine's own rtl_render_native independently converged
 * on a proportional servo over an EMA of queue occupancy with a +-0.5 %
 * authority limit -- an order of magnitude below the ~25 cents at which
 * pitch drift becomes audible. */
#define GOOF_AUDIO_SERVO_DEVIATION 0.005  /* +-0.5 %, ~8 cents              */
#define GOOF_AUDIO_SERVO_GAIN      4.0    /* saturates at +-12.5 % of target*/
#define GOOF_AUDIO_SERVO_EMA       0.02   /* occupancy smoothing            */

typedef struct {
  bool requested;             /* --no-audio not given                      */
  bool open;                  /* device is open                            */
  bool primed;                /* preroll satisfied; device unpaused        */
  bool servo;                 /* host-only stretch servo enabled           */
  bool force_open_failure;    /* PRESENT5 seam                             */
  SDL_AudioDeviceID dev;
  SDL_AudioSpec want, have;
  uint32_t frame_bytes;       /* channels * sizeof(int16)                  */
  int16_t *stage;             /* native frames drained this tick           */
  int16_t *out;               /* host frames after the stretch             */

  /* GOOF_AUDIO_PHYSICAL_BOUNDARY_PUMP_V1.  Host presentation is now
   * serviced at PHYSICAL boundary granularity from inside goof_app_step,
   * not only when the step returns.  `app` is the handle the boundary hook
   * needs to reach the drain seam; it is a pointer to guest-owning state
   * that this layer only ever OBSERVES through
   * goof_app_audio_available / _drain, both of which are documented not to
   * advance or block the guest. */
  GoofApp *app;
  HostFifo fifo;
  uint64_t boundary_drains;     /* hook invocations that harvested          */
  uint64_t boundary_harvested;  /* native frames harvested at boundaries    */
  uint64_t boundary_queued;     /* host frames queued from inside a step    */
  uint64_t tick_harvested;      /* native frames harvested at tick end      */
  uint64_t resync_dropped;      /* FIFO frames lost to an emergency resync  */
  /* Frames popped from the FIFO that SDL_QueueAudio then refused.  A SUBSET
   * of fifo.popped, so it is reported beside the identity, never added to
   * it -- adding it would double-count. */
  uint64_t service_lost, service_lost_events;
  uint32_t fifo_peak_epoch_max; /* peak FIFO depth, for the summary         */
  double fifo_sum;              /* FIFO occupancy accumulator (per tick)    */
  uint64_t fifo_samples;

  /* Occupancy policy, all in host frames, derived once from `have`. */
  uint32_t target_frames, preroll_frames, low_water, steady_ceiling,
           high_water;

  /* Stretch carry.  Host presentation state: it never enters a digest. */
  double ratio, position, fill_ema;
  int16_t last[2];
  bool stretch_primed;

  /* GOOF_ENHANCEMENTS_E3: host output gain (Settings > Audio volume, mute,
   * mute when unfocused), Q16.  Applied to the HOST copy of the frames,
   * after the stretch and right before SDL_QueueAudio -- downstream of the
   * canonical native-PCM digest (taken at the DSP) and of every FIFO /
   * queue decision, so occupancy, the servo and the guest are the same at
   * any volume.  Mute queues silence; it never stops the device.  Unity
   * (the default) leaves the bytes untouched. */
  int32_t gain_q16;
  uint64_t gain_frames_scaled, gain_frames_silenced;

  /* Diagnostics. */
  uint64_t native_drained, native_trimmed, host_queued;
  uint64_t underflows, resyncs, low_water_events, preroll_waits, ticks;
  uint32_t queue_min, queue_max;
  double queue_sum;
  uint64_t queue_samples;
  double ratio_min, ratio_max;

  /* ---- GOOF_AUDIO_TRANSITION_PACING_INVESTIGATION: OBSERVERS ONLY ----
   * Nothing below is read by any policy branch.  They exist so a tick can
   * be reconstructed after the fact; deleting the whole block changes no
   * host decision and no guest quantity. */
  FILE *trace;                /* per-tick TSV                              */
  FILE *cap;                  /* tee of the bytes handed to SDL_QueueAudio */
  uint64_t t_prev_produced;   /* audio_trace produced at the previous tick */
  uint64_t t_prev_dropped;    /* ... core-ring overflow drops              */
  uint64_t t_prev_dropped_audible;
  uint64_t t_prev_spc;        /* diag.spc_cycles at the previous tick      */
  uint64_t t_prev_avail_after;/* ring occupancy left at the previous tick  */
  uint64_t t_max_periods;     /* max physical periods seen in ONE pump     */
  uint64_t t_retire_events;   /* ticks in which the intake cap retired > 0 */
  uint64_t t_cap_frames;      /* host frames written to the capture file   */
  uint64_t t_prev_bdrains, t_prev_bharvest, t_prev_bqueued;
} HostAudio;

static uint32_t audio_queued_frames(const HostAudio *a) {
  if (!a->open) return 0;
  return (uint32_t)(SDL_GetQueuedAudioSize(a->dev) / a->frame_bytes);
}

/* Opens the device at the DSP's native rate and REQUIRES an exact match.
 * Following DKC2/DKC3, a substituted rate/format/channel count closes the
 * device and reports, rather than silently engaging a conversion path that
 * has no test.  Returns false when there will be no audio; the caller
 * continues muted, and the guest is unaffected either way. */
static bool audio_open(HostAudio *a) {
  a->ratio = 1.0;
  a->ratio_min = a->ratio_max = 1.0;
  a->queue_min = UINT32_MAX;
  if (!a->requested) return false;

  if (a->force_open_failure) {
    printf("AUDIO_DEVICE FAILED reason=forced_open_failure "
           "(PRESENT5 seam) audio=MUTED guest=UNAFFECTED\n");
    return false;
  }
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    printf("AUDIO_DEVICE FAILED reason=init_subsystem sdl_error=\"%s\" "
           "audio=MUTED guest=UNAFFECTED\n", SDL_GetError());
    return false;
  }
  SDL_AudioSpec want = {0};
  want.freq = (int)goof_app_audio_native_rate();
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = GOOF_AUDIO_DEVICE_SAMPLES;
  want.callback = NULL;          /* PUSH MODEL: no audio thread exists. */
  a->want = want;
  printf("AUDIO_DEVICE REQUESTED rate=%d format=S16SYS channels=%d "
         "samples=%d api=SDL_QueueAudio callback=NULL\n",
         want.freq, want.channels, want.samples);

  /* allowed_changes = 0: SDL must deliver the request or fail. */
  a->dev = SDL_OpenAudioDevice(NULL, 0, &want, &a->have, 0);
  if (a->dev == 0) {
    printf("AUDIO_DEVICE FAILED reason=open sdl_error=\"%s\" "
           "audio=MUTED guest=UNAFFECTED\n", SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return false;
  }
  printf("AUDIO_DEVICE OBTAINED rate=%d format=%04X channels=%d samples=%d "
         "size=%u\n", a->have.freq, (unsigned)a->have.format,
         a->have.channels, a->have.samples, (unsigned)a->have.size);
  if (a->have.freq != want.freq || a->have.format != want.format ||
      a->have.channels != want.channels) {
    printf("AUDIO_DEVICE REJECTED reason=spec_substituted "
           "(exact native match required for the MVP; no host conversion "
           "path is implemented) audio=MUTED guest=UNAFFECTED\n");
    SDL_CloseAudioDevice(a->dev);
    a->dev = 0;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return false;
  }

  a->frame_bytes = (uint32_t)a->have.channels * sizeof(int16_t);
  a->stage = malloc((size_t)GOOF_AUDIO_STAGE_FRAMES * 2 * sizeof(int16_t));
  a->out = malloc((size_t)GOOF_AUDIO_OUT_FRAMES * 2 * sizeof(int16_t));
  if (!fifo_init(&a->fifo)) { free(a->stage); free(a->out);
                              a->stage = a->out = NULL; }
  if (!a->stage || !a->out || !a->fifo.buf) {
    printf("AUDIO_DEVICE FAILED reason=host_buffer_alloc audio=MUTED "
           "guest=UNAFFECTED\n");
    free(a->stage); free(a->out); a->stage = a->out = NULL;
    fifo_free(&a->fifo);
    SDL_CloseAudioDevice(a->dev); a->dev = 0;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return false;
  }

  /* Latency target, in the ecosystem's measured band (~16-83 ms, clustered
   * at 50-67 ms): half a device block plus two native periods, which is
   * DKC2/DKC3's `device_frames/2 + 2*534` exactly. */
  uint32_t per_period = goof_app_audio_frames_per_period();
  a->target_frames = (uint32_t)a->have.samples / 2u + 2u * per_period;
  a->preroll_frames = a->target_frames;
  a->low_water = per_period;
  /* Routine ceiling: two targets, so a burst tops the queue up to ~2x the
   * latency target instead of to the emergency mark. */
  a->steady_ceiling = 2u * a->target_frames;
  a->high_water = 4u * a->target_frames;
  a->open = true;
  a->primed = false;
  a->fill_ema = (double)a->target_frames;
  printf("AUDIO_POLICY model=push servo=%s target_frames=%u (%.1f ms) "
         "preroll_frames=%u (%.1f ms) low_water=%u steady_ceiling=%u (%.1f ms) "
         "high_water=%u (%.1f ms) native_rate=%u frames_per_period=%u\n",
         a->servo ? "linear_occupancy" : "OFF", a->target_frames,
         1000.0 * a->target_frames / a->have.freq, a->preroll_frames,
         1000.0 * a->preroll_frames / a->have.freq, a->low_water,
         a->steady_ceiling, 1000.0 * a->steady_ceiling / a->have.freq,
         a->high_water, 1000.0 * a->high_water / a->have.freq,
         goof_app_audio_native_rate(), per_period);
  printf("AUDIO_BOUNDARY_PUMP model=physical_boundary_drain "
         "fifo=host_surplus_ring fifo_capacity_frames=%u (%.1f ms) "
         "intake_cap=REMOVED oldest_first_retirement=NONE "
         "callback=NONE thread=NONE\n",
         (unsigned)GOOF_AUDIO_FIFO_FRAMES,
         1000.0 * (double)GOOF_AUDIO_FIFO_FRAMES / (double)a->have.freq);
  return true;
}

/* Host-only linear stretch.  `ratio` is host output frames per native input
 * frame; at 1.0 this is a bit-exact copy, which is the configuration the
 * native-rate device runs in.  Phase and the previous input frame are
 * carried across calls so block edges have no seam.  None of this state is
 * ever hashed, and it cannot influence the producer. */
static uint32_t audio_stretch(HostAudio *a, const int16_t *in, uint32_t n,
                              int16_t *out, uint32_t out_cap) {
  if (n == 0) return 0;
  if (!a->stretch_primed) {
    a->last[0] = in[0];
    a->last[1] = in[1];
    a->position = 0.0;
    a->stretch_primed = true;
  }
  if (a->ratio == 1.0 && a->position == 0.0) {
    uint32_t take = n < out_cap ? n : out_cap;
    memcpy(out, in, (size_t)take * 2 * sizeof(int16_t));
    a->last[0] = in[2 * (n - 1)];
    a->last[1] = in[2 * (n - 1) + 1];
    return take;
  }
  double step = 1.0 / a->ratio;
  double pos = a->position;
  uint32_t produced = 0;
  while (pos < (double)n && produced < out_cap) {
    uint32_t k = (uint32_t)pos;
    double frac = pos - (double)k;
    const int16_t *prev = (k == 0) ? a->last : &in[2 * (k - 1)];
    const int16_t *cur = &in[2 * k];
    out[2 * produced] =
        (int16_t)((double)prev[0] + ((double)cur[0] - (double)prev[0]) * frac);
    out[2 * produced + 1] =
        (int16_t)((double)prev[1] + ((double)cur[1] - (double)prev[1]) * frac);
    produced++;
    pos += step;
  }
  a->position = pos - (double)n;
  if (a->position < 0.0) a->position = 0.0;
  a->last[0] = in[2 * (n - 1)];
  a->last[1] = in[2 * (n - 1) + 1];
  return produced;
}

/* --------------------------------------------------------------------- */
/* HARVEST and SERVICE -- the two halves of host presentation, deliberately */
/* kept separate (investigation section 13).                               */
/*                                                                        */
/*   HARVEST   guest DSP ring  ->  host surplus FIFO                      */
/*   SERVICE   host surplus FIFO -> stretch -> SDL_QueueAudio             */
/*                                                                        */
/* Both are safe to run from inside goof_app_step, at a physical boundary, */
/* because neither advances guest time and neither blocks:                */
/*   - harvest is goof_app_audio_drain, which the goof_app.h contract      */
/*     (invariants GOOF-AUDIO-1/2) states runs no SPC cycle and never      */
/*     waits;                                                             */
/*   - service is SDL_QueueAudio on a push-model device, which is a memcpy */
/*     under the device lock and never waits on the device.               */
/* Neither reads a wall clock and neither can influence what the guest     */
/* does next.  HOST AUDIO ADVANCES GUEST: NO.  HOST AUDIO BLOCKS GUEST: NO.*/
/* --------------------------------------------------------------------- */

/* Moves everything the guest has produced and not yet handed over out of
 * the 8192-frame core ring and into the host FIFO.  Running this at every
 * physical boundary is what stops the ring overflowing mid-epoch: at 534
 * frames per boundary it can never approach 8192. */
static uint32_t audio_harvest(HostAudio *a) {
  if (!a->open || !a->app) return 0;
  uint32_t total = 0;
  for (;;) {
    uint32_t got = goof_app_audio_drain(a->app, a->stage,
                                        GOOF_AUDIO_STAGE_FRAMES);
    if (!got) break;
    a->native_drained += got;
    fifo_push(&a->fifo, a->stage, got);
    total += got;
    if (got < GOOF_AUDIO_STAGE_FRAMES) break;
  }
  return total;
}

/* Feeds the device from the FIFO, in order, up to the occupancy ceiling.
 *
 * The ceiling is what bounds LATENCY; it is no longer what bounds INTAKE.
 * That is the whole substance of the fix.  Previously anything above the
 * ceiling was retired oldest-first and never played; now it simply stays in
 * the FIFO and is played by the next service call.  No frame is deleted, no
 * frame is reordered, and nothing is drained faster than 1.0x plus the
 * pre-existing +-0.5 % servo -- playing the backlog fast would reproduce the
 * very acceleration this milestone exists to remove. */
static uint32_t audio_service(HostAudio *a) {
  if (!a->open || !a->fifo.count) return 0;
  uint32_t queued = audio_queued_frames(a);
  uint32_t ceiling = a->primed ? a->steady_ceiling : a->preroll_frames;
  if (queued >= ceiling) return 0;
  uint32_t room = ceiling - queued;
  if (room > GOOF_AUDIO_STAGE_FRAMES) room = GOOF_AUDIO_STAGE_FRAMES;
  uint32_t take = fifo_pop(&a->fifo, a->stage, room);
  if (!take) return 0;
  uint32_t produced =
      audio_stretch(a, a->stage, take, a->out, GOOF_AUDIO_OUT_FRAMES);
  if (!produced) return 0;
  if (a->gain_q16 < GOOF_GAIN_UNITY) {           /* E3 volume / mute */
    goof_audio_apply_gain(a->out, (size_t)produced * a->have.channels, a->gain_q16);
    if (a->gain_q16 <= 0) a->gain_frames_silenced += produced;
    else a->gain_frames_scaled += produced;
  }
  if (SDL_QueueAudio(a->dev, a->out,
                     (Uint32)produced * a->frame_bytes) != 0) {
    /* The device refused the block.  `take` frames have already left the
     * FIFO, so they would otherwise disappear without a trace -- they are
     * still inside fifo.popped, so the accounting identity below still
     * closes, but nothing would say they never reached the speakers.
     * Counted here so a host failure that loses PCM is visible as a number
     * rather than as an unexplained quiet.  Never observed in any run. */
    a->service_lost += take;
    a->service_lost_events++;
    return 0;
  }
  a->host_queued += produced;
  /* DIAGNOSTIC tee: exactly the bytes SDL accepted, in order, so the
   * capture file IS what the speakers played. */
  if (a->cap) {
    fwrite(a->out, a->frame_bytes, produced, a->cap);
    a->t_cap_frames += produced;
  }
  return produced;
}

/* THE PHYSICAL-BOUNDARY HOOK.  Installed with goof_app_set_boundary_hook and
 * invoked once per physical frame boundary from inside goof_app_step, after
 * that boundary's 534 native frames exist.
 *
 * It harvests and then services, and does nothing else: no preroll decision,
 * no underflow accounting, no resync, no policy of any kind.  All of that
 * stays in audio_pump, at the logical-epoch boundary where the frontend owns
 * it.  The hook's only job is to make sure a 16-period epoch is serviced 16
 * times instead of once. */
static void audio_boundary_hook(void *user) {
  HostAudio *a = (HostAudio *)user;
  if (!a->open) return;
  a->boundary_drains++;
  a->boundary_harvested += audio_harvest(a);
  a->boundary_queued += audio_service(a);
}

/* --------------------------------------------------------------------- */
/* PRESENTATION PACING.                                                    */
/*                                                                        */
/* The physical-boundary hook removes the RING overflow and the intake     */
/* cap, so no PCM is deleted any more.  It cannot, by itself, remove the   */
/* backlog, and the reason is arithmetic rather than policy:              */
/*                                                                        */
/*   a logical epoch that crosses N physical boundaries contains N x 534   */
/*   native frames, which is N x 16.67 ms of music and can only be PLAYED  */
/*   in N x 16.67 ms of wall time -- but the legacy pacing holds that      */
/*   epoch for ONE 16.67 ms interval.                                     */
/*                                                                        */
/* Measured over the 2000-epoch campaign: 2287 physical periods in 2000    */
/* ticks, so 287 surplus periods = 153 258 frames = 4783 ms of music that  */
/* 33.3 s of wall clock has no room to play.  Before this milestone that   */
/* surplus was DELETED (48 083 frames retired + 97 388 dropped at the      */
/* ring).  With the FIFO alone it is PRESERVED but never drains: measured, */
/* the FIFO saturates its 4091 ms capacity by E673 and stays there, i.e.   */
/* the skip is traded for a permanent four-second A/V lag.                */
/*                                                                        */
/* There is no third option at fixed one-interval-per-epoch pacing. Either */
/* the music is deleted, or it lags, or the host gives a multi-period      */
/* epoch the wall time its own guest content occupies.  This does the      */
/* last, and it is HOST TIMING ONLY: the guest has already finished the    */
/* epoch before any of this runs.                                         */
/*                                                                        */
/* GOOF_ENHANCEMENTS_E4: the hold is no longer a blocking loop here.  The  */
/* pacer advances its deadline by the epoch's physical periods (a GUEST    */
/* quantity from GoofAppDiag, capped at GOOF_PACER_MAX_EPOCH_PERIODS =     */
/* 240, never occupancy or device state), and while that deadline is in    */
/* the future the main loop keeps servicing the device and the event queue */
/* itself.  Same wall time per epoch as before, now at the canonical       */
/* period instead of 1/60 s.                                               */
/* --------------------------------------------------------------------- */
enum {
  /* Kept for the GOOF_PRESENT_PACING report line (same value and meaning:
   * the pacer's bound on one epoch's hold). */
  GOOF_PRESENT_MAX_HOLD_PERIODS = GOOF_PACER_MAX_EPOCH_PERIODS,
};

/* Host-only resync: drop the device queue and wait for a fresh preroll.
 * Never touches the core ring's production, never runs the guest. */
static void audio_resync(HostAudio *a, const char *reason, uint64_t epoch,
                         uint32_t queued) {
  SDL_PauseAudioDevice(a->dev, 1);
  SDL_ClearQueuedAudio(a->dev);
  /* EMERGENCY ONLY.  A resync discards the host's whole in-flight timeline,
   * so the FIFO's contents go with the device queue -- keeping them would
   * re-present audio the resync just decided to abandon.  Counted, because
   * the gate requires resyncs to stay 0 and therefore requires this to stay
   * 0 as well. */
  a->resync_dropped += a->fifo.count;
  fifo_clear(&a->fifo);
  a->primed = false;
  a->stretch_primed = false;
  a->position = 0.0;
  a->fill_ema = (double)a->target_frames;
  a->resyncs++;
  printf("AUDIO_RESYNC reason=%s epoch=%" PRIu64 " queued_frames=%u "
         "high_water=%u resyncs=%" PRIu64 "\n", reason, epoch, queued,
         a->high_water, a->resyncs);
}

/* One guest step's worth of host audio (a "tick" below = one simulation
 * step since E4).  Called on the game thread
 * AFTER the guest has advanced, and only then: it consumes what that
 * advance produced and nothing else.  It cannot advance, block, or pace
 * the guest, and it is skipped entirely while the guest is paused. */
static void audio_pump(HostAudio *a, GoofApp *app, uint64_t epoch,
                       const GoofAppDiag *diag) {
  if (!a->open) return;
  a->ticks++;

  /* DIAGNOSTIC snapshot, taken before anything in this tick acts.  Read
   * only; `t_*` fields feed no branch below. */
  AudioTraceStats ts;
  audio_trace_get_stats(&ts);
  uint64_t d_produced = ts.produced - a->t_prev_produced;
  uint64_t d_dropped = ts.dropped - a->t_prev_dropped;
  uint64_t d_dropped_audible = ts.dropped_audible - a->t_prev_dropped_audible;
  uint64_t periods = diag ? diag->physical_periods : 0;
  uint64_t spc = diag ? diag->spc_cycles : 0;
  uint64_t d_spc = spc - a->t_prev_spc;
  if (periods > a->t_max_periods) a->t_max_periods = periods;
  uint32_t t_avail_before = goof_app_audio_available(app);
  uint32_t t_queued_before_all = audio_queued_frames(a);
  bool t_primed_before = a->primed;
  uint64_t t_underflows_before = a->underflows;
  uint64_t t_resyncs_before = a->resyncs;
  uint64_t t_trimmed_before = a->native_trimmed;
  /* The hook runs INSIDE goof_app_step, so by the time audio_pump is entered
   * this epoch's boundary work is already counted.  The delta is therefore
   * taken against the PREVIOUS tick's totals, like d_produced above, not
   * against the value at pump entry. */
  uint64_t t_bdrains_before = a->t_prev_bdrains;
  uint64_t t_bharvest_before = a->t_prev_bharvest;
  uint64_t t_bqueued_before = a->t_prev_bqueued;

  uint32_t queued = audio_queued_frames(a);
  if (queued < a->queue_min) a->queue_min = queued;
  if (queued > a->queue_max) a->queue_max = queued;
  a->queue_sum += queued;
  a->queue_samples++;

  /* Starvation.  The device keeps running and emits silence on its own; the
   * guest is NEVER run to satisfy it.  A drained queue means the host has
   * lost its cushion, so the timeline is re-prerolled rather than left to
   * stutter at the block rate (DKC1's ResetAudioTimeline policy). */
  if (a->primed && queued == 0) {
    a->underflows++;
    a->primed = false;
    a->stretch_primed = false;
    SDL_PauseAudioDevice(a->dev, 1);
    printf("AUDIO_UNDERFLOW epoch=%" PRIu64 " underflows=%" PRIu64 "\n",
           epoch, a->underflows);
  } else if (a->primed && queued < a->low_water) {
    a->low_water_events++;
  }

  /* Occupancy servo.  Host-only, +-0.5 % authority.  It bends the host's
   * consumption of already-produced PCM; it can never bend production.
   *
   * It is computed BEFORE this tick's service call, and the ratio it leaves
   * behind is also what the physical-boundary hook uses for the whole of the
   * next epoch.  That is deliberate: a hook running inside goof_app_step
   * cannot consult a servo that has not run yet, and the servo's EMA moves
   * far too slowly for one epoch's staleness to matter. */
  if (a->servo && a->primed) {
    a->fill_ema += ((double)queued - a->fill_ema) * GOOF_AUDIO_SERVO_EMA;
    double error = (a->fill_ema - (double)a->target_frames) /
                   (double)a->target_frames;
    double adj = -error * GOOF_AUDIO_SERVO_GAIN;
    if (adj > GOOF_AUDIO_SERVO_DEVIATION) adj = GOOF_AUDIO_SERVO_DEVIATION;
    if (adj < -GOOF_AUDIO_SERVO_DEVIATION) adj = -GOOF_AUDIO_SERVO_DEVIATION;
    a->ratio = 1.0 + adj;
  } else {
    a->ratio = 1.0;
  }
  if (a->ratio < a->ratio_min) a->ratio_min = a->ratio;
  if (a->ratio > a->ratio_max) a->ratio_max = a->ratio;

  /* Bound host LATENCY by construction, and no longer host INTAKE.
   *
   * This is where the defect used to live.  The old rule compared the core
   * ring's occupancy against the device queue's remaining headroom and
   * retired the difference OLDEST-FIRST -- 9824 native frames, 306.6 ms,
   * across one menu-to-gameplay transition, which a listener hears as the
   * music jumping forward.  It existed because a logical epoch spanning 16
   * physical periods handed the host 8544 frames in a single call and there
   * was nowhere to put them.
   *
   * Now there is.  The physical-boundary hook has already harvested this
   * epoch's PCM into the host FIFO, boundary by boundary, and serviced the
   * device between them.  What is left for this tick is the tail: whatever
   * the hook could not fit under the ceiling, plus -- for a one-period epoch,
   * which is 99.4 % of them -- simply this tick's 534 frames.  It goes into
   * the FIFO and is played in order.  Nothing is retired. */
  uint32_t harvested = audio_harvest(a);
  a->tick_harvested += harvested;
  uint32_t drained = harvested;
  uint32_t produced = audio_service(a);
  queued = audio_queued_frames(a);

  /* Overflow: the device is behind.  Resync host-side; never stall the
   * guest waiting for it. */
  if (queued > a->high_water) {
    audio_resync(a, "high_water", epoch, queued);
    goto trace_out;   /* DIAGNOSTIC: same early exit, one exit point */
  }

  /* Preroll: accumulate a small cushion before letting the device run, so
   * playback does not start underneath its own start-up transient. */
  if (!a->primed) {
    if (queued >= a->preroll_frames) {
      a->primed = true;
      SDL_PauseAudioDevice(a->dev, 0);
      printf("AUDIO_PREROLL DONE epoch=%" PRIu64 " queued_frames=%u "
             "latency_ms=%.1f\n", epoch, queued,
             1000.0 * queued / a->have.freq);
    } else {
      a->preroll_waits++;
    }
  }

trace_out:
  /* ---- DIAGNOSTIC ROW.  Pure observation; nothing above depends on it. */
  if (a->native_trimmed > t_trimmed_before) a->t_retire_events++;
  a->fifo_sum += (double)a->fifo.count;
  a->fifo_samples++;
  if (a->fifo.count > a->fifo_peak_epoch_max)
    a->fifo_peak_epoch_max = a->fifo.count;
  if (a->trace) {
    uint32_t ceiling_now = a->primed ? a->steady_ceiling : a->preroll_frames;
    uint32_t avail_after = goof_app_audio_available(app);
    double error = (a->fill_ema - (double)a->target_frames) /
                   (double)a->target_frames;
    fprintf(a->trace,
            "%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
            "\t%" PRIu64
            "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
            "\t%u\t%u\t%" PRIu64 "\t%u\t%u\t%u\t%.6f\t%.2f\t%.6f"
            "\t%u\t%u\t%d\t%d\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
            "\t%" PRIu64
            "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%u\t%u\t%" PRIu64 "\n",
            a->ticks, audio_trace_wall_ms(), epoch, periods, spc, d_spc,
            ts.produced, d_produced, d_dropped, d_dropped_audible,
            t_avail_before, ceiling_now,
            a->native_trimmed - t_trimmed_before,
            drained, avail_after, produced,
            a->ratio, a->fill_ema, error,
            t_queued_before_all, audio_queued_frames(a),
            t_primed_before ? 1 : 0, a->primed ? 1 : 0,
            a->underflows - t_underflows_before,
            a->resyncs - t_resyncs_before,
            a->native_trimmed, a->t_max_periods,
            /* GOOF_AUDIO_PHYSICAL_BOUNDARY_PUMP_V1 columns 27..32 */
            a->boundary_drains - t_bdrains_before,
            a->boundary_harvested - t_bharvest_before,
            a->boundary_queued - t_bqueued_before,
            a->fifo.count, a->fifo.peak, a->fifo.overflow_dropped);
  }
  a->t_prev_produced = ts.produced;
  a->t_prev_dropped = ts.dropped;
  a->t_prev_dropped_audible = ts.dropped_audible;
  a->t_prev_spc = spc;
  a->t_prev_bdrains = a->boundary_drains;
  a->t_prev_bharvest = a->boundary_harvested;
  a->t_prev_bqueued = a->boundary_queued;
  a->t_prev_avail_after = goof_app_audio_available(app);
}

static void audio_report(const HostAudio *a) {
  if (!a->requested) {
    printf("AUDIO_PRESENTATION DISABLED reason=--no-audio "
           "guest=UNAFFECTED\n");
    return;
  }
  if (!a->open) {
    printf("AUDIO_PRESENTATION MUTED reason=device_unavailable "
           "guest=UNAFFECTED\n");
    return;
  }
  uint32_t qmin = a->queue_min == UINT32_MAX ? 0 : a->queue_min;
  double qavg = a->queue_samples ? a->queue_sum / (double)a->queue_samples : 0.0;
  double favg = a->fifo_samples ? a->fifo_sum / (double)a->fifo_samples : 0.0;
  printf("AUDIO_PRESENTATION SUMMARY ticks=%" PRIu64 " native_frames_drained=%"
         PRIu64 " native_frames_trimmed=%" PRIu64 " host_frames_queued=%"
         PRIu64 "\n", a->ticks, a->native_drained, a->native_trimmed,
         a->host_queued);
  printf("AUDIO_QUEUE min_frames=%u max_frames=%u avg_frames=%.1f "
         "target_frames=%u high_water=%u avg_latency_ms=%.1f "
         "max_latency_ms=%.1f\n", qmin, a->queue_max, qavg, a->target_frames,
         a->high_water, 1000.0 * qavg / a->have.freq,
         1000.0 * a->queue_max / a->have.freq);
  printf("AUDIO_EVENTS underflows=%" PRIu64 " resyncs=%" PRIu64
         " low_water_events=%" PRIu64 " preroll_waits=%" PRIu64
         " stretch_ratio_min=%.5f stretch_ratio_max=%.5f servo=%s\n",
         a->underflows, a->resyncs, a->low_water_events, a->preroll_waits,
         a->ratio_min, a->ratio_max, a->servo ? "linear_occupancy" : "OFF");
  /* DIAGNOSTIC summary: the two numbers the transition-pacing investigation
   * exists to read, plus the core-ring loss that is NOT host retirement. */
  AudioTraceStats ts;
  audio_trace_get_stats(&ts);
  printf("AUDIO_PACING max_periods_per_pump=%" PRIu64 " retire_events=%" PRIu64
         " native_retired=%" PRIu64 " retired_ms=%.1f"
         " core_ring_dropped=%" PRIu64 " core_ring_dropped_audible=%" PRIu64
         " core_ring_dropped_ms=%.1f pcm_produced=%" PRIu64
         " host_frames_captured=%" PRIu64 "\n",
         a->t_max_periods, a->t_retire_events, a->native_trimmed,
         1000.0 * (double)a->native_trimmed / (double)a->have.freq,
         ts.dropped, ts.dropped_audible,
         1000.0 * (double)ts.dropped / (double)a->have.freq,
         ts.produced, a->t_cap_frames);
  printf("AUDIO_FIFO capacity_frames=%u peak_frames=%u peak_ms=%.1f "
         "final_frames=%u final_ms=%.1f avg_frames=%.1f avg_ms=%.1f "
         "pushed=%" PRIu64 " popped=%" PRIu64 " overflow_dropped=%" PRIu64
         " overflow_events=%" PRIu64 " resync_dropped=%" PRIu64
         " service_lost=%" PRIu64 " service_lost_events=%" PRIu64 "\n",
         (unsigned)GOOF_AUDIO_FIFO_FRAMES, a->fifo.peak,
         1000.0 * (double)a->fifo.peak / (double)a->have.freq,
         a->fifo.count, 1000.0 * (double)a->fifo.count / (double)a->have.freq,
         favg, 1000.0 * favg / (double)a->have.freq,
         a->fifo.pushed, a->fifo.popped, a->fifo.overflow_dropped,
         a->fifo.overflow_events, a->resync_dropped, a->service_lost,
         a->service_lost_events);
  printf("AUDIO_BOUNDARY drains=%" PRIu64 " harvested_at_boundary=%" PRIu64
         " queued_at_boundary=%" PRIu64 " harvested_at_tick=%" PRIu64
         " boundary_share=%.1f%%\n",
         a->boundary_drains, a->boundary_harvested, a->boundary_queued,
         a->tick_harvested,
         a->native_drained ? 100.0 * (double)a->boundary_harvested /
                             (double)a->native_drained : 0.0);
  /* GOOF_ENHANCEMENTS_E3: host-side gain only (never part of any digest). */
  printf("AUDIO_HOST_GAIN final_gain_q16=%d frames_scaled=%" PRIu64
         " frames_silenced=%" PRIu64 " frames_unity=%" PRIu64 "\n",
         (int)a->gain_q16, a->gain_frames_scaled, a->gain_frames_silenced,
         a->host_queued - a->gain_frames_scaled - a->gain_frames_silenced);
  /* Ring accounting, restated for the FIFO architecture.  Everything the
   * guest produced is either still in the core ring, resident in the host
   * FIFO, already queued to the device, retired (which must now be 0), or
   * lost to core-ring overflow BEFORE the host ever saw it.  The last term
   * is the only one that predates the canonical digest's capture point, and
   * `residual` must close at exactly 0. */
  printf("AUDIO_ACCOUNTING produced=%" PRIu64 " = drained=%" PRIu64
         " + retired=%" PRIu64 " + core_dropped=%" PRIu64 " + residual=%lld"
         " | drained = fifo_resident=%u + fifo_popped=%" PRIu64
         " + fifo_overflow=%" PRIu64 " + resync_dropped=%" PRIu64
         " (delta=%lld)\n",
         ts.produced, a->native_drained, a->native_trimmed, ts.dropped,
         (long long)((int64_t)ts.produced - (int64_t)a->native_drained -
                     (int64_t)a->native_trimmed - (int64_t)ts.dropped),
         a->fifo.count, a->fifo.popped, a->fifo.overflow_dropped,
         a->resync_dropped,
         (long long)((int64_t)a->native_drained - (int64_t)a->fifo.count -
                     (int64_t)a->fifo.popped -
                     (int64_t)a->fifo.overflow_dropped -
                     (int64_t)a->resync_dropped));
}

/* Race-safe teardown: stop the device before anything it could read is
 * released, and release the host buffers before the core that produced
 * what they hold.  With no audio thread there is no callback in flight,
 * which is precisely why this ordering is simple. */
static void audio_close(HostAudio *a) {
  /* DIAGNOSTIC sinks first: they are plain FILE*s and nothing downstream
   * reads them. */
  if (a->trace) { fclose(a->trace); a->trace = NULL; }
  if (a->cap) { fclose(a->cap); a->cap = NULL; }
  if (a->open) {
    SDL_PauseAudioDevice(a->dev, 1);
    SDL_ClearQueuedAudio(a->dev);
    SDL_CloseAudioDevice(a->dev);
    a->dev = 0;
    a->open = false;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
  }
  free(a->stage);
  free(a->out);
  a->stage = a->out = NULL;
  fifo_free(&a->fifo);
}

typedef struct {
  SDL_Window *window;
  SDL_Renderer *renderer;
  SDL_Texture *texture;
  int source_width, source_height;
  /* GOOF_ENHANCEMENTS_E3: host presentation geometry (Settings > Video).
   * `video` is what is live right now (a preview or the committed value);
   * it decides only the window and the destination rectangle of the copy
   * -- the texture stays the 256x224 framebuffer mirror. */
  GoofVideoSettings video;
  int window_scale;              /* resolved integer scale of the window */
  bool fullscreen;               /* SDL fullscreen-desktop is active      */
  bool fail_fullscreen;          /* --video-fail-fullscreen test seam      */
  uint64_t video_changes;        /* applied changes (diagnostic)          */
  /* GOOF_ENHANCEMENTS_E4: presentation pacing.  `vsync` is what the renderer
   * really does now (SDL_RENDERER_PRESENTVSYNC after creation / after
   * SDL_RenderSetVSync), which is also what p->video.vsync reports. */
  bool vsync;
  uint64_t vsync_changes, vsync_refused;
  char renderer_name[32];
  bool target_textures;          /* SDL_RENDERER_TARGETTEXTURE (E5 audit)  */
} Presentation;

/* The display's current mode, for the log only.  Nothing reads it back:
 * the refresh rate is information for the human, never a clock. */
static int presentation_refresh_hz(const Presentation *p) {
  SDL_DisplayMode dm;
  int display = p->window ? SDL_GetWindowDisplayIndex(p->window) : 0;
  if (display < 0 || SDL_GetCurrentDisplayMode(display, &dm) != 0) return 0;
  return dm.refresh_rate;
}

static void presentation_log_display(const Presentation *p, const char *what) {
  SDL_DisplayMode dm = {0};
  int display = p->window ? SDL_GetWindowDisplayIndex(p->window) : 0;
  if (display >= 0) SDL_GetCurrentDisplayMode(display, &dm);
  printf("HOST_DISPLAY %s index=%d mode=%dx%d refresh_hz=%d (information only; "
         "the guest cadence never follows it)\n", what, display, dm.w, dm.h,
         dm.refresh_rate);
}

static void presentation_destroy(Presentation *p) {
  if (p->texture) SDL_DestroyTexture(p->texture);
  if (p->renderer) SDL_DestroyRenderer(p->renderer);
  if (p->window) SDL_DestroyWindow(p->window);
  *p = (Presentation){0};
}

/* Usable desktop area of the display the window is on (or the primary
 * display before the window exists), for window_scale=auto. */
static void presentation_usable_area(const Presentation *p, int *w, int *h) {
  SDL_Rect r = {0, 0, 0, 0};
  int display = p->window ? SDL_GetWindowDisplayIndex(p->window) : 0;
  if (display < 0 || SDL_GetDisplayUsableBounds(display, &r) != 0) r.w = r.h = 0;
  *w = r.w;
  *h = r.h;
}

static void presentation_log(const Presentation *p, const char *what) {
  int ww = 0, wh = 0, ow = 0, oh = 0;
  SDL_GetWindowSize(p->window, &ww, &wh);
  if (p->renderer) SDL_GetRendererOutputSize(p->renderer, &ow, &oh);
  char scale[16];
  goof_window_scale_token(p->video.window_scale, scale, sizeof scale);
  printf("HOST_VIDEO %s display=%s window_scale=%s resolved_scale=%d "
         "pixel_aspect=%s fullscreen_scaling=%s window=%dx%d output=%dx%d "
         "framebuffer=256x224 vsync=%s filter=%s\n", what, goof_display_token(p->video.display),
         scale, p->window_scale, goof_aspect_token(p->video.aspect),
         goof_scaling_token(p->video.scaling), ww, wh, ow, oh,
         goof_vsync_token(p->vsync), goof_filter_token(p->video.filter));
}

/* Applies host presentation settings to the live window.  HOST ONLY: no
 * guest call, no framebuffer write, no clock change; the guest is paused
 * whenever this runs from the Settings overlay.  Returns false (and keeps
 * the previous display mode) when fullscreen is refused. */
static bool presentation_apply(Presentation *p, const GoofVideoSettings *v) {
  if (SDL_SetTextureScaleMode(p->texture, v->filter == GOOF_FILTER_BILINEAR
                              ? SDL_ScaleModeLinear : SDL_ScaleModeNearest)) {
    fprintf(stderr, "HOST_VIDEO WARNING filter change refused: %s\n", SDL_GetError());
    return false;
  }
  bool want_fs = v->display == GOOF_DISPLAY_FULLSCREEN;
  bool ok = true;
  bool left_fullscreen = false;
  if (want_fs && !p->fullscreen) {
    if (p->fail_fullscreen ||
        SDL_SetWindowFullscreen(p->window, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) {
      printf("HOST_VIDEO WARNING fullscreen refused (%s); staying windowed\n",
             p->fail_fullscreen ? "--video-fail-fullscreen" : SDL_GetError());
      ok = false;
    } else {
      p->fullscreen = true;
    }
  } else if (!want_fs && p->fullscreen) {
    if (SDL_SetWindowFullscreen(p->window, 0) != 0)
      printf("HOST_VIDEO WARNING leaving fullscreen: %s\n", SDL_GetError());
    p->fullscreen = false;
    left_fullscreen = true;
  }
  p->video = *v;
  if (!ok) p->video.display = GOOF_DISPLAY_WINDOWED;
  int aw, ah;
  presentation_usable_area(p, &aw, &ah);
  p->window_scale = goof_video_resolve_scale(&p->video, aw, ah);
  if (!p->fullscreen) {
    int w, h, cw = 0, ch = 0;
    goof_video_window_size(p->video.aspect, p->window_scale, &w, &h);
    SDL_GetWindowSize(p->window, &cw, &ch);
    /* Resize (and re-centre) only when the size really changes, so a
     * window the user moved stays where it is. */
    if (w != cw || h != ch || left_fullscreen) {
      SDL_SetWindowSize(p->window, w, h);
      SDL_SetWindowPosition(p->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
  }
  /* GOOF_ENHANCEMENTS_E4: vsync, live.  SDL_RenderSetVSync switches the swap
   * interval on the existing renderer (OpenGL / GLES2: SDL_GL_SetSwapInterval;
   * Direct3D 9: a device reset, which the SDL_RENDER_TARGETS_RESET handler
   * in the main loop already recovers from).  If the backend refuses, the
   * previous mode stays and the page steps back -- the renderer is never
   * torn down behind the game's back.  Pacing only: the guest is paused while
   * this runs and its cadence is the pacer's, not the display's. */
  if (v->vsync != p->vsync) {
    if (SDL_RenderSetVSync(p->renderer, v->vsync ? 1 : 0) == 0) {
      SDL_RendererInfo ri;
      bool now_on = SDL_GetRendererInfo(p->renderer, &ri) == 0
                        ? (ri.flags & SDL_RENDERER_PRESENTVSYNC) != 0 : v->vsync;
      printf("HOST_VSYNC CHANGED %s -> %s renderer=%s flag=%s\n",
             goof_vsync_token(p->vsync), goof_vsync_token(v->vsync),
             p->renderer_name, now_on ? "on" : "off");
      p->vsync = v->vsync;
      p->vsync_changes++;
    } else {
      printf("HOST_VSYNC WARNING change to %s refused by renderer %s (%s); "
             "keeping %s\n", goof_vsync_token(v->vsync), p->renderer_name,
             SDL_GetError(), goof_vsync_token(p->vsync));
      p->vsync_refused++;
      ok = false;
    }
  }
  p->video.vsync = p->vsync;
  p->video_changes++;
  return ok;
}

/* THE PRESENTATION PIPELINE, whole (GOOF_ENHANCEMENTS_E4):
 *
 *   guest framebuffer 256x224 (goof_app_framebuffer; the hashed oracle,
 *       read-only, uploaded to view->texture once per NEW guest frame)
 *     -> E5 nearest/bilinear sampling to destination (aspect, integer/fit)
 *     -> optional source-row-aligned scanline coverage in destination space
 *     -> host overlay (Settings), composited over the copy
 *     -> SDL_RenderPresent (vsync may block here; the guest never waits)
 *
 * Nothing in this function writes the framebuffer or runs the guest, so it
 * may run any number of times per guest frame (UI redraws while held). */
static SDL_Rect presentation_dest(const Presentation *p);
static void presentation_draw(Presentation *p, bool have_frame,
                              GoofSdlSettings *overlay) {
  SDL_RenderClear(p->renderer);
  if (have_frame) {
    SDL_Rect dst = presentation_dest(p);
    GoofPresentationImage image = {p->texture, p->source_width, p->source_height,
                                   SDL_PIXELFORMAT_XRGB8888};
    if (!goof_sdl_filter_draw(p->renderer, &image, p->video.filter, &dst))
      fprintf(stderr, "HOST_VIDEO WARNING filter draw: %s\n", SDL_GetError());
  }
  if (overlay) goof_sdl_settings_render(overlay, p->renderer);
  SDL_RenderPresent(p->renderer);
}

/* Where the 256x224 texture goes in the current output. */
static SDL_Rect presentation_dest(const Presentation *p) {
  int ow = 0, oh = 0;
  SDL_GetRendererOutputSize(p->renderer, &ow, &oh);
  GoofRect r = goof_video_dest_rect(p->video.aspect,
                                    p->fullscreen ? p->video.scaling
                                                  : GOOF_SCALING_INTEGER, ow, oh);
  return (SDL_Rect){r.x, r.y, r.w, r.h};
}

static bool presentation_create(Presentation *p, const GoofVideoSettings *v,
                                bool fail_fullscreen, uint32_t width,
                                uint32_t height) {
  *p = (Presentation){0};
  p->fail_fullscreen = fail_fullscreen;
  p->video = *v;
  p->source_width = (int)width;
  p->source_height = (int)height;
  int aw, ah;
  presentation_usable_area(p, &aw, &ah);
  p->window_scale = goof_video_resolve_scale(v, aw, ah);
  int ww, wh;
  goof_video_window_size(v->aspect, p->window_scale, &ww, &wh);
  /* Atlas creation default. E5 changes only the game texture's scale mode;
   * the canonical framebuffer has already passed the oracle boundary. */
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  /* Fullscreen from the config is requested at creation; if that fails the
   * window opens windowed with a warning and the config file is left as it
   * is (E3 section 8). */
  bool want_fs = v->display == GOOF_DISPLAY_FULLSCREEN && !fail_fullscreen;
  if (want_fs) {
    p->window = SDL_CreateWindow("Goof Troop Recomp", SDL_WINDOWPOS_CENTERED,
                                 SDL_WINDOWPOS_CENTERED, ww, wh,
                                 SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (p->window) p->fullscreen = true;
    else printf("HOST_VIDEO WARNING fullscreen window refused (%s); opening "
                "windowed, config unchanged\n", SDL_GetError());
  } else if (v->display == GOOF_DISPLAY_FULLSCREEN) {
    printf("HOST_VIDEO WARNING fullscreen refused (--video-fail-fullscreen); "
           "opening windowed, config unchanged\n");
  }
  if (!p->window)
    p->window = SDL_CreateWindow("Goof Troop Recomp", SDL_WINDOWPOS_CENTERED,
                                 SDL_WINDOWPOS_CENTERED, ww, wh, SDL_WINDOW_SHOWN);
  if (!p->fullscreen) p->video.display = GOOF_DISPLAY_WINDOWED;
  if (!p->window) {
    fprintf(stderr, "goof_recomp: SDL_CreateWindow: %s\n", SDL_GetError());
    return false;
  }
  /* Vsync as configured (default OFF = E3).  Before E4, vsync was OFF
   * deliberately: with one epoch per presentation tick it would have made
   * the guest's rate a property of the monitor (design section 8).  Since E4
   * the simulation clock is separate (host/goof_frame_pacer.c), so a
   * blocking present can only delay a due step, which the bounded catch-up
   * then runs -- the monitor is never the guest clock. */
  Uint32 vs = v->vsync ? SDL_RENDERER_PRESENTVSYNC : 0;
  p->renderer = SDL_CreateRenderer(p->window, -1, SDL_RENDERER_ACCELERATED | vs);
  if (!p->renderer)
    p->renderer = SDL_CreateRenderer(p->window, -1, SDL_RENDERER_SOFTWARE | vs);
  if (!p->renderer) {
    fprintf(stderr, "goof_recomp: SDL_CreateRenderer: %s\n", SDL_GetError());
    presentation_destroy(p);
    return false;
  }
  SDL_RendererInfo ri;
  if (SDL_GetRendererInfo(p->renderer, &ri) == 0) {
    snprintf(p->renderer_name, sizeof p->renderer_name, "%s", ri.name);
    p->vsync = (ri.flags & SDL_RENDERER_PRESENTVSYNC) != 0;
    p->target_textures = (ri.flags & SDL_RENDERER_TARGETTEXTURE) != 0;
  } else {
    snprintf(p->renderer_name, sizeof p->renderer_name, "unknown");
    p->vsync = v->vsync;
  }
  if (v->vsync && !p->vsync)
    printf("HOST_VSYNC WARNING vsync requested but renderer %s did not enable it; "
           "presenting without vsync, config unchanged\n", p->renderer_name);
  p->video.vsync = p->vsync;
  /* 0x00RRGGBB from the isolated renderer is exactly XRGB8888 on a
   * little-endian host: the texture is a mirror, with no conversion. */
  p->texture = SDL_CreateTexture(p->renderer, SDL_PIXELFORMAT_XRGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, (int)width,
                                 (int)height);
  if (!p->texture) {
    fprintf(stderr, "goof_recomp: SDL_CreateTexture: %s\n", SDL_GetError());
    presentation_destroy(p);
    return false;
  }
  if (SDL_SetTextureScaleMode(p->texture, v->filter == GOOF_FILTER_BILINEAR
                              ? SDL_ScaleModeLinear : SDL_ScaleModeNearest)) {
    presentation_destroy(p);
    return false;
  }
  return true;
}

/* --------------------------------------------------------------------- */
/* HOST SETTINGS (GOOF_ENHANCEMENTS_E2, extended by E3).  Host configuration */
/* only: the file holds bindings, whose only way to the guest is the         */
/* GoofInputSample input_sample builds from them, plus [video] / [audio]     */
/* presentation settings that never reach the guest at all.  Never used by   */
/* --headless-compare, --input-script or --no-input (invariant O12).         */
/*                                                                          */
/* E3 model: `stored` is what the file holds (and what every Save writes,   */
/* whole, through the one atomic writer); `video` / `audio` are the          */
/* COMMITTED EFFECTIVE values (stored + command-line overrides), which the  */
/* Settings pages edit.  --scale N is effective without being stored; Save  */
/* keeps the stored window_scale unless the user changed the scale.         */
/* --------------------------------------------------------------------- */

typedef struct {
  HostInput *input;
  bool writable;               /* Save may write `path`                    */
  bool no_config;
  char path[1024];
  char target[GOOF_UI_COLS + 1];  /* short label shown on the Input page  */
  /* E3 */
  GoofConfig stored;
  GoofVideoSettings video;     /* committed effective                      */
  GoofAudioSettings audio;     /* committed effective                      */
  GoofAudioSettings audio_live;/* what the host gain uses now (preview)    */
  bool scale_from_cli;         /* video.window_scale came from --scale     */
  bool vsync_from_cli;         /* E4: video.vsync came from --vsync        */
  bool filter_from_cli;
  Presentation *view;
  HostAudio *out;
  char video_note[GOOF_UI_COLS + 1];
} HostSettings;

/* Host gain from the live audio settings and the window focus. */
static void settings_update_gain(HostSettings *hs, bool focused) {
  if (!hs->out) return;
  int32_t g = goof_audio_gain_q16(&hs->audio_live, focused);
  if (g != hs->out->gain_q16) {
    printf("HOST_AUDIO_GAIN volume=%d mute=%d mute_when_unfocused=%d focused=%d "
           "gain_q16=%d\n", hs->audio_live.master_volume, hs->audio_live.mute ? 1 : 0,
           hs->audio_live.mute_when_unfocused ? 1 : 0, focused ? 1 : 0, (int)g);
    hs->out->gain_q16 = g;
  }
}

static void config_warn(void *user, const char *msg) {
  (void)user;
  printf("Config warning: %s\n", msg);
}

static void settings_target_label(HostSettings *hs, const char *reason) {
  if (reason) { snprintf(hs->target, sizeof hs->target, "%s", reason); return; }
  char shown[1024];
  const char *home = getenv("HOME");
  size_t hn = home ? strlen(home) : 0;
  if (hn > 1 && strncmp(hs->path, home, hn) == 0 && hs->path[hn] == '/')
    snprintf(shown, sizeof shown, "~%s", hs->path + hn);
  else
    snprintf(shown, sizeof shown, "%s", hs->path);
  /* Keep the tail (the file name) when it does not fit the overlay line. */
  size_t n = strlen(shown), room = sizeof hs->target - 1;
  const char *from = shown;
  size_t lead = 0;
  if (n > room) { from = shown + n - (room - 2); lead = 2; memcpy(hs->target, "..", 2); }
  size_t len = strlen(from);
  memcpy(hs->target + lead, from, len + 1);
}

/* built-in defaults -> config file -> command line (which only chooses the
 * file).  Never fatal: every failure leaves the E1 defaults in place. */
static void settings_load(const Options *o, HostSettings *hs) {
  GoofConfig cfg;
  goof_config_defaults(&cfg);
  hs->input->bindings = cfg.bindings;
  hs->stored = cfg;
  if (o->no_config) {
    hs->no_config = true;
    printf("Config: defaults (--no-config; Settings changes last this session)\n");
    settings_target_label(hs, "NOTHING: --NO-CONFIG (THIS SESSION ONLY)");
    return;
  }
  bool have_path = false;
  if (o->config_path) {
    have_path = snprintf(hs->path, sizeof hs->path, "%s", o->config_path) <
                (int)sizeof hs->path;
  } else if (o->portable) {
    char *base = SDL_GetBasePath();
    have_path = base && goof_config_portable_path(base, hs->path, sizeof hs->path);
    SDL_free(base);
  } else {
    GoofConfigEnv env;
    goof_config_env_from_process(&env);
    have_path = goof_config_default_path(goof_config_host_platform(), &env,
                                         hs->path, sizeof hs->path);
  }
  if (!have_path) {
    printf("Config: defaults (no config location: %s)\n",
           o->portable ? "executable directory unknown"
                       : "HOME / XDG_CONFIG_HOME / APPDATA not set");
    settings_target_label(hs, "NOTHING: NO CONFIG LOCATION");
    return;
  }
  printf("Config: %s\n", hs->path);
  GoofConfigLoadStatus st = goof_config_load_file(hs->path, &cfg, config_warn, NULL);
  hs->input->bindings = cfg.bindings;
  hs->stored = cfg;
  if (st == GOOF_CONFIG_LOADED) {
    printf("Config: loaded schema %d\n", GOOF_CONFIG_SCHEMA);
    hs->writable = true;
  } else if (st == GOOF_CONFIG_MISSING) {
    printf("Config: defaults (no file yet; Settings > Save creates it)\n");
    hs->writable = true;
  } else {
    /* The existing file is left exactly as it is -- it may belong to a newer
     * build -- and Save will not overwrite it. */
    printf("Config: defaults (file ignored; it will not be overwritten)\n");
    settings_target_label(hs, "NOTHING: EXISTING FILE NOT READ (SEE LOG)");
    return;
  }
  settings_target_label(hs, NULL);
}

/* The ONE writer: every page's Save serialises the whole stored model
 * (input + video + audio) through goof_config_save_atomic, so a Video save
 * can never drop bindings and an Input save can never drop video/audio. */
static GoofSaveResult settings_persist(HostSettings *hs, const char *what,
                                       char *msg, size_t cap) {
  if (!hs->writable) {
    printf("Settings applied (%s; not written: %s)\n", what,
           hs->no_config ? "--no-config" : "no writable config file");
    snprintf(msg, cap, "%s", hs->no_config ? "APPLIED FOR THIS SESSION (--NO-CONFIG)"
                                           : "APPLIED, NOT WRITTEN (SEE LOG)");
    return GOOF_SAVE_APPLIED_ONLY;
  }
  char err[512];
  if (!goof_config_save_atomic(hs->path, &hs->stored, err, sizeof err)) {
    printf("Settings save failed: %s (%s applied for this session)\n", err, what);
    snprintf(msg, cap, "APPLIED, BUT SAVE FAILED (SEE LOG)");
    return GOOF_SAVE_APPLIED_ONLY;
  }
  printf("Settings saved: %s (%s)\n", hs->path, what);
  snprintf(msg, cap, "SAVED - ACTIVE NOW");
  return GOOF_SAVE_PERSISTED;
}

/* GoofSettingsHost.save: validate already done by the menu (no conflicts);
 * apply live first, then persist atomically. */
static GoofSaveResult settings_save(void *user, const GoofBindings *b, char *msg,
                                    size_t cap) {
  HostSettings *hs = (HostSettings *)user;
  hs->input->bindings = *b;
  hs->stored.bindings = *b;
  return settings_persist(hs, "input", msg, cap);
}

/* ---- E3 Video / Audio pages (GoofHostPages) ---- */

static bool settings_preview_video(void *user, const GoofVideoSettings *v) {
  HostSettings *hs = (HostSettings *)user;
  bool ok = presentation_apply(hs->view, v);
  presentation_log(hs->view, ok ? "PREVIEW" : "PREVIEW_REFUSED");
  return ok;
}

static GoofSaveResult settings_save_video(void *user, const GoofVideoSettings *v,
                                          char *msg, size_t cap) {
  HostSettings *hs = (HostSettings *)user;
  if (!presentation_apply(hs->view, v)) {
    snprintf(msg, cap, "FULLSCREEN FAILED - NOT SAVED");
    presentation_log(hs->view, "SAVE_REFUSED");
    return GOOF_SAVE_FAILED;
  }
  GoofVideoSettings stored = *v;
  /* A --scale override stays a session value: it is written only when the
   * user actually changed the scale on the Video page. */
  if (hs->scale_from_cli && v->window_scale == hs->video.window_scale)
    stored.window_scale = hs->stored.video.window_scale;
  else
    hs->scale_from_cli = false;
  /* E4: --vsync is a session value the same way. */
  if (hs->vsync_from_cli && v->vsync == hs->video.vsync)
    stored.vsync = hs->stored.video.vsync;
  else
    hs->vsync_from_cli = false;
  if (hs->filter_from_cli && v->filter == hs->video.filter)
    stored.filter = hs->stored.video.filter;
  else
    hs->filter_from_cli = false;
  if (!hs->scale_from_cli && !hs->vsync_from_cli && !hs->filter_from_cli) hs->video_note[0] = '\0';
  hs->video = *v;
  hs->video.vsync = hs->view->vsync;     /* what the renderer really does */
  hs->stored.video = stored;
  presentation_log(hs->view, "COMMIT");
  return settings_persist(hs, "video", msg, cap);
}

static void settings_preview_audio(void *user, const GoofAudioSettings *a) {
  HostSettings *hs = (HostSettings *)user;
  hs->audio_live = *a;
  printf("HOST_AUDIO PREVIEW volume=%d mute=%d mute_when_unfocused=%d\n",
         a->master_volume, a->mute ? 1 : 0, a->mute_when_unfocused ? 1 : 0);
}

static GoofSaveResult settings_save_audio(void *user, const GoofAudioSettings *a,
                                          char *msg, size_t cap) {
  HostSettings *hs = (HostSettings *)user;
  hs->audio = *a;
  hs->audio_live = *a;
  hs->stored.audio = *a;
  printf("HOST_AUDIO COMMIT volume=%d mute=%d mute_when_unfocused=%d\n",
         a->master_volume, a->mute ? 1 : 0, a->mute_when_unfocused ? 1 : 0);
  return settings_persist(hs, "audio", msg, cap);
}

static void settings_describe_video(void *user, const GoofVideoSettings *v,
                                    char *out, size_t cap) {
  HostSettings *hs = (HostSettings *)user;
  if (v->display == GOOF_DISPLAY_FULLSCREEN) {
    SDL_DisplayMode dm;
    int display = SDL_GetWindowDisplayIndex(hs->view->window);
    if (display < 0 || SDL_GetDesktopDisplayMode(display, &dm) != 0) {
      snprintf(out, cap, "FULLSCREEN");
      return;
    }
    GoofRect r = goof_video_dest_rect(v->aspect, v->scaling, dm.w, dm.h);
    snprintf(out, cap, "SCREEN %dX%d  IMAGE %dX%d", dm.w, dm.h, r.w, r.h);
    return;
  }
  int aw, ah, w, h;
  presentation_usable_area(hs->view, &aw, &ah);
  int k = goof_video_resolve_scale(v, aw, ah);
  goof_video_window_size(v->aspect, k, &w, &h);
  if (v->window_scale == GOOF_WINDOW_SCALE_AUTO)
    snprintf(out, cap, "WINDOW %dX%d (AUTO = %dX)", w, h, k);
  else
    snprintf(out, cap, "WINDOW %dX%d", w, h);
}

/* --------------------------------------------------------------------- */

static int run_headless_compare(const Options *o) {
  FILE *record = NULL;
  GoofInputScript script = {0};
  if (o->input_script_path) {
    char err[256];
    if (!goof_input_script_load(o->input_script_path, &script, err, sizeof err)) {
      fprintf(stderr, "goof_recomp: input script: %s\n", err);
      return GOOF_EXIT_SDL;
    }
  }
  if (o->record_path && !(record = fopen(o->record_path, "wb"))) {
    fprintf(stderr, "goof_recomp: cannot write %s\n", o->record_path);
    goof_input_script_free(&script);
    return GOOF_EXIT_SDL;
  }
  GoofAppStatus status;
  GoofAppConfig cfg = {.rom_path = o->rom_path};
  GoofApp *app = goof_app_create(&cfg, &status);
  if (!app) {
    if (record) fclose(record);
    goof_input_script_free(&script);
    return status == GOOF_APP_ERR_ROM ? GOOF_EXIT_ROM : GOOF_EXIT_GUEST_INIT;
  }
  uint64_t epochs = o->epochs ? o->epochs
                              : goof_compare_last_checkpoint_epoch();
  /* See main_headless.c: neutral is a different call, not a NULL argument. */
  int rc = o->input_script_path
      ? goof_compare_run_scripted(app, epochs, o->quiet, record, &script)
      : goof_compare_run(app, epochs, o->quiet, record);
  goof_app_destroy(app);
  goof_input_script_free(&script);
  if (record && fclose(record) != 0) rc = 1;
  return rc;
}

int main(int argc, char **argv) {
  Options o = parse_options(argc, argv);
  setvbuf(stdout, NULL, _IOLBF, 0);

  /* --headless-compare runs the very same coordinator with no window at
   * all.  It goes through goof_compare, exactly as goof_app_headless does. */
  if (o.headless_compare) return run_headless_compare(&o);

  FILE *record = NULL;
  if (o.record_path && !(record = fopen(o.record_path, "wb"))) {
    fprintf(stderr, "goof_recomp: cannot write %s\n", o.record_path);
    return GOOF_EXIT_SDL;
  }

  /* GOOF_INPUT_V1.  The script is parsed and validated BEFORE SDL comes up
   * and before the guest is created: a malformed script must fail as an
   * argument error, never halfway through a run. */
  GoofInputScript script = {0};
  if (o.input_script_path) {
    char err[256];
    if (!goof_input_script_load(o.input_script_path, &script, err, sizeof err)) {
      fprintf(stderr, "goof_recomp: input script: %s\n", err);
      if (record) fclose(record);
      return GOOF_EXIT_SDL;
    }
    printf("GOOF_INPUT_SCRIPT entries=%zu script_digest=%016" PRIx64 "\n",
           script.count, script.digest);
  }

  /* SDL_INIT_VIDEO here; SDL_INIT_AUDIO is a separate subsystem, requested
   * inside audio_open() only when audio presentation is enabled.  A machine
   * with no audio device therefore still runs the video frontend, and the
   * headless targets (which never reach this file) never need one at all.
   *
   * SDL_INIT_GAMECONTROLLER is requested only when a gamepad is wanted, for
   * the same reason: a machine with no pad, or a --no-gamepad run, must not
   * need that subsystem to start.  Its failure is non-fatal -- the keyboard
   * is the guaranteed path (section 17). */
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "goof_recomp: SDL_Init: %s\n", SDL_GetError());
    goof_input_script_free(&script);
    if (record) fclose(record);
    return GOOF_EXIT_SDL;
  }

  HostInput input = {0};
  input.enabled = !o.no_input;
  input.ignore_focus = o.ignore_focus;
  input.focused = true;   /* re-derived from the window flag on the first tick */
  input.script = o.input_script_path ? &script : NULL;
  input.use_gamepad = !o.no_gamepad && !o.input_script_path && input.enabled;
  if (input.use_gamepad && !goof_sdl_pads_open(&input.pads, stdout))
    input.use_gamepad = false;   /* HOST_GAMEPAD UNAVAILABLE already printed */

  /* GOOF_ENHANCEMENTS_E2: host bindings and the Settings overlay exist only
   * for LIVE input.  A scripted or neutral run neither reads config.ini nor
   * reacts to F2, so no user file can touch a deterministic run. */
  goof_bindings_defaults(&input.bindings);
  HostSettings host_settings = {.input = &input};
  bool settings_enabled = input.enabled && !input.script;
  if (settings_enabled) settings_load(&o, &host_settings);
  else printf("Config: not used (%s)\n", input.script ? "--input-script" : "--no-input");
  GoofSdlSettings settings;
  goof_sdl_settings_init(&settings, &input.bindings,
                         (GoofSettingsHost){&host_settings, settings_save,
                                            host_settings.target});

  GoofAppStatus status;
  GoofAppConfig cfg = {.rom_path = o.rom_path};
  GoofApp *app = goof_app_create(&cfg, &status);
  if (!app) {
    goof_sdl_pads_close(&input.pads);
    SDL_Quit();
    goof_input_script_free(&script);
    if (record) fclose(record);
    return status == GOOF_APP_ERR_ROM ? GOOF_EXIT_ROM : GOOF_EXIT_GUEST_INIT;
  }

  /* GOOF_ENHANCEMENTS_E3: effective host video / audio settings.
   * built-in defaults -> config file (live input only) -> command line.
   * A scripted or neutral run reads no config, so it presents exactly as
   * E2 did (windowed, --scale, square pixels) with unity host gain. */
  GoofVideoSettings video_eff;
  GoofAudioSettings audio_eff;
  goof_video_defaults(&video_eff);
  goof_audio_defaults(&audio_eff);
  if (settings_enabled) {
    video_eff = host_settings.stored.video;
    audio_eff = host_settings.stored.audio;
  }
  if (o.scale_set) {
    video_eff.window_scale = o.scale;
    host_settings.scale_from_cli = settings_enabled;
    snprintf(host_settings.video_note, sizeof host_settings.video_note,
             "WINDOW SCALE FROM --SCALE %d (SESSION)", o.scale);
  }
  /* E4: --vsync on|off, a session override exactly like --scale. */
  if (o.vsync_set) {
    video_eff.vsync = o.vsync_on;
    host_settings.vsync_from_cli = settings_enabled;
    if (!o.scale_set)
      snprintf(host_settings.video_note, sizeof host_settings.video_note,
               "VSYNC FROM --VSYNC %s (SESSION)", o.vsync_on ? "ON" : "OFF");
  }

  if (o.filter_set) {
    video_eff.filter = o.filter;
    host_settings.filter_from_cli = settings_enabled;
    if (!o.scale_set && !o.vsync_set)
      snprintf(host_settings.video_note, sizeof host_settings.video_note,
               "FILTER FROM --FILTER (SESSION)");
  }
  Presentation view;
  if (!presentation_create(&view, &video_eff, o.video_fail_fullscreen, 256, 224)) {
    goof_app_destroy(app);
    goof_sdl_pads_close(&input.pads);
    SDL_Quit();
    goof_input_script_free(&script);
    if (record) fclose(record);
    return GOOF_EXIT_SDL;
  }

  presentation_log(&view, "START");
  host_settings.view = &view;
  host_settings.video = view.video;      /* committed = what actually opened */

  HostAudio audio = {0};
  audio.gain_q16 = GOOF_GAIN_UNITY;      /* E3: untouched bytes unless set  */
  audio.requested = !o.no_audio;
  audio.servo = !o.audio_no_servo;
  audio.force_open_failure = o.audio_fail_device;
  audio_open(&audio);
  /* GOOF_AUDIO_PHYSICAL_BOUNDARY_PUMP_V1.  Install the physical-boundary
   * host servicing hook, and ONLY if a device is actually open: a muted or
   * --no-audio run installs nothing, so its guest execution takes exactly
   * the same path it took before this milestone existed.  That is what keeps
   * the audio-on / audio-off and headless / frontend equalities meaningful
   * rather than merely asserted.
   *
   * `audio.app` is the handle the hook uses to reach the drain seam.  It is
   * the only place this frontend hands a guest-owning pointer to something
   * that runs INSIDE goof_app_step, and everything it may do with it is
   * spelled out in goof_app.h: observe already-produced PCM, never advance,
   * never block. */
  if (audio.open) {
    audio.app = app;
    goof_app_set_boundary_hook(app, audio_boundary_hook, &audio);
  }
  /* DIAGNOSTIC sinks, opened after the device so a muted run writes an empty
   * (header-only) trace rather than a misleading one. */
  if (o.audio_trace_path && audio.open) {
    audio.trace = fopen(o.audio_trace_path, "w");
    if (audio.trace) {
      fprintf(audio.trace,
              "tick\twall_ms\tepoch\tperiods\tspc_cycles\td_spc\t"
              "pcm_produced_total\tpcm_produced_tick\tring_dropped_tick\t"
              "ring_dropped_audible_tick\tavail_before\tceiling\t"
              "retired\tdrained\tavail_after\thost_out\tratio\tfill_ema\t"
              "servo_error\tqueued_before\tqueued_after\tprimed_before\t"
              "primed_after\tunderflow\tresync\tretired_total\t"
              "max_periods_per_pump\t"
              /* GOOF_AUDIO_PHYSICAL_BOUNDARY_PUMP_V1 */
              "boundary_drains\tboundary_harvested\tboundary_queued\t"
              "fifo_frames\tfifo_peak\tfifo_overflow\n");
      printf("AUDIO_TRACE OPEN path=%s units=native_frames,host_frames,"
             "spc_cycles,physical_periods\n", o.audio_trace_path);
    } else {
      printf("AUDIO_TRACE FAILED path=%s\n", o.audio_trace_path);
    }
  }
  if (o.audio_capture_path && audio.open) {
    audio.cap = fopen(o.audio_capture_path, "wb");
    printf("AUDIO_CAPTURE %s path=%s format=s16le channels=2 rate=%d\n",
           audio.cap ? "OPEN" : "FAILED", o.audio_capture_path,
           audio.open ? audio.have.freq : 0);
  }

  /* E3: host audio settings (gain only) and the Video / Audio pages. */
  host_settings.out = &audio;
  host_settings.audio = audio_eff;
  host_settings.audio_live = audio_eff;
  printf("HOST_AUDIO START volume=%d mute=%d mute_when_unfocused=%d "
         "device=default latency=fixed\n", audio_eff.master_volume,
         audio_eff.mute ? 1 : 0, audio_eff.mute_when_unfocused ? 1 : 0);
  settings_update_gain(&host_settings, true);
  if (settings_enabled)
    goof_menu_attach_host_pages(&settings.menu, (GoofHostPages){
        .user = &host_settings,
        .video = &host_settings.video,
        .audio = &host_settings.audio,
        .preview_video = settings_preview_video,
        .preview_audio = settings_preview_audio,
        .save_video = settings_save_video,
        .save_audio = settings_save_audio,
        .describe_video = settings_describe_video,
        .video_note = host_settings.video_note,
    });

  /* GOOF_ENHANCEMENTS_E4: the SIMULATION clock.  Canonical cadence unless the
   * --present-hz test seam overrides it (the gates' 30 / 60 / 120 Hz runs). */
  uint64_t freq = SDL_GetPerformanceFrequency();
  uint64_t pnum, pden;
  if (o.present_hz > 0.0) goof_pacer_period_for_hz(o.present_hz, &pnum, &pden);
  else goof_pacer_canonical_period(&pnum, &pden);
  GoofPacer pacer;
  uint64_t present_hold_capped = 0;   /* gate requires this to stay 0 */

  bool running = true, paused = false, have_frame = false;
  bool window_hidden = false;
  bool held = false;            /* paused or Settings open: pacer held       */
  bool force_present = true;    /* a redraw is owed (resize, expose, menu)   */
  int exit_code = 0;
  uint64_t presented = 0, suppressed = 0;
  uint64_t pause_resume_at = 0;   /* performance counter, V9 seam only */
  uint64_t debug_last_counter = SDL_GetPerformanceCounter();
  uint64_t debug_last_presented = 0;
  /* Expectation window: from the 2nd step (the clock is anchored after the
   * first, boot-loader, step -- goof_pacer_step_finished) to the last one. */
  uint64_t first_step_at = 0, last_step_at = 0, periods_before_last = 0, periods_at_first = 0;
  uint64_t held_before_last = 0, dropped_before_last = 0;
  bool stall_done = false;
  GoofAppDiag diag = {0};

  PresentObs pobs = {0};
  pobs.emulate_hz = o.emulate_refresh_hz;
  if (o.pacing_trace_path) {
    pobs.trace = fopen(o.pacing_trace_path, "w");
    if (pobs.trace)
      fprintf(pobs.trace, "kind\tt_ns\tepoch\tperiods\tlag_ns\titer_steps\t"
                          "present_ns\tvblank\tnew_frame\n");
    printf("PACING_TRACE %s path=%s (diagnostic)\n", pobs.trace ? "OPEN" : "FAILED",
           o.pacing_trace_path);
  }

  const char *input_mode = !input.enabled ? "NEUTRAL"
      : (input.script ? "SCRIPT"
                      : (input.use_gamepad ? "KEYBOARD+GAMEPAD" : "KEYBOARD"));
  goof_pacer_init(&pacer, freq, pnum, pden, host_now());
  pobs.trace_origin = pobs.emulate_origin = host_now();
  uint64_t period_ticks = goof_pacer_period_ticks(&pacer);
  uint64_t ui_next = host_now();
  printf("GOOF_RECOMP START rom=%s scale=%d present_hz=%.3f epochs=%" PRIu64
         " vsync=%s epochs_per_step=%d guest_input=%s host_audio=%s"
         " pacing=%s threads=1\n", o.rom_path, view.window_scale,
         goof_pacer_rate_hz(&pacer), o.epochs, view.vsync ? "ON" : "OFF",
         GOOF_EPOCHS_PER_STEP, input_mode,
         audio.open ? "PUSH_SDL_QUEUE" : (o.no_audio ? "DISABLED" : "MUTED"),
         o.pace_epoch ? "epoch" : "physical");
  printf("GOOF_PACING START sim_clock=%s sim_hz=%.6f period=%" PRIu64 "/%" PRIu64
         "_s period_ms=%.6f timer=SDL_GetPerformanceCounter timer_hz=%" PRIu64
         " present_policy=new_guest_frame repeat=display_holds_last_frame"
         " interpolation=none max_steps_per_iteration=%d resync_lag_periods=%d"
         " vsync=%s renderer=%s emulated_refresh_hz=%.3f\n",
         o.present_hz > 0.0 ? "override(--present-hz)" : "canonical_ntsc_357368mclk",
         goof_pacer_rate_hz(&pacer), pnum, pden, 1000.0 * (double)pnum / (double)pden,
         freq, GOOF_PACER_MAX_STEPS_PER_ITERATION, GOOF_PACER_RESYNC_LAG_PERIODS,
         view.vsync ? "on" : "off", view.renderer_name, o.emulate_refresh_hz);
  presentation_log_display(&view, "START");
  if (o.stall_ms > 0.0)
    printf("HOST_STALL_SEAM armed epoch=%" PRIu64 " ms=%.1f (diagnostic)\n",
           o.stall_at, o.stall_ms);
  if (!o.quiet) input_help(stdout);

  while (running) {
    /* 1. Drain every pending host event.  HOST CONTROLS ONLY: nothing here
     *    reaches the guest.  Window visibility is deliberately not consulted
     *    for the guest -- minimising or obscuring the window cannot change
     *    guest execution (design section 8, rule R4); it only lets the host
     *    skip presents nobody can see. */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      /* 1a. Host Settings first (GOOF_ENHANCEMENTS_E2).  F2 opens it; while
       *     it is open it takes every key and pad event, including P and
       *     ESC, so nothing typed into it can pause, quit or reach the game.
       *     Quit, window and pad hot-plug events always pass through. */
      if (settings_enabled) {
        unsigned f = goof_sdl_settings_event(&settings, &ev);
        if (f & GOOF_MENU_OPENED) {
          /* Same stop as P: no epoch runs, so no input is latched and no
           * guest time passes; the device stops so the queued tail does not
           * play over a frozen picture (the queue is kept, not grown). */
          if (audio.open) SDL_PauseAudioDevice(audio.dev, 1);
          printf("HOST_SETTINGS OPEN epoch=%" PRIu64 " logical=%016" PRIx64
                 " fb=%016" PRIx64 " spc_cycles=%" PRIu64 " apu_ram=%016" PRIx64
                 " audio_queued=%u fifo=%u\n", diag.epoch, diag.logical_hash,
                 diag.framebuffer_hash, diag.spc_cycles, diag.apu_ram_hash,
                 audio_queued_frames(&audio), audio.fifo.count);
        }
        if (f & GOOF_MENU_CLOSED) {
          /* Controls still held from the menu (the Enter that chose RESUME,
           * a pad button) stay hidden from the guest until released.  The
           * pacer re-anchors when the hold ends below (R5: zero catch-up,
           * however long the overlay was open). */
          GoofHostInputFrame held_keys;
          input_read_frame(&input, &held_keys);
          goof_input_guard_arm(&input.guard, &held_keys);
          if (audio.open && audio.primed && !paused)
            SDL_PauseAudioDevice(audio.dev, 0);
          printf("HOST_SETTINGS CLOSE epoch=%" PRIu64 " logical=%016" PRIx64
                 " fb=%016" PRIx64 " spc_cycles=%" PRIu64 " apu_ram=%016" PRIx64
                 " audio_queued=%u fifo=%u unsaved_discarded=%d held_guarded=%d\n",
                 diag.epoch, diag.logical_hash, diag.framebuffer_hash,
                 diag.spc_cycles, diag.apu_ram_hash, audio_queued_frames(&audio),
                 audio.fifo.count, settings.menu.discarded_on_close ? 1 : 0,
                 input.guard.active ? 1 : 0);
        }
        if (f) force_present = true;           /* the overlay changed */
        if (f & GOOF_MENU_CONSUMED) continue;
      }
      if (ev.type == SDL_QUIT) {
        running = false;
      } else if (ev.type == SDL_KEYDOWN && ev.key.repeat == 0) {
        SDL_Keycode key = ev.key.keysym.sym;
        if (key == SDLK_ESCAPE) {
          running = false;
        } else if (key == SDLK_p) {
          paused = !paused;
          /* The pacer re-anchors when the hold ends below (R5: zero
           * catch-up).  The guest stops, so production stops; the device
           * stops too, so the queued tail cannot keep playing over a frozen
           * picture.  The queue itself is RETAINED (it is guest PCM already
           * produced and not yet heard) -- at the ~49 ms target that tail is
           * one block, not hundreds of milliseconds.  On resume, audio_pump
           * re-prerolls by itself if the device drained below the preroll
           * mark. */
          if (audio.open) SDL_PauseAudioDevice(audio.dev, paused ? 1 : 0);
          printf("HOST_PAUSE %s epoch=%" PRIu64 " logical=%016" PRIx64
                 " spc_pc=%04X spc_cycles=%" PRIu64 "\n",
                 paused ? "ENTER" : "LEAVE", diag.epoch, diag.logical_hash,
                 diag.spc_pc, diag.spc_cycles);
        }
      } else if (ev.type == SDL_WINDOWEVENT) {
        if (ev.window.event == SDL_WINDOWEVENT_CLOSE) {
          running = false;
        } else if (ev.window.event == SDL_WINDOWEVENT_EXPOSED ||
                   ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                   ev.window.event == SDL_WINDOWEVENT_RESTORED) {
          /* E4: presents follow new guest frames, so a window that needs
           * repainting between two of them (a long multi-period hold, a
           * resize) asks for one host-only redraw of the SAME frame. */
          force_present = true;
        }
#if SDL_VERSION_ATLEAST(2, 0, 18)
        if (ev.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED)
          presentation_log_display(&view, "CHANGED");
#endif
      } else if (ev.type == SDL_RENDER_TARGETS_RESET ||
                 ev.type == SDL_RENDER_DEVICE_RESET) {
        /* GOOF_ENHANCEMENTS_E3: a fullscreen switch may reset the render
         * device (Direct3D).  Recreate what it owned and re-upload the LAST
         * completed framebuffer (read-only; the guest is not stepped), so a
         * paused picture does not vanish.  E4: a Direct3D 9 vsync change
         * (SDL_RenderSetVSync resets the device) lands here too. */
        if (ev.type == SDL_RENDER_DEVICE_RESET) {
          if (view.texture) SDL_DestroyTexture(view.texture);
          view.texture = SDL_CreateTexture(view.renderer, SDL_PIXELFORMAT_XRGB8888,
                                           SDL_TEXTUREACCESS_STREAMING,
                                           view.source_width, view.source_height);
          goof_sdl_settings_destroy(&settings);   /* font atlas: rebuilt lazily */
        }
        uint32_t fw, fh, fpitch;
        const uint32_t *fpx = goof_app_framebuffer(app, &fw, &fh, &fpitch);
        if (view.texture && fpx) SDL_UpdateTexture(view.texture, NULL, fpx, (int)fpitch);
        printf("HOST_VIDEO RENDER_RESET device=%d texture=%s\n",
               ev.type == SDL_RENDER_DEVICE_RESET ? 1 : 0,
               view.texture ? "OK" : "LOST");
        if (!view.texture) { exit_code = GOOF_EXIT_SDL; running = false; }
        force_present = true;
      } else if (ev.type == SDL_CONTROLLERDEVICEADDED ||
                 ev.type == SDL_CONTROLLERDEVICEREMOVED) {
        /* Host state only: which pad owns which slot.  Idempotent on the
         * ADDED events SDL queues at start-up for pads already adopted. */
        if (input.use_gamepad) goof_sdl_pads_handle_event(&input.pads, &ev);
      }
    }
    if (!running) break;

    /* 1b. Re-derive keyboard focus from SDL's own live window flag.
     *
     * HOST behaviour, not guest state: while another window owns the keyboard
     * the guest is presented NEUTRAL, so a key that was down when focus went
     * away cannot keep walking the character.  The guest keeps RUNNING, at
     * the same canonical cadence -- losing focus must not change guest
     * execution any more than minimising the window does (rule R4) -- it
     * simply stops receiving buttons.
     *
     * Polled rather than tracked through FOCUS_LOST / FOCUS_GAINED events: a
     * window manager that never delivers FOCUS_GAINED would otherwise leave
     * the pad dead for the whole session, and a dead pad is a much worse
     * failure than a stuck one.  --ignore-focus disables the gate entirely
     * for the unusual setups where even the flag is wrong. */
    Uint32 wflags = SDL_GetWindowFlags(view.window);
    {
      bool now_focused = (wflags & SDL_WINDOW_INPUT_FOCUS) != 0;
      if (now_focused != input.focused) {
        input.focused = now_focused;
        printf("HOST_FOCUS %s epoch=%" PRIu64 "\n",
               now_focused ? "GAINED" : "LOST", diag.epoch);
      }
      /* E3: host gain follows volume / mute / mute-when-unfocused.  Host
       * output only; the guest and its PCM digest never see it. */
      settings_update_gain(&host_settings, now_focused);
    }

    /* V9 seam: a scripted host pause, entered through the same state the P
     * key sets and left the same way.  No guest call happens meanwhile. */
    bool menu_open = goof_menu_is_open(&settings.menu);
    if (pause_resume_at && SDL_GetPerformanceCounter() >= pause_resume_at) {
      pause_resume_at = 0;
      paused = false;
      if (audio.open && audio.primed && !menu_open)
        SDL_PauseAudioDevice(audio.dev, 0);
      printf("HOST_PAUSE LEAVE epoch=%" PRIu64 " logical=%016" PRIx64
             " fb=%016" PRIx64 " spc_pc=%04X spc_cycles=%" PRIu64
             " apu_ram=%016" PRIx64 "\n", diag.epoch, diag.logical_hash,
             diag.framebuffer_hash, diag.spc_pc, diag.spc_cycles,
             diag.apu_ram_hash);
    }

    /* 1c. Hold / resume the SIMULATION clock.  P and the Settings overlay
     *     hold the guest; leaving the hold re-anchors the pacer to now, so a
     *     pause of any length -- 30 s in Settings, a minute on P -- resumes
     *     with the very next step and no catch-up (rule R5). */
    {
      uint64_t now = host_now();
      bool held_now = paused || menu_open;
      if (held_now != held) {
        if (held_now) goof_pacer_hold(&pacer, now);
        else goof_pacer_resume(&pacer, now);
        held = held_now;
        ui_next = now;
        force_present = true;
        pobs.last_frame_present = 0;   /* intervals never span a hold */
      }
    }

    /* 2. Advance the guest -- or, when held, do not.  While held no
     *    FramePlan is built, no NMI is requested, no APU catchup occurs and
     *    no timing debt is accumulated; the last completed texture is simply
     *    re-presented below.
     *
     *    Every due step runs here, at most GOOF_PACER_MAX_STEPS_PER_ITERATION
     *    of them before the next presentation opportunity.  Normally that is
     *    exactly one step per canonical period; after a short host delay (a
     *    present that blocked on vblank, a hiccup) it is the missed steps
     *    back to back, so the guest's long-term rate stays canonical at any
     *    display refresh.  A present is never a reason to step. */
    bool new_frame = false, finished = false;
    unsigned iter_steps = 0;
    if (!held) {
      goof_pacer_begin_iteration(&pacer);
      uint64_t started;
      while (running && !paused && goof_pacer_step_due(&pacer, started = host_now())) {
        for (int i = 0; i < GOOF_EPOCHS_PER_STEP; i++) {
          /* THE SEAM.  Host physical state becomes a guest-visible sample
           * here and nowhere else, once per guest step (never per present),
           * indexed by the GUEST's next epoch number.  What crosses is two
           * 12-bit masks; the SDL event that produced them, and the moment it
           * arrived, stay on this side of the call. */
          GoofInputSample in = input_sample(&input, goof_app_epoch(app) + 1);
          GoofAppStatus s = goof_app_step(app, &in, &diag);
          if (s != GOOF_APP_OK) {
            fprintf(stderr, "goof_recomp: %s: %s\n", goof_app_status_text(s),
                    goof_app_last_error(app));
            exit_code = GOOF_EXIT_STEP;
            running = false;
            break;
          }
          s = goof_app_render(app, &diag);
          if (s != GOOF_APP_OK) {
            fprintf(stderr, "goof_recomp: %s: %s\n", goof_app_status_text(s),
                    goof_app_last_error(app));
            exit_code = GOOF_EXIT_RENDER;
            running = false;
            break;
          }
          if (record) goof_compare_write_record(record, &diag);
        }
        if (!running) break;

        /* 2b. Consume the PCM that advance produced.  AFTER the guest, never
         *     before it and never instead of it: nothing in audio_pump can
         *     advance, pace or block goof_app_step, and skipping it entirely
         *     (--no-audio) leaves every guest quantity bit-identical.  That
         *     is gate PRESENT1. */
        audio_pump(&audio, app, diag.epoch, &diag);

        /* 2c. Tell the pacer how much guest time that step was.  `physical`
         *     pacing: the epoch's physical periods (a GUEST quantity), so a
         *     16-period transition occupies 16 canonical periods of wall
         *     time and its music is neither skipped nor delayed; `epoch`
         *     (legacy A/B): one period whatever it contained. */
        uint64_t periods = o.pace_epoch ? 1 : diag.physical_periods;
        if (periods > GOOF_PRESENT_MAX_HOLD_PERIODS) present_hold_capped++;
        if (pacer.st.steps == 1 && !first_step_at) {                           /* 2nd step */
          first_step_at = started;
          periods_at_first = pacer.st.periods;
        }
        last_step_at = started;
        periods_before_last = pacer.st.periods;
        held_before_last = pacer.st.held_ticks;
        dropped_before_last = pacer.st.resync_dropped_ticks;
        goof_pacer_stepped(&pacer, started, periods);
        goof_pacer_step_finished(&pacer, started, host_now());
        iter_steps++;
        new_frame = true;
        if (pobs.trace)
          fprintf(pobs.trace, "S\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%.0f\t%u\t\t\t\n",
                  (uint64_t)((double)(started - pobs.trace_origin) * 1e9 / (double)freq),
                  diag.epoch, periods,
                  (double)pacer.last_lag * 1e9 / (double)freq, iter_steps);

        if (o.pause_at && diag.epoch == o.pause_at && !pause_resume_at) {
          paused = true;
          pause_resume_at = SDL_GetPerformanceCounter() +
              (uint64_t)(o.pause_seconds * (double)freq);
          if (audio.open) SDL_PauseAudioDevice(audio.dev, 1);
          printf("HOST_PAUSE ENTER epoch=%" PRIu64 " logical=%016" PRIx64
                 " fb=%016" PRIx64 " spc_pc=%04X spc_cycles=%" PRIu64
                 " apu_ram=%016" PRIx64 " seconds=%.3f\n", diag.epoch,
                 diag.logical_hash, diag.framebuffer_hash, diag.spc_pc,
                 diag.spc_cycles, diag.apu_ram_hash, o.pause_seconds);
        }
        /* DIAGNOSTIC seam: a one-off host stall (window drag, compositor,
         * debugger) between two guest steps.  Host only. */
        if (o.stall_ms > 0.0 && !stall_done && diag.epoch == o.stall_at) {
          stall_done = true;
          uint64_t t0 = host_now();
          host_sleep_until(t0 + (uint64_t)(o.stall_ms * (double)freq / 1000.0), freq);
          printf("HOST_STALL epoch=%" PRIu64 " requested_ms=%.1f actual_ms=%.3f "
                 "(diagnostic seam)\n", diag.epoch, o.stall_ms,
                 (double)(host_now() - t0) * 1000.0 / (double)freq);
        }
        if (o.epochs && diag.epoch >= o.epochs) { finished = true; break; }
      }
      if (!running) break;

      if (new_frame) {
        /* Only the LATEST completed frame is uploaded; a frame a catch-up
         * step replaced before any present is simply never shown (its hash
         * is in the record like every other). */
        pobs.frames_superseded += iter_steps - 1;
        uint32_t w, h, pitch;
        const uint32_t *pixels = goof_app_framebuffer(app, &w, &h, &pitch);
        if (pixels) {
          if (SDL_UpdateTexture(view.texture, NULL, pixels, (int)pitch) != 0) {
            fprintf(stderr, "goof_recomp: SDL_UpdateTexture: %s\n",
                    SDL_GetError());
            exit_code = GOOF_EXIT_SDL;
            break;
          }
          have_frame = true;
        }
      } else {
        /* No step was due: keep the device fed while a multi-period epoch
         * is held (what the pre-E4 hold loop did between its intervals). */
        audio_service(&audio);
      }
    }

    /* 3. Present.  A suppressed, hidden or failed presentation is a host
     *    event and must not touch the guest, so the loop simply continues.
     *
     *    Policy: present when there is a NEW guest frame; while held, at the
     *    UI cadence (one canonical period) or when the overlay changed; and
     *    once more whenever the window needs repainting.  Between new guest
     *    frames the display itself keeps showing the last one -- at 120, 144
     *    or 240 Hz that IS the frame repetition, done by the display, with no
     *    extra guest step, render or input sample.  Presenting only on new
     *    frames also lets a VRR display refresh at the game's own 60.0985 Hz.
     *
     *    V8 seam: over the requested epoch range the window is genuinely
     *    minimised through SDL and presentation is skipped.  Guest execution
     *    is deliberately NOT conditioned on window visibility anywhere in
     *    this file, so this changes only what the host does. */
    bool suppress = o.suppress_to && diag.epoch >= o.suppress_from &&
                    diag.epoch <= o.suppress_to;
    if (suppress != window_hidden) {
      window_hidden = suppress;
      if (suppress) SDL_MinimizeWindow(view.window);
      else SDL_RestoreWindow(view.window);
      printf("HOST_WINDOW %s epoch=%" PRIu64 " logical=%016" PRIx64 "\n",
             suppress ? "MINIMIZED" : "RESTORED", diag.epoch,
             diag.logical_hash);
    }
    uint64_t now = host_now();
    bool ui_due = held && now >= ui_next;
    bool want = new_frame || ui_due || force_present;
    bool visible = (wflags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) == 0;
    if (want && (have_frame || menu_open) && !suppress && visible) {
      uint64_t t0 = host_now();
      presentation_draw(&view, have_frame, menu_open ? &settings : NULL);
      uint64_t t1 = host_now();
      uint64_t vblank = 0;
      if (pobs.emulate_hz > 0.0) {
        vblank = emulate_vblank_wait(&pobs, freq);
        if (new_frame && pobs.have_vblank) {
          /* refreshes the PREVIOUS frame stayed up; a multi-period epoch's
           * frame is meant to stay up for all its periods, so it is counted
           * apart instead of being called a "repeat" */
          uint64_t d = vblank - pobs.last_vblank;
          if (pobs.last_frame_periods <= 1)
            pobs.repeat_hist[d < GOOF_REPEAT_BINS - 1 ? d : GOOF_REPEAT_BINS - 1]++;
          else
            pobs.multi_period_frames++;
        }
        if (new_frame) {
          pobs.last_vblank = vblank;
          pobs.have_vblank = true;
          pobs.last_frame_periods = o.pace_epoch ? 1 : diag.physical_periods;
        }
        t1 = host_now();
      }
      presented++;
      pobs.presents++;
      goof_series_add(&pobs.present_call, (double)(t1 - t0) / (double)freq);
      if (new_frame) {
        pobs.frame_presents++;
        if (pobs.last_frame_present)
          goof_series_add(&pobs.frame_interval,
                          (double)(t1 - pobs.last_frame_present) / (double)freq);
        pobs.last_frame_present = t1;
      } else {
        pobs.ui_presents++;
      }
      if (pobs.trace)
        fprintf(pobs.trace, "P\t%" PRIu64 "\t%" PRIu64 "\t\t\t%u\t%" PRIu64 "\t%" PRIu64
                "\t%d\n", (uint64_t)((double)(t0 - pobs.trace_origin) * 1e9 / (double)freq),
                diag.epoch, iter_steps,
                (uint64_t)((double)(t1 - t0) * 1e9 / (double)freq), vblank,
                new_frame ? 1 : 0);
      force_present = false;
      if (ui_due) ui_next = (now - ui_next > period_ticks) ? now + period_ticks
                                                           : ui_next + period_ticks;
    } else if (want && suppress) {
      suppressed++;
    } else if (want && !visible) {
      pobs.hidden_skipped++;
    }

    if (o.debug && (diag.epoch % 100) == 0 && diag.epoch != 0 && !held &&
        new_frame) {
      uint64_t tnow = SDL_GetPerformanceCounter();
      double seconds = (double)(tnow - debug_last_counter) / (double)freq;
      double hz = seconds > 0.0
          ? (double)(presented - debug_last_presented) / seconds : 0.0;
      printf("epoch=%" PRIu64 " logical=%016" PRIx64 " fb=%016" PRIx64
             " present_hz=%.1f spc_pc=%04X spc_cycles=%" PRIu64
             " aq_frames=%u aq_ms=%.1f ratio=%.5f under=%" PRIu64
             " resync=%" PRIu64 " sim_catchup=%" PRIu64 " sim_resyncs=%" PRIu64
             " sim_hz=%.4f\n",
             diag.epoch, diag.logical_hash, diag.framebuffer_hash, hz,
             diag.spc_pc, diag.spc_cycles, audio_queued_frames(&audio),
             audio.open ? 1000.0 * audio_queued_frames(&audio) / audio.have.freq
                        : 0.0,
             audio.ratio, audio.underflows, audio.resyncs,
             pacer.st.catchup_steps, pacer.st.resyncs, goof_pacer_measured_hz(&pacer));
      debug_last_counter = tnow;
      debug_last_presented = presented;
    }

    if (finished) running = false;
    if (!running) break;

    /* 4. Wait.  Host time only: until the next guest deadline (or the next
     *    UI redraw while held), capped at one period so the device and the
     *    window are serviced at least that often, and cut short by any host
     *    event.  When a step is still due (the per-iteration clamp deferred
     *    it) the deadline is already past and this returns at once. */
    {
      uint64_t target = held ? ui_next : goof_pacer_deadline(&pacer);
      uint64_t cap = host_now() + period_ticks;
      if (target > cap) target = cap;
      host_wait_until(target, freq);
    }
  }

  printf("GOOF_RECOMP END epoch=%" PRIu64 " logical=%016" PRIx64
         " fb=%016" PRIx64 " spc_pc=%04X spc_cycles=%" PRIu64
         " apu_ram=%016" PRIx64 " presented=%" PRIu64
         " presentation_suppressed=%" PRIu64 "\n", diag.epoch,
         diag.logical_hash, diag.framebuffer_hash, diag.spc_pc,
         diag.spc_cycles, diag.apu_ram_hash, presented, suppressed);

  /* AUDIO1/AUDIO8: the canonical native-PCM digest, taken at the producer
   * (every (L,R) pair dsp_cycle emits, before the output ring's overflow
   * check) and therefore upstream of any host queue, resampler or device.
   * Printed here so the headless gate and the frontend can be compared on
   * the SAME number while neither has an audio device. */
  { AudioTraceStats _ats; audio_trace_get_stats(&_ats);
    printf("AUDIO1_NATIVE_PCM hash=%016" PRIx64 " frames=%" PRIu64 "\n",
           _ats.pcm_hash, _ats.pcm_frames); }
  /* GOOF_INPUT_DIGEST_V1 over every guest-visible latch of the run, plus the
   * two host-side observations that are NOT part of it.  For a scripted run
   * this line is the pinnable result; for a live run it is a record of what
   * the player actually did. */
  printf("GOOF_INPUT_SUMMARY mode=%s latches=%" PRIu64 " digest=%016" PRIx64
         " nonneutral_epochs=%" PRIu64 " focus_cleared_epochs=%" PRIu64
         " p1=%03X p2=%03X\n", input_mode, diag.input_latches,
         diag.input_digest, input.nonneutral_epochs, input.focus_clears,
         diag.input_p1, diag.input_p2);
  printf("GOOF_PRESENT_PACING mode=%s max_hold_periods=%d hold_capped=%"
         PRIu64 "\n", o.pace_epoch ? "epoch" : "physical",
         GOOF_PRESENT_MAX_HOLD_PERIODS, present_hold_capped);
  /* GOOF_ENHANCEMENTS_E4: the three clocks, measured.  Host observation
   * only, printed after the run; nothing above depended on any of it.
   *   expected_periods  canonical periods that fit in the un-held,
   *                     un-dropped host time from the first to the last step
   *   period_error      periods actually stepped before the last step minus
   *                     that expectation (a long-term drift would grow it)
   *   measured_hz       least-squares rate of step start times against the
   *                     period index over the last clean segment */
  {
    const GoofPacerStats *ps = &pacer.st;
    double run_ticks = 0.0;
    if (last_step_at > first_step_at)
      run_ticks = (double)(last_step_at - first_step_at) - (double)held_before_last -
                  (double)dropped_before_last;
    double expected = goof_pacer_expected_periods(&pacer, run_ticks > 0 ? (uint64_t)run_ticks : 0);
    double err = (double)(periods_before_last - periods_at_first) - expected;
    printf("GOOF_PACING sim_hz=%.6f steps=%" PRIu64 " periods=%" PRIu64
           " run_s=%.3f expected_periods=%.2f period_error=%.3f drift_ppm=%.2f"
           " measured_hz=%.6f rate_error_ppm=%.2f segments=%" PRIu64
           " catchup_steps=%" PRIu64 " max_steps_in_iteration=%" PRIu64
           " budget_deferrals=%" PRIu64 " resyncs=%" PRIu64 " resync_dropped_ms=%.1f"
           " boot_anchor_ms=%.1f"
           " reanchors=%" PRIu64 " held_ms=%.1f capped_epochs=%" PRIu64
           " max_lag_ms=%.3f\n",
           goof_pacer_rate_hz(&pacer), ps->steps, ps->periods, run_ticks / (double)freq,
           expected, err, expected > 0 ? err / expected * 1e6 : 0.0,
           goof_pacer_measured_hz(&pacer), goof_pacer_rate_error_ppm(&pacer),
           ps->segments, ps->catchup_steps, ps->max_steps_in_iteration,
           ps->budget_deferrals, ps->resyncs,
           1000.0 * (double)ps->resync_dropped_ticks / (double)freq,
           1000.0 * (double)ps->boot_anchor_ticks / (double)freq, ps->reanchors,
           1000.0 * (double)ps->held_ticks / (double)freq, ps->capped_epochs,
           1000.0 * (double)ps->max_lag_ticks / (double)freq);
    printf("GOOF_PACING_STEP lag_mean_ms=%.4f lag_sd_ms=%.4f lag_max_ms=%.4f"
           " interval_per_period_mean_ms=%.4f sd_ms=%.4f min_ms=%.4f max_ms=%.4f n=%" PRIu64 "\n",
           1000.0 * ps->lag.mean, 1000.0 * goof_series_sd(&ps->lag), 1000.0 * ps->lag.max,
           1000.0 * ps->step_interval.mean, 1000.0 * goof_series_sd(&ps->step_interval),
           1000.0 * ps->step_interval.min, 1000.0 * ps->step_interval.max,
           ps->step_interval.n);
    printf("GOOF_PACING_PRESENT vsync=%s renderer=%s display_refresh_hz=%d"
           " emulated_refresh_hz=%.3f presents=%" PRIu64 " new_frame=%" PRIu64
           " ui=%" PRIu64 " hidden_skipped=%" PRIu64 " frames_superseded=%" PRIu64
           " frame_interval_mean_ms=%.4f sd_ms=%.4f min_ms=%.4f max_ms=%.4f"
           " present_call_mean_ms=%.4f max_ms=%.4f vsync_changes=%" PRIu64
           " vsync_refused=%" PRIu64 "\n",
           view.vsync ? "on" : "off", view.renderer_name, presentation_refresh_hz(&view),
           o.emulate_refresh_hz, pobs.presents, pobs.frame_presents, pobs.ui_presents,
           pobs.hidden_skipped, pobs.frames_superseded,
           1000.0 * pobs.frame_interval.mean, 1000.0 * goof_series_sd(&pobs.frame_interval),
           1000.0 * pobs.frame_interval.min, 1000.0 * pobs.frame_interval.max,
           1000.0 * pobs.present_call.mean, 1000.0 * pobs.present_call.max,
           view.vsync_changes, view.vsync_refused);
    if (o.emulate_refresh_hz > 0.0) {
      uint64_t n = 0, sum = 0;
      printf("GOOF_PACING_REPEAT emulated_refresh_hz=%.3f refreshes_per_guest_frame",
             o.emulate_refresh_hz);
      for (int k = 0; k < GOOF_REPEAT_BINS; k++)
        if (pobs.repeat_hist[k]) {
          printf(" %d%s=%" PRIu64, k, k == GOOF_REPEAT_BINS - 1 ? "+" : "", pobs.repeat_hist[k]);
          n += pobs.repeat_hist[k];
          sum += (uint64_t)k * pobs.repeat_hist[k];
        }
      printf(" mean=%.4f ideal=%.4f multi_period_frames=%" PRIu64 " superseded=%" PRIu64 "\n",
             n ? (double)sum / (double)n : 0.0,
             o.emulate_refresh_hz / goof_pacer_rate_hz(&pacer), pobs.multi_period_frames,
             pobs.frames_superseded);
    }
  }
  if (pobs.trace) fclose(pobs.trace);
  audio_report(&audio);
  /* DIAGNOSTIC: the canonical native PCM the GUEST produced, straight out of
   * the always-on trace ring -- the A/B partner of --audio-capture, which
   * holds what the HOST actually queued.  Read-only; it is taken after the
   * run and changes nothing. */
  if (o.audio_native_wav_path) {
    uint64_t s0 = 0, sn = 0;
    int rc = audio_trace_dump_wav(o.audio_native_wav_path, -1, 0, &s0, &sn);
    printf("AUDIO_NATIVE_WAV %s path=%s start_frame=%" PRIu64 " frames=%" PRIu64
           "\n", rc == 0 ? "OK" : "FAILED", o.audio_native_wav_path, s0, sn);
  }
  /* Ordering: stop and close the device, release the host buffers that hold
   * copies of its PCM, then the window, then SDL, and only then the core
   * that produced the PCM.  audio_close must precede goof_app_destroy --
   * nothing may still be able to reach the DSP once the app is gone. */
  /* Remove the hook BEFORE anything it reaches is torn down, so no boundary
   * can be notified into freed host state. */
  goof_app_set_boundary_hook(app, NULL, NULL);
  audio.app = NULL;
  audio_close(&audio);
  goof_sdl_pads_close(&input.pads);
  goof_sdl_settings_destroy(&settings);
  presentation_destroy(&view);
  SDL_Quit();
  goof_app_destroy(app);
  goof_input_script_free(&script);
  if (record && fclose(record) != 0 && exit_code == 0) exit_code = 1;
  return exit_code;
}
