/* rgb/rowisr_diag.c: the row ISR rewrite's bench diagnostics (rowisr_diag.h). */
#include "rowisr_diag.h"

#ifdef AK820_DEV_HOOKS

#include <string.h>
#include <hal.h>
#include "gpio.h"
#include "timer.h"
#include "wait.h"
#ifdef RGB_MATRIX_ENABLE
#    include "rgb_matrix.h"
#endif
#include "rowisr.h"
#include "indicators.h"

/* ---- sub 10: the cost of a register read ----------------------------------- */
/* 256 reads of one register in a loop, interrupts masked, timed by CT16B5
 * (HCLK/256): the ticks for 256 reads equal the HCLK cycles per read, loop
 * included; the least of 8 trials. A RAM word gives the loop's own cost. The
 * USB block's registers (FRMNO, INSTS, CFG) are read 6 times a slot by the
 * handler and once a tick by the 20 kHz observer since D3 case 5; the warm
 * windows of 11-02 (19 us a slot, not the divides) ask what one costs. */
static volatile uint32_t read_sink;
static uint16_t read_trial(volatile const uint32_t *p) {
    uint16_t best = 0xFFFFu;
    for (uint8_t t = 0; t < 8u; t++) {
        uint32_t acc = 0;
        chSysLock();
        uint16_t t0 = (uint16_t)(*(volatile uint32_t *)(0x4000A004u));   /* CT16B5's TC: CT16_BASE(5) + CT_TC, spelled out so the loop has no call */
        for (uint16_t i = 0; i < 256u; i++) acc += *p;
        uint16_t t1 = (uint16_t)(*(volatile uint32_t *)(0x4000A004u));
        chSysUnlock();
        read_sink = acc;
        uint16_t d = (uint16_t)(t1 - t0);
        if (d < best) best = d;
    }
    return best;
}

/* ---- sub 0: the timers' registers ------------------------------------------ */

#define CT16_BASE(n) (0x40000000u + 0x2000u * (n))   /* docs/sn32f299.md, the memory map */
/* Offsets (docs/sn32f299.md, section 7, "Registers") */
#define CT_TMRCTRL   0x00u
#define CT_TC        0x04u
#define CT_PRE       0x08u
#define CT_PC        0x0Cu
#define CT_CNTCTRL   0x10u
#define CT_MCTRL     0x14u
#define CT_MCTRL2    0x18u   /* CT16B1 */
#define CT_MR(n)     (0x20u + 4u * (n))
#define CT_EM        0x8Cu
#define CT_PWMCTRL   0x98u
#define CT_PWMENB    0xA0u   /* CT16B1 */
#define CT_PWMIOENB  0xA4u   /* CT16B1 */
#define CT_RIS       0xA8u
#define CT_PWMNIOCTRL 0xB0u  /* CT16B0, B3, B4: the inverted outputs */

static inline uint32_t ct_rd(uint8_t t, uint32_t off) {
    return *(volatile uint32_t *)(CT16_BASE(t) + off);
}

static inline void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static void regs_fill(uint8_t t, uint8_t page, uint8_t *out) {   /* out = &data[4], 28 bytes, zeroed */
    out[0] = t;
    out[1] = page;
    if (page == 0) {
        put32(&out[2], ct_rd(t, CT_TMRCTRL));
        put16(&out[6], (uint16_t)ct_rd(t, CT_TC));
        put16(&out[8], (uint16_t)ct_rd(t, CT_PRE));
        put32(&out[10], ct_rd(t, CT_MCTRL));
        put32(&out[14], ct_rd(t, CT_PWMCTRL));
        put32(&out[18], ct_rd(t, CT_RIS));
        put32(&out[22], ct_rd(t, t == 1 ? CT_MR(12) : CT_MR(9)));
        return;
    }
    uint32_t w[6];
    if (t == 1) {
        w[0] = ct_rd(t, CT_MCTRL2);  w[1] = ct_rd(t, CT_PWMENB);  w[2] = ct_rd(t, CT_PWMIOENB);
        w[3] = ct_rd(t, CT_MR(0));   w[4] = ct_rd(t, CT_MR(1));   w[5] = ct_rd(t, CT_MR(2));
    } else {
        w[0] = ct_rd(t, CT_PC);      w[1] = ct_rd(t, CT_CNTCTRL); w[2] = ct_rd(t, CT_EM);
        w[3] = ct_rd(t, CT_MR(0));   w[4] = ct_rd(t, CT_MR(1));
        w[5] = (t == 3 || t == 4) ? ct_rd(t, CT_PWMNIOCTRL) : ct_rd(t, CT_MR(2));
    }
    for (uint8_t k = 0; k < 6; k++) put32(&out[2 + 4 * k], w[k]);
}

/* ---- subs 1, 2 and 4: the row-only hold-off with its marker, the scan skip ---
 *
 * One countdown serves both (they never run together): hold_ticks counts
 * 50 us ticks, up to 200,000 for 10 s, in one aligned word, so the tick's
 * decrement and the main loop's read never tear. The end comes from the
 * tick (rowisr_diag_tick), from a tick stop (rowisr_diag_cancelI, called by
 * pwm_tick_stopI before the tick's pins go low: light sleep's bit 3 and
 * deep sleep's entry both stop it, and a hold left armed across a sleep
 * would keep the line off after the wake until the countdown resumed,
 * losing the waking press; codex, stage 1's review, P1), or from the
 * housekeeping backstop 50 ms past the deadline. RAM is at the floor: the
 * counters are two bytes each. */
static volatile uint32_t hold_ticks  = 0;
static volatile uint8_t  hold_kind   = 0;      /* 0 none, 1 a hold, 4 a scan skip */
static uint8_t           hold_flags  = 0;      /* HF_ROW_WAS_ON: the line's state when the hold began (sub 2's [7]);
                                                * HF_OVERLAPPED: the last hold began or ended at a depth above its own */
#define HF_ROW_WAS_ON 1u
#define HF_OVERLAPPED 2u
static uint32_t          hold_deadline_ms;     /* the backstop's: the end plus 50 ms, on timer_read32 */
static uint8_t           holds_done = 0, holds_backstopped = 0, holds_cancelled = 0;   /* wrapping at 256: RAM is at the floor */

/* Inside a critical section (the caller's). For a hold: the marker's falling
 * edge and the line's release together, a few cycles apart. */
static void hold_endI(void) {
    if (hold_kind == 1) {
        if (sn32f2xx_row_irq_release() != 0) hold_flags |= HF_OVERLAPPED;
        indicators_marker(false);
    } else if (hold_kind == 4) {
        sn32f2xx_scan_skip = false;
    }
    hold_kind  = 0;
    hold_ticks = 0;
}

static bool hold_start(uint8_t kind, uint16_t ms) {
    if (ms < 1u || ms > 10000u || hold_kind != 0 || !pwm_tick_running()) return false;
    if (kind == 1 && (lock_state_caps() || (indicator_forced() & 1u))) return false;   /* no rising edge to mark from a lit pin */
    uint32_t deadline = timer_read32() + ms + 50u;   /* timer_read32 takes a lock: before ours */
    chSysLock();
    if (kind == 1 && gpio_read_pin(LED_CAPS_LOCK_PIN)) {   /* the pin itself, under the lock: Caps just cleared can leave it high until the next tick (codex, verify, P2) */
        chSysUnlock();
        return false;
    }
    hold_deadline_ms = deadline;
    hold_ticks       = (uint32_t)ms * 20u;
    hold_kind        = kind;
    if (kind == 1) {
        hold_flags = (NVIC->ISER[0] & (1u << sn32f2xx_row_irq_number())) ? HF_ROW_WAS_ON : 0u;
        indicators_marker(true);                                             /* the rising edge ... */
        if (sn32f2xx_row_irq_hold() != 1) hold_flags |= HF_OVERLAPPED;      /* ... and the NVIC write, with its barriers */
    } else {
        sn32f2xx_scan_skip = true;
    }
    chSysUnlock();
    return true;
}

void rowisr_diag_tick(void) {   /* ISR context, priority 2: above the row interrupt */
    if (!hold_ticks) return;
    if (--hold_ticks == 0) {
        chSysLockFromISR();
        hold_endI();
        holds_done++;
        chSysUnlockFromISR();
    }
}

/* ---- subs 7 and 8: a silence of every interrupt (D3's injections) ------------
 *
 * Armed here, fired by the housekeeping task once its delay has passed, so
 * this op's reply has reached the host first: under the system lock (PRIMASK
 * on this core: every interrupt), the marker on the Win Lock LED's pin C15
 * raised (the pin's read, its branch and the store after the mask), a spin
 * on CT16B5's TC with its wraps counted (polled every few cycles, far inside
 * the 349.5 ms wrap), the marker lowered by one store, the lock released;
 * the state checks and the bookkeeping lie outside the mask (the review's
 * P2 5; the masked part is its own function, the disassembly checked). The
 * length the spin measured, in CT16B5 ticks, the TC at each edge and the
 * extension's copied state before and 2 ms after (its time, generation,
 * validity, the last observer's stamp and the guard's last rejected gap,
 * through rowisr_time_read: a copy, not an observation) are the report
 * (subs 8 and 9);
 * the marker's edges on the witness are the timing. 1,000..8,000,000 us
 * (the watchdog, nominally 11.5 s, is not fed meanwhile; the plan's longest
 * case is 7 s); refused while another is armed, or while the Win Lock LED is
 * lit or previewed (the pin would give no rising edge), checked again under
 * the lock at the firing: if the pin is high then, the silence is not run
 * and counted as lost. A row-only hold may be running: the ordering case
 * nests the two, each on its own pin. A tick stop or a deep sleep's entry
 * cancels an armed silence. */
static volatile uint32_t sil_us      = 0;   /* armed: the length in us; 0 none */
static uint32_t          sil_fire_ms = 0;   /* timer_read32 at which it fires */
static uint8_t           sil_done = 0, sil_lost = 0;   /* run; not run at the firing (the marker's pin was high) */
static struct {                             /* the last silence's report (subs 8 and 9) */
    uint32_t      us, ticks;
    uint16_t      tc0, tc1;                 /* CT16B5 at the marker's edges */
    rowisr_time_t t0, t1;                   /* the extension before, and after the frame counter's next judgments */
    uint8_t       judged, waited_ms;        /* the services entered between the two copies (each judges the interval just ended), and the ms waited for two */
} sil_rep;

/* The masked part on its own, so the rising edge follows the mask by the
 * pin's read, its branch and the store, and the falling edge precedes the
 * unmask by one store (the ELF checked); 0 if the pin was high (no edge). */
__attribute__((noinline)) static uint32_t silence_masked(uint32_t want, uint16_t *tc0, uint16_t *tc1, uint16_t *jseq) {
    chSysLock();
    if (gpio_read_pin(LED_WINLOCK_PIN)) {   /* the pin itself, under the mask: the tick could have raised it since the check outside */
        chSysUnlock();
        return 0;
    }
    *jseq = rowisr_usb_judge_seq();   /* under the mask: no judgment falls between this read and the silence (codex's P2 on 1dfa597991) */
    gpio_write_pin_high(LED_WINLOCK_PIN);   /* the marker's rising edge */
    uint16_t t0   = (uint16_t)ct_rd(5, CT_TC);
    uint16_t last = t0;
    uint32_t acc  = 0;
    while (acc < want) {
        uint16_t now = (uint16_t)ct_rd(5, CT_TC);
        acc += (uint16_t)(now - last);
        last = now;
    }
    gpio_write_pin_low(LED_WINLOCK_PIN);    /* the falling edge: one store before the unmask */
    chSysUnlock();
    *tc0 = t0;
    *tc1 = last;
    return acc ? acc : 1u;
}

static bool silence_arm(uint32_t us, uint16_t delay_ms) {
    if (us < 1000u || us > 8000000u || delay_ms > 5000u || sil_us != 0) return false;
    if (lock_state_gui() || (indicator_forced() & 1u)) return false;
    uint32_t fire = timer_read32() + (delay_ms ? delay_ms : 200u);   /* timer_read32 takes a lock: before ours */
    chSysLock();
    if (gpio_read_pin(LED_WINLOCK_PIN)) {
        chSysUnlock();
        return false;
    }
    sil_fire_ms = fire;
    sil_us      = us;
    chSysUnlock();
    return true;
}

static void silence_run(void) {   /* the housekeeping task: thread context */
    uint32_t us   = sil_us;
    uint32_t want = (uint32_t)(((uint64_t)us * 3u + 8u) / 16u);   /* 187.5 ticks a ms: us x 3 / 16, rounded */
    if (lock_state_gui() || (indicator_forced() & 1u)) {   /* lit or previewed since the arm: no clean rising edge to mark */
        sil_us = 0;
        sil_lost++;
        return;
    }
    rowisr_time_t t0;
    rowisr_time_read(&t0);
    uint16_t tc0 = 0, tc1 = 0, jseq0 = 0;
    uint32_t acc = silence_masked(want, &tc0, &tc1, &jseq0);
    if (acc == 0) {
        sil_us = 0;
        sil_lost++;
        return;
    }
    indicators_out_resync();
    sil_rep.us = us; sil_rep.ticks = acc; sil_rep.tc0 = tc0; sil_rep.tc1 = tc1; sil_rep.t0 = t0;
    wait_ms(2);   /* the observers' first passes after the unmask: the tick's within 50 us, scan-only's slots within 625 us; the guard's verdict and the re-prime */
    uint8_t waited = 2;
    while ((uint16_t)(rowisr_usb_judge_seq() - jseq0) < 2u && waited < 20u) {   /* the frame counter's verdict too: the first service after the silence
                                                                                  * judges it at its entry, and two services since before the silence put
                                                                                  * one after it (the row ISR may be held off past the unmask: a row-only
                                                                                  * hold, scan-only's 625 us slots); 20 ms bounds the wait (codex's P2 on
                                                                                  * 1d73bf3ed5: the fixed 2 ms could copy the state before the verdict) */
        wait_ms(1);
        waited++;
    }
    uint16_t dj = (uint16_t)(rowisr_usb_judge_seq() - jseq0);   /* before the copy: the services the copy reflects (under two, the first service after the
                                                                 * silence had not run at the copy, a hold on the row ISR outlasting the wait; the host says so) */
    rowisr_time_read(&sil_rep.t1);
    sil_rep.judged = dj > 255u ? 255u : (uint8_t)dj; sil_rep.waited_ms = waited;
    sil_us = 0;
    sil_done++;
}

static void report_fill(uint8_t page, uint8_t *out) {   /* subs 8 and 9: out = &data[4], zeroed */
    if (page == 0) {
        put32(&out[0], sil_rep.us);
        put32(&out[4], sil_rep.ticks);
        put32(&out[8], sil_rep.t0.ext);
        put32(&out[12], sil_rep.t1.ext);
        put16(&out[16], (uint16_t)sil_rep.t0.gen);
        put16(&out[18], (uint16_t)sil_rep.t1.gen);
        put16(&out[20], sil_rep.tc0);
        put16(&out[22], sil_rep.tc1);
        out[24] = sil_rep.t0.valid;
        out[25] = sil_rep.t1.valid;
        out[26] = sil_lost;
        out[27] = sil_done;
    } else {
        put16(&out[0], sil_rep.t0.stamp);
        put16(&out[2], sil_rep.t1.stamp);
        put16(&out[4], sil_rep.t0.last_gap);
        put16(&out[6], sil_rep.t1.last_gap);
        put16(&out[8], sil_rep.t0.guard_rejects);
        put16(&out[10], sil_rep.t1.guard_rejects);
        out[12] = sil_rep.judged; out[13] = sil_rep.waited_ms;   /* 5g */
        out[14] = sil_done;   /* the run count, as page 0's [27]: the host pairs the two pages by it (a silence can finish between the reads) */
    }
}

/* ---- sub 14: the frame counter read back to back ---------------------------- */
/* FRMNO read in a tight loop from the housekeeping task, 3 ms a pass until
 * the asked time is spent, every step between consecutive reads classed as
 * 0, +1 or other (modulo 2,048), the first three others kept with their pair
 * of values. A read torn by the counter's update shows as a pair like 0x0FF
 * -> 0x000 (the trim's and the exit-only build's false rejections at spans
 * 0/256 ms, 11-02/03); a preemption between two reads (a clock-chip
 * transaction, a flash write) as a small step from a plausible value. Each
 * slice starts afresh, so a step never spans two passes. */
static uint32_t fscan_left_ms, fscan_reads, fscan_plus1, fscan_other;
static uint16_t fscan_ex[3][2];
static uint8_t  fscan_nex;
static void fscan_slice(void) {
    uint32_t t0   = timer_read32();
    uint16_t prev = (uint16_t)(SN32_USB->FRMNO & 0x7FFu);
    do {
        for (uint16_t i = 0; i < 512u; i++) {
            uint16_t f = (uint16_t)(SN32_USB->FRMNO & 0x7FFu);
            uint16_t d = (uint16_t)((f - prev) & 0x7FFu);
            if (d == 1u) {
                fscan_plus1++;
            } else if (d != 0u) {
                fscan_other++;
                if (fscan_nex < 3u) {
                    fscan_ex[fscan_nex][0] = prev;
                    fscan_ex[fscan_nex][1] = f;
                    fscan_nex++;
                }
            }
            prev = f;
        }
        fscan_reads += 512u;
    } while (timer_elapsed32(t0) < 3u);
    uint32_t spent = timer_elapsed32(t0);
    fscan_left_ms  = fscan_left_ms > spent ? fscan_left_ms - spent : 0u;
}
static void fscan_fill(uint16_t arm_ms, uint8_t *out) {   /* out = &data[4], zeroed */
    if (arm_ms != 0u) {
        fscan_reads = fscan_plus1 = fscan_other = 0;
        fscan_nex   = 0;
        fscan_left_ms = arm_ms > 60000u ? 60000u : arm_ms;
    }
    put32(&out[0], fscan_reads);
    put32(&out[4], fscan_plus1);
    put32(&out[8], fscan_other);
    put16(&out[12], (uint16_t)fscan_left_ms);
    out[14] = fscan_nex;
    for (uint8_t i = 0; i < 3u; i++) {
        put16(&out[15 + 4 * i], fscan_ex[i][0]);
        put16(&out[17 + 4 * i], fscan_ex[i][1]);
    }
}

void rowisr_diag_cancelI(void) {   /* I-class: pwm_tick_stopI(), and the driver's deep-sleep entry */
    sil_us = 0;   /* a silence armed across a sleep would fire at the wake */
    if (hold_kind == 0) return;
    hold_endI();
    holds_cancelled++;
}

void rowisr_diag_housekeeping(void) {
    if (hold_kind != 0 && (int32_t)(timer_read32() - hold_deadline_ms) > 0) {
        chSysLock();
        if (hold_kind != 0) {   /* the tick may have ended it between the test and the lock */
            hold_endI();
            holds_backstopped++;
        }
        chSysUnlock();
    }
    if (sil_us != 0 && (int32_t)(timer_read32() - sil_fire_ms) >= 0) silence_run();
    if (fscan_left_ms != 0u) fscan_slice();
}

static void status_fill(uint8_t *out) {   /* out = &data[4], zeroed */
    chSysLock();   /* one coherent snapshot: the tick cannot end the hold between these reads (codex, verify, P3) */
    uint8_t  kind  = hold_kind;
    uint16_t left  = (uint16_t)(hold_ticks / 20u);
    uint8_t  flags = hold_flags, done = holds_done, back = holds_backstopped, canc = holds_cancelled;
    uint8_t  depth = sn32f2xx_row_irq_depth();
    bool     on    = (NVIC->ISER[0] & (1u << sn32f2xx_row_irq_number())) != 0;
    uint32_t sus   = sil_us;
    uint32_t stk   = sil_rep.ticks;
    uint8_t  sdn   = sil_done;
    chSysUnlock();
    out[0] = kind == 1 ? 1u : 0u;
    put16(&out[1], kind == 1 ? left : 0u);
    out[3] = (flags & HF_ROW_WAS_ON) ? 1u : 0u;
    put16(&out[4], done);
    put16(&out[6], back);
    put16(&out[8], canc);
    out[10] = sn32f2xx_row_irq_number();
    out[11] = on ? 1u : 0u;
    out[12] = depth;
    out[13] = (flags & HF_OVERLAPPED) ? 1u : 0u;
    out[14] = sn32f2xx_prime_timed_out() ? 1u : 0u;
    out[15] = kind == 4 ? 1u : 0u;
    put16(&out[16], kind == 4 ? left : 0u);
    out[18] = sn32f2xx_hw_fault() ? 1u : 0u;   /* [22]: stage 2's init could not reset a timer */
    out[19] = sus != 0 ? 1u : 0u;              /* [23]: a silence of every interrupt is armed */
    put16(&out[20], (uint16_t)(sus / 1000u));  /* [24..25]: its length, ms */
    put32(&out[22], stk);                      /* [26..29]: the last silence's measured length, CT16B5 ticks */
    out[26] = sdn;                             /* [30]: silences run */
    out[27] = sn32f2xx_col_map_fault() ? 1u : 0u;   /* [31]: MATRIX_COL_PINS is not the order the scan's port reads assemble (stage 3) */
}

/* ---- sub 3: one LED, one colour --------------------------------------------- */

#ifdef RGB_MATRIX_ENABLE
static uint8_t pat_led = 0xFFu, pat_r = 0, pat_g = 0, pat_b = 0;

bool rgb_matrix_indicators_kb(void) {
    if (pat_led != 0xFFu) {
        rgb_matrix_set_color_all(0, 0, 0);
        if (pat_led < RGB_MATRIX_LED_COUNT) rgb_matrix_set_color(pat_led, pat_r, pat_g, pat_b);
    }
    return rgb_matrix_indicators_user();
}
#endif

/* ---- op 28 ------------------------------------------------------------------- */

bool rowisr_diag_op28(uint8_t *data) {
    uint8_t sub = data[4];
    switch (sub) {
        case 0: {
            uint8_t t = data[5], page = data[6];
            if (t > 5u || page > 1u) return false;
            memset(&data[4], 0, 28);
            regs_fill(t, page, &data[4]);
            return true;
        }
        case 1:
        case 4: {
            uint16_t ms = (uint16_t)(data[5] | (data[6] << 8));
            if (!hold_start(sub, ms)) return false;
        } /* fall through: the status */
        case 2:
            memset(&data[4], 0, 28);
            status_fill(&data[4]);
            return true;
#ifdef RGB_MATRIX_ENABLE
        case 3: {
            uint8_t led = data[5];
            if (led != 0xFFu && led >= RGB_MATRIX_LED_COUNT) return false;
            pat_r = data[6]; pat_g = data[7]; pat_b = data[8];
            pat_led = led;
            memset(&data[4], 0, 28);
            data[4] = led; data[5] = pat_r; data[6] = pat_g; data[7] = pat_b;
            return true;
        }
#endif
        case 5:   /* the record's remainder: the omissions per mux row (rowisr.h) */
            memset(&data[4], 0, 28);
            rowisr_record_rows(&data[4]);
            return true;
        case 6:   /* the frame counter's record and the clocks' state (rowisr.h) */
            memset(&data[4], 0, 28);
            rowisr_record_usb(&data[4]);
            return true;
        case 7: {   /* a silence of every interrupt: [5..8] us, [9..10] the delay before it, ms */
            uint32_t us    = (uint32_t)data[5] | (uint32_t)data[6] << 8 | (uint32_t)data[7] << 16 | (uint32_t)data[8] << 24;
            uint16_t delay = (uint16_t)(data[9] | (data[10] << 8));
            if (!silence_arm(us, delay)) return false;
            memset(&data[4], 0, 28);
            status_fill(&data[4]);
            return true;
        }
        case 8:   /* the last silence's report, and its second page */
        case 9:
            memset(&data[4], 0, 28);
            report_fill(sub == 9 ? 1u : 0u, &data[4]);
            return true;
        case 10:   /* the cost of a register read: [4..5] FRMNO, [6..7] INSTS, [8..9] CFG, [10..11] CT16B5's TC, [12..13] a RAM word; HCLK cycles a read */
            memset(&data[4], 0, 28);
            put16(&data[4],  read_trial(&SN32_USB->FRMNO));
            put16(&data[6],  read_trial(&SN32_USB->INSTS));
            put16(&data[8],  read_trial(&SN32_USB->CFG));
            put16(&data[10], read_trial((volatile const uint32_t *)0x4000A004u));
            put16(&data[12], read_trial(&read_sink));
            data[14] = 8; put16(&data[15], 256u);   /* trials, reads (codex on 95ef5cbc5d: the 256 sat one byte high) */
            return true;
#if ROWISR_SECTIONS
        case 11:   /* the handler's sections (rowisr.h); unhandled when not compiled in */
            memset(&data[4], 0, 28);
            rowisr_record_sections(&data[4]);
            return true;
#endif
        case 12:   /* the rig's keys counted at the scan: [5..8] the two positions to set (row, col, row, col; 0xFF keeps); the reply zeroes the counts */
            rowisr_rig_set(data[5], data[6], data[7], data[8]);
            memset(&data[4], 0, 28);
            rowisr_rig_read(&data[4]);
            return true;
        case 13: {   /* the column settle, HCLK ticks: [5..6] a new value (0xFFFF keeps); the reply [4..5] the value in force */
            uint16_t t = (uint16_t)(data[5] | (data[6] << 8));
            if (t != 0xFFFFu) rowisr_settle_set(t);
            memset(&data[4], 0, 28);
            put16(&data[4], rowisr_settle_get());
            return true;
        }
        case 14: {   /* FRMNO read back to back: [5..6] ms to scan (0 reads the state); the reply [4..7] reads, [8..11] steps of +1, [12..15] other
                      * steps, [16..17] ms left, [18] pairs kept, [19..30] three (prev, next) pairs */
            uint16_t ms = (uint16_t)(data[5] | (data[6] << 8));
            memset(&data[4], 0, 28);
            fscan_fill(ms, &data[4]);
            return true;
        }
        default:
            return false;
    }
}

#endif /* AK820_DEV_HOOKS */
