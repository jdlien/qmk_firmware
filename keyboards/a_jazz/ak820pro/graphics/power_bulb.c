// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
#include "power_bulb.h"

/* The power mode's bulb (plans/POWER-MODE-ICONS-PLAN.md), in the same slot on
 * battery, where the bolt and the cross never show; on USB every mode is
 * Always on, and the slot is theirs. One bulb losing its light: lit with
 * rays (Always on), lit (Balanced), dimmed (Power saver), unlit and struck
 * through (Lights out). The slash is the usual mark for "switched off" (a
 * muted bell); an X would echo the no-pack cross in this slot. Lights out is
 * the mode whose screen is dark but for a few seconds after a message, so its
 * bulb must read at a glance, and the slash parts it from Saver by shape,
 * not by fill alone. The bulb is cut back a pixel wherever it touches the
 * slash, at an edge or a corner, so the slash lies over it. '#' is the
 * outline or the lit glass, '*' the dimmed glass, '=' the collar, '+' the
 * screw base and '/' the slash. Row 0 is the slot's top, so the rays reach
 * it and the bulb sits on the battery's bottom row. (Moved from display.c,
 * 10-27, for the menu's Power page.) */
static const char bulb_art[BULB_MODES][BULB_H][BULB_W + 1] = {
    {   /* Always on */
        "......#......", ".#.........#.", "..#.......#..", ".....###.....",
        "...#######...", "##.#######.##", "..#########..", "..#########..",
        "..#########..", "...#######...", "....#####....", "....=====....",
        "....+++++....", ".....+++.....",
    },
    {   /* Balanced */
        ".............", ".............", ".............", ".....###.....",
        "...#######...", "...#######...", "..#########..", "..#########..",
        "..#########..", "...#######...", "....#####....", "....=====....",
        "....+++++....", ".....+++.....",
    },
    {   /* Power saver */
        ".............", ".............", ".............", ".....###.....",
        "...##***##...", "...#*****#...", "..#*******#..", "..#*******#..",
        "..#*******#..", "...#*****#...", "....#***#....", "....=====....",
        "....+++++....", ".....+++.....",
    },
    {   /* Lights out */
        ".............", ".............", ".//..........", "..//..##.....",
        "...//...##...", "....//...#...", "..#..//...#..", "..#...//..#..",
        "..#....//....", "...#....//...", "....#....//..", "....====..//.",
        "....+++++....", ".....+++.....",
    },
};
/* Mid grey (0x8410) was near-illegible as 5x7 text here (COL_AMPM); Saver's
 * glass is a filled area and survives it. The first base on the panel (3 wide,
 * 0x8C71, and 0x4228 under Saver and Lights out) barely showed, so it is 5
 * wide and lighter: still darker than the white glass, lighter than Saver's
 * grey glass. Its top row is a darker collar, which parts it from the glass
 * the way a real bulb's does, without the black gap that squashed the glass.
 * The glass changes with the mode, the metal does not, except in Lights out,
 * which is slate all over so the whole bulb looks switched off; on the panel
 * the slate reads nearly grey. Its slash is light grey, clearly not white yet
 * well apart from the slate (chosen on the panel from four shades). Never yellow
 * (the bolt's, in the bolt's place) or red and amber (a low battery, on this
 * row). */
#define COL_BULB_GLASS      0xFFFF   /* white: display.c's COL_FG, which it asserts */
#define COL_BULB_BASE       0xB5B6   /* #B4B4B4: the screw base */
#define COL_BULB_COLLAR     0x632C   /* #646464: the collar, the base's top row */
#define COL_BULB_DIM        0x8410   /* #808080: Saver's glass */
#define COL_BULB_OFF        0x8D18   /* #8EA2C6: Lights out's outline, slate */
#define COL_BULB_OFF_BASE   0x6BD2   /* #6B7994: Lights out's base, a darker slate */
#define COL_BULB_OFF_COLLAR 0x4AAD   /* #4C566A: Lights out's collar */
#define COL_BULB_OFF_SLASH  0xDEFB   /* #DCDCDC: Lights out's slash */
/* Per mode: '#', '*', '+', '=', '/'. */
static const uint16_t bulb_cols[BULB_MODES][5] = {
    {COL_BULB_GLASS, 0, COL_BULB_BASE, COL_BULB_COLLAR, 0},
    {COL_BULB_GLASS, 0, COL_BULB_BASE, COL_BULB_COLLAR, 0},
    {COL_BULB_GLASS, COL_BULB_DIM, COL_BULB_BASE, COL_BULB_COLLAR, 0},
    {COL_BULB_OFF, 0, COL_BULB_OFF_BASE, COL_BULB_OFF_COLLAR, COL_BULB_OFF_SLASH},
};

void power_bulb_row(uint8_t mode, uint8_t y, uint16_t *px, uint8_t w, uint16_t bg, bool swap) {
    uint16_t pal[6] = {bg, bg, bg, bg, bg, bg};   /* '.', then '#', '*', '+', '=', '/' */
    if (mode < BULB_MODES && y < BULB_H)
        for (uint8_t i = 0; i < 5u; i++) pal[i + 1u] = bulb_cols[mode][i];
    if (swap)
        for (uint8_t i = 0; i < 6u; i++) pal[i] = (uint16_t)((pal[i] >> 8) | (pal[i] << 8));
    const char *art = mode < BULB_MODES && y < BULB_H ? bulb_art[mode][y] : ".............";
    for (uint8_t i = 0; i < w; i++) {
        char c = i < BULB_W ? art[i] : '.';
        px[i]  = pal[c == '.' ? 0 : c == '#' ? 1 : c == '*' ? 2 : c == '+' ? 3 : c == '=' ? 4 : 5];
    }
}

uint16_t power_bulb_px(uint8_t mode, uint8_t x, uint8_t y, uint16_t bg) {
    if (mode >= BULB_MODES || x >= BULB_W || y >= BULB_H) return bg;
    char c = bulb_art[mode][y][x];
    if (c == '.') return bg;
    return bulb_cols[mode][c == '#' ? 0 : c == '*' ? 1 : c == '+' ? 2 : c == '=' ? 3 : 4];
}
