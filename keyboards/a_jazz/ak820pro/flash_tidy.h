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
 * So the board consolidates first, once the log is half full, at a moment
 * nobody is watching the keys:
 *   - dark: the committed LED frame all off (the ladder's dark stage, Lights
 *     out, the lights cut, the RGB toggled off) and no key or knob for 30 s;
 *   - lit: no key or knob for 10 min (JD: "Someone who hasn't typed for ten
 *     minutes is probably not looking closely at their keyboard"), but only
 *     where no dark stage is coming: on USB, or in Always on. On battery the
 *     other modes go dark (Balanced at 20 min, Power saver at 3, Lights out
 *     at once), and the pass that darkens before the deep sleep is a dark
 *     one: flash_tidy_task() runs just before sleep_task().
 * After a failure none more until a key or the knob; never with the menu
 * open, in a wake's recovery or on the battery's reserve. A full log still
 * consolidates where it fills, as before: now only after half a log of
 * writes without a pause (~30 settings changes, ~8 profile saves).
 *
 * ⚠️ Every consolidation, early or forced, erases the only copy before it
 * writes it back: a power loss inside its ~58 ms (a slider flip, the pack
 * dying) loses the settings and the keymap (flash.sh's backups restore
 * them). The early ones add none of their own kind of risk, but at most
 * double the count: at half full, never more than one per half log written
 * (codex: a quarter would allow four). The erases' wear is as small: 20,000
 * cycles at least (docs/sn32f299.md). */

/* Each pass, from housekeeping_task_kb(), just before sleep_task(). */
void flash_tidy_task(void);

/* HC_FLASHW page 4 (hid_protocol.c), 28 bytes: u16 the log's bytes in use,
 * u16 its size, u16 the threshold, u16 early cleanups since boot, u8 those
 * that failed, u8 the last's kind (1 dark, 2 lit, 0 none), the rest zero. */
#define FLASH_TIDY_PAGE 4u
void flash_tidy_fill(uint8_t *out28);
