/* rgb/rowisr_diag.h: the row ISR rewrite's bench diagnostics, dev builds only
 * (plans/ROW-ISR-PLAN.md, stage 0b; HC_DEVHOOK op 28). Empty otherwise.
 *
 * Op 28, [.., HC_DEVHOOK, 28, sub, args..], the reply at data[4..31]:
 *
 *   sub 0  timer registers   [5] timer 0-5 (CT16Bn), [6] page 0 or 1
 *          reply [4] timer, [5] page; page 0: [6..9] TMRCTRL, [10..11] TC,
 *          [12..13] PRE, [14..17] MCTRL, [18..21] PWMCTRL, [22..25] RIS,
 *          [26..29] the terminal match (MR9; MR12 on CT16B1);
 *          page 1, six words at [6..29]: CT16B1: MCTRL2, PWMENB, PWMIOENB,
 *          MR0, MR1, MR2; CT16B0/2/5: PC, CNTCTRL, EM, MR0, MR1, MR2;
 *          CT16B3/4: PC, CNTCTRL, EM, MR0, MR1, PWMmNIOCTRL. Reads only;
 *          nothing is cleared (IC is write-only and never written). The
 *          offsets are the manual's (docs/sn32f299.md, section 7), read only
 *          where that timer has the register.
 *   sub 1  a row-only hold-off  [5..6] ms, 1..10000, little-endian
 *          The row interrupt's NVIC line off for that long, everything else
 *          running (Q4: what a stall shows), bracketed by a MARKER on the
 *          Caps Lock LED's pin D15, driven high for the hold with the 20 kHz
 *          tick's writes to it paused: the rising edge and the NVIC disable
 *          sit in one critical section, the falling edge and the enable in
 *          another, so a probe on the LED sees the hold's length to a few
 *          cycles (the plan, stage 2's endpoint contract). The line is held
 *          through the driver's nesting hold (rowisr.h), so a clock-chip
 *          transaction overlapping the hold keeps the line off until its own
 *          end; the status says when that happened (the marked interval is
 *          then shorter than the line's, and the run is not a measurement).
 *          Timed by the 20 kHz tick (50 us steps); refused while a hold or a
 *          scan skip runs, while the tick is stopped (light sleep's bit 3),
 *          while the Caps Lock LED is lit or previewed (the pin would give no
 *          rising edge; the pin itself is checked under the lock, since a
 *          Caps just cleared stays high until the next tick), or out of
 *          range. A tick stop (light sleep's bit 3,
 *          deep sleep's entry) cancels the hold first, the line released and
 *          the marker dropped, counted apart; the housekeeping task ends a
 *          hold the tick somehow failed to end 50 ms past its deadline and
 *          counts that too. Reply as sub 2's.
 *   sub 2  status  reply [4] a hold is active, [5..6] its ms left, [7] the
 *          row line was enabled when the hold began, [8..9] holds ended by
 *          the tick, [10..11] ended by the backstop, [12..13] cancelled by a
 *          tick stop, [14] the row IRQ's number, [15] the row line enabled
 *          now, [16] the driver's hold depth now, [17] the last hold
 *          overlapped another holder (began or ended at depth above its
 *          own), [18] matrix_init_custom()'s prime timed out (Bootmagic
 *          scanned an empty matrix), [19] a scan skip is active, [20..21] its
 *          ms left.
 *   sub 3  one LED, one colour  [5] LED index 0..80 or 255 for off,
 *          [6..8] r, g, b
 *          Every other LED dark and that one at that colour, applied after
 *          each render (rgb_matrix_indicators_kb) until turned off, through
 *          the power scale as any frame is; it needs an effect rendering
 *          (RGB on, not NONE, not suspended), since it rides the render. For
 *          Q6 (which physical LED each mux pin drives) with a slow tagged
 *          slot sequence on the witness. Reply: the four bytes echoed at
 *          [4..7].
 *   sub 4  a scan skip  [5..6] ms, 1..10000
 *          The callback does its LED work and skips the key scan for that
 *          long (the matrix unsampled meanwhile), timed and cancelled as the
 *          hold is; health page 4's callback metric with and without it
 *          attributes the scan's cost (stage 0b). Refused under the hold's
 *          rules but for the Caps one. Measure in the ordinary mode: the
 *          skip does not touch scan-only's branch (light sleep's sizing
 *          bit). Reply as sub 2's.
 *
 * The replies carry no correlation field beyond the op byte, as every dev
 * op's do: one host process at a time, the bench's rule.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef AK820_DEV_HOOKS
bool rowisr_diag_op28(uint8_t *data);   /* data[4] = sub; false = RTC_UNHANDLED */
void rowisr_diag_tick(void);            /* from the 20 kHz tick's handler */
void rowisr_diag_cancelI(void);         /* from pwm_tick_stopI(): a hold or skip the tick can no longer end */
void rowisr_diag_housekeeping(void);    /* the backstop, once a pass */
#else
static inline void rowisr_diag_tick(void) {}
static inline void rowisr_diag_cancelI(void) {}
static inline void rowisr_diag_housekeeping(void) {}
#endif
