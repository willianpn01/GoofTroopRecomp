#ifndef GOOF_CONFIG_H
#define GOOF_CONFIG_H

/* Goof Troop Recomp -- host configuration file (config.ini, schema 1).
 * GOOF_ENHANCEMENTS_E2_BUTTON_REMAPPING_CONFIG; [video] / [audio] added by
 * GOOF_ENHANCEMENTS_E3_HOST_SETTINGS_UI (still schema 1: the sections are
 * optional, a missing one means defaults, and an E2 build reading an E3
 * file only warns "unknown section" and keeps its bindings).
 *
 * SDL-FREE and guest-free.  Host settings only: nothing read here can reach
 * the guest except through the bindings that produce GoofInputSample, and no
 * oracle executable links this module (invariant O12).
 *
 * Format (explicit, documented in architecture/E2_INPUT_REMAPPING_CONFIG.md):
 *
 *   ; comment            # comment
 *   [meta]
 *   schema=1
 *   [input.player1.keyboard]      (also player2, and .gamepad)
 *   b=Z                           comma-separated tokens; "none" = unbound
 *   [video]                       display_mode, window_scale, pixel_aspect,
 *                                 fullscreen_scaling  (goof_present_settings.h)
 *   [audio]                       master_volume, mute, mute_when_unfocused
 *                                 (goof_audio_settings.h)
 *
 * Precedence: built-in defaults < file < command line (--config PATH,
 * --no-config, --portable choose the file; there are no per-binding CLI
 * overrides; --scale N overrides window_scale for the session only).  An
 * action or setting missing from its section keeps its default.
 *
 * GoofConfig is the STORED model (what the file says / will say).  The
 * frontend keeps the EFFECTIVE runtime values separately, because a CLI
 * override is effective without being stored (E3 doc section 3).
 */

#include <stdbool.h>
#include <stddef.h>

#include "host/goof_audio_settings.h"
#include "host/goof_bindings.h"
#include "host/goof_present_settings.h"

enum { GOOF_CONFIG_SCHEMA = 1 };
enum { GOOF_CONFIG_MAX_BYTES = 65536 };

typedef struct {
  GoofBindings bindings;          /* [input.*]  (E2)                        */
  GoofVideoSettings video;        /* [video]    (E3)                        */
  GoofAudioSettings audio;        /* [audio]    (E3)                        */
} GoofConfig;

typedef enum {
  GOOF_CONFIG_LOADED = 0,      /* file read; recoverable problems warned    */
  GOOF_CONFIG_MISSING,         /* no file: defaults                          */
  GOOF_CONFIG_REJECTED,        /* fatal for the FILE (unsupported or missing
                                * schema, oversize, binary, unreadable):
                                * defaults used, file left untouched        */
} GoofConfigLoadStatus;

/* Receives one concise warning per problem (no trailing newline). */
typedef void (*GoofConfigWarnFn)(void *user, const char *message);

void goof_config_defaults(GoofConfig *c);

/* Parses `text` (need not be NUL-terminated).  Always leaves a usable
 * config in *out: defaults, overlaid with every valid entry. */
GoofConfigLoadStatus goof_config_parse(const char *text, size_t len,
                                       GoofConfig *out, GoofConfigWarnFn warn,
                                       void *user);

GoofConfigLoadStatus goof_config_load_file(const char *path, GoofConfig *out,
                                           GoofConfigWarnFn warn, void *user);

/* Deterministic, complete serialisation (every action of every section and
 * every [video] / [audio] setting,
 * canonical tokens, LF line ends).  Returns the length written, or 0 if
 * `cap` is too small.  NUL-terminated. */
size_t goof_config_serialize(const GoofConfig *c, char *buf, size_t cap);

/* Atomic replace: creates missing parent directories, writes
 * "<path>.tmp", flushes and syncs it, closes it, then renames it over
 * `path` (MoveFileEx REPLACE_EXISTING|WRITE_THROUGH on Windows).  On any
 * failure the old file is untouched and the temporary is removed. */
bool goof_config_save_atomic(const char *path, const GoofConfig *c,
                             char *err, size_t err_cap);

/* ---- locations -------------------------------------------------------- */
typedef enum { GOOF_CONFIG_PLATFORM_POSIX, GOOF_CONFIG_PLATFORM_WINDOWS } GoofConfigPlatform;

typedef struct {
  const char *xdg_config_home;   /* $XDG_CONFIG_HOME (may be NULL / empty) */
  const char *home;              /* $HOME                                   */
  const char *appdata;           /* %APPDATA%                               */
} GoofConfigEnv;

GoofConfigPlatform goof_config_host_platform(void);

/* Reads XDG_CONFIG_HOME / HOME (POSIX) or APPDATA (Windows, through the
 * wide API and converted to UTF-8) from this process.  The strings live in
 * static storage until the next call. */
void goof_config_env_from_process(GoofConfigEnv *env);

/* Default per-user path (decision D5):
 *   POSIX    $XDG_CONFIG_HOME/GoofTroopRecomp/config.ini, where an unset,
 *            empty or relative XDG_CONFIG_HOME falls back to
 *            $HOME/.config (XDG Base Directory rule);
 *   Windows  %APPDATA%\GoofTroopRecomp\config.ini.
 * False when the needed variable is missing (the caller then runs with
 * defaults and cannot save). */
bool goof_config_default_path(GoofConfigPlatform platform, const GoofConfigEnv *env,
                              char *out, size_t cap);

/* Portable mode (explicit --portable only): config.ini inside `exe_dir`
 * (which may or may not end with a separator). */
bool goof_config_portable_path(const char *exe_dir, char *out, size_t cap);

#endif
