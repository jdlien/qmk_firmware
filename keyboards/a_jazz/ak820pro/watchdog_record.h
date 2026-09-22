// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>

/* Stable wire IDs; append only. Main-loop context ONLY: ISR writers would
 * overwrite the operation they interrupted and require synchronization. */
enum watchdog_site {
    WDT_SITE_NONE = 0, WDT_SITE_MAIN_LOOP = 1, WDT_SITE_HOUSEKEEPING = 2,
    WDT_SITE_KEY_EVENT = 3, WDT_SITE_WIRELESS = 4, WDT_SITE_RAW_HID = 5,
    WDT_SITE_PARAM_REPEAT = 6, WDT_SITE_RTC_FAST = 7, WDT_SITE_SECOND_EDGE = 8,
    WDT_SITE_DISPLAY_PUMP = 9, WDT_SITE_DISPLAY_HK = 10, WDT_SITE_USER_HK = 11,
    WDT_SITE_LCD_WAIT = 12, WDT_SITE_LCD_TRANSFER = 13,
    WDT_SITE_FLASH_READ = 14, WDT_SITE_FLASH_ERASE = 15,
    WDT_SITE_FLASH_PROGRAM = 16, WDT_SITE_I2C_READ = 17, WDT_SITE_I2C_WRITE = 18,
    WDT_SITE_EFL_WRITE = 19, WDT_SITE_EFL_ERASE = 20, WDT_SITE_EFL_DRAIN = 21,
    WDT_SITE_TEST_STALL = 22,
    /* 32 + enum loop_site in ak820pro.h; keep that enum append-only too. */
    WDT_SITE_LOOP_BASE = 32
};

/* Capture BEFORE instrumented operations start. Returns consecutive resets.
 * A WDT-only warm reset may trust RAM; POR/LVD invalidate it even if magic
 * happens to survive. Old firmware's magic is deliberately not accepted. */
uint8_t watchdog_record_boot(uint8_t rstst);
void watchdog_record_uptime(uint32_t pass_uptime_ms);
uint32_t watchdog_record_enter(uint8_t site);
void watchdog_record_leave(uint32_t *previous);
/* GCC cleanup restores the parent on EVERY return, including early returns.
 * No timers, allocation, I/O or interrupt masking on these paths. */
#define WDT_SCOPE(site) uint32_t wdt_saved_scope __attribute__((cleanup(watchdog_record_leave), unused)) = watchdog_record_enter(site)
#define WDT_CALL(site, call) do { WDT_SCOPE(site); call; } while (0)

/* HC_GET5, protocol >=6, 28 bytes, little endian:
 * u8 format (=1), flags (bit0 valid), site, parent;
 * u32 last_pass_uptime_ms; u8 boot_rstst, consecutive_resets; 18 zero bytes.
 * Invalid records have zero site/parent/uptime. Boot facts remain available.
 * Frozen for this boot; health_reset() does not touch it. */
void watchdog_record_fill(uint8_t *out28);
