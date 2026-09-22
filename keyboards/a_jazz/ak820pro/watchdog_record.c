// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
#include "watchdog_record.h"
#include <stdbool.h>
#include <string.h>

/* Exactly the existing 16-byte NOLOAD ram7 region. No linker/DFU changes.
 * New magic versions the layout: the old image used only the first 8 bytes. */
typedef struct {
    uint32_t magic, count, path, pass_uptime_ms;
} watchdog_retained_t;
_Static_assert(sizeof(watchdog_retained_t) == 16, "watchdog ram7 budget");
#ifndef WATCHDOG_RECORD_TEST
static volatile watchdog_retained_t retained __attribute__((section(".ram7"), aligned(4)));
#else
static volatile watchdog_retained_t retained;
#endif
#define RECORD_MAGIC 0x4A445722u
#define RST_WDT (1u << 1)
#define RST_POWER ((1u << 2) | (1u << 4))
static bool ready;
static uint8_t boot_record[28];

/* One aligned atomic word: low byte current scope, next byte parent;
 * upper half complements the lower half to reject corrupt/stale contents.
 * Atomicity is per path, not across path and uptime: uptime is explicitly
 * the last housekeeping ENTRY, not the time of entry to this operation. */
static uint32_t encode_path(uint16_t path) {
    return (uint32_t)path | ((uint32_t)(uint16_t)~path << 16);
}

uint8_t watchdog_record_boot(uint8_t rstst) {
    ready = false;
    bool warm = retained.magic == RECORD_MAGIC && !(rstst & RST_POWER);
    uint32_t path = retained.path;
    bool valid = warm && (rstst & RST_WDT) &&
                 (uint16_t)(path >> 16) == (uint16_t)~path &&
                 (uint8_t)path != WDT_SITE_NONE;
    uint32_t count = warm ? retained.count : 0;
    if (count > 255) count = 0; /* corrupt count cannot wrap into a reset loop */
    count = (rstst & RST_WDT) ? (count < 255 ? count + 1 : 255) : 0;
    memset(boot_record, 0, sizeof(boot_record));
    boot_record[0] = 1;
    boot_record[1] = valid;
    if (valid) {
        boot_record[2] = path;
        boot_record[3] = path >> 8;
        uint32_t uptime = retained.pass_uptime_ms;
        for (unsigned i = 0; i < 4; ++i) boot_record[4 + i] = uptime >> (8 * i);
    }
    boot_record[8] = rstst;
    boot_record[9] = count;
    retained.magic = 0;
    retained.count = count;
    retained.path = encode_path(WDT_SITE_MAIN_LOOP);
    retained.pass_uptime_ms = 0;
    retained.magic = RECORD_MAGIC; /* commit the new layout last */
    ready = true;
    return count;
}

uint32_t watchdog_record_enter(uint8_t site) {
    if (!ready) return 0; /* boot init must not clobber the previous boot */
    uint32_t previous = retained.path;
    retained.path = encode_path(((uint16_t)(uint8_t)previous << 8) | site);
    return previous;
}

void watchdog_record_leave(uint32_t *previous) {
    if (ready && *previous) retained.path = *previous;
}

void watchdog_record_uptime(uint32_t pass_uptime_ms) {
    if (ready) retained.pass_uptime_ms = pass_uptime_ms;
}

void watchdog_record_fill(uint8_t *out28) {
    memcpy(out28, boot_record, sizeof(boot_record));
}
