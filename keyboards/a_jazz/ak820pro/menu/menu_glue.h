// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The menu wired into the firmware: the knob's hook, the task each pass, and
// op 27 (dev builds). The logic is menu_ops.c's; this file is the firmware's
// side of menu_backend.h and menu_env.h. plans/CONFIG-MENU-BUILD-PLAN.md.
#pragma once

#include "quantum.h"
#include "menu_ops.h"
#include "panel_owner.h"

/* ak820pro.c */
bool menu_pre_process(uint16_t keycode, keyrecord_t *record);   /* false: captured */
void menu_pass_begin(void);                                      /* first in each pass: C10's pass timing */
void menu_task(void);                                            /* each pass, before the pump */
void menu_prof_key(bool next);                                   /* P5b: PROF_NXT and PROF_PRV, on the press */

/* graphics/display.c: an event's actions, applied. */
uint16_t display_po_event(po_event_t ev);

#ifdef AK820_DEV_HOOKS
/* HC_DEVHOOK op 27: the whole 32-byte report, in place. False: unhandled. */
bool menu_op27(uint8_t *data);
#endif
