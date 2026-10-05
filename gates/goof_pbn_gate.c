/* GOOF_PBN_GATE -- independent acceptance oracles for the physical-frame
 * boundary milestone (PBN1, PBN3, PBN10).
 *
 * WHY THIS EXISTS.  The fix being certified replaces every logical and
 * framebuffer hash in the E1..E1000 domain, so no existing hash can be its
 * acceptance criterion.  These three tests depend on NOTHING the workspace
 * produces:
 *
 *   PBN1   VRAM $0000..$01FF must equal a 512-byte block captured from an
 *          independent accurate emulator and frozen in
 *          goof_pbn1_reference.h BEFORE the fix was written.
 *   PBN3   the causal chain of the proven root cause: $1800 queue entry #0
 *          must be drained through the NMI path, within one physical frame
 *          period of its append, from an unmodified staging buffer, and the
 *          queue must be emptied before the buffer is reused.
 *   PBN10  at the title/menu anchor, VRAM tile 0 must be blank and the
 *          rendered frame must be within 360 pixels of the reference at a
 *          phase-matched frame.
 *
 * Every one of the three FAILS on the pre-fix build, for the documented
 * reason.  That is the point: a fix whose test was written after it is not a
 * proof.  The gate prints the measured quantity next to every verdict, so a
 * failure says WHY and a pass is auditable.
 *
 * TEST INFRASTRUCTURE ONLY.  The link-time --wrap instrumentation lives on
 * this target, never in goof_core and never in a product target.
 *
 *   goof_pbn ROM [--epochs N] [--ref-dir DIR] [--title-epoch N]
 *                [--ref-window A:B] [--aot-kinds all|function] [--quiet]
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "apu.h"
#include "cart.h"
#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "cpu.h"
#include "cpu_state.h"
#include "cpu_trace.h"
#include "dma.h"
#include "dsp.h"
#include "goof_gate_common.h"
#include "goof_headless_renderer.h"
#include "goof_pbn1_reference.h"
#include "goof_run_frame_adapter.h"
#include "ppu_dma_trace.h"
#include "snes.h"
#include "spc.h"

/* The title/menu plateau after the fix is E407..E595 (189 identical
 * framebuffers); E500 is its middle, so the anchor represents a visual state
 * rather than one frame of a ramp. */
enum { kDefaultEpochs = 1000, kDefaultTitleEpoch = 500 };

/* NTSC frame period in guest master cycles, 1364 * 262.  The engine's own
 * kMasterCyclesPerFrame (common_rtl.c) is the same value; it is spelled out
 * as the product here so the constant is self-evidently the line*dot count
 * and not a number copied from a report. */
enum { kFramePeriodMaster = 1364 * 262 };

/* PBN10 tolerance.  The known 360-pixel residual is one animated
 * menu-highlight tile row whose phase semantics are a separate question;
 * this milestone is not required to remove it. */
enum { kPbn10MaxDiffPixels = 360 };

/* ---------------------------------------------------------------------- */

static GoofRunFrameAdapter g_run_frame;
static GoofHeadlessRenderer g_renderer;
static unsigned g_epoch;
static int g_quiet;
static uint64_t g_mx_entries, g_mx_mismatches, g_mx_suffixless;
static uint64_t g_nonneutral_input_events;

static void fail_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail_msg(const char *fmt, ...) {
  va_list ap;
  printf("GOOF_PBN_GATE FAIL ");
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
  fflush(stdout);
  exit(1);
}
#define GATE_CHECK(c) \
  do { if (!(c)) fail_msg("epoch=%u line=%d condition=%s", g_epoch, __LINE__, #c); } while (0)

/* ---------------------------------------------------------------------- */
/* WRAM addressing                                                         */
/* ---------------------------------------------------------------------- */

/* Maps a guest (bank, address) to a WRAM offset, or returns -1.
 * $7E/$7F are the direct windows; banks $00-$3F and $80-$BF mirror the first
 * 8 KiB of $7E at $0000..$1FFF, which is where the $1800 queue and $40 live. */
static long wram_offset(uint8_t bank, uint16_t addr) {
  if (bank == 0x7e) return addr;
  if (bank == 0x7f) return 0x10000 + addr;
  uint8_t b = bank & 0x7f;
  if (b <= 0x3f && addr < 0x2000) return addr;
  return -1;
}

/* ---------------------------------------------------------------------- */
/* PBN3 -- queue-drain causal oracle                                       */
/* ---------------------------------------------------------------------- */

/* The instrumented object is $1800 queue entry #0, identified by its own
 * committed descriptor, not by a guess:
 *     params $01, VRAM $0000, size $0200, source $7F:8000
 * It is committed by the params store at $80:AFCD (CODE_80AF9C) and consumed
 * by CODE_80838D, which is reached either from CODE_80822E (the NMI path,
 * correct) or from CODE_808381 (the ROM's synchronous flush, which is what
 * happens today and what lands the wrong payload). */
enum { kQueueBase = 0x1800, kStageBase = 0x18000, kStageSize = 0x200 };

static struct {
  bool append_seen;
  uint64_t append_mclk;
  uint64_t append_fnv;        /* staging buffer at the append          */
  uint8_t  append_queue_len;  /* $40 after the commit                  */

  bool dma_seen;
  uint64_t dma_mclk;
  uint64_t dma_fnv;           /* staging buffer at the DMA             */
  bool dma_in_nmi;
  unsigned dma_epoch;
  uint16_t dma_vram_pointer;

  bool queue_cleared;         /* $40 returned to 0 after the append    */
  uint64_t queue_clear_mclk;

  bool clobber_seen;          /* first post-append write into $7F:8000 */
  uint64_t clobber_mclk;
} g_pbn3;

static uint64_t stage_fnv(void) {
  return goof_gate_fnv1a64(g_ram + kStageBase, kStageSize);
}

static void pbn3_observe_write(uint8_t bank, uint16_t addr, unsigned width) {
  long off = wram_offset(bank, addr);
  if (off < 0) return;
  for (unsigned i = 0; i < width; i++) {
    long a = off + i;
    if (!g_pbn3.append_seen) {
      /* The params byte at $1800 is the commit of entry #0 and the last of
       * the five stores that build it. */
      if (a == kQueueBase && g_ram[kQueueBase] == 0x01) {
        g_pbn3.append_seen = true;
        g_pbn3.append_mclk = g_cpu.master_cycles;
        g_pbn3.append_fnv = stage_fnv();
        g_pbn3.append_queue_len = g_ram[0x40];
      }
      continue;
    }
    if (a == 0x40 && g_ram[0x40] == 0 && !g_pbn3.queue_cleared) {
      g_pbn3.queue_cleared = true;
      g_pbn3.queue_clear_mclk = g_cpu.master_cycles;
    }
    if (a >= kStageBase && a < kStageBase + kStageSize && !g_pbn3.clobber_seen) {
      g_pbn3.clobber_seen = true;
      g_pbn3.clobber_mclk = g_cpu.master_cycles;
    }
  }
}

/* ---------------------------------------------------------------------- */
/* Link-time instrumentation                                               */
/* ---------------------------------------------------------------------- */

void __real_cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 v);
void __real_cpu_write16(CpuState *cpu, uint8 bank, uint16 addr, uint16 v);

void __wrap_cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 v) {
  __real_cpu_write8(cpu, bank, addr, v);
  pbn3_observe_write(bank, addr, 1);
}
void __wrap_cpu_write16(CpuState *cpu, uint8 bank, uint16 addr, uint16 v) {
  __real_cpu_write16(cpu, bank, addr, v);
  pbn3_observe_write(bank, addr, 2);
}

void __real_ppudma_record_dma(int channel, int fromB, uint8_t aBank,
                              uint16_t aAdr, uint8_t bAdr, uint16_t size);
void __wrap_ppudma_record_dma(int channel, int fromB, uint8_t aBank,
                              uint16_t aAdr, uint8_t bAdr, uint16_t size) {
  __real_ppudma_record_dma(channel, fromB, aBank, aAdr, bAdr, size);
  /* Entry #0's transfer, matched on its full descriptor. */
  if (!g_pbn3.dma_seen && !fromB && aBank == 0x7f && aAdr == 0x8000 &&
      bAdr == 0x18 && size == 0x0200) {
    g_pbn3.dma_seen = true;
    g_pbn3.dma_mclk = g_cpu.master_cycles;
    g_pbn3.dma_fnv = stage_fnv();
    g_pbn3.dma_in_nmi = g_run_frame.execution.in_nmi;
    g_pbn3.dma_epoch = g_epoch;
    g_pbn3.dma_vram_pointer = g_ppu ? g_ppu->vramPointer : 0;
  }
}

/* ---------------------------------------------------------------------- */
/* PBN4 -- AOT granularity guard, and PBN5 -- NMI entry trace              */
/* ---------------------------------------------------------------------- */

/* The strategy-A decision (one boundary check at the top of the run_interval
 * loop) is only honest while an AOT activation cannot contain a frame period.
 * That is measured here on every campaign and gated, so a future AOT recipe
 * that produces a long activation fails closed instead of silently degrading
 * NMI cadence. */
/* kGoofMaxAotActivationMaster is the driver's own admission threshold
 * (goof_frame_driver.h).  The guard and the threshold MUST be the same number:
 * "no activation exceeds it" AND "no activation starts within it of a
 * deadline" is what proves no activation can span a boundary, and two copies
 * of the constant could drift apart and silently void that proof.
 *
 * kMaxDetectionOvershootMclk stays the outer fail-closed guard.  With the
 * admission rule in force every boundary is crossed by an interpreted
 * instruction, so the TARGET is one instruction (kTargetDetectionOvershootMclk);
 * exceeding the target while staying under the outer guard is REPORTED, never
 * silently accepted. */
enum { kMaxAotActivationMclk = kGoofMaxAotActivationMaster,
       kMaxDetectionOvershootMclk = 4096,
       kTargetDetectionOvershootMclk = 64 };

static uint64_t g_aot_activations, g_aot_max_activation;
static uint32_t g_aot_max_entry_pc;
static unsigned g_aot_max_epoch;

InterpAotResult __real_interp_cursor_dispatch_aot(InterpCursor *c, CpuState *cpu,
                                                  InterpAotFunction entry);
InterpAotResult __wrap_interp_cursor_dispatch_aot(InterpCursor *c, CpuState *cpu,
                                                  InterpAotFunction entry) {
  uint32_t entry_pc = ((uint32_t)c->cpu.k << 16) | c->cpu.pc;
  uint64_t before = cpu->master_cycles;
  InterpAotResult r = __real_interp_cursor_dispatch_aot(c, cpu, entry);
  uint64_t span = cpu->master_cycles - before;
  g_aot_activations++;
  if (span > g_aot_max_activation) {
    g_aot_max_activation = span;
    g_aot_max_entry_pc = entry_pc;
    g_aot_max_epoch = g_epoch;
  }
  return r;
}

/* PBN5: one record per NMI ENTRY, digested in order.  Deliberately free of
 * host timestamps, host addresses and SDL state, so two fresh processes can
 * be compared byte for byte. */
static uint64_t g_nmi_digest = UINT64_C(1469598103934665603);
/* PBN6 variant.  master_cycles is charged differently by the two execution
 * tiers -- the interpreter charges a flat 8 master clocks per CPU cycle while
 * generated code charges region-weighted block costs -- so deadline_mclk can
 * legitimately differ between an AOT run and a more-interpreted one.  This
 * digest replaces the deadline with the BOUNDARY ORDINAL, which is a count
 * and not a duration, so a tier difference cannot hide a state divergence
 * behind a timing tolerance. */
static uint64_t g_nmi_digest_ordinal = UINT64_C(1469598103934665603);
static uint64_t g_epoch_digest = UINT64_C(1469598103934665603);
static uint64_t g_nmi_entry_count;
static uint64_t g_nmi_first_entry_mclk, g_nmi_last_entry_mclk;
static uint64_t g_nmi_max_delivery_latency;

static void digest_into(uint64_t *h, uint64_t v) {
  for (unsigned i = 0; i < 8; i++)
    *h = (*h ^ (uint8_t)(v >> (8 * i))) * UINT64_C(1099511628211);
}

InterpCursorResult __real_interp_cursor_step(InterpCursor *c);
InterpCursorResult __wrap_interp_cursor_step(InterpCursor *c) {
  uint32_t interrupted = ((uint32_t)c->cpu.k << 16) | c->cpu.pc;
  InterpCursorResult r = __real_interp_cursor_step(c);
  if (r == INTERP_CURSOR_NMI) {
    const GuestExecution *g = &g_run_frame.execution;
    uint64_t deadline = g->last_boundary_mclk;
    uint64_t entry_mclk = g_cpu.master_cycles;
    digest_into(&g_nmi_digest, deadline);
    digest_into(&g_nmi_digest, interrupted);
    digest_into(&g_nmi_digest, g_ram[0x9a]);
    digest_into(&g_nmi_digest_ordinal, g->physical_periods);
    digest_into(&g_nmi_digest_ordinal, interrupted);
    digest_into(&g_nmi_digest_ordinal, g_ram[0x9a]);
    g_nmi_entry_count++;
    if (!g_nmi_first_entry_mclk) g_nmi_first_entry_mclk = entry_mclk;
    g_nmi_last_entry_mclk = entry_mclk;
    if (entry_mclk >= deadline &&
        entry_mclk - deadline > g_nmi_max_delivery_latency)
      g_nmi_max_delivery_latency = entry_mclk - deadline;
  }
  return r;
}

void cpu_trace_func_entry(CpuState *cpu, uint32_t pc24, const char *name) {
  (void)pc24;
  uint8_t m, x;
  g_mx_entries++;
  if (!goof_gate_parse_mx_suffix(name, &m, &x)) { g_mx_suffixless++; return; }
  if ((cpu->m_flag & 1) != m || (cpu->x_flag & 1) != x) g_mx_mismatches++;
}

static uint8_t logical_bus_read(void *mem, uint32_t address) {
  return cpu_read8((CpuState *)mem, (uint8_t)(address >> 16), (uint16_t)address);
}
static void logical_bus_write(void *mem, uint32_t address, uint8_t value) {
  cpu_write8((CpuState *)mem, (uint8_t)(address >> 16), (uint16_t)address, value);
}
static void logical_event(void *mem, bool nmi_edge, uint16_t joy1,
                          uint16_t joy2) {
  (void)mem;
  if (joy1 != 0 || joy2 != 0) g_nonneutral_input_events++;
  g_snes->input1_currentState = joy1;
  g_snes->input2_currentState = joy2;
  if (nmi_edge) g_snes->inNmi = true;
}

void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

static const RtlGameInfo kGoofPbnInfo = {
  .title = "goof-pbn-gate",
  .initialize = NULL,
  .run_frame = goof_boot_harness_run_frame,
  .draw_ppu_frame = NULL,
  .save_name_prefix = "goof-pbn",
};

/* ---------------------------------------------------------------------- */
/* PBN10 reference frames                                                  */
/* ---------------------------------------------------------------------- */

typedef struct { uint32_t width, height; uint32_t *pixels; } RefFrame;

static bool ref_frame_load(const char *dir, unsigned frame, RefFrame *out) {
  char path[4200];
  if (snprintf(path, sizeof path, "%s/frame_f%04u.raw", dir, frame) >=
      (int)sizeof path)
    return false;
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  uint32_t hdr[2];
  if (fread(hdr, sizeof hdr, 1, f) != 1) { fclose(f); return false; }
  size_t n = (size_t)hdr[0] * hdr[1];
  if (!n || n > 512u * 480u) { fclose(f); return false; }
  uint32_t *px = malloc(n * 4);
  if (!px) { fclose(f); return false; }
  if (fread(px, 4, n, f) != n) { free(px); fclose(f); return false; }
  fclose(f);
  out->width = hdr[0]; out->height = hdr[1]; out->pixels = px;
  return true;
}

/* ---------------------------------------------------------------------- */

typedef struct {
  unsigned epochs, title_epoch, ref_lo, ref_hi;
  const char *ref_dir;
  unsigned aot_kinds;
  unsigned visual_scan;   /* 0 = off; else profile every Nth epoch */
} Options;

static void usage_fail(void) {
  fprintf(stderr,
          "usage: goof_pbn ROM [--epochs N] [--ref-dir DIR] [--title-epoch N] "
          "[--ref-window A:B] [--aot-kinds all|function] [--visual-scan N] "
          "[--quiet]\n");
  exit(2);
}

static unsigned parse_u(const char *t) {
  char *end;
  unsigned long v = strtoul(t, &end, 10);
  if (*t == '\0' || *end != '\0' || v < 1 || v > 100000) usage_fail();
  return (unsigned)v;
}

static Options parse_options(int argc, char **argv) {
  Options o = {.epochs = kDefaultEpochs, .title_epoch = kDefaultTitleEpoch,
               .ref_lo = 1, .ref_hi = 1100,
               .aot_kinds = (1u << INTERP_AOT_ENTRY_FUNCTION) |
                            (1u << INTERP_AOT_ENTRY_CONTINUATION)};
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--quiet") == 0) { g_quiet = 1; continue; }
    if (i + 1 >= argc) usage_fail();
    if (strcmp(argv[i], "--epochs") == 0) o.epochs = parse_u(argv[++i]);
    else if (strcmp(argv[i], "--title-epoch") == 0) o.title_epoch = parse_u(argv[++i]);
    else if (strcmp(argv[i], "--ref-dir") == 0) o.ref_dir = argv[++i];
    else if (strcmp(argv[i], "--visual-scan") == 0) o.visual_scan = parse_u(argv[++i]);
    else if (strcmp(argv[i], "--ref-window") == 0) {
      const char *w = argv[++i];
      char *end;
      unsigned long lo = strtoul(w, &end, 10);
      if (*end != ':') usage_fail();
      unsigned long hi = strtoul(end + 1, &end, 10);
      if (*end != '\0' || lo < 1 || hi < lo) usage_fail();
      o.ref_lo = (unsigned)lo; o.ref_hi = (unsigned)hi;
    } else if (strcmp(argv[i], "--aot-kinds") == 0) {
      const char *k = argv[++i];
      if (strcmp(k, "all") == 0)
        o.aot_kinds = (1u << INTERP_AOT_ENTRY_FUNCTION) |
                      (1u << INTERP_AOT_ENTRY_CONTINUATION);
      else if (strcmp(k, "function") == 0)
        o.aot_kinds = 1u << INTERP_AOT_ENTRY_FUNCTION;
      else usage_fail();
    } else usage_fail();
  }
  return o;
}

int main(int argc, char **argv) {
  if (argc < 2) usage_fail();
  Options opt = parse_options(argc, argv);
  setvbuf(stdout, NULL, _IOLBF, 0);

  /* The frozen oracle must be intact before it is trusted. */
  if (goof_gate_fnv1a64(goof_pbn1_vram_reference,
                        GOOF_PBN1_VRAM_REFERENCE_SIZE) !=
      GOOF_PBN1_VRAM_REFERENCE_FNV1A64)
    fail_msg("frozen PBN1 reference table digest mismatch");

  size_t rom_size;
  uint8_t *rom = goof_gate_load_pinned_rom(argv[1], &rom_size);
  if (!rom) fail_msg("rom=%s condition=pinned ROM size/SHA-256", argv[1]);

  RtlRegisterGame(&kGoofPbnInfo);
  GATE_CHECK(SnesInit(rom, (int)rom_size));
  cpu_state_init(&g_cpu, g_ram);
  Interp816 reset = {.mem = &g_cpu, .read = logical_bus_read,
                     .write = logical_bus_write, .exact_pb = true};
  interp816_reset(&reset);
  GATE_CHECK(goof_run_frame_adapter_init(&g_run_frame, &reset, 0x1dff,
      g_aot_entry_registry, g_aot_entry_registry_count, g_ram,
      logical_event, 50000000));
  GATE_CHECK(goof_execution_set_aot_kinds(&g_run_frame.execution,
                                          opt.aot_kinds));
  goof_run_frame_adapter_bind(&g_run_frame);
  const GuestExecution *g = &g_run_frame.execution;
  GATE_CHECK(g_run_frame.last_result.reason == WAITING);

  printf("GOOF_PBN_GATE START epochs=%u title_epoch=%u ref_dir=%s "
         "ref_window=%u:%u aot_kinds=%u frame_period=%u\n",
         opt.epochs, opt.title_epoch, opt.ref_dir ? opt.ref_dir : "(none)",
         opt.ref_lo, opt.ref_hi, opt.aot_kinds, (unsigned)kFramePeriodMaster);

  /* PBN1 running state. */
  unsigned pbn1_first_match = 0, pbn1_first_mismatch = 0;
  unsigned pbn1_match_epochs = 0;
  unsigned pbn1_worst_diff = 0;

  /* PBN2/PBN4 accounting, accumulated per epoch. */
  /* Deltas start from the reset boundary, which may already hold bootstrap
   * NMIs (GOOF_CPU_DMA_REFRESH: the bootstrap's $4200=$B1 precedes boundary 1). */
  uint64_t prev_nmis = g->nmis, prev_epilogues = g->epilogues;
  uint8_t prev_9c = g_ram[0x9c];
  uint64_t e1_periods = 0, e1_requests = 0, e1_entries = 0, e1_delta_9c = 0;
  uint64_t total_periods = 0, total_requests = 0, total_entries = 0;
  uint64_t total_padding = 0;
  unsigned pbn4_violations = 0;
  unsigned first_pbn4_violation_epoch = 0;
  const char *first_pbn4_violation = NULL;

  /* PBN10 state, captured at the title anchor. */
  bool title_seen = false;
  bool title_tile0_blank = false;
  static uint32_t title_pixels[GOOF_HEADLESS_WIDTH * GOOF_HEADLESS_HEIGHT];
  uint64_t title_logical = 0, title_fb = 0;

  for (unsigned epoch = 1; epoch <= opt.epochs; epoch++) {
    g_epoch = epoch;
    alarm(300);
    RunResult r = goof_run_frame_adapter_advance(&g_run_frame, 20000000);
    alarm(0);
    if (r.reason != EPOCH_COMPLETE_WAITING)
      fail_msg("epoch=%u reason=%d pc=%06X consumed=%llu -- the guest did not "
               "reach certified quiescence", epoch, (int)r.reason, r.pc24,
               (unsigned long long)r.consumed);

    /* --- PBN4: per-epoch boundary accounting, checked as it is produced --- */
    uint8_t now_9c = g_ram[0x9c];
    uint64_t delta_9c = (uint8_t)(now_9c - prev_9c);
    uint64_t delta_nmis = g->nmis - prev_nmis;
    uint64_t delta_epilogues = g->epilogues - prev_epilogues;
    prev_9c = now_9c; prev_nmis = g->nmis; prev_epilogues = g->epilogues;
    digest_into(&g_epoch_digest, r.physical_periods);
    digest_into(&g_epoch_digest, r.nmi_requests);
    digest_into(&g_epoch_digest, r.nmi_entries);
    digest_into(&g_epoch_digest, delta_9c);
    total_periods += r.physical_periods;
    total_requests += r.nmi_requests;
    total_entries += r.nmi_entries;
    total_padding += r.idle_padding_master_cycles;
    if (epoch == 1) {
      e1_periods = r.physical_periods; e1_requests = r.nmi_requests;
      e1_entries = r.nmi_entries; e1_delta_9c = delta_9c;
    }
#define PBN4_REQUIRE(cond, name)                                            \
    do { if (!(cond)) { pbn4_violations++;                                  \
           if (!first_pbn4_violation) {                                     \
             first_pbn4_violation = (name);                                 \
             first_pbn4_violation_epoch = epoch; } } } while (0)
    PBN4_REQUIRE(r.nmi_entries == delta_nmis, "entries==delta_nmis");
    PBN4_REQUIRE(r.nmi_entries == delta_epilogues, "entries==delta_epilogues");
    PBN4_REQUIRE(delta_9c == (r.nmi_entries & 0xff), "delta_9c==entries_mod_256");

    const Ppu *live = g_ppu;
    const uint8_t *vram_bytes = (const uint8_t *)live->vram;

    /* --- PBN1: VRAM $0000..$01FF against the frozen reference --- */
    unsigned diff = 0;
    for (unsigned i = 0; i < GOOF_PBN1_VRAM_REFERENCE_SIZE; i++)
      diff += vram_bytes[i] != goof_pbn1_vram_reference[i];
    if (diff == 0) {
      pbn1_match_epochs++;
      if (!pbn1_first_match) pbn1_first_match = epoch;
    } else {
      if (!pbn1_first_mismatch) pbn1_first_mismatch = epoch;
      if (diff > pbn1_worst_diff) pbn1_worst_diff = diff;
    }

    /* --- render the certified boundary --- */
    uint64_t logical = goof_gate_logical_epoch_hash_v1(g);
    GATE_CHECK(goof_headless_render(&g_renderer, live, g_dma, epoch, logical));
    goof_headless_frame_measure(&g_renderer);
    uint64_t fb = g_renderer.meta.fnv1a64;
    if (epoch == opt.title_epoch) {
      title_seen = true;
      memcpy(title_pixels, g_renderer.pixels, sizeof title_pixels);
      title_logical = logical;
      title_fb = fb;
      title_tile0_blank = true;
      for (unsigned i = 0; i < 32; i++)
        if (vram_bytes[i] != 0) title_tile0_blank = false;
    }
    /* Optional campaign-wide visual profile: for every Nth epoch, the best
     * agreement with the reference in a window around the physical frame
     * index this epoch actually reached.  The window exists because
     * master_cycles is a deterministic estimate, not a hardware frame index
     * (the interpreter charges a flat 8 master clocks per CPU cycle while
     * generated code charges region-weighted costs), so the two clocks drift
     * slowly against each other.  This is reported, never asserted. */
    if (opt.visual_scan && opt.ref_dir && (epoch % opt.visual_scan) == 0) {
      unsigned centre = (unsigned)g->physical_periods;
      unsigned lo = centre > 150 ? centre - 150 : 1, hi = centre + 150;
      unsigned best_f = 0, best_d = UINT32_MAX, seen = 0;
      for (unsigned f = lo; f <= hi; f++) {
        RefFrame rf;
        if (!ref_frame_load(opt.ref_dir, f, &rf)) continue;
        seen++;
        if (rf.width == GOOF_HEADLESS_WIDTH && rf.height == GOOF_HEADLESS_HEIGHT) {
          unsigned d2 = 0;
          for (unsigned i = 0; i < GOOF_HEADLESS_WIDTH * GOOF_HEADLESS_HEIGHT; i++)
            d2 += (g_renderer.pixels[i] & 0xFFFFFFu) != (rf.pixels[i] & 0xFFFFFFu);
          if (d2 < best_d) { best_d = d2; best_f = f; }
        }
        free(rf.pixels);
      }
      printf("VISUAL_SCAN epoch=%u boundary_index=%u best_reference_frame=%u "
             "differing_pixels=%u window=%u:%u frames=%u\n",
             epoch, centre, best_f, best_d == UINT32_MAX ? 0u : best_d,
             lo, hi, seen);
    }
    goof_headless_discard(&g_renderer);

    if (!g_quiet)
      printf("EPOCH epoch=%u logical=%016llx fb=%016llx vram0000_diff=%u "
             "mclk=%llu periods=%llu requests=%llu entries=%llu d9c=%llu "
             "padding=%llu\n", epoch, (unsigned long long)logical,
             (unsigned long long)fb, diff,
             (unsigned long long)g_cpu.master_cycles,
             (unsigned long long)r.physical_periods,
             (unsigned long long)r.nmi_requests,
             (unsigned long long)r.nmi_entries,
             (unsigned long long)delta_9c,
             (unsigned long long)r.idle_padding_master_cycles);
  }
#undef PBN4_REQUIRE

  /* ------------------------------------------------------------------ */
  /* Verdicts                                                            */
  /* ------------------------------------------------------------------ */
  int failures = 0;

  /* PBN1 -- the block must equal the reference at EVERY certified boundary.
   * The reference reaches its final value at frame 4, which is inside our
   * epoch 1, so there is no warm-up window to excuse. */
  bool pbn1_pass = pbn1_match_epochs == opt.epochs;
  printf("PBN1_VRAM_REFERENCE %s matching_epochs=%u/%u first_match=%u "
         "first_mismatch=%u worst_differing_bytes=%u/%u reference_fnv=%016llx "
         "observed_fnv=%016llx\n",
         pbn1_pass ? "PASS" : "FAIL", pbn1_match_epochs, opt.epochs,
         pbn1_first_match, pbn1_first_mismatch, pbn1_worst_diff,
         (unsigned)GOOF_PBN1_VRAM_REFERENCE_SIZE,
         (unsigned long long)GOOF_PBN1_VRAM_REFERENCE_FNV1A64,
         (unsigned long long)goof_gate_fnv1a64((const uint8_t *)g_ppu->vram,
                                               GOOF_PBN1_VRAM_REFERENCE_SIZE));
  failures += !pbn1_pass;

  /* PBN3 -- the root-cause chain, as four independent conditions. */
  bool a = g_pbn3.dma_seen && g_pbn3.dma_in_nmi;
  bool b = g_pbn3.dma_seen && g_pbn3.append_seen &&
           g_pbn3.dma_mclk >= g_pbn3.append_mclk &&
           g_pbn3.dma_mclk - g_pbn3.append_mclk <= (uint64_t)kFramePeriodMaster;
  bool c = g_pbn3.dma_seen && g_pbn3.append_seen &&
           g_pbn3.dma_fnv == g_pbn3.append_fnv;
  bool d = g_pbn3.queue_cleared &&
           (!g_pbn3.clobber_seen ||
            g_pbn3.queue_clear_mclk <= g_pbn3.clobber_mclk);
  bool pbn3_pass = a && b && c && d;
  printf("PBN3_QUEUE_DRAIN %s append_mclk=%llu drain_mclk=%llu delta=%lld "
         "period=%u in_nmi=%d append_fnv=%016llx drain_fnv=%016llx "
         "queue_clear_mclk=%llu clobber_mclk=%llu vram_ptr=%04X epoch=%u\n",
         pbn3_pass ? "PASS" : "FAIL",
         (unsigned long long)g_pbn3.append_mclk,
         (unsigned long long)g_pbn3.dma_mclk,
         (long long)(g_pbn3.dma_mclk - g_pbn3.append_mclk),
         (unsigned)kFramePeriodMaster, (int)g_pbn3.dma_in_nmi,
         (unsigned long long)g_pbn3.append_fnv,
         (unsigned long long)g_pbn3.dma_fnv,
         (unsigned long long)g_pbn3.queue_clear_mclk,
         (unsigned long long)g_pbn3.clobber_mclk,
         g_pbn3.dma_vram_pointer, g_pbn3.dma_epoch);
  printf("PBN3_CONDITIONS drained_through_nmi=%s within_one_period=%s "
         "buffer_preserved=%s queue_emptied_before_reuse=%s\n",
         a ? "yes" : "no", b ? "yes" : "no", c ? "yes" : "no",
         d ? "yes" : "no");
  failures += !pbn3_pass;

  /* PBN10 -- the visual oracle at the title/menu anchor. */
  if (!title_seen) {
    printf("PBN10_VISUAL_REFERENCE NOT_RUN reason=title_epoch_beyond_run "
           "title_epoch=%u epochs=%u\n", opt.title_epoch, opt.epochs);
    failures++;
  } else if (!opt.ref_dir) {
    printf("PBN10_VISUAL_REFERENCE NOT_RUN reason=no_reference_directory "
           "tile0_blank=%s\n", title_tile0_blank ? "yes" : "no");
    failures++;
  } else {
    unsigned best_frame = 0, best_diff = UINT32_MAX, loaded = 0;
    for (unsigned f = opt.ref_lo; f <= opt.ref_hi; f++) {
      RefFrame rf;
      if (!ref_frame_load(opt.ref_dir, f, &rf)) continue;
      loaded++;
      if (rf.width == GOOF_HEADLESS_WIDTH && rf.height == GOOF_HEADLESS_HEIGHT) {
        unsigned d2 = 0;
        for (unsigned i = 0; i < GOOF_HEADLESS_WIDTH * GOOF_HEADLESS_HEIGHT; i++)
          d2 += (title_pixels[i] & 0xFFFFFFu) != (rf.pixels[i] & 0xFFFFFFu);
        if (d2 < best_diff) { best_diff = d2; best_frame = f; }
      }
      free(rf.pixels);
    }
    bool pbn10_pass = loaded > 0 && title_tile0_blank &&
                      best_diff <= kPbn10MaxDiffPixels;
    printf("PBN10_VISUAL_REFERENCE %s title_epoch=%u tile0_blank=%s "
           "best_reference_frame=%u differing_pixels=%u tolerance=%u "
           "total_pixels=%u reference_frames_scanned=%u logical=%016llx "
           "fb=%016llx\n",
           pbn10_pass ? "PASS" : "FAIL", opt.title_epoch,
           title_tile0_blank ? "yes" : "no", best_frame,
           best_diff == UINT32_MAX ? 0u : best_diff,
           (unsigned)kPbn10MaxDiffPixels,
           (unsigned)(GOOF_HEADLESS_WIDTH * GOOF_HEADLESS_HEIGHT), loaded,
           (unsigned long long)title_logical, (unsigned long long)title_fb);
    failures += !pbn10_pass;
  }

  /* PBN2 -- NMI cadence at epoch 1.  The exact count is NOT invented here:
   * delivering NMIs changes the guest's own work.  The property is that EVERY
   * absolute-grid boundary epoch 1 crosses (all of them with NMITIMEN.7 set)
   * delivers exactly one NMI, and the guest's own $9C counter agrees.  It was
   * previously spelled as ">= 140", the boundary count under the uniform
   * 8-clock CPU model; GOOF_CPU_DMA_REFRESH moved that count (143 -> 121), so
   * the bound is stated as the property itself until a measured value is
   * promoted at checkpoint. */
  bool pbn2_pass = e1_periods >= 1 && e1_entries == e1_periods &&
                   e1_requests == e1_periods &&
                   e1_delta_9c == (e1_entries & 0xff);
  printf("PBN2_NMI_CADENCE %s e1_physical_periods=%llu e1_nmi_requests=%llu "
         "e1_nmi_entries=%llu e1_delta_9c=%llu required=entries==requests==periods\n",
         pbn2_pass ? "PASS" : "FAIL", (unsigned long long)e1_periods,
         (unsigned long long)e1_requests, (unsigned long long)e1_entries,
         (unsigned long long)e1_delta_9c);
  failures += !pbn2_pass;

  /* PBN4 -- boundary accounting and the campaign-wide fail-closed guards. */
  const GuestExecution *ge = &g_run_frame.execution;
  bool aot_ok = g_aot_max_activation <= kMaxAotActivationMclk;
  bool over_ok = ge->max_detection_overshoot <= kMaxDetectionOvershootMclk;
  /* Tier-uniform master clock, design section 22/28.  With deadline-aware
   * admission in force, NO activation may span a physical deadline -- that is
   * what makes both tiers detect every boundary at an interpreted-instruction
   * edge, and therefore interrupt the same guest PC.  A straddle is a hard
   * failure, never a tolerance.
   *
   * The detection overshoot then collapses to one interpreted instruction.
   * kTargetDetectionOvershootMclk is that prediction; exceeding it while
   * staying inside the outer fail-closed guard is reported as EXCEEDED rather
   * than quietly accepted, because it would mean a boundary was crossed by
   * something other than a single interpreted step. */
  bool straddle_ok = ge->aot_boundary_straddles == 0;
  bool target_ok = ge->max_detection_overshoot <= kTargetDetectionOvershootMclk;
  bool pbn4_pass = pbn4_violations == 0 && aot_ok && over_ok && straddle_ok &&
                   ge->nmi_deferred_in_nmi == 0 &&
                   ge->nmi_deferred_collapsed == 0 &&
                   ge->nmi_request_collapsed == 0 &&
                   ge->multi_boundary_detections == 0;
  printf("PBN4_BOUNDARY_ACCOUNTING %s per_epoch_violations=%u first=%s@%u "
         "total_physical_periods=%llu total_nmi_requests=%llu "
         "total_nmi_entries=%llu total_idle_padding=%llu "
         "lifetime_physical_periods=%llu lifetime_nmi_requests=%llu "
         "lifetime_nmi_entries=%llu lifetime_idle_padding=%llu "
         "max_aot_activation=%llu limit=%u at_pc=%06X epoch=%u "
         "activations=%llu max_detection_overshoot=%llu limit=%u "
         "deferred_in_nmi=%llu deferred_collapsed=%llu "
         "request_collapsed=%llu multi_boundary_detections=%llu "
         "boundary_checks=%llu first_boundary_mclk=%llu "
         "last_boundary_mclk=%llu final_mclk=%llu "
         "aot_straddles=%llu aot_declined_near_boundary=%llu "
         "admission_window=%u overshoot_target=%u target=%s "
         "generated_block_moves=%llu\n",
         pbn4_pass ? "PASS" : "FAIL", pbn4_violations,
         first_pbn4_violation ? first_pbn4_violation : "(none)",
         first_pbn4_violation_epoch,
         (unsigned long long)total_periods, (unsigned long long)total_requests,
         (unsigned long long)total_entries, (unsigned long long)total_padding,
         (unsigned long long)ge->physical_periods,
         (unsigned long long)ge->nmi_requests,
         (unsigned long long)ge->nmi_entries,
         (unsigned long long)ge->idle_padding_master,
         (unsigned long long)g_aot_max_activation,
         (unsigned)kMaxAotActivationMclk, g_aot_max_entry_pc, g_aot_max_epoch,
         (unsigned long long)g_aot_activations,
         (unsigned long long)ge->max_detection_overshoot,
         (unsigned)kMaxDetectionOvershootMclk,
         (unsigned long long)ge->nmi_deferred_in_nmi,
         (unsigned long long)ge->nmi_deferred_collapsed,
         (unsigned long long)ge->nmi_request_collapsed,
         (unsigned long long)ge->multi_boundary_detections,
         (unsigned long long)ge->boundary_checks,
         (unsigned long long)ge->first_boundary_mclk,
         (unsigned long long)ge->last_boundary_mclk,
         (unsigned long long)g_cpu.master_cycles,
         (unsigned long long)ge->aot_boundary_straddles,
         (unsigned long long)ge->aot_declined_near_boundary,
         (unsigned)kGoofMaxAotActivationMaster,
         (unsigned)kTargetDetectionOvershootMclk,
         target_ok ? "MET" : "EXCEEDED",
         (unsigned long long)cpu_trace_block_move_count());
  /* TU-MV: residual R2 is only safe while no GENERATED block move runs. */
  bool blockmove_ok = cpu_trace_block_move_count() == 0;
  printf("PBN11_BLOCK_MOVE_UNREACHABLE %s generated_block_moves=%llu "
         "policy=defer_with_runtime_assert_unreachable\n",
         blockmove_ok ? "PASS" : "FAIL",
         (unsigned long long)cpu_trace_block_move_count());
  failures += !blockmove_ok;
  failures += !pbn4_pass;

  /* PBN5 -- NMI trace digest over (deadline_mclk, interrupted pc24, $9A). */
  /* PBN5/PBN6 digests.  `digest` is the full record including the deadline in
   * master cycles; `ordinal_digest` and `epoch_digest` are the tier-free
   * forms that an AOT-vs-interpreter comparison asserts. */
  printf("PBN5_NMI_TRACE_DIGEST digest=%016llx ordinal_digest=%016llx "
         "epoch_digest=%016llx entries=%llu "
         "first_entry_mclk=%llu last_entry_mclk=%llu max_delivery_latency=%llu\n",
         (unsigned long long)g_nmi_digest,
         (unsigned long long)g_nmi_digest_ordinal,
         (unsigned long long)g_epoch_digest,
         (unsigned long long)g_nmi_entry_count,
         (unsigned long long)g_nmi_first_entry_mclk,
         (unsigned long long)g_nmi_last_entry_mclk,
         (unsigned long long)g_nmi_max_delivery_latency);

  printf("GOOF_PBN_GATE %s epochs=%u failures=%d mx_entries=%llu "
         "mx_mismatch=%llu mx_suffixless=%llu nonneutral_input=%llu\n",
         failures ? "FAIL" : "PASS", opt.epochs, failures,
         (unsigned long long)g_mx_entries,
         (unsigned long long)g_mx_mismatches,
         (unsigned long long)g_mx_suffixless,
         (unsigned long long)g_nonneutral_input_events);
  free(rom);
  return failures ? 1 : 0;
}
