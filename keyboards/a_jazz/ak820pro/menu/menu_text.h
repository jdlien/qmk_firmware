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

/* The face's extras, as string pieces (the generator's order: its
 * FONT_MONO_* names; menu_text.c checks the count, the host tests each
 * string). A two-cell icon is two codes. Concatenate them: "\x80" "Back",
 * since a hex escape runs on through any hex digit after it. */
#define MT_LSAQUO       "\x80"       /* ‹ */
#define MT_RSAQUO       "\x81"       /* › */
#define MT_BULLET       "\x82"       /* • */
#define MT_TRIANGLE     "\x83"       /* ▸ */
#define MT_CHECK        "\x84"       /* ✓ */
/* JD's icons, 10-24 (the icon picker, scripts/gen_icon_gallery.py). */
#define MT_TRI_L        "\x85"       /* ◀ Back */
#define MT_TRI_R        "\x86"       /* ▶ a row that opens a page */
#define MT_CURSOR       "\x87"       /* the selected row's, in its lead: columns 0-2 */
#define MT_WRENCH       "\x88\x89"   /* before "Configuration" */
#define MT_CHECK_BOX    "\x8a\x8b"   /* the saved option */
#define MT_I_POWER      "\x8c"       /* the top level's icons, and its sections' titles' */
#define MT_I_CLOCK      "\x8d\x8e"
#define MT_I_DIAG       "\x8f"
#define MT_I_HELP       "\x90\x91"
#define MT_I_ABOUT      "\x92\x93"
#define MT_I_EXIT       "\x94"
#define MT_I_LIGHT      "\x95"       /* P1b */
#define MT_I_SCREEN     "\x96\x97"
#define MT_I_BOOT       "\x98\x99"   /* the bootloader item */
#define MT_I_IND        "\x9a\x9b"   /* P2a: Indicators */
#define MT_I_CONN       "\x9c\x9d"   /* Connection; in blue, a slot advertising (P3) */
#define MT_I_WAIT       "\x9e\x9f"   /* P3: a slot linking, in grey (the hourglass) */
#define MT_I_FAIL       "\xa0\xa1"   /* P3: a slot failed, in red (the circled X) */
#define MT_I_BATT       "\xa2"       /* P4: Battery */

/* A line as a slot paints it: `lead` pixels, then `cells` glyph cells (the
 * string, padded with spaces, codes the face hasn't shown as '?'), then
 * `trail` pixels. Each edge is ink or background, so the inverted cursor's
 * pad pixel (background) and its black right edge (ink) come out of the
 * line's two colours; or, with `lead_glyph`, the lead (at most a cell wide)
 * shows that glyph's first columns: the menu's cursor. */
typedef struct {
    const char *s;
    uint8_t     cells;
    uint8_t     lead, trail;
    bool        lead_ink, trail_ink;
    uint8_t     lead_glyph;   /* 0: none; else a code, over lead_ink */
} mt_line_t;

uint16_t mt_width(const mt_line_t *l);   /* in pixels */

/* Rows [r0, r0 + rows) of the line's columns [c0, c0 + w), into `bits`: rows
 * of `stride` bytes, column c0 in bit 7 of each row's first byte. The caller
 * keeps r0 + rows <= MT_CELL_H and w <= stride * 8. */
void mt_bits(const mt_line_t *l, uint16_t c0, uint16_t w, uint8_t r0, uint8_t rows, uint8_t *bits, uint8_t stride);
