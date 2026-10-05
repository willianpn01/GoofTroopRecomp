/* GOOF_ENHANCEMENTS_E1 -- player-slot ownership tests, no hardware.
 *
 * Drives goof_controller.[ch] with fake devices exactly as sdl_pads.c does:
 * startup enumeration = one add per present device in index order; SDL's
 * start-up CONTROLLERDEVICEADDED burst = the same ids added again.
 * T1..T12 are the E1 required cases; T13..T15 cover the waiting-device and
 * table-full policy; F1 is a randomized invariant check. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host/goof_controller.h"

static int failures;
static int case_failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; case_failures++; \
  printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Fake device handles: distinct addresses, like distinct SDL_GameController*. */
static char handle_store[64];
#define H(id) ((void *)&handle_store[(id)])

enum { A = 1, B = 2, C = 3, D = 4, E = 5 };

static GoofControllerManager m;

static GoofDeviceId owner(int slot) {
  const GoofControllerDevice *d = goof_controllers_slot(&m, slot);
  return d ? d->id : 0;          /* 0 = EMPTY (fake ids start at 1) */
}

static GoofControllerAddResult add(GoofDeviceId id) {
  static const char *names[] = {"", "pad A", "pad B", "pad C", "pad D", "pad E"};
  return goof_controllers_add(&m, id, H(id), id < 6 ? names[id] : "pad", NULL);
}

static void begin(void) { goof_controllers_init(&m); case_failures = 0; }

static void end(const char *name, const char *what) {
  CHECK(goof_controllers_consistent(&m), "invariants violated");
  printf("%s %s %s  [P1=%s P2=%s devices=%zu]\n", name,
         case_failures ? "FAIL" : "PASS", what,
         owner(0) ? goof_controllers_slot(&m, 0)->name : "EMPTY",
         owner(1) ? goof_controllers_slot(&m, 1)->name : "EMPTY", m.count);
}

/* Startup: enumerate once, then SDL's queued ADDED burst for the same ids. */
static void startup(const GoofDeviceId *ids, int n) {
  for (int i = 0; i < n; i++) add(ids[i]);
  for (int i = 0; i < n; i++)
    CHECK(add(ids[i]) == GOOF_CONTROLLER_ADD_DUPLICATE, "startup ADDED for %d not a duplicate", ids[i]);
}

int main(void) {
  /* T1 */
  begin(); startup(NULL, 0);
  CHECK(owner(0) == 0 && owner(1) == 0, "slots not empty");
  end("T1", "zero controllers -> P1 empty, P2 empty");

  /* T2 */
  begin(); { GoofDeviceId ids[] = {A}; startup(ids, 1); }
  CHECK(owner(0) == A && owner(1) == 0, "expected P1=A P2=empty");
  end("T2", "one controller -> P1=A, P2 empty");

  /* T3 */
  begin(); { GoofDeviceId ids[] = {A, B}; startup(ids, 2); }
  CHECK(owner(0) == A && owner(1) == B, "expected P1=A P2=B");
  end("T3", "two controllers -> P1=A, P2=B");

  /* T4: the pre-E1 defect path -- a re-scan re-offering the same device. */
  begin(); add(A);
  for (int i = 0; i < 5; i++)
    CHECK(add(A) == GOOF_CONTROLLER_ADD_DUPLICATE, "re-add of A not a duplicate");
  CHECK(owner(0) == A && owner(1) == 0, "A occupies more than one slot");
  { int slot = 99;
    CHECK(goof_controllers_add(&m, A, H(A), "pad A", &slot) == GOOF_CONTROLLER_ADD_DUPLICATE
          && slot == 0, "duplicate does not report A's own slot"); }
  end("T4", "one controller is never assigned twice (5 re-adds)");

  /* T5 */
  begin(); add(A);
  CHECK(add(B) == GOOF_CONTROLLER_ADD_ASSIGNED, "B not assigned");
  CHECK(owner(0) == A && owner(1) == B, "expected P1=A P2=B");
  end("T5", "A, then hot-plug B -> P1=A, P2=B");

  /* T6 */
  begin(); add(A); add(B);
  { GoofControllerRemoval r = goof_controllers_remove(&m, A);
    CHECK(r.removed && r.freed_slot == 0 && r.handle == H(A) && !r.promoted, "remove A result"); }
  CHECK(owner(0) == 0 && owner(1) == B, "expected P1=empty P2=B (no compaction)");
  end("T6", "A+B, remove A -> P1 empty, P2=B (no compaction)");

  /* T7 (continues T6) */
  case_failures = 0;
  CHECK(add(C) == GOOF_CONTROLLER_ADD_ASSIGNED, "C not assigned");
  CHECK(owner(0) == C && owner(1) == B, "expected P1=C P2=B");
  end("T7", "after T6, add C -> P1=C, P2=B");

  /* T8 */
  begin(); add(A); add(B);
  { GoofControllerRemoval r = goof_controllers_remove(&m, B);
    CHECK(r.removed && r.freed_slot == 1 && r.handle == H(B), "remove B result"); }
  CHECK(owner(0) == A && owner(1) == 0, "expected P1=A P2=empty");
  end("T8", "A+B, remove B -> P1=A, P2 empty");

  /* T9 */
  begin(); { GoofDeviceId ids[] = {A, B, C, D}; startup(ids, 4); }
  CHECK(owner(0) == A && owner(1) == B, "expected P1=A P2=B");
  CHECK(goof_controllers_find(&m, C) && goof_controllers_find(&m, C)->slot == GOOF_CONTROLLER_NO_SLOT,
        "C should be tracked and waiting");
  CHECK(goof_controllers_find(&m, D)->slot == GOOF_CONTROLLER_NO_SLOT, "D should wait");
  { int assigned = 0;
    for (size_t i = 0; i < m.count; i++) assigned += m.device[i].slot != GOOF_CONTROLLER_NO_SLOT;
    CHECK(assigned == 2, "%d devices assigned, expected 2", assigned); }
  end("T9", "four controllers -> only two assigned, C and D wait");

  /* T10 */
  begin(); add(A); add(B); add(C);
  { GoofControllerRemoval r = goof_controllers_remove(&m, C);
    CHECK(r.removed && r.freed_slot == GOOF_CONTROLLER_NO_SLOT && !r.promoted, "remove C result"); }
  CHECK(owner(0) == A && owner(1) == B, "assigned slots changed");
  { GoofControllerRemoval r = goof_controllers_remove(&m, 42);
    CHECK(!r.removed && r.handle == NULL, "unknown id reported as removed"); }
  CHECK(owner(0) == A && owner(1) == B, "unknown removal changed slots");
  end("T10", "remove unassigned (and unknown) controller -> slots unchanged");

  /* T11 */
  begin(); add(A); add(B);
  CHECK(add(A) == GOOF_CONTROLLER_ADD_DUPLICATE && add(B) == GOOF_CONTROLLER_ADD_DUPLICATE,
        "duplicate adds not reported");
  CHECK(owner(0) == A && owner(1) == B && m.count == 2, "duplicate add changed state");
  end("T11", "duplicate ADDED for already-owned instances -> no change");

  /* T12: startup enumeration followed by SDL's queued ADDED burst. */
  begin(); { GoofDeviceId ids[] = {A, B, C}; startup(ids, 3); }
  CHECK(owner(0) == A && owner(1) == B && m.count == 3, "startup burst duplicated a slot");
  end("T12", "startup ADDED events after enumeration -> no duplicate slots");

  /* T13: a freed slot goes to the longest-waiting device, and only it. */
  begin(); add(A); add(B); add(C); add(D);
  { GoofControllerRemoval r = goof_controllers_remove(&m, A);
    CHECK(r.promoted && r.promoted_id == C, "expected C promoted into P1"); }
  CHECK(owner(0) == C && owner(1) == B, "expected P1=C P2=B");
  CHECK(goof_controllers_find(&m, D)->slot == GOOF_CONTROLLER_NO_SLOT, "D should still wait");
  end("T13", "A+B+C+D, remove A -> waiting C takes P1, B stays P2, D waits");

  /* T14: reconnect of the same physical pad gets a NEW instance id in SDL and
   * is simply a new device: it fills the free slot. */
  begin(); add(A); add(B);
  goof_controllers_remove(&m, B);
  CHECK(add(E) == GOOF_CONTROLLER_ADD_ASSIGNED && owner(1) == E, "reconnect did not fill P2");
  CHECK(owner(0) == A, "P1 disturbed by reconnect");
  end("T14", "A+B, unplug B, replug (new id) -> fills P2 only");

  /* T15: the device table is bounded and fails closed. */
  begin();
  for (int i = 1; i <= GOOF_CONTROLLER_MAX_DEVICES; i++)
    CHECK(goof_controllers_add(&m, i, H(i), "pad", NULL) != GOOF_CONTROLLER_ADD_FULL, "early FULL");
  CHECK(goof_controllers_add(&m, 60, H(60), "pad", NULL) == GOOF_CONTROLLER_ADD_FULL, "no FULL");
  CHECK(owner(0) == 1 && owner(1) == 2, "FULL changed slots");
  end("T15", "device table full -> extra device ignored, slots unchanged");

  /* F1: random add/remove/duplicate sequences.  After every operation:
   * invariants hold; a slot whose owner is still connected keeps that owner;
   * an assigned new device got the lowest free slot. */
  begin();
  uint64_t rng = 0x243F6A8885A308D3ull;
  long ops = 0;
  for (int step = 0; step < 200000; step++) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    GoofDeviceId id = (GoofDeviceId)(1 + (rng >> 20) % 24);
    GoofDeviceId before[GOOF_CONTROLLER_SLOTS] = {owner(0), owner(1)};
    int lowest_free = !before[0] ? 0 : (!before[1] ? 1 : GOOF_CONTROLLER_NO_SLOT);
    if ((rng >> 40) & 1) {
      bool known = goof_controllers_find(&m, id) != NULL;
      int slot = 7;
      GoofControllerAddResult r = goof_controllers_add(&m, id, H(id), "pad", &slot);
      if (known) CHECK(r == GOOF_CONTROLLER_ADD_DUPLICATE, "F1 known id not duplicate");
      else if (r == GOOF_CONTROLLER_ADD_ASSIGNED) CHECK(slot == lowest_free, "F1 not lowest free slot");
      else if (r == GOOF_CONTROLLER_ADD_WAITING) CHECK(lowest_free == GOOF_CONTROLLER_NO_SLOT, "F1 waited with a free slot");
    } else {
      goof_controllers_remove(&m, id);
    }
    for (int k = 0; k < GOOF_CONTROLLER_SLOTS; k++)
      if (before[k] && before[k] != id)
        CHECK(owner(k) == before[k], "F1 slot %d moved from %d to %d", k, before[k], owner(k));
    CHECK(goof_controllers_consistent(&m), "F1 invariants violated at step %d", step);
    ops++;
    if (case_failures > 20) break;
  }
  CHECK(ops == 200000, "F1 stopped early after %ld ops", ops);
  printf("F1 %s randomized ops=%ld invariants+no_compaction+lowest_free_slot\n",
         case_failures ? "FAIL" : "PASS", ops);

  printf("GOOF_CONTROLLER_TEST %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
