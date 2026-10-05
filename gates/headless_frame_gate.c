/* Headless frame pipeline regression gate.
 *
 * Runs the certified logical epochs of the permanent gate (same init, same
 * adapter, same budgets) and, after each certified boundary, renders an
 * isolated PPU snapshot.  Fails closed if rendering perturbs the logical hash
 * sequence, any focused live-state block, or the validated credits-fade
 * framebuffer (E31; E10 before the coordinated CPU/DMA/refresh clock).
 *
 * usage: goof_headless_frames ROM [--epochs N] [--dump-frame N]...
 *                             [--output-dir DIR] */
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
#include "dma.h"
#include "dsp.h"
#include "goof_gate_common.h"
#include "goof_headless_renderer.h"
#include "goof_run_frame_adapter.h"
#include "sha256.h"
#include "snes.h"
#include "spc.h"

enum { kMaxEpochs = 200, kNormativeEpochs = 10 };

/* Validated no-render campaign, RE-BASED three times (third: see below).
 *
 * First after the physical-frame-boundary milestone: the sequence before that
 * encoded one NMI per logical epoch, which was the defect.
 *
 * Again after GOOF_TIER_UNIFORM_MASTER_CLOCK_IMPLEMENTATION: the logical hash
 * covers all of WRAM, and the NMI now interrupts the guest at the same PC the
 * fully-interpreted tier does, so the guest reaches each epoch boundary having
 * done its work at slightly different moments.  The FRAMEBUFFER pins below did
 * NOT move -- neither did the E6/E21 framebuffers, PBN1, PBN3 or PBN10 -- which
 * is why this is a cadence re-base and not a rendering change.
 *
 * These values were recorded only after PBN1/PBN3/PBN4/PBN8/PBN10 passed in
 * BOTH execution tiers in the same build, and after human visual validation. */
/* Re-based a third time after GOOF_CPU_DMA_REFRESH_MASTER_CLOCK_IMPLEMENTATION
 * (bus-speed CPU + General DMA + DRAM refresh): E1 is now 121 physical
 * periods instead of 143, so every WRAM-covering logical hash moves.  Recorded
 * after the bsnes $0070 oracle (79/80 events at 0 frames), PBN1/PBN3/PBN6/
 * PBN10 and human validation, in the same build. */
static const uint64_t kExpectedLogicalHash[kNormativeEpochs + 1] = {
  0,
  UINT64_C(0x7d31c6dbe05b8291), UINT64_C(0xc38d4230bca98aa1),
  UINT64_C(0xfe9888b1f41be20e), UINT64_C(0xf333104191068a13),
  UINT64_C(0xf671f52c1a9f2bf8), UINT64_C(0xc4b2e933f5b26f15),
  UINT64_C(0xde09f7631e0f9ff2), UINT64_C(0xe585194ad02265a7),
  UINT64_C(0xd5dce325ee81eb1c), UINT64_C(0xe801c28bf215d059),
};
/* Validated visual baseline (read_only_ppu_render_probe.md): the INIDISP=$0B
 * frame of the credits fade.  It was E10; the shorter loader epoch moved the
 * fade 21 epochs later on the logical axis, and the SAME frame (same FNV-1a,
 * same SHA-256) now renders at E31 -- the renderer did not change, only the
 * epoch that reaches it. */
enum { kVisualBaselineEpoch = 31 };
#define GOOF_VISUAL_BASELINE_LOGICAL UINT64_C(0xb9803fe764014e8d)
#define GOOF_E10_FRAMEBUFFER_FNV1A64 UINT64_C(0x01f3932bfc8670ad)
static const char kE10FramebufferSha256[] =
    "a206f6bce47fba7cabb8c30547082c01510dd3eef9f94524a291f52a97f7c5ca";

static GoofRunFrameAdapter g_run_frame;
static GoofHeadlessRenderer g_renderer;
static uint64_t g_mx_entries, g_mx_mismatches, g_mx_suffixless;
/* Any guest/bus/MMIO/generated-code activity while this is set is a
 * violation of the observational contract. */
static int g_in_render;
static uint64_t g_render_violations;
static uint64_t g_uploaded_spc_hash;
static unsigned g_upload_rom_disable_events;
static uint64_t g_ipl_image_hash, g_ipl_header_echoes;
static uint64_t g_driver_port2_ready_events;

static void gate_fail(const char *tag, const char *condition, int line) {
  printf("%s line=%d condition=%s\n", tag, line, condition);
  fflush(stdout);
  exit(1);
}

#define GATE_CHECK(condition) \
  do { if (!(condition)) gate_fail("HEADLESS_GATE FAIL", #condition, __LINE__); } while (0)

uint8 __real_ReadReg(uint16 reg);
uint16 __real_ReadRegWord(uint16 reg);
void __real_WriteReg(uint16 reg, uint8 value);
void __real_WriteRegWord(uint16 reg, uint16 value);
uint8 __wrap_ReadReg(uint16 reg) {
  g_render_violations += g_in_render;
  return __real_ReadReg(reg);
}
uint16 __wrap_ReadRegWord(uint16 reg) {
  g_render_violations += g_in_render;
  return __real_ReadRegWord(reg);
}
void __wrap_WriteReg(uint16 reg, uint8 value) {
  g_render_violations += g_in_render;
  __real_WriteReg(reg, value);
}
void __wrap_WriteRegWord(uint16 reg, uint16 value) {
  g_render_violations += g_in_render;
  __real_WriteRegWord(reg, value);
}

void cpu_trace_func_entry(CpuState *cpu, uint32_t pc24, const char *name) {
  (void)pc24;
  uint8_t expected_m, expected_x;
  g_render_violations += g_in_render;
  g_mx_entries++;
  if (!goof_gate_parse_mx_suffix(name, &expected_m, &expected_x)) {
    g_mx_suffixless++;
    return;
  }
  if ((cpu->m_flag & 1) != expected_m || (cpu->x_flag & 1) != expected_x)
    g_mx_mismatches++;
}

static uint8_t logical_bus_read(void *mem, uint32_t address) {
  CpuState *cpu = mem;
  g_render_violations += g_in_render;
  return cpu_read8(cpu, (uint8_t)(address >> 16), (uint16_t)address);
}

static void logical_bus_write(void *mem, uint32_t address, uint8_t value) {
  CpuState *cpu = mem;
  g_render_violations += g_in_render;
  cpu_write8(cpu, (uint8_t)(address >> 16), (uint16_t)address, value);
}

static void logical_event(void *mem, bool nmi_edge, uint16_t joy1,
                          uint16_t joy2) {
  (void)mem;
  g_render_violations += g_in_render;
  g_snes->input1_currentState = joy1;
  g_snes->input2_currentState = joy2;
  if (nmi_edge) g_snes->inNmi = true;
}

void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

static const RtlGameInfo kGoofHeadlessInfo = {
  .title = "goof-headless-frame-pipeline",
  .initialize = NULL,
  .run_frame = goof_boot_harness_run_frame,
  .draw_ppu_frame = NULL,
  .save_name_prefix = "goof-headless-frames",
};

void __real_apu_cpuWrite(Apu *apu, uint16_t adr, uint8_t value);
void __wrap_apu_cpuWrite(Apu *apu, uint16_t adr, uint8_t value) {
  bool rom_was_readable = apu->romReadable;
  __real_apu_cpuWrite(apu, adr, value);
  if (rom_was_readable && !apu->romReadable) {
    g_uploaded_spc_hash = goof_gate_fnv1a64(apu->ram, sizeof(apu->ram));
    g_upload_rom_disable_events++;
  }
}

void __real_audio_trace_on_spc_port_write(uint8_t port, uint8_t value);
void __wrap_audio_trace_on_spc_port_write(uint8_t port, uint8_t value) {
  uint16_t pc = g_snes->apu->spc->pc;
  if (pc >= 0xfff5 && pc <= 0xfff9) {
    g_ipl_image_hash = goof_gate_fnv1a64(g_snes->apu->ram,
                               sizeof(g_snes->apu->ram));
    g_ipl_header_echoes++;
  }
  if ((port & 3) == 2 && value == 0x01 && !g_snes->apu->romReadable)
    g_driver_port2_ready_events++;
  __real_audio_trace_on_spc_port_write(port, value);
}

/* Focused live-state blocks compared byte-for-byte around every render.
 * Pointer-bearing structs are compared only within one process, never
 * printed as cross-process evidence. */
typedef struct {
  const char *name;
  const void *live;
  size_t size;
  uint8_t *before;
} LiveBlock;

static LiveBlock g_blocks[16];
static unsigned g_block_count;

static void add_block(const char *name, const void *live, size_t size) {
  GATE_CHECK(live != NULL && size != 0);
  GATE_CHECK(g_block_count < sizeof(g_blocks) / sizeof(g_blocks[0]));
  LiveBlock *b = &g_blocks[g_block_count++];
  b->name = name;
  b->live = live;
  b->size = size;
  b->before = malloc(size);
  GATE_CHECK(b->before != NULL);
}

static void register_live_blocks(void) {
  add_block("wram", g_ram, 0x20000);
  add_block("run_frame_adapter", &g_run_frame, sizeof(g_run_frame));
  add_block("cpu_state", &g_cpu, sizeof(g_cpu));
  add_block("recomp_stack", g_cpu_entry_s, sizeof(uint16_t) * 64);
  add_block("recomp_stack_top", &g_recomp_stack_top, sizeof(g_recomp_stack_top));
  add_block("snes", g_snes, sizeof(Snes));
  add_block("ppu", g_ppu, sizeof(Ppu));
  add_block("dma", g_dma, sizeof(Dma));
  add_block("apu", g_snes->apu, sizeof(Apu));
  add_block("spc", g_snes->apu->spc, sizeof(Spc));
  add_block("dsp", g_snes->apu->dsp, sizeof(Dsp));
  add_block("snes_cpu", g_snes->cpu, sizeof(Cpu));
  add_block("cart", g_snes->cart, sizeof(Cart));
  add_block("last_hdmaen", &g_snesrecomp_last_hdmaen, 1);
  add_block("snes_frame_counter", &snes_frame_counter, sizeof(snes_frame_counter));
}

static void capture_live_blocks(void) {
  for (unsigned i = 0; i < g_block_count; i++)
    memcpy(g_blocks[i].before, g_blocks[i].live, g_blocks[i].size);
}

static size_t changed_live_bytes(void) {
  size_t changed = 0;
  for (unsigned i = 0; i < g_block_count; i++) {
    const uint8_t *now = g_blocks[i].live;
    size_t block_changed = 0;
    for (size_t j = 0; j < g_blocks[i].size; j++)
      block_changed += now[j] != g_blocks[i].before[j];
    if (block_changed)
      printf("LIVE_BLOCK_CHANGED block=%s bytes=%zu\n", g_blocks[i].name,
             block_changed);
    changed += block_changed;
  }
  return changed;
}

static const char *display_state(const Ppu *p) {
  if (p->inidisp & 0x80) return "forced_blank";
  if ((p->inidisp & 0x0f) == 0) return "brightness_0";
  if (p->screenEnabled[0] == 0 && p->screenEnabled[1] == 0) return "no_layers";
  return "display_on";
}

typedef struct {
  unsigned epochs;
  bool dump[kMaxEpochs + 1];
  const char *output_dir;
} Options;

static void usage_fail(void) {
  fprintf(stderr, "usage: goof_headless_frames ROM [--epochs N(1..%d)] "
                  "[--dump-frame N]... [--output-dir DIR]\n", kMaxEpochs);
  exit(2);
}

static unsigned parse_epoch(const char *text) {
  char *end;
  unsigned long v = strtoul(text, &end, 10);
  if (*text == '\0' || *end != '\0' || v < 1 || v > kMaxEpochs) usage_fail();
  return (unsigned)v;
}

static Options parse_options(int argc, char **argv) {
  Options o = {.epochs = kMaxEpochs};
  bool any_dump = false;
  for (int i = 2; i < argc; i++) {
    if (i + 1 >= argc) usage_fail();
    if (strcmp(argv[i], "--epochs") == 0) {
      o.epochs = parse_epoch(argv[++i]);
    } else if (strcmp(argv[i], "--dump-frame") == 0) {
      o.dump[parse_epoch(argv[++i])] = true;
      any_dump = true;
    } else if (strcmp(argv[i], "--output-dir") == 0) {
      o.output_dir = argv[++i];
    } else {
      usage_fail();
    }
  }
  if (any_dump && !o.output_dir) usage_fail();
  return o;
}

static void render_boundary(unsigned epoch, const Options *opt) {
  const GuestExecution *g = &g_run_frame.execution;
  const Ppu *const live_ppu = g_ppu;
  uint64_t logical_before = goof_gate_logical_epoch_hash_v1(g);
  uint8_t zp_9d = g_ram[0x9d], zp_9e = g_ram[0x9e];
  int recomp_top = g_recomp_stack_top;
  unsigned host_depth = g->host_depth;
  uint64_t vram = goof_gate_fnv1a64(live_ppu->vram, sizeof(live_ppu->vram));
  uint64_t oam = goof_gate_oam_hash(live_ppu);
  uint64_t cgram = goof_gate_fnv1a64(live_ppu->cgram, sizeof(live_ppu->cgram));
  capture_live_blocks();

  g_in_render = 1;
  bool rendered = goof_headless_render(&g_renderer, live_ppu, g_dma, epoch,
                                       logical_before);
  g_in_render = 0;
  GATE_CHECK(rendered);
  GATE_CHECK(g_render_violations == 0);
  goof_headless_frame_measure(&g_renderer);
  const GoofHeadlessFrameMeta *m = &g_renderer.meta;

  /* The snapshot must have been consumed by the render (non-vacuous proof)
   * and must never alias the live PPU or its framebuffer binding. */
  const Ppu *snap = &g_renderer.snapshot;
  GATE_CHECK(snap->renderBuffer == (uint8_t *)g_renderer.pixels);
  GATE_CHECK(snap->evenFrame != live_ppu->evenFrame);
  GATE_CHECK(live_ppu->renderBuffer != (uint8_t *)g_renderer.pixels);
  size_t snapshot_mutated = 0;
  for (size_t i = 0; i < sizeof(Ppu); i++)
    snapshot_mutated += ((const uint8_t *)snap)[i] != ((const uint8_t *)live_ppu)[i];
  bool guest_visible_if_in_place =
      snap->evenFrame != live_ppu->evenFrame ||
      snap->rangeOver != live_ppu->rangeOver ||
      snap->timeOver != live_ppu->timeOver;

  char ppm[4096] = "-";
  if (opt->dump[epoch]) {
    GATE_CHECK(snprintf(ppm, sizeof(ppm), "%s/goof_epoch%02u.ppm",
                        opt->output_dir, epoch) < (int)sizeof(ppm));
    GATE_CHECK(goof_headless_write_ppm(&g_renderer, ppm));
  }
  goof_headless_discard(&g_renderer);

  GATE_CHECK(changed_live_bytes() == 0);
  GATE_CHECK(g_ppu == live_ppu);
  GATE_CHECK(goof_gate_logical_epoch_hash_v1(g) == logical_before);
  GATE_CHECK(g_ram[0x9d] == zp_9d && g_ram[0x9e] == zp_9e);
  GATE_CHECK(g_recomp_stack_top == recomp_top && g->host_depth == host_depth);
  GATE_CHECK(goof_gate_fnv1a64(live_ppu->vram, sizeof(live_ppu->vram)) == vram);
  GATE_CHECK(goof_gate_oam_hash(live_ppu) == oam);
  GATE_CHECK(goof_gate_fnv1a64(live_ppu->cgram, sizeof(live_ppu->cgram)) == cgram);

  char sha_hex[65];
  for (int i = 0; i < 32; i++) snprintf(sha_hex + 2 * i, 3, "%02x", m->sha256[i]);
  printf("FRAME epoch=%u logical=%016llx fb_fnv1a64=%016llx fb_sha256=%s",
         epoch, (unsigned long long)m->logical_hash,
         (unsigned long long)m->fnv1a64, sha_hex);
  printf(" size=%ux%u unique_colors=%u non_black=%u display=%s inidisp=%02X "
         "tm=%02X ts=%02X 9D=%02X 9E=%02X vram=%016llx oam=%016llx "
         "cgram=%016llx snapshot_mutated_bytes=%zu "
         "guest_visible_if_in_place=%s live_changed_bytes=0 ppm=%s\n",
         m->width, m->height, m->unique_colors, m->non_black_pixels,
         display_state(live_ppu), live_ppu->inidisp,
         live_ppu->screenEnabled[0], live_ppu->screenEnabled[1], zp_9d, zp_9e,
         (unsigned long long)vram, (unsigned long long)oam,
         (unsigned long long)cgram, snapshot_mutated,
         guest_visible_if_in_place ? "YES" : "NO", ppm);

  if (epoch == kVisualBaselineEpoch) {
    GATE_CHECK(m->logical_hash == GOOF_VISUAL_BASELINE_LOGICAL);
    GATE_CHECK(m->width == 256 && m->height == 224);
    GATE_CHECK(m->fnv1a64 == GOOF_E10_FRAMEBUFFER_FNV1A64);
    GATE_CHECK(strcmp(sha_hex, kE10FramebufferSha256) == 0);
    printf("E10_FRAMEBUFFER_REGRESSION PASS epoch=31 logical=%016llx fb_fnv1a64=%016llx "
           "size=%ux%u\n", (unsigned long long)m->logical_hash,
           (unsigned long long)m->fnv1a64, m->width, m->height);
  }
  /* Credits plateau and the last pre-APU black frame, at their post-fix
   * epoch indices.  Both framebuffer hashes are UNCHANGED by the cadence
   * fix -- neither scene renders through VRAM tile 0 -- which is itself
   * evidence that the fix moved timing and not the renderer. */
  if (epoch == 27) GATE_CHECK(m->fnv1a64 == UINT64_C(0x2fe663de2c1a1c25));
  if (epoch == 42) GATE_CHECK(m->fnv1a64 == UINT64_C(0x06644c0aa7470383));
}

static void verify_apu_visibility_modes(void) {
  Apu *apu = g_snes->apu;
  GATE_CHECK(!rtl_apu_frame_timeline_active());
  GATE_CHECK(apu_portQueueDepth(apu) == 0);

  uint8_t immediate = (uint8_t)(apu->inPorts[3] ^ 0x5a);
  RtlApuWrite(0x2143, immediate);
  GATE_CHECK(apu->inPorts[3] == immediate);
  GATE_CHECK(apu_portQueueDepth(apu) == 0);

  rtl_apu_frame_timeline_begin();
  GATE_CHECK(rtl_apu_frame_timeline_active());
  uint8_t deferred = (uint8_t)(immediate ^ 0xa5);
  uint64_t before_cycles = apu->cycles;
  RtlApuWrite(0x2143, deferred);
  GATE_CHECK(apu->inPorts[3] == immediate);
  GATE_CHECK(apu_portQueueDepth(apu) == 1);
  GATE_CHECK(apu->cycles == before_cycles);

  uint8_t old0 = apu->inPorts[0];
  uint8_t old1 = apu->inPorts[1];
  uint64_t old_interleaves = g_apu_word_interleave_events;
  RtlApuWriteWord(0x2140, 0x6d2b);
  GATE_CHECK(apu->inPorts[0] == old0 && apu->inPorts[1] == old1);
  GATE_CHECK(apu_portQueueDepth(apu) == 3);
  GATE_CHECK(apu->cycles == before_cycles);
  GATE_CHECK(g_apu_word_interleave_events == old_interleaves);
  GATE_CHECK(apu->portQueue[(apu->portQHead + 1) & (APU_PORT_QUEUE_LEN - 1)].port == 1);
  GATE_CHECK(apu->portQueue[(apu->portQHead + 1) & (APU_PORT_QUEUE_LEN - 1)].val == 0x6d);
  GATE_CHECK(apu->portQueue[(apu->portQHead + 2) & (APU_PORT_QUEUE_LEN - 1)].port == 0);
  GATE_CHECK(apu->portQueue[(apu->portQHead + 2) & (APU_PORT_QUEUE_LEN - 1)].val == 0x2b);
  GATE_CHECK(apu->portQueue[(apu->portQHead + 1) & (APU_PORT_QUEUE_LEN - 1)].target_cycle ==
             apu->portQueue[(apu->portQHead + 2) & (APU_PORT_QUEUE_LEN - 1)].target_cycle);
  printf("APUIO_VISIBILITY_MODES PASS no_frame=IMMEDIATE frame=DEFERRED "
         "word_atomic=YES spc_between_halves=0\n");
}

int main(int argc, char **argv) {
  if (argc < 2) usage_fail();
  Options opt = parse_options(argc, argv);
  setvbuf(stdout, NULL, _IOLBF, 0);
  size_t rom_size;
  uint8_t *rom = goof_gate_load_pinned_rom(argv[1], &rom_size);
  GATE_CHECK(rom != NULL);

  RtlRegisterGame(&kGoofHeadlessInfo);
  GATE_CHECK(SnesInit(rom, (int)rom_size));
  cpu_state_init(&g_cpu, g_ram);
  Interp816 reset = {.mem = &g_cpu, .read = logical_bus_read,
                     .write = logical_bus_write, .exact_pb = true};
  interp816_reset(&reset);
  GATE_CHECK(goof_run_frame_adapter_init(&g_run_frame, &reset, 0x1dff,
      g_aot_entry_registry, g_aot_entry_registry_count, g_ram,
      logical_event, 50000000));
  goof_run_frame_adapter_bind(&g_run_frame);

  const GuestExecution *g = &g_run_frame.execution;
  GATE_CHECK(g_run_frame.last_result.reason == WAITING);
  GATE_CHECK(g_run_frame.last_result.certified && g->certified);
  GATE_CHECK(g->cursor.cpu.k == 0x80 && g->cursor.cpu.pc == 0x80a7);
  GATE_CHECK(g->cursor.cpu.sp == 0x1dff);
  /* Reset boundary: no NMI handler may be in flight.  The bootstrap itself
   * may service NMIs: with bus-speed CPU timing the $4200=$B1 write lands
   * before physical boundary 1, so that boundary's edge is delivered -- a
   * chronology consequence (GOOF_CPU_DMA_REFRESH), not a new behaviour. */
  GATE_CHECK(g->nmis == g->epilogues && !g->in_nmi && g->host_depth == 0 &&
             g_recomp_stack_top == 0);
  register_live_blocks();

  for (unsigned epoch = 1; epoch <= opt.epochs; epoch++) {
    alarm(30);
    kGoofHeadlessInfo.run_frame(); /* aborts unless EPOCH_COMPLETE_WAITING */
    alarm(0);
    GATE_CHECK(g_run_frame.last_result.reason == EPOCH_COMPLETE_WAITING);
    GATE_CHECK(g_run_frame.last_result.certified && g->certified);
    /* One logical epoch : N physical frame periods, N >= 1.  `nmis == epoch`
     * is retired with the cadence defect it described; what is asserted now
     * is that the epoch was serviced and that entry/epilogue balance. */
    GATE_CHECK(g_run_frame.logical_nmi_epochs == epoch);
    GATE_CHECK(g_run_frame.last_result.nmi_entries >= 1);
    GATE_CHECK(g_run_frame.last_result.physical_periods >=
               g_run_frame.last_result.nmi_entries);
    GATE_CHECK(g->nmis == g->epilogues && g->nmis == g->nmi_entries);
    GATE_CHECK(g->cursor.cpu.k == 0x80 && g->cursor.cpu.pc == 0x80a7);
    GATE_CHECK(g->cursor.cpu.sp == 0x1dff);
    GATE_CHECK(g->host_depth == 0 && g_recomp_stack_top == 0);
    GATE_CHECK(!g->cursor.cpu.irqWanted && !g->active_plan.irq);
    GATE_CHECK(!g->cursor.cpu.waiting && !g->cursor.cpu.stopped);
    GATE_CHECK(CpuUnresolvedAbandonTotal() == 0);
    GATE_CHECK(g_mx_mismatches == 0 && g_mx_suffixless == 0);
    uint64_t logical = goof_gate_logical_epoch_hash_v1(g);
    if (epoch <= kNormativeEpochs && logical != kExpectedLogicalHash[epoch]) {
      printf("EPOCH epoch=%u logical=%016llx expected=%016llx\n", epoch,
             (unsigned long long)logical,
             (unsigned long long)kExpectedLogicalHash[epoch]);
      gate_fail("HEADLESS_PIPELINE_INVASIVE", "logical == expected", __LINE__);
    }
    /* Semantic anchors, at their post-fix epoch indices (the spine is the
     * same; a logical epoch is now one physical frame period in gameplay, so
     * every scene arrives at a lower index). */
    if (epoch == 27) GATE_CHECK(logical == UINT64_C(0xb7f5b5d2cc95fe3f));
    if (epoch == 42) GATE_CHECK(logical == UINT64_C(0x2a7bbcc770e8e9c6));
    if (epoch == 43) GATE_CHECK(logical == UINT64_C(0x8f7bad25c295450c));
    render_boundary(epoch, &opt);
    if (epoch >= 43) {   /* the SPC IPL upload completes at E43 (was E22, E154) */
      printf("APU epoch=%u spc_pc=%04X cycles=%llu romReadable=%u "
             "inPorts=%02X%02X%02X%02X outPorts=%02X%02X%02X%02X "
             "ram_fnv1a64=%016llx\n", epoch, g_snes->apu->spc->pc,
             (unsigned long long)g_snes->apu->cycles,
             g_snes->apu->romReadable ? 1u : 0u,
             g_snes->apu->inPorts[3], g_snes->apu->inPorts[2],
             g_snes->apu->inPorts[1], g_snes->apu->inPorts[0],
             g_snes->apu->outPorts[3], g_snes->apu->outPorts[2],
             g_snes->apu->outPorts[1], g_snes->apu->outPorts[0],
             (unsigned long long)goof_gate_fnv1a64(g_snes->apu->ram,
                                         sizeof(g_snes->apu->ram)));
      GATE_CHECK(g_apu_word_transactions != 0);
      GATE_CHECK(g_apu_word_interleave_events == 0);
      GATE_CHECK(g_upload_rom_disable_events != 0);
      printf("IPL_CAPTURE image=%016llx header_echoes=%llu "
             "rom_disable_image=%016llx disable_events=%u\n",
             (unsigned long long)g_ipl_image_hash,
             (unsigned long long)g_ipl_header_echoes,
             (unsigned long long)g_uploaded_spc_hash,
             g_upload_rom_disable_events);
      GATE_CHECK(g_ipl_header_echoes == 3);
      GATE_CHECK(g_ipl_image_hash == UINT64_C(0x7bbbaa3712cba454));
      GATE_CHECK(!g_snes->apu->romReadable);
      GATE_CHECK(g_driver_port2_ready_events != 0);
      printf("IPL_UPLOAD image=%016llx stale_latches=%llu word_transactions=%llu "
             "romReadable=%u port2142_ready_events=%llu\n",
             (unsigned long long)g_ipl_image_hash,
             (unsigned long long)g_apu_word_interleave_events,
             (unsigned long long)g_apu_word_transactions,
             g_snes->apu->romReadable ? 1u : 0u,
             (unsigned long long)g_driver_port2_ready_events);
    }
  }

  GATE_CHECK(g_mx_entries != 0);
  verify_apu_visibility_modes();
  printf("GOOF_HEADLESS_FRAME_PIPELINE PASS epochs=%u logical_sequence=MATCH "
         "render_violations=%llu live_state_changed=NO recomp=%d host=%u "
         "mismatch=%llu abandon=%llu\n", opt.epochs,
         (unsigned long long)g_render_violations, g_recomp_stack_top,
         g->host_depth, (unsigned long long)g_mx_mismatches,
         (unsigned long long)CpuUnresolvedAbandonTotal());
  free(rom);
  return 0;
}
