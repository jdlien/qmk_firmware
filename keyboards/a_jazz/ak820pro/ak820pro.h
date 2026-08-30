// Copyright 2026 Fernando Birra
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "quantum.h"

// Keyboard-level custom keycodes handled in process_record_kb(). Based at
// QK_KB_0 (not SAFE_RANGE/QK_USER) so their values line up with VIA, which maps
// its customKeycodes[] to the QK_KB_0 range -- otherwise a keycode reassigned in
// VIA is stored as 0x7E00+i but the firmware's case is 0x7E40+i and never fires.
enum ak820pro_keycodes {
    SCR_TOG = QK_KB_0,     // toggle LCD backlight
    BT1,                   // Fn+Q: BT slot 1 (BT mode)
    BT2,                   // Fn+W: BT slot 2 (BT mode)
    BT3,                   // Fn+E: BT slot 3 (BT mode)
    BT24G,                 // Fn+R: 2.4G       (2.4G mode)
    BT_PAIR,               // Fn+P long-press: pair (BT/2.4G)
    // RGB-matrix control wrappers, exposed to VIA as custom keycodes. VIA's
    // built-in "Lighting" picker only offers the underglow RGB_* keycodes
    // (0x7820), which do nothing on this matrix-only board; these forward to the
    // rgb_matrix_* API so they can be assigned from VIA's Custom tab.
    RGBM_TOG,              // toggle
    RGBM_MOD,              // next effect
    RGBM_RMOD,             // previous effect
    RGBM_HUI,              // hue +
    RGBM_HUD,              // hue -
    RGBM_SAI,              // sat +
    RGBM_SAD,              // sat -
    RGBM_VAI,              // brightness +
    RGBM_VAD,              // brightness -
    RGBM_SPI,              // speed +
    RGBM_SPD,              // speed -
    ANIM_TOG,              // toggle the flash-animation player
    // NOTE: order here is index-matched to via.json customKeycodes[]. Append
    // only; inserting shifts every later keycode and silently breaks VIA maps.
    SCR_UP,                // LCD backlight brighter
    SCR_DN,                // LCD backlight dimmer
    AK820PRO_SAFE_RANGE
};
