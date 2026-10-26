// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
#include "rgb_speed.h"

/* The effects' speeds (JD, 10-25), each geometric in speed: every step of
 * the menu's 1% (2.55 of 255) scales the rate by the same 4.2%. Upstream's
 * runner is linear, (speed / 4 + 1) / 256 of a hue a millisecond, a cycle
 * in 65 s at 0 and 1 s at 255, so most of its range is fast:
 * - Cycle All, the whole board one colour: "even on 1 the color cycles about
 *   10x too fast ... At 100% it's just about a multi-color strobe." A cycle
 *   in 10 min at 0, 10 s at 255.
 * - Cycle L-R and U-D, the rainbow across the board: "I'd like them a little
 *   slower". 2 min at 0, 2 s at 255.
 * - Jellybean Raindrops: "I don't think speed even affects Jellybean at
 *   all". Upstream drops one when (ms * (speed + 16) / 256) % 5 == 0, a fifth
 *   of its frames at any speed (~12 a second at ~60 frames). A drop every
 *   2 s at 0, 30 a second at 255. */
const rgb_speed_curve_t RGB_SPEED_CYCLE_ALL   = {7158u, 1512u};    /* 2^32 / 600,000 ms; log2(60) */
const rgb_speed_curve_t RGB_SPEED_CYCLE_SWEEP = {35791u, 1512u};   /* 2^32 / 120,000 ms */
const rgb_speed_curve_t RGB_SPEED_JELLYBEAN   = {8389u, 1512u};    /* 2^24 / 2,000 ms */

/* 2^(e / 256) in 16.16, e under 15 octaves: 2^(i / 16) at the fraction's top
 * four bits, linear between (within 0.03%). */
static uint32_t exp2_q16(uint16_t e) {
    static const uint32_t t[17] = {65536, 68438,  71468,  74632,  77936,  81386,  84990,  88752, 92682,
                                   96785, 101070, 105545, 110218, 115098, 120194, 125515, 131072};
    uint8_t  f = (uint8_t)e, k = (uint8_t)(f >> 4);
    uint32_t v = t[k] + (((t[k + 1] - t[k]) * (f & 15u)) >> 4);
    return v << (e >> 8);
}

uint32_t rgb_speed_rate(const rgb_speed_curve_t *c, uint8_t speed) {
    uint16_t e = (uint16_t)(((uint32_t)speed * c->octaves * 257u + 32768u) >> 16);   /* speed / 255 of the octaves */
    return (uint32_t)(((uint64_t)c->r0 * exp2_q16(e)) >> 16);
}

uint32_t rgb_speed_phase(uint32_t ms, uint32_t rate) {
    return (uint32_t)(((uint64_t)ms * rate) >> 24);
}
