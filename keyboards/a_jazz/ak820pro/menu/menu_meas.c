// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
/* C10 of plans/CONFIG-MENU-BUILD-PLAN.md, the board's side. Everything here
 * runs on the main thread. The tick is loop_acct.h's: the 16-bit system
 * counter at CH_CFG_ST_FREQUENCY, read inline with no lock. */
#include "menu_meas.h"

#include "ch.h"
#include "hal.h"
#include "menu_backend.h"

#define OVER15_TICKS  ((15000u * MB_TICK_HZ + 999999u) / 1000000u)   /* the first count over 15,000 us */

static uint32_t pass_last, pass_max;
static bool     pass_have;
static uint16_t pass_over15, passes;
static uint32_t band_max;
static uint16_t token;                     /* 0 at boot: .bss */
static uint16_t token_next;

uint32_t meas_ticks(void) {
    static uint16_t last;
    static uint32_t high;
    uint16_t        now = (uint16_t)st_lld_get_counter();
    if (now < last) high += 0x10000u;
    last = now;
    return high + now;
}

void meas_pass(void) {
    uint32_t t = meas_ticks();
    if (pass_have) {
        uint32_t dt = t - pass_last;
        if (dt > pass_max) pass_max = dt;
        if (dt >= OVER15_TICKS && pass_over15 < 0xFFFFu) pass_over15++;
        passes++;
    }
    pass_last = t;
    pass_have = true;
}

void meas_band(uint32_t ticks) {
    if (ticks > band_max) band_max = ticks;
}

uint32_t meas_pass_max_ticks(void) { return pass_max; }
uint16_t meas_pass_over15(void)    { return pass_over15; }
uint16_t meas_passes(void)         { return passes; }
uint32_t meas_band_max_ticks(void) { return band_max; }
uint16_t meas_token(void)          { return token; }

uint16_t meas_arm(void) {
    pass_max    = 0;
    pass_over15 = 0;
    passes      = 0;
    band_max    = 0;
    if (++token_next == 0) token_next = 1;
    token = token_next;
    return token;
}
