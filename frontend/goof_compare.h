#ifndef GOOF_COMPARE_H
#define GOOF_COMPARE_H

/* Headless comparison of the application coordinator against the permanent
 * GOOF_BOOT_E1000_GATE checkpoints.  SDL-FREE.
 *
 * This module drives the guest ONLY through goof_app_step / goof_app_render.
 * It is the --headless-compare mode of goof_recomp and the whole body of the
 * always-built, SDL-free goof_app_headless target, so the comparison path and
 * the interactive player provably execute the same compiled coordinator
 * (gate V10). */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "goof_app.h"
#include "goof_input.h"

/* One deterministic record line: guest state only, no host timing, no
 * pointer, no timestamp.  Two runs of the same epochs must emit byte-
 * identical streams regardless of presentation rate (gates V6, V7, V8). */
void goof_compare_write_record(FILE *out, const GoofAppDiag *diag);

/* Runs `epochs` logical epochs through goof_app with NEUTRAL guest input,
 * rendering each certified boundary, and fails closed on any promoted
 * checkpoint mismatch.  `record` may be NULL.  Returns 0 on PASS, 1 on FAIL. */
int goof_compare_run(GoofApp *app, uint64_t epochs, bool quiet, FILE *record);

/* As above, presenting `script`'s sample for each epoch through the same
 * goof_app_step parameter the live SDL path uses.  `script` may be NULL,
 * which is exactly goof_compare_run.
 *
 * NOTE: the promoted checkpoints describe the NEUTRAL campaign.  A non-neutral
 * script changes guest execution by design, so a checkpoint mismatch under one
 * is the expected result and not a regression -- which is why the scripted
 * gates below assert record equality between runs rather than equality with
 * the neutral contract. */
int goof_compare_run_scripted(GoofApp *app, uint64_t epochs, bool quiet,
                              FILE *record, const GoofInputScript *script);

/* Report the APU-side checkpoint fields (spc_pc, spc_cycles, apu_ram) instead
 * of asserting them.  GOOF_APU_FRAME_RATE_FIDELITY moves exactly those three
 * fields, by design, and the audio-side pins are promoted only after every
 * other gate is green -- so this exists to let the CPU/PPU comparison be
 * certified in full during that window.  It is never an acceptance mode: the
 * logical and framebuffer anchors stay asserted regardless. */
void goof_compare_set_audio_pins_advisory(bool advisory);
/* Checkpoint discovery (the gate's --discover): every neutral checkpoint is
 * REPORTED, not asserted, so a chronology change can be recorded and compared
 * across hosts/rates before a rebaseline.  Records are unaffected. */
void goof_compare_set_discover(bool discover);

/* Highest epoch carrying a promoted checkpoint (1000). */
uint64_t goof_compare_last_checkpoint_epoch(void);

#endif
