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

/* C10's measures that live outside the menu's scheduler (menu_meas.c and
 * display.c on the board). */
typedef struct {
    uint32_t pass_max_ticks;    /* the longest main-loop pass in the interval, any owner */
    uint16_t pass_over15;       /* passes over 15,000 us in the interval (saturating) */
    uint16_t passes;            /* passes in the interval (wrapping) */
    uint32_t band_max_ticks;    /* the restore's and the debug page's clear bands */
    uint16_t token;             /* the session token: 0 at boot */
    uint8_t  health_gen;        /* moves at every health_reset() */
    uint8_t  page;              /* the debug page showing: 0 none, 1 Health, 2 Camera */
    uint8_t  dbg_gen, dbg_done_gen, dbg_sec;
    uint8_t  rtc_sec;           /* the RTC's current second, 0xFF unknown */
} env_meas_t;
void     env_meas(env_meas_t *m);
uint16_t env_meas_arm(void);         /* a new interval: the maxima and counts zeroed, a new token */
bool     env_debug_done(void);       /* sub 5 byte 31 bit 0 (C10) */
