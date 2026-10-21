# Architecture

## Concurrency model
- **`adsb_task` (pinned to core 0)**: maintains WiFi, fetches the feed every `POLL_INTERVAL_MS`, parses it, and atomically swaps the result into a shared `std::vector<Aircraft>` protected by `g_ac_mutex`. Three providers are tried in order — airplanes.live → adsb.fi → adsb.lol (`src/config.h`), each paced independently (403 parks it, 429/timeout/empty-200 backs it off; see `adsb_pacing.h`). Stale entries expire by `seen_pos`.
- **Render loop (core 1 / Arduino `loop()`)**: drives `lv_timer_handler()`, copies the aircraft snapshot under the mutex, and renders the active view. No blocking calls here.

```
[airplanes.live · adsb.fi · adsb.lol] --HTTPS--> adsb_task (core0) --mutex--> g_aircraft[] <--mutex-- render (core1) --> LVGL + compositor --> co5300 (QSPI, DMA) --> AMOLED
                                                                                  ^ touch (CST9217/FT3168) -> hit test
```

## Memory
- LVGL draws into one 466×40 partial buffer in internal DMA RAM (`LVGL_BUF_LINES`,
  `display.cpp`); rendering into PSRAM was the old bottleneck. The panel keeps its own GRAM, so
  there is no scanout framebuffer. PSRAM holds the compositor's copy of the panel (`s_phys`,
  434 KB), its bearing map (434 KB) and the scope cache (434 KB, below).
- LVGL objects live in its own 64 KB pool (`include/lv_conf.h`). The List view recycles 11
  row labels over a spacer instead of one label per aircraft, and a theme change rebuilds only
  the page on screen: with a 60-aircraft feed the pool keeps ~19 KB free. The circle-mask cache
  (`LV_CIRCLE_CACHE_SIZE`) stays at 4: each entry at screen radius takes ~1.4 KB of that pool.

## Rendering the scope
The scope bottom is a **full-screen RGB565 background cache** (434 KB in PSRAM, `radar_view.cpp`),
shown by one `lv_img` (`TRUE_COLOR`, `img_opa` cover) as the first child of the radar tile —
it passes LVGL's cover check, so a repaint of any scope area starts at it and everything
beneath is skipped. The cache holds, in z-order: the background (black; Orb's vertical
gradient), the persistent flow tracks, and the chrome the old grid layer drew (coastline,
airports, rings, crosshair, E/W ticks; Orb: grid, coastline, airports, centre triangle) —
all at their `dim()` opacities. `cache_render_rect()` renders any rectangle with LVGL's own
software renderer aimed at the buffer (the `lv_canvas_draw_*` trick, minus the slow
`set_px_cb` path), so a partial re-render is exact and repeatable: black underlay, the flow
segments whose bbox meets the rect, then the chrome (coastline segments bbox-prefiltered).
The whole cache re-renders on init, theme / airports / trail-length change, a clock dim
step, and a scope geometry change; per poll only the rects of flow segments added or pruned.
Aircraft render on an LVGL layer above the cache; `radar::update()` invalidates, per
aircraft hex, the union of the drawn bounds (glyph + rings/waves, trail, label box) before
and after the poll — a handful of merged areas, far under LVGL's 32-invalid-area limit,
instead of the whole layer. The animated sweep is composited per-pixel by `display.cpp`
(angular fade over the cache, fixed-point math, partial-refresh masks) on a 30 ms tick
(`SWEEP_FRAME_MS` in `config.h`; ~28 updates per second measured under load).
JSON documents are parsed with a PSRAM allocator (`adsb_client.cpp`) and mbedTLS buffers are
routed to PSRAM (`mbedtls_platform_set_calloc_free`, `main.cpp`), so TLS never starves internal RAM.

### Panel output
- `co5300.h/.cpp` is the panel driver: ESP-IDF `spi_master` at `LCD_QSPI_HZ`, one polling DMA
  transfer at a time, CS held low by hand across a window. `push()` fills rows into two 4 KB
  DMA buffers that take turns (the CPU fills one while the other is on the wire); compositor
  windows use it. `pushInPlace()` converts the rows of a caller's DMA-capable buffer in place
  and sends them from there; LVGL strips at 0° and 180° use it.
- Rotation (`display.cpp`, `flush_cb`): 0° goes straight out, 180° reverses the strip in
  place, 90°/270° read each column of the strip straight into the DMA buffers as one panel row
  (`row_from_lvgl_turned`), other angles sample a PSRAM copy of the logical frame.
- `s_phys` only has to match the panel while the compositor draws (radar tile on screen).
  Page swipes and the other pages skip saving strips into it; back on the radar, one full
  repaint refills it before the compositor draws again.
- The round backgrounds of list/stats/weather are drawn row by row (`round_panel_cb`,
  `ui.cpp`): only the ring's edge pixels need a distance, so a swipe frame no longer runs every
  pixel of a 462 px square through two circle masks.
- The GPS read (`gps.cpp`) needs the I2C bus to itself for ~0.3 s. During its waits the screen
  keeps rendering (`display::loopWithoutTouch`, the touch panel is not read), and a drain waits
  while the screen is in use (`GPS_TOUCH_QUIET_MS`, at most `GPS_DEFER_MAX_MS`).
- `GET /perf` (`perf.h/.cpp`) reports where core 1's time goes (LVGL, flush, push, compositor,
  feed, GPS, web, touch gaps), CPU load per core and task, and heap; `?bench=1` (page header
  required) times full repaints and animated page changes.

### UI look: tokens, styles, fonts
- `ui_tokens.h` is plain data (no LVGL): one `UiPalette` per theme, the altitude bands, sizes
  and spacing. `radar_view` draws the scope from it, so scope and chrome cannot drift apart.
- `ui_style.h/.cpp` holds one shared `lv_style_t` per role (ink, soft, accent, title, surface
  chip, emerg/warn/accent chips, divider, page dots, icon recolour, splash ring/arc).
  Objects take their role styles once, at creation, and set no local colour for a themed
  property. `radar::setTheme()` calls `ui_style_set_theme()`, which rewrites the styles in
  place and refreshes every object; `ui.cpp`'s theme hook then rebuilds the colours that live
  in text (list-row and stats recolour codes) and in data-driven local styles (the
  pull-down's battery, WiFi and GPS lines).
- Fonts: Departure Mono, 1-bit at 11/22/33 px (`src/fonts/`, see its README), the only fonts in
  the firmware; `UI_FONT_S/M/L` name them. Monospace digits keep the list columns aligned in
  one label per row. Icons (plane, bolt, weather) are 1-bit images from
  `tools/gen_ui_icons.py` (`ui_icons.h/.c`).
- Plane labels: `label_layout.h` (pure, host-tested in `tests/label_layout_test.cpp`) gives each
  label one of six places around its glyph, trying last frame's place first so labels do not
  jump. Planes go in priority order (emergencies, the selected plane, then nearest first), so
  labels stay inside the circle and clear of other labels, glyphs and the obstacles `ui.cpp`
  sends (`radar::setLabelObstacles`: status note, N, zoom chip, page dots, card, photo). A
  label with no free place is skipped.

### Status pull-down and note
The radar has no top row. A press that starts in the top band (`PULL_START_Y`) and moves
`PULL_MOVE_PX` down, more down than sideways, while no page scroll runs, opens the pull-down
(`ui.cpp`): a transparent full-screen catcher on `lv_layer_top()` (always shown, clickable only while open, press-locked) with a
248-px sheet (time, date, WiFi, GPS, aircraft count, battery, data source, THEME button). At
that moment `lv_indev_wait_release()` makes LVGL drop the rest of the opening press, so it is
neither a tap nor a theme hold. A drag up, a tap that began below the sheet, `UI_PULL_CLOSE_MS`
without a touch, a page change or the idle clock closes it; the sheet is a sweep mask while
open. The status note under the N (two chips, emerg and warn, at most one shown) names the
first problem: no WiFi, old data (`UI_FEED_STALE_S`), low battery (`UI_BATT_LOW_PCT`).

### Idle clock
`clock_face.h` is the animation model (pure, host-tested in `tests/clock_face_test.cpp`): from
the radar's sweep angle and the wall clock it gives, per moment, a `ClockFrame` — the sweep
line / second hand angle and shape, the hour/minute hands' growth, the cap, the trail's
opacity, and a scene step (0-3). `radar_view` owns the clock: `setClock()` starts or ends it,
its LVGL timer applies a scene step when it changes (dim the planes and map, hide labels,
fade the N out and the 12/3/6/9 dial in; one full repaint each, ~0.1 s
on the device), and `ui.cpp` hides the status note, zoom chip, card and page dots, closes the pull-down, shows the date, and
puts a transparent touch catcher on `lv_layer_top()`. Everything that moves is drawn by the compositor every tick
(`radar::clockFrame()`): the lead line becomes the second hand, and the hands and cap are
anti-aliased segments with their own boxes. Without a compositor (the SDL simulator) the
clock layer draws the hands in LVGL. `main.cpp` decides when (idle wait, the radar view
showing, the `clock` switch, no in-range emergency) and ramps the panel brightness with the
animation.

## Input
- Touch → screen coordinates → nearest-glyph hit test (within a tap radius) → select + populate detail card (HDG shows the aircraft's ground track). The card's X or a tap on empty scope closes it; a tap elsewhere on the card does nothing. The X is a radar-tile object just outside the card on the side facing the scope's centre (right edges flush), so its tap area (grown by `UI_CARD_CLOSE_HIT`) works on every side; it hides while the status pull-down is open.
- Swipe gestures switch views; a still 1.5 s hold on the radar (`UI_THEME_HOLD_MS`) then a lift cycles the visual theme (LVGL's 400 ms long press caught firm taps); the on-screen zoom button cycles the range preset (10/20/30/50/100 km; the web page also offers 15, 25 and 150 km).
- IMU posture from QMI8658 drives face-down sleep; touch inactivity drives the idle clock or dim; LVGL indev for touch. While the screen is dark, a touch only wakes it and never reaches the UI.

## Persistence & lifecycle
- `Preferences` (NVS), namespace `capsuleradar`; keys read in `main.cpp`: `homeLat`, `homeLon`,
  `rangeKm`, `bright`, `vol`, `mute`, `alertmode`, `proxkm`, `usegps`, `traillen`, `maxac`,
  `idledim`, `fdsleep`, `clock`, `units`, `tz`, `bigtext`, `theme` (saved on the theme hold or
  from the web page), `sweep`, `airports`, `hideground`, `minalt`, `maxalt`, `milonly`,
  `lastview`, `rotDeg` (older installs: `rot`). The route cache has its own namespace, `routes`
  (`route_client.cpp`).
- First boot without saved credentials opens the WiFiManager captive portal (WiFi only; the
  centre lat/lon and every other setting are configured on the device's web page). A held
  BOOT button is NOT wired up — `PIN_BOOT_BUTTON` is defined in the board headers but unused.
- ArduinoOTA enabled after WiFi is up.
- **Power off**: the Stats view has a hold-1.5 s "Power off" button and the web server answers `POST /poweroff`, both driving the AXP2101 shutdown. The button shows only when the build has a PMIC (`BOARD_HAS_PMIC`, the 1.75) and the PMIC answered at boot; otherwise it stays hidden and `POST /poweroff` answers 501. A power-off stores the radar as the view to show at the next start (other restarts restore the last view). While the button is held, a ring round the screen edge fills clockwise from 12 o'clock in the theme's emergency colour and unwinds if the finger lifts early.
- **Self-heal watchdog**: `FeedWatchdog` (`feed_watchdog.h`, driven by `adsb_task` in `main.cpp`) restarts the device when the feed has been stuck `ADSB_STUCK_MS` (180 s) with WiFi up and the largest free internal heap block is under `ADSB_STUCK_MIN_LARGEST_BLOCK` — internal-heap fragmentation starving TLS. Only a provider answering (any HTTP status) refreshes it; a poll the pacer skipped does not, because nothing was sent. A feed stuck with a healthy heap (refusals, internet down) keeps running with the amber "DATA OLD" note, up to a last-resort restart after 30 minutes without any answer (`ADSB_STUCK_HARD_MS`); that clock pauses while every provider is parked by a 403 or a Retry-After. `tests/feed_watchdog_test.cpp` drives it with the real pacer on the host.
- **Touch**: LVGL reads the panel only between frames, so `display.cpp` also samples it between flush strips (every `TOUCH_SAMPLE_MS`) and replays the samples to LVGL in order (`touch_queue.h`); a failed read is not a lift.

## Files
- `geo.h` — pure math: haversine, bearing, project-to-screen.
- `aircraft.h` — `Aircraft` struct + altitude→color + squawk/emergency helpers.
- `adsb_client.h/.cpp` + `adsb_pacing.h` — HTTPS GET + streaming ArduinoJson parse for the three providers, with host-testable per-provider pacing.
- `radar_view.h/.cpp` — scope rendering: rings, glyphs, trails, themes, plane labels (placed by `label_layout.h`), detail-card data, the idle clock's scene and dial.
- `label_layout.h` — label placement around the glyphs (host-tested).
- `ui_tokens.h`, `ui_style.h/.cpp` — design tokens and the shared LVGL styles built from them.
- `ui_icons.h/.c` — 1-bit icons, generated by `tools/gen_ui_icons.py`.
- `fonts/` — Departure Mono at 11/22/33 px (SIL OFL 1.1).
- `clock_face.h` — the idle clock's animation model (host-tested).
- `co5300.h/.cpp` — the CO5300 QSPI panel driver (DMA; see Panel output).
- `display.cpp` — LVGL display and touch drivers, rotation, and the per-pixel compositor (sweep, centre ripple, clock hands).
- `perf.h/.cpp` — render-path timing counters and the on-device benchmark (`GET /perf`).
- `ui.cpp` — LVGL views (radar/list/stats/weather), detail card, status pull-down and note, zoom chip, page dots, theme note, power-off button, clock touch catcher.
- `display_st7701.cpp` — the 2.1 board's RGB panel and LVGL glue (experimental): the same `display::` API as `display.cpp`, with no sweep compositor.
- `main.cpp` — tasks, WiFi/portal, web config server, NVS settings, self-heal watchdog, idle clock / dim / brightness.
