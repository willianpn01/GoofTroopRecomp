#ifndef GOOF_AUDIO_MENU_H
#define GOOF_AUDIO_MENU_H

/* Goof Troop Recomp -- Settings > Audio page (GOOF_ENHANCEMENTS_E3).
 * SDL-FREE.  Internal to host/ui. */

#include "host/ui/goof_input_menu.h"

extern const GoofMenuPageDef goof_audio_page_def;

void goof_audio_menu_on_open(GoofMenu *m);
void goof_audio_menu_revert(GoofMenu *m);
void goof_audio_menu_confirm(GoofMenu *m, GoofConfirmId id, bool yes);
const char *goof_audio_menu_confirm_text(GoofConfirmId id);

#endif
