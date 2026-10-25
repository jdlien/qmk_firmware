// Copyright 2026 Fernando Birra, JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
/* Wireless-mode UI. Moved verbatim from ak820pro.c in the phase-3 module
 * split; behaviour unchanged. See bt_ui.h. */
#include "bt_ui.h"
#include "kb_eeconfig.h"
#include "connection.h"
#include "graphics/display.h"
#include "bluetooth/ch582f_ajazz.h"
#include "ak820pro.h"

// Current wireless mode, derived from the tri-state slider. The Fn BT controls
// are only meaningful in the matching mode (e.g. Fn+Q selects a BT slot only
// while the slider is in the BT position), so they are gated on this.
enum wireless_mode {
    WL_MODE_USB = 0,
    WL_MODE_BT,
    WL_MODE_24G
};
static uint8_t wireless_mode = WL_MODE_USB;

// Last BT slot the user selected. Entering BT mode used to hardcode slot 1, so
// leaving BT for USB/2.4G and coming back silently dropped you onto slot 1
// whatever you had been connected to. Persisted (kb_eeconfig) so it survives
// mode switches and power cycles.
static ch582_profile_t last_bt_profile = CH582_PROFILE_BT_1;

static void save_bt_profile(ch582_profile_t p) {
    last_bt_profile = p;
    kb_eeconfig_set_bt_profile((uint8_t)p);
}

// Fn+P long-press tracking: pairing only starts after a sustained hold, and
// only while in a wireless mode.
/* 2 s, not 1 s. A slot-key press ALSO issues a select/reconnect, so a merely
 * slow tap at 1 s would drop a live link and start advertising -- recovering
 * needs a re-select. Pairing is rare and deliberate; reconnecting is the common
 * action, so the gesture must sit well clear of any tap. The progress bar makes
 * the longer hold legible rather than annoying. Measured caveat: the hold task
 * runs on the 10 Hz tick and the band redraws on the same tick, so the PERCEIVED
 * threshold is ~200 ms longer than this number. */
#define BT_PAIR_HOLD_MS 2000
static uint16_t bt_pair_timer = 0;
static bool     bt_pair_armed = false;

/* The menu's pair (P3): armed by bt_ui_pair_slot(), sent from
 * bt_pair_hold_task() once the link isn't linking -- the module drops A6 51
 * mid-attempt, and a pair sent at once loses to the slot's old host, whose
 * 5B 32 clears the pair's retry (docs/wireless.md) -- or after
 * BT_MENU_PAIR_WAIT_MS at the latest, when the driver's own retry takes over.
 * A slot key, Fn+P and every slider event disarm it. */
#define BT_MENU_PAIR_WAIT_MS 6000
static uint16_t menu_pair_timer = 0;
static bool     menu_pair_armed = false;

void bt_ui_resume_saved_profile(void) {
    last_bt_profile = (ch582_profile_t)kb_eeconfig_get_bt_profile();
    // dip_switch_init() runs BEFORE post-init, so its boot-time BT selection
    // used the default slot (EEPROM had not been read yet). Now that we have
    // the saved slot, re-select it if we booted with the slider already on BT.
    if (wireless_mode == WL_MODE_BT)
        ch582_set_profile(last_bt_profile);
}

void bt_ui_mode_slider(uint8_t index, bool active) {
    /* Release everything on the OUTGOING host BEFORE routing flips (analysis
     * credit: Rachel, 2026-09-01). QMK's handle_host_changed() never clears
     * the report state -- verified in quantum/connection/connection.c -- so
     * in principle a key held across the slide sends its press on one
     * route (USB) and its release on the other (CH582F), leaving the USB
     * host with a stuck, auto-repeating key.
     *
     * TESTED ON HARDWARE 2026-09-01 and the stuck key did NOT reproduce on
     * macOS (held 'a' across wired->BT on the non-rebooting edge: the key
     * simply stopped). Something un-traced clears it -- possibly host-side
     * behaviour -- but since the saving mechanism is UNIDENTIFIED, this
     * explicit clear stays: one zero-report per pin event buys correctness
     * that doesn't depend on an unexplained mercy or on macOS. (BT->cable
     * needs no help on this unit: the slider is a power-source switch and
     * that direction reboots the MCU -- see CLAUDE.md.)
     *
     * MUST run here, not in connection_host_changed_kb(): by the time that
     * callback fires, connection.c has already flipped desired_host, so a
     * clear there zero-reports the NEW host and strands the old one. */
    clear_keyboard();
    menu_pair_armed = false;   /* a menu pair belongs to the BT slot it was armed on */

    // The mode slider is a tri-state encoded by two dip pins: index 1 = BT,
    // index 2 = 2.4G, both inactive = USB. We must look at BOTH pins together
    // -- handling them independently lets the inactive sibling's "else" branch
    // call ch582_cancel_connect() and clobber connect_requested even while the
    // other mode is active (e.g. at boot in BT position), silently disabling
    // wireless key forwarding. Recompute the mode from the latched pin states.
    static bool bt_on  = false;
    static bool g24_on = false;
    if (index == 1) bt_on = active;
    if (index == 2) g24_on = active;

    // The CH582F handles both BT and 2.4G over the same UART, and QMK's
    // CONNECTION_HOST_2P4GHZ is not wired, so BOTH wireless positions map to
    // CONNECTION_HOST_BLUETOOTH (QMK then routes key reports to bt_driver ->
    // our bluetooth_send_keyboard). Our own A6 profile-select tells the
    // module which radio to use.
    if (bt_on) {
        wireless_mode = WL_MODE_BT;
        ch582_set_profile(last_bt_profile);  // resume the slot last selected
        connection_set_host_noeeprom(CONNECTION_HOST_BLUETOOTH);
        display_draw_bluetooth_logo();
    } else if (g24_on) {
        wireless_mode = WL_MODE_24G;
        ch582_set_profile(CH582_PROFILE_PEER_24G);
        connection_set_host_noeeprom(CONNECTION_HOST_BLUETOOTH);
        display_draw_2_4_g_logo();
    } else {
        // USB mode (both inactive): stop retrying, but keep the module alive
        // (it must stay powered to keep reporting battery level). Route key
        // reports back to USB.
        wireless_mode = WL_MODE_USB;
        ch582_cancel_connect();
        connection_set_host_noeeprom(CONNECTION_HOST_USB);
        display_draw_usb_logo();
    }
}

/* BT slot keys use the @isuua/edthu devctrl model: TAP = select the slot
 * (A6 <slot>, reconnect the existing bond); HOLD = select + pair (adds
 * A6 0x51 on the held slot). Select happens on press for instant feedback;
 * the pair is added if the key is still held at BT_PAIR_HOLD_MS. This
 * replaces needing a separate pair key, though Fn+P still works. */
/* The tap's select, shared with the menu's (P3). */
static void select_slot(ch582_profile_t slot) {
    save_bt_profile(slot);
    ch582_set_profile(last_bt_profile);          // tap action: select
    connection_set_host_noeeprom(CONNECTION_HOST_BLUETOOTH);
    display_draw_bluetooth_logo();
}

bool bt_ui_slot_key(uint16_t keycode, keyrecord_t *record) {
    if (record->event.pressed) {
        menu_pair_armed = false;   /* a press supersedes the menu's pair */
        if (wireless_mode == WL_MODE_BT) {
            ch582_profile_t slot = keycode == BT1 ? CH582_PROFILE_BT_1
                                 : keycode == BT2 ? CH582_PROFILE_BT_2
                                                  : CH582_PROFILE_BT_3;
            select_slot(slot);
            bt_pair_timer = timer_read();                // arm hold-to-pair
            bt_pair_armed = true;
            display_set_pair_hint(0);      // progress bar until it fires
        }
    } else {
        /* Release only disarms. Pairing now fires from bt_pair_hold_task()
         * the instant BT_PAIR_HOLD_MS elapses while the key is STILL DOWN --
         * previously it fired here, on key-up, so holding the key did nothing
         * visible until you let go and there was no way to tell whether the
         * hold had "taken". That also contradicted the comment above, which
         * describes firing at the threshold. */
        display_set_pair_hint(-1);      // released early: hold abandoned
        bt_pair_armed = false;
    }
    return false;
}

bool bt_ui_24g_key(keyrecord_t *record) {
    if (record->event.pressed && wireless_mode == WL_MODE_24G) {
        ch582_set_profile(CH582_PROFILE_PEER_24G);
        connection_set_host_noeeprom(CONNECTION_HOST_BLUETOOTH);
        display_draw_2_4_g_logo();
    }
    return false;
}

bool bt_ui_pair_key(keyrecord_t *record) {
    if (record->event.pressed) {
        menu_pair_armed = false;   /* a press supersedes the menu's pair */
        // Arm a long-press only in a wireless mode; ignore in USB.
        bt_pair_armed = (wireless_mode != WL_MODE_USB);
        bt_pair_timer = timer_read();
        if (bt_pair_armed) display_set_pair_hint(0);
    } else {
        display_set_pair_hint(-1);
        bt_pair_armed = false;   /* fired from bt_pair_hold_task() */
    }
    return false;
}

/* Fire hold-to-pair the moment the threshold passes, while the key is still
 * held, so the LCD reacts under your finger instead of on release. Shared by the
 * BT slot keys and Fn+P; both only ARM here and are disarmed on key-up. */
void bt_pair_hold_task(void) {
    if (menu_pair_armed) {
        /* Not while the select is still being sent: a bounce's own 5B 32 says
         * CONNECTED, and A6 51 then would replace the bounce and pair the
         * bounce slot (codex on P3's design). A select whose retries ran out
         * unanswered gets no pair at all: the module may be on another slot. */
        ch582_conn_state_t st  = ch582_get_conn_state();
        ch582_select_t     sel = ch582_select_state();
        if (wireless_mode != WL_MODE_BT || sel == CH582_SEL_LOST || (st == CH582_CONN_PAIRING && ch582_pair_confirmed())) {
            menu_pair_armed = false;   /* off BT; lost; or already advertising: pairable, nothing to send */
        } else if (sel == CH582_SEL_ACKED &&
                   (st != CH582_CONN_LINKING || timer_elapsed(menu_pair_timer) >= BT_MENU_PAIR_WAIT_MS)) {
            menu_pair_armed = false;
            ch582_enter_pairing();
        }
    }
    if (!bt_pair_armed) return;
    uint16_t held = timer_elapsed(bt_pair_timer);
    if (held < BT_PAIR_HOLD_MS) {
        display_set_pair_hint((int16_t)((held * 100u) / BT_PAIR_HOLD_MS));
        return;
    }
    bt_pair_armed = false;      /* one-shot: do not re-enter pairing while held */
    display_set_pair_hint(-1);      // resolved: the band now shows PAIRING
    ch582_enter_pairing();
}

/* --- The menu's Connection (P3) ------------------------------------------------
 * The same calls as the keys, decided here beside the hold; the menu only reads
 * the facts (menu_glue.c's env_bt_*). plans/CONFIG-MENU-P1-DESIGN.md, "P3". */

static ch582_profile_t slot_profile(uint8_t slot) {
    return slot == 0 ? CH582_PROFILE_BT_1 : slot == 1 ? CH582_PROFILE_BT_2 : CH582_PROFILE_BT_3;
}

void bt_ui_select_slot(uint8_t slot) {
    if (wireless_mode != WL_MODE_BT || slot > 2) return;
    menu_pair_armed = false;
    select_slot(slot_profile(slot));
}

void bt_ui_pair_slot(uint8_t slot) {
    if (wireless_mode != WL_MODE_BT || slot > 2) return;
    ch582_profile_t p = slot_profile(slot);
    /* Already advertising on it (the key's hold): pairable as it is. A
     * redundant A6 51 might go unanswered, and the page would read failed. */
    if (p == last_bt_profile && ch582_get_conn_state() == CH582_CONN_PAIRING && ch582_pair_confirmed()) return;
    /* The current slot isn't re-selected: that would only restart an attempt
     * the pair then has to wait out. Unless its select was lost (codex). */
    if (p != last_bt_profile || ch582_select_state() == CH582_SEL_LOST) select_slot(p);
    menu_pair_timer = timer_read();
    menu_pair_armed = true;
}

void bt_ui_pair_stop(void) {
    menu_pair_armed = false;
    if (wireless_mode != WL_MODE_BT) return;
    /* Advertising (optimistic or confirmed), or a pair still being resent:
     * the tap's select cancels it (ch582_set_profile()'s bounce, or a plain
     * select that supersedes the retry). Otherwise there's nothing to stop. */
    if (ch582_get_conn_state() == CH582_CONN_PAIRING || ch582_pair_retrying()) select_slot(last_bt_profile);
}

bool bt_ui_pair_waiting(void) { return menu_pair_armed; }
