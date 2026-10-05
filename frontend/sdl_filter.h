#ifndef GOOF_SDL_FILTER_H
#define GOOF_SDL_FILTER_H
#include <SDL.h>
#include "host/goof_present_settings.h"

/* Presentation-only image metadata. No guest pointer or guest callback.
 * Texture is an uploaded host copy. Any future source width is accepted. */
typedef struct {
  SDL_Texture *texture;
  int width, height;
  Uint32 format;
} GoofPresentationImage;

/* Single primary mode. Restores renderer primitive state, owns no resources.
 * Call between clear and Settings overlay. Never updates the source texture. */
bool goof_sdl_filter_draw(SDL_Renderer *renderer, const GoofPresentationImage *image,
                          GoofFilterMode mode, const SDL_Rect *dest);
/* Coverage-integrated lower-half source-row mask (0..64 alpha). */
Uint8 goof_scanline_alpha(int output_row, int output_height, int source_height);
#endif
