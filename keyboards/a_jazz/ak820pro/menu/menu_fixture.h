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

typedef enum { FX_NONE = 0, FX_DIAG, FX_CLOSE } fx_result_t;

void        fx_open(void);                 /* F1, pattern A, row 0, with the layout's clear */
void        fx_return_from_diag(void);     /* F1, row 7, with the clear */
void        fx_invalidate(void);           /* a loss */
fx_result_t fx_input(int8_t turns, mi_latch_t latch);
bool        fx_set_pattern(uint8_t p);     /* op 27 sub 0: 0 A, 1 B; false if not F1 */
void        fx_other_layout(void);         /* op 27 sub 0, pattern 3 */
uint8_t     fx_screen(void);
uint8_t     fx_cursor(void);
uint8_t     fx_pattern(void);

/* What the fixture's slots are, for the host's expected frames. */
const char *fx_title(uint8_t pattern);
