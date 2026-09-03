// Copyright 2026 Fernando Birra
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Bare-metal LCD bus for the AK820 Pro: we own SPI0 (+ its IRQ Vector58) and SPI1
// (flash), so the SPI-to-SPI flash->LCD DMA can be interrupt-driven. The GC9107 panel
// and dashboard are driven entirely bare-metal (no Quantum Painter, no ChibiOS SPI).
// See docs/LCD_FLASH_LAYER.md.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "res/flash_assets.h" // ASSET_* ids for the flash-resident set

// Bring up the GC9107 panel (reset + init sequence + rotation 270).
void lcd_init(void);

// Flash-animation player (interrupt-driven DMA; pauses the dashboard, borrows the bus).
void anim_toggle(void);
void anim_task(void);
// True while the animation owns the bus; suspend RTC I2C polling then (shared port A).
bool anim_active(void);

// --- Stage C tile primitives (see docs/LCD_FLASH_LAYER.md) -------------------
// The SN32 DMA is SPI-to-SPI ONLY (source = the other SPI's RX FIFO, no source-address
// register), so only FLASH-resident art can be DMA'd. RAM-resident art is CPU-pushed
// through the pipelined bulk writer -- fast (~wire speed) but blocking.

// DMA, non-blocking: flash tile -> panel rect. Poll lcd_blit_busy() for completion.
void lcd_blit_flash(uint32_t src, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
bool lcd_blit_busy(void);
/* Bounded wait that recovers a stuck blit rather than spinning forever.
 * Returns false if it had to abandon one. */
bool     lcd_blit_wait(void);
uint16_t lcd_blit_timeouts(void);
uint32_t lcd_blit_count_take(void);
uint16_t lcd_blit_retries(void);
void lcd_blit_flash_probe(uint32_t src, uint16_t w, uint16_t h);
// Brings up SPI1 (external flash). lcd_blit_flash does not do this itself, so
// call it before any blit outside the animation path.
void lcd_flash_init(void);

/* Non-blocking single-glyph draw + the font's advance, for the caller-driven
 * glyph pump. See lcd_draw_flash_glyph_try() in lcd_bus.c. */
bool     lcd_draw_flash_glyph_try(uint16_t font_id, char c, uint16_t x, uint16_t y);
uint16_t lcd_font_advance(uint16_t font_id);
uint16_t lcd_font_height(uint16_t font_id);

// --- external flash WRITE path (Stage D provisioning) -----------------------
// Device-asynchronous: the command returns as soon as it is on the wire, then
// the chip is busy on its own (page program ~1-3 ms, sector erase 50-300 ms).
// Poll flash_busy() rather than waiting -- a 300 ms stall wrecks the matrix scan.
// All of these refuse to run while anim_active(), since SPI1 feeds the DMA.
#define FLASH_ASSET_BASE 0x0CE0000u   // 3.12 MB erased since manufacture

bool     flash_busy(void);
bool     flash_erase_sector(uint32_t addr);                              // 4K
bool     flash_page_program(uint32_t addr, const uint8_t *src, uint32_t len);
void     flash_read_bytes(uint32_t addr, uint8_t *dst, uint32_t len);
uint32_t flash_crc32(uint32_t addr, uint32_t len);
// Resumable form: fold len bytes into crc. Seed 0xFFFFFFFF, invert at the end.
// Lets a big verify be split into short calls instead of one blocking read.
uint32_t flash_crc32_acc(uint32_t crc, uint32_t addr, uint32_t len);
uint32_t flash_jedec_id(void);
// Writes are refused below FLASH_ASSET_BASE unless unlocked, and even then only
// inside a known animation slot. The stock LCD assets are never writable: our
// only dump of them has read damage, so they cannot be restored.
bool     flash_writable(uint32_t addr, uint32_t len);
void     flash_set_unlocked(bool on);

// --- flash-resident assets (Stage D) ----------------------------------------
// Uploaded by `ak820ctl flash write 0x0CE0000 flash_assets.bin`. The asset
// authoring pipeline (source PNGs + mkraw.py, which emits both flash_assets.bin
// and the res/flash_assets.h committed here) lives in the time-util-ak820pro
// repo; see keyboards/a_jazz/ak820pro/readme.md.
// Fonts are packed as per-glyph contiguous tiles: an atlas cell is strided and
// the DMA can only stream consecutive bytes, so it could not draw one.
typedef struct {
    uint16_t id;
    uint32_t off;          // relative to FLASH_ASSET_BASE
    uint8_t  fmt;          // 0 = image, 1 = font (glyph tiles)
    uint16_t w, h;         // image size, or glyph cell size for fonts
    uint8_t  cell_w, cell_h, first, count;
} flash_asset_t;

bool                 flash_assets_init(void);   // false if no valid index in flash
uint8_t              flash_assets_count(void);
const flash_asset_t *flash_asset(uint16_t id);

void     lcd_draw_flash_image(uint16_t id, uint16_t x, uint16_t y);
void     lcd_draw_flash_glyph(uint16_t font_id, char c, uint16_t x, uint16_t y);
void     lcd_draw_flash_text(uint16_t font_id, uint16_t x, uint16_t y, const char *s);
/* Same, but composed in RAM and blitted ONCE -- one LCD operation per line
 * instead of one per glyph. Much faster and it appears instantly. */
void     lcd_draw_flash_text_staged(uint16_t font_id, uint16_t x, uint16_t y, const char *s);
uint16_t lcd_flash_text_width(uint16_t font_id, const char *s);

// CPU, blocking: RGB565 tile from a firmware/RAM array -> panel rect.
void lcd_blit_ram(const uint16_t *px, uint16_t x, uint16_t y, uint16_t w, uint16_t h);

// Bare-metal dashboard drawing (Quantum-Painter-free). Images and glyphs are
// pre-rendered RGB565 tiles drawn from external flash (see the flash-asset API
// above) -- no decode, no blending. Colours are baked into the tiles, so there
// are no fg/bg arguments.
void lcd_fill_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);
// Clear to black by DMA from the 128x128 black frame the stock image keeps at
// flash 0x000000. Zero CPU in the data path, vs ~11-13 ms for a full-screen CPU
// fill. Works for any rect: the source is uniform, so striding is irrelevant.
void lcd_clear_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
/* Same, but NON-BLOCKING: arms the DMA and returns. Poll lcd_blit_busy() before
 * touching the panel again.
 *
 * lcd_clear_rect() ends in lcd_blit_wait(), so a FULL-SCREEN clear parks the
 * main loop for the whole 32 KB transfer -- measured 44 ms, which
 * count_ge_25ms_nonflash flags as long enough to lose a keypress. The DMA needs
 * no CPU; waiting for it is pure loss. Use this wherever the caller can come
 * back on a later pass. */
void lcd_clear_rect_async(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
