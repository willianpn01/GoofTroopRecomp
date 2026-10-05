#include "goof_frame_driver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_rtl.h"   /* rtl_accumulate_apu_catchup, RtlApuLock/Unlock */
#include "common_cpu_infra.h" /* g_snes */
#include "snes.h"         /* g_snes->nmiEnabled: live NMITIMEN.7          */
#include "snes_cycles.h"  /* snes_region_speed: the one bus-speed authority */

/* The NTSC frame period in guest master cycles, spelled as the line*dot
 * product it is.  The engine's kMasterCyclesPerFrame (common_rtl.c) is the
 * same 357368; this is the one physical constant the boundary clock owns and
 * it is deliberately not a second literal copied from a report. */
enum { kGoofFramePeriodMaster = 1364u * 262u };

static uint32_t pc24(const GuestExecution *g) {
    return ((uint32_t)g->cursor.cpu.k << 16) | g->cursor.cpu.pc;
}
static void irq_diag(const char *kind, const GuestExecution *g,
                     uint64_t master, uint32_t pc) {
    if (!getenv("GOOF_IRQ_TRACE")) return;
    fprintf(stderr, "GOOF_IRQ_V1 kind=%s ordinal=%llu master=%llu "
            "h=%u v=%u pc=%06X sp=%04X i=%u timeup=%u\n",
            kind, (unsigned long long)g->irq_compare_count,
            (unsigned long long)master, (unsigned)g->irq_seen_h,
            (unsigned)g->irq_seen_v, pc, g->cursor.cpu.sp,
            (unsigned)g->cursor.cpu.i,
            (unsigned)(g_snes && g_snes->inIrq));
    if (kind[0] == 'e') {
        uint16_t s = g->cursor.cpu.sp;
        fprintf(stderr, "GOOF_IRQ_STACK_V1 ordinal=%llu sp=%04X "
                "push_p=%02X push_pc=%02X%02X push_pb=%02X "
                "return=%02X:%04X\n",
                (unsigned long long)g->irq_compare_count, s,
                g->read(g->bus, (uint16_t)(s + 1)),
                g->read(g->bus, (uint16_t)(s + 3)),
                g->read(g->bus, (uint16_t)(s + 2)),
                g->read(g->bus, (uint16_t)(s + 4)),
                g->irq_before_cpu.k, g->irq_before_cpu.pc);
    }
}
static void irq_digest_u64(GuestExecution *g, uint64_t value) {
    for (unsigned i = 0; i < 8; i++) {
        g->irq_digest ^= (uint8_t)(value >> (8 * i));
        g->irq_digest *= UINT64_C(1099511628211);
    }
}
static GoofPhase phase(const GuestExecution *g) {
    uint32_t pc = pc24(g);
    if (g->in_nmi) return GOOF_NMI;
    if (pc >= 0x80809f && pc < 0x8080c7) return GOOF_SCAN;
    if (pc >= 0x8080c7 && pc < 0x8080db) return GOOF_DISPATCH_SAVE;
    if (pc >= 0x8080db && pc < 0x8080e4) return GOOF_RESUME_POP;
    if (pc >= 0x8080e4 && pc < 0x808104) return GOOF_DISPATCH_SAVE;
    if ((pc >= 0x808142 && pc < 0x80816d) ||
        (pc >= 0x808196 && pc < 0x8081b5)) return GOOF_YIELD_SAVE;
    if ((pc >= 0x80816d && pc < 0x808176) ||
        (pc >= 0x8081b5 && pc < 0x8081e3) ||
        (pc >= 0x808122 && pc < 0x808136)) return GOOF_RESTORE_SCHED;
    return GOOF_TASK;
}
static uint8_t rd(GuestExecution *g, uint32_t a) { return g->read(g->bus, a); }
static bool runnable(uint8_t s) { return !(s & 0x80) && s >= 4; }
static void trace(GuestExecution *g, GoofTraceKind kind, uint32_t address, uint8_t value) {
    if (!g->trace_enabled && !g->observer) return;
    GoofTrace t = {0};
    t.ordinal = g->cursor.instructions; t.event = g->cursor.events;
    t.kind = kind; t.phase = phase(g); t.pc24 = pc24(g); t.address = address; t.value = value;
    t.opcode = g->cursor.last_opcode; t.cpu = g->cursor.cpu;
    t.p = interp816_getFlags(&g->cursor.cpu); t.stop = g->last_stop;
    for (unsigned i = 0; i < 32; i++) t.slots[i] = rd(g, 0x50+i);
    for (unsigned i = 0; i < 3; i++) t.abc[i] = rd(g, 0x9a+i);
    t.scheduler_stack = rd(g, 0x25) | (rd(g, 0x26) << 8); t.slot = rd(g, 0x27);
    if (g->trace_enabled) g->ring[g->trace_head++ % GOOF_TRACE_CAPACITY] = t;
    if (g->observer) g->observer(g->observer_user, &t);
}
/* Prices one real CPU bus transfer of the interpreted step in flight at the
 * bsnes wait state of its 24-bit address (snes_region_speed: FastROM 6 when
 * MEMSEL is set, WRAM/stack/DP 8, $2000-$3FFF and $4200-$5FFF 6, $4000-$41FF
 * 12), with MEMSEL as it is when the transfer happens.  Only the driver's bus
 * callbacks call this, so only the interpreter's own transfers are priced;
 * the driver's side-effect-free observation reads (rd) are not. */
static unsigned speed_class(uint8_t speed) {
    return speed == SNES_CYC_FAST ? 0 : speed == SNES_CYC_SLOW ? 1 : 2;
}
static void bus_time(GuestExecution *g, uint32_t a, bool is_read) {
    if (!g->bus_step.armed) return;
    uint8_t speed = (uint8_t)snes_region_speed(a & 0xFFFFFFu, g_memsel);
    if (!g->bus_step.accesses) {
        g->bus_step.first_addr = a & 0xFFFFFFu;
        g->bus_step.first_is_read = is_read;
        g->bus_step.first_speed = speed;
    }
    g->bus_step.accesses++;
    g->bus_step.by_speed[speed_class(speed)]++;
    g->bus_step.master += speed;
}

static uint8_t bus_read(void *mem, uint32_t a) {
    GuestExecution *g = mem;
    bus_time(g, a, true);
    uint8_t value = rd(g, a);
    if ((a & 0xffffu) == 0x4211 && ((a >> 16) & 0x7fu) < 0x40 &&
        (value & 0x80)) {
        g->irq_ack_count++;
        g->irq_last_ack_master = g_cpu.master_cycles + g->bus_step.master;
        irq_digest_u64(g, g->irq_last_ack_master);
        irq_diag("ack", g, g->irq_last_ack_master, g->cursor.last_pc);
    }
    return value;
}
static void bus_write(void *mem, uint32_t a, uint8_t v) {
    GuestExecution *g = mem;
    bus_time(g, a, false);
    uint8_t bank = a >> 16; uint16_t low = a;
    bool wram_low = bank == 0x7e || (bank & 0x7f) < 0x40;
    if (wram_low && low >= 0x50 && low < 0x70) {
        g->lap = g->certified = false;
        if (g->in_nmi && g->cursor.last_pc == 0x808362 &&
            v == 4 && rd(g, a) == 1) g->promotions++;
    }
    g->write(g->bus, a, v);
    trace(g, GOOF_WRITE, a, v);
}
static void journal_digest_byte(GuestExecution *g, uint8_t value) {
    g->ppu_journal_digest ^= value;
    g->ppu_journal_digest *= UINT64_C(1099511628211);
}
static void on_ppu_render_write(void *user, uint8_t adr,
                                uint16_t old_value, uint16_t new_value) {
    GuestExecution *g = user;
    uint64_t master = g->bus_step.armed ?
        g_cpu.master_cycles + g->bus_step.master :
        g_apu_clock_cpu ? g_apu_clock_cpu->master_cycles : g_cpu.master_cycles;
    uint64_t period = master / g->frame_period_master;
    uint64_t phase = master % g->frame_period_master;
    /* V225 is the period start; V0 begins 37 lines later. */
    if (phase < 37u * 1364u) return;
    GoofPpuJournal *j = &g->ppu_journal;
    if (j->count && (period < j->period ||
                     (period == j->period &&
                      master < j->entries[j->count - 1].master))) {
        j->overflow = true;
        g->ppu_journal_overflows++;
        return;
    }
    if (j->period != period) {
        j->period = period;
        j->count = 0;
        j->overflow = false;
    }
    if (j->count >= kGoofPpuJournalCapacity) {
        j->overflow = true;
        g->ppu_journal_overflows++;
        return;
    }
    j->entries[j->count++] = (GoofPpuWrite){master, adr,
                                             old_value, new_value};
    if (j->count > g->ppu_journal_max_used)
        g->ppu_journal_max_used = j->count;
    g->ppu_journal_writes++;
    for (unsigned i = 0; i < 8; i++) journal_digest_byte(g, period >> (8 * i));
    for (unsigned i = 0; i < 8; i++) journal_digest_byte(g, master >> (8 * i));
    journal_digest_byte(g, adr);
    journal_digest_byte(g, old_value);
    journal_digest_byte(g, old_value >> 8);
    journal_digest_byte(g, new_value);
    journal_digest_byte(g, new_value >> 8);
}
const GoofPpuJournal *goof_execution_render_journal(const GuestExecution *g,
                                                    uint64_t render_master) {
    if (!g || !g->frame_period_master || !render_master) return NULL;
    uint64_t period = (render_master - 1) / g->frame_period_master;
    if (g->ppu_journal.period != period) return NULL;
    return &g->ppu_journal;
}
static bool bus_apu_word(uint32_t adrl, uint32_t adrh) {
    if (adrh != adrl + 1) return false;
    uint16_t a = (uint16_t)adrl;
    uint8_t bank = (uint8_t)(adrl >> 16);
    return a >= 0x2140 && a < 0x217f &&
           (bank <= 0x3f || (bank >= 0x80 && bank <= 0xbf));
}
static bool bus_read_word(void *mem, uint32_t adrl, uint32_t adrh,
                          uint16_t *out) {
    GuestExecution *g = mem;
    if (!bus_apu_word(adrl, adrh)) return false;
    bus_time(g, adrl, true); bus_time(g, adrh, true);
    *out = cpu_read16((CpuState *)g->bus, (uint8_t)(adrl >> 16),
                      (uint16_t)adrl);
    return true;
}
static bool bus_write_word(void *mem, uint32_t adrl, uint32_t adrh,
                           uint16_t value, bool reversed) {
    GuestExecution *g = mem;
    if (reversed || !bus_apu_word(adrl, adrh)) return false;
    bus_time(g, adrl, false); bus_time(g, adrh, false);
    cpu_write16((CpuState *)g->bus, (uint8_t)(adrl >> 16),
                (uint16_t)adrl, value);
    return true;
}
void goof_external_change(GuestExecution *g) { g->certified = g->lap = false; }
void goof_execution_set_boundary_notify(GuestExecution *g,
                                        GoofBoundaryNotify fn, void *user) {
    if (!g) return;
    g->boundary_notify = fn;
    g->boundary_notify_user = user;
}
bool goof_execution_init(GuestExecution *g, const Interp816 *s, uint16_t scheduler_s) {
    memset(g, 0, sizeof(*g));
    if (!s || !s->read || !s->write || s->e || !s->exact_pb) return false;
    g->bus = s->mem; g->read = s->read; g->write = s->write;
    interp_cursor_init(&g->cursor, s);
    g->cursor.cpu.mem = g; g->cursor.cpu.read = bus_read; g->cursor.cpu.write = bus_write;
    g->cursor.cpu.read_word = bus_read_word;
    g->cursor.cpu.write_word = bus_write_word;
    g->scheduler_s = scheduler_s; g->min_s = s->sp; g->initialized = true;
    /* Phase origin is master_cycles == 0, i.e. engine reset (cpu_state_init),
     * so boundary k is at k * period.  Boundary k=1 falls inside the
     * bootstrap interval, where NMITIMEN.7 is still 0 and it is dropped --
     * which is correct: the guest has not enabled NMI yet. */
    g->frame_period_master = kGoofFramePeriodMaster;
    g->next_physical_boundary = kGoofFramePeriodMaster;
    g->ppu_journal_digest = UINT64_C(1469598103934665603);
    g->irq_digest = UINT64_C(1469598103934665603);
    ppu_set_render_write_observer(g_ppu, on_ppu_render_write, g);
    return true;
}
bool goof_execution_set_physical_frame(GuestExecution *g, uint64_t period) {
    if (!g || !g->initialized || !period) return false;
    if (g->physical_periods || g_cpu.master_cycles) return false;
    g->frame_period_master = period;
    g->next_physical_boundary = period;
    return true;
}
bool goof_execution_enable_aot(GuestExecution *g,
                               const InterpAotEntryDescriptor *entries,
                               unsigned entry_count, uint8_t *ram) {
    if (!g || !g->initialized || !entries || !entry_count || !ram) return false;
    for (unsigned i = 0; i < entry_count; i++) {
        if (!entries[i].entry || !entries[i].pc24 || entries[i].pc24 > 0xFFFFFFu ||
            (entries[i].kind != INTERP_AOT_ENTRY_FUNCTION &&
             entries[i].kind != INTERP_AOT_ENTRY_CONTINUATION) ||
            (i && entries[i - 1].pc24 >= entries[i].pc24)) return false;
    }
    memset(&g->aot_cpu, 0, sizeof(g->aot_cpu));
    g->aot_cpu.ram = ram;
    g->aot_points = entries;
    g->aot_point_count = entry_count;
    g->aot_kind_mask = 1u << INTERP_AOT_ENTRY_FUNCTION;
    return true;
}
bool goof_execution_set_aot_kinds(GuestExecution *g, unsigned kind_mask) {
    unsigned valid = (1u << INTERP_AOT_ENTRY_FUNCTION) |
                     (1u << INTERP_AOT_ENTRY_CONTINUATION);
    if (!g || !g->aot_points || !kind_mask || (kind_mask & ~valid)) return false;
    g->aot_kind_mask = kind_mask;
    return true;
}
/* DRAM refresh tax over guest time that has just elapsed in [from, now):
 * 40 master clocks for every refresh position (dot 536 of a 1364-clock line,
 * on the absolute grid whose origin is master 0, the same origin as the frame
 * deadlines) that the interval passes.  The tax is itself guest time, so it
 * can carry the clock over one more refresh position; the loop charges those
 * too, until none is left.  Called for executed and stalled time only --
 * interpreted steps (which include any DMA stall they caused) and AOT
 * activations -- never for idle padding, which lands exactly on a deadline
 * and represents no CPU work.  Applied piecewise or once over a whole
 * activation the total is the same: it counts refresh positions in one
 * contiguous span of guest time. */
static uint64_t refresh_positions_through(uint64_t m) {
    return (m + kGoofScanlineMaster - kGoofDramRefreshPosition) / kGoofScanlineMaster;
}
static void charge_dram_refresh(GuestExecution *g, uint64_t from) {
    uint64_t lo = from, hi = g_cpu.master_cycles;
    for (;;) {
        uint64_t n = refresh_positions_through(hi) - refresh_positions_through(lo);
        if (!n) break;
        uint64_t tax = n * kGoofDramRefreshMaster;
        g_cpu.master_cycles += tax;
        g->dram_refresh_master += tax;
        g->dram_refresh_events += n;
        lo = hi; hi = g_cpu.master_cycles;
    }
}

/* The bus-speed price of the interpreted step that just executed:
 *   sum(speed of every real transfer) + 6 * (cyclesUsed - transfers).
 * cyclesUsed stays the Shared Timing Contract cycle count, so this re-prices
 * the same cycles and cannot double-count one: a cycle is either one of the
 * step's transfers or it is internal.  An ordinary instruction's first read is
 * the cursor's opcode PEEK, made before interp816_runOpcode fetches the same
 * byte; it is removed.  NMI entry has no peek. */
static uint64_t interp_step_master(GuestExecution *g, bool is_instruction,
                                   unsigned cycles) {
    unsigned n = g->bus_step.accesses;
    uint64_t master = g->bus_step.master;
    if (is_instruction) {
        if (n && g->bus_step.first_is_read &&
            g->bus_step.first_addr == g->cursor.last_pc) {
            n--; master -= g->bus_step.first_speed;
            g->bus_step.by_speed[speed_class(g->bus_step.first_speed)]--;
            g->bus_peeks_excluded++;
        } else {
            g->bus_timing_anomalies++;
        }
    }
    const unsigned *k = g->bus_step.by_speed;
    g->bus_fast += k[0]; g->bus_slow += k[1]; g->bus_xslow += k[2];
    g->bus_master_fast += (uint64_t)k[0] * SNES_CYC_FAST;
    g->bus_master_slow += (uint64_t)k[1] * SNES_CYC_SLOW;
    g->bus_master_xslow += (uint64_t)k[2] * SNES_CYC_XSLOW;
    if (n > cycles) { g->bus_timing_anomalies++; n = cycles; }
    uint64_t internal = cycles - n;
    g->bus_internal_cycles += internal;
    g->bus_master_internal += internal * SNES_CYC_INTERNAL;
    return master + internal * SNES_CYC_INTERNAL;
}

/* Processes every physical frame boundary the guest has crossed, in
 * chronological deadline order, never collapsing two crossings into one.
 *
 * Each event is timestamped at its SCHEDULED DEADLINE, not at the moment the
 * check noticed it: detection can lag by at most one AOT activation, and the
 * accounting must not inherit that jitter.  The loop is a `while`, not an
 * `if`, because k crossed periods must be ACCOUNTED even though at most one
 * NMI edge can be DELIVERED -- period counts, APU sync points and the guest's
 * own $9C accounting all depend on the count, and a silent collapse would
 * make the clock measure work again instead of time.
 *
 * Observe-only unless the plan arms it: a plan without physical_boundaries
 * still has its boundaries counted, but requests no NMI, pulls no APU time
 * and mutates nothing the guest can see.  That is what makes the legacy
 * single-NMI baseline reproducible for diagnostics. */
static void process_physical_boundaries(GuestExecution *g, const FramePlan *p) {
    g->boundary_checks++;
    uint64_t m = g_cpu.master_cycles;
    if (m < g->next_physical_boundary) return;          /* the common path */

    uint64_t overshoot = m - g->next_physical_boundary;
    if (overshoot > g->max_detection_overshoot)
        g->max_detection_overshoot = overshoot;

    unsigned closed = 0;
    while (m >= g->next_physical_boundary) {
        uint64_t deadline = g->next_physical_boundary;
        g->physical_periods++;
        g->interval_physical_periods++;
        if (!g->first_boundary_mclk) g->first_boundary_mclk = deadline;
        g->last_boundary_mclk = deadline;
        closed++;

        if (p->physical_boundaries) {
            /* NMITIMEN.7 is sampled LIVE, per boundary, from the guest's own
             * register.  It is never cached for the epoch: CODE_808381 is
             * `STZ NMITIMEN / JSR CODE_80838D / LDA #$B1 / STA NMITIMEN`, so
             * a cached flag would either fire an NMI inside the ROM's own
             * synchronous flush or suppress NMIs for the rest of the epoch.
             * Epoch 1 executes that sequence, so this is not hypothetical. */
            bool enabled = g_snes && g_snes->nmiEnabled;

            /* One APU sync per boundary, anchored in GUEST TIME.
             *
             * This used to be rtl_accumulate_apu_catchup() + snes_catchupApu(),
             * which asked for a full nominal period of SPC time and received
             * at most 10000 cycles, because snes_catchupApu's cap is a
             * DESTRUCTIVE assignment on the debt accumulator rather than a
             * bound on the work.  Measured over E1..E1000: 1,183 of 1,227
             * armed boundaries hit it and 8,380,182 SPC cycles were erased,
             * leaving the APU at 58.5 % of nominal.  Nothing else in the
             * campaign reached the cap -- not one port read, not one port
             * write -- so the boundary pull was the whole defect.
             *
             * The cap is deliberately NOT changed.  The mature ports keep it
             * verbatim as a residual guard and make it unreachable instead,
             * by advancing the APU to an absolute guest-time target rather
             * than by handing it a relative debt.  That is what this call
             * does.  The clamp stays exactly where it is, on the pre-timeline
             * path, and the gate asserts it is never reached.
             *
             * The DEADLINE is passed, not master_cycles: an event is
             * timestamped at the time it was scheduled for, never at the
             * instant the check noticed it, exactly as the NMI accounting
             * above does.  Idle padding is included by construction, because
             * padding advances master_cycles onto the next deadline.
             *
             * The APU is a consumer of guest time here and produces none:
             * this touches no master_cycles, no NMI state, no period count. */
            RtlApuLock();
            rtl_sync_apu_frame_boundary(deadline);
            RtlApuUnlock();

            if (enabled) {
                if (g->in_nmi) {
                    /* At most ONE deferred edge is remembered, matching the
                     * boolean nmiWanted/inNmi edge model.  A second boundary
                     * inside one handler is dropped and counted; the campaign
                     * gate requires both counters to stay zero, so nesting
                     * becomes a visible failure with data attached rather
                     * than an invented model. */
                    if (g->boundary_nmi_pending) g->nmi_deferred_collapsed++;
                    g->boundary_nmi_pending = true;
                    g->nmi_deferred_in_nmi++;
                } else {
                    if (g->cursor.cpu.nmiWanted) g->nmi_request_collapsed++;
                    g->certified = g->lap = false;
                    trace(g, GOOF_NMI_REQUEST, 0, 0);
                    if (p->hardware) p->hardware(g->bus, true, p->joy1, p->joy2);
                    interp816_request_nmi(&g->cursor.cpu);
                    g->nmi_requests++; g->interval_nmi_requests++;
                }
            }
            /* NMI disabled at the boundary: the period is counted and the
             * opportunity is DROPPED.  Nothing accumulates and nothing is
             * delivered later -- the hardware edge model, and what makes the
             * ROM's own serialised flush behave as it does on hardware. */

            /* HOST PRESENTATION NOTIFICATION, last in the boundary's own
             * order: the period has been counted, its APU/DSP time pulled --
             * so its 534 native frames are in the output ring -- and its NMI
             * opportunity settled.  A host servicing hook therefore observes
             * a boundary that is complete, never one in progress.
             *
             * Placed inside the `physical_boundaries` arm deliberately: an
             * unarmed plan pulls no APU time, so there is no new PCM to
             * service and a notification would be a lie.  Nothing here reads
             * a return value, touches g_cpu.master_cycles, the cursor, the
             * NMI state or the period count; see GoofBoundaryNotify. */
            if (g->boundary_notify)
                g->boundary_notify(g->boundary_notify_user, deadline);
        }
        g->next_physical_boundary += g->frame_period_master;
    }
    if (closed > 1) g->multi_boundary_detections++;
}

/* The physical boundary is V225/H0. Goof only uses H+V mode; the timer
 * registers are sampled live after every instruction. A changed compare is
 * armed from the current boundary onward, never retroactively. */
static uint64_t next_irq_deadline(GuestExecution *g) {
    if (!g_snes || !g_snes->hIrqEnabled || !g_snes->vIrqEnabled) {
        g->irq_seen_enabled = false;
        return UINT64_MAX;
    }
    uint16_t h = g_snes->hTimer, v = g_snes->vTimer;
    uint64_t now = g_cpu.master_cycles;
    if (h >= 323 || v >= 262) return UINT64_MAX;
    if (!g->irq_config_seen || !g->irq_seen_enabled ||
        h != g->irq_seen_h || v != g->irq_seen_v) {
        g->irq_enable_floor_master = now;
        g->irq_seen_h = h; g->irq_seen_v = v;
        g->irq_seen_enabled = g->irq_config_seen = true;
    }
    uint64_t phase = ((uint64_t)(v + 37) % 262) * 1364 + 4 * h;
    uint64_t deadline = (now / g->frame_period_master) *
                        g->frame_period_master + phase;
    if (deadline <= g->irq_last_compare_master ||
        deadline <= g->irq_enable_floor_master)
        deadline += g->frame_period_master;
    return deadline;
}

static bool process_irq_deadline(GuestExecution *g) {
    if (g_snes && (g_snes->hIrqEnabled != g_snes->vIrqEnabled ||
                   (g_snes->hIrqEnabled &&
                    (g_snes->hTimer >= 323 || g_snes->vTimer >= 262))))
        return false;
    uint64_t deadline = next_irq_deadline(g);
    if (deadline == UINT64_MAX || g_cpu.master_cycles < deadline) return true;
    if (g_snes->inIrq) { g->irq_duplicate++; return false; }
    g_snes->inIrq = true;
    g->cursor.cpu.irqWanted = true;
    g->irq_last_compare_master = deadline;
    g->irq_compare_count++;
    irq_digest_u64(g, g->irq_compare_count);
    irq_digest_u64(g, ((uint64_t)g->irq_seen_v << 16) | g->irq_seen_h);
    irq_digest_u64(g, deadline);
    irq_diag("compare", g, deadline, pc24(g));
    if (g->in_nmi) g->irq_inside_nmi_count++;
    if (g->cursor.cpu.i) g->irq_deferred_i_count++;
    return true;
}

static bool idle_to_irq(GuestExecution *g, const FramePlan *p) {
    if (!p->physical_boundaries || g->in_irq || g->in_nmi) return false;
    uint64_t deadline = next_irq_deadline(g);
    if (deadline >= g->next_physical_boundary) return false;
    if (deadline <= g_cpu.master_cycles) return false;
    uint64_t pad = deadline - g_cpu.master_cycles;
    g_cpu.master_cycles = deadline;
    g->idle_padding_master += pad;
    g->interval_idle_padding += pad;
    g->irq_idle_wake_count++;
    return true;
}

/* Releases the single deferred edge at the instruction that clears in_nmi.
 * NMITIMEN.7 is re-tested live at that instant: if the guest disabled NMI
 * while inside the handler, the pending edge is discarded, not delivered. */
static void release_deferred_boundary_edge(GuestExecution *g, const FramePlan *p) {
    if (!p->physical_boundaries || !g->boundary_nmi_pending) return;
    g->boundary_nmi_pending = false;
    if (!(g_snes && g_snes->nmiEnabled)) return;
    if (g->cursor.cpu.nmiWanted) g->nmi_request_collapsed++;
    g->certified = g->lap = false;
    trace(g, GOOF_NMI_REQUEST, 0, 0);
    if (p->hardware) p->hardware(g->bus, true, p->joy1, p->joy2);
    interp816_request_nmi(&g->cursor.cpu);
    g->nmi_requests++; g->interval_nmi_requests++;
}

static bool scheduler_context(GuestExecution *g) {
    Interp816 *c = &g->cursor.cpu;
    return pc24(g) == 0x8080a7 && c->sp == g->scheduler_s &&
        c->db == 0x83 && c->mf && c->xf && !c->d && !c->e &&
        !c->waiting && !c->stopped && !g->in_nmi &&
        !c->nmiWanted && !c->irqWanted && c->y == 0x20 &&
        rd(g, 0x9a) == 0 && rd(g, 0x9b) == 0;
}
static bool slots_wait(GuestExecution *g) {
    for (unsigned i=0; i<32; i+=8)
        if (runnable(rd(g, 0x50+i)) || rd(g, 0x50+i) == 2) return false;
    return true;
}
static RunResult finish(GuestExecution *g, GoofStop reason) {
    g->last_stop = reason;
    /* Deterministic idle padding: at certified quiescence the guest has
     * nothing left to do this frame, so guest master time advances to the
     * next physical deadline.  This is the half of the design that turns the
     * clock from a measure of WORK into a measure of TIME -- without it a
     * 1000-epoch campaign spans 329 frame periods of busy time and would
     * receive ~330 NMIs instead of ~1300.
     *
     * NO CPU instruction executes and NO execution budget is consumed;
     * consumed/remaining are untouched.  A cut or a failure (SLICE_COMPLETE,
     * BUDGET_EXHAUSTED, INVALID_STATE, EXECUTION_ERROR, UNSUPPORTED_EVENT) is
     * NOT evidence that the guest is idle, so those never pad.
     *
     * The boundary is deliberately NOT fired here.  Padding lands exactly ON
     * next_physical_boundary and the NEXT interval's first loop-top check
     * processes it, so there stays exactly one delivery path for an NMI. */
    if (g->active_plan.physical_boundaries &&
        (reason == EPOCH_COMPLETE_WAITING || reason == WAITING) &&
        g_cpu.master_cycles < g->next_physical_boundary) {
        uint64_t pad = g->next_physical_boundary - g_cpu.master_cycles;
        g_cpu.master_cycles = g->next_physical_boundary;
        g->idle_padding_master += pad;
        g->interval_idle_padding += pad;
    }
    trace(g, GOOF_STOP, 0, 0);
    RunResult r = {reason, pc24(g), g->cursor.consumed, g->cursor.remaining,
                   g->in_nmi, g->certified, phase(g)};
    r.physical_periods = g->interval_physical_periods;
    r.nmi_requests = g->interval_nmi_requests;
    r.nmi_entries = g->interval_nmi_entries;
    r.idle_padding_master_cycles = g->interval_idle_padding;
    return r;
}
static bool same_event(const FramePlan *a, const FramePlan *b) {
    /* Fences may advance when resuming an interrupted epoch/slice. The
     * event/input portion is immutable, so a retry cannot replay an edge. */
    return a->interval == b->interval && a->request_nmi == b->request_nmi &&
        a->physical_boundaries == b->physical_boundaries &&
        a->nmi_enabled == b->nmi_enabled && a->irq == b->irq &&
        a->nmi_instruction == b->nmi_instruction && a->nmi_pc == b->nmi_pc &&
        a->nmi_occurrence == b->nmi_occurrence && a->joy1 == b->joy1 &&
        a->joy2 == b->joy2 && a->hardware == b->hardware;
}
static RunResult run_interval(GuestExecution *g, const FramePlan *p, SafetyBudget budget) {
    if (!g || !p) return (RunResult){.reason=INVALID_STATE};
    interp_cursor_budget(&g->cursor, budget.steps);
    g->interval_physical_periods = g->interval_nmi_requests = 0;
    g->interval_nmi_entries = g->interval_idle_padding = 0;
    if (!g->initialized || !p->id || (unsigned)p->interval > DRAIN_TO_WAIT ||
        g->cursor.cpu.e || !g->cursor.cpu.exact_pb) return finish(g, INVALID_STATE);
    if (p->irq ||
        (p->request_nmi && !p->nmi_enabled)) return finish(g, UNSUPPORTED_EVENT);
    /* The two event models are alternatives, never simultaneous: one offers a
     * single plan-chosen edge, the other offers an edge at every physical
     * deadline.  Accepting both would make "which edge was that" unanswerable. */
    if (p->request_nmi && p->physical_boundaries) return finish(g, INVALID_STATE);
    if (p->fence_instruction && p->fence_instruction < g->cursor.instructions)
        return finish(g, INVALID_STATE);
    if (p->nmi_pc && !p->nmi_occurrence) return finish(g, INVALID_STATE);
    if (p->fence_pc && !p->fence_occurrence) return finish(g, INVALID_STATE);
    if (p->interval == SCRIPTED_EVENT_SLICE && !p->fence_pc && !p->fence_instruction)
        return finish(g, INVALID_STATE);
    if (p->id != g->plan_id) {
        if ((g->plan_id && g->active_plan.request_nmi && !g->event_consumed) ||
            p->id < g->plan_id || (g->plan_id &&
            g->active_plan.interval == LOGICAL_NMI_EPOCH && !g->epoch_done))
            return finish(g, INVALID_STATE);
        if (p->interval == LOGICAL_NMI_EPOCH &&
            ((!p->request_nmi && !p->physical_boundaries) ||
             p->nmi_pc || p->nmi_instruction ||
             !g->certified || !scheduler_context(g) || !slots_wait(g)))
            return finish(g, INVALID_STATE);
        if (g->cursor.cpu.nmiWanted || (p->request_nmi && g->in_nmi))
            return finish(g, UNSUPPORTED_EVENT);
        g->plan_id = p->id; g->active_plan = *p;
        g->event_consumed = false; g->epoch_done = false;
        g->nmi_visits = g->fence_visits = 0;
        if (p->hardware) p->hardware(g->bus, false, p->joy1, p->joy2);
    } else {
        if (!same_event(p, &g->active_plan)) return finish(g, INVALID_STATE);
        if (p->fence_pc != g->active_plan.fence_pc) g->fence_visits = 0;
        g->active_plan = *p;
    }
    if (g->epoch_done) return finish(g, EPOCH_COMPLETE_WAITING);
    for (;;) {
        /* Physical boundaries are settled FIRST, before the fence tests, the
         * legacy due test and the AOT gate.  Placing it here preserves the
         * existing invariant that a pending interrupt has priority over
         * entering an AOT activation -- which is exactly why no post-
         * activation check is needed: an activation can only start at a
         * moment when the boundary state has just been settled.  It is also
         * what makes the deadline-aware admission test below exact: the
         * remaining window to the next deadline is settled at that point, so
         * an activation is never admitted within one maximum-activation
         * window of a boundary and therefore never spans one.
         * The AOT arm ends in `continue`, so this is also the post-AOT site. */
        process_physical_boundaries(g, p);
        if (g->ppu_journal.overflow)
            return finish(g, UNSUPPORTED_EVENT);
        if (p->physical_boundaries && !process_irq_deadline(g))
            return finish(g, UNSUPPORTED_EVENT);
        if (g_snes) g->cursor.cpu.irqWanted = g_snes->inIrq;
        uint32_t pc = pc24(g);
        /* Cut BEFORE events/opcodes at the next interval boundary. */
        if (p->fence_instruction && g->cursor.instructions >= p->fence_instruction)
            return finish(g, SLICE_COMPLETE);
        if (p->fence_pc == pc && g->fence_visits + 1 == p->fence_occurrence)
            return finish(g, SLICE_COMPLETE);
        bool due = p->request_nmi && !g->event_consumed;
        if (due && p->nmi_pc) due = p->nmi_pc == pc && g->nmi_visits + 1 == p->nmi_occurrence;
        else if (due && p->nmi_instruction) due = g->cursor.instructions >= p->nmi_instruction;
        if (due) {
            if (!g->cursor.remaining) return finish(g, BUDGET_EXHAUSTED);
            if (g->in_nmi) return finish(g, UNSUPPORTED_EVENT);
            g->event_consumed = true; g->certified = g->lap = false;
            trace(g, GOOF_NMI_REQUEST, 0, 0);
            if (p->hardware) p->hardware(g->bus, true, p->joy1, p->joy2);
            interp816_request_nmi(&g->cursor.cpu);
        }
        /* AOT is entered only at a declared task safepoint and only after a
         * pending interrupt has had priority.  The whole activation is one
         * watchdog unit; arbitrary opcodes inside generated C are not
         * preemptible in this first profile. */
        const InterpAotEntryDescriptor *aot = NULL;
        for (unsigned i = 0; i < g->aot_point_count; i++)
            if (g->aot_points[i].pc24 == pc &&
                (g->aot_kind_mask & (1u << g->aot_points[i].kind))) {
                aot = &g->aot_points[i]; break;
            }
        /* DEADLINE-AWARE ADMISSION.  An activation is atomic, so one that
         * spans a physical deadline can only notice it on return -- which put
         * the interrupted PC up to 1,936 master clocks (and a whole NMI
         * handler) away from where the interpreted tier interrupts.  The cure
         * is not to interrupt the activation but not to START it: if the next
         * deadline is closer than the longest activation the contract allows,
         * this PC is interpreted instead.
         *
         * Evaluated HERE, after process_physical_boundaries() has run at the
         * loop top, so next_physical_boundary is settled and strictly ahead of
         * master_cycles; the `>` test keeps the arithmetic unsigned-safe even
         * if that ever stops holding.  The decision reads only the clock, the
         * absolute deadline grid and a compile-time constant -- no host state,
         * so it is as deterministic as the rest of the model.  Declining is
         * semantically neutral: same guest PC, same state, no tier bookkeeping
         * touched, and the scheduler/continuation semantics are untouched. */
        if (aot) {
            uint64_t now = g_cpu.master_cycles;
            uint64_t deadline = g->next_physical_boundary;
            uint64_t irq_deadline = p->physical_boundaries ?
                next_irq_deadline(g) : UINT64_MAX;
            if (irq_deadline < deadline) deadline = irq_deadline;
            uint64_t window = deadline > now ? deadline - now : 0;
            if (window < (uint64_t)kGoofMaxAotActivationMaster) {
                aot = NULL;
                if (irq_deadline < g->next_physical_boundary)
                    g->irq_aot_declines++;
                else g->aot_declined_near_boundary++;
            }
        }
        if (aot &&
            !g->cursor.cpu.nmiWanted && !g->cursor.cpu.irqWanted) {
            if (!g->cursor.remaining) return finish(g, BUDGET_EXHAUSTED);
            trace(g, GOOF_AOT_ENTER, pc, 0);
            uint64_t deadline_at_entry = g->next_physical_boundary;
            uint64_t irq_deadline_at_entry = p->physical_boundaries ?
                next_irq_deadline(g) : UINT64_MAX;
            uint64_t master_at_entry = g_cpu.master_cycles;
            uint64_t stall_at_entry = g_guest_stall_master_total;
            g->aot_cpu.master_cycles = g_cpu.master_cycles;
            g->last_aot = interp_cursor_dispatch_aot(
                &g->cursor, &g->aot_cpu, aot->entry);
            g_cpu.master_cycles = g->aot_cpu.master_cycles;
            /* The activation priced itself (generated block constants plus
             * paced data accessors, and any guest stall it caused charged to
             * its own clock); what it cannot know is where the scanline
             * refresh positions fall, so the tax is applied over its span. */
            g->aot_master += g_cpu.master_cycles - master_at_entry;
            if (g_guest_stall_master_total != stall_at_entry) {
                g->aot_stall_master += g_guest_stall_master_total - stall_at_entry;
                g->aot_stall_activations++;
            }
            charge_dram_refresh(g, master_at_entry);
            /* The admission rule above is meant to make this impossible; it is
             * counted rather than assumed, and the campaign gate requires 0. */
            if (g_cpu.master_cycles >= deadline_at_entry)
                g->aot_boundary_straddles++;
            if (irq_deadline_at_entry < UINT64_MAX &&
                g_cpu.master_cycles >= irq_deadline_at_entry) {
                g->irq_aot_straddles++;
                uint64_t gap = g_cpu.master_cycles - irq_deadline_at_entry;
                if (gap > g->irq_max_straddle) g->irq_max_straddle = gap;
                return finish(g, UNSUPPORTED_EVENT);
            }
            g->aot_entries++;
            if (aot->kind == INTERP_AOT_ENTRY_FUNCTION) g->aot_initial_entries++;
            else g->aot_resume_entries++;
            if (!g->aot_host_frame_min ||
                g->last_aot.host_frame < g->aot_host_frame_min)
                g->aot_host_frame_min = g->last_aot.host_frame;
            if (g->last_aot.host_frame > g->aot_host_frame_max)
                g->aot_host_frame_max = g->last_aot.host_frame;
            if (g->last_aot.kind == INTERP_AOT_EXIT_YIELD) g->aot_yields++;
            else if (g->last_aot.kind == INTERP_AOT_EXIT_TERMINATE)
                g->aot_terminations++;
            else if (g->last_aot.kind == INTERP_AOT_EXIT_CONTINUE_GUEST) {
                /* A guest RTS/RTL already consumed its return frame.  The
                 * bridge installed its authoritative post-transfer PC. */
                g->aot_guest_continuations++;
            }
            else return finish(g, EXECUTION_ERROR);
            if (g->last_aot.recomp_depth_before !=
                g->last_aot.recomp_depth_after)
                return finish(g, EXECUTION_ERROR);
            g->certified = g->lap = false;
            if (g->cursor.cpu.sp < g->min_s) g->min_s = g->cursor.cpu.sp;
            trace(g, GOOF_AOT_EXIT, g->last_aot.next_pc24,
                  (uint8_t)g->last_aot.kind);
            continue;
        }
        if (pc == 0x8080a7 && !g->cursor.cpu.nmiWanted) {
            bool proof = g->lap && g->visited == 15 && scheduler_context(g);
            if (proof) {
                for (unsigned i=0;i<32;i+=8)
                    if (rd(g,0x50+i)==2) return finish(g, INVALID_STATE);
                if (slots_wait(g)) {
                    g->certified = true;
                    /* With physical boundaries there is no single event to
                     * consume, so the completion clause becomes "at least one
                     * NMI was serviced in this interval".  That preserves the
                     * existing guarantee that an epoch always contains a
                     * serviced NMI: a frame in which nothing was serviced is
                     * not a completed frame, and such an epoch simply pads to
                     * the next boundary and keeps going. */
                    bool serviced = p->physical_boundaries ?
                        g->interval_nmi_entries >= 1 : g->event_consumed;
                    if (p->interval == LOGICAL_NMI_EPOCH && serviced && !g->in_nmi) {
                        g->epoch_done = true;
                        if (idle_to_irq(g, p)) continue;
                        return finish(g, EPOCH_COMPLETE_WAITING);
                    }
                    if (p->interval == DRAIN_TO_WAIT) {
                        if (idle_to_irq(g, p)) continue;
                        return finish(g, WAITING);
                    }
                }
            }
            g->lap = true; g->visited = 0;
        }
        if (!g->cursor.remaining) return finish(g, BUDGET_EXHAUSTED);
        g->certified = false;
        if (pc == 0x80809f || pc == 0x8080c7 || pc == 0x8080e4) g->lap = false;
        if (pc == 0x8080a9 && g->cursor.cpu.y < 32 && !(g->cursor.cpu.y & 7))
            g->visited |= 1u << (g->cursor.cpu.y / 8);
        /* Counters classify opcodes only AFTER interrupt entry is excluded. */
        bool pending = g->cursor.cpu.nmiWanted ||
                       (g->cursor.cpu.irqWanted && !g->cursor.cpu.i);
        if (!pending) {
            if (pc == 0x8080c7 || pc == 0x8080e4) {
                unsigned offset = g->cursor.cpu.y;
                if (offset >= 32 || (offset & 7) || g->cursor.cpu.sp != g->scheduler_s)
                    return finish(g, INVALID_STATE);
                uint16_t ss = rd(g, 0x52+offset) | (rd(g, 0x53+offset) << 8);
                uint16_t s0 = rd(g, 0x54+offset) | (rd(g, 0x55+offset) << 8);
                if (s0 != 0x1cff + 8*offset) return finish(g, INVALID_STATE);
                if (pc == 0x8080e4) {
                    uint8_t selector = rd(g, 0x56+offset);
                    if (ss != s0 || (selector & 1) || selector > 0x16)
                        return finish(g, INVALID_STATE);
                } else if (ss > s0-13) return finish(g, INVALID_STATE);
            }
            if (pc == 0x8080c7 || pc == 0x8080e4) g->dispatches++;
            if (g->task_probe_pc && pc == g->task_probe_pc) g->tasks++;
            if (pc == 0x808356 && g->cursor.cpu.x == 24) g->countdown_passes++;
        }
        g->cursor.last_pc = pc;
        if (!pending) g->cursor.last_opcode = rd(g, pc);
        trace(g, GOOF_BEFORE, 0, 0);
        uint64_t master_before_step = g_cpu.master_cycles;
        g->bus_step.armed = true;
        g->bus_step.accesses = 0;
        g->bus_step.by_speed[0] = g->bus_step.by_speed[1] = g->bus_step.by_speed[2] = 0;
        g->bus_step.master = 0;
        Interp816 before_irq_cpu;
        if (g->cursor.cpu.irqWanted && !g->cursor.cpu.i &&
            !g->cursor.cpu.nmiWanted) before_irq_cpu = g->cursor.cpu;
        InterpCursorResult step = interp_cursor_step(&g->cursor);
        g->bus_step.armed = false;
        /* Charge guest master time exactly ONCE, and only for a step that
         * actually executed something.  interp_cursor_step returns early --
         * without clearing cpu.cyclesUsed -- on INVALID, BUDGET, STOPPED,
         * WAITING, UNSUPPORTED and ERROR, so an unconditional charge here
         * re-applies the PREVIOUS instruction's cycles on those paths.  Only
         * interp816_runOpcode (INSTRUCTION) and interp816_accept_interrupt
         * (NMI) write cyclesUsed, so those are the only two results that may
         * be charged.  master_cycles is the normative physical-boundary
         * clock, so a double charge would move a frame boundary.
         *
         * The price is the step's bus-speed cost (interp_step_master).  A
         * General DMA the step started has already advanced this same clock
         * as a guest stall, inside the step; the refresh tax then covers the
         * whole span, executed and stalled. */
        if ((step == INTERP_CURSOR_INSTRUCTION || step == INTERP_CURSOR_NMI ||
             step == INTERP_CURSOR_IRQ) &&
            g->cursor.cpu.cyclesUsed > 0) {
            uint64_t charge = interp_step_master(
                g, step == INTERP_CURSOR_INSTRUCTION,
                (unsigned)g->cursor.cpu.cyclesUsed);
            g_cpu.master_cycles += charge;
            g->interp_master += charge;
            charge_dram_refresh(g, master_before_step);
        }
        if (g->cursor.cpu.sp < g->min_s) g->min_s = g->cursor.cpu.sp;
        switch (step) {
        case INTERP_CURSOR_NMI:
            g->nmis++; g->nmi_entries++; g->interval_nmi_entries++;
            g->in_nmi = true; g->lap = false;
            trace(g, GOOF_INTERRUPT, 0, 0); break;
        case INTERP_CURSOR_IRQ:
            if (g->in_irq || g->in_nmi) {
                g->irq_duplicate++;
                return finish(g, UNSUPPORTED_EVENT);
            }
            g->irq_entry_count++;
            g->irq_entry_master += g_cpu.master_cycles - master_before_step;
            g->irq_last_entry_master = master_before_step;
            irq_digest_u64(g, master_before_step);
            irq_digest_u64(g, pc);
            irq_digest_u64(g, pc24(g));
            uint64_t delay = master_before_step - g->irq_last_compare_master;
            if (delay > g->irq_max_entry_delay) g->irq_max_entry_delay = delay;
            g->irq_entry_sp = g->cursor.cpu.sp;
            g->irq_before_cpu = before_irq_cpu;
            uint16_t s = g->irq_entry_sp;
            uint8_t pushed_p = g->read(g->bus, (uint16_t)(s + 1));
            uint16_t pushed_pc = g->read(g->bus, (uint16_t)(s + 2)) |
                                 ((uint16_t)g->read(g->bus, (uint16_t)(s + 3)) << 8);
            uint8_t pushed_pb = g->read(g->bus, (uint16_t)(s + 4));
            if (pushed_pc != before_irq_cpu.pc ||
                pushed_pb != before_irq_cpu.k ||
                pushed_p != interp816_getFlags(&before_irq_cpu)) {
                g->irq_stack_failures++;
                return finish(g, EXECUTION_ERROR);
            }
            g->in_irq = true; g->lap = false;
            irq_diag("entry", g, master_before_step, pc);
            trace(g, GOOF_INTERRUPT, 0, 0); break;
        case INTERP_CURSOR_INSTRUCTION:
            if (pc == p->fence_pc) g->fence_visits++;
            if (pc == p->nmi_pc) g->nmi_visits++;
            if (pc == 0x808380 && g->cursor.last_opcode == 0x40 && g->in_nmi) {
                g->epilogues++; g->in_nmi = false;
                release_deferred_boundary_edge(g, p);
            }
            if (pc == 0x8084a8 && g->cursor.last_opcode == 0x40 && g->in_irq) {
                g->in_irq = false;
                if (g->cursor.cpu.sp != (uint16_t)(g->irq_entry_sp + 4))
                    return finish(g, EXECUTION_ERROR);
                Interp816 *before = &g->irq_before_cpu;
                Interp816 *after = &g->cursor.cpu;
                if (after->pc != before->pc || after->k != before->k ||
                    after->sp != before->sp || after->a != before->a ||
                    after->x != before->x || after->y != before->y ||
                    after->dp != before->dp || after->db != before->db ||
                    interp816_getFlags(after) != interp816_getFlags(before)) {
                    g->irq_stack_failures++;
                    return finish(g, EXECUTION_ERROR);
                }
                g->irq_rti_count++;
                irq_digest_u64(g, g_cpu.master_cycles);
                uint64_t duration = g_cpu.master_cycles - g->irq_last_entry_master;
                if (duration > g->irq_max_handler_master)
                    g->irq_max_handler_master = duration;
                irq_diag("rti", g, g_cpu.master_cycles, pc);
            }
            trace(g, GOOF_AFTER, 0, 0); break;
        case INTERP_CURSOR_WAITING: return finish(g, WAITING);
        case INTERP_CURSOR_BUDGET: return finish(g, BUDGET_EXHAUSTED);
        case INTERP_CURSOR_UNSUPPORTED: return finish(g, UNSUPPORTED_EVENT);
        case INTERP_CURSOR_INVALID: return finish(g, INVALID_STATE);
        default: return finish(g, EXECUTION_ERROR);
        }
    }
}

/* Nonrecursive ownership boundary, measurable even in an unoptimized build. */
RunResult goof_run_frame(GuestExecution *g, const FramePlan *p, SafetyBudget budget) {
    if (!g || !p || g->host_depth) return (RunResult){.reason=INVALID_STATE};
    g->host_depth++;
    if (g->host_depth > g->max_host_depth) g->max_host_depth = g->host_depth;
    RunResult result = run_interval(g, p, budget);
    g->host_depth--;
    return result;
}
