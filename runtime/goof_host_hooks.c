/* goof_host_hooks.c -- inert definitions of three engine host hooks that no
 * Goof target provides and no Goof execution path reaches.
 *
 * The snesrecomp runner references them from code Goof never runs (the SPC
 * player init, the per-frame dump callback and the savestate breadcrumb in
 * common_rtl.c).  On ELF, -Wl,--gc-sections drops those dead sections before
 * the references are resolved, so Linux links without any definition.  COFF
 * linkers (GNU ld PE, ld.lld MinGW) still require one, so this file is
 * compiled into goof_core on WIN32 only (cmake/goof_core.cmake).  The Linux
 * link inputs are unchanged.
 *
 * Every definition is semantically inert: null pointers and a no-op.  None
 * touches guest, scheduler, audio, input or renderer state.
 * GOOF_WINDOWS_PORTABILITY_IMPLEMENTATION. */
#include <stddef.h>

#include "framedump.h"
#include "host_report.h"
#include "spc_player.h"

SpcPlayer *g_spc_player = NULL;
FrameDumpCallback g_framedump_callback = NULL;

void host_report_breadcrumb(const char *fmt, ...) { (void)fmt; }
