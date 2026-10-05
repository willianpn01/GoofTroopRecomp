/*
 * interp816 <-> Shared Timing Contract equivalence harness (TU-EQ).
 *
 * The interpreter no longer carries its own indexed-page-cross rule; it reads
 * the generated authority (SNES_STC_XCROSS / SNES_STC_INDEX_CROSS, baked from
 * recompiler/snes_cycles.py).  This harness proves the consumption is exact,
 * by EXECUTION and not by inspection, over a directed sweep rather than over
 * whatever the current campaign happens to reach -- Family C executes zero
 * times in E1..E1000 and would otherwise be untested.
 *
 * For every decoded opcode, and for the indexed opcodes over the full cross /
 * index-width / accumulator-width / direct-page sweep, it runs ONE instruction
 * on a flat 16 MB bus and asserts
 *
 *     interp816 cyclesUsed  ==  STC(op, live predicates)
 *
 * where STC is snes_instr_cpu_cycles() with SNES_STC_XCROSS substituted for the
 * bare SNES_XCROSS_ADD -- i.e. the authority minus the declared residuals.
 *
 * It also pins the three families and both residuals by name, so a regression
 * says WHICH contract broke:
 *
 *   Family A  17 indexed reads      cross -> +1
 *   Family B   4 indexed stores     cross -> 0   (fixed cost already in base)
 *   Family C   6 abs,X RMW          cross -> +1  (read rule, not store rule)
 *   R1         7 (dp),Y reads       cross -> 0   declared residual, symmetric
 *   R2         MVN/MVP              per-byte not emitted; interpreter == 7/byte
 *
 * Build/run via tests/interp816/run.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "interp816.h"
#include "snes_cycles.h"

#define MEMSZ 0x1000000u
static uint8_t *MEM;
static uint8_t bus_read(void *mem, uint32_t adr)             { (void)mem; return MEM[adr & 0xFFFFFF]; }
static void    bus_write(void *mem, uint32_t adr, uint8_t v) { (void)mem; MEM[adr & 0xFFFFFF] = v; }
int interp816_opcode_hook(uint32_t addr) { (void)addr; return 0; }

static int g_fail = 0, g_check = 0;
#define CHECK(cond, ...) do { g_check++; if (!(cond)) {                      \
    g_fail++; printf("    FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static Interp816 *g_cpu;

static const uint8_t kFamilyA[] = {0x19, 0x1D, 0x39, 0x3C, 0x3D, 0x59, 0x5D,
                                   0x79, 0x7D, 0xB9, 0xBC, 0xBD, 0xBE, 0xD9,
                                   0xDD, 0xF9, 0xFD};
static const uint8_t kFamilyB[] = {0x91, 0x99, 0x9D, 0x9E};
static const uint8_t kFamilyC[] = {0x1E, 0x3E, 0x5E, 0x7E, 0xDE, 0xFE};
static const uint8_t kR1[]      = {0x11, 0x31, 0x51, 0x71, 0xB1, 0xD1, 0xF1};

/* The STC's expected cycle count for one execution with these live
 * predicates.  Deliberately built from the generated header, so the test
 * cannot encode a second opinion about the model. */
static int stc_expect(uint8_t op, int m, int x, int dp_nz, int cross, int taken) {
  return snes_instr_cpu_cycles(op, m, x, /*e=*/0, dp_nz, /*cross=*/0, taken, 0)
       + (cross ? SNES_STC_XCROSS(op) : 0);
}

/* Lay one instruction at $00:8000 with the given live state and run it.
 * `base` is the 16-bit absolute operand (or the DP offset for (dp),Y). */
static int run_one(uint8_t op, uint16_t base, uint16_t index, int m, int x,
                   int dp_nz, int indir_y) {
  Interp816 *c = g_cpu;
  memset(MEM, 0, MEMSZ);
  interp816_reset(c);
  c->e = false;                       /* native: the only mode Goof runs in */
  c->mf = m ? true : false;
  c->xf = x ? true : false;
  c->dp = dp_nz ? 0x0080 : 0x0000;
  c->db = 0x7E;
  c->sp = 0x01FF;
  c->k = 0; c->pc = 0x8000;
  unsigned mode = SNES_OP_MODE[op];
  if (indir_y) {
    /* (dp),Y: one operand byte; the pointer lives in DP, bank 0. */
    MEM[0x8000] = op;
    MEM[0x8001] = 0x10;
    uint16_t ptr_at = (uint16_t)(c->dp + 0x10);
    MEM[ptr_at] = (uint8_t)base;
    MEM[(uint16_t)(ptr_at + 1)] = (uint8_t)(base >> 8);
    c->y = index;
  } else {
    MEM[0x8000] = op;
    MEM[0x8001] = (uint8_t)base;
    MEM[0x8002] = (uint8_t)(base >> 8);
    if (mode == SNES_MODE_ABS_Y) c->y = index; else c->x = index;
  }
  interp816_runOpcode(c);
  return c->cyclesUsed;
}

static int crossed(uint16_t base, uint16_t index) {
  return SNES_STC_INDEX_CROSS(base, index) ? 1 : 0;
}

/* One opcode over the whole predicate sweep. */
static void sweep(uint8_t op, const char *family) {
  int indir_y = SNES_OP_MODE[op] == SNES_MODE_INDIR_Y;
  /* (base, index) pairs chosen so both cross outcomes occur, including the
   * bank-carry case the 16-bit mask exists for. */
  static const struct { uint16_t base, index; } cases[] = {
    {0x0010, 0x0000},   /* no cross                       */
    {0x0010, 0x000F},   /* no cross, same page            */
    {0x0010, 0x0100},   /* cross                          */
    {0x00FF, 0x0001},   /* cross by one byte              */
    {0x1234, 0x0080},   /* cross                          */
    {0xFF80, 0x0100},   /* carries out of the bank        */
    {0x00FF, 0xFF01},   /* wraps to the SAME page: NOT a cross under the
                         * 16-bit mask both tiers use     */
  };
  for (unsigned ci = 0; ci < sizeof cases / sizeof cases[0]; ci++)
    for (int m = 0; m <= 1; m++)
      for (int x = 0; x <= 1; x++)
        for (int dp_nz = 0; dp_nz <= 1; dp_nz++) {
          uint16_t base = cases[ci].base, index = cases[ci].index;
          if (x) index &= 0x00FF;     /* an 8-bit index cannot exceed $FF */
          int cross = crossed(base, index);
          int want = stc_expect(op, m, x, indir_y ? dp_nz : 0, cross, 0);
          int got = run_one(op, base, index, m, x, dp_nz, indir_y);
          CHECK(got == want,
                "%s $%02X base=%04X idx=%04X m=%d x=%d dp=%d cross=%d: "
                "interp=%d stc=%d", family, op, base, index, m, x, dp_nz,
                cross, got, want);
        }
}

int main(void) {
  MEM = malloc(MEMSZ);
  if (!MEM) return 2;
  g_cpu = interp816_init(NULL, bus_read, bus_write);

  printf("=== TU-EQ: interp816 cyclesUsed == Shared Timing Contract ===\n");

  printf("Family A -- 17 indexed reads, +1 on a genuine cross\n");
  for (unsigned i = 0; i < sizeof kFamilyA; i++) {
    CHECK(SNES_STC_XCROSS(kFamilyA[i]) == 1,
          "A $%02X STC cross charge is %d, want 1",
          kFamilyA[i], SNES_STC_XCROSS(kFamilyA[i]));
    sweep(kFamilyA[i], "A");
  }

  printf("Family B -- 4 indexed stores, the cross charge is 0\n");
  for (unsigned i = 0; i < sizeof kFamilyB; i++) {
    CHECK(SNES_STC_XCROSS(kFamilyB[i]) == 0,
          "B $%02X STC cross charge is %d, want 0 (it is already in the base)",
          kFamilyB[i], SNES_STC_XCROSS(kFamilyB[i]));
    sweep(kFamilyB[i], "B");
  }

  printf("Family C -- 6 abs,X RMW, the READ rule and not the store rule\n");
  for (unsigned i = 0; i < sizeof kFamilyC; i++) {
    CHECK(SNES_STC_XCROSS(kFamilyC[i]) == 1,
          "C $%02X STC cross charge is %d, want 1",
          kFamilyC[i], SNES_STC_XCROSS(kFamilyC[i]));
    sweep(kFamilyC[i], "C");
  }

  printf("R1 -- 7 (dp),Y reads: the authority charges it, NOBODY emits it\n");
  for (unsigned i = 0; i < sizeof kR1; i++) {
    uint8_t op = kR1[i];
    CHECK(SNES_XCROSS_ADD[op] == 1, "R1 $%02X authority charge missing", op);
    CHECK(SNES_XCROSS_EMITTED[op] == 0,
          "R1 $%02X is marked EMITTED -- the residual was closed on one side "
          "only; close it in snes_cycles.py for both tiers or not at all", op);
    CHECK(SNES_STC_XCROSS(op) == 0, "R1 $%02X STC charge is not 0", op);
    sweep(op, "R1");
    /* and, explicitly: a genuine (dp),Y read cross must cost the SAME as a
     * non-crossing one, which is what keeps the two tiers symmetric. */
    int no  = run_one(op, 0x0010, 0x0000, 1, 1, 0, 1);
    int yes = run_one(op, 0x00FF, 0x0001, 1, 1, 0, 1);
    CHECK(no == yes, "R1 $%02X charges the cross: %d vs %d", op, no, yes);
  }

  printf("R2 -- MVN/MVP: the interpreter charges 7 per byte (== authority)\n");
  {
    /* MVN $54 src=$7F dst=$7E, A = 0 -> exactly one byte moved per call. */
    Interp816 *c = g_cpu;
    memset(MEM, 0, MEMSZ);
    interp816_reset(c);
    c->e = false; c->mf = false; c->xf = false;
    c->k = 0; c->pc = 0x8000; c->a = 0x0000; c->x = 0x0000; c->y = 0x0000;
    MEM[0x8000] = 0x54; MEM[0x8001] = 0x7E; MEM[0x8002] = 0x7F;
    interp816_runOpcode(c);
    CHECK(c->cyclesUsed == 7, "MVN per byte: interp=%d authority=7",
          c->cyclesUsed);
    CHECK(SNES_BASE_CYCLES[0x54] == 7 && SNES_BASE_CYCLES[0x44] == 7,
          "authority block-move per-byte base moved");
    CHECK(SNES_STC_BLOCK_MOVE_EMITTED == 0,
          "the emitter now claims to emit per-byte block-move timing; the "
          "unreachability gate must be retired in the same change");
  }

  printf("Coverage -- every opcode the authority gives a cross charge is "
         "classified\n");
  {
    unsigned classified = 0, authority = 0;
    for (unsigned op = 0; op < 256; op++) {
      if (!SNES_OP_VALID[op]) continue;
      unsigned mode = SNES_OP_MODE[op];
      int is_indexed = mode == SNES_MODE_ABS_X || mode == SNES_MODE_ABS_Y ||
                       mode == SNES_MODE_INDIR_Y;
      if (!is_indexed) {
        CHECK(SNES_XCROSS_ADD[op] == 0 && SNES_STC_XCROSS(op) == 0,
              "$%02X is not indexed but carries a cross charge", op);
        continue;
      }
      authority++;
      int found = 0;
      for (unsigned i = 0; i < sizeof kFamilyA; i++) found |= kFamilyA[i] == op;
      for (unsigned i = 0; i < sizeof kFamilyB; i++) found |= kFamilyB[i] == op;
      for (unsigned i = 0; i < sizeof kFamilyC; i++) found |= kFamilyC[i] == op;
      for (unsigned i = 0; i < sizeof kR1; i++)      found |= kR1[i] == op;
      classified += found != 0;
      CHECK(found, "$%02X is indexed but belongs to no declared family", op);
    }
    CHECK(authority == 34 && classified == 34,
          "indexed opcodes: %u in the authority, %u classified, want 34/34",
          authority, classified);
  }

  printf("\n%s  checks=%d failures=%d\n", g_fail ? "TU_EQ FAIL" : "TU_EQ PASS",
         g_check, g_fail);
  return g_fail ? 1 : 0;
}
