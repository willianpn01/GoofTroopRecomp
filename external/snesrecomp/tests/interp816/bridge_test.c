/*
 * interp_bridge Phase-1 contract harness (no game / no ROM).
 *
 * Proves the interp<->AOT bridge mechanics deterministically with fakes:
 *   - cpu_read8/cpu_write8  -> a flat RAM (the bus the bridge routes through);
 *   - cpu_dispatch_pc / cpu_dispatch_has_entry -> ONE known "compiled" entry
 *     whose fake body mutates A and pops its return frame (modelling a real
 *     AOT function's RTS: pop frame, dispatch-miss on return addr, S restored).
 *
 * Scenarios:
 *   S1: interp routine that JSRs into the compiled entry -> the bounce runs the
 *       compiled body, state syncs, stack stays balanced, resume at return addr.
 *   S2: pure interp routine (no call) -> exits balanced, no bounce.
 *   S3: interp routine that JSRs a NON-compiled target -> interpreted through,
 *       its RTS returns to caller level (no premature exit), final RTS exits.
 *
 * Build/run: tests/interp816/run.sh (WSL gcc). Validation only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "interp_bridge.h"   /* -> cpu_state.h (types, inline frame helpers) */

#define MEMSZ 0x1000000u
static uint8_t *RAM;
static uint8_t *ROM512;
static int g_lorom_map;
static int      g_aot_called;
#define FAKE_AOT 0x008100u
#define FAKE_AOT_LONG 0x818100u
static int g_fake_host_depth, g_fake_host_peak;
static uint8_t g_fake_seen_pb, g_fake_seen_hrv;

/* ── fakes the bridge links against (cpu_state.c provides these in prod) ── */
/* The Phase-2 manifest recorder stamps the live frame counter on each
 * discovery; the bridge references it as extern. */
int snes_frame_counter = 0;
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {
    (void)cpu;
    if (g_lorom_map && addr >= 0x8000)
        return ROM512[((((uint32)bank & 0x7F) << 15) | (addr & 0x7FFF)) & 0x7FFFF];
    return RAM[(((uint32)bank << 16) | addr) & 0xFFFFFF];
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 v) {
    (void)cpu; RAM[(((uint32)bank << 16) | addr) & 0xFFFFFF] = v;
}
int cpu_dispatch_has_entry(CpuState *cpu, uint32 pc24) {
    (void)cpu; pc24 &= 0xFFFFFF;
    return pc24 == FAKE_AOT || pc24 == FAKE_AOT_LONG;
}
static int g_abandon_called;
RecompReturn cpu_unresolved_abandon_balanced(CpuState *cpu, uint32 site_pc24,
                                             uint16 entry_s, uint8 hrv) {
    (void)site_pc24; g_abandon_called++;
    cpu->S = (uint16)(entry_s + hrv);
    return RECOMP_RETURN_NORMAL;
}
RecompReturn cpu_dispatch_pc(CpuState *cpu, uint32 pc24, uint16 miss_restore) {
    if ((pc24 & 0xFFFFFF) == FAKE_AOT_LONG) {
        g_aot_called++;
        g_fake_host_depth++;
        if (g_fake_host_depth > g_fake_host_peak) g_fake_host_peak = g_fake_host_depth;
        cpu->A = (uint16)(cpu->A + 0x0200);
        g_fake_seen_pb = cpu->PB;
        g_fake_seen_hrv = cpu->host_return_valid;
        cpu->PB = RAM[(cpu->S + 3) & 0xFFFF]; /* model RTL's PB pull */
        cpu->S = (uint16)(cpu->S + 3);
        g_fake_host_depth--;
        return RECOMP_RETURN_NORMAL;
    }
    if ((pc24 & 0xFFFFFF) == FAKE_AOT) {
        g_aot_called++;
        cpu->A = (uint16)(cpu->A + 0x0100);     /* observable "compiled" work */
        cpu->S = (uint16)(cpu->S + 2);          /* models RTS popping its frame */
        return RECOMP_RETURN_NORMAL;
    }
    cpu->S = miss_restore;
    return RECOMP_RETURN_NORMAL;
}

static int g_fail = 0, g_check = 0;
#define CHECK(cond, ...) do { g_check++; if (!(cond)) { \
    g_fail++; printf("    FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static CpuState g_c;
static void init_cpu(void) {
    memset(&g_c, 0, sizeof g_c);
    g_c.S = 0x01FF; g_c.emulation = 1; g_c.m_flag = 1; g_c.x_flag = 1;
    g_c._flag_I = 1; g_c.ram = RAM; cpu_mirrors_to_p(&g_c);
}
static void load(uint32 pc24, const uint8_t *code, int len) {
    memcpy(&RAM[pc24 & 0xFFFFFF], code, (size_t)len);
}

int main(void) {
    RAM = malloc(MEMSZ);
    ROM512 = malloc(0x80000);

    /* S1: LDA #$01 ; JSR $8100 (compiled) ; RTS */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      uint8_t c[] = {0xA9,0x01, 0x20,0x00,0x81, 0x60};
      load(0x8000, c, sizeof c);
      cpu_push_jsr_return_frame(&g_c);          /* sentinel return frame */
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S1 JSR-into-compiled bounce\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 1, "aot_called=%d exp 1", g_aot_called);
      CHECK(g_c.A == 0x0101, "A=%04X exp 0101 (01 from LDA + 0100 from AOT)", g_c.A);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (balanced)", g_c.S); }

    /* S2: LDA #$09 ; RTS  (no call) */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      uint8_t c[] = {0xA9,0x09, 0x60};
      load(0x8000, c, sizeof c);
      cpu_push_jsr_return_frame(&g_c);
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S2 pure interp routine\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 0, "aot_called=%d exp 0", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x09, "A.lo=%02X exp 09", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF", g_c.S); }

    /* S3: JSR $8200 (NOT compiled) ; RTS  /  $8200: LDA #$33 ; RTS */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      uint8_t caller[] = {0x20,0x00,0x82, 0x60};
      uint8_t callee[] = {0xA9,0x33, 0x60};
      load(0x8000, caller, sizeof caller);
      load(0x8200, callee, sizeof callee);
      cpu_push_jsr_return_frame(&g_c);
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S3 interpret-through non-compiled call\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 0, "aot_called=%d exp 0 (no compiled body)", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x33, "A.lo=%02X exp 33", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (balanced through nested RTS)", g_c.S); }

    /* S4: interp_tier_dispatch (the production tier-down entry, tail-dispatch
     * shape): a caller frame is on the stack (as after a JSR into the
     * dispatcher); the dispatched target runs and RTSes past entry. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      uint8_t c[] = {0xA9,0x07, 0x60};      /* $8000: LDA #$07 ; RTS */
      load(0x8000, c, sizeof c);
      long hits0 = interp_tier_hit_count();
      cpu_push_jsr_return_frame(&g_c);       /* the (inherited) caller frame */
      RecompReturn r = interp_tier_dispatch(&g_c, 0x008000);
      printf("S4 interp_tier_dispatch (tail-dispatch entry)\n");
      CHECK(r == RECOMP_RETURN_NORMAL, "r=%d exp NORMAL(0)", (int)r);
      CHECK((g_c.A & 0xFF) == 0x07, "A.lo=%02X exp 07", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (caller frame consumed)", g_c.S);
      CHECK(interp_tier_hit_count() == hits0 + 1, "hit_count delta exp 1"); }

    /* S5: interp_tier_dispatch_balanced (SM abandon-site upgrade). A clean
     * routine interprets to completion -> NORMAL, balanced, abandon NOT used. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0; g_abandon_called = 0;
      uint8_t c[] = {0xA9,0x0C, 0x60};      /* $8000: LDA #$0C ; RTS */
      load(0x8000, c, sizeof c);
      cpu_push_jsr_return_frame(&g_c);       /* inherited caller frame (hrv=2) */
      uint16 entry_s = g_c.S;                /* function entry S = after caller's push */
      RecompReturn r = interp_tier_dispatch_balanced(&g_c, 0x008000, 0x00C0DE,
                                                     entry_s, 2);
      printf("S5 interp_tier_dispatch_balanced (clean -> interpret, no abandon)\n");
      CHECK(r == RECOMP_RETURN_NORMAL, "r=%d exp NORMAL", (int)r);
      CHECK((g_c.A & 0xFF) == 0x0C, "A.lo=%02X exp 0C (interpreted)", g_c.A & 0xFF);
      CHECK(g_abandon_called == 0, "abandon_called=%d exp 0 (clean interp)", g_abandon_called);
      CHECK(g_c.S == (uint16)(entry_s + 2), "S=%04X exp %04X (frame popped)", g_c.S, (uint16)(entry_s + 2)); }

    /* PB1: the balanced production tier must preserve logical PB across a
     * long call.  The flat test bus deliberately supplies equal bytes in the
     * low mirrors so a lookup/mapping implementation may normalize physical
     * storage, but PHK must still observe the guest-visible $81/$80 banks. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0; g_abandon_called = 0;
      uint8_t caller[] = {0x22,0x00,0x90,0x81, /* JSL $81:9000 */
                          0x4B,0x68,0x8D,0x01,0x01, /* PHK; PLA; STA $0101 */
                          0x60};                    /* RTS */
      uint8_t callee[] = {0x4B,0x68,0x8D,0x00,0x01,0x6B};
      load(0x808000, caller, sizeof caller);
      load(0x008000, caller, sizeof caller);
      load(0x819000, callee, sizeof callee);
      load(0x019000, callee, sizeof callee);
      cpu_push_jsr_return_frame(&g_c);
      uint16 entry_s = g_c.S;
      RecompReturn r = interp_tier_dispatch_balanced(&g_c, 0x808000, 0x80C0DE,
                                                     entry_s, 2);
      printf("PB1 balanced tier logical PB across JSL/RTL\n");
      CHECK(r == RECOMP_RETURN_NORMAL, "r=%d exp NORMAL", (int)r);
      CHECK(RAM[0x0100] == 0x81, "PHK callee=%02X exp 81", RAM[0x0100]);
      CHECK(RAM[0x0101] == 0x80, "PHK caller=%02X exp 80", RAM[0x0101]);
      CHECK(g_c.PB == 0x80, "PB=%02X exp 80", g_c.PB);
      CHECK(g_abandon_called == 0, "abandon_called=%d exp 0", g_abandon_called);
      CHECK(g_c.S == (uint16)(entry_s + 2), "S=%04X exp %04X", g_c.S,
            (uint16)(entry_s + 2)); }

    /* PB2: JML changes logical PB without changing it to the low mirror. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_abandon_called = 0;
      uint8_t from[] = {0x5C,0x00,0x90,0x81};
      uint8_t to[] = {0x4B,0x68,0x8D,0x02,0x01,0x60};
      load(0x808000, from, sizeof from); load(0x819000, to, sizeof to);
      cpu_push_jsr_return_frame(&g_c); uint16 entry_s = g_c.S;
      RecompReturn r = interp_tier_dispatch_balanced(&g_c,0x808000,0x80C0D2,entry_s,2);
      printf("PB2 balanced tier logical PB across JML\n");
      CHECK(r==RECOMP_RETURN_NORMAL,"r=%d exp NORMAL",(int)r);
      CHECK(RAM[0x0102]==0x81,"PHK destination=%02X exp 81",RAM[0x0102]);
      CHECK(g_c.PB==0x81,"PB=%02X exp 81",g_c.PB); }

    /* PB3: low mirror lookup remains legal while architectural PB stays low. */
    { memset(RAM,0,MEMSZ); init_cpu();
      uint8_t from[]={0x5C,0x00,0x90,0x01};
      uint8_t to[]={0x4B,0x68,0x8D,0x03,0x01,0x60};
      load(0x008000,from,sizeof from); load(0x019000,to,sizeof to);
      cpu_push_jsr_return_frame(&g_c); uint16 entry_s=g_c.S;
      (void)interp_tier_dispatch_balanced(&g_c,0x008000,0x00C0D3,entry_s,2);
      printf("PB3 low mirror preserves logical PB\n");
      CHECK(RAM[0x0103]==0x01,"PHK destination=%02X exp 01",RAM[0x0103]);
      CHECK(g_c.PB==0x01,"PB=%02X exp 01",g_c.PB); }

    /* PB4: the production bridge supports execution from WRAM through its
     * normal bus; the flat fixture models the same logical $7E address. */
    { memset(RAM,0,MEMSZ); init_cpu();
      uint8_t code[]={0x4B,0x68,0x8D,0x04,0x01,0x60};
      load(0x7E8000,code,sizeof code); cpu_push_jsr_return_frame(&g_c);
      uint16 entry_s=g_c.S;
      (void)interp_tier_dispatch_balanced(&g_c,0x7E8000,0x7EC0D4,entry_s,2);
      printf("PB4 WRAM execution preserves bank $7E\n");
      CHECK(RAM[0x0104]==0x7E,"PHK WRAM=%02X exp 7E",RAM[0x0104]);
      CHECK(g_c.PB==0x7E,"PB=%02X exp 7E",g_c.PB); }

    /* PB5: tier-down -> interpreted JSL -> exact AOT bounce -> RTL model ->
     * interpreted remainder -> host return. */
    { memset(RAM,0,MEMSZ); init_cpu(); g_aot_called=0; g_abandon_called=0;
      g_fake_host_depth=0; g_fake_host_peak=0;
      uint8_t code[]={0x22,0x00,0x81,0x81,0x4B,0x68,0x8D,0x05,0x01,0x60};
      load(0x808000,code,sizeof code); cpu_push_jsr_return_frame(&g_c);
      uint16 entry_s=g_c.S;
      RecompReturn r=interp_tier_dispatch_balanced(&g_c,0x808000,0x80C0D5,entry_s,2);
      printf("PB5 nested interpreted JSL exact-AOT bounce\n");
      CHECK(r==RECOMP_RETURN_NORMAL,"r=%d exp NORMAL",(int)r);
      CHECK(g_aot_called==1,"aot_called=%d exp 1",g_aot_called);
      CHECK(g_fake_seen_pb==0x81,"PB at exact AOT bounce=%02X exp 81",g_fake_seen_pb);
      CHECK(g_fake_seen_hrv==3,"bounce hrv=%u exp 3",g_fake_seen_hrv);
      CHECK(RAM[0x0105]==0x80,"PHK remainder=%02X exp 80",RAM[0x0105]);
      CHECK(g_c.PB==0x80,"PB=%02X exp 80",g_c.PB);
      CHECK(g_c.S==(uint16)(entry_s+2),"S=%04X exp %04X",g_c.S,(uint16)(entry_s+2));
      CHECK(g_fake_host_depth==0 && g_fake_host_peak==1,"host depth=%d peak=%d exp 0/1",g_fake_host_depth,g_fake_host_peak);
      CHECK(g_abandon_called==0,"abandon_called=%d exp 0",g_abandon_called); }

    /* XY1/XY2: the common generated-C P synchronizer implements the same
     * high-byte clearing rule as interp816_setFlags for SEP and PLP. */
    { init_cpu(); g_c.emulation=0; g_c.P=0x20; g_c.X=0xABCD; g_c.Y=0x1234;
      g_c.P |= CPU_P_X; cpu_p_to_mirrors(&g_c); /* SEP #$10 */
      printf("XY1 SEP/REP index high-byte semantics\n");
      CHECK(g_c.X==0x00CD && g_c.Y==0x0034,"after SEP X/Y=%04X/%04X exp 00CD/0034",g_c.X,g_c.Y);
      g_c.P &= (uint8)~CPU_P_X; cpu_p_to_mirrors(&g_c); /* REP #$10 */
      CHECK(g_c.X==0x00CD && g_c.Y==0x0034,"after REP X/Y=%04X/%04X exp 00CD/0034",g_c.X,g_c.Y); }
    { init_cpu(); g_c.emulation=0; g_c.X=0xABCD; g_c.Y=0x1234;
      g_c.P=0x30; cpu_p_to_mirrors(&g_c); /* PLP result with X=1 */
      printf("XY2 PLP index high-byte semantics\n");
      CHECK(g_c.X==0x00CD && g_c.Y==0x0034,"after PLP X/Y=%04X/%04X exp 00CD/0034",g_c.X,g_c.Y); }

    /* PB6 / $908000: physical 512-KiB LoROM mapping aliases $90:8000 to
     * offset zero while architectural PB and PHK retain logical bank $90. */
    { memset(RAM,0,MEMSZ); memset(ROM512,0,0x80000); init_cpu(); g_lorom_map=1;
      uint8_t jml[]={0x5C,0x00,0x80,0x90};
      uint8_t dest[]={0x4B,0x68,0x8D,0x06,0x01,0x60};
      memcpy(ROM512+0x021B,jml,sizeof jml); memcpy(ROM512,dest,sizeof dest);
      cpu_push_jsr_return_frame(&g_c); uint16 entry_s=g_c.S;
      RecompReturn r=interp_tier_dispatch_balanced(&g_c,0x80821B,0x80821B,entry_s,2);
      printf("PB6 $908000 physical mirror and logical PB\n");
      CHECK(r==RECOMP_RETURN_NORMAL,"r=%d exp NORMAL",(int)r);
      CHECK(RAM[0x0106]==0x90,"PHK mirror destination=%02X exp 90",RAM[0x0106]);
      CHECK(g_c.PB==0x90,"PB=%02X exp 90",g_c.PB);
      CHECK(g_c.S==(uint16)(entry_s+2),"S=%04X exp %04X",g_c.S,(uint16)(entry_s+2));
      g_lorom_map=0; }

    printf("\n==== interp_bridge Phase-1: %d/%d checks passed ====\n", g_check - g_fail, g_check);
    if (g_fail) { printf("RESULT: FAIL (%d)\n", g_fail); return 1; }
    printf("RESULT: PASS\n");
    return 0;
}
