// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
/* Moved verbatim from ak820pro.c in the phase-3 module split. */
#include "consumer_mod.h"

/* Sequence a MODIFIED CONSUMER keycode (e.g. LSA(KC_VOLU) on the knob) so the
 * host actually sees the modifiers before the media usage.
 *
 * THE PROBLEM IS ENDPOINT ORDERING, not timing between press and release.
 * usb_main.c sends the consumer/extra report on USB_ENDPOINT_IN_SHARED while the
 * keyboard report goes out on its own endpoint (KEYBOARD_SHARED_EP is not
 * defined). The host polls those independently and guarantees no ordering
 * between them, so QMK's register_code16() -- which registers the mods and sends
 * the consumer usage back-to-back with no gap -- lets macOS service the shared
 * endpoint first and see the volume event with the wrong modifier state.
 *
 * Measured on hardware with the knob set to LSA(KC_VOLD/U):
 *   default (no delay)                ~1/3 of clicks did full steps
 *   ENCODER_MAP_KEY_DELAY 10          ~1/8  -- better, still wrong
 *   this sequencing                   to be confirmed
 *
 * ENCODER_MAP_KEY_DELAY only spaces press from release, which is why it reduced
 * the failure rate without fixing it: the race is INSIDE the press.
 *
 * Rare third symptom worth knowing, because it identifies the fault: seeing Alt
 * WITHOUT Shift opens the macOS Sound settings dialog. Three different outcomes
 * from one gesture is a host sampling a transient state, not a mapping error. */
/* Modifiers currently held for a modified-consumer burst, and when the last
 * event landed. Real mods rather than weak ones: weak mods are cleared by the
 * action layer on ordinary keypresses, which would silently drop them mid-spin. */
static uint8_t  mcons_mods = 0;
static uint16_t mcons_last = 0;

bool process_modified_consumer(uint16_t keycode, keyrecord_t *record) {
    if (!IS_QK_MODS(keycode)) return true;

    uint16_t base = keycode & 0xFF;
    if (!IS_CONSUMER_KEYCODE(base)) return true;

    extern uint8_t extract_mod_bits(uint16_t code);   /* quantum/quantum.c */
    uint8_t mods = extract_mod_bits(keycode);

    if (record->event.pressed) {
        /* Only pay the ordering cost when the modifiers are not already up. On a
         * spin every click after the first takes this branch and costs nothing,
         * which is what makes fast turning usable. */
        if (mcons_mods != mods) {
            if (mcons_mods) unregister_mods(mcons_mods);
            register_mods(mods);
            send_keyboard_report();              /* flush the mods FIRST */
            wait_ms(MODIFIED_CONSUMER_GAP_MS);   /* let the host poll and apply them */
            mcons_mods = mods;
        }
        mcons_last = timer_read();
        register_code(base);                     /* now the consumer usage */
    } else {
        unregister_code(base);
        mcons_last = timer_read();               /* mods released on idle, below */
    }
    return false;   /* fully handled */
}

/* Drop the held modifiers once the spin stops. Called from the housekeeping tick
 * so the release costs nothing on the encoder path itself. */
void modified_consumer_task(void) {
    if (mcons_mods && timer_elapsed(mcons_last) >= MODIFIED_CONSUMER_HOLD_MS) {
        unregister_mods(mcons_mods);
        send_keyboard_report();
        mcons_mods = 0;
    }
}
