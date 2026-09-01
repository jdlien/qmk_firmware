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

// GC9107 MADCTL. fpb's units want rotation 270 = BGR(0x08) | MV(0x20) | MY(0x80)
// = 0xA8. This unit (shipped on stock v1.10) has the panel mounted 180 deg from
// that and rendered the dashboard upside down, so trade MY for MX:
// BGR(0x08) | MV(0x20) | MX(0x40) = 0x68. The dashboard and the animation share
// this orientation.
#define MADCTL_DASH 0x68
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
    if (x1 < x0 || y1 < y0) return;
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
    if (!px || !w || !h) return;
    lcd_window(x, y, x + w - 1, y + h - 1);
    tx_pixels(px, (uint32_t)w * (uint32_t)h);
    cs(1);
}

// ---------------------------------------------------------------------------
// Panel bring-up (bare-metal GC9107 init; literal opcodes, no Quantum Painter)
// ---------------------------------------------------------------------------
static void send_cmd(uint8_t c) { dc(0); tx8(c); dc(1); }
static void send_seq(const uint8_t *seq, uint32_t len) {   // cmd, delay_ms, nparams, params...
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
        0x21, 0, 0,                 // display inversion ON. Without it this panel
                                    // renders the assets' white-on-black artwork as
                                    // black-on-white. Set before sleep-out so the
                                    // very first frame is already correct.
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
    gpio_write_pin(FLASH_CS, 0);
    spi1_xfer(cmd);
    spi1_xfer((a >> 16) & 0xFF); spi1_xfer((a >> 8) & 0xFF); spi1_xfer(a & 0xFF);
}

static uint8_t flash_status(void) {
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
    gpio_write_pin(FLASH_CS, 0);
    spi1_xfer(FLASH_CMD_WREN);
    gpio_write_pin(FLASH_CS, 1);
}

uint32_t flash_jedec_id(void) {
    lcd_flash_init();
    gpio_write_pin(FLASH_CS, 0);
    spi1_xfer(FLASH_CMD_JEDEC);
    uint32_t id = ((uint32_t)spi1_rw(0xFF) << 16);
    id |= ((uint32_t)spi1_rw(0xFF) << 8);
    id |= spi1_rw(0xFF);
    gpio_write_pin(FLASH_CS, 1);
    return id;
}

void flash_read_bytes(uint32_t addr, uint8_t *dst, uint32_t len) {
    lcd_flash_init();
    flash_cmd_addr(FLASH_CMD_READ, addr);
    for (uint32_t i = 0; i < len; i++) dst[i] = spi1_rw(0xFF);
    gpio_write_pin(FLASH_CS, 1);
}

// Erase one 4K sector. Returns false if the address is not writable, the chip
// is still busy, or the animation owns the bus. The chip stays busy for
// 50-300 ms afterwards -- poll flash_busy().
bool flash_erase_sector(uint32_t addr) {
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
static bool     blit_retrying = false;

uint16_t lcd_blit_retries(void) { return blit_retries; }

uint32_t lcd_blit_count_take(void) { uint32_t n = blit_count; blit_count = 0; return n; }

// DMA completion is serviced by the driver's SPI0 handler (the spiSN32FlashDma
// extension); it calls blit_done_cb below. No Vector58 here anymore.
static void blit_done_cb(void) {
    gpio_write_pin(FLASH_CS, 1);
    cs(1);
    blit_done = true;
}

// Stage C: blit a w*h RGB565 tile from flash offset `src` to the panel rect at (x,y).
// Interrupt-driven and NON-BLOCKING: arms the SPI1(flash)->SPI0(LCD) engine and returns;
// Vector58 signals completion via blit_done. Animation frames are just the full-frame case.
// NOTE: the panel's MADCTL orientation is the caller's business -- flash art authored for
// the animation orientation (MADCTL_ANIM) will not match the dashboard's (MADCTL_DASH).
void lcd_blit_flash(uint32_t src, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!w || !h) return;
    // SPI1 must be up or the DMA has a dead source: it never completes, the
    // caller spins out its timeout, and SPI0 is left in DMA mode with FLASH_CS
    // asserted -- which then corrupts the next flash read. This used to be the
    // caller's job and the ordering was load-bearing but invisible (it only
    // worked because flash_assets_init() happened to run first). Cheap: a bool.
    lcd_flash_init();
    blit_count++;
    uint32_t bytes = (uint32_t)w * (uint32_t)h * 2u;
    blit_len_words = bytes - 1;      // what Prepare() loads into DMACNT
    blit_src = src; blit_x = x; blit_y = y; blit_w = w; blit_h = h;
    blit_done = false;
    // SPI0 (sink) into DMA config + counts; SPI1 (source) recorded for Step 2.
    // SPI0 stays 8-bit so the command phase (window) can go out first.
    spiSN32FlashDmaPrepare(&SPID0, &SPID1, bytes);
    SN_SPI1->CTRL0_b.FRESET = 0b11;                 // flash side (bare-metal, ours)
    lcd_window(x, y, x + w - 1, y + h - 1);         // via spiSend (SPI0 still 8-bit)
    gpio_write_pin(FLASH_CS, 0);
    // Prepare() disabled SPID1's NVIC vector for the DMA window, so the READ+addr
    // command goes out via the raw poll primitive (not spiSend, which needs the ISR).
    spi1_raw_byte(FLASH_CMD_READ); spi1_raw_byte((src>>16)&0xFF); spi1_raw_byte((src>>8)&0xFF); spi1_raw_byte(src&0xFF);
    SN_SPI1->IC = 0x3F;
    // Flip to 16-bit pixels and arm; blit_done_cb fires at completion.
    spiSN32FlashDmaFire(&SPID0, blit_done_cb);
}

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
#ifdef LOOPGAP_INSTRUMENT
    loop_stall_mark = LOOP_MARK_BLIT;
#endif
    /* PHASE 1 -- did the DMA actually start? Watch DMACNT move (or a transfer
     * flag appear) rather than waiting out a whole transfer. A blit that never
     * starts is detected in ~10 ms instead of 250 ms, so the retry lands before
     * the stall is visible. */
    bool started = false;
    for (uint32_t i = 0; i < BLIT_START_SPINS && !blit_done; i++) {
        if (SN_SPI0->DMACNT_b.CNT != blit_len_words || (SN_SPI0->RIS & 0x30u)) {
            started = true;
            break;
        }
    }
    /* PHASE 2 -- it is moving, so give it the generous bound to finish. A
     * started transfer must NOT be cut short: it has already pushed pixels, so
     * it cannot be retried blind. */
    if (started) {
        for (uint32_t i = 0; i < BLIT_WAIT_SPINS && !blit_done; i++) {
            __asm__ volatile("nop");
        }
    }
    if (blit_done) return true;

    /* NEVER STARTED -> retry once, and the frame is not even lost.
     *
     * Measured on hardware over 1h45m: all 13 failures read cnt == the
     * programmed length with neither DMATCIF nor DMAHTIF set. The transfer had
     * not begun, so nothing partial happened, nothing was half-written to the
     * panel, and re-arming is exactly as safe as arming was the first time.
     * That is a much better answer than dropping the frame, and it is only
     * valid for this specific reading -- a transfer that stalled PARTWAY has
     * pushed pixels already and must not be repeated blind.
     *
     * One retry, not a loop: if a second arm also fails to start, something is
     * wrong beyond a missed trigger and spinning on it would be the original
     * bug again. blit_retrying guards against recursing through the abort. */
    /* Re-read the registers HERE rather than trusting the phase-1 result alone.
     * The start loop can only prove it saw no movement WHILE IT RAN; a transfer
     * that began just after the loop exited would still have started == false,
     * and retrying it would replay a blit that had already pushed pixels. The
     * window is tiny but the consequence is a corrupted panel, so confirm the
     * counter is still untouched at the moment we decide. */
    bool never_started = !started &&
                         (SN_SPI0->DMACNT_b.CNT == blit_len_words) &&
                         ((SN_SPI0->RIS & 0x30u) == 0u);


    /* Abort through the LLD, which restores BOTH controllers -- crucially it
     * re-enables SPI1's NVIC vector, which Prepare() disabled for the DMA
     * window. An earlier version of this recovery reset the flash FIFO by hand
     * and skipped that, leaving SPI1 deaf: the board went totally silent within
     * two seconds of the "recovery", which was far worse than the stall. */
    spiSN32FlashDmaAbort(&SPID0);
    gpio_write_pin(FLASH_CS, 1);          // then the CS lines, as blit_done_cb does
    cs(1);
    blit_done = true;

    if (blit_timeouts < 0xFFFFu) blit_timeouts++;
    /* Capture BEFORE the abort clears anything. DMACNT is the discriminator and
     * distinguishes three completely different faults with three different fixes:
     *
     *   cnt == the programmed length  -> the transfer never started
     *   cnt somewhere in between      -> the SOURCE starved mid-transfer, which
     *                                    is what an SPI1 glitch looks like (see
     *                                    the RTC I2C / port-A note below)
     *   cnt == 0, ris DMATCIF set     -> it finished and the IRQ was LOST, i.e.
     *                                    an interrupt-delivery problem, not a bus one
     *
     * ris bit5 = DMATCIF (transfer complete), bit4 = DMAHTIF (half). */
    dprintf("[lcd] blit timeout #%u ris=%02lx cnt=%lu/%lu s0=%lx s1=%lx i2c=%u\n",
            (unsigned)blit_timeouts,
            (unsigned long)(SN_SPI0->RIS & 0x3F),
            (unsigned long)SN_SPI0->DMACNT_b.CNT,
            (unsigned long)blit_len_words,
            (unsigned long)SN_SPI0->STAT,
            (unsigned long)SN_SPI1->STAT,
            (unsigned)rtc_i2c_overlaps());

    if (never_started && !blit_retrying && blit_w && blit_h) {
        if (blit_retries < 0xFFFFu) blit_retries++;
        blit_retrying = true;
        lcd_blit_flash(blit_src, blit_x, blit_y, blit_w, blit_h);
        bool ok = lcd_blit_wait();
        blit_retrying = false;
        return ok;
    }

    return false;
}

uint16_t lcd_blit_timeouts(void) { return blit_timeouts; }

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

static void set_madctl(uint8_t v) { cs(0); dc(0); tx8(0x36); dc(1); tx8(v); cs(1); }

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
