// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* The emulated EEPROM's cleanup, early, where it can't be seen (2026-10-27;
 * plans/BACKLOG.md, "The LED blink at an internal-flash write").
 *
 * Every setting the board saves appends to the wear-levelling write log, and
 * when the log fills the write that overflowed it consolidates: the 2 KB
 * backing store erased and the 1 KB cache written back, ~58 ms in one pass.
 * That is the blink JD sees ("the board flickers once in a while"; on the
 * dev Write test page, One write and Ten writes show nothing, Cleanup is "a
 * very noticeable flicker"), and it comes mid-use, at whatever save filled
 * the log.
 *
 * So the board consolidates first, once the log is a quarter full, at a
 * moment nobody is watching the keys:
 *   - dark: the committed LED frame all off (the ladder's dark stage, Lights
 *     out, the lights cut, the RGB toggled off) and no key or knob for 30 s;
 *   - lit: no key or knob for 10 min (JD: "Someone who hasn't typed for ten
 *     minutes is probably not looking closely at their keyboard"), but only
 *     where no dark stage is coming: on USB, or in Always on. On battery the
 *     other modes go dark (Balanced at 20 min, Power saver at 3, Lights out
 *     at once), and the pass that darkens before the deep sleep is a dark
 *     one: flash_tidy_task() runs just before sleep_task().
 * One try a pause (a failure waits for the next key), never with the menu
 * open, in a wake's recovery or on the battery's reserve. A full log still
 * consolidates where it fills, as before; the early one leaves it ~3/4 of a
 * log of headroom (~45 settings changes, ~12 profile saves) for the next
 * stretch of use. */

/* Each pass, from housekeeping_task_kb(), just before sleep_task(). */
void flash_tidy_task(void);

/* HC_FLASHW page 4 (hid_protocol.c), 28 bytes: u16 the log's bytes in use,
 * u16 its size, u16 the threshold, u16 early cleanups since boot, u8 those
 * that failed, u8 the last's kind (1 dark, 2 lit, 0 none), the rest zero. */
#define FLASH_TIDY_PAGE 4u
void flash_tidy_fill(uint8_t *out28);
