#include "sdl_filter.h"
#include <stdint.h>

/* Integral of a periodic lower-half mask. Units are 1/(2*output_height)
 * source rows; integer arithmetic keeps the result independent of time,
 * floating-point platform behavior and the number of repeated presents. */
static int64_t dark_integral(int64_t position, int output_height) {
  int64_t period = 2LL * output_height;
  int64_t rem = position % period;
  return (position / period) * output_height +
         (rem > output_height ? rem - output_height : 0);
}

Uint8 goof_scanline_alpha(int row, int out_h, int src_h) {
  if (row < 0 || row >= out_h || out_h <= 0 || src_h <= 0) return 0;
  int64_t start = 2LL * row * src_h, end = 2LL * (row + 1) * src_h;
  int64_t coverage = dark_integral(end, out_h) - dark_integral(start, out_h);
  return (Uint8)((64 * coverage + src_h) / (2LL * src_h));
}

bool goof_sdl_filter_draw(SDL_Renderer *r, const GoofPresentationImage *image,
                          GoofFilterMode mode, const SDL_Rect *dst) {
  if (!r || !image || !image->texture || image->width <= 0 || image->height <= 0 ||
      !dst || dst->w <= 0 || dst->h <= 0 || (unsigned)mode >= GOOF_FILTER_COUNT)
    return false;
  /* Explicit per-texture state also restores true nearest immediately after
   * bilinear, including after device-reset recreation. No global hint leak. */
  SDL_ScaleMode wanted = mode == GOOF_FILTER_BILINEAR ? SDL_ScaleModeLinear : SDL_ScaleModeNearest;
  SDL_ScaleMode previous;
  if (SDL_GetTextureScaleMode(image->texture, &previous)) return false;
  /* Flush old uses before changing sampling (SDL2 does not flush here).
   * D3D9 2.30 caches its sampler until a texture rebind. An offscreen
   * untextured point forces the rebind via SDL without affecting any pixel.
   * Also needed when main_sdl already selected the texture's mode in preview. */
  if (previous != wanted && SDL_RenderFlush(r)) return false;
  if (SDL_SetTextureScaleMode(image->texture, wanted) ||
      SDL_RenderDrawPoint(r, -1, -1) ||
      SDL_RenderCopy(r, image->texture, NULL, dst)) return false;
  if (mode != GOOF_FILTER_SCANLINES) return true;
  Uint8 red, green, blue, alpha;
  SDL_BlendMode blend;
  SDL_GetRenderDrawColor(r, &red, &green, &blue, &alpha);
  SDL_GetRenderDrawBlendMode(r, &blend);
  bool ok = SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND) == 0;
  /* Merge equal neighboring coverage rows, including the 1X average case. */
  for (int y = 0; ok && y < dst->h;) {
    Uint8 dark = goof_scanline_alpha(y, dst->h, image->height);
    int end = y + 1;
    while (end < dst->h && goof_scanline_alpha(end, dst->h, image->height) == dark) end++;
    if (dark) {
      SDL_Rect band = {dst->x, dst->y + y, dst->w, end - y};
      ok = SDL_SetRenderDrawColor(r, 0, 0, 0, dark) == 0 && SDL_RenderFillRect(r, &band) == 0;
    }
    y = end;
  }
  if (SDL_SetRenderDrawColor(r, red, green, blue, alpha)) ok = false;
  if (SDL_SetRenderDrawBlendMode(r, blend)) ok = false;
  return ok;
}
