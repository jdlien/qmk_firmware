// Copyright 2026 Fernando Birra
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "quantum.h"

/* Layer indices, shared between the keymaps and board code. Historically the
 * keymaps owned this enum and indicators.c hand-wrote the Fn mask as
 * (1<<1)|(1<<3) -- a silent-corruption coupling if layers were ever
 * rearranged. The mac/win dip switch selects the base layer, so per-key
 * remaps usually need doing on both bases. */
enum ak820pro_layers {
    WINBASE = 0,
    WINFN,
    MACBASE,
    MACFN,
};

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
    BT_PAIR,               /* Pair the CURRENTLY-SELECTED profile (A6 51).
                            * UNBOUND in the default keymap as of 2026-08-29 --
                            * kept here because this enum is INDEX-MATCHED to
                            * via.json's customKeycodes[]; removing it would shift
                            * every later keycode and corrupt existing VIA keymaps.
                            * Assign it in VIA if you want it back.
                            *
                            * Dropped because it is redundant where it works and
                            * destructive where it is unique: holding Fn+Q/W/E
                            * already selects AND pairs a BT slot, while in 2.4G it
                            * is the only pairing key but drops a working dongle
                            * link -- and the dongle almost certainly cannot be put
                            * into pairing mode without vendor software, so the
                            * broadcast is half a handshake that never completes.
                            * Confirmed on hardware: pressing it killed the link,
                            * and toggling the slider restored it, which means the
                            * dongle never lost its bond. */
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

/* Loop-stall attribution (see loop_gap_task in ak820pro.c). Which long
 * operation was in progress when the main loop stalled -- the three suspects
 * need entirely different fixes, so the probe reports the culprit, not just
 * the duration. */
#define LOOP_MARK_NONE  0
#define LOOP_MARK_FLASH 1
#define LOOP_MARK_BLIT  2
#define LOOP_MARK_I2C   3
extern volatile uint8_t loop_stall_mark;
