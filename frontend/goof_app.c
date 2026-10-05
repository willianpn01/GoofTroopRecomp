/* Goof Troop Recomp -- application coordinator.  SDL-free by construction:
 * this translation unit must never gain an SDL include, a wall-clock read or
 * a host event.  See goof_app.h and design sections 6, 7, 8 and 22. */
#include "goof_app.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apu.h"
#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "dsp.h"
#include "cpu_state.h"
#include "goof_gate_common.h"
#include "goof_headless_renderer.h"
#include "goof_input.h"
#include "goof_run_frame_adapter.h"
#include "sha256.h"
#include "snes.h"
#include "spc.h"

/* Same budget the permanent gates hand goof_run_frame_adapter_advance. */
enum { GOOF_APP_EPOCH_BUDGET = 20000000 };
enum { GOOF_APP_RESET_BUDGET = 50000000 };
/* Scheduler stack pointer of the validated cursor contract. */
enum { GOOF_APP_SCHEDULER_S = 0x1dff };
/* Canonical headerless ROM size, and the SMC copier header length. */
enum { GOOF_APP_ROM_SIZE = 524288, GOOF_APP_SMC_HEADER = 512 };

struct GoofApp {
  uint8_t *rom;
  size_t rom_size;
  GoofRunFrameAdapter adapter;
  GoofHeadlessRenderer renderer;
  uint64_t epoch;
  uint64_t logical_hash;
  uint64_t framebuffer_hash;
  /* Per-epoch physical-frame accounting, carried for diagnostics only. */
  uint64_t physical_periods, nmi_requests, nmi_entries;
  uint64_t idle_padding_master_cycles;
  /* GOOF_INPUT_V1.  `sample` is the epoch's immutable guest-visible joypad
   * state, normalised at the seam; `latched_*` is what the last hardware
   * event actually wrote into the engine's auto-joypad result registers.
   * They are separate because the two answer different questions: one is
   * "what did the host present for this epoch", the other is "what has the
   * guest been able to read so far". */
  GoofInputSample sample;
  uint16_t latched_p1, latched_p2;
  GoofInputDigest input_digest;
  bool rendered;
  /* Host presentation wiring (GOOF_AUDIO_PHYSICAL_BOUNDARY_PUMP_V1).  Not
   * guest state and never hashed: it only decides whether an observer is
   * told that a physical boundary closed. */
  GoofAppBoundaryHook boundary_hook;
  void *boundary_hook_user;
  char error[512];
};

/* The engine state this coordinator drives is global and not re-entrant, so
 * exactly one app may be live at a time. */
static GoofApp *s_live_app;

/* --------------------------------------------------------------------- */
/* Host hooks the engine expects the application to provide.              */
/* Single-threaded by policy (design section 21): the locks are no-ops,   */
/* exactly as in the validated gate path.                                 */
/* --------------------------------------------------------------------- */

void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

/* M/X entry gate.  Called by the generated AOT banks on every function entry
 * because the shared core compiles them with SNESRECOMP_MX_ENTRY_GATE=1, the
 * same definition the permanent gates use.  Counted, never fatal: the
 * permanent M/X verifier owns the assertion, this is the player's own
 * evidence that its run saw no mismatch. */
static uint64_t s_mx_entries, s_mx_mismatches;

void cpu_trace_func_entry(CpuState *cpu, uint32_t pc24, const char *name) {
  (void)pc24;
  uint8_t expected_m, expected_x;
  s_mx_entries++;
  if (!goof_gate_parse_mx_suffix(name, &expected_m, &expected_x)) {
    s_mx_mismatches++;
    return;
  }
  if ((cpu->m_flag & 1) != expected_m || (cpu->x_flag & 1) != expected_x)
    s_mx_mismatches++;
}

void goof_app_mx_stats(uint64_t *entries, uint64_t *mismatches) {
  if (entries) *entries = s_mx_entries;
  if (mismatches) *mismatches = s_mx_mismatches;
}

static uint8_t goof_app_bus_read(void *mem, uint32_t address) {
  CpuState *cpu = mem;
  return cpu_read8(cpu, (uint8_t)(address >> 16), (uint16_t)address);
}

static void goof_app_bus_write(void *mem, uint32_t address, uint8_t value) {
  CpuState *cpu = mem;
  cpu_write8(cpu, (uint8_t)(address >> 16), (uint16_t)address, value);
}

/* THE GUEST-VISIBLE JOYPAD LATCH.  Delivered by goof_frame_driver at plan
 * start and again at every internal physical-boundary NMI edge; joy1/joy2
 * arrive from the FramePlan and are therefore constant for the whole epoch
 * (GOOF_INPUT_V1, goof_run_frame_adapter_advance_input).
 *
 * This is the ONLY place a host-originated value becomes guest-visible, and
 * all it does is write the engine's two auto-joypad state words.  The engine
 * derives $4218/$4219/$421A/$421B from them with SwapInputBits() exactly as
 * before -- no engine change, and no write to gameplay RAM from here, ever.
 *
 * The digest is taken HERE, at the latch, with the guest master clock as the
 * timestamp: a host event time never becomes input identity. */
static void goof_app_hardware_event(void *mem, bool nmi_edge, uint16_t joy1,
                                    uint16_t joy2) {
  (void)mem;
  g_snes->input1_currentState = joy1;
  g_snes->input2_currentState = joy2;
  if (s_live_app) {
    s_live_app->latched_p1 = joy1;
    s_live_app->latched_p2 = joy2;
    goof_input_digest_update(&s_live_app->input_digest, g_cpu.master_cycles,
                             joy1, joy2);
  }
  if (nmi_edge) g_snes->inNmi = true;
}

static const RtlGameInfo kGoofAppGameInfo = {
  .title = "goof-troop-recomp",
  .initialize = NULL,
  .run_frame = goof_boot_harness_run_frame,
  .draw_ppu_frame = NULL,
  .save_name_prefix = "goof-troop-recomp",
};

/* --------------------------------------------------------------------- */
/* ROM identity                                                           */
/* --------------------------------------------------------------------- */

static void goof_app_fail(GoofApp *app, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void goof_app_fail(GoofApp *app, const char *fmt, ...) {
  if (!app) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(app->error, sizeof(app->error), fmt, ap);
  va_end(ap);
}

static void goof_app_hex(const uint8_t *bytes, size_t n, char *out) {
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = kHex[bytes[i] >> 4];
    out[i * 2 + 1] = kHex[bytes[i] & 0xf];
  }
  out[n * 2] = '\0';
}

/* Loads the external ROM and verifies the pinned canonical identity.
 *
 * A perfectly valid dump may carry a 512-byte SMC copier header.  That
 * prefix is a container artefact, not ROM content, so it is removed before
 * hashing -- and only when the file size is exactly 512 bytes beyond a
 * 1 KiB multiple, which is the structural rule for such a header.  The
 * verification itself is NOT relaxed: after normalisation the payload must
 * match the pinned size and the pinned SHA-256 exactly, or the load fails
 * closed with expected-vs-observed diagnostics.  Design section 26. */
static uint8_t *goof_app_load_rom(GoofApp *app, const char *path,
                                  size_t *size_out) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    goof_app_fail(app, "rom=%s condition=open failed", path);
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long length = ftell(f);
  rewind(f);
  if (length <= 0) {
    fclose(f);
    goof_app_fail(app, "rom=%s condition=empty file", path);
    return NULL;
  }
  uint8_t *file = malloc((size_t)length);
  if (!file) { fclose(f); return NULL; }
  if (fread(file, 1, (size_t)length, f) != (size_t)length) {
    free(file); fclose(f);
    goof_app_fail(app, "rom=%s condition=short read", path);
    return NULL;
  }
  fclose(f);

  size_t offset = 0, payload = (size_t)length;
  bool smc_header = ((size_t)length % 1024u) == GOOF_APP_SMC_HEADER;
  if (smc_header) {
    offset = GOOF_APP_SMC_HEADER;
    payload = (size_t)length - GOOF_APP_SMC_HEADER;
  }
  if (payload != GOOF_APP_ROM_SIZE) {
    goof_app_fail(app, "rom=%s condition=size expected=%d observed=%zu "
                  "smc_header=%s", path, GOOF_APP_ROM_SIZE, payload,
                  smc_header ? "stripped" : "none");
    free(file);
    return NULL;
  }
  uint8_t digest[32];
  sha256_compute(file + offset, payload, digest);
  if (memcmp(digest, goof_gate_pinned_rom_sha256, sizeof(digest)) != 0) {
    char expected[65], observed[65];
    goof_app_hex(goof_gate_pinned_rom_sha256, 32, expected);
    goof_app_hex(digest, 32, observed);
    goof_app_fail(app, "rom=%s condition=sha256 expected=%s observed=%s "
                  "smc_header=%s", path, expected, observed,
                  smc_header ? "stripped" : "none");
    free(file);
    return NULL;
  }
  if (offset != 0) memmove(file, file + offset, payload);
  *size_out = payload;
  return file;
}

/* --------------------------------------------------------------------- */
/* Lifecycle                                                              */
/* --------------------------------------------------------------------- */

static uint64_t goof_app_apu_ram_hash(void) {
  const Apu *apu = g_snes->apu;
  return goof_gate_fnv1a64(apu->ram, sizeof(apu->ram));
}

/* Little-endian WRAM word / 24-bit read.  READ-ONLY: this module never writes
 * a byte of guest RAM, and guest input reaches the guest only through the
 * auto-joypad registers (goof_app_hardware_event). */
static uint16_t goof_app_wram16(uint32_t offset) {
  return (uint16_t)(g_ram[offset] | ((uint16_t)g_ram[offset + 1] << 8));
}
static uint32_t goof_app_wram24(uint32_t offset) {
  return (uint32_t)g_ram[offset] | ((uint32_t)g_ram[offset + 1] << 8) |
         ((uint32_t)g_ram[offset + 2] << 16);
}

/* Goof Troop's own input mirrors and Player 1 position; see goof_app.h. */
enum {
  GOOF_WRAM_P1_HELD    = 0x0044, GOOF_WRAM_P1_PRESSED = 0x0048,
  GOOF_WRAM_P2_HELD    = 0x004a, GOOF_WRAM_P2_PRESSED = 0x004e,
  GOOF_WRAM_P1_SUB_X   = 0x0110, GOOF_WRAM_P1_SUB_Y   = 0x0113,
  GOOF_WRAM_P1_HANDS_UP = 0x0103, GOOF_WRAM_P1_HELD_ITEM = 0x0142
};

static void goof_app_fill_diag(const GoofApp *app, GoofAppDiag *diag) {
  if (!diag) return;
  const Apu *apu = g_snes->apu;
  const GuestExecution *execution = &app->adapter.execution;
  const GoofPpuJournal *journal = goof_execution_render_journal(
      execution, g_cpu.master_cycles);
  *diag = (GoofAppDiag){
    .epoch = app->epoch,
    .logical_hash = app->logical_hash,
    .framebuffer_hash = app->framebuffer_hash,
    .spc_pc = apu->spc->pc,
    .spc_cycles = apu->cycles,
    .apu_ram_hash = goof_app_apu_ram_hash(),
    .physical_periods = app->physical_periods,
    .nmi_requests = app->nmi_requests,
    .nmi_entries = app->nmi_entries,
    .idle_padding_master_cycles = app->idle_padding_master_cycles,
    .irq_compares = execution->irq_compare_count,
    .irq_entries = execution->irq_entry_count,
    .irq_acks = execution->irq_ack_count,
    .irq_digest = execution->irq_digest,
    .journal_writes = execution->ppu_journal_writes,
    .journal_digest = execution->ppu_journal_digest,
    .journal_count = journal ? journal->count : 0,
    /* Now really measured; see goof_app.h.  Reported, never paced on. */
    .epoch_duration_frame_periods = (double)app->physical_periods,
    .input_p1 = app->sample.p1,
    .input_p2 = app->sample.p2,
    .input_digest = app->input_digest.hash,
    .input_latches = app->input_digest.latches,
    .guest_p1_held    = goof_app_wram16(GOOF_WRAM_P1_HELD),
    .guest_p1_pressed = goof_app_wram16(GOOF_WRAM_P1_PRESSED),
    .guest_p2_held    = goof_app_wram16(GOOF_WRAM_P2_HELD),
    .guest_p2_pressed = goof_app_wram16(GOOF_WRAM_P2_PRESSED),
    .p1_x = goof_app_wram24(GOOF_WRAM_P1_SUB_X),
    .p1_y = goof_app_wram24(GOOF_WRAM_P1_SUB_Y),
    .p1_hands_up = g_ram[GOOF_WRAM_P1_HANDS_UP],
    .p1_held_item = g_ram[GOOF_WRAM_P1_HELD_ITEM],
  };
}

GoofApp *goof_app_create(const GoofAppConfig *cfg, GoofAppStatus *status) {
  GoofAppStatus ignored;
  if (!status) status = &ignored;
  if (!cfg || !cfg->rom_path) { *status = GOOF_APP_ERR_ARG; return NULL; }
  if (s_live_app) { *status = GOOF_APP_ERR_ARG; return NULL; }

  GoofApp *app = calloc(1, sizeof(*app));
  if (!app) { *status = GOOF_APP_ERR_INIT; return NULL; }

  app->rom = goof_app_load_rom(app, cfg->rom_path, &app->rom_size);
  if (!app->rom) {
    *status = GOOF_APP_ERR_ROM;
    /* Keep the diagnostic reachable for the caller before freeing. */
    static char kept[512];
    snprintf(kept, sizeof(kept), "%s", app->error);
    free(app);
    s_live_app = NULL;
    fprintf(stderr, "[goof_app] %s\n", kept);
    return NULL;
  }

  RtlRegisterGame(&kGoofAppGameInfo);
  if (!SnesInit(app->rom, (int)app->rom_size)) {
    goof_app_fail(app, "stage=SnesInit");
    fprintf(stderr, "[goof_app] init failed: %s\n", app->error);
    free(app->rom); free(app);
    *status = GOOF_APP_ERR_INIT;
    return NULL;
  }
  cpu_state_init(&g_cpu, g_ram);
  Interp816 reset = {.mem = &g_cpu, .read = goof_app_bus_read,
                     .write = goof_app_bus_write, .exact_pb = true};
  interp816_reset(&reset);
  if (!goof_run_frame_adapter_init(&app->adapter, &reset, GOOF_APP_SCHEDULER_S,
                                   g_aot_entry_registry,
                                   g_aot_entry_registry_count, g_ram,
                                   goof_app_hardware_event,
                                   GOOF_APP_RESET_BUDGET)) {
    goof_app_fail(app, "stage=reset_prefix reason=%d",
                  (int)app->adapter.last_result.reason);
    fprintf(stderr, "[goof_app] init failed: %s\n", app->error);
    free(app->rom); free(app);
    *status = GOOF_APP_ERR_INIT;
    return NULL;
  }
  goof_run_frame_adapter_bind(&app->adapter);

  const GuestExecution *g = &app->adapter.execution;
  if (app->adapter.last_result.reason != WAITING ||
      !app->adapter.last_result.certified || !g->certified ||
      g->cursor.cpu.k != 0x80 || g->cursor.cpu.pc != 0x80a7 ||
      g->cursor.cpu.sp != GOOF_APP_SCHEDULER_S ||
      /* no handler in flight; the bootstrap may service NMIs (see gates) */
      g->nmis != g->epilogues || g->in_nmi ||
      g->host_depth != 0 || g_recomp_stack_top != 0) {
    goof_app_fail(app, "stage=reset_boundary pc=%02X:%04X s=%04X certified=%d",
                  g->cursor.cpu.k, g->cursor.cpu.pc, g->cursor.cpu.sp,
                  (int)g->certified);
    fprintf(stderr, "[goof_app] init failed: %s\n", app->error);
    free(app->rom); free(app);
    *status = GOOF_APP_ERR_INIT;
    return NULL;
  }

  /* GOOF_INPUT_V1.  Reset before the app becomes live, so latch ordinal 1 is
   * the first latch of epoch 1 and nothing from the architectural reset
   * prefix can enter the digest.  (The reset FramePlan carries no hardware
   * callback at all, so there is nothing to exclude; this is belt and
   * braces on a contract goof_run_frame_adapter_init owns.) */
  goof_input_digest_reset(&app->input_digest);
  s_live_app = app;
  *status = GOOF_APP_OK;
  return app;
}

GoofAppStatus goof_app_step(GoofApp *app, const GoofInputSample *input,
                            GoofAppDiag *diag) {
  if (!app) return GOOF_APP_ERR_ARG;

  /* GOOF_INPUT_V1.  `input` is the host's sample for THIS epoch and the only
   * channel by which host state can reach the guest.  NULL means neutral, so
   * every caller that does not participate in input is structurally neutral
   * rather than conventionally so, and the neutral baseline is reproduced by
   * a code path that cannot carry a stale mask.
   *
   * Normalised here, once, in the SDL-free core: the live path and the
   * scripted path therefore agree bit-for-bit on what a legal pad state is. */
  app->sample = input ? *input : (GoofInputSample){0, 0};
  goof_input_normalize(&app->sample);

  RunResult r = goof_run_frame_adapter_advance_input(&app->adapter,
                                                     GOOF_APP_EPOCH_BUDGET,
                                                     app->sample.p1,
                                                     app->sample.p2);
  const GuestExecution *g = &app->adapter.execution;
  if (r.reason != EPOCH_COMPLETE_WAITING || !r.certified || !g->certified) {
    goof_app_fail(app, "epoch=%llu reason=%d certified=%d pc=%06X "
                  "consumed=%llu nmis=%llu",
                  (unsigned long long)(app->epoch + 1), (int)r.reason,
                  (int)r.certified, r.pc24, (unsigned long long)r.consumed,
                  (unsigned long long)g->nmis);
    return GOOF_APP_ERR_STEP;
  }
  app->epoch++;
  app->physical_periods = r.physical_periods;
  app->nmi_requests = r.nmi_requests;
  app->nmi_entries = r.nmi_entries;
  app->idle_padding_master_cycles = r.idle_padding_master_cycles;
  /* One logical epoch : N physical frame periods, N >= 1.  `nmis == epoch`
   * is retired: it asserted one NMI per epoch, which was the cadence defect.
   * The epoch must still have been SERVICED, and entries must balance the
   * guest's own NMI epilogues. */
  if (r.nmi_entries < 1 || r.physical_periods < r.nmi_entries ||
      g->nmis != g->epilogues || g->nmis != g->nmi_entries ||
      app->adapter.logical_nmi_epochs != app->epoch ||
      g->cursor.cpu.k != 0x80 || g->cursor.cpu.pc != 0x80a7 ||
      g->cursor.cpu.sp != GOOF_APP_SCHEDULER_S || g_recomp_stack_top != 0 ||
      g->host_depth != 0) {
    goof_app_fail(app, "epoch=%llu condition=boundary_integrity nmis=%llu "
                  "periods=%llu entries=%llu pc=%02X:%04X s=%04X",
                  (unsigned long long)app->epoch,
                  (unsigned long long)g->nmis,
                  (unsigned long long)r.physical_periods,
                  (unsigned long long)r.nmi_entries, g->cursor.cpu.k,
                  g->cursor.cpu.pc, g->cursor.cpu.sp);
    return GOOF_APP_ERR_STEP;
  }
  app->logical_hash = goof_gate_logical_epoch_hash_v1(g);
  goof_app_fill_diag(app, diag);
  return GOOF_APP_OK;
}

GoofAppStatus goof_app_render(GoofApp *app, GoofAppDiag *diag) {
  if (!app) return GOOF_APP_ERR_ARG;
  if (app->epoch == 0) return GOOF_APP_ERR_RENDER;
  /* The live Ppu and Dma are handed over as const and are only read: the
   * renderer copies the Ppu and runs every line -- and every HDMA transfer --
   * on its own snapshot, because ppu_runLine is consumptive.  Nothing below
   * mutates guest state. */
  const Ppu *const live = g_ppu;
  const GoofPpuJournal *journal = goof_execution_render_journal(
      &app->adapter.execution, g_cpu.master_cycles);
  if (!goof_headless_render_with_journal(&app->renderer, live, g_dma,
                                        journal, app->epoch,
                                        app->logical_hash)) {
    goof_app_fail(app, "epoch=%llu condition=256x224 render contract "
                  "violated (widescreen columns or non-window HDMA?)",
                  (unsigned long long)app->epoch);
    return GOOF_APP_ERR_RENDER;
  }
  const GoofHeadlessFrameMeta *m = &app->renderer.meta;
  if (m->width != GOOF_HEADLESS_WIDTH || m->height != GOOF_HEADLESS_HEIGHT) {
    goof_app_fail(app, "epoch=%llu condition=framebuffer geometry %ux%u",
                  (unsigned long long)app->epoch, m->width, m->height);
    return GOOF_APP_ERR_RENDER;
  }
  app->framebuffer_hash = m->fnv1a64;
  app->rendered = true;
  /* Drop the isolated snapshot; the pixels stay valid until the next render. */
  goof_headless_discard(&app->renderer);
  goof_app_fill_diag(app, diag);
  return GOOF_APP_OK;
}

const uint32_t *goof_app_framebuffer(const GoofApp *app, uint32_t *width,
                                     uint32_t *height, uint32_t *pitch) {
  if (!app || !app->rendered) return NULL;
  if (width) *width = GOOF_HEADLESS_WIDTH;
  if (height) *height = GOOF_HEADLESS_HEIGHT;
  if (pitch) *pitch = GOOF_HEADLESS_PITCH;
  return app->renderer.pixels;
}

void goof_app_diagnostics(const GoofApp *app, GoofAppDiag *out) {
  if (!app || !out) return;
  goof_app_fill_diag(app, out);
}

uint64_t goof_app_epoch(const GoofApp *app) { return app ? app->epoch : 0; }

const char *goof_app_last_error(const GoofApp *app) {
  return app ? app->error : "";
}

const char *goof_app_status_text(GoofAppStatus status) {
  switch (status) {
    case GOOF_APP_OK:          return "ok";
    case GOOF_APP_ERR_ARG:     return "invalid argument";
    case GOOF_APP_ERR_ROM:     return "ROM identity";
    case GOOF_APP_ERR_INIT:    return "guest initialisation";
    case GOOF_APP_ERR_STEP:    return "uncertified epoch boundary";
    case GOOF_APP_ERR_RENDER:  return "render contract";
  }
  return "unknown";
}

/* --------------------------------------------------------------------- */
/* Deterministic native-PCM drain -- the host presentation seam.           */
/*                                                                        */
/* CONSUMER ONLY.  Nothing below calls apu_cycle, snes_catchupApu,         */
/* rtl_sync_apu_frame_boundary or any other path that can create APU time, */
/* and nothing below reads a clock or blocks.  These functions move the    */
/* ring's READ cursor and copy bytes out; the guest's production cursor    */
/* and the canonical AUDIO1 digest are untouched, because that digest is   */
/* taken inside dsp_cycle before the ring is ever consulted.               */
/*                                                                        */
/* RtlApuLock/RtlApuUnlock are taken around the ring accesses purely to    */
/* match the engine's documented call contract for dsp_advance (which      */
/* records into audio_trace).  In this single-threaded frontend both are   */
/* no-ops, and no audio thread exists that they could serialise against.   */
/* --------------------------------------------------------------------- */

/* 17088 SPC cycles per physical frame period / 32 APU cycles per DSP tick. */
enum { GOOF_APP_AUDIO_NATIVE_RATE = 32040,
       GOOF_APP_AUDIO_FRAMES_PER_PERIOD = 534 };

static Dsp *goof_app_dsp(const GoofApp *app) {
  if (!app || app != s_live_app) return NULL;
  if (!g_snes || !g_snes->apu) return NULL;
  return g_snes->apu->dsp;
}

uint32_t goof_app_audio_available(const GoofApp *app) {
  const Dsp *dsp = goof_app_dsp(app);
  if (!dsp) return 0;
  RtlApuLock();
  uint32_t avail = dsp_available(dsp);
  RtlApuUnlock();
  return avail;
}

uint32_t goof_app_audio_drain(GoofApp *app, int16_t *out, uint32_t max_frames) {
  Dsp *dsp = goof_app_dsp(app);
  if (!dsp || !out || max_frames == 0) return 0;
  RtlApuLock();
  uint32_t avail = dsp_available(dsp);
  uint32_t take = avail < max_frames ? avail : max_frames;
  for (uint32_t i = 0; i < take; i++)
    dsp_peek(dsp, i, &out[2 * i], &out[2 * i + 1]);
  if (take) dsp_advance(dsp, take);
  RtlApuUnlock();
  return take;
}

void goof_app_audio_discard(GoofApp *app, uint32_t frames) {
  Dsp *dsp = goof_app_dsp(app);
  if (!dsp || frames == 0) return;
  RtlApuLock();
  dsp_advance(dsp, frames);   /* clamps to available internally */
  RtlApuUnlock();
}

/* GOOF_AUDIO_PHYSICAL_BOUNDARY_PUMP_V1.  The hook is stored in the driver's
 * persistent execution context, not in a FramePlan, so it is not part of plan
 * identity and cannot make two otherwise-identical epochs compare unequal.
 *
 * The signatures differ on purpose.  The driver hands its notification the
 * boundary's scheduled deadline in master cycles; this seam drops it.  That
 * value is GUEST TIME, and letting it cross into a host presentation layer
 * would give host code a guest clock to pace on -- exactly the coupling the
 * SDL-free rule in this header exists to prevent.  The host is told THAT a
 * physical boundary completed, which is all a presentation pump needs, and
 * never WHEN in guest time it happened. */
static void goof_app_boundary_trampoline(void *user, uint64_t deadline_master) {
  const GoofApp *app = (const GoofApp *)user;
  (void)deadline_master;
  if (app && app->boundary_hook) app->boundary_hook(app->boundary_hook_user);
}

void goof_app_set_boundary_hook(GoofApp *app, GoofAppBoundaryHook fn,
                                void *user) {
  if (!app) return;
  app->boundary_hook = fn;
  app->boundary_hook_user = user;
  goof_run_frame_adapter_set_boundary_notify(
      &app->adapter, fn ? goof_app_boundary_trampoline : NULL, app);
}

uint32_t goof_app_audio_native_rate(void) {
  return GOOF_APP_AUDIO_NATIVE_RATE;
}

uint32_t goof_app_audio_frames_per_period(void) {
  return GOOF_APP_AUDIO_FRAMES_PER_PERIOD;
}

void goof_app_destroy(GoofApp *app) {
  if (!app) return;
  goof_run_frame_adapter_bind(NULL);
  free(app->rom);
  if (s_live_app == app) s_live_app = NULL;
  free(app);
}
