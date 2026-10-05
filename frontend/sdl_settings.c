/* Goof Troop Recomp -- SDL2 adapter for the host Settings overlay.
 * See sdl_settings.h.  GOOF_ENHANCEMENTS_E2_BUTTON_REMAPPING_CONFIG. */
#include "sdl_settings.h"

#include <string.h>

#include "host/goof_bindings.h"
#include "host/ui/goof_bitmap_font.h"

/* The menu speaks USB HID usages and E1 pad positions; SDL2 uses the same
 * numbering for both (sdl_pads.c asserts the buttons). */
_Static_assert((int)SDL_SCANCODE_F2 == GOOF_UI_SC_F2, "HID usage");
_Static_assert((int)SDL_SCANCODE_ESCAPE == GOOF_UI_SC_ESCAPE, "HID usage");
_Static_assert((int)SDL_SCANCODE_RETURN == GOOF_UI_SC_RETURN, "HID usage");
_Static_assert((int)SDL_SCANCODE_KP_ENTER == GOOF_UI_SC_KP_ENTER, "HID usage");
_Static_assert((int)SDL_SCANCODE_P == GOOF_UI_SC_P, "HID usage");
_Static_assert((int)SDL_SCANCODE_UP == GOOF_UI_SC_UP, "HID usage");
_Static_assert((int)SDL_SCANCODE_DOWN == GOOF_UI_SC_DOWN, "HID usage");
_Static_assert((int)SDL_SCANCODE_LEFT == GOOF_UI_SC_LEFT, "HID usage");
_Static_assert((int)SDL_SCANCODE_RIGHT == GOOF_UI_SC_RIGHT, "HID usage");
_Static_assert((int)SDL_NUM_SCANCODES == GOOF_KEY_CODE_LIMIT, "HID range");
_Static_assert((int)SDL_CONTROLLER_BUTTON_A == GOOF_HOST_BTN_SOUTH, "position");
_Static_assert((int)SDL_CONTROLLER_BUTTON_B == GOOF_HOST_BTN_EAST, "position");
_Static_assert((int)SDL_CONTROLLER_BUTTON_DPAD_RIGHT == GOOF_HOST_BTN_DPAD_RIGHT,
               "position");

void goof_sdl_settings_init(GoofSdlSettings *s, const GoofBindings *runtime,
                            GoofSettingsHost host) {
  memset(s, 0, sizeof *s);
  goof_menu_init(&s->menu, runtime, host);
}

/* ---- events ----------------------------------------------------------- */

enum { DIR_UP = 1, DIR_DOWN = 2, DIR_LEFT = 4, DIR_RIGHT = 8 };

static uint8_t *stick_slot(GoofSdlSettings *s, SDL_JoystickID id) {
  for (int i = 0; i < GOOF_SDL_SETTINGS_MAX_PADS; i++)
    if (s->stick[i].dirs && s->stick[i].id == id) return &s->stick[i].dirs;
  for (int i = 0; i < GOOF_SDL_SETTINGS_MAX_PADS; i++)
    if (!s->stick[i].dirs) { s->stick[i].id = id; return &s->stick[i].dirs; }
  return NULL;
}

int goof_sdl_settings_translate(GoofSdlSettings *s, const SDL_Event *ev,
                                GoofUiEvent out[2]) {
  switch (ev->type) {
    case SDL_KEYDOWN: case SDL_KEYUP: {
      int sc = (int)ev->key.keysym.scancode;
      if (sc <= 0 || sc >= GOOF_KEY_CODE_LIMIT) return 0;
      out[0] = (GoofUiEvent){ev->type == SDL_KEYDOWN ? GOOF_UI_KEY_DOWN : GOOF_UI_KEY_UP,
                             (uint16_t)sc, ev->key.repeat != 0, -1};
      return 1;
    }
    case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: {
      int b = ev->cbutton.button;
      if (b < 0 || b >= GOOF_HOST_BTN_COUNT) return 0;
      out[0] = (GoofUiEvent){ev->type == SDL_CONTROLLERBUTTONDOWN ? GOOF_UI_PAD_DOWN
                                                                  : GOOF_UI_PAD_UP,
                             (uint16_t)b, false, ev->cbutton.which};
      return 1;
    }
    case SDL_CONTROLLERAXISMOTION: {
      int axis = ev->caxis.axis;
      if (axis != SDL_CONTROLLER_AXIS_LEFTX && axis != SDL_CONTROLLER_AXIS_LEFTY) return 0;
      /* Same digital stick as the game path (E1 threshold, strict). */
      int v = ev->caxis.value;
      uint8_t neg = axis == SDL_CONTROLLER_AXIS_LEFTX ? DIR_LEFT : DIR_UP;
      uint8_t pos = axis == SDL_CONTROLLER_AXIS_LEFTX ? DIR_RIGHT : DIR_DOWN;
      uint8_t want = v < -GOOF_HOST_STICK_THRESHOLD ? neg
                   : (v > GOOF_HOST_STICK_THRESHOLD ? pos : 0);
      uint8_t *dirs = stick_slot(s, ev->caxis.which);
      if (!dirs) return 0;
      int n = 0;
      static const struct { uint8_t bit; uint16_t code; } kMap[4] = {
        {DIR_UP, GOOF_PAD_LSTICK_UP}, {DIR_DOWN, GOOF_PAD_LSTICK_DOWN},
        {DIR_LEFT, GOOF_PAD_LSTICK_LEFT}, {DIR_RIGHT, GOOF_PAD_LSTICK_RIGHT},
      };
      for (int i = 0; i < 4; i++) {
        uint8_t bit = kMap[i].bit;
        if (!(bit & (neg | pos))) continue;
        bool was = (*dirs & bit) != 0, now = (want & bit) != 0;
        if (was == now) continue;
        if (now) *dirs |= bit; else *dirs &= (uint8_t)~bit;
        out[n++] = (GoofUiEvent){now ? GOOF_UI_PAD_DOWN : GOOF_UI_PAD_UP,
                                 kMap[i].code, false, ev->caxis.which};
      }
      return n;
    }
    default:
      return 0;
  }
}

unsigned goof_sdl_settings_event(GoofSdlSettings *s, const SDL_Event *ev) {
  GoofUiEvent ue[2];
  int n = goof_sdl_settings_translate(s, ev, ue);
  unsigned flags = 0;
  for (int i = 0; i < n; i++) {
    /* A closed menu only ever wants F2; P stays the pause key and ESC the
     * quit key exactly as before E2.  SDL keycode P (layout letter) is also
     * refused as a capture so the pause key can never become a binding. */
    if (s->menu.capture == GOOF_CAPTURE_ARMED && ue[i].type == GOOF_UI_KEY_DOWN &&
        (ev->key.keysym.sym == SDLK_p || ev->key.keysym.sym == SDLK_ESCAPE ||
         ev->key.keysym.sym == SDLK_F2) && ue[i].code != GOOF_UI_SC_ESCAPE &&
        ue[i].code != GOOF_UI_SC_F2) {
      goof_menu_set_status(&s->menu, GOOF_UI_C_WARN, "THAT KEY IS RESERVED");
      flags |= GOOF_MENU_CONSUMED;
      continue;
    }
    flags |= goof_menu_handle(&s->menu, &ue[i]);
  }
  /* While open, every keyboard and controller-input event is the host's,
   * even ones the menu does not use (they must not reach the game). */
  if (s->menu.open || (flags & GOOF_MENU_CLOSED)) {
    switch (ev->type) {
      case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT:
      case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
      case SDL_CONTROLLERAXISMOTION:
        flags |= GOOF_MENU_CONSUMED;
        break;
      default: break;
    }
  }
  return flags;
}

/* ---- rendering -------------------------------------------------------- */

static const SDL_Color kColors[GOOF_UI_C_COUNT] = {
  [GOOF_UI_C_PANEL] = {6, 8, 20, 224},
  [GOOF_UI_C_TEXT] = {226, 229, 236, 255},
  [GOOF_UI_C_TITLE] = {255, 214, 64, 255},
  [GOOF_UI_C_DIM] = {150, 156, 184, 255},
  [GOOF_UI_C_SEL_BAR] = {36, 92, 204, 255},
  [GOOF_UI_C_SEL_TEXT] = {255, 255, 255, 255},
  [GOOF_UI_C_WARN] = {255, 104, 104, 255},
  [GOOF_UI_C_OK] = {112, 232, 124, 255},
  [GOOF_UI_C_BOX] = {24, 30, 60, 255},
  [GOOF_UI_C_RULE] = {84, 92, 132, 255},
};

enum { ATLAS_GLYPHS = GOOF_FONT_GLYPHS, ATLAS_CELL = 6, ATLAS_H = 8 };

static bool ensure_font(GoofSdlSettings *s, SDL_Renderer *r) {
  if (s->font && s->font_owner == r) return true;
  if (s->font) SDL_DestroyTexture(s->font);
  s->font = NULL;
  static uint32_t px[ATLAS_H][ATLAS_GLYPHS * ATLAS_CELL];
  for (int g = 0; g < ATLAS_GLYPHS; g++)
    for (int y = 0; y < ATLAS_H; y++)
      for (int x = 0; x < ATLAS_CELL; x++) {
        uint8_t row = y < GOOF_FONT_H ? goof_font_row((char)(GOOF_FONT_FIRST + g), y) : 0;
        bool on = x < GOOF_FONT_W && ((row >> (GOOF_FONT_W - 1 - x)) & 1u);
        px[y][g * ATLAS_CELL + x] = on ? 0xFFFFFFFFu : 0x00FFFFFFu;
      }
  s->font = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
                              ATLAS_GLYPHS * ATLAS_CELL, ATLAS_H);
  if (!s->font) return false;
  SDL_UpdateTexture(s->font, NULL, px, (int)sizeof px[0]);
  SDL_SetTextureBlendMode(s->font, SDL_BLENDMODE_BLEND);
  s->font_owner = r;
  return true;
}

void goof_sdl_settings_render(GoofSdlSettings *s, SDL_Renderer *r) {
  if (!s->menu.open || !r) return;
  static GoofUiDrawList d;
  goof_menu_draw(&s->menu, &d);

  int w = 0, h = 0;
  if (SDL_GetRendererOutputSize(r, &w, &h) != 0) return;
  int k = w / GOOF_UI_CANVAS_W < h / GOOF_UI_CANVAS_H ? w / GOOF_UI_CANVAS_W
                                                      : h / GOOF_UI_CANVAS_H;
  if (k < 1) k = 1;
  int ox = (w - GOOF_UI_CANVAS_W * k) / 2, oy = (h - GOOF_UI_CANVAS_H * k) / 2;

  SDL_BlendMode saved_mode;
  Uint8 sr, sg, sb, sa;
  SDL_GetRenderDrawBlendMode(r, &saved_mode);
  SDL_GetRenderDrawColor(r, &sr, &sg, &sb, &sa);
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
  bool font = ensure_font(s, r);
  for (int layer = 0; layer < GOOF_UI_LAYERS; layer++) {
    for (int i = 0; i < d.nrect; i++) {
      const GoofUiRect *q = &d.rect[i];
      if (q->layer != layer) continue;
      SDL_Color c = kColors[q->color < GOOF_UI_C_COUNT ? q->color : GOOF_UI_C_TEXT];
      SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
      SDL_Rect dst = {ox + q->x * k, oy + q->y * k, q->w * k, q->h * k};
      SDL_RenderFillRect(r, &dst);
    }
    if (!font) continue;
    for (int i = 0; i < d.ntext; i++) {
      const GoofUiText *t = &d.text[i];
      if (t->layer != layer) continue;
      SDL_Color c = kColors[t->color < GOOF_UI_C_COUNT ? t->color : GOOF_UI_C_TEXT];
      SDL_SetTextureColorMod(s->font, c.r, c.g, c.b);
      for (int j = 0; t->text[j]; j++) {
        unsigned ch = (unsigned char)t->text[j];
        if (ch < GOOF_FONT_FIRST || ch >= GOOF_FONT_FIRST + GOOF_FONT_GLYPHS) ch = '?';
        if (ch == ' ') continue;
        SDL_Rect src = {(int)(ch - GOOF_FONT_FIRST) * ATLAS_CELL, 0, GOOF_FONT_W,
                        GOOF_FONT_H};
        SDL_Rect dst = {ox + (t->x + j * GOOF_UI_CELL_W) * k, oy + t->y * k,
                        GOOF_FONT_W * k, GOOF_FONT_H * k};
        SDL_RenderCopy(r, s->font, &src, &dst);
      }
    }
  }
  SDL_SetRenderDrawBlendMode(r, saved_mode);
  SDL_SetRenderDrawColor(r, sr, sg, sb, sa);
}

void goof_sdl_settings_destroy(GoofSdlSettings *s) {
  if (s->font) SDL_DestroyTexture(s->font);
  s->font = NULL;
  s->font_owner = NULL;
}
