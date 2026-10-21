// UI: tileview (radar / list / stats / weather) + tap-to-inspect detail card, in the Pixel
// look: one pixel font in three sizes and shared, theme-coloured styles (ui_style.h, tokens
// in ui_tokens.h). Pure LVGL, portable (device + SDL simulator). Taps hit-test via
// radar::hitTest; selection lives in radar. All coordinates are logical screen pixels (the
// panel is 466x466, centre 233,233).
#include "ui.h"
#include "radar_view.h"
#include "route.h"
#include "photo.h"
#include "weather.h"
#include "wx_radar.h"
#include "cloud_image.h"
#include "airports.h"
#include "config.h"
#include "ui_style.h"
#include "ui_icons.h"
#include <lvgl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <vector>

static lv_obj_t *s_tv = nullptr;
static lv_obj_t *s_tileRadar = nullptr, *s_tileList = nullptr, *s_tileStats = nullptr, *s_tileWeather = nullptr;

// detail card
static lv_obj_t *s_card = nullptr, *s_cardTitle = nullptr, *s_cardChip = nullptr, *s_cardChipLbl = nullptr;
static lv_obj_t *s_cardCap = nullptr, *s_cardVal = nullptr, *s_cardDet = nullptr, *s_cardDiv = nullptr;
static lv_obj_t *s_cardRoute = nullptr;
static lv_obj_t *s_cardClose = nullptr;                          // the card's X: just outside it, right edges flush
static lv_obj_t *s_photo = nullptr, *s_photoCredit = nullptr;   // aircraft photo above the card
static char s_lastRouteReq[12] = "";
static lv_coord_t s_cardTop = -1;                               // card's current top edge (-1 = hidden)

// status: the note under the N (only when something is wrong) and the pull-down
static lv_obj_t *s_noteEmerg = nullptr, *s_noteEmergLbl = nullptr;   // NO WIFI / LOW BATTERY
static lv_obj_t *s_noteWarn = nullptr, *s_noteWarnLbl = nullptr;     // DATA OLD
static lv_obj_t *s_pull = nullptr, *s_pullSheet = nullptr;           // catcher (lv_layer_top) + sheet
static lv_obj_t *s_pullTime = nullptr, *s_pullDate = nullptr, *s_pullData = nullptr;
static lv_obj_t *s_pullWifi = nullptr, *s_pullBars[4] = { nullptr, nullptr, nullptr, nullptr };
static lv_obj_t *s_pullWifiLbl = nullptr, *s_pullGpsLbl = nullptr;
static lv_obj_t *s_pullPlane = nullptr, *s_pullPlanesLbl = nullptr;
static lv_obj_t *s_pullBatt = nullptr, *s_pullBattFrame = nullptr, *s_pullBattFill = nullptr;
static lv_obj_t *s_pullBattNub = nullptr, *s_pullBattLbl = nullptr, *s_pullBolt = nullptr;
static lv_obj_t *s_pullTheme = nullptr, *s_pullThemeLbl = nullptr;
static lv_timer_t *s_pullTimer = nullptr;                           // auto-close, paused while closed
static bool s_pullOpen = false;
static lv_obj_t *s_clockDate = nullptr;                          // the date, on the idle clock

// list / stats
static lv_obj_t *s_list = nullptr, *s_listTitle = nullptr, *s_listHdrAlt = nullptr, *s_listHdrDist = nullptr;
static lv_obj_t *s_statsNames = nullptr, *s_statsVals = nullptr, *s_statsNet = nullptr;

// weather
static lv_obj_t *s_weatherTitle = nullptr;
static lv_obj_t *s_wxCanvas = nullptr, *s_wxStatus = nullptr, *s_wxAirport = nullptr;
static lv_obj_t *s_wxFooter = nullptr, *s_wxMeta = nullptr, *s_wxAttrib = nullptr;
static lv_obj_t *s_wxRings[3] = { nullptr, nullptr, nullptr };
static lv_obj_t *s_wxNorth = nullptr, *s_wxCenter = nullptr, *s_wxRange = nullptr;
static lv_obj_t *s_wxMode[3] = { nullptr, nullptr, nullptr }, *s_wxModeLbl[3] = { nullptr, nullptr, nullptr };
enum WeatherViewMode { WEATHER_RADAR, WEATHER_CLOUDS, WEATHER_FORECAST };
static WeatherViewMode s_weatherMode = WEATHER_RADAR;
static lv_obj_t *s_fcIcon = nullptr, *s_fcCurrent = nullptr, *s_fcCondition = nullptr, *s_fcUpdated = nullptr;
static lv_obj_t *s_fcMetricName[3] = { nullptr, nullptr, nullptr };
static lv_obj_t *s_fcMetricValue[3] = { nullptr, nullptr, nullptr };
static lv_obj_t *s_fcDay[3] = { nullptr, nullptr, nullptr };
static lv_obj_t *s_fcDayIcon[3] = { nullptr, nullptr, nullptr };
static lv_obj_t *s_fcDayTemp[3] = { nullptr, nullptr, nullptr };
static lv_obj_t *s_fcDayRain[3] = { nullptr, nullptr, nullptr };

// on lv_layer_top(): page dots and the theme toast
static lv_obj_t *s_dots = nullptr, *s_dot[4] = { nullptr, nullptr, nullptr, nullptr };
static lv_obj_t *s_toast = nullptr, *s_toastLbl = nullptr;
static lv_timer_t *s_toastTimer = nullptr;

static void send_obstacles(void);
static void apply_status_note(void);
static void pull_refresh(void);
static void pull_open(void);
static void set_text(lv_obj_t *l, const char *t);   // guarded label write (defined below)
static void pull_close(void);

// --------------------------------------------------------------------- units
// 0 = Aviation (ft, kt, nm) · 1 = Metric (m, km/h, km) · 2 = Imperial (ft, mph, mi).
// The feed gives altitude in ft, speed in kt, vertical speed in fpm, distance in km.
static int s_units = 0;
void ui_set_units(int u) {
    s_units = (u < 0 || u > 2) ? 0 : u;
    radar::setUnits(s_units);   // scope altitude labels (ft/m) follow the same preset
}

// Accessibility: "large text" puts the data that is small in this design (plane labels, the
// zoom chip, the card's detail and route lines) into the M size. Fonts are baked into the
// widgets at creation, so the flag must be set BEFORE ui_create() (the web toggle reboots).
static bool s_bigText = false;
void ui_set_large_text(bool on) { s_bigText = on; }

static void fmt_alt(char *b, size_t n, float ft, bool gnd) {
    if (gnd)            snprintf(b, n, "GND");
    else if (s_units == 1) snprintf(b, n, "%.0f m",  ft * 0.3048f);
    else                snprintf(b, n, "%.0f ft", ft);
}
static void fmt_alt_num(char *b, size_t n, float ft, bool gnd) {   // number only (unit in the caption)
    if (gnd)            snprintf(b, n, "GND");
    else if (s_units == 1) snprintf(b, n, "%.0f", ft * 0.3048f);
    else                snprintf(b, n, "%.0f", ft);
}
static void fmt_spd_num(char *b, size_t n, float kt) {
    if (kt != kt)          snprintf(b, n, "-");
    else if (s_units == 1) snprintf(b, n, "%.0f", kt * 1.852f);
    else if (s_units == 2) snprintf(b, n, "%.0f", kt * 1.15078f);
    else                   snprintf(b, n, "%.0f", kt);
}
static void fmt_vs(char *b, size_t n, float fpm, bool unit) {
    if (fpm != fpm)        snprintf(b, n, "-");
    else if (s_units == 1) snprintf(b, n, unit ? "%+.1f m/s" : "%+.1f", fpm * 0.00508f);
    else                   snprintf(b, n, unit ? "%+.0f fpm" : "%+.0f", fpm);
}
static float dist_val(float km) {
    if (s_units == 0) return km * 0.539957f;   // Aviation -> nautical miles
    if (s_units == 2) return km * 0.621371f;   // Imperial -> miles
    return km;                                   // Metric   -> km
}
static const char *dist_unit(void) { return s_units == 0 ? "nm" : (s_units == 2 ? "mi" : "km"); }
static const char *dist_unit_up(void) { return s_units == 0 ? "NM" : (s_units == 2 ? "MI" : "KM"); }
static const char *alt_unit_up(void) { return s_units == 1 ? "M" : "FT"; }
static const char *spd_unit_up(void) { return s_units == 1 ? "KM/H" : (s_units == 2 ? "MPH" : "KT"); }
static void fmt_dist4(char *b, size_t n, float km) {   // 4 characters wide: "0.9", "14.7", " 123"
    const float v = dist_val(km);
    snprintf(b, n, v < 100.0f ? "%4.1f" : "%4.0f", v);
}

static float weather_temp(float c) { return s_units == 2 ? c * 1.8f + 32.0f : c; }
static const char *weather_temp_unit(void) { return s_units == 2 ? "F" : "C"; }
static float weather_wind(float kmh) {
    if (s_units == 0) return kmh * 0.539957f;
    if (s_units == 2) return kmh * 0.621371f;
    return kmh;
}
static const char *weather_wind_unit(void) { return s_units == 0 ? "KT" : (s_units == 2 ? "MPH" : "KM/H"); }
static const char *cardinal(float deg) {
    static const char *p[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int i = ((int)(deg + 22.5f) / 45) & 7;
    return p[i];
}

// Fold Latin-1 accents / drop any other non-ASCII: the pixel font has ASCII plus four
// symbols, and a missing glyph draws nothing. For place names on the card.
static void fold_ascii(char *s) {
    char *o = s;
    for (unsigned char *p = (unsigned char *)s; *p; ) {
        if (*p < 0x80) { *o++ = (char)*p++; continue; }
        if (*p == 0xC3 && p[1]) {                       // Latin-1 Supplement (U+00C0..U+00FF)
            const unsigned char d = p[1];
            char r;
            if      (d >= 0x80 && d <= 0x85) r = 'A';
            else if (d >= 0xA0 && d <= 0xA5) r = 'a';
            else if (d == 0x87)              r = 'C';
            else if (d == 0xA7)              r = 'c';
            else if (d >= 0x88 && d <= 0x8B) r = 'E';
            else if (d >= 0xA8 && d <= 0xAB) r = 'e';
            else if (d >= 0x8C && d <= 0x8F) r = 'I';
            else if (d >= 0xAC && d <= 0xAF) r = 'i';
            else if (d == 0x91)              r = 'N';
            else if (d == 0xB1)              r = 'n';
            else if (d >= 0x92 && d <= 0x96) r = 'O';
            else if (d >= 0xB2 && d <= 0xB6) r = 'o';
            else if (d >= 0x99 && d <= 0x9C) r = 'U';
            else if (d >= 0xB9 && d <= 0xBC) r = 'u';
            else                             r = '?';
            *o++ = r; p += 2; continue;
        }
        ++p;                                            // skip other multibyte lead + continuation
        while (*p >= 0x80 && *p < 0xC0) ++p;
    }
    *o = 0;
}

#define SYM_ARROW  "\xE2\x86\x92"   // U+2192, in the font
#define SYM_SQUARE "\xE2\x96\xA0"   // U+25A0
#define SYM_DOT    "\xC2\xB7"       // U+00B7
#define SYM_DEG    "\xC2\xB0"       // U+00B0

// ------------------------------------------------------------------- builders
static inline const UiPalette &P() { return ui_style_palette(); }
static inline lv_color_t COL(uint32_t c) { return lv_color_hex(c); }   // not HEX: Arduino defines it

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, lv_style_t *role, const char *txt) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_style_all(l);
    if (role) lv_obj_add_style(l, role, 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_label_set_text(l, txt ? txt : "");
    return l;
}
// A label centred on x 233 with its box top at screen y (for objects on a full-screen tile).
static lv_obj_t *mk_label_c(lv_obj_t *parent, const lv_font_t *font, lv_style_t *role, const char *txt, lv_coord_t y) {
    lv_obj_t *l = mk_label(parent, font, role, txt);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}
static lv_obj_t *mk_box(lv_obj_t *parent, lv_style_t *role, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    if (role) lv_obj_add_style(o, role, 0);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}
static lv_obj_t *mk_img(lv_obj_t *parent, lv_style_t *role, const lv_img_dsc_t *src) {   // a 1-bit icon, coloured by role
    lv_obj_t *o = lv_img_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_add_style(o, role, 0);
    lv_img_set_src(o, src);
    return o;
}
static lv_obj_t *mk_divider(lv_obj_t *parent, lv_coord_t x1, lv_coord_t x2, lv_coord_t y, lv_point_t *pts) {
    lv_obj_t *l = lv_line_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_add_style(l, &UI_ST_DIVIDER, 0);
    pts[0].x = 0; pts[0].y = 1;
    pts[1].x = (lv_coord_t)(x2 - x1); pts[1].y = 1;
    lv_line_set_points(l, pts, 2);
    lv_obj_set_pos(l, x1, (lv_coord_t)(y - 1));
    return l;
}
static void show(lv_obj_t *o, bool v) {   // no change, no flag write (LVGL repaints a shown object on every clear)
    if (!o || v == !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    if (v) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else   lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}
static inline bool shown(lv_obj_t *o) { return o && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN); }
static inline int text_w(lv_obj_t *l, int adv) { return l ? (int)strlen(lv_label_get_text(l)) * adv : 0; }

// ----------------------------------------------------------------- detail card
// Idle clock up (radar_view scene step >= 1): the status note, zoom chip, card and pull-down
// make way for it.
static bool s_clockUp = false;
static int  s_clockStage = 0;
static lv_obj_t *s_zoomBtn = nullptr, *s_zoomLbl = nullptr;

#define CARD_X          86            // the card's left edge (screen) and width
#define CARD_W          294
#define CARD_CLOSE_GAP  (UI_SP + 2)   // card edge to its X
static inline lv_coord_t card_h(void) { return s_bigText ? 150 : 128; }
static inline lv_coord_t card_bottom_top(void) { return s_bigText ? 258 : 280; }
static inline lv_coord_t card_close_size(void) { return s_bigText ? 32 : 28; }   // the X's square box
// The X's top edge for a card whose top edge is `top`: on the side facing the scope's centre,
// above the card at the bottom, below it at the top.
static inline lv_coord_t card_close_y(lv_coord_t top) {
    return top == card_bottom_top() ? (lv_coord_t)(top - CARD_CLOSE_GAP - card_close_size())
                                    : (lv_coord_t)(top + card_h() + CARD_CLOSE_GAP);
}

// The X shows with the card, except while the status pull-down is open: the sheet would cut
// it (or leave it floating below), and a tap there closes the sheet, not the card.
static void card_close_sync(void) { show(s_cardClose, shown(s_card) && !s_pullOpen); }

static void card_set_emergency(bool emg) {
    if (emg) lv_obj_set_style_text_color(s_cardTitle, COL(P().emerg), 0);
    else     lv_obj_remove_local_style_prop(s_cardTitle, LV_STYLE_TEXT_COLOR, 0);
    show(s_cardChip, emg);
}

static void refresh_card(void) {
    AcInfo in;
    const bool sel = radar::selected(in);
    if (s_clockUp || !sel) {
        show(s_card, false);
        show(s_photo, false);
        show(s_photoCredit, false);
        if (!sel) s_lastRouteReq[0] = 0;
        s_cardTop = -1;
        show(s_zoomBtn, !s_clockUp);
        card_close_sync();
        send_obstacles();
        return;
    }
    // Bottom, unless the plane it describes would sit under it: then the top (no photo there).
    int16_t px = 0, py = 0;
    const bool onScope = radar::selectedScreenPos(&px, &py);
    const lv_coord_t top = (onScope && py + 25 >= card_bottom_top()) ? 92 : card_bottom_top();
    if (top != s_cardTop) {
        lv_obj_set_y(s_card, top);
        lv_obj_set_y(s_cardClose, card_close_y(top));
        s_cardTop = top;
    }
    show(s_card, true);
    card_close_sync();
    show(s_zoomBtn, false);                          // the card sits over its corner of the scope

    set_text(s_cardTitle, in.call[0] ? in.call : in.hex);
    card_set_emergency(in.emergency);
    if (in.emergency) {
        char c[24];
        if (s_bigText) snprintf(c, sizeof(c), "%04d", in.squawk < 0 ? 0 : in.squawk);
        else           snprintf(c, sizeof(c), "%04d EMERGENCY", in.squawk < 0 ? 0 : in.squawk);
        set_text(s_cardChipLbl, c);
        const lv_coord_t w = (lv_coord_t)(strlen(c) * (s_bigText ? UI_M_ADV : UI_S_ADV) + 16);
        lv_obj_set_width(s_cardChip, w);
        lv_obj_set_x(s_cardChip, (lv_coord_t)(CARD_W - 12 - w));
    }

    char cap[48], val[48];
    char a1[12], a2[12], a3[12];
    snprintf(a1, sizeof(a1), "ALT %s", alt_unit_up());
    snprintf(a2, sizeof(a2), "SPD %s", spd_unit_up());
    snprintf(a3, sizeof(a3), "DIST %s", dist_unit_up());
    snprintf(cap, sizeof(cap), "%-14s%-14s%s", a1, a2, a3);   // S columns 0 / 14 / 28
    set_text(s_cardCap, cap);
    char altS[12], spdS[12], dS[12];
    fmt_alt_num(altS, sizeof(altS), in.altFt, in.onGround);
    fmt_spd_num(spdS, sizeof(spdS), in.gsKt);
    snprintf(dS, sizeof(dS), "%.1f", dist_val(in.distKm));
    snprintf(val, sizeof(val), "%-7s%-7s%s", altS, spdS, dS);  // M columns 0 / 7 / 14
    set_text(s_cardVal, val);

    // HDG is the aircraft's own ground track (what rotates the glyph), not the bearing
    // from home: NaN (unknown) prints as "-".
    char vsS[16], hdgS[8], sqS[8], det[64];
    fmt_vs(vsS, sizeof(vsS), in.vsFpm, !s_bigText);
    if (in.track != in.track) snprintf(hdgS, sizeof(hdgS), "-");
    else                      snprintf(hdgS, sizeof(hdgS), "%03.0f", in.track);
    if (in.squawk < 0) snprintf(sqS, sizeof(sqS), "-");
    else               snprintf(sqS, sizeof(sqS), "%04d", in.squawk);
    if (s_bigText)          snprintf(det, sizeof(det), "V/S %s  HDG %s", vsS, hdgS);
    else if (in.type[0])    snprintf(det, sizeof(det), "%s  V/S %s  HDG %s  SQK %s", in.type, vsS, hdgS, sqS);
    else                    snprintf(det, sizeof(det), "V/S %s  HDG %s  SQK %s", vsS, hdgS, sqS);
    fold_ascii(det);
    set_text(s_cardDet, det);

    // route (origin -> destination), looked up asynchronously by callsign
    if (in.call[0] && strcmp(in.call, s_lastRouteReq) != 0) {
        snprintf(s_lastRouteReq, sizeof(s_lastRouteReq), "%s", in.call);
        route_request(in.call);
    }
    char rfrom[40], rto[40];
    if (!in.call[0]) {
        set_text(s_cardRoute, "Route -");               // no callsign -> nothing to look up
    } else if (route_get(in.call, rfrom, sizeof(rfrom), rto, sizeof(rto))) {
        char rt[96];
        if (rfrom[0] || rto[0]) {
            fold_ascii(rfrom);
            fold_ascii(rto);
            snprintf(rt, sizeof(rt), "%s " SYM_ARROW " %s", rfrom[0] ? rfrom : "?", rto[0] ? rto : "?");
        } else {
            snprintf(rt, sizeof(rt), "Route unavailable");
        }
        set_text(s_cardRoute, rt);
    } else {
        set_text(s_cardRoute, "Looking up route...");   // pending: lookup in flight
    }

    // aircraft photo (planespotters), above the centre while the card sits at the bottom
    if (in.hex[0]) photo_request(in.hex);
    const bool photoRoom = top != 92;
    int pw = 0, ph = 0; char pcred[40];
    if (photoRoom && s_photo && in.hex[0] && photo_get(in.hex, &pw, &ph, pcred, sizeof(pcred)) && pw > 0 && ph > 0) {
        int mw, mh;
        lv_color_t *pbuf = photo_buffer(&mw, &mh);
        // Re-point / resize / invalidate the canvas only when the photo itself changed
        // (hex, size, buffer): the re-point repaints the whole image every poll otherwise.
        static char lastHex[8] = "";
        static int lastW = 0, lastH = 0;
        static lv_color_t *lastBuf = nullptr;
        if (strcmp(in.hex, lastHex) != 0 || pw != lastW || ph != lastH || pbuf != lastBuf) {
            snprintf(lastHex, sizeof(lastHex), "%s", in.hex);
            lastW = pw; lastH = ph; lastBuf = pbuf;
            lv_canvas_set_buffer(s_photo, pbuf, pw, ph, LV_IMG_CF_TRUE_COLOR);
            lv_obj_set_size(s_photo, pw, ph);
            lv_obj_align(s_photo, LV_ALIGN_CENTER, 0, -28 - ph / 2);   // sit lower: fill the band down to the card
            lv_obj_invalidate(s_photo);
        }
        show(s_photo, true);
        if (s_photoCredit) {
            char c[52];
            snprintf(c, sizeof(c), "Photo: %s", pcred[0] ? pcred : "planespotters.net");
            fold_ascii(c);
            set_text(s_photoCredit, c);
            lv_obj_align_to(s_photoCredit, s_photo, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
            show(s_photoCredit, true);
        }
    } else if (s_photo) {
        // No image to show: hide the canvas, and say quietly what is happening ("Loading..."
        // while the fetch is in flight, "No photo" once it finished without one).
        show(s_photo, false);
        if (s_photoCredit) {
            const bool done = in.hex[0] && photo_done(in.hex);
            set_text(s_photoCredit, done ? "No photo available" : "Loading photo...");
            lv_obj_align(s_photoCredit, LV_ALIGN_CENTER, 0, -104);   // where the photo would sit
            show(s_photoCredit, photoRoom);
        }
    }
    send_obstacles();
}

// --------------------------------------------------------------------- input
static bool s_themeHeld = false;   // this press reached the theme hold: its lift is not a tap
static int s_rangeIdx = -1;
static float s_rangeKm = RANGE_KM_DEFAULT;   // current display range (km), for the stats view
static void (*s_rangeCb)(float) = nullptr;
static lv_obj_t *s_offBtn = nullptr, *s_offLbl = nullptr, *s_offRing = nullptr;
static void (*s_poweroffCb)(void) = nullptr;
static uint32_t s_offT0 = 0;
static bool s_offFired = false;
static lv_timer_t *s_offRestoreTimer = nullptr;

// The button exists only on boards that can actually power off: it stays hidden until a
// non-null callback is registered (null hides it again).
void ui_set_poweroff_cb(void (*cb)(void)) {
    s_poweroffCb = cb;
    show(s_offBtn, cb != nullptr);
}

// Power off needs a 1.5 s HOLD so a swipe or stray tap can't turn the radar off.
#define POWEROFF_HOLD_MS 1500
// If the radar is still running a few seconds after "Powering off", the shutdown didn't
// happen (e.g. the PMIC refused): go back to the hold prompt so the owner isn't stuck.
#define POWEROFF_RESTORE_MS 5000
static void poweroff_restore_cb(lv_timer_t *t) {
    s_offRestoreTimer = nullptr;
    lv_timer_del(t);
    s_offFired = false;
    if (s_offLbl) lv_label_set_text(s_offLbl, "HOLD TO POWER OFF");
    if (s_offRing) lv_arc_set_end_angle(s_offRing, 0);
}

// Hold progress: a ring round the screen edge fills clockwise from 12 o'clock over the hold
// and unwinds if the finger lifts early (the finger covers the button label).
#define POWEROFF_UNWIND_MS 400   // a full ring unwinds in this long (a partial one, pro rata)
// The ring is never hidden, just set to zero length: showing or hiding a full-screen object
// repaints the whole screen (~200 ms), while an angle change repaints only the changed part.
static void off_ring_set(int32_t deg) {
    if (!s_offRing) return;
    lv_arc_set_end_angle(s_offRing, (uint16_t)(deg <= 0 ? 0 : deg > 360 ? 360 : deg));
}
static void off_ring_anim_cb(void *var, int32_t v) { (void)var; off_ring_set(v); }
static int32_t off_ring_deg() {
    if (!s_offRing) return 0;
    return lv_arc_get_angle_end(s_offRing);
}
static void off_ring_unwind() {
    const int32_t deg = off_ring_deg();
    if (deg <= 0) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_offRing);
    lv_anim_set_exec_cb(&a, off_ring_anim_cb);
    lv_anim_set_values(&a, deg, 0);
    lv_anim_set_time(&a, (uint32_t)(POWEROFF_UNWIND_MS * deg / 360));
    lv_anim_start(&a);
}

static void poweroff_cb(lv_event_t *e) {
    const lv_event_code_t c = lv_event_get_code(e);
    if (!s_offLbl) return;
    if (c == LV_EVENT_PRESSED) {
        if (s_offRestoreTimer) { lv_timer_del(s_offRestoreTimer); s_offRestoreTimer = nullptr; }
        // Pressed again mid-unwind: carry on from where the ring is instead of from empty.
        const int32_t deg = off_ring_deg();
        if (s_offRing) lv_anim_del(s_offRing, off_ring_anim_cb);
        s_offT0 = lv_tick_get() - (uint32_t)(POWEROFF_HOLD_MS * deg / 360); s_offFired = false;
        lv_label_set_text(s_offLbl, "KEEP HOLDING...");
    } else if (c == LV_EVENT_PRESSING && !s_offFired) {
        const uint32_t held = lv_tick_elaps(s_offT0);
        off_ring_set((int32_t)(360 * held / POWEROFF_HOLD_MS));
        if (held >= POWEROFF_HOLD_MS) {
            s_offFired = true;
            lv_label_set_text(s_offLbl, "POWERING OFF");
            if (s_poweroffCb) s_poweroffCb();
            s_offRestoreTimer = lv_timer_create(poweroff_restore_cb, POWEROFF_RESTORE_MS, nullptr);
            lv_timer_set_repeat_count(s_offRestoreTimer, 1);
        }
    } else if ((c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) && !s_offFired) {
        lv_label_set_text(s_offLbl, "HOLD TO POWER OFF");
        off_ring_unwind();
    }
}

void ui_set_range_cb(void (*cb)(float)) { s_rangeCb = cb; }

static void zoom_cb(lv_event_t *e) {   // fires on PRESS (robust vs scroll-cancel on the tileview)
    (void)e;
    static uint32_t last = 0;
    const uint32_t now = lv_tick_get();
    if (now - last < 250) return;      // debounce repeated/held presses
    last = now;
    if (!s_rangeCb) return;
    const int n = (int)(sizeof(RANGE_STEPS_KM) / sizeof(RANGE_STEPS_KM[0]));
    s_rangeIdx = (s_rangeIdx + 1) % n;
    s_rangeCb(RANGE_STEPS_KM[s_rangeIdx]);
}

void ui_set_range_km(float km) {
    s_rangeKm = km;
    if (s_zoomLbl) {
        char b[20];
        snprintf(b, sizeof(b), "%.0f %s", dist_val(km), dist_unit_up());
        set_text(s_zoomLbl, b);
    }
    int best = 0; float bd = 1e9f;                 // sync the cycle index to the shown range
    const int n = (int)(sizeof(RANGE_STEPS_KM) / sizeof(RANGE_STEPS_KM[0]));
    for (int i = 0; i < n; ++i) { float d = km - RANGE_STEPS_KM[i]; if (d < 0) d = -d; if (d < bd) { bd = d; best = i; } }
    s_rangeIdx = best;
}

// The note in the middle of the radar. ms = 0 keeps it up until toast_hide() (the
// "LIFT TO CHANGE THEME" hint while a theme hold is armed); otherwise it hides itself.
static void toast_hide(void) {
    if (s_toastTimer) { lv_timer_del(s_toastTimer); s_toastTimer = nullptr; }
    show(s_toast, false);
}
static void toast_hide_cb(lv_timer_t *t) {
    s_toastTimer = nullptr;
    lv_timer_del(t);
    show(s_toast, false);
}
static void toast_show(const char *txt, uint32_t ms) {
    if (!s_toast) return;
    lv_label_set_text(s_toastLbl, txt);
    // padding 10/8 inside the 2-px frame: text + 2 * (10 + 2) by line + 2 * (8 + 2)
    const lv_coord_t w = (lv_coord_t)(strlen(txt) * UI_M_ADV + 24), h = UI_M_LINE + 20;
    lv_obj_set_size(s_toast, w, h);
    lv_obj_set_pos(s_toast, (lv_coord_t)(SCREEN_CX - w / 2), (lv_coord_t)(300 - h / 2));
    show(s_toast, true);
    if (s_toastTimer) { lv_timer_del(s_toastTimer); s_toastTimer = nullptr; }
    if (ms) {
        s_toastTimer = lv_timer_create(toast_hide_cb, ms, nullptr);
        lv_timer_set_repeat_count(s_toastTimer, 1);
    }
}

// A still hold of UI_THEME_HOLD_MS on the radar cycles the theme, on lift. When the hold
// time passes, a note says "LIFT TO CHANGE THEME"; moving the finger then cancels. Cycling
// on lift (not when the time passes) keeps a press that turns into a swipe from changing the
// theme. LVGL's own long press (400 ms) is not used: firm taps reached it, so tapping off a
// plane card with a half-second press changed the theme and left the card open.
static bool s_lpArmed = false;      // hold time passed, finger still down and still
static bool s_lpMoved = false;      // finger travelled past the drag threshold since the press
static lv_point_t s_lpOrigin;
static uint32_t s_lpT0 = 0;         // when the press began (LVGL tick)
// Finger travel that counts as a drag. 10 px is under 1 mm on these panels, less than a still
// fingertip drifts: on a 1.43, 3 of 15 holds were cancelled at 12-14 px (@h0verin, #36).
// A real swipe still cancels: any drag past this ends the hold.
#define LP_MOVE_PX 25

static void radar_press_cb(lv_event_t *e) {
    s_themeHeld = false;
    s_lpArmed = false;
    s_lpMoved = false;
    s_lpT0 = lv_tick_get();
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev) lv_indev_get_point(indev, &s_lpOrigin);
}

static void radar_pressing_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const long dx = (long)p.x - s_lpOrigin.x, dy = (long)p.y - s_lpOrigin.y;
    // A drag down from the top band opens the status pull-down, unless the drag has already
    // become a page swipe. LVGL then drops the rest of this press (no tap, no hold, no scroll;
    // PRESS_LOST at the lift).
    if (!s_clockUp && s_lpOrigin.y <= PULL_START_Y && dy >= PULL_MOVE_PX && dy > labs(dx) &&
        lv_indev_get_scroll_obj(indev) == NULL) {
        if (s_lpArmed) toast_hide();
        s_lpArmed = false;
        s_lpMoved = true;
        s_themeHeld = false;
        lv_indev_wait_release(indev);
        pull_open();
        return;
    }
    if (s_lpMoved) return;
    if (dx * dx + dy * dy > (long)LP_MOVE_PX * LP_MOVE_PX) {   // a drag: no theme change
        s_lpMoved = true;
        if (s_lpArmed) { s_lpArmed = false; toast_hide(); }
        return;
    }
    if (!s_lpArmed && lv_tick_elaps(s_lpT0) >= UI_THEME_HOLD_MS) {
        s_lpArmed = true;
        s_themeHeld = true;
        toast_show("LIFT TO CHANGE THEME", 0);
    }
}

static const char *const THEME_NAMES[THEME_COUNT] = { "PHOSPHOR", "ORB", "AMBER", "MILITARY" };

static void radar_release_cb(lv_event_t *e) {   // still finger lifted after the hold -> next theme
    (void)e;
    if (s_lpArmed && !s_lpMoved) {
        radar::cycleTheme();
        char t[32];
        snprintf(t, sizeof(t), "THEME " SYM_DOT " %s", THEME_NAMES[radar::theme() % THEME_COUNT]);
        toast_show(t, UI_TOAST_MS);
    }
    s_lpArmed = false;
}

static void radar_presslost_cb(lv_event_t *e) {   // scroll stole the press: a swipe, not a hold
    (void)e;
    if (s_lpArmed) toast_hide();
    s_lpArmed = false;
}

static void radar_clicked_cb(lv_event_t *e) {
    (void)e;
    if (s_themeHeld) { s_themeHeld = false; return; }   // the lift after a theme hold is not a tap
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    radar::select(radar::hitTest(p.x, p.y));   // hit -> select; miss -> clear
    refresh_card();
}

// --------------------------------------------------------- status: note and pull-down
// The radar has no top row: a note under the N shows the first problem (no WiFi, old data,
// low battery), and a drag down from the top opens a pull-down with the full status and a
// THEME button.
static bool s_stKnown = false;              // main has reported the status at least once
static bool s_stWifiUp = false, s_stFeedOk = false;
static int  s_stRssi = -127;
static char s_stClock[8] = "--:--";
static char s_stDate[20] = "";
static int  s_battPct = -1;
static bool s_battCharging = false, s_battPresent = false;
static int  s_gpsState = 0, s_gpsSats = 0;
static float s_gpsAltM = NAN;
static char s_feedHost[16] = "-";
static uint32_t s_feedAgeMs = UINT32_MAX;
static lv_point_t s_pullOrigin;             // where the current press on the pull-down began

#define NOTE_Y        46    // status note box top: just under the N (box y 12-38)
#define PULL_SHEET_H  248   // the pull-down sheet covers y 0 .. 247

static void fmt_age(char *b, size_t n, uint32_t ms);

static inline bool batt_known(void) { return s_battPresent && s_battPct >= 0; }
static inline bool batt_low(void) { return batt_known() && s_battPct <= UI_BATT_LOW_PCT && !s_battCharging; }
static int wifi_level(void) {                // lit bars from RSSI (dBm)
    if (!s_stWifiUp)     return 0;
    if (s_stRssi >= -55) return 4;           // excellent
    if (s_stRssi >= -67) return 3;           // good
    if (s_stRssi >= -75) return 2;           // ok
    return 1;                                // weak (connected but marginal)
}
static void set_text(lv_obj_t *l, const char *t) {   // only a real change repaints
    if (l && strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}
static void chip_fit(lv_obj_t *box, lv_obj_t *lbl, const char *t, lv_coord_t y) {   // S chip, centred on x
    set_text(lbl, t);
    const lv_coord_t w = (lv_coord_t)(strlen(t) * UI_S_ADV + 16);
    lv_obj_set_size(box, w, 24);
    lv_obj_set_pos(box, (lv_coord_t)(SCREEN_CX - w / 2), y);
}

// The note under the N: the first problem that applies, or none. Nothing before main's first
// status report (the flags start as "down") and nothing while the idle clock is up.
static void apply_status_note(void) {
    if (!s_noteEmerg) return;
    const char *emerg = nullptr, *warn = nullptr;
    if (s_stKnown && !s_clockUp) {
        if (!s_stWifiUp)      emerg = "NO WIFI";
        else if (!s_stFeedOk) warn  = "DATA OLD";
        else if (batt_low())  emerg = "LOW BATTERY";
    }
    const bool changed = shown(s_noteEmerg) != (emerg != nullptr) || shown(s_noteWarn) != (warn != nullptr) ||
                         (emerg && strcmp(lv_label_get_text(s_noteEmergLbl), emerg) != 0);
    if (emerg) chip_fit(s_noteEmerg, s_noteEmergLbl, emerg, NOTE_Y);
    if (warn)  chip_fit(s_noteWarn, s_noteWarnLbl, warn, NOTE_Y);
    show(s_noteEmerg, emerg != nullptr);
    show(s_noteWarn, warn != nullptr);
    if (changed) send_obstacles();
}

// Status cells fill these slots row by row, leaving out GPS (no module / off) and battery
// (none fitted, or level unknown).
static void pull_layout(void) {
    static const lv_coord_t SX[4] = { 62, 262, 62, 262 }, SY[4] = { 108, 108, 142, 142 };
    int k = 0;
    lv_obj_set_pos(s_pullWifi, SX[k], (lv_coord_t)(SY[k] + 4));     // bars end at cell y + 18
    lv_obj_set_pos(s_pullWifiLbl, (lv_coord_t)(SX[k] + 26), SY[k]);
    ++k;
    const bool gps = s_gpsState > 0;
    show(s_pullGpsLbl, gps);
    if (gps) { lv_obj_set_pos(s_pullGpsLbl, SX[k], SY[k]); ++k; }
    lv_obj_set_pos(s_pullPlane, SX[k], (lv_coord_t)(SY[k] + 4));
    lv_obj_set_pos(s_pullPlanesLbl, (lv_coord_t)(SX[k] + 22), SY[k]);
    ++k;
    const bool batt = batt_known();
    show(s_pullBatt, batt);
    show(s_pullBattLbl, batt);
    show(s_pullBolt, batt && s_battCharging);
    if (batt) {
        lv_obj_set_pos(s_pullBatt, SX[k], (lv_coord_t)(SY[k] + 5));
        lv_obj_set_pos(s_pullBattLbl, (lv_coord_t)(SX[k] + 30), SY[k]);
        lv_obj_set_pos(s_pullBolt, (lv_coord_t)(SX[k] + 30 + text_w(s_pullBattLbl, UI_M_ADV) + 6),
                       (lv_coord_t)(SY[k] + 4));
    }
}

// Everything on the sheet from the stored values and today's palette. No work while closed:
// opening refreshes first. Local colours are written only when they change (LVGL repaints an
// object on every style write, even an equal one); open and the theme hook reset that memory.
static uint32_t s_pullColKey = UINT32_MAX;   // what the WiFi/GPS/battery colours were last set from
static void pull_refresh(void) {
    if (!s_pullOpen) return;
    const UiPalette &p = P();
    set_text(s_pullTime, s_stClock);
    set_text(s_pullDate, s_stDate);

    const int level = wifi_level();
    const uint32_t key = (uint32_t)level | (uint32_t)s_stWifiUp << 3 | (uint32_t)s_stFeedOk << 4 |
                         (uint32_t)(s_gpsState >= 2) << 5 | (uint32_t)batt_low() << 6 |
                         (uint32_t)ui_style_theme() << 7;
    const bool recolour = key != s_pullColKey;
    s_pullColKey = key;
    const lv_color_t wcol = COL(!s_stWifiUp ? p.emerg : (s_stFeedOk ? p.ink : p.warn));
    if (recolour) {
        for (int i = 0; i < 4; ++i) {
            lv_obj_set_style_bg_color(s_pullBars[i], wcol, 0);
            lv_obj_set_style_bg_opa(s_pullBars[i], (i < level) ? LV_OPA_COVER : 45, 0);
        }
        lv_obj_set_style_text_color(s_pullWifiLbl, wcol, 0);
    }
    set_text(s_pullWifiLbl, !s_stWifiUp ? "NO WIFI" : !s_stFeedOk ? "DATA OLD" :
                            level >= 3 ? "WIFI GOOD" : level == 2 ? "WIFI OK" : "WIFI WEAK");
    // (WiFi text colour set above, with the bars)

    if (s_gpsState > 0) {
        char g[16];
        if (s_gpsState >= 2) snprintf(g, sizeof(g), "GPS %d SAT", s_gpsSats);
        else                 snprintf(g, sizeof(g), "GPS SEARCH");
        set_text(s_pullGpsLbl, g);
        if (recolour) lv_obj_set_style_text_color(s_pullGpsLbl, COL(s_gpsState >= 2 ? p.accent : p.warn), 0);
    }

    char t[48];
    const int n = radar::countInRange();
    snprintf(t, sizeof(t), n == 1 ? "%d PLANE" : "%d PLANES", n);
    set_text(s_pullPlanesLbl, t);

    if (batt_known()) {
        const int pct = s_battPct > 100 ? 100 : s_battPct;
        if (recolour) {
            const lv_color_t ink = COL(p.ink);
            lv_obj_set_style_border_color(s_pullBattFrame, ink, 0);
            lv_obj_set_style_bg_color(s_pullBattNub, ink, 0);
            lv_obj_set_style_bg_color(s_pullBattFill, batt_low() ? COL(p.emerg) : ink, 0);
        }
        lv_obj_set_width(s_pullBattFill, (lv_coord_t)(pct * 14 / 100 < 1 ? 1 : pct * 14 / 100));
        snprintf(t, sizeof(t), "%d %%", pct);
        set_text(s_pullBattLbl, t);
    }

    if (strcmp(s_feedHost, "-") == 0) {
        set_text(s_pullData, "NO DATA YET");
    } else {
        char age[16];
        fmt_age(age, sizeof(age), s_feedAgeMs);
        snprintf(t, sizeof(t), "DATA FROM %s " SYM_DOT " %s", s_feedHost, age);
        set_text(s_pullData, t);
    }

    snprintf(t, sizeof(t), "THEME: %s", THEME_NAMES[radar::theme() % THEME_COUNT]);
    chip_fit(s_pullTheme, s_pullThemeLbl, t, 200);
    pull_layout();
}

// The catcher stays shown and only becomes clickable while open; hiding the full-screen
// catcher would repaint the whole screen, hiding the sheet repaints its 248 rows.
static void pull_open(void) {
    if (!s_pull || s_pullOpen || s_clockUp) return;
    if (s_tv && lv_tileview_get_tile_act(s_tv) != s_tileRadar) return;
    s_pullOpen = true;
    card_close_sync();
    s_pullColKey = UINT32_MAX;                // set every colour once
    pull_refresh();
    lv_obj_add_flag(s_pull, LV_OBJ_FLAG_CLICKABLE);
    show(s_pullSheet, true);
    lv_timer_reset(s_pullTimer);
    lv_timer_resume(s_pullTimer);
}

// Closes at once. A press already on the catcher or the THEME chip stays with that object
// (LVGL's press lock, on by default), so its lift never reaches the radar; their handlers
// do nothing once closed. A non-clickable catcher with a hidden sheet takes no new press.
static void pull_close(void) {
    if (!s_pullOpen) return;
    s_pullOpen = false;
    card_close_sync();
    lv_obj_clear_flag(s_pull, LV_OBJ_FLAG_CLICKABLE);
    show(s_pullSheet, false);
    lv_timer_pause(s_pullTimer);
}

void ui_set_pulldown(bool open) {
    if (open) pull_open();
    else      pull_close();
}

static void pull_timer_cb(lv_timer_t *t) { (void)t; pull_close(); }

static void pull_press_cb(lv_event_t *e) {   // also the THEME chip's press (it bubbles)
    (void)e;
    if (!s_pullOpen) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev) lv_indev_get_point(indev, &s_pullOrigin);
    lv_timer_reset(s_pullTimer);
}

static void pull_pressing_cb(lv_event_t *e) {   // a drag up closes it
    (void)e;
    if (!s_pullOpen) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const long up = (long)s_pullOrigin.y - p.y, dx = labs((long)p.x - s_pullOrigin.x);
    if (up >= PULL_MOVE_PX && up > dx) pull_close();
}

static void pull_release_cb(lv_event_t *e) {
    (void)e;
    if (s_pullOpen) lv_timer_reset(s_pullTimer);
}

static void pull_click_cb(lv_event_t *e) {   // a tap that began below the sheet closes it
    (void)e;
    if (s_pullOpen && s_pullOrigin.y >= PULL_SHEET_H) pull_close();
}

static void pull_theme_cb(lv_event_t *e) {   // the theme hook refreshes the chip text
    (void)e;
    if (s_pullOpen) radar::cycleTheme();
}

void ui_set_status(bool wifiUp, bool feedOk, int rssi, const char *clock) {
    s_stKnown = true;
    s_stWifiUp = wifiUp; s_stFeedOk = feedOk; s_stRssi = rssi;
    if (clock) snprintf(s_stClock, sizeof(s_stClock), "%s", clock);
    apply_status_note();
    pull_refresh();
}

void ui_set_battery(int pct, bool charging, bool present) {
    if (pct == s_battPct && charging == s_battCharging && present == s_battPresent) return;
    s_battPct = pct; s_battCharging = charging; s_battPresent = present;
    apply_status_note();
    pull_refresh();
}

void ui_set_date(const char *date) {
    if (!date) return;
    snprintf(s_stDate, sizeof(s_stDate), "%s", date);
    set_text(s_clockDate, s_stDate);
    pull_refresh();
}

static char s_netInfo[160] = "";
static void apply_netinfo(void) {
    if (!s_statsNet) return;
    char t[220];
    snprintf(t, sizeof(t), "%s\n#%06lx v" FW_VERSION "#", s_netInfo, (unsigned long)P().faint);
    lv_label_set_text(s_statsNet, t);
}
void ui_set_netinfo(const char *line) {
    if (!line || strcmp(line, s_netInfo) == 0) return;
    snprintf(s_netInfo, sizeof(s_netInfo), "%s", line);
    apply_netinfo();
}

// Which provider served the last aircraft data. Shown on the Stats view and in the pull-down,
// so users can tell whether they are on airplanes.live (feeder households) or a fallback.
void ui_set_feed_source(const char *host) {
    if (!host || !host[0] || host[0] == '?') {
        snprintf(s_feedHost, sizeof(s_feedHost), "-");
    } else {
        // strip the machine prefixes so the label reads like the project name users know
        if      (strncmp(host, "api.",      4) == 0) host += 4;
        else if (strncmp(host, "opendata.", 9) == 0) host += 9;
        snprintf(s_feedHost, sizeof(s_feedHost), "%.14s", host);
    }
    pull_refresh();
}

static void build_stats(void);
void ui_set_feed_age(uint32_t ms) {
    s_feedAgeMs = ms;
    if (s_tv && lv_tileview_get_tile_act(s_tv) == s_tileStats) build_stats();   // ticks while visible
    pull_refresh();
}

// GPS: state 0 = off / no module (no line), 1 = acquiring (warn), 2 = fix (accent).
void ui_set_gps(int state, int sats, float altM) {
    const bool same = state == s_gpsState && sats == s_gpsSats &&
                      (altM == s_gpsAltM || (altM != altM && s_gpsAltM != s_gpsAltM));
    s_gpsState = state; s_gpsSats = sats; s_gpsAltM = altM;
    if (same) return;
    pull_refresh();
}

// ------------------------------------------------------------- list / stats
// The contact list recycles a small pool of row labels: a spacer as tall as every row gives
// the list its scroll range, and the pool rows are moved to whichever rows are in view.
// One label per aircraft (the old way) cost ~0.23 KB of LVGL's 64 KB pool each and left
// only ~11 KB with the full 60-aircraft feed; the pool costs the same at 9 or 60 aircraft.
// A row's monospace text holds every column; the band square, the distance and emergency
// rows are coloured with label recolour codes, rebuilt on every refresh (so they follow the
// theme). Rows are re-texted in place: deleting the row under a pressing finger ate taps.
#define LIST_ROW_H 34
#define LIST_POOL  11                      // the 298-px viewport shows 8.8 rows; +2 for partial rows
static lv_obj_t *s_listSpacer = nullptr;
static lv_obj_t *s_listRow[LIST_POOL];
static int s_listRowIdx[LIST_POOL];        // which radar::info() index each pooled row shows (-1 none)
static lv_style_t s_rowStyle;
static bool s_rowStyleInit = false;

// Taps land on the list container (rows are not clickable); map the point to a row.
// The aircraft is picked at touch-down, by hex: build_list rewrites rows in place on every
// poll, so mapping the row only at release could open a different plane if a poll landed
// between press and release.
static char s_listPressHex[sizeof(AcInfo::hex)];

static void list_press_cb(lv_event_t *e) {
    (void)e;
    s_listPressHex[0] = '\0';
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t la, a;
    lv_obj_update_layout(s_list);                   // rows moved by list_place_rows() this frame
    lv_obj_get_coords(s_list, &la);
    if (p.x < la.x1 || p.x > la.x2) return;
    for (int k = 0; k < LIST_POOL; ++k) {
        if (s_listRowIdx[k] < 0 || !shown(s_listRow[k])) continue;
        lv_obj_get_coords(s_listRow[k], &a);
        if (p.y >= a.y1 && p.y <= a.y2) {
            AcInfo in;
            if (radar::info(s_listRowIdx[k], in)) snprintf(s_listPressHex, sizeof(s_listPressHex), "%s", in.hex);
            return;
        }
    }
}

static void list_tap_cb(lv_event_t *e) {
    (void)e;
    if (!s_listPressHex[0] || !radar::selectHex(s_listPressHex)) return;   // left the feed mid-press
    refresh_card();
    lv_obj_set_tile_id(s_tv, 0, 0, LV_ANIM_ON);   // jump back to the radar
}

static uint32_t band_hex(float altFt, bool gnd) {
    if (gnd) return UI_ALT_GROUND;
    return UI_ALT_BANDS[altFt < 3000 ? 0 : altFt < 10000 ? 1 : altFt < 20000 ? 2 : altFt < 30000 ? 3 : 4];
}

static void list_fill_row(lv_obj_t *row, int idx) {
    AcInfo in;
    if (!radar::info(idx, in)) return;
    char altS[12], dS[12], call[12], txt[96];
    fmt_alt_num(altS, sizeof(altS), in.altFt, in.onGround);
    fmt_dist4(dS, sizeof(dS), in.distKm);
    snprintf(call, sizeof(call), "%s", in.call[0] ? in.call : in.hex);   // callsigns never hold '#'
    if (in.emergency)
        snprintf(txt, sizeof(txt), "#%06lx " SYM_SQUARE " %-8.8s  %5.5s  %4.4s#",
                 (unsigned long)P().emerg, call, altS, dS);
    else
        snprintf(txt, sizeof(txt), "#%06lx " SYM_SQUARE "# %-8.8s  %5.5s  #%06lx %4.4s#",
                 (unsigned long)band_hex(in.altFt, in.onGround), call, altS,
                 (unsigned long)P().soft, dS);
    if (strcmp(lv_label_get_text(row), txt) != 0) lv_label_set_text(row, txt);
}

// Put the pool rows on the rows in view (after a scroll or a data change).
static void list_place_rows(void) {
    if (!s_list) return;
    const int n = radar::count();
    int first = (int)(lv_obj_get_scroll_y(s_list) / LIST_ROW_H);
    if (first < 0) first = 0;
    for (int k = 0; k < LIST_POOL; ++k) {
        const int idx = first + k;
        if (idx >= n) { show(s_listRow[k], false); s_listRowIdx[k] = -1; continue; }
        lv_obj_set_y(s_listRow[k], (lv_coord_t)(idx * LIST_ROW_H));
        list_fill_row(s_listRow[k], idx);
        show(s_listRow[k], true);
        s_listRowIdx[k] = idx;
    }
}

static void list_scroll_cb(lv_event_t *e) {
    (void)e;
    static int lastFirst = -1;
    const int first = (int)(lv_obj_get_scroll_y(s_list) / LIST_ROW_H);
    if (first == lastFirst) return;                 // re-text only when a row boundary is crossed
    lastFirst = first;
    list_place_rows();
}

static void build_list(void) {
    if (!s_list) return;
    const int n = radar::count();
    char t[32];
    snprintf(t, sizeof(t), "AIRCRAFT  %d", n);
    set_text(s_listTitle, t);
    char h[12];
    snprintf(h, sizeof(h), "ALT %s", alt_unit_up());
    set_text(s_listHdrAlt, h);
    set_text(s_listHdrDist, dist_unit_up());
    lv_obj_set_height(s_listSpacer, (lv_coord_t)(n * LIST_ROW_H));
    lv_obj_update_layout(s_list);                   // the scroll range follows the spacer
    const lv_coord_t maxY = (lv_coord_t)LV_MAX(0, n * LIST_ROW_H - lv_obj_get_height(s_list));
    // Only an idle list: mid-drag or mid-bounce LVGL overshoots on purpose and re-clamps itself.
    if (!lv_obj_is_scrolling(s_list) && lv_obj_get_scroll_y(s_list) > maxY)
        lv_obj_scroll_to_y(s_list, maxY, LV_ANIM_OFF);
    list_place_rows();
}

static void fmt_age(char *b, size_t n, uint32_t ms) {
    if (ms == UINT32_MAX)  snprintf(b, n, "-");
    else if (ms < 120000UL) snprintf(b, n, "%lu s ago", (unsigned long)(ms / 1000UL));
    else                    snprintf(b, n, "%lu min ago", (unsigned long)(ms / 60000UL));
}

static void build_stats(void) {
    if (!s_statsVals) return;
    const int n = radar::count();
    int emg = 0;
    float nearest = 1e9f, highest = -1e9f;
    char nearestCall[12] = "-";
    for (int i = 0; i < n; ++i) {
        AcInfo in;
        radar::info(i, in);
        if (in.emergency) emg++;
        if (in.distKm < nearest) { nearest = in.distKm; snprintf(nearestCall, sizeof(nearestCall), "%s", in.call[0] ? in.call : in.hex); }
        if (!in.onGround && in.altFt > highest) highest = in.altFt;
    }
    const UiPalette &p = P();
    char altH[16], age[24], emgS[24], nearD[32], gps[48] = "";
    fmt_alt(altH, sizeof(altH), (highest > -1e8f) ? highest : 0.0f, false);
    fmt_age(age, sizeof(age), s_feedAgeMs);
    const bool stale = s_feedAgeMs != UINT32_MAX && s_feedAgeMs >= (uint32_t)UI_FEED_STALE_S * 1000UL;
    if (emg > 0) snprintf(emgS, sizeof(emgS), "#%06lx %d#", (unsigned long)p.emerg, emg);
    else         snprintf(emgS, sizeof(emgS), "0");
    snprintf(nearD, sizeof(nearD), "#%06lx %.1f %s#", (unsigned long)p.soft, dist_val(n ? nearest : 0.0f), dist_unit());
    if (s_gpsState > 0) {
        char altS[16] = "";
        if (s_gpsState >= 2 && s_gpsAltM == s_gpsAltM) {
            if (s_units == 1) snprintf(altS, sizeof(altS), "  %.0f M", s_gpsAltM);
            else              snprintf(altS, sizeof(altS), "  %.0f FT", s_gpsAltM * 3.28084f);
        }
        if (s_gpsState >= 2) snprintf(gps, sizeof(gps), "\n#%06lx FIX %d%s#", (unsigned long)p.accent, s_gpsSats, altS);
        else                 snprintf(gps, sizeof(gps), "\n#%06lx SEARCH %d#", (unsigned long)p.warn, s_gpsSats);
    }
    char st[320];
    snprintf(st, sizeof(st), "%d\n%s\n%s\n%s\n%s\n%.0f %s\n%s\n#%06lx %s#%s",
             n, emgS, n ? nearestCall : "-", nearD, altH, dist_val(s_rangeKm), dist_unit(), s_feedHost,
             (unsigned long)(stale ? p.warn : p.accentHi), age, gps);
    set_text(s_statsVals, st);
    set_text(s_statsNames, s_gpsState > 0
        ? "Aircraft\nEmergency\nNearest\n\nHighest\nRange\nFeed\nUpdated\nGPS"
        : "Aircraft\nEmergency\nNearest\n\nHighest\nRange\nFeed\nUpdated");
}

// -------------------------------------------------------------------- weather
static void set_wx_mode_chips(void) {
    static int lastMode = -1, lastTheme = -1;   // recolour only on a real (mode, theme) change
    const int theme = ui_style_theme();
    if ((int)s_weatherMode == lastMode && theme == lastTheme) return;
    lastMode = (int)s_weatherMode;
    lastTheme = theme;
    const UiPalette &p = P();
    for (int i = 0; i < 3; ++i) {
        if (s_wxRings[i]) lv_obj_set_style_border_color(s_wxRings[i], COL(p.accent), 0);
        if (!s_wxMode[i]) continue;
        const bool on = i == (int)s_weatherMode;
        lv_obj_set_style_border_color(s_wxMode[i], COL(on ? p.accent : p.faint), 0);
        lv_obj_set_style_text_color(s_wxModeLbl[i], COL(on ? p.accentHi : p.soft), 0);
    }
}

// Place an icon + text pair centred on x 233 (the forecast's current conditions).
static void centre_icon_text(lv_obj_t *icon, lv_obj_t *txt, int adv, lv_coord_t y) {
    const int tw = text_w(txt, adv), gw = 32 + 10 + tw;
    const lv_coord_t x0 = (lv_coord_t)(SCREEN_CX - gw / 2);
    lv_obj_set_pos(icon, x0, (lv_coord_t)(y + 1));
    lv_obj_set_pos(txt, (lv_coord_t)(x0 + 42), y);
}

static void build_weather(void) {
    if (!s_wxFooter) return;
    WeatherSnapshot w;
    const bool have = weather_get(w);
    if (!have) {
        set_text(s_wxFooter, "WEATHER DATA PENDING");
        set_text(s_wxMeta, "");
        set_text(s_fcCurrent, "--");
        set_text(s_fcCondition, "Waiting for data");
        lv_img_set_src(s_fcIcon, &ui_icon_wx[UI_WX_CLOUDY]);
        centre_icon_text(s_fcIcon, s_fcCurrent, UI_L_ADV, 90);
        for (int i = 0; i < 3; ++i) set_text(s_fcMetricValue[i], "-");
        set_text(s_fcUpdated, "");
    } else {
        char footer[64];
        snprintf(footer, sizeof(footer), "%.0f" SYM_DEG "%s  %s", weather_temp(w.tempC),
                 weather_temp_unit(), weather_condition(w.code));
        set_text(s_wxFooter, footer);
        char wxmeta[64];
        snprintf(wxmeta, sizeof(wxmeta), "WIND %s %.0f %s   HUM %d%%",
                 cardinal((float)w.windDeg), weather_wind(w.windKmh), weather_wind_unit(), w.humidity);
        set_text(s_wxMeta, wxmeta);

        char current[24];
        snprintf(current, sizeof(current), "%.0f" SYM_DEG "%s", weather_temp(w.tempC), weather_temp_unit());
        set_text(s_fcCurrent, current);
        lv_img_set_src(s_fcIcon, &ui_icon_wx[ui_wx_icon(w.code)]);
        centre_icon_text(s_fcIcon, s_fcCurrent, UI_L_ADV, 90);
        set_text(s_fcCondition, weather_condition(w.code));
        char feels[24]; snprintf(feels, sizeof(feels), "%.0f" SYM_DEG "%s", weather_temp(w.feelsC), weather_temp_unit());
        set_text(s_fcMetricValue[0], feels);
        char hum[16]; snprintf(hum, sizeof(hum), "%d%%", w.humidity);
        set_text(s_fcMetricValue[1], hum);
        char wind[28]; snprintf(wind, sizeof(wind), "%s %.0f %s", cardinal((float)w.windDeg),
                                weather_wind(w.windKmh), weather_wind_unit());
        set_text(s_fcMetricValue[2], wind);
        char updated[24]; snprintf(updated, sizeof(updated), "UPDATED %s", w.updated);
        set_text(s_fcUpdated, updated);
    }
    for (int col = 0; col < 3; ++col) {
        const int i = col + 1;
        if (have && i < w.dayCount) {
            set_text(s_fcDay[col], weather_day_name(w.days[i].date));
            lv_img_set_src(s_fcDayIcon[col], &ui_icon_wx[ui_wx_icon(w.days[i].code)]);
            show(s_fcDayIcon[col], s_weatherMode == WEATHER_FORECAST);
            char temps[28];
            snprintf(temps, sizeof(temps), "%.0f/%.0f" SYM_DEG "%s",
                     weather_temp(w.days[i].tempMaxC), weather_temp(w.days[i].tempMinC), weather_temp_unit());
            set_text(s_fcDayTemp[col], temps);
            char chance[20]; snprintf(chance, sizeof(chance), "RAIN %d%%", w.days[i].rainChance);
            set_text(s_fcDayRain[col], chance);
        } else {
            set_text(s_fcDay[col], "-");
            show(s_fcDayIcon[col], false);
            set_text(s_fcDayTemp[col], "");
            set_text(s_fcDayRain[col], "");
        }
    }

    const uint16_t *radarPixels = nullptr, *cloudPixels = nullptr;
    uint32_t frameTime = 0, version = 0;
    double rlat = 0, rlon = 0;
    const bool cloudMode = s_weatherMode == WEATHER_CLOUDS;
    const bool forecastMode = s_weatherMode == WEATHER_FORECAST;
    bool haveImage = false;
    if (cloudMode)
        haveImage = cloud_image_front(&cloudPixels, &frameTime, &rlat, &rlon, &version);
    else
        haveImage = wx_radar_front(&radarPixels, &frameTime, &rlat, &rlon, &version);
    const uint16_t *pixels = cloudMode ? cloudPixels : radarPixels;
    if (haveImage && pixels && s_wxCanvas) {
        // Re-point the canvas only when the front buffer or its version changed: a
        // re-point invalidates the whole 360x360 image even for identical pixels.
        static const uint16_t *lastPix = nullptr;
        static uint32_t lastVer = 0;
        if (pixels != lastPix || version != lastVer) {
            lastPix = pixels;
            lastVer = version;
            lv_canvas_set_buffer(s_wxCanvas, (void *)pixels, WX_RADAR_SIZE, WX_RADAR_SIZE, LV_IMG_CF_TRUE_COLOR);
            lv_obj_invalidate(s_wxCanvas);
        }
        char iata[4]; float d = 0, b = 0;
        if (airports_nearest_iata(rlat, rlon, 200.0f, iata, &d, &b)) {
            char apt[64];
            snprintf(apt, sizeof(apt), "%s " SYM_DOT " %.0f %s %s", iata, dist_val(d), dist_unit_up(), cardinal(b));
            set_text(s_wxAirport, apt);
        } else set_text(s_wxAirport, "RADAR CENTRE");
        char stamp[6] = "--:--";
        time_t ft = (time_t)frameTime; struct tm ti;
        if (frameTime && localtime_r(&ft, &ti)) snprintf(stamp, sizeof(stamp), "%02d:%02d", ti.tm_hour, ti.tm_min);
        char attr[64];
        snprintf(attr, sizeof(attr), cloudMode ? "SAT %s  |  EUMETSAT" : "RADAR %s  |  RAINVIEWER", stamp);
        set_text(s_wxAttrib, attr);
    } else {
        set_text(s_wxAirport, "RADAR CENTRE");
        set_text(s_wxAttrib, cloudMode ? "WAITING FOR SATELLITE DATA" : "WAITING FOR RADAR DATA");
        set_text(s_wxStatus, cloudMode ? "ACQUIRING SAT CLOUDS..." : "ACQUIRING WX RADAR...");
    }

    lv_obj_t *forecastObjs[] = {
        s_fcIcon, s_fcCurrent, s_fcCondition, s_fcUpdated,
        s_fcMetricName[0], s_fcMetricName[1], s_fcMetricName[2],
        s_fcMetricValue[0], s_fcMetricValue[1], s_fcMetricValue[2],
        s_fcDay[0], s_fcDay[1], s_fcDay[2],
        s_fcDayTemp[0], s_fcDayTemp[1], s_fcDayTemp[2],
        s_fcDayRain[0], s_fcDayRain[1], s_fcDayRain[2]
    };
    lv_obj_t *radarObjs[] = { s_wxAirport, s_wxFooter, s_wxMeta, s_wxAttrib, s_wxNorth, s_wxCenter,
                              s_wxRange, s_wxRings[0], s_wxRings[1], s_wxRings[2] };
    for (lv_obj_t *o : forecastObjs) show(o, forecastMode);
    for (int col = 0; col < 3; ++col)
        if (!forecastMode) show(s_fcDayIcon[col], false);
    for (lv_obj_t *o : radarObjs) show(o, !forecastMode);
    show(s_wxCanvas, !forecastMode && haveImage);
    show(s_wxStatus, !forecastMode && !haveImage);
    // today's image radii (75 km radar, 200 km clouds), in the preset unit
    char rng[16];
    snprintf(rng, sizeof(rng), "%.0f %s", dist_val(cloudMode ? 200.0f : 75.0f), dist_unit_up());
    set_text(s_wxRange, rng);
    set_text(s_weatherTitle,
        s_weatherMode == WEATHER_RADAR ? "WX RADAR" :
        s_weatherMode == WEATHER_CLOUDS ? "SAT CLOUDS" : "WEATHER");
    set_wx_mode_chips();
}

// Registered once, on the whole weather panel (see ui_create) -- the mode chips and the
// image are deliberately non-clickable so every tap funnels here. The debounce swallows
// repeats from a held press.
static void weather_mode_cb(lv_event_t *) {
    static uint32_t last = 0;
    const uint32_t now = lv_tick_get();
    if (now - last < 250) return;
    last = now;
    s_weatherMode = (WeatherViewMode)(((int)s_weatherMode + 1) % 3);
    build_weather();
}

void ui_set_weather_forecast(bool forecast) {
    s_weatherMode = forecast ? WEATHER_FORECAST : WEATHER_RADAR;
    build_weather();
}

static void (*s_viewChangedCb)(int) = nullptr;
void ui_set_view_changed_cb(void (*cb)(int idx)) { s_viewChangedCb = cb; }

// ---------------------------------------------------------------- idle clock
// radar_view's scene steps. From the first one the status note, the zoom chip, the card, the
// page dots and the pull-down make way for the dial, and a transparent layer on top takes the touch that wakes the
// radar, so that touch does not also select a plane, swipe a view or change the theme. LVGL
// counts the press as activity, which is what makes main.cpp send the clock away.
static lv_obj_t *s_clockCatcher = nullptr;

static void clock_stage_cb(int stage) {
    s_clockStage = stage;
    show(s_clockDate, stage >= 2);
    const bool up = stage != 0;
    if (up == s_clockUp) return;
    s_clockUp = up;
    show(s_dots, !up);
    if (up) pull_close();
    apply_status_note();                    // no note over the dial
    refresh_card();                         // hides the card (and shows the zoom chip) as needed
    if (!s_clockCatcher) return;
    if (up) lv_obj_clear_flag(s_clockCatcher, LV_OBJ_FLAG_HIDDEN);
    else if (!lv_obj_has_state(s_clockCatcher, LV_STATE_PRESSED))
        lv_obj_add_flag(s_clockCatcher, LV_OBJ_FLAG_HIDDEN);   // still pressed: hidden on release
}

static void clock_catcher_release_cb(lv_event_t *e) {
    if (!s_clockUp) lv_obj_add_flag(lv_event_get_target(e), LV_OBJ_FLAG_HIDDEN);
}

// ------------------------------------------------- obstacles, masks, views
// Areas the plane labels must keep clear of, in scope coordinates (the radar tile's own).
// Sent only when they change: every call re-places the labels and repaints the aircraft layer.
static void send_obstacles(void) {
    if (!s_tileRadar) return;
    lv_obj_update_layout(s_tileRadar);               // coords of objects moved since the last frame
    if (s_dots) lv_obj_update_layout(s_dots);
    lv_area_t tile;                                  // off-screen while another page is shown
    lv_obj_get_coords(s_tileRadar, &tile);
    LlRect r[8];
    int n = 0;
    auto add_box = [&](lv_obj_t *o, bool onTile) {   // the page dots live on lv_layer_top()
        if (!o || n >= 8) return;
        lv_area_t a;
        lv_obj_get_coords(o, &a);
        const lv_coord_t dx = onTile ? tile.x1 : 0, dy = onTile ? tile.y1 : 0;
        r[n++] = { (int16_t)(a.x1 - dx), (int16_t)(a.y1 - dy), (int16_t)(a.x2 - dx), (int16_t)(a.y2 - dy) };
    };
    auto add_obj = [&](lv_obj_t *o, bool onTile) { if (shown(o)) add_box(o, onTile); };
    add_obj(s_noteEmerg, true);                      // the status note, when one shows
    add_obj(s_noteWarn, true);
    r[n++] = { 219, 12, 247, 38 };                   // the N
    add_obj(s_zoomBtn, true);
    add_obj(s_dots, false);
    add_obj(s_card, true);
    if (shown(s_card)) add_box(s_cardClose, true);   // its X, also while the open pull-down hides it
    add_obj(s_photo, true);
    add_obj(s_photoCredit, true);
    static LlRect last[8];
    static int lastN = -1;
    if (n == lastN && memcmp(r, last, sizeof(LlRect) * (size_t)n) == 0) return;
    memcpy(last, r, sizeof(LlRect) * (size_t)n);
    lastN = n;
    radar::setLabelObstacles(r, n);
}

// Sweep compositor hooks (polled by display.cpp every loop pass).
bool ui_sweep_visible(void) {
    // lv_obj_is_scrolling() sees a finger drag only: without the anim check the
    // compositor keeps drawing the sweep over the radar while an animated page
    // change still slides it in.
    return s_tv && lv_tileview_get_tile_act(s_tv) == s_tileRadar && !lv_obj_is_scrolling(s_tv) &&
           lv_anim_get(s_tv, NULL) == NULL;
}

int ui_sweep_masks(int16_t out[][5], int max) {
    lv_obj_t *objs[] = { s_card, s_cardClose, s_photo, s_photoCredit, s_zoomBtn, s_dots, s_toast,
                         s_noteEmerg, s_noteWarn, s_pullSheet };   // the notes never show together
    int n = 0;
    for (lv_obj_t *o : objs) {
        if (n >= max || !shown(o)) continue;
        lv_area_t a;
        lv_obj_get_coords(o, &a);
        out[n][0] = a.x1; out[n][1] = a.y1; out[n][2] = a.x2; out[n][3] = a.y2;
        out[n][4] = lv_obj_get_style_radius(o, LV_PART_MAIN);
        n++;
    }
    return n;
}

static int active_view_index(void) {
    if (!s_tv) return 0;
    lv_obj_t *act = lv_tileview_get_tile_act(s_tv);
    if (act == s_tileList)    return 1;
    if (act == s_tileStats)   return 2;
    if (act == s_tileWeather) return 3;
    return 0;
}

static void update_dots(void) {
    const int a = active_view_index();
    for (int i = 0; i < 4; ++i) {
        if (!s_dot[i]) continue;
        if (i == a) lv_obj_add_state(s_dot[i], LV_STATE_CHECKED);
        else        lv_obj_clear_state(s_dot[i], LV_STATE_CHECKED);
    }
}

// Rebuild whichever of list/stats/weather is currently on screen (on poll and on swipe).
static void refresh_active_tile(void) {
    if (!s_tv) return;
    lv_obj_t *act = lv_tileview_get_tile_act(s_tv);
    if (act == s_tileList)  build_list();
    else if (act == s_tileStats) build_stats();
    else if (act == s_tileWeather) build_weather();
}

void ui_on_data_updated(void) {
    refresh_card();
    pull_refresh();          // the plane count, when the pull-down is open
    refresh_active_tile();   // only the visible tile pays the rebuild cost
}

// The round panels behind list/stats/weather (make_round_panel): drawn from the palette, not
// from a style, so a theme change has to repaint them itself.
static lv_obj_t *s_panels[3];
static int       s_panelN = 0;

// Colours that live in labels' text (recolour codes) or in local styles follow a theme
// change here. The first call comes from radar::init(), before most objects exist.
// Only the page on screen is rebuilt (the others rebuild when swiped to): building every
// page at once spikes LVGL's pool for nothing.
static void theme_hook(int theme) {
    (void)theme;
    if (s_rowStyleInit) {
        lv_style_set_border_color(&s_rowStyle, COL(P().faint));
        lv_style_set_text_color(&s_rowStyle, COL(P().ink));
        lv_obj_report_style_change(&s_rowStyle);
    }
    for (int i = 0; i < s_panelN; ++i) lv_obj_invalidate(s_panels[i]);
    pull_refresh();          // its WiFi, GPS and battery colours and the THEME chip text
    apply_netinfo();
    if (s_card && shown(s_card)) refresh_card();
    refresh_active_tile();
    if (s_offRing) lv_obj_set_style_arc_color(s_offRing, COL(P().emerg), LV_PART_INDICATOR);   // the ring follows the theme
}

void ui_show_view(int idx) {
    if (idx != 0) pull_close();
    if (s_tv && idx >= 0 && idx <= 3) lv_obj_set_tile_id(s_tv, (uint32_t)idx, 0, LV_ANIM_OFF);
    update_dots();
}

// GET /perf?bench=1 (perf.cpp): an animated page change, as a swipe ends with, and whether
// the tileview is still moving (lv_obj_is_scrolling() sees finger scrolls only, not anims).
void ui_bench_page(int idx) {
    if (idx != 0) pull_close();
    if (s_tv && idx >= 0 && idx <= 3) lv_obj_set_tile_id(s_tv, (uint32_t)idx, 0, LV_ANIM_ON);
}
bool ui_bench_moving(void) { return s_tv && (lv_anim_get(s_tv, nullptr) || lv_obj_is_scrolling(s_tv)); }
int  ui_bench_view(void) { return active_view_index(); }

// ------------------------------------------------------------------- splash
static void splash_fade_cb(void *obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0); }
static void splash_del_cb(lv_anim_t *a) { lv_obj_del((lv_obj_t *)a->var); }

static void splash_dismiss_cb(lv_timer_t *t) {
    lv_obj_t *cont = (lv_obj_t *)t->user_data;
    lv_timer_del(t);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cont);
    lv_anim_set_exec_cb(&a, splash_fade_cb);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_time(&a, 600);
    lv_anim_set_ready_cb(&a, splash_del_cb);
    lv_anim_start(&a);
}

// Built at boot with the default theme; main.cpp applies the saved theme right after, so
// every colour here comes from a shared style and follows it.
void ui_splash_show(void) {
    lv_obj_t *cont = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, SCREEN_W, SCREEN_H);
    lv_obj_center(cont);
    lv_obj_set_style_bg_color(cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    // concentric rings
    const lv_coord_t dia[3] = { 210, 142, 78 };
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *r = lv_obj_create(cont);
        lv_obj_remove_style_all(r);
        lv_obj_add_style(r, &UI_ST_RING, 0);
        lv_obj_set_size(r, dia[i], dia[i]);
        lv_obj_align(r, LV_ALIGN_CENTER, 0, -8);
        lv_obj_set_style_border_opa(r, (lv_opa_t)(90 + 35 * i), 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    }
    // rotating sweep
    lv_obj_t *sweep = lv_spinner_create(cont, 1400, 55);
    lv_obj_remove_style_all(sweep);
    lv_obj_add_style(sweep, &UI_ST_ARC_HI, LV_PART_INDICATOR);
    lv_obj_set_size(sweep, 210, 210);
    lv_obj_align(sweep, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_style_arc_opa(sweep, 0, LV_PART_MAIN);

    mk_label_c(cont, UI_FONT_L, &UI_ST_ACCENT_HI, "CAPSULE RADAR", 334);
    mk_label_c(cont, UI_FONT_S, &UI_ST_SOFT, "LIVE ADS-B RADAR", 374);

    lv_timer_t *t = lv_timer_create(splash_dismiss_cb, 2200, cont);   // hold, then fade out
    lv_timer_set_repeat_count(t, 1);
}

// ------------------------------------------------------------------- building
// The full-screen round background of list/stats/weather: the panel colour (or the object's
// local bg colour, if one is set) inside a circle that fills the object, a UI_FRAME_W ring of
// the frame colour at its edge, black outside.
// Content goes on the tile, in screen coordinates, above this panel; the panel stays the
// click target for the weather view.
// Drawn row by row straight into LVGL's draw buffer. As a style (radius LV_RADIUS_CIRCLE and a
// border) every pixel of the 462 px square went through two circle masks: ~20 ms per full
// repaint on the device, so on every frame of a page swipe. Only the ring's edges need a
// per-pixel distance; the rest of each row is two plain fills.

static void round_panel_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    const lv_area_t &a = obj->coords;
    if (lv_event_get_code(e) == LV_EVENT_COVER_CHECK) {          // every pixel of the box is drawn
        lv_cover_check_info_t *info = (lv_cover_check_info_t *)lv_event_get_param(e);
        if (info->res != LV_COVER_RES_MASKED)
            info->res = _lv_area_is_in(info->area, &a, 0) ? LV_COVER_RES_COVER : LV_COVER_RES_NOT_COVER;
        return;
    }
    lv_draw_ctx_t *d = lv_event_get_draw_ctx(e);
    lv_area_t clip;
    if (!_lv_area_intersect(&clip, &a, d->clip_area)) return;
    lv_style_value_t local;
    const lv_color_t black = lv_color_black(), frame = COL(P().faint);
    const lv_color_t panel = lv_obj_get_local_style_prop(obj, LV_STYLE_BG_COLOR, &local, 0) == LV_RES_OK
                             ? local.color : COL(P().panel);
    const float cx = (a.x1 + a.x2) * 0.5f, cy = (a.y1 + a.y2) * 0.5f;
    const float ro = lv_area_get_width(&a) * 0.5f, ri = ro - UI_FRAME_W;   // ring between ri and ro
    const float out2 = (ro + 0.5f) * (ro + 0.5f), in2 = (ri - 0.5f) * (ri - 0.5f);
    const lv_coord_t bw = lv_area_get_width(d->buf_area);
    for (lv_coord_t y = clip.y1; y <= clip.y2; ++y) {
        lv_color_t *row = (lv_color_t *)d->buf + (size_t)(y - d->buf_area->y1) * bw;
        const float dy = (float)y - cy, dy2 = dy * dy;
        // columns: black | edge (per pixel) | panel | edge | black
        const float xo = dy2 < out2 ? sqrtf(out2 - dy2) : -1.0f;
        const float xi = dy2 < in2 ? sqrtf(in2 - dy2) : -1.0f;
        const int aL = xo < 0 ? clip.x2 + 1 : (int)ceilf(cx - xo), aR = xo < 0 ? clip.x2 : (int)floorf(cx + xo);
        const int pL = xi < 0 ? aR + 1 : (int)ceilf(cx - xi), pR = xi < 0 ? aR : (int)floorf(cx + xi);
        for (int x = clip.x1; x <= clip.x2;) {
            lv_color_t *px = row + (x - d->buf_area->x1);
            if (x < aL || x > aR) {                                // outside the circle
                const int e = x < aL ? LV_MIN(aL - 1, clip.x2) : clip.x2;
                lv_color_fill(px, black, (uint32_t)(e - x + 1));
                x = e + 1;
            } else if (x >= pL && x <= pR) {                       // inside the ring
                const int e = LV_MIN(pR, clip.x2);
                lv_color_fill(px, panel, (uint32_t)(e - x + 1));
                x = e + 1;
            } else {                                               // on the ring's edges
                const float dx = (float)x - cx, dist = sqrtf(dx * dx + dy2);
                const float edge = LV_CLAMP(0.0f, ro + 0.5f - dist, 1.0f);       // outer edge
                const float ring = LV_CLAMP(0.0f, dist - (ri - 0.5f), 1.0f);     // inner edge
                const lv_color_t c = lv_color_mix(frame, panel, (lv_opa_t)(ring * 255.0f + 0.5f));
                *px = lv_color_mix(c, black, (lv_opa_t)(edge * 255.0f + 0.5f));
                ++x;
            }
        }
    }
}

static lv_obj_t *make_round_panel(lv_obj_t *tile) {
    lv_obj_t *p = lv_obj_create(tile);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, 462, 462);
    lv_obj_center(p);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p, round_panel_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(p, round_panel_cb, LV_EVENT_COVER_CHECK, nullptr);
    if (s_panelN < 3) s_panels[s_panelN++] = p;
    return p;
}

static void card_close_cb(lv_event_t *e) {   // the card's X: as a tap on empty scope
    (void)e;
    radar::select(-1);
    refresh_card();
}

static void build_card(void) {
    s_card = mk_box(s_tileRadar, &UI_ST_SURFACE, CARD_X, card_bottom_top(), CARD_W, card_h());
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_CLICKABLE);   // consume taps (don't deselect)
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);

    s_cardTitle = mk_label(s_card, UI_FONT_M, &UI_ST_INK, "");
    lv_obj_set_pos(s_cardTitle, 12, 10);
    s_cardChip = mk_box(s_card, &UI_ST_CHIP_EMERG, 150, 9, 132, s_bigText ? 30 : 25);
    s_cardChipLbl = mk_label(s_cardChip, s_bigText ? UI_FONT_M : UI_FONT_S, nullptr, "");
    lv_obj_center(s_cardChipLbl);
    lv_obj_add_flag(s_cardChip, LV_OBJ_FLAG_HIDDEN);

    s_cardCap = mk_label(s_card, UI_FONT_S, &UI_ST_SOFT, "");
    lv_obj_set_pos(s_cardCap, 12, 38);
    s_cardVal = mk_label(s_card, UI_FONT_M, &UI_ST_INK, "");
    lv_obj_set_pos(s_cardVal, 12, 51);

    const lv_font_t *small = s_bigText ? UI_FONT_M : UI_FONT_S;
    s_cardDet = mk_label(s_card, small, &UI_ST_SOFT, "");
    lv_obj_set_width(s_cardDet, 270);
    lv_label_set_long_mode(s_cardDet, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_cardDet, 12, 79);
    static lv_point_t divPts[2];
    s_cardDiv = mk_divider(s_card, 12, 282, s_bigText ? 106 : 95, divPts);
    s_cardRoute = mk_label(s_card, small, &UI_ST_ACCENT_HI, "");
    lv_obj_set_width(s_cardRoute, 270);
    lv_label_set_long_mode(s_cardRoute, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_cardRoute, 12, s_bigText ? 113 : 102);

    // The X closes the card (a tap elsewhere on the card does nothing). It sits outside the
    // card, right edges flush (card_close_y() picks the side), on the radar tile, so its tap
    // area (grown by UI_CARD_CLOSE_HIT) is not clipped to the card. Built right after the
    // card: LVGL hit-tests siblings from the last one, so it wins where that area overlaps
    // the card's edge.
    const lv_coord_t cs = card_close_size();
    s_cardClose = mk_box(s_tileRadar, &UI_ST_SURFACE, (lv_coord_t)(CARD_X + CARD_W - cs),
                         card_close_y(card_bottom_top()), cs, cs);
    lv_obj_add_flag(s_cardClose, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_cardClose, LV_OBJ_FLAG_SCROLL_CHAIN);   // a small drift must not start a page swipe
    lv_obj_set_ext_click_area(s_cardClose, UI_CARD_CLOSE_HIT);
    lv_obj_add_event_cb(s_cardClose, card_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(mk_img(s_cardClose, &UI_ST_IMG_HI, &ui_icon_close));

    // aircraft photo + credit, floating above the card (hidden until one loads)
    s_photo = lv_canvas_create(s_tileRadar);
    lv_obj_remove_style_all(s_photo);
    lv_obj_add_style(s_photo, &UI_ST_SURFACE, 0);
    lv_obj_clear_flag(s_photo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);

    s_photoCredit = mk_label(s_tileRadar, UI_FONT_S, &UI_ST_SOFT, "");
    lv_obj_add_flag(s_photoCredit, LV_OBJ_FLAG_HIDDEN);
}

// The status note under the N (two chips, one per colour, at most one shown) and the idle
// clock's date, both on the radar tile.
static void build_status_note(void) {
    s_noteEmerg = mk_box(s_tileRadar, &UI_ST_CHIP_EMERG, 0, NOTE_Y, 60, 24);
    s_noteEmergLbl = mk_label(s_noteEmerg, UI_FONT_S, nullptr, "");
    lv_obj_center(s_noteEmergLbl);
    show(s_noteEmerg, false);
    s_noteWarn = mk_box(s_tileRadar, &UI_ST_CHIP_WARN, 0, NOTE_Y, 60, 24);
    s_noteWarnLbl = mk_label(s_noteWarn, UI_FONT_S, nullptr, "");
    lv_obj_center(s_noteWarnLbl);
    show(s_noteWarn, false);

    s_clockDate = mk_label_c(s_tileRadar, UI_FONT_S, &UI_ST_SOFT, "", 350);
    show(s_clockDate, false);
}

// The pull-down: a transparent full-screen catcher on lv_layer_top() holding the sheet. The
// catcher is always shown but clickable only while open (a press on it then stays on it:
// press lock); the sheet is hidden while closed. Built last, so it is the top object on
// lv_layer_top().
static void build_pulldown(void) {
    s_pull = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_pull);
    lv_obj_set_size(s_pull, SCREEN_W, SCREEN_H);
    lv_obj_clear_flag(s_pull, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_pull, LV_OBJ_FLAG_PRESS_LOCK);   // shown always; clickable only while open
    lv_obj_add_event_cb(s_pull, pull_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_pull, pull_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_pull, pull_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_pull, pull_click_cb, LV_EVENT_CLICKED, NULL);

    s_pullSheet = mk_box(s_pull, &UI_ST_SURFACE, 0, 0, SCREEN_W, PULL_SHEET_H);
    lv_obj_set_style_border_width(s_pullSheet, 0, 0);   // children sit at sheet x/y (a border would shift them)
    lv_obj_add_flag(s_pullSheet, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_HIDDEN);   // the THEME chip's events reach the catcher
    mk_box(s_pullSheet, &UI_ST_SURFACE, 0, PULL_SHEET_H - UI_FRAME_W, SCREEN_W, UI_FRAME_W);   // the edge line (all frame)

    s_pullTime = mk_label_c(s_pullSheet, UI_FONT_L, &UI_ST_INK, "--:--", 30);
    s_pullDate = mk_label_c(s_pullSheet, UI_FONT_S, &UI_ST_SOFT, "", 70);
    static lv_point_t divPts[2];
    mk_divider(s_pullSheet, 70, 396, 94, divPts);

    s_pullWifi = mk_box(s_pullSheet, nullptr, 0, 0, 18, 14);
    for (int i = 0; i < 4; ++i)
        s_pullBars[i] = mk_box(s_pullWifi, nullptr, (lv_coord_t)(i * 5), (lv_coord_t)(14 - (4 + i * 3)),
                               3, (lv_coord_t)(4 + i * 3));   // 4, 7, 10, 13 px tall
    s_pullWifiLbl = mk_label(s_pullSheet, UI_FONT_M, nullptr, "");
    s_pullGpsLbl = mk_label(s_pullSheet, UI_FONT_M, nullptr, "");
    s_pullPlane = mk_img(s_pullSheet, &UI_ST_IMG_INK, &ui_icon_plane);
    s_pullPlanesLbl = mk_label(s_pullSheet, UI_FONT_M, &UI_ST_INK, "");
    s_pullBatt = mk_box(s_pullSheet, nullptr, 0, 0, 24, 12);
    s_pullBattFrame = mk_box(s_pullBatt, nullptr, 0, 0, 22, 12);
    lv_obj_set_style_border_width(s_pullBattFrame, UI_FRAME_W, 0);
    lv_obj_set_style_border_opa(s_pullBattFrame, LV_OPA_COVER, 0);
    s_pullBattFill = mk_box(s_pullBatt, nullptr, 4, 4, 14, 4);
    lv_obj_set_style_bg_opa(s_pullBattFill, LV_OPA_COVER, 0);
    s_pullBattNub = mk_box(s_pullBatt, nullptr, 22, 3, 2, 6);
    lv_obj_set_style_bg_opa(s_pullBattNub, LV_OPA_COVER, 0);
    s_pullBattLbl = mk_label(s_pullSheet, UI_FONT_M, &UI_ST_INK, "");
    s_pullBolt = mk_img(s_pullSheet, &UI_ST_IMG_INK, &ui_icon_bolt);

    s_pullData = mk_label_c(s_pullSheet, UI_FONT_S, &UI_ST_SOFT, "", 180);
    s_pullTheme = mk_box(s_pullSheet, &UI_ST_CHIP_ACCENT, 0, 200, 100, 24);
    lv_obj_add_flag(s_pullTheme, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_ext_click_area(s_pullTheme, 10);
    lv_obj_add_event_cb(s_pullTheme, pull_theme_cb, LV_EVENT_CLICKED, NULL);
    s_pullThemeLbl = mk_label(s_pullTheme, UI_FONT_S, nullptr, "");
    lv_obj_center(s_pullThemeLbl);
    mk_box(s_pullSheet, &UI_ST_DOT, SCREEN_CX - 20, 236, 40, 4);   // the handle: faint, says "swipe up"

    s_pullTimer = lv_timer_create(pull_timer_cb, UI_PULL_CLOSE_MS, nullptr);
    lv_timer_pause(s_pullTimer);
}

static void build_zoom(void) {
    // on-screen range/zoom chip: reliable single tap, lower right
    s_zoomBtn = s_bigText ? mk_box(s_tileRadar, &UI_ST_SURFACE, 298, 358, 88, 34)
                          : mk_box(s_tileRadar, &UI_ST_SURFACE, 326, 366, 64, 26);
    lv_obj_add_flag(s_zoomBtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_zoomBtn, 18);   // invisibly enlarge the tap target (easier to hit)
    lv_obj_clear_flag(s_zoomBtn, LV_OBJ_FLAG_SCROLL_CHAIN);  // tapping it must not swipe the tileview
    lv_obj_add_event_cb(s_zoomBtn, zoom_cb, LV_EVENT_PRESSED, NULL);  // fire on touch-down, not release
    s_zoomLbl = mk_label(s_zoomBtn, s_bigText ? UI_FONT_M : UI_FONT_S, &UI_ST_ACCENT_HI, "30 KM");
    lv_obj_center(s_zoomLbl);
}

static void build_list_tile(void) {
    make_round_panel(s_tileList);
    s_listTitle = mk_label_c(s_tileList, UI_FONT_M, &UI_ST_TITLE, "AIRCRAFT", 36);
    lv_obj_t *call = mk_label(s_tileList, UI_FONT_S, &UI_ST_SOFT, "CALL");
    lv_obj_set_pos(call, 100, 76);
    s_listHdrAlt = mk_label(s_tileList, UI_FONT_S, &UI_ST_SOFT, "ALT FT");
    lv_obj_set_width(s_listHdrAlt, 70);
    lv_obj_set_style_text_align(s_listHdrAlt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_listHdrAlt, 310 - 70, 76);
    s_listHdrDist = mk_label(s_tileList, UI_FONT_S, &UI_ST_SOFT, "NM");
    lv_obj_set_width(s_listHdrDist, 40);
    lv_obj_set_style_text_align(s_listHdrDist, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_listHdrDist, 394 - 40, 76);

    if (!s_rowStyleInit) {   // one shared row style (rows are plain labels)
        lv_style_init(&s_rowStyle);
        lv_style_set_width(&s_rowStyle, 326);
        lv_style_set_height(&s_rowStyle, LIST_ROW_H);   // 5 + 1 border allowance + 22 line + 5 + 1 border
        lv_style_set_pad_hor(&s_rowStyle, 2);
        lv_style_set_pad_ver(&s_rowStyle, 5);
        lv_style_set_border_side(&s_rowStyle, LV_BORDER_SIDE_BOTTOM);
        lv_style_set_border_width(&s_rowStyle, 1);
        lv_style_set_border_opa(&s_rowStyle, LV_OPA_COVER);
        lv_style_set_border_color(&s_rowStyle, COL(P().faint));
        lv_style_set_text_font(&s_rowStyle, UI_FONT_M);
        lv_style_set_text_color(&s_rowStyle, COL(P().ink));
        s_rowStyleInit = true;
    }
    s_list = lv_obj_create(s_tileList);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_pos(s_list, 70, 94);
    lv_obj_set_size(s_list, 326, 298);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_list, list_press_cb, LV_EVENT_PRESSED, NULL);  // pick the plane at touch-down
    lv_obj_add_event_cb(s_list, list_tap_cb, LV_EVENT_CLICKED, NULL);   // rows aren't clickable
    lv_obj_add_event_cb(s_list, list_scroll_cb, LV_EVENT_SCROLL, NULL);
    s_listSpacer = lv_obj_create(s_list);           // as tall as every row: the scroll range
    lv_obj_remove_style_all(s_listSpacer);
    lv_obj_clear_flag(s_listSpacer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_listSpacer, 1, 0);
    for (int k = 0; k < LIST_POOL; ++k) {
        lv_obj_t *l = lv_label_create(s_list);
        lv_obj_remove_style_all(l);
        lv_obj_add_style(l, &s_rowStyle, 0);
        lv_label_set_recolor(l, true);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_label_set_text(l, "");
        show(l, false);
        s_listRow[k] = l;
        s_listRowIdx[k] = -1;
    }
}

static void build_stats_tile(void) {
    make_round_panel(s_tileStats);
    mk_label_c(s_tileStats, UI_FONT_M, &UI_ST_TITLE, "STATUS", 36);
    static lv_point_t divPts[2];
    mk_divider(s_tileStats, 150, 316, 66, divPts);

    s_statsNames = mk_label(s_tileStats, UI_FONT_M, &UI_ST_SOFT, "");
    lv_obj_set_style_text_line_space(s_statsNames, 4, 0);
    lv_obj_set_pos(s_statsNames, 86, 78);
    s_statsVals = mk_label(s_tileStats, UI_FONT_M, &UI_ST_INK, "");
    lv_label_set_recolor(s_statsVals, true);
    lv_obj_set_style_text_line_space(s_statsVals, 4, 0);
    lv_obj_set_style_text_align(s_statsVals, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_statsVals, 294);
    lv_obj_set_pos(s_statsVals, 86, 78);

    // footer: where to reach the configuration page (IP / hostname / setup AP) + version
    s_statsNet = mk_label(s_tileStats, UI_FONT_S, &UI_ST_SOFT, "");
    lv_label_set_recolor(s_statsNet, true);
    lv_obj_set_width(s_statsNet, 380);
    lv_obj_set_style_text_align(s_statsNet, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_statsNet, 3, 0);
    lv_obj_set_pos(s_statsNet, SCREEN_CX - 190, 318);
    apply_netinfo();

    // power off (hold): a chip at the bottom, shown only where the board can power off
    s_offBtn = mk_box(s_tileStats, &UI_ST_CHIP_EMERG, 159, 380, 148, 28);
    lv_obj_add_flag(s_offBtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_offBtn, 12);
    lv_obj_clear_flag(s_offBtn, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(s_offBtn, poweroff_cb, LV_EVENT_ALL, NULL);
    s_offLbl = mk_label(s_offBtn, UI_FONT_S, nullptr, "HOLD TO POWER OFF");
    lv_obj_center(s_offLbl);
    show(s_offBtn, s_poweroffCb != nullptr);   // shown only when ui_set_poweroff_cb(non-null)

    // hold-progress ring round the screen edge (top layer, so it shows over the Stats tile)
    s_offRing = lv_arc_create(lv_layer_top());
    lv_obj_set_size(s_offRing, SCREEN_W, SCREEN_H);
    lv_obj_center(s_offRing);
    lv_obj_remove_style(s_offRing, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_offRing, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_rotation(s_offRing, 270);              // 0 degrees = 12 o'clock
    lv_arc_set_bg_angles(s_offRing, 0, 360);
    lv_arc_set_angles(s_offRing, 0, 0);
    lv_obj_set_style_arc_opa(s_offRing, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_offRing, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_offRing, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_offRing, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_offRing, COL(P().emerg), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_offRing, false, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s_offRing, 0, 0);
}

static void build_weather_tile(void) {
    lv_obj_t *wp = make_round_panel(s_tileWeather);
    // The entire view cycles the weather mode: there is nothing else to touch here, and a
    // small chip at the bottom of a ROUND panel is a genuinely hard target. Everything else
    // is left non-clickable so all taps funnel here.
    //
    // CLICKED, not PRESSED: this panel scroll-chains to the tileview, so dragging across it
    // is how you swipe off the view. On PRESSED every such swipe would also cycle the mode.
    lv_obj_add_flag(wp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wp, weather_mode_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_style_bg_color(wp, lv_color_black(), 0);   // the square image has black edges
    lv_obj_t *t = s_tileWeather;

    // radar / clouds: the image and rings first (background), text on chips above them
    s_wxCanvas = lv_canvas_create(t);
    lv_obj_set_size(s_wxCanvas, WX_RADAR_SIZE, WX_RADAR_SIZE);
    lv_obj_align(s_wxCanvas, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_clear_flag(s_wxCanvas, LV_OBJ_FLAG_CLICKABLE);   // taps go to the panel
    lv_obj_add_flag(s_wxCanvas, LV_OBJ_FLAG_HIDDEN);
    const int ringSize[3] = { 360, 240, 120 };
    for (int i = 0; i < 3; ++i) {
        s_wxRings[i] = mk_box(t, nullptr, (lv_coord_t)(SCREEN_CX - ringSize[i] / 2),
                              (lv_coord_t)(52 + (360 - ringSize[i]) / 2), ringSize[i], ringSize[i]);
        lv_obj_set_style_radius(s_wxRings[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_wxRings[i], 1, 0);
        lv_obj_set_style_border_opa(s_wxRings[i], i == 0 ? 180 : 90, 0);   // colour: set_wx_mode_chips()
    }
    s_wxNorth = mk_label_c(t, UI_FONT_S, &UI_ST_ACCENT, "N", 80);
    s_wxCenter = mk_label_c(t, UI_FONT_M, &UI_ST_INK, "+", 221);
    s_wxRange = mk_label(t, UI_FONT_S, &UI_ST_ACCENT, "");
    lv_obj_set_width(s_wxRange, 70);
    lv_obj_set_style_text_align(s_wxRange, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_wxRange, 361 - 35, 226);
    s_wxStatus = mk_label_c(t, UI_FONT_S, &UI_ST_FAINT, "ACQUIRING WX RADAR...", 252);

    s_weatherTitle = mk_label_c(t, UI_FONT_M, &UI_ST_TITLE, "WX RADAR", 36);
    lv_obj_add_style(s_weatherTitle, &UI_ST_OVERLAY, 0);
    s_wxAirport = mk_label_c(t, UI_FONT_S, &UI_ST_SOFT, "RADAR CENTRE", 64);
    lv_obj_add_style(s_wxAirport, &UI_ST_OVERLAY, 0);
    s_wxFooter = mk_label_c(t, UI_FONT_M, &UI_ST_INK, "WEATHER DATA PENDING", 322);
    lv_obj_add_style(s_wxFooter, &UI_ST_OVERLAY, 0);
    s_wxMeta = mk_label_c(t, UI_FONT_S, &UI_ST_SOFT, "", 350);
    lv_obj_add_style(s_wxMeta, &UI_ST_OVERLAY, 0);
    s_wxAttrib = mk_label_c(t, UI_FONT_S, &UI_ST_FAINT, "WAITING FOR RADAR DATA", 368);
    lv_obj_add_style(s_wxAttrib, &UI_ST_OVERLAY, 0);

    // forecast: current conditions, three metrics, three days
    s_fcIcon = lv_img_create(t);
    lv_obj_remove_style_all(s_fcIcon);
    lv_obj_add_style(s_fcIcon, &UI_ST_IMG_INK, 0);
    lv_img_set_src(s_fcIcon, &ui_icon_wx[UI_WX_CLOUDY]);
    s_fcCurrent = mk_label(t, UI_FONT_L, &UI_ST_INK, "--");
    s_fcCondition = mk_label_c(t, UI_FONT_M, &UI_ST_SOFT, "Waiting for data", 130);
    const char *metricNames[3] = { "FEELS", "HUMIDITY", "WIND" };
    const lv_coord_t colX[3] = { 123, 233, 343 };
    for (int i = 0; i < 3; ++i) {
        s_fcMetricName[i] = mk_label(t, UI_FONT_S, &UI_ST_SOFT, metricNames[i]);
        s_fcMetricValue[i] = mk_label(t, UI_FONT_M, &UI_ST_INK, "-");
        s_fcDay[i] = mk_label(t, UI_FONT_M, &UI_ST_ACCENT, "---");
        s_fcDayTemp[i] = mk_label(t, UI_FONT_M, &UI_ST_INK, "");
        s_fcDayRain[i] = mk_label(t, UI_FONT_S, &UI_ST_INFO, "");
        lv_obj_t *col[5] = { s_fcMetricName[i], s_fcMetricValue[i], s_fcDay[i], s_fcDayTemp[i], s_fcDayRain[i] };
        const lv_coord_t ys[5] = { 166, 180, 220, 286, 314 };
        for (int k = 0; k < 5; ++k) {
            lv_obj_set_width(col[k], 110);
            lv_obj_set_style_text_align(col[k], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(col[k], (lv_coord_t)(colX[i] - 55), ys[k]);
        }
        s_fcDayIcon[i] = lv_img_create(t);
        lv_obj_remove_style_all(s_fcDayIcon[i]);
        lv_obj_add_style(s_fcDayIcon[i], &UI_ST_IMG_INK, 0);
        lv_img_set_src(s_fcDayIcon[i], &ui_icon_wx[UI_WX_CLOUDY]);
        lv_obj_set_pos(s_fcDayIcon[i], (lv_coord_t)(colX[i] - 16), 248);
    }
    s_fcUpdated = mk_label_c(t, UI_FONT_S, &UI_ST_FAINT, "", 336);

    // mode chips: what each tap cycles through, the current one framed
    static const char *const MODES[3] = { "RADAR", "CLOUDS", "3-DAY" };
    int x = SCREEN_CX - 86;   // 51 + 6 + 58 + 6 + 51 = 172 px
    for (int i = 0; i < 3; ++i) {
        const int w = (int)strlen(MODES[i]) * UI_S_ADV + 16;
        s_wxMode[i] = mk_box(t, &UI_ST_SURFACE, (lv_coord_t)x, 410, (lv_coord_t)w, 24);
        s_wxModeLbl[i] = mk_label(s_wxMode[i], UI_FONT_S, nullptr, MODES[i]);
        lv_obj_center(s_wxModeLbl[i]);
        x += w + 6;
    }
}

static void build_dots_and_toast(void) {
    s_dots = mk_box(lv_layer_top(), nullptr, SCREEN_CX - 24, 446, 48, 6);
    for (int i = 0; i < 4; ++i) {
        s_dot[i] = lv_obj_create(s_dots);
        lv_obj_remove_style_all(s_dot[i]);
        lv_obj_add_style(s_dot[i], &UI_ST_DOT, 0);
        lv_obj_add_style(s_dot[i], &UI_ST_DOT_ON, LV_STATE_CHECKED);
        lv_obj_clear_flag(s_dot[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(s_dot[i], LV_ALIGN_CENTER, (lv_coord_t)(i * 14 - 21), 0);   // 4-px and 6-px dots share a centre
    }
    s_toast = mk_box(lv_layer_top(), &UI_ST_SURFACE, 0, 0, 100, 38);
    s_toastLbl = mk_label(s_toast, UI_FONT_M, &UI_ST_ACCENT_HI, "");
    lv_obj_center(s_toastLbl);
    show(s_toast, false);
}

void ui_create(void) {
    ui_style_set_theme(radar::theme());   // the shared styles exist before any object uses them
    ui_style_set_theme_hook(theme_hook);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_tv = lv_tileview_create(scr);
    lv_obj_set_size(s_tv, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_tv, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);

    s_tileRadar = lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_RIGHT);
    s_tileList  = lv_tileview_add_tile(s_tv, 1, 0, LV_DIR_HOR);
    s_tileStats = lv_tileview_add_tile(s_tv, 2, 0, LV_DIR_HOR);
    s_tileWeather = lv_tileview_add_tile(s_tv, 3, 0, LV_DIR_LEFT);
    // Rebuild the list/stats with the latest data the moment they slide into view
    // (between polls they'd otherwise show whatever was there when last visible).
    lv_obj_add_event_cb(s_tv, [](lv_event_t *) {
        if (active_view_index() != 0) pull_close();   // a page swipe never leaves it over another page
        refresh_active_tile();
        update_dots();
        if (s_viewChangedCb) s_viewChangedCb(active_view_index());   // let main remember the last view
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    // --- radar tile ---
    lv_obj_clear_flag(s_tileRadar, LV_OBJ_FLAG_SCROLLABLE);
    radar::init(s_tileRadar);
    radar::setRangeLabelVisible(false);                     // the zoom chip shows the range instead
    lv_obj_add_flag(s_tileRadar, LV_OBJ_FLAG_CLICKABLE);     // receive taps (planes/empty)
    lv_obj_add_event_cb(s_tileRadar, radar_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_tileRadar, radar_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_tileRadar, radar_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_tileRadar, radar_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_tileRadar, radar_presslost_cb, LV_EVENT_PRESS_LOST, NULL);
    build_card();
    build_zoom();
    build_status_note();

    // idle clock: a transparent layer above everything takes the touch that wakes the radar
    s_clockCatcher = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_clockCatcher);
    lv_obj_set_size(s_clockCatcher, SCREEN_W, SCREEN_H);
    lv_obj_clear_flag(s_clockCatcher, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(s_clockCatcher, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_clockCatcher, clock_catcher_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_clockCatcher, clock_catcher_release_cb, LV_EVENT_PRESS_LOST, NULL);
    radar::setClockStageCb(clock_stage_cb);

    build_list_tile();
    build_stats_tile();
    build_weather_tile();
    build_dots_and_toast();
    build_pulldown();             // last: the top object on lv_layer_top()

    lv_obj_set_tile_id(s_tv, 0, 0, LV_ANIM_OFF);
    update_dots();
    theme_hook(radar::theme());   // local colours (rings, chips, rows) for the starting theme
    send_obstacles();

    ui_splash_show();   // branded boot splash on top (auto-fades)
}
