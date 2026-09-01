// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdint.h>

/* Persisted keyboard config (the EEPROM kb datablock). One module owns the
 * struct so its layout has exactly one home; everything else goes through
 * accessors. The block is 4 bytes with 3 reserved -- phase 4 of the
 * hardening plan assigns them (rtc_period u16, lcd_brightness u8), so this
 * layout is APPEND/ASSIGN-ONLY: existing on-device bytes must keep meaning. */

/* Read the block from EEPROM. Call once from post-init. */
void kb_eeconfig_init(void);

/* Last BT slot selected (CH582_PROFILE_BT_1..3), validated; falls back to
 * slot 1 on a fresh/garbage block. */
uint8_t kb_eeconfig_get_bt_profile(void);

/* Persist a new BT slot. Writes only on change -- wear-levelled internal
 * flash is cheap but not free, and slot changes are user-initiated. */
void kb_eeconfig_set_bt_profile(uint8_t p);

#ifdef WDT_TEST_HOOKS
/* Force a REAL kb-eeconfig flash program (toggles a reserved pad byte so
 * wear-levelling cannot skip an identical write). Test hook only. */
void kb_eeconfig_test_write(void);
#endif
