// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Text in the menu's 1-bit small face (plans/CONFIG-MENU-BUILD-PLAN.md, S1):
// a line of glyph cells composed into bits for the mono blit, a band of pixel
// rows at a time, so a slot's op can go out in pieces. Pure: the host tests
// run it.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MT_CELL_W 6    /* FONT_MONO_W */
#define MT_CELL_H 14   /* FONT_MONO_H */

/* The face's five extras, as string pieces (the generator's order;
 * menu_text.c checks the count). Concatenate them: "\x80" "Back", since a
 * hex escape runs on through any hex digit after it. */
#define MT_LSAQUO   "\x80"   /* ‹ */
#define MT_RSAQUO   "\x81"   /* › */
#define MT_BULLET   "\x82"   /* • */
#define MT_TRIANGLE "\x83"   /* ▸ */
#define MT_CHECK    "\x84"   /* ✓ */

/* A line as a slot paints it: `lead` pixels, then `cells` glyph cells (the
 * string, padded with spaces, codes the face hasn't shown as '?'), then
 * `trail` pixels. Each edge is ink or background, so the inverted cursor's
 * pad pixel (background) and its black right edge (ink) come out of the
 * line's two colours. */
typedef struct {
    const char *s;
    uint8_t     cells;
    uint8_t     lead, trail;
    bool        lead_ink, trail_ink;
} mt_line_t;

uint16_t mt_width(const mt_line_t *l);   /* in pixels */

/* Rows [r0, r0 + rows) of the line's columns [c0, c0 + w), into `bits`: rows
 * of `stride` bytes, column c0 in bit 7 of each row's first byte. The caller
 * keeps r0 + rows <= MT_CELL_H and w <= stride * 8. */
void mt_bits(const mt_line_t *l, uint16_t c0, uint16_t w, uint8_t r0, uint8_t rows, uint8_t *bits, uint8_t stride);
