// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// How much of a host text line the dashboard's text slot shows in each face
// (plans/MENU-8X16-PLAN.md, B6), pure so the host test runs it
// (scripts/text_fit_test.c).
#pragma once

#include <stdint.h>

/* The faces, kb_eeconfig's KB_FONT_* (display.c checks they agree): Large,
 * Terminus Bold cropped to 8x14, the default; Small, Cozette 6x14. Anything
 * but Small is Large, as kb_eeconfig reads an unset or garbage byte. */
#define TEXT_FACE_LARGE   0u
#define TEXT_FACE_SMALL   1u
#define TEXT_FACE_REGULAR 2u   /* 10-28: Terminus regular, Large's cell and budgets */

/* Each line's cells. Line 0 (the artist, or the title alone) starts beside
 * the transport icon, line 1 (the title) at the bezel's margin; display.c
 * derives each from its x and the face's advance. Small's are the host's
 * budget and the buffers' (DISPLAY_TEXT_MAX_L0/_L1), so in Small nothing the
 * slot holds is cut. */
#define TEXT_FIT_SMALL_L0 19   /* (128 - 14) / 6 */
#define TEXT_FIT_SMALL_L1 21   /* (128 - 2) / 6 */
#define TEXT_FIT_LARGE_L0 14   /* (128 - 14) / 8 */
#define TEXT_FIT_LARGE_L1 15   /* (128 - 2) / 8 */

/* Line `line`'s budget in `face`; 0 for a line other than 0 or 1. */
uint8_t text_fit_budget(uint8_t face, uint8_t line);

/* How many of `s`'s characters line `line` shows in `face`: its length, cut
 * to the budget. The slot keeps the text whole and cuts it here, at the draw,
 * so a change of face shows more or less of the same text without the host
 * sending it again. Plainly cut, no ellipsis, as the host cuts. 0 for NULL. */
uint8_t text_fit_len(uint8_t face, uint8_t line, const char *s);
