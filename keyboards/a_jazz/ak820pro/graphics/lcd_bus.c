#include "watchdog_record.h"
// Copyright 2026 Fernando Birra
// SPDX-License-Identifier: GPL-2.0-or-later
//
// [UNIFIED EXPERIMENT — ak820pro-flashlcd-unified]
// LCD bus over the ChibiOS SN32 SPI driver: SPI0 (the panel) uses spiSend (FIFO-
// batched by spi_fifo_pump.diff) for all commands/pixels, and the flash->LCD DMA is
// the driver's spiSN32FlashDma* extension. SPI1 (flash reads) stays bare-metal. This
// is the experiment sibling of ak820pro-flashlcd-tiles (which does the same drawing
// fully bare-metal) -- built to measure whether one driver can replace the bare-metal
// bus without losing throughput. See docs/LCD_FLASH_LAYER.md.

#include <string.h>

#include "quantum.h"
#include "gpio.h"
#include "lcd_bus.h"
#include "../rtc/rtc.h"
#include "../ak820pro.h"

extern void display_set_paused(bool paused);   // graphics/display.c

// --- pins --------------------------------------------------------------------
#define PANEL_DC   D14
#define PANEL_CS   B8
#define PANEL_RST  A17
#define FLASH_CS   A13

#define FRAME_W 128
#define FRAME_H 128
#define FRAME_BYTES (FRAME_W * FRAME_H * 2)
#define LCD_OFF_X 1
#define LCD_OFF_Y 2
#define FLASH_CMD_READ 0x03

// GC9107 MADCTL -- PANEL VARIANT FLAG (hardening plan phase 5.2). At least two
// hardware revisions of this keyboard exist: JD's unit (shipped on stock v1.10)
// has the panel mounted 180 degrees from fpb's units AND needs display
// inversion, so the wrong build shows an upside-down, colour-inverted panel
// that reads as broken firmware. The default is JD's variant; build with
//   -e EXTRAFLAGS=-DAK820PRO_LCD_VARIANT_FPB
// (or #define AK820PRO_LCD_VARIANT_FPB in config.h) for fpb-style panels:
// rotation 270 = BGR(0x08)|MV(0x20)|MY(0x80) = 0xA8, no INVON. JD's trades MY
// for MX: BGR|MV|MX(0x40) = 0x68, with INVON in the init sequence. The
// dashboard and the animation share this orientation.
#ifdef AK820PRO_LCD_VARIANT_FPB
#    define MADCTL_DASH 0xA8
#else
#    define MADCTL_DASH 0x68
#endif
#define MADCTL_ANIM MADCTL_DASH

// Animation slot. The header at ANIM_BASE is the stock format we reverse-engineered:
//   byte 0        = frame count
//   bytes 1..n    = per-frame duration, one byte each
//   ...           = padding to ANIM_HDR (0x00 when stock-written, 0xFF when the
//                   AJAZZ uploader wrote it into freshly erased flash)
// Frames follow at ANIM_HDR, ANIM_STRIDE each. Nothing validates this -- there is
// no magic or checksum -- so a stale slot yields garbage rather than "no animation".
#define ANIM_BASE   0x540000u
#define ANIM_HDR    0x100u
#define ANIM_STRIDE 0x8000u
// Frame-count ceiling, derived rather than guessed: the slot grows upward from
// ANIM_BASE and must not reach the asset region. hdr[0] is one byte, so 255 is
// the format's own ceiling; whichever is smaller wins.
//   (0x0CE0000 - 0x540000 - 0x100) / 0x8000 = 244
#define ANIM_ROOM   ((FLASH_ASSET_BASE - ANIM_BASE - ANIM_HDR) / ANIM_STRIDE)
#define ANIM_MAX    (ANIM_ROOM < 255u ? ANIM_ROOM : 255u)

// Playback is paced by the 10 Hz housekeeping slot -- one frame per 100 ms --
// which matches stock speed by observation. The header's per-frame duration
// bytes are deliberately IGNORED: disassembly of V1.13's player shows it never
// reads them either (only hdr[0], the count), and they are uniform in every
// stock animation we have seen (0x2D throughout Mario, 0x14 throughout the
// 125-frame one) -- exactly what an unread field looks like. What actually sets
// the stock frame rate was not identified; 100 ms is fitted to observation, not
// derived.

// ---------------------------------------------------------------------------
// Low-level bus
// ---------------------------------------------------------------------------
// [UNIFIED EXPERIMENT] SPI0 (the LCD) is driven by the ChibiOS SN32 SPI driver
// (spiSend, FIFO-batched by spi_fifo_pump.diff) instead of bare-metal pokes, and
// the flash->LCD DMA is its extension (spiSN32FlashDma*). We keep manual CS/DC as
// GPIO; every SPI0 byte goes through the driver, because leaving the driver's RX
// FIFO IRQ enabled while poking SN_SPI0->DATA directly would fire its handler
// spuriously. 8-bit, mode 0, 24 MHz -- matches the panel and the DMA extension.
static const SPIConfig spicfg = {
    .ctrl0  = SPI_DATA_LENGTH(8),
    .ctrl1  = SPI_MLSB_MSB | SPI_CPOL_LOW | SPI_CPHA_FALLING,   // mode 0, MSB first
    .clkdiv = 0,                                                // 24 MHz
};

static bool spi1_inited = false;
static void spi1_setup(void) {
    // [dualspi Step 2] SPI1 (external flash) is now a ChibiOS driver instance
    // (SPID1). spiStart configures CTRL0/CTRL1/CLKDIV (8-bit, mode 0, 24 MHz --
    // reuse the panel's spicfg), enables the SPI1 clock (AHB bit 13) + NVIC
    // vector, and sets RXFIFOTHIE/SPIEN. We still apply the SN32 specifics the
    // generic driver config does not: the EBI/LCD DMA-datapath clocks, the SPI1
    // PFPA pin-mux, and the DMA auto-fetch (DFETCH_EN). Job-1 flash I/O now goes
    // through spiSend/spiExchange(&SPID1); only the DMA blit command phase still
    // pokes SN_SPI1 directly (with the vector disabled -- see lcd_blit_flash).
    SN_SYS1->AHBCLKEN |= (1u << 15) | (1u << 26);   // EBI+LCD (shared DMA datapath)
    // NOTE: do NOT touch SN_FLASH->LPCTRL here. ChibiOS sets it to 0x5AFA0029 (correct
    // wait-states for 48MHz). Overriding it to the ">48MHz" preset (0x39) added extra
    // internal-flash wait-states that slowed CPU instruction fetch (~5% matrix-scan
    // drop that persisted after the first animation). It was never needed for the DMA.
    SN_PFPA->SPI_b.MISO1 = 0b01; SN_PFPA->SPI_b.MOSI1 = 0b01;
    SN_PFPA->SPI_b.SCK1 = 0b11;  SN_PFPA->SPI_b.SEL1  = 0b01;
    gpio_set_pin_output(FLASH_CS); gpio_write_pin(FLASH_CS, 1);
    spiStart(&SPID1, &spicfg);
    SN_SPI1->DFDLY_b.DFETCH_EN = 1;                 // DMA source auto-fetch
    SN_SPI1->CTRL0_b.FRESET = 0b11;
}

static inline void cs(bool hi) { gpio_write_pin(PANEL_CS, hi); }
static inline void dc(bool data){ gpio_write_pin(PANEL_DC, data); }

/* Wait out a flash->LCD DMA still in flight before any CPU transaction on
 * either bus. Defined with the blit state below; see its comment. */
static void bus_quiesce(void);

static void tx8(uint8_t b) { spiSend(&SPID0, 1, &b); }

// RGB565 is streamed hi-byte-first to match the panel. The driver takes a byte
// buffer, but px[] is a little-endian uint16 array (lo byte first in memory), so
// we byte-swap into a scratch buffer in chunks and hand each chunk to spiSend.
// (This swap-copy is pure overhead versus the old inline tx_pipe -- it is one of
// the costs the unified experiment is meant to expose.)
static void tx_pixels(const uint16_t *px, uint32_t n) {
    static uint8_t buf[512];
    while (n) {
        uint32_t c = n < 256u ? n : 256u;
        for (uint32_t i = 0; i < c; i++) {
            buf[2*i]   = (uint8_t)(px[i] >> 8);
            buf[2*i+1] = (uint8_t)(px[i] & 0xFF);
        }
        spiSend(&SPID0, c * 2u, buf);
        px += c; n -= c;
    }
}

static void reset_panel(void) {
    gpio_set_pin_output(PANEL_RST);
    gpio_write_pin(PANEL_RST, 1); wait_ms(20);
    gpio_write_pin(PANEL_RST, 0); wait_ms(20);
    gpio_write_pin(PANEL_RST, 1); wait_ms(200);
}

// Address window + RAMWR (leaves CS asserted, DC=data). Used by the DMA blit.
static void lcd_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    x0 += LCD_OFF_X; x1 += LCD_OFF_X; y0 += LCD_OFF_Y; y1 += LCD_OFF_Y;
    cs(0);
    dc(0); tx8(0x2A); dc(1); tx8(x0>>8); tx8(x0); tx8(x1>>8); tx8(x1);
    dc(0); tx8(0x2B); dc(1); tx8(y0>>8); tx8(y0); tx8(y1>>8); tx8(y1);
    dc(0); tx8(0x2C); dc(1);
}

// ---------------------------------------------------------------------------
// Bare-metal dashboard drawing (replaces Quantum Painter). RGB565 is streamed
// hi-byte-first to match the panel.
//
// Byte order differs by path, and both are correct -- verified on hardware by
// drawing the same stock asset (usb_dongle, flash 0x0D8310) each way and getting
// an identical green dongle:
//   RAM  -> CPU  (here):            uint16 colour values, emitted hi byte first.
//   flash-> DMA  (lcd_blit_flash):  bytes stored LO first; CTRL0.DL=0xF packs the
//                                   pair into a 16-bit word and shifts it out MSB
//                                   first, which swaps them back.
// So a RAM tile promoted to flash must have its bytes SWAPPED on the way in --
// it is not a straight copy. See docs/LCD_FLASH_LAYER.md (Stage D).
// ---------------------------------------------------------------------------
void lcd_fill_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color) {
    WDT_SCOPE(WDT_SITE_LCD_TRANSFER);
    if (x1 < x0 || y1 < y0) return;
    bus_quiesce();
    lcd_window(x0, y0, x1, y1);
    uint32_t px = (uint32_t)(x1 - x0 + 1) * (uint32_t)(y1 - y0 + 1);
    static uint8_t buf[512];
    for (uint32_t i = 0; i < 256u; i++) { buf[2*i] = color >> 8; buf[2*i+1] = color & 0xFF; }
    while (px) {
        uint32_t c = px < 256u ? px : 256u;
        spiSend(&SPID0, c * 2u, buf);
        px -= c;
    }
    cs(1);
}

// Stage C: blit a w*h RGB565 tile from RAM (firmware array) to (x,y).
// The SN32 DMA is SPI-to-SPI only -- its source is the other SPI's RX FIFO, with no
// source-address register -- so RAM-resident art cannot be DMA'd and is CPU-pushed here
// (pipelined, ~wire speed). Only flash-resident art can use lcd_blit_flash(). See
// docs/LCD_FLASH_LAYER.md.
// Clear a rect by DMA instead of pushing pixels from the CPU.
//
// The stock image keeps a 128x128 all-black frame at flash 0x000000 -- exactly
// 32768 bytes -- precisely so the panel can be cleared with zero CPU in the data
// path. A full-screen CPU fill is 32 KB through tx_pipe, ~11-13 ms of blocking;
// this is a fire-and-forget DMA.
//
// It works for ANY rect, not just full-screen: the source is uniform, so the
// usual "a sub-rect of a wide image is strided and undrawable" problem does not
// apply -- any contiguous run of w*h*2 zero bytes is the correct source.
#define FLASH_BLACK_FRAME 0x000000u

void lcd_clear_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!w || !h) return;
    if ((uint32_t)w * h * 2u > 0x8000u) return;      // larger than the black frame
    lcd_blit_flash(FLASH_BLACK_FRAME, x, y, w, h);
    lcd_blit_wait();
}

void lcd_blit_ram(const uint16_t *px, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    WDT_SCOPE(WDT_SITE_LCD_TRANSFER);
    if (!px || !w || !h) return;
    bus_quiesce();
    lcd_window(x, y, x + w - 1, y + h - 1);
    tx_pixels(px, (uint32_t)w * (uint32_t)h);
    cs(1);
}

// ---------------------------------------------------------------------------
// Panel bring-up (bare-metal GC9107 init; literal opcodes, no Quantum Painter)
// ---------------------------------------------------------------------------
static void send_cmd(uint8_t c) { dc(0); tx8(c); dc(1); }
static void send_seq(const uint8_t *seq, uint32_t len) {   // cmd, delay_ms, nparams, params...
    bus_quiesce();
    cs(0);
    for (uint32_t i = 0; i < len;) {
        uint8_t cmd = seq[i], delay = seq[i+1], num = seq[i+2];
        send_cmd(cmd);
        for (uint8_t k = 0; k < num; k++) tx8(seq[i+3+k]);
        if (delay) wait_ms(delay);
        i += 3 + num;
    }
    cs(1);
}

void lcd_init(void) {
    gpio_set_pin_output(PANEL_CS); gpio_write_pin(PANEL_CS, 1);
    gpio_set_pin_output(PANEL_DC); gpio_write_pin(PANEL_DC, 1);
    spiStart(&SPID0, &spicfg);          // driver owns SPI0 (8-bit, mode 0, 24 MHz)
    reset_panel();
    static const uint8_t seq[] = {
        0xFE, 5, 0,                 // inter-register enable 1
        0xEF, 5, 0,                 // inter-register enable 2
        0xB6, 0, 1, 0x19,           // function ctl6: allow complement-RGB + framerate
        0xAC, 0, 1, 0xC0,           // complement RGB
        0xAB, 0, 1, 0x0E,
        0xA8, 0, 1, 0x19,           // frame rate
        0x3A, 0, 1, 0x05,           // pixel format: 16bpp RGB565
#ifndef AK820PRO_LCD_VARIANT_FPB
        0x21, 0, 0,                 // display inversion ON (JD-variant panels
                                    // render white-on-black artwork as black-on-
                                    // white without it; fpb-variant panels need
                                    // no inversion). Set before sleep-out so the
                                    // very first frame is already correct.
#endif
        0x11, 120, 0,               // sleep out
        0x29, 20, 0,                // display on
        0x36, 0, 1, MADCTL_DASH,    // memory access ctl: see MADCTL_DASH above
    };
    send_seq(seq, sizeof(seq));
}

// ---------------------------------------------------------------------------
// SPI-to-SPI DMA (interrupt-driven via Vector58)
// ---------------------------------------------------------------------------
// Job-1 flash I/O now goes through the ChibiOS driver (SPID1). CS stays manual
// (FLASH_CS via gpio) exactly as before; only the byte movement changed.
static bool spi1_xfer(uint8_t out) { spiSend(&SPID1, 1, &out); return true; }
static uint8_t spi1_rw(uint8_t out) {
    uint8_t in = 0xFF;
    spiExchange(&SPID1, 1, &out, &in);
    return in;
}
// Bare-metal single byte, used ONLY by the DMA blit command phase, which runs
// with SPID1's NVIC vector disabled by the extension -- so the driver ISR (and
// therefore spiSend/spiExchange) is unavailable and we must poll directly.
/* Drains by waiting on BUSY (shift complete) and then reading DATA. That is
 * NOT the driver's own idiom, which is `while (STAT_b.RX_EMPTY);` before
 * reading (hal_spi_v2_lld.c:569), so this read can land before the byte is
 * published and leave residue in the RX FIFO.
 *
 * ⚠️ THE "CORRECT" VERSION WAS TESTED ON HARDWARE AND IS NOT BETTER -- do not
 * re-derive it. Adding an RX_EMPTY wait here was measured 2026-08-30 against
 * the never-started-DMA failure:
 *
 *     BUSY-only drain (this):  3 failures / 27.0 min = 1 per 9.0 min
 *     RX_EMPTY drain:          3 failures / 16.1 min = 1 per 5.4 min
 *
 * The hypothesis was that the DMA starts because of RESIDUE in SPI1's RX FIFO
 * (every captured failure has SPI1 STAT = 0x25, i.e. RX_EMPTY set), so draining
 * properly should have broken nearly every blit. It did not: the rate barely
 * moved and is if anything marginally worse, which at n=3 is noise either way.
 *
 * Conclusion: residue is not the trigger, and the drain is not the cause. Left
 * as it was because the tested change showed no benefit; the discarded read
 * only exists to drain, and the next blit's FRESET clears any residue anyway. */
static inline void spi1_raw_byte(uint8_t out) {
    SN_SPI1->DATA = out; uint32_t n = 0;
    while (SN_SPI1->STAT_b.BUSY) { if (++n > 500000u) break; }
    (void)SN_SPI1->DATA;
}
// ---------------------------------------------------------------------------
// External flash WRITE path (Stage D provisioning)
//
// Everything here is non-blocking with respect to the *device*: a command is
// issued over SPI (microseconds) and the chip then goes busy on its own -- a
// page program takes ~1-3 ms, a 4K sector erase 50-300 ms. Blocking the matrix
// scan for 300 ms is not acceptable, so callers must poll flash_busy() instead
// of waiting here. Every SPI-level spin below is bounded; an unbounded one
// hangs the keyboard before USB enumerates.
// ---------------------------------------------------------------------------
#define FLASH_CMD_WREN      0x06
#define FLASH_CMD_RDSR      0x05
#define FLASH_CMD_PAGE_PROG 0x02
#define FLASH_CMD_SEC_ERASE 0x20
#define FLASH_CMD_JEDEC     0x9F
#define FLASH_PAGE          256u
#define FLASH_SECTOR        4096u
#define FLASH_CHIP_SIZE     0x1000000u   // PY25Q128HA, 16MB

// Write policy. The stock LCD assets below 0x1AA000 are effectively
// irreplaceable (our only dump of them has read damage), so they are never
// writable. The animation slots are stock-owned but legitimately rewritable,
// behind an explicit unlock. Our own Stage D assets live in the 3.12 MB that
// has been erased (0xFF) since manufacture and is always writable.
#define FLASH_ASSET_BASE    0x0CE0000u
static bool flash_unlocked = false;

void flash_set_unlocked(bool on) { flash_unlocked = on; }

// Animation slots seen in stock firmware: V1.13 boot/user, V1.14 boot/user.
static bool in_anim_slot(uint32_t a, uint32_t len) {
    static const uint32_t slots[] = {0x1AA000u, 0x200000u, 0x38B000u, 0x540000u};
    for (uint8_t i = 0; i < 4; i++) {
        // Slots are sized generously: header + up to 132 frames.
        if (a >= slots[i] && a + len <= slots[i] + 0x100u + 132u * 0x8000u) return true;
    }
    return false;
}

bool flash_writable(uint32_t addr, uint32_t len) {
    if (!len || addr >= FLASH_CHIP_SIZE || len > FLASH_CHIP_SIZE - addr) return false;
    if (addr >= FLASH_ASSET_BASE) return true;            // our region, always
    return flash_unlocked && in_anim_slot(addr, len);     // stock slots, on request
}

static void flash_cmd_addr(uint8_t cmd, uint32_t a) {
    bus_quiesce();
    gpio_write_pin(FLASH_CS, 0);
    spi1_xfer(cmd);
    spi1_xfer((a >> 16) & 0xFF); spi1_xfer((a >> 8) & 0xFF); spi1_xfer(a & 0xFF);
}

static uint8_t flash_status(void) {
    bus_quiesce();
    gpio_write_pin(FLASH_CS, 0);
    spi1_xfer(FLASH_CMD_RDSR);
    uint8_t s = spi1_rw(0xFF);
    gpio_write_pin(FLASH_CS, 1);
    return s;
}

// True while a program/erase is still running (status bit 0 = WIP).
bool flash_busy(void) {
    lcd_flash_init();
    return (flash_status() & 0x01) != 0;
}

static void flash_wren(void) {
    bus_quiesce();
    gpio_write_pin(FLASH_CS, 0);
    spi1_xfer(FLASH_CMD_WREN);
    gpio_write_pin(FLASH_CS, 1);
}

uint32_t flash_jedec_id(void) {
    lcd_flash_init();
    bus_quiesce();
    gpio_write_pin(FLASH_CS, 0);
    spi1_xfer(FLASH_CMD_JEDEC);
    uint32_t id = ((uint32_t)spi1_rw(0xFF) << 16);
    id |= ((uint32_t)spi1_rw(0xFF) << 8);
    id |= spi1_rw(0xFF);
    gpio_write_pin(FLASH_CS, 1);
    return id;
}

void flash_read_bytes(uint32_t addr, uint8_t *dst, uint32_t len) {
    WDT_SCOPE(WDT_SITE_FLASH_READ);
    lcd_flash_init();
    flash_cmd_addr(FLASH_CMD_READ, addr);
    for (uint32_t i = 0; i < len; i++) dst[i] = spi1_rw(0xFF);
    gpio_write_pin(FLASH_CS, 1);
}

// Erase one 4K sector. Returns false if the address is not writable, the chip
// is still busy, or the animation owns the bus. The chip stays busy for
// 50-300 ms afterwards -- poll flash_busy().
bool flash_erase_sector(uint32_t addr) {
    WDT_SCOPE(WDT_SITE_FLASH_ERASE);
    if (anim_active()) return false;                 // SPI1 is shared with the DMA
    addr &= ~(FLASH_SECTOR - 1u);
    if (!flash_writable(addr, FLASH_SECTOR)) return false;
    lcd_flash_init();
    if (flash_busy()) return false;
    flash_wren();
    flash_cmd_addr(FLASH_CMD_SEC_ERASE, addr);
    gpio_write_pin(FLASH_CS, 1);
    return true;
}

// Program up to one 256-byte page. The write must not cross a page boundary --
// the chip wraps to the start of the page instead of continuing, silently
// corrupting data, so that case is rejected rather than split here.
bool flash_page_program(uint32_t addr, const uint8_t *src, uint32_t len) {
    WDT_SCOPE(WDT_SITE_FLASH_PROGRAM);
    if (anim_active()) return false;
    if (!len || len > FLASH_PAGE) return false;
    if ((addr & (FLASH_PAGE - 1u)) + len > FLASH_PAGE) return false;
    if (!flash_writable(addr, len)) return false;
    lcd_flash_init();
    if (flash_busy()) return false;
    flash_wren();
    flash_cmd_addr(FLASH_CMD_PAGE_PROG, addr);
    for (uint32_t i = 0; i < len; i++) spi1_xfer(src[i]);
    gpio_write_pin(FLASH_CS, 1);
    return true;
}

// CRC32 (IEEE, reflected) folded over a flash range, resuming from a caller-held
// accumulator so a large verify can be split across many short calls. Reading a
// whole range in one go blocks for the entire read -- see the CRC_SLICE note in
// ak820pro.c. Seed with 0xFFFFFFFF and invert the final result.
uint32_t flash_crc32_acc(uint32_t crc, uint32_t addr, uint32_t len) {
    lcd_flash_init();
    flash_cmd_addr(FLASH_CMD_READ, addr);
    for (uint32_t i = 0; i < len; i++) {
        crc ^= spi1_rw(0xFF);
        for (uint8_t b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1)));
    }
    gpio_write_pin(FLASH_CS, 1);
    return crc;
}

uint32_t flash_crc32(uint32_t addr, uint32_t len) {
    return ~flash_crc32_acc(0xFFFFFFFFu, addr, len);
}

// ---------------------------------------------------------------------------
// Flash-resident asset index (Stage D)
//
// res/mkraw.py --flash packs every asset into one blob written at
// FLASH_ASSET_BASE: a 4K index sector, then the assets page-aligned. Entry
// offsets are stored RELATIVE to the region base so the blob can be relocated.
//
// Font atlases are packed as per-glyph CONTIGUOUS tiles, not as a wide atlas
// image: a glyph cell inside an atlas is strided (cell_w wide, img_w apart) and
// the DMA can only stream consecutive bytes, so an atlas is undrawable by it.
// Glyph n is therefore one flat blit at off + n*cell_w*cell_h*2.
// ---------------------------------------------------------------------------
#define FA_MAGIC   0x53414B41u   // "AKAS"
#define FA_MAX     32

static flash_asset_t fa_tab[FA_MAX];
static uint8_t       fa_count = 0;

bool flash_assets_init(void) {
    uint8_t hdr[8];
    fa_count = 0;
    lcd_flash_init();
    flash_read_bytes(FLASH_ASSET_BASE, hdr, sizeof hdr);
    uint32_t magic = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                     ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    if (magic != FA_MAGIC || hdr[4] != 1) return false;   // absent or wrong version
    uint8_t n = hdr[5] > FA_MAX ? FA_MAX : hdr[5];

    uint8_t e[16];
    for (uint8_t i = 0; i < n; i++) {
        flash_read_bytes(FLASH_ASSET_BASE + 8u + (uint32_t)i * 16u, e, sizeof e);
        flash_asset_t *a = &fa_tab[i];
        a->id     = (uint16_t)(e[0] | (e[1] << 8));
        a->off    = (uint32_t)e[2] | ((uint32_t)e[3] << 8) | ((uint32_t)e[4] << 16);
        a->fmt    = e[5];
        a->w      = (uint16_t)(e[6] | (e[7] << 8));
        a->h      = (uint16_t)(e[8] | (e[9] << 8));
        a->cell_w = e[10]; a->cell_h = e[11];
        a->first  = e[12]; a->count  = e[13];
    }
    fa_count = n;
    return true;
}

uint8_t flash_assets_count(void) { return fa_count; }

const flash_asset_t *flash_asset(uint16_t id) {
    for (uint8_t i = 0; i < fa_count; i++)
        if (fa_tab[i].id == id) return &fa_tab[i];
    return NULL;
}

// Blit a flash asset and wait for the DMA to finish. Bounded: a stuck DMA must
// not wedge the caller. A 24x24 icon is ~0.5 ms; the 128x128 splash ~13 ms.
static void blit_flash_sync(uint32_t src, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    lcd_blit_flash(src, x, y, w, h);
    lcd_blit_wait();
}

void lcd_draw_flash_image(uint16_t id, uint16_t x, uint16_t y) {
    const flash_asset_t *a = flash_asset(id);
    if (!a) return;
    blit_flash_sync(FLASH_ASSET_BASE + a->off, x, y, a->w, a->h);
}

// Draw one glyph of a flash font: tile index (c - first), each cell_w*cell_h.
void lcd_draw_flash_glyph(uint16_t font_id, char c, uint16_t x, uint16_t y) {
    const flash_asset_t *a = flash_asset(font_id);
    if (!a || a->fmt != 1) return;
    if ((uint8_t)c < a->first || (uint8_t)c >= a->first + a->count) return;
    uint32_t tile = (uint32_t)((uint8_t)c - a->first) * a->cell_w * a->cell_h * 2u;
    blit_flash_sync(FLASH_ASSET_BASE + a->off + tile, x, y, a->cell_w, a->cell_h);
}

/* Arm ONE glyph's flash->LCD DMA and return immediately.
 *
 * The blocking wrapper (blit_flash_sync) is what made a 20-glyph line cost
 * ~53 ms on the loop that also scans the matrix -- not the DMA itself, which
 * costs the CPU nothing. The caller pumps these from housekeeping_task_kb() at
 * main-loop rate and simply skips a turn while lcd_blit_busy(), so a line lands
 * in ~50 ms of wall clock and ~0 ms of CPU.
 *
 * ⚠️ An earlier attempt composed the line in RAM and blitted once. That looked
 * like the obvious win and is strictly WORSE here: flash_read_bytes() pulls
 * every pixel through spi1_rw(), which is a full spiExchange() driver call PER
 * BYTE -- ~5.5 KB of them for one 12-char line at 20px. Do not re-derive it.
 *
 * Returns false if the bus is busy (nothing armed; try again next pass) or the
 * glyph is not in the atlas. */
bool lcd_draw_flash_glyph_try(uint16_t font_id, char c, uint16_t x, uint16_t y) {
    if (lcd_blit_busy()) return false;
    const flash_asset_t *a = flash_asset(font_id);
    if (!a || a->fmt != 1) return true;            /* nothing to draw: consume it */
    uint8_t ch = (uint8_t)c;
    if (ch < a->first || ch >= a->first + a->count) ch = ' ';
    if (ch < a->first || ch >= a->first + a->count) return true;
    uint32_t tile = (uint32_t)(ch - a->first) * a->cell_w * a->cell_h * 2u;
    lcd_flash_init();
    lcd_blit_flash(FLASH_ASSET_BASE + a->off + tile, x, y, a->cell_w, a->cell_h);
    return true;
}

uint16_t lcd_font_advance(uint16_t font_id) {
    const flash_asset_t *a = flash_asset(font_id);
    return (a && a->fmt == 1) ? a->cell_w : 0;
}

uint16_t lcd_font_height(uint16_t font_id) {
    const flash_asset_t *a = flash_asset(font_id);
    return (a && a->fmt == 1) ? a->cell_h : 0;
}

/* Blocking whole-string draw. Still used by the band owners that paint rarely
 * and briefly (clock, battery, locks, connection digit). The host text slot
 * goes through the glyph queue instead -- it is the long one, and the only one
 * that ran often enough to cost a keystroke. */
void lcd_draw_flash_text(uint16_t font_id, uint16_t x, uint16_t y, const char *s) {
    const flash_asset_t *a = flash_asset(font_id);
    if (!a) return;
    for (; *s; s++, x += a->cell_w) lcd_draw_flash_glyph(font_id, *s, x, y);
}

uint16_t lcd_flash_text_width(uint16_t font_id, const char *s) {
    const flash_asset_t *a = flash_asset(font_id);
    return a ? (uint16_t)(strlen(s) * a->cell_w) : 0;
}

static volatile bool blit_done = true;

/* Blits armed since the last read. The missed-completion failure scales with
 * how many blits run, so this is the number that says whether a display change
 * raised the risk -- and it is the only way to check that without guessing. */
static uint32_t blit_count = 0;
static uint32_t blit_len_words = 0;   // programmed DMACNT, for the timeout report
/* Last blit's parameters, so a transfer that NEVER STARTED can be re-armed
 * exactly. Safe to repeat precisely because nothing partial happened -- see
 * lcd_blit_wait(). */
static uint32_t blit_src = 0;
static uint16_t blit_x = 0, blit_y = 0, blit_w = 0, blit_h = 0;
static uint16_t blit_retries = 0;
static uint16_t blit_retry_successes = 0;
static uint16_t blit_busy_waits = 0;   /* see bus_quiesce() */

#ifdef CONSOLE_ENABLE
/* Arm-window timing, instrumented builds only (crash hunt, 2026-09-23). The
 * hypothesis it tested -- an interrupt landing between the flash READ command
 * and Fire() loses the DMA trigger -- was REFUTED: 12% of arms have a slow
 * command phase, and none of the failures did. Kept as a base-rate line.
 * ST ticks are 5.33 us; an unpreempted command phase is a tick or two, the
 * row ISR alone ~35. `cmd` = CS low to the last address byte, `fire` = from
 * there, across the SPI1 IC clear, to DMAEN. */
#define ARM_SLOW_TICKS 10u
static uint16_t arm_cmd_ticks, arm_fire_ticks;          /* the most recent arm */
static uint32_t arms_total, arms_slow_cmd, arms_slow_fire;
/* Result: every never-start armed fast, so the arm window was not it. They
 * were all 660-byte clock digits, and the cause was a completion lost in the
 * SPI0 handler (ChibiOS c57623d0d2; plans/FIRMWARE-FINDINGS-2026-09-23.md).
 * The diagnostics that found it stay, for the next hunt: the blit before
 * each arm and the gap since it completed; arms, timeouts and the handler's
 * rescues per transfer size; the SPI state just before Fire(); and CURCNT. */
static uint32_t arm_prev_src, arm_prev_bytes;           /* the blit before this one */
static uint16_t arm_gap_ticks;                          /* its completion -> this arm */
static volatile systime_t blit_done_at;                 /* set by blit_done_cb */
#define SIZE_SLOTS 16u  /* boot draws many clear sizes; last slot pools the rest */
static uint32_t size_bytes[SIZE_SLOTS], size_arms[SIZE_SLOTS], size_never[SIZE_SLOTS];
/* Completions the SPI0 handler rescued (its re-check found a DMATCIF that
 * raced its flag clear), per size. The timing model puts them only on
 * transfers that complete 188-258 us after the arm: measured 2026-09-23 on
 * 660 and 672 bytes only, none of ~530,000 others. */
static uint32_t size_rescued[SIZE_SLOTS], arm_rescues;
static uint8_t  arm_size_slot;
/* SPI0/SPI1 as the arm leaves them, just before Fire(): STAT and RIS, and
 * how often either RX FIFO is not empty or SPI0 is still busy (never, in
 * 580,000 arms). A full SPI0 RX FIFO at a timeout is the junk the panel side
 * receives while the DMA shifts pixels out -- evidence the transfer ran. */
static uint8_t  pre_s0, pre_ris0, pre_s1, pre_ris1;
static uint32_t arms_s0_rx, arms_s0_busy, arms_s1_rx;
/* DMACNT does NOT count down: it holds the programmed length through and
 * after a transfer (every completion read it unchanged, 2026-09-23). Progress
 * is CURCNT, "count from 0 to DMACNT". So at each completion: CURCNT at the
 * programmed length is a full transfer (`tc_full`), anything else is counted
 * (`tc_short`) with the last value kept; and CURCNT just before Fire() says
 * whether it restarts from 0 (`pre_cur`, `arms_cur_stale`). */
static uint32_t tc_full, tc_short, tc_short_cur, tc_short_len;
static uint32_t pre_cur, arms_cur_stale;
static uint8_t size_slot(uint32_t bytes) {
    for (uint8_t i = 0; i < SIZE_SLOTS - 1u; i++) {
        if (size_bytes[i] == bytes) return i;
        if (size_bytes[i] == 0) { size_bytes[i] = bytes; return i; }
    }
    return SIZE_SLOTS - 1u;
}
#endif
static bool     blit_retrying = false;
/* Since boot, never reset: the exposure a soak reports against (finding 15). */
static uint32_t blits_issued = 0;
static uint16_t blit_kinds[BLIT_FAULT_KINDS];

uint16_t lcd_blit_retries(void) { return blit_retries; }

/* While a flash->LCD DMA is in flight, SPI0's interrupt enable holds only the
 * DMA bits and SPI1's NVIC vector is off (Prepare() disabled it): a spiSend()
 * or spiExchange() on EITHER bus waits for an interrupt that cannot fire, and
 * ChibiOS gives it no timeout. The watchdog then resets the board -- retained
 * record of the first provoked hang (crash hunt, 2026-09-22 21:18): "lcd_
 * transfer within text", a re-arm in lcd_blit_flash(). A second review found
 * the same hole in the CPU draws (the Caps padlock, battery fill, icons, via
 * lcd_fill_rect) and in every external-flash transaction.
 *
 * So every CPU transaction on either bus starts here, and a transfer still in
 * flight is waited out through the bounded, recovering lcd_blit_wait() --
 * normally well under a millisecond. Counted: each is an overlap that could
 * have hung before this existed (not necessarily one that would have).
 *
 * NOT in lcd_window(): lcd_blit_flash() calls it AFTER clearing blit_done for
 * its own transfer, and would wait on itself. Only main-loop code arms a DMA,
 * so a transaction that starts quiesced stays quiesced to its end. */
static void bus_quiesce(void) {
    if (blit_done) return;
    if (blit_busy_waits < 0xFFFFu) blit_busy_waits++;
    lcd_blit_wait();
}

uint32_t lcd_blit_count_take(void) { uint32_t n = blit_count; blit_count = 0; return n; }

// DMA completion is serviced by the driver's SPI0 handler (the spiSN32FlashDma
// extension); it calls blit_done_cb below. No Vector58 here anymore.
static void blit_done_cb(void) {
    gpio_write_pin(FLASH_CS, 1);
    cs(1);
#ifdef CONSOLE_ENABLE
    blit_done_at = chVTGetSystemTimeX();
    if (spiSN32FlashDmaRescues() != arm_rescues) size_rescued[arm_size_slot]++;
    {
        uint32_t cur = SN_SPI0->CURCNT_b.CURCNT;
        if (cur == blit_len_words) tc_full++;
        else { tc_short++; tc_short_cur = cur; tc_short_len = blit_len_words; }
    }
#endif
    blit_done = true;
}

// Stage C: blit a w*h RGB565 tile from flash offset `src` to the panel rect at (x,y).
// Interrupt-driven and NON-BLOCKING: arms the SPI1(flash)->SPI0(LCD) engine and returns;
// Vector58 signals completion via blit_done. Animation frames are just the full-frame case.
// NOTE: the panel's MADCTL orientation is the caller's business -- flash art authored for
// the animation orientation (MADCTL_ANIM) will not match the dashboard's (MADCTL_DASH).
void lcd_blit_flash(uint32_t src, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    WDT_SCOPE(WDT_SITE_LCD_TRANSFER);
    if (!w || !h) return;
    /* NEVER RE-ARM UNDER A TRANSFER STILL IN FLIGHT. The glyph pump arms a DMA
     * and returns (lcd_draw_flash_glyph_try), and on the LAST glyph it resets
     * the queue -- so gq_pending() reads false while that transfer is still
     * running. A synchronous draw later in the same main-loop pass (the 10 Hz
     * block: a band clear, an icon, a lock indicator) came straight here, and
     * Prepare() rewrote SPI0 under the live DMA. That stopped it before its
     * completion IRQ could hand SPI0 back to FIFO mode, so IE was left with
     * only the DMA bits, the window command's spiSend() waited for an RX
     * interrupt that could no longer fire -- with no timeout -- and the
     * watchdog reset the board. Retained record of the first provoked hang
     * (crash hunt, 2026-09-22 21:18): "lcd_transfer within text".
     *
     * Wait it out through the bounded, recovering wait (normally well under a
     * millisecond). The CPU draw and flash paths had the same hole; every
     * CPU transaction on either bus now goes through bus_quiesce(). */
    bus_quiesce();
    // SPI1 must be up or the DMA has a dead source: it never completes, the
    // caller spins out its timeout, and SPI0 is left in DMA mode with FLASH_CS
    // asserted -- which then corrupts the next flash read. This used to be the
    // caller's job and the ordering was load-bearing but invisible (it only
    // worked because flash_assets_init() happened to run first). Cheap: a bool.
    lcd_flash_init();
    blit_count++;
    if (blits_issued != UINT32_MAX) blits_issued++;
    uint32_t bytes = (uint32_t)w * (uint32_t)h * 2u;
#ifdef CONSOLE_ENABLE
    arm_prev_src   = blit_src;
    arm_prev_bytes = blit_len_words + 1u;
    sysinterval_t gap = chVTTimeElapsedSinceX(blit_done_at);
    arm_gap_ticks  = gap > 0xFFFFu ? 0xFFFFu : (uint16_t)gap;
    arm_size_slot  = size_slot(bytes);
    size_arms[arm_size_slot]++;
    arm_rescues    = spiSN32FlashDmaRescues();
#endif
    blit_len_words = bytes - 1;      // what Prepare() loads into DMACNT
    blit_src = src; blit_x = x; blit_y = y; blit_w = w; blit_h = h;
    blit_done = false;
    // SPI0 (sink) into DMA config + counts; SPI1 (source) recorded for Step 2.
    // SPI0 stays 8-bit so the command phase (window) can go out first.
    spiSN32FlashDmaPrepare(&SPID0, &SPID1, bytes);
    SN_SPI1->CTRL0_b.FRESET = 0b11;                 // flash side (bare-metal, ours)
    lcd_window(x, y, x + w - 1, y + h - 1);         // via spiSend (SPI0 still 8-bit)
#ifdef CONSOLE_ENABLE
    systime_t arm_t0 = chVTGetSystemTimeX();
#endif
    gpio_write_pin(FLASH_CS, 0);
    // Prepare() disabled SPID1's NVIC vector for the DMA window, so the READ+addr
    // command goes out via the raw poll primitive (not spiSend, which needs the ISR).
    spi1_raw_byte(FLASH_CMD_READ); spi1_raw_byte((src>>16)&0xFF); spi1_raw_byte((src>>8)&0xFF); spi1_raw_byte(src&0xFF);
#ifdef CONSOLE_ENABLE
    systime_t arm_t1 = chVTGetSystemTimeX();
#endif
    SN_SPI1->IC = 0x3F;
#ifdef CONSOLE_ENABLE
    pre_cur = SN_SPI0->CURCNT_b.CURCNT;
    if (pre_cur != 0) arms_cur_stale++;
    pre_s0 = (uint8_t)SN_SPI0->STAT; pre_ris0 = (uint8_t)SN_SPI0->RIS;
    pre_s1 = (uint8_t)SN_SPI1->STAT; pre_ris1 = (uint8_t)SN_SPI1->RIS;
    if (!(pre_s0 & 0x04u)) arms_s0_rx++;      /* RX_EMPTY clear */
    if (pre_s0 & 0x10u)    arms_s0_busy++;    /* BUSY */
    if (!(pre_s1 & 0x04u)) arms_s1_rx++;
#endif
    // Flip to 16-bit pixels and arm; blit_done_cb fires at completion.
    spiSN32FlashDmaFire(&SPID0, blit_done_cb);
#ifdef CONSOLE_ENABLE
    systime_t arm_t2 = chVTGetSystemTimeX();
    arm_cmd_ticks  = (uint16_t)(arm_t1 - arm_t0);
    arm_fire_ticks = (uint16_t)(arm_t2 - arm_t1);
    arms_total++;
    if (arm_cmd_ticks  > ARM_SLOW_TICKS) arms_slow_cmd++;
    if (arm_fire_ticks > ARM_SLOW_TICKS) arms_slow_fire++;
#endif
}

#ifdef CONSOLE_ENABLE
/* Once a minute from blit_stat_task(): the base rate the never-started
 * timeouts' own cmd/fire ticks are compared against. */
void lcd_blit_arm_report(void) {
    dprintf("[lcd] arms=%lu slow_cmd=%lu slow_fire=%lu never=%u busy=%u\n",
            (unsigned long)arms_total, (unsigned long)arms_slow_cmd,
            (unsigned long)arms_slow_fire, (unsigned)blit_kinds[BLIT_NEVER_STARTED],
            (unsigned)blit_busy_waits);
    dprintf("[lcd] tc: full=%lu short=%lu (last %lu/%lu) arms_cur_stale=%lu\n",
            (unsigned long)tc_full, (unsigned long)tc_short,
            (unsigned long)tc_short_cur, (unsigned long)tc_short_len,
            (unsigned long)arms_cur_stale);
    dprintf("[lcd] pre-fire: s0_rx=%lu s0_busy=%lu s1_rx=%lu\n",
            (unsigned long)arms_s0_rx, (unsigned long)arms_s0_busy,
            (unsigned long)arms_s1_rx);
    /* bytes:arms/timeouts/rescued per transfer size; the last slot pools the rest. */
    dprintf("[lcd] sizes");
    for (uint8_t i = 0; i < SIZE_SLOTS; i++) {
        if (!size_arms[i]) continue;
        if (i == SIZE_SLOTS - 1u) dprintf(" other");
        else dprintf(" %lu", (unsigned long)size_bytes[i]);
        dprintf(":%lu/%lu/%lu", (unsigned long)size_arms[i], (unsigned long)size_never[i],
                (unsigned long)size_rescued[i]);
    }
    dprintf("\n");
}
#endif

// Animation frames are full-screen tiles.
static inline void blit_arm(uint32_t addr) { lcd_blit_flash(addr, 0, 0, FRAME_W, FRAME_H); }

// True once the in-flight DMA blit has completed (Vector58 sets it).
bool lcd_blit_busy(void) { return !blit_done; }

/* Bounded wait that RECOVERS instead of merely giving up.
 *
 * blit_done is cleared when a blit is armed and set only by blit_done_cb, off
 * the SPI0 DMA completion IRQ. If that IRQ is ever missed the flag stays false
 * FOREVER: every later wait spins its full bound, and SPI0 is left in DMA mode
 * with FLASH_CS asserted, which then corrupts the next flash read.
 *
 * That is the "hang". Captured on the console 2026-08-30: an 8-second gap in
 * the scan-rate stream, then 178 Hz against a normal ~400. The board was never
 * dead -- it was spinning ~1 s per blit attempt, several times per housekeeping
 * pass, and it could not recover because nothing else ever writes blit_done.
 * From the outside that is indistinguishable from a parked CPU: raw HID times
 * out, typing is lost, and only a power cycle clears it.
 *
 * On timeout: put the bus back exactly where a successful completion would have
 * left it, then declare the blit done. One dropped frame beats a permanent
 * crawl, and the count makes the failure visible instead of silent.
 *
 * BLIT_WAIT_SPINS is ~250 ms, generous against the worst real blit (a 32 KB
 * full-screen animation frame is ~11 ms) and 4x tighter than the old 4,000,000
 * that made each stall a full second. */
#define BLIT_WAIT_SPINS 1000000u
/* ~1 ms. Only long enough to see whether the DMA STARTED.
 *
 * MEASURED CONSEQUENCE OF GETTING THIS WRONG: with the window at 10 ms, a
 * never-started blit cost 10 ms to detect plus 10 ms for the retry, and the
 * loop-gap probe reported "Gap 24 blit" -- a 24 ms main-loop stall. At typing
 * speed a character arrives roughly every 125 ms, so a 24 ms hole in the scan
 * is enough to LOSE A KEYSTROKE. This is the link between the DMA fault and
 * the dropped-character reports.
 *
 * 10 ms was sized against the worst plausible TRANSFER (a 32 KB full-screen
 * frame is ~11 ms). That was the wrong quantity: phase 1 only has to see the
 * counter move, and the DMA request fires off the SPI1 RX threshold within
 * microseconds of arming. A millisecond is three orders of magnitude of
 * headroom over that.
 *
 * Failure is one-directional: too short and a slow-to-start transfer is
 * misread as never-started and retried, which is harmless because nothing
 * partial happened. Too long and every occurrence costs a keystroke. */
#define BLIT_START_SPINS 4000u

static uint16_t blit_timeouts = 0;

bool lcd_blit_wait(void) {
    WDT_SCOPE(WDT_SITE_LCD_WAIT);
    /* OUTERMOST MARK WINS. This wait is NESTED inside other marked
     * operations -- backing_store_pre_write_hook() marks FLASH and then calls
     * this to drain the DMA, and rtc_bus_guard() marks I2C and does the same.
     * An unconditional store here made every flash stall report as "blit",
     * which sent the first phase-1 measurement chasing the wrong subsystem. */
    if (loop_stall_mark == LOOP_MARK_NONE) loop_stall_mark = LOOP_MARK_BLIT;
    /* PHASE 1 -- did the DMA actually start? ⚠️ DMACNT NEVER MOVES: it holds
     * the programmed length through and after the transfer (measured
     * 2026-09-23; progress is CURCNT). So this really asks "did a DMA flag
     * show before the ISR cleared it", and a transfer that started without
     * one being caught reads as not started. Harmless for the recovery (the
     * retry repaints the whole window) but it makes the classification below
     * a symptom label, not a diagnosis. Reworking it on CURCNT/DMAEN is open
     * (plans/FIRMWARE-FINDINGS-2026-09-23.md, disposition 4). */
    bool started = false;
    for (uint32_t i = 0; i < BLIT_START_SPINS && !blit_done; i++) {
        if (SN_SPI0->DMACNT_b.CNT != blit_len_words || (SN_SPI0->RIS & 0x30u)) {
            started = true;
            break;
        }
    }
    /* PHASE 2 -- a flag was seen, so give it the generous bound to finish
     * (measured at up to 1.95 s of wall time under the row ISR's load). */
    if (started) {
        for (uint32_t i = 0; i < BLIT_WAIT_SPINS && !blit_done; i++) {
            __asm__ volatile("nop");
        }
    }
    if (blit_done) return true;

    /* "NEVER STARTED" -> retry once, and the frame is not even lost.
     *
     * Every such timeout on record (hundreds, 2026-08-30 to 2026-09-23) was
     * in fact a transfer that had COMPLETED -- CURCNT full, DMAEN cleared by
     * the hardware -- whose DMATCIF the SPI0 handler cleared unseen. Fixed in
     * the driver (ChibiOS c57623d0d2): E5 ran 580,000 blits with none. The
     * retry was right for a different reason than it used to claim: it
     * reissues the window command and RAMWR, so it repaints the whole
     * rectangle whatever the first attempt did (codex review, 2026-09-23).
     *
     * One retry, not a loop: if a second arm also fails, something is wrong
     * beyond one lost event and spinning on it would be the original bug
     * again. blit_retrying guards against recursing through the abort. */
    /* ONE observation decides everything below, taken with the completion ISR
     * held off (crash-hunt implementation review, finding 5):
     *   - blit_done is re-checked: a completion that landed after the wait
     *     gave up is a success, not a timeout;
     *   - the registers are read once, BEFORE the abort -- which clears the
     *     SPI flags and the NVIC pending state, so a read afterwards (as the
     *     console line once did) describes the recovery, not the fault;
     *   - never_started comes from this snapshot, not the start loop. The loop
     *     can only prove it saw no movement WHILE IT RAN; a transfer that began
     *     just after would still read started == false, and retrying it would
     *     replay a blit that had already pushed pixels onto the panel.
     *
     * The abort goes through the LLD, which restores BOTH controllers --
     * crucially it re-enables SPI1's NVIC vector, which Prepare() disabled for
     * the DMA window. An earlier version of this recovery reset the flash FIFO
     * by hand and skipped that, leaving SPI1 deaf: the board went totally
     * silent within two seconds of the "recovery", which was far worse than
     * the stall. */
    chSysLock();
    bool completed = blit_done;
    uint32_t ris = SN_SPI0->RIS & 0x3Fu;
    uint32_t cnt = SN_SPI0->DMACNT_b.CNT;
    uint32_t s0 = SN_SPI0->STAT, s1 = SN_SPI1->STAT, ris1 = SN_SPI1->RIS & 0x3Fu;
#ifdef CONSOLE_ENABLE
    uint32_t cur = SN_SPI0->CURCNT_b.CURCNT, dmactrl = SN_SPI0->DMACTRL;
#endif
    if (!completed) spiSN32FlashDmaAbort(&SPID0);
    chSysUnlock();
    (void)s0; (void)s1; (void)ris1;   /* console-only: dprintf is empty on the daily build */
    if (completed) return true;
    bool never_started = !started && cnt == blit_len_words && (ris & 0x30u) == 0u;

    gpio_write_pin(FLASH_CS, 1);          // then the CS lines, as blit_done_cb does
    cs(1);
    blit_done = true;

    if (blit_timeouts < 0xFFFFu) blit_timeouts++;
    /* ⚠️ DMACNT never moves (see phase 1), so `cnt` is always the programmed
     * length and this table degenerates to "was a flag pending": never
     * started = no flag; IRQ lost / stalled cannot occur as written. Kept for
     * the wire format (page 6) until it is reworked on CURCNT and DMAEN. The
     * table as originally designed:
     *
     *   cnt == the programmed length  -> the transfer never started
     *   cnt somewhere in between      -> the SOURCE starved mid-transfer, which
     *                                    is what an SPI1 glitch looks like (see
     *                                    the RTC I2C / port-A note below)
     *   cnt == 0, ris DMATCIF set     -> it finished and the IRQ was LOST, i.e.
     *                                    an interrupt-delivery problem, not a bus one
     *
     * ris bit5 = DMATCIF (transfer complete), bit4 = DMAHTIF (half).
     *
     * Anything else -- a completion that landed between the wait giving up and
     * the snapshot, a counter at zero with no flag -- is UNKNOWN, kept apart
     * rather than forced into one of the three. Counted on every build: the
     * daily one has no console. */
    enum blit_fault kind = never_started                          ? BLIT_NEVER_STARTED
                         : (cnt == 0 && (ris & 0x20u))            ? BLIT_IRQ_LOST
                         : (cnt != 0 && cnt < blit_len_words)     ? BLIT_STALLED
                                                                  : BLIT_UNKNOWN;
    if (blit_kinds[kind] < 0xFFFFu) blit_kinds[kind]++;
#ifdef CONSOLE_ENABLE
    dprintf("[lcd] blit timeout #%u kind=%u ris=%02lx cnt=%lu/%lu s0=%lx s1=%lx ris1=%02lx cmd=%u fire=%u i2c=%u\n",
            (unsigned)blit_timeouts, (unsigned)kind,
            (unsigned long)ris, (unsigned long)cnt, (unsigned long)blit_len_words,
            (unsigned long)s0, (unsigned long)s1, (unsigned long)ris1,
            (unsigned)arm_cmd_ticks, (unsigned)arm_fire_ticks,
            (unsigned)rtc_i2c_overlaps());
    dprintf("[lcd]   src=%06lx %ux%u@%u,%u prev=%06lx/%lu gap=%u pre0=%02x/%02x pre1=%02x/%02x\n",
            (unsigned long)blit_src, (unsigned)blit_w, (unsigned)blit_h,
            (unsigned)blit_x, (unsigned)blit_y, (unsigned long)arm_prev_src,
            (unsigned long)arm_prev_bytes, (unsigned)arm_gap_ticks,
            (unsigned)pre_s0, (unsigned)pre_ris0, (unsigned)pre_s1, (unsigned)pre_ris1);
    dprintf("[lcd]   curcnt=%lu/%lu pre_cur=%lu dmactrl=%lx started=%u\n",
            (unsigned long)cur, (unsigned long)blit_len_words, (unsigned long)pre_cur,
            (unsigned long)dmactrl, (unsigned)started);
    size_never[arm_size_slot]++;             /* every timeout, of any kind */
#endif

    if (never_started && !blit_retrying && blit_w && blit_h) {
        if (blit_retries < 0xFFFFu) blit_retries++;   /* attempts, before the outcome */
        blit_retrying = true;
        lcd_blit_flash(blit_src, blit_x, blit_y, blit_w, blit_h);
        bool ok = lcd_blit_wait();
        blit_retrying = false;
        if (ok && blit_retry_successes < 0xFFFFu) blit_retry_successes++;
        return ok;
    }

    return false;
}

uint16_t lcd_blit_timeouts(void) { return blit_timeouts; }

void lcd_blit_stats(lcd_blit_stats_t *out) {
    for (unsigned i = 0; i < BLIT_FAULT_KINDS; i++) out->kinds[i] = blit_kinds[i];
    out->busy_waits      = blit_busy_waits;
    out->retry_successes = blit_retry_successes;
    out->issued          = blits_issued;
}

// The RAM/CPU text and image helpers are gone: all art is flash-resident and
// DMA-drawn now (lcd_draw_flash_*). lcd_blit_ram() stays for anything that
// still needs to push a RAM tile.

// ---------------------------------------------------------------------------
// Animation player
// ---------------------------------------------------------------------------
static bool     anim_on  = false;
static uint8_t  anim_idx = 0;
static uint8_t  anim_count = 0;              // from the header, 0 = nothing to play

// Read the slot header. Returns false if it describes nothing playable, which is
// the normal state for an empty or never-provisioned slot.
static bool anim_read_header(void) {
    uint8_t hdr;
    lcd_flash_init();
    flash_read_bytes(ANIM_BASE, &hdr, 1);
    anim_count = hdr > ANIM_MAX ? 0 : hdr;
    return anim_count != 0;
}

// True while the flash-animation player owns the bus. The bit-banged RTC I2C (SCL=A14,
// SDA=A15) shares port A with the flash SPI1 pins (SCK=A12, CS=A13); its open-drain
// pin-mode toggling glitches A12/A13 mid-DMA and corrupts the flash read. Callers must
// suspend RTC polling while this is true.
bool anim_active(void) { return anim_on; }

static void set_madctl(uint8_t v) { bus_quiesce(); cs(0); dc(0); tx8(0x36); dc(1); tx8(v); cs(1); }

// SPI1 (external flash) is brought up lazily -- lcd_blit_flash does NOT do it,
// so any caller outside the animation path must call this first.
void lcd_flash_init(void) {
    if (!spi1_inited) { spi1_setup(); spi1_inited = true; }
}

// One-shot self-contained flash blit: brings up SPI1, blits, waits with a bound,
// then puts SPI0 back exactly as anim_toggle's stop path does so the dashboard
// runs unaffected. The wait is bounded on purpose -- completion rides the SPI0
// IRQ, and an unbounded spin here hangs the keyboard before USB enumerates.
void lcd_blit_flash_probe(uint32_t src, uint16_t w, uint16_t h) {
    lcd_flash_init();
    lcd_blit_flash(src, 0, 0, w, h);
    lcd_blit_wait();
    // The DMA extension already restored SPI0 to the driver's 8-bit FIFO mode at
    // completion; nothing to tear down here.
    gpio_write_pin(FLASH_CS, 1); cs(1);
}

void anim_toggle(void) {
    lcd_flash_init();
    if (!anim_on) {
        /* Check the slot BEFORE disturbing anything. This used to pause the
         * dashboard and flip the panel orientation first, then discover there
         * were no frames and undo both -- and display_set_paused(false) forces a
         * FULL REPAINT, so an empty slot blinked the whole screen black for
         * about a second and did nothing. On this board the stock header reads
         * zero frames, so that was the ONLY thing Fn+Delete ever did.
         *
         * Safe to read here: it is a 1-byte SPI1 flash read, and the dashboard
         * reads flash constantly anyway (every glyph comes from there via
         * lcd_draw_flash_text). */
        if (!anim_read_header()) return;    // empty slot: true no-op
        display_set_paused(true);           // stop QP touching the bus
        set_madctl(MADCTL_ANIM);            // frames authored for this orientation
        anim_on = true; anim_idx = 0;
        blit_arm(ANIM_BASE + ANIM_HDR);
    } else {
        anim_on = false;
        /* Was an UNBOUNDED spin. A missed completion IRQ leaves blit_done
         * false forever, so this would wedge the main loop with no way out at
         * all -- strictly worse than the bounded waits elsewhere, and in the
         * same failure. lcd_blit_wait() gives up and puts the bus back. */
        lcd_blit_wait();
        gpio_write_pin(FLASH_CS, 1); cs(1);
        // The DMA extension restored SPI0 to the driver's 8-bit FIFO mode at the
        // last frame's completion, so the dashboard's spiSend path is ready again.
        set_madctl(MADCTL_DASH);             // restore dashboard orientation
        display_set_paused(false);          // resume + full repaint
    }
}
// Called from the 10 Hz housekeeping slot, so one frame per 100 ms.
void anim_task(void) {
    if (!anim_on || !blit_done) return;     // previous frame still in flight
    anim_idx = (uint8_t)((anim_idx + 1) % anim_count);
    blit_arm(ANIM_BASE + ANIM_HDR + (uint32_t)anim_idx * ANIM_STRIDE);
}
