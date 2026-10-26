// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The RGB effects' speed curves (pure logic; rgb_effects.c has the QMK hooks).
// A rate counts in 2^-24 of a unit a millisecond, so a phase of
// (ms * rate) >> 24 moves one unit every 2^24 / rate ms: a hue's 256 make a
// cycle of 2^32 / rate ms.
#pragma once

#include <stdint.h>

typedef struct {
    uint32_t r0;        /* the rate at speed 0 */
    uint16_t octaves;   /* its doublings to speed 255, 8.8 (under 15) */
} rgb_speed_curve_t;

extern const rgb_speed_curve_t RGB_SPEED_CYCLE_ALL;     /* a hue cycle in 10 min to 10 s */
extern const rgb_speed_curve_t RGB_SPEED_CYCLE_SWEEP;   /* Cycle L-R and U-D: 2 min to 2 s */
extern const rgb_speed_curve_t RGB_SPEED_JELLYBEAN;     /* a drop every 2 s to 30 a second */

uint32_t rgb_speed_rate(const rgb_speed_curve_t *c, uint8_t speed);
uint32_t rgb_speed_phase(uint32_t ms, uint32_t rate);   /* (ms * rate) >> 24 */
