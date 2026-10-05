#include "goof_headless_renderer.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_rtl.h"
#include "sha256.h"

/* ppu.c selects its line renderer through this engine global and the engine
 * runner defines none.  Engine precedents (cosim/harness_glue.c,
 * cosim/ref_driver.c) use the new renderer; the validated E10 framebuffer was
 * produced with it. */
bool g_new_ppu = true;

/* Shallow-copy safety is audited against engine f0c85b9 (snes/ppu.h):
 * Ppu holds scalars and inline arrays (priority buffers, render caches,
 * CGRAM/OAM/VRAM) plus exactly two pointers.  renderBuffer is a non-owning
 * binding set by PpuBeginDrawing and is rebound below; pad2 is referenced
 * nowhere.  ppu.c/ppu_old.c reach state only through their Ppu argument and
 * g_new_ppu, so rendering the copy cannot touch the original.  A layout change
 * breaks these assertions and requires a new audit. */
_Static_assert(sizeof(Ppu) == 69808, "Ppu layout changed: re-audit snapshot copy");
_Static_assert(offsetof(Ppu, renderBuffer) == 2816, "Ppu pointer layout changed");
_Static_assert(offsetof(Ppu, pad2) == 3208, "Ppu pointer layout changed");
_Static_assert(sizeof(uint32_t) * GOOF_HEADLESS_WIDTH == GOOF_HEADLESS_PITCH,
               "pitch must hold one 256-pixel row");

static uint64_t fnv1a64_pixels(const uint32_t *px, size_t count) {
  uint64_t h = UINT64_C(1469598103934665603);
  for (size_t i = 0; i < count; i++)
    for (unsigned byte = 0; byte < 4; byte++)
      h = (h ^ (uint8_t)(px[i] >> (8 * byte))) * UINT64_C(1099511628211);
  return h;
}

/* Render-time replay is only equivalent to hardware HDMA for destinations
 * whose writes the guest can never observe again: the window, screen
 * designation and colour-math registers $2123-$2132 are write-only and keep
 * no write latch.  Anything else an HDMA channel can reach -- scroll (write
 * latch), VRAM/CGRAM/OAM data (readable memory), APU ports, WRAM port --
 * would need guest-side HDMA, so the render fails closed instead of
 * silently forking that state; so does a B->A (write-to-memory) channel.
 * Span = last B-bus offset of each DMAP mode. */
static bool hdma_target_is_render_only(const DmaChannel *c) {
  static const uint8_t kSpan[8] = {0, 1, 0, 1, 3, 1, 0, 1};
  unsigned first = c->bAdr, last = first + kSpan[c->mode & 7];
  return !c->fromB && first >= 0x23 && last <= 0x32;
}

static void apply_render_reg(Ppu *ppu, const GoofPpuWrite *write,
                             bool old) {
  uint16_t value = old ? write->old_value : write->new_value;
  if (write->reg == 0x32) ppu->fixedColor = value;
  else ppu_write(ppu, write->reg, (uint8_t)value);
}

bool goof_headless_render_with_journal(GoofHeadlessRenderer *r,
                          const Ppu *live, const Dma *live_dma,
                          const GoofPpuJournal *journal, uint64_t epoch,
                          uint64_t logical_hash) {
  if (!r || !live || live == &r->snapshot) return false;
  if (live->extraLeftRight || live->extraLeftCur || live->extraRightCur)
    return false;
  memcpy(&r->snapshot, live, sizeof(Ppu));
  if (journal) {
    if (journal->overflow || journal->count > kGoofPpuJournalCapacity)
      return false;
    bool rewound[0x34] = {0};
    uint64_t previous = 0;
    for (unsigned i = 0; i < journal->count; i++) {
      const GoofPpuWrite *w = &journal->entries[i];
      if (w->reg < 0x23 || w->reg > 0x33 ||
          w->master / (1364u * 262u) != journal->period ||
          w->master % (1364u * 262u) < 37u * 1364u ||
          (i && w->master < previous)) return false;
      previous = w->master;
      if (!rewound[w->reg]) {
        apply_render_reg(&r->snapshot, w, true);
        rewound[w->reg] = true;
      }
    }
  }
  PpuBeginDrawing(&r->snapshot, (uint8_t *)r->pixels, GOOF_HEADLESS_PITCH, 0);
  /* Per-scanline HDMA replay onto the snapshot.  Each render starts every
   * channel afresh from the live HDMAEN bits and A1T/DMAP/BBAD registers, as
   * the hardware's V=0 HDMA init does, so rendering the same boundary twice
   * gives the same frame.  The channel registers are copied: SimpleHdma only
   * reads them, and the table itself is read from WRAM/ROM, never written.
   * The transfer that follows line N sets the registers line N+1 is drawn
   * with; channels step in hardware priority order 0..7. */
  SimpleHdma hdma[8];
  uint8_t hdma_mask = 0;
  for (int ch = 0; ch < 8; ch++) {
    DmaChannel regs = live_dma ? live_dma->channel[ch] : (DmaChannel){0};
    if (regs.hdmaActive && !hdma_target_is_render_only(&regs)) return false;
    SimpleHdma_Init(&hdma[ch], &regs);
    if (hdma[ch].table) hdma_mask |= 1u << ch;
  }
  /* Line 0 is the pre-render line; lines 1..224 write rows 0..223. */
  unsigned next_write = 0;
  for (int line = 0; line <= GOOF_HEADLESS_HEIGHT; line++) {
    if (line > 0) {
      uint8_t *wh = r->line_windows[line - 1];
      wh[0] = r->snapshot.window1left;
      wh[1] = r->snapshot.window1right;
      wh[2] = r->snapshot.window2left;
      wh[3] = r->snapshot.window2right;
    }
    ppu_runLine(&r->snapshot, line);
    /* CPU writes in line V affect the next rendered line. HDMA runs at
     * HBlank dot ~278, between the two chronological CPU-write groups. */
    while (journal && next_write < journal->count) {
      const GoofPpuWrite *w = &journal->entries[next_write];
      uint64_t active = w->master % (1364u * 262u) - 37u * 1364u;
      if (active / 1364u != (uint64_t)line || active % 1364u >= 278u * 4u)
        break;
      apply_render_reg(&r->snapshot, w, false);
      next_write++;
    }
    if (hdma_mask && line < GOOF_HEADLESS_HEIGHT)
      for (int ch = 0; ch < 8; ch++)
        SimpleHdma_DoLineTo(&hdma[ch], &r->snapshot);
    while (journal && next_write < journal->count) {
      const GoofPpuWrite *w = &journal->entries[next_write];
      uint64_t active = w->master % (1364u * 262u) - 37u * 1364u;
      if (active / 1364u != (uint64_t)line) break;
      apply_render_reg(&r->snapshot, w, false);
      next_write++;
    }
  }
  r->meta = (GoofHeadlessFrameMeta){
    .epoch = epoch,
    .logical_hash = logical_hash,
    .hdma_channels = hdma_mask,
    .fnv1a64 = fnv1a64_pixels(r->pixels,
                              GOOF_HEADLESS_WIDTH * GOOF_HEADLESS_HEIGHT),
    .width = GOOF_HEADLESS_WIDTH,
    .height = GOOF_HEADLESS_HEIGHT,
  };
  return true;
}

bool goof_headless_render(GoofHeadlessRenderer *r, const Ppu *live,
                          const Dma *live_dma, uint64_t epoch,
                          uint64_t logical_hash) {
  return goof_headless_render_with_journal(r, live, live_dma, NULL,
                                           epoch, logical_hash);
}

static int cmp_u32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return x < y ? -1 : x > y;
}

void goof_headless_frame_measure(GoofHeadlessRenderer *r) {
  enum { N = GOOF_HEADLESS_WIDTH * GOOF_HEADLESS_HEIGHT };
  static uint32_t sorted[N];
  static uint8_t raw[N * 4];
  uint32_t non_black = 0, unique = 0;
  for (size_t i = 0; i < N; i++) {
    uint32_t v = r->pixels[i];
    non_black += (v & 0xffffff) != 0;
    sorted[i] = v;
    for (unsigned byte = 0; byte < 4; byte++)
      raw[i * 4 + byte] = (uint8_t)(v >> (8 * byte));
  }
  qsort(sorted, N, sizeof(sorted[0]), cmp_u32);
  for (size_t i = 0; i < N; i++)
    unique += i == 0 || sorted[i] != sorted[i - 1];
  sha256_compute(raw, sizeof(raw), r->meta.sha256);
  r->meta.unique_colors = unique;
  r->meta.non_black_pixels = non_black;
  r->meta.measured = true;
}

void goof_headless_discard(GoofHeadlessRenderer *r) {
  memset(&r->snapshot, 0, sizeof(r->snapshot));
}

bool goof_headless_write_ppm(const GoofHeadlessRenderer *r, const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  bool ok = fprintf(f, "P6\n%d %d\n255\n", GOOF_HEADLESS_WIDTH,
                    GOOF_HEADLESS_HEIGHT) > 0;
  for (size_t i = 0; ok && i < GOOF_HEADLESS_WIDTH * GOOF_HEADLESS_HEIGHT; i++) {
    uint32_t v = r->pixels[i];
    uint8_t rgb[3] = {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    ok = fwrite(rgb, 1, 3, f) == 3;
  }
  return fclose(f) == 0 && ok;
}
