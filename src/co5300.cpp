// CO5300 AMOLED over QSPI with double-buffered DMA (see co5300.h).
// The init sequence, reset timing and framing are Arduino_GFX 1.6.4's Arduino_CO5300 +
// Arduino_ESP32QSPI (BSD licence), which this firmware used before.
#if !defined(BOARD_LCD21)     // the LCD 2.1 has no CO5300: display_st7701.cpp drives its RGB panel
#include "co5300.h"
#include "config.h"

#include <Arduino.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <soc/gpio_reg.h>
#include <string.h>

// Pixels per transfer when rows are copied into the driver's own buffers (push). Two
// buffers this size (8 KB of internal DMA RAM together) take turns.
#define CHUNK_PX        2048
// Pixels per transfer when the caller's buffer goes out as it is (pushInPlace).
#define BLOCK_PX        4096
static_assert(SCREEN_W <= CHUNK_PX && SCREEN_W <= BLOCK_PX, "a row must fit in one transfer");
#define RST_DELAY_MS    200       // reset held low, then the wait after releasing it
#define SLPOUT_DELAY_MS 120

// QSPI framing: every transaction starts with an 8-bit instruction and a 24-bit address.
// 0x02 = a register write on one line, the register in address bits 15..8.
// 0x32 = pixel data on four lines; address 0x003C00 = "memory write continue" after RAMWR.
#define QSPI_WRITE_REG  0x02
#define QSPI_WRITE_PIX  0x32
#define ADDR_RAMWRC     0x003C00

// CO5300 commands
#define CMD_SWRESET     0x01
#define CMD_SLPOUT      0x11
#define CMD_INVOFF      0x20
#define CMD_DISPON      0x29
#define CMD_CASET       0x2A
#define CMD_PASET       0x2B
#define CMD_RAMWR       0x2C
#define CMD_MADCTL      0x36
#define CMD_PIXFMT      0x3A
#define CMD_BRIGHTNESS  0x51
#define CMD_CTRLD1      0x53
#define CMD_CE          0x58      // contrast enhancement
#define CMD_HBM_BRIGHT  0x63
#define CMD_SPIMODE     0xC4
#define CMD_PAGE        0xFE

namespace co5300 {

static spi_device_handle_t   s_dev = nullptr;
static uint16_t             *s_buf[2] = { nullptr, nullptr };
static spi_transaction_ext_t s_tx;                // the transfer on the bus
static int                   s_next = 0;          // buffer the next copy fills
static bool                  s_busy = false;      // a transfer is on the bus
static bool                  s_open = false;      // a pixel stream holds CS low
static int                   s_khz = 0;           // the QSPI clock the driver actually runs

// CS is driven here, not by the SPI peripheral: it must stay low across all the transfers
// of one window (only the first carries the instruction and address).
static inline void cs_low() {
#if PIN_LCD_CS < 32
    REG_WRITE(GPIO_OUT_W1TC_REG, 1UL << PIN_LCD_CS);
#else
    REG_WRITE(GPIO_OUT1_W1TC_REG, 1UL << (PIN_LCD_CS - 32));
#endif
}
static inline void cs_high() {
#if PIN_LCD_CS < 32
    REG_WRITE(GPIO_OUT_W1TS_REG, 1UL << PIN_LCD_CS);
#else
    REG_WRITE(GPIO_OUT1_W1TS_REG, 1UL << (PIN_LCD_CS - 32));
#endif
}

// One transfer at a time, as a polling transaction: started, then finished by spinning on
// the peripheral's done flag. No interrupt and no queue round trip per transfer.
static void wait_done() {
    if (s_busy) {
        spi_device_polling_end(s_dev, portMAX_DELAY);
        s_busy = false;
    }
}

// End the current pixel stream: the last transfer done, CS high. Commands need this first.
static void stream_end() {
    wait_done();
    if (s_open) {
        cs_high();
        s_open = false;
    }
}

// One register write with up to 4 data bytes, single line.
static void reg(uint8_t cmd, const uint8_t *data = nullptr, int n = 0) {
    if (!s_dev) return;                 // begin() failed: no panel (e.g. setBrightness after that)
    stream_end();
    spi_transaction_t t;
    memset(&t, 0, sizeof t);
    t.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
    t.cmd = QSPI_WRITE_REG;
    t.addr = (uint32_t)cmd << 8;
    if (n) {
        t.flags |= SPI_TRANS_USE_TXDATA;
        memcpy(t.tx_data, data, (size_t)n);
        t.length = (size_t)n * 8;
    }
    cs_low();
    spi_device_polling_transmit(s_dev, &t);
    cs_high();
}
static void reg8(uint8_t cmd, uint8_t v) { reg(cmd, &v, 1); }

static void window(int x, int y, int w, int h) {
    const uint16_t x1 = (uint16_t)(x + LCD_COL_OFFSET), x2 = (uint16_t)(x1 + w - 1);
    const uint16_t y1 = (uint16_t)(y + LCD_ROW_OFFSET), y2 = (uint16_t)(y1 + h - 1);
    const uint8_t ca[4] = { (uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2 };
    const uint8_t pa[4] = { (uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2 };
    reg(CMD_CASET, ca, 4);
    reg(CMD_PASET, pa, 4);
    reg(CMD_RAMWR);
}

static void open_window(int x, int y, int w, int h) {
    window(x, y, w, h);
    cs_low();
    s_open = true;
}

// Start sending n pixels from src (DMA-capable RAM). The first transfer of a window carries
// the pixel-write instruction and address; the rest are data only.
static void send(const uint16_t *src, size_t n, bool first) {
    memset(&s_tx, 0, sizeof s_tx);
    s_tx.base.flags = SPI_TRANS_MODE_QIO;
    if (first) {
        s_tx.base.cmd = QSPI_WRITE_PIX;
        s_tx.base.addr = ADDR_RAMWRC;
    } else {                                        // zero instruction/address/dummy bits
        s_tx.base.flags |= SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
    }
    s_tx.base.tx_buffer = src;
    s_tx.base.length = n * 16;
    spi_device_polling_start(s_dev, &s_tx.base, portMAX_DELAY);
    s_busy = true;
}

void push(int x, int y, int w, int h, RowFn fn, void *ctx) {
    if (!s_dev || w <= 0 || h <= 0) return;
    open_window(x, y, w, h);
    const int rowsPer = CHUNK_PX / w;
    for (int r = 0; r < h;) {
        const int n = rowsPer < h - r ? rowsPer : h - r;
        uint16_t *dst = s_buf[s_next];
        for (int i = 0; i < n; ++i) fn(dst + (size_t)i * w, y + r + i, ctx);
        wait_done();
        send(dst, (size_t)n * w, r == 0);
        s_next ^= 1;
        r += n;
    }
}

bool canSendInPlace(const void *px, int w) {
    return esp_ptr_dma_capable(px) && ((uintptr_t)px & 3) == 0 && (w & 1) == 0;
}

void pushInPlace(int x, int y, int w, int h, uint16_t *px, RowFn prep, void *ctx) {
    if (!s_dev || w <= 0 || h <= 0) return;
    open_window(x, y, w, h);
    const int rowsPer = BLOCK_PX / w;
    for (int r = 0; r < h;) {
        const int n = rowsPer < h - r ? rowsPer : h - r;
        uint16_t *blk = px + (size_t)r * w;
        for (int i = 0; i < n; ++i) prep(blk + (size_t)i * w, y + r + i, ctx);
        wait_done();
        send(blk, (size_t)n * w, r == 0);
        r += n;
    }
    stream_end();                                   // px is the caller's again
}

static void fill_row(uint16_t *dst, int y, void *ctx) {
    (void)y;
    const uint16_t c = *(const uint16_t *)ctx;
    for (int i = 0; i < SCREEN_W; ++i) dst[i] = c;
}

void fill(uint16_t rgb565) {
    uint16_t c = (uint16_t)((rgb565 >> 8) | (rgb565 << 8));   // panel byte order
    push(0, 0, SCREEN_W, SCREEN_H, fill_row, &c);
    stream_end();
}

int clockKhz() { return s_khz; }

void setBrightness(uint8_t v) { reg8(CMD_BRIGHTNESS, v); }

bool begin() {
    pinMode(PIN_LCD_CS, OUTPUT);
    cs_high();

    spi_bus_config_t bus;
    memset(&bus, 0, sizeof bus);
    bus.mosi_io_num = PIN_LCD_D0;
    bus.miso_io_num = PIN_LCD_D1;
    bus.quadwp_io_num = PIN_LCD_D2;
    bus.quadhd_io_num = PIN_LCD_D3;
    bus.sclk_io_num = PIN_LCD_SCLK;
    bus.data4_io_num = bus.data5_io_num = bus.data6_io_num = bus.data7_io_num = -1;
    bus.max_transfer_sz = BLOCK_PX * 2;
    bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

    spi_device_interface_config_t dev;
    memset(&dev, 0, sizeof dev);
    dev.command_bits = 8;
    dev.address_bits = 24;
    dev.mode = 0;
    dev.clock_source = SPI_CLK_SRC_DEFAULT;
    dev.clock_speed_hz = LCD_QSPI_HZ;
    dev.spics_io_num = -1;
    dev.flags = SPI_DEVICE_HALFDUPLEX;
    dev.queue_size = 1;                             // polling transfers only; the queue stays empty
    if (spi_bus_add_device(SPI2_HOST, &dev, &s_dev) != ESP_OK) return false;
    spi_device_acquire_bus(s_dev, portMAX_DELAY);   // the only device on this bus
    spi_device_get_actual_freq(s_dev, &s_khz);

    for (int i = 0; i < 2; ++i) {
        s_buf[i] = (uint16_t *)heap_caps_aligned_alloc(16, CHUNK_PX * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_buf[i]) return false;
    }

#if PIN_LCD_RST >= 0
    pinMode(PIN_LCD_RST, OUTPUT);
    digitalWrite(PIN_LCD_RST, HIGH);
    delay(10);
    digitalWrite(PIN_LCD_RST, LOW);
    delay(RST_DELAY_MS);
    digitalWrite(PIN_LCD_RST, HIGH);
    delay(RST_DELAY_MS);
#else
    reg(CMD_SWRESET);
    delay(RST_DELAY_MS);
#endif
    reg(CMD_SLPOUT);
    delay(SLPOUT_DELAY_MS);
    reg8(CMD_PAGE, 0x00);
    reg8(CMD_SPIMODE, 0x80);
    reg8(CMD_PIXFMT, 0x55);          // 16 bits per pixel
    reg8(CMD_CTRLD1, 0x20);
    reg8(CMD_HBM_BRIGHT, 0xFF);
    reg(CMD_DISPON);
    reg8(CMD_BRIGHTNESS, 0xD0);
    reg8(CMD_CE, 0x00);              // contrast enhancement off
    delay(10);
    reg(CMD_INVOFF);
    reg8(CMD_MADCTL, 0x00);          // RGB order, no mirroring
    fill(0x0000);
    return true;
}

} // namespace co5300

#endif  // !BOARD_LCD21
