// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
/* C9 of plans/CONFIG-MENU-BUILD-PLAN.md. F1 is C8's list geometry: the title
 * slot, the separator, eight rows of two half-row fills, and the scrollbar.
 * F2 is a 4 x 4 grid. The colours are fixed here, and the host's expected
 * frames are computed from these same tables. */
#include "menu_fixture.h"
#include "menu_backend.h"
#include "menu_slots.h"
#include "menu_text.h"

/* Neighbouring slots differ, A and B differ in every slot, and nothing is
 * black, so a slot left unpainted shows. */
const uint16_t fx_row_colors[2][FX_ROWS][2] = {
    {   /* A */
        {0xF800, 0x07E0}, {0x001F, 0xFFE0}, {0xF81F, 0x07FF}, {0xFD20, 0x8010},
        {0x4208, 0xC618}, {0x8400, 0x0410}, {0xFBE0, 0x041F}, {0xB5B6, 0x2945},
    },
    {   /* B */
        {0x07FF, 0xF81F}, {0xFFE0, 0x001F}, {0x07E0, 0xF800}, {0x8010, 0xFD20},
        {0xC618, 0x4208}, {0x0410, 0x8400}, {0x041F, 0xFBE0}, {0x2945, 0xB5B6},
    },
};
const uint16_t fx_sep_colors[2]   = {0x7BEF, 0x39E7};
const uint16_t fx_track_colors[2] = {0x4208, 0x8410};
const uint16_t fx_thumb_colors[2] = {0xAD55, 0xFFFF};
const uint16_t fx_tile_colors[16] = {
    0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F, 0xFD20, 0x8010,
    0x4208, 0xC618, 0x8400, 0x0410, 0xFBE0, 0x041F, 0xB5B6, 0x2945,
};

#define TITLE_LEN  9
#define ROW_Y0     15
#define ROW_H      14
#define ROW_X0     3
#define ROW_XMID   63
#define ROW_X1     124
#define SEP_Y      14
#define BAR_X      125
#define BAR_Y0     15
#define BAR_Y1     126

static uint8_t screen  = FX_SCREEN_F1;
static uint8_t pattern = 0;
static uint8_t row     = 0;

const char *fx_title(uint8_t p) { return p ? "FIXTURE B" : "FIXTURE A"; }

/* A fill sent in pieces of whole rows, each at most MB_PASS_BYTES (C4). */
static uint16_t rows_per_piece(uint16_t x0, uint16_t x1) {
    uint16_t r = (uint16_t)(MB_PASS_BYTES / ((uint32_t)(x1 - x0 + 1) * 2u));
    return r ? r : 1;
}
static uint8_t fill_pieces(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint16_t r = rows_per_piece(x0, x1), h = (uint16_t)(y1 - y0 + 1);
    return (uint8_t)((h + r - 1) / r);
}
static ms_paint_t fill_piece(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t col, uint8_t p) {
    uint16_t r  = rows_per_piece(x0, x1);
    uint16_t ya = (uint16_t)(y0 + p * r);
    uint16_t yb = (uint16_t)(ya + r - 1);
    if (yb > y1) yb = y1;
    return mb_fill(x0, ya, x1, yb, col) ? MS_PAINT_DONE : MS_PAINT_FAILED;
}

/* A slot's signature: the layout, the slot, the pattern and the selection.
 * Distinct for every target a slot can have. */
static uint16_t f1_sig(uint8_t s) {
    uint16_t v = (uint16_t)(0x1000u | ((uint16_t)s << 4) | ((uint16_t)pattern << 1));
    if (s >= 2 && s < 2 + FX_ROWS && (uint8_t)(s - 2) == row) v |= 1u;
    return v;
}
static uint8_t f1_chunks(uint8_t s) {
    if (s == 0) return TITLE_LEN;
    if (s >= 2 && s < 2 + FX_ROWS) return 2;
    return 1;
}
/* The half rows are the only fills over the budget: 61 x 14 px is 1,708
 * bytes, four pieces (488 B at most). The title's glyphs go by DMA; the
 * separator (240 B) and the scrollbar (track 224 B and thumb 52 B, 276 B
 * together) are one piece each, which the budget must hold. */
_Static_assert(MB_PASS_BYTES >= 276u, "the scrollbar and the separator are one piece each");
static uint8_t f1_pieces(uint8_t s, uint8_t c) {
    if (s >= 2 && s < 2 + FX_ROWS) {
        uint16_t y0 = (uint16_t)(ROW_Y0 + (s - 2) * ROW_H);
        return c == 0 ? fill_pieces(ROW_X0, y0, ROW_XMID, (uint16_t)(y0 + ROW_H - 1))
                      : fill_pieces(ROW_XMID + 1, y0, ROW_X1, (uint16_t)(y0 + ROW_H - 1));
    }
    return 1;
}
static ms_paint_t f1_paint(uint8_t s, uint8_t c, uint8_t p) {
    if (s == 0) return mb_glyph(fx_title(pattern)[c], (uint16_t)(4 + c * MB_GLYPH_W), 0) ? MS_PAINT_DONE : MS_PAINT_BUSY;
    if (s == 1) return mb_fill(4, SEP_Y, 123, SEP_Y, fx_sep_colors[pattern]) ? MS_PAINT_DONE : MS_PAINT_FAILED;
    if (s < 2 + FX_ROWS) {
        uint8_t  r   = (uint8_t)(s - 2);
        bool     sel = r == row;
        uint16_t y0  = (uint16_t)(ROW_Y0 + r * ROW_H);
        uint16_t col = fx_row_colors[pattern][r][sel ? 1 - c : c];   /* the cursor's row swapped */
        if (c == 0) return fill_piece(ROW_X0, y0, ROW_XMID, (uint16_t)(y0 + ROW_H - 1), col, p);
        return fill_piece(ROW_XMID + 1, y0, ROW_X1, (uint16_t)(y0 + ROW_H - 1), col, p);
    }
    /* The scrollbar (C8: x 125, y 15-126): its track, then a thumb at the top,
     * one operation. A failed track stops it: no thumb sent after a give-up. */
    if (!mb_fill(BAR_X, BAR_Y0, BAR_X, BAR_Y1, fx_track_colors[pattern])) return MS_PAINT_FAILED;
    return mb_fill(BAR_X, BAR_Y0, BAR_X, BAR_Y0 + FX_THUMB_H - 1, fx_thumb_colors[pattern]) ? MS_PAINT_DONE : MS_PAINT_FAILED;
}
static const ms_layout_t f1 = {(uint8_t)(2 + FX_ROWS + 1), f1_sig, f1_chunks, f1_pieces, f1_paint, 0, 0};

static uint16_t f2_sig(uint8_t s) { return (uint16_t)(0x2000u | s); }
static uint8_t  f2_chunks(uint8_t s) { (void)s; return 1; }
static uint8_t  f2_pieces(uint8_t s, uint8_t c) { (void)s; (void)c; return fill_pieces(0, 0, 31, 31); }   /* 2,048 B: four */
static ms_paint_t f2_paint(uint8_t s, uint8_t c, uint8_t p) {
    (void)c;
    uint16_t x = (uint16_t)((s % 4u) * 32u), y = (uint16_t)((s / 4u) * 32u);
    return fill_piece(x, y, (uint16_t)(x + 31), (uint16_t)(y + 31), fx_tile_colors[s], p);
}
static const ms_layout_t f2 = {16, f2_sig, f2_chunks, f2_pieces, f2_paint, 0, 0};

/* F3: the slots of F1, in text. A text slot is x 3-124: the inverted
 * cursor's pad pixel at x 3, 20 cells from x 4, and x 124, which is ink
 * (black) on the inverted row and background elsewhere. Half rows are two
 * chunks, x 3-63 and x 64-124 (61 px: four pieces of 4, 4, 4 and 2 pixel
 * rows, 488 B at most); full rows one (122 px: seven pieces of 2 rows, 488
 * B). The title is a text slot too, white on black, chunked the same way. */
const char *const fx_mono_rows[FX_ROWS] = {
    MT_LSAQUO " Back",
    "Lighting           >",
    "Screen           75%",
    "Connection       BT1",
    "Power       Balanced",
    "Glyphs " MT_LSAQUO MT_RSAQUO MT_BULLET MT_TRIANGLE MT_CHECK " gjpqy",
    "Layout F2          >",
    "Diagnostics        >",
};
const char *const fx_mono_titles[FX_MONO_STYLES] = {
    "Lit text, half rows", "Lit text, full rows", "Inverted, half rows", "Inverted, full rows",
};
static uint8_t mono = 0;

#define TEXT_W   (1 + 20 * MT_CELL_W + 1)   /* 122: x 3-124 */
#define HALF_W   61                         /* x 3-63, then x 64-124 */
#define BITS_MAX 64                         /* a piece's bits: 4 rows of 8 B, or 2 of 16 */
_Static_assert(TEXT_W == ROW_X1 - ROW_X0 + 1 && HALF_W == ROW_XMID - ROW_X0 + 1, "C8's row slot");

static bool f3_text_slot(uint8_t s) { return s == 0 || (s >= 2 && s < 2 + FX_ROWS); }
static uint16_t f3_sig(uint8_t s) {
    if (!f3_text_slot(s)) return (uint16_t)(0x3000u | ((uint16_t)s << 4));   /* the separator, the scrollbar */
    uint16_t v = (uint16_t)(0x3000u | ((uint16_t)s << 4) | ((uint16_t)mono << 1));
    if (s >= 2 && (uint8_t)(s - 2) == row) v |= 1u;
    return v;
}
static uint8_t f3_chunks(uint8_t s) { return f3_text_slot(s) && !(mono & FX_MONO_FULL) ? 2 : 1; }
static uint8_t f3_rows_per_piece(void) { return (uint8_t)(MB_PASS_BYTES / ((mono & FX_MONO_FULL ? TEXT_W : HALF_W) * 2u)); }
static uint8_t f3_pieces(uint8_t s, uint8_t c) {
    (void)c;
    if (!f3_text_slot(s)) return 1;
    uint8_t r = f3_rows_per_piece();
    return (uint8_t)((MT_CELL_H + r - 1) / r);
}
_Static_assert(MB_PASS_BYTES / (TEXT_W * 2u) >= 1 && MB_PASS_BYTES / (TEXT_W * 2u) <= MT_CELL_H &&
                   (MB_PASS_BYTES / (HALF_W * 2u)) * ((HALF_W + 7u) / 8u) <= BITS_MAX &&
                   (MB_PASS_BYTES / (TEXT_W * 2u)) * ((TEXT_W + 7u) / 8u) <= BITS_MAX,
               "a piece is one to 14 pixel rows, and its bits fit");
static ms_paint_t f3_paint(uint8_t s, uint8_t c, uint8_t p) {
    if (s == 1) return mb_fill(4, SEP_Y, 123, SEP_Y, FX_MONO_GREY) ? MS_PAINT_DONE : MS_PAINT_FAILED;
    if (s == 2 + FX_ROWS) {
        if (!mb_fill(BAR_X, BAR_Y0, BAR_X, BAR_Y1, FX_MONO_TRACK)) return MS_PAINT_FAILED;
        return mb_fill(BAR_X, BAR_Y0, BAR_X, BAR_Y0 + FX_THUMB_H - 1, FX_MONO_WHITE) ? MS_PAINT_DONE : MS_PAINT_FAILED;
    }
    const bool inv = (mono & FX_MONO_INV) != 0;
    const bool sel = s >= 2 && (uint8_t)(s - 2) == row;
    uint16_t   fg = FX_MONO_WHITE, bg = FX_MONO_BLACK, y0 = 0;
    mt_line_t  l  = {fx_mono_titles[mono], 20, 1, 1, false, false, 0};
    if (s >= 2) {
        l.s  = fx_mono_rows[s - 2];
        y0   = (uint16_t)(ROW_Y0 + (s - 2) * ROW_H);
        if (inv && sel) {
            fg          = FX_MONO_BLACK;
            bg          = FX_MONO_WHITE;
            l.trail_ink = true;   /* x 124 stays black */
        } else if (!inv) {
            fg = sel ? FX_MONO_LIT : FX_MONO_DIM;
        }
    }
    const uint8_t  rpp  = f3_rows_per_piece();
    const uint8_t  r0   = (uint8_t)(p * rpp);
    const uint8_t  rows = (uint8_t)(r0 + rpp > MT_CELL_H ? MT_CELL_H - r0 : rpp);
    const uint16_t c0   = (mono & FX_MONO_FULL) ? 0 : (uint16_t)(c * HALF_W);
    const uint16_t w    = (mono & FX_MONO_FULL) ? TEXT_W : HALF_W;
    const uint8_t  st   = (uint8_t)((w + 7u) / 8u);
    uint8_t        bits[BITS_MAX];
    mt_bits(&l, c0, w, r0, rows, bits, st);
    return mb_mono(bits, st, (uint16_t)(ROW_X0 + c0), (uint16_t)(y0 + r0), w, rows, fg, bg) ? MS_PAINT_DONE : MS_PAINT_FAILED;
}
static const ms_layout_t f3 = {(uint8_t)(2 + FX_ROWS + 1), f3_sig, f3_chunks, f3_pieces, f3_paint, 0, 0};

void fx_open(void) {
    screen  = FX_SCREEN_F1;
    pattern = 0;
    row     = 0;
    ms_set_layout(&f1, true);
}

void fx_return_from_diag(void) {
    screen = FX_SCREEN_F1;
    row    = FX_ROW_DIAG;
    ms_set_layout(&f1, true);
}

void fx_invalidate(void) { ms_invalidate_all(); }

void fx_other_layout(void) {
    if (screen != FX_SCREEN_F2) {   /* F1, or F3's "Layout F2" row */
        screen = FX_SCREEN_F2;
        ms_set_layout(&f2, false);
    } else {
        screen = FX_SCREEN_F1;
        row    = FX_ROW_F2;
        ms_set_layout(&f1, false);
    }
}

bool fx_set_pattern(uint8_t p) {
    if (screen != FX_SCREEN_F1) return false;
    pattern = p ? 1 : 0;
    ms_retarget();
    return true;
}

/* F3 in a style: from another layout a layout change (its clear); within F3,
 * a retarget, as a style's repaint over the same slots. */
void fx_set_mono(uint8_t style) {
    mono = (uint8_t)(style % FX_MONO_STYLES);
    if (screen == FX_SCREEN_F3) {
        ms_retarget();
        return;
    }
    screen = FX_SCREEN_F3;
    ms_set_layout(&f3, false);
}

fx_result_t fx_input(int8_t turns, mi_latch_t latch) {
    if (latch == MI_LATCH_CLOSE || latch == MI_LATCH_BACK) return FX_CLOSE;
    if (screen != FX_SCREEN_F2 && turns) {
        int16_t r = (int16_t)((row + turns) % FX_ROWS);
        if (r < 0) r += FX_ROWS;
        row = (uint8_t)r;
        ms_retarget();
    }
    if (latch != MI_LATCH_CLICK) return FX_NONE;
    if (screen == FX_SCREEN_F2) {
        fx_other_layout();
        return FX_NONE;
    }
    if (row == FX_ROW_DIAG) return FX_DIAG;
    if (row == FX_ROW_F2) {
        fx_other_layout();
        return FX_NONE;
    }
    if (screen == FX_SCREEN_F3) fx_set_mono((uint8_t)(mono + 1u));   /* the next style */
    else fx_set_pattern((uint8_t)(1u - pattern));
    return FX_NONE;
}

uint8_t fx_screen(void) { return screen; }
uint8_t fx_cursor(void) { return row; }
uint8_t fx_pattern(void) { return screen == FX_SCREEN_F3 ? (uint8_t)(FX_MONO_FIRST + mono) : pattern; }
