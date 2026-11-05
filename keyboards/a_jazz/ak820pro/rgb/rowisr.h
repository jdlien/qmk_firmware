/* The AK820 Pro's RGB matrix driver and key matrix, board-local
 * (plans/ROW-ISR-PLAN.md, D1; stage 1, 11-01).
 *
 * Until stage 1 the driver was QMK's drivers/led/sn32f2xx.c carrying this
 * repository's local commits, selected by keyboard.json's "sn32f2xx", and the
 * key matrix was quantum/matrix.c's row helpers called from that driver's
 * row ISR. Both are board code now, rgb/rowisr.c, selected by "custom" and
 * CUSTOM_MATRIX = lite (rules.mk). The exported names are kept as they were,
 * so every caller in the board (battery.c, power.c, sleep.c, rtc/rtc.c,
 * flash_tidy.c, hid_protocol.c, health.c, menu/) reads as before; renaming
 * is a later cleanup, not part of the rewrite. drivers/led/sn32f2xx.c is no
 * longer compiled for this board and stays in the branch for the other SN32
 * boards and UPSTREAM-CONTRIBUTIONS.md.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "matrix.h"   /* matrix_row_t */

/* The configuration the old driver took from config.h and its own defaults,
 * fixed to this board: 6 key rows x 3 colours = 18 mux rows (active high,
 * plain GPIO), 15 columns (hardware PWM, active low, shared with the key
 * matrix), and a CT16B0/B1/B2 column map (config.h, SN32F2XX_PWM_COL_MAP). */
#define SN32F2XX_RGB_MATRIX_ROW_CHANNELS 3
#define SN32F2XX_RGB_MATRIX_ROWS         MATRIX_ROWS
#define SN32F2XX_RGB_MATRIX_COLS         MATRIX_COLS
#define SN32F2XX_RGB_MATRIX_ROWS_HW      (SN32F2XX_RGB_MATRIX_ROWS * SN32F2XX_RGB_MATRIX_ROW_CHANNELS)
#ifdef RGB_MATRIX_ENABLE
#    define SN32F2XX_LED_COUNT RGB_MATRIX_LED_COUNT
#endif
#ifndef SN32F2XX_LED_OUTPUT_LUMINOSITY_R
#    define SN32F2XX_LED_OUTPUT_LUMINOSITY_R 1
#endif
#ifndef SN32F2XX_LED_OUTPUT_LUMINOSITY_G
#    define SN32F2XX_LED_OUTPUT_LUMINOSITY_G 1
#endif
#ifndef SN32F2XX_LED_OUTPUT_LUMINOSITY_B
#    define SN32F2XX_LED_OUTPUT_LUMINOSITY_B 1
#endif

/* The epoch clock (D3 step 1): the 32-bit extension of CT16B5 that the row
 * handler reconstructs its slots from, kept VALID by two observers, the
 * 20 kHz tick (indicators.c calls rowisr_time_observe() every 50 us) and the
 * handler itself; INVALID across an exclusion, which the owner of each mode
 * declares: deep sleep (the driver's own sleep hooks), degraded mode
 * (watchdog.c, read at init) and the dev lock mode (overrun_diag.c). */
#define ROWISR_EXCL_SLEEP    0x01u
#define ROWISR_EXCL_DEGRADED 0x02u
#define ROWISR_EXCL_LOCK     0x04u
void rowisr_time_observe(void);                 /* I-class: from the 20 kHz tick */
void rowisr_time_exclude(uint8_t bit, bool on); /* thread context */
typedef struct {   /* the extension's state, copied (not an observation): the silence's report */
    uint32_t ext, gen;
    uint16_t stamp, last_gap;   /* the last observer's CT16B5 stamp; the last gap the 8 ms guard rejected */
    uint16_t guard_rejects;     /* the guard's rejections so far, wrapping */
    uint8_t  valid;
} rowisr_time_t;
void rowisr_time_read(rowisr_time_t *t);         /* thread context */
uint16_t rowisr_usb_judge_seq(void);               /* thread context: the services entered, wrapping, an entry counted whether it judged or not; the silence report waits on two (rowisr_diag.c) */

/* The handler's record (D10): health page 7 (HC_GET7, the totals) and op 28
 * sub 5 (the rest). Its elapsed figures run from the entry snapshot to the
 * exit snapshot in the scheduler's 48 MHz ticks: the port's prologue and
 * epilogue, the health hooks and the accounting after the exit snapshot lie
 * outside them; page 4's hooks bracket the whole handler at 5.33 us
 * resolution, and the two together bound the complete cost. Reset with the
 * health counters. */
void rowisr_record_reset(void);
void rowisr_record_fill(uint8_t *out28);        /* page 7's 28 bytes (health.h) */
void rowisr_record_rows(uint8_t *out28);        /* op 28 sub 5 (rowisr_diag.h) */
void rowisr_record_usb(uint8_t *out28);         /* op 28 sub 6: the frame counter's record (D3 case 5) and the clocks' state */
#ifndef ROWISR_SECTIONS
#    define ROWISR_SECTIONS 0                   /* the handler's sections stamped (op 28 sub 11): ~6 us a slot of commit, so opt-in even in a dev build */
#endif
#if defined(AK820_DEV_HOOKS) && ROWISR_SECTIONS
void rowisr_record_sections(uint8_t *out28);    /* op 28 sub 11: the handler's sections, HCLK ticks summed; reset on read */
#endif
#ifdef AK820_DEV_HOOKS
void     rowisr_rig_set(uint8_t r0, uint8_t c0, uint8_t r1, uint8_t c1);   /* op 28 sub 12: the two positions counted at the scan (0xFF keeps one) */
void     rowisr_rig_read(uint8_t *out28);                                  /* their presses and releases, the presses elsewhere; zeroed on read */
uint16_t rowisr_settle_get(void);                                          /* op 28 sub 13: the column settle after the row select, HCLK ticks */
void     rowisr_settle_set(uint16_t ticks);
#endif
bool sn32f2xx_hw_fault(void);                   /* a timer's reset never completed at init: nothing runs, the columns released */
bool sn32f2xx_col_map_fault(void);              /* MATRIX_COL_PINS is not the order the scan's port reads assemble (stage 3): the matrix would read wrong */

/* rgb_matrix's driver entry points (rgb_matrix_driver, defined in rowisr.c).
 * Since stage 1 sn32f2xx_init() touches no hardware: the timers, the pins
 * and the row ISR start in matrix_init_custom(), before rgb_matrix exists. */
void sn32f2xx_init(void);
void sn32f2xx_flush(void);
/* D9 revised (11-04): the precision path. In a wide mode (power_wide_mode():
 * every enabled effect) the render buffer holds the chroma, the colours at
 * full global value (an LED's own brightness in them on the two Rains, the Heatmap
 * and Drift; the board's rgb_matrix_hsv_to_rgb() wrap sees to the rest), and
 * the flush applies the frame's value
 * (the config's under the ladder's cap) at 12 bits through the curve
 * (curve12.h). A pattern that writes raw bytes after the effect (the dev
 * Floor probe, the host's led command) blocks the wide path while it
 * stands, so the flush takes the byte path; its release re-inits the
 * effect for a full repaint (rowisr_diag.c). Thread context. */
void sn32f2xx_wide_block(bool on);
void sn32f2xx_set_color(int index, uint8_t r, uint8_t g, uint8_t b);
void sn32f2xx_set_color_all(uint8_t r, uint8_t g, uint8_t b);


/* The drive actually shown, time-weighted: the sum of every channel's value in
 * the COMMITTED frame (what the row ISR drives), times the ms it was shown,
 * accumulated since the last call, which closes the interval at `now_ms` and
 * starts the next. Divided by the elapsed ms it is the mean drive over the
 * interval, whatever the effect's frame pattern. `lit_ms`, if not NULL, gets
 * the ms of the interval in which any channel was lit; `rgb_ms`, if not NULL,
 * the same sum x ms per colour (red, green, blue), whose total is returned.
 * Main-thread only. */
uint64_t sn32f2xx_led_drive_take(uint32_t now_ms, uint32_t *lit_ms, uint64_t rgb_ms[3]);

/* True when every channel of the COMMITTED frame is zero. A scale or a colour
 * set this pass reaches that frame only at the next flush, so a board that
 * stops the row ISR (a deep sleep) waits for this: asleep the ISR keeps its
 * frame, and at the restart it drives that frame again until a render
 * replaces it. Main-thread only. */
bool sn32f2xx_led_committed_dark(void);

/* sn32f2xx_flush()'s calls, wrapping: one at the end of each of rgb_matrix's
 * renders, whether the frame changed or not (a steady Solid changes nothing).
 * A caller that changed what renders waits for it to move twice: the render
 * under way may have begun before the change, the next one began after it;
 * and a pass blocked meanwhile (a consolidation) flushes nothing.
 * Main-thread only. */
uint8_t sn32f2xx_led_commits(void);

/* What the row ISR does while an internal-flash program runs (EFLD1.state ==
 * FLASH_PGM). true (the default): the scan runs straight through, as between
 * programs; interrupts are masked only for each line's 10-20 us window, so a
 * row is at most that late. false: every mux row de-selected, the board dark
 * for the program's length (the 09-01 guard). A dev build's Write test page
 * switched it at run time to compare the two by eye (10-27 to 11-04;
 * plans/BACKLOG.md, "The LED blink at an internal-flash write"); nothing
 * sets it now. What scanning leaves is the erase,
 * which neither changes: the CPU, the ISR with it, stalls for each page
 * (docs/sn32f299.md, section 10). */
extern volatile bool sn32f2xx_pgm_scan;

/* Scale every LED's output by scale/256 (255 = unscaled) below the rgb_matrix
 * config: dim or darken the matrix without touching the user's brightness.
 * Takes effect as the next frame renders. Main-thread only. */
void sn32f2xx_set_power_scale(uint8_t scale);

/* The quiesce before the bootloader (D7): one idempotent teardown on every
 * jump route (watchdog.c's bootloader_jump: Bootmagic, Fn+Esc, the raw-HID
 * jump): the row interrupt off and its pending flag cleared, every column
 * channel off, every mux row low, the three timers stopped, then every LED
 * pin high-impedance, so no slot can be left driving and the bootloader
 * stays dark. Safe before the hardware is up. Thread context.
 * sn32f2xx_blank() is the old name for the same thing. */
void sn32f2xx_quiesce(void);
void sn32f2xx_blank(void);

/* True if matrix_init_custom()'s prime gave up before every key row had
 * been sampled (50 ms): Bootmagic then scanned an empty matrix. Dev op 28
 * reports it. */
bool sn32f2xx_prime_timed_out(void);

/* Scan-only, for a board's light sleep (dev builds, SN32F2XX_SLEEP_HOOKS):
 * while on, the row ISR drives no LED and scans one key row per call at a
 * longer period (SN32F2XX_SCAN_ONLY_TICKS), each key sampled every ~3.9 ms
 * against the field's ~4.6. The ISR applies it at its next call, and the LED
 * field resumes where it was when it ends. Only for a dark matrix: the
 * caller's job. */
void sn32f2xx_set_scan_only(bool on);
bool sn32f2xx_scan_only_active(void);

/* Deep sleep, for a board's, with interrupts locked (I-class): every LED
 * output off and the row ISR stopped, the key columns left as inputs with
 * pull-ups; and back. */
void sn32f2xx_sleep_enterI(void);
void sn32f2xx_sleep_exitI(void);

/* The row interrupt's line held off and released, nesting: the dev hold-off
 * (rowisr_diag.c) since stage 4 of the rewrite (11-03); until then the clock
 * chip's transaction (rtc/rtc.c) held it too, for the port MODE writes the
 * interim scanner made, which stage 3's scan does not. The
 * vector is CT16B4's since stage 2 (D2), and nothing outside this driver
 * names it. Holds NEST: the first hold disables the line and
 * remembers whether it was enabled, the last release puts that back, so two
 * holders overlapping never re-enable the line inside the other's hold.
 * Both return the depth after the call (hold: 1 means the sole holder;
 * release: 0 means the line is back as found). NVIC writes with their
 * barriers; the caller holds the system lock or runs I-class. */
uint8_t sn32f2xx_row_irq_hold(void);
uint8_t sn32f2xx_row_irq_release(void);
uint8_t sn32f2xx_row_irq_depth(void);
uint8_t sn32f2xx_row_irq_number(void);   /* the IRQn, for a diagnostic's reply */

#ifdef AK820_DEV_HOOKS
/* Dev op 28 sub 4: while true the callback does its LED work and skips the
 * key scan, so health page 4's callback metric with and without it
 * attributes the scan's cost (stage 0b). The matrix is unsampled meanwhile. */
extern volatile bool sn32f2xx_scan_skip;
#endif

/* Health hooks (health.c defines them; weak no-ops here). The ISR pair brackets
 * the callback's body; input_note_row_scan() runs in the ISR on every key-row
 * sample; input_note_consume() in the main loop after the locked copy, with
 * the XOR per row of the snapshot it copied (stage 3: exact). */
void sn32f2xx_isr_enter_hook(void);
void sn32f2xx_isr_exit_hook(void);
void input_note_row_scan(uint8_t row);
void input_note_consume(const matrix_row_t *diff, uint8_t rows);
