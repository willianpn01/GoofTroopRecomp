/* GOOF_BOOT_E1000_GATE -- permanent regression gate for the proven
 * deterministic execution domain [E1, E1000].
 *
 * The domain was established observationally by
 * the project's long-boot observation study and re-proved
 * twice, in two fresh processes with independent configure and build, by
 * the gate-promotion study that preceded this file.  This gate
 * turns that domain into a compact contract.
 *
 * Contract philosophy.  1000 exact logical hashes plus 1000 exact
 * framebuffer hashes would be a brittle contract: any benign change to an
 * unrelated fade ramp would break hundreds of assertions at once and tell
 * the reader nothing.  Instead this gate pins
 *
 *   - twelve semantic checkpoints, each a distinct transition in the boot
 *     spine (reset/loader -> credits -> fade -> SPC upload -> post-APU ->
 *     CAPCOM -> title/menu -> scene transition -> GAMEPLAY_LIKE -> endpoint),
 *     carrying an exact logical hash and exact architectural PPU state;
 *   - six framebuffer hashes, each sampled in the middle of a long stable
 *     plateau so that the value represents a visual state rather than one
 *     frame of a ramp;
 *   - the APU IPL upload contract at E22 and the APU endpoint at E1000;
 *   - the physical-frame boundary contract: one logical epoch spans N >= 1
 *     NTSC frame periods and services one NMI at each enabled boundary, with
 *     the chain request -> entry -> guest $9C epilogue asserted per epoch;
 *   - global invariants that must hold at every one of the 1000 certified
 *     boundaries.
 *
 * Visual prose classifications ("menu", "gameplay-like") appear only in
 * diagnostics.  Every gate condition is an exact hash or an exact
 * architectural register/counter value.
 *
 * NO gameplay control is claimed.  Input is neutral throughout; the guest
 * walks its own attract sequence.  See the neutral-input invariant below.
 *
 * Headless by construction: no SDL, no OpenGL, no image file, no host
 * presentation, and no PPM/PNG output unless --dump-frame is passed
 * explicitly for evidence work.
 *
 * usage: goof_boot_e1000 ROM [--epochs N] [--dump-frame N]...
 *                        [--output-dir DIR] [--quiet]
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "apu.h"
#include "apu_frame_clock.h"
#include "audio_trace.h"
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
#include "snes.h"
#include "spc.h"

enum { kGateEpochs = 1000 };

/* ---------------------------------------------------------------------- */
/* Declarative checkpoint table                                            */
/* ---------------------------------------------------------------------- */

enum {
  CHK_FB      = 1u << 0,  /* framebuffer hash is normative              */
  CHK_PPU     = 1u << 1,  /* inidisp / tm / colors / non-black are      */
  CHK_SCHED   = 1u << 2,  /* scheduler slot states+selectors are        */
  CHK_APU_PRE = 1u << 3,  /* SPC still in IPL ROM (upload not done)     */
  CHK_APU_IPL = 1u << 4,  /* full IPL upload contract                   */
  CHK_APU_END = 1u << 5,  /* SPC pc / cycles / RAM digest are           */
};

typedef struct {
  unsigned epoch;
  const char *milestone;      /* diagnostics only, never a gate condition */
  uint64_t logical;           /* always normative                         */
  uint64_t framebuffer;       /* CHK_FB                                   */
  uint8_t inidisp, tm;        /* CHK_PPU: $2100, $212C                    */
  uint32_t colors, non_black; /* CHK_PPU                                  */
  /* CHK_SCHED: slot state byte and selector byte for slots 0..3, taken
   * from WRAM $50/$58/$60/$68 (+6 for the selector). */
  uint8_t slot_state[4], slot_sel[4];
  uint16_t spc_pc;            /* CHK_APU_END                              */
  uint64_t spc_cycles;        /* CHK_APU_END                              */
  uint64_t apu_ram;           /* CHK_APU_END                              */
  unsigned flags;
} Checkpoint;

/* Every value below was produced by the committed logical path and the
 * committed isolated renderer AFTER the coordinated CPU bus-speed + General
 * DMA + DRAM refresh master-clock milestone, and only after PBN1 (reference
 * VRAM), PBN3 (queue-drain causal chain), PBN10 (reference visual) and the
 * bsnes $0070 event oracle (80 events, 79 at 0 frames) passed in the same
 * build, and after human validation.  A hash here is a RECORD of a run that
 * already passed independent acceptance -- never itself the criterion for
 * accepting the fix.
 *
 * The epoch indices moved again, deliberately: the loader epoch E1 is now 121
 * physical periods (was 143, which the uniform 8-clock CPU over-counted), so
 * every later scene arrives 21 epochs later on the logical axis while landing
 * 1-6 frames EARLIER in physical time -- where bsnes has it.  The SEMANTIC
 * SPINE is unchanged, milestone for milestone:
 *
 *     reset/loader   E1    (121 physical periods, 121 NMIs; was 143)
 *     credits end    E27   (was E6)
 *     pre-APU fade   E42   (was E21)
 *     SPC IPL upload E43   (was E22)
 *     post-APU       E65   (was E44)
 *     display on     E90   (was E69)
 *     CAPCOM         E244  (was E223)
 *     title/menu     E522  (was E501)
 *     transition     E701  (was E680)
 *     GAMEPLAY_LIKE  E721  (was E700)
 *     gameplay       E903  (was E882)
 *     endpoint       E1000 (was E1000; the old E1000 frame now recurs at E1021)
 *
 * Provenance: goof_cpu_dma_refresh_master_clock_implementation.md.  The
 * previous table is in goof_physical_boundary_within_epoch_implementation.md
 * and the evidence directories. */
static const Checkpoint kCheckpoints[] = {
  { 1, "reset boundary / loader epoch: 121 physical frame periods, 121 NMIs",
    UINT64_C(0x2e4caf44f19d0392), UINT64_C(0x2fe663de2c1a1c25),
    0x0f, 0x04, 4, 2765, {1,0,0,0}, {0x00,0x00,0x08,0x06},
    0xffcf, 2067648, UINT64_C(0x2e44d3c25b026800),
    CHK_FB | CHK_PPU | CHK_SCHED | CHK_APU_PRE },
  { 27, "credits presentation ends; scheduler slot2 selector -> $0A",
    UINT64_C(0x17a020c916d79a7b), UINT64_C(0x2fe663de2c1a1c25),
    0x0f, 0x04, 4, 2765, {1,0,1,0}, {0x00,0x00,0x0a,0x06},
    0xffd2, 2511936, UINT64_C(0x2e44d3c25b026800),
    CHK_FB | CHK_PPU | CHK_SCHED | CHK_APU_PRE },
  { 42, "fade-out complete; last frame before the SPC IPL upload",
    UINT64_C(0x8966bba58cc2f800), UINT64_C(0x06644c0aa7470383),
    0x00, 0x04, 1, 0, {1,0,0,0}, {0x00,0x00,0x0a,0x06},
    0xffd2, 2768256, UINT64_C(0x2e44d3c25b026800),
    CHK_FB | CHK_PPU | CHK_SCHED | CHK_APU_PRE },
  { 43, "deterministic SPC IPL upload completes",
    UINT64_C(0x98454f0976ddab1a), 0,
    0x80, 0x04, 1, 0, {1,0,0,0}, {0x00,0x00,0x0a,0x06},
    0, 0, 0,
    CHK_PPU | CHK_APU_IPL },
  { 65, "established post-APU anchor; SPC driver running",
    UINT64_C(0xca59d7ce793383a4), 0,
    0x80, 0x01, 1, 0, {1,0,0,0}, {0x00,0x00,0x0a,0x06},
    0x0300, 3930240, UINT64_C(0x0172921e63b53fc3),
    CHK_PPU | CHK_APU_END },
  { 90, "forced blank released; first post-APU visual output",
    UINT64_C(0xfa6401deb979dc4e), 0,
    0x0f, 0x01, 3, 112, {1,0,1,0}, {0x00,0x00,0x0c,0x06},
    0, 0, 0,
    CHK_PPU | CHK_SCHED },
  { 244, "CAPCOM logo, stable plateau E185..E304",
    UINT64_C(0x39f6206ed133692f), UINT64_C(0xdbff93edb06b69bd),
    0x0f, 0x01, 15, 1615, {1,0,0,0}, {0x00,0x00,0x0c,0x06},
    0, 0, 0,
    CHK_FB | CHK_PPU | CHK_SCHED },
  { 522, "title screen with menu, stable plateau E428..E616",
    UINT64_C(0x7b931c0b307ef255), UINT64_C(0x6793cde5aa7b786e),
    0x0f, 0x07, 51, 57344, {1,0,0,0}, {0x02,0x00,0x08,0x06},
    0, 0, 0,
    CHK_FB | CHK_PPU | CHK_SCHED },
  { 701, "scene transition: slot1 first runnable, stage loading",
    UINT64_C(0x04f0685ab43c6e03), 0,
    0x80, 0x07, 1, 0, {1,1,0,0}, {0x02,0x12,0x0a,0x14},
    0, 0, 0,
    CHK_PPU | CHK_SCHED },
  { 721, "GAMEPLAY_LIKE boundary: OBJ on main screen, full brightness",
    UINT64_C(0x6faaf73ba74decf2), 0,
    0x0f, 0x17, 92, 57344, {1,1,0,0}, {0x02,0x12,0x08,0x14},
    0, 0, 0,
    CHK_PPU | CHK_SCHED },
  { 903, "GAMEPLAY_LIKE plateau E874..E933",
    UINT64_C(0xddf73e13801e8cae), UINT64_C(0x3d8b38d15850ca04),
    0x0f, 0x17, 92, 57344, {1,1,0,0}, {0x02,0x12,0x08,0x14},
    0, 0, 0,
    CHK_FB | CHK_PPU | CHK_SCHED },
  { 1000, "long-run endpoint, GAMEPLAY_LIKE",
    UINT64_C(0x039d1c22980acac9), UINT64_C(0x7ca1b37acb690109),
    0x0f, 0x17, 92, 57344, {1,1,0,0}, {0x02,0x12,0x08,0x14},
    0x038a, 20505600, UINT64_C(0x2c9a2d8c25bb8256),
    CHK_FB | CHK_PPU | CHK_SCHED | CHK_APU_END },
};
enum { kCheckpointCount = sizeof(kCheckpoints) / sizeof(kCheckpoints[0]) };

/* SPC IPL upload contract, now observed at E43 (was E22, and E154 before
 * that; the epoch index moved with each timing fix, the contract did not). */
#define GATE_IPL_IMAGE          UINT64_C(0x7bbbaa3712cba454)
#define GATE_IPL_HEADER_ECHOES  3u
#define GATE_IPL_FIRST_DATA     0x40u
#define GATE_IPL_FIRST_INDEX    0x00u
#define GATE_IPL_READY_WORD     0xbbaau
#define GATE_IPL_PORT2_READY    1u

/* APU command protocol across the campaign: the loaded driver's port-2 ack
 * counter advances once per new CPU command.  We assert the shape of the
 * progression, not the epoch of each event -- see the report, section on
 * post-upload APU command evidence. */
#define GATE_MIN_ACK_ADVANCES   5u
/* Re-measured after the cadence fix.  1000 logical epochs now span 1227 NTSC
 * frame periods instead of 329 (1200 since the coordinated CPU/DMA/refresh
 * master clock), so the guest reaches one command further into its own
 * sequence; the SHAPE of the progression is what is contracted. */
#define GATE_FINAL_ACK          0x07u

/* The physical frame clock.  The period is the NTSC line*dot product and is
 * asserted at every boundary, so a retuned clock cannot slip in unnoticed. */
#define GATE_FRAME_PERIOD_MASTER      (1364u * 262u)
/* Detection can lag a deadline by at most one AOT activation.  This bound and
 * the activation bound are what keep the "one check at the loop top" strategy
 * honest: if a future AOT recipe produced a long activation, NMI cadence
 * would silently degrade, and instead this fails. */
#define GATE_MAX_DETECTION_OVERSHOOT  4096u

/* Generous bounds, not exact counts: these exist to catch a regression that
 * introduces a polling wait, not to freeze incidental access counts.  The
 * joypad bound is now expressed PER NMI ENTRY rather than per epoch, because
 * the guest's auto-read happens in its NMI handler and an epoch now contains
 * one entry per physical frame period it spans. */
#define GATE_MAX_BEAM_READS_PER_ENTRY   4u
#define GATE_MAX_JOY_READS_PER_ENTRY    8u
#define GATE_APU_SPIN_THRESHOLD         (1u << 20)

/* ---- AUDIO6: guest-time APU/PCM rate fidelity --------------------------
 * The nominal relation, which is the spine of this gate and is the SAME
 * relation the physical boundary clock already enforces on the CPU side:
 *
 *     1 physical period = 357368 master clocks
 *                       =  17088 SPC cycles
 *                       =    534 native stereo PCM frames
 *
 * 17088 = 534 * 32 exactly (apu_cycle emits one dsp_cycle every 32 APU
 * cycles), and 357368 = 1364 * 262, so the three are one constant written
 * three ways.  AUDIO6 asserts SPC cycles and PCM frames per physical period
 * against it -- no audio device, no host queue, no consumer required.
 *
 * The expectation is expressed as an interval measured BETWEEN two armed
 * physical boundaries, not from reset: before the first boundary the SPC is
 * advanced by the boot/IPL handshake at its own handshake-paced rate, which
 * is a different (and correct) regime.  See the implementation report,
 * "Bootstrap semantics". */
#define GATE_APU_CYCLES_PER_PERIOD   17088u
#define GATE_PCM_FRAMES_PER_PERIOD     534u
#define GATE_MASTER_PER_PERIOD      357368u

/* ---------------------------------------------------------------------- */

static GoofRunFrameAdapter g_run_frame;
static GoofHeadlessRenderer g_renderer;
static uint64_t g_mx_entries, g_mx_mismatches, g_mx_suffixless;
static int g_in_render;
static uint64_t g_render_violations;
static unsigned g_upload_rom_disable_events;
static uint64_t g_ipl_image_hash, g_ipl_header_echoes;
static uint64_t g_driver_port2_ready_events;
static uint8_t g_ipl_first_data_byte, g_ipl_first_index_byte;
static int g_ipl_first_data_seen;
static uint16_t g_ipl_ready_word; static int g_ipl_ready_seen;
static int g_ipl_contract_verified;   /* discovery: verified at upload epoch */

/* MMIO observation: observe-then-delegate, never altering value or order. */
static uint64_t g_beam_reads;        /* $4212                              */
static uint64_t g_hvbjoy_digest = UINT64_C(0xcbf29ce484222325);

/* ---- GOOF_AUTOJOY_BUSY_TIMING -- read-only diagnostic ------------------
 * Nothing here is read by the runtime: every field is OBSERVED from state
 * the guest already produced, and the reference columns are computed, never
 * applied.  $4212 keeps returning exactly what the production accessor
 * returns, so HVBJOY_READ_DIGEST_V1 and every canonical checkpoint are
 * unaffected by construction.
 *
 * Reference timing (SNESdev): auto-read starts at H=32.5..95.5 of the first
 * VBlank line (+130..382 master clocks after the physical deadline) and
 * asserts HVBJOY.0 for 4224 master clocks, clearing by +4606 at the latest.
 *
 * Two reference columns exist.  The "naive" one applies a [0, 4224) window
 * to the RUNTIME clock; it is a counterfactual only, because the runtime
 * charges no master clocks for general DMA (1046/1179 reads would falsely
 * spin).  The "hw" bounds below add the uncharged DMA time and are the
 * verdict: no Goof read can see busy.  Do not implement busy from runtime
 * master_cycles before GOOF_GENERAL_DMA_MASTER_CLOCK_ACCOUNTING.
 * See goof_autojoy_busy_timing_investigation.md. */
#define AUTOJOY_REF_BUSY_MASTER 4224u
/* One spin iteration at $00:830B is LDA abs (4) + AND #imm (2) + BNE taken
 * (2+1) = 9 CPU cycles; the driver charges cyclesUsed * 8 master clocks. */
#define AUTOJOY_SPIN_MASTER_PER_ITER 72u

static uint64_t g_autojoy_digest = UINT64_C(0xcbf29ce484222325);
static uint64_t g_autojoy_events;          /* rows folded into the digest  */
static uint64_t g_autojoy_busy_reads;      /* $4212 reads, current runtime */
static uint64_t g_autojoy_busy_set_current;/* ... that returned bit0 = 1   */
static uint64_t g_autojoy_busy_set_ref;    /* ... busy under the reference */
static uint64_t g_autojoy_phase_min = UINT64_MAX, g_autojoy_phase_max;
static uint64_t g_autojoy_phase_sum;
static uint64_t g_autojoy_pred_iters;      /* extra taken iterations       */
static uint64_t g_autojoy_pred_master;     /* extra master clocks          */
static uint64_t g_autojoy_pred_iters_max;
static uint64_t g_autojoy_spin_observed;   /* >1 read at one NMI, current  */
static uint64_t g_autojoy_prev_read_master;
static uint64_t g_autojoy_enable_writes;   /* $4200 writes, bit0 = 1       */
static uint64_t g_autojoy_disable_writes;  /* $4200 writes, bit0 = 0       */
static uint64_t g_autojoy_disabled_at_read;/* $4212 read with $4200.0 = 0  */
static uint64_t g_autojoy_result_reads;    /* $4218-$421f reads            */
static uint64_t g_autojoy_result_before_ref_clear; /* ... inside busy win  */
static uint64_t g_autojoy_nmi_edges;
static uint64_t g_autojoy_nmi_edge_phase_nonzero;
static uint64_t g_autojoy_timer_nonzero;   /* autoJoyTimer ever started?   */
static uint64_t g_autojoy_edges_autojoy_on;/* NMI edges with $4200.0 = 1   */

/* Hardware-time reconstruction.  The runtime charges 8 master clocks per CPU
 * cycle and charges NOTHING for general DMA, so `master % frame` at the read
 * is not the phase real hardware would be at.  Bounds on the hardware phase
 * of the $4212 read, measured from the NMI edge (hardware NMI edge = VBlank
 * entry, phase ~0):
 *   lower = cpu*6/8 + 8*bytes + 8*channels + 12*starts
 *   upper = cpu     + 8*bytes + 8*channels + 24*starts
 * (fast 6-clock lower bound: Goof sets MEMSEL=1 at $00:8017 and runs the
 * NMI from bank $80; DMA: 8/byte, 8/channel, 12-24 per start). */
#define AUTOJOY_HW_START_MIN 130u   /* H=32.5 dots  x 4 */
#define AUTOJOY_HW_START_MAX 382u   /* H=95.5 dots  x 4 */
#define AUTOJOY_HW_CLEAR_MAX (AUTOJOY_HW_START_MAX + AUTOJOY_REF_BUSY_MASTER)
static uint64_t g_autojoy_edge_master;     /* runtime master at NMI edge   */
static uint64_t g_autojoy_dma_bytes, g_autojoy_dma_channels,
                g_autojoy_dma_starts;       /* since the last NMI edge      */
static uint64_t g_autojoy_hw_lb_min = UINT64_MAX, g_autojoy_hw_ub_max;
static uint64_t g_autojoy_hw_busy_possible;/* reads with lower < clear_max */
static uint64_t g_autojoy_dma_bytes_min = UINT64_MAX, g_autojoy_dma_bytes_max;
static uint64_t g_autojoy_cpu_min = UINT64_MAX;
static uint64_t g_autojoy_first_read_in_nmi, g_autojoy_prev_edge_seen;

static void autojoy_observe_dma_start(uint8_t mask) {
  if (!mask || !g_snes) return;
  g_autojoy_dma_starts++;
  for (unsigned c = 0; c < 8; c++)
    if (mask & (1u << c)) {
      uint16_t sz = g_snes->dma->channel[c].size;
      g_autojoy_dma_channels++;
      g_autojoy_dma_bytes += sz ? sz : 0x10000u;
    }
}

static void autojoy_fold(const uint8_t *b, unsigned n) {
  for (unsigned i = 0; i < n; i++) {
    g_autojoy_digest ^= b[i];
    g_autojoy_digest *= UINT64_C(0x100000001b3);
  }
  g_autojoy_events++;
}
static uint64_t g_beam_latch_reads;  /* $4211 / $2137 / $213C / $213D      */
static uint64_t g_joy_auto_reads;    /* $4218-$421F                        */
static uint64_t g_joy_manual_reads;  /* $4016 / $4017                      */
static uint64_t g_unsupported_mmio;
static uint64_t g_hdma_nonzero_writes;
static uint64_t g_apu_read_streak, g_apu_read_streak_max;
/* Per-epoch digest of the CPU's OBSERVATION of the APU: FNV-1a 64 over every
 * (register, value) pair the guest reads from $2140-$2143 within the epoch,
 * in order, plus the count.  This is the ONLY channel through which an APU
 * rate change can reach guest logic, so comparing this digest between two
 * builds localises CPU<->APU coupling to the exact epoch it first appears --
 * rather than leaving "the logical hash moved" as an unexplained fact. */
static uint64_t g_apu_read_digest = UINT64_C(0xcbf29ce484222325);
static uint64_t g_apu_reads_epoch, g_apu_reads_total;
static uint16_t g_apu_streak_reg; static uint8_t g_apu_streak_val;

/* Boundary-sampled APU ack progression (robust: derived from the certified
 * boundary state, not from individual port-write events, which can observe
 * transient intra-epoch values). */
static uint8_t g_ack_last; static uint64_t g_ack_advances;

static unsigned g_epoch;
static int g_quiet;
static int g_discover;
/* Phase 0 diagnostic mode: AUDIO6 is MEASURED and REPORTED but not asserted,
 * so the pre-fix defect can be recorded from a run that still completes and
 * still certifies every CPU/PPU contract.  Never used for acceptance. */
static int opt_audio6_advisory;
/* Audio-side pin mode.  The APU-side checkpoint fields (spc_pc, spc_cycles,
 * apu_ram) are the ones this milestone is EXPECTED to move; the CPU/PPU
 * fields are the ones it must not.  This flag reports the audio-side fields
 * instead of asserting them, so the CPU/PPU baseline can be certified in
 * full BEFORE any audio hash is promoted -- promotion order, not a way to
 * make a red gate green. */
static int opt_audio_pins_advisory;
/* AUDIO4: attach a deterministic PCM consumer.  Once per epoch it drains
 * everything the guest has produced through dsp_available / dsp_peek /
 * dsp_advance and digests it.  The consumer OBSERVES ONLY -- it never calls
 * apu_cycle and cannot create APU time.  Running the campaign with and
 * without it must leave every guest field bit-identical; that is what stops
 * the retired "callback cycles the SPC for its own shortfall" design from
 * coming back in through a test harness. */
static int opt_pcm_drain;
static uint64_t g_drain_hash = UINT64_C(0xcbf29ce484222325);
static uint64_t g_drain_frames, g_drain_calls, g_drain_highwater;

/* ---- AUDIO6 / AUDIO5 sampling ------------------------------------------
 * Two samples of the same three-tuple, taken at two epoch boundaries, give
 * the per-physical-period production rate of the interval between them
 * without any dependence on the boot regime that precedes the first one. */
typedef struct {
  int valid;
  unsigned epoch;
  uint64_t apu_cycles;      /* apu->cycles                                */
  uint64_t pcm_frames;      /* audio_trace produced (pre-overflow-check)  */
  uint64_t periods;         /* GuestExecution lifetime physical_periods   */
  uint64_t master;          /* g_cpu.master_cycles                        */
} AudioSample;
/* AUDIO6 measures the steady-state interval E65 (documented "post-APU
 * anchor; SPC driver running", 22 epochs after the IPL upload) -> E1000, so
 * the IPL upload handshake and the pre-driver regime are outside the
 * measured window by construction. */
#define GATE_AUDIO6_START_EPOCH 65u
/* Discovery re-derives the anchor semantically: the pinned E65 sits 22
 * epochs after the pinned IPL upload epoch (E43), so a chronology change that
 * moves the upload moves the anchor with it (see DISCOVER_IPL_UPLOAD_EPOCH). */
static unsigned g_audio6_start_epoch = GATE_AUDIO6_START_EPOCH;
static AudioSample g_audio_start, g_audio_end;

/* Boundary accounting carried across the campaign. */
static uint8_t g_prev_9c;
static uint64_t g_prev_nmis, g_prev_epilogues;
static uint64_t g_total_periods, g_total_requests, g_total_entries;
static uint64_t g_total_padding;

static void gate_fail_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void gate_fail_msg(const char *fmt, ...) {
  va_list ap;
  printf("GOOF_BOOT_E1000_GATE FAIL ");
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
  fflush(stdout);
  exit(1);
}

static void gate_fail(const char *condition, int line) {
  gate_fail_msg("epoch=%u line=%d condition=%s", g_epoch, line, condition);
}

#define GATE_CHECK(condition) \
  do { if (!(condition)) gate_fail(#condition, __LINE__); } while (0)

/* Fail-closed comparison that names epoch, checkpoint, expected, observed. */
#define GATE_EXPECT(chk, field, observed, expected)                         \
  do {                                                                      \
    unsigned long long o_ = (unsigned long long)(observed);                 \
    unsigned long long e_ = (unsigned long long)(expected);                 \
    if (o_ != e_)                                                           \
      gate_fail_msg("epoch=%u checkpoint=\"%s\" field=%s expected=%016llx " \
                    "observed=%016llx", (chk)->epoch, (chk)->milestone,     \
                    field, e_, o_);                                         \
  } while (0)

#define GATE_INVARIANT(cond, name, first_epoch, detail_fmt, ...)            \
  do {                                                                      \
    if (!(cond))                                                            \
      gate_fail_msg("invariant=%s first_violating_epoch=%u " detail_fmt,    \
                    name, (unsigned)(first_epoch), __VA_ARGS__);            \
  } while (0)

/* The three APU-side checkpoint fields, asserted or reported. */
static void audio_pins(const Checkpoint *chk, const Apu *apu) {
  uint64_t ram = goof_gate_fnv1a64(apu->ram, sizeof(apu->ram));
  if (opt_audio_pins_advisory) {
    printf("AUDIO_PINS epoch=%u spc_pc=%04X spc_cycles=%llu apu_ram=%016llx "
           "(pinned: spc_pc=%04X spc_cycles=%llu apu_ram=%016llx)\n",
           chk->epoch, apu->spc->pc, (unsigned long long)apu->cycles,
           (unsigned long long)ram, chk->spc_pc,
           (unsigned long long)chk->spc_cycles,
           (unsigned long long)chk->apu_ram);
    return;
  }
  GATE_EXPECT(chk, "spc_pc", apu->spc->pc, chk->spc_pc);
  GATE_EXPECT(chk, "spc_cycles", apu->cycles, chk->spc_cycles);
  GATE_EXPECT(chk, "apu_ram", ram, chk->apu_ram);
}

static void pcm_drain(void) {
  Dsp *dsp = g_snes->apu->dsp;
  uint32_t avail = dsp_available(dsp);
  g_drain_calls++;
  if (avail > g_drain_highwater) g_drain_highwater = avail;
  for (uint32_t i = 0; i < avail; i++) {
    int16_t l, r;
    dsp_peek(dsp, i, &l, &r);
    uint16_t ul = (uint16_t)l, ur = (uint16_t)r;
    uint8_t bytes[4] = { (uint8_t)(ul & 0xff), (uint8_t)(ul >> 8),
                         (uint8_t)(ur & 0xff), (uint8_t)(ur >> 8) };
    for (int b = 0; b < 4; b++) {
      g_drain_hash ^= bytes[b];
      g_drain_hash *= UINT64_C(0x100000001b3);
    }
  }
  dsp_advance(dsp, avail);
  g_drain_frames += avail;
}

static void audio_sample(AudioSample *out, unsigned epoch,
                         const GuestExecution *g) {
  AudioTraceStats st;
  audio_trace_get_stats(&st);
  out->valid = 1;
  out->epoch = epoch;
  out->apu_cycles = g_snes->apu->cycles;
  out->pcm_frames = st.produced;
  out->periods = g->physical_periods;
  out->master = g_cpu.master_cycles;
}

static int reg_supported(uint16_t r) {
  return (r >= 0x2100 && r <= 0x2183) || (r >= 0x4016 && r <= 0x4017) ||
         (r >= 0x4200 && r <= 0x421f) || (r >= 0x4300 && r <= 0x437f);
}

/* VBLANK1/VBLANK2: exercise the production $4212 accessor at fixed guest
 * clocks.  The retained HBlank compatibility oscillator is allowed to move,
 * so every assertion masks bit 7.  Restore the tiny synthetic state exactly:
 * this is a directed hardware-register test, not part of the campaign. */
static void vblank_boundary_test(void) {
  uint64_t saved_master = g_cpu.master_cycles;
  uint32_t saved_hpos = g_snes->hPos, saved_vpos = g_snes->vPos;
  bool saved_in_vblank = g_snes->inVblank;
  uint64_t boundary2 = 2 * RTL_MASTER_CYCLES_PER_FRAME;

  g_cpu.master_cycles = RTL_MASTER_CYCLES_PER_FRAME - 1;
  GATE_CHECK(!(snes_readReg(g_snes, 0x4212) & 0x80));
  g_cpu.master_cycles = RTL_MASTER_CYCLES_PER_FRAME;
  GATE_CHECK(snes_readReg(g_snes, 0x4212) & 0x80);
  g_cpu.master_cycles = RTL_MASTER_CYCLES_PER_FRAME +
                        RTL_VBLANK_MASTER_CYCLES - 1;
  GATE_CHECK(snes_readReg(g_snes, 0x4212) & 0x80);
  g_cpu.master_cycles = RTL_MASTER_CYCLES_PER_FRAME +
                        RTL_VBLANK_MASTER_CYCLES;
  GATE_CHECK(!(snes_readReg(g_snes, 0x4212) & 0x80));

  g_cpu.master_cycles = boundary2 + 4776;
  GATE_CHECK(snes_readReg(g_snes, 0x4212) & 0x80);

  /* Same guest instant, deliberately very different read counts. */
  g_cpu.master_cycles = boundary2 + 123;
  uint8_t few_reads = snes_readReg(g_snes, 0x4212) & 0x80;
  for (unsigned i = 0; i < 64; i++)
    GATE_CHECK((snes_readReg(g_snes, 0x4212) & 0x80) == few_reads);

  g_cpu.master_cycles = saved_master;
  g_snes->hPos = saved_hpos;
  g_snes->vPos = saved_vpos;
  g_snes->inVblank = saved_in_vblank;
  printf("VBLANK_BOUNDARY_TEST PASS boundary2=%llu read=719512 phase=4776 "
         "vblank=1 read_frequency_independent=YES\n",
         (unsigned long long)boundary2);
}

/* AUTOJOY_TRACE_DIGEST_V1 -- one row per guest $4212 read at $00:830B.
 * Semantic scope differs from HVBJOY_READ_DIGEST_V1 (which folds the raw
 * returned byte): this folds the busy-relevant guest-time columns and the
 * reference verdict, so the two digests are deliberately not interchangeable.
 *
 * Two reference columns are kept on purpose:
 *   naive  -- runtime `master % frame` < 4224 (what a timer started at the
 *             deadline would report on THIS runtime's clock);
 *   hw     -- the reconstructed hardware phase bounds above, which add the
 *             DMA time the runtime does not charge. */
static void autojoy_observe_busy_read(uint64_t ordinal, uint64_t master,
                                      uint8_t value) {
  uint64_t phase = master % RTL_MASTER_CYCLES_PER_FRAME;
  unsigned cur_busy = value & 1u;
  unsigned ref_busy = phase < AUTOJOY_REF_BUSY_MASTER;
  uint64_t iters = 0;
  if (ref_busy)
    iters = (AUTOJOY_REF_BUSY_MASTER - phase + AUTOJOY_SPIN_MASTER_PER_ITER - 1)
            / AUTOJOY_SPIN_MASTER_PER_ITER;
  uint64_t cpu = master - g_autojoy_edge_master;
  uint64_t dma = 8 * g_autojoy_dma_bytes + 8 * g_autojoy_dma_channels;
  uint64_t hw_lb = cpu * 6 / 8 + dma + 12 * g_autojoy_dma_starts;
  uint64_t hw_ub = cpu + dma + 24 * g_autojoy_dma_starts;
  unsigned hw_busy_possible = hw_lb < AUTOJOY_HW_CLEAR_MAX;
  unsigned first = g_autojoy_prev_edge_seen != g_autojoy_nmi_edges;
  g_autojoy_prev_edge_seen = g_autojoy_nmi_edges;
  if (first) {
    g_autojoy_first_read_in_nmi++;
    if (hw_lb < g_autojoy_hw_lb_min) g_autojoy_hw_lb_min = hw_lb;
    if (hw_ub > g_autojoy_hw_ub_max) g_autojoy_hw_ub_max = hw_ub;
    if (cpu < g_autojoy_cpu_min) g_autojoy_cpu_min = cpu;
    if (g_autojoy_dma_bytes < g_autojoy_dma_bytes_min)
      g_autojoy_dma_bytes_min = g_autojoy_dma_bytes;
    if (g_autojoy_dma_bytes > g_autojoy_dma_bytes_max)
      g_autojoy_dma_bytes_max = g_autojoy_dma_bytes;
    g_autojoy_hw_busy_possible += hw_busy_possible;
  }

  g_autojoy_busy_reads++;
  g_autojoy_busy_set_current += cur_busy;
  g_autojoy_busy_set_ref += ref_busy;
  if (phase < g_autojoy_phase_min) g_autojoy_phase_min = phase;
  if (phase > g_autojoy_phase_max) g_autojoy_phase_max = phase;
  g_autojoy_phase_sum += phase;
  g_autojoy_pred_iters += iters;
  g_autojoy_pred_master += iters * AUTOJOY_SPIN_MASTER_PER_ITER;
  if (iters > g_autojoy_pred_iters_max) g_autojoy_pred_iters_max = iters;
  if (g_snes) {
    if (!g_snes->autoJoyRead) g_autojoy_disabled_at_read++;
    if (g_snes->autoJoyTimer) g_autojoy_timer_nonzero++;
  }
  /* A second read inside the SAME physical period is a real spin iteration
   * of the current runtime; zero of them means the guest never waits. */
  if (g_autojoy_prev_read_master &&
      g_autojoy_prev_read_master / RTL_MASTER_CYCLES_PER_FRAME ==
      master / RTL_MASTER_CYCLES_PER_FRAME)
    g_autojoy_spin_observed++;
  g_autojoy_prev_read_master = master;

  uint16_t joy1 = g_snes ? g_snes->input1_currentState : 0xffff;
  uint16_t joy2 = g_snes ? g_snes->input2_currentState : 0xffff;
  uint8_t b[64]; unsigned n = 0;
  b[n++] = 0x01;                                   /* row type: busy read */
  for (unsigned i = 0; i < 8; i++) b[n++] = (uint8_t)(ordinal >> (8 * i));
  for (unsigned i = 0; i < 8; i++) b[n++] = (uint8_t)(master >> (8 * i));
  for (unsigned i = 0; i < 4; i++) b[n++] = (uint8_t)(phase >> (8 * i));
  b[n++] = value;
  b[n++] = (uint8_t)cur_busy;
  b[n++] = (uint8_t)ref_busy;
  b[n++] = (uint8_t)(iters > 255 ? 255 : iters);
  b[n++] = (uint8_t)(g_snes ? (g_snes->autoJoyRead ? 1 : 0) : 0xff);
  b[n++] = (uint8_t)(g_snes ? (g_snes->nmiEnabled ? 1 : 0) : 0xff);
  b[n++] = (uint8_t)(g_snes ? (g_snes->autoJoyTimer & 0xff) : 0xff);
  for (unsigned i = 0; i < 4; i++) b[n++] = (uint8_t)(cpu >> (8 * i));
  for (unsigned i = 0; i < 4; i++) b[n++] = (uint8_t)(g_autojoy_dma_bytes >> (8 * i));
  for (unsigned i = 0; i < 4; i++) b[n++] = (uint8_t)(hw_lb >> (8 * i));
  for (unsigned i = 0; i < 4; i++) b[n++] = (uint8_t)(hw_ub >> (8 * i));
  b[n++] = (uint8_t)hw_busy_possible;
  b[n++] = (uint8_t)joy1; b[n++] = (uint8_t)(joy1 >> 8);
  b[n++] = (uint8_t)joy2; b[n++] = (uint8_t)(joy2 >> 8);
  autojoy_fold(b, n);

  if (getenv("GOOF_AUTOJOY_TRACE"))
    fprintf(stderr, "AUTOJOY_BUSY_READ_V1 ordinal=%llu master=%llu phase=%llu "
            "value=%02X busy_current=%u busy_naive=%u naive_iters=%llu "
            "nmitimen_autojoy=%u nmitimen_nmi=%u timer=%u edge_master=%llu "
            "cpu_since_edge=%llu dma_bytes=%llu dma_channels=%llu "
            "dma_starts=%llu hw_phase_lb=%llu hw_phase_ub=%llu "
            "hw_busy_possible=%u joy1=%04X joy2=%04X\n",
            (unsigned long long)ordinal, (unsigned long long)master,
            (unsigned long long)phase, value, cur_busy, ref_busy,
            (unsigned long long)iters,
            g_snes ? (unsigned)g_snes->autoJoyRead : 2u,
            g_snes ? (unsigned)g_snes->nmiEnabled : 2u,
            g_snes ? (unsigned)g_snes->autoJoyTimer : 0u,
            (unsigned long long)g_autojoy_edge_master,
            (unsigned long long)cpu,
            (unsigned long long)g_autojoy_dma_bytes,
            (unsigned long long)g_autojoy_dma_channels,
            (unsigned long long)g_autojoy_dma_starts,
            (unsigned long long)hw_lb, (unsigned long long)hw_ub,
            hw_busy_possible, joy1, joy2);
}

static void autojoy_observe_result_read(uint16_t reg, uint8_t value) {
  uint64_t master = g_apu_clock_cpu ? g_apu_clock_cpu->master_cycles : 0;
  uint64_t phase = master % RTL_MASTER_CYCLES_PER_FRAME;
  g_autojoy_result_reads++;
  if (phase < AUTOJOY_REF_BUSY_MASTER) g_autojoy_result_before_ref_clear++;
  uint8_t b[16]; unsigned n = 0;
  b[n++] = 0x02;                                 /* row type: result read */
  b[n++] = (uint8_t)reg; b[n++] = (uint8_t)(reg >> 8);
  for (unsigned i = 0; i < 4; i++) b[n++] = (uint8_t)(phase >> (8 * i));
  b[n++] = value;
  autojoy_fold(b, n);
}

/* ---- GOOF_GENERAL_DMA_MASTER_CLOCK_ACCOUNTING -- read-only diagnostic ----
 * One row per channel executed by a guest $420B (MDMAEN) write, observed in
 * the WriteReg wrapper BEFORE the engine runs the transfer, so aAdr/size are
 * the configured values.  Nothing here writes master_cycles, the DMA
 * registers, the cursor, or any guest-visible state.
 *
 * Reference cost (bsnes sfc/cpu/timing.cpp dmaEdge + sfc/cpu/dma.cpp; the
 * anomie timing doc on the SFC dev wiki): the CPU is halted; align to a
 * multiple of 8 (2..8), 8 fixed, then per channel 8 + 8 per byte regardless
 * of memory region, then realign to the next CPU cycle length (2..6/8/12).
 * Fixed total 12..24.  DRAM refresh (40 per scanline) also stalls DMA; it is
 * uncharged everywhere in this runtime and is reported separately.
 *
 * "carry" is a COUNTERFACTUAL: the DMA time the runtime would have charged
 * so far, used to predict where later events would land.  It never feeds
 * back into execution. */
#define GDMA_FIXED_LB 12u
#define GDMA_FIXED_UB 24u
#define GDMA_F RTL_MASTER_CYCLES_PER_FRAME
enum { GDMA_DST_OAM, GDMA_DST_CGRAM, GDMA_DST_VRAM, GDMA_DST_WRAM,
       GDMA_DST_APU, GDMA_DST_OTHER, GDMA_DST_N };
static const char *const kGdmaDstName[GDMA_DST_N] =
  { "OAM", "CGRAM", "VRAM", "WRAM", "APU", "OTHER" };
static uint64_t g_gdma_digest = UINT64_C(0xcbf29ce484222325);
static uint64_t g_gdma_rows, g_gdma_starts, g_gdma_zero_mask_writes;
static uint64_t g_gdma_channels, g_gdma_bytes, g_gdma_multi_channel_starts;
static uint64_t g_gdma_das_zero, g_gdma_from_b, g_gdma_fixed_src, g_gdma_decrement;
static uint64_t g_gdma_bytes_dst[GDMA_DST_N], g_gdma_rows_dst[GDMA_DST_N];
static uint64_t g_gdma_bytes_mode[8], g_gdma_rows_mode[8];
static uint64_t g_gdma_starts_nmi, g_gdma_starts_main;
static uint64_t g_gdma_starts_interp, g_gdma_starts_aot;
static uint64_t g_gdma_cost_lb, g_gdma_cost_ub;
static uint64_t g_gdma_max_cost_ub, g_gdma_max_cost_ub_aot;
static uint64_t g_gdma_min_dist = UINT64_MAX;       /* start -> next deadline */
/* local crossing: runtime start + reference cost vs the runtime deadline */
static uint64_t g_gdma_local_none, g_gdma_local_touch, g_gdma_local_cross1,
                g_gdma_local_crossn, g_gdma_local_max_crossed;
static uint64_t g_gdma_local_cross_aot;
/* counterfactual timeline (carry since epoch start, ub cost) */
static uint64_t g_gdma_carry_lb, g_gdma_carry_ub;           /* epoch-local   */
static uint64_t g_gdma_nmi_carry_lb, g_gdma_nmi_carry_ub;   /* since edge    */
static uint64_t g_gdma_cf_none, g_gdma_cf_touch, g_gdma_cf_cross1,
                g_gdma_cf_crossn, g_gdma_cf_max_crossed;
static uint64_t g_gdma_cf_start_past_deadline, g_gdma_cf_min_margin = UINT64_MAX;
static uint64_t g_gdma_epoch_cost_lb, g_gdma_epoch_cost_ub, g_gdma_epoch_starts;
static uint64_t g_gdma_epoch_bytes, g_gdma_epoch_cf_cross;
static uint64_t g_gdma_epochs_with_dma, g_gdma_epochs_overflow_lb,
                g_gdma_epochs_overflow_ub, g_gdma_min_slack_ub = UINT64_MAX;
static uint64_t g_gdma_min_slack_epoch;
static uint64_t g_gdma_edges, g_gdma_edges_with_carry_lb;
static uint64_t g_gdma_epoch_edges_with_carry;
/* NMI $4212 first-read phase (current vs counterfactual) */
static uint64_t g_gdma_edge_seen_at_read;
static uint64_t g_gdma_read_n, g_gdma_read_phase_min = UINT64_MAX,
                g_gdma_read_phase_max, g_gdma_read_cf_lb_min = UINT64_MAX,
                g_gdma_read_cf_ub_max, g_gdma_read_nmi_bytes_min = UINT64_MAX;
static uint64_t g_gdma_nmi_bytes, g_gdma_nmi_starts, g_gdma_nmi_channels;
static uint64_t g_gdma_read_modal_phase, g_gdma_read_modal_count;
static uint64_t g_gdma_read_hist_phase[64], g_gdma_read_hist_count[64];
static unsigned g_gdma_read_hist_n;
static uint32_t g_gdma_last_func_pc24;               /* last AOT function entry */
/* distinct $420B sites */
typedef struct {
  uint32_t pc24; char tier; uint64_t starts, nmi, main, channels, bytes;
  uint64_t bytes_min, bytes_max; unsigned dst_mask, mode_mask, chan_mask;
  uint8_t bbad_first; uint32_t src_first; uint64_t cost_ub_max;
} GdmaSite;
static GdmaSite g_gdma_sites[64];
static unsigned g_gdma_site_n;

static unsigned gdma_dst_class(uint8_t bbad) {
  if (bbad == 0x04) return GDMA_DST_OAM;
  if (bbad == 0x22) return GDMA_DST_CGRAM;
  if (bbad == 0x18 || bbad == 0x19) return GDMA_DST_VRAM;
  if (bbad == 0x80) return GDMA_DST_WRAM;
  if (bbad >= 0x40 && bbad < 0x80) return GDMA_DST_APU;
  return GDMA_DST_OTHER;
}
static void gdma_fold(const uint8_t *b, unsigned n) {
  for (unsigned i = 0; i < n; i++) {
    g_gdma_digest ^= b[i];
    g_gdma_digest *= UINT64_C(0x100000001b3);
  }
}
static unsigned gdma_put(uint8_t *b, unsigned n, uint64_t v, unsigned bytes) {
  for (unsigned i = 0; i < bytes; i++) b[n++] = (uint8_t)(v >> (8 * i));
  return n;
}
/* Deadlines D = k*F strictly inside (start, end) are crossed; D == end is a
 * touch (the transfer ends exactly on the deadline). */
static uint64_t gdma_crossed(uint64_t start, uint64_t end, int *touch) {
  uint64_t first = (start / GDMA_F + 1) * GDMA_F;
  *touch = 0;
  if (first > end) return 0;
  uint64_t n = (end - first) / GDMA_F + 1;
  if ((end % GDMA_F) == 0) { n--; *touch = 1; }
  return n;
}
static void gdma_classify(uint64_t n, int touch, uint64_t *none, uint64_t *t,
                          uint64_t *c1, uint64_t *cn, uint64_t *mx) {
  if (n == 0 && !touch) (*none)++;
  else if (n == 0) (*t)++;
  else if (n == 1) (*c1)++;
  else (*cn)++;
  if (n > *mx) *mx = n;
}
static GdmaSite *gdma_site(uint32_t pc24, char tier) {
  for (unsigned i = 0; i < g_gdma_site_n; i++)
    if (g_gdma_sites[i].pc24 == pc24 && g_gdma_sites[i].tier == tier)
      return &g_gdma_sites[i];
  if (g_gdma_site_n == 64) return NULL;
  GdmaSite *s = &g_gdma_sites[g_gdma_site_n++];
  memset(s, 0, sizeof(*s));
  s->pc24 = pc24; s->tier = tier; s->bytes_min = UINT64_MAX;
  return s;
}

static void general_dma_observe_start(uint8_t mask) {
  if (!mask) { g_gdma_zero_mask_writes++; return; }
  const GuestExecution *g = &g_run_frame.execution;
  CpuState *clock = g_apu_clock_cpu ? g_apu_clock_cpu : &g_cpu;
  uint64_t master = clock->master_cycles;
  char tier = clock == &g_cpu ? 'I' : 'A';
  uint32_t pc24 = tier == 'I' ? g->cursor.last_pc : g_gdma_last_func_pc24;
  uint32_t next_pc = 0;
  if (tier == 'I' && (pc24 & 0x8000)) {
    uint8_t op = cpu_read8(&g_cpu, (uint8_t)(pc24 >> 16), (uint16_t)pc24);
    unsigned len = op == 0x8f ? 4 : 3;
    next_pc = (pc24 & 0xff0000) | (uint16_t)(pc24 + len);
  }
  uint64_t deadline = g->next_physical_boundary;
  uint64_t dist = deadline > master ? deadline - master : 0;
  if (dist < g_gdma_min_dist) g_gdma_min_dist = dist;
  int in_nmi = g->in_nmi ? 1 : 0;

  uint64_t bytes = 0, channels = 0;
  for (unsigned c = 0; c < 8; c++) {
    if (!(mask & (1u << c))) continue;
    uint16_t sz = g_snes->dma->channel[c].size;
    bytes += sz ? sz : 0x10000u;
    channels++;
  }
  uint64_t cost_lb = GDMA_FIXED_LB + 8 * channels + 8 * bytes;
  uint64_t cost_ub = GDMA_FIXED_UB + 8 * channels + 8 * bytes;

  g_gdma_starts++;
  g_gdma_channels += channels;
  g_gdma_bytes += bytes;
  g_gdma_cost_lb += cost_lb; g_gdma_cost_ub += cost_ub;
  if (channels > 1) g_gdma_multi_channel_starts++;
  if (in_nmi) g_gdma_starts_nmi++; else g_gdma_starts_main++;
  if (tier == 'I') g_gdma_starts_interp++; else g_gdma_starts_aot++;
  if (cost_ub > g_gdma_max_cost_ub) g_gdma_max_cost_ub = cost_ub;
  if (tier == 'A' && cost_ub > g_gdma_max_cost_ub_aot)
    g_gdma_max_cost_ub_aot = cost_ub;

  /* Local: the runtime start as-is plus this transfer's reference cost. */
  int touch;
  uint64_t n = gdma_crossed(master, master + cost_ub, &touch);
  gdma_classify(n, touch, &g_gdma_local_none, &g_gdma_local_touch,
                &g_gdma_local_cross1, &g_gdma_local_crossn,
                &g_gdma_local_max_crossed);
  if (tier == 'A' && (n || touch)) g_gdma_local_cross_aot++;
  /* Counterfactual, per physical period: shifted by every earlier uncharged
   * transfer since the last NMI edge.  (An epoch-wide carry would ignore the
   * guest's own wait loops between NMIs, which absorb the shift; it is kept
   * only as the EPOCHS_V1 upper bound.) */
  uint64_t cf_start = master + g_gdma_nmi_carry_ub;
  uint64_t cf_end = cf_start + cost_ub;
  uint64_t cf_margin = deadline > cf_end ? deadline - cf_end : 0;
  if (cf_margin < g_gdma_cf_min_margin) g_gdma_cf_min_margin = cf_margin;
  if (cf_start >= deadline) g_gdma_cf_start_past_deadline++;
  int cf_touch;
  uint64_t cf_n = gdma_crossed(cf_start, cf_start + cost_ub, &cf_touch);
  gdma_classify(cf_n, cf_touch, &g_gdma_cf_none, &g_gdma_cf_touch,
                &g_gdma_cf_cross1, &g_gdma_cf_crossn, &g_gdma_cf_max_crossed);
  if (cf_n || cf_touch) g_gdma_epoch_cf_cross++;
  g_gdma_carry_lb += cost_lb; g_gdma_carry_ub += cost_ub;
  g_gdma_nmi_carry_lb += cost_lb; g_gdma_nmi_carry_ub += cost_ub;
  g_gdma_epoch_cost_lb += cost_lb; g_gdma_epoch_cost_ub += cost_ub;
  g_gdma_epoch_starts++; g_gdma_epoch_bytes += bytes;
  g_gdma_nmi_bytes += bytes; g_gdma_nmi_starts++; g_gdma_nmi_channels += channels;

  GdmaSite *site = gdma_site(pc24, tier);
  if (site) {
    site->starts++; site->channels += channels; site->bytes += bytes;
    if (in_nmi) site->nmi++; else site->main++;
    if (bytes < site->bytes_min) site->bytes_min = bytes;
    if (bytes > site->bytes_max) site->bytes_max = bytes;
    site->chan_mask |= mask;
    if (cost_ub > site->cost_ub_max) site->cost_ub_max = cost_ub;
  }

  for (unsigned c = 0; c < 8; c++) {
    if (!(mask & (1u << c))) continue;
    const DmaChannel *ch = &g_snes->dma->channel[c];
    uint8_t dmap = (uint8_t)(ch->mode | ch->fixed << 3 | ch->decrement << 4 |
                             ch->unusedBit << 5 | ch->indirect << 6 |
                             ch->fromB << 7);
    uint32_t src24 = (uint32_t)ch->aBank << 16 | ch->aAdr;
    uint64_t sz = ch->size ? ch->size : 0x10000u;
    unsigned dst = gdma_dst_class(ch->bAdr);
    g_gdma_rows++;
    if (!ch->size) g_gdma_das_zero++;
    if (ch->fromB) g_gdma_from_b++;
    if (ch->fixed) g_gdma_fixed_src++;
    if (ch->decrement) g_gdma_decrement++;
    g_gdma_bytes_dst[dst] += sz; g_gdma_rows_dst[dst]++;
    g_gdma_bytes_mode[ch->mode] += sz; g_gdma_rows_mode[ch->mode]++;
    if (site) {
      site->dst_mask |= 1u << dst; site->mode_mask |= 1u << ch->mode;
      if (site->starts == 1) { site->bbad_first = ch->bAdr; site->src_first = src24; }
    }
    uint8_t b[48]; unsigned k = 0;
    k = gdma_put(b, k, g_gdma_starts, 8);
    k = gdma_put(b, k, master, 8);
    k = gdma_put(b, k, pc24, 4);
    b[k++] = (uint8_t)tier; b[k++] = mask; b[k++] = (uint8_t)c;
    b[k++] = dmap; b[k++] = ch->bAdr;
    k = gdma_put(b, k, src24, 4);
    k = gdma_put(b, k, sz, 4);
    k = gdma_put(b, k, 8 + 8 * sz, 4);       /* per-channel reference cost */
    b[k++] = (uint8_t)in_nmi;
    gdma_fold(b, k);
    if (getenv("GOOF_GENERAL_DMA_TRACE"))
      fprintf(stderr, "GENERAL_DMA_V1 ord=%llu epoch=%u master=%llu "
              "boundary=%llu phase=%llu pc=%06X tier=%c next_pc=%06X "
              "nmi=%d mask=%02X ch=%u dmap=%02X mode=%u fixed=%u dec=%u "
              "fromB=%u bbad=21%02X dst=%s src=%06X bytes=%llu "
              "start_bytes=%llu start_channels=%llu cost_lb=%llu "
              "cost_ub=%llu deadline=%llu dist=%llu local_crossed=%llu "
              "local_touch=%d cf_start=%llu cf_crossed=%llu cf_touch=%d "
              "period_carry_ub_before=%llu epoch_carry_ub_before=%llu inidisp=%02X\n",
              (unsigned long long)g_gdma_starts, g_epoch,
              (unsigned long long)master,
              (unsigned long long)(master / GDMA_F),
              (unsigned long long)(master % GDMA_F), pc24, tier, next_pc,
              in_nmi, mask, c, dmap, ch->mode, (unsigned)ch->fixed,
              (unsigned)ch->decrement, (unsigned)ch->fromB, ch->bAdr,
              kGdmaDstName[dst], src24, (unsigned long long)sz,
              (unsigned long long)bytes, (unsigned long long)channels,
              (unsigned long long)cost_lb, (unsigned long long)cost_ub,
              (unsigned long long)deadline, (unsigned long long)dist,
              (unsigned long long)n, touch,
              (unsigned long long)cf_start, (unsigned long long)cf_n, cf_touch,
              (unsigned long long)(g_gdma_nmi_carry_ub - cost_ub),
              (unsigned long long)(g_gdma_carry_ub - cost_ub),
              g_ppu ? g_ppu->inidisp : 0xffu);
  }
}

static void general_dma_on_nmi_edge(void) {
  g_gdma_edges++;
  if (getenv("GOOF_GENERAL_DMA_TRACE")) {
    const GuestExecution *g = &g_run_frame.execution;
    fprintf(stderr, "GENERAL_DMA_EDGE_V1 edge=%llu epoch=%u master=%llu "
            "boundary=%llu interrupted_pc=%02X%04X prev_period_starts=%llu "
            "prev_period_bytes=%llu prev_period_cost_ub=%llu\n",
            (unsigned long long)g_gdma_edges, g_epoch,
            (unsigned long long)g_cpu.master_cycles,
            (unsigned long long)(g_cpu.master_cycles / GDMA_F),
            g->cursor.cpu.k, g->cursor.cpu.pc,
            (unsigned long long)g_gdma_nmi_starts,
            (unsigned long long)g_gdma_nmi_bytes,
            (unsigned long long)g_gdma_nmi_carry_ub);
  }
  if (g_gdma_carry_lb) { g_gdma_edges_with_carry_lb++; g_gdma_epoch_edges_with_carry++; }
  g_gdma_nmi_carry_lb = g_gdma_nmi_carry_ub = 0;
  g_gdma_nmi_bytes = g_gdma_nmi_starts = g_gdma_nmi_channels = 0;
}

/* First $4212 read after each NMI edge: runtime phase vs the phase once the
 * uncharged DMA time since that edge is added (CPU time left at 8/cycle). */
static void general_dma_on_hvbjoy_read(uint64_t master) {
  if (g_gdma_edge_seen_at_read == g_gdma_edges) return;
  g_gdma_edge_seen_at_read = g_gdma_edges;
  uint64_t phase = master % GDMA_F;
  uint64_t lb = phase + g_gdma_nmi_carry_lb, ub = phase + g_gdma_nmi_carry_ub;
  g_gdma_read_n++;
  if (phase < g_gdma_read_phase_min) g_gdma_read_phase_min = phase;
  if (phase > g_gdma_read_phase_max) g_gdma_read_phase_max = phase;
  if (lb < g_gdma_read_cf_lb_min) g_gdma_read_cf_lb_min = lb;
  if (ub > g_gdma_read_cf_ub_max) g_gdma_read_cf_ub_max = ub;
  if (g_gdma_nmi_bytes < g_gdma_read_nmi_bytes_min)
    g_gdma_read_nmi_bytes_min = g_gdma_nmi_bytes;
  unsigned i;
  for (i = 0; i < g_gdma_read_hist_n; i++)
    if (g_gdma_read_hist_phase[i] == phase) break;
  if (i == g_gdma_read_hist_n && g_gdma_read_hist_n < 64)
    g_gdma_read_hist_phase[g_gdma_read_hist_n++] = phase;
  if (i < 64) {
    g_gdma_read_hist_count[i]++;
    if (g_gdma_read_hist_count[i] > g_gdma_read_modal_count) {
      g_gdma_read_modal_count = g_gdma_read_hist_count[i];
      g_gdma_read_modal_phase = phase;
    }
  }
  if (getenv("GOOF_GENERAL_DMA_TRACE"))
    fprintf(stderr, "GENERAL_DMA_HVBJOY_V1 edge=%llu epoch=%u master=%llu "
            "phase=%llu nmi_starts=%llu nmi_channels=%llu nmi_bytes=%llu "
            "cf_phase_lb=%llu cf_phase_ub=%llu\n",
            (unsigned long long)g_gdma_edges, g_epoch,
            (unsigned long long)master, (unsigned long long)phase,
            (unsigned long long)g_gdma_nmi_starts,
            (unsigned long long)g_gdma_nmi_channels,
            (unsigned long long)g_gdma_nmi_bytes,
            (unsigned long long)lb, (unsigned long long)ub);
}

/* Epoch end: the epoch padded `padding` master clocks to its closing
 * deadline.  If the counterfactual carry fits in that padding, the extra DMA
 * time is absorbed and the epoch still closes on the same deadline. */
static void general_dma_on_epoch_end(unsigned epoch, uint64_t periods,
                                     uint64_t padding) {
  uint64_t slack = padding >= g_gdma_carry_ub ? padding - g_gdma_carry_ub : 0;
  if (g_gdma_epoch_starts) {
    g_gdma_epochs_with_dma++;
    if (g_gdma_carry_lb > padding) g_gdma_epochs_overflow_lb++;
    if (g_gdma_carry_ub > padding) g_gdma_epochs_overflow_ub++;
    if (slack < g_gdma_min_slack_ub) {
      g_gdma_min_slack_ub = slack; g_gdma_min_slack_epoch = epoch;
    }
  }
  if (getenv("GOOF_GENERAL_DMA_TRACE") || periods > 1)
    fprintf(stderr, "GENERAL_DMA_EPOCH_V1 epoch=%u periods=%llu starts=%llu "
            "bytes=%llu cost_lb=%llu cost_ub=%llu padding=%llu "
            "carry_ub=%llu absorbed=%d edges_with_carry=%llu cf_cross=%llu\n",
            epoch, (unsigned long long)periods,
            (unsigned long long)g_gdma_epoch_starts,
            (unsigned long long)g_gdma_epoch_bytes,
            (unsigned long long)g_gdma_epoch_cost_lb,
            (unsigned long long)g_gdma_epoch_cost_ub,
            (unsigned long long)padding, (unsigned long long)g_gdma_carry_ub,
            g_gdma_carry_ub <= padding,
            (unsigned long long)g_gdma_epoch_edges_with_carry,
            (unsigned long long)g_gdma_epoch_cf_cross);
  g_gdma_carry_lb = g_gdma_carry_lb > padding ? g_gdma_carry_lb - padding : 0;
  g_gdma_carry_ub = g_gdma_carry_ub > padding ? g_gdma_carry_ub - padding : 0;
  g_gdma_epoch_cost_lb = g_gdma_epoch_cost_ub = g_gdma_epoch_starts = 0;
  g_gdma_epoch_bytes = g_gdma_epoch_edges_with_carry = g_gdma_epoch_cf_cross = 0;
}

/* GOOF_CPU_DMA_REFRESH accounting, observe-only (the driver's lifetime
 * counters).  The interpreted-tier inventory covers interpreted steps only;
 * AOT activations price themselves and are reported as one total.  The
 * identity checked here is that everything charged is accounted for:
 *     final master = interp + aot + refresh + interpreted-tier stall
 *                    + idle padding
 * where interpreted-tier stall is the guest stall charged outside AOT. */
static void cpu_bus_timing_report(const GuestExecution *g) {
  uint64_t interp_stall = g_guest_stall_master_total - g->aot_stall_master;
  uint64_t sum = g->interp_master + g->aot_master + g->dram_refresh_master +
                 interp_stall + g->idle_padding_master;
  printf("CPU_BUS_SPEED_V1 interp_fast6=%llu interp_slow8=%llu "
         "interp_xslow12=%llu interp_internal6=%llu peeks_excluded=%llu "
         "anomalies=%llu\n",
         (unsigned long long)g->bus_fast, (unsigned long long)g->bus_slow,
         (unsigned long long)g->bus_xslow,
         (unsigned long long)g->bus_internal_cycles,
         (unsigned long long)g->bus_peeks_excluded,
         (unsigned long long)g->bus_timing_anomalies);
  printf("CPU_BUS_MASTER_V1 interp_fast=%llu interp_slow=%llu "
         "interp_xslow=%llu interp_internal=%llu interp_total=%llu "
         "aot_total=%llu\n",
         (unsigned long long)g->bus_master_fast,
         (unsigned long long)g->bus_master_slow,
         (unsigned long long)g->bus_master_xslow,
         (unsigned long long)g->bus_master_internal,
         (unsigned long long)g->interp_master,
         (unsigned long long)g->aot_master);
  printf("GUEST_STALL_V1 dma_stall_master=%llu stall_events=%llu "
         "aot_stall_master=%llu aot_stall_activations=%llu\n",
         (unsigned long long)g_guest_stall_master_total,
         (unsigned long long)g_guest_stall_events,
         (unsigned long long)g->aot_stall_master,
         (unsigned long long)g->aot_stall_activations);
  printf("DRAM_REFRESH_V1 master=%llu events=%llu per_event=%u\n",
         (unsigned long long)g->dram_refresh_master,
         (unsigned long long)g->dram_refresh_events,
         (unsigned)kGoofDramRefreshMaster);
  printf("MASTER_CLOCK_IDENTITY_V1 interp=%llu aot=%llu refresh=%llu "
         "interp_stall=%llu padding=%llu sum=%llu final=%llu %s\n",
         (unsigned long long)g->interp_master, (unsigned long long)g->aot_master,
         (unsigned long long)g->dram_refresh_master,
         (unsigned long long)interp_stall,
         (unsigned long long)g->idle_padding_master, (unsigned long long)sum,
         (unsigned long long)g_cpu.master_cycles,
         sum == g_cpu.master_cycles ? "EXACT" : "MISMATCH");
  GATE_INVARIANT(g->bus_timing_anomalies == 0, "cpu_bus_timing_anomaly",
                 kGateEpochs, "anomalies=%llu",
                 (unsigned long long)g->bus_timing_anomalies);
  GATE_INVARIANT(g->aot_stall_activations == 0, "no_guest_stall_in_aot",
                 kGateEpochs, "activations=%llu",
                 (unsigned long long)g->aot_stall_activations);
  GATE_INVARIANT(sum == g_cpu.master_cycles, "master_clock_identity",
                 kGateEpochs, "sum=%llu final=%llu", (unsigned long long)sum,
                 (unsigned long long)g_cpu.master_cycles);
}

static void general_dma_report(void) {
  printf("GENERAL_DMA_INVENTORY_V1 starts=%llu zero_mask_writes=%llu "
         "channel_executions=%llu bytes=%llu multi_channel_starts=%llu "
         "starts_in_nmi=%llu starts_outside_nmi=%llu starts_interp=%llu "
         "starts_aot=%llu das_zero=%llu from_b=%llu fixed_src=%llu "
         "decrement=%llu\n",
         (unsigned long long)g_gdma_starts,
         (unsigned long long)g_gdma_zero_mask_writes,
         (unsigned long long)g_gdma_rows, (unsigned long long)g_gdma_bytes,
         (unsigned long long)g_gdma_multi_channel_starts,
         (unsigned long long)g_gdma_starts_nmi,
         (unsigned long long)g_gdma_starts_main,
         (unsigned long long)g_gdma_starts_interp,
         (unsigned long long)g_gdma_starts_aot,
         (unsigned long long)g_gdma_das_zero,
         (unsigned long long)g_gdma_from_b,
         (unsigned long long)g_gdma_fixed_src,
         (unsigned long long)g_gdma_decrement);
  printf("GENERAL_DMA_DEST_V1");
  for (unsigned d = 0; d < GDMA_DST_N; d++)
    printf(" %s=%llu/%llu", kGdmaDstName[d],
           (unsigned long long)g_gdma_rows_dst[d],
           (unsigned long long)g_gdma_bytes_dst[d]);
  printf("\nGENERAL_DMA_MODE_V1");
  for (unsigned m = 0; m < 8; m++)
    if (g_gdma_rows_mode[m])
      printf(" mode%u=%llu/%llu", m, (unsigned long long)g_gdma_rows_mode[m],
             (unsigned long long)g_gdma_bytes_mode[m]);
  printf("\n");
  for (unsigned i = 0; i < g_gdma_site_n; i++) {
    const GdmaSite *s = &g_gdma_sites[i];
    printf("GENERAL_DMA_SITE_V1 pc=%06X tier=%c starts=%llu nmi=%llu "
           "main=%llu channels=%llu bytes=%llu bytes_min=%llu bytes_max=%llu "
           "chan_mask=%02X mode_mask=%02X dst_mask=%02X first_bbad=21%02X "
           "first_src=%06X cost_ub_max=%llu\n", s->pc24, s->tier,
           (unsigned long long)s->starts, (unsigned long long)s->nmi,
           (unsigned long long)s->main, (unsigned long long)s->channels,
           (unsigned long long)s->bytes, (unsigned long long)s->bytes_min,
           (unsigned long long)s->bytes_max, s->chan_mask, s->mode_mask,
           s->dst_mask, s->bbad_first, s->src_first,
           (unsigned long long)s->cost_ub_max);
  }
  /* charged = guest stall time actually charged by the runtime (DMA module
   * timer: 16 fixed + 8/channel + 8/byte), inside the reference 12..24 fixed
   * bounds [expected_lb, expected_ub].  The cf_* columns below and the
   * hw_phase/cf_phase columns of the AUTOJOY/HVBJOY lines were written when
   * the runtime charged 0: they add the DMA time AGAIN on top of a clock that
   * now contains it, so since GOOF_CPU_DMA_REFRESH they are upper bounds,
   * not predictions. */
  printf("GENERAL_DMA_COST_V1 expected_lb=%llu expected_ub=%llu "
         "charged=%llu max_start_cost_ub=%llu max_start_cost_ub_aot=%llu "
         "min_start_to_deadline=%llu\n",
         (unsigned long long)g_gdma_cost_lb, (unsigned long long)g_gdma_cost_ub,
         (unsigned long long)g_guest_stall_master_total,
         (unsigned long long)g_gdma_max_cost_ub,
         (unsigned long long)g_gdma_max_cost_ub_aot,
         (unsigned long long)g_gdma_min_dist);
  printf("GENERAL_DMA_CROSSING_V1 local_none=%llu local_touch=%llu "
         "local_cross1=%llu local_crossn=%llu local_max_crossed=%llu "
         "local_cross_aot=%llu cf_none=%llu cf_touch=%llu cf_cross1=%llu "
         "cf_crossn=%llu cf_max_crossed=%llu cf_start_past_deadline=%llu "
         "cf_min_margin=%llu\n",
         (unsigned long long)g_gdma_local_none,
         (unsigned long long)g_gdma_local_touch,
         (unsigned long long)g_gdma_local_cross1,
         (unsigned long long)g_gdma_local_crossn,
         (unsigned long long)g_gdma_local_max_crossed,
         (unsigned long long)g_gdma_local_cross_aot,
         (unsigned long long)g_gdma_cf_none, (unsigned long long)g_gdma_cf_touch,
         (unsigned long long)g_gdma_cf_cross1,
         (unsigned long long)g_gdma_cf_crossn,
         (unsigned long long)g_gdma_cf_max_crossed,
         (unsigned long long)g_gdma_cf_start_past_deadline,
         (unsigned long long)g_gdma_cf_min_margin);
  printf("GENERAL_DMA_EPOCHS_V1 epochs_with_dma=%llu overflow_lb=%llu "
         "overflow_ub=%llu min_slack_ub=%llu min_slack_epoch=%llu "
         "nmi_edges=%llu edges_with_carry=%llu\n",
         (unsigned long long)g_gdma_epochs_with_dma,
         (unsigned long long)g_gdma_epochs_overflow_lb,
         (unsigned long long)g_gdma_epochs_overflow_ub,
         (unsigned long long)(g_gdma_epochs_with_dma ? g_gdma_min_slack_ub : 0),
         (unsigned long long)g_gdma_min_slack_epoch,
         (unsigned long long)g_gdma_edges,
         (unsigned long long)g_gdma_edges_with_carry_lb);
  printf("GENERAL_DMA_HVBJOY_PHASE_V1 first_reads=%llu runtime_phase_min=%llu "
         "runtime_phase_modal=%llu modal_count=%llu runtime_phase_max=%llu "
         "nmi_dma_bytes_min=%llu cf_phase_lb_min=%llu cf_phase_ub_max=%llu\n",
         (unsigned long long)g_gdma_read_n,
         (unsigned long long)g_gdma_read_phase_min,
         (unsigned long long)g_gdma_read_modal_phase,
         (unsigned long long)g_gdma_read_modal_count,
         (unsigned long long)g_gdma_read_phase_max,
         (unsigned long long)g_gdma_read_nmi_bytes_min,
         (unsigned long long)g_gdma_read_cf_lb_min,
         (unsigned long long)g_gdma_read_cf_ub_max);
  /* Context only (IRQ is out of scope): $4200 = $B1 also enables H/V IRQ;
   * the runtime has no H/V IRQ source, and the gate asserts none is wanted. */
  printf("GENERAL_DMA_IRQ_CONTEXT_V1 nmitimen_hirq=%u nmitimen_virq=%u "
         "htime=%u vtime=%u irq_wanted=%u\n",
         g_snes ? (unsigned)g_snes->hIrqEnabled : 0,
         g_snes ? (unsigned)g_snes->vIrqEnabled : 0,
         g_snes ? (unsigned)g_snes->hTimer : 0,
         g_snes ? (unsigned)g_snes->vTimer : 0,
         (unsigned)g_run_frame.execution.cursor.cpu.irqWanted);
  printf("GENERAL_DMA_TRACE_DIGEST_V1 rows=%llu digest=%016llx\n",
         (unsigned long long)g_gdma_rows, (unsigned long long)g_gdma_digest);
}

static void observe_read(uint16_t reg, uint8_t value) {
  if (g_in_render) { g_render_violations++; return; }
  if (!reg_supported(reg)) { g_unsupported_mmio++; return; }
  if (reg == 0x4212) {
    uint64_t ordinal = ++g_beam_reads;
    uint64_t master = g_apu_clock_cpu ? g_apu_clock_cpu->master_cycles : 0;
    uint32_t pc24 = 0x00830b; /* sole executable ROM read site */
    uint8_t bytes[21];
    unsigned n = 0;
    for (unsigned i = 0; i < 8; i++) bytes[n++] = (uint8_t)(ordinal >> (8 * i));
    for (unsigned i = 0; i < 8; i++) bytes[n++] = (uint8_t)(master >> (8 * i));
    for (unsigned i = 0; i < 4; i++) bytes[n++] = (uint8_t)(pc24 >> (8 * i));
    bytes[n++] = value;
    for (unsigned i = 0; i < n; i++) {
      g_hvbjoy_digest ^= bytes[i];
      g_hvbjoy_digest *= UINT64_C(0x100000001b3);
    }
    if (getenv("GOOF_HVBJOY_TRACE"))
      fprintf(stderr, "HVBJOY_READ_V1 ordinal=%llu master_cycles=%llu "
              "pc=%06X value=%02X vblank=%u hblank=%u busy=%u\n",
              (unsigned long long)ordinal, (unsigned long long)master,
              pc24, value, (value >> 7) & 1, (value >> 6) & 1, value & 1);
    autojoy_observe_busy_read(ordinal, master, value);
    general_dma_on_hvbjoy_read(master);
  }
  else if (reg == 0x4211 || reg == 0x2137 || reg == 0x213c || reg == 0x213d)
    g_beam_latch_reads++;
  else if (reg >= 0x4218 && reg <= 0x421f) {
    g_joy_auto_reads++;
    autojoy_observe_result_read(reg, value);
  }
  else if (reg == 0x4016 || reg == 0x4017) g_joy_manual_reads++;
  if (reg >= 0x2140 && reg <= 0x2143) {
    uint8_t bytes[3] = { (uint8_t)(reg & 0xff), (uint8_t)(reg >> 8), value };
    for (int i = 0; i < 3; i++) {
      g_apu_read_digest ^= bytes[i];
      g_apu_read_digest *= UINT64_C(0x100000001b3);
    }
    g_apu_reads_epoch++;
    g_apu_reads_total++;
    /* Keyed on register+value only: the measured streak is an upper bound
     * on any PC-qualified spin, so the spin check stays conservative. */
    if (reg == g_apu_streak_reg && value == g_apu_streak_val) {
      g_apu_read_streak++;
    } else {
      g_apu_read_streak = 1;
      g_apu_streak_reg = reg;
      g_apu_streak_val = value;
    }
    if (g_apu_read_streak > g_apu_read_streak_max)
      g_apu_read_streak_max = g_apu_read_streak;
  }
}

/* CPU -> APU writes during the IPL upload.  ReadReg/WriteReg are the real
 * guest-side APUIO path (common_rtl.c routes them to RtlApuWrite), so the
 * handshake is observed exactly as the guest performs it. */
static void observe_ipl_write(uint16_t reg, uint8_t value) {
  if (reg < 0x2140 || reg > 0x2143 || !g_snes || !g_snes->apu) return;
  if (!g_snes->apu->romReadable) return;
  if (!g_ipl_ready_seen) {
    uint16_t ready = (uint16_t)((g_snes->apu->outPorts[1] << 8) |
                                g_snes->apu->outPorts[0]);
    if (ready == GATE_IPL_READY_WORD) { g_ipl_ready_word = ready; g_ipl_ready_seen = 1; }
  }
}

/* The payload itself is delivered as atomic $2140 word transactions:
 * high byte = data ($2141), low byte = transfer index ($2140).  The first
 * such word is the normative "first data byte" of the upload. */
static void observe_ipl_write_word(uint16_t reg, uint16_t value) {
  if (reg != 0x2140 || !g_snes || !g_snes->apu) return;
  if (!g_snes->apu->romReadable || !g_ipl_ready_seen) return;
  if (g_ipl_first_data_seen) return;
  g_ipl_first_data_byte = (uint8_t)(value >> 8);
  g_ipl_first_index_byte = (uint8_t)value;
  g_ipl_first_data_seen = 1;
}

uint8 __real_ReadReg(uint16 reg);
uint16 __real_ReadRegWord(uint16 reg);
void __real_WriteReg(uint16 reg, uint8 value);
void __real_WriteRegWord(uint16 reg, uint16 value);

uint8 __wrap_ReadReg(uint16 reg) {
  uint8 v = __real_ReadReg(reg);
  observe_read(reg, v);
  return v;
}
uint16 __wrap_ReadRegWord(uint16 reg) {
  uint16 v = __real_ReadRegWord(reg);
  observe_read(reg, (uint8_t)v);
  observe_read((uint16_t)(reg + 1), (uint8_t)(v >> 8));
  return v;
}
void __wrap_WriteReg(uint16 reg, uint8 value) {
  if (g_in_render) { g_render_violations++; }
  else {
    if (reg == 0x420c && value != 0) g_hdma_nonzero_writes++;
    if (reg == 0x420b) autojoy_observe_dma_start(value);
    if (reg == 0x420b) general_dma_observe_start(value);
    if (reg == 0x4200) {
      if (value & 0x01) g_autojoy_enable_writes++;
      else g_autojoy_disable_writes++;
      uint8_t b[4] = { 0x03, value,
                       (uint8_t)(g_cpu.master_cycles %
                                 RTL_MASTER_CYCLES_PER_FRAME),
                       (uint8_t)((g_cpu.master_cycles %
                                  RTL_MASTER_CYCLES_PER_FRAME) >> 8) };
      autojoy_fold(b, 4);
      if (getenv("GOOF_AUTOJOY_TRACE"))
        fprintf(stderr, "AUTOJOY_NMITIMEN_V1 ordinal=%llu master=%llu "
                "phase=%llu value=%02X nmi=%u virq=%u hirq=%u autojoy=%u\n",
                (unsigned long long)(g_autojoy_enable_writes +
                                     g_autojoy_disable_writes),
                (unsigned long long)g_cpu.master_cycles,
                (unsigned long long)(g_cpu.master_cycles %
                                     RTL_MASTER_CYCLES_PER_FRAME),
                value, (value >> 7) & 1, (value >> 5) & 1, (value >> 4) & 1,
                value & 1);
    }
    if (!reg_supported(reg)) g_unsupported_mmio++;
    observe_ipl_write(reg, value);
  }
  __real_WriteReg(reg, value);
}
void __wrap_WriteRegWord(uint16 reg, uint16 value) {
  if (g_in_render) { g_render_violations++; }
  else {
    if (reg == 0x420c && (value & 0xff) != 0) g_hdma_nonzero_writes++;
    if (reg == 0x420b) autojoy_observe_dma_start((uint8_t)value);
    if (reg == 0x420a) autojoy_observe_dma_start((uint8_t)(value >> 8));
    if (reg == 0x420b) general_dma_observe_start((uint8_t)value);
    if (reg == 0x420a) general_dma_observe_start((uint8_t)(value >> 8));
    observe_ipl_write_word(reg, value);
  }
  __real_WriteRegWord(reg, value);
}

void cpu_trace_func_entry(CpuState *cpu, uint32_t pc24, const char *name) {
  g_gdma_last_func_pc24 = pc24;   /* GENERAL_DMA diagnostic: AOT site */
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

/* Neutral-input contract: the only joypad values this gate ever supplies
 * are the ones the driver hands it, and the driver's are zero.  No input is
 * synthesised anywhere in this file. */
static uint64_t g_nonneutral_input_events;
static void logical_event(void *mem, bool nmi_edge, uint16_t joy1,
                          uint16_t joy2) {
  (void)mem;
  g_render_violations += g_in_render;
  if (joy1 != 0 || joy2 != 0) g_nonneutral_input_events++;
  g_snes->input1_currentState = joy1;
  g_snes->input2_currentState = joy2;
  if (nmi_edge) {
    g_snes->inNmi = true;
    g_autojoy_nmi_edges++;
    general_dma_on_nmi_edge();
    g_autojoy_edge_master = g_cpu.master_cycles;
    g_autojoy_dma_bytes = g_autojoy_dma_channels = g_autojoy_dma_starts = 0;
    uint64_t phase = g_cpu.master_cycles % RTL_MASTER_CYCLES_PER_FRAME;
    if (phase) g_autojoy_nmi_edge_phase_nonzero++;
    if (g_snes->autoJoyRead) g_autojoy_edges_autojoy_on++;
    uint8_t b[16]; unsigned n = 0;
    b[n++] = 0x04;                                  /* row type: NMI edge */
    for (unsigned i = 0; i < 8; i++)
      b[n++] = (uint8_t)(g_cpu.master_cycles >> (8 * i));
    b[n++] = (uint8_t)(g_snes->autoJoyRead ? 1 : 0);
    b[n++] = (uint8_t)joy1; b[n++] = (uint8_t)(joy1 >> 8);
    b[n++] = (uint8_t)joy2; b[n++] = (uint8_t)(joy2 >> 8);
    autojoy_fold(b, n);
    if (getenv("GOOF_AUTOJOY_TRACE"))
      fprintf(stderr, "AUTOJOY_NMI_EDGE_V1 edge=%llu master=%llu "
              "boundary=%llu phase=%llu nmitimen_autojoy=%u joy1=%04X "
              "joy2=%04X\n", (unsigned long long)g_autojoy_nmi_edges,
              (unsigned long long)g_cpu.master_cycles,
              (unsigned long long)(g_cpu.master_cycles /
                                   RTL_MASTER_CYCLES_PER_FRAME),
              (unsigned long long)phase, (unsigned)g_snes->autoJoyRead,
              joy1, joy2);
  } else if (getenv("GOOF_AUTOJOY_TRACE")) {
    fprintf(stderr, "AUTOJOY_INPUT_EVENT_V1 master=%llu phase=%llu "
            "joy1=%04X joy2=%04X\n",
            (unsigned long long)g_cpu.master_cycles,
            (unsigned long long)(g_cpu.master_cycles %
                                 RTL_MASTER_CYCLES_PER_FRAME), joy1, joy2);
  }
}

void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

static const RtlGameInfo kGoofBootE1000Info = {
  .title = "goof-boot-e1000-gate",
  .initialize = NULL,
  .run_frame = goof_boot_harness_run_frame,
  .draw_ppu_frame = NULL,
  .save_name_prefix = "goof-boot-e1000",
};

void __real_apu_cpuWrite(Apu *apu, uint16_t adr, uint8_t value);
void __wrap_apu_cpuWrite(Apu *apu, uint16_t adr, uint8_t value) {
  bool rom_was_readable = apu->romReadable;
  /* First data byte the CPU hands the IPL after the $BBAA ready word. */
  __real_apu_cpuWrite(apu, adr, value);
  if (rom_was_readable && !apu->romReadable) g_upload_rom_disable_events++;
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

/* ---------------------------------------------------------------------- */
/* Render isolation: focused live-state blocks compared byte for byte.      */
/* Pointer-bearing structs are compared only within one process.           */
/* ---------------------------------------------------------------------- */

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
      printf("LIVE_BLOCK_CHANGED epoch=%u block=%s bytes=%zu\n", g_epoch,
             g_blocks[i].name, block_changed);
    changed += block_changed;
  }
  return changed;
}

/* ---------------------------------------------------------------------- */

typedef struct {
  unsigned epochs;
  bool *dump;
  const char *output_dir;
} Options;

static void usage_fail(void) {
  fprintf(stderr,
          "usage: goof_boot_e1000 ROM [--epochs N(1..%d)] [--dump-frame N]... "
          "[--output-dir DIR] [--discover] [--quiet] [--audio6-advisory]\n",
          kGateEpochs);
  exit(2);
}

static unsigned parse_epoch(const char *text) {
  char *end;
  unsigned long v = strtoul(text, &end, 10);
  if (*text == '\0' || *end != '\0' || v < 1 || v > kGateEpochs) usage_fail();
  return (unsigned)v;
}

static Options parse_options(int argc, char **argv) {
  Options o = {.epochs = kGateEpochs};
  o.dump = calloc(kGateEpochs + 1, sizeof(bool));
  if (!o.dump) exit(2);
  bool any_dump = false;
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--quiet") == 0) { g_quiet = 1; continue; }
    if (strcmp(argv[i], "--audio6-advisory") == 0) {
      opt_audio6_advisory = 1; continue;
    }
    if (strcmp(argv[i], "--audio-pins-advisory") == 0) {
      opt_audio_pins_advisory = 1; continue;
    }
    if (strcmp(argv[i], "--pcm-drain") == 0) { opt_pcm_drain = 1; continue; }
    /* Checkpoint discovery: report the full architectural record at every
     * boundary and assert no checkpoint VALUE, so a re-baselining session can
     * read the new anchors out of one run instead of bisecting failures.
     * Every invariant still holds; only the twelve pinned values are skipped. */
    if (strcmp(argv[i], "--discover") == 0) { g_discover = 1; continue; }
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

static const Checkpoint *checkpoint_for(unsigned epoch) {
  for (unsigned i = 0; i < kCheckpointCount; i++)
    if (kCheckpoints[i].epoch == epoch) return &kCheckpoints[i];
  return NULL;
}

static const char *display_state(const Ppu *p) {
  if (p->inidisp & 0x80) return "forced_blank";
  if ((p->inidisp & 0x0f) == 0) return "brightness_0";
  if (p->screenEnabled[0] == 0 && p->screenEnabled[1] == 0) return "no_layers";
  return "display_on";
}

/* Verifies the E154 IPL upload contract in full. */
static void verify_ipl_contract(const Checkpoint *chk) {
  const Apu *apu = g_snes->apu;
  GATE_EXPECT(chk, "ipl_ready_word", g_ipl_ready_word, GATE_IPL_READY_WORD);
  GATE_EXPECT(chk, "ipl_first_data_byte", g_ipl_first_data_byte,
              GATE_IPL_FIRST_DATA);
  GATE_EXPECT(chk, "ipl_first_index_byte", g_ipl_first_index_byte,
              GATE_IPL_FIRST_INDEX);
  GATE_EXPECT(chk, "ipl_image", g_ipl_image_hash, GATE_IPL_IMAGE);
  GATE_EXPECT(chk, "ipl_header_echoes", g_ipl_header_echoes,
              GATE_IPL_HEADER_ECHOES);
  GATE_EXPECT(chk, "stale_latches", g_apu_word_interleave_events, 0);
  GATE_EXPECT(chk, "rom_disable_events", g_upload_rom_disable_events, 1);
  GATE_EXPECT(chk, "romReadable", apu->romReadable ? 1u : 0u, 0);
  GATE_EXPECT(chk, "port2142_ready_events", g_driver_port2_ready_events,
              GATE_IPL_PORT2_READY);
  GATE_EXPECT(chk, "apuio_port_queue_depth", apu_portQueueDepth((Apu *)apu), 0);
  GATE_CHECK(g_apu_word_transactions != 0);
  printf("SPC_IPL_CONTRACT PASS image=%016llx ready_word=%04X "
         "first_data=%02X first_index=%02X stale_latches=%llu romReadable=%u "
         "port2142_ready=%llu word_transactions=%llu\n",
         (unsigned long long)g_ipl_image_hash, g_ipl_ready_word,
         g_ipl_first_data_byte, g_ipl_first_index_byte,
         (unsigned long long)g_apu_word_interleave_events,
         apu->romReadable ? 1u : 0u,
         (unsigned long long)g_driver_port2_ready_events,
         (unsigned long long)g_apu_word_transactions);
}

int main(int argc, char **argv) {
  if (argc < 2) usage_fail();
  Options opt = parse_options(argc, argv);
  setvbuf(stdout, NULL, _IOLBF, 0);
  size_t rom_size;
  uint8_t *rom = goof_gate_load_pinned_rom(argv[1], &rom_size);
  if (!rom) gate_fail_msg("rom=%s condition=pinned ROM size/SHA-256", argv[1]);

  RtlRegisterGame(&kGoofBootE1000Info);
  GATE_CHECK(SnesInit(rom, (int)rom_size));
  cpu_state_init(&g_cpu, g_ram);
  vblank_boundary_test();
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
  /* Per-epoch deltas start from the reset boundary, which may already hold
   * bootstrap NMIs (and their $9C increments). */
  g_prev_9c = g_ram[0x9c]; g_prev_nmis = g->nmis; g_prev_epilogues = g->epilogues;
  register_live_blocks();

  uint64_t certified_boundaries = 0;
  unsigned checkpoints_met = 0;

  for (unsigned epoch = 1; epoch <= opt.epochs; epoch++) {
    g_epoch = epoch;
    const Checkpoint *chk = checkpoint_for(epoch);
    /* Full live-block isolation diff is O(WRAM) per epoch.  Run it at every
     * checkpoint, on a periodic sample, and on the first and last epoch;
     * the MMIO / bus / trace wraps above still count any render-time guest
     * activity on all 1000 boundaries. */
    bool deep_isolation = chk != NULL || epoch == 1 ||
                          epoch == opt.epochs || (epoch % 50) == 0;

    alarm(120);
    kGoofBootE1000Info.run_frame(); /* aborts unless EPOCH_COMPLETE_WAITING */
    alarm(0);

    /* --- global invariants, asserted at every certified boundary --- */
    GATE_INVARIANT(g_run_frame.last_result.reason == EPOCH_COMPLETE_WAITING,
                   "certified_boundary", epoch, "reason=%d",
                   (int)g_run_frame.last_result.reason);
    GATE_INVARIANT(g_run_frame.last_result.certified && g->certified,
                   "certified_boundary", epoch, "certified=%d", (int)g->certified);
    /* One logical epoch : N physical frame boundaries, N >= 1.  The former
     * `nmis == epoch` invariant is RETIRED: it asserted exactly one NMI per
     * logical epoch, which is the defect this milestone removed (epoch 1
     * alone spans ~121 NTSC frame periods and now services one NMI at each).
     * What replaces it is the three-point chain request -> entry -> guest
     * epilogue, asserted per epoch, plus the boundary accounting. */
    if (opt_pcm_drain) pcm_drain();
    if (epoch == g_audio6_start_epoch) audio_sample(&g_audio_start, epoch, g);
    if (epoch == opt.epochs)               audio_sample(&g_audio_end, epoch, g);

    const RunResult *rr = &g_run_frame.last_result;
    uint8_t now_9c = g_ram[0x9c];
    uint64_t d_9c = (uint8_t)(now_9c - g_prev_9c);
    uint64_t d_nmis = g->nmis - g_prev_nmis;
    uint64_t d_epi = g->epilogues - g_prev_epilogues;
    g_prev_9c = now_9c; g_prev_nmis = g->nmis; g_prev_epilogues = g->epilogues;
    g_total_periods += rr->physical_periods;
    general_dma_on_epoch_end(epoch, rr->physical_periods,
                             rr->idle_padding_master_cycles);
    if (getenv("GOOF_AUTOJOY_TRACE"))
      fprintf(stderr, "AUTOJOY_EPOCH_V1 epoch=%u periods=%llu entries=%llu "
              "master=%llu\n", epoch,
              (unsigned long long)rr->physical_periods,
              (unsigned long long)rr->nmi_entries,
              (unsigned long long)g_cpu.master_cycles);
    g_total_requests += rr->nmi_requests;
    g_total_entries += rr->nmi_entries;
    g_total_padding += rr->idle_padding_master_cycles;
    GATE_INVARIANT(g_run_frame.logical_nmi_epochs == epoch, "epoch_accounting",
                   epoch, "logical_nmi_epochs=%llu",
                   (unsigned long long)g_run_frame.logical_nmi_epochs);
    GATE_INVARIANT(rr->nmi_entries >= 1, "epoch_serviced", epoch,
                   "nmi_entries=%llu", (unsigned long long)rr->nmi_entries);
    GATE_INVARIANT(rr->nmi_entries == d_nmis && rr->nmi_entries == d_epi,
                   "nmi_entry_epilogue_balance", epoch,
                   "entries=%llu delta_nmis=%llu delta_epilogues=%llu",
                   (unsigned long long)rr->nmi_entries,
                   (unsigned long long)d_nmis, (unsigned long long)d_epi);
    GATE_INVARIANT(d_9c == (rr->nmi_entries & 0xff), "guest_nmi_counter", epoch,
                   "delta_9C=%llu entries_mod_256=%llu",
                   (unsigned long long)d_9c,
                   (unsigned long long)(rr->nmi_entries & 0xff));
    GATE_INVARIANT(rr->nmi_requests >= rr->nmi_entries &&
                   rr->nmi_requests - rr->nmi_entries <= 1,
                   "at_most_one_edge_in_flight", epoch,
                   "requests=%llu entries=%llu",
                   (unsigned long long)rr->nmi_requests,
                   (unsigned long long)rr->nmi_entries);
    GATE_INVARIANT(rr->physical_periods >= 1 &&
                   rr->physical_periods >= rr->nmi_entries,
                   "boundary_accounting", epoch,
                   "periods=%llu entries=%llu",
                   (unsigned long long)rr->physical_periods,
                   (unsigned long long)rr->nmi_entries);
    /* Fail-closed guards on the boundary engine itself.  The deferred-edge
     * rule is real code, but E1..E1000 must never need it: the longest NMI
     * handler is ~6% of a frame period and every handler starts at a
     * boundary, so a second boundary inside one handler would mean the
     * handler grew 16x and the question must be reopened WITH DATA. */
    GATE_INVARIANT(g->nmi_deferred_in_nmi == 0 && g->nmi_deferred_collapsed == 0,
                   "no_boundary_inside_nmi", epoch, "deferred=%llu collapsed=%llu",
                   (unsigned long long)g->nmi_deferred_in_nmi,
                   (unsigned long long)g->nmi_deferred_collapsed);
    GATE_INVARIANT(g->nmi_request_collapsed == 0, "no_edge_collapse", epoch,
                   "collapsed=%llu", (unsigned long long)g->nmi_request_collapsed);
    GATE_INVARIANT(g->multi_boundary_detections == 0, "no_multi_boundary_detection",
                   epoch, "detections=%llu",
                   (unsigned long long)g->multi_boundary_detections);
    GATE_INVARIANT(g->max_detection_overshoot <= GATE_MAX_DETECTION_OVERSHOOT,
                   "boundary_detection_latency", epoch, "overshoot=%llu limit=%u",
                   (unsigned long long)g->max_detection_overshoot,
                   GATE_MAX_DETECTION_OVERSHOOT);
    GATE_INVARIANT(g->frame_period_master == GATE_FRAME_PERIOD_MASTER,
                   "frame_period", epoch, "period=%llu expected=%u",
                   (unsigned long long)g->frame_period_master,
                   GATE_FRAME_PERIOD_MASTER);
    GATE_INVARIANT(g->cursor.cpu.k == 0x80 && g->cursor.cpu.pc == 0x80a7 &&
                   g->cursor.cpu.sp == 0x1dff, "cursor_integrity", epoch,
                   "pc=%02X:%04X s=%04X", g->cursor.cpu.k, g->cursor.cpu.pc,
                   g->cursor.cpu.sp);
    GATE_INVARIANT(g_recomp_stack_top == 0, "recomp_stack_boundary", epoch,
                   "recomp_stack_top=%d", g_recomp_stack_top);
    GATE_INVARIANT(g->host_depth == 0, "host_depth_boundary", epoch,
                   "host_depth=%u", g->host_depth);
    GATE_INVARIANT(g_mx_mismatches == 0 && g_mx_suffixless == 0, "mx_mismatch",
                   epoch, "mismatch=%llu suffixless=%llu",
                   (unsigned long long)g_mx_mismatches,
                   (unsigned long long)g_mx_suffixless);
    GATE_INVARIANT(CpuUnresolvedAbandonTotal() == 0, "abandon", epoch,
                   "abandon=%llu", (unsigned long long)CpuUnresolvedAbandonTotal());
    GATE_INVARIANT(g_unsupported_mmio == 0, "unsupported_mmio", epoch,
                   "unsupported=%llu", (unsigned long long)g_unsupported_mmio);
    GATE_INVARIANT(g_render_violations == 0, "render_isolation", epoch,
                   "violations=%llu", (unsigned long long)g_render_violations);
    GATE_INVARIANT(g_apu_word_interleave_events == 0, "apuio_atomicity", epoch,
                   "interleaves=%llu",
                   (unsigned long long)g_apu_word_interleave_events);
    GATE_INVARIANT(g_apu_read_streak_max < GATE_APU_SPIN_THRESHOLD,
                   "apu_no_spin_wait", epoch, "streak_max=%llu threshold=%u",
                   (unsigned long long)g_apu_read_streak_max,
                   GATE_APU_SPIN_THRESHOLD);
    /* Every H+V compare in this domain executes the normal no-op handler. */
    GATE_INVARIANT(!g->cursor.cpu.irqWanted && !g->active_plan.irq &&
                   g->irq_compare_count == g->irq_entry_count &&
                   g->irq_entry_count == g->irq_ack_count &&
                   g->irq_ack_count == g->irq_rti_count &&
                   !g->irq_stack_failures && !g->irq_aot_straddles &&
                   !g->irq_inside_nmi_count && !g->irq_duplicate &&
                   !g->ppu_journal_overflows && !g->in_irq,
                   "normal_irq_accounting", epoch,
                   "compare=%llu entry=%llu ack=%llu wanted=%d in_irq=%d",
                   (unsigned long long)g->irq_compare_count,
                   (unsigned long long)g->irq_entry_count,
                   (unsigned long long)g->irq_ack_count,
                   (int)g->cursor.cpu.irqWanted, (int)g->in_irq);
    GATE_INVARIANT(g_ram[0x9d] == 0 && g_ram[0x9e] == 0, "no_irq_armed", epoch,
                   "$9D=%02X $9E=%02X", g_ram[0x9d], g_ram[0x9e]);
    /* HDMA: never enabled, so never required for progress. */
    GATE_INVARIANT(g_hdma_nonzero_writes == 0 && g_snesrecomp_last_hdmaen == 0,
                   "no_hdma_required", epoch, "nonzero_writes=%llu hdmaen=%02X",
                   (unsigned long long)g_hdma_nonzero_writes,
                   g_snesrecomp_last_hdmaen);
    /* $4211 reads are the handler's TIMEUP acknowledgments. */
    GATE_INVARIANT(g_beam_latch_reads == g->irq_ack_count,
                   "only_timeup_latch_reads", epoch,
                   "latch_reads=%llu ack=%llu",
                   (unsigned long long)g_beam_latch_reads,
                   (unsigned long long)g->irq_ack_count);
    /* Per NMI ENTRY, like the joypad bound: the guest reads $4212 inside its
     * NMI handler, so the natural denominator is the number of serviced
     * frames, not the number of logical epochs. */
    GATE_INVARIANT(g_beam_reads <= g->nmis * GATE_MAX_BEAM_READS_PER_ENTRY,
                   "no_beam_dependent_wait", epoch, "reads=%llu bound=%llu",
                   (unsigned long long)g_beam_reads,
                   (unsigned long long)(g->nmis * GATE_MAX_BEAM_READS_PER_ENTRY));
    /* Neutral input: nothing is ever injected, and the guest never falls
     * back to the manual joypad ports. */
    GATE_INVARIANT(g_nonneutral_input_events == 0, "neutral_input", epoch,
                   "nonneutral_events=%llu",
                   (unsigned long long)g_nonneutral_input_events);
    GATE_INVARIANT(g_joy_manual_reads == 0, "no_manual_joypad_read", epoch,
                   "manual_reads=%llu", (unsigned long long)g_joy_manual_reads);
    GATE_INVARIANT(g_joy_auto_reads <= g->nmis * GATE_MAX_JOY_READS_PER_ENTRY,
                   "no_input_poll_growth", epoch, "reads=%llu bound=%llu",
                   (unsigned long long)g_joy_auto_reads,
                   (unsigned long long)(g->nmis * GATE_MAX_JOY_READS_PER_ENTRY));
    GATE_INVARIANT(!g->cursor.cpu.waiting && !g->cursor.cpu.stopped,
                   "no_wai_stp", epoch, "waiting=%d stopped=%d",
                   (int)g->cursor.cpu.waiting, (int)g->cursor.cpu.stopped);
    certified_boundaries++;

    uint64_t logical = goof_gate_logical_epoch_hash_v1(g);
    const Apu *apu = g_snes->apu;

    /* Boundary-sampled APU ack progression. */
    if (!apu->romReadable) {
      uint8_t ack = apu->outPorts[2];
      if (ack > g_ack_last && ack <= 0x7f) {
        if (g_ack_last != 0) g_ack_advances++;
        g_ack_last = ack;
      }
    }

    /* --- isolated render of the certified boundary --- */
    const Ppu *const live_ppu = g_ppu;
    uint64_t vram = goof_gate_fnv1a64(live_ppu->vram, sizeof(live_ppu->vram));
    uint64_t oam = goof_gate_oam_hash(live_ppu);
    uint64_t cgram = goof_gate_fnv1a64(live_ppu->cgram, sizeof(live_ppu->cgram));
    if (deep_isolation) capture_live_blocks();

    g_in_render = 1;
    bool rendered = goof_headless_render(&g_renderer, live_ppu, g_dma, epoch,
                                         logical);
    g_in_render = 0;
    GATE_CHECK(rendered);
    GATE_INVARIANT(g_render_violations == 0, "render_isolation", epoch,
                   "violations=%llu", (unsigned long long)g_render_violations);
    goof_headless_frame_measure(&g_renderer);
    const GoofHeadlessFrameMeta *m = &g_renderer.meta;

    /* The snapshot must have been consumed by the render and must never
     * alias the live PPU or its framebuffer binding. */
    const Ppu *snap = &g_renderer.snapshot;
    GATE_CHECK(snap->renderBuffer == (uint8_t *)g_renderer.pixels);
    GATE_CHECK(snap->evenFrame != live_ppu->evenFrame);
    GATE_CHECK(live_ppu->renderBuffer != (uint8_t *)g_renderer.pixels);

    if (opt.dump[epoch]) {
      char ppm[4096];
      GATE_CHECK(snprintf(ppm, sizeof(ppm), "%s/goof_epoch%04u.ppm",
                          opt.output_dir, epoch) < (int)sizeof(ppm));
      GATE_CHECK(goof_headless_write_ppm(&g_renderer, ppm));
    }
    uint64_t framebuffer = m->fnv1a64;
    uint32_t colors = m->unique_colors, non_black = m->non_black_pixels;
    goof_headless_discard(&g_renderer);

    if (deep_isolation) {
      GATE_INVARIANT(changed_live_bytes() == 0, "render_isolation", epoch,
                     "live_state_changed=%s", "YES");
    }
    GATE_CHECK(g_ppu == live_ppu);
    GATE_INVARIANT(goof_gate_logical_epoch_hash_v1(g) == logical,
                   "render_isolation", epoch, "logical_perturbed=%s", "YES");
    GATE_CHECK(goof_gate_fnv1a64(live_ppu->vram, sizeof(live_ppu->vram)) == vram);
    GATE_CHECK(goof_gate_oam_hash(live_ppu) == oam);
    GATE_CHECK(goof_gate_fnv1a64(live_ppu->cgram, sizeof(live_ppu->cgram)) == cgram);
    GATE_CHECK(m->width == GOOF_HEADLESS_WIDTH &&
               m->height == GOOF_HEADLESS_HEIGHT);

    if (g_discover)
      printf("APUREAD epoch=%u reads=%llu total=%llu digest=%016llx\n",
             epoch, (unsigned long long)g_apu_reads_epoch,
             (unsigned long long)g_apu_reads_total,
             (unsigned long long)g_apu_read_digest);
    g_apu_reads_epoch = 0;
    if (g_discover)
      printf("RECORD epoch=%u logical=%016llx fb=%016llx inidisp=%02X tm=%02X "
             "colors=%u non_black=%u slots=%02X,%02X,%02X,%02X "
             "sel=%02X,%02X,%02X,%02X spc_pc=%04X spc_cycles=%llu "
             "apu_ram=%016llx rom_readable=%u periods=%llu requests=%llu "
             "entries=%llu d9c=%llu padding=%llu display=%s\n",
             epoch, (unsigned long long)logical, (unsigned long long)framebuffer,
             live_ppu->inidisp, live_ppu->screenEnabled[0], colors, non_black,
             g_ram[0x50], g_ram[0x58], g_ram[0x60], g_ram[0x68],
             g_ram[0x56], g_ram[0x5e], g_ram[0x66], g_ram[0x6e],
             apu->spc->pc, (unsigned long long)apu->cycles,
             (unsigned long long)goof_gate_fnv1a64(apu->ram, sizeof(apu->ram)),
             apu->romReadable ? 1u : 0u,
             (unsigned long long)rr->physical_periods,
             (unsigned long long)rr->nmi_requests,
             (unsigned long long)rr->nmi_entries, (unsigned long long)d_9c,
             (unsigned long long)rr->idle_padding_master_cycles,
             display_state(live_ppu));

    /* --- contracts that are NOT per-epoch hashes -----------------------
     * The SPC IPL upload contract is pinned to the APU protocol (image
     * digest, ready word, first data/index byte, latch and queue state), not
     * to the epoch's logical hash, so it must hold even in discovery mode --
     * otherwise a re-baselining run would silently stop certifying the one
     * contract a cadence change must NOT move. */
    /* In discovery the contract is verified where the upload actually
     * completes (the IPL ROM is switched off), not at a pinned epoch number:
     * a chronology change moves the epoch in which the upload happens, and
     * the rebaseline session needs to read that epoch out of the run. */
    if (g_discover && !g_ipl_contract_verified && g_upload_rom_disable_events) {
      const Checkpoint *ipl = NULL;
      for (unsigned i = 0; i < kCheckpointCount; i++)
        if (kCheckpoints[i].flags & CHK_APU_IPL) ipl = &kCheckpoints[i];
      GATE_CHECK(ipl != NULL);
      verify_ipl_contract(ipl);
      g_audio6_start_epoch = epoch + (GATE_AUDIO6_START_EPOCH - ipl->epoch);
      printf("DISCOVER_IPL_UPLOAD_EPOCH epoch=%u (pinned checkpoint epoch=%u) "
             "audio6_anchor=%u (pinned %u)\n", epoch, ipl->epoch,
             g_audio6_start_epoch, GATE_AUDIO6_START_EPOCH);
      g_ipl_contract_verified = 1;
    }

    /* --- checkpoint contract --- */
    if (chk && !g_discover) {
      GATE_EXPECT(chk, "logical", logical, chk->logical);
      if (chk->flags & CHK_FB)
        GATE_EXPECT(chk, "framebuffer", framebuffer, chk->framebuffer);
      if (chk->flags & CHK_PPU) {
        GATE_EXPECT(chk, "$2100_inidisp", live_ppu->inidisp, chk->inidisp);
        GATE_EXPECT(chk, "$212C_tm", live_ppu->screenEnabled[0], chk->tm);
        GATE_EXPECT(chk, "unique_colors", colors, chk->colors);
        GATE_EXPECT(chk, "non_black_pixels", non_black, chk->non_black);
      }
      if (chk->flags & CHK_SCHED) {
        for (unsigned s = 0; s < 4; s++) {
          char f[32];
          snprintf(f, sizeof(f), "slot%u_state", s);
          GATE_EXPECT(chk, f, g_ram[0x50 + 8 * s], chk->slot_state[s]);
          snprintf(f, sizeof(f), "slot%u_selector", s);
          GATE_EXPECT(chk, f, g_ram[0x56 + 8 * s], chk->slot_sel[s]);
        }
      }
      if (chk->flags & CHK_APU_PRE) {
        /* The SPC is now driven from guest master time at every physical
         * boundary, so it is already running inside the IPL ROM before the
         * upload -- `apu_cycles == 0` was only ever true because the epoch
         * clock hardly advanced.  The stronger assertion replaces it: still
         * in IPL ROM, and at an exact SPC pc / cycle count / RAM digest. */
        GATE_EXPECT(chk, "apu_romReadable", apu->romReadable ? 1u : 0u, 1);
        audio_pins(chk, apu);
      }
      if (chk->flags & CHK_APU_IPL) verify_ipl_contract(chk);
      if (chk->flags & CHK_APU_END) {
        audio_pins(chk, apu);
        GATE_EXPECT(chk, "apu_romReadable", apu->romReadable ? 1u : 0u, 0);
        GATE_EXPECT(chk, "apu_port_queue_depth",
                    apu_portQueueDepth((Apu *)apu), 0);
      }
      checkpoints_met++;
      printf("CHECKPOINT epoch=%u logical=%016llx fb=%016llx inidisp=%02X "
             "tm=%02X colors=%u non_black=%u display=%s PASS milestone=\"%s\"\n",
             epoch, (unsigned long long)logical,
             (unsigned long long)framebuffer, live_ppu->inidisp,
             live_ppu->screenEnabled[0], colors, non_black,
             display_state(live_ppu), chk->milestone);
    } else if (!g_quiet) {
      printf("EPOCH epoch=%u logical=%016llx fb=%016llx inidisp=%02X tm=%02X "
             "colors=%u non_black=%u vram=%016llx oam=%016llx cgram=%016llx\n",
             epoch, (unsigned long long)logical,
             (unsigned long long)framebuffer, live_ppu->inidisp,
             live_ppu->screenEnabled[0], colors, non_black,
             (unsigned long long)vram, (unsigned long long)oam,
             (unsigned long long)cgram);
    }
  }

  /* --- campaign-level contracts --- */
  GATE_CHECK(g_mx_entries != 0);
  printf("GOOF_IRQ_ACCOUNT_V1 compare=%llu entry=%llu ack=%llu rti=%llu "
         "idle_wake=%llu aot_declines=%llu aot_straddles=%llu "
         "entry_master=%llu max_entry_delay=%llu max_handler_master=%llu "
         "lost=%llu duplicate=%llu inside_nmi=%llu deferred_i=%llu "
         "stack_failures=%llu\n",
         (unsigned long long)g->irq_compare_count,
         (unsigned long long)g->irq_entry_count,
         (unsigned long long)g->irq_ack_count,
         (unsigned long long)g->irq_rti_count,
         (unsigned long long)g->irq_idle_wake_count,
         (unsigned long long)g->irq_aot_declines,
         (unsigned long long)g->irq_aot_straddles,
         (unsigned long long)g->irq_entry_master,
         (unsigned long long)g->irq_max_entry_delay,
         (unsigned long long)g->irq_max_handler_master,
         (unsigned long long)g->irq_lost,
         (unsigned long long)g->irq_duplicate,
         (unsigned long long)g->irq_inside_nmi_count,
         (unsigned long long)g->irq_deferred_i_count,
         (unsigned long long)g->irq_stack_failures);
  printf("GOOF_IRQ_DIGEST_V1 %016llx\n",
         (unsigned long long)g->irq_digest);
  printf("GOOF_PPU_JOURNAL_DIGEST_V1 writes=%llu max_used=%u overflows=%llu "
         "digest=%016llx\n",
         (unsigned long long)g->ppu_journal_writes,
         g->ppu_journal_max_used,
         (unsigned long long)g->ppu_journal_overflows,
         (unsigned long long)g->ppu_journal_digest);
  printf("PHYSICAL_BOUNDARIES total_periods=%llu nmi_requests=%llu "
         "nmi_entries=%llu idle_padding_master=%llu lifetime_periods=%llu "
         "max_detection_overshoot=%llu deferred_in_nmi=%llu "
         "deferred_collapsed=%llu request_collapsed=%llu "
         "multi_boundary_detections=%llu final_master_cycles=%llu "
         "frame_period=%u\n",
         (unsigned long long)g_total_periods,
         (unsigned long long)g_total_requests,
         (unsigned long long)g_total_entries,
         (unsigned long long)g_total_padding,
         (unsigned long long)g->physical_periods,
         (unsigned long long)g->max_detection_overshoot,
         (unsigned long long)g->nmi_deferred_in_nmi,
         (unsigned long long)g->nmi_deferred_collapsed,
         (unsigned long long)g->nmi_request_collapsed,
         (unsigned long long)g->multi_boundary_detections,
         (unsigned long long)g_cpu.master_cycles, GATE_FRAME_PERIOD_MASTER);
  if (opt.epochs == kGateEpochs) {
    unsigned expected_checkpoints = kCheckpointCount;
    if (!g_discover && checkpoints_met != expected_checkpoints)
      gate_fail_msg("checkpoints_met=%u expected=%u", checkpoints_met,
                    expected_checkpoints);
    if (certified_boundaries != kGateEpochs)
      gate_fail_msg("certified_boundaries=%llu expected=%u",
                    (unsigned long long)certified_boundaries, kGateEpochs);
    /* The APU command protocol is asserted by SHAPE (the driver's port-2 ack
     * counter advances, and ends where the protocol says), never by an epoch
     * index, so it holds in discovery mode too. */
    if (g_ack_advances < GATE_MIN_ACK_ADVANCES)
      gate_fail_msg("invariant=apu_command_progress advances=%llu minimum=%u",
                    (unsigned long long)g_ack_advances, GATE_MIN_ACK_ADVANCES);
    if (g_ack_last != GATE_FINAL_ACK)
      gate_fail_msg("invariant=apu_final_ack expected=%02X observed=%02X",
                    GATE_FINAL_ACK, g_ack_last);
    printf("APU_COMMAND_PROGRESS PASS post_upload_advances=%llu final_ack=%02X "
           "spin_streak_max=%llu\n", (unsigned long long)g_ack_advances,
           g_ack_last, (unsigned long long)g_apu_read_streak_max);
  }

  /* ---- AUDIO5 (producer health) / AUDIO6 (rate fidelity) / clamp -------
   * Reported unconditionally, including in discovery mode: these are
   * contracts on the guest's own production rate, not baselined hashes, so
   * a re-baselining run must not be able to stop certifying them. */
  {
    AudioTraceStats st;
    SnesApuCatchupStats cs;
    audio_trace_get_stats(&st);
    snes_apu_catchup_stats(&cs);

    printf("AUDIO5_PRODUCER_HEALTH produced=%llu dropped=%llu "
           "dropped_audible=%llu drop_runs=%llu occupancy_highwater=%u "
           "consumed=%llu consume_calls=%llu\n",
           (unsigned long long)st.produced, (unsigned long long)st.dropped,
           (unsigned long long)st.dropped_audible,
           (unsigned long long)st.drop_runs, st.occupancy_highwater,
           (unsigned long long)st.consumed,
           (unsigned long long)st.consume_calls);
    if (opt_pcm_drain)
      printf("AUDIO4_PCM_DRAIN drained_frames=%llu drain_hash=%016llx "
             "drain_calls=%llu drain_highwater=%llu\n",
             (unsigned long long)g_drain_frames,
             (unsigned long long)g_drain_hash,
             (unsigned long long)g_drain_calls,
             (unsigned long long)g_drain_highwater);
    printf("AUDIO1_NATIVE_PCM hash=%016llx frames=%llu\n",
           (unsigned long long)st.pcm_hash,
           (unsigned long long)st.pcm_frames);
    {
      uint64_t bsyncs, bmax, btimeouts, bresidual;
      rtl_apu_boundary_stats(&bsyncs, &bmax, &btimeouts, &bresidual);
      printf("APU_BOUNDARY_SYNC syncs=%llu max_advance=%llu timeouts=%llu "
             "max_post_sync_residual=%llu clock_valid=%d\n",
             (unsigned long long)bsyncs, (unsigned long long)bmax,
             (unsigned long long)btimeouts, (unsigned long long)bresidual,
             (int)rtl_apu_boundary_clock_valid());
      if (!opt_audio6_advisory && bsyncs) {
        /* §48 debt bound: no full frame of APU debt may survive a boundary,
         * in either direction.  A single sync never advances more than one
         * period in steady state, and never leaves the SPC a whole period
         * ahead of the boundary target. */
        GATE_INVARIANT(btimeouts == 0, "AUDIO6_boundary_sync_timeout",
                       g_audio_end.epoch, "timeouts=%llu",
                       (unsigned long long)btimeouts);
        GATE_INVARIANT(bmax <= GATE_APU_CYCLES_PER_PERIOD,
                       "AUDIO6_boundary_advance_bound", g_audio_end.epoch,
                       "max_advance=%llu limit=%u",
                       (unsigned long long)bmax, GATE_APU_CYCLES_PER_PERIOD);
        GATE_INVARIANT(bresidual < GATE_APU_CYCLES_PER_PERIOD,
                       "AUDIO6_boundary_residual_bound", g_audio_end.epoch,
                       "max_residual=%llu limit=%u",
                       (unsigned long long)bresidual,
                       GATE_APU_CYCLES_PER_PERIOD);
      }
    }
    printf("APU_CATCHUP_CLAMP calls=%llu cycles=%llu clamp_hits=%llu "
           "clamp_lost_cycles=%llu max_request=%llu max_residual=%llu "
           "hits_read=%llu hits_write=%llu hits_boundary=%llu hits_other=%llu "
           "lost_read=%llu lost_write=%llu lost_boundary=%llu lost_other=%llu\n",
           (unsigned long long)cs.calls, (unsigned long long)cs.cycles,
           (unsigned long long)cs.clamp_hits,
           (unsigned long long)cs.clamp_lost_cycles,
           (unsigned long long)cs.max_request,
           (unsigned long long)cs.max_residual,
           (unsigned long long)cs.clamp_hits_by_site[SNES_APU_SITE_PORT_READ],
           (unsigned long long)cs.clamp_hits_by_site[SNES_APU_SITE_PORT_WRITE],
           (unsigned long long)cs.clamp_hits_by_site[SNES_APU_SITE_BOUNDARY],
           (unsigned long long)cs.clamp_hits_by_site[SNES_APU_SITE_OTHER],
           (unsigned long long)cs.clamp_lost_by_site[SNES_APU_SITE_PORT_READ],
           (unsigned long long)cs.clamp_lost_by_site[SNES_APU_SITE_PORT_WRITE],
           (unsigned long long)cs.clamp_lost_by_site[SNES_APU_SITE_BOUNDARY],
           (unsigned long long)cs.clamp_lost_by_site[SNES_APU_SITE_OTHER]);

    if (g_audio_start.valid && g_audio_end.valid &&
        g_audio_end.periods > g_audio_start.periods) {
      uint64_t d_periods = g_audio_end.periods - g_audio_start.periods;
      uint64_t d_apu = g_audio_end.apu_cycles - g_audio_start.apu_cycles;
      uint64_t d_pcm = g_audio_end.pcm_frames - g_audio_start.pcm_frames;
      uint64_t d_master = g_audio_end.master - g_audio_start.master;
      uint64_t exp_apu = d_periods * GATE_APU_CYCLES_PER_PERIOD;
      uint64_t exp_pcm = d_periods * GATE_PCM_FRAMES_PER_PERIOD;
      printf("AUDIO6_INTERVAL from_epoch=%u to_epoch=%u periods=%llu "
             "master=%llu apu_cycles=%llu expected_apu=%llu "
             "pcm_frames=%llu expected_pcm=%llu "
             "apu_per_period=%.2f pcm_per_period=%.2f ratio=%.4f\n",
             g_audio_start.epoch, g_audio_end.epoch,
             (unsigned long long)d_periods, (unsigned long long)d_master,
             (unsigned long long)d_apu, (unsigned long long)exp_apu,
             (unsigned long long)d_pcm, (unsigned long long)exp_pcm,
             (double)d_apu / (double)d_periods,
             (double)d_pcm / (double)d_periods,
             exp_apu ? (double)d_apu / (double)exp_apu : 0.0);
      /* The master-cycle span of the interval must itself be the period
       * count times the period -- otherwise the denominator is wrong and
       * everything built on it is meaningless. */
      GATE_INVARIANT(d_master == d_periods * GATE_MASTER_PER_PERIOD,
                     "audio6_period_denominator", g_audio_end.epoch,
                     "master=%llu periods*period=%llu",
                     (unsigned long long)d_master,
                     (unsigned long long)(d_periods * GATE_MASTER_PER_PERIOD));
      if (!opt_audio6_advisory) {
        GATE_INVARIANT(d_apu == exp_apu, "AUDIO6_apu_cycles_per_period",
                       g_audio_end.epoch,
                       "observed=%llu expected=%llu periods=%llu",
                       (unsigned long long)d_apu, (unsigned long long)exp_apu,
                       (unsigned long long)d_periods);
        GATE_INVARIANT(d_pcm == exp_pcm, "AUDIO6_pcm_frames_per_period",
                       g_audio_end.epoch,
                       "observed=%llu expected=%llu periods=%llu",
                       (unsigned long long)d_pcm, (unsigned long long)exp_pcm,
                       (unsigned long long)d_periods);
        GATE_INVARIANT(cs.clamp_hits == 0, "AUDIO6_clamp_unreachable",
                       g_audio_end.epoch,
                       "clamp_hits=%llu lost_cycles=%llu",
                       (unsigned long long)cs.clamp_hits,
                       (unsigned long long)cs.clamp_lost_cycles);
        printf("AUDIO6 PASS apu_per_period=%u pcm_per_period=%u "
               "destructive_clamp_hits=0\n",
               GATE_APU_CYCLES_PER_PERIOD, GATE_PCM_FRAMES_PER_PERIOD);
      } else {
        printf("AUDIO6 ADVISORY (assertions suppressed by --audio6-advisory)\n");
      }
    }
  }

  printf("GLOBAL_INVARIANTS certified_boundaries=%llu recomp_violations=0 "
         "host_depth_violations=0 mx_mismatch=%llu abandon=%llu "
         "unsupported=%llu render_isolation_violations=%llu "
         "apuio_atomicity_violations=%llu hdma_nonzero=%llu "
         "beam_latch_reads=%llu manual_joypad_reads=%llu "
         "nonneutral_input=%llu\n",
         (unsigned long long)certified_boundaries,
         (unsigned long long)g_mx_mismatches,
         (unsigned long long)CpuUnresolvedAbandonTotal(),
         (unsigned long long)g_unsupported_mmio,
         (unsigned long long)g_render_violations,
         (unsigned long long)g_apu_word_interleave_events,
         (unsigned long long)g_hdma_nonzero_writes,
         (unsigned long long)g_beam_latch_reads,
         (unsigned long long)g_joy_manual_reads,
         (unsigned long long)g_nonneutral_input_events);
  printf("NEUTRAL_INPUT PASS joy1=0 joy2=0 auto_reads=%llu manual_reads=0 "
         "reads_per_nmi_entry=%.2f input_required_for_progress=NO\n",
         (unsigned long long)g_joy_auto_reads,
         g->nmis ? (double)g_joy_auto_reads / (double)g->nmis : 0.0);
  printf("GOOF_BOOT_E1000_GATE PASS epochs=%u checkpoints=%u/%u "
         "beam_reads=%llu mx_entries=%llu\n",
         opt.epochs, checkpoints_met, kCheckpointCount,
         (unsigned long long)g_beam_reads, (unsigned long long)g_mx_entries);
  printf("HVBJOY_READ_DIGEST_V1 count=%llu digest=%016llx\n",
         (unsigned long long)g_beam_reads,
         (unsigned long long)g_hvbjoy_digest);
  printf("AUTOJOY_TRACE_DIGEST_V1 rows=%llu digest=%016llx\n",
         (unsigned long long)g_autojoy_events,
         (unsigned long long)g_autojoy_digest);
  printf("AUTOJOY_BUSY_V1 reads=%llu busy_current=%llu busy_reference=%llu "
         "phase_min=%llu phase_max=%llu phase_mean=%llu "
         "spin_iterations_current=%llu pred_extra_iterations=%llu "
         "pred_extra_master=%llu pred_iterations_max=%llu "
         "ref_busy_window=%u spin_master_per_iter=%u\n",
         (unsigned long long)g_autojoy_busy_reads,
         (unsigned long long)g_autojoy_busy_set_current,
         (unsigned long long)g_autojoy_busy_set_ref,
         (unsigned long long)(g_autojoy_busy_reads ? g_autojoy_phase_min : 0),
         (unsigned long long)g_autojoy_phase_max,
         (unsigned long long)(g_autojoy_busy_reads
                              ? g_autojoy_phase_sum / g_autojoy_busy_reads : 0),
         (unsigned long long)g_autojoy_spin_observed,
         (unsigned long long)g_autojoy_pred_iters,
         (unsigned long long)g_autojoy_pred_master,
         (unsigned long long)g_autojoy_pred_iters_max,
         AUTOJOY_REF_BUSY_MASTER, AUTOJOY_SPIN_MASTER_PER_ITER);
  printf("AUTOJOY_ENABLE_V1 nmitimen_writes_autojoy_on=%llu "
         "nmitimen_writes_autojoy_off=%llu reads_with_autojoy_disabled=%llu "
         "timer_nonzero_at_read=%llu nmi_edges=%llu "
         "nmi_edges_off_phase_zero=%llu result_reads=%llu "
         "result_reads_inside_ref_busy=%llu\n",
         (unsigned long long)g_autojoy_enable_writes,
         (unsigned long long)g_autojoy_disable_writes,
         (unsigned long long)g_autojoy_disabled_at_read,
         (unsigned long long)g_autojoy_timer_nonzero,
         (unsigned long long)g_autojoy_nmi_edges,
         (unsigned long long)g_autojoy_nmi_edge_phase_nonzero,
         (unsigned long long)g_autojoy_result_reads,
         (unsigned long long)g_autojoy_result_before_ref_clear);
  printf("AUTOJOY_EDGES_V1 nmi_edges=%llu edges_with_autojoy_enabled=%llu\n",
         (unsigned long long)g_autojoy_nmi_edges,
         (unsigned long long)g_autojoy_edges_autojoy_on);
  printf("AUTOJOY_HW_PHASE_V1 first_reads=%llu cpu_since_edge_min=%llu "
         "dma_bytes_min=%llu dma_bytes_max=%llu hw_phase_lb_min=%llu "
         "hw_phase_ub_max=%llu hw_busy_clear_max=%u hw_busy_possible=%llu\n",
         (unsigned long long)g_autojoy_first_read_in_nmi,
         (unsigned long long)g_autojoy_cpu_min,
         (unsigned long long)g_autojoy_dma_bytes_min,
         (unsigned long long)g_autojoy_dma_bytes_max,
         (unsigned long long)g_autojoy_hw_lb_min,
         (unsigned long long)g_autojoy_hw_ub_max,
         AUTOJOY_HW_CLEAR_MAX,
         (unsigned long long)g_autojoy_hw_busy_possible);
  general_dma_report();
  cpu_bus_timing_report(g);
  free(rom);
  free(opt.dump);
  return 0;
}
