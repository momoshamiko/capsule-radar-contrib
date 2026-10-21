#pragma once
// Design tokens: every colour, size and spacing choice the screens share, in one place.
// Plain data (no LVGL), so the scope (radar_view) and the chrome (ui, ui_style) read the
// same values. One palette per RadarTheme; the fonts are the three Departure Mono sizes
// declared in include/lv_conf.h (font_px11 / font_px22 / font_px33).
#include <stdint.h>

struct UiPalette {
    uint32_t ring, lead, ink, soft;          // scope: rings, sweep lead, plane names, muted text
    uint32_t accent, accentHi, faint;        // chrome: highlights, titles, dividers/inactive
    uint32_t surface, surfaceLine, panel;    // card/chip fill, frames, round-panel background
    uint32_t emerg, warn, info;              // emergency, stale/acquiring, rain
};

// Order = RadarTheme (radar_view.h): Phosphor, Orb, Amber, Military. Orb keeps the Phosphor
// scope values (its balls and grid draw with their own colours); its chrome is yellow on green.
inline constexpr UiPalette UI_PALETTES[4] = {
    // ring      lead      ink       soft      accent    accentHi  faint     surface   surfLine  panel     emerg     warn      info
    { 0x1DFF86, 0x3DFF9A, 0xEAFFF3, 0x9AFFC8, 0x1DFF86, 0x3DFF9A, 0x2D6046, 0x07140C, 0x1F6A40, 0x05100A, 0xFF5A3C, 0xFFB23C, 0x4DDCFF },
    { 0x1DFF86, 0x3DFF9A, 0xEAFFF3, 0x9AFFC8, 0xFFE11A, 0xFFC24D, 0x4A5A2A, 0x0B2A0C, 0x3F8B30, 0x09250A, 0xFF5A3C, 0xFF8A1E, 0x4DDCFF },
    { 0xFFB23C, 0xFFD27A, 0xFFE9C2, 0xFFC98A, 0xFFB23C, 0xFFD27A, 0x60421E, 0x120A02, 0x6E481A, 0x0C0702, 0xFF5A3C, 0xFFF07A, 0x4DDCFF },
    { 0x49C46B, 0x76E08C, 0xE0FFE6, 0x9FD7A8, 0x49C46B, 0x76E08C, 0x34603F, 0x06120A, 0x285C3A, 0x030A06, 0xFF5A3C, 0xFFB23C, 0x4DDCFF },
};

inline const UiPalette &ui_palette(int theme) {
    return UI_PALETTES[(theme >= 0 && theme < 4) ? theme : 0];
}

// Altitude bands for planes, the same in every theme. Red is kept for emergencies only, so
// the lowest band is pink (upstream issue #25: low traffic in red read as an emergency).
inline constexpr uint32_t UI_ALT_BANDS[5] = { 0xFF7AD9, 0xFFB23C, 0xC8FF3C, 0x39FF14, 0x3CE0FF };
//                                            <3000 ft  <10000    <20000    <30000    above
inline constexpr uint32_t UI_ALT_GROUND = 0x888888;

inline constexpr int UI_SP          = 4;    // spacing unit: gaps are multiples of it
inline constexpr int UI_FRAME_W     = 2;    // frames and dividers: one pixel of the M font
inline constexpr int UI_TITLE_SPACE = 4;    // title letter space: two M-font pixels

// Font metrics (as generated, see src/fonts/README.md): advance and line height per size.
inline constexpr int UI_S_ADV = 7,  UI_S_LINE = 11;
inline constexpr int UI_M_ADV = 14, UI_M_LINE = 22;
inline constexpr int UI_L_ADV = 21, UI_L_LINE = 33;
