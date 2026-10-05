#include "goof_run_frame_adapter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GoofRunFrameAdapter *s_bound_adapter;

bool goof_run_frame_adapter_init(GoofRunFrameAdapter *a,
                                 const Interp816 *reset_snapshot,
                                 uint16_t scheduler_s,
                                 const InterpAotEntryDescriptor *entries,
                                 unsigned entry_count, uint8_t *ram,
                                 GoofHardwareEvent hardware,
                                 uint64_t reset_budget) {
  if (!a || !reset_snapshot || !entries || !entry_count || !ram) return false;
  memset(a, 0, sizeof(*a));
  /* The validated cursor contract starts in native mode.  Execute only the
   * architectural reset prefix here, then transfer ownership of the exact
   * live snapshot into the one persistent cursor.  This is a one-time move,
   * not a second persistent executor and not a reset hidden in run_frame. */
  Interp816 native = *reset_snapshot;
  uint64_t prefix_steps = 0;
  while (native.e && prefix_steps < reset_budget) {
    if (native.irqWanted || native.nmiWanted || native.waiting || native.stopped ||
        native.read(native.mem, ((uint32_t)native.k << 16) | native.pc) == 0x00)
      return false;
    interp816_runOpcode(&native);
    prefix_steps++;
  }
  if (native.e || prefix_steps == reset_budget) return false;
  if (!goof_execution_init(&a->execution, &native, scheduler_s) ||
      !goof_execution_enable_aot(&a->execution, entries, entry_count, ram) ||
      !goof_execution_set_aot_kinds(&a->execution,
          (1u << INTERP_AOT_ENTRY_FUNCTION) |
          (1u << INTERP_AOT_ENTRY_CONTINUATION))) return false;
  /* The bootstrap runs the physical clock too.  Boundary k=1 falls inside it
   * while NMITIMEN.7 is still 0, so it is counted and its NMI dropped; at the
   * bootstrap's certified quiescence the idle-padding rule advances guest
   * master time to boundary k=2.  That is what makes E1 -- and, because every
   * epoch ends padded, every later epoch -- open exactly ON a boundary. */
  FramePlan reset = {.id = 1, .interval = DRAIN_TO_WAIT,
                     .nmi_enabled = true, .physical_boundaries = true};
  a->hardware = hardware;
  a->last_result = goof_run_frame(&a->execution, &reset,
                                  (SafetyBudget){reset_budget - prefix_steps});
  if (a->last_result.reason != WAITING || !a->last_result.certified) return false;
  a->next_plan_id = 2;
  a->ready = true;
  return true;
}

RunResult goof_run_frame_adapter_advance(GoofRunFrameAdapter *a,
                                        uint64_t budget) {
  return goof_run_frame_adapter_advance_input(a, budget, 0, 0);
}

RunResult goof_run_frame_adapter_advance_input(GoofRunFrameAdapter *a,
                                              uint64_t budget,
                                              uint16_t joy1, uint16_t joy2) {
  if (!a || !a->ready)
    return (RunResult){.reason = INVALID_STATE};
  /* One logical epoch : N physical frame boundaries, N >= 1.  The epoch is
   * still the scheduler/quiescence semantic boundary and the unit of
   * certification, hashing and rendering; what changed is that interrupt
   * opportunity is now a consequence of guest master time rather than of a
   * plan-chosen moment.  request_nmi stays false here: it is retained in the
   * plan only to reproduce the legacy single-NMI baseline in diagnostics. */
  FramePlan epoch = {.id = a->next_plan_id++,
                     .interval = LOGICAL_NMI_EPOCH,
                     .physical_boundaries = true, .nmi_enabled = true,
                     /* GOOF_INPUT_V1: the epoch's immutable guest-visible
                      * joypad sample.  Masked to 12 bits here as well as at
                      * the frontend seam, because bits 12..15 map onto the
                      * $4218 controller-type nibble and Goof discards the
                      * whole pad when that nibble is nonzero ($00:8318). */
                     .joy1 = (uint16_t)(joy1 & 0x0fffu),
                     .joy2 = (uint16_t)(joy2 & 0x0fffu),
                     .hardware = a->hardware};
  a->callbacks++;
  a->last_result = goof_run_frame(&a->execution, &epoch,
                                  (SafetyBudget){budget});
  if (a->last_result.reason == EPOCH_COMPLETE_WAITING)
    a->logical_nmi_epochs++;
  else
    a->ready = false;
  return a->last_result;
}

void goof_run_frame_adapter_set_boundary_notify(GoofRunFrameAdapter *a,
                                                GoofBoundaryNotify fn,
                                                void *user) {
  if (!a) return;
  goof_execution_set_boundary_notify(&a->execution, fn, user);
}

void goof_run_frame_adapter_bind(GoofRunFrameAdapter *a) {
  s_bound_adapter = a;
}

void goof_boot_harness_run_frame(void) {
  RunResult result = s_bound_adapter ?
      goof_run_frame_adapter_advance(s_bound_adapter, 20000000) :
      (RunResult){.reason = INVALID_STATE};
  if (result.reason != EPOCH_COMPLETE_WAITING) {
    fprintf(stderr, "[phase4] logical run_frame failed reason=%d pc=%06X "
                    "consumed=%llu nmi=%llu irq=%u wai=%u stp=%u\n",
            result.reason, result.pc24, (unsigned long long)result.consumed,
            s_bound_adapter ? (unsigned long long)s_bound_adapter->execution.nmis : 0,
            s_bound_adapter ? s_bound_adapter->execution.cursor.cpu.irqWanted : 0,
            s_bound_adapter ? s_bound_adapter->execution.cursor.cpu.waiting : 0,
            s_bound_adapter ? s_bound_adapter->execution.cursor.cpu.stopped : 0);
    abort();
  }
}
