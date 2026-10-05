#ifndef GOOF_BITMAP_FONT_H
#define GOOF_BITMAP_FONT_H

/* Host-authored 5x7 bitmap font (see goof_bitmap_font.c).  SDL-FREE. */
#include <stdint.h>

enum { GOOF_FONT_W = 5, GOOF_FONT_H = 7, GOOF_FONT_FIRST = 0x20,
       GOOF_FONT_GLYPHS = 95 };

/* Row `row` (0 = top) of the glyph for `c`: bit (GOOF_FONT_W-1-x) set when
 * pixel x is lit.  Anything outside printable ASCII draws as '?'. */
uint8_t goof_font_row(char c, int row);

#endif
