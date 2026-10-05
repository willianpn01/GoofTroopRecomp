/* Goof Troop Recomp -- controller ownership.  SDL-FREE.  See goof_controller.h. */
#include "goof_controller.h"

#include <string.h>

void goof_controllers_init(GoofControllerManager *m) {
  memset(m, 0, sizeof *m);
}

static int find_index(const GoofControllerManager *m, GoofDeviceId id) {
  for (size_t i = 0; i < m->count; i++)
    if (m->device[i].id == id) return (int)i;
  return -1;
}

static bool slot_taken(const GoofControllerManager *m, int slot) {
  for (size_t i = 0; i < m->count; i++)
    if (m->device[i].slot == slot) return true;
  return false;
}

static int lowest_free_slot(const GoofControllerManager *m) {
  for (int k = 0; k < GOOF_CONTROLLER_SLOTS; k++)
    if (!slot_taken(m, k)) return k;
  return GOOF_CONTROLLER_NO_SLOT;
}

static void copy_name(char *dst, const char *src) {
  if (!src) src = "";
  size_t n = strlen(src);
  if (n >= GOOF_CONTROLLER_NAME_MAX) n = GOOF_CONTROLLER_NAME_MAX - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

GoofControllerAddResult goof_controllers_add(GoofControllerManager *m,
                                             GoofDeviceId id, void *handle,
                                             const char *name, int *out_slot) {
  int existing = find_index(m, id);
  if (existing >= 0) {
    if (out_slot) *out_slot = m->device[existing].slot;
    return GOOF_CONTROLLER_ADD_DUPLICATE;
  }
  if (out_slot) *out_slot = GOOF_CONTROLLER_NO_SLOT;
  if (m->count >= GOOF_CONTROLLER_MAX_DEVICES) return GOOF_CONTROLLER_ADD_FULL;
  int slot = lowest_free_slot(m);      /* before the new entry is visible */
  GoofControllerDevice *d = &m->device[m->count++];
  d->id = id;
  d->handle = handle;
  d->slot = slot;
  copy_name(d->name, name);
  if (out_slot) *out_slot = d->slot;
  return d->slot == GOOF_CONTROLLER_NO_SLOT ? GOOF_CONTROLLER_ADD_WAITING
                                            : GOOF_CONTROLLER_ADD_ASSIGNED;
}

GoofControllerRemoval goof_controllers_remove(GoofControllerManager *m,
                                              GoofDeviceId id) {
  GoofControllerRemoval r;
  memset(&r, 0, sizeof r);
  r.freed_slot = GOOF_CONTROLLER_NO_SLOT;
  int i = find_index(m, id);
  if (i < 0) return r;
  r.removed = true;
  r.handle = m->device[i].handle;
  r.freed_slot = m->device[i].slot;
  copy_name(r.name, m->device[i].name);
  /* Keep connection order: shift the tail down by one. */
  memmove(&m->device[i], &m->device[i + 1],
          (m->count - (size_t)i - 1) * sizeof m->device[0]);
  m->count--;
  if (r.freed_slot != GOOF_CONTROLLER_NO_SLOT) {
    for (size_t k = 0; k < m->count; k++) {
      if (m->device[k].slot != GOOF_CONTROLLER_NO_SLOT) continue;
      m->device[k].slot = r.freed_slot;
      r.promoted = true;
      r.promoted_id = m->device[k].id;
      break;
    }
  }
  return r;
}

const GoofControllerDevice *goof_controllers_find(const GoofControllerManager *m,
                                                  GoofDeviceId id) {
  int i = find_index(m, id);
  return i < 0 ? NULL : &m->device[i];
}

const GoofControllerDevice *goof_controllers_slot(const GoofControllerManager *m,
                                                  int slot) {
  for (size_t i = 0; i < m->count; i++)
    if (m->device[i].slot == slot) return &m->device[i];
  return NULL;
}

bool goof_controllers_consistent(const GoofControllerManager *m) {
  if (m->count > GOOF_CONTROLLER_MAX_DEVICES) return false;
  int owners[GOOF_CONTROLLER_SLOTS] = {0};
  size_t waiting = 0;
  for (size_t i = 0; i < m->count; i++) {
    for (size_t j = i + 1; j < m->count; j++) {
      if (m->device[i].id == m->device[j].id) return false;   /* one id, once */
      if (m->device[i].handle && m->device[i].handle == m->device[j].handle)
        return false;                                         /* one handle, once */
    }
    int s = m->device[i].slot;
    if (s == GOOF_CONTROLLER_NO_SLOT) { waiting++; continue; }
    if (s < 0 || s >= GOOF_CONTROLLER_SLOTS) return false;
    owners[s]++;
  }
  for (int k = 0; k < GOOF_CONTROLLER_SLOTS; k++) {
    if (owners[k] > 1) return false;                          /* one owner  */
    if (owners[k] == 0 && waiting > 0) return false;          /* no idle slot while a device waits */
  }
  return true;
}
