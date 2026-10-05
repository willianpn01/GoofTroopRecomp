/* Mechanical helpers shared by the phase4_boot gates.  See the header:
 * test infrastructure only, lifted verbatim from headless_frame_gate.c. */
#include "goof_gate_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "sha256.h"

const uint8_t goof_gate_pinned_rom_sha256[32] = {
  0x2b, 0xb3, 0x68, 0xc4, 0x71, 0x89, 0xce, 0x81,
  0x3a, 0xd7, 0x16, 0xee, 0xf1, 0x6c, 0x01, 0xcd,
  0x47, 0x68, 0x5c, 0xb9, 0x8e, 0x2c, 0x1c, 0xb3,
  0x5f, 0xa6, 0xf0, 0x17, 0x3c, 0x97, 0xdd, 0x7c,
};

uint64_t goof_gate_fnv1a64(const void *data, size_t size) {
  const uint8_t *b = data;
  uint64_t h = UINT64_C(1469598103934665603);
  for (size_t i = 0; i < size; i++) h = (h ^ b[i]) * UINT64_C(1099511628211);
  return h;
}

uint64_t goof_gate_logical_epoch_hash_v1(const GuestExecution *g) {
  uint64_t h = UINT64_C(1469598103934665603);
  for (size_t i = 0; i < 0x20000; i++)
    h = (h ^ g_ram[i]) * UINT64_C(1099511628211);
  const Interp816 *c = &g->cursor.cpu;
  const uint64_t words[] = {
    c->a, c->x, c->y, c->sp, c->pc, c->dp, c->k, c->db,
    interp816_getFlags((Interp816 *)c), g->nmis, g->dispatches, g->min_s,
    (uint64_t)g_recomp_stack_top,
  };
  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
    for (unsigned byte = 0; byte < 8; byte++)
      h = (h ^ (uint8_t)(words[i] >> (8 * byte))) *
          UINT64_C(1099511628211);
  return h;
}

uint64_t goof_gate_oam_hash(const Ppu *p) {
  return goof_gate_fnv1a64(p->oam, sizeof(p->oam)) ^
         (UINT64_C(3) * goof_gate_fnv1a64(p->highOam, sizeof(p->highOam)));
}

int goof_gate_parse_mx_suffix(const char *name, uint8_t *m, uint8_t *x) {
  size_t n = name ? strlen(name) : 0;
  if (n < 5 || name[n - 5] != '_' || name[n - 4] != 'M' ||
      name[n - 2] != 'X' || (name[n - 3] != '0' && name[n - 3] != '1') ||
      (name[n - 1] != '0' && name[n - 1] != '1'))
    return 0;
  *m = (uint8_t)(name[n - 3] - '0');
  *x = (uint8_t)(name[n - 1] - '0');
  return 1;
}

uint8_t *goof_gate_load_pinned_rom(const char *path, size_t *size_out) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long length = ftell(f);
  if (length != 524288) { fclose(f); return NULL; }
  rewind(f);
  uint8_t *rom = malloc((size_t)length);
  if (!rom) { fclose(f); return NULL; }
  if (fread(rom, 1, (size_t)length, f) != (size_t)length) {
    free(rom); fclose(f); return NULL;
  }
  if (fclose(f) != 0) { free(rom); return NULL; }
  uint8_t digest[32];
  sha256_compute(rom, (size_t)length, digest);
  if (memcmp(digest, goof_gate_pinned_rom_sha256, sizeof(digest)) != 0) {
    free(rom); return NULL;
  }
  *size_out = (size_t)length;
  return rom;
}
