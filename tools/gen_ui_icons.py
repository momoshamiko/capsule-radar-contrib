#!/usr/bin/env python3
"""Write src/ui_icons.c from the pixel art below (Python 3, standard library only).

Every icon is drawn on a small grid ('#' = lit) and stored at 2x, one art pixel = 2x2
screen pixels, the same pixel size as the M font (font_px22). Format: LV_IMG_CF_ALPHA_1BIT,
rows padded to whole bytes, most significant bit first; the colour comes from the object's
img_recolor style. Run from the repository root:  python3 tools/gen_ui_icons.py
"""
import os

SCALE = 2

ART = {
    "plane": [          # the scope's own plane glyph (an arrowhead), so the count reads as "these"
        "...#...",
        "...#...",
        "..###..",
        "..###..",
        ".#####.",
        ".##.##.",
        ".#...#.",
    ],
    "bolt": [
        "..##",
        ".##.",
        "##..",
        "####",
        "..##",
        ".##.",
        "##..",
    ],
    "close": [          # the plane card's X: a thin diagonal cross
        "#.....#",
        ".#...#.",
        "..#.#..",
        "...#...",
        "..#.#..",
        ".#...#.",
        "#.....#",
    ],
    "wx_clear": [
        "................",
        ".......##.......",
        ".......##.......",
        "..##........##..",
        "..##..####..##..",
        ".....######.....",
        "....########....",
        ".##.########.##.",
        ".##.########.##.",
        "....########....",
        ".....######.....",
        "..##..####..##..",
        "..##........##..",
        ".......##.......",
        ".......##.......",
        "................",
    ],
    "wx_partly": [
        "................",
        "....#...........",
        ".#.....#........",
        "...###..........",
        "..#####.........",
        "#.#####.#.......",
        "..#####.........",
        "...###.####.....",
        ".#....#....#....",
        "....###.....##..",
        "...#..........#.",
        "..#...........#.",
        "..#...........#.",
        "...###########..",
        "................",
        "................",
    ],
    "wx_cloudy": [
        "................",
        "................",
        "................",
        "......####......",
        ".....#....#.....",
        "..###......#....",
        ".#..........##..",
        "#.............#.",
        "#.............#.",
        "#.............#.",
        ".#############..",
        "................",
        "................",
        "................",
        "................",
        "................",
    ],
    "wx_fog": [
        "................",
        "................",
        "......####......",
        ".....#....#.....",
        "..###......#....",
        ".#..........##..",
        "#.............#.",
        "................",
        "###.######.####.",
        "................",
        ".######.#######.",
        "................",
        "####.######.###.",
        "................",
        "................",
        "................",
    ],
    "wx_rain": [
        "................",
        "......####......",
        ".....#....#.....",
        "..###......#....",
        ".#..........##..",
        "#.............#.",
        "#.............#.",
        ".#############..",
        "................",
        "...#....#....#..",
        "..#....#....#...",
        "................",
        ".#....#....#....",
        "#....#....#.....",
        "................",
        "................",
    ],
    "wx_snow": [
        "................",
        "......####......",
        ".....#....#.....",
        "..###......#....",
        ".#..........##..",
        "#.............#.",
        "#.............#.",
        ".#############..",
        "................",
        "..#....#....#...",
        ".###..###..###..",
        "..#....#....#...",
        "................",
        "....#....#......",
        "...###..###.....",
        "....#....#......",
    ],
    "wx_thunder": [
        "................",
        "......####......",
        ".....#....#.....",
        "..###......#....",
        ".#..........##..",
        "#.............#.",
        "#.............#.",
        ".####...#######.",
        "......##........",
        ".....##.........",
        "....######......",
        "......##........",
        ".....##.........",
        "....#...........",
        "................",
        "................",
    ],
}

# (C name, art key) in the order ui_icons.h expects; ui_icon_wx[] follows UI_WX_* there.
SINGLE = [("ui_icon_plane", "plane"), ("ui_icon_bolt", "bolt"), ("ui_icon_close", "close")]
WX = ['wx_clear', 'wx_partly', 'wx_cloudy', 'wx_fog', 'wx_rain', 'wx_snow', 'wx_thunder']


def bitmap(rows):
    w, h = len(rows[0]) * SCALE, len(rows) * SCALE
    stride = (w + 7) // 8
    out = []
    for y in range(h):
        row = rows[y // SCALE]
        line = bytearray(stride)
        for x in range(w):
            if row[x // SCALE] == "#":
                line[x // 8] |= 0x80 >> (x % 8)
        out.extend(line)
    return w, h, bytes(out)


def c_bytes(data):
    hexes = ["0x%02x" % b for b in data]
    return "\n".join("    " + ", ".join(hexes[i:i + 16]) + "," for i in range(0, len(hexes), 16))


def dsc(w, h, size, data_name, indent=""):
    return (f"{indent}{{ .header.cf = LV_IMG_CF_ALPHA_1BIT, .header.always_zero = 0, .header.reserved = 0,\n"
            f"{indent}  .header.w = {w}, .header.h = {h}, .data_size = {size}, .data = {data_name} }}")


def main():
    for rows in ART.values():
        assert all(len(r) == len(rows[0]) for r in rows), rows
    parts = ["/* Generated by tools/gen_ui_icons.py -- edit the art there, then rerun. */",
             '#include "ui_icons.h"', ""]
    for cname, key in SINGLE:
        w, h, data = bitmap(ART[key])
        parts.append(f"static const uint8_t {cname}_map[] = {{\n{c_bytes(data)}\n}};")
        parts.append(f"const lv_img_dsc_t {cname} = {dsc(w, h, len(data), cname + '_map')};\n")
    entries = []
    for key in WX:
        w, h, data = bitmap(ART[key])
        parts.append(f"static const uint8_t ui_icon_{key}_map[] = {{\n{c_bytes(data)}\n}};")
        entries.append(dsc(w, h, len(data), f"ui_icon_{key}_map", "    "))
    parts.append("const lv_img_dsc_t ui_icon_wx[UI_WX_COUNT] = {\n" + ",\n".join(entries) + ",\n};")
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(root, "src", "ui_icons.c"), "w") as fh:
        fh.write("\n".join(parts) + "\n")


if __name__ == "__main__":
    main()
