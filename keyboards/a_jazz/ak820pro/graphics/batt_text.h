// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The dashboard's battery text for a known level (draw_battery()), pure so
// the host tests run it (plans/CONFIG-MENU-P1-DESIGN.md, P4).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BATT_TEXT_MAX 7   /* cells: the text slot between the icon slot and the bezel */

/* The level `pct` (0-100) as `out` shows it, at most BATT_TEXT_MAX characters
 * and a NUL. The estimate is ready on battery with a mature average
 * (`avg_ready`) and a time left (`left_min`, minutes of use; 0xFFFF none).
 * Percent (`hours` false): "85%", and under 10 h "18% 6h" or "99% <1h".
 * Hours: "160h", under 1 h "<1h"; the percent until the estimate is ready.
 * Hours round down. */
void batt_level_text(char *out, uint8_t pct, bool on_battery, uint16_t left_min, bool avg_ready, bool hours);
