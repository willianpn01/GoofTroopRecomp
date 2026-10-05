#ifndef GOOF_VIDEO_MENU_H
#define GOOF_VIDEO_MENU_H

/* Goof Troop Recomp -- Settings > Video page (GOOF_ENHANCEMENTS_E3).
 * SDL-FREE.  Internal to host/ui; the page table contract is
 * GoofMenuPageDef (goof_input_menu.h). */

#include "host/ui/goof_input_menu.h"

extern const GoofMenuPageDef goof_video_page_def;

void goof_video_menu_on_open(GoofMenu *m);
/* Drops the working copy and, if a preview is live, re-applies the
 * committed values through the host.  No-op when nothing was previewed. */
void goof_video_menu_revert(GoofMenu *m);
void goof_video_menu_confirm(GoofMenu *m, GoofConfirmId id, bool yes);
/* "" for ids that are not Video's. */
const char *goof_video_menu_confirm_text(GoofConfirmId id);

#endif
