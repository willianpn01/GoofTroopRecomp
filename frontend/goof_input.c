/* GOOF_INPUT_V1 -- the deterministic guest input model.  SDL-FREE.
 * See goof_input.h for the normative bit layout and script format. */
#include "goof_input.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "goof_gate_common.h"   /* goof_gate_fnv1a64 -- the one hash in use */

void goof_input_normalize(GoofInputSample *s) {
  if (!s) return;
  s->p1 &= GOOF_BTN_MASK;
  s->p2 &= GOOF_BTN_MASK;
  /* Engine precedent, common_rtl.c:453: both opposing directions held cancel
   * to neither, on this exact bit layout. */
  if ((s->p1 & GOOF_BTN_VERTICAL)   == GOOF_BTN_VERTICAL)   s->p1 &= (uint16_t)~GOOF_BTN_VERTICAL;
  if ((s->p1 & GOOF_BTN_HORIZONTAL) == GOOF_BTN_HORIZONTAL) s->p1 &= (uint16_t)~GOOF_BTN_HORIZONTAL;
  if ((s->p2 & GOOF_BTN_VERTICAL)   == GOOF_BTN_VERTICAL)   s->p2 &= (uint16_t)~GOOF_BTN_VERTICAL;
  if ((s->p2 & GOOF_BTN_HORIZONTAL) == GOOF_BTN_HORIZONTAL) s->p2 &= (uint16_t)~GOOF_BTN_HORIZONTAL;
}

bool goof_input_is_neutral(const GoofInputSample *s) {
  return !s || (!(s->p1 & GOOF_BTN_MASK) && !(s->p2 & GOOF_BTN_MASK));
}

/* --- button names -------------------------------------------------------- */

typedef struct { const char *name; uint16_t bit; } GoofButtonName;

/* Order is the print order, so mask text is canonical and diffable. */
static const GoofButtonName kButtons[] = {
  {"Up", GOOF_BTN_UP}, {"Down", GOOF_BTN_DOWN},
  {"Left", GOOF_BTN_LEFT}, {"Right", GOOF_BTN_RIGHT},
  {"Start", GOOF_BTN_START}, {"Select", GOOF_BTN_SELECT},
  {"A", GOOF_BTN_A}, {"B", GOOF_BTN_B},
  {"X", GOOF_BTN_X}, {"Y", GOOF_BTN_Y},
  {"L", GOOF_BTN_L}, {"R", GOOF_BTN_R},
};
enum { kButtonCount = (int)(sizeof kButtons / sizeof kButtons[0]) };

void goof_input_mask_text(uint16_t mask, char *out, size_t out_size) {
  if (!out || out_size == 0) return;
  mask &= GOOF_BTN_MASK;
  if (!mask) { snprintf(out, out_size, "-"); return; }
  size_t used = 0;
  out[0] = '\0';
  for (int i = 0; i < kButtonCount; i++) {
    if (!(mask & kButtons[i].bit)) continue;
    int n = snprintf(out + used, out_size - used, "%s%s",
                     used ? "+" : "", kButtons[i].name);
    if (n < 0 || (size_t)n >= out_size - used) return;   /* truncated */
    used += (size_t)n;
  }
}

static bool name_equal(const char *a, const char *b) {
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    a++; b++;
  }
  return !*a && !*b;
}

bool goof_input_parse_mask(const char *text, uint16_t *out) {
  if (!text || !out) return false;
  while (*text && isspace((unsigned char)*text)) text++;
  if (!*text) return false;
  if (name_equal(text, "-") || name_equal(text, "0") ||
      name_equal(text, "none") || name_equal(text, "neutral")) {
    *out = 0; return true;
  }
  if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    char *end;
    unsigned long v = strtoul(text + 2, &end, 16);
    if (end == text + 2 || *end != '\0' || (v & ~(unsigned long)GOOF_BTN_MASK))
      return false;
    *out = (uint16_t)v;
    return true;
  }
  uint16_t mask = 0;
  const char *p = text;
  while (*p) {
    char token[16];
    size_t n = 0;
    while (*p && *p != '+' && !isspace((unsigned char)*p)) {
      if (n + 1 >= sizeof token) return false;
      token[n++] = *p++;
    }
    token[n] = '\0';
    if (n == 0) return false;
    int i = 0;
    for (; i < kButtonCount; i++)
      if (name_equal(token, kButtons[i].name)) { mask |= kButtons[i].bit; break; }
    if (i == kButtonCount) return false;
    if (*p == '+') p++;
    else break;
  }
  *out = mask;
  return true;
}

/* --- script ------------------------------------------------------------- */

static void script_fail(char *err, size_t err_size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void script_fail(char *err, size_t err_size, const char *fmt, ...) {
  if (!err || err_size == 0) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_size, fmt, ap);
  va_end(ap);
}

bool goof_input_script_load(const char *path, GoofInputScript *script,
                            char *err, size_t err_size) {
  if (!script) return false;
  memset(script, 0, sizeof *script);
  if (err && err_size) err[0] = '\0';
  if (!path) { script_fail(err, err_size, "no path"); return false; }

  FILE *f = fopen(path, "r");
  if (!f) { script_fail(err, err_size, "cannot open %s", path); return false; }

  size_t capacity = 16;
  GoofInputScriptEntry *entries = calloc(capacity, sizeof *entries);
  if (!entries) { fclose(f); script_fail(err, err_size, "out of memory"); return false; }
  size_t count = 0;
  char line[256];
  unsigned long lineno = 0;
  bool ok = true;

  while (ok && fgets(line, sizeof line, f)) {
    lineno++;
    char *hash = strchr(line, '#');
    if (hash) *hash = '\0';
    char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) continue;

    char epoch_tok[32], p1_tok[64], p2_tok[64];
    int fields = sscanf(p, "%31s %63s %63s", epoch_tok, p1_tok, p2_tok);
    if (fields < 2) {
      script_fail(err, err_size, "%s:%lu: expected '<epoch> <p1> [p2]'",
                  path, lineno);
      ok = false; break;
    }
    char *end;
    unsigned long long epoch = strtoull(epoch_tok, &end, 10);
    if (*end != '\0' || epoch == 0) {
      script_fail(err, err_size, "%s:%lu: bad epoch '%s' (must be >= 1)",
                  path, lineno, epoch_tok);
      ok = false; break;
    }
    if (count && (uint64_t)epoch <= entries[count - 1].epoch) {
      script_fail(err, err_size,
                  "%s:%lu: epoch %llu not strictly after %llu", path, lineno,
                  epoch, (unsigned long long)entries[count - 1].epoch);
      ok = false; break;
    }
    GoofInputSample s = {0, 0};
    if (!goof_input_parse_mask(p1_tok, &s.p1)) {
      script_fail(err, err_size, "%s:%lu: bad p1 spec '%s'", path, lineno, p1_tok);
      ok = false; break;
    }
    if (fields >= 3 && !goof_input_parse_mask(p2_tok, &s.p2)) {
      script_fail(err, err_size, "%s:%lu: bad p2 spec '%s'", path, lineno, p2_tok);
      ok = false; break;
    }
    /* Normalised at PARSE time as well as at latch time, so the digest of the
     * script and the digest of what the guest saw describe the same masks. */
    goof_input_normalize(&s);

    if (count == capacity) {
      size_t grown = capacity * 2;
      GoofInputScriptEntry *bigger = realloc(entries, grown * sizeof *entries);
      if (!bigger) { script_fail(err, err_size, "out of memory"); ok = false; break; }
      entries = bigger;
      capacity = grown;
    }
    /* Zeroed explicitly, not just by calloc: realloc does not zero the grown
     * tail, and GoofInputScriptEntry carries 4 bytes of tail padding that the
     * script digest below hashes.  Without this a >16-entry script would get
     * a nondeterministic digest. */
    memset(&entries[count], 0, sizeof entries[count]);
    entries[count].epoch = (uint64_t)epoch;
    entries[count].sample = s;
    count++;
  }
  fclose(f);

  if (!ok) { free(entries); return false; }
  if (count == 0) {
    script_fail(err, err_size, "%s: no entries", path);
    free(entries);
    return false;
  }
  script->entries = entries;
  script->count = count;
  /* The entry array is fixed-width POD with no padding holes that matter --
   * it is memset to 0 up front and every byte is written -- so hashing it
   * directly is stable across processes. */
  script->digest = goof_gate_fnv1a64(entries, count * sizeof *entries);
  return true;
}

GoofInputSample goof_input_script_sample(const GoofInputScript *script,
                                         uint64_t epoch) {
  GoofInputSample out = {0, 0};
  if (!script || !script->entries) return out;
  /* Ascending, so a linear scan from the end finds the governing entry.  The
   * scripts this drives are tens of entries long; a binary search would be
   * the same answer with more code. */
  for (size_t i = script->count; i-- > 0;)
    if (script->entries[i].epoch <= epoch) return script->entries[i].sample;
  return out;
}

void goof_input_script_free(GoofInputScript *script) {
  if (!script) return;
  free(script->entries);
  memset(script, 0, sizeof *script);
}

/* --- digest ------------------------------------------------------------- */

void goof_input_digest_reset(GoofInputDigest *d) {
  if (!d) return;
  d->hash = UINT64_C(0xcbf29ce484222325);   /* FNV-1a 64 offset basis */
  d->latches = 0;
}

void goof_input_digest_update(GoofInputDigest *d, uint64_t guest_master_cycles,
                              uint16_t p1, uint16_t p2) {
  if (!d) return;
  if (!d->latches && !d->hash) goof_input_digest_reset(d);
  d->latches++;
  /* ordinal | guest time | p1 | p2, little-endian, in that order.  No host
   * quantity appears: the ordinal counts latches, not presentation ticks. */
  struct {
    uint64_t ordinal;
    uint64_t master_cycles;
    uint16_t p1, p2;
    uint16_t pad[2];
  } rec = {d->latches, guest_master_cycles, (uint16_t)(p1 & GOOF_BTN_MASK),
           (uint16_t)(p2 & GOOF_BTN_MASK), {0, 0}};
  const uint8_t *bytes = (const uint8_t *)&rec;
  uint64_t h = d->hash;
  for (size_t i = 0; i < sizeof rec; i++) {
    h ^= bytes[i];
    h *= UINT64_C(0x100000001b3);
  }
  d->hash = h;
}
