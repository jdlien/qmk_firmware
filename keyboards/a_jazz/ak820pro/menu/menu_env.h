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
