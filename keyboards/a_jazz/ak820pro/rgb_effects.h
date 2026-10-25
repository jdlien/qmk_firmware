// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The enabled RGB effects, named as the Fn readout names them: one table for
// the readout (param_overlay.c) and the menu's Effect choice (P1b,
// plans/CONFIG-MENU-P1-DESIGN.md), in mode order.
#pragma once

#include <stdint.h>

typedef struct {
    uint8_t mode;       /* QMK's rgb_matrix mode */
    char    name[12];   /* at most 11 characters: "Alphas/Mods" */
} rgb_effect_t;

extern const rgb_effect_t rgb_effects[];
extern const uint8_t      rgb_effects_n;

const char *rgb_effect_name(uint8_t mode);   /* "Off" for RGB_MATRIX_NONE; NULL for a mode the table doesn't list */
