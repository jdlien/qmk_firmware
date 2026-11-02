/* rgb/rowisr.c: the AK820 Pro's RGB matrix driver and key matrix in one file,
 * board-local (plans/ROW-ISR-PLAN.md, D1-D8, D10).
 *
 * Stage 2 (11-01): the hot path. ChibiOS's PWM service is gone from this
 * board (HAL_USE_PWM off, SN32_PWM_USE_CT16B0/1/2 off); the four timers are
 * programmed here by register offset from their bases (D1: the LLD's structs
 * index MR[9] through a five-entry array on this part):
 *
 * - CT16B4 is the slot scheduler (D2): HCLK, PRE 0, MR9 the slot with IE and
 *   RST, no stop. Its match is the row handler, at priority 3 as the old row
 *   ISR was. The slot is fixed; a late handler moves its pulse inside the slot
 *   or omits it, and never lengthens the field. At stage 2 the slot is the
 *   measured stage-0 slot (267 us: 3,744 ISR/s on the base, 11-01), so the
 *   field is unchanged and the move can be measured; stage 5 sets 125 us.
 * - CT16B0, B1 and B2 are one-shot pulse generators with no interrupt: 48 MHz,
 *   PRE 0, the terminal match (B0's and B2's MR9, B1's MR12) with STOP set and
 *   RST clear, every duty match plain. In PWM mode 1 a channel is low (the
 *   LED on; the columns are active low) until its match and high after it, so
 *   a counter parked at its terminal holds every column off: a stall of any
 *   length is dark by hardware. Never STOP and RST together (the plan). Each
 *   slot's restart writes TC = 0 and then TMRCTRL with CEN, for the three
 *   timers in a row, inside one short critical section after the admission
 *   check (D3 step 6).
 * - The columns' duties come through a scale table (D4, D8): MR = scale_lut[byte],
 *   k = 10 at this cadence, calibrated against the stage-0 holds; the nine
 *   positions with no LED get an explicit zero (F-033). A match of 0 is a
 *   constant inactive level (docs/sn32f299.md, "PWM behaviour").
 * - The mux rows are plain GPIO through BSET and BCLR (D3): four stores
 *   deselect every row, one store selects the next.
 * - The key scan is the interim scanner of stage 1 (D1): quantum/matrix.c's
 *   row helpers with I-class locking, every third slot, with the columns
 *   released from the PWM function around it (the control words of D2, stores
 *   not read-modify-writes). Stage 3 replaces it with D3 step 3.
 * - The epoch clock (D3 step 1): one 32-bit extension of CT16B5 kept by two
 *   observers, the 20 kHz tick and this handler, VALID or INVALID with a loss
 *   generation; the handler reconstructs the slots elapsed since its last
 *   service from (system time, scheduler TC) pairs, never from the flag.
 * - The record (D10): the handler's latency and elapsed time in the
 *   scheduler's own ticks, the slots elapsed, serviced, the pulses emitted and
 *   omitted, the longest omission run, the epoch losses; health page 7.
 * - The power accounting (D8): the emitted widths per colour in 64-bit
 *   accumulators, converted in sn32f2xx_led_drive_take() to the model's unit.
 *
 * Kept from stage 1: the exported names (rowisr.h), the frames and the masked
 * flush, the prime in matrix_init_custom(), the nesting hold on the row
 * interrupt's line (now CT16B4's), the quiesce on every bootloader route, the
 * flash-program guard, scan-only for light sleep's sizing.
 */
#include <string.h>
#include <hal.h>
#include "gpio.h"
#include "timer.h"
#include "matrix.h"
#ifdef RGB_MATRIX_ENABLE
#    include "rgb_matrix.h"
#    include "rgb_matrix_drivers.h"
#endif
#include "rowisr.h"
#include "rowisr_diag.h"   /* the quiesce cancels a dev hold-off first (an inline no-op in other builds) */

#define ROWS_PER_HAND (MATRIX_ROWS)
#ifndef MATRIX_IO_DELAY
#    define MATRIX_IO_DELAY 30
#endif
#ifndef MATRIX_INPUT_PRESSED_STATE
#    define MATRIX_INPUT_PRESSED_STATE 0
#endif
#if (DIODE_DIRECTION != COL2ROW)
#    error "rowisr.c scans COL2ROW: a row driven low, the columns read"
#endif
#ifndef MATRIX_UNSELECT_DRIVE_HIGH
#    error "rowisr.c drives a deselected key row high (the shared columns): config.h"
#endif
#if HAL_USE_PWM
#    error "rowisr.c owns CT16B0/B1/B2/B4 by register: HAL_USE_PWM must be off (halconf.h)"
#endif

/* ---- The timers, by register ------------------------------------------------
 *
 * docs/sn32f299.md, section 7: bases 0x40000000 + 0x2000 n; TMRCTRL 0x00 (CEN
 * b0, CRST b1), TC 0x04, PRE 0x08, MCTRL 0x14 (a trio IE/RST/STOP per match;
 * MR9's at bits 21-23 on B0/B2/B3/B4/B5), MCTRL2 0x18 (B1's MR10-12, MR12's
 * trio at bits 6-8), MRn 0x20 + 4n, PWMCTRL 0x98, B1's PWMENB 0xA0 and
 * PWMIOENB 0xA4, RIS 0xA8, IC 0xAC (write 1 to clear). On B0, B2, B4 writes to
 * MCTRL, MRn, EM and PWMCTRL need 0x5A in bits 31:24; B1 has no key. */
#define CT_BASE(n)        (0x40000000UL + 0x2000UL * (n))
#define CT_REG(n, off)    (*(volatile uint32_t *)(CT_BASE(n) + (off)))
#define CT_TMRCTRL        0x00u
#define CT_TC             0x04u
#define CT_PRE            0x08u
#define CT_MCTRL          0x14u
#define CT_MCTRL2         0x18u
#define CT_MR(i)          (0x20u + 4u * (i))
#define CT_PWMCTRL        0x98u
#define CT_PWMENB         0xA0u
#define CT_PWMIOENB       0xA4u
#define CT_RIS            0xA8u
#define CT_IC             0xACu
#define CT_KEY            0x5A000000UL
#define CT_CEN            0x1u
#define CT_CRST           0x2u

#define T_B0    0u
#define T_B1    1u
#define T_B2    2u
#define T_SCHED 4u   /* CT16B4 */
#define T_SYS   5u   /* CT16B5, ChibiOS's system time: HCLK/256, 16 bits */

#define SCHED_IRQn  CT16B4_IRQn
#define SCHED_PRIO  3u   /* as the old row ISR (SN32_PWM_CT16B1_IRQ_PRIORITY), below the 20 kHz tick's 2 */

#define HCLK_HZ               48000000UL
#define ROWISR_SLOT_US        267u                               /* the measured stage-0 slot (11-01) */
#define ROWISR_SLOT_TICKS     (ROWISR_SLOT_US * 48u)             /* 12,816 at HCLK */
#define ROWISR_TERMINAL       4095u                              /* the pulse timers' terminal match */
#define ROWISR_WINDOW_TICKS   (ROWISR_TERMINAL + 1u)             /* 85.3 us: a counter runs to its terminal whatever the duties */
#define ROWISR_ARM_MARGIN     120u                               /* the three arms' skew, the tail and the guard: 2.5 us */
#define ROWISR_ARM_DEADLINE   (ROWISR_SLOT_TICKS - ROWISR_WINDOW_TICKS - ROWISR_ARM_MARGIN)   /* 8,600 ticks, 179 us into the slot */
#define ROWISR_K              10u                                /* the scale: ticks per unit of byte (D4; calibrated, stage 2's gate) */
#define SYS_TICK_HCLK         256u                               /* one CT16B5 tick in HCLK ticks (5.33 us) */
#define EPOCH_INVALID_GAP     1500u                              /* 8 ms of CT16B5 ticks: an observation gap this long is abnormal */
#define EPOCH_CONSUMER_MAX    (3600UL * 187500UL)                /* an hour: the supported consumer interval */

/* MCTRL words: the scheduler's MR9 with IE and RST (bits 21, 22); the pulse
 * timers' terminal with STOP only (B0/B2: bit 23; B1: MCTRL2 bit 8). */
#define SCHED_MCTRL   (CT_KEY | (1UL << 21) | (1UL << 22))
#define PULSE_MCTRL   (CT_KEY | (1UL << 23))
#define B1_MCTRL2     (1UL << 8)
#define RIS_MR9       (1UL << 9)
#define RIS_ALL       0x3FFFUL

/* The PWM control words (D2), stores not read-modify-writes. B0 and B2 keep
 * enables, modes and IO enables in one keyed PWMCTRL: PWMnEN at bit n, the
 * mode at bits 4 + 2n (mode 1 encodes as zero), IOEN at bit 20 + n. B1 has
 * PWMCTRL for the modes (zero: mode 1), PWMENB and PWMIOENB for the enables.
 * The columns: B1 channels 4-11, B0 channels 0-3, B2 channels 1-3. */
#define B0_PWMCTRL_ACTIVE    0x5AF0000FUL
#define B0_PWMCTRL_RELEASED  0x5A00000FUL
#define B2_PWMCTRL_ACTIVE    0x5AE0000EUL
#define B2_PWMCTRL_RELEASED  0x5A00000EUL
#define B1_PWMENB            0x00000FF0UL
#define B1_IOEN_ACTIVE       0x00000FF0UL
#define B1_IOEN_RELEASED     0x00000000UL

/* ---- Configuration ------------------------------------------------------ */

#ifndef RGB_MATRIX_MAXIMUM_BRIGHTNESS
#    define RGB_MATRIX_MAXIMUM_BRIGHTNESS 255
#endif

static const pin_t led_row_pins[SN32F2XX_RGB_MATRIX_ROWS_HW] = SN32F2XX_RGB_MATRIX_ROW_PINS; /* the mux rows: R, B, G per key row, config.h's order (Q6, settled on white 11-01) */
static const pin_t led_col_pins[SN32F2XX_RGB_MATRIX_COLS]    = MATRIX_COL_PINS;             /* shared with the key matrix */
static const pin_t key_row_pins[MATRIX_ROWS]                 = MATRIX_ROW_PINS;

/* Each mux row's port and bit, for one BSET; the four ports' masks for the
 * deselect (D3's table: A8, A9, A11; B4, B5; C10-13; D8-13, D16-18). */
typedef struct {
    ioportid_t port;
    uint32_t   bit;
} mux_sel_t;
#define MUX(p) { PAL_PORT(p), 1UL << PAL_PAD(p) }
static const mux_sel_t mux_sel[SN32F2XX_RGB_MATRIX_ROWS_HW] = {
    MUX(A11), MUX(B4), MUX(B5), MUX(A8), MUX(A9), MUX(D8), MUX(D9), MUX(D10), MUX(D11),
    MUX(D12), MUX(D13), MUX(D16), MUX(D17), MUX(D18), MUX(C10), MUX(C11), MUX(C12), MUX(C13)};
#define MUX_MASK_A 0x00000B00UL
#define MUX_MASK_B 0x00000030UL
#define MUX_MASK_C 0x00003C00UL
#define MUX_MASK_D 0x00073F00UL

/* The columns' match registers, in MATRIX_COL_PINS order (config.h's map:
 * timer, channel), each with its key word: a const table in flash. */
typedef struct {
    volatile uint32_t *mr;
    uint32_t           key;
} col_mr_t;
#define COLMR(t, ch) { (volatile uint32_t *)(CT_BASE(t) + CT_MR(ch)), ((t) == T_B1 ? 0UL : CT_KEY) }
static const col_mr_t col_mr[SN32F2XX_RGB_MATRIX_COLS] = {
    COLMR(T_B1, 4), COLMR(T_B1, 5), COLMR(T_B1, 8), COLMR(T_B2, 1), COLMR(T_B2, 2), COLMR(T_B2, 3),
    COLMR(T_B1, 6), COLMR(T_B1, 7), COLMR(T_B1, 9), COLMR(T_B0, 1), COLMR(T_B0, 0), COLMR(T_B1, 10),
    COLMR(T_B0, 3), COLMR(T_B0, 2), COLMR(T_B1, 11)};

/* The scale table (D8): MR = scale_lut[byte], linear at k = ROWISR_K at this
 * cadence, 256 x 16 bits in flash; every entry strictly below the terminal. */
#define S1(i) (uint16_t)(ROWISR_K * (i))
#define S4(i) S1(i), S1((i) + 1), S1((i) + 2), S1((i) + 3)
#define S16(i) S4(i), S4((i) + 4), S4((i) + 8), S4((i) + 12)
#define S64(i) S16(i), S16((i) + 16), S16((i) + 32), S16((i) + 48)
static const uint16_t scale_lut[256] = {S64(0), S64(64), S64(128), S64(192)};
#if (ROWISR_K * 255u) >= ROWISR_TERMINAL
#    error "every channel match must stay strictly below the terminal (D2)"
#endif

#ifdef RGB_MATRIX_ENABLE
static RGB  led_state[SN32F2XX_LED_COUNT];     /* the front frame: what the handler drives */
static RGB  led_state_buf[SN32F2XX_LED_COUNT]; /* rgb_matrix's: the next frame */
static bool led_state_buf_update_required = false;
#endif

/* The rolling matrix the handler writes and the main loop consumes.
 * matrix_scanned is a DIRTY flag, set when something new has been sampled,
 * cleared by the consume; NOT a scan gate. */
static matrix_row_t  shared_matrix[MATRIX_ROWS];
static volatile bool matrix_scanned = false;
static volatile uint8_t rows_seen = 0;   /* bit per key row: readiness is withheld until every row has been sampled once */
#define SHARED_MATRIX_ALL_ROWS ((uint8_t)((1u << ROWS_PER_HAND) - 1u))

static bool hw_up           = false;   /* rowisr_hw_init() done (matrix_init_custom) */
static bool prime_timed_out = false;   /* the prime's 50 ms passed before the six rows */

volatile bool sn32f2xx_pgm_scan = true;   /* rowisr.h: straight through a flash program (10-27) */
#ifdef AK820_DEV_HOOKS
volatile bool sn32f2xx_scan_skip = false; /* rowisr.h: op 28 sub 4 */
#endif

/* The rotation: the mux row the handler lights next (0..17) and the key row
 * it scans (0..5); the key row advances when the mux row's colour index is 0,
 * the first pin of its group (R, B, G per key row: config.h's order, Q6).
 * The old driver submitted B, G, then R of the NEXT key row, one slot ahead
 * of the pins, and rendered right, so its widths evidently reached the LEDs
 * a cycle after their write (the CT16's doing, not measured). The one-shot
 * restart takes the widths written before it, so stage 2's first flash
 * rendered every frame rotated through that lead: R from B, B from G, G
 * from the row below's R (peach alphas and pink mods from a purple and blue
 * theme, and magenta above each of the nine holes at a white frame; white,
 * 11-01). */
static uint8_t cur_row = 0;
static uint8_t key_row = 0;

/* ---- The epoch clock (D3 step 1) ----------------------------------------
 *
 * One 32-bit extension of CT16B5 (5.33 us a tick, wrapping after 6 h 22 min),
 * kept by observation: read TC, add the raw gap since the stamp, store the
 * stamp. Two observers from interrupt context: the 20 kHz tick
 * (rowisr_time_observe() from indicators.c, priority 2) and the row handler.
 * VALID or INVALID: a raw gap of 8 ms or more since the last observation
 * moves it to INVALID (nothing silences every interrupt that long; such a
 * reading may be a wrap's remainder); deep sleep, degraded mode and the dev
 * lock mode hold it INVALID while they last (exclusions); every transition to
 * INVALID bumps the loss generation, and so does every later discard of
 * time while INVALID (a re-prime refused for its own gap); INVALID becomes
 * VALID only by a re-prime whose gap passes the guard, while no exclusion
 * is active. Each observation is a critical section of a few instructions. */
/* The frame counter's freshness (below, "The USB frame counter"): FRMNO's
 * last seen value and the extension's time then, tracked by both observers
 * while the extension runs; staled wherever the extension discards time
 * (the guard's rejected gap, an exclusion's end), never by the handler's
 * own rejection, which discards nothing. */
#define USB_FRMNO_MASK    0x7FFu          /* the LLD's usbhw.h (mskFRAME_NO, mskBUS_*), not on this file's include path */
#define USB_FRM_FRESH     750u            /* 4 ms of CT16B5 ticks: FRMNO must have moved this recently to be trusted */
#define USB_FRM_PENDING   1500u           /* 8 ms more: a stale's recovery ends without a frame (no host), pending no longer */
static uint16_t frm_seen     = 0xFFFFu;
static uint32_t frm_seen_ext = 0;
static uint8_t  frm_pending  = 0;         /* a stale happened and no frame has moved since: the recovery (the fourth verify review's 1) */

static volatile uint32_t time_ext   = 0;
static volatile uint16_t time_stamp = 0;
static volatile uint8_t  time_valid = 0;
static volatile uint32_t time_gen   = 0;   /* 32 bits (the verify review's 7: 16 could alias after 65,536 losses) */
static volatile uint16_t time_last_gap = 0;   /* the last observation gap the guard rejected, ticks (the record) */
static volatile uint16_t time_guard_rejects = 0;   /* the guard's rejections, wrapping (the report's attribution) */

/* The frames' freshness staled, with a new baseline: FRMNO as it reads now,
 * not fresh; only a frame seen moving after this restores the trust (a
 * movement inside a discarded gap cannot: the second verify review's 1). */
static inline void frm_staleI(void) {
    frm_seen     = (uint16_t)(SN32_USB->FRMNO & USB_FRMNO_MASK);
    frm_seen_ext = time_ext - USB_FRM_FRESH;
    frm_pending  = 1;
}
static volatile uint8_t  time_excl  = 0;   /* ROWISR_EXCL_* bits */

static inline uint16_t sys_tc(void) {
    return (uint16_t)CT_REG(T_SYS, CT_TC);
}

/* I-class or under the system lock. */
static inline void time_observeI(void) {
    uint16_t tc  = sys_tc();
    uint16_t gap = (uint16_t)(tc - time_stamp);
    time_stamp   = tc;
    if (!time_valid) {
        if (time_excl == 0) {   /* the re-prime: the stamp and the high word afresh ... */
            if (gap >= EPOCH_INVALID_GAP) {   /* ... but a guarded observation itself: an unguarded residue (a long silence's wrap
                                               * remainder) must not enter the extension, where it could age the frames' recovery
                                               * out as if 8 ms had passed without a frame (the fifth verify review's 1); more
                                               * time is discarded here, so the generation moves (a baseline a consumer adopted
                                               * while INVALID must not stay continuous across it: the sixth verify review's 1;
                                               * the handler counts at most one loss per entry, so a sleep stays one loss and a
                                               * second silence counts as the second it is), the frames' baseline is taken anew,
                                               * and the next observation, a short gap on, re-primes */
                time_gen++;
                time_last_gap = gap;
                time_guard_rejects++;
                frm_staleI();
                return;
            }
            time_ext  += gap;
            time_valid = 1;
        }
        return;
    }
    if (gap >= EPOCH_INVALID_GAP) {
        time_valid    = 0;
        time_gen++;
        time_last_gap = gap;   /* the rejected gap, for the record */
        time_guard_rejects++;
        frm_staleI();          /* the discarded time stales the frames' freshness (the verify reviews' 1) */
        return;
    }
    time_ext += gap;
}

/* stale_frames: an exclusion discards time (the re-prime after it adds only
 * the gap since the last stamp), so the frames' freshness goes with it; the
 * handler's own rejection discards nothing (the next observation re-primes
 * with its whole gap) and the frames, which disagreed with the extension
 * because they counted the silence, stay trusted (the second verify
 * review's new P1: staling them made the next interval a false rejection). */
static inline void time_invalidateI(bool stale_frames) {
    if (time_valid) {
        time_valid = 0;
        time_gen++;
        if (stale_frames) frm_staleI();
    }
}

/* FRMNO tracked by both observers, so a row-only hold the tick keeps VALID
 * keeps the frames fresh too (a hold across a 2.048 s frame wrap could
 * otherwise return to an equal FRMNO; the verify review). I-class. */
static inline void frm_trackI(void) {
    if (time_excl) return;   /* excluded: the extension is frozen, so a stamp now would read as no age at all (the third verify review's 1) */
    uint16_t frm = (uint16_t)(SN32_USB->FRMNO & USB_FRMNO_MASK);
    if (frm != frm_seen) {
        frm_seen     = frm;
        frm_seen_ext = time_ext;
        frm_pending  = 0;
    } else if (frm_pending && (time_ext - frm_seen_ext) >= USB_FRM_FRESH + USB_FRM_PENDING) {
        frm_pending = 0;   /* 8 ms after the stale and no frame: the bus has no host; the recovery ends, the frames untrusted */
    }
}

void rowisr_time_observe(void) {   /* the 20 kHz tick's observation (indicators.c) */
    chSysLockFromISR();
    time_observeI();
    frm_trackI();
    chSysUnlockFromISR();
}

void rowisr_time_exclude(uint8_t bit, bool on) {   /* thread context: degraded mode, the dev lock mode */
    chSysLock();
    if (on) {
        time_excl |= bit;
        time_invalidateI(true);
    } else {
        uint8_t was = time_excl;
        time_excl &= (uint8_t)~bit;
        if (was != 0 && time_excl == 0) frm_staleI();   /* an exclusion really ended: the re-prime that follows adds only the gap since the last stamp, the excluded time is discarded here (not on clearing a clear mask: the fourth verify review's 4) */
    }
    chSysUnlock();
}

void rowisr_time_read(rowisr_time_t *t) {   /* thread context: the silence's report (rowisr_diag.c); a copy, not an observation */
    chSysLock();
    t->ext           = time_ext;
    t->gen           = time_gen;
    t->stamp         = time_stamp;
    t->last_gap      = time_last_gap;
    t->guard_rejects = time_guard_rejects;
    t->valid         = time_valid;
    chSysUnlock();
}

static inline void time_excludeI(uint8_t bit, bool on) {
    if (on) {
        time_excl |= bit;
        time_invalidateI(true);
    } else {
        uint8_t was = time_excl;
        time_excl &= (uint8_t)~bit;
        if (was != 0 && time_excl == 0) frm_staleI();
    }
}

/* ---- The USB frame counter (D3 step 1, case 5) ------------------------------
 *
 * The host's SOF advances FRMNO (11 bits, 1 kHz) whatever the core does, so
 * on USB a returning silence of every observer that the 8 ms guard cannot see
 * (349.5 ms and more, CT16B5's wrap, which the guard reads as the remainder)
 * is caught after the fact: an interval's span by the extension and its span
 * by the frame counter must agree, modulo 2.048 s, within 3 ms plus 0.2%
 * (HCLK and the host's SOF differ by 0.09% at most: docs/sn32f299.md,
 * "IHRC"). An end of an interval is TRUSTED when the driver is ACTIVE with
 * its transceiver on (USB_CFG.PHY_EN: the LLD clears it at a suspend, and
 * one of its paths returns to ACTIVE without it), no bus-event flag is
 * pending, and FRMNO was seen moving within the last 4 ms of the extension's
 * time, tracked by both observers while the extension runs and staled
 * wherever it discards time, the guard's rejected gap and an exclusion's
 * end, never by the handler's own rejection (the bench's data cut leaves the driver
 * ACTIVE with the frames stopped, sleep.c; the hardware's own suspend needs
 * 3 ms of idle bus). After a stale the frames are in RECOVERY until one is
 * seen moving, 8 ms of guarded time at most. An interval is judged when it
 * began with the frames expected, fresh or in a recovery (its start FRMNO is
 * real either way), and ends fresh with the bus up; it is rejected when the
 * bus is down at its end, a bus event was serviced inside it (a generation
 * the USB handler bumps through usb_event_kb() at every reset, suspend and
 * wakeup it raises) or the frames were moving at its start and not within
 * 4 ms of its end, never healthy, since SOFs stop at a suspend; it is
 * excluded without a loss of its own when both its ends lie in a recovery
 * with no frame yet; otherwise its two spans must agree. A rejected interval is INVALID with one loss, as the
 * guard's: the extension invalidated, the generation bumped and adopted by
 * the committed baseline (only when no other observer invalidated first:
 * that observer's loss is then the one), so the next observation re-primes
 * and the next interval starts fresh; nothing is replayed. Off USB nothing
 * is judged: the declared blind spot. Outside the guarantee, and said so in
 * the plan: SOFs that stop with no flag raised inside the 4 ms before a
 * silence that aliases the guard. The proof's scope is 13 s (the plan, D3):
 * past it the check still runs, and the two moduli can come within the
 * tolerance. */
static volatile uint16_t usb_gen = 0;   /* bus events the USB handler serviced: reset, suspend, wakeup */
#define USB_INSTS_WAKEUP  (1UL << 25)
#define USB_INSTS_RESUME  (1UL << 29)
#define USB_INSTS_SUSPEND (1UL << 30)
#define USB_INSTS_RESET   (1UL << 31)
#define USB_BUS_EVENTS    (USB_INSTS_RESET | USB_INSTS_SUSPEND | USB_INSTS_RESUME | USB_INSTS_WAKEUP)
#define USB_CFG_PHY_EN    (1UL << 30)     /* USB_CFG.PHY_EN, the transceiver on (sleep.c, F-045) */

void usb_event_kb(usbevent_t event);
void usb_event_kb(usbevent_t event) {   /* the USB handler (ISR context): tmk_core/protocol/chibios/usb_main.c */
    if (event == USB_EVENT_RESET || event == USB_EVENT_SUSPEND || event == USB_EVENT_WAKEUP) usb_gen++;
}

/* A coherent snapshot of the clocks: the extension observed, the scheduler's
 * TC, the generation and the validity (step 6 and the exit take this one);
 * step 1's adds the frame counter with its trust and the bus-event
 * generation, four more loads and the freshness bookkeeping, in the same
 * critical section. */
typedef struct {
    uint32_t ext;
    uint32_t gen;
    uint16_t tc;
    uint16_t frm;      /* FRMNO at the observation (step 1's snapshot) */
    uint16_t ugen;     /* the bus-event generation (step 1's) */
    uint8_t  valid;
    uint8_t  bus;      /* the bus up at this end: the driver ACTIVE, the transceiver on, no bus-event flag pending (step 1's) */
    uint8_t  fresh;    /* a frame seen moving within the last 4 ms (step 1's) */
    uint8_t  pending;  /* a stale's recovery: no frame seen since it, for 8 ms at most (step 1's) */
} snap_t;

static inline void take_timeI(snap_t *s) {
    time_observeI();
    s->ext   = time_ext;
    s->tc    = (uint16_t)CT_REG(T_SCHED, CT_TC);
    s->gen   = time_gen;
    s->valid = time_valid;
    s->frm     = 0;
    s->ugen    = 0;
    s->bus     = 0;
    s->fresh   = 0;
    s->pending = 0;
}

static inline void take_snapI(snap_t *s) {
    take_timeI(s);
    frm_trackI();
    s->frm     = frm_seen;
    s->ugen    = usb_gen;
    s->bus     = (USBD1.state == USB_ACTIVE && (SN32_USB->CFG & USB_CFG_PHY_EN) != 0 && (SN32_USB->INSTS & USB_BUS_EVENTS) == 0) ? 1u : 0u;
    s->fresh   = (time_ext - frm_seen_ext) < USB_FRM_FRESH ? 1u : 0u;
    s->pending = frm_pending;
}

/* The slot boundaries crossed from a to b: round((dt - dphase) / slot), dt the
 * extension's difference in HCLK ticks (x 256), dphase the scheduler TC's.
 * Subtracting the phase makes the quotient an exact multiple of the slot to
 * within the resolution, so the rounding is unambiguous. Only when both are
 * VALID in the same generation; the caller checks. 32-bit arithmetic below
 * ~44 s of gap, 64-bit above it (up to the hour). */
static uint32_t slot_ticks = ROWISR_SLOT_TICKS;   /* the scheduler's period in force: the slot, or scan-only's (codex, P2 5) */

static uint32_t boundaries_between(const snap_t *a, const snap_t *b) {
    uint32_t dext   = b->ext - a->ext;
    int32_t  dphase = (int32_t)b->tc - (int32_t)a->tc;
    uint32_t half   = slot_ticks / 2u;
    if (dext < 0x700000UL) {   /* 7.3 M ticks, 39 s: dext x 256 plus any phase stays inside int32 (codex, P2 7) */
        int32_t dt = (int32_t)(dext * SYS_TICK_HCLK) - dphase;
        if (dt < 0) dt = 0;
        return ((uint32_t)dt + half) / slot_ticks;
    }
    int64_t dt = (int64_t)dext * SYS_TICK_HCLK - dphase;
    if (dt < 0) dt = 0;
    return (uint32_t)(((uint64_t)dt + half) / slot_ticks);
}

/* Whether a slot boundary lies between a and b: step 6's crossed check,
 * inside the masked section, so a comparison and a shift, never
 * boundaries_between()'s divide (a software __udivsi3 on this core; the
 * verify review's timing concern, 11-01). Same validity rule as above. */
static inline bool crossed_between(const snap_t *a, const snap_t *b) {
    uint32_t dext = b->ext - a->ext;
    if (dext >= 0x700000UL) return true;   /* 39 s and more: crossed many times over */
    int32_t dphase = (int32_t)b->tc - (int32_t)a->tc;
    int32_t dt     = (int32_t)(dext * SYS_TICK_HCLK) - dphase;
    if (dt < 0) dt = 0;
    return (uint32_t)dt + (slot_ticks >> 1) >= slot_ticks;
}

/* ---- The record (D10) ------------------------------------------------------ */
static struct {
    uint32_t slots_elapsed;    /* boundaries crossed, summed over services with a VALID interval */
    uint32_t serviced;         /* every service of an epoch (scan-only and the guard included) */
    uint32_t serviced_valid;   /* the services whose interval since the last was VALID: the population of slots_elapsed */
    uint32_t emitted;          /* lit pulses armed */
    uint32_t omitted;          /* lit rows left dark: late for the arm, or preempted across a slot; a dark row is neither */
    uint32_t elapsed_sum;      /* entry snapshot to exit snapshot, HCLK ticks, wrapping after 89 s of handler time: windows, not lifetimes */
    uint32_t elapsed_samples;  /* the population of elapsed_sum */
    uint16_t elapsed_min, elapsed_max;   /* the maximum clips at 0xFFFF (1,365 us) */
    uint16_t latency_max;      /* entry TC, services with exactly one boundary crossed */
    uint16_t omit_run, omit_run_max;
    uint8_t  epoch_losses;     /* generation changes the handler saw, saturating */
    uint8_t  invalid_services; /* services whose interval was INVALID, saturating */
    uint8_t  spurious;         /* invocations inside the epoch already serviced, saturating */
    uint8_t  late_omit;        /* omissions by the arm deadline (a subset of omitted), saturating */
    uint8_t  omit_row[SN32F2XX_RGB_MATRIX_ROWS_HW];   /* omissions per mux row (row and colour), saturating */
    uint32_t usb_checked;      /* intervals the frame counter judged: VALID, the driver ACTIVE at both ends, no bus event */
    uint16_t usb_worst_ms;     /* the worst modular distance among the intervals it accepted, ms */
    uint16_t usb_last_dt_ms, usb_last_df;   /* the last rejected interval's two spans: the extension's and the frame counter's, ms */
    uint8_t  usb_rejects;      /* intervals the frame counter rejected: the spans disagree, saturating */
    uint8_t  usb_events;       /* intervals rejected for a bus event inside them, serviced or pending, or the frames stopped, saturating */
    uint8_t  usb_unjudged;     /* intervals excluded without a loss: both ends inside a stale's recovery, no frame yet, saturating */
} rec;
static uint32_t seen_gen = 0;        /* the loss generation the handler last saw (the losses, across re-primes) */
static bool     hw_fault = false;    /* a timer's reset never completed at init: nothing started, the columns released */

static inline void sat8(uint8_t *c) {
    if (*c != 0xFFu) (*c)++;
}
static uint64_t width_acc[3] = {0};   /* emitted width sums per colour, ticks: r, g, b (D8) */

static snap_t  last_snap;          /* the last serviced invocation's step-1 pair */
static bool    have_last = false;
static bool     adopt_pending = false;   /* a frame-counter rejection's generation, for the committed baseline (this invocation only) */
static uint32_t adopt_gen     = 0;

void rowisr_record_reset(void) {
    chSysLock();
    memset(&rec, 0, sizeof(rec));
    rec.elapsed_min = 0xFFFFu;
    chSysUnlock();
}

void rowisr_record_fill(uint8_t *out28) {   /* page 7: the totals (health.h) */
    chSysLock();
    uint32_t se = rec.slots_elapsed, sv = rec.serviced_valid, em = rec.emitted, om = rec.omitted;
    uint32_t es = rec.elapsed_sum, ns = rec.elapsed_samples;
    uint16_t lm = rec.latency_max, emx = rec.elapsed_max;
    chSysUnlock();
    uint8_t *p = out28;
#define PUT32(x) do { uint32_t v_ = (x); *p++ = v_ & 0xFF; *p++ = (v_ >> 8) & 0xFF; *p++ = (v_ >> 16) & 0xFF; *p++ = (v_ >> 24) & 0xFF; } while (0)
#define PUT16(x) do { uint16_t v_ = (x); *p++ = v_ & 0xFF; *p++ = (v_ >> 8) & 0xFF; } while (0)
    PUT32(se); PUT32(sv); PUT32(em); PUT32(om);   /* 16 */
    PUT32(es); PUT32(ns);                         /* 24 */
    PUT16(lm); PUT16(emx);                        /* 28 */
#undef PUT16
#undef PUT32
}

void rowisr_record_rows(uint8_t *out28) {   /* op 28 sub 5: the rest (rowisr_diag.h) */
    chSysLock();
    uint16_t orm = rec.omit_run_max, emn = rec.elapsed_min;
    uint32_t sv  = rec.serviced;
    uint8_t  sp = rec.spurious, lo = rec.late_omit, el = rec.epoch_losses, iv = rec.invalid_services;
    uint8_t  rows[SN32F2XX_RGB_MATRIX_ROWS_HW];
    memcpy(rows, rec.omit_row, sizeof(rows));
    chSysUnlock();
    out28[0] = orm & 0xFF; out28[1] = orm >> 8;
    out28[2] = sp; out28[3] = lo; out28[4] = el; out28[5] = iv;
    emn = emn == 0xFFFFu ? 0u : emn;
    out28[6] = emn & 0xFF; out28[7] = emn >> 8;
    memcpy(&out28[8], rows, SN32F2XX_RGB_MATRIX_ROWS_HW);          /* 8..25 */
    out28[26] = sv & 0xFF; out28[27] = (sv >> 8) & 0xFF;            /* every service, low 16 bits */
}

void rowisr_record_usb(uint8_t *out28) {   /* op 28 sub 6: the frame counter's record and the clocks' state (rowisr_diag.h) */
    chSysLock();
    uint32_t ck = rec.usb_checked, gen = time_gen;
    uint16_t wm = rec.usb_worst_ms, ldt = rec.usb_last_dt_ms, ldf = rec.usb_last_df, ug = usb_gen;
    uint8_t  rj = rec.usb_rejects, ev = rec.usb_events, uj = rec.usb_unjudged, tv = time_valid, tx = time_excl;
    uint16_t frm = (uint16_t)(SN32_USB->FRMNO & USB_FRMNO_MASK);
    uint8_t  st  = (uint8_t)USBD1.state;
    uint8_t  fresh = (time_ext - frm_seen_ext) < USB_FRM_FRESH ? 1u : 0u;
    uint8_t  live = (USBD1.state == USB_ACTIVE && (SN32_USB->CFG & USB_CFG_PHY_EN) != 0 &&
                     (SN32_USB->INSTS & USB_BUS_EVENTS) == 0 && fresh) ? 1u : 0u;
    uint8_t  pend = frm_pending;
    uint16_t lgap = time_last_gap;
    chSysUnlock();
    uint8_t *p = out28;
#define PUT32(x) do { uint32_t v_ = (x); *p++ = v_ & 0xFF; *p++ = (v_ >> 8) & 0xFF; *p++ = (v_ >> 16) & 0xFF; *p++ = (v_ >> 24) & 0xFF; } while (0)
#define PUT16(x) do { uint16_t v_ = (x); *p++ = v_ & 0xFF; *p++ = (v_ >> 8) & 0xFF; } while (0)
    PUT32(ck); *p++ = rj; *p++ = ev; PUT16(wm);   /* 8 */
    PUT16(ldt); PUT16(ldf); PUT32(gen);           /* 16 */
    PUT16(ug); *p++ = st; *p++ = live;            /* 20 */
    PUT16(frm); *p++ = tv; *p++ = tx;             /* 24 */
    PUT16(lgap); *p++ = fresh; *p++ = (uint8_t)(uj | (pend ? 0x80u : 0u));   /* 28: the unjudged count, bit 7 the recovery pending now */
#undef PUT16
#undef PUT32
}

bool sn32f2xx_hw_fault(void) {
    return hw_fault;
}

/* ---- The key matrix's row helpers (the interim scanner, D1) ----------------
 *
 * From quantum/matrix.c, COL2ROW, with I-class locking. Each select or
 * unselect re-asserts output mode (a read-modify-write of the port's MODE,
 * idempotent on a pin that is already an output) and writes the level; the
 * clock chip's transaction holds this interrupt off for its own MODE writes
 * (rtc/rtc.c, sn32f2xx_row_irq_hold). Stage 3 removes the mode writes. */
static inline void select_row(uint8_t row) {
    chSysLockFromISR();
    gpio_set_pin_output(key_row_pins[row]);
    gpio_write_pin_low(key_row_pins[row]);
    chSysUnlockFromISR();
}

static inline void unselect_row(uint8_t row) {   /* MATRIX_UNSELECT_DRIVE_HIGH: the shared columns */
    chSysLockFromISR();
    gpio_set_pin_output(key_row_pins[row]);
    gpio_write_pin_high(key_row_pins[row]);
    chSysUnlockFromISR();
}

static inline uint8_t read_matrix_pin(pin_t pin) {
    return (gpio_read_pin(pin) == MATRIX_INPUT_PRESSED_STATE) ? 0 : 1;
}

/* The settle after a deselect, as the old driver had it: TIME_US2I(30)
 * iterations of a compiler barrier, about six. Q1's sweep sets stage 3's. */
void matrix_output_unselect_delay(uint8_t line, bool key_pressed) {
    (void)line;
    (void)key_pressed;
    for (int i = 0; i < TIME_US2I(MATRIX_IO_DELAY); ++i) {
        __asm__ volatile("" ::: "memory");
    }
}

static void matrix_read_cols_on_row(matrix_row_t current_matrix[], uint8_t current_row_idx) {
    matrix_row_t current_row_value = 0;
    select_row(current_row_idx);
    matrix_output_select_delay();   /* matrix_common.c: waitInputPinDelay(), 12 cycles */
    matrix_row_t row_shifter = MATRIX_ROW_SHIFTER;
    for (uint8_t col_index = 0; col_index < MATRIX_COLS; col_index++, row_shifter <<= 1) {
        current_row_value |= read_matrix_pin(led_col_pins[col_index]) ? 0 : row_shifter;
    }
    unselect_row(current_row_idx);
    matrix_output_unselect_delay(current_row_idx, current_row_value != 0);
    current_matrix[current_row_idx] = current_row_value;
}

/* Per-row input instrumentation (health.c defines the strong ones). */
__attribute__((weak)) void input_note_row_scan(uint8_t row) { (void)row; }
__attribute__((weak)) void input_note_consume(const matrix_row_t *raw, const matrix_row_t *fresh, uint8_t rows) {
    (void)raw; (void)fresh; (void)rows;
}
/* The handler's bracket (health.c, page 4's callback metric, kept for the
 * before/after comparison; the record above is the stage-2 instrument). */
__attribute__((weak)) void sn32f2xx_isr_enter_hook(void) {}
__attribute__((weak)) void sn32f2xx_isr_exit_hook(void) {}

static void scan_key_row(uint8_t row) {
    matrix_read_cols_on_row(shared_matrix, row);
    input_note_row_scan(row);
    if (rows_seen != SHARED_MATRIX_ALL_ROWS) {
        rows_seen |= (uint8_t)(1u << row);
        if (rows_seen != SHARED_MATRIX_ALL_ROWS) return;   /* not complete yet */
    }
    matrix_scanned = true;   /* dirty flag: something new since last consume */
}

/* ---- The pins and the control words ----------------------------------------- */

static inline void mux_deselect_all(void) {
    palClearPort(GPIOA, MUX_MASK_A);
    palClearPort(GPIOB, MUX_MASK_B);
    palClearPort(GPIOC, MUX_MASK_C);
    palClearPort(GPIOD, MUX_MASK_D);
}

static inline void mux_select(uint8_t row) {
    palSetPort(mux_sel[row].port, mux_sel[row].bit);
}

/* The columns released from the PWM function (plain inputs with their
 * pull-ups, for the scan) and taken back (the parked counters hold them
 * inactive): three stores each. */
static inline void columns_release(void) {
    CT_REG(T_B0, CT_PWMCTRL)  = B0_PWMCTRL_RELEASED;
    CT_REG(T_B2, CT_PWMCTRL)  = B2_PWMCTRL_RELEASED;
    CT_REG(T_B1, CT_PWMIOENB) = B1_IOEN_RELEASED;
}

static inline void columns_active(void) {
    CT_REG(T_B0, CT_PWMCTRL)  = B0_PWMCTRL_ACTIVE;
    CT_REG(T_B2, CT_PWMCTRL)  = B2_PWMCTRL_ACTIVE;
    CT_REG(T_B1, CT_PWMIOENB) = B1_IOEN_ACTIVE;
}

/* A pulse timer parked: stopped, its counter at the terminal, so every
 * channel's output is past its match and inactive. */
static inline void pulse_park(uint32_t t) {
    CT_REG(t, CT_TMRCTRL) = 0;
    CT_REG(t, CT_TC)      = ROWISR_TERMINAL;
}

static inline void pulse_arm_all(void) {   /* TC = 0 then CEN, each; a stop match clears CEN */
    CT_REG(T_B0, CT_TC) = 0;
    CT_REG(T_B1, CT_TC) = 0;
    CT_REG(T_B2, CT_TC) = 0;
    CT_REG(T_B0, CT_TMRCTRL) = CT_CEN;
    CT_REG(T_B1, CT_TMRCTRL) = CT_CEN;
    CT_REG(T_B2, CT_TMRCTRL) = CT_CEN;
}

static bool timer_reset(uint32_t t) {   /* CRST is synchronous and self-clearing; false if it never clears */
    CT_REG(t, CT_TMRCTRL) = CT_CRST;
    for (uint32_t i = 0; i < 1000u; i++) {
        if (!(CT_REG(t, CT_TMRCTRL) & CT_CRST)) return true;
        __asm__ volatile("" ::: "memory");
    }
    return false;
}

/* ---- Scan-only (dev, light sleep's sizing) ---------------------------------- */
#ifdef SN32F2XX_SLEEP_HOOKS
#    ifndef SN32F2XX_SCAN_ONLY_TICKS
#        define SN32F2XX_SCAN_ONLY_TICKS 30000u   /* 625 us at HCLK: each key every 3.75 ms, under sym_defer_pk's 5 ms */
#    endif
static volatile bool scan_only_req = false;
static bool          scan_only     = false;
static uint8_t       scan_only_row = 0;
void sn32f2xx_set_scan_only(bool on) {
    scan_only_req = on;
}
bool sn32f2xx_scan_only_active(void) {
    return scan_only;
}
#else
void sn32f2xx_set_scan_only(bool on) { (void)on; }
bool sn32f2xx_scan_only_active(void) { return false; }
#endif

static inline void sched_set_period(uint32_t ticks) {   /* I-class; MR9 = ticks - 1, TC restarted so a shorter period cannot be passed */
    slot_ticks                = ticks;
    CT_REG(T_SCHED, CT_MR(9)) = CT_KEY | (ticks - 1u);
    CT_REG(T_SCHED, CT_TC)    = 0;
}

/* A service's commit: the serviced epoch advances here, and the statistics
 * that depend on the interval's validity are taken only now (codex, P2 4
 * and 6). Called under the lock from step 6, unlocked on the paths that
 * arm nothing (the record's reader masks this handler). */
static inline void service_commit(const snap_t *s1, bool valid1, uint32_t boundaries) {
    last_snap = *s1;
    if (adopt_pending) {   /* the frame counter rejected this interval and bumped the generation itself: the baseline takes it, so the next
                            * interval starts fresh and is judged as any other (the frames were not staled by the rejection, so it cannot be a
                            * second verdict on the same silence; left unjudged it could hide a second one: the third verify review's 3) */
        last_snap.gen = adopt_gen;
        adopt_pending = false;
    }
    have_last = true;
    rec.serviced++;
    if (valid1) {
        rec.slots_elapsed += boundaries;
        rec.serviced_valid++;
        if (boundaries == 1 && s1->tc > rec.latency_max) rec.latency_max = s1->tc;
    }
}

/* ---- The handler (D3) ------------------------------------------------------- */

static void rowisr_slot(void) {
    sn32f2xx_isr_enter_hook();
    /* 1. The flag and the entry snapshot, coherent: acknowledged and
     * captured in one section, so a boundary crossed between the two cannot
     * leave a flagged epoch the snapshot already belongs to (codex, P2 4). */
    snap_t s1;
    chSysLockFromISR();
    uint32_t ris = CT_REG(T_SCHED, CT_RIS);
    CT_REG(T_SCHED, CT_IC) = ris;
    take_snapI(&s1);
    chSysUnlockFromISR();
    adopt_pending = false;
    if (s1.gen != seen_gen) {   /* a loss since the handler last looked, whichever observer saw it, re-primed or not */
        seen_gen = s1.gen;
        sat8(&rec.epoch_losses);
    }
    bool valid1 = have_last && s1.valid && last_snap.valid && s1.gen == last_snap.gen &&
                  (s1.ext - last_snap.ext) <= EPOCH_CONSUMER_MAX;
    uint32_t boundaries = 1;
    if (valid1) {
        boundaries = boundaries_between(&last_snap, &s1);
        if (boundaries == 0) {   /* inside the epoch already serviced: count and leave, before any verdict
                                  * on the interval (the epoch's ownership is not a statistic; the review's P1 3) */
            sat8(&rec.spurious);
            sn32f2xx_isr_exit_hook();
            return;
        }
    }
    if (valid1 && last_snap.bus && (last_snap.fresh || last_snap.pending)) {
        /* D3 case 5: the frame counter's verdict on an interval that began with
         * the frames expected: trusted (fresh), or inside a stale's recovery
         * (the start's FRMNO is real either way: the stale took it from the
         * hardware). Not judged when the bus is down or the frames are known
         * stopped at the start: the declared blind spot. */
        bool     reject  = false, unjudged = false;
        uint32_t dext  = s1.ext - last_snap.ext;                      /* 5.33 us ticks; under an hour, so x2 fits */
        uint32_t dt_ms = (dext * 2u + 187u) / 375u;                   /* the extension's span, ms, rounded */
        uint32_t df    = (uint32_t)(s1.frm - last_snap.frm) & USB_FRMNO_MASK;   /* the frame counter's, modulo 2,048 */
        if (!s1.bus || s1.ugen != last_snap.ugen) {   /* the bus down at the end, or a bus event serviced inside: never healthy */
            sat8(&rec.usb_events);
            reject = true;
        } else if (!s1.fresh) {
            if (last_snap.fresh) {   /* moving at the start, not within 4 ms of the end: the frames stopped inside, no flag raised */
                sat8(&rec.usb_events);
                reject = true;
            } else {                 /* both ends inside the recovery with no frame yet (its first slots, or a bus that never
                                      * sends one): unjudgeable, excluded without a loss of its own, the stale's loss covers it;
                                      * a silence inside is excluded with it, never healthy (the fourth verify review's 1) */
                unjudged = true;
            }
        } else {
            uint32_t diff = (dt_ms - df) & USB_FRMNO_MASK;
            if (diff > 1024u) diff = 2048u - diff;                    /* the modular distance */
            rec.usb_checked++;
            reject = diff > 3u + (dt_ms >> 9);                        /* 3 ms plus the span / 512, about 0.2% */
            if (reject) sat8(&rec.usb_rejects);
            else if (diff > rec.usb_worst_ms) rec.usb_worst_ms = (uint16_t)diff;
        }
        if (reject) {
            rec.usb_last_dt_ms = dt_ms > 0xFFFFu ? 0xFFFFu : (uint16_t)dt_ms;
            rec.usb_last_df    = (uint16_t)df;
            /* INVALID with one loss, as the guard's. The generation is bumped
             * only if no other observer has since the snapshot (the generation
             * unchanged: a tick's invalidation, re-primed or not, is that
             * observer's loss, counted once at the next entry by the
             * generation check, and this baseline is not adopted, since its
             * time predates a gap the tick discarded; the review's P2 4). The
             * snapshot stays as taken, so step 6 and the exit see the
             * generation moved and measure nothing across it. */
            chSysLockFromISR();
            bool fresh = time_valid != 0 && time_gen == s1.gen;   /* no other observer invalidated since the snapshot, re-primed or not */
            if (fresh) {
                time_invalidateI(false);   /* nothing discarded, the frames stay trusted (time_invalidateI's comment) */
                adopt_gen = time_gen;
            }
            chSysUnlockFromISR();
            if (fresh) {
                seen_gen      = adopt_gen;
                adopt_pending = true;
                sat8(&rec.epoch_losses);
            }
            valid1 = false;   /* an invalid service below; the next observation re-primes */
        } else if (unjudged) {
            if (rec.usb_unjudged < 127u) rec.usb_unjudged++;   /* seven bits on the wire: bit 7 is the recovery flag */
            valid1 = false;   /* excluded: an invalid service below, no loss, nothing adopted; the baseline moves on */
        }
    }
    if (!valid1 && have_last) sat8(&rec.invalid_services);

    /* 2. Deselect every mux row (the outputs are parked: belt and braces). */
    mux_deselect_all();

#ifdef SN32F2XX_SLEEP_HOOKS
    /* Scan-only: applied here, between slots. The invocation ends with the
     * cadence change, so nothing is reconstructed across it (codex, P2 5),
     * and the columns' ownership follows the mode at once: released
     * throughout scan-only, taken back before any ordinary arm (codex, P1). */
    if (scan_only_req != scan_only) {
        scan_only = scan_only_req;
        chSysLockFromISR();
        sched_set_period(scan_only ? SN32F2XX_SCAN_ONLY_TICKS : ROWISR_SLOT_TICKS);
        have_last = false;
        chSysUnlockFromISR();
        if (scan_only) columns_release(); else columns_active();
        sn32f2xx_isr_exit_hook();
        return;
    }
    if (scan_only) {
        if (EFLD1.state != FLASH_PGM || sn32f2xx_pgm_scan) {
            if (++scan_only_row >= ROWS_PER_HAND) scan_only_row = 0;
            scan_key_row(scan_only_row);
        }
        service_commit(&s1, valid1, boundaries);
        sn32f2xx_isr_exit_hook();
        return;
    }
#endif

    if (EFLD1.state == FLASH_PGM && !sn32f2xx_pgm_scan) {
        /* The 09-01 guard, only where sn32f2xx_pgm_scan is false (a dev
         * build's write test): a program in progress, the board dark for it
         * (the rows are deselected above and nothing is armed). */
        service_commit(&s1, valid1, boundaries);
        sn32f2xx_isr_exit_hook();
        return;
    }

    /* The rotation: the next mux row; every third one a new key row (the
     * pins' order: see cur_row's comment). */
    if (++cur_row >= SN32F2XX_RGB_MATRIX_ROWS_HW) cur_row = 0;
    uint8_t colour = cur_row % SN32F2XX_RGB_MATRIX_ROW_CHANNELS;   /* 0 R, 1 B, 2 G of this key row */
    bool    scan   = false;
    if (colour == 0) {
        if (++key_row >= ROWS_PER_HAND) key_row = 0;
        scan = true;
    }

    /* 3. The scan slot (the interim scanner): the columns released around it. */
    if (scan) {
        columns_release();
#ifdef AK820_DEV_HOOKS
        if (!sn32f2xx_scan_skip)   /* op 28 sub 4: the LED work alone, to attribute the scan's cost */
#endif
        scan_key_row(key_row);
        columns_active();
    }

    /* 4. The fifteen match registers for the next mux row, through the scale
     * table, zeros for the nine holes. The sum of the widths feeds D8. */
    uint32_t width_sum = 0;
#ifdef RGB_MATRIX_ENABLE
    const uint8_t *co = g_led_config.matrix_co[key_row];
    for (uint8_t c = 0; c < SN32F2XX_RGB_MATRIX_COLS; c++) {
        uint8_t  li = co[c];
        uint32_t w  = 0;
        if (li < SN32F2XX_LED_COUNT) {
            uint8_t b = (colour == 0) ? led_state[li].r : (colour == 1) ? led_state[li].b : led_state[li].g;
            w         = scale_lut[b];
        }
        *col_mr[c].mr = col_mr[c].key | w;
        width_sum += w;
    }
#else
    for (uint8_t c = 0; c < SN32F2XX_RGB_MATRIX_COLS; c++) *col_mr[c].mr = col_mr[c].key;
#endif

    /* 5. Select the mux row for it (the outputs are parked: this lights
     * nothing until the restart). A row with every width zero stays
     * deselected: no transistor base current for a dark row (the old
     * driver's enable_pwm_output; F-033's phantom was the exception). */
    if (width_sum) mux_select(cur_row);

    /* 6. Inside one critical section: a fresh snapshot against step 1's; a
     * boundary crossed meanwhile means the work ran across a slot (no arm,
     * nothing committed, the flag left for the pending interrupt, which
     * services the new epoch and reconstructs this one as missed);
     * otherwise the arm admitted against the whole terminal window, and the
     * serviced epoch committed here, at the decision. */
    bool armed = false, crossed;
    chSysLockFromISR();
    snap_t s6;
    take_timeI(&s6);
    crossed = (CT_REG(T_SCHED, CT_RIS) & RIS_MR9) != 0;
    if (!crossed && s1.valid && s6.valid && s6.gen == s1.gen && crossed_between(&s1, &s6)) crossed = true;
    if (!crossed) {
        if (width_sum && s6.tc <= ROWISR_ARM_DEADLINE) {
            pulse_arm_all();
            armed = true;
        }
        service_commit(&s1, valid1, boundaries);
    }
    chSysUnlockFromISR();

    /* The pulse's account (D8, D10): a dark row emits nothing and omits
     * nothing whatever the timing (codex, P2 9); a lit row armed is
     * emitted, one late or crossed is omitted, per mux row. */
    if (width_sum) {
        if (armed) {
            rec.emitted++;
            width_acc[colour == 0 ? 0 : (colour == 1 ? 2 : 1)] += width_sum;   /* r, g, b from the pins' R, B, G */
            rec.omit_run = 0;
        } else {
            rec.omitted++;
            sat8(&rec.omit_row[cur_row]);
            if (!crossed) sat8(&rec.late_omit);
            if (++rec.omit_run > rec.omit_run_max) rec.omit_run_max = rec.omit_run;
            if (!crossed) mux_deselect_all();   /* late: leave the row dark (a crossed one is deselected by the re-entry) */
        }
    }

    /* The exit snapshot: the handler's elapsed time, entry snapshot to exit
     * snapshot (the port's prologue and epilogue, the health hooks and this
     * accounting lie outside it: page 4's hooks bracket the whole handler
     * at 5.33 us resolution), both ends VALID in one generation. */
    snap_t sx;
    chSysLockFromISR();
    take_snapI(&sx);
    chSysUnlockFromISR();
    bool dur_ok = s1.valid && sx.valid && sx.gen == s1.gen;
    if (dur_ok && s1.bus && (s1.fresh || s1.pending)) {   /* an aliasing interruption inside the handler is judged at the next entry; this span too, before its sample is taken (the verify review's 4) */
        if (!sx.bus || !sx.fresh || sx.ugen != s1.ugen) {
            dur_ok = false;
        } else {
            uint32_t dt_ms = ((sx.ext - s1.ext) * 2u + 187u) / 375u;
            uint32_t diff  = (dt_ms - ((uint32_t)(sx.frm - s1.frm) & USB_FRMNO_MASK)) & USB_FRMNO_MASK;
            if (diff > 1024u) diff = 2048u - diff;
            if (diff > 3u + (dt_ms >> 9)) dur_ok = false;
        }
    }
    if (dur_ok) {
        uint32_t b2 = boundaries_between(&s1, &sx);
        uint32_t el = b2 * slot_ticks + (uint32_t)((int32_t)sx.tc - (int32_t)s1.tc);
        rec.elapsed_sum += el;
        rec.elapsed_samples++;
        if (el > rec.elapsed_max) rec.elapsed_max = el > 0xFFFFu ? 0xFFFFu : (uint16_t)el;
        if (el < rec.elapsed_min) rec.elapsed_min = (uint16_t)el;
    }
    sn32f2xx_isr_exit_hook();
}

OSAL_IRQ_HANDLER(SN32_CT16B4_HANDLER) {
    OSAL_IRQ_PROLOGUE();
    rowisr_slot();
    OSAL_IRQ_EPILOGUE();
}

/* ---- Sleep (D7) ---------------------------------------------------------------
 *
 * Deep sleep (sleep.c), I-class: the scheduler's interrupt off and its pending
 * state cleared, the columns released, every mux row deselected, the pulse
 * timers parked and the scheduler stopped; the columns are left as pull-up
 * inputs for the wake. The epoch clock INVALID until the first tick after
 * the wake re-primes it. At the exit: the columns back, the scheduler
 * restarted from zero, its flag cleared and its interrupt on; the dark frame
 * kept until the wake's normal rendering lights it. */
void sn32f2xx_sleep_enterI(void) {
    rowisr_diag_cancelI();   /* dev: a silence armed while the tick was already stopped would otherwise fire after the wake (the review's P1 2) */
    NVIC_DisableIRQ(SCHED_IRQn);
    CT_REG(T_SCHED, CT_TMRCTRL) = 0;
    CT_REG(T_SCHED, CT_IC)      = RIS_ALL;
    NVIC_ClearPendingIRQ(SCHED_IRQn);
    columns_release();
    mux_deselect_all();
    pulse_park(T_B0);
    pulse_park(T_B1);
    pulse_park(T_B2);
    time_excludeI(ROWISR_EXCL_SLEEP, true);
    have_last = false;
}

void sn32f2xx_sleep_exitI(void) {
#ifdef SN32F2XX_SLEEP_HOOKS
    if (scan_only) columns_release(); else
#endif
    columns_active();
    CT_REG(T_SCHED, CT_TC) = 0;
    CT_REG(T_SCHED, CT_IC) = RIS_ALL;
    NVIC_ClearPendingIRQ(SCHED_IRQn);
    time_excludeI(ROWISR_EXCL_SLEEP, false);   /* the first tick after the wake re-primes */
    have_last = false;
    CT_REG(T_SCHED, CT_TMRCTRL) = CT_CEN;
    NVIC_EnableIRQ(SCHED_IRQn);
}

/* ---- The row interrupt's line ---------------------------------------------- */
/* Holds nest: the clock chip's transaction and a dev hold-off (op 28) can
 * overlap, and the line comes back only when the last holder releases it, to
 * the state the first holder found. Every caller holds the system lock or runs
 * I-class, so the depth needs no lock of its own. */
static uint8_t row_hold_depth  = 0;
static bool    row_hold_was_on = false;

uint8_t sn32f2xx_row_irq_hold(void) {
    if (row_hold_depth++ == 0) {
        row_hold_was_on = (NVIC->ISER[0] & (1u << SCHED_IRQn)) != 0;
        NVIC_DisableIRQ(SCHED_IRQn);   /* with its barriers (core_cm0.h); never ChibiOS's vector helpers, which clear the pending state */
    }
    return row_hold_depth;
}

uint8_t sn32f2xx_row_irq_release(void) {
    if (row_hold_depth == 0) return 0;
    if (--row_hold_depth == 0 && row_hold_was_on) NVIC_EnableIRQ(SCHED_IRQn);
    return row_hold_depth;
}

uint8_t sn32f2xx_row_irq_depth(void) {
    return row_hold_depth;
}

uint8_t sn32f2xx_row_irq_number(void) {
    return (uint8_t)SCHED_IRQn;
}

/* ---- Init ------------------------------------------------------------------ */

static bool pulse_timer_init(uint32_t t) {
    if (!timer_reset(t)) return false;
    CT_REG(t, CT_PRE) = 0;   /* HCLK */
    if (t == T_B1) {
        CT_REG(t, CT_MR(12))   = ROWISR_TERMINAL;
        CT_REG(t, CT_MCTRL)    = 0;
        CT_REG(t, CT_MCTRL2)   = B1_MCTRL2;        /* MR12: STOP only */
        CT_REG(t, CT_PWMCTRL)  = 0;                /* every channel mode 1 */
        CT_REG(t, CT_PWMENB)   = B1_PWMENB;
        CT_REG(t, CT_PWMIOENB) = B1_IOEN_RELEASED; /* taken at the start */
    } else {
        CT_REG(t, CT_MR(9))    = CT_KEY | ROWISR_TERMINAL;
        CT_REG(t, CT_MCTRL)    = PULSE_MCTRL;      /* MR9: STOP only */
        CT_REG(t, CT_PWMCTRL)  = (t == T_B0) ? B0_PWMCTRL_RELEASED : B2_PWMCTRL_RELEASED;
    }
    for (uint8_t i = 0; i < 12; i++) {   /* every duty match at zero: a constant inactive level */
        if (t != T_B1 && i >= 4) break;
        CT_REG(t, CT_MR(i)) = (t == T_B1) ? 0 : CT_KEY;
    }
    CT_REG(t, CT_IC) = RIS_ALL;
    pulse_park(t);
    return true;
}

static bool sched_init(void) {
    if (!timer_reset(T_SCHED)) return false;
    CT_REG(T_SCHED, CT_PRE)     = 0;
    CT_REG(T_SCHED, CT_PWMCTRL) = CT_KEY;   /* no PWM on the scheduler */
    sched_set_period(ROWISR_SLOT_TICKS);
    CT_REG(T_SCHED, CT_MCTRL)   = SCHED_MCTRL;
    CT_REG(T_SCHED, CT_IC)      = RIS_ALL;
    nvicEnableVector(SCHED_IRQn, SCHED_PRIO);
    CT_REG(T_SCHED, CT_TMRCTRL) = CT_CEN;
    return true;
}

/* The hardware, once, from matrix_init_custom(): the pins, the pin mux, the
 * four timers, the handler. */
static void rowisr_hw_init(void) {
    /* The key matrix's pins, as quantum/matrix.c's matrix_init_pins() set
     * them: every key row an output driven high (deselected), every column an
     * input with its pull-up. Thread context, before any interrupt touches a
     * port's MODE: no lock. */
    for (uint8_t x = 0; x < MATRIX_ROWS; x++) {
        gpio_set_pin_output(key_row_pins[x]);
        gpio_write_pin_high(key_row_pins[x]);
    }
    for (uint8_t x = 0; x < MATRIX_COLS; x++) {
        gpio_set_pin_input_high(led_col_pins[x]);
    }
    /* The mux rows: outputs, off (active high) */
    for (uint8_t x = 0; x < SN32F2XX_RGB_MATRIX_ROWS_HW; x++) {
        gpio_set_pin_output_push_pull(led_row_pins[x]);
        gpio_write_pin_low(led_row_pins[x]);
    }
    /* The pin mux for the columns spread across CT16B0/B1/B2 (config.h). */
    SN_PFPA->CT16B0 = SN32F2XX_PWM_PFPA_CT16B0;
    SN_PFPA->CT16B1 = SN32F2XX_PWM_PFPA_CT16B1;
    SN_PFPA->CT16B2 = SN32F2XX_PWM_PFPA_CT16B2;
    /* The timers' clocks, then the blocks reset whole, as the PWM LLD's
     * start did (a bootloader hand-off can leave a counter mode, a capture
     * or an inverted output configured; codex, P2 13), then the
     * configuration. A reset that never completes starts nothing: the
     * columns stay released and op 28's status says so. */
    sys1EnableCT16B0();
    sys1EnableCT16B1();
    sys1EnableCT16B2();
    sys1EnableCT16B4();
    sys1ResetCT16B0();
    sys1ResetCT16B1();
    sys1ResetCT16B2();
    sys1ResetCT16B4();
    rowisr_record_reset();
    if (!pulse_timer_init(T_B0) || !pulse_timer_init(T_B1) || !pulse_timer_init(T_B2) || !sched_init()) {
        hw_fault = true;
        columns_release();
        return;
    }
    columns_active();
}

/* CUSTOM_MATRIX = lite: quantum/matrix_common.c's matrix_init() calls this
 * first, then zeroes raw_matrix and matrix and starts debounce. The scan is
 * running when it returns, and every key row has been sampled once. */
void matrix_init_custom(void) {
    if (hw_up) return;   /* once: a second matrix_init() must not restart the timers */
    hw_up = true;
    rowisr_hw_init();
    /* The prime: Bootmagic's matrix_scan() comes next (quantum/keyboard.c:444,
     * bootmagic.c:63) and must find samples. The handler samples one key row
     * every third slot, so the six take 18 slots, ~5 ms; bounded at 50 ms, and
     * a timeout leaves the board as it was before stage 1, never stuck. */
    uint32_t t0 = timer_read32();
    while (rows_seen != SHARED_MATRIX_ALL_ROWS && timer_elapsed32(t0) < 50u) {
        __asm__ volatile("" ::: "memory");
    }
    prime_timed_out = rows_seen != SHARED_MATRIX_ALL_ROWS;   /* reported by op 28, never silent */
}

bool sn32f2xx_prime_timed_out(void) {
    return prime_timed_out;
}

/* rgb_matrix's driver init (rgb_matrix_init, after matrix_init): nothing to
 * do since stage 1. The frames are static zero; the hardware is up. */
void sn32f2xx_init(void) {}

/* ---- The quiesce, before the bootloader (D7) ---------------------------------- */
/* One idempotent teardown on every jump route (watchdog.c's bootloader_jump):
 * under the lock, the scheduler stopped with its interrupt off and nothing
 * pending, the columns released, every mux row low, the pulse timers parked;
 * then every LED pin high-impedance, so no slot can drive whatever the
 * polarity. Safe before the hardware is up (the pins alone). Thread context;
 * a second call does nothing. */
static bool quiesced = false;
void sn32f2xx_quiesce(void) {
    if (quiesced) return;
    quiesced = true;
    chSysLock();
    rowisr_diag_cancelI();   /* a dev hold-off's tick expiry could otherwise re-enable the line after this */
    if (hw_up) {
        NVIC_DisableIRQ(SCHED_IRQn);
        CT_REG(T_SCHED, CT_TMRCTRL) = 0;
        CT_REG(T_SCHED, CT_MCTRL)   = CT_KEY;   /* no interrupt, no reset */
        CT_REG(T_SCHED, CT_IC)      = RIS_ALL;
        NVIC_ClearPendingIRQ(SCHED_IRQn);
        columns_release();
        pulse_park(T_B0);
        pulse_park(T_B1);
        pulse_park(T_B2);
    }
    mux_deselect_all();
    chSysUnlock();
    for (uint8_t y = 0; y < SN32F2XX_RGB_MATRIX_COLS; y++) {
        gpio_set_pin_input(led_col_pins[y]);
    }
    for (uint8_t x = 0; x < SN32F2XX_RGB_MATRIX_ROWS_HW; x++) {
        gpio_set_pin_input(led_row_pins[x]);
    }
}

void sn32f2xx_blank(void) {
    sn32f2xx_quiesce();
}

/* ---- The frames, the flush and the power accounting (D8) ------------------- */
#ifdef RGB_MATRIX_ENABLE
/* The committed frame's channel sums, for sn32f2xx_led_committed_dark() and
 * lit_ms (elapsed time with any committed channel non-zero, its present
 * meaning); the drive itself is what was emitted: width_acc[], ticks per
 * colour, converted here to the model's unit (value x ms): a LED at byte b
 * emits k.b ticks once per field, so ticks x T_field / k is b x ms. The
 * remainder is carried into the next take. */
static uint32_t drive_now[3] = {0};   /* r, g, b */
static uint32_t drive_since  = 0;     /* when the frame was committed, ms */
static uint32_t drive_lit    = 0;     /* ms with any channel lit, since the last take */
static uint64_t drive_rem[3] = {0};   /* the conversion's remainder, ticks x us */
static uint8_t  commits      = 0;     /* flushes, wrapping (sn32f2xx_led_commits()) */
#define FIELD_US (SN32F2XX_RGB_MATRIX_ROWS_HW * ROWISR_SLOT_US)   /* 4,806 us */

static void drive_close(uint32_t now) {
    uint32_t dt = now - drive_since;
    if (drive_now[0] | drive_now[1] | drive_now[2]) drive_lit += dt;
    drive_since = now;
}

uint64_t sn32f2xx_led_drive_take(uint32_t now_ms, uint32_t *lit_ms, uint64_t rgb_ms[3]) {
    uint64_t acc[3];
    chSysLock();
    for (uint8_t k = 0; k < 3; k++) {
        acc[k]       = width_acc[k];
        width_acc[k] = 0;
    }
    chSysUnlock();
    drive_close(now_ms);
    uint64_t total = 0;
    for (uint8_t k = 0; k < 3; k++) {
        uint64_t num = acc[k] * FIELD_US + drive_rem[k];   /* ticks x us */
        uint64_t q   = num / (1000ULL * ROWISR_K);           /* value x ms */
        drive_rem[k] = num - q * (1000ULL * ROWISR_K);
        if (rgb_ms) rgb_ms[k] = q;
        total += q;
    }
    if (lit_ms) *lit_ms = drive_lit;
    drive_lit = 0;
    return total;
}

bool sn32f2xx_led_committed_dark(void) {
    return (drive_now[0] | drive_now[1] | drive_now[2]) == 0;
}

uint8_t sn32f2xx_led_commits(void) {
    return commits;
}

void sn32f2xx_flush(void) {
    commits++;   /* every render's, changed or not */
    if (led_state_buf_update_required) {
        /* The handler reads led_state[] for each slot's duties, and this
         * memcpy runs on the main loop: masked for its 10-16 us so no slot
         * sees a torn frame (D8 keeps this for the first implementation; a
         * slot starts up to 16 us late, inside its admission). */
        chSysLock();
        memcpy(led_state, led_state_buf, sizeof(RGB) * SN32F2XX_LED_COUNT);
        led_state_buf_update_required = false;
        chSysUnlock();
        uint32_t sr = 0, sg = 0, sb = 0;
        for (int i = 0; i < SN32F2XX_LED_COUNT; i++) {
            sr += led_state[i].r;
            sg += led_state[i].g;
            sb += led_state[i].b;
        }
        drive_close(timer_read32());
        drive_now[0] = sr;
        drive_now[1] = sg;
        drive_now[2] = sb;
    }
}

/* Board power scale, 255 = as requested, applied below the rgb_matrix config. */
static uint8_t power_scale = 255;
void sn32f2xx_set_power_scale(uint8_t scale) {
    power_scale = scale;
}
void sn32f2xx_set_color(int index, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t color_r = r * SN32F2XX_LED_OUTPUT_LUMINOSITY_R;
    uint8_t color_g = g * SN32F2XX_LED_OUTPUT_LUMINOSITY_G;
    uint8_t color_b = b * SN32F2XX_LED_OUTPUT_LUMINOSITY_B;
    if (power_scale != 255) {
        color_r = (uint8_t)(((uint16_t)color_r * (power_scale + 1u)) >> 8);
        color_g = (uint8_t)(((uint16_t)color_g * (power_scale + 1u)) >> 8);
        color_b = (uint8_t)(((uint16_t)color_b * (power_scale + 1u)) >> 8);
    }
    if (led_state_buf[index].r == color_r && led_state_buf[index].b == color_b && led_state_buf[index].g == color_g) {
        return;
    }
    led_state_buf[index].r        = color_r;
    led_state_buf[index].b        = color_b;
    led_state_buf[index].g        = color_g;
    led_state_buf_update_required = true;
}

void sn32f2xx_set_color_all(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < SN32F2XX_LED_COUNT; i++) {
        sn32f2xx_set_color(i, r, g, b);
    }
}

uint32_t sn32f2xx_led_load(void) {
    uint32_t sum = 0;
    for (int i = 0; i < SN32F2XX_LED_COUNT; i++) {
        sum += (uint32_t)led_state_buf[i].r + led_state_buf[i].g + led_state_buf[i].b;
    }
    return sum;
}

const rgb_matrix_driver_t rgb_matrix_driver = {
    .init          = sn32f2xx_init,
    .flush         = sn32f2xx_flush,
    .set_color     = sn32f2xx_set_color,
    .set_color_all = sn32f2xx_set_color_all,
};
#else  /* !RGB_MATRIX_ENABLE: the scan alone; the callers' accounting reads zeros */
uint64_t sn32f2xx_led_drive_take(uint32_t now_ms, uint32_t *lit_ms, uint64_t rgb_ms[3]) {
    (void)now_ms;
    if (lit_ms) *lit_ms = 0;
    if (rgb_ms) rgb_ms[0] = rgb_ms[1] = rgb_ms[2] = 0;
    return 0;
}
bool     sn32f2xx_led_committed_dark(void) { return true; }
uint8_t  sn32f2xx_led_commits(void) { return 0; }
void     sn32f2xx_flush(void) {}
void     sn32f2xx_set_power_scale(uint8_t scale) { (void)scale; }
void     sn32f2xx_set_color(int index, uint8_t r, uint8_t g, uint8_t b) { (void)index; (void)r; (void)g; (void)b; }
void     sn32f2xx_set_color_all(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
uint32_t sn32f2xx_led_load(void) { return 0; }
#endif

/* ---- The consume ------------------------------------------------------------- */

bool matrix_can_read(void) {
    return matrix_scanned;
}

/* CUSTOM_MATRIX = lite: quantum/matrix_common.c's matrix_scan() calls this
 * with raw_matrix and runs debounce on what it returns. */
bool matrix_scan_custom(matrix_row_t current_matrix[]) {
    if (!matrix_scanned) return false; /* Nothing to process until we have the matrix scanned */

    /* Instrumentation first and UNLOCKED on purpose: it reads the timer, which
     * must not be called inside chSysLock(). A torn read here only skews a
     * counter, never behaviour. (It can also miss an ISR update that lands
     * between it and the copy: raw_edges undercounts by design, BACKLOG.md;
     * stage 3 makes the count exact.) */
    input_note_consume(current_matrix, shared_matrix, MATRIX_ROWS);

    /* The ISR writes shared_matrix on every key-row transition, so the
     * compare/copy must exclude it or the loop can consume a matrix that is
     * part new and part old -- the same hazard sn32f2xx_flush() locks against. */
    chSysLock();
    bool changed = memcmp(current_matrix, shared_matrix, sizeof(shared_matrix)) != 0;
    if (changed) memcpy(current_matrix, shared_matrix, sizeof(shared_matrix));
    matrix_scanned = false;
    chSysUnlock();

    return changed;
}
