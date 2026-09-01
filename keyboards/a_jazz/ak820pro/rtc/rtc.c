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

/* ---- Phase 0 instrumentation (clock-sync plan) ----------------------------
 * Everything here is OBSERVATION: reads of SECCNT / FRMNO and a few adds. No
 * behaviour changes in this phase. Layouts are served by rtc_status_fill(). */
static volatile uint32_t rtc_seconds_count = 0;   /* moved up: needed by stamps */

static uint16_t stale_count    = 0;   /* rtc_now() gave up (SECIF pending, ISR starved) */
static uint16_t i2c_fail_count = 0;   /* PCF transactions that returned != MSG_OK */
static uint32_t i2c_max_cycles = 0;   /* longest PCF transaction, ILRC cycles */
static uint32_t last_sync_secs = 0;   /* rtc_seconds_count at the last host set */
static bool     host_synced    = false;

/* Tick-ISR service latency: SECCNT at callback entry (cycles since the match
 * restarted the counter). Sets LATE_CYCLES for later phases (T0.2). */
static volatile uint16_t lat_min = 0xFFFF, lat_max = 0, lat_n = 0;
static volatile uint32_t lat_sum = 0;

/* USB SOF frame-number observation (T0.3). The ISR reads the volatile mirror
 * ONCE and never touches SN_USB when it is false. */
static volatile uint8_t  usb_active_mirror = 0;
static volatile uint8_t  fn_valid = 0;
static volatile uint16_t fn_last  = 0;
static volatile uint32_t sof_frames_total = 0;    /* continuous while continuity holds */
static volatile uint8_t  sof_epoch = 0;           /* bumps on every continuity break */
static volatile uint16_t d_zero = 0, d_reject = 0, d_ok = 0;
#define D_RING_N 14
static volatile uint16_t d_ring[D_RING_N];
static volatile uint8_t  d_ring_i = 0;

/* Elapsed ILRC cycles between two (seconds_count, SECCNT) stamps; only valid
 * for spans well under a second (an I2C transaction). */
static inline void cyc_stamp(uint32_t *sc, uint32_t *cnt) {
    *sc  = rtc_seconds_count;
    *cnt = SN_RTC->SECCNT;
}
static inline uint32_t cyc_elapsed(uint32_t sc0, uint32_t c0, uint32_t sc1, uint32_t c1) {
    int32_t d = (int32_t)c1 - (int32_t)c0 + (int32_t)(sc1 - sc0) * (int32_t)(rtc_lld_get_period(&RTCD1) + 1);
    return d < 0 ? 0 : (uint32_t)d;
}
static void i2c_note(msg_t r, uint32_t sc0, uint32_t c0) {
    uint32_t sc1, c1; cyc_stamp(&sc1, &c1);
    uint32_t e = cyc_elapsed(sc0, c0, sc1, c1);
    if (e > i2c_max_cycles) i2c_max_cycles = e;
    if (r != MSG_OK && i2c_fail_count < 0xFFFF) i2c_fail_count++;
}

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


    uint32_t sc0, c0; cyc_stamp(&sc0, &c0);
    msg_t r = i2cMasterTransmitTimeout(&I2CD1,
                                 PCF8563_ADDR,
                                 &reg,
                                 1,
                                 buf,
                                 sizeof(buf),
                                 PCF8563_I2C_TIMEOUT);
    i2c_note(r, sc0, c0);
    if (r != MSG_OK) {
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


    uint32_t sc0, c0; cyc_stamp(&sc0, &c0);
    msg_t r = i2cMasterTransmitTimeout(&I2CD1,
                                    PCF8563_ADDR,
                                    buf,
                                    sizeof(buf),
                                    NULL,
                                    0,
                                    PCF8563_I2C_TIMEOUT);
    i2c_note(r, sc0, c0);
    return r == MSG_OK;
}

/* Single-register PCF access, bus-guarded like the rest. Phase 0 uses these
 * for the STOP-bit and 1-byte-read timing tests; Phase 3's deferred state
 * machine will call the same primitives without the guard (and lift the
 * ifdef -- until then the daily build has no caller, and -Werror objects). */
#ifdef WDT_TEST_HOOKS
static bool pcf_reg_read(uint8_t reg, uint8_t *v) {
    rtc_bus_guard();
    uint32_t sc0, c0; cyc_stamp(&sc0, &c0);
    msg_t r = i2cMasterTransmitTimeout(&I2CD1, PCF8563_ADDR, &reg, 1, v, 1, PCF8563_I2C_TIMEOUT);
    i2c_note(r, sc0, c0);
    return r == MSG_OK;
}
static bool pcf_reg_write(uint8_t reg, uint8_t v) {
    rtc_bus_guard();
    uint8_t buf[2] = { reg, v };
    uint32_t sc0, c0; cyc_stamp(&sc0, &c0);
    msg_t r = i2cMasterTransmitTimeout(&I2CD1, PCF8563_ADDR, buf, 2, NULL, 0, PCF8563_I2C_TIMEOUT);
    i2c_note(r, sc0, c0);
    return r == MSG_OK;
}
#endif /* WDT_TEST_HOOKS */



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
// (rtc_seconds_count itself is declared above, with the Phase 0 instruments.)

static void rtc_second_cb(RTCDriver *rtcp, rtcevent_t event)
{
    (void)rtcp;
    (void)event;

    /* T0.2: service latency -- cycles since the match restarted the counter.
     * Read FIRST, before anything else in the handler adds to it. */
    uint32_t L = SN_RTC->SECCNT;

    rtc_seconds_count++;

#ifdef RTC_AUTO_CALIBRATION
    rtc_check_seconds = MIN(rtc_check_seconds + 1, RTC_CHECK_INTERVAL_S);
#endif

    if (L < lat_min) lat_min = (uint16_t)L;
    if (L > lat_max) lat_max = (uint16_t)L;
    if (lat_n < 0xFFFF) { lat_n++; lat_sum += L; }

    /* T0.3: USB SOF frame-number delta per RTC second. Observation only in
     * Phase 0 (no discipline). Mirror read once; SN_USB untouched when down. */
    uint8_t ua = usb_active_mirror;
    if (!ua) {
        if (fn_valid) sof_epoch++;
        fn_valid = 0;
    } else {
        uint16_t fn = (uint16_t)(SN_USB->FRMNO & 0x7FFu);
        uint16_t d  = fn_valid ? (uint16_t)((fn - fn_last) & 0x7FFu) : 0;
        fn_last  = fn;
        if (!fn_valid) { fn_valid = 1; }
        else {
            if (d) sof_frames_total += d;
            if (d == 0)                 { d_zero++;   sof_epoch++; }
            else if (d < 900 || d > 1100) { d_reject++; sof_epoch++; }
            else                        { d_ok++; }
            d_ring[d_ring_i] = d;
            d_ring_i = (uint8_t)((d_ring_i + 1) % D_RING_N);
        }
    }
}

/* Coherent sub-second read (PLAN.md 3.1). Lock-free: a tick landing inside
 * the snapshot shows as c1 != c2; a match whose ISR has not yet run shows as
 * SECIF pending. Either way retry; after 8 tries report failure and NO time. */
bool rtc_now(rtc_stamp_t *s)
{
    if (!rtc_valid) return false;
    for (int tries = 0; tries < 8; tries++) {
        uint32_t c1  = rtc_seconds_count;
        uint32_t cnt = SN_RTC->SECCNT;
        uint32_t pa  = rtc_lld_get_period(&RTCD1);
        bool pending = (SN_RTC->RIS & mskRTC_SECIF) != 0;
        RTCDateTime dt;
        rtcGetTime(&RTCD1, &dt);
        uint32_t c2  = rtc_seconds_count;
        if (c1 == c2 && !pending) {
            chibiostime_to_rtc(&dt, &s->t);
            s->cnt = cnt;
            s->period_active = pa;
            s->seconds_count = c1;
            return true;
        }
    }
    if (stale_count < 0xFFFF) stale_count++;
    return false;
}

/* Status blocks for raw HID -- layouts mirrored in hostagent/rtc_phase0.py. */
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

void rtc_status_fill(uint8_t page, uint8_t *out)
{
    switch (page) {
    case 1: {   /* 21 bytes: the RTC_GET_TIME[11..31] tail */
        rtc_stamp_t s;
        bool ok = rtc_now(&s);
        out[0] = 2;                                          /* RTC_PROTO_VERSION */
        put32(&out[1], ok ? s.cnt : 0);                      /* [12..15] */
        put16(&out[5], (uint16_t)rtc_lld_get_period(&RTCD1));/* [16..17] active */
        put16(&out[7], (uint16_t)rtc_lld_get_period(&RTCD1));/* [18..19] nominal (== active until Phase 2) */
        out[9]  = (host_synced ? 0x01 : 0) | (stale_count ? 0x10 : 0);   /* [20] flags */
        put16(&out[10], 0);                                  /* [21..22] last_host_offset_ms (Phase 1) */
        put16(&out[12], 0);                                  /* [23..24] sof_bias_ppm (Phase 2) */
        out[14] = 1;                                         /* [25] ref_state: PCF_LEGACY */
        uint32_t age = host_synced ? (rtc_seconds_count - last_sync_secs) / 60u : 255u;
        out[15] = (uint8_t)(age > 254 ? 255 : age);          /* [26] sync_age_min */
        out[16] = sof_epoch;                                 /* [27] */
        put32(&out[17], sof_frames_total);                   /* [28..31] */
        break;
    }
    case 2: {   /* 28 bytes of counters */
        put16(&out[0], stale_count);
        put16(&out[2], i2c_fail_count);
        put16(&out[4], 0);                                   /* deferred_passes (Phase 1) */
        put32(&out[6], i2c_max_cycles);
        put16(&out[10], 0);                                  /* window_rejects (Phase 2) */
        put16(&out[12], 0);                                  /* ref_transitions (Phase 2) */
        put16(&out[14], lat_min == 0xFFFF ? 0 : lat_min);
        put16(&out[16], lat_max);
        put16(&out[18], lat_n ? (uint16_t)(lat_sum / lat_n) : 0);
        put16(&out[20], lat_n);
        put16(&out[22], d_zero);
        put16(&out[24], d_reject);
        out[26] = (uint8_t)sizeof(time_t);
        out[27] = usb_active_mirror | (fn_valid ? 2 : 0);
        break;
    }
    case 3: {   /* 28 bytes: the delta ring, oldest first */
        for (int i = 0; i < D_RING_N; i++) {
            put16(&out[2 * i], d_ring[(d_ring_i + i) % D_RING_N]);
        }
        break;
    }
    default:
        memset(out, 0, 28);
        break;
    }
}

#ifdef WDT_TEST_HOOKS
/* Phase 0 hardware-fact tests. arg = request bytes from [4], reply written
 * from [3]. All of these mutate the clock or the PCF on purpose. */
void rtc_test_op(uint8_t op, const uint8_t *arg, uint8_t *reply)
{
    memset(reply, 0, 29);
    reply[0] = op;
    switch (op) {
    case 1:     /* T0.1: does writing SECCNTV (same value) reset SECCNT?
                 * -> [1..4] cnt before, [5..8] right after, [9..12] ~100 us later, [13..16] period */
    case 2: {   /* same but write period+1 then restore (two writes; second reset expected too) */
        uint32_t p = rtc_lld_get_period(&RTCD1);
        chSysLock();
        uint32_t a = SN_RTC->SECCNT;
        rtc_lld_set_period(&RTCD1, op == 2 ? p + 1 : p);
        uint32_t b = SN_RTC->SECCNT;
        chSysUnlock();
        wait_us(100);
        uint32_t c = SN_RTC->SECCNT;
        if (op == 2) rtc_lld_set_period(&RTCD1, p);
        put32(&reply[1], a); put32(&reply[5], b); put32(&reply[9], c); put32(&reply[13], p);
        break;
    }
    case 3:     /* reset the latency / delta statistics */
        chSysLock();
        lat_min = 0xFFFF; lat_max = 0; lat_n = 0; lat_sum = 0;
        d_zero = d_reject = d_ok = 0;
        chSysUnlock();
        break;
    case 4: {   /* T0.4: time one 1-byte PCF read -> [1] byte, [2..5] cycles, [6] ok */
        uint32_t before = i2c_max_cycles; i2c_max_cycles = 0;
        uint8_t v = 0;
        bool ok = pcf_reg_read(PCF8563_REG_SECONDS, &v);
        reply[1] = v; put32(&reply[2], i2c_max_cycles); reply[6] = ok;
        if (before > i2c_max_cycles) i2c_max_cycles = before;
        break;
    }
    case 5: {   /* T0.4: time one 8-byte PCF write (rewrites the CURRENT time) -> [2..5] cycles, [6] ok */
        rtc_time_t t;
        if (!rtc_get_time(&t)) { reply[6] = 0xFF; break; }
        uint32_t before = i2c_max_cycles; i2c_max_cycles = 0;
        bool ok = pcf_write(&t);
        put32(&reply[2], i2c_max_cycles); reply[6] = ok;
        if (before > i2c_max_cycles) i2c_max_cycles = before;
        break;
    }
    case 6: {   /* T0.5 step 1: RMW STOP=1, then write the 7 time bytes in arg[0..6]
                 * (yy mm dd wd hh mi ss) -> [1] ctl before, [2] ctl written, [3] ok */
        uint8_t ctl = 0;
        if (!pcf_reg_read(0x00, &ctl)) { reply[3] = 0; break; }
        reply[1] = ctl;
        uint8_t nctl = (uint8_t)(ctl | 0x20);
        if (!pcf_reg_write(0x00, nctl)) { reply[3] = 0; break; }
        reply[2] = nctl;
        rtc_time_t t = { .year = (uint16_t)(2000 + arg[0]), .month = arg[1], .day = arg[2],
                         .weekday = arg[3], .hours = arg[4], .minutes = arg[5], .seconds = arg[6] };
        reply[3] = pcf_write(&t) ? 1 : 0;
        break;
    }
    case 7: {   /* T0.5 step 2: release STOP now -> [1..4] sec_count before, [5..8] cnt before,
                 * [9..12] sec_count after, [13..16] cnt after, [17] ctl written, [18] ok */
        uint8_t ctl = 0;
        if (!pcf_reg_read(0x00, &ctl)) { reply[18] = 0; break; }
        uint8_t nctl = (uint8_t)(ctl & ~0x20);
        uint32_t sc0, c0, sc1, c1;
        cyc_stamp(&sc0, &c0);
        bool ok = pcf_reg_write(0x00, nctl);
        cyc_stamp(&sc1, &c1);
        put32(&reply[1], sc0); put32(&reply[5], c0); put32(&reply[9], sc1); put32(&reply[13], c1);
        reply[17] = nctl; reply[18] = ok;
        break;
    }
    case 8: {   /* T0.5 step 3 (polled): read the PCF seconds byte with a board stamp
                 * -> [1] byte, [2..5] sec_count, [6..9] cnt (stamp taken AFTER the read) */
        uint8_t v = 0;
        bool ok = pcf_reg_read(PCF8563_REG_SECONDS, &v);
        uint32_t sc, c; cyc_stamp(&sc, &c);
        reply[1] = v; put32(&reply[2], sc); put32(&reply[6], c); reply[10] = ok;
        break;
    }
    case 9: {   /* read Control_status_1 -> [1] value, [2] ok */
        uint8_t v = 0;
        reply[2] = pcf_reg_read(0x00, &v); reply[1] = v;
        break;
    }
    case 10: {  /* T0.1 fallback: RTCEN 1->0->1 -> [1..4] cnt before, [5..8] after */
        chSysLock();
        uint32_t a = SN_RTC->SECCNT;
        SN_RTC->CTRL &= ~1u;
        SN_RTC->CTRL |= 1u;
        uint32_t b = SN_RTC->SECCNT;
        chSysUnlock();
        put32(&reply[1], a); put32(&reply[5], b);
        break;
    }
    default:
        reply[0] = 0xFF;
        break;
    }
}
#endif

/* Live SN32 divider period (the trimmed value in the register), for the
 * HC_CONN readout -- lets the host compare it against the persisted seed. */
uint32_t rtc_get_period(void) {
    return rtc_lld_get_period(&RTCD1);
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
    host_synced    = true;
    last_sync_secs = rtc_seconds_count;

    return ok;
}



void rtc_task(void)
{
    /* USB-active mirror for the tick ISR (PLAN.md 3.4): I-class read under a
     * short lock, published as a plain volatile byte. */
    chSysLock();
    uint8_t ua = (usbGetDriverStateI(&USBD1) == USB_ACTIVE) ? 1 : 0;
    chSysUnlock();
    usb_active_mirror = ua;

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