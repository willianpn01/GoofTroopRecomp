/* Goof Troop Recomp -- host configuration file.  SDL-FREE.
 * See goof_config.h.  GOOF_ENHANCEMENTS_E2_BUTTON_REMAPPING_CONFIG;
 * [video] / [audio] sections: GOOF_ENHANCEMENTS_E3_HOST_SETTINGS_UI. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L   /* fileno, fsync, open */
#endif
#include "host/goof_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

void goof_config_defaults(GoofConfig *c) {
  memset(c, 0, sizeof *c);
  goof_bindings_defaults(&c->bindings);
  goof_video_defaults(&c->video);
  goof_audio_defaults(&c->audio);
}

static void warnf(GoofConfigWarnFn warn, void *user, const char *fmt, ...) {
  if (!warn) return;
  char msg[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  warn(user, msg);
}

/* ---- line scanner ----------------------------------------------------- */

typedef struct {
  const char *p, *end;
  int line;
  char buf[512];
  bool overlong;
} Lines;

/* Next line, CR/LF stripped, surrounding blanks trimmed. */
static const char *next_line(Lines *ls) {
  if (ls->p >= ls->end) return NULL;
  const char *start = ls->p;
  while (ls->p < ls->end && *ls->p != '\n') ls->p++;
  const char *stop = ls->p;
  if (ls->p < ls->end) ls->p++;
  ls->line++;
  if (stop > start && stop[-1] == '\r') stop--;
  while (start < stop && isspace((unsigned char)*start)) start++;
  while (stop > start && isspace((unsigned char)stop[-1])) stop--;
  size_t n = (size_t)(stop - start);
  ls->overlong = n >= sizeof ls->buf;
  if (ls->overlong) n = sizeof ls->buf - 1;
  memcpy(ls->buf, start, n);
  ls->buf[n] = '\0';
  return ls->buf;
}

static char *trim(char *s) {
  while (isspace((unsigned char)*s)) s++;
  char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
  return s;
}

typedef enum { SEC_NONE, SEC_META, SEC_BIND, SEC_VIDEO, SEC_AUDIO, SEC_UNKNOWN } SectionKind;

typedef struct {
  SectionKind kind;
  int player;
  GoofBindKind bind;
} Section;

static Section parse_section(const char *name) {
  Section s = {SEC_UNKNOWN, 0, GOOF_BIND_KEY};
  if (strcmp(name, "meta") == 0) { s.kind = SEC_META; return s; }
  if (strcmp(name, "video") == 0) { s.kind = SEC_VIDEO; return s; }
  if (strcmp(name, "audio") == 0) { s.kind = SEC_AUDIO; return s; }
  static const char *const kNames[2][2] = {
    {"input.player1.keyboard", "input.player1.gamepad"},
    {"input.player2.keyboard", "input.player2.gamepad"},
  };
  for (int p = 0; p < 2; p++)
    for (int k = 0; k < 2; k++)
      if (strcmp(name, kNames[p][k]) == 0) {
        s.kind = SEC_BIND; s.player = p; s.bind = (GoofBindKind)k;
      }
  return s;
}

static const char *kind_name(GoofBindKind k) {
  return k == GOOF_BIND_KEY ? "keyboard" : "gamepad";
}

/* Pass 1: find [meta] schema.  Silent.  Returns -1 when absent, -2 when
 * present but not a plain decimal. */
static long find_schema(const char *text, size_t len) {
  Lines ls = {text, text + len, 0, {0}, false};
  Section sec = {SEC_NONE, 0, GOOF_BIND_KEY};
  long schema = -1;
  const char *l;
  while ((l = next_line(&ls))) {
    if (!*l || *l == ';' || *l == '#') continue;
    if (*l == '[') {
      const char *close = strchr(l, ']');
      if (close && close[1] == '\0') {
        char name[128];
        size_t n = (size_t)(close - l - 1);
        if (n < sizeof name) {
          memcpy(name, l + 1, n); name[n] = '\0';
          sec = parse_section(trim(name));
        } else {
          sec.kind = SEC_UNKNOWN;
        }
      }
      continue;
    }
    if (sec.kind != SEC_META || schema != -1) continue;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", l);
    char *eq = strchr(tmp, '=');
    if (!eq) continue;
    *eq = '\0';
    if (strcmp(trim(tmp), "schema") != 0) continue;
    char *v = trim(eq + 1);
    if (!*v) return -2;
    long n = 0;
    for (char *q = v; *q; q++) {
      if (!isdigit((unsigned char)*q) || n > 100000) return -2;
      n = n * 10 + (*q - '0');
    }
    schema = n;
  }
  return schema;
}

/* Parses one binding value into *out.  Returns true if the line should
 * replace the default (explicit "none"/empty, or at least one valid token). */
static bool parse_value(char *value, GoofBindKind kind, GoofBindList *out,
                        int line, int player, const char *action,
                        GoofConfigWarnFn warn, void *user) {
  goof_bindlist_clear(out);
  char *v = trim(value);
  if (*v == '\0' || strcmp(v, "none") == 0) return true;
  int tokens = 0;
  char *save = v;
  for (;;) {
    char *comma = strchr(save, ',');
    if (comma) *comma = '\0';
    char *tok = trim(save);
    tokens++;
    uint16_t code;
    if (*tok == '\0') {
      warnf(warn, user, "line %d: empty token in P%d %s '%s'; ignored", line,
            player + 1, kind_name(kind), action);
    } else if (kind == GOOF_BIND_KEY && !goof_key_from_token(tok, &code)) {
      warnf(warn, user, "line %d: unknown key '%s' for P%d '%s'; ignored",
            line, tok, player + 1, action);
    } else if (kind == GOOF_BIND_KEY && goof_key_is_reserved(code)) {
      warnf(warn, user, "line %d: key '%s' is reserved for the host "
            "(F2 Settings, ESCAPE, P pause); ignored", line, tok);
    } else if (kind == GOOF_BIND_PAD && !goof_pad_from_token(tok, &code)) {
      warnf(warn, user, "line %d: unknown gamepad input '%s' for P%d '%s'; "
            "ignored", line, tok, player + 1, action);
    } else if (!goof_bindlist_contains(out, code) && !goof_bindlist_add(out, code)) {
      warnf(warn, user, "line %d: more than %d %s bindings for P%d '%s'; "
            "'%s' ignored", line, GOOF_BIND_MAX_PER_KIND, kind_name(kind),
            player + 1, action, tok);
    }
    if (!comma) break;
    save = comma + 1;
  }
  if (out->count == 0) {
    warnf(warn, user, "line %d: no valid %s binding for P%d '%s'; default kept",
          line, kind_name(kind), player + 1, action);
    return false;
  }
  (void)tokens;
  return true;
}

/* Keep-first conflict resolution in canonical order (player 1 keyboard,
 * player 1 gamepad, player 2 keyboard, player 2 gamepad; action order;
 * token order).  A later duplicate of a physical input is dropped. */
static void resolve_conflicts(GoofBindings *b, GoofConfigWarnFn warn, void *user) {
  for (int p = 0; p < GOOF_HOST_PLAYERS; p++)
    for (int k = 0; k < GOOF_BIND_KINDS; k++)
      for (int a = 0; a < GOOF_ACT_COUNT; a++) {
        GoofBindList *l = &b->player[p].list[k][a];
        for (int i = 0; i < l->count;) {
          uint16_t code = l->code[i];
          int owner_p = -1, owner_a = -1;
          for (int q = 0; q <= p && owner_p < 0; q++) {
            if (q != p && k == GOOF_BIND_PAD) continue;
            int limit = q == p ? a : GOOF_ACT_COUNT;
            for (int c = 0; c < limit; c++)
              if (goof_bindlist_contains(&b->player[q].list[k][c], code)) {
                owner_p = q; owner_a = c; break;
              }
          }
          if (owner_p < 0) { i++; continue; }
          char name[32];
          if (k == GOOF_BIND_KEY) goof_key_token(code, name, sizeof name);
          else snprintf(name, sizeof name, "%s", goof_pad_token(code));
          warnf(warn, user, "conflict: %s '%s' is bound to P%d %s and P%d %s; "
                "kept P%d %s, dropped from P%d %s", kind_name((GoofBindKind)k),
                name, owner_p + 1, goof_action_label((GoofSnesAction)owner_a),
                p + 1, goof_action_label((GoofSnesAction)a), owner_p + 1,
                goof_action_label((GoofSnesAction)owner_a), p + 1,
                goof_action_label((GoofSnesAction)a));
          goof_bindlist_remove(l, code);
        }
      }
}

/* ---- [video] / [audio] (E3) ------------------------------------------ */

enum { VK_DISPLAY, VK_SCALE, VK_ASPECT, VK_SCALING, VK_VSYNC, VK_FILTER, VK_COUNT };
static const char *const kVideoKeys[VK_COUNT] = {
  "display_mode", "window_scale", "pixel_aspect", "fullscreen_scaling",
  "vsync",                                             /* E4 */
  "filter",                                            /* E5 */
};
enum { AK_VOLUME, AK_MUTE, AK_MUTE_UNFOCUSED, AK_COUNT };
static const char *const kAudioKeys[AK_COUNT] = {
  "master_volume", "mute", "mute_when_unfocused",
};

static int key_index(const char *key, const char *const *keys, int n) {
  for (int i = 0; i < n; i++) if (strcmp(key, keys[i]) == 0) return i;
  return -1;
}

/* One [video] line.  An invalid value keeps the default (one warning). */
static void parse_video(GoofVideoSettings *v, int k, const char *val, int line,
                        GoofConfigWarnFn warn, void *user) {
  bool ok = false;
  switch (k) {
    case VK_DISPLAY: ok = goof_display_from_token(val, &v->display); break;
    case VK_SCALE:   ok = goof_window_scale_from_token(val, &v->window_scale); break;
    case VK_ASPECT:  ok = goof_aspect_from_token(val, &v->aspect); break;
    case VK_SCALING: ok = goof_scaling_from_token(val, &v->scaling); break;
    case VK_VSYNC:   ok = goof_bool_from_token(val, &v->vsync); break;
    case VK_FILTER:  ok = goof_filter_from_token(val, &v->filter); break;
    default: break;
  }
  static const char *const kExpect[VK_COUNT] = {
    "windowed|fullscreen", "auto|1|2|3|4", "square|8:7", "integer|fit", "on|off",
    "nearest|bilinear|scanlines",
  };
  if (!ok)
    warnf(warn, user, "line %d: invalid [video] %s '%s' (expected %s); default kept",
          line, kVideoKeys[k], val, kExpect[k]);
}

static void parse_audio(GoofAudioSettings *a, int k, const char *val, int line,
                        GoofConfigWarnFn warn, void *user) {
  bool ok = false;
  switch (k) {
    case AK_VOLUME:         ok = goof_volume_from_token(val, &a->master_volume); break;
    case AK_MUTE:           ok = goof_bool_from_token(val, &a->mute); break;
    case AK_MUTE_UNFOCUSED: ok = goof_bool_from_token(val, &a->mute_when_unfocused); break;
    default: break;
  }
  if (!ok)
    warnf(warn, user, "line %d: invalid [audio] %s '%s' (expected %s); default kept",
          line, kAudioKeys[k], val, k == AK_VOLUME ? "0..100" : "on|off");
}

GoofConfigLoadStatus goof_config_parse(const char *text, size_t len,
                                       GoofConfig *out, GoofConfigWarnFn warn,
                                       void *user) {
  goof_config_defaults(out);
  if (len > GOOF_CONFIG_MAX_BYTES) {
    warnf(warn, user, "file larger than %d bytes; ignored, defaults used",
          GOOF_CONFIG_MAX_BYTES);
    return GOOF_CONFIG_REJECTED;
  }
  if (memchr(text, '\0', len)) {
    warnf(warn, user, "file contains NUL bytes (not a text INI); ignored, "
          "defaults used");
    return GOOF_CONFIG_REJECTED;
  }
  if (len >= 3 && (unsigned char)text[0] == 0xEF &&
      (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
    text += 3; len -= 3;                       /* UTF-8 BOM */
  }
  long schema = find_schema(text, len);
  if (schema == -1) {
    warnf(warn, user, "no 'schema' in [meta]; file ignored, defaults used");
    return GOOF_CONFIG_REJECTED;
  }
  if (schema != GOOF_CONFIG_SCHEMA) {
    if (schema == -2)
      warnf(warn, user, "[meta] schema is not a number; file ignored, "
            "defaults used");
    else
      warnf(warn, user, "unsupported schema %ld (this build reads %d); file "
            "ignored, defaults used", schema, GOOF_CONFIG_SCHEMA);
    return GOOF_CONFIG_REJECTED;
  }

  Lines ls = {text, text + len, 0, {0}, false};
  Section sec = {SEC_NONE, 0, GOOF_BIND_KEY};
  bool seen[GOOF_HOST_PLAYERS][GOOF_BIND_KINDS][GOOF_ACT_COUNT];
  memset(seen, 0, sizeof seen);
  bool schema_seen = false;
  bool video_seen[VK_COUNT] = {false}, audio_seen[AK_COUNT] = {false};
  char sec_name[128] = "";
  const char *l;
  while ((l = next_line(&ls))) {
    if (!*l || *l == ';' || *l == '#') continue;
    if (ls.overlong) {
      warnf(warn, user, "line %d: longer than %zu characters; ignored", ls.line,
            sizeof ls.buf - 1);
      continue;
    }
    char line[512];
    snprintf(line, sizeof line, "%s", l);
    if (*line == '[') {
      char *close = strchr(line, ']');
      if (!close || close[1] != '\0' || (size_t)(close - line - 1) >= sizeof sec_name) {
        warnf(warn, user, "line %d: malformed section header; ignored", ls.line);
        sec.kind = SEC_UNKNOWN;
        snprintf(sec_name, sizeof sec_name, "?");
        continue;
      }
      *close = '\0';
      snprintf(sec_name, sizeof sec_name, "%s", trim(line + 1));
      sec = parse_section(sec_name);
      if (sec.kind == SEC_UNKNOWN)
        warnf(warn, user, "line %d: unknown section [%s]; its keys are ignored",
              ls.line, sec_name);
      continue;
    }
    char *eq = strchr(line, '=');
    if (!eq) {
      warnf(warn, user, "line %d: expected key=value; ignored", ls.line);
      continue;
    }
    *eq = '\0';
    char *key = trim(line);
    char *value = eq + 1;
    if (sec.kind == SEC_NONE) {
      warnf(warn, user, "line %d: '%s' outside any section; ignored", ls.line, key);
      continue;
    }
    if (sec.kind == SEC_UNKNOWN) continue;
    if (sec.kind == SEC_META) {
      if (strcmp(key, "schema") == 0) {
        if (schema_seen)
          warnf(warn, user, "line %d: duplicate 'schema'; first one used", ls.line);
        schema_seen = true;
      } else {
        warnf(warn, user, "line %d: unknown key '%s' in [meta]; ignored",
              ls.line, key);
      }
      continue;
    }
    if (sec.kind == SEC_VIDEO || sec.kind == SEC_AUDIO) {
      bool video = sec.kind == SEC_VIDEO;
      int k = video ? key_index(key, kVideoKeys, VK_COUNT)
                    : key_index(key, kAudioKeys, AK_COUNT);
      if (k < 0) {
        warnf(warn, user, "line %d: unknown key '%s' in [%s]; ignored", ls.line,
              key, sec_name);
        continue;
      }
      bool *seen_k = video ? &video_seen[k] : &audio_seen[k];
      if (*seen_k) {
        warnf(warn, user, "line %d: duplicate '%s' in [%s]; first one used",
              ls.line, key, sec_name);
        continue;
      }
      *seen_k = true;
      char *val = trim(value);
      if (video) parse_video(&out->video, k, val, ls.line, warn, user);
      else parse_audio(&out->audio, k, val, ls.line, warn, user);
      continue;
    }
    GoofSnesAction act;
    if (!goof_action_from_token(key, &act)) {
      warnf(warn, user, "line %d: unknown key '%s' in [%s]; ignored", ls.line,
            key, sec_name);
      continue;
    }
    if (seen[sec.player][sec.bind][act]) {
      warnf(warn, user, "line %d: duplicate '%s' in [%s]; first one used",
            ls.line, key, sec_name);
      continue;
    }
    seen[sec.player][sec.bind][act] = true;
    GoofBindList parsed;
    if (parse_value(value, sec.bind, &parsed, ls.line, sec.player, key, warn, user))
      out->bindings.player[sec.player].list[sec.bind][act] = parsed;
  }
  resolve_conflicts(&out->bindings, warn, user);
  return GOOF_CONFIG_LOADED;
}

/* ---- UTF-8 file helpers (Windows needs the wide API for non-ASCII) ----- */

#ifdef _WIN32
static bool widen(const char *s, wchar_t *out, int cap) {
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, cap) > 0;
}
static FILE *u8_fopen(const char *path, const char *mode) {
  wchar_t wp[1024], wm[8];
  if (!widen(path, wp, 1024) || !widen(mode, wm, 8)) return NULL;
  return _wfopen(wp, wm);
}
static int u8_mkdir(const char *path) {
  wchar_t wp[1024];
  if (!widen(path, wp, 1024)) return -1;
  return _wmkdir(wp);
}
static int u8_remove(const char *path) {
  wchar_t wp[1024];
  if (!widen(path, wp, 1024)) return -1;
  return _wremove(wp);
}
#else
static FILE *u8_fopen(const char *path, const char *mode) { return fopen(path, mode); }
static int u8_mkdir(const char *path) { return mkdir(path, 0755); }
static int u8_remove(const char *path) { return remove(path); }
#endif

GoofConfigLoadStatus goof_config_load_file(const char *path, GoofConfig *out,
                                           GoofConfigWarnFn warn, void *user) {
  goof_config_defaults(out);
  FILE *f = u8_fopen(path, "rb");
  if (!f) {
    if (errno == ENOENT) return GOOF_CONFIG_MISSING;
    warnf(warn, user, "cannot read %s (%s); defaults used", path, strerror(errno));
    return GOOF_CONFIG_REJECTED;
  }
  char *buf = malloc(GOOF_CONFIG_MAX_BYTES + 1);
  if (!buf) { fclose(f); return GOOF_CONFIG_REJECTED; }
  size_t n = fread(buf, 1, GOOF_CONFIG_MAX_BYTES + 1, f);
  bool read_error = ferror(f) != 0;
  fclose(f);
  GoofConfigLoadStatus st;
  if (read_error) {
    warnf(warn, user, "read error on %s; defaults used", path);
    st = GOOF_CONFIG_REJECTED;
  } else {
    st = goof_config_parse(buf, n, out, warn, user);
  }
  free(buf);
  return st;
}

/* ---- serialisation ---------------------------------------------------- */

typedef struct { char *buf; size_t cap, len; bool overflow; } Out;

static void emit(Out *o, const char *fmt, ...) {
  if (o->overflow) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= o->cap - o->len) { o->overflow = true; return; }
  o->len += (size_t)n;
}

size_t goof_config_serialize(const GoofConfig *c, char *buf, size_t cap) {
  if (!buf || cap == 0) return 0;
  Out o = {buf, cap, 0, false};
  emit(&o,
       "; Goof Troop Recomp -- host configuration.\n"
       "; Host-only: nothing here changes how the game runs, only which\n"
       "; physical key or controller input produces each SNES button, and\n"
       "; how the picture and sound are presented ([video], [audio]).\n"
       "; Values are comma-separated tokens; \"none\" = unbound.  Keys are\n"
       "; physical positions (US layout names); gamepad inputs are named by\n"
       "; position (bottom/right/left/top = SNES B/A/Y/X by default).\n"
       "[meta]\n"
       "schema=%d\n", GOOF_CONFIG_SCHEMA);
  for (int p = 0; p < GOOF_HOST_PLAYERS; p++)
    for (int k = 0; k < GOOF_BIND_KINDS; k++) {
      emit(&o, "\n[input.player%d.%s]\n", p + 1, kind_name((GoofBindKind)k));
      for (int a = 0; a < GOOF_ACT_COUNT; a++) {
        const GoofBindList *l = &c->bindings.player[p].list[k][a];
        emit(&o, "%s=", goof_action_token((GoofSnesAction)a));
        if (l->count == 0) emit(&o, "none");
        for (int i = 0; i < l->count; i++) {
          char tok[32];
          if (k == GOOF_BIND_KEY) {
            if (!goof_key_token(l->code[i], tok, sizeof tok)) snprintf(tok, sizeof tok, "none");
          } else {
            const char *t = goof_pad_token(l->code[i]);
            snprintf(tok, sizeof tok, "%s", t ? t : "none");
          }
          emit(&o, "%s%s", i ? "," : "", tok);
        }
        emit(&o, "\n");
      }
    }
  char scale[16];
  goof_window_scale_token(c->video.window_scale, scale, sizeof scale);
  emit(&o, "\n[video]\n"
           "display_mode=%s\n"
           "window_scale=%s\n"
           "pixel_aspect=%s\n"
           "fullscreen_scaling=%s\n"
           "vsync=%s\n"
           "filter=%s\n",
       goof_display_token(c->video.display), scale,
       goof_aspect_token(c->video.aspect), goof_scaling_token(c->video.scaling),
       goof_vsync_token(c->video.vsync), goof_filter_token(c->video.filter));
  emit(&o, "\n[audio]\n"
           "master_volume=%d\n"
           "mute=%s\n"
           "mute_when_unfocused=%s\n",
       c->audio.master_volume, c->audio.mute ? "on" : "off",
       c->audio.mute_when_unfocused ? "on" : "off");
  if (o.overflow) { buf[0] = '\0'; return 0; }
  return o.len;
}

/* ---- atomic save ------------------------------------------------------ */

static bool is_sep(char ch) {
#ifdef _WIN32
  return ch == '/' || ch == '\\';
#else
  return ch == '/';
#endif
}

/* mkdir -p of the directory part of `path`. */
static bool make_parents(const char *path, char *err, size_t err_cap) {
  char dir[1024];
  size_t n = strlen(path);
  if (n >= sizeof dir) { snprintf(err, err_cap, "path too long"); return false; }
  memcpy(dir, path, n + 1);
  char *last = NULL;
  for (char *p = dir; *p; p++) if (is_sep(*p)) last = p;
  if (!last) return true;
  *last = '\0';
  for (char *p = dir + 1; ; p++) {
    bool end = *p == '\0';
    if (end || is_sep(*p)) {
      char saved = *p;
      *p = '\0';
      /* Skip "C:" on Windows. */
      bool drive = strlen(dir) == 2 && dir[1] == ':';
      if (*dir && !drive && u8_mkdir(dir) != 0 && errno != EEXIST) {
        snprintf(err, err_cap, "cannot create directory %s: %s", dir, strerror(errno));
        return false;
      }
      *p = saved;
    }
    if (end) break;
  }
  return true;
}

bool goof_config_save_atomic(const char *path, const GoofConfig *c,
                             char *err, size_t err_cap) {
  char dummy[8];
  if (!err || !err_cap) { err = dummy; err_cap = sizeof dummy; }
  err[0] = '\0';
  if (!path || !*path) { snprintf(err, err_cap, "no config path"); return false; }
  char *text = malloc(GOOF_CONFIG_MAX_BYTES);
  if (!text) { snprintf(err, err_cap, "out of memory"); return false; }
  size_t len = goof_config_serialize(c, text, GOOF_CONFIG_MAX_BYTES);
  if (!len) { free(text); snprintf(err, err_cap, "serialisation overflow"); return false; }
  if (!make_parents(path, err, err_cap)) { free(text); return false; }

  char tmp[1100];
  if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) {
    free(text); snprintf(err, err_cap, "path too long"); return false;
  }
  FILE *f = u8_fopen(tmp, "wb");
  if (!f) {
    snprintf(err, err_cap, "cannot write %s: %s", tmp, strerror(errno));
    free(text);
    return false;
  }
  bool ok = fwrite(text, 1, len, f) == len && fflush(f) == 0;
#ifdef _WIN32
  ok = ok && _commit(_fileno(f)) == 0;
#else
  ok = ok && fsync(fileno(f)) == 0;
#endif
  ok = (fclose(f) == 0) && ok;
  free(text);
  if (!ok) {
    snprintf(err, err_cap, "write failed for %s: %s", tmp, strerror(errno));
    u8_remove(tmp);
    return false;
  }
#ifdef _WIN32
  wchar_t wt[1024], wp[1024];
  ok = widen(tmp, wt, 1024) && widen(path, wp, 1024) &&
       MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  if (!ok) snprintf(err, err_cap, "cannot replace %s (error %lu)", path,
                    (unsigned long)GetLastError());
#else
  ok = rename(tmp, path) == 0;
  if (!ok) {
    snprintf(err, err_cap, "cannot replace %s: %s", path, strerror(errno));
  } else {
    /* Make the rename itself durable (best effort). */
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
      if (slash == dir) slash[1] = '\0'; else *slash = '\0';
      int fd = open(dir, O_RDONLY);
      if (fd >= 0) { (void)fsync(fd); close(fd); }
    }
  }
#endif
  if (!ok) u8_remove(tmp);
  return ok;
}

/* ---- locations -------------------------------------------------------- */

GoofConfigPlatform goof_config_host_platform(void) {
#ifdef _WIN32
  return GOOF_CONFIG_PLATFORM_WINDOWS;
#else
  return GOOF_CONFIG_PLATFORM_POSIX;
#endif
}

void goof_config_env_from_process(GoofConfigEnv *env) {
  memset(env, 0, sizeof *env);
#ifdef _WIN32
  static char appdata[1024];
  const wchar_t *w = _wgetenv(L"APPDATA");
  if (w && WideCharToMultiByte(CP_UTF8, 0, w, -1, appdata, (int)sizeof appdata,
                               NULL, NULL) > 0)
    env->appdata = appdata;
#else
  env->xdg_config_home = getenv("XDG_CONFIG_HOME");
  env->home = getenv("HOME");
#endif
}

static bool fmt_ok(int n, size_t cap) { return n > 0 && (size_t)n < cap; }

bool goof_config_default_path(GoofConfigPlatform platform, const GoofConfigEnv *env,
                              char *out, size_t cap) {
  if (!env || !out || !cap) return false;
  if (platform == GOOF_CONFIG_PLATFORM_WINDOWS) {
    if (!env->appdata || !*env->appdata) return false;
    return fmt_ok(snprintf(out, cap, "%s\\GoofTroopRecomp\\config.ini",
                           env->appdata), cap);
  }
  /* XDG Base Directory: a relative XDG_CONFIG_HOME is invalid and ignored. */
  if (env->xdg_config_home && env->xdg_config_home[0] == '/')
    return fmt_ok(snprintf(out, cap, "%s/GoofTroopRecomp/config.ini",
                           env->xdg_config_home), cap);
  if (!env->home || !*env->home) return false;
  return fmt_ok(snprintf(out, cap, "%s/.config/GoofTroopRecomp/config.ini",
                         env->home), cap);
}

bool goof_config_portable_path(const char *exe_dir, char *out, size_t cap) {
  if (!exe_dir || !*exe_dir || !out || !cap) return false;
  size_t n = strlen(exe_dir);
  bool sep = is_sep(exe_dir[n - 1]);
#ifdef _WIN32
  const char *s = "\\";
#else
  const char *s = "/";
#endif
  return fmt_ok(snprintf(out, cap, "%s%sconfig.ini", exe_dir, sep ? "" : s), cap);
}
