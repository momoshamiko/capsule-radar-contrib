// Label placement (src/label_layout.h). On the round scope a label must never sit
// on another label, on an obstacle or on another plane's glyph, and must stay
// inside the circle. These checks pin the slot geometry, the trial order, the
// sticky `prev` slot and determinism — the renderer trusts all four. Each block
// prints its case name so a broken placer shows exactly which cases it fails.
#include "label_layout.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef LlRect R;

static R tl_box(int x, int y, int w, int h) {
    R b = { int16_t(x), int16_t(y), int16_t(x + w - 1), int16_t(y + h - 1) };
    return b;
}

static R glyph_box(const LlItem &it) {
    return tl_box(it.x - it.g, it.y - it.g, it.g * 2 + 1, it.g * 2 + 1);
}

// True when the two rects break the 2 px gap rule (one grown by the gap).
static bool too_close(const R &a, const R &b) {
    return a.x1 - 2 <= b.x2 && b.x1 <= a.x2 + 2 && a.y1 - 2 <= b.y2 && b.y1 <= a.y2 + 2;
}

static bool in_circle(const R &b, int cx, int cy, int r) {
    const int xs[2] = { b.x1, b.x2 }, ys[2] = { b.y1, b.y2 };
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            const int dx = xs[i] - cx, dy = ys[j] - cy;
            if (dx * dx + dy * dy > r * r) return false;
        }
    return true;
}

// 30 planes scattered by a fixed LCG: dense, reproducible, and dense enough that
// slots genuinely contend. Reused by the determinism block.
static void make_scene(LlItem it[30]) {
    uint32_t s = 20261008u;
    for (int i = 0; i < 30; ++i) {
        s = s * 1664525u + 1013904223u;
        it[i].x = int16_t(233 + int(s >> 24) % 240 - 120);
        s = s * 1664525u + 1013904223u;
        it[i].y = int16_t(233 + int(s >> 24) % 240 - 120);
        s = s * 1664525u + 1013904223u;
        it[i].w = int16_t(30 + (i % 4) * 10);
        it[i].h = (i % 3) ? 10 : 16;
        it[i].g = (i % 5) ? 12 : 21;
        it[i].prev = int8_t(int(s >> 28) % 7 - 1);   // mix of new and sticky planes
    }
}

static void case_name(const char *name) { printf("case: %s\n", name); }

int main() {
    const int CX = 233, CY = 233, RAD = 233;   // the 466 px scope

    // --- two close planes: the first keeps R, the second is pushed aside ---
    case_name("two close planes");
    {
        const LlItem it[2] = {
            { 200, 200, 40, 10, 12, LL_NONE },
            { 200, 205, 40, 10, 12, LL_NONE },
        };
        int8_t slot[2]; R box[2];
        const int placed = ll_place(it, 2, 0, 0, CX, CY, RAD, slot, box);
        assert(placed == 2);
        assert(slot[0] == LL_R);
        assert(box[0].x1 == 215 && box[0].y1 == 192 && box[0].x2 == 254 && box[0].y2 == 201);
        assert(slot[1] == LL_L);            // R would sit on plane 1's label
        assert(!too_close(box[0], box[1]));  // the two labels keep their gap
    }

    // --- a plane near the right edge cannot fit R inside the circle, so it flips to L ---
    case_name("right edge flips to L");
    {
        const LlItem it[1] = { { 440, 233, 40, 10, 12, LL_NONE } };
        int8_t slot[1]; R box[1];
        const int placed = ll_place(it, 1, 0, 0, CX, CY, RAD, slot, box);
        assert(placed == 1);
        assert(slot[0] == LL_L);
        assert(in_circle(box[0], CX, CY, RAD));
    }

    // --- an obstacle covering R forces the next free slot ---
    case_name("obstacle forces next slot");
    {
        const LlItem it[1] = { { 200, 200, 40, 10, 12, LL_NONE } };
        const R obs[1] = { { 246, 190, 260, 210 } };
        int8_t slot[1]; R box[1];
        const int placed = ll_place(it, 1, obs, 1, CX, CY, RAD, slot, box);
        assert(placed == 1);
        assert(slot[0] == LL_L);
        assert(!too_close(box[0], obs[0]));
    }

    // --- a plane boxed in on all six slots gets nothing, and the count excludes it ---
    case_name("boxed in on all sides");
    {
        const LlItem it[1] = { { 233, 233, 40, 10, 12, LL_NONE } };
        const R obs[6] = {
            { 246, 223, 289, 236 },   // R
            { 176, 223, 219, 236 },   // L
            { 187, 245, 230, 258 },   // BL
            { 235, 245, 278, 258 },   // BR
            { 235, 212, 278, 225 },   // AR
            { 187, 212, 230, 225 },   // AL
        };
        int8_t slot[1]; R box[1];
        const int placed = ll_place(it, 1, obs, 6, CX, CY, RAD, slot, box);
        assert(placed == 0);
        assert(slot[0] == LL_NONE);
        assert(box[0].x1 == 0 && box[0].y1 == 0 && box[0].x2 == -1 && box[0].y2 == -1);
    }

    // --- dense seeded scene: no placed label touches a glyph, another label, or the edge ---
    case_name("dense scene invariants");
    {
        LlItem it[30]; int8_t slot[30]; R box[30];
        make_scene(it);
        const int placed = ll_place(it, 30, 0, 0, CX, CY, RAD, slot, box);
        assert(placed > 0);
        for (int i = 0; i < 30; ++i) {
            if (slot[i] == LL_NONE) continue;
            assert(in_circle(box[i], CX, CY, RAD));
            for (int j = 0; j < 30; ++j)
                if (j != i) assert(!too_close(box[i], glyph_box(it[j])));
            for (int j = 0; j < 30; ++j)
                if (j != i && slot[j] != LL_NONE) assert(!too_close(box[i], box[j]));
        }
    }

    // --- priority: whoever comes first in the item list keeps R ---
    case_name("priority order swap");
    {
        const LlItem ab[2] = { { 200, 200, 40, 10, 12, LL_NONE },
                               { 200, 205, 40, 10, 12, LL_NONE } };
        const LlItem ba[2] = { { 200, 205, 40, 10, 12, LL_NONE },
                               { 200, 200, 40, 10, 12, LL_NONE } };
        int8_t s1[2], s2[2]; R b1[2], b2[2];
        ll_place(ab, 2, 0, 0, CX, CY, RAD, s1, b1);
        ll_place(ba, 2, 0, 0, CX, CY, RAD, s2, b2);
        assert(s1[0] == LL_R && s1[1] != LL_R);   // plane 1 wins R ...
        assert(s2[0] == LL_R && s2[1] != LL_R);   // ... and plane 2 wins it after the swap
    }

    // --- prev is kept while free, and dropped when something blocks it ---
    case_name("sticky prev slot");
    {
        const LlItem it[1] = { { 200, 200, 40, 10, 12, LL_BL } };
        const R obs[1] = { { 154, 212, 197, 225 } };   // swallows the BL box
        int8_t slot[1]; R box[1];
        ll_place(it, 1, 0, 0, CX, CY, RAD, slot, box);
        assert(slot[0] == LL_BL);   // R is free, but the sticky slot wins
        ll_place(it, 1, obs, 1, CX, CY, RAD, slot, box);
        assert(slot[0] == LL_R);    // BL blocked, so the default order takes over
    }

    // --- determinism: the same scene placed twice gives identical output ---
    case_name("determinism");
    {
        LlItem a[30], b[30];
        make_scene(a); make_scene(b);
        int8_t sa[30], sb[30]; R ba[30], bb[30];
        const int pa = ll_place(a, 30, 0, 0, CX, CY, RAD, sa, ba);
        const int pb = ll_place(b, 30, 0, 0, CX, CY, RAD, sb, bb);
        assert(pa == pb && pa > 0);
        for (int i = 0; i < 30; ++i) {
            assert(sa[i] == sb[i]);
            assert(ba[i].x1 == bb[i].x1 && ba[i].y1 == bb[i].y1 &&
                   ba[i].x2 == bb[i].x2 && ba[i].y2 == bb[i].y2);
        }
    }

    return 0;
}
