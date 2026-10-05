#ifndef GOOF_SDL_SETTINGS_H
#define GOOF_SDL_SETTINGS_H

/* Goof Troop Recomp -- SDL2 adapter for the host Settings overlay.
 * GOOF_ENHANCEMENTS_E2_BUTTON_REMAPPING_CONFIG.
 *
 * The SDL half of host/ui/goof_menu.h: turns SDL keyboard and
 * game-controller events into GoofUiEvents (keys by SDL_Scancode == USB HID
 * usage, pad buttons by position), and draws the menu's draw list with a
 * glyph atlas built from the host bitmap font.
 *
 * Drawing happens on the window's renderer AFTER the guest texture has been
 * copied, so the overlay is composited over the presented image only; the
 * 256x224 framebuffer the gates hash is never written.
 *
 * Linked only into goof_recomp and the host tests (never into
 * goof_app_headless or a phase4 gate).
 */

#include <stdbool.h>

#include <SDL.h>

#include "host/ui/goof_menu.h"

enum { GOOF_SDL_SETTINGS_MAX_PADS = 16 };

typedef struct {
  GoofMenu menu;
  SDL_Texture *font;                     /* glyph atlas, NULL until first draw */
  SDL_Renderer *font_owner;
  struct { SDL_JoystickID id; uint8_t dirs; } stick[GOOF_SDL_SETTINGS_MAX_PADS];
} GoofSdlSettings;

void goof_sdl_settings_init(GoofSdlSettings *s, const GoofBindings *runtime,
                            GoofSettingsHost host);

/* Routes one SDL event.  Returns the GOOF_MENU_* flags; when
 * GOOF_MENU_CONSUMED is set the caller must NOT give the event to anything
 * else (game input, P pause, ESC quit).  SDL_QUIT, window and controller
 * ADDED/REMOVED events are never consumed. */
unsigned goof_sdl_settings_event(GoofSdlSettings *s, const SDL_Event *ev);

/* Translation only (exposed for tests).  Returns how many GoofUiEvents
 * (0..2) the SDL event becomes; a stick crossing from one side to the other
 * in one motion event releases one direction and presses the other. */
int goof_sdl_settings_translate(GoofSdlSettings *s, const SDL_Event *ev,
                                GoofUiEvent out[2]);

/* Composites the overlay on `r`'s current target, scaled by the largest
 * integer factor that fits the output and centred.  No-op when closed. */
void goof_sdl_settings_render(GoofSdlSettings *s, SDL_Renderer *r);

void goof_sdl_settings_destroy(GoofSdlSettings *s);

#endif
