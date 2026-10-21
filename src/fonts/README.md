# UI fonts

`font_px11.c`, `font_px22.c` and `font_px33.c` are Departure Mono 1.500 by Helena Zhang
(https://departuremono.com, release zip from https://github.com/rektdeckard/departure-mono),
licensed under the SIL Open Font License 1.1 (`OFL-DepartureMono.txt`).

They are 1-bit (no anti-aliasing) at 1×, 2× and 3× the font's 11-px pixel grid, so every
glyph stays on whole pixels. Other sizes would blur the pixels; add a new multiple instead.

Generated with lv_font_conv 1.5.2 from `DepartureMono-Regular.otf` (one command per size):

```
npx lv_font_conv@1.5.2 --font DepartureMono-Regular.otf \
    -r 0x20-0x7E,0xB0,0xB7,0x2192,0x25A0 --size 22 --bpp 1 --no-compress \
    --format lvgl --lv-include lvgl.h -o font_px22.c
```

Extra glyphs: ° (U+00B0), · (U+00B7), → (U+2192), ■ (U+25A0). To add one, append its code
point to `-r` and regenerate all three sizes. The LVGL symbol is the output file name
(`font_px22`); `include/lv_conf.h` declares the three and makes `font_px22` the default.
