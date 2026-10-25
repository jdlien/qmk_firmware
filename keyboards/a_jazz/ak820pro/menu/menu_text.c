// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
#include "menu_text.h"

#include <string.h>

#include "../graphics/res/font_small_mono.h"

_Static_assert(MT_CELL_W == FONT_MONO_W && MT_CELL_H == FONT_MONO_H, "menu_text.h's cell is the face's");
_Static_assert(FONT_MONO_XFIRST == 0x80 && FONT_MONO_N - FONT_MONO_ASCII == 23, "menu_text.h's MT_LSAQUO..MT_I_EXIT");

static const uint8_t *glyph(uint8_t code) {
    if (code >= FONT_MONO_FIRST && code < FONT_MONO_FIRST + FONT_MONO_ASCII) return font_small_mono[code - FONT_MONO_FIRST];
    if (code >= FONT_MONO_XFIRST && code < FONT_MONO_XFIRST + (FONT_MONO_N - FONT_MONO_ASCII))
        return font_small_mono[FONT_MONO_ASCII + (code - FONT_MONO_XFIRST)];
    return font_small_mono['?' - FONT_MONO_FIRST];
}

uint16_t mt_width(const mt_line_t *l) { return (uint16_t)(l->lead + l->cells * MT_CELL_W + l->trail); }

/* Set columns [a, b) of the band (band-relative, already clipped) in every row. */
static void ink_cols(uint8_t *bits, uint8_t stride, uint8_t rows, uint16_t a, uint16_t b) {
    for (uint16_t i = a; i < b; i++)
        for (uint8_t r = 0; r < rows; r++) bits[(uint16_t)r * stride + (i >> 3)] |= (uint8_t)(0x80u >> (i & 7u));
}

/* A cell at a time, not a column at a time: each glyph row's six bits are
 * shifted into place with two byte ORs. Per column, the M0 under the row
 * ISR's load spent ~4 ms of a 488 B piece composing and expanding (op 27
 * sub 10, 10-24), against ~4.3 ms for a whole fill piece. */
void mt_bits(const mt_line_t *l, uint16_t c0, uint16_t w, uint8_t r0, uint8_t rows, uint8_t *bits, uint8_t stride) {
    memset(bits, 0, (size_t)stride * rows);
    const uint16_t end  = (uint16_t)(c0 + w);                    /* line columns [c0, end) */
    const uint16_t text = (uint16_t)(l->lead + l->cells * MT_CELL_W);
    /* The edges, ink only (background is the zeroed bits). A lead glyph's
     * columns land where they are, cut to the band: lead <= a cell. */
    if (l->lead_glyph && c0 < l->lead) {
        const uint8_t *g   = glyph(l->lead_glyph) + r0;
        const uint16_t lim = end < l->lead ? end : l->lead;
        for (uint16_t i = c0; i < lim; i++)
            for (uint8_t r = 0; r < rows; r++)
                if (g[r] & (0x80u >> i)) bits[(uint16_t)r * stride + ((i - c0) >> 3)] |= (uint8_t)(0x80u >> ((i - c0) & 7u));
    } else if (l->lead_ink && c0 < l->lead) ink_cols(bits, stride, rows, 0, (uint16_t)((end < l->lead ? end : l->lead) - c0));
    if (l->trail_ink && end > text) ink_cols(bits, stride, rows, (uint16_t)((c0 > text ? c0 : text) - c0), w);
    /* The cells the band reaches, each clipped to it. */
    if (end <= l->lead || c0 >= text) return;
    const size_t len   = strlen(l->s);
    uint8_t      first = c0 > l->lead ? (uint8_t)((c0 - l->lead) / MT_CELL_W) : 0;
    uint16_t     past  = (uint16_t)((end - l->lead + MT_CELL_W - 1) / MT_CELL_W);   /* one past: clamped before it narrows (codex) */
    uint8_t      last  = past > l->cells ? l->cells : (uint8_t)past;
    int16_t x = (int16_t)(l->lead + first * MT_CELL_W - c0);   /* the cell's column 0, band-relative; < 0 if cut */
    for (uint8_t cell = first; cell < last; cell++, x = (int16_t)(x + MT_CELL_W)) {
        if (cell >= len) break;   /* spaces: background */
        const uint8_t *g = glyph((uint8_t)l->s[cell]) + r0;
        /* Clipped to the band. A cell cut on the left: shifted left by the
         * cut, so its first visible column sits at band column 0 and the cut
         * ones fall off the byte. On the right: no bit at or past w. The cell
         * starts inside the band (x < w), so `room` is at least 1. */
        const uint8_t  lshift = x < 0 ? (uint8_t)-x : 0;
        const uint16_t base   = x < 0 ? 0 : (uint16_t)x;
        const uint16_t room   = (uint16_t)(w - base);
        const uint8_t  rmask  = room >= 8u ? 0xFFu : (uint8_t)(0xFFu << (8u - room));
        const uint8_t  byte = (uint8_t)(base >> 3), sh = (uint8_t)(base & 7u);
        for (uint8_t r = 0; r < rows; r++) {
            const uint8_t gb = (uint8_t)(((g[r] & 0xFCu) << lshift) & rmask);   /* six columns, bits 7..2 */
            if (!gb) continue;
            const uint16_t v   = (uint16_t)((uint16_t)gb << 8 >> sh);
            uint8_t       *row = bits + (uint16_t)r * stride + byte;
            row[0] |= (uint8_t)(v >> 8);
            if ((v & 0xFFu) && byte + 1u < stride) row[1] |= (uint8_t)v;
        }
    }
}
