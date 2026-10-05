#ifndef APU_FRAME_CLOCK_H
#define APU_FRAME_CLOCK_H
#include <stdint.h>

/* Guest-time APU clock.
 *
 * BACKPORT PROVENANCE
 *   source pin      mstan-snesrecomp b04d8fa00cbb554f868449bd9038bca90c4d312c
 *                   (byte-identical in SuperMetroid 63c8be8 and MegaManX
 *                    95df4b5; the ecosystem's T3 reference)
 *   source file     runner/src/apu_frame_clock.h
 *   local adaptation  the two rate constants are carried over verbatim; the
 *                     RtlApuFrameClock STRUCT is deliberately NOT carried
 *                     over.  See "Why the struct is not ported" below.
 *
 * THE RELATION
 *   1 physical frame period = 357368 master clocks   (1364 dots * 262 lines)
 *                           =  17088 SPC cycles
 *                           =    534 native stereo PCM frames (17088 / 32,
 *                              because apu_cycle runs dsp_cycle every 32)
 *   All three are one constant written three ways.  The Goof physical
 *   boundary clock (goof_frame_driver.c) already owns the first; this header
 *   is what lets the APU consume the same one instead of inventing a second.
 *
 * WHY THE STRUCT IS NOT PORTED
 *   The reference clock interpolates WITHIN a host frame iteration and has to
 *   carry `start_guest` / `next_guest` / `last_duration` across iterations,
 *   because upstream the frame loop is the only timeline and an NMI-disabled
 *   loader can span many hardware frames inside one host iteration.
 *
 *   Goof does not have that problem: its physical boundaries are an ABSOLUTE
 *   deadline grid, k * 357368 from master_cycles == 0, owned by
 *   GuestExecution and already validated (tier-uniform master clock).  A
 *   position on that grid is a pure function of master_cycles, so the clock
 *   needs no state, no begin/finish pairing and no WAI floor -- the floor
 *   exists upstream to stop a stalled iteration mapping to zero elapsed
 *   guest time, and here idle padding already advances master_cycles to the
 *   next deadline.  Porting the struct would have introduced exactly the
 *   second frame clock the milestone forbids.
 *
 * DETERMINISM
 *   Integer only.  No floating point anywhere on this path -- the reference
 *   avoids it and so does this.  The conversion is a floor of an exact
 *   rational, so the fractional remainder is carried implicitly and exactly
 *   by the arithmetic itself; there is no accumulator to drift.
 *
 *   The fraction is reduced by gcd(17088, 357368) = 8 purely for headroom:
 *   floor(m*17088/357368) and floor(m*2136/44671) are the same value for
 *   every m (reducing a rational to lowest terms does not move its floor),
 *   and the reduced form keeps the product inside uint64 up to
 *   m ~ 8.6e15 master cycles -- about 76 years of guest time.
 *
 *   Because 357368 = 8 * 44671, a deadline at k * 357368 maps to EXACTLY
 *   k * 17088 with zero remainder.  Frame-boundary targets are therefore
 *   exact integers, not rounded ones.
 */

#define RTL_MASTER_CYCLES_PER_FRAME 357368ull
#define RTL_APU_CYCLES_PER_FRAME     17088ull
#define RTL_APU_FRAMES_PER_FRAME       534ull  /* native stereo PCM frames */

/* The physical-frame driver owns the absolute deadline grid
 * k * RTL_MASTER_CYCLES_PER_FRAME, with a deadline as VBlank-entry phase
 * zero.  These are pure queries of that existing guest-time grid: they carry
 * no beam state and never depend on MMIO read frequency or host pacing. */
#define RTL_VBLANK_MASTER_CYCLES (37ull * 1364ull)

static inline uint64_t rtl_physical_frame_phase(uint64_t master) {
  return master % RTL_MASTER_CYCLES_PER_FRAME;
}

static inline int rtl_ppu_vblank_at_master_cycles(uint64_t master) {
  return rtl_physical_frame_phase(master) < RTL_VBLANK_MASTER_CYCLES;
}

/* 17088 / 357368 reduced by 8 -- the same rational, more headroom. */
#define RTL_APU_CLOCK_NUM  2136ull
#define RTL_APU_CLOCK_DEN 44671ull

/* Guest APU-cycle position of a guest master-clock instant. Monotonic. */
static inline uint64_t rtl_apu_guest_cycle_at(uint64_t master) {
  return master * RTL_APU_CLOCK_NUM / RTL_APU_CLOCK_DEN;
}

#endif
