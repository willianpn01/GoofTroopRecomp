/* Headless comparison against the permanent GOOF_BOOT_E1000_GATE contract.
 * SDL-free; see goof_compare.h. */
#include "goof_compare.h"

#include <inttypes.h>
#include <string.h>

enum {
  CMP_LOGICAL = 1u << 0,
  CMP_FB      = 1u << 1,
  CMP_APU     = 1u << 2,
};

typedef struct {
  uint64_t epoch;
  const char *milestone;
  uint64_t logical;
  uint64_t framebuffer;
  uint32_t spc_pc;
  uint64_t spc_cycles;
  uint64_t apu_ram;
  unsigned flags;
} CompareCheckpoint;

/* Every value below is copied from the promoted contract in
 * phase4_boot/goof_boot_e1000_gate.c.  This table is a consumer of that
 * contract, never a second source of truth for it: a divergence here is a
 * failure of this file, and V5 is what detects it. */
static const CompareCheckpoint kCheckpoints[] = {
  {    1, "reset boundary / loader epoch: 121 physical frame periods",
       UINT64_C(0x2e4caf44f19d0392), UINT64_C(0x2fe663de2c1a1c25),
       0xffcf, 2067648, UINT64_C(0x2e44d3c25b026800),
       CMP_LOGICAL | CMP_FB | CMP_APU },
  {   27, "credits presentation ends",
       UINT64_C(0x17a020c916d79a7b), UINT64_C(0x2fe663de2c1a1c25), 0, 0, 0,
       CMP_LOGICAL | CMP_FB },
  {   42, "fade-out complete; last frame before the SPC IPL upload",
       UINT64_C(0x8966bba58cc2f800), UINT64_C(0x06644c0aa7470383), 0, 0, 0,
       CMP_LOGICAL | CMP_FB },
  {   43, "deterministic SPC IPL upload completes",
       UINT64_C(0x98454f0976ddab1a), 0, 0, 0, 0, CMP_LOGICAL },
  {   65, "established post-APU anchor; SPC driver running",
       UINT64_C(0xca59d7ce793383a4), 0,
       0x0300, 3930240, UINT64_C(0x0172921e63b53fc3), CMP_LOGICAL | CMP_APU },
  {   90, "forced blank released; first post-APU visual output",
       UINT64_C(0xfa6401deb979dc4e), 0, 0, 0, 0, CMP_LOGICAL },
  {  244, "CAPCOM logo, stable plateau E185..E304",
       UINT64_C(0x39f6206ed133692f), UINT64_C(0xdbff93edb06b69bd), 0, 0, 0,
       CMP_LOGICAL | CMP_FB },
  {  522, "title screen with menu, stable plateau E428..E616",
       UINT64_C(0x7b931c0b307ef255), UINT64_C(0x6793cde5aa7b786e), 0, 0, 0,
       CMP_LOGICAL | CMP_FB },
  {  701, "scene transition: slot1 first runnable, stage loading",
       UINT64_C(0x04f0685ab43c6e03), UINT64_C(0x06644c0aa7470383), 0, 0, 0,
       CMP_LOGICAL | CMP_FB },
  {  721, "GAMEPLAY_LIKE boundary: OBJ on main screen",
       UINT64_C(0x6faaf73ba74decf2), 0, 0, 0, 0, CMP_LOGICAL },
  {  903, "GAMEPLAY_LIKE plateau E874..E933",
       UINT64_C(0xddf73e13801e8cae), UINT64_C(0x3d8b38d15850ca04), 0, 0, 0,
       CMP_LOGICAL | CMP_FB },
  { 1000, "long-run endpoint, GAMEPLAY_LIKE",
       UINT64_C(0x039d1c22980acac9), UINT64_C(0x7ca1b37acb690109),
       0x038a, 20505600, UINT64_C(0x2c9a2d8c25bb8256),
       CMP_LOGICAL | CMP_FB | CMP_APU },
};
enum { kCheckpointCount = sizeof(kCheckpoints) / sizeof(kCheckpoints[0]) };

uint64_t goof_compare_last_checkpoint_epoch(void) {
  return kCheckpoints[kCheckpointCount - 1].epoch;
}

void goof_compare_write_record(FILE *out, const GoofAppDiag *d) {
  if (!out || !d) return;
  /* The physical-frame columns are guest quantities, derived only from
   * g_cpu.master_cycles.  Writing them into the record is what turns the
   * 30 Hz / 120 Hz comparison from "the same hashes" into "the same NUMBER
   * OF EMULATED FRAMES and the same number of NMIs", which is the property
   * a host-rate-independent guest clock actually has to have. */
  fprintf(out,
          "epoch=%" PRIu64 " logical=%016" PRIx64 " fb=%016" PRIx64
          " spc_pc=%04X spc_cycles=%" PRIu64 " apu_ram=%016" PRIx64
          " periods=%" PRIu64 " requests=%" PRIu64 " entries=%" PRIu64
          " padding=%" PRIu64 " p1=%03X p2=%03X latches=%" PRIu64
          " input=%016" PRIx64 " g1h=%04X g1p=%04X g2h=%04X g2p=%04X"
          " px=%06X py=%06X hands=%02X item=%02X"
          " irq=%" PRIu64 "/%" PRIu64 "/%" PRIu64
          " irq_digest=%016" PRIx64 " journal=%" PRIu64 "/%u"
          " journal_digest=%016" PRIx64 "\n",
          d->epoch, d->logical_hash, d->framebuffer_hash, d->spc_pc,
          d->spc_cycles, d->apu_ram_hash, d->physical_periods,
          d->nmi_requests, d->nmi_entries, d->idle_padding_master_cycles,
          d->input_p1, d->input_p2, d->input_latches, d->input_digest,
          d->guest_p1_held, d->guest_p1_pressed, d->guest_p2_held,
          d->guest_p2_pressed, d->p1_x, d->p1_y, d->p1_hands_up,
          d->p1_held_item, d->irq_compares, d->irq_entries, d->irq_acks,
          d->irq_digest, d->journal_writes, d->journal_count,
          d->journal_digest);
}

static const CompareCheckpoint *checkpoint_for(uint64_t epoch) {
  for (unsigned i = 0; i < kCheckpointCount; i++)
    if (kCheckpoints[i].epoch == epoch) return &kCheckpoints[i];
  return NULL;
}

static int fail(const CompareCheckpoint *c, const char *field,
                uint64_t expected, uint64_t observed) {
  printf("GOOF_HEADLESS_COMPARE FAIL epoch=%" PRIu64 " checkpoint=\"%s\" "
         "field=%s expected=%016" PRIx64 " observed=%016" PRIx64 "\n",
         c->epoch, c->milestone, field, expected, observed);
  fflush(stdout);
  return 1;
}

static bool s_audio_pins_advisory;

void goof_compare_set_audio_pins_advisory(bool advisory) {
  s_audio_pins_advisory = advisory;
}

static bool s_discover;

void goof_compare_set_discover(bool discover) {
  s_discover = discover;
}

int goof_compare_run(GoofApp *app, uint64_t epochs, bool quiet, FILE *record) {
  return goof_compare_run_scripted(app, epochs, quiet, record, NULL);
}

int goof_compare_run_scripted(GoofApp *app, uint64_t epochs, bool quiet,
                              FILE *record, const GoofInputScript *script) {
  if (!app) return 1;
  unsigned met = 0, logical_met = 0, fb_met = 0;
  /* "Non-neutral" is a property of the SCRIPT, decided once, not of the epoch
   * reached: it must not flip mid-run, or the oracle would change underneath
   * the campaign. */
  bool nonneutral = false;
  if (script)
    for (size_t i = 0; i < script->count; i++)
      if (!goof_input_is_neutral(&script->entries[i].sample)) nonneutral = true;
  if (script)
    printf("GOOF_INPUT_SCRIPT entries=%zu script_digest=%016" PRIx64
           " nonneutral=%s\n", script->count, script->digest,
           nonneutral ? "yes" : "no");
  for (uint64_t epoch = 1; epoch <= epochs; epoch++) {
    GoofAppDiag diag;
    /* The scripted path presents its sample through the SAME goof_app_step
     * parameter the live SDL path uses.  There is deliberately no second,
     * test-only route into the guest: a script that could write RAM directly
     * would prove nothing about the path a player exercises. */
    GoofInputSample in = goof_input_script_sample(script, epoch);
    GoofAppStatus s = goof_app_step(app, &in, &diag);
    if (s != GOOF_APP_OK) {
      printf("GOOF_HEADLESS_COMPARE FAIL epoch=%" PRIu64 " stage=step "
             "status=%s detail=%s\n", epoch, goof_app_status_text(s),
             goof_app_last_error(app));
      return 1;
    }
    s = goof_app_render(app, &diag);
    if (s != GOOF_APP_OK) {
      printf("GOOF_HEADLESS_COMPARE FAIL epoch=%" PRIu64 " stage=render "
             "status=%s detail=%s\n", epoch, goof_app_status_text(s),
             goof_app_last_error(app));
      return 1;
    }
    if (record) goof_compare_write_record(record, &diag);

    const CompareCheckpoint *c = checkpoint_for(epoch);
    if (!c) continue;
    /* A non-neutral script CHANGES guest execution -- that is the whole point
     * of it -- so the neutral checkpoint table stops being an oracle from the
     * first epoch the guest actually responds to input.  Report the anchor
     * instead of asserting it: the divergence is evidence the guest reacted,
     * and the scripted gates get their determinism from record equality
     * between runs.  Neutral runs (script == NULL, or an all-neutral script)
     * still take the assert path below and stay fail-closed. */
    if (nonneutral || s_discover) {
      if (!quiet)
        printf("COMPARE_SCRIPTED epoch=%" PRIu64 " logical=%016" PRIx64
               " fb=%016" PRIx64 " p1=%03X p2=%03X neutral_logical=%016" PRIx64
               " neutral_fb=%016" PRIx64 " diverged=%s milestone=\"%s\"\n",
               diag.epoch, diag.logical_hash, diag.framebuffer_hash,
               diag.input_p1, diag.input_p2, c->logical, c->framebuffer,
               diag.logical_hash != c->logical ? "yes" : "no", c->milestone);
      met++;
      continue;
    }
    if ((c->flags & CMP_LOGICAL) && diag.logical_hash != c->logical)
      return fail(c, "logical", c->logical, diag.logical_hash);
    if (c->flags & CMP_LOGICAL) logical_met++;
    if (c->flags & CMP_FB) {
      if (diag.framebuffer_hash != c->framebuffer)
        return fail(c, "framebuffer", c->framebuffer, diag.framebuffer_hash);
      fb_met++;
    }
    if (c->flags & CMP_APU) {
      if (s_audio_pins_advisory) {
        printf("COMPARE_AUDIO_PINS epoch=%" PRIu64 " spc_pc=%04X spc_cycles=%"
               PRIu64 " apu_ram=%016" PRIx64 " (pinned: spc_pc=%04X "
               "spc_cycles=%" PRIu64 " apu_ram=%016" PRIx64 ")\n",
               diag.epoch, diag.spc_pc, diag.spc_cycles, diag.apu_ram_hash,
               c->spc_pc, c->spc_cycles, c->apu_ram);
      } else {
        if (diag.spc_pc != c->spc_pc)
          return fail(c, "spc_pc", c->spc_pc, diag.spc_pc);
        if (diag.spc_cycles != c->spc_cycles)
          return fail(c, "spc_cycles", c->spc_cycles, diag.spc_cycles);
        if (diag.apu_ram_hash != c->apu_ram)
          return fail(c, "apu_ram", c->apu_ram, diag.apu_ram_hash);
      }
    }
    met++;
    if (!quiet)
      printf("COMPARE_CHECKPOINT epoch=%" PRIu64 " logical=%016" PRIx64
             " fb=%016" PRIx64 " PASS milestone=\"%s\"\n",
             diag.epoch, diag.logical_hash, diag.framebuffer_hash,
             c->milestone);
  }

  /* GOOF_INPUT_DIGEST_V1 for the whole campaign, printed unconditionally so
   * the neutral baseline and every scripted scenario have one pinnable line. */
  { GoofAppDiag last; goof_app_diagnostics(app, &last);
    printf("GOOF_INPUT_DIGEST epochs=%" PRIu64 " latches=%" PRIu64
           " digest=%016" PRIx64 " nonneutral_script=%s\n", epochs,
           last.input_latches, last.input_digest, nonneutral ? "yes" : "no"); }

  uint64_t expected_last = goof_compare_last_checkpoint_epoch();
  if (nonneutral || s_discover) {
    printf("GOOF_HEADLESS_COMPARE %s epochs=%" PRIu64 " anchors_seen=%u"
           " (neutral checkpoint table reported, not asserted)\n",
           nonneutral ? "SCRIPTED" : "DISCOVER", epochs, met);
    fflush(stdout);
    return 0;
  }
  if (epochs >= expected_last && met != kCheckpointCount) {
    printf("GOOF_HEADLESS_COMPARE FAIL checkpoints_met=%u expected=%u\n",
           met, kCheckpointCount);
    return 1;
  }
  printf("GOOF_HEADLESS_COMPARE PASS epochs=%" PRIu64 " checkpoints=%u "
         "logical_anchors=%u framebuffer_anchors=%u\n",
         epochs, met, logical_met, fb_met);
  fflush(stdout);
  return 0;
}
