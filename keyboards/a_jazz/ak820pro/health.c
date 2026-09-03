// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
/* Unified health counters -- see health.h for the design rules. */
#include "quantum.h"
#include "health.h"
#include "ak820pro.h"   /* LOOP_MARK_*, loop_stall_mark */
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

/* Phase-1 stall measurement. Counters, not a histogram: the question is "how
 * often does a keystroke-eating stall happen, and what was it", which buckets
 * answer worse than thresholds do. All of this is a compare and an increment
 * on a timer read that already happens -- no new timer read per pass. */
static uint32_t count_ge_10ms = 0, count_ge_25ms = 0, passes = 0;
static uint32_t flash_writes = 0;
/* Per-mark maxima are u16: a gap needing 65 s would be a hang, not a stall,
 * and the narrower fields buy room on a page that was exactly full. */
static uint16_t flash_gap_max = 0, blit_gap_max = 0, i2c_gap_max = 0;
/* The gate's discriminator. A >=25 ms stall attributed to FLASH is a
 * wear-levelling consolidation: understood, bounded, and expected whenever a
 * test writes enough entries. Anything else at that length is a stall we
 * cannot explain, and that is what must never regress. */
static uint16_t count_ge_25ms_nonflash = 0;
static uint16_t key_presses = 0;
static uint8_t  loop_gap_max_mark = LOOP_MARK_NONE;
static uint8_t  last_mark = LOOP_MARK_NONE;

void health_loop_tick(void) {
    static uint32_t last = 0;
    uint32_t now = timer_read32();

    /* Sole owner of loop_stall_mark: read once, clear once, publish via
     * health_last_mark(). This runs BEFORE loop_gap_task() in the same pass,
     * so a second reader of loop_stall_mark would always see NONE. */
    last_mark = loop_stall_mark;
    loop_stall_mark = LOOP_MARK_NONE;

    if (now < HEALTH_SETTLE_MS) {
        last = now;
        return;
    }
    uint32_t gap = now - last;
    last = now;
    passes++;
    if (gap >= 10) count_ge_10ms++;
    if (gap >= 25) count_ge_25ms++;
    if (gap > loop_gap_max) { loop_gap_max = gap; loop_gap_max_mark = last_mark; }
    uint16_t g16 = (gap > 0xFFFFu) ? 0xFFFFu : (uint16_t)gap;
    switch (last_mark) {
        case LOOP_MARK_FLASH: if (g16 > flash_gap_max) flash_gap_max = g16; break;
        case LOOP_MARK_BLIT:  if (g16 > blit_gap_max)  blit_gap_max  = g16; break;
        case LOOP_MARK_I2C:   if (g16 > i2c_gap_max)   i2c_gap_max   = g16; break;
        default: break;
    }
    if (gap >= 25 && last_mark != LOOP_MARK_FLASH) count_ge_25ms_nonflash++;
}

uint8_t health_last_mark(void) { return last_mark; }

void health_note_flash_write(void) { flash_writes++; }
void health_note_key_press(void)   { key_presses++; }

void health_reset(void) {
    /* Watchdog counters are boot facts and are NOT cleared -- see health.h. */
    chSysLock();
    loop_gap_max = 0; loop_gap_max_mark = LOOP_MARK_NONE;
    count_ge_10ms = 0; count_ge_25ms = 0; passes = 0;
    flash_writes = 0; flash_gap_max = 0; blit_gap_max = 0; i2c_gap_max = 0;
    count_ge_25ms_nonflash = 0; key_presses = 0; rx_malformed = 0;
    chSysUnlock();
}

void health_fill2(uint8_t *out28) {
    uint32_t c10, c25, ps, fw;
    uint16_t fg, bg, ig, nf, kp;
    uint8_t  mk;
    chSysLock();
    c10 = count_ge_10ms; c25 = count_ge_25ms; ps = passes; fw = flash_writes;
    fg  = flash_gap_max; bg  = blit_gap_max;  ig = i2c_gap_max;
    nf  = count_ge_25ms_nonflash; kp = key_presses; mk = loop_gap_max_mark;
    chSysUnlock();

    uint32_t v;
    uint8_t *p = out28;
#define PUT32(x) do { v = (x); *p++ = v & 0xFF; *p++ = (v >> 8) & 0xFF; *p++ = (v >> 16) & 0xFF; *p++ = (v >> 24) & 0xFF; } while (0)
#define PUT16(x) do { v = (x); *p++ = v & 0xFF; *p++ = (v >> 8) & 0xFF; } while (0)
    PUT32(c10); PUT32(c25); PUT32(ps); PUT32(fw);          /* 16 */
    PUT16(fg);  PUT16(bg);  PUT16(ig);                      /* 22 */
    PUT16(nf);  PUT16(kp);                                  /* 26 */
#undef PUT16
#undef PUT32
    *p++ = mk;
    *p++ = 0;   /* reserved */
}

void health_note_rx_malformed(void) {
    rx_malformed++;
}

void health_fill(uint8_t *out28) {
    /* Snapshot everything inside one short critical section so the reply is
     * one instant, not a mix. Today every source is main-loop-owned and the
     * mask is technically free insurance -- but rx_malformed is documented
     * as a future parser hook and scan rate lives in platform code, so the
     * atomicity is part of the contract, not an optimisation. */
    uint32_t sent, to, drop, blit, gap, malf;
    chSysLock();
    ch582_tx_stats(&sent, &to, &drop);
    blit = lcd_blit_timeouts();
    gap  = loop_gap_max;
    malf = rx_malformed;
#ifdef DEBUG_MATRIX_SCAN_RATE
    uint16_t sr = (uint16_t)get_matrix_scan_rate();
#else
    uint16_t sr = 0;
#endif
    chSysUnlock();

    uint32_t v;
    uint8_t *p = out28;
#define PUT32(x) do { v = (x); *p++ = v & 0xFF; *p++ = (v >> 8) & 0xFF; *p++ = (v >> 16) & 0xFF; *p++ = (v >> 24) & 0xFF; } while (0)
    PUT32(blit);
    PUT32(sent);
    PUT32(to);
    PUT32(drop);
    PUT32(malf);
    PUT32(gap);
#undef PUT32
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
    static uint8_t  s_wdt = 0;
    static bool     primed = false;

    uint32_t sent, to, drop;
    ch582_tx_stats(&sent, &to, &drop);
    uint32_t blit = lcd_blit_timeouts();
    uint8_t  wdt  = watchdog_reset_count();
#ifdef DEBUG_MATRIX_SCAN_RATE
    uint16_t band = (uint16_t)(get_matrix_scan_rate() / 25);
#else
    uint16_t band = 0;
#endif

    /* `sent` is printed as context but deliberately does NOT trigger a line
     * -- it rises on every BT keystroke and would make the output continuous. */
    bool changed = blit != s_blit || to != s_to || drop != s_drop ||
                   rx_malformed != s_malf || loop_gap_max != s_gap ||
                   band != s_band || wdt != s_wdt;
    if (!primed) {
        /* Skip the boot transient; report only changes after settle. */
        if (timer_read32() < HEALTH_SETTLE_MS) return;
        primed = true;
        changed = true; /* one baseline line so the log shows the healthy state */
    }
    if (!changed) return;

    s_blit = blit; s_to = to; s_drop = drop;
    s_malf = rx_malformed; s_gap = loop_gap_max; s_band = band; s_wdt = wdt;

    printf("[health] blit_to=%lu tx=%lu to=%lu drop=%lu malf=%lu gap=%lums scan~%u wdt=%u%s%s\n",
           blit, sent, to, drop, rx_malformed, loop_gap_max,
           (unsigned)(band * 25), watchdog_reset_count(),
           watchdog_fired_last_boot() ? " FIRED" : "",
           watchdog_degraded() ? " DEGRADED" : "");
#endif
}
