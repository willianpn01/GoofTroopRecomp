#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "goof_run_frame_adapter.h"
#include "sha256.h"
#include "snes.h"

#define GOOF_LOGICAL_EPOCH_HASH_V1  UINT64_C(0x7d31c6dbe05b8291)

static const uint8_t kExpectedRomSha256[32] = {
  0x2b, 0xb3, 0x68, 0xc4, 0x71, 0x89, 0xce, 0x81,
  0x3a, 0xd7, 0x16, 0xee, 0xf1, 0x6c, 0x01, 0xcd,
  0x47, 0x68, 0x5c, 0xb9, 0x8e, 0x2c, 0x1c, 0xb3,
  0x5f, 0xa6, 0xf0, 0x17, 0x3c, 0x97, 0xdd, 0x7c,
};

static GoofRunFrameAdapter g_run_frame;
static uint64_t g_mx_entries;
static uint64_t g_mx_mismatches;
static uint64_t g_mx_suffixless;

static void gate_fail(const char *condition, int line) {
  fprintf(stderr, "LOGICAL_EPOCH_GATE FAIL line=%d condition=%s\n",
          line, condition);
  exit(1);
}

#define GATE_CHECK(condition) \
  do { if (!(condition)) gate_fail(#condition, __LINE__); } while (0)

static int parse_mx_suffix(const char *name, uint8_t *m, uint8_t *x) {
  size_t n = name ? strlen(name) : 0;
  if (n < 5 || name[n - 5] != '_' || name[n - 4] != 'M' ||
      name[n - 2] != 'X' || (name[n - 3] != '0' && name[n - 3] != '1') ||
      (name[n - 1] != '0' && name[n - 1] != '1'))
    return 0;
  *m = (uint8_t)(name[n - 3] - '0');
  *x = (uint8_t)(name[n - 1] - '0');
  return 1;
}

/* This build-only hook receives the live CpuState from every generated entry
 * without enabling the heavyweight trace rings. */
void cpu_trace_func_entry(CpuState *cpu, uint32_t pc24, const char *name) {
  (void)pc24;
  uint8_t expected_m, expected_x;
  g_mx_entries++;
  if (!parse_mx_suffix(name, &expected_m, &expected_x)) {
    g_mx_suffixless++;
    return;
  }
  if ((cpu->m_flag & 1) != expected_m || (cpu->x_flag & 1) != expected_x)
    g_mx_mismatches++;
}

static uint8_t logical_bus_read(void *mem, uint32_t address) {
  CpuState *cpu = mem;
  return cpu_read8(cpu, (uint8_t)(address >> 16), (uint16_t)address);
}

static void logical_bus_write(void *mem, uint32_t address, uint8_t value) {
  CpuState *cpu = mem;
  cpu_write8(cpu, (uint8_t)(address >> 16), (uint16_t)address, value);
}

static void logical_event(void *mem, bool nmi_edge, uint16_t joy1,
                          uint16_t joy2) {
  (void)mem;
  g_snes->input1_currentState = joy1;
  g_snes->input2_currentState = joy2;
  if (nmi_edge) g_snes->inNmi = true;
}

/* Single-threaded headless gate: no audio thread exists to serialize. */
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

static uint64_t logical_epoch_hash_v1(const GuestExecution *g) {
  uint64_t h = UINT64_C(1469598103934665603);
  for (size_t i = 0; i < 0x20000; i++)
    h = (h ^ g_ram[i]) * UINT64_C(1099511628211);
  const Interp816 *c = &g->cursor.cpu;
  const uint64_t words[] = {
    c->a, c->x, c->y, c->sp, c->pc, c->dp, c->k, c->db,
    interp816_getFlags((Interp816 *)c), g->nmis, g->dispatches, g->min_s,
    (uint64_t)g_recomp_stack_top,
  };
  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
    for (unsigned byte = 0; byte < 8; byte++)
      h = (h ^ (uint8_t)(words[i] >> (8 * byte))) *
          UINT64_C(1099511628211);
  return h;
}

static const RtlGameInfo kGoofPhase4Info = {
  .title = "goof-logical-epoch-gate",
  .initialize = NULL,
  .run_frame = goof_boot_harness_run_frame,
  .draw_ppu_frame = NULL,
  .save_name_prefix = "goof-logical-epoch",
};

static uint8_t *load_pinned_rom(const char *path, size_t *size_out) {
  FILE *f = fopen(path, "rb");
  if (!f) gate_fail("ROM opens", __LINE__);
  GATE_CHECK(fseek(f, 0, SEEK_END) == 0);
  long length = ftell(f);
  GATE_CHECK(length == 524288);
  rewind(f);
  uint8_t *rom = malloc((size_t)length);
  GATE_CHECK(rom != NULL);
  GATE_CHECK(fread(rom, 1, (size_t)length, f) == (size_t)length);
  GATE_CHECK(fclose(f) == 0);
  uint8_t digest[32];
  sha256_compute(rom, (size_t)length, digest);
  GATE_CHECK(memcmp(digest, kExpectedRomSha256, sizeof(digest)) == 0);
  *size_out = (size_t)length;
  return rom;
}

int main(int argc, char **argv) {
  GATE_CHECK(argc == 2);
  size_t rom_size;
  uint8_t *rom = load_pinned_rom(argv[1], &rom_size);

  RtlRegisterGame(&kGoofPhase4Info);
  GATE_CHECK(SnesInit(rom, (int)rom_size));
  cpu_state_init(&g_cpu, g_ram);
  Interp816 reset = {.mem = &g_cpu, .read = logical_bus_read,
                     .write = logical_bus_write, .exact_pb = true};
  interp816_reset(&reset);
  GATE_CHECK(goof_run_frame_adapter_init(&g_run_frame, &reset, 0x1dff,
      g_aot_entry_registry, g_aot_entry_registry_count, g_ram,
      logical_event, 50000000));
  goof_run_frame_adapter_bind(&g_run_frame);

  const GuestExecution *initial = &g_run_frame.execution;
  GATE_CHECK(g_run_frame.last_result.reason == WAITING);
  GATE_CHECK(g_run_frame.last_result.certified && initial->certified);
  GATE_CHECK(initial->cursor.cpu.k == 0x80 &&
             initial->cursor.cpu.pc == 0x80a7);
  GATE_CHECK(initial->cursor.cpu.sp == 0x1dff);
  /* Reset boundary: no NMI handler may be in flight.  The bootstrap itself
   * may service NMIs: with bus-speed CPU timing the $4200=$B1 write lands
   * before physical boundary 1, so that boundary's edge is delivered -- a
   * chronology consequence (GOOF_CPU_DMA_REFRESH), not a new behaviour. */
  GATE_CHECK(initial->nmis == initial->epilogues && !initial->in_nmi &&
             initial->host_depth == 0);
  GATE_CHECK(g_recomp_stack_top == 0);
  GATE_CHECK(!initial->cursor.cpu.irqWanted && !initial->active_plan.irq);
  GATE_CHECK(CpuUnresolvedAbandonTotal() == 0);
  GATE_CHECK(g_mx_mismatches == 0 && g_mx_suffixless == 0);

  const uint64_t bootstrap_nmis = initial->nmis;
  alarm(30);
  kGoofPhase4Info.run_frame();
  alarm(0);

  const GuestExecution *final = &g_run_frame.execution;
  GATE_CHECK(g_run_frame.callbacks == 1 &&
             g_run_frame.logical_nmi_epochs == 1);
  GATE_CHECK(g_run_frame.last_result.reason == EPOCH_COMPLETE_WAITING);
  GATE_CHECK(g_run_frame.last_result.certified && final->certified);
  /* Epoch 1 is the loader: ONE logical epoch spanning 121 NTSC frame
   * periods, servicing one NMI at each (143 under the uniform 8-clock CPU,
   * before GOOF_CPU_DMA_REFRESH_MASTER_CLOCK_IMPLEMENTATION).  The former
   * `nmis == 1` assertion described the cadence defect that the
   * physical-boundary milestone removed.  NMIs the bootstrap itself serviced
   * are not part of epoch 1. */
  GATE_CHECK(final->nmis == bootstrap_nmis + 121);
  GATE_CHECK(g_run_frame.last_result.physical_periods == 121);
  GATE_CHECK(g_run_frame.last_result.nmi_entries == 121);
  GATE_CHECK(g_run_frame.last_result.nmi_requests == 121);
  GATE_CHECK(final->nmis == final->epilogues);
  GATE_CHECK(final->cursor.cpu.k == 0x80 && final->cursor.cpu.pc == 0x80a7);
  GATE_CHECK(final->cursor.cpu.sp == 0x1dff);
  GATE_CHECK(final->host_depth == 0 && g_recomp_stack_top == 0);
  GATE_CHECK(!final->cursor.cpu.irqWanted && !final->active_plan.irq);
  GATE_CHECK(!final->cursor.cpu.waiting && !final->cursor.cpu.stopped);
  GATE_CHECK(CpuUnresolvedAbandonTotal() == 0);
  GATE_CHECK(g_mx_entries != 0);
  GATE_CHECK(g_mx_mismatches == 0);
  GATE_CHECK(g_mx_suffixless == 0);

  uint64_t hash = logical_epoch_hash_v1(final);
  GATE_CHECK(hash == GOOF_LOGICAL_EPOCH_HASH_V1);
  printf("GOOF_LOGICAL_EPOCH_HASH_V1 PASS hash=%016llx nmi=%llu "
         "physical_periods=121 initial_q=1 final_q=1 pc=%02X:%04X s=%04X recomp=%d host=%u "
         "mismatch=%llu abandon=%llu unsupported=0 irq_attempts=0 "
         "aot=%llu interp=%llu continue=%llu tier=%ld host_max=%u\n",
         (unsigned long long)hash, (unsigned long long)final->nmis,
         final->cursor.cpu.k, final->cursor.cpu.pc, final->cursor.cpu.sp,
         g_recomp_stack_top, final->host_depth,
         (unsigned long long)g_mx_mismatches,
         (unsigned long long)CpuUnresolvedAbandonTotal(),
         (unsigned long long)final->aot_entries,
         (unsigned long long)final->cursor.instructions,
         (unsigned long long)final->aot_guest_continuations,
         interp_tier_hit_count(), final->max_host_depth);
  free(rom);
  return 0;
}
