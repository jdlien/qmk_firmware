// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "quantum.h"

/* Wireless-mode UI: the tri-state slider, the BT slot keys (tap = select,
 * hold = pair), Fn+P, and the hold-to-pair progress machinery. Owns the
 * current wireless mode and the remembered slot (persisted via
 * kb_eeconfig). All main-loop context. */

/* The mode slider (dip index 1 = BT, 2 = 2.4G, both inactive = USB). Call
 * from dip_switch_update_kb for those indices. */
void bt_ui_mode_slider(uint8_t index, bool active);

/* Re-select the saved slot after kb_eeconfig_init() -- dip_switch_init runs
 * before post-init, so its boot-time selection used the default slot. */
void bt_ui_resume_saved_profile(void);

/* Keycode handlers, called from process_record_kb. Each fully handles its
 * key and returns false (stop processing). */
bool bt_ui_slot_key(uint16_t keycode, keyrecord_t *record);   /* BT1/BT2/BT3 */
bool bt_ui_24g_key(keyrecord_t *record);                      /* BT24G */
bool bt_ui_pair_key(keyrecord_t *record);                     /* BT_PAIR */

/* 10 Hz housekeeping: fire hold-to-pair at the threshold, under the finger. */
void bt_pair_hold_task(void);
