// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The power modes' bulbs (plans/POWER-MODE-ICONS-PLAN.md): the dashboard's
// battery row draws the mode's in the bolt's slot (display.c), and the menu's
// Power page each mode's on its row (10-27, JD: "use the power mode icons
// that we show on the battery row, right aligned on the right edge"). Pure,
// so the menu's host tests draw them too.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BULB_W     13
#define BULB_H     14
#define BULB_MODES 4   /* POWER_MODE_COUNT, in its order: display.c asserts it */

/* The bulb of mode's pixel (x, y), x < BULB_W, y < BULB_H: its RGB565, or bg
 * where the art is empty (or for a mode past the four). */
uint16_t power_bulb_px(uint8_t mode, uint8_t x, uint8_t y, uint16_t bg);
/* Row y of mode's bulb into px[0..w): bg past the art (x >= BULB_W) and
 * where it's empty; with `swap`, every pixel's bytes swapped (the panel's
 * order, the menu's MB_PANEL()). The colours looked up once a row, not a
 * call a pixel: the menu's piece (10-27, the board: 9.7 ms against a text
 * piece's 6). */
void power_bulb_row(uint8_t mode, uint8_t y, uint16_t *px, uint8_t w, uint16_t bg, bool swap);
