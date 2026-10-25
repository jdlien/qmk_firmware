// Copyright 2026 Fernando Birra, JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
/* Moved verbatim from ak820pro.c in the phase-3 module split. */
#include "quantum.h"
#include "indicators.h"
#include "ak820pro.h"   /* the shared layer enum for FN_LAYER_MASK */
#include "graphics/display.h"
#include "bluetooth/ch582f_ajazz.h"   /* the module's battery level */
#include "battery.h"                  /* battery_ran_from_pack */
#include "overrun_diag.h"             /* dev: the interrupt-service-gap monitor rides this tick */
#include "kb_eeconfig.h"              /* the saved levels (P2a) */
#include <hal.h>

/* --- Indicator LEDs -------------------------------------------------------
 *
 * Caps Lock (D15), Win Lock (C15) and Charging (B18) are plain GPIOs, and at
 * full drive they are painfully bright on a desk in a dark room -- the charging
 * one especially, since it is not under user control at all.
 *
 * So they are dimmed in software rather than switched, sharing the 20 kHz
 * CT16B3 tick the LCD backlight uses (pwm_tick_cb below). The levels are the
 * menu's 0-100 (P2a, plans/CONFIG-MENU-P1-DESIGN.md; indicator_levels.h):
 * perceptually spaced, because a linear ramp wastes its steps at the top and
 * gives nothing usable at the bottom. Level 0 is fully off, if you would
 * rather lose an indicator than see it.
 *
 * These functions only record desired state now; the ISR drives the pins. */
/* The period's history: one pulse of one tick every IND_PWM_TICKS
 * (indicator_levels.h) is the dimmest level, and the sigma-delta below keeps
 * that floor. 96 rather than the backlight's 64, tuned on hardware.
 *
 * Minimum brightness is always 1 tick, so a dimmer floor needs a longer period
 * -- which lowers the switching frequency, since one pulse per period IS the
 * fundamental. At a given low brightness and tick rate the flicker frequency is
 * therefore fixed; the only real lever is the tick rate, and raising that costs
 * matrix scan rate.
 *
 * Measured on this unit:
 * Measured at the OLD 18750/s ISR rate:
 *   64 ticks  -> 293 Hz, 1/64 (1.6%)   no flicker, slightly too bright
 *   128 ticks -> 146 Hz, 1/128 (0.78%) good brightness, Caps visibly flickered
 *   96 ticks  -> 195 Hz, 1/96 (1.0%)   the compromise
 *
 * This no longer rides the RGB row ISR. It ticks from CT16B3 (GPTD4) at a
 * measured 20000 Hz, so RGB_MATRIX_SPD_STEP does not affect it and the old
 * three-way coupling between LED field rate, backlight and indicators is gone.
 *
 * If flicker reappears, shorten the floor's period (brighter, faster) rather
 * than reaching for the ISR rate. IND_PWM_TICKS is 48: 20000/48 = 417 Hz,
 * floor 1/48 = 2.1%. The earlier flicker at 96 ticks was measured against a
 * tick that was dropping 23% of its interrupts; with a steady 20000 the same
 * period is now 208 Hz. */
#include "indicator_levels.h"
_Static_assert(IND_PWM_TICKS == 48 && IND_LEVEL_MAX == 100, "the levels' floor and range");

/* Per-group levels (0-100): the charging LED duplicates what the battery icon
 * already shows, so it defaults to 0 (off) while the locks stay dimly lit (1,
 * the old level 1). The menu's Indicators set and save them (kb_eeconfig). */
#ifndef INDICATOR_BRIGHTNESS_DEFAULT
#    define INDICATOR_BRIGHTNESS_DEFAULT 1
#endif
#ifndef CHARGING_LED_BRIGHTNESS
#    define CHARGING_LED_BRIGHTNESS 0
#endif
_Static_assert(INDICATOR_BRIGHTNESS_DEFAULT <= IND_LEVEL_MAX && CHARGING_LED_BRIGHTNESS <= IND_LEVEL_MAX,
               "the defaults are levels 0-100");

static volatile bool    ind_caps     = false;
static volatile bool    ind_winlock  = false;
static volatile bool    ind_scroll   = false;   /* host-reported; no key on this board */

/* Scroll Lock as the LCD band shows it: settled. A host that signals through
 * the lock lights (plans/BACKLOG.md, the lock-LED channel) toggles Scroll Lock
 * up to ~33 times a second and puts it back when its frame ends. Shown as sent,
 * that repaints the band on every 10 Hz tick for nothing (the one pass >= 25 ms
 * of the 10-17 drain came in the 5 minutes that held the probe's burst, and
 * two Caps presses). So a change shows once it has held SCR_SETTLE_MS. Only a
 * Scroll Lock transition restarts that clock, not any LED report, so a frame
 * stays invisible only if it never leaves Scroll Lock changed for
 * SCR_SETTLE_MS at a stretch, pauses and retries included: a rule for the
 * encoding (codex, flash 2g). A real change still shows, that much late.
 * The exception is this board's own KC_SCRL: a change within SCR_OWN_KEY_MS of
 * its press is taken for the host answering it (Windows echoes Scroll Lock;
 * macOS ignores the key), so it shows at once. A timing heuristic: an
 * unrelated change in that window takes it too. Main-loop context only. */
#define SCR_SETTLE_MS  1500u
#define SCR_OWN_KEY_MS 500u
static bool     scr_shown      = false;
static uint32_t scr_changed_at = 0;
static uint32_t scr_key_at     = 0;
static bool     scr_key_armed  = false;   /* a KC_SCRL press not yet answered */
static volatile bool    ind_charging = false;

/* The groups' levels (main loop), and their duties for the ISR: each one
 * aligned uint16_t store, so the tick never reads half of one. The force bits
 * (IND_LOCKS, IND_CHARGING as bits 0 and 1) light a group whatever its state:
 * the menu's preview. */
static uint8_t           ind_lvl[2] = {INDICATOR_BRIGHTNESS_DEFAULT, CHARGING_LED_BRIGHTNESS};
static volatile uint16_t ind_duty_locks, ind_duty_chg;   /* from ind_lvl in pwm_tick_init() */
static volatile uint8_t  ind_force = 0;

/* ISR context. Each group's duty is spread by a first-order sigma-delta
 * (indicator_levels.h, ind_tick_out): at level 1 one tick in IND_PWM_TICKS,
 * as the old phase compare's level 1; above it the on-ticks spread evenly, so
 * no gap between pulses is longer than at level 1. A pin is written only when
 * its bit changes, or all three after a restart (ind_pins_to_write). */
static uint16_t ind_acc_locks, ind_acc_chg;
static uint8_t  ind_last_out = IND_OUT_UNKNOWN;

static void indicators_tick(void) {
    const uint8_t out = ind_tick_out(ind_caps, ind_winlock, ind_charging, ind_force, &ind_acc_locks, &ind_acc_chg,
                                     ind_duty_locks, ind_duty_chg);
    const uint8_t ch  = ind_pins_to_write(out, ind_last_out);
    if (!ch) return;
    ind_last_out = out;
    if (ch & 1u) gpio_write_pin(LED_CAPS_LOCK_PIN, (out & 1u) != 0u);
    if (ch & 2u) gpio_write_pin(LED_WINLOCK_PIN, (out & 2u) != 0u);
    if (ch & 4u) gpio_write_pin(LED_CHARGING_PIN, (out & 4u) != 0u);
}

/* Backlight + indicator software PWM tick, on its OWN timer (CT16B3/GPTD4).
 *
 * This used to hang off a weak hook in the RGB row-scan ISR (since removed from
 * drivers/led/sn32f2xx.c), which coupled the PWM rate to RGB_MATRIX_SPD_STEP.
 * That was a mistake: raising
 * SPD_STEP for a dimmer backlight starved the CH582F UART and Bluetooth typing
 * collapsed to ~7 characters/second (measured: 3.7 ACK retransmits per frame at
 * an 8963/s row ISR, versus 0.38 at the stock 2180/s).
 *
 * The two jobs want opposite things, so they get separate timers. What made the
 * row ISR harmful was its WORK -- it reloads 15 PWM channels per invocation --
 * not merely its rate. This handler is a counter, a compare and a few GPIO
 * writes, roughly a microsecond, so it can run fast without denying the UART.
 *
 * 1 MHz timebase / 50 = 20 kHz tick. Keep this comfortably shorter than the
 * ~87us UART byte time at 115200. */
#define PWM_TICK_HZ       20000U
#define PWM_TICK_TIMEBASE 1000000U   /* 48 MHz / 48, an exact prescaler */

/* The configured tick rate and the real one have diverged before (the GPT LLD
 * never enabled reset-on-match, giving ~15 Hz instead of 20 kHz), and a slow
 * tick is also the first sign the MCU is out of CPU. To measure it again, add a
 * `static volatile uint32_t pwm_tick_count` incremented here, and print the
 * delta once a second from housekeeping_task_kb():
 *
 *     [pwmtick] 20000 Hz   -- healthy
 *     [pwmtick] 15385 Hz   -- ticks being lost (priority inversion; see mcuconf.h)
 *     [pwmtick] 20250 Hz   -- CANNOT speed up: the ms timebase has slowed, i.e.
 *                             systick is being lost under interrupt load
 *
 * Removed from the shipped build because it prints every second. */
static void pwm_tick_cb(GPTDriver *gptp) {
    (void)gptp;
#ifdef AK820_DEV_HOOKS
    od_tick();   /* first, so the gap is this entry's */
#endif
    display_backlight_tick();
    indicators_tick();
}

static const GPTConfig pwm_tick_cfg = {
    .frequency = PWM_TICK_TIMEBASE,
    .callback  = pwm_tick_cb,
};

void pwm_tick_init(void) {
    ind_duty_locks = ind_level_duty[ind_lvl[IND_LOCKS]];   /* the defaults; indicators_load_saved() follows */
    ind_duty_chg   = ind_level_duty[ind_lvl[IND_CHARGING]];
    gptStart(&GPTD4, &pwm_tick_cfg);
    gptStartContinuous(&GPTD4, PWM_TICK_TIMEBASE / PWM_TICK_HZ);

    /* WORKAROUND: the SN32 GPT LLD's continuous mode enables the MR0 match
     * INTERRUPT but never reset-on-match, so the counter free-runs the full
     * 16-bit range and the callback fires once per wrap instead of once per
     * interval -- 1 MHz / 65536 = 15 Hz, not the 20 kHz asked for. Observed as
     * the backlight blinking at full brightness every ~4.5 s.
     *
     * Enable MR0RST ourselves. MCTRL on every CT16 except CT16B1 ignores writes
     * without 0x5A in bits[31:24] (see docs/HARDWARE_PWM.md "Register-model
     * gotchas"), hence the unlock -- the same trap that bit the PWM driver.
     *
     * Belongs upstream in hal_gpt_lld.c; kept here to avoid another ChibiOS
     * working-tree patch to reapply after every submodule update. */
    SN_CT16B3->MCTRL = CT16_PWM_UNLOCK(SN_CT16B3->MCTRL | mskCT16_MRnRST_EN(0));
}

/* Stopping the tick (sleep.c: light sleep's bit 3, and the deep-sleep
 * experiment): every pin it drives low first, since a stopped tick leaves them
 * where they were; then the timer. The restart redoes the MR0RST fix and
 * forgets the last output, so the next tick writes every pin. Thread
 * (_run) and locked (I) forms. */
static bool pwm_tick_stopped = false;

static void pwm_tick_pins_offI(void) {
    display_backlight_pin_off();
    gpio_write_pin_low(LED_CAPS_LOCK_PIN);
    gpio_write_pin_low(LED_WINLOCK_PIN);
    gpio_write_pin_low(LED_CHARGING_PIN);
}

void pwm_tick_stopI(void) {
    if (pwm_tick_stopped) return;
    gptStopTimerI(&GPTD4);
    /* The LLD's stop leaves a latched match (its IC write is a
     * read-modify-write of a write-only register; codex): clear it here and
     * in the NVIC. */
    SN_CT16B3->IC = SN_CT16B3->RIS;
    NVIC_ClearPendingIRQ(CT16B3_IRQn);
    pwm_tick_pins_offI();
    pwm_tick_stopped = true;
}

void pwm_tick_startI(void) {
    if (!pwm_tick_stopped) return;
    ind_last_out = IND_OUT_UNKNOWN;   /* every pin written on the next tick */
    gptStartContinuousI(&GPTD4, PWM_TICK_TIMEBASE / PWM_TICK_HZ);
    SN_CT16B3->MCTRL = CT16_PWM_UNLOCK(SN_CT16B3->MCTRL | mskCT16_MRnRST_EN(0));
    pwm_tick_stopped = false;
#ifdef AK820_DEV_HOOKS
    od_tick_reprime();   /* the gap across the stop is not a service gap */
#endif
}

void pwm_tick_run(bool on) {
    chSysLock();
    if (on) pwm_tick_startI(); else pwm_tick_stopI();
    chSysUnlock();
}

bool indicators_any_lit(void) {
    return ind_caps || ind_winlock || ind_charging || ind_force;   /* a preview counts (P2a) */
}

/* The row-scan hook is now unused; leave the weak no-op in the driver. */

/* Claim Caps Lock from QMK core: returning false skips led_update_ports(), so
 * the pin is ours to PWM. keyboard.json keeps the `indicators` entry, which is
 * what defines LED_CAPS_LOCK_PIN and configures it as an output at init. */
bool led_update_kb(led_t led_state) {
    if (!led_update_user(led_state)) return false;
    ind_caps   = led_state.caps_lock;
    if (led_state.scroll_lock != ind_scroll) {
        ind_scroll     = led_state.scroll_lock;
        scr_changed_at = timer_read32();
        /* Not mid-frame in one that can move Scroll: a frame's first edge
         * could pass for the answer to a press made just before it (codex,
         * flash 2h). */
        if (scr_key_armed && timer_elapsed32(scr_key_at) < SCR_OWN_KEY_MS && !ch582_lockchan_holds_scroll()) {
            scr_shown     = ind_scroll;
            scr_key_armed = false;
        }
    }
    /* NOTE: led_state comes from whichever host driver is active
     * (host_keyboard_leds -> host_get_active_driver). In cable mode that is the
     * USB LED report and it is reliable. In BT/2.4G it is the CH582F's
     * host_leds, which ch582f_ajazz.c gates on is_module_connected and zeroes on
     * every 5B link-down -- so a wrong link state silently drops LED reports and
     * this LED goes dark while the host thinks Caps is on. Confirmed on hardware
     * 2026-08-28: flaky over BT, reliable wired. Same root cause as the BT
     * channel-digit bug. */
    return false;
}

/* --- Battery presence -----------------------------------------------------
 *
 * The module reports a LEVEL even with no pack fitted -- measured 0 on a naked
 * board (2026-09-09, the white unit) -- so the level alone cannot separate a
 * missing battery from a flat one, and the panel drew a confident "0%" for a
 * battery that was not there.
 *
 * The charger's two status pins can separate them, but not from their
 * INSTANTANEOUS state: what differs is duration. A flat cell on USB charges
 * continuously -- CHRG low, STDBY high, held for hours. With no cell the
 * charger either never starts, or terminates and restarts forever (these parts
 * cycle into an empty output). So the test is "has charging held unbroken for
 * CELL_CONFIRM_MS", which is true for a real pack and false for BOTH no-cell
 * behaviours -- and needs no knowledge of which of the two this board's charger
 * does, which is the part that could not be looked up.
 *
 * Biased towards false negatives deliberately. `cell_seen` LATCHES: a confirmed
 * pack is never later called missing. A spurious "no battery" mark on a healthy
 * board is worse than a missed one, and pulling a cell mid-session is not worth
 * flickering over -- it resolves on the next boot. */
#define CELL_CONFIRM_MS 3000u   /* unbroken charging that proves a pack exists */
#define CELL_GRACE_MS   15000u  /* don't judge before the charger has had a go  */
#define CELL_CONFIRM_TICKS (CELL_CONFIRM_MS / 100u)   /* update_leds() is 10 Hz */

static volatile bool     cell_seen = false;
static          uint16_t chrg_run  = 0;   /* consecutive 10 Hz ticks charging */

bool charge_is_charging(void) {
    return ind_charging;
}

bool battery_is_absent(void) {
    /* A pack we have already proved exists. */
    if (cell_seen) return false;
    /* The board has run from the pack this boot, so there is one. This used to
     * be "the slider is not in the cable position", but USB powers the board
     * in the BT position too, so a pack-less board on USB in BT never said
     * "No Batt" (codex review of the gauge plan, 2026-09-28). */
    if (battery_ran_from_pack()) return false;
    /* The module's recent reports are not all empty: not our case. The fresh
     * estimate (battery.c), not the last raw value, so one stray 0 from a pack
     * that is there cannot call it missing; no estimate yet (0xFF) is also no
     * verdict. */
    if (battery_5c_rounded() != 0) return false;
    /* Boot: give the charger time to assert before calling it missing. */
    return timer_read32() >= CELL_GRACE_MS;
}

/* Lock states for the LCD indicator band. Caps and Scroll are host-reported (and
 * so unreliable over BT -- see led_update_kb); GUI lock is a local QMK flag and
 * is always trustworthy. */
/* Fn-layer state, derived from the shared layer enum in ak820pro.h (it used
 * to be a hand-written (1<<1)|(1<<3) because the enum lived in keymap.c). */
#define FN_LAYER_MASK ((layer_state_t)((1UL << WINFN) | (1UL << MACFN)))

static volatile bool ind_fn = false;

layer_state_t layer_state_set_kb(layer_state_t state) {
    state  = layer_state_set_user(state);
    ind_fn = (state & FN_LAYER_MASK) != 0;
    return state;
}

bool lock_state_fn(void)      { return ind_fn; }
bool lock_state_caps(void)    { return ind_caps; }
bool lock_state_gui(void)     { return ind_winlock; }
bool lock_state_scroll(void)  { return scr_shown; }

void lock_note_scroll_key(void) {
    scr_key_at    = timer_read32();
    scr_key_armed = true;
}

uint8_t indicator_get_level(uint8_t which) {
    return which <= IND_CHARGING ? ind_lvl[which] : 0;
}

void indicator_set_level(uint8_t which, uint8_t level) {
    if (which > IND_CHARGING) return;
    if (level > IND_LEVEL_MAX) level = IND_LEVEL_MAX;
    ind_lvl[which] = level;
    if (which == IND_LOCKS) ind_duty_locks = ind_level_duty[level];
    else ind_duty_chg = ind_level_duty[level];
}

void indicator_force(uint8_t which, bool on) {
    if (which > IND_CHARGING) return;
    const uint8_t bit = (uint8_t)(1u << which);
    /* One byte store: the ISR reads it whole. Main loop only. */
    ind_force = on ? (uint8_t)(ind_force | bit) : (uint8_t)(ind_force & (uint8_t)~bit);
}

uint8_t indicator_forced(void) { return ind_force; }

void indicators_load_saved(void) {
    for (uint8_t w = IND_LOCKS; w <= IND_CHARGING; w++) {
        uint8_t level;
        if (kb_eeconfig_get_ind_level(w, &level)) indicator_set_level(w, level);
    }
}

void update_leds(void) {
    // Charging: on only while actively charging -- CHRG low (active) AND
    // STDBY high (not "done").
    ind_charging = !gpio_read_pin(CHARGE_CHRG_PIN) && gpio_read_pin(CHARGE_STDBY_PIN);

    /* Unbroken-charging run length -- the discriminator described above. Any
     * gap resets it, which is what makes a cycling charger fail to confirm. */
    if (ind_charging) {
        if (chrg_run < CELL_CONFIRM_TICKS) chrg_run++;
        if (chrg_run >= CELL_CONFIRM_TICKS) cell_seen = true;
    } else {
        chrg_run = 0;
    }

    // Windows Lock: mirrors the GUI-lock flag.
    ind_winlock = keymap_config.no_gui;

    // Scroll Lock for the band, once it has settled (see scr_shown). A frame
    // in progress that can move Scroll owns it: the settle clock starts after
    // it ends. P1 and P3 frames never touch Scroll, so they do not hold it.
    if (ch582_lockchan_holds_scroll()) scr_changed_at = timer_read32();
    if (scr_shown != ind_scroll && timer_elapsed32(scr_changed_at) >= SCR_SETTLE_MS) {
        scr_shown = ind_scroll;
    }
    // An unanswered KC_SCRL press expires, or ~49.7 days on the timer's wrap
    // would make it look recent again (codex, flash 2g).
    if (scr_key_armed && timer_elapsed32(scr_key_at) >= SCR_OWN_KEY_MS) {
        scr_key_armed = false;
    }
}
