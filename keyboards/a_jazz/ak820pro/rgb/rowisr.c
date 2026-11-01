/* rgb/rowisr.c: the AK820 Pro's RGB matrix driver and key matrix in one file,
 * board-local (plans/ROW-ISR-PLAN.md, D1).
 *
 * Stage 1 (11-01): the code of drivers/led/sn32f2xx.c (Dimitris Mantzouranis,
 * GPL-2.0-or-later, with this repository's local commits) moved here with
 * every other configuration's branches removed -- this board is COL2ROW,
 * hardware PWM over CT16B0/B1/B2, the columns shared with the key matrix,
 * SN32F290 -- plus quantum/matrix.c's row helpers, which the row ISR called
 * across the core, now beside it with I-class locking. The pulse machinery is
 * unchanged on purpose, ChibiOS's PWM service included: the same
 * pwmEnableChannel() calls, the same end-of-callback re-arm, the same flush.
 * Stage 1 measures the move (the gates in the plan); stage 2 changes the hot
 * path.
 *
 * What moved where:
 * - the key matrix starts in matrix_init_custom() (CUSTOM_MATRIX = lite:
 *   quantum/matrix_common.c calls it from matrix_init(), before rgb_matrix
 *   exists), which brings up the pins, the three PWM timers and the callback,
 *   then waits, bounded, until every key row has been sampled once, so
 *   Bootmagic's first matrix_scan() holds samples that can start debounce
 *   (quantum/keyboard.c:444; a timer merely started leaves it empty);
 * - sn32f2xx_init(), rgb_matrix's driver init, touches no hardware and clears
 *   no matrix state: later RGB initialization must not lose acquired samples;
 * - the exported names are kept (rowisr.h), so the board's callers are
 *   unchanged;
 * - the row interrupt's NVIC line is held and released through this file
 *   (sn32f2xx_row_irq_hold/release), since its vector moves at stage 2.
 *
 * Known and deliberately kept at stage 1, for stage 2 (so the move can be
 * measured against the old driver, not a changed one):
 * - the hardware-PWM path reads led_state[led_index] without the NO_LED check
 *   that only the software-PWM path had: nine matrix positions have no LED
 *   and their 255 index reads past the array, harmlessly (the plan, "What is
 *   known"). Stage 2's driver produces explicit zeros;
 * - pwmEnableChannel()/pwmDisableChannel() from the callback are not I-class
 *   (they unlock interrupts on their way out), as before;
 * - the column release one channel at a time before the mux row's deselect
 *   (Q12).
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

/* The columns' PWM polarity and the mux rows' drive, as the old driver's
 * defaults had them for this board (never overridden in config.h): a column
 * is active LOW (the output low lights the LED), a mux row active HIGH. */
#define COL_PWM_ACTIVE_LEVEL PWM_OUTPUT_ACTIVE_LOW

#ifndef SN32F2XX_PWM_MULTI_TIMER
#    error "rowisr.c is written for the three-timer column map (config.h, SN32F2XX_PWM_COL_MAP)"
#endif
#if (DIODE_DIRECTION != COL2ROW)
#    error "rowisr.c scans COL2ROW: a row driven low, the columns read"
#endif
#ifndef MATRIX_UNSELECT_DRIVE_HIGH
#    error "rowisr.c drives a deselected key row high (the shared columns): config.h"
#endif

/* ---- Configuration ------------------------------------------------------ */

typedef struct {
    PWMDriver *drv;
    uint8_t    ch;
} sn32_pwm_col_t;
/* COL_PINS order: the column's timer and channel (config.h). */
static const sn32_pwm_col_t sn32_pwm_col_map[SN32F2XX_RGB_MATRIX_COLS] = SN32F2XX_PWM_COL_MAP;
#define COL_PWM_DRV(col) (sn32_pwm_col_map[col].drv)
#define COL_PWM_CH(col)  (sn32_pwm_col_map[col].ch)

static uint8_t current_row     = 0;   /* LED (mux) row scan counter, 0..17 */
static uint8_t current_key_row = 0;   /* key row scan counter, 0..5 */

/* +1 so a channel at FULL brightness still has a match to hit (the 08-31
 * lesson: pwm_lld_start writes the period match as period - 1, and a channel
 * at MR = 255 never matched a counter that reset at 254, so full value went
 * DARK). One tick a period. */
#ifndef RGB_MATRIX_MAXIMUM_BRIGHTNESS
#    define RGB_MATRIX_MAXIMUM_BRIGHTNESS 255
#endif
static const uint32_t periodticks = RGB_MATRIX_MAXIMUM_BRIGHTNESS + 1;
/* The PWM clock, pinned by config.h (SN32F2XX_RGB_PWM_FREQ, 4.8 MHz): the
 * stock driver derived it from the UI step sizes. */
static const uint32_t freq = SN32F2XX_RGB_PWM_FREQ;

static const pin_t led_row_pins[SN32F2XX_RGB_MATRIX_ROWS_HW] = SN32F2XX_RGB_MATRIX_ROW_PINS; /* the mux rows, B, G, R-of-next per key row (the plan, Q6) */
static const pin_t led_col_pins[SN32F2XX_RGB_MATRIX_COLS]    = MATRIX_COL_PINS;             /* shared with the key matrix */
static const pin_t key_row_pins[MATRIX_ROWS]                 = MATRIX_ROW_PINS;

#ifdef RGB_MATRIX_ENABLE
static RGB  led_state[SN32F2XX_LED_COUNT];     /* the front frame: what the ISR drives */
static RGB  led_state_buf[SN32F2XX_LED_COUNT]; /* rgb_matrix's: the next frame */
static bool led_state_buf_update_required = false;
#endif

/* The rolling matrix the ISR writes and the main loop consumes. matrix_scanned
 * is a DIRTY flag, set when something new has been sampled, cleared by the
 * consume; NOT a scan gate (shared_matrix_scan_keys). */
static matrix_row_t  shared_matrix[MATRIX_ROWS];
static volatile bool matrix_scanned = false;
/* Bit per key row: readiness is withheld until every row has been sampled
 * once, so the main loop never consumes uninitialised rows after boot; the
 * prime in matrix_init_custom() waits on it. */
static volatile uint8_t rows_seen = 0;
#define SHARED_MATRIX_ALL_ROWS ((uint8_t)((1u << ROWS_PER_HAND) - 1u))
static bool hw_up           = false;   /* rowisr_hw_init() done (matrix_init_custom) */
static bool prime_timed_out = false;   /* the prime's 50 ms passed before the six rows */

volatile bool sn32f2xx_pgm_scan = true;   /* rowisr.h: straight through a flash program (10-27) */
#ifdef AK820_DEV_HOOKS
volatile bool sn32f2xx_scan_skip = false; /* rowisr.h: op 28 sub 4 */
#endif

/* ---- The key matrix's row helpers (from quantum/matrix.c, COL2ROW) --------
 *
 * I-class: the row ISR is the only caller once the prime has waited for it
 * (matrix_init_custom). quantum/matrix.c wrapped the same two operations in
 * ATOMIC_BLOCK_FORCEON, chSysLock()/chSysUnlock() from interrupt context --
 * on this ARMv6-M port the lock pair is the same two PRIMASK instructions as
 * the I-class pair (chcore.h), with the debug state checks off, so the
 * contract changes and the lock itself does not. The surrounding code is
 * not instruction-identical to the old scan (no NO_PIN branches, since this
 * board has none; the compiler's own choices), which is why stage 1's gate
 * is a measured 3%, not an assumption (codex, stage 1's review). Each
 * select or unselect re-asserts output mode (a read-modify-write of the
 * port's MODE, idempotent on a pin that is already an output) and writes the
 * level; the clock chip's transaction holds this interrupt off for its own
 * MODE writes (rtc/rtc.c, sn32f2xx_row_irq_hold). Stage 3 removes the mode
 * writes. */
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

/* The settle after a deselect, as the old driver had it (its override of
 * matrix_common.c's wait_us(30)): TIME_US2I(30) iterations of a compiler
 * barrier, about six -- short and uncalibrated, not 30 us (the plan, "What
 * is known"). Kept for the stage 1 A/B; Q1's sweep sets stage 3's. */
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
/* Row-ISR cost instrumentation (health.c): the counter is re-armed at the END
 * of rgb_callback(), so the ISR's own duration adds to its period and sets
 * the real row rate; these let the board measure that. */
__attribute__((weak)) void sn32f2xx_isr_enter_hook(void) {}
__attribute__((weak)) void sn32f2xx_isr_exit_hook(void) {}

/* Scan on EVERY key-row transition, straight into shared_matrix.
 *
 * The key row advances every third PWM ISR, so every row is sampled once per
 * 18-slot cycle regardless of the consume rate or its phase (the 09-03 fix:
 * before it, at most one row was sampled per main-loop consume, and a key
 * went 156-169 ms unsampled on an idle board; docs/hardware.md,
 * plans/MATRIX-PUBLISH-PLAN.md). Measured ~3,870 ISR/s and ~215 samples/s a
 * row, because the callback re-arms the counter at its end. */
static void shared_matrix_scan_keys(matrix_row_t current_matrix[], uint8_t current_key, uint8_t last_key) {
    if (last_key != current_key) {   /* a genuinely new row is energised */
        matrix_read_cols_on_row(current_matrix, current_key);
        input_note_row_scan(current_key);
        if (rows_seen != SHARED_MATRIX_ALL_ROWS) {
            rows_seen |= (uint8_t)(1u << current_key);
            if (rows_seen != SHARED_MATRIX_ALL_ROWS) return;   /* not complete yet */
        }
        matrix_scanned = true;   /* dirty flag: something new since last consume */
    }
}

/* ---- The PWM timers ------------------------------------------------------ */

static void rgb_callback(PWMDriver *pwmp);

static PWMConfig pwmcfg = {
    freq,        /* PWM clock frequency. */
    periodticks, /* PWM period in ticks: 256 at 4.8 MHz, 53.3 us */
    NULL,        /* RGB callback: set at enable */
    {
        /* Default all channels to disabled - Channels will be configured during init */
        [0 ... PWM_CHANNELS - 1] = {PWM_OUTPUT_DISABLED, NULL, 0},
    },
    0 /* HW dependent part.*/
};
/* The columns that spill over to CT16B0 / CT16B2. Channel modes are filled in
 * by rgb_ch_ctrl() from the column map. */
static PWMConfig pwmcfg_b0 = {freq, periodticks, NULL, {[0 ... PWM_CHANNELS - 1] = {PWM_OUTPUT_DISABLED, NULL, 0}}, 0};
static PWMConfig pwmcfg_b2 = {freq, periodticks, NULL, {[0 ... PWM_CHANNELS - 1] = {PWM_OUTPUT_DISABLED, NULL, 0}}, 0};

/* Resolve a column's PWMDriver to its owning PWMConfig. */
static PWMConfig *sn32_pwm_cfg_for(PWMDriver *drv) {
    if (drv == &PWMD0) return &pwmcfg_b0;
    if (drv == &PWMD2) return &pwmcfg_b2;
    return &pwmcfg; /* PWMD1 */
}

/* Mark each used channel active on its owning timer's config: the
 * column->(timer,channel) mapping and the pin mux are fixed by the board
 * (SN32F2XX_PWM_COL_MAP + the SN_PFPA writes in rowisr_hw_init). */
static void rgb_ch_ctrl(void) {
    for (uint8_t i = 0; i < SN32F2XX_RGB_MATRIX_COLS; i++) {
        if (led_col_pins[i] > C15) continue; /* Only P0.0..P2.15 can be PWM outputs */
        sn32_pwm_cfg_for(sn32_pwm_col_map[i].drv)->channels[sn32_pwm_col_map[i].ch].mode = COL_PWM_ACTIVE_LEVEL;
    }
}

static void shared_matrix_rgb_enable(void) {
    pwmcfg.callback = rgb_callback;
    pwmEnablePeriodicNotification(&PWMD1);
}

static void shared_matrix_rgb_disable_output(void) {
    /* Disable PWM outputs on column pins: one at a time, before the mux row
     * goes (Q12: the right-hand columns stay lit longer than the left). */
    for (uint8_t y = 0; y < SN32F2XX_RGB_MATRIX_COLS; y++) {
        pwmDisableChannel(COL_PWM_DRV(y), COL_PWM_CH(y));
    }
    /* Disable LED outputs on RGB channel pins (active high: low is off) */
    for (uint8_t x = 0; x < SN32F2XX_RGB_MATRIX_ROWS_HW; x++) {
        gpio_write_pin_low(led_row_pins[x]);
    }
}

static void update_pwm_channels(PWMDriver *pwmp) {
    (void)pwmp;
    /* Advance to the next LED RGB channels */
    current_row++;
    /* Check if counter has wrapped around, reset before the next pass */
    if (current_row == SN32F2XX_RGB_MATRIX_ROWS_HW) current_row = 0;
    uint8_t last_key_row = current_key_row;
    /* Advance to the next key matrix row: every third mux row */
    if (current_row % SN32F2XX_RGB_MATRIX_ROW_CHANNELS == 2) current_key_row++;
    /* Check if counter has wrapped around, reset before the next pass */
    if (current_key_row == SN32F2XX_RGB_MATRIX_ROWS) current_key_row = 0;
    /* Disable LED output before scanning the key matrix */
    if (current_key_row < ROWS_PER_HAND) {
        shared_matrix_rgb_disable_output();
#ifdef AK820_DEV_HOOKS
        if (!sn32f2xx_scan_skip)   /* op 28 sub 4: the LED work alone, to attribute the scan's cost */
#endif
        shared_matrix_scan_keys(shared_matrix, current_key_row, last_key_row);
    }
#ifdef RGB_MATRIX_ENABLE
    bool enable_pwm_output = false;
    for (uint8_t current_key_col = 0; current_key_col < SN32F2XX_RGB_MATRIX_COLS; current_key_col++) {
        uint8_t led_index = g_led_config.matrix_co[current_key_row][current_key_col];
        /* Check if we need to enable RGB output */
        if (led_state[led_index].b > 0) enable_pwm_output |= true;
        if (led_state[led_index].g > 0) enable_pwm_output |= true;
        if (led_state[led_index].r > 0) enable_pwm_output |= true;
        /* Update matching RGB channel PWM configuration: the colour of this
         * mux row (0 -> B, 1 -> G, 2 -> R; the plan, Q6) */
        switch (current_row % SN32F2XX_RGB_MATRIX_ROW_CHANNELS) {
            case 0:
                pwmEnableChannel(COL_PWM_DRV(current_key_col), COL_PWM_CH(current_key_col), led_state[led_index].b);
                break;
            case 1:
                pwmEnableChannel(COL_PWM_DRV(current_key_col), COL_PWM_CH(current_key_col), led_state[led_index].g);
                break;
            case 2:
                pwmEnableChannel(COL_PWM_DRV(current_key_col), COL_PWM_CH(current_key_col), led_state[led_index].r);
                break;
            default:;
        }
    }
    /* Enable RGB output: select the mux row (active high) */
    if (enable_pwm_output) {
        gpio_write_pin_high(led_row_pins[current_row]);
    }
#endif
}

/* Scan-only (a board's light sleep, sn32f2xx_set_scan_only): no LED work, one
 * key row per ISR at a longer period, on its own row counter so the LED
 * field's row pairing is untouched and resumes where it was. ISR-owned but for
 * the request flag. */
#ifdef SN32F2XX_SLEEP_HOOKS
#    ifndef SN32F2XX_SCAN_ONLY_TICKS
#        define SN32F2XX_SCAN_ONLY_TICKS 3000u /* 0.625 ms at 4.8 MHz, plus the body: each key every ~3.9 ms,
                                                * under sym_defer_pk's 5 ms so debounce sees a fresh sample */
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

static void rgb_callback(PWMDriver *pwmp) {
    sn32f2xx_isr_enter_hook();
    /* Disable the interrupt */
    pwmDisablePeriodicNotification(pwmp);
#ifdef SN32F2XX_SLEEP_HOOKS
    /* Scan-only: the switch is applied here, between periods. Entering blanks
     * every output once (the columns stay released from PWM, so they read as
     * the inputs every scan needs); the period changes in the locked re-arm
     * below; leaving restores the field's period, and the next
     * update_pwm_channels() drives it again. */
    bool period_change = false;
    if (scan_only_req != scan_only) {
        scan_only     = scan_only_req;
        period_change = true;
        if (scan_only) shared_matrix_rgb_disable_output();
    }
    if (scan_only) {
        if (EFLD1.state != FLASH_PGM || sn32f2xx_pgm_scan) {   /* as below: the scan through a flash program, unless the guard is asked for */
            uint8_t last = scan_only_row;
            if (++scan_only_row >= ROWS_PER_HAND) scan_only_row = 0;
            shared_matrix_scan_keys(shared_matrix, scan_only_row, last);
        }
    } else
#endif
    /* Scan the rgb and key matrix */
    if (EFLD1.state != FLASH_PGM || sn32f2xx_pgm_scan) {   /* straight through a program, the default since 10-27 */
        update_pwm_channels(pwmp);
    } else {
        /* The 09-01 guard, now only where sn32f2xx_pgm_scan is false (a dev
         * build's write test, to compare). A flash program is in progress:
         * de-select every mux pin -- plain GPIO writes, ISR-safe
         * (sn32f2xx_blank() is NOT: it calls non-I-class pwmDisableChannel)
         * -- so the frozen state is dark. The first tick after the operation
         * re-drives the mux via update_pwm_channels. */
        for (uint8_t x = 0; x < SN32F2XX_RGB_MATRIX_ROWS_HW; x++) {
            gpio_write_pin_low(led_row_pins[x]);
        }
    }
    sn32f2xx_isr_exit_hook();
    chSysLockFromISR();
#ifdef SN32F2XX_SLEEP_HOOKS
    if (period_change) pwmChangePeriodI(pwmp, scan_only ? SN32F2XX_SCAN_ONLY_TICKS : periodticks);
#endif
    /* Advance the timer to just before the wrap-around, that will start a new
     * PWM cycle; the auxiliary timers on the same boundary, which is what
     * keeps their hardware PWM phase-locked to the row scan every period
     * (without it they free-run and drift: irregular flicker + a one-phase
     * colour shift). All three share PWMD1's clock and period. */
    pwm_lld_change_counter(pwmp, UINT16_MAX);
    pwm_lld_change_counter(&PWMD0, UINT16_MAX);
    pwm_lld_change_counter(&PWMD2, UINT16_MAX);
    /* Enable the interrupt */
    pwmEnablePeriodicNotificationI(pwmp);
    chSysUnlockFromISR();
}

/* ---- Sleep ----------------------------------------------------------------- */

/* Deep sleep (sleep.c): every output off and the row ISR's notification off,
 * the columns left released from PWM (inputs with their pull-ups, as for a
 * scan); then put back, the counters re-armed on one boundary as the ISR's
 * end does. I-class: called with interrupts locked, so no lock call
 * (pwmDisableChannel would unlock on its way out). */
void sn32f2xx_sleep_enterI(void) {
    pwmDisablePeriodicNotificationI(&PWMD1);
    for (uint8_t y = 0; y < SN32F2XX_RGB_MATRIX_COLS; y++) {
        pwmDisableChannelI(COL_PWM_DRV(y), COL_PWM_CH(y));
    }
    for (uint8_t x = 0; x < SN32F2XX_RGB_MATRIX_ROWS_HW; x++) {
        gpio_write_pin_low(led_row_pins[x]);
    }
}

void sn32f2xx_sleep_exitI(void) {
    /* A match latched while the notification was off: cleared, in the timer
     * and the NVIC, so the first ISR after this is a real period (codex). */
    uint32_t ris = SN32_CT_PWM_GET(&PWMD1, irq.RIS);
    SN32_CT_PWM_SET(&PWMD1, irq.IC, ris);
    NVIC_ClearPendingIRQ(CT16B1_IRQn);
    pwm_lld_change_counter(&PWMD1, UINT16_MAX);
    pwm_lld_change_counter(&PWMD0, UINT16_MAX);
    pwm_lld_change_counter(&PWMD2, UINT16_MAX);
    pwmEnablePeriodicNotificationI(&PWMD1);
}

/* ---- The row interrupt's line ---------------------------------------------- */

/* Holds nest: the clock chip's transaction and a dev hold-off (op 28) can
 * overlap, and the line comes back only when the last holder releases it,
 * to the state the first holder found. With a saved bit per holder, the dev
 * hold's release re-enabled the line inside the transaction (codex, stage
 * 1's review, P1). Every caller holds the system lock or runs I-class, so
 * the depth needs no lock of its own. */
static uint8_t row_hold_depth  = 0;
static bool    row_hold_was_on = false;

uint8_t sn32f2xx_row_irq_hold(void) {
    if (row_hold_depth++ == 0) {
        row_hold_was_on = (NVIC->ISER[0] & (1u << CT16B1_IRQn)) != 0;
        NVIC_DisableIRQ(CT16B1_IRQn);   /* with its barriers (core_cm0.h); never ChibiOS's vector helpers, which clear the pending state */
    }
    return row_hold_depth;
}

uint8_t sn32f2xx_row_irq_release(void) {
    if (row_hold_depth == 0) return 0;
    if (--row_hold_depth == 0 && row_hold_was_on) NVIC_EnableIRQ(CT16B1_IRQn);
    return row_hold_depth;
}

uint8_t sn32f2xx_row_irq_depth(void) {
    return row_hold_depth;
}

uint8_t sn32f2xx_row_irq_number(void) {
    return (uint8_t)CT16B1_IRQn;
}

/* ---- Init ------------------------------------------------------------------ */

/* The hardware, once, from matrix_init_custom(): the pins, the pin mux, the
 * three PWM timers phase-locked, the callback. What sn32f2xx_init() did
 * before stage 1, minus the matrix state it zeroed (static now). */
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

    /* Determine which PWM channels we need to control */
    rgb_ch_ctrl();

    /* Program the pin-mux for the columns spread across CT16B0/B1/B2. The
     * three constants are board-specific (config.h; docs/HARDWARE_PWM.md). */
    SN_PFPA->CT16B0 = SN32F2XX_PWM_PFPA_CT16B0;
    SN_PFPA->CT16B1 = SN32F2XX_PWM_PFPA_CT16B1;
    SN_PFPA->CT16B2 = SN32F2XX_PWM_PFPA_CT16B2;
    /* All three timers share HCLK. Only PWMD1 drives the row-advance callback,
     * which reloads the duty of every column (across all three timers) once
     * per period. For that to land in the right PWM cycle on PWMD0/PWMD2 they
     * must be phase-locked to PWMD1. */
    pwmStart(&PWMD0, &pwmcfg_b0);
    pwmStart(&PWMD1, &pwmcfg);
    pwmStart(&PWMD2, &pwmcfg_b2);
    /* pwmStart() launches the counters a few instructions apart, leaving a
     * fixed phase offset; the PWMD1 ISR then writes PWMD0/PWMD2 duties
     * mid-cycle, which glitches (irregular flicker + one-phase colour shift).
     * Reset all three counters together so they share a cycle boundary;
     * identical periods on a shared clock keep them locked afterwards. */
    chSysLock();
    pwm_lld_change_counter(&PWMD0, 0);
    pwm_lld_change_counter(&PWMD1, 0);
    pwm_lld_change_counter(&PWMD2, 0);
    chSysUnlock();
    shared_matrix_rgb_enable();
}

/* CUSTOM_MATRIX = lite: quantum/matrix_common.c's matrix_init() calls this
 * first, then zeroes raw_matrix and matrix and starts debounce. The scan is
 * running when it returns, and every key row has been sampled once. */
void matrix_init_custom(void) {
    if (hw_up) return;   /* once: a second matrix_init() must not restart the timers */
    hw_up = true;
    rowisr_hw_init();
    /* The prime: Bootmagic's matrix_scan() comes next (quantum/keyboard.c:444,
     * bootmagic.c:63) and must find samples, or it starts debounce on nothing.
     * The callback samples one key row every third slot, so the six take
     * ~18 slots, about 5 ms today; bounded at 50 ms, and a timeout leaves the
     * board as it was before stage 1 (the first scan empty), never stuck. The
     * row ISR is the only writer, and it runs: the wait is the readiness mask
     * of D1, not a thread-side pass through the row helpers. */
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

/* The row scan is a MUX: exactly one of the 18 row slots is energised at any
 * instant, and the ISR moves it on. Stop the ISR and whichever row and
 * channel happened to be live stays live, at full duty instead of its 1/18
 * share: the stuck bright row seen during flash writes, and after a jump to
 * the ROM bootloader, permanent. Since stage 1 the timers and the callback
 * run from matrix_init_custom(), so Bootmagic's jump finds them running
 * (before, the driver started at rgb_matrix_init, after Bootmagic), and the
 * frame it drives is zero but for the NO_LED read past led_state[] (the file
 * header): a stuck row is possible there without this. One idempotent
 * teardown on every jump route (watchdog.c's bootloader_jump): under the
 * lock, the notification and the NVIC line off with the pending flag
 * cleared, every column channel off (I-class), every mux row low; then the
 * three timers stopped; then every LED pin high-impedance, so no slot can
 * drive whatever the polarity. Safe before the hardware is up (the pins
 * alone). Thread context; a second call does nothing. */
static bool quiesced = false;

void sn32f2xx_quiesce(void) {
    if (quiesced) return;
    quiesced = true;
    chSysLock();
    rowisr_diag_cancelI();   /* a dev hold-off's tick expiry could otherwise re-enable the line after this (codex, verify, P2) */
    if (hw_up) {
        pwmDisablePeriodicNotificationI(&PWMD1);
        NVIC_DisableIRQ(CT16B1_IRQn);
        NVIC_ClearPendingIRQ(CT16B1_IRQn);
        for (uint8_t y = 0; y < SN32F2XX_RGB_MATRIX_COLS; y++) {
            pwmDisableChannelI(COL_PWM_DRV(y), COL_PWM_CH(y));
        }
    }
    for (uint8_t x = 0; x < SN32F2XX_RGB_MATRIX_ROWS_HW; x++) {
        gpio_write_pin_low(led_row_pins[x]);
    }
    chSysUnlock();
    if (hw_up) {
        pwmStop(&PWMD0);
        pwmStop(&PWMD1);
        pwmStop(&PWMD2);
    }
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

/* ---- The frames, the flush and the power accounting ------------------------- */

#ifdef RGB_MATRIX_ENABLE
/* The committed frame's drive, time-weighted (sn32f2xx_led_drive_take), per
 * colour: red, green and blue draw differently per unit (battery.c's power
 * model weights them). The sums are taken after the masked copy, not inside
 * it: only sn32f2xx_flush() writes led_state[], and the ISR only reads it. */
static uint32_t drive_now[3] = {0};   /* the committed frame's channel sums: r, g, b */
static uint32_t drive_since  = 0;     /* when it was committed, ms */
static uint64_t drive_acc[3] = {0};   /* sum x ms since the last take, per colour */
static uint32_t drive_lit    = 0;     /* ms with any channel lit, since the last take */
static uint8_t  commits      = 0;     /* flushes, wrapping (sn32f2xx_led_commits()) */

static void drive_close(uint32_t now) {
    uint32_t dt = now - drive_since;
    for (uint8_t k = 0; k < 3; k++) drive_acc[k] += (uint64_t)drive_now[k] * dt;
    if (drive_now[0] | drive_now[1] | drive_now[2]) drive_lit += dt;
    drive_since = now;
}

uint64_t sn32f2xx_led_drive_take(uint32_t now_ms, uint32_t *lit_ms, uint64_t rgb_ms[3]) {
    drive_close(now_ms);
    uint64_t r = drive_acc[0] + drive_acc[1] + drive_acc[2];
    for (uint8_t k = 0; k < 3; k++) {
        if (rgb_ms) rgb_ms[k] = drive_acc[k];
        drive_acc[k] = 0;
    }
    if (lit_ms) *lit_ms = drive_lit;
    drive_lit = 0;
    return r;
}

bool sn32f2xx_led_committed_dark(void) {
    return (drive_now[0] | drive_now[1] | drive_now[2]) == 0;
}

uint8_t sn32f2xx_led_commits(void) {
    return commits;
}

void sn32f2xx_flush(void) {
    commits++;   /* every render's, changed or not: a Solid frame after a dim changes nothing (codex) */
    if (led_state_buf_update_required) {
        /* The row ISR reads led_state[] to set each channel's duty, and this
         * memcpy runs on the MAIN LOOP -- so without a lock the ISR can land
         * mid-copy and drive a frame that is part new and part old. The split
         * falls at whatever byte was reached, which shows on the panel as a
         * single row briefly lit in a wrong colour. Visible while sweeping hue,
         * where consecutive frames differ enough to notice.
         *
         * Masking for the duration is cheap and bounded: ~10-16 us for the
         * 81-LED copy at 48 MHz. Worst case the current row is armed a few
         * microseconds late, which is invisible; a torn frame is not. (D8
         * keeps this for the first implementation.) */
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

/* Board power scale, 255 = as requested. Applied here, below the rgb_matrix
 * config, so a board can dim or darken the matrix to save power without
 * touching the user's brightness (or its EEPROM copy). No divide: this runs
 * for every channel of every LED on every frame, on a core without one. */
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
