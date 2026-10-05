/* GOOF_PPU_HDMA_WINDOW_SPOTLIGHT -- focused PPU window-logic and render-time
 * HDMA tests.  No ROM, no guest execution: every scene is built through
 * ppu_write on a private Ppu, every HDMA table is placed in WRAM by the test.
 *
 *   W  window logic ($212A/$212B OR/AND/XOR/XNOR) for BG1, BG2, OBJ and the
 *      colour window, every enable/invert combination, six window geometries,
 *      against an independent per-pixel oracle -- on ppu.c AND ppu_old.c;
 *   G  Goof's dark-room configuration (inverted W1, inverted W2, AND), one
 *      and two players;
 *   H  SimpleHdma_DoLineTo stepping: mode 4 direct one-line entries, count >1
 *      non-repeat (mode 1), repeat, indirect, termination;
 *   R  goof_headless_render: per-line WH0..WH3, live Ppu/Dma/WRAM untouched,
 *      deterministic re-render, HDMA-free frames unchanged, fail-closed
 *      guard for non-window HDMA destinations. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_rtl.h"
#include "dma.h"
#include "goof_headless_renderer.h"
#include "ppu.h"

extern bool g_new_ppu;

static unsigned g_checks, g_failures;
#define CHECK(cond, ...)                                             \
  do {                                                               \
    g_checks++;                                                      \
    if (!(cond)) {                                                   \
      if (g_failures++ < 40) {                                       \
        printf("FAIL %s:%d: ", __func__, __LINE__);                  \
        printf(__VA_ARGS__);                                         \
        printf("\n");                                                \
      }                                                              \
    }                                                                \
  } while (0)

static uint32_t g_fb[GOOF_HEADLESS_WIDTH * GOOF_HEADLESS_HEIGHT];
enum { kRow = 20 };  /* sampled screen row, inside the OBJ band */

static uint64_t fnv(const void *p, size_t n) {
  const uint8_t *b = p;
  uint64_t h = UINT64_C(1469598103934665603);
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * UINT64_C(1099511628211);
  return h;
}

static void vram_word(Ppu *p, uint16_t addr, uint16_t v) {
  ppu_write(p, 0x15, 0x80);
  ppu_write(p, 0x16, addr & 0xff);
  ppu_write(p, 0x17, addr >> 8);
  ppu_write(p, 0x18, v & 0xff);
  ppu_write(p, 0x19, v >> 8);
}

static void cgram_word(Ppu *p, uint8_t index, uint16_t v) {
  ppu_write(p, 0x21, index);
  ppu_write(p, 0x22, v & 0xff);
  ppu_write(p, 0x22, v >> 8);
}

/* Mode 1.  BG1 and BG2 are a solid colour-1 plane; four 64x64 sprites cover
 * rows 0..63 edge to edge.  Backdrop is black, so a pixel is non-black
 * exactly where the one enabled layer survives its window. */
static void build_scene(Ppu *p) {
  ppu_reset(p);
  PpuBeginDrawing(p, (uint8_t *)g_fb, GOOF_HEADLESS_PITCH, 0);
  ppu_write(p, 0x00, 0x0f);  /* INIDISP: full brightness */
  ppu_write(p, 0x05, 0x01);  /* BGMODE 1 */
  ppu_write(p, 0x07, 0x00);  /* BG1SC  @0000 */
  ppu_write(p, 0x08, 0x04);  /* BG2SC  @0400 */
  ppu_write(p, 0x0b, 0x11);  /* BG1/BG2 characters @1000 */
  ppu_write(p, 0x01, 0xa1);  /* OBSEL: 32/64, names @2000 */
  for (unsigned i = 0; i < 0x400; i++) {
    vram_word(p, i, 1);
    vram_word(p, 0x400 + i, 1);
  }
  for (unsigned r = 0; r < 8; r++) vram_word(p, 0x1010 + r, 0x00ff);
  for (unsigned t = 0; t < 128; t++)
    for (unsigned r = 0; r < 8; r++) vram_word(p, 0x2000 + t * 16 + r, 0x00ff);
  cgram_word(p, 0, 0x0000);
  cgram_word(p, 1, 0x001f);
  cgram_word(p, 129, 0x03e0);
  ppu_write(p, 0x02, 0);
  ppu_write(p, 0x03, 0);
  for (unsigned s = 0; s < 128; s++) {
    ppu_write(p, 0x04, s < 4 ? s * 64 : 0);
    ppu_write(p, 0x04, s < 4 ? 0 : 0xf0);
    ppu_write(p, 0x04, 0);
    ppu_write(p, 0x04, s < 4 ? 0x30 : 0);
  }
  for (unsigned i = 0; i < 32; i++) ppu_write(p, 0x04, i == 0 ? 0xaa : 0);
}

static void render_rows(Ppu *p, int last_line) {
  for (int line = 0; line <= last_line; line++) ppu_runLine(p, line);
}

/* Hardware window rule for one layer at one x (fullsnes, anomie). */
static bool oracle_masked(unsigned flags, unsigned logic, int x,
                          const int w[4]) {
  bool e1 = flags & 2, e2 = flags & 8;
  bool t1 = x >= w[0] && x <= w[1], t2 = x >= w[2] && x <= w[3];
  if (flags & 1) t1 = !t1;
  if (flags & 4) t2 = !t2;
  if (!e1 && !e2) return false;
  if (e1 && !e2) return t1;
  if (!e1 && e2) return t2;
  switch (logic) {
    case 0: return t1 || t2;
    case 1: return t1 && t2;
    case 2: return t1 != t2;
    default: return t1 == t2;
  }
}

static const int kGeometries[6][4] = {
  {40, 120, 80, 200},   /* overlapping */
  {10, 60, 100, 150},   /* disjoint */
  {106, 146, 1, 0},     /* W2 empty: Goof one-player */
  {1, 0, 1, 0},         /* both empty: Goof's cleared rows */
  {0, 255, 0, 0},       /* full width / single column */
  {50, 90, 50, 90},     /* identical */
};
static const char *const kLogicName[4] = {"OR", "AND", "XOR", "XNOR"};

/* layer: 0 BG1, 1 BG2, 4 OBJ, 5 colour window (clip-to-black inside). */
static unsigned window_case(Ppu *p, unsigned layer, unsigned flags,
                            unsigned logic, const int w[4]) {
  build_scene(p);
  uint32_t sel = flags << (layer * 4);
  ppu_write(p, 0x23, sel & 0xff);
  ppu_write(p, 0x24, (sel >> 8) & 0xff);
  ppu_write(p, 0x25, (sel >> 16) & 0xff);
  ppu_write(p, 0x26, w[0]);
  ppu_write(p, 0x27, w[1]);
  ppu_write(p, 0x28, w[2]);
  ppu_write(p, 0x29, w[3]);
  /* Every OTHER layer gets a different logic, so reading the wrong field
   * would be caught. */
  uint16_t log = 0;
  for (unsigned l = 0; l < 6; l++)
    log |= (l == layer ? logic : (logic ^ 1)) << (l * 2);
  ppu_write(p, 0x2a, log & 0xff);
  ppu_write(p, 0x2b, log >> 8);
  unsigned tm = layer == 5 ? 0x01 : 1u << layer;
  ppu_write(p, 0x2c, tm);
  ppu_write(p, 0x2e, layer == 5 ? 0 : tm);
  if (layer == 5) ppu_write(p, 0x30, 0x80);  /* CGWSEL: black inside */
  render_rows(p, kRow + 1);
  unsigned bad = 0;
  for (int x = 0; x < 256; x++) {
    bool visible = (g_fb[kRow * GOOF_HEADLESS_WIDTH + x] & 0xffffff) != 0;
    bad += visible == oracle_masked(flags, logic, x, w);
  }
  return bad;
}

static void test_window_logic(Ppu *p, bool new_ppu) {
  static const unsigned kLayers[4] = {0, 1, 4, 5};
  g_new_ppu = new_ppu;
  unsigned cases = 0;
  for (unsigned li = 0; li < 4; li++)
    for (unsigned logic = 0; logic < 4; logic++)
      for (unsigned flags = 0; flags < 16; flags++)
        for (unsigned g = 0; g < 6; g++) {
          unsigned bad = window_case(p, kLayers[li], flags, logic,
                                     kGeometries[g]);
          cases++;
          CHECK(bad == 0, "%s layer=%u %s flags=%X geometry=%u bad_px=%u",
                new_ppu ? "ppu.c" : "ppu_old.c", kLayers[li],
                kLogicName[logic], flags, g, bad);
        }
  g_new_ppu = true;
  printf("W  window logic %-9s layers=BG1,BG2,OBJ,COLOR logic=OR,AND,XOR,XNOR "
         "flags=16 geometries=6 cases=%u\n", new_ppu ? "ppu.c" : "ppu_old.c",
         cases);
}

/* Goof: W12SEL=$FF, WOBJSEL=$0F, WBGLOG=$05, WOBJLOG=$01, TMW=$13 --
 * BG1/BG2/OBJ survive only inside W1 u W2. */
static void goof_config_row(Ppu *p, unsigned layer, const int w[4],
                            uint8_t out[256]) {
  build_scene(p);
  ppu_write(p, 0x23, 0xff);
  ppu_write(p, 0x25, 0x0f);
  ppu_write(p, 0x2a, 0x05);
  ppu_write(p, 0x2b, 0x01);
  for (unsigned i = 0; i < 4; i++) ppu_write(p, 0x26 + i, w[i]);
  ppu_write(p, 0x2c, 1u << layer);
  ppu_write(p, 0x2e, 0x13);
  render_rows(p, kRow + 1);
  for (int x = 0; x < 256; x++)
    out[x] = (g_fb[kRow * GOOF_HEADLESS_WIDTH + x] & 0xffffff) != 0;
}

static void test_goof_config(Ppu *p) {
  static const unsigned kLayers[3] = {0, 1, 4};
  static const int one_player[4] = {106, 146, 1, 0};
  static const int two_players[4] = {40, 80, 150, 200};
  static const int two_overlap[4] = {40, 120, 100, 200};
  static const int p2_only[4] = {1, 0, 150, 200};
  static const int none[4] = {1, 0, 1, 0};
  const int *cfg[5] = {one_player, two_players, two_overlap, p2_only, none};
  const char *name[5] = {"P1 only", "P1+P2 disjoint", "P1+P2 overlap",
                         "P2 only", "no player"};
  uint8_t row[256];
  for (unsigned li = 0; li < 3; li++)
    for (unsigned c = 0; c < 5; c++) {
      goof_config_row(p, kLayers[li], cfg[c], row);
      unsigned bad = 0, lit = 0;
      for (int x = 0; x < 256; x++) {
        const int *w = cfg[c];
        bool inside = (x >= w[0] && x <= w[1]) || (x >= w[2] && x <= w[3]);
        bad += row[x] != inside;
        lit += row[x];
      }
      CHECK(bad == 0, "goof layer=%u %s bad_px=%u", kLayers[li], name[c], bad);
      if (li == 0)
        printf("G  goof W12SEL=FF WBGLOG=05 %-15s W1=[%d,%d] W2=[%d,%d] "
               "lit_px=%u %s\n", name[c], cfg[c][0], cfg[c][1], cfg[c][2],
               cfg[c][3], lit, bad ? "FAIL" : "PASS");
    }
  /* The pre-fix renderer ORed the inverted windows: with W2 empty that is
   * (not W1) or (everything) = everything masked.  Prove AND is in force. */
  goof_config_row(p, 0, one_player, row);
  CHECK(row[126] == 1 && row[105] == 0 && row[147] == 0,
        "goof AND not in force");
}

/* ------------------------------------------------------------------ HDMA */

static uint8_t *wram7f(uint16_t addr) { return &g_ram[0x10000 + addr]; }

static DmaChannel hdma_channel(uint8_t mode, bool indirect, uint8_t badr,
                               uint16_t aadr, uint8_t abank, uint8_t indbank) {
  DmaChannel c;
  memset(&c, 0, sizeof(c));
  c.hdmaActive = true;
  c.mode = mode;
  c.indirect = indirect;
  c.bAdr = badr;
  c.aAdr = aadr;
  c.aBank = abank;
  c.indBank = indbank;
  return c;
}

static void wh(const Ppu *p, uint8_t v[4]) {
  v[0] = p->window1left;
  v[1] = p->window1right;
  v[2] = p->window2left;
  v[3] = p->window2right;
}

static void test_hdma_mode4_direct(Ppu *p) {
  /* 224 one-line entries {01, a, b, c, d} then the terminator. */
  memset(wram7f(0xf000), 0xcc, 0x600);
  for (unsigned i = 0; i < 224; i++) {
    uint8_t *e = wram7f(0xf000 + 5 * i);
    e[0] = 0x01;
    e[1] = i;
    e[2] = i + 10;
    e[3] = 255 - i;
    e[4] = (i * 7) & 0xff;
  }
  *wram7f(0xf000 + 5 * 224) = 0x00;
  DmaChannel ch = hdma_channel(4, false, 0x26, 0xf000, 0x7f, 0);
  DmaChannel before = ch;
  SimpleHdma h;
  SimpleHdma_Init(&h, &ch);
  ppu_reset(p);
  unsigned bad = 0;
  for (unsigned i = 0; i < 224; i++) {
    SimpleHdma_DoLineTo(&h, p);
    uint8_t v[4];
    wh(p, v);
    bad += v[0] != i || v[1] != (uint8_t)(i + 10) || v[2] != 255 - i ||
           v[3] != ((i * 7) & 0xff);
  }
  CHECK(bad == 0, "mode4 per-line WH mismatch lines=%u", bad);
  CHECK(h.table == wram7f(0xf000 + 5 * 224), "table stepped %td bytes",
        h.table - wram7f(0xf000));
  SimpleHdma_DoLineTo(&h, p);  /* reads the $00 terminator */
  CHECK(h.table == NULL, "terminator did not stop the channel");
  uint8_t end[4];
  wh(p, end);
  CHECK(end[0] == 223 && end[1] == 233 && end[2] == 32 &&
        end[3] == ((223 * 7) & 0xff),
        "end state after termination %u %u %u %u", end[0], end[1], end[2],
        end[3]);
  SimpleHdma_DoLineTo(&h, p);
  uint8_t after[4];
  wh(p, after);
  CHECK(!memcmp(end, after, 4), "terminated channel still writes");
  CHECK(!memcmp(&before, &ch, sizeof(ch)), "DmaChannel mutated");
  printf("H  mode4 direct $2126-9: 224 one-line entries, per-line WH0-3 %s, "
         "stepped %u bytes, terminator stops, channel regs unchanged\n",
         bad ? "FAIL" : "PASS", 5 * 224);
}

static void test_hdma_line_semantics(Ppu *p) {
  unsigned failures_before = g_failures;
  /* Mode 1 (WH0/WH1), count > 1 non-repeat: CODE_828BBA's ROM table
   * DATA_83B7D8, copied to WRAM: rows 140..193 get [$1D,$E2]. */
  static const uint8_t box[] = {0x70, 0x01, 0x00, 0x1c, 0x01, 0x00, 0x36,
                                0x1d, 0xe2, 0x01, 0x01, 0x00, 0x00};
  memcpy(wram7f(0xf000), box, sizeof(box));
  DmaChannel ch = hdma_channel(1, false, 0x26, 0xf000, 0x7f, 0);
  SimpleHdma h;
  SimpleHdma_Init(&h, &ch);
  ppu_reset(p);
  ppu_write(p, 0x28, 0x55);
  ppu_write(p, 0x29, 0x66);
  unsigned bad = 0, writes_rows = 0;
  for (unsigned row = 0; row < 224; row++) {
    SimpleHdma_DoLineTo(&h, p);
    uint8_t v[4];
    wh(p, v);
    bool box_row = row >= 140 && row <= 193;
    uint8_t l = box_row ? 0x1d : 0x01, r = box_row ? 0xe2 : 0x00;
    bad += v[0] != l || v[1] != r || v[2] != 0x55 || v[3] != 0x66;
    writes_rows += box_row;
  }
  CHECK(bad == 0, "mode1 count>1 rows wrong=%u", bad);
  CHECK(h.table == NULL, "mode1 table not terminated");
  printf("H  mode1 count>1 non-repeat (DATA_83B7D8 shape): rows 140..193 "
         "window [1D,E2], WH2/WH3 untouched %s\n", bad ? "FAIL" : "PASS");

  /* Repeat: $83 = three lines, one transfer each; row 3+ keeps the last. */
  static const uint8_t rep[] = {0x83, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                                0x00};
  memcpy(wram7f(0xf000), rep, sizeof(rep));
  ch = hdma_channel(4, false, 0x26, 0xf000, 0x7f, 0);
  SimpleHdma_Init(&h, &ch);
  ppu_reset(p);
  bad = 0;
  for (unsigned row = 0; row < 6; row++) {
    SimpleHdma_DoLineTo(&h, p);
    uint8_t v[4];
    wh(p, v);
    unsigned k = row < 3 ? row : 2;
    bad += v[0] != 1 + 4 * k || v[3] != 4 + 4 * k;
  }
  CHECK(bad == 0, "repeat rows wrong=%u", bad);

  /* Indirect: one repeat entry of two lines whose data lives at 7F:F800. */
  static const uint8_t ind[] = {0x82, 0x00, 0xf8, 0x00};
  static const uint8_t data[] = {21, 22, 23, 24, 31, 32, 33, 34};
  memcpy(wram7f(0xf000), ind, sizeof(ind));
  memcpy(wram7f(0xf800), data, sizeof(data));
  ch = hdma_channel(4, true, 0x26, 0xf000, 0x7f, 0x7f);
  SimpleHdma_Init(&h, &ch);
  ppu_reset(p);
  uint8_t v0[4], v1[4];
  SimpleHdma_DoLineTo(&h, p);
  wh(p, v0);
  SimpleHdma_DoLineTo(&h, p);
  wh(p, v1);
  CHECK(!memcmp(v0, data, 4) && !memcmp(v1, data + 4, 4), "indirect wrong");
  SimpleHdma_DoLineTo(&h, p);
  CHECK(h.table == NULL, "indirect not terminated");
  printf("H  repeat ($83) and indirect ($82 -> 7F:F800) %s\n",
         g_failures != failures_before ? "FAIL" : "PASS");
}

/* SimpleHdma_DoLine keeps its historical target: the live g_ppu. */
static void test_hdma_live_wrapper(Ppu *p) {
  static const uint8_t t[] = {0x01, 9, 8, 7, 6, 0x00};
  memcpy(wram7f(0xf000), t, sizeof(t));
  DmaChannel ch = hdma_channel(4, false, 0x26, 0xf000, 0x7f, 0);
  SimpleHdma h;
  SimpleHdma_Init(&h, &ch);
  Ppu *saved = g_ppu;
  ppu_reset(p);
  g_ppu = p;
  SimpleHdma_DoLine(&h);
  g_ppu = saved;
  uint8_t v[4];
  wh(p, v);
  CHECK(v[0] == 9 && v[1] == 8 && v[2] == 7 && v[3] == 6,
        "SimpleHdma_DoLine no longer writes g_ppu");
}

/* ------------------------------------------------------------- renderer */

static GoofHeadlessRenderer g_r;

static void test_renderer(Ppu *live) {
  /* Goof-shaped: W12SEL=$FF AND, ch7 mode 4 at $2126, table with a band of
   * non-empty rows.  Rows r get entry r. */
  build_scene(live);
  ppu_write(live, 0x23, 0xff);
  ppu_write(live, 0x2a, 0x05);
  ppu_write(live, 0x2c, 0x01);
  ppu_write(live, 0x2e, 0x13);
  for (unsigned i = 0; i < 224; i++) {
    uint8_t *e = wram7f(0xf000 + 5 * i);
    bool band = i >= 100 && i < 141;
    e[0] = 0x01;
    e[1] = band ? 100 : 1;
    e[2] = band ? 140 : 0;
    e[3] = 1;
    e[4] = 0;
  }
  *wram7f(0xf000 + 5 * 224) = 0x00;
  static Dma dma;
  memset(&dma, 0, sizeof(dma));
  dma.channel[7] = hdma_channel(4, false, 0x26, 0xf000, 0x7f, 0);

  uint64_t live_ppu = fnv(live, sizeof(Ppu));
  uint64_t live_dma = fnv(&dma, sizeof(dma));
  uint64_t ram = fnv(g_ram, sizeof(g_ram));
  CHECK(goof_headless_render(&g_r, live, &dma, 1, 0), "render refused");
  uint64_t fb1 = g_r.meta.fnv1a64;
  uint8_t lw1[GOOF_HEADLESS_HEIGHT][4];
  memcpy(lw1, g_r.line_windows, sizeof(lw1));
  CHECK(g_r.meta.hdma_channels == 0x80, "hdma mask %02X",
        g_r.meta.hdma_channels);
  unsigned bad = 0, lit_rows = 0, widest = 0;
  for (unsigned row = 0; row < 224; row++) {
    const uint8_t *e = wram7f(0xf000 + 5 * row + 1);
    bad += memcmp(lw1[row], e, 4) != 0;
    unsigned lit = 0;
    for (int x = 0; x < 256; x++)
      lit += (g_r.pixels[row * GOOF_HEADLESS_WIDTH + x] & 0xffffff) != 0;
    bool band = row >= 100 && row < 141;
    bad += lit != (band ? 41u : 0u);
    lit_rows += lit != 0;
    if (lit > widest) widest = lit;
  }
  CHECK(bad == 0, "renderer per-row WH/pixels wrong=%u", bad);
  CHECK(lit_rows == 41 && widest == 41, "lit rows=%u widest=%u", lit_rows,
        widest);
  CHECK(fnv(live, sizeof(Ppu)) == live_ppu, "live Ppu mutated by render");
  CHECK(fnv(&dma, sizeof(dma)) == live_dma, "live Dma mutated by render");
  CHECK(fnv(g_ram, sizeof(g_ram)) == ram, "WRAM mutated by render");
  CHECK(live->window1left == 0 && live->window1right == 0,
        "live WH0/WH1 changed");

  /* Rendering the same boundary again replays HDMA from the top. */
  CHECK(goof_headless_render(&g_r, live, &dma, 1, 0), "second render");
  CHECK(g_r.meta.fnv1a64 == fb1, "re-render differs");
  CHECK(!memcmp(lw1, g_r.line_windows, sizeof(lw1)), "re-render windows");
  CHECK(fnv(live, sizeof(Ppu)) == live_ppu && fnv(&dma, sizeof(dma)) ==
        live_dma && fnv(g_ram, sizeof(g_ram)) == ram, "second render mutated");
  printf("R  renderer: ch7 mode4 per-row WH0-3 == table, band 41 rows x 41 px, "
         "live Ppu/Dma/WRAM byte-identical, re-render identical fb=%016llx\n",
         (unsigned long long)fb1);

  /* Same-register CPU/HDMA chronology in V100: early CPU write, HDMA,
   * late CPU write. The next line's HDMA replaces the late value again. */
  GoofPpuJournal journal = {.period = 0, .count = 2};
  uint64_t v100 = 37u * 1364u + 100u * 1364u;
  journal.entries[0] = (GoofPpuWrite){v100 + 100u * 4u, 0x26, 0, 22};
  journal.entries[1] = (GoofPpuWrite){v100 + 300u * 4u, 0x26, 22, 77};
  ppu_write(live, 0x26, 77);
  CHECK(goof_headless_render_with_journal(&g_r, live, &dma, &journal, 1, 0),
        "journal/HDMA render refused");
  CHECK(g_r.line_windows[100][0] == 77,
        "late CPU write did not follow HDMA");
  CHECK(g_r.line_windows[101][0] == 100,
        "next HDMA line did not replace CPU write");
  journal.count = 1;
  ppu_write(live, 0x26, 22);
  CHECK(goof_headless_render_with_journal(&g_r, live, &dma, &journal, 1, 0),
        "early journal/HDMA render refused");
  CHECK(g_r.line_windows[100][0] == 100,
        "early CPU write incorrectly followed HDMA");
  journal.overflow = true;
  CHECK(!goof_headless_render_with_journal(&g_r, live, &dma, &journal, 1, 0),
        "journal overflow was not refused");
  printf("R  journal: CPU before HDMA, HDMA, CPU after HDMA; overflow refused\n");

  /* No HDMA: NULL Dma and an all-inactive Dma render the same pixels. */
  memset(&dma, 0, sizeof(dma));
  CHECK(goof_headless_render(&g_r, live, NULL, 1, 0), "render NULL dma");
  uint64_t a = g_r.meta.fnv1a64;
  CHECK(goof_headless_render(&g_r, live, &dma, 1, 0), "render idle dma");
  CHECK(g_r.meta.fnv1a64 == a && g_r.meta.hdma_channels == 0,
        "idle Dma changed the frame");

  /* Fail closed on destinations the snapshot cannot own. */
  struct { uint8_t mode, badr; bool fromB, ok; const char *what; } g[] = {
    {4, 0x26, false, true, "WH0-3"},
    {1, 0x26, false, true, "WH0-1"},
    {4, 0x2f, false, true, "TSW..COLDATA"},
    {0, 0x23, false, true, "W12SEL"},
    {4, 0x30, false, false, "CGWSEL..$2133"},
    {1, 0x18, false, false, "VMDATA"},
    {3, 0x0d, false, false, "BG1 scroll (latched)"},
    {0, 0x22, false, false, "CGDATA"},
    {0, 0x40, false, false, "APUIO0"},
    {4, 0x26, true, false, "B->A"},
  };
  for (unsigned i = 0; i < sizeof(g) / sizeof(g[0]); i++) {
    memset(&dma, 0, sizeof(dma));
    dma.channel[3] = hdma_channel(g[i].mode, false, g[i].badr, 0xf000, 0x7f, 0);
    dma.channel[3].fromB = g[i].fromB;
    bool ok = goof_headless_render(&g_r, live, &dma, 1, 0);
    CHECK(ok == g[i].ok, "guard %s: render=%d", g[i].what, ok);
  }
  printf("R  fail-closed guard: $2123-$2132 A->B accepted; VRAM/CGRAM/scroll/"
         "APU/B->A refused\n");
}

int main(void) {
  Ppu *p = ppu_init();
  if (!p) return 2;
  test_window_logic(p, true);
  test_window_logic(p, false);
  test_goof_config(p);
  test_hdma_mode4_direct(p);
  test_hdma_line_semantics(p);
  test_hdma_live_wrapper(p);
  test_renderer(p);
  ppu_free(p);
  printf("GOOF_PPU_WINDOW_HDMA_TEST %s checks=%u failures=%u\n",
         g_failures ? "FAIL" : "PASS", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
