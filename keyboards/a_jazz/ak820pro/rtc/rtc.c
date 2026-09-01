// rtc.c
// Copyright 2026 Fernando Birra
// SPDX-License-Identifier: GPL-2.0-or-later

#include "rtc.h"
#include "quantum.h"
#include "kb_eeconfig.h"   /* persisted divider period (phase 4) */
#include "hal.h"
#include "../graphics/lcd_bus.h"
#include "../ak820pro.h"

#include <time.h>


#ifndef RTC_SCL_PIN
#    define RTC_SCL_PIN A14
#endif

#ifndef RTC_SDA_PIN
#    define RTC_SDA_PIN A15
#endif


#define PCF8563_ADDR            0x51
#define PCF8563_REG_SECONDS     0x02
#define PCF8563_VL_FLAG         0x80
#define PCF8563_I2C_TIMEOUT     TIME_MS2I(20)


#ifndef PCF8563_I2C_DELAY_NOPS
#    define PCF8563_I2C_DELAY_NOPS 15
#endif


static void rtc_i2c_delay(void)
{

    for (volatile uint32_t i = 0;
         i < PCF8563_I2C_DELAY_NOPS;
         i++) {
        __asm__ volatile("nop");
    }

}


static const I2CConfig i2ccfg = {
    .addr10 = false,
    .scl    = RTC_SCL_PIN,
    .sda    = RTC_SDA_PIN,
    .delay  = rtc_i2c_delay,
};



static inline uint8_t bcd2dec(uint8_t v)
{
    return (uint8_t)(((v >> 4) * 10) + (v & 0x0F));
}


static inline uint8_t dec2bcd(uint8_t v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}


/*
 * ============================================================================
 * PCF8563 reference RTC
 * ============================================================================
 */



/* ---- Bus guard: never bit-bang I2C while a flash->LCD DMA is in flight ----
 *
 * lcd_bus.c states the hazard outright: "The bit-banged RTC I2C (SCL=A14,
 * SDA=A15) shares port A with the flash SPI1 pins (SCK=A12, CS=A13); its
 * open-drain pin-mode toggling glitches A12/A13 mid-DMA and corrupts the flash
 * read. Callers must suspend RTC polling while this is true."
 *
 * That contract was enforced ONLY for the animation player, via anim_active().
 * The dashboard blits constantly -- clock, text, battery, playback -- and had no
 * guard at all, so every PCF transaction was free to land in the middle of one.
 * A glitched SPI1 starves the DMA, the completion IRQ never arrives, and
 * blit_done is stuck false forever: the hang.
 *
 * Draining first is sufficient rather than merely narrowing: both the I2C and
 * every blit are started from the main loop, so once the in-flight blit is done
 * no new one can begin before this transaction finishes. Same argument as
 * backing_store_pre_write_hook().
 *
 * The counter is the evidence. It records how often a transaction WOULD have
 * landed on a live blit, so it measures the hazard rate even now that it is
 * prevented -- if it climbs while timeouts stay at zero, this was the cause. */
static uint16_t i2c_blit_overlap = 0;

uint16_t rtc_i2c_overlaps(void) { return i2c_blit_overlap; }

static void rtc_bus_guard(void) {
#ifdef LOOPGAP_INSTRUMENT
    loop_stall_mark = LOOP_MARK_I2C;
#endif
    if (lcd_blit_busy() && i2c_blit_overlap < 0xFFFFu) {
        i2c_blit_overlap++;
    }
    lcd_blit_wait();
}

static bool pcf_read(rtc_time_t *out)
{
    rtc_bus_guard();
    uint8_t reg = PCF8563_REG_SECONDS;
    uint8_t buf[7];


    if (i2cMasterTransmitTimeout(&I2CD1,
                                 PCF8563_ADDR,
                                 &reg,
                                 1,
                                 buf,
                                 sizeof(buf),
                                 PCF8563_I2C_TIMEOUT) != MSG_OK) {
        return false;
    }


    if (buf[0] & PCF8563_VL_FLAG) {
        return false;
    }


    out->seconds = bcd2dec(buf[0] & 0x7F);
    out->minutes = bcd2dec(buf[1] & 0x7F);
    out->hours   = bcd2dec(buf[2] & 0x3F);
    out->day     = bcd2dec(buf[3] & 0x3F);
    out->weekday = buf[4] & 0x07;
    out->month   = bcd2dec(buf[5] & 0x1F);
    out->year    = 2000U + bcd2dec(buf[6]);


    if (out->seconds > 59 ||
        out->minutes > 59 ||
        out->hours > 23 ||
        out->day < 1 ||
        out->day > 31 ||
        out->month < 1 ||
        out->month > 12) {
        return false;
    }


    return true;
}



static bool pcf_write(const rtc_time_t *t)
{
    rtc_bus_guard();
    uint8_t buf[8] = {
        PCF8563_REG_SECONDS,
        dec2bcd(t->seconds),
        dec2bcd(t->minutes),
        dec2bcd(t->hours),
        dec2bcd(t->day),
        t->weekday & 0x07,
        dec2bcd(t->month),
        dec2bcd((uint8_t)(t->year % 100)),
    };


    return i2cMasterTransmitTimeout(&I2CD1,
                                    PCF8563_ADDR,
                                    buf,
                                    sizeof(buf),
                                    NULL,
                                    0,
                                    PCF8563_I2C_TIMEOUT) == MSG_OK;
}



/*
 * ============================================================================
 * ChibiOS RTC conversion helpers
 * ============================================================================
 */


static void rtc_to_chibiostime(const rtc_time_t *src,
                               RTCDateTime *dst)
{
    dst->year = src->year - RTC_BASE_YEAR;
    dst->month = src->month;
    dst->day = src->day;
    dst->dayofweek = src->weekday;
    dst->dstflag = 0;


    dst->millisecond =
        ((uint32_t)src->hours * 3600UL +
         (uint32_t)src->minutes * 60UL +
         src->seconds) * 1000UL;
}



static void chibiostime_to_rtc(const RTCDateTime *src,
                               rtc_time_t *dst)
{
    dst->year = src->year + RTC_BASE_YEAR;
    dst->month = src->month;
    dst->day = src->day;
    dst->weekday = src->dayofweek;


    dst->hours =
        (uint8_t)(src->millisecond / 3600000UL);

    dst->minutes =
        (uint8_t)((src->millisecond / 60000UL) % 60);

    dst->seconds =
        (uint8_t)((src->millisecond / 1000UL) % 60);
}


/*
 * ============================================================================
 * Synchronization
 * ============================================================================
 */


static bool rtc_valid;


#ifdef RTC_AUTO_CALIBRATION

#ifndef RTC_CHECK_INTERVAL_S
#    define RTC_CHECK_INTERVAL_S 3600
#endif


#ifndef RTC_DRIFT_THRESHOLD_S
#    define RTC_DRIFT_THRESHOLD_S 2
#endif

/* Minimum rate-measurement window, in reference seconds. The PCF reads to 1 s,
 * so this sets the best resolution a single trim can have (~0.3% at 300 s). */
#ifndef RTC_CAL_MIN_WINDOW_S
#    define RTC_CAL_MIN_WINDOW_S 300
#endif

/* Minimum tick-vs-real discrepancy worth acting on. Must be > 1 so reference
 * quantisation alone cannot trigger a trim. */
#ifndef RTC_CAL_MIN_DIFF_S
#    define RTC_CAL_MIN_DIFF_S 2
#endif

static volatile uint32_t rtc_check_seconds;

/* Rate-trim state. The SN32 RTC is clocked from the ILRC (see
 * SN32_RTC_CLK_SOURCE in hal_rtc_lld.h), an untrimmed internal RC oscillator,
 * against a hardcoded SN32_RTC_PERIOD_DEFAULT of 32000 that assumes exactly
 * 32 kHz. Measured on this unit: ~34.3 kHz, i.e. the clock gains ~4 s per
 * minute. Phase snapping alone therefore leaves a sawtooth as large as the
 * check interval's worth of drift, which is what made the clock look "absolutely
 * horrid" -- it was, but only between snaps.
 *
 * These open a rate-measurement window so the divider itself can be corrected.
 * Both quantities are snap-immune -- a free-running tick count and the PCF's own
 * absolute time -- so a phase correction mid-window does not invalidate it. */
static uint32_t rtc_cal_ticks0;     /* rtc_seconds_count when the window opened */
static int32_t  rtc_cal_ref0;       /* PCF epoch seconds when the window opened */
static bool     rtc_cal_valid;      /* the two above are meaningful */

#endif



// Free-running count of RTC second interrupts (~seconds since rtc_init). A cheap
// once-per-second edge source (no localtime()) for pacing the display refresh.
static volatile uint32_t rtc_seconds_count = 0;

static void rtc_second_cb(RTCDriver *rtcp, rtcevent_t event)
{
    (void)rtcp;
    (void)event;

    rtc_seconds_count++;

#ifdef RTC_AUTO_CALIBRATION
    rtc_check_seconds = MIN(rtc_check_seconds + 1, RTC_CHECK_INTERVAL_S);
#endif
}

uint32_t rtc_get_seconds(void) {
    return rtc_seconds_count;
}


static void rtc_seed_from_pcf(void)
{
    rtc_time_t pcf;
    RTCDateTime dt;

    if (!pcf_read(&pcf)) {
        return;
    }

    rtc_to_chibiostime(&pcf, &dt);
    rtcSetTime(&RTCD1,&dt);

    rtc_valid = true;
}

/*
 * ============================================================================
 * Reference check
 * ============================================================================
 */


#ifdef RTC_AUTO_CALIBRATION


static void rtc_clock_discipline(void)
{
    rtc_time_t reference;
    RTCDateTime current;
    RTCDateTime target;

    /*
     * PCF8563 is only accessed during a reference check.
     */
    if (!pcf_read(&reference)) {
        return;
    }

    rtcGetTime(&RTCD1, &current);
    rtc_to_chibiostime(&reference, &target);

    struct tm ref_tm;
    struct tm cur_tm;

    rtcConvertDateTimeToStructTm(&target, &ref_tm, NULL);
    rtcConvertDateTimeToStructTm(&current, &cur_tm, NULL);

    time_t  ref_epoch = mktime(&ref_tm);
    int32_t error     = (int32_t)(ref_epoch - mktime(&cur_tm));

    /*
     * Rate trim. Snapping the phase alone leaves the divider wrong, so the clock
     * immediately re-drifts at the same rate; correcting SECCNTV is what actually
     * makes it keep time.
     *
     * The measurement window is built from two snap-IMMUNE quantities:
     * rtc_seconds_count (a free-running count of SN32 second IRQs, untouched by
     * rtcSetTime) and the PCF's own absolute time (never written here). A phase
     * snap therefore does NOT invalidate the window.
     *
     * An earlier version got exactly that wrong -- it restarted the span on every
     * snap, and since the snap fires whenever drift exceeds the threshold, the
     * trim never survived to collect a second sample and so never ran once.
     *
     * Self-refining: as the period converges, the window must grow longer before
     * ticks and real seconds differ by a whole second, so each estimate is finer
     * than the last with no hand-tuned constant.
     */
    uint32_t ticks_now = rtc_seconds_count;

    if (!rtc_cal_valid) {
        rtc_cal_ticks0 = ticks_now;
        rtc_cal_ref0   = (int32_t)ref_epoch;
        rtc_cal_valid  = true;
    } else {
        int32_t ticks = (int32_t)(ticks_now - rtc_cal_ticks0);
        int32_t real  = (int32_t)ref_epoch - rtc_cal_ref0;

        int32_t diff = ticks - real;

        /* Two guards, both learned the hard way. The reference has 1 s
         * resolution, so a short window ALWAYS shows |diff| == 1 whether or not
         * the clock is wrong. An earlier version trimmed on any nonzero diff and
         * restarted the window each time, so the window never grew past 60 ticks,
         * resolution stayed at 1.7%, and it limit-cycled between the two adjacent
         * quantised answers (33103 <-> 33664) indefinitely.
         *
         * Requiring a minimum window makes quantisation a small fraction of the
         * measurement; requiring |diff| >= 2 means noise alone cannot trigger a
         * trim. Together the window grows on its own as the period converges. */
        if ((ticks > 0) && (real >= RTC_CAL_MIN_WINDOW_S) &&
            ((diff >= RTC_CAL_MIN_DIFF_S) || (diff <= -RTC_CAL_MIN_DIFF_S))) {
            uint32_t period = rtc_lld_get_period(&RTCD1);

            if (period > 0) {
                uint32_t np = (uint32_t)(((uint64_t)period * (uint64_t)ticks) /
                                         (uint64_t)real);

                /* Damp to half the computed step. The estimate carries up to a
                 * quantum of error, and applying it in full is what lets an
                 * overshoot become a standing oscillation; half-stepping turns
                 * that into convergence at the cost of one extra window. */
                np = (uint32_t)((int32_t)period + (((int32_t)np - (int32_t)period) / 2));

                /* Clamp hard: one bad PCF read must not strand the divider
                 * somewhere the clock cannot recover from. */
                if (np < (SN32_RTC_PERIOD_DEFAULT / 2U)) np = SN32_RTC_PERIOD_DEFAULT / 2U;
                if (np > (SN32_RTC_PERIOD_DEFAULT * 2U)) np = SN32_RTC_PERIOD_DEFAULT * 2U;

                if (np != period) {
                    rtc_lld_set_period(&RTCD1, np);
                    /* Persist ANY accepted, sane trim once the board has been
                     * up a while and the value moved meaningfully from what is
                     * stored. NOT a wait-for-convergence rule: near lock the
                     * measurement windows grow very long and step sizes are
                     * not guaranteed monotonic (the recorded 695->46 trace
                     * happened to shrink), so a "two small steps" trigger
                     * could wait forever. Persisting incrementally means every
                     * boot starts from the best value yet seen; the 32-tick
                     * threshold (~0.1%) keeps temperature wander from causing
                     * steady rewrites. The write itself is coalesced/deferred
                     * by kb_eeconfig. (Phase 4.1, Codex plan-review #15.) */
                    if (timer_read32() >= 600000u && np >= 28000 && np <= 40000) {
                        uint16_t st = kb_eeconfig_get_rtc_period();
                        uint32_t d  = (np > st) ? (np - st) : (st - np);
                        if (st == 0 || d >= 32) kb_eeconfig_set_rtc_period((uint16_t)np);
                    }
                    printf("[rtc] trim %lu -> %lu (%ld ticks / %ld s)\n",
                           (unsigned long)period, (unsigned long)np,
                           (long)ticks, (long)real);
                }
            }
            /* Period changed: ticks either side are different units. New window. */
            rtc_cal_ticks0 = ticks_now;
            rtc_cal_ref0   = (int32_t)ref_epoch;
        }
    }

    /*
     * Phase snap. Independent of the trim, and deliberately does NOT restart the
     * calibration window -- see above.
     */
#ifdef RTC_LOG_EVERY_PASS
    /* The snap log below fires only when |error| > threshold, so errors of 0, 1
     * and 2 s -- exactly the regime worth watching -- cannot be seen at all.
     * Behind a flag because at RTC_CHECK_INTERVAL_S 60 this is ~1440 lines/day. */
    printf("[rtc] error %ld s\n", (long)error);
#endif
    if ((error > RTC_DRIFT_THRESHOLD_S) || (error < -RTC_DRIFT_THRESHOLD_S)) {
        rtcSetTime(&RTCD1, &target);
        printf("[rtc] corrected drift of %ld seconds\n", (long)error);
    }
}


#endif

/*
 * ============================================================================
 * Public API
 * ============================================================================
 */


void rtc_init(void)
{
    i2cStart(&I2CD1, &i2ccfg);

    /* Divider seed, best first: the PERSISTED converged period (phase 4 --
     * survives power cycles, correct on ANY unit, makes the compile-time seed
     * irrelevant once the trim has run once), else RTC_PERIOD_INITIAL (a
     * hand-measured, per-unit value -- see the CLAUDE.md warning never to
     * ship it to another board), else the LLD's nominal 32000 and a ~40 min
     * climb. The sanity window brackets any plausible ILRC (spec'd loosely
     * around 32 kHz): a stored value outside it is garbage, not data. */
    uint16_t stored = kb_eeconfig_get_rtc_period();
    if (stored >= 28000 && stored <= 40000) {
        rtc_lld_set_period(&RTCD1, stored);
    }
#ifdef RTC_PERIOD_INITIAL
    else {
        rtc_lld_set_period(&RTCD1, RTC_PERIOD_INITIAL);
    }
#endif

    rtcSetCallback(&RTCD1, rtc_second_cb);
    rtc_seed_from_pcf();
}



bool rtc_get_time(rtc_time_t *out)
{
    RTCDateTime dt;

    if (!rtc_valid) {
        return false;
    }

    rtcGetTime(&RTCD1, &dt);
    chibiostime_to_rtc(&dt, out);

    return true;
}



bool rtc_set_time(const rtc_time_t *t)
{
    RTCDateTime dt;

    rtc_to_chibiostime(t, &dt);

    bool ok = pcf_write(t);

    rtcSetTime(&RTCD1, &dt);
    rtc_valid = true;

    return ok;
}



void rtc_task(void)
{
#ifdef RTC_AUTO_CALIBRATION

    if (rtc_check_seconds < RTC_CHECK_INTERVAL_S) {
        return;
    }

    /* The 1 Hz RTC callback RMWs rtc_check_seconds in ISR context; a bare
     * main-loop store here can lose against it (load/store/store-old+1) and
     * re-trigger a check ~1 s later. Benign -- the trim measures from
     * snap-immune sources, not this counter -- but masking one store is
     * cheaper than reasoning about it again. (Audit C-1.) */
    chSysLock();
    rtc_check_seconds = 0;
    chSysUnlock();
    rtc_clock_discipline();
#endif
}