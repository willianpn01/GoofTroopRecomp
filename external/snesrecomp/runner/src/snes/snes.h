
#ifndef SNES_H
#define SNES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct Snes Snes;

#include "cpu.h"
#include "apu.h"
#include "dma.h"
#include "ppu.h"
#include "cart.h"
#include "saveload.h"

struct Snes {
  Cpu* cpu;
  Apu* apu;
  Ppu* ppu;
  Dma* dma;
  Cart* cart;
  uint16 input1_currentState;
  uint16 input2_currentState;
  bool disableRender;

  // ram data port ($2180-$2183)
  uint32_t ramAdr;
  uint8_t *ram;

  // --- saveload blob starts here (hPos .. divideResult) ---
  uint16_t hPos;
  uint16_t vPos;
  double apuCatchupCycles;
  // nmi / irq
  bool hIrqEnabled;
  bool vIrqEnabled;
  bool nmiEnabled;
  uint16_t hTimer;
  uint16_t vTimer;
  bool inNmi;
  bool inIrq;
  bool inVblank;
  // joypad
  bool autoJoyRead;
  uint16_t autoJoyTimer;
  bool ppuLatch;
  // multiplication/division
  uint8_t multiplyA;
  uint16_t multiplyResult;
  uint16_t divideA;
  uint16_t divideResult;
};

Snes* snes_init(uint8_t *ram);
void snes_free(Snes* snes);
void snes_reset(Snes* snes, bool hard);
// used by dma, cpu
uint8_t snes_readBBus(Snes* snes, uint8_t adr);
void snes_writeBBus(Snes* snes, uint8_t adr, uint8_t val);
uint8_t snes_read(Snes* snes, uint32_t adr);
void snes_write(Snes* snes, uint32_t adr, uint8_t val);
uint8_t snes_readReg(Snes* snes, uint16_t adr);
void snes_writeReg(Snes* snes, uint16_t adr, uint8_t val);
uint16_t SwapInputBits(uint16_t x);


// snes_other.c functions:

bool snes_loadRom(Snes* snes, const uint8_t* data, int length);
void snes_saveload(Snes *snes, SaveLoadInfo *sli);
void snes_catchupApu(Snes *snes);

/* ---- APU catch-up observability (GOOF_APU_FRAME_RATE_FIDELITY) ----------
 * Pure instrumentation: snes_catchupApu's semantics are unchanged.  The
 * 10000-cycle cap in snes_catchupApu is a DESTRUCTIVE assignment -- debt
 * above it is erased from the accumulator before it is ever executed -- so
 * a gate cannot infer loss from cycle counts alone.  These counters make
 * the loss directly measurable, per call site, so "the clamp is unreachable
 * in normal execution" becomes an asserted fact instead of a claim.
 *
 * Call sites tag themselves through snes_apu_catchup_site() so a clamp hit
 * can be attributed; the tag is a plain global because the runtime is
 * single-threaded on the guest path and the tag is never guest-visible. */
enum {
  SNES_APU_SITE_OTHER    = 0,  /* unclassified / interp bridge            */
  SNES_APU_SITE_PORT_READ = 1, /* snes_readBBus + ReadRegWord $2140-$217F */
  SNES_APU_SITE_PORT_WRITE = 2,/* RtlApuWrite pre-timeline path           */
  SNES_APU_SITE_BOUNDARY = 3,  /* physical frame boundary sync            */
  SNES_APU_SITE_COUNT    = 4
};
void snes_apu_catchup_site(int site);
typedef struct SnesApuCatchupStats {
  uint64_t calls;                        /* snes_catchupApu invocations   */
  uint64_t cycles;                       /* SPC cycles actually executed  */
  uint64_t clamp_hits;                   /* calls where debt > 10000      */
  uint64_t clamp_lost_cycles;            /* SPC cycles destroyed by clamp */
  uint64_t max_request;                  /* largest debt seen, pre-clamp  */
  uint64_t max_residual;                 /* largest post-call accumulator */
  uint64_t calls_by_site[SNES_APU_SITE_COUNT];
  uint64_t clamp_hits_by_site[SNES_APU_SITE_COUNT];
  uint64_t clamp_lost_by_site[SNES_APU_SITE_COUNT];
} SnesApuCatchupStats;
void snes_apu_catchup_stats(SnesApuCatchupStats *out);

extern int snes_frame_counter;
#endif
