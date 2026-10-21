// Shared styles from the design tokens; see ui_style.h.
#include "ui_style.h"

lv_style_t UI_ST_INK, UI_ST_SOFT, UI_ST_FAINT, UI_ST_ACCENT, UI_ST_ACCENT_HI, UI_ST_INFO;
lv_style_t UI_ST_TITLE, UI_ST_SURFACE, UI_ST_CHIP_EMERG, UI_ST_OVERLAY;
lv_style_t UI_ST_DIVIDER, UI_ST_DOT, UI_ST_DOT_ON, UI_ST_IMG_INK, UI_ST_IMG_SOFT, UI_ST_IMG_HI;
lv_style_t UI_ST_RING, UI_ST_ARC_HI, UI_ST_CHIP_WARN, UI_ST_CHIP_ACCENT;

static bool s_init = false;
static int  s_theme = 0;
static void (*s_hook)(int) = nullptr;

static inline lv_color_t C(uint32_t hex) { return lv_color_hex(hex); }

// Everything that does not depend on the theme: fonts, frames, shapes.
static void init_styles(void) {
    lv_style_t *all[] = { &UI_ST_INK, &UI_ST_SOFT, &UI_ST_FAINT, &UI_ST_ACCENT, &UI_ST_ACCENT_HI,
                          &UI_ST_INFO, &UI_ST_TITLE, &UI_ST_SURFACE, &UI_ST_CHIP_EMERG, &UI_ST_OVERLAY,
                          &UI_ST_DIVIDER, &UI_ST_DOT, &UI_ST_DOT_ON,
                          &UI_ST_IMG_INK, &UI_ST_IMG_SOFT, &UI_ST_IMG_HI, &UI_ST_RING, &UI_ST_ARC_HI,
                          &UI_ST_CHIP_WARN, &UI_ST_CHIP_ACCENT };
    for (lv_style_t *s : all) lv_style_init(s);

    lv_style_set_text_font(&UI_ST_TITLE, UI_FONT_M);
    lv_style_set_text_letter_space(&UI_ST_TITLE, UI_TITLE_SPACE);

    lv_style_t *boxes[] = { &UI_ST_SURFACE, &UI_ST_CHIP_EMERG, &UI_ST_CHIP_WARN, &UI_ST_CHIP_ACCENT };
    for (lv_style_t *s : boxes) {
        lv_style_set_bg_opa(s, LV_OPA_COVER);
        lv_style_set_border_width(s, UI_FRAME_W);
        lv_style_set_border_opa(s, LV_OPA_COVER);
        lv_style_set_radius(s, 0);
    }
    lv_style_set_bg_opa(&UI_ST_OVERLAY, 170);
    lv_style_set_radius(&UI_ST_OVERLAY, 0);
    lv_style_set_pad_hor(&UI_ST_OVERLAY, UI_SP + 2);

    lv_style_set_line_width(&UI_ST_DIVIDER, UI_FRAME_W);
    lv_style_set_line_dash_width(&UI_ST_DIVIDER, 2);
    lv_style_set_line_dash_gap(&UI_ST_DIVIDER, 2);

    lv_style_set_bg_opa(&UI_ST_DOT, LV_OPA_COVER);
    lv_style_set_radius(&UI_ST_DOT, 0);
    lv_style_set_size(&UI_ST_DOT, 4);
    lv_style_set_bg_opa(&UI_ST_DOT_ON, LV_OPA_COVER);
    lv_style_set_radius(&UI_ST_DOT_ON, 0);
    lv_style_set_size(&UI_ST_DOT_ON, 6);

    lv_style_set_img_recolor_opa(&UI_ST_IMG_INK, LV_OPA_COVER);
    lv_style_set_img_recolor_opa(&UI_ST_IMG_SOFT, LV_OPA_COVER);
    lv_style_set_img_recolor_opa(&UI_ST_IMG_HI, LV_OPA_COVER);

    lv_style_set_radius(&UI_ST_RING, LV_RADIUS_CIRCLE);
    lv_style_set_border_width(&UI_ST_RING, UI_FRAME_W);
    lv_style_set_arc_width(&UI_ST_ARC_HI, 4);
    lv_style_set_arc_rounded(&UI_ST_ARC_HI, false);
    s_init = true;
}

void ui_style_set_theme(int theme) {
    if (!s_init) init_styles();
    s_theme = (theme >= 0 && theme < 4) ? theme : 0;
    const UiPalette &p = ui_palette(s_theme);
    lv_style_set_text_color(&UI_ST_INK, C(p.ink));
    lv_style_set_text_color(&UI_ST_SOFT, C(p.soft));
    lv_style_set_text_color(&UI_ST_FAINT, C(p.faint));
    lv_style_set_text_color(&UI_ST_ACCENT, C(p.accent));
    lv_style_set_text_color(&UI_ST_ACCENT_HI, C(p.accentHi));
    lv_style_set_text_color(&UI_ST_INFO, C(p.info));
    lv_style_set_text_color(&UI_ST_TITLE, C(p.accentHi));
    lv_style_set_bg_color(&UI_ST_SURFACE, C(p.surface));
    lv_style_set_border_color(&UI_ST_SURFACE, C(p.surfaceLine));
    lv_style_set_bg_color(&UI_ST_CHIP_EMERG, C(p.surface));
    lv_style_set_border_color(&UI_ST_CHIP_EMERG, C(p.emerg));
    lv_style_set_text_color(&UI_ST_CHIP_EMERG, C(p.emerg));
    lv_style_set_bg_color(&UI_ST_CHIP_WARN, C(p.surface));
    lv_style_set_border_color(&UI_ST_CHIP_WARN, C(p.warn));
    lv_style_set_text_color(&UI_ST_CHIP_WARN, C(p.warn));
    lv_style_set_bg_color(&UI_ST_CHIP_ACCENT, C(p.surface));
    lv_style_set_border_color(&UI_ST_CHIP_ACCENT, C(p.accent));
    lv_style_set_text_color(&UI_ST_CHIP_ACCENT, C(p.accentHi));
    lv_style_set_bg_color(&UI_ST_OVERLAY, C(p.surface));
    lv_style_set_line_color(&UI_ST_DIVIDER, C(p.faint));
    lv_style_set_bg_color(&UI_ST_DOT, C(p.faint));
    lv_style_set_bg_color(&UI_ST_DOT_ON, C(p.accentHi));
    lv_style_set_img_recolor(&UI_ST_IMG_INK, C(p.ink));
    lv_style_set_img_recolor(&UI_ST_IMG_SOFT, C(p.soft));
    lv_style_set_img_recolor(&UI_ST_IMG_HI, C(p.accentHi));
    lv_style_set_border_color(&UI_ST_RING, C(p.accent));
    lv_style_set_arc_color(&UI_ST_ARC_HI, C(p.accentHi));
    lv_obj_report_style_change(NULL);   // every object using any of these styles refreshes
    if (s_hook) s_hook(s_theme);
}

const UiPalette &ui_style_palette(void) { return ui_palette(s_theme); }
int ui_style_theme(void) { return s_theme; }
void ui_style_set_theme_hook(void (*cb)(int)) { s_hook = cb; }
