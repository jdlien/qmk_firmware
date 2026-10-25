// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The panel as the menu's pure code sees it. menu_glue.c implements these on
// lcd_bus.c; the host tests (scripts/menu_s0_test.c) implement them on a fake
// framebuffer, so the state machines they test are the firmware's own.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MB_W 128
#define MB_H 128
#define MB_BAND_H 16                      /* the banded clear, as the restore's */
#define MB_BANDS  (MB_H / MB_BAND_H)
#define MB_GLYPH_W 6                      /* FONT_SMALL's cell */
#define MB_GLYPH_H 14
/* C4's MENU_PASS_BYTES: the menu's CPU-fed pixel payload in one main-loop
 * pass. A slot's op is sent in pieces of at most this, one piece a pass, so
 * no pass sends more. 512 is S1's starting point, before the measurement
 * (C10) sets it. */
#define MB_PASS_BYTES 512u

bool mb_clear_band(uint8_t band);                                  /* false: its transfer was given up */
bool mb_fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t rgb565);   /* synchronous, inclusive; false: given up, a loss latched */
bool mb_glyph(char c, uint16_t x, uint16_t y);                     /* false: the bus is busy, try again */
#define MB_BIG_W 10                       /* FONT_STATUS's cell (P1b's editors' values) */
#define MB_BIG_H 23
bool mb_glyph_big(char c, uint16_t x, uint16_t y);                 /* as mb_glyph, in FONT_STATUS */
/* P1b's generated rows: fn(ctx, row, px, w) fills a row's w pixels in panel
 * order (MB_PANEL(c): RGB565's bytes swapped), w x h at (x, y), at most
 * MB_PASS_BYTES; synchronous through the shared scratch, as mb_mono. False:
 * given up, a loss latched. */
#define MB_PANEL(c) ((uint16_t)(((uint16_t)(c) >> 8) | ((uint16_t)(c) << 8)))
typedef void (*mb_row_fn)(const void *ctx, uint16_t row, uint16_t *px, uint16_t w);
bool mb_rows(uint16_t x, uint16_t y, uint16_t w, uint16_t h, mb_row_fn fn, const void *ctx);
/* A 1-bit bitmap in two colours (rows of `stride` bytes, column 0 in bit 7),
 * w x h at (x, y): synchronous, every pixel sent; false: given up, a loss
 * latched. */
bool mb_mono(const uint8_t *bits, uint8_t stride, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t fg, uint16_t bg);
bool mb_busy(void);                                                /* a DMA in flight */
/* C10's clock: the system tick (MB_TICK_HZ, 5.33 us), extended to 32 bits.
 * Differences are taken modulo 2^32. */
#define MB_TICK_HZ 187500u
uint32_t mb_now_ticks(void);
uint32_t mb_dma_end_ticks(void);   /* when the last DMA ended, on the same clock */
bool mb_lost_pending(void);                                        /* a loss latched, not yet taken */
