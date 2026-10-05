#ifndef GOOF_FRAME_DRIVER_H
#define GOOF_FRAME_DRIVER_H
#include "interp_bridge.h"

/* Logical test intervals only. Not connected to boot_harness.run_frame. */
typedef enum { LOGICAL_NMI_EPOCH, SCRIPTED_EVENT_SLICE, DRAIN_TO_WAIT } GoofInterval;
typedef enum {
    EPOCH_COMPLETE_WAITING, SLICE_COMPLETE, WAITING, BUDGET_EXHAUSTED,
    INVALID_STATE, EXECUTION_ERROR, UNSUPPORTED_EVENT
} GoofStop;
typedef enum { GOOF_SCAN, GOOF_DISPATCH_SAVE, GOOF_RESUME_POP, GOOF_TASK,
               GOOF_YIELD_SAVE, GOOF_RESTORE_SCHED, GOOF_NMI } GoofPhase;
typedef enum { GOOF_BEFORE, GOOF_AFTER, GOOF_WRITE, GOOF_NMI_REQUEST,
               GOOF_INTERRUPT, GOOF_AOT_ENTER, GOOF_AOT_EXIT,
               GOOF_STOP } GoofTraceKind;
typedef struct {
    uint64_t ordinal, event;
    GoofTraceKind kind;
    GoofPhase phase;
    uint32_t pc24, address;
    uint8_t opcode, value, p, slots[32], abc[3], slot;
    uint16_t scheduler_stack;
    Interp816 cpu;
    GoofStop stop;
} GoofTrace;
#define GOOF_TRACE_CAPACITY 256
struct GuestExecution;
typedef void (*GoofObserver)(void *user, const GoofTrace *trace);
/* Hardware callbacks may set latches/input, never guest slots or $9A/B/C.
 * They run once per plan / edge, on the caller's deterministic bus. */
typedef void (*GoofHardwareEvent)(void *bus, bool nmi_edge, uint16_t joy1, uint16_t joy2);
/* GOOF_AUDIO_PHYSICAL_BOUNDARY_PUMP_V1 -- host presentation notification.
 *
 * Invoked once per PHYSICAL FRAME BOUNDARY that an armed plan closes, after
 * that boundary's APU/DSP time has been pulled and therefore after the
 * boundary's 534 native PCM frames exist in the DSP output ring.  It exists
 * so a host can service presentation at physical granularity instead of
 * waiting for a logical epoch that may span 143 boundaries.
 *
 * STRICTLY OBSERVATIONAL.  The contract on an implementation is:
 *   - it may read and retire already-produced native PCM, and update its own
 *     host-side bookkeeping;
 *   - it may NOT advance the guest, the SPC, the DSP or master_cycles, may
 *     NOT block, sleep, wait on a device, or read a wall clock to decide
 *     anything about guest progress, and may NOT touch guest state.
 * Nothing in the driver reads a return value, so it cannot steer execution.
 *
 * It is NOT part of a FramePlan and therefore NOT part of plan identity
 * (`same_event`): it is persistent host wiring, not a semantic property of
 * an interval, and two plans that differ only in it are the same plan.
 * Unset (the default after goof_execution_init) it costs one predictable
 * NULL test per boundary and changes nothing observable -- which is what
 * keeps headless and --no-audio byte-identical. */
typedef void (*GoofBoundaryNotify)(void *user, uint64_t deadline_master);
typedef struct {
    uint64_t id;                 /* nonzero, increasing for a NEW interval */
    GoofInterval interval;
    bool request_nmi, nmi_enabled, irq;
    /* Run the physical-frame boundary engine for this interval: one NMI
     * opportunity at every absolute frame deadline the guest's own work
     * crosses, plus deterministic idle padding to the next deadline at
     * certified quiescence.  Mutually exclusive with request_nmi, which is
     * retained ONLY to reproduce the legacy single-NMI baseline in
     * diagnostics.  Period and phase are NOT here: they are persistent
     * clock state (GuestExecution), because two consecutive plans must not
     * be able to contradict each other about what time it is. */
    bool physical_boundaries;
    uint64_t nmi_instruction;    /* absolute ordinal, before opcode; 0 = now */
    uint32_t nmi_pc;             /* optional PC/occurrence instead */
    uint64_t nmi_occurrence;     /* 1-based */
    uint64_t fence_instruction;  /* absolute ordinal; 0 disables */
    uint32_t fence_pc;
    uint64_t fence_occurrence;   /* 1-based, counts within this plan */
    uint16_t joy1, joy2;
    GoofHardwareEvent hardware;
} FramePlan;
typedef struct { uint64_t steps; } SafetyBudget;
/* The longest AOT activation the physical-boundary contract tolerates, and --
 * the SAME number, deliberately, because the two must never drift apart -- the
 * width of the window before a physical deadline in which an activation is not
 * admitted at all (tier-uniform master clock, design section 22/25).
 *
 * The conjunction is what makes the no-straddle guarantee airtight: an
 * activation starts only with at least this much margin, and PBN4 fails closed
 * if any activation ever exceeds it.  Therefore no activation can contain or
 * span a boundary, every boundary in every tier is crossed by an INTERPRETED
 * step, and the interrupted PC is determined by the clock alone.
 *
 * Cost is host-side only: 16,384 of the 357,368-master-clock period is 4.58 %
 * of the grid, and declining is semantically free -- the AOT-eligible PC is
 * simply interpreted, which the FUNCTION-only tier proves is always possible. */
enum { kGoofMaxAotActivationMaster = 16384 };
enum { kGoofPpuJournalCapacity = 64 };
typedef struct {
    uint64_t master;
    uint8_t reg;
    uint16_t old_value, new_value;
} GoofPpuWrite;
typedef struct {
    uint64_t period;
    unsigned count;
    bool overflow;
    GoofPpuWrite entries[kGoofPpuJournalCapacity];
} GoofPpuJournal;
/* DRAM refresh (bsnes/LakeSnes): the CPU loses 40 master clocks once per
 * 1364-clock scanline, at dot position 536 of the line.  The position is the
 * reference emulators' and only fixes WHERE in the line the tax falls; the
 * milestone requires the elapsed tax, not beam-exact placement. */
enum { kGoofScanlineMaster = 1364, kGoofDramRefreshPosition = 536,
       kGoofDramRefreshMaster = 40 };
typedef struct {
    GoofStop reason;
    uint32_t pc24;
    uint64_t consumed, remaining;
    bool nmi_in_progress, certified;
    GoofPhase phase;
    /* Semantic result of THIS interval, as per-interval deltas.  Each one is
     * read by an acceptance assertion (PBN2/PBN3/PBN4); implementation
     * statistics stay lifetime counters inside GuestExecution instead, so a
     * debug number cannot acquire a per-epoch tolerance by being reported. */
    uint64_t physical_periods;           /* frame boundaries crossed        */
    uint64_t nmi_requests;               /* edges handed to the cursor      */
    uint64_t nmi_entries;                /* handlers actually entered       */
    uint64_t idle_padding_master_cycles; /* cycles added by padding at end  */
} RunResult;
typedef struct GuestExecution {
    InterpCursor cursor;
    void *bus;
    Interp816ReadHandler read;
    Interp816WriteHandler write;
    uint16_t scheduler_s;
    bool initialized, certified, lap, in_nmi, in_irq, epoch_done, event_consumed;
    uint8_t visited;
    uint64_t plan_id, nmi_visits, fence_visits;
    FramePlan active_plan;
    uint64_t nmis, epilogues, countdown_passes, promotions, dispatches, tasks;
    uint64_t aot_entries, aot_initial_entries, aot_resume_entries;
    uint64_t aot_yields, aot_terminations, aot_guest_continuations;
    const InterpAotEntryDescriptor *aot_points;
    unsigned aot_point_count;
    unsigned aot_kind_mask;
    CpuState aot_cpu;
    InterpAotResult last_aot;
    uintptr_t aot_host_frame_min, aot_host_frame_max;
    /* ---- physical frame clock (persistent; never in a plan, never in the
     * frontend, never an engine global).  The source of truth is
     * g_cpu.master_cycles against an ABSOLUTE deadline sequence
     * k * frame_period_master; a modulo is deliberately not used, because it
     * cannot express "two boundaries were crossed by one activation", it
     * re-derives phase from a value idle padding mutates, and it silently
     * absorbs drift. */
    uint64_t frame_period_master;     /* 1364*262 NTSC master cycles       */
    uint64_t next_physical_boundary;  /* absolute deadline; init = period  */
    uint64_t physical_periods;        /* boundaries crossed, lifetime      */
    uint64_t nmi_requests, nmi_entries;            /* lifetime             */
    uint64_t idle_padding_master;                  /* lifetime             */
    bool     boundary_nmi_pending;    /* the single deferred edge          */
    /* Per-interval deltas, reported through RunResult. */
    uint64_t interval_physical_periods, interval_nmi_requests;
    uint64_t interval_nmi_entries, interval_idle_padding;
    /* Debug-only, context-resident, gated at zero by the campaign. */
    uint64_t nmi_deferred_in_nmi;      /* boundary crossed while in_nmi    */
    uint64_t nmi_deferred_collapsed;   /* 2nd deferred edge, dropped       */
    uint64_t nmi_request_collapsed;    /* edge requested over a live one   */
    uint64_t multi_boundary_detections;/* one check closed >1 boundary     */
    uint64_t boundary_checks;
    uint64_t max_detection_overshoot;  /* detection mclk - deadline mclk   */
    /* Deadline-aware AOT admission (tier-uniform master clock, design 22).
     * An activation is atomic, so one that spans a physical deadline defers
     * detection to its end and interrupts a different guest PC than the
     * interpreted tier does.  The driver therefore DECLINES to start an
     * activation whose worst case could reach the next deadline; the work is
     * interpreted instead, which tier B proves is always possible.  These two
     * counters make the policy measurable rather than assumed. */
    uint64_t aot_declined_near_boundary; /* activations tiered down          */
    uint64_t aot_boundary_straddles;     /* MUST stay 0 (acceptance TU)      */
    uint64_t irq_compare_count, irq_entry_count, irq_ack_count;
    uint64_t irq_deferred_i_count, irq_aot_declines, irq_aot_straddles;
    uint64_t irq_idle_wake_count, irq_inside_nmi_count, irq_lost, irq_duplicate;
    uint64_t irq_entry_master, irq_max_straddle;
    uint64_t irq_rti_count, irq_max_entry_delay, irq_max_handler_master;
    uint64_t irq_last_compare_master, irq_last_entry_master, irq_last_ack_master;
    uint64_t irq_enable_floor_master, irq_digest;
    uint16_t irq_seen_h, irq_seen_v;
    bool irq_seen_enabled, irq_config_seen;
    uint16_t irq_entry_sp;
    Interp816 irq_before_cpu;
    uint64_t irq_stack_failures;
    GoofPpuJournal ppu_journal;
    uint64_t ppu_journal_digest, ppu_journal_writes, ppu_journal_overflows;
    unsigned ppu_journal_max_used;
    /* ---- CPU bus-speed master-clock accounting (GOOF_CPU_DMA_REFRESH).
     * An interpreted step is charged the bsnes price of what it did: the
     * region speed of every real bus transfer (6/8/12, MEMSEL-aware) plus 6
     * per remaining internal cycle.  bus_step collects the transfers of the
     * step in flight through the bus callbacks; the cursor's own opcode PEEK
     * (interp_cursor_step reads the opcode once before the CPU fetches it) is
     * not a bus cycle and is removed, and bus_timing_anomalies counts any
     * step whose transfers do not fit its cycle count.  AOT activations are
     * priced by the generated code itself (block fetch/internal constant +
     * paced data accessors).  Every counter below is observe-only. */
    struct {
        bool armed;
        unsigned accesses;
        unsigned by_speed[3];               /* 6 / 8 / 12, peek included */
        uint64_t master;
        uint32_t first_addr;
        bool first_is_read;
        uint8_t first_speed;
    } bus_step;
    uint64_t bus_fast, bus_slow, bus_xslow;       /* interpreted transfers   */
    uint64_t bus_master_fast, bus_master_slow, bus_master_xslow;
    uint64_t bus_internal_cycles, bus_master_internal;
    uint64_t bus_peeks_excluded;
    uint64_t bus_timing_anomalies;                /* MUST stay 0             */
    uint64_t interp_master;                       /* charged to interp steps */
    uint64_t aot_master;                          /* charged by activations  */
    /* DRAM refresh: 40 master clocks each time guest time -- executed or
     * stalled, never idle padding -- passes the refresh position of a
     * scanline on the absolute physical grid. */
    uint64_t dram_refresh_master, dram_refresh_events;
    /* General DMA / guest stall time charged inside an AOT activation.  The
     * charge itself lands on the active (AOT) clock; the counter exists so a
     * campaign can require it to be 0 while no activation is admitted with
     * DMA in it (the 16,384-clock admission bound assumes CPU work only). */
    uint64_t aot_stall_master, aot_stall_activations;
    uint64_t first_boundary_mclk, last_boundary_mclk;
    uint32_t task_probe_pc; /* optional observer counter; zero disables */
    uint16_t min_s;
    unsigned host_depth, max_host_depth;
    GoofStop last_stop;
    bool trace_enabled;
    uint64_t trace_head;
    GoofTrace ring[GOOF_TRACE_CAPACITY];
    GoofObserver observer;
    void *observer_user;
    /* Host presentation notification (see GoofBoundaryNotify).  Host wiring,
     * never guest state: it is set after init, is not in any plan, and is
     * excluded from every digest by construction because nothing writes it
     * into one. */
    GoofBoundaryNotify boundary_notify;
    void *boundary_notify_user;
} GuestExecution;
/* snapshot must be native, exact_pb, deterministic byte bus, no live AOT.
 * Context has stable address for its entire lifetime; do not shallow-copy it. */
bool goof_execution_init(GuestExecution *ctx, const Interp816 *snapshot, uint16_t scheduler_s);
bool goof_execution_enable_aot(GuestExecution *ctx,
                               const InterpAotEntryDescriptor *entries,
                               unsigned entry_count, uint8_t *ram);
bool goof_execution_set_aot_kinds(GuestExecution *ctx, unsigned kind_mask);
/* Test-only: retune the physical frame period.  Rejected once the clock has
 * advanced, so a campaign cannot change phase halfway through.  No product
 * caller exists; the period is 1364*262 everywhere else. */
bool goof_execution_set_physical_frame(GuestExecution *ctx, uint64_t period);
void goof_external_change(GuestExecution *ctx); /* invalidate proof on external writes */
/* Installs (or, with fn == NULL, removes) the host presentation notification.
 * Must be called after goof_execution_init, which zeroes the context.  Purely
 * additive: the guest cannot observe whether one is installed. */
void goof_execution_set_boundary_notify(GuestExecution *ctx,
                                        GoofBoundaryNotify fn, void *user);
const GoofPpuJournal *goof_execution_render_journal(const GuestExecution *ctx,
                                                    uint64_t render_master);
RunResult goof_run_frame(GuestExecution *ctx, const FramePlan *plan, SafetyBudget budget);
#endif
