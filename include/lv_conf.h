/**
 * Plane Radar 2.0 — LVGL v8.x configuration.
 * Reached via -DLV_CONF_INCLUDE_SIMPLE (LVGL does #include "lv_conf.h").
 * Only the settings we care about are listed; everything else falls back to the
 * library defaults in lv_conf_internal.h. Tuned for the CO5300 466x466 AMOLED.
 */
#if 1 /* Enable content */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH 16
/* 0: native RGB565 order. display.cpp swaps the two bytes of each pixel for the panel as it
   pushes (swap_row), and the sweep compositor works in this order too. */
#define LV_COLOR_16_SWAP 0
#define LV_COLOR_SCREEN_TRANSP 0
#define LV_COLOR_MIX_ROUND_OFS 0
#define LV_COLOR_CHROMA_KEY lv_color_hex(0x00ff00)

/*=========================
   MEMORY SETTINGS
 *=========================*/
/* Object/style pool (internal RAM). Big bitmap draw buffers are allocated
   separately in PSRAM in display.cpp. Bump this if the radar UI grows. */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (64U * 1024U)
#define LV_MEM_ADR 0
#define LV_MEM_BUF_MAX_NUM 16
#define LV_MEMCPY_MEMSET_STD 0

/*====================
   HAL SETTINGS
 *====================*/
#define LV_DISP_DEF_REFR_PERIOD 16   /* ms; ~60 Hz cap (SPI bandwidth is the real limit) */
#define LV_INDEV_DEF_READ_PERIOD 20  /* ms */

/* On the device, drive LVGL's tick from Arduino's millis() — no separate ticker.
   On the native SDL simulator there is no Arduino.h, so fall back to lv_tick_inc()
   (called from the sim main loop). */
#if defined(ARDUINO) || defined(ESP_PLATFORM)
#  define LV_TICK_CUSTOM 1
#  define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#  define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#else
#  define LV_TICK_CUSTOM 0
#endif

#define LV_DPI_DEF 130

/*=======================
   FEATURE / DRAW CONFIG
 *=======================*/
#define LV_DRAW_COMPLEX 1
#define LV_SHADOW_CACHE_SIZE 0
/* Each cached radius takes ~6 bytes per pixel of radius from LV_MEM_SIZE (~1.4 KB at the
   screen's radius). With 10 entries the pool ran out in the simulator one second after boot.
   The big circles (scope rings, round page panels) no longer go through masks per frame. */
#define LV_CIRCLE_CACHE_SIZE 4
#define LV_DISP_ROT_MAX_BUF (10 * 1024)

/*==================
   LOG (serial)
 *==================*/
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/*==================
   ASSERTS / DEBUG
 *==================*/
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_USE_REFR_DEBUG 0

/*==================
   FONTS
 *==================*/
/* The UI uses one pixel font, Departure Mono, 1-bit at 11/22/33 px (src/fonts/README.md).
   No Montserrat: the UI draws its own icons, so no LV_SYMBOL glyphs are needed. */
#define LV_FONT_MONTSERRAT_12 0
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_16 0
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_CUSTOM_DECLARE LV_FONT_DECLARE(font_px11) LV_FONT_DECLARE(font_px22) LV_FONT_DECLARE(font_px33)
#define LV_FONT_DEFAULT &font_px22

/*==================
   WIDGETS
 *==================*/
/* Core widgets default to enabled in v8. Spinner lives in "extra" — enable it
   explicitly for the M0 hello screen. */
#define LV_USE_ARC 1
#define LV_USE_LABEL 1
#define LV_USE_SPINNER 1
#define LV_USE_LIST 1
#define LV_USE_TILEVIEW 1

#endif /* LV_CONF_H */
#endif /* Enable content */
