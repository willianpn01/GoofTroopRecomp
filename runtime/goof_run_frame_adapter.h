#ifndef GOOF_RUN_FRAME_ADAPTER_H
#define GOOF_RUN_FRAME_ADAPTER_H

#include "goof_frame_driver.h"

typedef struct {
  GuestExecution execution;
  uint64_t next_plan_id;
  uint64_t callbacks;
  uint64_t logical_nmi_epochs;
  RunResult last_result;
  GoofHardwareEvent hardware;
  bool ready;
} GoofRunFrameAdapter;

bool goof_run_frame_adapter_init(GoofRunFrameAdapter *adapter,
                                 const Interp816 *reset_snapshot,
                                 uint16_t scheduler_s,
                                 const InterpAotEntryDescriptor *entries,
                                 unsigned entry_count, uint8_t *ram,
                                 GoofHardwareEvent hardware,
                                 uint64_t reset_budget);
/* Neutral advance: joy1 = joy2 = 0.  This is what every permanent gate
 * calls, so a gate cannot present non-neutral input even by accident. */
RunResult goof_run_frame_adapter_advance(GoofRunFrameAdapter *adapter,
                                        uint64_t budget);

/* GOOF_INPUT_V1.  Advances one logical epoch with an explicit guest-visible
 * joypad sample, in the engine's own Snes.input{1,2}_currentState bit order
 * (frontend/goof_input.h).  `joy1`/`joy2` become the epoch's FramePlan input
 * and are therefore IMMUTABLE for the whole epoch: the driver latches them
 * once at plan start and again at every internal physical-boundary NMI edge,
 * so one host sample serves every boundary inside the epoch.  That is the
 * boundary model documented in goof_physical_boundary_within_epoch_
 * implementation.md section 46.3, and it is the reason host presentation
 * cadence cannot move WHEN the guest sees input. */
RunResult goof_run_frame_adapter_advance_input(GoofRunFrameAdapter *adapter,
                                               uint64_t budget,
                                               uint16_t joy1, uint16_t joy2);
/* GOOF_AUDIO_PHYSICAL_BOUNDARY_PUMP_V1.  Pass-through to the driver's host
 * presentation notification (goof_frame_driver.h, GoofBoundaryNotify): the
 * host learns of every physical frame boundary an epoch closes, as it closes
 * it, instead of only at the epoch's return.  Host-only and observational;
 * it changes nothing about how the epoch executes. */
void goof_run_frame_adapter_set_boundary_notify(GoofRunFrameAdapter *adapter,
                                                GoofBoundaryNotify fn,
                                                void *user);
void goof_run_frame_adapter_bind(GoofRunFrameAdapter *adapter);
void goof_boot_harness_run_frame(void);

#endif
