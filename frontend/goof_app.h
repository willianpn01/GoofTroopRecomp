#ifndef GOOF_APP_H
#define GOOF_APP_H

/* Goof Troop Recomp -- application coordinator.
 *
 * This is the ONE body of the guest stepping loop.  The interactive SDL
 * player and the headless comparison path both drive the guest exclusively
 * through this module; neither contains a second loop.  See
 * interactive_presentation_frontend_design.md section 6 (Model C) and
 * gate V10.
 *
 * THIS MODULE IS SDL-FREE, PLATFORM-FREE AND HOST-CLOCK-FREE.
 * It must never include SDL, never read a wall clock, never see a keyboard
 * event and never learn that a window exists.  Guest input (GOOF_INPUT_V1)
 * does not weaken that: what crosses this boundary is a pair of 12-bit
 * button masks in the engine's own bit order, which is guest currency, not
 * host currency -- see goof_input.h.  goof_core.cmake enforces the
 * SDL half of that mechanically (gate V11); the rest is enforced by this
 * header's public surface, which has no way to express host time.
 *
 * Terminology, normative (design section 7): this module advances a LOGICAL
 * EPOCH -- a certified NMI boundary of guest execution.  A logical epoch is
 * NOT a physical SNES frame, and nothing here may be named "frame".
 */

#include <stdbool.h>
#include <stdint.h>

#include "goof_input.h"

typedef struct GoofApp GoofApp;

typedef enum {
  GOOF_APP_OK = 0,
  GOOF_APP_ERR_ARG,     /* null/invalid argument                           */
  GOOF_APP_ERR_ROM,     /* ROM missing, wrong size, or identity mismatch   */
  GOOF_APP_ERR_INIT,    /* SnesInit / adapter init / reset prefix failed   */
  GOOF_APP_ERR_STEP,    /* epoch did not end on a certified boundary       */
  GOOF_APP_ERR_RENDER   /* 256x224 render contract violated                */
} GoofAppStatus;

typedef struct {
  /* Path to the external ROM.  Borrowed; must outlive goof_app_create.
   * A valid 512-byte SMC copier header is normalised away before the
   * canonical identity is verified.  Verification itself is never relaxed. */
  const char *rom_path;
} GoofAppConfig;

/* Read-only diagnostics for one logical epoch.  Every field below is guest
 * state or a digest of guest state: none of it depends on host timing, so
 * two runs at different presentation rates must produce identical records. */
typedef struct {
  uint64_t epoch;             /* logical epochs completed since reset      */
  uint64_t logical_hash;      /* GOOF_LOGICAL_EPOCH_HASH_V1                */
  uint64_t framebuffer_hash;  /* last render; 0 before the first render    */
  uint32_t spc_pc;
  uint64_t spc_cycles;
  uint64_t apu_ram_hash;

  /* Physical-frame accounting for the epoch that just completed.  These are
   * DIAGNOSTICS: the frontend reports them and paces on none of them.
   *
   * Rules, still normative:
   *   - the number of epochs advanced per presentation tick is a constant
   *     and is never a function of these values or of host time (R1);
   *   - they do NOT redefine the logical epoch, which remains a certified
   *     semantic boundary;
   *   - they are guest quantities, derived from g_cpu.master_cycles only,
   *     so they are identical at 30 Hz, at 120 Hz and headless. */
  uint64_t physical_periods;           /* frame boundaries the epoch crossed */
  uint64_t nmi_requests;               /* edges offered to the guest         */
  uint64_t nmi_entries;                /* NMI handlers actually entered      */
  uint64_t idle_padding_master_cycles; /* deterministic pad at quiescence    */
  uint64_t irq_compares, irq_entries, irq_acks, irq_digest;
  uint64_t journal_writes, journal_digest;
  unsigned journal_count;

  /* Measured physical duration of the epoch in nominal SNES frame periods.
   * Formerly reserved and always 0; it is now really measured, and equals
   * physical_periods.  0 still means "not measured", never "zero duration". */
  double epoch_duration_frame_periods;

  /* GOOF_INPUT_V1 -- guest input, reported as guest state.
   *
   * `input_p1`/`input_p2` are the epoch's IMMUTABLE guest-visible sample
   * after normalisation: exactly what the auto-joypad result registers held
   * at every latch inside this epoch.  `input_latches` counts guest-visible
   * latches since reset and `input_digest` is GOOF_INPUT_DIGEST_V1 over
   * (ordinal, guest master time, p1, p2) for all of them.
   *
   * These are host-rate-independent for the same reason the columns above
   * are: the digest's timestamp is g_cpu.master_cycles, never a wall clock,
   * so 30 Hz, 60 Hz, 120 Hz and headless must produce the same digest for
   * the same script. */
  uint16_t input_p1, input_p2;
  uint64_t input_latches;
  uint64_t input_digest;

  /* GUEST-SIDE OBSERVATION of the same input, read out of WRAM at the
   * certified boundary.  These are NOT what the host presented -- they are
   * what Goof Troop's own NMI input routine at $00:830B..$0850 stored after
   * reading $4218/$421A -- so they are the evidence that the guest actually
   * consumed the sample, as opposed to "a register changed".
   *
   * Addresses come from the disassembly's own layout (bank $83 direct page):
   *   $0044 P1 held   $0048 P1 newly-pressed   (EOR $46 / AND $44 at $833D)
   *   $004A P2 held   $004E P2 newly-pressed
   * Byte order is the game's, not the register's: it applies XBA, so bit 0 is
   * Right and bit 7 is B.  Reported raw, unreinterpreted.
   *
   * `p1_x` / `p1_y` are Player 1's 24-bit sub-pixel position from
   * !RAM_GOOFT_Player1_SubXPos ($83:0110) and SubYPos ($83:0113): the
   * gameplay consequence of a direction, not another copy of the button. */
  uint16_t guest_p1_held, guest_p1_pressed;
  uint16_t guest_p2_held, guest_p2_pressed;
  uint32_t p1_x, p1_y;

  /* Player 1 action state, also straight from the disassembly's RAM map:
   *   !RAM_GOOFT_Player1_HandsUpFlag  $83:0103
   *   !RAM_GOOFT_Player1_HeldItem     $83:0142
   * These name the gameplay meaning of a face button in the ROM's own terms,
   * which is why INPUT4 can assert an ACTION rather than a hash change. */
  uint8_t p1_hands_up, p1_held_item;
} GoofAppDiag;

/* Loads and verifies the ROM, initialises the engine, runs the architectural
 * reset prefix and takes ownership of the epoch adapter and the isolated
 * renderer.  Returns NULL on failure, with *status set and nothing leaked.
 *
 * Only one GoofApp may exist per process: the engine state it drives
 * (g_cpu, g_snes, g_ppu, g_ram) is global and is not re-entrant. */
GoofApp *goof_app_create(const GoofAppConfig *cfg, GoofAppStatus *status);

/* Advances EXACTLY ONE logical epoch and returns OK only if the epoch ended
 * on a certified boundary (EPOCH_COMPLETE_WAITING && certified).
 *
 * GOOF_INPUT_V1.  `input` is the guest-visible joypad sample for THIS epoch
 * and is THE ONLY channel by which host state reaches the guest.  Passing
 * NULL is exactly neutral (joy1 = joy2 = 0) and reproduces the canonical
 * pre-input baseline bit for bit.
 *
 * What arrives here is a pair of 12-bit button masks and nothing else: no
 * key code, no SDL event, no host timestamp, no wall clock.  The sample is
 * normalised once inside this call and then becomes the epoch's FramePlan
 * input, immutable for the whole epoch -- the driver latches it at plan
 * start and again at every internal physical-boundary NMI edge.  Host
 * presentation cadence therefore cannot change WHEN the guest sees input.
 *
 * `diag` is optional and is filled only on OK. */
GoofAppStatus goof_app_step(GoofApp *app, const GoofInputSample *input,
                            GoofAppDiag *diag);

/* Renders the visual state of the last certified boundary onto the renderer's
 * isolated PPU snapshot.  The live Ppu is only read.  Call after a step that
 * returned OK.  `diag` is optional and is filled only on OK. */
GoofAppStatus goof_app_render(GoofApp *app, GoofAppDiag *diag);

/* Framebuffer of the last successful render.  Owned by the app, valid until
 * the next render or destroy.  uint32 0x00RRGGBB, 256 x 224, pitch 1024.
 * The caller NEVER writes through this pointer. */
const uint32_t *goof_app_framebuffer(const GoofApp *app, uint32_t *width,
                                     uint32_t *height, uint32_t *pitch);

/* Current diagnostics without stepping. */
void goof_app_diagnostics(const GoofApp *app, GoofAppDiag *out);

/* Number of certified logical epochs completed so far. */
uint64_t goof_app_epoch(const GoofApp *app);

/* Human-readable detail for the last non-OK status, or "" when there is
 * none.  Carries the guest-side diagnosis (epoch, stop reason, PC) that the
 * gates use, so a frontend failure is never aggregated into "it broke". */
const char *goof_app_last_error(const GoofApp *app);

const char *goof_app_status_text(GoofAppStatus status);

/* ---- Deterministic native-PCM drain -------------------------------------
 *
 * THE HOST PRESENTATION SEAM.  Everything above this block advances guest
 * time; everything reachable through this block only OBSERVES PCM the guest
 * has already produced.  The three functions are a thin, SDL-free cover over
 * the engine's dsp_available / dsp_peek / dsp_advance -- the consumer-only
 * API backported by GOOF_APU_FRAME_RATE_FIDELITY -- so that main_sdl.c can
 * feed an audio device without ever reaching into engine internals.
 *
 * INVARIANT GOOF-AUDIO-1.  None of these runs the SPC, calls apu_cycle, or
 * touches g_cpu.master_cycles.  Host audio never advances guest time.
 * INVARIANT GOOF-AUDIO-2.  None of these blocks, sleeps or waits.  Host
 * audio never blocks guest execution.
 *
 * The samples and the production cursor (Dsp.sampleWrite) are GUEST state.
 * The consume cursor (Dsp.sampleRead) is HOST state and must never enter a
 * determinism digest -- which is why draining changes no gated quantity:
 * the canonical AUDIO1 digest is taken in dsp_cycle BEFORE the output ring's
 * overflow check, upstream of anything a consumer can influence.
 *
 * Format, fixed by the DSP and asserted by goof_app_audio_native_rate():
 * interleaved stereo int16 L,R at 32040 Hz, 534 frames per physical period. */

/* Native stereo frames the guest has produced and the host has not yet
 * retired.  Bounded by the engine's 8192-frame output ring. */
uint32_t goof_app_audio_available(const GoofApp *app);

/* Copies up to `max_frames` interleaved (L,R) int16 pairs into `out` and
 * retires exactly that many from the ring.  Returns the frames copied.
 * `out` must hold 2 * max_frames int16.  Observes only. */
uint32_t goof_app_audio_drain(GoofApp *app, int16_t *out, uint32_t max_frames);

/* Retires `frames` without copying them: the host-only trim used to keep a
 * late consumer from pinning the ring full.  Clamped to what is available.
 * Discarding here is a PRESENTATION decision and changes no guest state --
 * the discarded frames were already counted by the canonical digest. */
void goof_app_audio_discard(GoofApp *app, uint32_t frames);

/* The DSP's native output rate in Hz.  A constant of the guest hardware
 * model (1.024 MHz / 32), not a host or device property. */
uint32_t goof_app_audio_native_rate(void);

/* Native stereo frames one physical frame boundary produces: 17088 SPC
 * cycles / 32.  The producer's quantum, never the consumer's. */
uint32_t goof_app_audio_frames_per_period(void);

/* ---- Physical-boundary host presentation hook -------------------------
 *
 * GOOF_AUDIO_PHYSICAL_BOUNDARY_PUMP_V1.  A logical epoch is a certified
 * semantic boundary and may span many PHYSICAL frame periods -- 1 in the
 * steady state, but 16 at a scene transition and 143 at the loader epoch.
 * Each physical period produces 534 native PCM frames.  A host that services
 * audio only when goof_app_step RETURNS therefore receives up to 2.4 s of
 * already-produced music in one call, which is a presentation problem and
 * not a guest one (goof_audio_transition_pacing_investigation.md).
 *
 * This hook is the seam that removes it: `fn` is invoked ONCE PER PHYSICAL
 * BOUNDARY from inside goof_app_step, immediately after that boundary's
 * APU/DSP time has been pulled, so the boundary's PCM is already in the ring
 * and reachable through goof_app_audio_available / _drain / _discard.
 *
 * WHAT AN IMPLEMENTATION MAY DO: drain and retire already-produced native
 * PCM, and update its own host-side bookkeeping.
 * WHAT IT MAY NOT DO: advance the guest, the SPC, the DSP or master_cycles;
 * block, sleep or wait on a device; read a wall clock to decide anything
 * about guest progress; or touch guest state.  INVARIANTS GOOF-AUDIO-1 and
 * GOOF-AUDIO-2 above apply to it verbatim and unmodified.
 *
 * It is host wiring, not guest state: nothing about it enters a digest, the
 * driver reads no return value, and with no hook installed (the default)
 * behaviour is bit-identical -- which is what keeps headless, --no-audio and
 * every permanent gate unchanged.  Pass fn == NULL to remove it.
 *
 * This header stays SDL-free: `user` is an opaque host pointer and the
 * callback carries no host currency of any kind. */
typedef void (*GoofAppBoundaryHook)(void *user);
void goof_app_set_boundary_hook(GoofApp *app, GoofAppBoundaryHook fn,
                                void *user);

/* M/X entry-gate accounting for this process.  With SNESRECOMP_MX_ENTRY_GATE
 * on -- the same definition the permanent gates compile the generated banks
 * with -- every generated function entry reports its M/X state, and this
 * module checks it against the _M<m>X<x> suffix exactly as the permanent M/X
 * verifier does.  `mismatches` must stay 0. */
void goof_app_mx_stats(uint64_t *entries, uint64_t *mismatches);

/* Releases everything the app owns.  Idempotent; NULL-safe. */
void goof_app_destroy(GoofApp *app);

#endif
