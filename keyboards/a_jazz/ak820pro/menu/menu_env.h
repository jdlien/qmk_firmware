// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// What menu_ops.c needs from the rest of the firmware, beyond the panel
// (menu_backend.h). menu_glue.c implements these on display.c and lcd_bus.c;
// the host tests implement them on their fake.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "panel_owner.h"

uint16_t env_po_event(po_event_t ev);   /* the event, with every action applied (display_po_event) */
void     env_lost_inject(void);         /* op 27 sub 8 */
bool     env_wake_notice_take(void);
bool     env_wake_held(void);
bool     env_batt_dirty(void);          /* op 27 sub 5's view of the dashboard */
bool     env_logo_stale(void);
uint8_t  env_queue_depths(void);
bool     env_probe_handoff(uint8_t *out4);   /* op 27 sub 9: false unless the dashboard is DONE */

/* P1a's settings and facts (plans/CONFIG-MENU-P1-DESIGN.md). */
uint8_t  env_power_mode(void);          /* 0 Always on, 1 Balanced, 2 Power saver, 3 Lights out */
void     env_power_set(uint8_t m);      /* power_set_mode() */
uint8_t  env_clock_mode(void);          /* enum display_clock_mode: 0 24H, 1 12H, 2 OFF, 3 DATE */
void     env_clock_set(uint8_t m);      /* display_set_clock_mode() + kb_eeconfig_set_clock_mode() */
void     env_health_reset(void);        /* health_reset() + display_debug_refresh() */

/* P1b's primitives (menu_edit.c's): each one QMK or board call in
 * menu_glue.c; the host's fakes copy their semantics. QMK's mode and HSV
 * setters return early while the lights are off (rgb_matrix.c:606, 658);
 * speed's, flags' and enable's don't. `save`: the saving setter, which flags
 * the config dirty; otherwise the _noeeprom one, which leaves the flag as it
 * was. */
typedef struct { uint8_t mode, h, s, v, speed, enable, flags; } env_rgb_t;
void     env_rgb_get(env_rgb_t *c);
void     env_rgb_mode(uint8_t mode, bool save);
void     env_rgb_hsv(uint8_t h, uint8_t s, uint8_t v, bool save);
void     env_rgb_speed(uint8_t speed, bool save);
void     env_rgb_flags(uint8_t flags);              /* _noeeprom only: restores a snapshot */
void     env_rgb_enable(bool on, bool save);        /* rgb_matrix_enable/disable, never a toggle */
uint8_t  env_lcd_level(void);                       /* display_get_brightness(): the requested level */
void     env_lcd_set(uint8_t level, bool save);     /* display_set_brightness(), and kb_eeconfig_set_lcd_brightness() to save */
uint8_t  env_effects(void);                         /* rgb_effects_n */
uint8_t  env_effect_mode(uint8_t i);
const char *env_effect_name(uint8_t i);
bool     env_effect_two_tone(uint8_t mode);         /* the mode uses speed as its second colour's hue offset (Alphas/Mods) */
/* The bootloader item: Fn+Esc's path (the splash, 1.5 s, reset_keyboard());
 * and whether the battery is in its reserve, when it refuses (flash.sh's
 * rule: power_lights_cut()). */
bool     env_bootloader(void);   /* false: a key's release couldn't be sent over the radio: not entered */
bool     env_reserve(void);
#define  ENV_UNKNOWN 0xFFFFu
typedef struct {
    uint32_t uptime_s;
    uint16_t batt_pm;     /* permille, ENV_UNKNOWN */
    uint16_t pack_mv;     /* ENV_UNKNOWN (battery_pack_mv()'s 0 normalized) */
    uint16_t left_min;    /* minutes of use left, ENV_UNKNOWN */
    uint8_t  mode;        /* env_power_mode() */
    uint8_t  link;        /* ENV_LINK_* packed: transport bits 1-0, slot bits 3-2 (BT 0-2), state bits 6-4 */
} env_about_t;
_Static_assert(sizeof(env_about_t) == 12, "About's snapshot: 12 B");
enum { ENV_LINK_USB = 0, ENV_LINK_BT = 1, ENV_LINK_24G = 2 };
enum { ENV_LINK_IDLE = 0, ENV_LINK_LINKING, ENV_LINK_PAIRING, ENV_LINK_CONNECTED, ENV_LINK_REJECTED };
void     env_about(env_about_t *a);
uint32_t env_build_hash(void);          /* AK820_BUILD_HASH: 8 hex digits */
bool     env_build_dirty(void);
uint32_t env_build_token(void);
const char *env_build_flavour(void);    /* dev, daily, fpb, instr, fault: at most 5 characters */

/* C10's measures that live outside the menu's scheduler (menu_meas.c and
 * display.c on the board). */
typedef struct {
    uint32_t pass_max_ticks;    /* the longest main-loop pass in the interval, any owner */
    uint16_t pass_over15;       /* passes over 15,000 us in the interval (saturating) */
    uint16_t passes;            /* passes in the interval (wrapping) */
    uint32_t band_max_ticks;    /* the restore's and the debug page's clear bands */
    uint16_t token;             /* the session token: 0 at boot */
    bool     aliased;           /* a pass of 340 ms or more in the interval: its ticks are ambiguous */
    uint8_t  health_gen;        /* moves at every health_reset() */
    uint8_t  page;              /* the debug page showing: 0 none, 1 Health, 2 Camera */
    uint8_t  dbg_gen, dbg_done_gen, dbg_sec;
    uint8_t  rtc_sec;           /* the RTC's current second, 0xFF unknown */
} env_meas_t;
void     env_meas(env_meas_t *m);
uint16_t env_meas_arm(void);         /* a new interval: the maxima and counts zeroed, a new token */
bool     env_debug_done(void);       /* sub 5 byte 31 bit 0 (C10) */
