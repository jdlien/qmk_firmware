// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
/* Moved verbatim from ak820pro.c in the phase-3 module split. */
#include "consumer_mod.h"
#include "keycode_config.h"   /* keycode_config(), for the modifier tracker */

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
 * action layer on ordinary keypresses, which would silently drop them mid-spin.
 *
 * Real mods are a plain bitmask, not a count, so the burst must never release
 * a bit a modifier key is physically holding (the 10-31 audit, F-004: a held
 * Shift went up 150 ms after a detent, and the next letters came out
 * lowercase), and must put back a bit a key released mid-spin (a Shift tapped
 * between detents sent the rest of the spin as Alt+Volume, which opens the
 * Sound settings). mcons_phys follows the modifier keys themselves, after
 * keycode_config() as QMK's action path applies it (an Alt/GUI swap holds
 * GUI); mods from MT(), OSM() or LM() keys are not seen, and JD's keymap has
 * none. */
static uint8_t  mcons_mods = 0;
static uint8_t  mcons_phys = 0;   /* modifier keys physically down */
static uint16_t mcons_last = 0;

void consumer_mod_note_key(uint16_t keycode, keyrecord_t *record) {
    keycode = keycode_config(keycode);   /* what register_code() will be given */
    if (!IS_MODIFIER_KEYCODE(keycode)) return;
    uint8_t bit = MOD_BIT(keycode);
    if (record->event.pressed) mcons_phys |= bit;
    else                       mcons_phys &= (uint8_t)~bit;
}

bool process_modified_consumer(uint16_t keycode, keyrecord_t *record) {
    if (!IS_QK_MODS(keycode)) return true;

    uint16_t base = keycode & 0xFF;
    if (!IS_CONSUMER_KEYCODE(base)) return true;

    extern uint8_t extract_mod_bits(uint16_t code);   /* quantum/quantum.c */
    uint8_t mods = extract_mod_bits(keycode);

    if (record->event.pressed) {
        bool start = mcons_mods != mods;
        if (start) {
            /* A new burst or a different set: drop the old one's bits, but not
             * one a key holds. */
            uint8_t drop = mcons_mods & (uint8_t)~mods & (uint8_t)~mcons_phys;
            if (drop) unregister_mods(drop);
            mcons_mods = mods;
        }
        /* A bit a key's release took mid-spin goes back. */
        uint8_t missing = mods & (uint8_t)~get_mods();
        if (missing) register_mods(missing);
        /* Only pay the ordering cost at a burst's start or a bit put back. At
         * the start even when the keys already hold every bit: a modifier just
         * pressed may still be queued on the keyboard endpoint (codex on F-004:
         * Option held, Shift pressed just before the detent). On a spin every
         * other click costs nothing, which is what makes fast turning usable. */
        if (start || missing) {
            send_keyboard_report();              /* flush the mods FIRST */
            wait_ms(MODIFIED_CONSUMER_GAP_MS);   /* let the host poll and apply them */
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
        uint8_t drop = mcons_mods & (uint8_t)~mcons_phys;   /* a key still holds the rest */
        if (drop) unregister_mods(drop);
        send_keyboard_report();
        mcons_mods = 0;
    }
}
