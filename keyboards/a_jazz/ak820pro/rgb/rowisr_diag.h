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
 *          ms left, [22] stage 2's init found a timer whose reset never
 *          completed (nothing runs, the columns released), [23] a silence
 *          of every interrupt is armed (sub 7), [24..25] its length in ms,
 *          [26..29] the last silence's measured length in CT16B5 ticks
 *          (5.33 us), [30] silences run (wrapping), [31] MATRIX_COL_PINS is not
 *          the order the scan's two port reads assemble (stage 3: the
 *          matrix would read wrong; rowisr.c, col_order).
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
 *   sub 5  the handler's record, the rest of it (stage 2, D10; page 7 has
 *          the totals): [4..5] the longest run of omissions, [6] invocations
 *          inside an epoch already serviced, [7] omissions by the arm
 *          deadline, [8] epoch losses the handler saw, [9] services whose
 *          interval was INVALID (each saturating at 255), [10..11] the
 *          handler's shortest elapsed time in 48 MHz ticks, [12..29]
 *          omissions per mux row, 18 bytes saturating at 255, [30..31]
 *          the elapsed samples dropped for a boundary inside the handler (stage 5a; page 7 has every service's count). Reset with the health
 *          counters.
 *   sub 6  the USB frame counter's record (D3 case 5) and the clocks' state:
 *          [4..7] intervals the frame counter judged (VALID, the driver
 *          ACTIVE at both ends, no bus event), [8] intervals it rejected for
 *          a disagreement of the spans, [9] intervals rejected for a bus
 *          event inside them (each saturating; each rejection is one epoch
 *          loss), [10..11] the worst modular distance among the accepted,
 *          ms, [12..13] the last rejected interval's span by the extension
 *          and [14..15] by the frame counter, ms, [16..19] the loss
 *          generation now (32 bits), [20..21] the bus-event generation,
 *          [22] the USB driver's state (hal_usb.h: 4 ACTIVE, 5 SUSPENDED),
 *          [23] the frame counter trusted now (ACTIVE, the transceiver on,
 *          no bus-event flag pending, FRMNO seen moving within 4 ms),
 *          [24..25] FRMNO now, [26] the extension VALID now, [27] the
 *          exclusion bits in force, [28..29] the last observation gap the
 *          8 ms guard rejected, CT16B5 ticks, [30] FRMNO seen moving within
 *          the last 4 ms, [31] bits 0-6 the intervals excluded without a
 *          loss (both ends inside a stale's recovery with no frame yet,
 *          saturating at 127), bit 7 the recovery pending now. The counters
 *          reset with the health counters.
 *   sub 7  a silence of every interrupt  [5..8] the length in us,
 *          1,000..8,000,000, little-endian; [9..10] a delay in ms before it,
 *          0 = 200 (so this reply reaches the host first)
 *          Fired by the housekeeping task after the delay: under the system
 *          lock, which masks every interrupt on this core, a MARKER on the
 *          Win Lock LED's pin C15 raised (the pin's read, its branch and the
 *          store after the mask), a spin on CT16B5 for the length with its
 *          wraps counted, the marker lowered by one store before the
 *          unmask, the lock released; each marker edge lies inside the
 *          silence, a few instructions from its end (the plan's endpoint
 *          contract; the masked part is one function and its disassembly
 *          is the count: nine instructions with three loads before the
 *          rising store, about 14 cycles, 0.3 us at 48 MHz, up to twice
 *          that with the flash's wait states; one store before the
 *          unmask), so a probe on that LED sees the silence to under a
 *          microsecond. The 20 kHz
 *          tick, the row handler and the USB handler all stop meanwhile.
 *          What the record (sub 6, page 7) shows afterwards is the verdict
 *          on the OBSERVATION GAP, the silence plus the observers' offsets at each
 *          end (the tick's 50 us period plus its service latency, which
 *          higher-priority handlers lengthen and no nominal figure bounds;
 *          scan-only's 625 us slot plus the handler's: the plan, D3 case
 *          2), not on the requested length: a
 *          gap under 8 ms exact, 8 ms and more INVALID by the guard with one
 *          loss, a gap near a multiple of 349.5 ms past the guard and, on
 *          USB, INVALID by the frame counter inside its 13 s scope; with no
 *          host's frames such a gap can read healthy: the plan's declared
 *          limit. The watchdog is not fed meanwhile (11.5 s nominal).
 *          Refused while another is armed or the Win Lock LED is lit or
 *          previewed, and not run (counted as lost, sub 8's [30]) if the
 *          pin is high at the firing; a tick stop or a deep sleep's entry
 *          cancels an armed one. May nest inside a row-only hold (sub 1),
 *          each on its own pin: the ordering case. Reply as sub 2's.
 *   sub 8  the last silence's report: [4..7] its length as requested, us,
 *          [8..11] as the spin measured it, CT16B5 ticks (5.33 us), [12..15]
 *          the extension's time before it and [16..19] 2 ms after it,
 *          [20..21] the loss generation before and [22..23] after (low 16
 *          bits), [24..25] CT16B5's TC at the rising edge and [26..27] at the
 *          falling edge, [28] the extension VALID before and [29] after, [30]
 *          silences not run for a high pin, [31] silences run.
 *   sub 9  its second page: [4..5] the last observer's CT16B5 stamp before
 *          and [6..7] after, [8..9] the last gap the 8 ms guard rejected,
 *          before and [10..11] after (CT16B5 ticks), [12..13] the guard's
 *          rejection count before and [14..15] after (wrapping). The
 *          extension's state is copied at each end, not observed. Read with
 *          sub 6's counts: a generation moved across the silence is one
 *          loss, the guard's if its count moved too, the frame counter's if
 *          sub 6's rejections did; a generation unchanged with the
 *          extension's reading far short of the spin's is the alias past
 *          the guard.
 *
 * The replies carry no correlation field beyond the op byte, as every dev
 * op's do: one host process at a time, the bench's rule.
 *   sub 10 the cost of a register read: 256 reads of one register in a masked
 *          loop timed by CT16B5, the least of 8 trials; the ticks for 256
 *          reads are the HCLK cycles a read costs, the loop included. Reply
 *          [4..5] USB FRMNO, [6..7] USB INSTS, [8..9] USB CFG, [10..11]
 *          CT16B5's TC, [12..13] a RAM word (the loop alone), [14] the
 *          trials, [15..16] the reads. The warm windows of 11-02.
 *   sub 11 the handler's sections: T_SCHED's TC stamped at seven points of
 *          the full path (rowisr.c, SEC_STAMP), the six deltas summed over
 *          the services that reached the exit. Reply [4..7] the services,
 *          [8..31] the six sums in HCLK ticks: 0 the entry snapshot, 1 step
 *          1's checks and the frame counter's judgment, 2 steps 2 to 5 (the
 *          mux rows, the scan, the match registers), 3 step 6 (the arm), 4
 *          the exit snapshot, 5 the exit judgment and the record. Reset on
 *          read: a window is two reads. Only in a build with
 *          ROWISR_SECTIONS=1 (rowisr.h): the commit costs ~6 us a slot
 *          (11-03), so the dev build JD runs leaves it out and answers
 *          unhandled.
 *   sub 12 the rig's keys counted at the scan (stage 3): [5..8] the two
 *          positions, row, col, row, col (0xFF keeps one; (0,13) and
 *          (3,14) at boot, the presser's plugs of 11-03). Reply [4..5] the
 *          presses (rising edges) and [6..7] the releases on the first,
 *          [8..9] and [10..11] on the second, [12..13] presses on every
 *          other key (phantoms while only the presser presses), [14..17]
 *          the positions in force; the counts zeroed by the read. Unlike
 *          raw_edges these see a press and release inside one consume.
 *   sub 13 the column settle after the row select, HCLK ticks (48 MHz):
 *          [5..6] a new value, 0..half the arm deadline, 1,916 at 125 us (0xFFFF keeps). Reply [4..5] the
 *          value in force. 96 (2 us) at boot; Q1's sweep: lower it with a
 *          cross-row chord held and the presser's batches running, and
 *          keep margin above the shortest setting that still counts exact.
 *   sub 14 FRMNO read back to back: [5..6] ms to scan, 1..60000 (0 reads
 *          the state without arming). The housekeeping task reads the
 *          register in a tight loop 3 ms a pass until the time is spent,
 *          every step between consecutive reads classed modulo 2,048.
 *          Reply [4..7] reads, [8..11] steps of +1, [12..15] steps other
 *          than 0 or +1, [16..17] ms left, [18] pairs kept (up to 3),
 *          [19..30] those (previous, next) pairs. A torn read shows as
 *          0x0FF -> 0x000 or the like; a preemption as a small step.
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
