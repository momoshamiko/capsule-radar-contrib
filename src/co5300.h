#pragma once
// CO5300 AMOLED over QSPI, on ESP-IDF's spi_master (device only).
// Same wire protocol and init sequence as Arduino_GFX's Arduino_CO5300 + Arduino_ESP32QSPI,
// which this replaces: that bus byte-swapped every pixel on the CPU and then waited for each
// 2 KB transfer with the CPU idle. Here the CPU prepares the next rows while the rows before
// them are on the wire: copied into two DMA buffers that take turns (push), or converted in
// place in a caller's buffer that DMA reads directly (pushInPlace).
#include <stdint.h>

namespace co5300 {

// Writes the `w` pixels of physical row `y` of the window into dst, in panel byte order
// (RGB565, high byte first). `ctx` is the caller's. For pushInPlace, dst is the row itself.
typedef void (*RowFn)(uint16_t *dst, int y, void *ctx);

bool begin();                      // reset + init sequence; the panel is left black and lit
void setBrightness(uint8_t v);     // 0..255 (command 0x51)
// Stream the window x..x+w-1, y..y+h-1 (the panel needs an even x and y and even w and h).
// Returns once every row is filled; the last transfer may still be on the wire and finishes
// before the next panel command or window.
void push(int x, int y, int w, int h, RowFn fn, void *ctx);
// Same window, sent straight from px (h rows of w pixels) after fn has converted each row in
// place. Returns when the last row is on the panel, so px is free again. Only for buffers
// that pass canSendInPlace (DMA-capable internal RAM, 4-byte aligned, even w).
void pushInPlace(int x, int y, int w, int h, uint16_t *px, RowFn fn, void *ctx);
bool canSendInPlace(const void *px, int w);
int clockKhz();                    // the QSPI clock the SPI driver actually runs
void fill(uint16_t rgb565);        // the whole panel in one colour

} // namespace co5300
