/* GOOF_ENHANCEMENTS_E2 -- config.ini tests, SDL-free (POSIX file system).
 *
 *   C1   no file -> built-in defaults (== E1 bindings)
 *   C2   explicit valid file loads (partial file: missing actions keep
 *        their defaults; explicit "none" clears)
 *   C3   missing file -> MISSING + defaults, no warning
 *   C4   malformed binding -> warning; invalid tokens dropped, valid ones
 *        kept, all-invalid line keeps the default; reserved keys refused;
 *        file conflicts resolved keep-first with a warning
 *   C5   unsupported / missing / non-numeric schema -> REJECTED + defaults,
 *        file untouched
 *   C6   unknown sections / keys / junk lines / oversize / NUL -> no crash
 *   C7   Player 1 and Player 2 sections are independent
 *   C8   save -> reload round-trip through the file system
 *   C9   deterministic serialisation (same config -> same bytes; parse of
 *        the output re-serialises identically; CRLF input accepted)
 *   C10  atomic write: temp + rename (new inode), no temp left, parent dirs
 *        created, stale temp ignored, failed write leaves the old file
 *        (inode and read-only-directory sub-checks are POSIX-only; on
 *        Windows the rename is MoveFileExW REPLACE_EXISTING|WRITE_THROUGH)
 *   C11  reset defaults serialises to the default file
 *   C12  "no config": defaults without touching the file system
 *   C13  default path resolution (XDG, HOME fallback, empty/relative XDG,
 *        Windows %APPDATA%) and explicit path
 *   C14  portable path (config.ini beside the executable)
 *   C15  output holds only schema + symbolic tokens (no SDL instance ids,
 *        GUIDs or raw numbers for named inputs)
 *
 * usage: goof_config_test SCRATCH_DIR   (an empty directory it may write) */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#define strtok_r strtok_s
static int mkdir_compat(const char *p) { return _mkdir(p); }
#else
static int mkdir_compat(const char *p) { return mkdir(p, 0755); }
#endif

#include "host/goof_config.h"

static int failures, case_failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; case_failures++; \
  printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void report(const char *name, const char *what) {
  printf("%s %s %s\n", name, case_failures ? "FAIL" : "PASS", what);
  case_failures = 0;
}

static char warnings[8192];
static int nwarn;
static void collect(void *user, const char *msg) {
  (void)user;
  nwarn++;
  size_t n = strlen(warnings);
  snprintf(warnings + n, sizeof warnings - n, "%s\n", msg);
}
static void clear_warn(void) { warnings[0] = '\0'; nwarn = 0; }

static GoofConfigLoadStatus parse(const char *text, GoofConfig *c) {
  clear_warn();
  return goof_config_parse(text, strlen(text), c, collect, NULL);
}

static bool write_file(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  fputs(text, f);
  return fclose(f) == 0;
}

static size_t read_file(const char *path, char *buf, size_t cap) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  size_t n = fread(buf, 1, cap - 1, f);
  buf[n] = '\0';
  fclose(f);
  return n;
}

static const GoofBindList *L(const GoofConfig *c, int p, int k, int a) {
  return &c->bindings.player[p].list[k][a];
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: goof_config_test SCRATCH_DIR\n"); return 2; }
  const char *dir = argv[1];
  char path[1024], path2[1024];
  GoofConfig c, def;
  goof_config_defaults(&def);
  GoofBindings e1;
  goof_bindings_defaults(&e1);
  static char buf[GOOF_CONFIG_MAX_BYTES], buf2[GOOF_CONFIG_MAX_BYTES];

  /* C1 */
  snprintf(path, sizeof path, "%s/c1-absent/config.ini", dir);
  clear_warn();
  GoofConfigLoadStatus st = goof_config_load_file(path, &c, collect, NULL);
  CHECK(st == GOOF_CONFIG_MISSING, "status %d", st);
  CHECK(goof_bindings_equal(&c.bindings, &e1), "defaults == E1 bindings");
  report("C1", "no file -> built-in defaults (E1 bindings)");

  /* C2 */
  snprintf(path, sizeof path, "%s/c2.ini", dir);
  write_file(path,
             "; user file\n[meta]\nschema=1\n\n[input.player1.keyboard]\n"
             "b = Q, W\nstart=space\n\n[input.player1.gamepad]\nselect=none\n"
             "a=top\nx=right\n");                  /* swap: no conflict */
  clear_warn();
  st = goof_config_load_file(path, &c, collect, NULL);
  CHECK(st == GOOF_CONFIG_LOADED && nwarn == 0, "status %d warnings: %s", st, warnings);
  CHECK(L(&c, 0, GOOF_BIND_KEY, GOOF_ACT_B)->count == 2 &&
        L(&c, 0, GOOF_BIND_KEY, GOOF_ACT_B)->code[0] == 20 &&
        L(&c, 0, GOOF_BIND_KEY, GOOF_ACT_B)->code[1] == 26, "b = Q, W");
  CHECK(L(&c, 0, GOOF_BIND_KEY, GOOF_ACT_START)->count == 1 &&
        L(&c, 0, GOOF_BIND_KEY, GOOF_ACT_START)->code[0] == 44, "start = SPACE");
  CHECK(L(&c, 0, GOOF_BIND_PAD, GOOF_ACT_SELECT)->count == 0, "select cleared");
  CHECK(L(&c, 0, GOOF_BIND_PAD, GOOF_ACT_A)->code[0] == GOOF_HOST_BTN_NORTH, "a = top");
  CHECK(L(&c, 0, GOOF_BIND_KEY, GOOF_ACT_UP)->code[0] == GOOF_HOST_KEY_UP &&
        memcmp(&c.bindings.player[1], &e1.player[1], sizeof e1.player[1]) == 0,
        "unlisted actions and P2 keep defaults");
  report("C2", "explicit valid file loads");

  /* C3 */
  snprintf(path, sizeof path, "%s/definitely-missing.ini", dir);
  clear_warn();
  st = goof_config_load_file(path, &c, collect, NULL);
  CHECK(st == GOOF_CONFIG_MISSING && nwarn == 0 && goof_bindings_equal(&c.bindings, &e1),
        "missing -> defaults silently");
  report("C3", "missing file -> defaults");

  /* C4 */
  st = parse("[meta]\nschema=1\n[input.player1.keyboard]\n"
             "b=Q,NOSUCHKEY\n"          /* one bad token: Q kept          */
             "a=BOGUS\n"               /* all bad: default X kept        */
             "y=F2\n"                  /* reserved: default A kept       */
             "x=P,S\n"                 /* reserved dropped, S kept       */
             "l=A,B,C,D,E\n"           /* > 4: E dropped                 */
             "[input.player1.gamepad]\n"
             "b=bottom\n"
             "a=bottom\n"              /* conflict with b: dropped from a */
             "r=wiggle\n", &c);
  CHECK(st == GOOF_CONFIG_LOADED, "loaded");
  CHECK(L(&c, 0, 0, GOOF_ACT_B)->count == 1 && L(&c, 0, 0, GOOF_ACT_B)->code[0] == 20,
        "b=Q kept");
  CHECK(L(&c, 0, 0, GOOF_ACT_A)->code[0] == GOOF_HOST_KEY_X, "a default kept");
  CHECK(L(&c, 0, 0, GOOF_ACT_Y)->code[0] == GOOF_HOST_KEY_A, "y default kept (F2 reserved)");
  CHECK(L(&c, 0, 0, GOOF_ACT_X)->count == 1 && L(&c, 0, 0, GOOF_ACT_X)->code[0] == 22,
        "x = S only");
  /* l=A,B,C,D: A is P1 Y's default key and B is P2 Y's -> keep-first drops them. */
  CHECK(strstr(warnings, "unknown key 'NOSUCHKEY'") && strstr(warnings, "'a'; default kept") &&
        strstr(warnings, "reserved") && strstr(warnings, "more than 4") &&
        strstr(warnings, "unknown gamepad input 'wiggle'") &&
        strstr(warnings, "conflict: gamepad 'bottom'"), "warnings:\n%s", warnings);
  CHECK(L(&c, 0, 1, GOOF_ACT_B)->code[0] == GOOF_HOST_BTN_SOUTH &&
        !goof_bindlist_contains(L(&c, 0, 1, GOOF_ACT_A), GOOF_HOST_BTN_SOUTH),
        "keep-first: bottom stays on B");
  CHECK(goof_bindings_conflicts(&c.bindings, NULL, 0) == 0, "loaded config conflict-free");
  printf("  C4 warnings=%d\n", nwarn);
  report("C4", "malformed bindings -> warning + documented fallback");

  /* C5 */
  snprintf(path, sizeof path, "%s/c5.ini", dir);
  const char *future = "[meta]\nschema=2\n[input.player1.keyboard]\nb=Q\n";
  write_file(path, future);
  clear_warn();
  st = goof_config_load_file(path, &c, collect, NULL);
  CHECK(st == GOOF_CONFIG_REJECTED && goof_bindings_equal(&c.bindings, &e1) &&
        strstr(warnings, "unsupported schema 2"), "schema 2 rejected: %s", warnings);
  read_file(path, buf, sizeof buf);
  CHECK(strcmp(buf, future) == 0, "file untouched");
  CHECK(parse("[input.player1.keyboard]\nb=Q\n", &c) == GOOF_CONFIG_REJECTED &&
        strstr(warnings, "no 'schema'") && goof_bindings_equal(&c.bindings, &e1),
        "missing schema rejected");
  CHECK(parse("[meta]\nschema=one\n", &c) == GOOF_CONFIG_REJECTED, "non-numeric schema");
  CHECK(parse("schema=1\n", &c) == GOOF_CONFIG_REJECTED, "schema outside [meta]");
  report("C5", "unsupported schema handled clearly (defaults, file untouched)");

  /* C6 */
  /* E3: [video] is a real section now; a still-unknown one keeps the check. */
  st = parse("garbage line\norphan=1\n[meta]\nschema=1\nfuture_key=7\n[widescreen]\nscale=9\n"
             "[input.player1.keyboard]\nturbo_b=Q\nb\n[broken\n"
             "[input.player3.keyboard]\nb=Q\n", &c);
  CHECK(st == GOOF_CONFIG_LOADED && goof_bindings_equal(&c.bindings, &e1),
        "unknown things ignored, defaults intact (%d warnings)", nwarn);
  CHECK(strstr(warnings, "unknown section [widescreen]") && strstr(warnings, "unknown key 'turbo_b'") &&
        strstr(warnings, "unknown key 'future_key' in [meta]") &&
        strstr(warnings, "malformed section header") && strstr(warnings, "expected key=value") &&
        strstr(warnings, "outside any section"), "warnings:\n%s", warnings);
  char *big = malloc(GOOF_CONFIG_MAX_BYTES + 10);
  memset(big, ';', GOOF_CONFIG_MAX_BYTES + 10);
  clear_warn();
  CHECK(goof_config_parse(big, GOOF_CONFIG_MAX_BYTES + 10, &c, collect, NULL) ==
        GOOF_CONFIG_REJECTED, "oversize rejected");
  free(big);
  static const char nul[] = "[meta]\0schema=1\n";
  clear_warn();
  CHECK(goof_config_parse(nul, sizeof nul - 1, &c, collect, NULL) == GOOF_CONFIG_REJECTED,
        "NUL rejected");
  char longline[2000];
  memset(longline, 'x', sizeof longline);
  static const char prefix[] = "[meta]\nschema=1\n[input.player1.keyboard]\nb=";
  memcpy(longline, prefix, sizeof prefix - 1);
  longline[sizeof longline - 1] = '\0';
  CHECK(parse(longline, &c) == GOOF_CONFIG_LOADED && goof_bindings_equal(&c.bindings, &e1),
        "overlong line ignored");
  CHECK(parse("\xEF\xBB\xBF[meta]\r\nschema=1\r\n[input.player1.keyboard]\r\nb=Q\r\n", &c) ==
        GOOF_CONFIG_LOADED && L(&c, 0, 0, GOOF_ACT_B)->code[0] == 20, "BOM + CRLF");
  report("C6", "unknown keys / sections / junk do not crash");

  /* C7 */
  st = parse("[meta]\nschema=1\n[input.player2.keyboard]\nb=Q\n[input.player2.gamepad]\n"
             "b=top\nx=bottom\n", &c);
  CHECK(st == GOOF_CONFIG_LOADED && nwarn == 0, "loaded: %s", warnings);
  CHECK(memcmp(&c.bindings.player[0], &e1.player[0], sizeof e1.player[0]) == 0,
        "P1 untouched by P2 sections");
  CHECK(L(&c, 1, 0, GOOF_ACT_B)->code[0] == 20 && L(&c, 1, 1, GOOF_ACT_B)->code[0] == GOOF_HOST_BTN_NORTH &&
        L(&c, 1, 1, GOOF_ACT_X)->code[0] == GOOF_HOST_BTN_SOUTH, "P2 values");
  report("C7", "Player 1 and Player 2 independent");

  /* C8 */
  GoofConfig custom;
  goof_config_defaults(&custom);
  goof_bindlist_set_single(&custom.bindings.player[0].list[0][GOOF_ACT_B], 20);
  goof_bindlist_add(&custom.bindings.player[0].list[0][GOOF_ACT_B], 300);  /* HID_300 */
  goof_bindlist_clear(&custom.bindings.player[1].list[1][GOOF_ACT_SELECT]);
  goof_bindlist_set_single(&custom.bindings.player[1].list[1][GOOF_ACT_L], GOOF_PAD_LSTICK_LEFT);
  goof_bindlist_remove(&custom.bindings.player[1].list[1][GOOF_ACT_LEFT], GOOF_PAD_LSTICK_LEFT);
  snprintf(path, sizeof path, "%s/c8/config.ini", dir);
  char err[256];
  CHECK(goof_config_save_atomic(path, &custom, err, sizeof err), "save: %s", err);
  clear_warn();
  st = goof_config_load_file(path, &c, collect, NULL);
  CHECK(st == GOOF_CONFIG_LOADED && nwarn == 0 && goof_bindings_equal(&c.bindings, &custom.bindings),
        "reload equal (%s)", warnings);
  report("C8", "save -> reload round-trip");

  /* C9 */
  size_t n1 = goof_config_serialize(&custom, buf, sizeof buf);
  size_t n2 = goof_config_serialize(&custom, buf2, sizeof buf2);
  CHECK(n1 && n1 == n2 && memcmp(buf, buf2, n1) == 0, "same bytes twice");
  GoofConfig re;
  goof_config_parse(buf, n1, &re, NULL, NULL);
  n2 = goof_config_serialize(&re, buf2, sizeof buf2);
  CHECK(n1 == n2 && memcmp(buf, buf2, n1) == 0, "parse(serialize) re-serialises identically");
  snprintf(path2, sizeof path2, "%s/c9/config.ini", dir);
  goof_config_save_atomic(path2, &custom, err, sizeof err);
  size_t f1 = read_file(path, buf2, sizeof buf2);
  CHECK(f1 == n1 && memcmp(buf, buf2, n1) == 0, "file bytes == serialisation");
  CHECK(strchr(buf, '\r') == NULL, "LF line ends");
  CHECK(goof_config_serialize(&custom, buf2, 100) == 0, "too-small buffer -> 0");
  report("C9", "deterministic serialisation");

  /* C10 */
  struct stat s1, s2;
  stat(path, &s1);
  goof_bindlist_set_single(&custom.bindings.player[0].list[0][GOOF_ACT_A], 21);
  CHECK(goof_config_save_atomic(path, &custom, err, sizeof err), "re-save: %s", err);
  stat(path, &s2);
#ifndef _WIN32
  CHECK(s1.st_ino != s2.st_ino, "replaced by rename (new inode %lu -> %lu)",
        (unsigned long)s1.st_ino, (unsigned long)s2.st_ino);
#else
  (void)s1; (void)s2;
#endif
  char tmp[1100];
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  CHECK(access(tmp, F_OK) != 0, "no temp left behind");
  /* A crash between write and rename leaves only a stale .tmp: the loader
   * never reads it, and the next save simply replaces it. */
  write_file(tmp, "[meta]\nschema=1\n[input.player1.keyboard]\nb=M\n");
  goof_config_load_file(path, &c, NULL, NULL);
  CHECK(L(&c, 0, 0, GOOF_ACT_A)->code[0] == 21, "stale temp ignored by the loader");
  CHECK(goof_config_save_atomic(path, &custom, err, sizeof err) && access(tmp, F_OK) != 0,
        "stale temp replaced");
  snprintf(path2, sizeof path2, "%s/c10/a/b/c/config.ini", dir);
  CHECK(goof_config_save_atomic(path2, &custom, err, sizeof err), "nested dirs: %s", err);
  /* Failed write: directory not writable -> old file byte-identical. */
#ifndef _WIN32
  snprintf(path2, sizeof path2, "%s/c10ro", dir);
  mkdir_compat(path2);
  snprintf(tmp, sizeof tmp, "%s/config.ini", path2);
  goof_config_save_atomic(tmp, &def, err, sizeof err);
  read_file(tmp, buf, sizeof buf);
  chmod(path2, 0555);
  bool root = geteuid() == 0;
  bool wrote = goof_config_save_atomic(tmp, &custom, err, sizeof err);
  read_file(tmp, buf2, sizeof buf2);
  chmod(path2, 0755);
  CHECK(root || (!wrote && strcmp(buf, buf2) == 0 && err[0]),
        "unwritable dir: save fails (%s), old file intact", err);
  printf("  C10 failure message: %s\n", err);
#else
  mkdir_compat(dir);
  printf("  C10 read-only-directory and inode sub-checks: POSIX only (skipped)\n");
#endif
  report("C10", "atomic write behaviour");

  /* C11 */
  GoofConfig reset = custom;
  goof_bindings_defaults(&reset.bindings);
  n1 = goof_config_serialize(&reset, buf, sizeof buf);
  n2 = goof_config_serialize(&def, buf2, sizeof buf2);
  CHECK(n1 == n2 && memcmp(buf, buf2, n1) == 0, "reset == default file");
  CHECK(goof_bindings_equal(&reset.bindings, &e1), "reset == E1");
  /* Reference copy of the default file, for the end-to-end reset check and
   * the evidence (config_examples.txt). */
  snprintf(path, sizeof path, "%s/default_config.ini", dir);
  CHECK(goof_config_save_atomic(path, &def, err, sizeof err), "write reference: %s", err);
  report("C11", "reset defaults");

  /* C12 */
  goof_config_defaults(&c);
  CHECK(goof_bindings_equal(&c.bindings, &e1), "--no-config == goof_config_defaults, no I/O");
  report("C12", "no-config defaults (frontend flag checked end-to-end separately)");

  /* C13 */
  GoofConfigEnv env = {"/xdg/cfg", "/home/u", NULL};
  CHECK(goof_config_default_path(GOOF_CONFIG_PLATFORM_POSIX, &env, path, sizeof path) &&
        strcmp(path, "/xdg/cfg/GoofTroopRecomp/config.ini") == 0, "XDG: %s", path);
  env.xdg_config_home = NULL;
  CHECK(goof_config_default_path(GOOF_CONFIG_PLATFORM_POSIX, &env, path, sizeof path) &&
        strcmp(path, "/home/u/.config/GoofTroopRecomp/config.ini") == 0, "HOME: %s", path);
  env.xdg_config_home = "";
  CHECK(goof_config_default_path(GOOF_CONFIG_PLATFORM_POSIX, &env, path, sizeof path) &&
        strstr(path, "/home/u/.config/"), "empty XDG -> HOME");
  env.xdg_config_home = "relative/dir";
  CHECK(goof_config_default_path(GOOF_CONFIG_PLATFORM_POSIX, &env, path, sizeof path) &&
        strstr(path, "/home/u/.config/"), "relative XDG ignored (XDG spec)");
  env.home = NULL; env.xdg_config_home = NULL;
  CHECK(!goof_config_default_path(GOOF_CONFIG_PLATFORM_POSIX, &env, path, sizeof path),
        "no HOME -> no path");
  GoofConfigEnv wenv = {NULL, NULL, "C:\\Users\\u\\AppData\\Roaming"};
  CHECK(goof_config_default_path(GOOF_CONFIG_PLATFORM_WINDOWS, &wenv, path, sizeof path) &&
        strcmp(path, "C:\\Users\\u\\AppData\\Roaming\\GoofTroopRecomp\\config.ini") == 0,
        "APPDATA: %s", path);
  wenv.appdata = "";
  CHECK(!goof_config_default_path(GOOF_CONFIG_PLATFORM_WINDOWS, &wenv, path, sizeof path),
        "no APPDATA -> no path");
#ifdef _WIN32
  CHECK(goof_config_host_platform() == GOOF_CONFIG_PLATFORM_WINDOWS, "this build is Windows");
#else
  CHECK(goof_config_host_platform() == GOOF_CONFIG_PLATFORM_POSIX, "this build is POSIX");
#endif
  snprintf(path, sizeof path, "%s/explicit-dir/my settings.ini", dir);
  CHECK(goof_config_save_atomic(path, &custom, err, sizeof err) &&
        goof_config_load_file(path, &c, NULL, NULL) == GOOF_CONFIG_LOADED &&
        goof_bindings_equal(&c.bindings, &custom.bindings), "explicit path with a space");
  report("C13", "default path resolution + explicit --config path");

  /* C14 */
  CHECK(goof_config_portable_path("/opt/goof/", path, sizeof path) &&
        strcmp(path, "/opt/goof/config.ini") == 0, "portable: %s", path);
#ifdef _WIN32
  CHECK(goof_config_portable_path("/opt/goof", path, sizeof path) &&
        strcmp(path, "/opt/goof\\config.ini") == 0, "portable without slash: %s", path);
  CHECK(goof_config_portable_path("C:\\Games\\Goof\\", path, sizeof path) &&
        strcmp(path, "C:\\Games\\Goof\\config.ini") == 0, "portable Windows dir: %s", path);
#else
  CHECK(goof_config_portable_path("/opt/goof", path, sizeof path) &&
        strcmp(path, "/opt/goof/config.ini") == 0, "portable without slash: %s", path);
#endif
  CHECK(!goof_config_portable_path("", path, sizeof path), "empty exe dir");
  report("C14", "portable config path");

  /* C15 */
  n1 = goof_config_serialize(&custom, buf, sizeof buf);
  bool ok = true;
  char *save_ptr = NULL;
  int value_lines = 0;
  for (char *line = strtok_r(buf, "\n", &save_ptr); line; line = strtok_r(NULL, "\n", &save_ptr)) {
    /* E3: [video] / [audio] follow the input sections; they hold no device
     * identity either (checked below and in goof_config_e3_test K9). */
    if (strcmp(line, "[video]") == 0) break;
    if (line[0] == ';' || line[0] == '[' || strcmp(line, "schema=1") == 0) continue;
    char *eq = strchr(line, '=');
    GoofSnesAction a;
    if (!eq) { ok = false; break; }
    *eq = '\0';
    if (!goof_action_from_token(line, &a)) { ok = false; break; }
    value_lines++;
    for (char *t = strtok(eq + 1, ","); t; t = strtok(NULL, ",")) {
      uint16_t code;
      bool known = strcmp(t, "none") == 0 || goof_key_from_token(t, &code) ||
                   goof_pad_from_token(t, &code);
      bool raw = strncmp(t, "HID_", 4) == 0 && strcmp(t, "HID_300") != 0;
      if (!known || raw) { ok = false; printf("  bad token %s\n", t); }
    }
  }
  CHECK(ok && value_lines == 48, "every line is schema/section/action=tokens (%d)", value_lines);
  CHECK(!strstr(buf, "instance") && !strstr(buf, "guid") && !strstr(buf, "which"),
        "no device identity");
  report("C15", "no SDL instance ids / device identity serialised");

  printf("GOOF_CONFIG_TEST %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
