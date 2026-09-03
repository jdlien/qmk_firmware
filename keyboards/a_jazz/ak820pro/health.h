// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdint.h>

/* Unified health counters.
 *
 * The counters accumulate in RAM unconditionally (they cost a compare and an
 * increment); only the REPORTING differs by build flavor. Raw HID is the
 * primary channel -- it exists in every build, and console-only
 * observability disappears during exactly the stalls being investigated.
 * The console [health] line is the instrumented-flavor extra.
 *
 * Lessons this design carries (from the loop-gap investigation):
 * an instrument must not touch the subsystem under test -- reporting goes to
 * an inert sink, never the panel; and thresholds/quantisation sit well above
 * the noise floor so the output cannot become continuous traffic. */

/* Every housekeeping pass: worst main-loop gap since boot (max-hold, first
 * 10 s ignored -- boot blocks deliberately). */
void health_loop_tick(void);

/* 10 Hz: refreshes derived values; in instrumented builds prints the
 * [health] line when something meaningful changed. */
void health_task(void);

/* Phase-2 hook: count a malformed/unparseable inbound frame (CH582F). */
void health_note_rx_malformed(void);

/* Every internal-flash write passes backing_store_pre_write_hook(); count it.
 * An ORDINARY write is a single 8-byte line program -- tens of microseconds,
 * far too short to lose a keypress. The expensive event is wear-levelling
 * CONSOLIDATION (~every 127 entries: two 1 KB sector erases, synchronous and
 * unmasked, believed 50-300 ms). Counting writes here plus flash_gap_max_ms
 * below makes a consolidation visible without patching core wear levelling --
 * it shows up as a FLASH-marked gap an order of magnitude above the rest. */
void health_note_flash_write(void);

/* Every key PRESS (not release), counted on the daily build. Was
 * CONSOLE_ENABLE-only, which made it useless for the question it exists to
 * answer: firmware count == characters received means the matrix never saw
 * the press; firmware count > received means the report was lost downstream. */
void health_note_key_press(void);

/* The stall mark captured for the CURRENT pass. health_loop_tick() is the sole
 * owner of loop_stall_mark: it reads and clears it once per pass, so anything
 * else wanting attribution must read it from here or it will see NONE. */
uint8_t health_last_mark(void);

/* Clear the resettable counters. Watchdog counters are BOOT FACTS and are
 * deliberately NOT cleared -- a reset must not be able to erase the evidence
 * that the board reset itself. Without this every reading is contaminated by
 * boot and no experiment repeats. */
void health_reset(void);

/* Fill a raw-HID reply payload (28 bytes) with the current snapshot.
 * Layout, little-endian:
 *   u32 blit_timeouts, u32 tx_sent, u32 tx_timeouts, u32 tx_drops,
 *   u32 rx_malformed, u32 loop_gap_max_ms, u16 scan_rate,
 *   u8 wdt_consecutive_resets, u8 flags (bit0 wdt fired last boot,
 *   bit1 wdt degraded) */
void health_fill(uint8_t *out28);

/* Second page (28 bytes), little-endian:
 *   u32 count_ge_10ms   -- gaps >= 10 ms: latency events
 *   u32 count_ge_25ms   -- gaps >= 25 ms: THE KEYSTROKE-LOSING CLASS. A press
 *                          holds contact 25-80 ms, so a shorter stall ends with
 *                          the key still down and only delays it.
 *   u32 passes          -- denominator, to turn the counts into rates
 *   u32 flash_writes
 *   u16 flash_gap_max_ms -- worst gap attributed to internal flash
 *   u16 blit_gap_max_ms  -- worst gap attributed to the flash->LCD DMA wait
 *   u16 i2c_gap_max_ms   -- worst gap attributed to the bit-banged PCF8563
 *   u16 count_ge_25ms_nonflash -- THE GATE'S DISCRIMINATOR. A >=25 ms stall
 *                          marked FLASH is a wear-levelling consolidation:
 *                          understood, bounded, and expected whenever a test
 *                          writes enough entries. Anything else that long is
 *                          unexplained, and is what must never regress.
 *   u16 key_presses
 *   u8  loop_gap_max_mark -- what the overall worst gap was (LOOP_MARK_*)
 *   u8  reserved */
void health_fill2(uint8_t *out28);

/* Third page (28 bytes), little-endian. Exists because the SN32 driver
 * publishes ONE row per matrix_scan() call, so scan_rate is not a full-matrix
 * refresh rate and cannot answer "how often is this key actually looked at".
 *   u16 row_samples[6]  -- fairness across rows
 *   u16 row_gap_max_ms  -- worst interval between samples of the SAME row
 *   u32 raw_edges       -- raw transitions BEFORE debounce
 *   u32 consumes        -- matrix_scan_custom calls
 *   u32 cooked_changes  -- rows differing at consume time
 *   u8  row_gap_max_row
 *   u8  matrix_rows */
void health_fill3(uint8_t *out28);
