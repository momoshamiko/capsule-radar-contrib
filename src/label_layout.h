#pragma once
// Label placement for the round scope.
//
// Pure geometry, no LVGL or Arduino types, so the device build and the host tests
// run the exact same code. Each plane is offered six slots around its glyph and
// takes the first one that is fully inside the scope circle and keeps a 2 px gap
// from obstacles, from labels placed earlier in this call, and from every other
// plane's glyph. Items arrive in priority order, so an earlier plane wins a
// contested slot, and the output is a pure function of the input.

#include <stdint.h>

struct LlRect { int16_t x1, y1, x2, y2; };   // inclusive corners

struct LlItem {
    int16_t x, y;   // glyph centre on screen
    int16_t w, h;   // label block size (1 or 2 lines)
    int16_t g;      // glyph half-size (12, or 21 with a ring)
    int8_t  prev;   // slot held last frame; LL_NONE when the plane is new
};

enum LlSlot : int8_t { LL_NONE = -1, LL_R, LL_L, LL_BL, LL_BR, LL_AR, LL_AL };

// Fills slot[i] and box[i] for every item. Returns how many items got a place.
inline int ll_place(const LlItem *items, int n, const LlRect *obstacles, int nObs,
                    int cx, int cy, int r, int8_t *slot, LlRect *box) {
    constexpr int GAP = 2;
    // "Keeps a 2 px gap" becomes a plain intersection test once one rect is grown
    // by the gap on every side; growing either side gives the same answer.
    const auto hits = [](const LlRect &a, const LlRect &b) {
        return a.x1 - GAP <= b.x2 && b.x1 <= a.x2 + GAP &&
               a.y1 - GAP <= b.y2 && b.y1 <= a.y2 + GAP;
    };
    const auto in_circle = [cx, cy, r](int px, int py) {
        const int dx = px - cx, dy = py - cy;
        return dx * dx + dy * dy <= r * r;
    };

    int placed = 0;
    for (int i = 0; i < n; ++i) {
        const LlItem &it = items[i];

        // Candidate top-left corners, indexed by LlSlot.
        const struct { int16_t x, y; } tl[6] = {
            { int16_t(it.x + it.g + 3),          int16_t(it.y - 8) },               // LL_R
            { int16_t(it.x - it.g - 3 - it.w),   int16_t(it.y - 8) },               // LL_L
            { int16_t(it.x - 4 - it.w),          int16_t(it.y + it.g + 2) },        // LL_BL
            { int16_t(it.x + 4),                 int16_t(it.y + it.g + 2) },        // LL_BR
            { int16_t(it.x + 4),                 int16_t(it.y - it.g - 2 - it.h) }, // LL_AR
            { int16_t(it.x - 4 - it.w),          int16_t(it.y - it.g - 2 - it.h) }, // LL_AL
        };

        // Trial order: the sticky slot first, then the fixed preference order.
        uint8_t order[6];
        int m = 0;
        if (it.prev >= LL_R && it.prev <= LL_AL) order[m++] = uint8_t(it.prev);
        for (int s = 0; s < 6; ++s)
            if (m == 0 || s != order[0]) order[m++] = uint8_t(s);

        int8_t chosen = LL_NONE;
        for (int oi = 0; oi < m && chosen == LL_NONE; ++oi) {
            const LlRect c = { tl[order[oi]].x, tl[order[oi]].y,
                               int16_t(tl[order[oi]].x + it.w - 1),
                               int16_t(tl[order[oi]].y + it.h - 1) };
            if (!in_circle(c.x1, c.y1) || !in_circle(c.x2, c.y1) ||
                !in_circle(c.x1, c.y2) || !in_circle(c.x2, c.y2))
                continue;   // a label must never bleed past the round edge

            bool free = true;
            for (int k = 0; k < nObs && free; ++k) free = !hits(c, obstacles[k]);
            for (int j = 0; j < i && free; ++j)   // labels placed earlier this call
                if (slot[j] != LL_NONE) free = !hits(c, box[j]);
            for (int j = 0; j < n && free; ++j)   // every other glyph, placed or not
                if (j != i) {
                    const LlRect gl = { int16_t(items[j].x - items[j].g),
                                        int16_t(items[j].y - items[j].g),
                                        int16_t(items[j].x + items[j].g),
                                        int16_t(items[j].y + items[j].g) };
                    free = !hits(c, gl);
                }
            if (free) { chosen = int8_t(order[oi]); box[i] = c; }
        }

        slot[i] = chosen;
        if (chosen != LL_NONE) ++placed;
        else box[i] = { 0, 0, -1, -1 };   // empty sentinel: the caller must not draw it
    }
    return placed;
}
