// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// C10's measures on the board (plans/CONFIG-MENU-BUILD-PLAN.md): the clock
// the menu's scheduler times itself with, the main loop's pass times, the
// restore's and the debug page's clear bands, and the measurement session's
// token. Read through op 27 sub 10 (dev builds). Each restore stage's own
// worst time is the display probe's (DPROBE, scripts/devhook.py read).
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The system tick (MB_TICK_HZ, 5.33 us), extended to 32 bits in software.
 * Main thread only, and read at least every 349 ms (meas_pass() does, each
 * pass) or a wrap of the 16-bit counter is missed. */
uint32_t meas_ticks(void);

void     meas_pass(void);                                 /* once a main-loop pass */
void     meas_band(uint32_t ticks);                       /* a restore or debug clear band */

uint32_t meas_pass_max_ticks(void);
uint16_t meas_pass_over15(void);
uint16_t meas_passes(void);
uint32_t meas_band_max_ticks(void);
uint16_t meas_token(void);
uint16_t meas_arm(void);                                  /* zero the interval, a new token */
