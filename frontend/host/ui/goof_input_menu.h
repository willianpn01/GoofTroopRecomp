#ifndef GOOF_INPUT_MENU_H
#define GOOF_INPUT_MENU_H

/* Goof Troop Recomp -- Settings pages: page table contract and the Input
 * section (GOOF_ENHANCEMENTS_E2).  SDL-FREE.  Internal to host/ui. */

#include "host/ui/goof_menu.h"

/* One page of the Settings stack.  E3 adds pages by adding one of these and
 * an entry in goof_menu.c's page table / root section list. */
typedef struct {
  const char *title;                                   /* breadcrumb part  */
  int (*count)(const GoofMenu *m);                     /* selectable items */
  void (*activate)(GoofMenu *m, int item, const GoofUiEvent *trigger);
  void (*back)(GoofMenu *m);                           /* NULL: pop        */
  void (*lateral)(GoofMenu *m, int dir);               /* NULL: ignored    */
  void (*draw)(const GoofMenu *m, GoofUiDrawList *d);
} GoofMenuPageDef;

extern const GoofMenuPageDef goof_input_page_def;
extern const GoofMenuPageDef goof_player_page_def;
extern const GoofMenuPageDef goof_edit_page_def;

void goof_menu_push(GoofMenu *m, GoofMenuPageId page);
void goof_menu_pop(GoofMenu *m);
void goof_menu_ask(GoofMenu *m, GoofConfirmId id);

/* Input-section hooks called by goof_menu.c. */
void goof_input_menu_on_open(GoofMenu *m);
unsigned goof_input_menu_capture_event(GoofMenu *m, const GoofUiEvent *ev);
void goof_input_menu_confirm(GoofMenu *m, GoofConfirmId id, bool yes);
const char *goof_input_menu_confirm_text(GoofConfirmId id);
void goof_input_menu_draw_capture(const GoofMenu *m, GoofUiDrawList *d);

/* "P1 B" style labels and one-line binding lists for display. */
void goof_input_menu_list_label(const GoofBindList *l, GoofBindKind kind,
                                char *out, size_t cap, size_t width);

/* Tries to save the working copy (conflict check, then host->save). */
unsigned goof_input_menu_save(GoofMenu *m);

#endif
