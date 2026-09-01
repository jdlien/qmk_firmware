// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
/* Owner of the persisted kb datablock. See kb_eeconfig.h for the layout
 * contract (assign-only reserved bytes). */
#include "quantum.h"
#include "kb_eeconfig.h"
#include "bluetooth/ch582f_ajazz.h"

// Persisted keyboard config (EEPROM kb datablock; 4 bytes reserved, room to grow).
typedef struct __attribute__((packed)) {
    uint8_t bt_profile;   // last BT slot selected (CH582_PROFILE_BT_1..3)
    uint8_t _pad[3];
} kb_config_t;
static kb_config_t kb_config;

void kb_eeconfig_init(void) {
    // A fresh/invalid EEPROM zero-fills the block, so a 0 (or out-of-range)
    // bt_profile falls back to slot 1 in the getter.
    eeconfig_read_kb_datablock(&kb_config, 0, sizeof(kb_config));
}

uint8_t kb_eeconfig_get_bt_profile(void) {
    if (kb_config.bt_profile >= CH582_PROFILE_BT_1 &&
        kb_config.bt_profile <= CH582_PROFILE_BT_3) {
        return kb_config.bt_profile;
    }
    return CH582_PROFILE_BT_1;
}

// Write the remembered slot back only when it actually changed -- wear-leveling
// on internal flash is cheap but not free, and slot changes are user-initiated.
void kb_eeconfig_set_bt_profile(uint8_t p) {
    if (kb_config.bt_profile == p) return;
    kb_config.bt_profile = p;
    eeconfig_update_kb_datablock(&kb_config, 0, sizeof(kb_config));
}

#ifdef WDT_TEST_HOOKS
void kb_eeconfig_test_write(void) {
    /* Toggle a reserved pad byte so wear-levelling cannot skip an identical
     * write -- the flash really programs. */
    kb_config._pad[0] ^= 1;
    eeconfig_update_kb_datablock(&kb_config, 0, sizeof(kb_config));
}
#endif
