// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
#include "rgb_effects.h"
#include "rgb_speed.h"

#include "quantum.h"
#include <lib/lib8tion/lib8tion.h>   /* scale16by8, qadd8: upstream's time for other effects */

/* Moved out of param_overlay.c's rgb_mode_short() for P1b, so the readout
 * and the menu share it (the plan's "named by one shared table"). NOT
 * rgb_matrix_get_mode_name(): that is gated behind RGB_MATRIX_MODE_NAME_ENABLE,
 * costs flash for all ~40 effect names, and returns the raw enum spelling --
 * "RAINBOW_MOVING_CHEVRON" is 22 characters against a 12-character band.
 * Each entry is #ifdef'd on the effect's own enable, so this still builds if
 * the animation list changes. The entries are in mode order:
 * rgb_matrix_effects.inc's, then rgb_matrix_kb.inc's customs.
 * The second field is the effect's STABLE id (P5a, the settings profiles):
 * a profile stores it, since QMK's mode numbers move when an effect is
 * enabled or disabled. Append-only, never renumbered or reused, and 0 is
 * RGB_MATRIX_NONE's; scripts/check_effect_ids.py guards it. */
const rgb_effect_t rgb_effects[] = {
#ifdef ENABLE_RGB_MATRIX_SOLID_COLOR
    {RGB_MATRIX_SOLID_COLOR, 1, "Solid"},
#endif
#ifdef ENABLE_RGB_MATRIX_ALPHAS_MODS
    {RGB_MATRIX_ALPHAS_MODS, 2, "Alphas/Mods"},
#endif
#ifdef ENABLE_RGB_MATRIX_BREATHING
    {RGB_MATRIX_BREATHING, 3, "Breathing"},
#endif
#ifdef ENABLE_RGB_MATRIX_CYCLE_ALL
    {RGB_MATRIX_CYCLE_ALL, 4, "Cycle All"},
#endif
#ifdef ENABLE_RGB_MATRIX_CYCLE_LEFT_RIGHT
    {RGB_MATRIX_CYCLE_LEFT_RIGHT, 5, "Cycle L-R"},
#endif
#ifdef ENABLE_RGB_MATRIX_CYCLE_UP_DOWN
    {RGB_MATRIX_CYCLE_UP_DOWN, 6, "Cycle U-D"},
#endif
#ifdef ENABLE_RGB_MATRIX_RAINBOW_MOVING_CHEVRON
    {RGB_MATRIX_RAINBOW_MOVING_CHEVRON, 7, "Chevron"},
#endif
#ifdef ENABLE_RGB_MATRIX_CYCLE_PINWHEEL
    {RGB_MATRIX_CYCLE_PINWHEEL, 8, "Pinwheel"},
#endif
#ifdef ENABLE_RGB_MATRIX_JELLYBEAN_RAINDROPS
    {RGB_MATRIX_JELLYBEAN_RAINDROPS, 9, "Jellybean"},
#endif
#ifdef ENABLE_RGB_MATRIX_TYPING_HEATMAP
    {RGB_MATRIX_TYPING_HEATMAP, 10, "Heatmap"},
#endif
#if defined(RGB_MATRIX_CUSTOM_KB)
    {RGB_MATRIX_CUSTOM_RAINFALL, 11, "Rainfall"},
    {RGB_MATRIX_CUSTOM_DRIFT, 12, "Drift"},
#endif
};
const uint8_t rgb_effects_n = sizeof rgb_effects / sizeof rgb_effects[0];

const char *rgb_effect_name(uint8_t mode) {
    if (mode == RGB_MATRIX_NONE) return "Off";
    for (uint8_t i = 0; i < rgb_effects_n; i++)
        if (rgb_effects[i].mode == mode) return rgb_effects[i].name;
    return NULL;
}

/* The effects' speeds (rgb_speed.c): quantum/rgb_matrix's two board hooks.
 * Any other effect_runner_i effect keeps upstream's time. */
uint8_t rgb_matrix_runner_i_time(void) {
    uint8_t s = rgb_matrix_config.speed;
    switch (rgb_matrix_get_mode()) {
#ifdef ENABLE_RGB_MATRIX_CYCLE_ALL
        case RGB_MATRIX_CYCLE_ALL:
            return (uint8_t)rgb_speed_phase(g_rgb_timer, rgb_speed_rate(&RGB_SPEED_CYCLE_ALL, s));
#endif
#ifdef ENABLE_RGB_MATRIX_CYCLE_LEFT_RIGHT
        case RGB_MATRIX_CYCLE_LEFT_RIGHT:
            return (uint8_t)rgb_speed_phase(g_rgb_timer, rgb_speed_rate(&RGB_SPEED_CYCLE_SWEEP, s));
#endif
#ifdef ENABLE_RGB_MATRIX_CYCLE_UP_DOWN
        case RGB_MATRIX_CYCLE_UP_DOWN:
            return (uint8_t)rgb_speed_phase(g_rgb_timer, rgb_speed_rate(&RGB_SPEED_CYCLE_SWEEP, s));
#endif
        default:
            return scale16by8(g_rgb_timer, qadd8(s / 4, 1));
    }
}

/* A drop each time the count moves: one a frame at most, and 30 a second
 * against ~60 frames, so its low byte is enough. */
bool rgb_matrix_jellybean_drop(void) {
    static uint8_t last;
    uint8_t        n = (uint8_t)rgb_speed_phase(g_rgb_timer, rgb_speed_rate(&RGB_SPEED_JELLYBEAN, rgb_matrix_config.speed));
    if (n == last) return false;
    last = n;
    return true;
}
