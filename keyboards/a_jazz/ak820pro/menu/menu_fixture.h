// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// S0's fixture (plans/CONFIG-MENU-BUILD-PLAN.md, C9): the MENU owner's screen
// until P1's tree replaces it. Two layouts of fixed slots in deterministic
// colours, so ownership, damage and the restore can be seen and photographed.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "menu_input.h"

#define FX_SCREEN_F1 1   /* op 27 sub 5, byte 14 */
#define FX_SCREEN_F2 2
#define FX_SCREEN_F3 3   /* S1's mono text, op 27 sub 0 patterns 16-19 */
#define FX_ROWS      8
#define FX_ROW_F2    6   /* a click here switches to F2 */
#define FX_ROW_DIAG  7   /* Diagnostics */
#define FX_F1_OPS    27  /* a full paint: 9 + 1 + 16 + 1 */

/* The colours, shared with the host's expected frames. */
extern const uint16_t fx_row_colors[2][FX_ROWS][2];   /* [pattern][row][half] */
extern const uint16_t fx_tile_colors[16];
extern const uint16_t fx_sep_colors[2];     /* [pattern] */
extern const uint16_t fx_track_colors[2];
extern const uint16_t fx_thumb_colors[2];
#define FX_THUMB_H     26

/* F3, S1's mono text in C8's list geometry: the cursor style (lit text or
 * inverted, plan "Open" 1) and half or full rows ("Open" 2), op 27 sub 0's
 * patterns FX_MONO_FIRST + style. Style bit 0: full rows; bit 1: inverted. */
#define FX_MONO_FIRST  16
#define FX_MONO_STYLES 4
#define FX_MONO_FULL   1u
#define FX_MONO_INV    2u
extern const char *const fx_mono_rows[FX_ROWS];
extern const char *const fx_mono_titles[FX_MONO_STYLES];
#define FX_MONO_WHITE  0xFFFFu
#define FX_MONO_BLACK  0x0000u
#define FX_MONO_DIM    0xCE59u   /* lit text's rows, ~#C8C8C8 (the mockup's) */
#define FX_MONO_LIT    0xFF94u   /* lit text's selection, ~#FFF2A0 */
#define FX_MONO_GREY   0x632Cu   /* the separator, #646464 */
#define FX_MONO_TRACK  0x4228u   /* the scrollbar's track, #464646 */

typedef enum { FX_NONE = 0, FX_DIAG, FX_CLOSE } fx_result_t;

void        fx_open(void);                 /* F1, pattern A, row 0, with the layout's clear */
void        fx_return_from_diag(void);     /* F1, row 7, with the clear */
void        fx_invalidate(void);           /* a loss */
fx_result_t fx_input(int8_t turns, mi_latch_t latch);
bool        fx_set_pattern(uint8_t p);     /* op 27 sub 0: 0 A, 1 B; false if not F1 */
void        fx_set_mono(uint8_t style);    /* op 27 sub 0, patterns 16-19: F3 in that style */
void        fx_other_layout(void);         /* op 27 sub 0, pattern 3: F2, or from F2 back to F1 */
uint8_t     fx_screen(void);
uint8_t     fx_cursor(void);
uint8_t     fx_pattern(void);              /* F3: FX_MONO_FIRST + its style */

/* What the fixture's slots are, for the host's expected frames. */
const char *fx_title(uint8_t pattern);
