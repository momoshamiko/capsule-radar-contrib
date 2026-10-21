#pragma once
#include <stdint.h>
// UI: swipeable views (radar / list / stats / weather) + tap-to-inspect detail card, in the
// Pixel look (ui_style.h, ui_tokens.h). Pure LVGL, portable (device + SDL simulator).
void ui_create(void);            // build the whole UI on the active screen
bool ui_sweep_visible(void);     // radar tile shown and not mid-swipe (sweep compositor)
int  ui_sweep_masks(int16_t out[][5], int max);   // logical x1,y1,x2,y2,radius: rounded boxes the sweep must not paint over
void ui_on_data_updated(void);   // refresh card/list/stats after radar::update()
void ui_show_view(int idx);      // 0 = radar, 1 = list, 2 = stats, 3 = weather
void ui_bench_page(int idx);     // GET /perf?bench=1: animated change to page idx (like the end of a swipe)
bool ui_bench_moving(void);      // ...the tileview is still scrolling or animating
int  ui_bench_view(void);        // ...the page on screen (0..3)
void ui_set_view_changed_cb(void (*cb)(int idx));  // fired on swipe — lets main remember the last view
void ui_set_status(bool wifiUp, bool feedOk, int rssi, const char *clock);  // status note (NO WIFI / DATA OLD) + the pull-down's WiFi line and clock
void ui_set_battery(int pct, bool charging, bool present);  // the pull-down's battery line + the LOW BATTERY note (no line without a battery)
void ui_set_pulldown(bool open);             // open/close the status pull-down (radar view only, never over the idle clock); the simulator's shots use it
void ui_set_date(const char *date);  // the date on the idle clock face and in the pull-down (e.g. "08 Jun 2026")
void ui_set_feed_age(uint32_t ms);   // Stats "Updated" and the pull-down's data line: ms since the last good poll (UINT32_MAX = never)
void ui_set_netinfo(const char *line);  // stats view footer: how to reach the config page
void ui_set_feed_source(const char *host);  // Stats "Feed" line and the pull-down's data line: which ADS-B provider served the last data
void ui_set_gps(int state, int sats, float altM);   // GPS: state 0=off/hidden 1=acquiring 2=fix; the pull-down's GPS line + Stats row (altM = GPS altitude, NAN if unknown)
void ui_splash_show(void);  // branded boot splash (auto-fades, covers init time)
void ui_set_range_cb(void (*cb)(float km));  // on-screen zoom chip -> notify main
void ui_set_range_km(float km);              // update the zoom chip label / sync the cycle
void ui_set_units(int preset);               // 0 = Aviation (ft,kt,nm) · 1 = Metric (m,km/h,km) · 2 = Imperial (ft,mph,mi)
void ui_set_large_text(bool on);             // accessibility: plane labels, zoom chip and card details in M. Call BEFORE ui_create()
void ui_set_weather_forecast(bool forecast); // false = WX radar, true = 3-day forecast
void ui_set_poweroff_cb(void (*cb)(void));   // Stats view: hold the Power off button
