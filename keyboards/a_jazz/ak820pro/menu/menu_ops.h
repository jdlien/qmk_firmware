// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The menu's per-pass task, op 27 and the menu's side of an owner event:
// pure logic over the four modules and menu_env.h, so the host tests
// (scripts/menu_s0_test.c) run it as the firmware does. menu_glue.c wires it
// in. plans/CONFIG-MENU-BUILD-PLAN.md.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "menu_input.h"
#include "panel_owner.h"

/* An owner event's menu-side actions, after the display's own: the input
 * cleared, the screen opened, and a loss dropped at an ownership change
 * ending op 27's sub 8. */
void menu_ops_apply(uint16_t acts, bool loss_dropped, uint32_t now);

/* A physical knob event (menu_glue.c has its identity and Fn). False: captured. */
bool menu_physical(mi_control_t c, bool pressed, bool fn, uint32_t now);
bool menu_enabled(void);

void menu_ops_task(uint32_t now);  /* each pass, before the pump */
void menu_pump(void);              /* the MENU owner's one bounded operation */
void menu_screen_invalidate(void); /* a loss under MENU */
void menu_note_loss(void);         /* a loss was taken (op 27's count) */

/* HC_DEVHOOK op 27: the whole 32-byte report, in place. False: unhandled.
 * Built with AK820_DEV_HOOKS (and by the host tests). */
bool menu_ops_op27(uint8_t *data, uint32_t now);
