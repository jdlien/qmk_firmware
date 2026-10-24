// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
#include "menu_text.h"

#include <string.h>

#include "../graphics/res/font_small_mono.h"

_Static_assert(MT_CELL_W == FONT_MONO_W && MT_CELL_H == FONT_MONO_H, "menu_text.h's cell is the face's");
_Static_assert(FONT_MONO_XFIRST == 0x80 && FONT_MONO_N - FONT_MONO_ASCII == 5, "menu_text.h's MT_LSAQUO..MT_CHECK");

static const uint8_t *glyph(uint8_t code) {
    if (code >= FONT_MONO_FIRST && code < FONT_MONO_FIRST + FONT_MONO_ASCII) return font_small_mono[code - FONT_MONO_FIRST];
    if (code >= FONT_MONO_XFIRST && code < FONT_MONO_XFIRST + (FONT_MONO_N - FONT_MONO_ASCII))
        return font_small_mono[FONT_MONO_ASCII + (code - FONT_MONO_XFIRST)];
    return font_small_mono['?' - FONT_MONO_FIRST];
}

uint16_t mt_width(const mt_line_t *l) { return (uint16_t)(l->lead + l->cells * MT_CELL_W + l->trail); }

/* The cell and glyph column advance a column at a time: one division before
 * the loop, none in it (the M0 has no divider: each is a library call, and
 * two a column were most of a piece's composing, codex 10-24). */
void mt_bits(const mt_line_t *l, uint16_t c0, uint16_t w, uint8_t r0, uint8_t rows, uint8_t *bits, uint8_t stride) {
    memset(bits, 0, (size_t)stride * rows);
    const size_t   len  = strlen(l->s);
    const uint16_t text = (uint16_t)(l->cells * MT_CELL_W);
    uint8_t        cell = 0, gcol = 0;   /* under the first text column this band reaches */
    if (c0 > l->lead) {
        uint16_t t = (uint16_t)(c0 - l->lead);
        cell       = (uint8_t)(t / MT_CELL_W);
        gcol       = (uint8_t)(t % MT_CELL_W);
    }
    const uint8_t *g = NULL;
    for (uint16_t i = 0; i < w; i++) {
        const uint16_t c    = (uint16_t)(c0 + i);
        const uint8_t  mask = (uint8_t)(0x80u >> (i & 7u));
        uint8_t       *col  = bits + (i >> 3);
        if (c < l->lead || c >= l->lead + text) {   /* an edge pixel */
            if (c < l->lead ? l->lead_ink : l->trail_ink)
                for (uint8_t r = 0; r < rows; r++) col[(uint16_t)r * stride] |= mask;
            continue;
        }
        if (!g) g = glyph(cell < len ? (uint8_t)l->s[cell] : (uint8_t)' ');
        const uint8_t gbit = (uint8_t)(0x80u >> gcol);
        for (uint8_t r = 0; r < rows; r++)
            if (g[r0 + r] & gbit) col[(uint16_t)r * stride] |= mask;
        if (++gcol == MT_CELL_W) {
            gcol = 0;
            cell++;
            g = NULL;
        }
    }
}
