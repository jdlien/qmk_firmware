// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
/* The emulated EEPROM's cleanup, early, where it can't be seen: flash_tidy.h. */
#include <string.h>
#include "quantum.h"
#include "wear_leveling.h"
#include "wear_leveling_efl_config.h"   /* WEAR_LEVELING_BACKING_SIZE, _LOGICAL_SIZE */
#include "flash_tidy.h"
#include "power.h"
#include "battery.h"
#include "sleep.h"
#include "graphics/display.h"
#include "drivers/led/sn32f2xx.h"

/* The log follows the consolidated copy and its 8-byte hash (wear_leveling.c). */
#define LOG_BYTES  ((WEAR_LEVELING_BACKING_SIZE) - (WEAR_LEVELING_LOGICAL_SIZE) - 8u)
#define TIDY_BYTES (LOG_BYTES / 4u)   /* a quarter: 254 of 1016 B, ~31 lines */
#define DARK_IDLE_MS 30000u
#define LIT_IDLE_MS  600000u

enum { TIDY_DARK = 1, TIDY_LIT = 2 };

static bool     tried;      /* a try since boot */
static uint32_t tried_at;   /* last_input_activity_time() at it: one a pause */
static uint16_t tidies, failures;
static uint32_t last_ms;
static uint8_t  last_kind;

void flash_tidy_task(void) {
    if (wear_leveling_log_used() < TIDY_BYTES) return;
    uint32_t idle = last_input_activity_elapsed();
    if (idle < DARK_IDLE_MS) return;
    uint8_t kind;
    if (sn32f2xx_led_committed_dark()) {
        kind = TIDY_DARK;
    } else if (idle >= LIT_IDLE_MS && (!battery_on_battery() || power_mode() == POWER_MODE_FULL)) {
        kind = TIDY_LIT;
    } else {
        return;
    }
    if (tried && last_input_activity_time() == tried_at) return;
    if (display_menu_sleep_blocked() || sleep_recovering() || power_lights_cut()) return;
    tried     = true;
    tried_at  = last_input_activity_time();
    last_kind = kind;
    last_ms   = timer_read32();
    tidies++;
    if (wear_leveling_consolidate() == WEAR_LEVELING_FAILED) failures++;
}

void flash_tidy_fill(uint8_t *out28) {
    uint8_t *p = out28;
#define PUT16(x) do { uint16_t v_ = (x); *p++ = v_ & 0xFF; *p++ = v_ >> 8; } while (0)
    memset(out28, 0, 28);
    PUT16(wear_leveling_log_used());
    PUT16(LOG_BYTES);
    PUT16(TIDY_BYTES);
    PUT16(tidies);
    PUT16(failures);
    PUT16(last_ms & 0xFFFFu);
    PUT16(last_ms >> 16);
    *p = last_kind;
#undef PUT16
}
