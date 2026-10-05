/* Goof Troop Recomp -- SDL2 GameController adapter.  See sdl_pads.h.
 *
 * SDL2 identity rules this file relies on (SDL 2.30, SDL_joystick.h /
 * SDL_gamecontroller.h / SDL_events.h):
 *
 *   device index    position in the CURRENT enumeration, 0..SDL_NumJoysticks()-1.
 *                   It shifts when another device goes away, so it is used only
 *                   to open a device, never to remember one.
 *   instance id     SDL_JoystickID, unique for as long as the device stays
 *                   connected, never reused within a run.  This is the key.
 *   CONTROLLERDEVICEADDED   cdevice.which = DEVICE INDEX.  Delivered at
 *                   subsystem start for every controller already present, and
 *                   again whenever one is attached.
 *   CONTROLLERDEVICEREMOVED cdevice.which = INSTANCE ID.
 *   SDL_GameControllerOpen on an already-open device returns the SAME pointer
 *                   with its reference count raised.  The pre-E1 frontend
 *                   re-opened on every ADDED and so put one pad into both
 *                   slots; here a tracked instance id is never opened again.
 */
#include "sdl_pads.h"

#include <string.h>

/* goof_host_input.h numbers buttons and axes the SDL way; keep them equal so
 * reading a pad is a straight copy. */
_Static_assert((int)SDL_CONTROLLER_BUTTON_A == (int)GOOF_HOST_BTN_SOUTH, "button order");
_Static_assert((int)SDL_CONTROLLER_BUTTON_B == (int)GOOF_HOST_BTN_EAST, "button order");
_Static_assert((int)SDL_CONTROLLER_BUTTON_X == (int)GOOF_HOST_BTN_WEST, "button order");
_Static_assert((int)SDL_CONTROLLER_BUTTON_Y == (int)GOOF_HOST_BTN_NORTH, "button order");
_Static_assert((int)SDL_CONTROLLER_BUTTON_BACK == (int)GOOF_HOST_BTN_BACK, "button order");
_Static_assert((int)SDL_CONTROLLER_BUTTON_GUIDE == (int)GOOF_HOST_BTN_GUIDE, "button order");
_Static_assert((int)SDL_CONTROLLER_BUTTON_START == (int)GOOF_HOST_BTN_START, "button order");
_Static_assert((int)SDL_CONTROLLER_BUTTON_LEFTSTICK == (int)GOOF_HOST_BTN_LEFT_STICK, "button order");
_Static_assert((int)SDL_CONTROLLER_BUTTON_RIGHTSTICK == (int)GOOF_HOST_BTN_RIGHT_STICK, "button order");
_Static_assert((int)SDL_CONTROLLER_BUTTON_LEFTSHOULDER == (int)GOOF_HOST_BTN_LEFT_SHOULDER, "button order");
_Static_assert((int)SDL_CONTROLLER_BUTTON_RIGHTSHOULDER == (int)GOOF_HOST_BTN_RIGHT_SHOULDER, "button order");
_Static_assert((int)SDL_CONTROLLER_BUTTON_DPAD_UP == (int)GOOF_HOST_BTN_DPAD_UP, "button order");
_Static_assert((int)SDL_CONTROLLER_BUTTON_DPAD_DOWN == (int)GOOF_HOST_BTN_DPAD_DOWN, "button order");
_Static_assert((int)SDL_CONTROLLER_BUTTON_DPAD_LEFT == (int)GOOF_HOST_BTN_DPAD_LEFT, "button order");
_Static_assert((int)SDL_CONTROLLER_BUTTON_DPAD_RIGHT == (int)GOOF_HOST_BTN_DPAD_RIGHT, "button order");
_Static_assert((int)SDL_CONTROLLER_AXIS_LEFTX == (int)GOOF_HOST_AXIS_LEFT_X, "axis order");
_Static_assert((int)SDL_CONTROLLER_AXIS_LEFTY == (int)GOOF_HOST_AXIS_LEFT_Y, "axis order");
_Static_assert((int)SDL_CONTROLLER_AXIS_RIGHTX == (int)GOOF_HOST_AXIS_RIGHT_X, "axis order");
_Static_assert((int)SDL_CONTROLLER_AXIS_RIGHTY == (int)GOOF_HOST_AXIS_RIGHT_Y, "axis order");
_Static_assert((int)SDL_CONTROLLER_AXIS_TRIGGERLEFT == (int)GOOF_HOST_AXIS_TRIGGER_LEFT, "axis order");
_Static_assert((int)SDL_CONTROLLER_AXIS_TRIGGERRIGHT == (int)GOOF_HOST_AXIS_TRIGGER_RIGHT, "axis order");
_Static_assert(GOOF_HOST_BTN_COUNT <= 32, "buttons fit GoofHostPadState.buttons");

static const char *type_name(SDL_GameControllerType t) {
  switch (t) {
    case SDL_CONTROLLER_TYPE_XBOX360:             return "XBOX360";
    case SDL_CONTROLLER_TYPE_XBOXONE:             return "XBOXONE";
    case SDL_CONTROLLER_TYPE_PS3:                 return "PS3";
    case SDL_CONTROLLER_TYPE_PS4:                 return "PS4";
    case SDL_CONTROLLER_TYPE_PS5:                 return "PS5";
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return "SWITCH_PRO";
    case SDL_CONTROLLER_TYPE_VIRTUAL:             return "VIRTUAL";
    default:                                      return "OTHER";
  }
}

static const char *slot_name(int slot) {
  switch (slot) {
    case 0:  return "P1";
    case 1:  return "P2";
    default: return "NONE";
  }
}

/* Informational only: the Steam Input virtual pad is USB 28DE:11FF (the ID
 * SDL's own controller database lists as "Steam Virtual Gamepad").  It is
 * reported so a human can see it, and is NOT suppressed -- see
 * E1_MODERN_CONTROLLER.md section 5. */
static const char *origin_of(int device_index, SDL_GameController *c) {
  if (SDL_JoystickIsVirtual(device_index)) return "sdl_virtual";  /* takes an INDEX */
  if (SDL_GameControllerGetVendor(c) == 0x28DE &&
      SDL_GameControllerGetProduct(c) == 0x11FF) return "steam_virtual";
  return "device";
}

static void log_slots(const GoofSdlPads *p) {
  if (!p->log) return;
  fprintf(p->log, "HOST_GAMEPAD SLOTS");
  for (int k = 0; k < GOOF_CONTROLLER_SLOTS; k++) {
    const GoofControllerDevice *d = goof_controllers_slot(&p->slots, k);
    if (d) fprintf(p->log, " %s=\"%s\"(id=%d)", slot_name(k), d->name, (int)d->id);
    else   fprintf(p->log, " %s=EMPTY", slot_name(k));
  }
  size_t waiting = 0;
  for (size_t i = 0; i < p->slots.count; i++)
    if (p->slots.device[i].slot == GOOF_CONTROLLER_NO_SLOT) waiting++;
  fprintf(p->log, " waiting=%zu\n", waiting);
  fflush(p->log);
}

/* Returns true if the tracked set changed. */
static bool adopt_index(GoofSdlPads *p, int index) {
  if (!SDL_IsGameController(index)) return false;
  SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
  if (id < 0 || goof_controllers_find(&p->slots, id)) return false;
  SDL_GameController *c = SDL_GameControllerOpen(index);
  if (!c) {
    if (p->log) fprintf(p->log, "HOST_GAMEPAD OPEN_FAILED id=%d reason=\"%s\"\n",
                        (int)id, SDL_GetError());
    return false;
  }
  /* What was opened is authoritative: if the enumeration shifted between the
   * two calls, key on the instance we actually hold. */
  SDL_JoystickID got = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
  if (got < 0 || goof_controllers_find(&p->slots, got)) {
    SDL_GameControllerClose(c);          /* drops only the extra reference */
    return false;
  }
  /* `name` is the joystick name, per device; `mapping` is the SDL controller
   * mapping's name ("Steam Virtual Gamepad", ...), shared by every device on
   * that mapping.  The slot table keeps the device name; both are logged. */
  const char *name = SDL_JoystickName(SDL_GameControllerGetJoystick(c));
  const char *mapping = SDL_GameControllerName(c);
  int slot = GOOF_CONTROLLER_NO_SLOT;
  GoofControllerAddResult r = goof_controllers_add(&p->slots, got, c, name, &slot);
  if (r == GOOF_CONTROLLER_ADD_FULL) {
    SDL_GameControllerClose(c);
    if (p->log) fprintf(p->log, "HOST_GAMEPAD IGNORED id=%d reason=device_table_full\n",
                        (int)got);
    return false;
  }
  if (p->log) {
    fprintf(p->log,
            "HOST_GAMEPAD CONNECT id=%d slot=%s name=\"%s\" mapping=\"%s\" type=%s"
            " origin=%s vid=%04x pid=%04x\n",
            (int)got, r == GOOF_CONTROLLER_ADD_ASSIGNED ? slot_name(slot) : "WAITING",
            name ? name : "", mapping ? mapping : "",
            type_name(SDL_GameControllerGetType(c)), origin_of(index, c),
            SDL_GameControllerGetVendor(c), SDL_GameControllerGetProduct(c));
  }
  return true;
}

static bool scan(GoofSdlPads *p) {
  bool changed = false;
  int n = SDL_NumJoysticks();
  for (int i = 0; i < n; i++) changed |= adopt_index(p, i);
  return changed;
}

bool goof_sdl_pads_open(GoofSdlPads *p, FILE *log) {
  memset(p, 0, sizeof *p);
  p->log = log;
  goof_controllers_init(&p->slots);
  /* POSITION-TRUE (decision D4).  SDL2 defaults this hint to "1", which makes
   * Nintendo-layout pads report the button LABELLED A (on the right) as
   * SDL_CONTROLLER_BUTTON_A.  "0" makes every pad report by position, so the
   * bottom button is always SDL A.  Normal priority: an explicit environment
   * variable still wins, which keeps it overridable for diagnosis. */
  SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
    if (log) fprintf(log, "HOST_GAMEPAD UNAVAILABLE reason=\"%s\"\n", SDL_GetError());
    return false;
  }
  p->active = true;
  /* Startup enumeration, exactly once, in device-index order.  SDL has also
   * queued one CONTROLLERDEVICEADDED per device found here; handling those
   * later finds every instance already tracked and changes nothing. */
  scan(p);
  log_slots(p);
  return true;
}

void goof_sdl_pads_scan(GoofSdlPads *p) {
  if (!p->active) return;
  if (scan(p)) log_slots(p);
}

bool goof_sdl_pads_handle_event(GoofSdlPads *p, const SDL_Event *ev) {
  if (ev->type == SDL_CONTROLLERDEVICEADDED) {
    /* `which` is a device index and may already be stale; a full scan keyed
     * by instance id is both simpler and immune to that. */
    goof_sdl_pads_scan(p);
    return true;
  }
  if (ev->type == SDL_CONTROLLERDEVICEREMOVED) {
    if (!p->active) return true;
    GoofControllerRemoval r = goof_controllers_remove(&p->slots, ev->cdevice.which);
    if (!r.removed) return true;       /* never tracked: nothing to release */
    SDL_GameControllerClose((SDL_GameController *)r.handle);
    if (p->log) {
      fprintf(p->log, "HOST_GAMEPAD DISCONNECT id=%d slot=%s name=\"%s\"\n",
              (int)ev->cdevice.which,
              r.freed_slot == GOOF_CONTROLLER_NO_SLOT ? "WAITING" : slot_name(r.freed_slot),
              r.name);
      if (r.promoted) {
        const GoofControllerDevice *d = goof_controllers_find(&p->slots, r.promoted_id);
        fprintf(p->log, "HOST_GAMEPAD ASSIGN id=%d slot=%s name=\"%s\" reason=slot_freed\n",
                (int)r.promoted_id, slot_name(r.freed_slot), d ? d->name : "");
      }
    }
    log_slots(p);
    return true;
  }
  return false;
}

bool goof_sdl_pads_read(const GoofSdlPads *p, int slot, GoofHostPadState *out) {
  memset(out, 0, sizeof *out);
  if (!p->active) return false;
  const GoofControllerDevice *d = goof_controllers_slot(&p->slots, slot);
  if (!d) return false;
  SDL_GameController *c = (SDL_GameController *)d->handle;
  for (int b = 0; b < GOOF_HOST_BTN_COUNT; b++)
    if (SDL_GameControllerGetButton(c, (SDL_GameControllerButton)b))
      out->buttons |= 1u << b;
  for (int a = 0; a < GOOF_HOST_AXIS_COUNT; a++)
    out->axes[a] = SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)a);
  return true;
}

void goof_sdl_pads_close(GoofSdlPads *p) {
  for (size_t i = 0; i < p->slots.count; i++)
    if (p->slots.device[i].handle)
      SDL_GameControllerClose((SDL_GameController *)p->slots.device[i].handle);
  goof_controllers_init(&p->slots);
  p->active = false;
}
