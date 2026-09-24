// Copyright 2026 quill4gen7
// SPDX-License-Identifier: GPL-2.0-or-later
/* Host notifications: a lighting effect plus either two lines of text in the
 * dashboard's host-text band, or a full-screen page (a GIF from flash, or
 * word-wrapped text) that stays up until a key is pressed.
 *
 * Two transports carry the same frame:
 *   - raw HID channel 0x14 (cable attached, any slider position);
 *   - the host LED report, which is the only host->keyboard path the CH582F
 *     forwards over BT/2.4G. Num Lock and Scroll Lock are the data lines (this
 *     board has neither key); Caps Lock is never touched. See notify.c. */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Feed every host LED bitmap change (led_t.raw) to the LED-channel decoder. */
void notify_leds_changed(uint8_t raw);

/* 10 Hz: closes LED-channel frames on the inter-frame gap, ends effects. */
void notify_task(void);

/* Decode and run one frame (the same bytes either transport carries, CRC
 * included). Returns false if the frame is malformed. */
bool notify_frame(const uint8_t *buf, uint8_t len);

/* Key events, first thing in process_record_kb. Returns true when the event
 * was consumed (it dismissed a notification page, or is that key's release). */
#include "action.h"
bool notify_process_record(keyrecord_t *record);

/* LED-channel counters for the raw-HID STATS reply (NOTIFY_STATS_LEN bytes). */
#define NOTIFY_STATS_LEN 20
void notify_stats_fill(uint8_t *out);
