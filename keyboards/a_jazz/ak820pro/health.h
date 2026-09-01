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

/* Fill a raw-HID reply payload (28 bytes) with the current snapshot.
 * Layout, little-endian:
 *   u32 blit_timeouts, u32 tx_sent, u32 tx_timeouts, u32 tx_drops,
 *   u32 rx_malformed, u32 loop_gap_max_ms, u16 scan_rate,
 *   u8 wdt_consecutive_resets, u8 flags (bit0 wdt fired last boot,
 *   bit1 wdt degraded) */
void health_fill(uint8_t *out28);
