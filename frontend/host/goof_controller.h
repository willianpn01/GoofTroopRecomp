#ifndef GOOF_CONTROLLER_H
#define GOOF_CONTROLLER_H

/* Goof Troop Recomp -- controller OWNERSHIP: which connected device drives
 * which player slot.  GOOF_ENHANCEMENTS_E1_MODERN_CONTROLLER.  SDL-FREE.
 *
 * Invariants (tested by goof_controller_test.c):
 *
 *   - a slot is EMPTY or owned by exactly one device;
 *   - a device owns at most one slot;
 *   - a device is identified by its runtime instance id (SDL_JoystickID in
 *     the SDL adapter), never by an enumeration index, so adding an id that
 *     is already known changes nothing;
 *   - a new device takes the lowest free slot; with none free it WAITS,
 *     unassigned, in connection order;
 *   - removing a device clears only its own slot.  The other slot never
 *     moves (no compaction: P2 stays P2).  A freed slot is given to the
 *     longest-waiting unassigned device, if any, else it stays empty until
 *     the next connection.
 *
 * The manager never opens or closes anything.  It stores an opaque handle
 * per device and hands it back on removal, so the caller (sdl_pads.c) owns
 * every open/close and the logic here runs without hardware.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  GOOF_CONTROLLER_SLOTS = 2,          /* P1, P2                              */
  GOOF_CONTROLLER_MAX_DEVICES = 16,   /* assigned + waiting                  */
  GOOF_CONTROLLER_NAME_MAX = 64
};

#define GOOF_CONTROLLER_NO_SLOT (-1)

typedef int32_t GoofDeviceId;

typedef struct {
  GoofDeviceId id;
  void *handle;                       /* opaque, owned by the caller         */
  int slot;                           /* 0..SLOTS-1, or GOOF_CONTROLLER_NO_SLOT */
  char name[GOOF_CONTROLLER_NAME_MAX];
} GoofControllerDevice;

/* Devices are kept in connection order: that order is what makes waiting
 * and promotion deterministic. */
typedef struct {
  GoofControllerDevice device[GOOF_CONTROLLER_MAX_DEVICES];
  size_t count;
} GoofControllerManager;

typedef enum {
  GOOF_CONTROLLER_ADD_ASSIGNED,       /* took a slot                         */
  GOOF_CONTROLLER_ADD_WAITING,        /* known, no slot free                 */
  GOOF_CONTROLLER_ADD_DUPLICATE,      /* id already known: nothing changed   */
  GOOF_CONTROLLER_ADD_FULL            /* device table full: not tracked      */
} GoofControllerAddResult;

typedef struct {
  bool removed;                       /* false: id was unknown, no change    */
  void *handle;                       /* the removed device's handle         */
  int freed_slot;                     /* slot it owned, or NO_SLOT           */
  char name[GOOF_CONTROLLER_NAME_MAX];
  /* A waiting device that received freed_slot, if any. */
  bool promoted;
  GoofDeviceId promoted_id;
} GoofControllerRemoval;

void goof_controllers_init(GoofControllerManager *m);

GoofControllerAddResult goof_controllers_add(GoofControllerManager *m,
                                             GoofDeviceId id, void *handle,
                                             const char *name, int *out_slot);

GoofControllerRemoval goof_controllers_remove(GoofControllerManager *m,
                                              GoofDeviceId id);

const GoofControllerDevice *goof_controllers_find(const GoofControllerManager *m,
                                                  GoofDeviceId id);

/* The device owning `slot`, or NULL when the slot is empty. */
const GoofControllerDevice *goof_controllers_slot(const GoofControllerManager *m,
                                                  int slot);

/* Checks every invariant above; false means a bug in this module. */
bool goof_controllers_consistent(const GoofControllerManager *m);

#endif
