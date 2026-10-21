// CO5300 466x466 AMOLED (co5300.cpp, QSPI with DMA) + LVGL.
// Pins, panel gaps and the touch driver all come from the board header selected in
// config.h (see src/boards/). On boards with an AXP2101 the panel runs off the
// always-on DC1 rail, so it lights up without configuring the PMIC first.
// NOTE: co5300::begin() must stay ahead of touch_begin() — on the 1.43 the touch
// controller's reset is tied to the panel reset and it is absent from I2C until then.
// The actual UI is built by ui_boot_create() (shared with the native SDL sim).
#include "config.h"
#if !defined(BOARD_LCD21)      // CO5300 AMOLED driver (1.75 + 1.43); the LCD-2.1 build uses display_st7701.cpp
#include "display.h"
#include "radar_view.h"
#include "ui.h"
#include "perf.h"
#include "touch.h"
#include "touch_queue.h"
#include "co5300.h"

#include <Arduino.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

// --- LVGL plumbing -----------------------------------------------------------
#define LVGL_BUF_LINES 40    // partial draw-buffer height (lines); kept in fast internal RAM
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;
static lv_indev_drv_t     s_indev_drv;
static lv_color_t        *s_buf1 = nullptr;
static lv_color_t        *s_buf2 = nullptr;

static volatile uint32_t s_frameCount = 0;   // rendered frames (last-flush), for FPS measurement
uint32_t display_frames() { return s_frameCount; }

static volatile uint16_t s_rot = 0;          // clockwise display rotation, 0..359 degrees
static float      s_rotCos = 1.0f;
static float      s_rotSin = 0.0f;
static int32_t    s_rotCosQ16 = 65536;       // fixed-point values used in the per-pixel hot path
static int32_t    s_rotSinQ16 = 0;
static lv_color_t *s_rotBuf = nullptr;       // PSRAM scratch for arbitrary-angle output (see begin())
static lv_color_t *s_frameBuf = nullptr;     // full logical framebuffer for arbitrary-angle sampling

// --- sweep compositor ----------------------------------------------------------------
// Idea and blend from ijord's 2.8C fork (46e0822e, upstream issue #14), re-written for
// LVGL 8 and this QSPI panel, which keeps its own GRAM (no scanout framebuffer).
// LVGL renders the scope WITHOUT the sweep. Every block pushed to the panel is also kept
// in s_phys (physical orientation, so any rotation works). Each sweep tick re-pushes only
// the pixels under the old + new overlays (row spans, grouped into COMP_BAND-row windows):
// s_phys plus a per-pixel angular fade. LVGL output that lands under the wedge between
// ticks is blended the same way before it is pushed. Compositor windows are filled row by
// row into the panel driver's DMA buffers (co5300::push); LVGL strips are converted in
// LVGL's draw buffer and sent from there (co5300::pushInPlace).
// The idle clock's moving parts (radar_view's ClockFrame) are drawn here too: the lead
// line becomes the second hand, and the hour/minute hands and the centre cap are line
// segments with boxes of their own, so they move at the sweep's rate, not LVGL's.
// Pixels are LVGL's RGB565 (LV_COLOR_16_SWAP 0); a compile-time check is below.
#define ANG_UNITS      32768                     // 360 degrees in bearing-map units
#define ANG_OUTSIDE    0xFFFF                    // bearing-map value outside the scope disc
#define TRAIL_U        ((int)(SWEEP_TRAIL_DEG / 360.0f * ANG_UNITS))
#define FADE_SHIFT     3
#define FADE_N         ((TRAIL_U >> FADE_SHIFT) + 2)
#define COMP_BAND      32                        // rows per compositor window (even: the panel needs even heights)
#define SWEEP_MASKS    4                         // boxes the sweep must not paint over (ui_sweep_masks() in ui.cpp)
static_assert(LV_COLOR_16_SWAP == 0, "compositor assumes native-order RGB565");

struct Rect { int x1, y1, x2, y2; bool valid; };
static uint16_t *s_phys    = nullptr;   // last pushed LVGL content, physical orientation (PSRAM)
static uint16_t *s_angMap  = nullptr;   // per-pixel physical bearing, 0 = up, clockwise (PSRAM)
static uint16_t  s_rowBuf[SCREEN_W] __attribute__((aligned(4)));   // one s_phys row being blended
static uint8_t   s_fadeLut[FADE_N];
static uint16_t  s_swRing = 0x07E0, s_swLead = 0xFFFF;
static bool      s_swWanted  = false;   // radar_view: sweep on and the theme has one
static bool      s_swVisible = false;   // UI: radar tile shown, not mid-swipe
static bool      s_sweepWas = false, s_pulseWas = false;   // wanted states at the last tick
static int       s_angU      = 0;       // sweep line angle on the panel (bearing-map units)
static float     s_pcx = 0, s_pcy = 0;  // scope centre, physical pixels
// A line from the scope centre along (ux, uy), from t0 to t1 px (t0 < 0: a tail past the
// centre), hw px half-width, round ends. The sweep's lead line, the clock's hands and its
// cap (a zero-length segment: a disc) are all drawn as one of these.
struct Seg { float ux, uy, t0, t1, hw; uint16_t col; uint32_t opa; Rect box; float iuy; };   // iuy: 1/uy (0 if ~horizontal)
// The pixels the trail can touch: bearings between two directions (the trail's ends, widened
// a little; see trail_sector) as two half-planes through the scope centre. k0/k1: dx/dy of
// each direction, so a row's bounds need no division.
struct Sector { float d0x, d0y, d1x, d1y, k0, k1; };
static bool      s_trailOn   = false;   // the sweep trail is on the panel (s_trailBox, at s_angU)
static Rect      s_trailBox  = {};
static Sector    s_trailSec  = {};
static uint32_t  s_trailMul  = 256;     // trail opacity, /256: dimmer behind the clock's second hand
static bool      s_leadOn    = false;   // the lead line (the clock's second hand) is on the panel
static Seg       s_lead      = {};
// Idle clock parts below and above the second hand: two segments each (outline + hand,
// cap + dot), re-pushed only when the part changes.
enum { PART_HOUR, PART_MIN, PART_CAP, PARTS };
struct Part { bool on; Seg seg[2]; float deg, size; };   // deg/size: change detection
static Part      s_part[PARTS] = {};
// Objects the sweep must not paint over: the logical rounded rectangle (exact shape) and
// its physical bounding box (cheap first test).
struct SweepMask { Rect box; float x1, y1, x2, y2, r; };
static SweepMask s_mask[SWEEP_MASKS];
static int       s_maskN     = 0;
static uint32_t  s_lastComposeMs = 0;
static uint32_t  s_swPhase = 0;         // sweep angle in ANG_UNITS << 8 (sub-unit precision)
#define SWEEP_MAX_STEP_MS (2 * SWEEP_FRAME_MS)   // after a stall, resume instead of jumping ahead
// Centre ripple (the expanding ring at home), drawn here too so it moves at the sweep's rate.
static bool      s_pulseWanted = false; // radar_view: the theme has one
static bool      s_pulseOn   = false;   // a ring is currently on the panel
static uint16_t  s_pulseInk  = 0xFFFF;
static uint32_t  s_pulseMs   = 0;       // position in the expansion, 0..PULSE_PERIOD_MS
static float     s_pulseR    = 0;       // outer radius of the ring on the panel
static uint32_t  s_pulseOpa  = 0;
static Rect      s_pulseBox  = {};      // physical box of the ring currently on the panel
// Per-row dirty columns of one compositor tick (the old and new overlays), pushed in bands.
static int16_t   s_spanL[SCREEN_H], s_spanR[SCREEN_H];
static int       s_spanY1 = SCREEN_H, s_spanY2 = -1;

// Logical point -> physical, the same mapping the flush paths use (rotation about the
// panel centre, (W-1)/2).
static inline void log_to_phys(float lx, float ly, float *px, float *py) {
    const float c = (SCREEN_W - 1) * 0.5f;
    const float rx = lx - c, ry = ly - c;
    *px = c + s_rotCos * rx - s_rotSin * ry;
    *py = c + s_rotSin * rx + s_rotCos * ry;
}

// Half-width of the trail's disc (radius RADAR_R_OUTER_PX + 3, a little past the bearing
// map's) on each physical row, from the scope centre; negative: the row misses it.
#define TRAIL_DISC_R   (RADAR_R_OUTER_PX + 3)
static float s_discHalf[SCREEN_H];

static void sweep_build_map() {
    if (!s_angMap) return;
    float pcx, pcy;
    log_to_phys((float)SCREEN_CX, (float)SCREEN_CY, &pcx, &pcy);
    s_pcx = pcx; s_pcy = pcy;
    const float rMax2 = (float)(RADAR_R_OUTER_PX + 2) * (float)(RADAR_R_OUTER_PX + 2);
    for (int y = 0; y < SCREEN_H; ++y) {
        uint16_t *row = s_angMap + (size_t)y * SCREEN_W;
        const float dy = (float)y - pcy;
        const float h2 = (float)TRAIL_DISC_R * (float)TRAIL_DISC_R - dy * dy;
        s_discHalf[y] = h2 < 0 ? -1.0f : sqrtf(h2);
        for (int x = 0; x < SCREEN_W; ++x) {
            const float dx = (float)x - pcx;
            if (dx * dx + dy * dy > rMax2) { row[x] = ANG_OUTSIDE; continue; }
            float a = atan2f(dx, -dy);                       // 0 = up, clockwise
            if (a < 0) a += 2.0f * (float)M_PI;
            int u = (int)(a * ((float)ANG_UNITS / (2.0f * (float)M_PI)));
            row[x] = (uint16_t)(u & (ANG_UNITS - 1));
        }
    }
}

static void sweep_build_lut() {
    for (int i = 0; i < FADE_N; ++i) {
        const float delta = (float)((i << FADE_SHIFT) + (1 << (FADE_SHIFT - 1)));
        float frac = 1.0f - delta / (float)TRAIL_U;          // 1 at the lead edge -> 0 at the tail
        if (frac < 0) frac = 0;
        s_fadeLut[i] = (uint8_t)(frac * frac * (float)SWEEP_TRAIL_OPA);
    }
}

// Blend `tint` over `dst` at `opa` (0..255), ordered-dithered back to RGB565 so the
// fade has no visible banding (ijord's blend565_dither, with a /256 approximation).
static const uint8_t BAYER4[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
static inline uint16_t blend565(uint16_t dst, uint16_t tint, uint32_t opa, uint32_t d) {
    const uint32_t ia = 256 - opa;
    uint32_t dr = (dst >> 11) & 0x1F, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
    uint32_t tr = (tint >> 11) & 0x1F, tg = (tint >> 5) & 0x3F, tb = tint & 0x1F;
    dr = (dr << 3) | (dr >> 2); dg = (dg << 2) | (dg >> 4); db = (db << 3) | (db >> 2);
    tr = (tr << 3) | (tr >> 2); tg = (tg << 2) | (tg >> 4); tb = (tb << 3) | (tb >> 2);
    uint32_t r = ((tr * opa + dr * ia) >> 8) + (d >> 1);
    uint32_t g = ((tg * opa + dg * ia) >> 8) + (d >> 2);
    uint32_t b = ((tb * opa + db * ia) >> 8) + (d >> 1);
    r >>= 3; g >>= 2; b >>= 3;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// The trail over black, the scope background under most of it, is one table lookup instead
// of a blend per pixel: the colour for each opacity (0..SWEEP_TRAIL_OPA) and dither value.
// s_trailOpa folds s_trailMul into the fade. Both are rebuilt when their inputs change.
static uint16_t s_trailBlack[SWEEP_TRAIL_OPA + 1][16];
static uint8_t  s_trailOpa[FADE_N];
static int      s_trailEndU = TRAIL_U;     // bearing units behind the lead where the trail fades under 2/256
static uint16_t s_tblRing = 0;
static uint32_t s_tblMul = 0;
static bool     s_tblOk = false;
static void trail_tables() {
    if (!s_tblOk || s_tblRing != s_swRing) {
        for (int o = 0; o <= SWEEP_TRAIL_OPA; ++o)
            for (int d = 0; d < 16; ++d) s_trailBlack[o][d] = blend565(0, s_swRing, (uint32_t)o, (uint32_t)d);
        s_tblRing = s_swRing;
    }
    if (!s_tblOk || s_tblMul != s_trailMul) {
        int last = -1;                       // the fade only falls: past `last` nothing is blended
        for (int i = 0; i < FADE_N; ++i) {
            s_trailOpa[i] = (uint8_t)(((uint32_t)s_fadeLut[i] * s_trailMul) >> 8);
            if (s_trailOpa[i] >= 2) last = i;
        }
        s_trailEndU = min(TRAIL_U, (last + 1) << FADE_SHIFT);
        s_tblMul = s_trailMul;
    }
    s_tblOk = true;
}

// Is physical pixel (x,y) inside the mask's rounded rectangle? Tested in logical
// coordinates (inverse rotation), so the shape is exact at any display angle.
static bool mask_hit(const SweepMask &m, int x, int y) {
    const float c = (SCREEN_W - 1) * 0.5f;
    const float rx = (float)x - c, ry = (float)y - c;
    const float lx = c + s_rotCos * rx + s_rotSin * ry;
    const float ly = c - s_rotSin * rx + s_rotCos * ry;
    const float cx = lx < m.x1 + m.r ? m.x1 + m.r : (lx > m.x2 - m.r ? m.x2 - m.r : lx);
    const float cy = ly < m.y1 + m.r ? m.y1 + m.r : (ly > m.y2 - m.r ? m.y2 - m.r : ly);
    const float dx = lx - cx, dy = ly - cy, rr = m.r + 0.5f;    // +0.5: keep the anti-aliased edge too
    return dx * dx + dy * dy <= rr * rr;
}

// The columns of one physical row that the masks cover: one run per mask (a rounded
// rectangle is convex), sorted by start. Found once per row with a few mask_hit() calls from
// the box ends, so the blends below never test single pixels against the masks.
struct MaskRuns { int n; int16_t a[SWEEP_MASKS], b[SWEEP_MASKS]; };
static void row_mask_runs(int y, MaskRuns &r) {
    r.n = 0;
    for (int i = 0; i < s_maskN; ++i) {
        const SweepMask &m = s_mask[i];
        if (y < m.box.y1 || y > m.box.y2) continue;
        int a = max(m.box.x1, 0), b = min(m.box.x2, SCREEN_W - 1);
        while (a <= b && !mask_hit(m, a, y)) ++a;
        while (b >= a && !mask_hit(m, b, y)) --b;
        if (a > b) continue;
        int k = r.n++;                                  // insertion sort by start
        while (k > 0 && r.a[k - 1] > a) { r.a[k] = r.a[k - 1]; r.b[k] = r.b[k - 1]; --k; }
        r.a[k] = (int16_t)a;
        r.b[k] = (int16_t)b;
    }
}
static inline bool in_runs(const MaskRuns &r, int x) {
    for (int i = 0; i < r.n; ++i)
        if (x >= r.a[i] && x <= r.b[i]) return true;
    return false;
}

// The overlays below are blended into one physical row at a time: row[i] is pixel x0 + i of
// row y, and only columns xa..xb are touched. mr: the masked columns of row y.

// The trail at bearing unit u as a sector: from its last visible pixel (s_trailEndU) to the
// lead, widened by a few units each way so it holds every pixel the bearing-map test below
// can pass.
static Sector trail_sector(int u) {
    const float k = 2.0f * (float)M_PI / (float)ANG_UNITS;
    const float a0 = (float)(u - s_trailEndU - 2) * k, a1 = (float)(u + 3) * k;
    Sector s = { sinf(a0), -cosf(a0), sinf(a1), -cosf(a1), 0, 0 };
    s.k0 = fabsf(s.d0y) > 1e-6f ? s.d0x / s.d0y : 0;
    s.k1 = fabsf(s.d1y) > 1e-6f ? s.d1x / s.d1y : 0;
    return s;
}

// Columns of physical row y inside sector s (clockwise from d0 to d1, under half a turn) and
// within the trail's disc, widened by `pad` px; false when the row misses it.
// A point (X, Y) from the centre is clockwise of d0 when d0x*Y - d0y*X >= 0 and anticlockwise
// of d1 when X*d1y - Y*d1x >= 0: each bounds X on one side. Rows next to the centre always
// keep it, where the sector is narrower than a pixel.
static bool sector_span(const Sector &s, int y, int pad, int *xa, int *xb) {
    const float half = s_discHalf[y];
    if (half < 0) return false;
    const float Y = (float)y - s_pcy;
    float lo = -half, hi = half;
    bool in = true;
    if (fabsf(s.d0y) > 1e-6f) {
        const float b = Y * s.k0;
        if (s.d0y > 0) hi = fminf(hi, b); else lo = fmaxf(lo, b);
    } else if (s.d0x * Y < 0) {
        in = false;
    }
    if (fabsf(s.d1y) > 1e-6f) {
        const float b = Y * s.k1;
        if (s.d1y > 0) lo = fmaxf(lo, b); else hi = fminf(hi, b);
    } else if (Y * s.d1x > 0) {
        in = false;
    }
    if (!in || lo > hi) {
        if (fabsf(Y) > 2.0f) return false;
        lo = hi = 0;
    }
    *xa = (int)floorf(s_pcx + lo) - pad;
    *xb = (int)ceilf(s_pcx + hi) + pad;
    return true;
}

// A trail pixel over anything but black (rings, map, glyphs): kept out of line so the loop
// below has registers for its own values.
__attribute__((noinline)) static uint16_t trail_blend_px(uint16_t p, uint32_t opa, uint32_t d) {
    return blend565(p, s_swRing, opa, d);
}

// The trail over columns x..xe of one row, none of them masked: px and am point at column x.
// Same result per pixel as blend565(pixel, s_swRing, fade * s_trailMul / 256, dither) (see
// trail_tables).
static void trail_run(uint16_t *px, const uint16_t *am, int x, int xe, const uint8_t *bay) {
    const uint32_t u = (uint32_t)s_angU;
    const uint8_t *opaOf = s_trailOpa;
    const uint16_t *black = &s_trailBlack[0][0];
    for (; x <= xe; ++x, ++px, ++am) {
        const uint32_t m = *am;
        const uint32_t delta = (u - m) & (ANG_UNITS - 1);
        if (delta >= (uint32_t)TRAIL_U || m == ANG_OUTSIDE) continue;
        const uint32_t opa = opaOf[delta >> FADE_SHIFT];
        if (opa < 2) continue;
        const uint32_t d = bay[x & 3];
        const uint16_t p = *px;
        *px = (p || opa > SWEEP_TRAIL_OPA) ? trail_blend_px(p, opa, d) : black[(opa << 4) | d];
    }
}

// The sweep trail: the angular fade behind the lead at s_angU, outside the masked runs.
// The row's bearings are block-copied out of PSRAM first: loads from PSRAM one pixel at a
// time stall the loop several times longer than the copy takes.
static uint16_t s_amRow[SCREEN_W] __attribute__((aligned(4)));
static void trail_row(uint16_t *row, int x0, int y, int xa, int xb, const MaskRuns &mr) {
    int sa, sb;
    if (!sector_span(s_trailSec, y, 2, &sa, &sb)) return;
    sa = max(sa, xa);
    sb = min(sb, xb);
    if (sa > sb) return;
    const int ca = sa & ~1;                                // even: both rows stay word-aligned
    memcpy(s_amRow + ca, s_angMap + (size_t)y * SCREEN_W + ca, (size_t)(sb - ca + 1) * 2);
    const uint8_t *bay = &BAYER4[(y & 3) << 2];
    int x = sa;
    for (int i = 0; i <= mr.n && x <= sb; ++i) {          // the unmasked runs, left to right
        const int e = i < mr.n ? min(sb, mr.a[i] - 1) : sb;
        if (e >= x) trail_run(row + (x - x0), s_amRow + x, x, e, bay);
        if (i < mr.n) x = max(x, mr.b[i] + 1);
    }
}

// Columns of row y within reach of segment s's line, inside its box; false when none.
static bool seg_cols(const Seg &s, int y, int *xa, int *xb) {
    if (!s.box.valid || y < s.box.y1 || y > s.box.y2) return false;
    // (x - cx)*uy - fy*ux = +-reach at the edges of the band around the line
    const float reach = s.hw + 0.5f, fy = (float)y - s_pcy;
    int x1 = s.box.x1, x2 = s.box.x2;
    if (s.iuy != 0) {
        float a = s_pcx + (fy * s.ux - reach) * s.iuy, b = s_pcx + (fy * s.ux + reach) * s.iuy;
        if (a > b) { const float t = a; a = b; b = t; }
        x1 = max(x1, (int)floorf(a)); x2 = min(x2, (int)ceilf(b));
    } else if (fabsf(fy * s.ux) >= reach) {
        return false;                                // horizontal line, this row is off it
    }
    *xa = x1;
    *xb = x2;
    return x1 <= x2;
}

// Segment s, drawn by distance to the segment (anti-aliased, round ends), not as an angular
// slice: a slice is under a pixel wide near the centre, which made the lead look dotted
// there and shimmer as it turned.
static void seg_row(uint16_t *row, int x0, int y, int xa, int xb, const Seg &s, const MaskRuns &mr) {
    int x1, x2;
    if (!seg_cols(s, y, &x1, &x2)) return;
    x1 = max(x1, xa);
    x2 = min(x2, xb);
    const float reach = s.hw + 0.5f, fy = (float)y - s_pcy;
    const uint8_t *bay = &BAYER4[(y & 3) << 2];
    for (int x = x1; x <= x2; ++x) {
        // distance to the segment (clamping gives the round ends)
        const float fx = (float)x - s_pcx;
        float t = fx * s.ux + fy * s.uy;
        t = t < s.t0 ? s.t0 : (t > s.t1 ? s.t1 : t);
        const float ex = fx - t * s.ux, ey = fy - t * s.uy;
        const float cov = reach - sqrtf(ex * ex + ey * ey);
        if (cov <= 0) continue;
        if (mr.n && in_runs(mr, x)) continue;
        const uint32_t opa = (uint32_t)((cov > 1 ? 1 : cov) * (float)s.opa);
        if (opa >= 2) row[x - x0] = blend565(row[x - x0], s.col, opa, bay[x & 3]);
    }
}

// The centre ring. It is centred on the panel centre, which every rotation leaves in place,
// as LVGL centred it.
static void pulse_row(uint16_t *row, int x0, int y, int xa, int xb, const MaskRuns &mr) {
    if (y < s_pulseBox.y1 || y > s_pulseBox.y2) return;
    xa = max(xa, s_pulseBox.x1);
    xb = min(xb, s_pulseBox.x2);
    const float c = (SCREEN_W - 1) * 0.5f;
    const float ro = s_pulseR, ri = ro - PULSE_W;
    const uint8_t *bay = &BAYER4[(y & 3) << 2];
    const float fy = (float)y - c;
    // Only the columns within the ring's outer edge and outside its hole need the
    // per-pixel distance: two spans per row, found with two square roots.
    const float o2 = (ro + 0.5f) * (ro + 0.5f) - fy * fy;
    if (o2 <= 0) return;
    const float ow = sqrtf(o2);
    const float i2 = ri > 0.5f ? (ri - 0.5f) * (ri - 0.5f) - fy * fy : 0;
    const float iw = i2 > 0 ? sqrtf(i2) : -1;
    const int x1 = max(xa, (int)floorf(c - ow)), x4 = min(xb, (int)ceilf(c + ow));
    const int x2 = iw < 0 ? x4 : (int)ceilf(c - iw), x3 = iw < 0 ? x4 + 1 : (int)floorf(c + iw);
    for (int x = x1; x <= x4; ++x) {
        if (x > x2 && x < x3) { x = x3 - 1; continue; }   // jump over the hole
        const float fx = (float)x - c;
        const float d = sqrtf(fx * fx + fy * fy);
        float cov = min(d - ri, ro - d) + 0.5f;      // anti-aliased inner and outer edges
        if (cov <= 0) continue;
        if (cov > 1) cov = 1;
        if (d < 4.5f) cov *= max(d - 3.5f, 0.0f);    // stay under the centre dot (7 px)
        const uint32_t opa = (uint32_t)(cov * (float)s_pulseOpa);
        if (opa < 2) continue;
        if (mr.n && in_runs(mr, x)) continue;
        row[x - x0] = blend565(row[x - x0], s_pulseInk, opa, bay[x & 3]);
    }
}

static Rect pulse_box(float ro) {
    const int c = (SCREEN_W - 1) / 2, e = (int)ceilf(ro) + 2;
    Rect r = { (c - e) & ~1, (c - e) & ~1, (c + e + 1) | 1, (c + e + 1) | 1, true };
    r.x1 = max(r.x1, 0); r.y1 = max(r.y1, 0);
    r.x2 = min(r.x2, SCREEN_W - 1); r.y2 = min(r.y2, SCREEN_H - 1);
    return r;
}

// Physical box of the wedge at angle `u`, padded and aligned for the CO5300 (even start,
// odd end).
static Rect wedge_box(int u) {
    float pcx, pcy;
    log_to_phys((float)SCREEN_CX, (float)SCREEN_CY, &pcx, &pcy);
    float minX = pcx, maxX = pcx, minY = pcy, maxY = pcy;
    const float toRad = 2.0f * (float)M_PI / (float)ANG_UNITS;
    for (int i = 0; i <= 12; ++i) {
        const float a = (float)(u - s_trailEndU * i / 12) * toRad;   // the 3 px pad covers the lead's far half
        const float x = pcx + (RADAR_R_OUTER_PX + 2) * sinf(a);
        const float y = pcy - (RADAR_R_OUTER_PX + 2) * cosf(a);
        minX = min(minX, x); maxX = max(maxX, x);
        minY = min(minY, y); maxY = max(maxY, y);
    }
    Rect r = { ((int)minX - 3) & ~1, ((int)minY - 3) & ~1, ((int)maxX + 3) | 1, ((int)maxY + 3) | 1, true };
    r.x1 = max(r.x1, 0); r.y1 = max(r.y1, 0);
    r.x2 = min(r.x2, SCREEN_W - 1); r.y2 = min(r.y2, SCREEN_H - 1);
    return r;
}

// Physical box of segment `s`, padded and aligned like the wedge's.
static Rect seg_box(const Seg &s) {
    const float ax = s_pcx + s.t0 * s.ux, ay = s_pcy + s.t0 * s.uy;
    const float bx = s_pcx + s.t1 * s.ux, by = s_pcy + s.t1 * s.uy;
    const float e = s.hw + 2.0f;
    Rect r = { (int)floorf(min(ax, bx) - e) & ~1, (int)floorf(min(ay, by) - e) & ~1,
               (int)ceilf(max(ax, bx) + e) | 1, (int)ceilf(max(ay, by) + e) | 1, true };
    r.x1 = max(r.x1, 0); r.y1 = max(r.y1, 0);
    r.x2 = min(r.x2, SCREEN_W - 1); r.y2 = min(r.y2, SCREEN_H - 1);
    if (r.x1 > r.x2 || r.y1 > r.y2) r.valid = false;
    return r;
}

// A segment at logical angle `deg` (0 = up on the LVGL screen, clockwise): the display
// rotation is added here, as for the sweep, so it lines up with the LVGL dial.
static Seg make_seg(float deg, float t0, float t1, float width, uint16_t col, uint32_t opa) {
    const float a = (deg + (float)s_rot) * ((float)M_PI / 180.0f);
    Seg s = { sinf(a), -cosf(a), t0, t1, width * 0.5f, col, opa, {} };
    s.box = seg_box(s);
    s.iuy = fabsf(s.uy) > 1e-4f ? 1.0f / s.uy : 0;
    return s;
}

// The clock's hour hand, minute hand and centre cap for frame `f` (all off without one).
static void clock_parts(const ClockFrame *f, Part out[PARTS]) {
    const float R = (float)RADAR_R_OUTER_PX;
    for (int i = 0; i < PARTS; ++i) out[i] = Part{};
    if (!f) return;
    if (f->grow > 0.001f) {
        const float deg[2] = { f->hourDeg, f->minDeg };
        const float len[2] = { R * CLOCK_HOUR_LEN * f->grow, R * CLOCK_MIN_LEN * f->grow };
        const int   w[2]   = { CLOCK_HOUR_W, CLOCK_MIN_W };
        for (int k = 0; k < 2; ++k) {
            Part &p = out[PART_HOUR + k];
            p.on = true;
            p.deg = deg[k];
            p.size = len[k];
            p.seg[0] = make_seg(deg[k], 0, len[k], (float)(w[k] + CLOCK_OUTLINE_W), 0x0000, CLOCK_OUTLINE_OPA);
            p.seg[1] = make_seg(deg[k], 0, len[k], (float)w[k], s_pulseInk, 255);
        }
    }
    if (f->capR > 0) {
        Part &p = out[PART_CAP];
        p.on = true;
        p.size = f->capR + f->capDot;                  // either changing means a re-push
        p.seg[0] = make_seg(0, 0, 0, 2.0f * f->capR, s_pulseInk, 255);
        p.seg[1] = make_seg(0, 0, 0, 2.0f * CLOCK_CAP_DOT_R, s_swLead, (uint32_t)lroundf(f->capDot * 255.0f));
    }
}

// Re-push only parts that visibly changed: on/off, colour (sweepConfig), 0.1 degree of
// turn, or a quarter pixel.
static bool part_changed(const Part &a, const Part &b) {
    if (a.on != b.on) return true;
    if (!a.on) return false;
    if (a.seg[0].col != b.seg[0].col || a.seg[1].col != b.seg[1].col) return true;
    return fabsf(a.deg - b.deg) >= 0.1f || fabsf(a.size - b.size) >= 0.25f;
}

static inline bool box_hits(const Rect &b, int y, int xa, int xb) {
    return b.valid && y >= b.y1 && y <= b.y2 && xb >= b.x1 && xa <= b.x2;
}

// Does any overlay reach row y between columns xa and xb? Rows that miss skip the blending.
static bool overlays_hit(int y, int xa, int xb) {
    if (s_trailOn && box_hits(s_trailBox, y, xa, xb)) return true;
    if (s_leadOn && box_hits(s_lead.box, y, xa, xb)) return true;
    for (const Part &p : s_part)
        if (p.on && (box_hits(p.seg[0].box, y, xa, xb) || box_hits(p.seg[1].box, y, xa, xb))) return true;
    return s_pulseOn && box_hits(s_pulseBox, y, xa, xb);
}

// Blend every overlay into one row (layout as above), bottom to top: sweep trail, hour hand,
// minute hand, lead line / second hand, cap, centre ripple.
static void overlays_row(uint16_t *row, int x0, int y, int xa, int xb) {
    MaskRuns mr;
    row_mask_runs(y, mr);
    if (s_trailOn && box_hits(s_trailBox, y, xa, xb))
        trail_row(row, x0, y, max(xa, s_trailBox.x1), min(xb, s_trailBox.x2), mr);
    for (int i = PART_HOUR; i <= PART_MIN; ++i)
        if (s_part[i].on)
            for (const Seg &s : s_part[i].seg) seg_row(row, x0, y, xa, xb, s, mr);
    if (s_leadOn) seg_row(row, x0, y, xa, xb, s_lead, mr);
    if (s_part[PART_CAP].on)
        for (const Seg &s : s_part[PART_CAP].seg) seg_row(row, x0, y, xa, xb, s, mr);
    if (s_pulseOn) pulse_row(row, x0, y, xa, xb, mr);
}

// RGB565 -> panel byte order, two pixels per 32-bit word. Every row the panel takes starts
// at an even column of a 4-byte-aligned buffer (even windows, even SCREEN_W).
static inline void swap_row(uint16_t *dst, const uint16_t *src, int w) {
    const uint32_t *s = (const uint32_t *)src;
    uint32_t *d = (uint32_t *)dst;
    for (int i = 0, n = w >> 1; i < n; ++i) {
        const uint32_t v = s[i];
        d[i] = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    }
    if (w & 1) dst[w - 1] = (uint16_t)((src[w - 1] >> 8) | (src[w - 1] << 8));
}

// One window's pixels: LVGL output (px, rows of w from row y) or, with px null, s_phys.
struct PushSrc { uint16_t *px; int x, y, w; };

// s_phys only has to match the panel while the compositor draws (the radar tile on screen,
// with a sweep, a ripple or the clock). Saving every LVGL strip into PSRAM costs about as much
// as pushing it, so page swipes and the other pages skip it; back on the radar, one full
// repaint refills it before the compositor draws again (sweep_poll_ui, monitor_cb).
static bool s_physNeed = false;     // keep s_phys current
static bool s_physOk = false;       // s_phys matches the panel
static bool s_physResync = false;   // a full repaint to refill it is on its way
static bool s_physSkipped = false;  // ...and a strip went out unsaved since it was asked for

// The way out for every row of LVGL output (src, physical row y, columns x..x+w-1): saved
// into s_phys as it is, the overlays blended over it in place, then turned into panel byte
// order in dst. dst may be src itself (pushInPlace, row_from_lvgl_turned).
static inline void lvgl_row_out(uint16_t *dst, uint16_t *src, int x, int y, int w) {
    const int xb = x + w - 1;
    if (s_physNeed) {
        memcpy(s_phys + (size_t)y * SCREEN_W + x, src, (size_t)w * 2);
        if (overlays_hit(y, x, xb)) overlays_row(src, x, y, x, xb);
    } else {
        s_physOk = false;
        s_physSkipped = true;
    }
    swap_row(dst, src, w);
}

static void row_from_lvgl(uint16_t *dst, int y, void *ctx) {
    const PushSrc &p = *(const PushSrc *)ctx;
    lvgl_row_out(dst, p.px + (size_t)(y - p.y) * p.w, p.x, y, p.w);
}

// At 90° and 270° an LVGL block (w x h) lands transposed, in an h-wide, w-tall window at
// (x, y): each physical row is one column of the block, read straight out of LVGL's draw
// buffer into the driver's DMA buffer. 90° (cw) takes column r bottom-up, 270° takes
// column w-1-r top-down. (Locals, not t's fields, in the loop, so each field is read once
// whatever the compiler's aliasing rules.)
struct TurnSrc { const uint16_t *px; int w, h, x, y; bool cw; };
static void row_from_lvgl_turned(uint16_t *dst, int y, void *ctx) {
    const TurnSrc &t = *(const TurnSrc *)ctx;
    const int w = t.w, h = t.h, r = y - t.y;
    const int step = t.cw ? -w : w;
    const uint16_t *s = t.cw ? t.px + (size_t)(h - 1) * w + r : t.px + (w - 1 - r);
    for (int c = 0; c < h; ++c, s += step) dst[c] = *s;
    lvgl_row_out(dst, dst, t.x, y, h);
}

// s_phys with the overlays blended over a copy of the row.
static void row_from_phys(uint16_t *dst, int y, void *ctx) {
    const PushSrc &p = *(const PushSrc *)ctx;
    const uint16_t *src = s_phys + (size_t)y * SCREEN_W + p.x;
    const int xb = p.x + p.w - 1;
    if (overlays_hit(y, p.x, xb)) {
        memcpy(s_rowBuf, src, (size_t)p.w * 2);
        overlays_row(s_rowBuf, p.x, y, p.x, xb);
        src = s_rowBuf;
    }
    swap_row(dst, src, p.w);
}

// Every LVGL block goes through here: remember it in s_phys, blend the overlays into the
// part that lies under them, then push. LVGL's draw buffer is internal DMA RAM, so the panel
// reads the converted rows from it directly; a block in PSRAM (the arbitrary-angle scratch,
// or the draw buffer's PSRAM fallback) goes through the driver's own buffers.
static void draw_block(int16_t x, int16_t y, lv_color_t *pixels, uint16_t w, uint16_t h) {
    perf::Scope t(perf::PUSH);
    PushSrc p = { (uint16_t *)pixels, x, y, w };
    if (co5300::canSendInPlace(pixels, w)) co5300::pushInPlace(x, y, w, h, p.px, row_from_lvgl, &p);
    else co5300::push(x, y, w, h, row_from_lvgl, &p);
}

// The same for a block at 90°/270° (row_from_lvgl_turned), through the driver's buffers.
static void draw_block_turned(const lv_area_t *a, const lv_color_t *pixels, bool cw) {
    perf::Scope t(perf::PUSH);
    const int w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
    TurnSrc s = { (const uint16_t *)pixels, w, h,
                  cw ? SCREEN_H - 1 - a->y2 : a->y1, cw ? a->x1 : SCREEN_W - 1 - a->x2, cw };
    co5300::push(s.x, s.y, h, w, row_from_lvgl_turned, &s);
}

// ---- one compositor tick's dirty pixels: a column span per physical row ----
static void spans_reset() {
    for (int y = 0; y < SCREEN_H; ++y) { s_spanL[y] = SCREEN_W; s_spanR[y] = -1; }
    s_spanY1 = SCREEN_H;
    s_spanY2 = -1;
}
static void span_add(int y, int xa, int xb) {
    if (y < 0 || y >= SCREEN_H) return;
    xa = max(xa, 0);
    xb = min(xb, SCREEN_W - 1);
    if (xa > xb) return;
    if (xa < s_spanL[y]) s_spanL[y] = (int16_t)xa;
    if (xb > s_spanR[y]) s_spanR[y] = (int16_t)xb;
    s_spanY1 = min(s_spanY1, y);
    s_spanY2 = max(s_spanY2, y);
}
static void spans_add_rect(const Rect &r) {
    if (!r.valid) return;
    for (int y = r.y1; y <= r.y2; ++y) span_add(y, r.x1, r.x2);
}
static void spans_add_trail(const Sector &s, const Rect &box) {
    if (!box.valid) return;
    for (int y = box.y1; y <= box.y2; ++y) {
        int xa, xb;
        if (sector_span(s, y, 2, &xa, &xb)) span_add(y, max(xa, box.x1), min(xb, box.x2));
    }
}
static void spans_add_seg(const Seg &s) {
    for (int y = s.box.y1; s.box.valid && y <= s.box.y2; ++y) {
        int xa, xb;
        if (seg_cols(s, y, &xa, &xb)) span_add(y, xa, xb);
    }
}

// Re-push the dirty spans from s_phys with the overlays blended in: one window per band of
// COMP_BAND rows, as wide as the band's widest span, aligned for the CO5300 (even start, odd end).
static void spans_push() {
    perf::Scope t(perf::PUSH);
    for (int y1 = s_spanY1 & ~1; y1 <= s_spanY2; y1 += COMP_BAND) {
        const int y2 = min(y1 + COMP_BAND - 1, SCREEN_H - 1);
        int xa = SCREEN_W, xb = -1;
        for (int y = y1; y <= y2; ++y) { xa = min(xa, (int)s_spanL[y]); xb = max(xb, (int)s_spanR[y]); }
        if (xa > xb) continue;
        xa &= ~1;
        xb = min(xb | 1, SCREEN_W - 1);
        PushSrc p = { nullptr, xa, y1, xb - xa + 1 };
        co5300::push(xa, y1, p.w, y2 - y1 + 1, row_from_phys, &p);
    }
}

static void sweep_poll_ui() {
    s_swVisible = ui_sweep_visible();
    int16_t m[SWEEP_MASKS][5];
    const int n = ui_sweep_masks(m, SWEEP_MASKS);
    s_maskN = 0;
    for (int i = 0; i < n; ++i) {              // logical box -> physical bounding box
        const float xs[4] = { (float)m[i][0], (float)m[i][2], (float)m[i][2], (float)m[i][0] };
        const float ys[4] = { (float)m[i][1], (float)m[i][1], (float)m[i][3], (float)m[i][3] };
        float x1 = 1e9f, y1 = 1e9f, x2 = -1e9f, y2 = -1e9f;
        for (int k = 0; k < 4; ++k) {
            float px, py;
            log_to_phys(xs[k], ys[k], &px, &py);
            x1 = min(x1, px); y1 = min(y1, py); x2 = max(x2, px); y2 = max(y2, py);
        }
        float rad = (float)m[i][4];                  // LVGL caps the radius at half the short side
        rad = min(rad, (float)(m[i][2] - m[i][0] + 1) * 0.5f);
        rad = min(rad, (float)(m[i][3] - m[i][1] + 1) * 0.5f);
        s_mask[s_maskN++] = { { (int)floorf(x1) - 1, (int)floorf(y1) - 1, (int)ceilf(x2) + 1, (int)ceilf(y2) + 1, true },
                              (float)m[i][0], (float)m[i][1], (float)m[i][2], (float)m[i][3], rad };
    }
    s_physNeed = s_swVisible && (s_swWanted || s_pulseWanted || radar::clockActive());
    if (s_physNeed && !s_physOk && !s_physResync) {   // refill s_phys: one full repaint
        s_physResync = true;
        s_physSkipped = false;
        lv_obj_invalidate(lv_scr_act());
    }
}

static bool sweep_ready() { return s_phys && s_angMap; }

static void compose_step() {
    if (!sweep_ready() || !s_physOk) return;
    const bool sweepOn = s_swWanted && s_swVisible;
    const bool pulseOn = s_pulseWanted && s_swVisible;
    const uint32_t now = millis();
    // A tick every SWEEP_FRAME_MS, or at once when the sweep or the ripple is switched. The
    // clock is only looked at on ticks: it changes the picture at most that often anyway.
    const bool switched = sweepOn != s_sweepWas || pulseOn != s_pulseWas;
    if (!switched && now - s_lastComposeMs < SWEEP_FRAME_MS) return;
    s_sweepWas = sweepOn;
    s_pulseWas = pulseOn;
    ClockFrame cf;
    const bool clk = s_swVisible && radar::clockFrame(cf);
    bool anyOn = s_trailOn || s_leadOn || s_pulseOn;
    for (const Part &p : s_part) anyOn = anyOn || p.on;
    if (!sweepOn && !pulseOn && !clk && !anyOn) return;   // nothing to draw, nothing to clear
    perf::Scope t(perf::COMPOSE);
    // Advance by elapsed time, but never more than SWEEP_MAX_STEP_MS per tick: a blocked
    // loop (GPS read, full repaint) then pauses the sweep briefly instead of making it jump.
    uint32_t dt = anyOn ? now - s_lastComposeMs : 0;
    if (dt > SWEEP_MAX_STEP_MS) dt = SWEEP_MAX_STEP_MS;
    s_lastComposeMs = now;

    // The sweep line. On the clock it is the second hand, and the free-running phase follows
    // it so the radar sweep goes on from the hand afterwards; otherwise it turns on its own.
    // The clock also draws it with the radar sweep off (Orb, or switched off): no trail then.
    const bool trailOn = sweepOn && (!clk || cf.trailOpa > 0);
    const bool leadOn  = clk ? cf.leadLen > 0.001f : sweepOn;
    if (clk)
        s_swPhase = (uint32_t)(clock_wrap360(cf.leadDeg) * ((float)(ANG_UNITS * 256) / 360.0f)) & (ANG_UNITS * 256 - 1);
    else if (sweepOn)
        s_swPhase = (s_swPhase + (uint32_t)(((uint64_t)dt * ANG_UNITS * 256) / SWEEP_COMP_PERIOD_MS)) & (ANG_UNITS * 256 - 1);
    const int u = (int)((s_swPhase >> 8) + ((uint32_t)s_rot * ANG_UNITS) / 360) & (ANG_UNITS - 1);
    s_trailMul = clk ? cf.trailOpa : 256;
    trail_tables();                                    // before the new sector: it sets the trail's extent

    // Dirty spans: what the old overlays covered (to restore) and what the new ones cover.
    spans_reset();
    if (s_trailOn) spans_add_trail(s_trailSec, s_trailBox);
    if (s_leadOn) spans_add_seg(s_lead);
    if (trailOn || leadOn) {
        const float R = (float)RADAR_R_OUTER_PX;
        const float a = (float)u * (2.0f * (float)M_PI / (float)ANG_UNITS);   // 0 = up, clockwise, as in the bearing map
        s_lead = Seg{ sinf(a), -cosf(a), clk ? -R * cf.leadTail : 0.0f, clk ? R * cf.leadLen : R,
                      (clk ? cf.leadW : SWEEP_LEAD_W) * 0.5f, s_swLead, SWEEP_LEAD_OPA, {}, 0 };
        s_lead.box = seg_box(s_lead);
        s_lead.iuy = fabsf(s_lead.uy) > 1e-4f ? 1.0f / s_lead.uy : 0;
        s_angU = u;
        s_trailBox = trailOn ? wedge_box(u) : Rect{};
        s_trailSec = trail_sector(u);
        if (trailOn) spans_add_trail(s_trailSec, s_trailBox);
        if (leadOn) spans_add_seg(s_lead);
    }
    s_trailOn = trailOn;
    s_leadOn = leadOn;

    // The clock's hands and cap: each re-pushed (old + new) only when it visibly changed.
    Part np[PARTS];
    clock_parts(clk ? &cf : nullptr, np);
    for (int i = 0; i < PARTS; ++i) {
        if (!part_changed(s_part[i], np[i])) continue;
        if (s_part[i].on) for (const Seg &s : s_part[i].seg) spans_add_seg(s);
        if (np[i].on) for (const Seg &s : np[i].seg) spans_add_seg(s);
        s_part[i] = np[i];
    }

    const bool pOn = pulseOn && !clk;                  // the cap sits where the ripple would be
    if (s_pulseOn) spans_add_rect(s_pulseBox);
    if (pOn) {
        s_pulseMs = (s_pulseMs + dt) % PULSE_PERIOD_MS;
        const float f = (float)s_pulseMs / (float)PULSE_PERIOD_MS;
        s_pulseR = (PULSE_D_MIN + f * (PULSE_D_MAX - PULSE_D_MIN)) * 0.5f;
        s_pulseOpa = (uint32_t)((1.0f - f) * PULSE_OPA);
        s_pulseBox = pulse_box(s_pulseR);
        spans_add_rect(s_pulseBox);
    } else {
        s_pulseBox.valid = false;
    }
    s_pulseOn = pOn;
    spans_push();
}

// Render the physical bounding box affected by a logical dirty rectangle. Sampling
// from the full logical framebuffer avoids gaps/overdraw artifacts that forward-mapping
// individual source pixels would create at non-cardinal angles.
static void flush_arbitrary(const lv_area_t *area) {
    const float cx = (SCREEN_W - 1) * 0.5f;
    const float cy = (SCREEN_H - 1) * 0.5f;
    const float xs[4] = {(float)area->x1, (float)area->x2, (float)area->x2, (float)area->x1};
    const float ys[4] = {(float)area->y1, (float)area->y1, (float)area->y2, (float)area->y2};
    float minX = (float)SCREEN_W, minY = (float)SCREEN_H, maxX = -1.0f, maxY = -1.0f;
    for (int i = 0; i < 4; ++i) {
        const float rx = xs[i] - cx;
        const float ry = ys[i] - cy;
        const float dx = cx + s_rotCos * rx - s_rotSin * ry;
        const float dy = cy + s_rotSin * rx + s_rotCos * ry;
        if (dx < minX) minX = dx;
        if (dx > maxX) maxX = dx;
        if (dy < minY) minY = dy;
        if (dy > maxY) maxY = dy;
    }

    // Include nearest-neighbour edge pixels and preserve the panel's required 2-pixel
    // alignment (even start, odd end) in both axes.
    int x1 = (int)floorf(minX) - 1;
    int y1 = (int)floorf(minY) - 1;
    int x2 = (int)ceilf(maxX) + 1;
    int y2 = (int)ceilf(maxY) + 1;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    x1 &= ~1;
    y1 &= ~1;
    x2 |= 1;
    y2 |= 1;
    if (x2 >= SCREEN_W) x2 = SCREEN_W - 1;
    if (y2 >= SCREEN_H) y2 = SCREEN_H - 1;
    if (x1 > x2 || y1 > y2) return;

    const int outW = x2 - x1 + 1;
    const lv_color_t black = lv_color_black();
    const int64_t centerX2Q16 = (int64_t)(SCREEN_W - 1) * 65536;
    const int64_t centerY2Q16 = (int64_t)(SCREEN_H - 1) * 65536;
    const int32_t stepX = s_rotCosQ16 * 2;
    const int32_t stepY = -s_rotSinQ16 * 2;

    // Batch scanlines while keeping every physical write 2-pixel aligned. Fewer QSPI
    // transactions matter here because an arbitrary-angle dirty box can be fairly large.
    for (int dy = y1; dy <= y2; ) {
        int rows = y2 - dy + 1;
        if (rows > LVGL_BUF_LINES) rows = LVGL_BUF_LINES;
        rows &= ~1;
        for (int row = 0; row < rows; ++row) {
            const int py = dy + row;
            const int relX2 = 2 * x1 - (SCREEN_W - 1);
            const int relY2 = 2 * py - (SCREEN_H - 1);
            int64_t srcX2Q16 = centerX2Q16 + (int64_t)s_rotCosQ16 * relX2
                                                + (int64_t)s_rotSinQ16 * relY2;
            int64_t srcY2Q16 = centerY2Q16 - (int64_t)s_rotSinQ16 * relX2
                                                + (int64_t)s_rotCosQ16 * relY2;
            lv_color_t *out = s_rotBuf + row * outW;
            for (int dx = 0; dx < outW; ++dx) {
                const int sx = (int)((srcX2Q16 + 65536) >> 17);
                const int sy = (int)((srcY2Q16 + 65536) >> 17);
                out[dx] = (sx >= 0 && sx < SCREEN_W && sy >= 0 && sy < SCREEN_H)
                            ? s_frameBuf[sy * SCREEN_W + sx]
                            : black;
                srcX2Q16 += stepX;
                srcY2Q16 += stepY;
            }
        }
        draw_block((int16_t)x1, (int16_t)dy, s_rotBuf, (uint16_t)outW, (uint16_t)rows);
        dy += rows;
    }
}

// LVGL -> panel, applying the chosen rotation while pushing.
//   0°   : straight through.
//   180° : reverse the flat block in place — no scratch buffer.
//   90°/270° : the block transposes (w<->h); its columns are read straight into the DMA
//              buffers as the panel's rows (row_from_lvgl_turned) — no scratch buffer.
//   other: update the logical framebuffer and inverse-sample the rotated dirty bounds.
static void compose_step();
static void touch_sample(bool force);
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    // Let the sweep tick between strips of a long LVGL frame (a full repaint takes ~0.1 s).
    // Safe mid-frame: strips already flushed are in s_phys and on the panel; the rest of
    // s_phys still matches what the panel shows, so the composite is never wrong.
    compose_step();
    // Sample the touch panel too: LVGL reads it only between frames (see touch_queue.h).
    touch_sample(false);
    perf::Scope t(perf::FLUSH);
    const int w = (int)(area->x2 - area->x1 + 1);
    const int h = (int)(area->y2 - area->y1 + 1);
    const uint16_t angle = s_rot;
    const bool arbitrary = (angle != 0 && angle != 90 && angle != 180 && angle != 270);
    // Mirror into the logical framebuffer ONLY at non-cardinal angles (it's what
    // flush_arbitrary samples from). At 0/90/180/270 this copy would be pure overhead
    // on every frame for every user; skipping it keeps those paths exactly as before.
    // Switching TO an arbitrary angle invalidates the whole screen (see setRotation),
    // so the framebuffer is fully repopulated before it is ever sampled.
    if (arbitrary && s_frameBuf) {
        for (int row = 0; row < h; ++row) {
            memcpy(s_frameBuf + (area->y1 + row) * SCREEN_W + area->x1,
                   px + row * w, (size_t)w * sizeof(lv_color_t));
        }
    }

    if (arbitrary && s_frameBuf && s_rotBuf) {
        flush_arbitrary(area);
        if (lv_disp_flush_is_last(drv)) s_frameCount++;
        lv_disp_flush_ready(drv);
        return;
    }

    switch (angle) {
        case 180:
            for (int i = 0, j = w * h - 1; i < j; ++i, --j) { lv_color_t t = px[i]; px[i] = px[j]; px[j] = t; }
            draw_block((int16_t)(SCREEN_W - 1 - area->x2), (int16_t)(SCREEN_H - 1 - area->y2), px,
                       (uint16_t)w, (uint16_t)h);
            break;
        case 90:
        case 270:
            draw_block_turned(area, px, angle == 90);
            break;
        default:                                     // 0°
            draw_block(area->x1, area->y1, px, (uint16_t)w, (uint16_t)h);
            break;
    }
    if (lv_disp_flush_is_last(drv)) s_frameCount++;
    lv_disp_flush_ready(drv);
}

// CO5300 (QSPI) requires 2-pixel-aligned flush windows: even start, odd end.
// Without this, partial-area updates (e.g. the radar sweep) tear / ghost / flicker.
static void rounder_cb(lv_disp_drv_t *drv, lv_area_t *area) {
    (void)drv;
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

// LVGL monitor_cb: one screen refresh done (GET /perf).
static void monitor_cb(lv_disp_drv_t *drv, uint32_t ms, uint32_t px) {
    (void)drv;
    perf::refresh(ms, px);
    if (s_physResync && px >= (uint32_t)SCREEN_W * SCREEN_H) {   // the refill repaint is done
        s_physResync = false;
        s_physOk = !s_physSkipped;
    }
}

// CST9217 / FT3168 touch -> LVGL pointer, through TouchQueue (see touch_queue.h).
static TouchQueue s_touchQ;
static bool       s_touchOk = false;            // a touch controller is registered with LVGL
static uint32_t   s_touchLastMs = 0;            // when the panel was last sampled
static bool       s_touchReplaying = false;     // LVGL is calling back for the queued samples
static bool       s_touchHeld = false;          // the shared I2C bus is busy (GPS read): no panel reads

// Physical panel point -> logical UI point (inverse of the display rotation). False for the
// black corners outside the logical UI at arbitrary angles.
static bool touch_to_logical(uint16_t x, uint16_t y, int *ox, int *oy) {
    int lx = x, ly = y;
    const uint16_t angle = s_rot;
    switch (angle) {
        case 90:  lx = y;                        ly = SCREEN_H - 1 - x; break;
        case 180: lx = SCREEN_W - 1 - x;         ly = SCREEN_H - 1 - y; break;
        case 270: lx = SCREEN_W - 1 - y;         ly = x; break;
        default:
            if (angle != 0) {
                const int relX2 = 2 * (int)x - (SCREEN_W - 1);
                const int relY2 = 2 * (int)y - (SCREEN_H - 1);
                const int64_t sx2q = (int64_t)(SCREEN_W - 1) * 65536
                                   + (int64_t)s_rotCosQ16 * relX2
                                   + (int64_t)s_rotSinQ16 * relY2;
                const int64_t sy2q = (int64_t)(SCREEN_H - 1) * 65536
                                   - (int64_t)s_rotSinQ16 * relX2
                                   + (int64_t)s_rotCosQ16 * relY2;
                lx = (int)((sx2q + 65536) >> 17);
                ly = (int)((sy2q + 65536) >> 17);
            }
            break;
    }
    if (lx < 0 || lx >= SCREEN_W || ly < 0 || ly >= SCREEN_H) return false;
    *ox = lx;
    *oy = ly;
    return true;
}

// Read the panel into the queue: always from the read callback (force), and at most every
// TOUCH_SAMPLE_MS between flush strips. Both run on the LVGL task. Not while the GPS read
// owns the I2C bus (loopWithoutTouch): LVGL then keeps the last state it was given, so a
// finger that stays down is not mistaken for a lift.
static void touch_sample(bool force) {
    if (!s_touchOk || s_touchHeld) return;
    const uint32_t now = millis();
    if (!force && now - s_touchLastMs < TOUCH_SAMPLE_MS) return;
    if (s_touchLastMs) perf::add(perf::TOUCH_GAP, (now - s_touchLastMs) * 1000);
    s_touchLastMs = now;
    uint16_t x, y;
    int lx, ly;
    switch (touch_read(&x, &y)) {
        case TOUCH_NODATA: s_touchQ.add(TouchQueue::NODATA, 0, 0, now); break;
        case TOUCH_UP:     s_touchQ.add(TouchQueue::UP, 0, 0, now); break;
        case TOUCH_DOWN:
            if (touch_to_logical(x, y, &lx, &ly))
                s_touchQ.add(TouchQueue::DOWN, (int16_t)lx, (int16_t)ly, now);
            else
                s_touchQ.add(TouchQueue::UP, 0, 0, now);   // black corners are outside the logical UI
            // A press held back from LVGL (wake-only) is still a touch: it restarts LVGL's
            // inactivity time, which wakes the screen and keeps it lit while the finger rests.
            if (s_touchQ.holding()) lv_disp_trig_activity(NULL);
            break;
    }
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    // Take the current state once per LVGL read, not again for each queued sample LVGL
    // asks for: a finger that keeps moving would otherwise keep the queue from emptying.
    if (!s_touchReplaying) touch_sample(true);
    TouchQueue::Sample s;
    if (!s_touchQ.next(s)) s = s_touchQ.delivered();   // nothing new: same state as before
    data->point.x = s.x;
    data->point.y = s.y;
    data->state = s.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = !s_touchQ.empty();
    s_touchReplaying = data->continue_reading;
}

namespace display {

bool begin() {
    Serial.println("[display] init CO5300 QSPI...");
    if (!co5300::begin()) {
        Serial.println("[display] co5300::begin() FAILED");
        return false;
    }
    co5300::setBrightness(BRIGHTNESS_DEFAULT);
    Serial.println("[display] panel up; init LVGL...");

    lv_init();

    // Draw scratch in INTERNAL DMA RAM: rendering anti-aliased graphics into PSRAM is
    // slow (that, not QSPI bandwidth, was the bottleneck). Keep the active buffer in fast
    // internal SRAM; single partial buffer to stay within the internal-RAM budget.
    const size_t buf_px = (size_t)SCREEN_W * LVGL_BUF_LINES;
    s_buf1 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_buf2 = nullptr;
    if (!s_buf1) {
        Serial.println("[display] internal draw buffer failed; falling back to PSRAM");
        s_buf1 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    }
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, buf_px);

    // Rotation buffers live in PSRAM so the internal contiguous block needed by TLS remains
    // available. The full logical framebuffer makes inverse sampling at arbitrary angles
    // possible without holes; the smaller scratch holds the output batches.
    s_rotBuf = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    s_frameBuf = (lv_color_t *)heap_caps_calloc((size_t)SCREEN_W * SCREEN_H,
                                                sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!s_rotBuf || !s_frameBuf) {
        Serial.println("[display] WARNING: rotation buffer allocation failed; arbitrary angles unavailable");
    }
    // Sweep compositor buffers (PSRAM; ~0.9 MB). Without them the sweep stays in LVGL.
    s_phys    = (uint16_t *)heap_caps_calloc((size_t)SCREEN_W * SCREEN_H, 2, MALLOC_CAP_SPIRAM);
    s_angMap  = (uint16_t *)heap_caps_malloc((size_t)SCREEN_W * SCREEN_H * 2, MALLOC_CAP_SPIRAM);
    if (sweep_ready()) {
        const uint32_t t0 = millis();
        sweep_build_map();
        sweep_build_lut();
        Serial.printf("[display] sweep compositor ready (map %lu ms)\n", (unsigned long)(millis() - t0));
    } else {
        heap_caps_free(s_phys); heap_caps_free(s_angMap);
        s_phys = s_angMap = nullptr;
        Serial.println("[display] WARNING: compositor buffers failed; sweep stays in LVGL");
    }

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res  = SCREEN_W;
    s_disp_drv.ver_res  = SCREEN_H;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.rounder_cb = rounder_cb;     // CO5300 needs 2-px-aligned windows
    s_disp_drv.monitor_cb = monitor_cb;     // GET /perf: refresh timing
    s_disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&s_disp_drv);

    // Touch input (CST9217) -> LVGL pointer indev (drives tap-to-inspect + swipe).
    if (touch_begin()) {
        lv_indev_drv_init(&s_indev_drv);
        s_indev_drv.type = LV_INDEV_TYPE_POINTER;
        s_indev_drv.read_cb = touch_read_cb;
        lv_indev_drv_register(&s_indev_drv);
        s_touchOk = true;
        Serial.println("[display] touch registered (" BOARD_NAME ")");
    }

    Serial.printf("[display] PSRAM free: %u KB\n", (unsigned)(ESP.getFreePsram() / 1024));
    ui_create();                   // M3: radar/list/stats views + tap-to-inspect
    Serial.println("[display] LVGL ready");
    return true;
}

void loop() {
    if (sweep_ready()) sweep_poll_ui();
    {
        perf::Scope t(perf::LVGL);
        lv_timer_handler();
    }
    compose_step();
}

void loopWithoutTouch() {
    s_touchHeld = true;
    loop();
    s_touchHeld = false;
}

void sweepConfig(uint16_t ring565, uint16_t lead565, uint16_t ink565, bool sweep, bool pulse) {
    s_swRing = ring565;
    s_swLead = lead565;
    s_swWanted = sweep;
    s_pulseInk = ink565;
    s_pulseWanted = pulse;
    trail_tables();                  // LVGL strips may blend the trail before the next tick
}
void sweepTick() { compose_step(); }
bool sweepAvailable() { return sweep_ready(); }
float sweepAngleDeg() { return (float)(s_swPhase >> 8) * (360.0f / (float)ANG_UNITS); }

void setBrightness(uint8_t v) {
    co5300::setBrightness(v);
    s_touchQ.setWakeOnly(v == 0);   // a dark panel takes a touch only as a wake-up
}

void setRotation(uint16_t degrees) {
    uint16_t normalized = (uint16_t)(degrees % 360);
    if (normalized != 0 && normalized != 90 && normalized != 180 && normalized != 270
        && (!s_frameBuf || !s_rotBuf)) {
        Serial.println("[display] arbitrary rotation unavailable without both PSRAM buffers");
        normalized = 0;
    }
    if (normalized == s_rot) return;
    const float radians = normalized * ((float)M_PI / 180.0f);
    s_rotCos = cosf(radians);
    s_rotSin = sinf(radians);
    s_rotCosQ16 = (int32_t)lroundf(s_rotCos * 65536.0f);
    s_rotSinQ16 = (int32_t)lroundf(s_rotSin * 65536.0f);
    s_rot = normalized;
    co5300::fill(0x0000);                        // clear pixels no longer covered after an angle change
    if (sweep_ready()) {                         // the compositor's copy must match the panel
        memset(s_phys, 0, (size_t)SCREEN_W * SCREEN_H * 2);
        s_trailOn = s_leadOn = false;
        s_trailBox.valid = false;
        for (Part &p : s_part) p.on = false;     // redrawn on the next tick in the new orientation
        s_pulseOn = false;
        s_pulseBox.valid = false;
        sweep_build_map();                       // the scope centre moves by a fraction of a pixel
    }
    lv_obj_t *scr = lv_scr_act();
    if (scr) lv_obj_invalidate(scr);   // full repaint in the new orientation
}
uint16_t rotation() { return s_rot; }

uint32_t inactiveMs() { return lv_disp_get_inactive_time(NULL); }

} // namespace display

#endif  // !BOARD_LCD21
