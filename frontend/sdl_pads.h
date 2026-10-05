#ifndef GOOF_SDL_PADS_H
#define GOOF_SDL_PADS_H

/* Goof Troop Recomp -- SDL2 GameController adapter for the player slots.
 * GOOF_ENHANCEMENTS_E1_MODERN_CONTROLLER.
 *
 * The SDL half of the pad path: opens and closes SDL_GameController handles,
 * translates SDL device events into goof_controller.h ownership calls keyed
 * by SDL_JoystickID (the runtime instance id), and reads each slot's pad into
 * a GoofHostPadState.  No mapping policy lives here (goof_host_input.h) and
 * no ownership policy either (goof_controller.h).
 *
 * Linked only into goof_recomp and the frontend host tests -- never into
 * goof_app_headless or a phase4 gate, so no headless path enumerates a pad.
 */

#include <stdbool.h>
#include <stdio.h>

#include <SDL.h>

#include "host/goof_controller.h"
#include "host/goof_host_input.h"

typedef struct {
  bool active;                  /* subsystem up, devices tracked            */
  GoofControllerManager slots;
  FILE *log;                    /* HOST_GAMEPAD lines; NULL = silent        */
} GoofSdlPads;

/* Sets the position-true hint, starts SDL_INIT_GAMECONTROLLER and adopts
 * every controller already present, once, in device-index order.  False
 * (and pads->active == false) when the subsystem cannot start; the keyboard
 * path is unaffected. */
bool goof_sdl_pads_open(GoofSdlPads *pads, FILE *log);

/* Adopts any present controller whose instance id is not yet tracked.
 * Idempotent: already-tracked instances are never reopened. */
void goof_sdl_pads_scan(GoofSdlPads *pads);

/* Handles SDL_CONTROLLERDEVICEADDED / _REMOVED; true if the event was one of
 * them.  Every other event is left to the caller. */
bool goof_sdl_pads_handle_event(GoofSdlPads *pads, const SDL_Event *ev);

/* Reads the pad owning `slot`.  False (and *out zeroed) when it is empty. */
bool goof_sdl_pads_read(const GoofSdlPads *pads, int slot, GoofHostPadState *out);

/* Closes every handle and forgets every device.  Leaves the subsystem to
 * SDL_Quit. */
void goof_sdl_pads_close(GoofSdlPads *pads);

#endif
