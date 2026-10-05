#ifndef GOOF_GATE_COMMON_H
#define GOOF_GATE_COMMON_H

/* Mechanical helpers shared by the phase4_boot gates.
 *
 * TEST INFRASTRUCTURE ONLY.  Nothing here is runtime or engine code, and
 * nothing here changes semantics: every function below was lifted verbatim
 * out of headless_frame_gate.c so that the E200 headless gate and the
 * E1000 boot gate cannot drift apart on the one definition that must stay
 * bit-identical between them -- GOOF_LOGICAL_EPOCH_HASH_V1.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "goof_frame_driver.h"
#include "ppu.h"

/* Pinned canonical Goof Troop ROM identity (headerless payload).  Single
 * source of truth: goof_gate_load_pinned_rom verifies against it, and the
 * frontend's SMC-normalising loader verifies against the same bytes. */
extern const uint8_t goof_gate_pinned_rom_sha256[32];

/* FNV-1a 64, the hash used for every framebuffer / memory-block digest. */
uint64_t goof_gate_fnv1a64(const void *data, size_t size);

/* GOOF_LOGICAL_EPOCH_HASH_V1: WRAM plus the certified cursor words.
 * Same domain as boot_harness.c.  Contains no host pointer, so it is
 * comparable across processes. */
uint64_t goof_gate_logical_epoch_hash_v1(const GuestExecution *g);

/* OAM digest (low table mixed with the high table). */
uint64_t goof_gate_oam_hash(const Ppu *p);

/* Parses the _M<m>X<x> suffix of a generated function symbol.
 * Returns 0 when the symbol carries no suffix. */
int goof_gate_parse_mx_suffix(const char *name, uint8_t *m, uint8_t *x);

/* Loads the pinned Goof Troop ROM, verifying size and SHA-256.
 * Returns NULL on any mismatch; *size_out is set on success. */
uint8_t *goof_gate_load_pinned_rom(const char *path, size_t *size_out);

#endif
