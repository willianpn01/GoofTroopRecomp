/* GOOF_ENHANCEMENTS_E1 -- the production SDL adapter (sdl_pads.c) driven by
 * real SDL2 device events from SDL virtual game controllers.  No hardware:
 * SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 hides every real pad.
 *
 * Each scenario starts SDL from scratch, in the frontend's order: the
 * devices that exist "at boot" are attached first, then goof_sdl_pads_open
 * (startup enumeration), then the event queue is drained through
 * goof_sdl_pads_handle_event exactly as main_sdl.c does -- which includes the
 * CONTROLLERDEVICEADDED burst SDL queues for pads already present, the event
 * that made the pre-E1 frontend open one pad into both slots. */
#include <SDL.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "goof_input.h"
#include "host/goof_host_input.h"
#include "sdl_pads.h"

static int failures;
static int case_failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; case_failures++; \
  printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static GoofSdlPads pads;

/* Test isolation: only SDL virtual pads may be visible.  The vendor filter
 * is enough on Linux; on Windows (and Wine) XInput/RawInput/WGI/HIDAPI pads
 * bypass it, so those hardware backends are switched off as well. */
static void hide_real_pads(void) {
  SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x0000/0x0000");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
  SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
  SDL_SetHint(SDL_HINT_DIRECTINPUT_ENABLED, "0");
}

static int added_events, removed_events;

static SDL_JoystickID attach(const char *name) {
  SDL_VirtualJoystickDesc d;
  memset(&d, 0, sizeof d);
  d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
  d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
  d.naxes = SDL_CONTROLLER_AXIS_MAX;
  d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
  d.name = name;
  int index = SDL_JoystickAttachVirtualEx(&d);
  CHECK(index >= 0, "attach %s: %s", name, SDL_GetError());
  return index >= 0 ? SDL_JoystickGetDeviceInstanceID(index) : -1;
}

static void detach(SDL_JoystickID id) {
  for (int i = 0; i < SDL_NumJoysticks(); i++)
    if (SDL_JoystickGetDeviceInstanceID(i) == id) {
      CHECK(SDL_JoystickDetachVirtual(i) == 0, "detach %d: %s", (int)id, SDL_GetError());
      return;
    }
  CHECK(0, "detach: id %d not found", (int)id);
}

/* The frontend's event loop, reduced to the pad events. */
static void pump(void) {
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    if (ev.type == SDL_CONTROLLERDEVICEADDED) added_events++;
    if (ev.type == SDL_CONTROLLERDEVICEREMOVED) removed_events++;
    goof_sdl_pads_handle_event(&pads, &ev);
  }
}

static SDL_JoystickID owner(int slot) {
  const GoofControllerDevice *d = goof_controllers_slot(&pads.slots, slot);
  return d ? d->id : -1;
}

static void press(SDL_JoystickID id, int button, bool down) {
  SDL_Joystick *j = SDL_JoystickFromInstanceID(id);
  CHECK(j != NULL, "press: id %d not open", (int)id);
  if (j) SDL_JoystickSetVirtualButton(j, button, down ? 1 : 0);
}

/* What main_sdl.c's input_sample hands goof_app_step, keyboard idle. */
static GoofInputSample sample(void) {
  SDL_GameControllerUpdate();
  GoofHostInputFrame f;
  memset(&f, 0, sizeof f);
  for (int k = 0; k < GOOF_HOST_PLAYERS; k++)
    f.pad_present[k] = goof_sdl_pads_read(&pads, k, &f.pad[k]);
  return goof_host_default_sample(&f);
}

static void boot(void) {
  hide_real_pads();
  if (SDL_Init(SDL_INIT_JOYSTICK) != 0) { printf("SDL_Init: %s\n", SDL_GetError()); failures++; }
  added_events = removed_events = 0;
  case_failures = 0;
}

static void start_frontend(void) {
  CHECK(goof_sdl_pads_open(&pads, stdout), "goof_sdl_pads_open");
  pump();                               /* first frame: the startup ADDED burst */
}

static void shutdown_all(void) {
  goof_sdl_pads_close(&pads);
  SDL_Quit();
}

static void result(const char *name, const char *what) {
  CHECK(goof_controllers_consistent(&pads.slots), "invariants");
  const GoofControllerDevice *p1 = goof_controllers_slot(&pads.slots, 0);
  const GoofControllerDevice *p2 = goof_controllers_slot(&pads.slots, 1);
  CHECK(!(p1 && p2 && p1->handle == p2->handle), "one handle in both slots");
  printf("%s %s %s  [P1=%s P2=%s tracked=%zu added_events=%d removed_events=%d]\n",
         name, case_failures ? "FAIL" : "PASS", what,
         p1 ? p1->name : "EMPTY", p2 ? p2->name : "EMPTY", pads.slots.count,
         added_events, removed_events);
  case_failures = 0;
}

typedef struct { int button; uint16_t snes; const char *label; } Bind;
static const Bind kBinds[] = {
  {SDL_CONTROLLER_BUTTON_DPAD_UP, GOOF_BTN_UP, "Up"},
  {SDL_CONTROLLER_BUTTON_DPAD_DOWN, GOOF_BTN_DOWN, "Down"},
  {SDL_CONTROLLER_BUTTON_DPAD_LEFT, GOOF_BTN_LEFT, "Left"},
  {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, GOOF_BTN_RIGHT, "Right"},
  {SDL_CONTROLLER_BUTTON_A, GOOF_BTN_B, "bottom->B"},
  {SDL_CONTROLLER_BUTTON_B, GOOF_BTN_A, "right->A"},
  {SDL_CONTROLLER_BUTTON_X, GOOF_BTN_Y, "left->Y"},
  {SDL_CONTROLLER_BUTTON_Y, GOOF_BTN_X, "top->X"},
  {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, GOOF_BTN_L, "L"},
  {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, GOOF_BTN_R, "R"},
  {SDL_CONTROLLER_BUTTON_START, GOOF_BTN_START, "Start"},
  {SDL_CONTROLLER_BUTTON_BACK, GOOF_BTN_SELECT, "Select"},
};

static void check_mapping(SDL_JoystickID id, int player) {
  for (size_t i = 0; i < sizeof kBinds / sizeof kBinds[0]; i++) {
    press(id, kBinds[i].button, true);
    GoofInputSample s = sample();
    uint16_t mine = player == 0 ? s.p1 : s.p2, other = player == 0 ? s.p2 : s.p1;
    CHECK(mine == kBinds[i].snes && other == 0, "P%d %s: p1=%03x p2=%03x",
          player + 1, kBinds[i].label, s.p1, s.p2);
    press(id, kBinds[i].button, false);
  }
  GoofInputSample s = sample();
  CHECK(s.p1 == 0 && s.p2 == 0, "release left bits %03x/%03x", s.p1, s.p2);
}

/* argc/argv: on Windows SDL.h renames main to SDL_main(int, char **). */
int main(int argc, char **argv) {
  (void)argc; (void)argv;
  /* V1: zero pads. */
  boot(); start_frontend();
  CHECK(owner(0) == -1 && owner(1) == -1, "slots not empty");
  result("V1", "startup, zero pads -> P1 EMPTY, P2 EMPTY");
  shutdown_all();

  /* V2: one pad at boot -- the E0 defect scenario. */
  boot();
  SDL_JoystickID a = attach("virtual pad A");
  start_frontend();
  CHECK(added_events >= 1, "SDL delivered no startup ADDED event (scenario not exercised)");
  CHECK(owner(0) == a && owner(1) == -1, "expected P1=A P2=EMPTY, got %d/%d", owner(0), owner(1));
  press(a, SDL_CONTROLLER_BUTTON_START, true);
  { GoofInputSample s = sample();
    CHECK(s.p1 == GOOF_BTN_START && s.p2 == 0, "A Start -> p1=%03x p2=%03x", s.p1, s.p2); }
  press(a, SDL_CONTROLLER_BUTTON_START, false);
  result("V2", "one pad at boot + startup ADDED burst -> P1=A only; A's Start reaches p1 only");

  /* V3: hot-plug B; two-player isolation and full per-player mapping. */
  SDL_JoystickID b = attach("virtual pad B");
  pump();
  CHECK(owner(0) == a && owner(1) == b, "expected P1=A P2=B");
  check_mapping(a, 0);
  check_mapping(b, 1);
  press(a, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, true);
  press(b, SDL_CONTROLLER_BUTTON_A, true);
  { GoofInputSample s = sample();
    CHECK(s.p1 == GOOF_BTN_RIGHT && s.p2 == GOOF_BTN_B, "simultaneous p1=%03x p2=%03x", s.p1, s.p2); }
  press(a, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, false);
  press(b, SDL_CONTROLLER_BUTTON_A, false);
  result("V3", "hot-plug B -> P2=B; 12 controls map per player; A->p1 only, B->p2 only, simultaneous");

  /* V4: remove A; B keeps P2 and keeps working. */
  detach(a);
  pump();
  CHECK(owner(0) == -1 && owner(1) == b, "expected P1=EMPTY P2=B");
  press(b, SDL_CONTROLLER_BUTTON_START, true);
  { GoofInputSample s = sample();
    CHECK(s.p1 == 0 && s.p2 == GOOF_BTN_START, "B after A removed: %03x/%03x", s.p1, s.p2); }
  press(b, SDL_CONTROLLER_BUTTON_START, false);
  result("V4", "remove A -> P1 EMPTY, P2=B unchanged and still driving p2");

  /* V5: reconnect (a new instance) fills only the free slot. */
  SDL_JoystickID c = attach("virtual pad C");
  pump();
  CHECK(owner(0) == c && owner(1) == b, "expected P1=C P2=B");
  press(c, SDL_CONTROLLER_BUTTON_Y, true);
  { GoofInputSample s = sample();
    CHECK(s.p1 == GOOF_BTN_X && s.p2 == 0, "C top -> %03x/%03x", s.p1, s.p2); }
  press(c, SDL_CONTROLLER_BUTTON_Y, false);
  result("V5", "connect C -> fills P1 only; P2=B undisturbed");

  /* V6: remove B; C keeps P1. */
  detach(b);
  pump();
  CHECK(owner(0) == c && owner(1) == -1, "expected P1=C P2=EMPTY");
  press(c, SDL_CONTROLLER_BUTTON_START, true);
  { GoofInputSample s = sample();
    CHECK(s.p1 == GOOF_BTN_START && s.p2 == 0, "C after B removed: %03x/%03x", s.p1, s.p2); }
  press(c, SDL_CONTROLLER_BUTTON_START, false);
  result("V6", "remove B -> P1=C unchanged, P2 EMPTY");

  /* V7: more pads than slots; removing a waiting pad changes nothing;
   * removing an owner hands its slot to the longest-waiting pad. */
  SDL_JoystickID d = attach("virtual pad D");
  SDL_JoystickID e = attach("virtual pad E");
  SDL_JoystickID f = attach("virtual pad F");
  pump();
  CHECK(owner(0) == c && owner(1) == d, "expected P1=C P2=D");
  press(e, SDL_CONTROLLER_BUTTON_START, true);
  { GoofInputSample s = sample();
    CHECK(s.p1 == 0 && s.p2 == 0, "waiting pad E reached the guest: %03x/%03x", s.p1, s.p2); }
  press(e, SDL_CONTROLLER_BUTTON_START, false);
  detach(e);
  pump();
  CHECK(owner(0) == c && owner(1) == d, "removing waiting E changed slots");
  detach(c);
  pump();
  CHECK(owner(0) == f && owner(1) == d, "expected waiting F promoted to P1");
  result("V7", "5 pads seen: 2 assigned, waiting pad inert, waiting removal inert, freed P1 -> waiting F");
  shutdown_all();

  /* V8: two pads at boot, plus a third. */
  boot();
  SDL_JoystickID p = attach("virtual pad P");
  SDL_JoystickID q = attach("virtual pad Q");
  SDL_JoystickID r = attach("virtual pad R");
  start_frontend();
  CHECK(added_events == 3, "expected 3 startup ADDED events, got %d", added_events);
  CHECK(owner(0) == p && owner(1) == q, "expected P1=P P2=Q");
  CHECK(pads.slots.count == 3, "expected 3 tracked, got %zu", pads.slots.count);
  { const GoofControllerDevice *rd = goof_controllers_find(&pads.slots, r);
    CHECK(rd && rd->slot == GOOF_CONTROLLER_NO_SLOT, "R should wait"); }
  /* A redundant rescan (as the ADDED handler does) is idempotent. */
  goof_sdl_pads_scan(&pads);
  goof_sdl_pads_scan(&pads);
  CHECK(owner(0) == p && owner(1) == q && pads.slots.count == 3, "rescan changed state");
  result("V8", "three pads at boot, deterministic index order -> P1=P, P2=Q, R waits; rescans idempotent");
  shutdown_all();

  printf("GOOF_SDL_PADS_VIRTUAL_TEST %s failures=%d sdl=%d.%d.%d\n",
         failures ? "FAIL" : "PASS", failures, SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
         SDL_PATCHLEVEL);
  return failures ? 1 : 0;
}
