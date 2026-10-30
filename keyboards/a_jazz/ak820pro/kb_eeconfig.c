// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
/* Owner of the persisted kb datablock. See kb_eeconfig.h for the layout
 * contract (assign-only reserved bytes).
 *
 * ALL writes are COALESCED AND DEFERRED: setters update RAM and mark the
 * block dirty; kb_eeconfig_task() (10 Hz housekeeping) programs the flash
 * once the values have been stable for KB_EECONFIG_SETTLE_MS. One policy for
 * every field -- a brightness sweep, a slot change and an RTC trim all cost
 * at most one program each settle window, and every write goes through the
 * wear-levelling layer whose backing_store_pre_write_hook() drains any
 * in-flight LCD blit first (the historical hang). The cost is that a change
 * made within ~5 s of power loss is not persisted; every field here is a
 * remembered convenience, so that trade is correct. */
#include "quantum.h"
#include "sleep.h"   /* sleep_recovering() */
#include "kb_eeconfig.h"
#include "bluetooth/ch582f_ajazz.h"
#include "graphics/display.h"   /* DISPLAY_CLOCK_MODE_COUNT */
#include "power.h"              /* POWER_MODE_COUNT */
#include "indicators.h"         /* IND_LOCKS, IND_CHARGING */
#include "indicator_levels.h"   /* IND_LEVEL_MAX */

/* Layout (10 bytes; EECONFIG_KB_DATA_SIZE in config.h must match).
 * A fresh block holds ZEROS, so 0 must mean "unset" for every field added
 * after first ship -- hence the +1 encodings. Bytes 0..3 are the phase-4
 * block; byte 4 (clock format) was appended 2026-09-01, byte 5 (battery
 * level) 2026-09-28, byte 6 (power mode) 2026-10-18, bytes 7-8 (the
 * indicator levels, the menu's P2a) 2026-10-25 and byte 9 (the battery
 * readout, P4) the same day, each with a version bump. Byte 9's bit 2, the
 * menu's face (10-28), took no bump: a zeroed byte reads Large, the default,
 * and a firmware from before reads the bit set as garbage (percent). Nor did
 * its bit 4, the readout's Icon only (10-30), for the same reasons. */
typedef struct __attribute__((packed)) {
    uint8_t bt_profile;        // last BT slot selected (CH582_PROFILE_BT_1..3)
    uint8_t rtc_period_lo;     // converged RTC divider period, LE; 0 = unset
    uint8_t rtc_period_hi;
    uint8_t lcd_brightness_p1; // backlight level + 1; 0 = unset
    uint8_t clock_mode_p1;     // clock band format (display_clock_mode) + 1; 0 = unset
    uint8_t batt_level_p1;     // battery level in 0.5 % steps + 1 (1..201); 0 = unset
    uint8_t power_mode_p1;     // power mode (power_mode_t) + 1; 0 = unset
    uint8_t ind_locks_p1;      // the lock lights' level (0-100) + 1; 0 = unset
    uint8_t ind_charging_p1;   // the charging light's level (0-100) + 1; 0 = unset
    uint8_t batt_view;         // bits 0-1 the battery readout: 0 unset, 1 percent, 2 hours, 3 both (frozen; P4); bit 2 the menu's face, set Small (10-28); bit 4 Icon only (10-30)
} kb_config_t;
_Static_assert(sizeof(kb_config_t) == 10, "block layout is assign-only; do not grow casually");
_Static_assert(sizeof(kb_config_t) == EECONFIG_KB_DATA_SIZE, "config.h EECONFIG_KB_DATA_SIZE must match the struct");

static kb_config_t kb_config;
static bool        kb_dirty = false;
static uint32_t    kb_dirty_since;

#ifndef KB_EECONFIG_SETTLE_MS
#    define KB_EECONFIG_SETTLE_MS 5000u
#endif

void kb_eeconfig_init(void) {
    eeconfig_read_kb_datablock(&kb_config, 0, sizeof(kb_config));
}

void kb_eeconfig_task(void) {
    if (!kb_dirty || timer_elapsed32(kb_dirty_since) < KB_EECONFIG_SETTLE_MS) return;
    /* Not inside a deep sleep's recovery: its passes are budgeted, and a
     * flash write blocks (plans/DEEP-SLEEP-BUILD-PLAN.md, section 4). The
     * write waits a few passes. */
    if (sleep_recovering()) return;
    kb_dirty = false;
    eeconfig_update_kb_datablock(&kb_config, 0, sizeof(kb_config));
}

/* HC_PROFILES' flush (P5a): the pending write now, unsettled; never inside a
 * deep sleep's recovery either. */
void kb_eeconfig_flush(void) {
    if (!kb_dirty || sleep_recovering()) return;
    kb_dirty = false;
    eeconfig_update_kb_datablock(&kb_config, 0, sizeof(kb_config));
}

bool kb_eeconfig_write_pending(void) {
    return kb_dirty;
}

static void kb_mark_dirty(void) {
    kb_dirty       = true;
    kb_dirty_since = timer_read32();
}

/* P5b: the settings profiles' active slot rides this settle. profiles.c's
 * prof_tick() writes its pending byte once this is clean, so a run of
 * profile keys costs one write, and the sleep waits on it here. */
void kb_eeconfig_touch(void) {
    kb_mark_dirty();
}

uint8_t kb_eeconfig_get_bt_profile(void) {
    if (kb_config.bt_profile >= CH582_PROFILE_BT_1 &&
        kb_config.bt_profile <= CH582_PROFILE_BT_3) {
        return kb_config.bt_profile;
    }
    return CH582_PROFILE_BT_1;
}

void kb_eeconfig_set_bt_profile(uint8_t p) {
    if (kb_config.bt_profile == p) return;
    kb_config.bt_profile = p;
    kb_mark_dirty();
}

uint16_t kb_eeconfig_get_rtc_period(void) {
    return (uint16_t)(kb_config.rtc_period_lo | (kb_config.rtc_period_hi << 8));
}

void kb_eeconfig_set_rtc_period(uint16_t period) {
    if (kb_eeconfig_get_rtc_period() == period) return;
    kb_config.rtc_period_lo = (uint8_t)(period & 0xFF);
    kb_config.rtc_period_hi = (uint8_t)(period >> 8);
    kb_mark_dirty();
}

bool kb_eeconfig_get_lcd_brightness(uint8_t *level) {
    /* P2b: the level is 0-100 (version 6; display.c's BKL_MAX_LEVEL) */
    if (kb_config.lcd_brightness_p1 == 0 || kb_config.lcd_brightness_p1 > IND_LEVEL_MAX + 1u) {
        return false;   /* unset, or garbage (e.g. the test hook's poke) */
    }
    *level = (uint8_t)(kb_config.lcd_brightness_p1 - 1);
    return true;
}

void kb_eeconfig_set_lcd_brightness(uint8_t level) {
    uint8_t p1 = (uint8_t)(level + 1);
    if (kb_config.lcd_brightness_p1 == p1) return;
    kb_config.lcd_brightness_p1 = p1;
    kb_mark_dirty();
}

bool kb_eeconfig_get_clock_mode(uint8_t *mode) {
    if (kb_config.clock_mode_p1 == 0 || kb_config.clock_mode_p1 > DISPLAY_CLOCK_MODE_COUNT) {
        return false;   /* unset, or garbage */
    }
    *mode = (uint8_t)(kb_config.clock_mode_p1 - 1);
    return true;
}

void kb_eeconfig_set_clock_mode(uint8_t mode) {
    uint8_t p1 = (uint8_t)(mode + 1);
    if (kb_config.clock_mode_p1 == p1) return;
    kb_config.clock_mode_p1 = p1;
    kb_mark_dirty();
}

bool kb_eeconfig_get_batt_level(uint16_t *permille) {
    if (kb_config.batt_level_p1 == 0 || kb_config.batt_level_p1 > 201) {
        return false;   /* unset, or garbage */
    }
    *permille = (uint16_t)((kb_config.batt_level_p1 - 1u) * 5u);
    return true;
}

void kb_eeconfig_set_batt_level(uint16_t permille) {
    if (permille > 1000u) return;
    /* 0.5 % steps, so battery.c's 2.5 % save points store exactly. It writes
     * only when the value changes: battery.c calls this every second but
     * changes what it passes ~6 times a charge cycle. */
    uint8_t p1 = (uint8_t)(permille / 5u + 1u);
    if (kb_config.batt_level_p1 == p1) return;
    kb_config.batt_level_p1 = p1;
    kb_mark_dirty();
}

bool kb_eeconfig_get_power_mode(uint8_t *mode) {
    if (kb_config.power_mode_p1 == 0 || kb_config.power_mode_p1 > POWER_MODE_COUNT) {
        return false;   /* unset, or garbage */
    }
    *mode = (uint8_t)(kb_config.power_mode_p1 - 1);
    return true;
}

void kb_eeconfig_set_power_mode(uint8_t mode) {
    uint8_t p1 = (uint8_t)(mode + 1);
    if (kb_config.power_mode_p1 == p1) return;
    kb_config.power_mode_p1 = p1;
    kb_mark_dirty();
}

static uint8_t *ind_field(uint8_t which) {
    return which == IND_LOCKS ? &kb_config.ind_locks_p1 : which == IND_CHARGING ? &kb_config.ind_charging_p1 : NULL;
}

bool kb_eeconfig_get_ind_level(uint8_t which, uint8_t *level) {
    const uint8_t *f = ind_field(which);
    if (!f || *f == 0 || *f > IND_LEVEL_MAX + 1u) return false;   /* unset, or garbage */
    *level = (uint8_t)(*f - 1u);
    return true;
}

void kb_eeconfig_set_ind_level(uint8_t which, uint8_t level) {
    uint8_t *f = ind_field(which);
    if (!f || level > IND_LEVEL_MAX) return;
    uint8_t p1 = (uint8_t)(level + 1u);
    if (*f == p1) return;
    *f = p1;
    kb_mark_dirty();
}

/* Byte 9: the readout in bits 0-1 (0 unset), the menu's face in bits 2-3
 * (10-28: bit 2 set Small, bit 3 set Regular, neither Large), and Icon only
 * in bit 4 with bits 0-1 clear (10-30: the 2-bit field was full). A byte with
 * any of bits 5-7 set (0xFF, an erased block), both face bits, or bit 4 beside
 * a readout, is garbage as a whole: every reader and writer takes it as 0,
 * both defaults, before it reads, compares or keeps a field (codex, 10-28:
 * keeping the raw bit 2 of 0xFF would turn Large into Small). */
#define BV_VIEW  0x03u
#define BV_SMALL 0x04u
#define BV_REG   0x08u
#define BV_FACE  (BV_SMALL | BV_REG)
#define BV_ICON  0x10u
static uint8_t bv_norm(void) {
    uint8_t b   = kb_config.batt_view;
    bool    bad = (b & (uint8_t)~(BV_VIEW | BV_FACE | BV_ICON)) || (b & BV_FACE) == BV_FACE || ((b & BV_ICON) && (b & BV_VIEW));
    return bad ? 0u : b;
}
/* The canonical byte, written when it isn't what is stored. A valid byte
 * differs only in the field being set; a garbage one always differs, so a
 * setter repairs it (Hours over 0xFE writes 0x02, no early return). */
static void bv_put(uint8_t b) {
    if (kb_config.batt_view == b) return;
    kb_config.batt_view = b;
    kb_mark_dirty();
}

uint8_t kb_eeconfig_get_batt_view(void) {
    uint8_t b = bv_norm(), v = (uint8_t)(b & BV_VIEW);
    if (b & BV_ICON) return KB_BATT_VIEW_ICON;
    return v ? v : KB_BATT_VIEW_PERCENT;   /* unset or garbage: percent */
}

void kb_eeconfig_set_batt_view(uint8_t v) {
    if (v < KB_BATT_VIEW_PERCENT || v > KB_BATT_VIEW_ICON) return;
    bv_put((uint8_t)((bv_norm() & BV_FACE) | (v == KB_BATT_VIEW_ICON ? BV_ICON : v)));   /* the face kept */
}

uint8_t kb_eeconfig_get_font(void) {
    uint8_t b = bv_norm();
    return (b & BV_SMALL) ? KB_FONT_SMALL : (b & BV_REG) ? KB_FONT_REGULAR : KB_FONT_LARGE;   /* unset or garbage: Large */
}

void kb_eeconfig_set_font(uint8_t font) {
    if (font != KB_FONT_LARGE && font != KB_FONT_SMALL && font != KB_FONT_REGULAR) return;
    bv_put((uint8_t)((bv_norm() & (BV_VIEW | BV_ICON)) | (font == KB_FONT_SMALL ? BV_SMALL : font == KB_FONT_REGULAR ? BV_REG : 0u)));   /* the readout kept */
}

#ifdef WDT_TEST_HOOKS
void kb_eeconfig_test_write(void) {
    /* Force a REAL, IMMEDIATE flash program (the point is to wedge the CPU
     * right after one). The pattern always keeps bit 7 set AND changes the
     * low bits, so every invocation writes a different, ALWAYS-invalid value
     * (> 101 guard above) -- repeated test runs cannot XOR back to validity.
     * The board runs on the compile-time default until the next settled
     * write of any field stores a real value again; the poke itself does not
     * schedule that heal. */
    kb_config.lcd_brightness_p1 = (uint8_t)(0x80 | ((kb_config.lcd_brightness_p1 + 1) & 0x3F));
    eeconfig_update_kb_datablock(&kb_config, 0, sizeof(kb_config));
}
#endif
