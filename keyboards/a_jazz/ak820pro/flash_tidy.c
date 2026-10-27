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
#define TIDY_BYTES (LOG_BYTES / 2u)   /* half: 508 of 1016 B, ~63 lines */
#define DARK_IDLE_MS 30000u
#define LIT_IDLE_MS  600000u

enum { TIDY_DARK = 1, TIDY_LIT = 2 };

/* Small: the build's heap gap is at its floor (C5). */
static struct {
    uint16_t tidies;     /* since boot */
    uint8_t  failures;
    uint8_t  kind;       /* the last's, TIDY_DARK or TIDY_LIT; 0 none tried */
    bool     blocked;    /* one failed: none more until a key or the knob is seen */
} t;

void flash_tidy_task(void) {
    if (t.blocked) {
        /* After a failure, none more until input is seen: a new pause. Read
         * before the threshold, which a failed program passes empty (the
         * store resets its log whatever the program did: codex); a stored
         * activity time would alias where this can't. Its timer read is
         * paid only while blocked. */
        if (last_input_activity_elapsed() >= DARK_IDLE_MS) return;
        t.blocked = false;
    }
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
    if (display_menu_sleep_blocked() || sleep_recovering() || power_lights_cut()) return;
    t.kind = kind;
    t.tidies++;
    if (wear_leveling_consolidate() == WEAR_LEVELING_FAILED) {
        t.blocked = true;
        if (t.failures < 0xFFu) t.failures++;
    }
}

void flash_tidy_fill(uint8_t *out28) {
    uint8_t *p = out28;
#define PUT16(x) do { uint16_t v_ = (x); *p++ = v_ & 0xFF; *p++ = v_ >> 8; } while (0)
    memset(out28, 0, 28);
    PUT16(wear_leveling_log_used());
    PUT16(LOG_BYTES);
    PUT16(TIDY_BYTES);
    PUT16(t.tidies);
    *p++ = t.failures;
    *p   = t.kind;
#undef PUT16
}
