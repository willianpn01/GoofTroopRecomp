#ifndef GOOF_HEADLESS_RENDERER_H
#define GOOF_HEADLESS_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#include "dma.h"
#include "ppu.h"
#include "goof_frame_driver.h"

/* Observational headless render of a certified logical epoch boundary.
 *
 * The live Ppu is only read (one memcpy).  All rendering runs on the
 * renderer-owned snapshot, because ppu_runLine is consumptive: it toggles
 * evenFrame ($213F b7), rewrites rangeOver/timeOver ($213E b6/b7) and render
 * caches.  No guest CPU, NMI, IRQ, scheduler, APU, DMA or beam code is
 * reached from this module.
 *
 * HDMA is the one per-scanline input: the channels enabled in the live Dma
 * are replayed line by line onto the SNAPSHOT (SimpleHdma_DoLineTo), reading
 * the game's own tables from WRAM/ROM.  The live Dma, Ppu and WRAM are only
 * read.  The replayed registers are write-only on hardware and Goof never
 * reads its HDMA channel state back, so the live Ppu keeps the values the
 * CPU wrote (see goof_ppu_hdma_window_spotlight_implementation.md). */

enum {
  GOOF_HEADLESS_WIDTH = 256,
  GOOF_HEADLESS_HEIGHT = 224,
  GOOF_HEADLESS_PITCH = GOOF_HEADLESS_WIDTH * 4,
};

typedef struct {
  uint64_t epoch;          /* caller-supplied logical epoch number */
  uint64_t logical_hash;   /* caller-supplied GOOF_LOGICAL_EPOCH_HASH_V1 */
  uint64_t fnv1a64;        /* raw framebuffer, little-endian uint32 pixels */
  uint32_t width, height;
  uint8_t hdma_channels;   /* HDMAEN channels replayed onto the snapshot */
  /* Filled only by goof_headless_frame_measure. */
  bool measured;
  uint32_t unique_colors;
  uint32_t non_black_pixels;
  uint8_t sha256[32];
} GoofHeadlessFrameMeta;

typedef struct {
  /* Isolated render state.  Valid from render until discard; its only
   * pointer (renderBuffer) is rebound to `pixels`, never to the live Ppu. */
  Ppu snapshot;
  /* uint32 0x00RRGGBB, row-major, GOOF_HEADLESS_WIDTH per row. */
  uint32_t pixels[GOOF_HEADLESS_WIDTH * GOOF_HEADLESS_HEIGHT];
  /* WH0..WH3 each row was drawn with (after any HDMA replay). */
  uint8_t line_windows[GOOF_HEADLESS_HEIGHT][4];
  GoofHeadlessFrameMeta meta;
} GoofHeadlessRenderer;

/* Copies `live`, renders lines 0..224 on the copy -- applying the HDMA
 * channels enabled in `live_dma` between lines -- and hashes the result.
 * `live_dma` may be NULL (no HDMA).  Returns false if the live PPU is
 * outside the audited 256x224 contract (widescreen columns enabled) or an
 * enabled HDMA channel targets a register outside $2123-$2132. */
bool goof_headless_render(GoofHeadlessRenderer *r, const Ppu *live,
                          const Dma *live_dma, uint64_t epoch,
                          uint64_t logical_hash);
bool goof_headless_render_with_journal(GoofHeadlessRenderer *r,
                          const Ppu *live, const Dma *live_dma,
                          const GoofPpuJournal *journal, uint64_t epoch,
                          uint64_t logical_hash);
/* Optional diagnostics: unique colors, non-black pixels, SHA-256. */
void goof_headless_frame_measure(GoofHeadlessRenderer *r);
/* Drops the isolated snapshot. */
void goof_headless_discard(GoofHeadlessRenderer *r);
/* Binary PPM (P6, RGB24) of the current framebuffer. */
bool goof_headless_write_ppm(const GoofHeadlessRenderer *r, const char *path);

#endif
