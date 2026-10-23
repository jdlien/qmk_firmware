// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The menu's pure modules wired into the firmware: the knob's hook, the task
// each pass, the menu's paint from the pump, and op 27 (dev builds).
// plans/CONFIG-MENU-BUILD-PLAN.md.
#pragma once

#include "quantum.h"
#include "panel_owner.h"

/* ak820pro.c */
bool menu_pre_process(uint16_t keycode, keyrecord_t *record);   /* false: captured */
void menu_task(void);                                            /* each pass, before the pump */

/* graphics/display.c calls these from po_apply() and the pump. */
void menu_screen_open(bool from_debug);
void menu_screen_invalidate(void);
void menu_pump(void);
void menu_input_clear(void);
void menu_loss_dropped(void);   /* an ownership change dropped a latched loss */
void menu_note_loss(void);      /* a loss was taken (op 27's count) */

/* graphics/display.c: an event's actions, applied. */
uint16_t display_po_event(po_event_t ev);

#ifdef AK820_DEV_HOOKS
/* HC_DEVHOOK op 27: the whole 32-byte report, in place. False: unhandled. */
bool menu_op27(uint8_t *data);
#endif
