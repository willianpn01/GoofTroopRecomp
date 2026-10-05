/*
 * EXPERIMENTAL, uncommitted (Phase 5.5 safety investigation, 2026-07-29).
 *
 * Standalone harness (no game / no ROM / no cfg), following the exact pattern
 * of tests/interp816/bridge_test.c: a flat-RAM bus + fakes for the symbols
 * interp_bridge.c references, so _interp_run_core's stopping conditions can
 * be exercised deterministically in isolation.
 *
 * NOTE ON A PRE-EXISTING GAP: at the time this file was written,
 * tests/interp816/run.sh's own Phase-1 build (bridge_test.c) FAILED TO LINK
 * against the current runner/src/snes/interp_bridge.c — it is missing stubs
 * for cpu_dispatch_pc_paired, g_interp_apu_driving, RtlApuLock/Unlock, g_snes,
 * g_apu_last_sync_master, snes_catchupApu, cpu_read16/write16, and
 * cpu_take_tailcall_return_context, all of which interp_bridge.c now
 * references unconditionally. This is baseline debt that predates this
 * session's change (confirmed by running tests/interp816/run.sh unmodified
 * before touching interp_bridge.c: Phase 0 passes, Phase 1 fails to link with
 * exactly those undefined references). This file provides its own superset of
 * stubs rather than editing bridge_test.c, so that pre-existing breakage is
 * left exactly as found and not conflated with this session's own testing.
 * See ENGINE_EXTENSION_SAFETY.md.
 *
 * Purpose: two things, and only two things:
 *   (1) REGRESSION — prove the pre-existing yield_pc (spin-on-flag) stopping
 *       mode is untouched by the conv_pc addition, on a synthetic MMX-shaped
 *       loop.
 *   (2) NEW MODE — prove the new conv_pc (lap-convergence) stopping mode
 *       correctly (a) does not stop mid-lap / under-process, and (b) detects
 *       real convergence only after a lap where nothing changed and the flag
 *       is clear, on a synthetic Goof-shaped loop (restart-on-flag, multiple
 *       watched slot bytes, flag cleared at lap START not lap END).
 *
 * This is a unit-level proof, not a full-game boot smoke test. A true SMW/MMX
 * boot smoke test against the current engine baseline is currently blocked by
 * a separate, already-documented pre-existing condition (RISK_REGISTER.md
 * R11/R12: neither reference regenerates cleanly on this exact snesrecomp
 * commit). See ENGINE_EXTENSION_SAFETY.md for why that gate could not be
 * exercised this session and what this test substitutes for it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "interp_bridge.h"

#define MEMSZ 0x1000000u
static uint8_t *RAM;

/* ── stubs interp_bridge.c references (superset of bridge_test.c's) ───── */
int snes_frame_counter = 0;
int g_interp_apu_driving = 0;
uint64_t g_apu_last_sync_master = 0;
typedef struct Snes { double apuCatchupCycles; } Snes;
static Snes s_fake_snes;
Snes *g_snes = &s_fake_snes;
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
void snes_catchupApu(Snes *snes) { (void)snes; }
int cpu_take_tailcall_return_context(uint16_t *entry_s, uint8_t *hrv) {
    (void)entry_s; (void)hrv; return 0;
}

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {
    (void)cpu; return RAM[(((uint32)bank << 16) | addr) & 0xFFFFFF];
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 v) {
    (void)cpu; RAM[(((uint32)bank << 16) | addr) & 0xFFFFFF] = v;
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 addr) {
    uint16 lo = cpu_read8(cpu, bank, addr);
    uint16 hi = cpu_read8(cpu, bank, (uint16)(addr + 1));
    return (uint16)(lo | (hi << 8));
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 addr, uint16 v) {
    cpu_write8(cpu, bank, addr, (uint8)(v & 0xFF));
    cpu_write8(cpu, bank, (uint16)(addr + 1), (uint8)(v >> 8));
}
/* For Tests A-D: no compiled bodies registered at all; every JSR/JSL
 * interprets straight through. This isolated the stopping-condition change
 * from the bounce/unwind machinery for those tests.
 *
 * For Test E (added after the user asked for the exact detail behind the
 * "one disclosed exception," which prompted a closer read of every place
 * `yield_pc` gates behavior, not just the two lines already disclosed): one
 * fake "compiled" entry can be armed at FAKE_YIELD_ENTRY. When armed, a JSR
 * to it does NOT run normally — it calls the real
 * interp_bridge_lle_yield_unwind(), exactly as a real hle yield stub would
 * under interp_bridge_in_lle_scheduler()==true, modelling the actual
 * production path a Goof yield primitive's stub would take. Disarmed
 * (default 0) for Tests A-D, so their behavior is unchanged by this
 * addition. */
#define FAKE_YIELD_ENTRY 0x008100u
static int      g_yield_entry_enabled = 0;
static uint32_t g_yield_resume_pc24   = 0;
static int      g_yield_stub_called   = 0;
int cpu_dispatch_has_entry(CpuState *cpu, uint32 pc24) {
    (void)cpu;
    return g_yield_entry_enabled && (pc24 & 0xFFFFFF) == FAKE_YIELD_ENTRY;
}
RecompReturn cpu_dispatch_pc(CpuState *cpu, uint32 pc24, uint16 miss_restore) {
    (void)pc24; cpu->S = miss_restore; return RECOMP_RETURN_NORMAL;
}
RecompReturn cpu_dispatch_pc_paired(CpuState *cpu, uint32 pc24, uint8 frame_size) {
    (void)frame_size;
    if (g_yield_entry_enabled && (pc24 & 0xFFFFFF) == FAKE_YIELD_ENTRY) {
        g_yield_stub_called++;
        /* Mirrors gen_stubs.c's real pattern (LLE_SCHEDULER.md:62-71): test
         * the scheduler-frame predicate, then arm the unwind to the
         * primitive's real ROM entry instead of returning normally. */
        if (interp_bridge_in_lle_scheduler())
            return interp_bridge_lle_yield_unwind(cpu, g_yield_resume_pc24);
        return RECOMP_RETURN_NORMAL;
    }
    return RECOMP_RETURN_NORMAL;
}
RecompReturn cpu_unresolved_abandon_balanced(CpuState *cpu, uint32 site_pc24,
                                             uint16 entry_s, uint8 hrv) {
    (void)site_pc24; cpu->S = (uint16)(entry_s + hrv); return RECOMP_RETURN_NORMAL;
}

static int g_fail = 0, g_check = 0;
#define CHECK(cond, ...) do { g_check++; if (!(cond)) { \
    g_fail++; printf("    FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static CpuState g_c;
static void init_cpu(void) {
    memset(&g_c, 0, sizeof g_c);
    g_c.S = 0x1FFF; g_c.emulation = 1; g_c.m_flag = 1; g_c.x_flag = 1;
    g_c._flag_I = 1; g_c.ram = RAM; cpu_mirrors_to_p(&g_c);
}
static void load(uint32 pc24, const uint8_t *code, int len) {
    memcpy(&RAM[pc24 & 0xFFFFFF], code, (size_t)len);
}

int main(void) {
    RAM = malloc(MEMSZ);

    /* ── Test A (REGRESSION): synthetic MMX-shaped spin-on-flag loop ──────
     * $008000: INC $10 ; BRA $8000   (flag cleared right before it would
     * genuinely idle would be the ROM's job; here we just prove yield_pc
     * mode's PC+flag semantics are bit-for-bit what they were before this
     * session's patch: stop the FIRST time PC==yield_pc with flag byte==0,
     * having executed the loop body each time it wasn't yet 0.)
     * Flag starts at 0xFE so the loop must execute exactly twice (0xFE->0xFF,
     * 0xFF->0x00) before the third arrival at $8000 sees byte($10)==0 and
     * stops — this pins down the exact existing contract with a concrete,
     * checkable number, not just "it returns something". */
    { memset(RAM, 0, MEMSZ); init_cpu();
      RAM[0x10] = 0xFE;
      uint8_t c[] = { 0xE6, 0x10,        /* INC $10 (dp) */
                      0x80, 0xFC };      /* BRA $8000 */
      load(0x8000, c, sizeof c);
      int rc = interp_bridge_run_scheduler(&g_c, 0x008000, 0x008000, 0x0010);
      printf("A: yield_pc regression (spin-on-flag)\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(RAM[0x10] == 0x00, "flag=%02X exp 00 (two increments then stop)", RAM[0x10]);
    }

    /* ── Test B (NEW MODE): synthetic Goof-shaped restart-on-flag scan ────
     * Layout (bank 0, all 8-bit ops, emulation mode like init_cpu()):
     *   $0020        "new NMI" flag (Goof's $9B analog) — CLEARED AT LAP
     *                START, not lap end (the structural difference this
     *                whole investigation is about).
     *   $0030,$0031  two watched "slot state" bytes (Goof's $50,Y analog).
     *   $0040        lap counter, used only to script the test's own state
     *                changes (not part of the mechanism under test).
     *
     *   $9000: LDA $20        ; AD 20 00   check flag
     *          BEQ $9008      ; F0 04      if already 0, skip clear
     *          STZ $20        ; 9C 20 00   clear it (lap start, Goof-style)
     *   $9008: (conv_pc — reached exactly once per lap)
     *          LDA $40        ; AD 40 00   lap counter
     *          INC A          ; 1A
     *          STA $40        ; 8D 40 00
     *          CMP #$01       ; C9 01      first lap only: mutate slot0
     *          BNE $9016      ; D0 04
     *          INC $30        ; E6 30      slot0: 0x00 -> 0x01 (simulates a
     *                                       task becoming ready this lap)
     *   $9016: LDA $30        ; AD 30 00   "process slot 0" (placeholder read)
     *          LDA $31        ; AD 31 00   "process slot 1" (placeholder read)
     *          BRA $9000      ; 80 E6
     *
     * Expected: lap 1 mutates slot0 (0->1), so the conv check at the START of
     * lap 1 has no prior snapshot (first visit, never stops); at the START of
     * lap 2 the snapshot from lap 1's visit (post-mutation value, since the
     * snapshot is taken AFTER the mutation code hasn't run yet at the moment
     * of the $9008 check — see the precise ordering assertion in the CHECK
     * below) is compared; lap 2 does NOT mutate slot0 (lap counter != 1), so
     * slot0/slot1 are stable from lap 2's start-of-lap snapshot onward, and
     * the engine must run at least lap 2 AND lap 3 (to observe two identical
     * consecutive snapshots) before stopping — never stopping after only
     * partial processing of a lap, and never stopping while state is still
     * changing lap-to-lap. */
    { memset(RAM, 0, MEMSZ); init_cpu();
      RAM[0x20] = 0x01;  /* flag set: "an NMI already landed", like boot */
      RAM[0x30] = 0x00; RAM[0x31] = 0x00; RAM[0x40] = 0x00;
      uint8_t c[] = {
          /* $9000 */ 0xAD, 0x20, 0x00,             /* LDA $0020 */
          /* $9003 */ 0xF0, 0x04,                   /* BEQ $9009 (+4) */
          /* $9005 */ 0x9C, 0x20, 0x00,              /* STZ $0020 */
          /* $9008 */ 0xEA,                          /* NOP (pad so $9009 is exact) */
          /* $9009: conv_pc */
                      0xAD, 0x40, 0x00,             /* LDA $0040 */
                      0x1A,                          /* INC A */
                      0x8D, 0x40, 0x00,             /* STA $0040 */
                      0xC9, 0x01,                    /* CMP #$01 */
                      0xD0, 0x04,                    /* BNE +4 */
                      0xE6, 0x30,                    /* INC $0030 */
          /* processing */
                      0xAD, 0x30, 0x00,             /* LDA $0030 */
                      0xAD, 0x31, 0x00,             /* LDA $0031 */
                      0x80, 0xDA,                    /* BRA $9000 */
      };
      load(0x9000, c, sizeof c);
      const uint16_t watch[2] = { 0x0030, 0x0031 };
      int lap_count_before = 0; (void)lap_count_before;
      int rc = interp_bridge_run_scheduler_conv(&g_c, 0x009000, 0x009009,
                                                watch, 2, 0x0020);
      printf("B: conv_pc new mode (restart-on-flag, multi-slot)\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(RAM[0x30] == 0x01, "slot0=%02X exp 01 (mutated once on lap 1, then stable)", RAM[0x30]);
      /* Ground truth, verified by running this harness (not predicted ahead
       * of time — an earlier version of this assertion guessed >=3 and was
       * wrong; corrected against the actual, inspected trace below).
       * conv_pc (=$9009) is checked BEFORE executing the code that follows
       * it, so the snapshot taken at a visit reflects state as of the START
       * of that lap's body, not its end:
       *   lap 1 visit: no prior snapshot -> record (slot0=0,slot1=0), run
       *                lap 1's body, which mutates slot0 0->1 at its end.
       *   lap 2 visit: compare current (slot0=1,slot1=0) vs lap-1 snapshot
       *                (0,0) -> DIFFERENT (correctly not fooled by only
       *                slot1 matching) -> record (1,0), run lap 2's body
       *                (lap_counter now 2, CMP#1 fails, slot0 untouched).
       *   lap 3 visit: compare current (1,0) vs lap-2 snapshot (1,0) ->
       *                MATCH and flag clear -> STOP, before lap 3's own body
       *                (which would have bumped the counter to 3) executes.
       * So the counter reads 2 at the stop, and TWO consecutive matching
       * lap-start snapshots were required — proving both that a single
       * still-differing lap (2) did not cause a false stop, and that
       * convergence was detected as soon as it genuinely held, not later. */
      CHECK(RAM[0x40] == 0x02, "lap_counter=%d exp 02 (see comment: stops at lap-3's visit, before lap 3's body runs)", RAM[0x40]);
      CHECK(RAM[0x20] == 0x00, "flag=%02X exp 00 (clear at convergence)", RAM[0x20]);
    }

    /* ── Test C (NEW MODE, non-convergence guard): confirm it does NOT
     * falsely stop after only the first watched byte's worth of "progress" —
     * i.e. it never mistakes "one of several slots checked" for "a full lap
     * with nothing changed". Same loop as B, but slot1 keeps incrementing
     * forever (models a slot that never settles); the engine must NOT return
     * 1 and must instead hit the step cap (rc==0), because convergence is
     * genuinely never reached — proving the check compares the WHOLE watched
     * set, not a subset, and doesn't paper over an always-changing slot. */
    { memset(RAM, 0, MEMSZ); init_cpu();
      RAM[0x20] = 0x00; RAM[0x30] = 0x00; RAM[0x31] = 0x00; RAM[0x40] = 0x00;
      uint8_t c[] = {
          /* $9000 */ 0xAD, 0x20, 0x00,
          /* $9003 */ 0xF0, 0x04,
          /* $9005 */ 0x9C, 0x20, 0x00,
          /* $9008 */ 0xEA,
          /* $9009: conv_pc */
                      0xE6, 0x31,                    /* INC $0031 — every lap, forever */
                      0xAD, 0x30, 0x00,
                      0xAD, 0x31, 0x00,
                      0x80, 0xF4,                    /* BRA $9000 */
      };
      load(0x9000, c, sizeof c);
      const uint16_t watch[2] = { 0x0030, 0x0031 };
      int rc = interp_bridge_run_scheduler_conv(&g_c, 0x009000, 0x009009,
                                                watch, 2, 0x0020);
      printf("C: conv_pc never-converges guard (step-cap bail expected)\n");
      CHECK(rc == 0, "rc=%d exp 0 (must bail via step cap, not falsely converge)", rc);
    }

    /* ── Test D (REAL ROM grounding, regression): the actual MMX scheduler's
     * spin bytes at $00:8099-$00:80A6, read directly from the user's
     * mmx.sfc (not synthesized), loaded verbatim. Confirms yield_pc mode's
     * PC+flag arithmetic still lands on the exact real address the ROM uses
     * for its spin ($0080A1, per snesrecomp/docs/LLE_SCHEDULER.md:52-56 and
     * independently confirmed here by decoding the fetched bytes), using
     * genuine dense 65816 bytes rather than a hand-written loop. Kept
     * deliberately bounded to the self-contained spin (flag held at 0, so
     * control never advances past the loaded bytes into unmapped memory) —
     * this is a stability/address-correctness check on real ROM bytes, not a
     * claim about MMX's full scheduler semantics (Test A/B/C already cover
     * the mechanism). If mmx.sfc isn't found at the expected investigation
     * path, this test is skipped (reported, not silently ignored) rather
     * than failing the whole run on an environment difference. */
    {
        FILE *f = fopen("../../mmx.sfc", "rb");
        if (!f) f = fopen("../mmx.sfc", "rb");
        if (!f) f = fopen("mmx.sfc", "rb");
        if (!f) {
            printf("D: real-ROM grounding SKIPPED (mmx.sfc not found from cwd)\n");
        } else {
            uint8_t rombuf[64];
            /* LoROM bank-0 offset for $8099 is addr-$8000 (headerless dump,
             * confirmed 1,572,864 bytes = clean 48 x 32 KiB, no copier header). */
            const uint32_t rom_off = 0x8099 - 0x8000;
            fseek(f, (long)rom_off, SEEK_SET);
            size_t got = fread(rombuf, 1, sizeof rombuf, f);
            fclose(f);
            CHECK(got >= 8, "read %zu bytes from mmx.sfc, need >=8", got);
            /* Decode-time self-check: REP #$10, LDX #$02FF, TXS, SEP #$30 must
             * precede the spin, exactly as LLE_SCHEDULER.md describes. If this
             * doesn't hold the ROM/offset assumption is wrong and the rest of
             * the test would be meaningless — fail loudly instead of
             * proceeding on a bad premise. */
            CHECK(rombuf[0]==0xC2 && rombuf[1]==0x10 &&
                  rombuf[2]==0xA2 && rombuf[3]==0xFF && rombuf[4]==0x02 &&
                  rombuf[5]==0x9A && rombuf[6]==0xE2 && rombuf[7]==0x30,
                  "unexpected bytes at mmx.sfc+0x%X: %02X %02X %02X %02X %02X %02X %02X %02X "
                  "(expected REP#$10,LDX#$02FF,TXS,SEP#$30 — offset/ROM assumption wrong)",
                  rom_off, rombuf[0],rombuf[1],rombuf[2],rombuf[3],rombuf[4],rombuf[5],rombuf[6],rombuf[7]);

            /* This fragment runs mid-reset, AFTER the ROM's earlier XCE has
             * already left emulation mode (native mode, E=0) — unlike Tests
             * A-C's self-contained 8-bit loops, which deliberately start in
             * emulation mode. Reusing init_cpu()'s emulation=1 here produced
             * a first FAIL (rc=0): in real emulation mode M/X are hardwired
             * 8-bit regardless of REP, so `REP #$10; LDX #$02FF` misdecodes
             * (LDX takes only one immediate byte, the second byte is read as
             * a separate bogus opcode) and the spin PC is never reached.
             * That was this test's own setup bug, not a finding about the
             * engine — corrected by starting in native mode, matching where
             * the ROM actually is at this PC. */
            memset(RAM, 0, MEMSZ); init_cpu();
            g_c.emulation = 0; cpu_mirrors_to_p(&g_c);
            load(0x008099, rombuf, (int)got);
            RAM[0x0B9D] = 0x00;  /* flag already clear: spin's precondition to stop immediately */
            int rc = interp_bridge_run_scheduler(&g_c, 0x008099, 0x0080A1, 0x0B9D);
            printf("D: real MMX ROM bytes, yield_pc mode (regression, real bytes)\n");
            CHECK(rc == 1, "rc=%d exp 1 (real ROM spin PC/flag matched immediately)", rc);
        }
    }

    /* ── Test E (bounce + LLE yield-unwind under conv_pc mode) ───────────
     * Added after being asked to detail the "one disclosed exception"
     * precisely — re-reading every `yield_pc`-gated branch (not just the two
     * already disclosed at the s_lle_sched_depth counter) surfaced a THIRD,
     * more serious one: interp_bridge.c's unwind-consumption branch (the
     * `if (s_lle_unwind_active) { if (yield_pc) { ...consume, resume at real
     * entry... } ... return 1; }` block) checks the literal `yield_pc`
     * parameter, which is always 0 under conv_pc mode. Tests A-D never
     * exercised this because their cpu_dispatch_has_entry always returned 0
     * (no compiled bodies registered at all -> no bounce ever happens -> this
     * branch is dead code in those tests). This test registers ONE fake
     * compiled entry that behaves like a real hle yield stub (calls
     * interp_bridge_lle_yield_unwind exactly as gen_stubs.c does), so the
     * bounce+unwind path is actually exercised under conv_pc mode for the
     * first time.
     *
     * Loop at $9100 (separate from Test B/C's $9000 to avoid any possible
     * PC collision): same restart-on-flag shape as Test B (including the
     * "mutate the watched byte on lap 1 only" trick, so convergence cannot
     * possibly happen after a single lap regardless of whether the fix
     * works — see the note below about this test's FIRST version, which did
     * not have this and could not actually distinguish "fixed" from "still
     * broken"), but each lap does a real JSR $8100 (the fake yield
     * primitive) instead of a placeholder read.
     *
     * IMPORTANT CORRECTION, kept visible rather than silently fixed: this
     * test's first version watched a byte that never changed, so it
     * legitimately converged after exactly ONE completed lap — meaning
     * g_yield_stub_called==1 was the CORRECT outcome for both "fixed" and
     * "still broken" (the bug also produces stub_called==1, for the wrong
     * reason). That version's FAIL was real (it caught the bug), but a
     * hypothetical re-run after the fix would have produced a PASSING
     * result that didn't actually prove the fix worked — it would have
     * passed by coincidence. Rewritten so the watched byte mutates on lap 1,
     * forcing convergence to require two consecutive matching snapshots
     * (lap 2's start vs lap 3's start, exactly Test B's already-verified
     * timing), which needs the JSR/unwind/resume cycle to complete
     * successfully TWICE before the run can end. Expected: rc==1,
     * g_yield_stub_called==2 (matching Test B's identical lap arithmetic),
     * lap_counter==2. If the yield_pc-only gate bug is reintroduced, the
     * first bounce ends the whole run immediately: g_yield_stub_called
     * stays at 1 and lap_counter stays at 1. */
    { memset(RAM, 0, MEMSZ); init_cpu();
      RAM[0x20] = 0x01; RAM[0x30] = 0x00; RAM[0x40] = 0x00;
      g_yield_entry_enabled = 1; g_yield_resume_pc24 = 0x009119; g_yield_stub_called = 0;
      uint8_t c[] = {
          /* $9100 */ 0xAD, 0x20, 0x00,   /* LDA $0020 */
          /* $9103 */ 0xF0, 0x04,          /* BEQ +4 -> $9109 */
          /* $9105 */ 0x9C, 0x20, 0x00,   /* STZ $0020 */
          /* $9108 */ 0xEA,                /* NOP */
          /* $9109: conv_pc */
                      0xAD, 0x40, 0x00,   /* LDA $0040 (lap counter) */
                      0x1A,                /* INC A */
                      0x8D, 0x40, 0x00,   /* STA $0040 */
                      0xC9, 0x01,          /* CMP #$01 */
                      0xD0, 0x02,          /* BNE +2 -> $9116 (skip mutate) */
                      0xE6, 0x30,          /* INC $0030 (lap-1-only mutate) */
          /* $9116: */
                      0x20, 0x00, 0x81,   /* JSR $8100 (FAKE_YIELD_ENTRY) */
          /* $9119: resume_pc24 target */
                      0xAD, 0x30, 0x00,   /* LDA $0030 */
                      0x80, 0xE2,          /* BRA $9100 */
      };
      load(0x9100, c, sizeof c);
      const uint16_t watch[1] = { 0x0030 };
      int rc = interp_bridge_run_scheduler_conv(&g_c, 0x009100, 0x009109,
                                                watch, 1, 0x0020);
      printf("E: bounce + LLE yield-unwind under conv_pc mode\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_yield_stub_called == 2,
            "yield_stub_called=%d exp 2 (two full laps must complete via proper "
            "unwind-consume-and-resume before genuine convergence; ==1 would mean "
            "the yield_pc-only gate bug terminated the run after the FIRST bounce)",
            g_yield_stub_called);
      CHECK(RAM[0x40] == 0x02, "lap_counter=%d exp 02 (matches Test B's identical timing)", RAM[0x40]);
      g_yield_entry_enabled = 0; /* disarm: don't affect any test added after this one */
    }

    /* ── Test F (SMW real-ROM grounding, regression) ──────────────────────
     * Added because the user pointed out Test D only covered MMX real-ROM
     * bytes — SMW was never exercised this way, and "zero behavioral change"
     * should not be claimed for a baseline that was never actually checked.
     * Same purpose and same bounded-scope caveat as Test D: real SMW ROM
     * bytes at $00:806B, read directly from the user's smw.sfc (SlowROM
     * LoROM: bank-0 offset for $806B is addr-$8000, same formula as MMX's
     * LoROM), fed through the unmodified interp_bridge_run_scheduler.
     * smw_rtl.c documents this exact loop: "$806B: LDA $10 ; BEQ $806B ;
     * wait for vblank ... CLI ; INC $13 ; JSR ProcessGameMode ; STZ $10 ;
     * BRA $806B" (smw_rtl.c:113-115); self-checked below by asserting the
     * fetched bytes decode as LDA $10 (A5 10) followed by BEQ -4 (F0 FC)
     * before proceeding, so a wrong offset/assumption fails loudly. Flag
     * held clear so the run stops immediately without executing into
     * ProcessGameMode (unmapped in this bare harness — deliberately not
     * exercised, for the same reason Test D stayed bounded to the spin). */
    {
        FILE *f = fopen("../../smw.sfc", "rb");
        if (!f) f = fopen("../smw.sfc", "rb");
        if (!f) f = fopen("smw.sfc", "rb");
        if (!f) {
            printf("F: SMW real-ROM grounding SKIPPED (smw.sfc not found from cwd)\n");
        } else {
            uint8_t rombuf[16];
            const uint32_t rom_off = 0x806B - 0x8000;
            fseek(f, (long)rom_off, SEEK_SET);
            size_t got = fread(rombuf, 1, sizeof rombuf, f);
            fclose(f);
            CHECK(got >= 4, "read %zu bytes from smw.sfc, need >=4", got);
            CHECK(rombuf[0]==0xA5 && rombuf[1]==0x10 &&
                  rombuf[2]==0xF0 && rombuf[3]==0xFC,
                  "unexpected bytes at smw.sfc+0x%X: %02X %02X %02X %02X "
                  "(expected LDA $10, BEQ $806B — offset/ROM assumption wrong)",
                  rom_off, rombuf[0],rombuf[1],rombuf[2],rombuf[3]);

            memset(RAM, 0, MEMSZ); init_cpu();
            g_c.emulation = 0; cpu_mirrors_to_p(&g_c); /* native mode by this point, like Test D's MMX fragment */
            load(0x00806B, rombuf, (int)got);
            RAM[0x0010] = 0x00;  /* flag already clear: spin's precondition to stop immediately */
            int rc = interp_bridge_run_scheduler(&g_c, 0x00806B, 0x00806B, 0x0010);
            printf("F: real SMW ROM bytes, yield_pc mode (regression, real bytes)\n");
            CHECK(rc == 1, "rc=%d exp 1 (real SMW ROM spin PC/flag matched immediately)", rc);
        }
    }

    printf("\n==== sched_conv_test (Phase 5.5 experimental): %d/%d checks passed ====\n",
           g_check - g_fail, g_check);
    if (g_fail) { printf("RESULT: FAIL (%d)\n", g_fail); return 1; }
    printf("RESULT: PASS\n");
    return 0;
}
