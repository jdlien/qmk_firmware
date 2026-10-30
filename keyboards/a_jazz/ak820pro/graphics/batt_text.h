// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The dashboard's battery text for a known level (draw_battery()), pure so
// the host tests run it (plans/CONFIG-MENU-P1-DESIGN.md, P4).
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 8 cells: 7 fit the text slot between the icon slot and the bezel, and an
 * 8th only with its space closed up (draw_battery()): "99% 160h". */
#define BATT_TEXT_MAX 8

/* The readout, kb_eeconfig's KB_BATT_VIEW_* (menu_glue.c checks they agree). */
#define BATT_VIEW_PERCENT 1u
#define BATT_VIEW_HOURS   2u
#define BATT_VIEW_BOTH    3u   /* JD, 10-26: the level and the hours together */
#define BATT_VIEW_ICON    4u   /* JD, 10-27: Icon only, no text */

/* The level `pct` (0-100) as `out` shows it, at most BATT_TEXT_MAX characters
 * and a NUL. The estimate is ready on battery with a mature average
 * (`avg_ready`) and a time left (`left_min`, minutes of use; 0xFFFF none).
 * Percent (`view` BATT_VIEW_PERCENT, or anything unknown): "85%", and under
 * 10 h "18% 6h" or "99% <1h". Hours: "160h", under 1 h "<1h", at most
 * "999h". Both: "85% 160h", "100% 51h", "18% <1h", but "100%" alone where
 * the two take 9 cells ("100% 160h"). Hours and Both show the percent until
 * the estimate is ready. Hours round down. Icon only: "", the icon's fill
 * the only level shown (JD, 10-27: "some people don't like the stress of
 * seeing the percent drop"). */
void batt_level_text(char *out, uint8_t pct, bool on_battery, uint16_t left_min, bool avg_ready, uint8_t view);
