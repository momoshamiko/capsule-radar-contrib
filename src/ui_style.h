#pragma once
// Shared LVGL styles built from the design tokens (ui_tokens.h): one lv_style_t per role.
// Objects get their role styles once, at creation, and set no local colour for a themed
// property; a theme change rewrites these styles in place, so every object follows.
// Colours that depend on live data (emergency text, battery fill, list-row recolour codes)
// are local and rebuilt by ui.cpp's theme hook.
#include <lvgl.h>
#include "ui_tokens.h"

#define UI_FONT_S (&font_px11)
#define UI_FONT_M (&font_px22)
#define UI_FONT_L (&font_px33)

// First call initialises the styles (ui_create() makes it before creating any object; never
// before lv_init()). Every call writes ui_palette(theme) into the styles, refreshes all
// objects and then calls the hook. radar::setTheme() calls it, so boot, the theme hold and
// the simulator all recolour the chrome together with the scope.
void ui_style_set_theme(int theme);
const UiPalette &ui_style_palette(void);
int ui_style_theme(void);
void ui_style_set_theme_hook(void (*cb)(int theme));

// Text colour roles.
extern lv_style_t UI_ST_INK, UI_ST_SOFT, UI_ST_FAINT, UI_ST_ACCENT, UI_ST_ACCENT_HI, UI_ST_INFO;
extern lv_style_t UI_ST_TITLE;        // M font, accentHi, title letter space
extern lv_style_t UI_ST_SURFACE;      // card/chip: surface fill, 2-px surfaceLine frame, square
extern lv_style_t UI_ST_CHIP_EMERG;   // emergency chip / power-off / status note: surface fill, emerg frame + text
extern lv_style_t UI_ST_CHIP_WARN;    // status note "DATA OLD": surface fill, warn frame + text
extern lv_style_t UI_ST_CHIP_ACCENT;  // pull-down THEME button: surface fill, accent frame, accentHi text
extern lv_style_t UI_ST_OVERLAY;      // text over the weather image: surface at 170/255, square
extern lv_style_t UI_ST_DIVIDER;      // lv_line: faint, 2 px, dotted (2 on / 2 off)
extern lv_style_t UI_ST_DOT, UI_ST_DOT_ON;   // page dots: 4x4 faint; DOT_ON (LV_STATE_CHECKED) 6x6 accentHi
extern lv_style_t UI_ST_IMG_INK, UI_ST_IMG_SOFT, UI_ST_IMG_HI;   // 1-bit icons recoloured to ink / soft / accentHi
extern lv_style_t UI_ST_RING;         // splash ring: round, 2-px accent border (opacity set per ring)
extern lv_style_t UI_ST_ARC_HI;       // splash spinner indicator: 4-px square-ended accentHi arc
