// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
#include "watchdog_record.h"
#include <stdbool.h>
#include <string.h>

/* Exactly the existing 16-byte NOLOAD ram7 region. No linker/DFU changes.
 * The magic versions the layout: the old image used only the first 8 bytes.
 *
 * Two layouts share the four words:
 *   RECORD_MAGIC  count = consecutive resets; last word = pass uptime (ms).
 *   FAULT_MAGIC   count = consecutive resets | exception number << 8, upper
 *                 half zero; last word = the PC. Written once, by a terminal
 *                 site (fault.c), and never by a scope. */
typedef struct {
    uint32_t magic, count, path, pass_uptime_ms;
} watchdog_retained_t;
_Static_assert(sizeof(watchdog_retained_t) == 16, "watchdog ram7 budget");
#ifndef WATCHDOG_RECORD_TEST
static volatile watchdog_retained_t retained __attribute__((section(".ram7"), aligned(4)));
extern uint32_t __ram0_base__, __ram0_end__;
#define FRAME_LO ((uintptr_t)&__ram0_base__)
#define FRAME_HI ((uintptr_t)&__ram0_end__)
#else
static volatile watchdog_retained_t retained;
static uintptr_t test_frame_lo, test_frame_hi;
#define FRAME_LO test_frame_lo
#define FRAME_HI test_frame_hi
#endif
#define RECORD_MAGIC 0x4A445722u
#define FAULT_MAGIC  0x4A445746u
#define RST_WDT (1u << 1)
#define RST_POWER ((1u << 2) | (1u << 4))
static bool ready;
static uint8_t boot_record[28];
static volatile uint32_t write_drain; /* commit_terminal: never read */

/* One aligned atomic word: low byte current scope, next byte parent;
 * upper half complements the lower half to reject corrupt/stale contents.
 * Atomicity is per path, not across path and uptime: uptime is explicitly
 * the last housekeeping ENTRY, not the time of entry to this operation. */
static uint32_t encode_path(uint16_t path) {
    return (uint32_t)path | ((uint32_t)(uint16_t)~path << 16);
}

static bool path_ok(uint32_t path) {
    return (uint16_t)(path >> 16) == (uint16_t)~path;
}

static void put32(uint8_t *out, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) out[i] = v >> (8 * i);
}

uint8_t watchdog_record_boot(uint8_t rstst) {
    ready = false;
#if defined(WDT_TEST_HOOKS) && !defined(WATCHDOG_RECORD_TEST)
    WDT_BOOT_RAW[0] = retained.magic; WDT_BOOT_RAW[1] = retained.count;
    WDT_BOOT_RAW[2] = retained.path;  WDT_BOOT_RAW[3] = retained.pass_uptime_ms;
#endif
    uint32_t magic = retained.magic;
    bool terminal = magic == FAULT_MAGIC;
    bool warm = (magic == RECORD_MAGIC || terminal) && !(rstst & RST_POWER);
    uint32_t path = retained.path;
    uint32_t raw = retained.count;
    /* Each layout validates its own count word: a plain count cannot exceed
     * 255, and a packed one has nothing above the exception byte. A corrupt
     * count cannot wrap into a reset loop, and a terminal record's exception
     * byte is never read as a count. */
    bool count_ok = terminal ? (raw >> 16) == 0 : raw <= 255;
    uint32_t count = (warm && count_ok) ? (raw & 0xFFu) : 0;
    bool valid = warm && count_ok && (rstst & RST_WDT) && path_ok(path) &&
                 (uint8_t)path != WDT_SITE_NONE;
    count = (rstst & RST_WDT) ? (count < 255 ? count + 1 : 255) : 0;
    memset(boot_record, 0, sizeof(boot_record));
    boot_record[0] = (valid && terminal) ? 2 : 1;
    boot_record[1] = valid;
    if (valid) {
        boot_record[2] = path;
        boot_record[3] = path >> 8;
        if (terminal) {
            put32(&boot_record[10], retained.pass_uptime_ms);   /* the PC */
            boot_record[14] = raw >> 8;                          /* exception */
        } else {
            put32(&boot_record[4], retained.pass_uptime_ms);
        }
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
    if (!ready) return;
    retained.pass_uptime_ms = pass_uptime_ms;
    if (pass_uptime_ms >= WATCHDOG_COUNT_AGE_OUT_MS && retained.count) retained.count = 0;
}

void watchdog_record_fill(uint8_t *out28) {
    memcpy(out28, boot_record, sizeof(boot_record));
}

/* Runs on fault.c's private stack with the core stopped in a handler, so:
 * no calls, no locks, nothing that could fault. The magic is cleared FIRST
 * and set LAST, the order boot uses: a reset part-way leaves no magic and so
 * no record -- never a PC under the ordinary magic, where it would read as an
 * uptime. The first terminal record wins; nothing may overwrite it. */
static void commit_terminal(uint8_t site, uint8_t exception, uint32_t pc) {
    if (!ready) return;
    ready = false;
    uint32_t path = retained.path;
    uint8_t parent = path_ok(path) ? (uint8_t)path : WDT_SITE_NONE;
    uint32_t count = retained.count & 0xFFu;
    retained.magic = 0;
    retained.count = count | ((uint32_t)exception << 8);
    retained.path = encode_path((uint16_t)(((uint16_t)parent << 8) | site));
    retained.pass_uptime_ms = pc;
    retained.magic = FAULT_MAGIC;
    /* One sacrificial write, and it must stay. On this part the LAST SRAM
     * write before a loop that never writes again does not survive the
     * watchdog reset: it waits in a posted-write stage that only a later
     * WRITE drains. Reads do not drain it, and neither does DSB. Every
     * caller spins or locks up right after this, so without this line the
     * magic alone was missing at boot and every terminal record came back
     * invalid -- measured 2026-09-23 on the board, DSB tried and failed
     * the same way. Upstream's sn32_dfu.c waits 1 us "for memory to be
     * set" before its reset; likely the same property. */
    write_drain = FAULT_MAGIC;
}

void watchdog_record_fault_frame(uintptr_t frame_addr) {
    uint32_t pc = 0xFFFFFFFFu;
    uint8_t exception = 0xFF;
    /* The eight-word frame must lie wholly in SRAM and be word-aligned before
     * anything is loaded from it. This bounds the loads; it cannot vouch for
     * what the frame contains. */
    if ((frame_addr & 3u) == 0 && frame_addr >= FRAME_LO && FRAME_HI >= 32u &&
        frame_addr <= FRAME_HI - 32u) {
        const volatile uint32_t *frame = (const volatile uint32_t *)frame_addr;
        pc = frame[6];
        exception = (uint8_t)(frame[7] & 0x3Fu);
    }
    commit_terminal(WDT_SITE_HARD_FAULT, exception, pc);
}

void watchdog_record_stop(uint8_t site, uint8_t exception, uint32_t pc) {
    commit_terminal(site, exception, pc);
}
