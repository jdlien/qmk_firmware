// Copyright 2026 Fernando Birra
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Wall date/time in plain decimal (human-natural: month 1-12, year e.g. 2026). */
typedef struct {
    uint8_t  seconds;  // 0-59
    uint8_t  minutes;  // 0-59
    uint8_t  hours;    // 0-23
    uint8_t  day;      // 1-31
    uint8_t  weekday;  // 0-6
    uint8_t  month;    // 1-12
    uint16_t year;     // e.g. 2026
} rtc_time_t;

/* The clock behind this API is two physical devices that the module hides: an
 * external battery-backed PCF8563 (accurate reference) and the SN32's internal
 * RTC (the live 1 Hz clock the display reads), seeded from and -- with
 * RTC_AUTO_CALIBRATION -- continuously disciplined to the PCF8563. Consumers just
 * ask for the time. */

/* Bring up the clock: start I2C, enable the SN32 1 Hz counter, and take a first
 * seed from the PCF8563. Call once from keyboard_post_init_kb (thread context).
 * If the PCF isn't readable/set yet, rtc_task() keeps retrying. */
void rtc_init(void);

/* Read the current wall time. Returns false until the clock has been seeded from
 * a valid PCF8563 (caller should show an "unset" placeholder in that case). */
bool rtc_get_time(rtc_time_t *out);

/* Set the time: persist to the PCF8563 AND set the running SN32 clock (so the
 * display updates immediately). Returns false if the PCF write failed (i.e. the
 * time was applied but not persisted). Blocking I2C -- call from thread context. */
bool rtc_set_time(const rtc_time_t *t);

/* Free-running count of RTC second interrupts (~seconds since rtc_init) -- cheap,
 * no calendar conversion. For once-per-second edge detection (e.g. pacing the
 * display refresh). NOT wall time; use rtc_get_time() for that. */
uint32_t rtc_get_seconds(void);
/* Live SN32 divider period (trimmed register value). */
uint32_t rtc_get_period(void);

/* Per-housekeeping tick: retries the initial seed until it takes and runs the
 * auto-calibration pass when one is due. Cheap no-op otherwise. Call every
 * housekeeping pass from thread context. */
void rtc_task(void);

/* How many PCF8563 transactions would have collided with a live flash->LCD DMA.
 * See the bus-guard note in rtc.c: the bit-banged I2C shares port A with the
 * flash SPI1 pins, so an overlap glitches the transfer. Nonzero means the
 * hazard is real on this unit. */
uint16_t rtc_i2c_overlaps(void);
