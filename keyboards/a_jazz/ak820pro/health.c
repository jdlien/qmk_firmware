// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
/* Unified health counters -- see health.h for the design rules. */
#include "quantum.h"
#include "health.h"
#include "watchdog.h"
#include "graphics/lcd_bus.h"
#include "bluetooth/ch582f_ajazz.h"

/* Worst main-loop gap since boot, max-hold. Measured unconditionally: one
 * timer read and a compare per pass. The first 10 s are ignored -- boot
 * blocks deliberately (lcd_init alone spends 240 ms in wait_ms), and a
 * max-hold seeded by boot would never report the steady state again. */
#define HEALTH_SETTLE_MS 10000u

static uint32_t loop_gap_max = 0;
static uint32_t rx_malformed = 0;

void health_loop_tick(void) {
    static uint32_t last = 0;
    uint32_t now = timer_read32();
    if (now < HEALTH_SETTLE_MS) {
        last = now;
        return;
    }
    uint32_t gap = now - last;
    last = now;
    if (gap > loop_gap_max) loop_gap_max = gap;
}

void health_note_rx_malformed(void) {
    rx_malformed++;
}

void health_fill(uint8_t *out28) {
    uint32_t sent, to, drop;
    ch582_tx_stats(&sent, &to, &drop);

    uint32_t v;
    uint8_t *p = out28;
#define PUT32(x) do { v = (x); *p++ = v & 0xFF; *p++ = (v >> 8) & 0xFF; *p++ = (v >> 16) & 0xFF; *p++ = (v >> 24) & 0xFF; } while (0)
    PUT32(lcd_blit_timeouts());
    PUT32(sent);
    PUT32(to);
    PUT32(drop);
    PUT32(rx_malformed);
    PUT32(loop_gap_max);
#undef PUT32
#ifdef DEBUG_MATRIX_SCAN_RATE
    uint16_t sr = (uint16_t)get_matrix_scan_rate();
#else
    uint16_t sr = 0;
#endif
    *p++ = sr & 0xFF;
    *p++ = (sr >> 8) & 0xFF;
    *p++ = watchdog_reset_count();
    *p++ = (watchdog_fired_last_boot() ? 1 : 0) | (watchdog_degraded() ? 2 : 0);
}

void health_task(void) {
#ifdef CONSOLE_ENABLE
    /* On-meaningful-change only. Scan rate is quantised into 25 Hz bands and
     * the loop gap is already a max-hold, so a healthy board prints this a
     * handful of times after boot and then goes quiet -- console traffic
     * itself perturbs the host (2026-08-30 incident). */
    static uint32_t s_blit = 0, s_to = 0, s_drop = 0, s_malf = 0, s_gap = 0;
    static uint16_t s_band = 0;
    static bool     primed = false;

    uint32_t sent, to, drop;
    ch582_tx_stats(&sent, &to, &drop);
    uint32_t blit = lcd_blit_timeouts();
#ifdef DEBUG_MATRIX_SCAN_RATE
    uint16_t band = (uint16_t)(get_matrix_scan_rate() / 25);
#else
    uint16_t band = 0;
#endif

    bool changed = blit != s_blit || to != s_to || drop != s_drop ||
                   rx_malformed != s_malf || loop_gap_max != s_gap ||
                   band != s_band;
    if (!primed) {
        /* Skip the boot transient; report only changes after settle. */
        if (timer_read32() < HEALTH_SETTLE_MS) return;
        primed = true;
        changed = true; /* one baseline line so the log shows the healthy state */
    }
    if (!changed) return;

    s_blit = blit; s_to = to; s_drop = drop;
    s_malf = rx_malformed; s_gap = loop_gap_max; s_band = band;

    printf("[health] blit_to=%lu tx=%lu to=%lu drop=%lu malf=%lu gap=%lums scan~%u wdt=%u%s%s\n",
           blit, sent, to, drop, rx_malformed, loop_gap_max,
           (unsigned)(band * 25), watchdog_reset_count(),
           watchdog_fired_last_boot() ? " FIRED" : "",
           watchdog_degraded() ? " DEGRADED" : "");
#endif
}
