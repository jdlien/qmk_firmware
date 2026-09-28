// Copyright 2026 quill4gen7
// SPDX-License-Identifier: GPL-2.0-or-later
/* Settings menu on the LCD, opened with Fn + a turn of the knob.
 *
 * It is a notification page (display.c) with its own keys: the knob or the
 * arrows move, a knob press, Enter or Space changes the item, Esc leaves, and
 * it closes by itself after MENU_IDLE_MS. Anything notify.c puts on the panel
 * takes it over. It does not open over a question waiting on the board. */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "action.h"

/* Key events, first thing in process_record_kb. True when consumed. */
bool menu_process_record(uint16_t keycode, keyrecord_t *record);

/* 10 Hz: the idle timeout, and noticing the page was closed under it. */
void menu_task(void);

/* notify.c took the panel: the menu is gone. */
void menu_lost(void);
