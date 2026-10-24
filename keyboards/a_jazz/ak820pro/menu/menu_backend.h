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
bool mb_busy(void);                                                /* a DMA in flight */
bool mb_lost_pending(void);                                        /* a loss latched, not yet taken */
