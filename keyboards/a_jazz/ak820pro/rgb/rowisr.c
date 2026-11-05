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
 * - The columns' duties are the bytes scaled (D4, D8): MR = byte x k, k a
 *   fixed-point constant per cadence (10 at 267 us, 4.6875 at 125, the same
 *   duty; stage 2 calibrated 10 against the stage-0 holds, stage 5 checks
 *   125's against the logger), rounded; a multiply and a shift, not the flash table
 *   stages 2-4 carried (stage 5a: a flash load a column). The nine positions
 *   with no LED get an explicit zero (F-033). A match of 0 is a constant
 *   inactive level (docs/sn32f299.md, "PWM behaviour").
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
#include <stddef.h>
#include <string.h>
#include <hal.h>
#include "gpio.h"
#include "timer.h"
#include "matrix.h"
#ifdef RGB_MATRIX_ENABLE
#    include "rgb_matrix.h"
#    include "rgb_matrix_drivers.h"
#    include "curve12.h"   /* D9: the value's curve at 16 bits (scripts/gen_curve12.py) */
#endif
#include "rowisr.h"
#include "rowisr_diag.h"   /* the quiesce cancels a dev hold-off first (an inline no-op in other builds) */
#include "power.h"         /* D9: the ladder's value cap and the wide modes, read at the flush */

#define ROWS_PER_HAND (MATRIX_ROWS)
#ifndef MATRIX_INPUT_PRESSED_STATE
#    define MATRIX_INPUT_PRESSED_STATE 0
#endif
#if MATRIX_INPUT_PRESSED_STATE != 0
#    error "rowisr.c reads the columns as two port words with a press low (the pull-ups, COL2ROW)"
#endif
#ifndef ROWISR_SETTLE_TICKS
#    define ROWISR_SETTLE_TICKS 24u   /* the column settle after the row select, HCLK ticks: 0.5 us at 48 MHz (D3 step 3; Q1's sweep on stage 3, 11-03, was exact down to 0 ticks, so 2 us was margin the scan slot cannot afford at 125 us: stage 5d) */
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
/* The cadence (D2, D4; stage 5): the slot, the pulse timers' terminal and the
 * scale go together. 267 us is the measured stage-0 slot (11-01), stages 2-4's;
 * 125 us is stage 5's, a 444 Hz field: the window halves with the slot (the
 * literal terminal 2,047, 42.7 us, an arm deadline ~80 us in) and the scale
 * follows the field so the duty is the same (the measured 267 us duty at k =
 * 10 scaled by 125/267 = 4.6817; here 300/64 = 4.6875, checked against the
 * logger at stage 5's gate; D4's 4.845 came from the stage-0 slot and field).
 * The scale is Q6 fixed point, rounded: ticks = (byte x ROWISR_K_Q6 + 32) >> 6. */
#ifndef ROWISR_CADENCE_US
#    define ROWISR_CADENCE_US 267u
#endif
#if ROWISR_CADENCE_US == 125u
#    define ROWISR_SLOT_US    125u
#    define ROWISR_TERMINAL   1535u                              /* the pulse timers' terminal match: a 32 us window (stage 5d: 2,047's 80 us arm deadline omitted 0.29% of the scan slots on white, every one by the deadline, 12:49-12:54 11-03; 1,535 keeps 1.28x headroom over the full width, 1,195, and puts the deadline at 90.5 us) */
#    define ROWISR_K_REF_Q6   300u                             /* 4.6875 ticks a unit: the measured 267 us duty at k = 10 scaled by 125/267 (4.6817; D4's 4.845 came from the stage-0 slot and field), the logger's hold the arbiter (stage 5's gate; codex on 4294a3befd); the drive accounting's unit (D8) whatever scale is in force */
#    ifndef ROWISR_K_Q6   /* the floor probe gives 64 on the command line (build.sh devprobe): a byte is its width in ticks, the board at 21% of its light, for stage 6's first measurement (plans/ROW-ISR-PLAN.md, "The floor is measured first"); the accounting keeps the reference unit, so the power model reads the light emitted (codex's P2 on a1a0c783d5) */
#        define ROWISR_K_Q6   ROWISR_K_REF_Q6
#    endif
#elif ROWISR_CADENCE_US == 267u
#    define ROWISR_SLOT_US    267u
#    define ROWISR_TERMINAL   4095u                              /* an 85.3 us window */
#    define ROWISR_K_REF_Q6   640u                             /* 10 ticks a unit (stage 2's calibration) */
#    ifndef ROWISR_K_Q6
#        define ROWISR_K_Q6   ROWISR_K_REF_Q6
#    endif
#else
#    error "ROWISR_CADENCE_US: 267 or 125 (D2's cadences; 62.5 is the experiment, not provided)"
#endif
#define ROWISR_SLOT_TICKS     (ROWISR_SLOT_US * 48u)             /* 12,816 or 6,000 at HCLK */
#define ROWISR_WINDOW_TICKS   (ROWISR_TERMINAL + 1u)             /* a counter runs to its terminal whatever the duties */
#define ROWISR_ARM_MARGIN     120u                               /* the three arms' skew, the tail and the guard: 2.5 us */
#define ROWISR_ARM_DEADLINE   (ROWISR_SLOT_TICKS - ROWISR_WINDOW_TICKS - ROWISR_ARM_MARGIN)   /* 8,600 ticks (179 us in) at 267 us; 4,344 (90.5 us) at 125 */
#define ROWISR_WIDTH(b)       (((uint32_t)(b) * ROWISR_K_Q6 + 32u) >> 6)   /* a byte's pulse width, ticks, rounded (D4: floored, byte 1 was 20% under; exact at 640/64) */
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

/* The scale (D8): a byte's width is width_lut[byte], the rounded linear
 * formula ROWISR_WIDTH() tabulated in flash (512 B). Until D9 (11-04) the
 * handler loaded it a column; since D9 the flush does, for the byte path
 * (the effects whose value varies per LED, and raw patterns), and the wide
 * path uses curve_q6[] (curve12.h) instead; the handler reads the widths
 * themselves. Every width strictly below the terminal (D2). */
#if ((255u * ROWISR_K_Q6 + 32u) >> 6) >= ROWISR_TERMINAL   /* the preprocessor cannot see ROWISR_WIDTH()'s cast */
#    error "every channel match must stay strictly below the terminal (D2)"
#endif
#define W1(i) (uint16_t)ROWISR_WIDTH(i)
#define W4(i) W1(i), W1((i) + 1), W1((i) + 2), W1((i) + 3)
#define W16(i) W4(i), W4((i) + 4), W4((i) + 8), W4((i) + 12)
#define W64(i) W16(i), W16((i) + 16), W16((i) + 32), W16((i) + 48)
static const uint16_t width_lut[256] = {W64(0), W64(64), W64(128), W64(192)};

#ifdef RGB_MATRIX_ENABLE
/* The front frame (D9 revised, 11-04): each LED's pulse widths in ticks, in
 * the pins' order R, B, G, 12 bits each; a value above the terminal is a
 * dithered tick (DITHER_HALF: one tick every second field, DITHER_QUARTER:
 * every fourth), resolved by the handler against field_phase, which the
 * rotation's wrap advances. rgb_matrix's render buffer stays bytes: the
 * flush converts, by the curve at the frame's value (the config's under
 * the ladder's cap, read there) on every enabled effect
 * (power_wide_mode(): the bytes are the chroma, at full global value, an LED's
 * own brightness in them on the effects that have one), or by width_lut[] for
 * raw bytes (the dev pattern while it stands, and a future effect not added
 * to that list). */
static uint16_t led_w[SN32F2XX_LED_COUNT][3];
/* A dithered entry (JD's eye, 11-04 evening: whole-tick rounding moved a
 * purple's red-to-blue ratio by a third between 3% and 6%): bit 15 set, a
 * four-field pattern in bits 11-14 (one bit a field, the field's bit the
 * phase's low two bits select) and the base width in bits 0-10; the
 * handler drives base + the pattern's bit. The fraction of a tick is kept
 * to a quarter for widths under 16 ticks. */
#define DITHER_FLAG    0x8000u
#define DITHER_BASE    0x07FFu
#define DITHER_PAT_SHIFT 11u
static uint8_t  field_phase = 0;
static RGB      led_state_buf[SN32F2XX_LED_COUNT]; /* rgb_matrix's: the next frame */
static bool     led_state_buf_update_required = false;
static bool     wide_blocked = false;   /* a raw pattern stands (the dev Floor probe, the host's led command): the byte path whatever the mode */
static bool     last_wide    = false;   /* the committed frame's encoding and value: an encoding or a value change forces the conversion */
static uint8_t  last_v       = 0;
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
 * it prepares (0..5; the row SCANNED in that slot is the one before it, the
 * slot handler's rotation comment, 11-04); the key row advances when the mux row's colour index is 0,
 * the first pin of its group (R, B, G per key row: config.h's order, Q6).
 * The old driver submitted B, G, then R of the NEXT key row, one slot ahead
 * of the pins, and rendered right, so its widths evidently reached the LEDs
 * a cycle after their write (the CT16's doing, not measured). The one-shot
 * restart takes the widths written before it, so stage 2's first flash
 * rendered every frame rotated through that lead: R from B, B from G, G
 * from the row below's R (peach alphas and pink mods from a purple and blue
 * theme, and magenta above each of the nine holes at a white frame; white,
 * 11-01). */
static uint8_t cur_row    = 0;
static uint8_t key_row    = 0;
static uint8_t cur_colour = 0;   /* cur_row % SN32F2XX_RGB_MATRIX_ROW_CHANNELS, counted beside it: no divide on the slot */
_Static_assert(SN32F2XX_RGB_MATRIX_ROWS_HW % SN32F2XX_RGB_MATRIX_ROW_CHANNELS == 0, "the mux rows are whole key rows, so cur_row and cur_colour wrap together");

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
 * time while INVALID (a re-prime refused for its own gap, an exclusion's
 * entry while already INVALID, an exclusion's end): wherever time is
 * discarded the generation moves, so no consumer baseline stays continuous
 * across it; INVALID becomes VALID only by a re-prime whose gap passes the
 * guard, while no exclusion is active. Each observation is a critical
 * section of a few instructions. */
/* The frame counter's freshness (below, "The USB frame counter"): FRMNO's
 * last seen value and the extension's time then, tracked by both observers
 * while the extension runs; staled wherever the extension discards time
 * (the guard's rejected gap, an exclusion's end), never by the handler's
 * own rejection, which discards nothing. */
#if ROWISR_USB_DETECT   /* the detector compiled in (config.h: the dev build's default; the released builds leave it out, ~7 us a slot) */
#define USB_FRMNO_MASK    0x7FFu          /* the LLD's usbhw.h (mskFRAME_NO, mskBUS_*), not on this file's include path */
#define USB_FRM_FRESH     750u            /* 4 ms of CT16B5 ticks: FRMNO must have moved this recently to be trusted */
#define USB_FRM_PENDING   1500u           /* 8 ms more: a stale's recovery ends without a frame (no host), pending no longer */
#define USB_FRM_TICKS_X2  375u            /* one frame, 1 ms, is 187.5 CT16B5 ticks: the spans compare at twice the tick */
#define USB_WRAP_X2       768000UL        /* 2,048 frames at twice the tick: the frame counter's modulus */
#define USB_TOL_TICKS     563u            /* 3 ms of ticks, rounded up: the tolerance's fixed part */

/* The modular distance between an interval's two spans, the extension's in
 * CT16B5 ticks and the frame counter's in frames (ms), returned in ticks:
 * both taken at twice the tick so a frame's half tick is kept, the
 * difference folded at the frame counter's modulus. No divide below
 * 2.048 s, which is every interval the handler judges (a slot, or a
 * hold-off; the guard ends any gap of 8 ms between observations); the one
 * divide stays for a longer span (the supported hour, never seen). The
 * milliseconds the record reports are made off the handler
 * (rowisr_record_usb). The warm windows of 11-02 put the two ms
 * conversions that stood here, with the snapshots, at 19 us a slot: a
 * software divide each on this core. */
static inline uint32_t usb_distance_ticks(uint32_t dext, uint32_t df) {
    uint32_t dx2 = dext < USB_WRAP_X2 / 2u ? dext * 2u : (dext * 2u) % USB_WRAP_X2;   /* x2 fits: the span is under an hour */
    uint32_t fx2 = df * USB_FRM_TICKS_X2;
    uint32_t d   = dx2 > fx2 ? dx2 - fx2 : fx2 - dx2;
    if (d > USB_WRAP_X2 / 2u) d = USB_WRAP_X2 - d;
    return d >> 1;
}

/* FRMNO read twice at least twelve cycles apart, and a third time if the
 * two differ: the register updates in two steps at a byte carry, the low
 * byte first (0x7FF reads 0x700 then 0x000, 0x1FF reads 0x100 then 0x200:
 * the back-to-back scan of 11-03, op 28 sub 14, 48 torn reads in 16.8
 * million over 92 carries, a torn state lasting 4-6 HCLK cycles, about one
 * 12 MHz USB clock), and a torn value in the detector's snapshot gave the
 * pairs of false rejections at spans 0/256 ms (the trim, the exit-only
 * build, and once on stage 3 with two reads back to back, which can both
 * land inside the torn state). The spacing puts the second read past any
 * torn state the first was in; two reads that agree are then a settled
 * value, and a third, spaced again, is past a step that fell between the
 * first two. A mitigation measured on this unit, not a datasheet figure
 * (codex on cf7b61560d). */
#define FRMNO_SPACE() __asm__ volatile("nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop")
static inline uint16_t frmno_read(void) {
    uint16_t a = (uint16_t)(SN32_USB->FRMNO & USB_FRMNO_MASK);
    FRMNO_SPACE();
    uint16_t b = (uint16_t)(SN32_USB->FRMNO & USB_FRMNO_MASK);
    if (a == b) return a;
    FRMNO_SPACE();
    return (uint16_t)(SN32_USB->FRMNO & USB_FRMNO_MASK);
}
static uint16_t frm_seen     = 0xFFFFu;
static uint32_t frm_seen_ext = 0;
static uint8_t  frm_pending  = 0;         /* a stale happened and no frame has moved since: the recovery (the fourth verify review's 1) */
#endif /* ROWISR_USB_DETECT */

static volatile uint32_t time_ext   = 0;
static volatile uint16_t time_stamp = 0;
static volatile uint8_t  time_valid = 0;
static volatile uint32_t time_gen   = 0;   /* 32 bits (the verify review's 7: 16 could alias after 65,536 losses) */
static volatile uint16_t time_last_gap = 0;   /* the last observation gap the guard rejected, ticks (the record) */
static volatile uint16_t time_guard_rejects = 0;   /* the guard's rejections, wrapping (the report's attribution) */

/* The frames' freshness staled, with a new baseline: FRMNO as it reads now,
 * not fresh; only a frame seen moving after this restores the trust (a
 * movement inside a discarded gap cannot: the second verify review's 1). */
#if ROWISR_USB_DETECT
static inline void frm_staleI(void) {
    frm_seen     = frmno_read();
    frm_seen_ext = time_ext - USB_FRM_FRESH;
    frm_pending  = 1;
}
#else
static inline void frm_staleI(void) {}   /* no detector in this build: the extension's own accounting is unchanged (the stales were its bookkeeping for the frames) */
#endif
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
    time_valid = 0;   /* unconditionally: an exclusion entered while already INVALID discards more time, so the generation
                       * moves again, and a baseline a consumer adopted meanwhile cannot stay continuous across it (the
                       * seventh verify review's 1); the handler counts at most one loss per entry, whatever the count of bumps */
    time_gen++;
    if (stale_frames) frm_staleI();
}

/* FRMNO tracked by both observers, so a row-only hold the tick keeps VALID
 * keeps the frames fresh too (a hold across a 2.048 s frame wrap could
 * otherwise return to an equal FRMNO; the verify review). I-class. */
#if ROWISR_USB_DETECT
static inline void frm_trackI(void) {
    if (time_excl) return;   /* excluded: the extension is frozen, so a stamp now would read as no age at all (the third verify review's 1) */
    uint16_t frm = frmno_read();
    if (frm != frm_seen) {
        frm_seen     = frm;
        frm_seen_ext = time_ext;
        frm_pending  = 0;
    } else if (frm_pending && (time_ext - frm_seen_ext) >= USB_FRM_FRESH + USB_FRM_PENDING) {
        frm_pending = 0;   /* 8 ms after the stale and no frame: the bus has no host; the recovery ends, the frames untrusted */
    }
}
#else
static inline void frm_trackI(void) {}
#endif

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
        if (was != 0 && time_excl == 0) {   /* an exclusion really ended (not a clear mask cleared again: the fourth verify review's 4): the excluded
                                             * time is discarded here, so the generation moves, the frames' baseline is taken anew, and the stamp is
                                             * rebased to now, so the re-prime that follows adds only the time since the release, none from before it
                                             * (the eighth verify review's 1: pre-release time could age the recovery out before its first frame) */
            time_gen++;
            time_stamp = sys_tc();
            frm_staleI();
        }
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
        if (was != 0 && time_excl == 0) {
            time_gen++;
            time_stamp = sys_tc();
            frm_staleI();
        }
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
 * tolerance.
 *
 * The judgment runs at every service, on the interval from the last
 * serviced entry (last_snap) to this one. Stage 5g (11-03, 1dfa597991 to
 * 4f19e6c31f) ran it every eighth service instead, the evidence kept at
 * every one, a discontinuity judged before its interval was discarded and
 * three rules for the coverage, through three codex rounds; measured on
 * white it saved 0.2 us of the detector's ~7 (52.1 us a slot and 41.7%
 * against 52.3 and 41.8% at full white), since the cost is the snapshot's
 * register reads, not the judgment, so the cadence was retired for the
 * per-slot form and the detector compiled out of the released builds
 * instead (ROWISR_USB_DETECT, config.h: the dev build's default, where the
 * bench reads it through op 28). Without it the handler's snapshot is the
 * extension alone, and the tick's 349.5 ms alias is the declared limit on
 * USB as it is off it (plans/ROW-ISR-PLAN.md, D3 case 5, "Stage 5g's
 * reviews"). */
#if ROWISR_USB_DETECT
static volatile uint16_t usb_gen = 0;   /* bus events the USB handler serviced: reset, suspend, wakeup */
#define USB_INSTS_WAKEUP  (1UL << 25)
#define USB_INSTS_RESUME  (1UL << 29)
#define USB_INSTS_SUSPEND (1UL << 30)
#define USB_INSTS_RESET   (1UL << 31)
#define USB_BUS_EVENTS    (USB_INSTS_RESET | USB_INSTS_SUSPEND | USB_INSTS_RESUME | USB_INSTS_WAKEUP)
#define USB_CFG_PHY_EN    (1UL << 30)     /* USB_CFG.PHY_EN, the transceiver on (sleep.c, F-045) */

void usb_event_kb(usbevent_t event);
void usb_event_kb(usbevent_t event) {   /* the USB handler (ISR context): tmk_core/protocol/chibios/usb_main.c; its weak default stands without the detector */
    if (event == USB_EVENT_RESET || event == USB_EVENT_SUSPEND || event == USB_EVENT_WAKEUP) usb_gen++;
}
#endif /* ROWISR_USB_DETECT */

/* A coherent snapshot of the clocks: the extension observed, the scheduler's
 * TC, the generation and the validity (step 6 and the exit take this one);
 * step 1's adds the frame counter with its trust and the bus-event
 * generation, four more loads and the freshness bookkeeping, in the same
 * critical section. */
typedef struct {
    uint32_t ext;
    uint32_t gen;
    uint16_t tc;
#if ROWISR_USB_DETECT
    uint16_t frm;      /* FRMNO at the observation (step 1's snapshot) */
    uint16_t ugen;     /* the bus-event generation (step 1's) */
#endif
    uint8_t  valid;
#if ROWISR_USB_DETECT
    uint8_t  bus;      /* the bus up at this end: the driver ACTIVE, the transceiver on, no bus-event flag pending (step 1's) */
    uint8_t  fresh;    /* a frame seen moving within the last 4 ms (step 1's) */
    uint8_t  pending;  /* a stale's recovery: no frame seen since it, for 8 ms at most (step 1's) */
#endif
} snap_t;

static inline void take_timeI(snap_t *s) {
    time_observeI();
    s->ext   = time_ext;
    s->tc    = (uint16_t)CT_REG(T_SCHED, CT_TC);
    s->gen   = time_gen;
    s->valid = time_valid;
#if ROWISR_USB_DETECT
    s->frm     = 0;
    s->ugen    = 0;
    s->bus     = 0;
    s->fresh   = 0;
    s->pending = 0;
#endif
}

static inline void take_snapI(snap_t *s) {   /* step 1's: the time, and with the detector the frame counter's trust and the bus (six register reads, the detector's cost) */
    take_timeI(s);
#if ROWISR_USB_DETECT
    frm_trackI();
    s->frm     = frm_seen;
    s->ugen    = usb_gen;
    s->bus     = (USBD1.state == USB_ACTIVE && (SN32_USB->CFG & USB_CFG_PHY_EN) != 0 && (SN32_USB->INSTS & USB_BUS_EVENTS) == 0) ? 1u : 0u;
    s->fresh   = (time_ext - frm_seen_ext) < USB_FRM_FRESH ? 1u : 0u;
    s->pending = frm_pending;
#endif
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
        uint32_t q = (uint32_t)dt + half;
        if (q < 2u * slot_ticks) return q >= slot_ticks ? 1u : 0u;   /* one boundary or none, every slot's and every exit's case: by
                                                                       * comparison, not the software divide this core has (the warm
                                                                       * windows of 11-02: a divide here at step 1 and at the exit) */
        return q / slot_ticks;                                        /* a late service: the count */
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
    uint32_t serviced_valid;   /* the services whose interval since the last was VALID: the population of slots_elapsed */
    uint32_t emitted;          /* lit pulses armed */
    uint32_t omitted;          /* lit rows left dark: late for the arm, or preempted across a slot; a dark row is neither */
    uint32_t elapsed_sum;      /* entry snapshot to exit snapshot, HCLK ticks, wrapping after 89 s of handler time: windows, not lifetimes */
    uint32_t elapsed_samples;  /* the population of elapsed_sum */
    uint16_t elapsed_min, elapsed_max;   /* the maximum clips at 0xFFFF (1,365 us) */
    uint16_t latency_max;      /* entry TC, services with exactly one boundary crossed */
    uint16_t elapsed_crossed;  /* services whose elapsed sample was dropped for a boundary inside the handler (stage 5a), wrapping */
    uint16_t omit_run, omit_run_max;
    uint8_t  epoch_losses;     /* generation changes the handler saw, saturating */
    uint8_t  invalid_services; /* services whose interval was INVALID, saturating */
    uint8_t  spurious;         /* invocations inside the epoch already serviced, saturating */
    uint8_t  late_omit;        /* omissions by the arm deadline (a subset of omitted), saturating */
    uint8_t  omit_row[SN32F2XX_RGB_MATRIX_ROWS_HW];   /* omissions per mux row (row and colour), saturating */
    uint32_t usb_checked;      /* intervals the frame counter judged: VALID, the driver ACTIVE at both ends, no bus event */
    uint16_t usb_worst;        /* the worst modular distance among the intervals it accepted, CT16B5 ticks, saturating */
    uint32_t usb_last_dt;      /* the last rejected interval's span by the extension, CT16B5 ticks (reported in ms) */
    uint16_t usb_last_df;      /* and by the frame counter, frames (ms) */
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
static volatile uint16_t usb_judge_seq = 0;   /* the services entered past the spurious check, wrapping, an entry counted whether it judged or not
                                               * (one the generation had invalidated judges nothing): with the detector, two of them since before a
                                               * silence put one after it, which judges it if anything does; the silence report waits on it
                                               * (rowisr_diag.c); counted in every build so that wait holds without the detector */
#if ROWISR_USB_DETECT
static bool     adopt_pending = false;   /* a frame-counter rejection's generation, for the committed baseline (this invocation only) */
static uint32_t adopt_gen     = 0;

/* D3 case 5: the frame counter's verdict on the interval a to b, a function
 * of the two snapshots alone (the record's counts aside). Judged when the
 * interval began with the frames expected: trusted (fresh), or inside a
 * stale's recovery (the start's FRMNO is real either way: the stale took it
 * from the hardware); not judged (UNTRUSTED) when the bus is down or the
 * frames are known stopped at the start, the declared blind spot. */
typedef enum { USB_UNTRUSTED = 0, USB_KEPT, USB_REJECT, USB_UNJUDGED } usb_verdict_t;
static inline bool snap_trusted(const snap_t *s) {   /* the frames expected at this end: the bus up, and a frame seen within 4 ms or a stale's recovery */
    return s->bus && (s->fresh || s->pending);
}
static usb_verdict_t usb_judge(const snap_t *a, const snap_t *b) {
    if (!snap_trusted(a)) return USB_UNTRUSTED;
    bool     reject = false;
    uint32_t dext   = b->ext - a->ext;                                 /* the extension's span, 5.33 us ticks; under an hour */
    uint32_t df     = (uint32_t)(b->frm - a->frm) & USB_FRMNO_MASK;    /* the frame counter's, frames, modulo 2,048 */
    if (!b->bus || b->ugen != a->ugen) {   /* the bus down at the end, or a bus event serviced inside: never healthy */
        sat8(&rec.usb_events);
        reject = true;
    } else if (!b->fresh) {
        if (a->fresh) {   /* moving at the start, not within 4 ms of the end: the frames stopped inside, no flag raised */
            sat8(&rec.usb_events);
            reject = true;
        } else {          /* both ends inside the recovery with no frame yet (its first slots, or a bus that never
                           * sends one): unjudgeable, excluded without a loss of its own, the stale's loss covers it;
                           * a silence inside is excluded with it, never healthy (the fourth verify review's 1) */
            if (rec.usb_unjudged < 127u) rec.usb_unjudged++;   /* seven bits on the wire: bit 7 is the recovery flag */
            return USB_UNJUDGED;
        }
    } else {
        uint32_t diff = usb_distance_ticks(dext, df);             /* the modular distance, ticks, no divide */
        rec.usb_checked++;
        reject = diff > USB_TOL_TICKS + (dext >> 9);              /* 3 ms plus the span / 512, about 0.2% */
        if (reject) sat8(&rec.usb_rejects);
        else if (diff > rec.usb_worst) rec.usb_worst = diff > 0xFFFFu ? 0xFFFFu : (uint16_t)diff;   /* under the tolerance, which grows with
                                                                                                    * the span: saturated for a span past 3 min */
    }
    if (!reject) return USB_KEPT;
    rec.usb_last_dt = dext;
    rec.usb_last_df = (uint16_t)df;
    return USB_REJECT;
}
#endif /* ROWISR_USB_DETECT */

uint16_t rowisr_usb_judge_seq(void) {   /* thread context: a volatile halfword, one load */
    return usb_judge_seq;
}

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
    uint16_t orm = rec.omit_run_max, emn = rec.elapsed_min, ecr = rec.elapsed_crossed;
    uint8_t  sp = rec.spurious, lo = rec.late_omit, el = rec.epoch_losses, iv = rec.invalid_services;
    uint8_t  rows[SN32F2XX_RGB_MATRIX_ROWS_HW];
    memcpy(rows, rec.omit_row, sizeof(rows));
    chSysUnlock();
    out28[0] = orm & 0xFF; out28[1] = orm >> 8;
    out28[2] = sp; out28[3] = lo; out28[4] = el; out28[5] = iv;
    emn = emn == 0xFFFFu ? 0u : emn;
    out28[6] = emn & 0xFF; out28[7] = emn >> 8;
    memcpy(&out28[8], rows, SN32F2XX_RGB_MATRIX_ROWS_HW);          /* 8..25 */
    out28[26] = ecr & 0xFF; out28[27] = ecr >> 8;                   /* the elapsed samples dropped for a boundary inside the handler (stage 5a; page 7 has the services) */
}

void rowisr_record_usb(uint8_t *out28) {   /* op 28 sub 6: the frame counter's record and the clocks' state (rowisr_diag.h) */
    chSysLock();
    uint32_t ck = rec.usb_checked, gen = time_gen;
    uint32_t ldt_t = rec.usb_last_dt;
    uint16_t wm_t = rec.usb_worst, ldf = rec.usb_last_df;
    uint8_t  rj = rec.usb_rejects, ev = rec.usb_events, uj = rec.usb_unjudged, tv = time_valid, tx = time_excl;
    uint8_t  st  = (uint8_t)USBD1.state;
#if ROWISR_USB_DETECT
    uint16_t ug  = usb_gen;
    uint16_t frm = frmno_read();
    uint8_t  fresh = (time_ext - frm_seen_ext) < USB_FRM_FRESH ? 1u : 0u;
    uint8_t  live = (USBD1.state == USB_ACTIVE && (SN32_USB->CFG & USB_CFG_PHY_EN) != 0 &&
                     (SN32_USB->INSTS & USB_BUS_EVENTS) == 0 && fresh) ? 1u : 0u;
    uint8_t  pend = frm_pending;
#else
    uint16_t ug = 0, frm = 0;              /* no detector in this build: the frame counter's bytes read zero (the counts above stay zero too) */
    uint8_t  fresh = 0, live = 0, pend = 0;
#endif
    uint16_t lgap = time_last_gap;
    chSysUnlock();
    uint16_t wm    = (uint16_t)((wm_t * 2u + 187u) / 375u);        /* ticks to ms, rounded: the divides live here, off the handler */
    uint32_t ldt32 = (ldt_t * 2u + 187u) / 375u;                    /* an hour's ticks x 2 fits */
    uint16_t ldt   = ldt32 > 0xFFFFu ? 0xFFFFu : (uint16_t)ldt32;
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

/* ---- The key matrix's scan (stage 3, D3 step 3) ------------------------------
 *
 * The key rows are push-pull outputs driven high and the columns inputs with
 * their pull-ups from rowisr_hw_init() (deep sleep drives the rows low for
 * its wake and high again at it, sleep.c, before the scan resumes), and the
 * running handler never writes a port's MODE, CFG or whole DATA register:
 * the select is one BCLR, the deselect one BSET. The interim scanner's
 * select re-asserted output mode on every scan, a read-modify-write of the
 * port's MODE, which is why rtc/rtc.c holds this interrupt off through the
 * clock chip's transaction (its own MODE writes on the same port); the
 * hold-off stays through stage 3 and D6 lifts it at stage 4. The columns
 * are read as two port words, GPIO0 and GPIO2, and the 15 bits assembled in
 * MATRIX_COL_PINS order (keyboard.json: A4, A5, C0-C3, A6, A7, C4-C7, C14,
 * C8, C9; a press reads low, pulled through its diode into the driven-low
 * row); rowisr_hw_init() checks the pin table against that order and op 28
 * sub 2 reports a mismatch. The settle between the select and the read is
 * timed on the scheduler's counter in HCLK ticks, ROWISR_SETTLE_TICKS (2 us)
 * by default; a dev build sets it (sub 13) for Q1's sweep. No spin after the
 * deselect: the PWM drives the columns next, and the next scan is three
 * slots away. */
static inline matrix_row_t columns_read(void) {
    uint32_t a = ~palReadPort(GPIOA);   /* inverted: a set bit is a press */
    uint32_t c = ~palReadPort(GPIOC);
    return (matrix_row_t)(((a >> 4) & 0x3u) | ((c & 0xFu) << 2) | (((a >> 6) & 0x3u) << 6) | (((c >> 4) & 0xFu) << 8) |
                          (((c >> 14) & 0x1u) << 12) | (((c >> 8) & 0x3u) << 13));
}
static const pin_t col_order[SN32F2XX_RGB_MATRIX_COLS] = {A4, A5, C0, C1, C2, C3, A6, A7, C4, C5, C6, C7, C14, C8, C9};   /* what columns_read() assembles */
static bool col_map_fault = false;   /* MATRIX_COL_PINS is not col_order: columns_read() would assemble the wrong bits (op 28 sub 2, [31]) */
bool sn32f2xx_col_map_fault(void) {
    return col_map_fault;
}

#ifdef AK820_DEV_HOOKS
static volatile uint16_t settle_ticks = ROWISR_SETTLE_TICKS;   /* op 28 sub 13: Q1's sweep */
uint16_t rowisr_settle_get(void) {
    return settle_ticks;
}
void rowisr_settle_set(uint16_t ticks) {
    settle_ticks = ticks > ROWISR_ARM_DEADLINE / 2u ? (uint16_t)(ROWISR_ARM_DEADLINE / 2u) : ticks;   /* half the arm deadline at most (89 us at 267, 40 at 125): a scan slot must still reach its arm */
}
#else
#    define settle_ticks ROWISR_SETTLE_TICKS
#endif
static inline void scan_settle(void) {
    uint32_t t0 = CT_REG(T_SCHED, CT_TC);
    for (;;) {
        int32_t d = (int32_t)CT_REG(T_SCHED, CT_TC) - (int32_t)t0;
        if (d < 0) d += (int32_t)slot_ticks;   /* the counter's reset at the slot boundary (a late scan): the elapsed time modulo the period in force, so the
                                                * wait is never cut short (codex's P1 on cf7b61560d: an unsigned difference across the reset ended it at once) */
        if ((uint32_t)d >= settle_ticks) break;   /* settle_ticks is bounded under every period (half the arm deadline at most, against the slot and scan-only's), so this ends */
    }
}

#ifdef AK820_DEV_HOOKS
/* The rig's keys counted at the scan (stage 3; op 28 sub 12): the rising and
 * falling edges on two positions, the presser's, and the rising edges on
 * every other key, which are phantoms while only the presser presses.
 * raw_edges, counted at the consume, cannot see a press and release inside
 * one consume; these can. Set, read and zeroed off the handler. */
static volatile uint8_t rig_row[2] = {0, 3}, rig_col[2] = {13, 14};   /* (0,13) Delete and (3,14) PgDn: the plugs of 11-03 */
static uint16_t rig_press[2], rig_release[2], rig_other_press;
static inline void rig_note(uint8_t row, matrix_row_t prev, matrix_row_t now) {
    matrix_row_t rise = (matrix_row_t)(now & ~prev), fall = (matrix_row_t)(prev & ~now);
    if ((rise | fall) == 0) return;   /* the common case: a few instructions on a scan slot */
    for (uint8_t i = 0; i < 2u; i++) {
        if (row != rig_row[i]) continue;
        matrix_row_t m = (matrix_row_t)(1u << rig_col[i]);
        if (rise & m) {
            rig_press[i]++;
            rise = (matrix_row_t)(rise & ~m);
        }
        if (fall & m) rig_release[i]++;
    }
    while (rise) {
        rig_other_press += (uint16_t)(rise & 1u);
        rise >>= 1;
    }
}
void rowisr_rig_set(uint8_t r0, uint8_t c0, uint8_t r1, uint8_t c1) {   /* 0xFF keeps a position; two equal positions are refused (key 0 would eat key 1's rises) */
    uint8_t nr0 = rig_row[0], nc0 = rig_col[0], nr1 = rig_row[1], nc1 = rig_col[1];
    if (r0 != 0xFFu && r0 < MATRIX_ROWS && c0 < MATRIX_COLS) { nr0 = r0; nc0 = c0; }
    if (r1 != 0xFFu && r1 < MATRIX_ROWS && c1 < MATRIX_COLS) { nr1 = r1; nc1 = c1; }
    if (nr0 == nr1 && nc0 == nc1) return;
    chSysLock();
    rig_row[0] = nr0; rig_col[0] = nc0; rig_row[1] = nr1; rig_col[1] = nc1;
    chSysUnlock();
}
void rowisr_rig_read(uint8_t *out28) {   /* [0..1] presses and [2..3] releases on key 0, [4..7] key 1's, [8..9] presses elsewhere; [10..13] the positions; zeroed */
    uint16_t p0, r0, p1, r1, o;
    chSysLock();
    p0 = rig_press[0]; r0 = rig_release[0]; p1 = rig_press[1]; r1 = rig_release[1]; o = rig_other_press;
    rig_press[0] = rig_press[1] = rig_release[0] = rig_release[1] = rig_other_press = 0;
    chSysUnlock();
    out28[0] = p0 & 0xFF; out28[1] = p0 >> 8; out28[2] = r0 & 0xFF; out28[3] = r0 >> 8;
    out28[4] = p1 & 0xFF; out28[5] = p1 >> 8; out28[6] = r1 & 0xFF; out28[7] = r1 >> 8;
    out28[8] = o & 0xFF;  out28[9] = o >> 8;
    out28[10] = rig_row[0]; out28[11] = rig_col[0]; out28[12] = rig_row[1]; out28[13] = rig_col[1];
}
#    define RIG_NOTE(r, p, n) rig_note((r), (p), (n))
#else
#    define RIG_NOTE(r, p, n) ((void)(r), (void)(p), (void)(n))   /* the daily build: prev is otherwise unused (-Werror) */
#endif

/* Per-row input instrumentation (health.c defines the strong ones). */
__attribute__((weak)) void input_note_row_scan(uint8_t row) { (void)row; }
__attribute__((weak)) void input_note_consume(const matrix_row_t *diff, uint8_t rows) {
    (void)diff; (void)rows;
}
/* The handler's bracket (health.c, page 4's callback metric, kept for the
 * before/after comparison; the record above is the stage-2 instrument). */
__attribute__((weak)) void sn32f2xx_isr_enter_hook(void) {}
__attribute__((weak)) void sn32f2xx_isr_exit_hook(void) {}

/* The electrical scan: the row selected, the settle, the columns read, the
 * row deselected. Its bookkeeping (scan_publish) runs after the arm in the
 * slot handler since stage 5e, off the deadline's path (codex's judgment,
 * 11-03 13:21); scan-only publishes at once, and the flash-program guard's
 * slot neither scans nor publishes (codex, 11-04). A held key's column
 * goes low here through its diode, and whichever mux
 * rail still holds charge lights its LED at that column for the read (the
 * slot handler's rotation comment, 11-04): the slot scans the row that
 * rail belongs to. */
static inline matrix_row_t scan_key_row_sample(uint8_t row) {
    const pin_t pin = key_row_pins[row];
    palClearLine(pin);   /* select: one BCLR, the row driven low (push-pull from init) */
    scan_settle();
    matrix_row_t now = columns_read();
    palSetLine(pin);     /* deselect: one BSET, the row back high */
    return now;
}
static void scan_publish(uint8_t row, matrix_row_t now) {
    input_note_row_scan(row);   /* the row's sample time: taken here, a few tens of us after the read since 5e, the same offset every slot */
    matrix_row_t prev  = shared_matrix[row];
    shared_matrix[row] = now;
    RIG_NOTE(row, prev, now);
    if (rows_seen != SHARED_MATRIX_ALL_ROWS) {
        rows_seen |= (uint8_t)(1u << row);
        if (rows_seen != SHARED_MATRIX_ALL_ROWS) return;   /* not complete yet */
    }
    matrix_scanned = true;   /* dirty flag: something new since last consume */
}
#ifdef SN32F2XX_SLEEP_HOOKS   /* scan-only's slots alone call this (the handler's own scan publishes after the arm); a released build has no scan-only */
static void scan_key_row(uint8_t row) {
    scan_publish(row, scan_key_row_sample(row));
}
#endif

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
#if ROWISR_USB_DETECT
    if (adopt_pending) {   /* the frame counter rejected this interval and bumped the generation itself: the baseline takes it, so the next
                            * interval starts fresh and is judged as any other (the frames were not staled by the rejection, so it cannot be a
                            * second verdict on the same silence; left unjudged it could hide a second one: the third verify review's 3) */
        last_snap.gen = adopt_gen;
        adopt_pending = false;
    }
#endif
    have_last = true;
    if (valid1) {
        rec.slots_elapsed += boundaries;
        rec.serviced_valid++;
        if (boundaries == 1 && s1->tc > rec.latency_max) rec.latency_max = s1->tc;
    }
}

/* ---- The handler's sections, dev only (op 28 sub 11) ------------------------
 *
 * T_SCHED's TC stamped at seven points of the full path, the six deltas
 * summed in HCLK ticks over the services that reached the exit, read and
 * reset off the handler (rowisr_record_sections). A wrap of the TC inside a
 * section (a late service across the slot's reset) is unwrapped by the
 * period. Early returns commit nothing. The warm windows of 11-02 put the
 * detector at 19 us a slot over stage 2 and the trim of its divides moved
 * nothing: these say where the time is. Opt-in (ROWISR_SECTIONS, rowisr.h),
 * off even in the dev build JD runs: the commit after stamp 6 is ~6 us a
 * slot, which page 4 sees and the stamps do not (the exit-only build's
 * windows, 11-03); the daily build carries neither the stamps nor the sums. */
#if defined(AK820_DEV_HOOKS) && ROWISR_SECTIONS
#define SEC_N 6
static uint32_t sec_sum[SEC_N];
static uint32_t sec_count;
static uint16_t sec_t[SEC_N + 1];
#define SEC_STAMP(i) (sec_t[i] = (uint16_t)CT_REG(T_SCHED, CT_TC))
static inline void sec_commit(void) {
    for (uint8_t i = 0; i < SEC_N; i++) {
        int32_t d = (int32_t)sec_t[i + 1] - (int32_t)sec_t[i];
        if (d < 0) d += (int32_t)slot_ticks;
        sec_sum[i] += (uint32_t)d;
    }
    sec_count++;
}
void rowisr_record_sections(uint8_t *out28) {   /* [0..3] the services summed, [4..27] the six sums, HCLK ticks; reset on read */
    uint32_t c, s[SEC_N];
    chSysLock();
    c = sec_count; sec_count = 0;
    for (uint8_t i = 0; i < SEC_N; i++) { s[i] = sec_sum[i]; sec_sum[i] = 0; }
    chSysUnlock();
    uint8_t *p = out28;
#define PUT32(x) do { uint32_t v_ = (x); *p++ = v_ & 0xFF; *p++ = (v_ >> 8) & 0xFF; *p++ = (v_ >> 16) & 0xFF; *p++ = (v_ >> 24) & 0xFF; } while (0)
    PUT32(c);
    for (uint8_t i = 0; i < SEC_N; i++) PUT32(s[i]);
#undef PUT32
}
#else
#define SEC_STAMP(i) ((void)0)
#define sec_commit() ((void)0)
#endif

/* ---- The hot section (D3 steps 3-5), in SRAM when ROWISR_RAMTEXT ------------
 *
 * The scan slot's electrical scan, the fifteen match registers and the mux
 * select: the handler's largest block (section 2 of the cost diagnostic,
 * 37.9 us at 267 us). On this core an instruction fetched from flash costs
 * 3-5 cycles (the diagnostic: the reads are free, the instruction count is
 * the cost), from SRAM one; .ramtext is copied at boot with .data, as
 * sleep.c's ds_deep_wfi (stage 5f's trial, codex's first-ranked lever, 11-03
 * 13:21). long_call: flash and SRAM are 512 MB apart, past a BL. With the
 * flag off the same function is inline in flash: the A/B is the flag. */
#ifndef ROWISR_RAMTEXT
#    define ROWISR_RAMTEXT 0
#endif
#if ROWISR_RAMTEXT
#    define HOT_ATTR __attribute__((section(".ramtext"), noinline, long_call)) static
#else
#    define HOT_ATTR static inline
#endif
typedef struct {
    matrix_row_t scan_now;
    uint32_t     width_sum;
    bool         scanned;
} hot_out_t;
HOT_ATTR void rowisr_hot(uint8_t mux_row, uint8_t krow, uint8_t scan_row, uint8_t colour, bool scan, hot_out_t *o) {
    /* 3. The scan slot (the interim scanner): the columns released around the
     * electrical scan; its bookkeeping after the arm (stage 5e). */
    o->scan_now = 0;
    o->scanned  = false;
    if (scan) {
        columns_release();
#ifdef AK820_DEV_HOOKS
        if (!sn32f2xx_scan_skip)   /* op 28 sub 4: the LED work alone, to attribute the scan's cost */
#endif
        {
            o->scan_now = scan_key_row_sample(scan_row);
            o->scanned  = true;
        }
        columns_active();
    }

    /* 4. The fifteen match registers for the next mux row, the widths read
     * from the frame (D9: converted at the flush), zeros for the nine holes.
     * The sum of the widths feeds D8. */
    uint32_t width_sum = 0;
#ifdef RGB_MATRIX_ENABLE
    /* Stage 5a: the colour's plane chosen once (the pins' R, B, G are the
     * frame's columns), the fifteen columns unrolled so each match
     * register's address and key are the compiler's constants: the loop was
     * the handler's largest block (section 2 of the cost diagnostic, 11-03).
     * Since D9 the width is a halfword load from the frame, converted at the
     * flush, instead of a byte load and a table load. */
    const uint8_t  *co = g_led_config.matrix_co[krow];
    const uint16_t *wp = &led_w[0][colour];   /* the plane: the pins' R, B, G are the frame's columns, stride 3 (D9: the width itself, no table load) */
    uint32_t        dither_shift = DITHER_PAT_SHIFT + (field_phase & 3u);          /* this field's bit of a dithered entry's pattern */
#if !ROWISR_RAMTEXT
#    pragma GCC unroll 15   /* in flash the unroll pays; in SRAM the loop's overhead is a cycle an instruction and the 300 B are the dev build's gap (the first 5f link overflowed the RAM by 8 B) */
#endif
    for (uint8_t c = 0; c < SN32F2XX_RGB_MATRIX_COLS; c++) {
        uint8_t  li = co[c];
        uint32_t w  = 0;
        if (li < SN32F2XX_LED_COUNT) {
            w = wp[(uint32_t)li * 3u];
            if (w > ROWISR_TERMINAL) w = (w & DITHER_BASE) + ((w >> dither_shift) & 1u);   /* a dithered entry: the base plus this field's tick; one compare on the common path */
        }
        *col_mr[c].mr = col_mr[c].key | w;
        width_sum += w;
    }
#else
    (void)colour;
    for (uint8_t c = 0; c < SN32F2XX_RGB_MATRIX_COLS; c++) *col_mr[c].mr = col_mr[c].key;
#endif
    o->width_sum = width_sum;

    /* 5. Select the mux row for it (the outputs are parked: this lights
     * nothing until the restart). A row with every width zero stays
     * deselected: no transistor base current for a dark row (the old
     * driver's enable_pwm_output; F-033's phantom was the exception). */
    if (width_sum) mux_select(mux_row);
}

/* ---- The handler (D3) ------------------------------------------------------- */

static void rowisr_slot(void) {
    sn32f2xx_isr_enter_hook();
    SEC_STAMP(0);
    /* 1. The flag and the entry snapshot, coherent: acknowledged and
     * captured in one section, so a boundary crossed between the two cannot
     * leave a flagged epoch the snapshot already belongs to (codex, P2 4). */
    snap_t s1;
    chSysLockFromISR();
    uint32_t ris = CT_REG(T_SCHED, CT_RIS);
    CT_REG(T_SCHED, CT_IC) = ris;
    take_snapI(&s1);
    chSysUnlockFromISR();
    SEC_STAMP(1);   /* section 0: the entry snapshot */
#if ROWISR_USB_DETECT
    adopt_pending = false;
#endif
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
    usb_judge_seq++;   /* a service past the spurious check: the silence report's wait (rowisr_diag.c) */
#if ROWISR_USB_DETECT
    /* D3 case 5: the frame counter's verdict on the interval just ended,
     * last_snap to s1, at every service (the design's comment above; nothing
     * when it began untrusted, usb_judge's UNTRUSTED). A rejection is
     * INVALID with one loss, as the guard's. The generation is bumped only
     * if no other observer has since the snapshot (the generation unchanged:
     * a tick's invalidation, re-primed or not, is that observer's loss,
     * counted once at the next entry by the generation check, and this
     * baseline is not adopted, since its time predates a gap the tick
     * discarded; the review's P2 4). The snapshot stays as taken and this
     * service commits invalid, no statistics; the next observation re-primes. */
    if (valid1) {
        usb_verdict_t v = usb_judge(&last_snap, &s1);
        if (v == USB_REJECT) {
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
        } else if (v == USB_UNJUDGED) {
            valid1 = false;   /* excluded: an invalid service below, no loss, nothing adopted; the baseline moves on */
        }
    }
#endif
    if (!valid1 && have_last) sat8(&rec.invalid_services);

    SEC_STAMP(2);   /* section 1: step 1's checks and the frame counter's judgment */
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
        /* The 09-01 guard, only where sn32f2xx_pgm_scan is false (nothing
         * sets it so since the dev Write test page went, 11-04): a program
         * in progress, the board dark for it
         * (the rows are deselected above and nothing is armed). */
        service_commit(&s1, valid1, boundaries);
        sn32f2xx_isr_exit_hook();
        return;
    }

    /* The rotation: the next mux row; every third one a new key row (the
     * pins' order: see cur_row's comment). The key row scanned in that
     * slot is the one BEFORE the row being prepared (11-04): the slot's
     * step 2 has just deselected the previous key row's last rail (its
     * green), whose anode line still holds its charge, and the scan pulls a
     * held key's column low through its diode, so that charge lights the
     * LED at that column on that rail for the scan's microsecond, about a
     * tick's worth once a field (JD's eye on the probe build at 1-12
     * ticks: a held key brightened the key above it, green; the top row
     * the bottom row's). Scanning the row the rail belongs to puts that
     * flash on the held key itself, under the finger. Each key row is
     * still scanned once a field, one slot-triple later than before. */
    if (++cur_row >= SN32F2XX_RGB_MATRIX_ROWS_HW) {
        cur_row = 0;
#ifdef RGB_MATRIX_ENABLE
        field_phase++;   /* the dither's clock: one count a field (D9) */
#endif
    }
    if (++cur_colour >= SN32F2XX_RGB_MATRIX_ROW_CHANNELS) cur_colour = 0;   /* 0 R, 1 B, 2 G of this key row, counted beside cur_row (the
                                                                           * `% 3` was a software divide on every slot); they wrap together */
    uint8_t colour   = cur_colour;
    bool    scan     = false;
    uint8_t scan_row = key_row;   /* before the advance: the row whose green rail step 2 just deselected (codex: no wrap expression) */
    if (colour == 0) {
        if (++key_row >= ROWS_PER_HAND) key_row = 0;
        scan = true;
    }

    /* 3 to 5: the scan, the match registers and the mux select (rowisr_hot,
     * in SRAM when ROWISR_RAMTEXT). */
    hot_out_t hot;
    rowisr_hot(cur_row, key_row, scan_row, colour, scan, &hot);
    matrix_row_t scan_now  = hot.scan_now;
    bool         scanned   = hot.scanned;
    uint32_t     width_sum = hot.width_sum;

    SEC_STAMP(3);   /* section 2: steps 2 to 5, the mux rows, the scan and the match registers */
    /* 6. Inside one critical section: a fresh snapshot against step 1's; a
     * boundary crossed meanwhile means the work ran across a slot (no arm,
     * nothing committed, the flag left for the pending interrupt, which
     * services the new epoch and reconstructs this one as missed);
     * otherwise the arm admitted against the whole terminal window, and the
     * serviced epoch committed here, at the decision. */
    bool armed = false, crossed;
    chSysLockFromISR();
    /* Stage 5a: the boundary since step 1 read off the hardware, the flag (set
     * at every match, cleared at step 1) and the counter (reset at the match,
     * so a value below step 1's is a reset under us), not the extension's
     * reconstruction (crossed_between: an observation, a difference and the
     * validity rules, ~5 us a slot by the cost diagnostic). Either read
     * catches a boundary that falls between the two. */
    uint16_t tc6 = (uint16_t)CT_REG(T_SCHED, CT_TC);
    crossed      = (CT_REG(T_SCHED, CT_RIS) & RIS_MR9) != 0 || tc6 < s1.tc;
    if (!crossed) {
        if (width_sum && tc6 <= ROWISR_ARM_DEADLINE) {
            pulse_arm_all();
            armed = true;
        }
        service_commit(&s1, valid1, boundaries);
    }
    chSysUnlockFromISR();

    if (scanned) scan_publish(scan_row, scan_now);   /* the scan's bookkeeping, off the deadline's path (5e) */

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

    /* The handler's elapsed time: the scheduler's counter at the exit against
     * step 1's, at 48 MHz, a sample kept when no boundary passed between the
     * two reads (the flag clear at the exit and the counter not reset under
     * it), preemption between them included. So the population is the
     * boundary-free services: a service that ran across a boundary, before
     * the arm (step 6 counts it crossed) or after it (armed, nothing counts
     * it), drops its sample and is counted here instead (elapsed_crossed,
     * op 28 sub 5); the mean and the maximum describe the kept population
     * only, and at 125 us the maximum is under a slot by construction. Page
     * 4's hooks time the handler inclusively, preemption and all, between
     * the port's prologue and epilogue (not the wrapper or the exception
     * return: D10's separate bound). Stage 5a: the exit's extension snapshot
     * and its verdict by the two spans are gone with the reconstruction they
     * protected (codex on 4294a3befd). */
    SEC_STAMP(4);   /* section 3: step 6, the arm */
    uint16_t tc_x = (uint16_t)CT_REG(T_SCHED, CT_TC);
    if (tc_x >= s1.tc && (CT_REG(T_SCHED, CT_RIS) & RIS_MR9) == 0) {
        uint32_t el = (uint32_t)(tc_x - s1.tc);
        rec.elapsed_sum += el;
        rec.elapsed_samples++;
        if (el > rec.elapsed_max) rec.elapsed_max = (uint16_t)el;
        if (el < rec.elapsed_min) rec.elapsed_min = (uint16_t)el;
    } else {
        rec.elapsed_crossed++;
    }
    SEC_STAMP(5);   /* section 4: the exit (the elapsed sample) */
    SEC_STAMP(6);   /* section 5: the exit judgment and the record */
    sec_commit();
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
/* Holds nest: two holders could overlap when the clock chip's transaction
 * held the line too (until stage 4, 11-03; the dev hold-off, op 28 sub 1, is
 * the one holder since), so the first hold remembers the line's state and
 * the last release puts it back. */
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
    for (uint8_t x = 0; x < SN32F2XX_RGB_MATRIX_COLS; x++) {
        if (led_col_pins[x] != col_order[x]) col_map_fault = true;   /* columns_read() assembles col_order: a changed pin table needs it rewritten */
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
/* The committed frame's channel sums (of the widths since D9), for
 * sn32f2xx_led_committed_dark() and lit_ms (elapsed time with any committed
 * channel non-zero, its present meaning); the drive itself is what was
 * emitted: width_acc[], ticks per
 * colour, converted here to the model's unit (value x ms): a LED at byte b
 * emits k.b ticks once per field at the calibrated scale k (the reference,
 * ROWISR_K_REF_Q6), so ticks x T_field / k is b x ms; under a probe's
 * scale it is b x k_probe / k x ms, the light emitted in the reference's
 * unit. The remainder is carried into the next take. */
static uint32_t drive_now[3] = {0};   /* r, g, b */
static uint32_t drive_since  = 0;     /* when the frame was committed, ms */
static uint32_t drive_lit    = 0;     /* ms with any channel lit, since the last take */
static uint64_t drive_rem[3] = {0};   /* the conversion's remainder, ticks x us */
static uint8_t  commits      = 0;     /* flushes, wrapping (sn32f2xx_led_commits()) */
#define FIELD_US (SN32F2XX_RGB_MATRIX_ROWS_HW * ROWISR_SLOT_US)   /* 4,806 us at 267; 2,250 at 125 */

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
        uint64_t num = acc[k] * FIELD_US * 64u + drive_rem[k];   /* ticks x us x 64: the scale is Q6 (D8: rgb_ms = sum of ticks x T_field / k, k the calibrated reference, so a probe's scale changes the light and the figure alike, not the unit) */
        uint64_t q   = num / (1000ULL * ROWISR_K_REF_Q6);         /* value x ms */
        drive_rem[k] = num - q * (1000ULL * ROWISR_K_REF_Q6);
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

/* D9 revised: a byte's width on the wide path. full_q8 is the frame's
 * light at full chroma in Q8 ticks (the curve at the value, scaled by the
 * cadence's k), recip its 1/255 in Q8 (one divide a frame; Q4 put a full
 * chroma at the value whose light is 1.03 ticks under a tick); a channel is
 * a multiply and a shift. Under 16 ticks the fraction is kept to a quarter
 * as a four-field pattern (a quarter: one field in four lit a tick more,
 * a half: two, three quarters: three; under a tick the base is 0), so the
 * channels' ratios hold to an eighth of a tick at the dim end; from 16
 * ticks the width is rounded. */
static const uint8_t dither_pat[4] = {0u, 0x1u, 0x5u, 0x7u};   /* quarters 0-3: the fields lit a tick more, of four */
static inline uint16_t wide_width(uint8_t chroma, uint32_t recip) {
    uint32_t q8 = ((uint32_t)chroma * recip + 128u) >> 8;   /* Q8 ticks: chroma (8 bits) x recip (under 2^20 at the 267 us cadence's k = 640) stays under 2^28 */
    uint32_t w  = q8 >> 8, frac = q8 & 0xFFu;
    if (w >= 16u) return (uint16_t)(w + (frac >= 128u ? 1u : 0u));
    uint32_t quarters = (frac + 32u) >> 6;   /* 0..4, rounded */
    if (quarters == 4u) { w++; quarters = 0u; }
    if (quarters == 0u) return (uint16_t)w;
    return (uint16_t)(DITHER_FLAG | ((uint32_t)dither_pat[quarters] << DITHER_PAT_SHIFT) | w);
}

void sn32f2xx_wide_block(bool on) {
    wide_blocked = on;
}

void sn32f2xx_flush(void) {
    commits++;   /* every render's, changed or not */
    /* The frame's encoding is the mode's (codex's design review, 11-04): a
     * wide mode's render buffer holds chroma whether or not this render
     * called the wrap (Jellybean paints nothing in a frame without a drop),
     * and its value is read here from the config under the ladder's cap,
     * not from the wrap's calls. An encoding change (the mode, a raw
     * pattern) or a value change converts the frame even when no byte
     * moved: the chroma of a dim Solid is the chroma of a bright one. */
    bool    wide = !wide_blocked && power_wide_mode();
    uint8_t v    = rgb_matrix_get_val();
    if (v > power_val_cap()) v = power_val_cap();
    if (!led_state_buf_update_required && wide == last_wide && (!wide || v == last_v)) return;
    led_state_buf_update_required = false;
    last_wide                     = wide;
    last_v                        = v;
    /* The conversion into the live frame, unmasked (D9 revised): a halfword
     * store is atomic on this core and the handler reads one channel of one
     * LED at a time, so no width tears, but a field can mix old and new
     * LEDs across a hue or brightness change: D8's coherent-frame invariant
     * is relaxed deliberately (codex, 11-04), to be seen by camera at the
     * transitions; the masked 243 B memcpy of the byte frame is gone with
     * it. Main-loop time a changed frame, from the ELF's instruction counts: the
     * wide loop about 0.3-0.7 ms of CPU, 0.6-1.5 ms beside the row ISR, the byte
     * loop about a quarter of that. Rainfall and Drift pay it every frame. */
    uint32_t sr = 0, sg = 0, sb = 0;
    if (wide) {
        uint32_t full_q8 = ((uint32_t)curve_q6[v] * ROWISR_K_Q6) >> 4;   /* Q6 x Q6 >> 4: Q8 ticks at full chroma, 1,195.3 at the reference's top (16,320 x 640 >> 4 at the 267 us cadence: under 2^20) */
        uint32_t recip   = (full_q8 * 256u + 127u) / 255u;                    /* Q8 of a chroma unit's ticks in Q8 (full_q8 x 256 under 2^28: 652,800 x 256 at k = 640) */
        for (int i = 0; i < SN32F2XX_LED_COUNT; i++) {
            uint16_t wr = wide_width(led_state_buf[i].r, recip);
            uint16_t wb = wide_width(led_state_buf[i].b, recip);
            uint16_t wg = wide_width(led_state_buf[i].g, recip);
            led_w[i][0] = wr;
            led_w[i][1] = wb;
            led_w[i][2] = wg;
            sr += wr; sb += wb; sg += wg;
        }
    } else {
        for (int i = 0; i < SN32F2XX_LED_COUNT; i++) {
            uint16_t wr = width_lut[led_state_buf[i].r];
            uint16_t wb = width_lut[led_state_buf[i].b];
            uint16_t wg = width_lut[led_state_buf[i].g];
            led_w[i][0] = wr;
            led_w[i][1] = wb;
            led_w[i][2] = wg;
            sr += wr; sb += wb; sg += wg;
        }
    }
    drive_close(timer_read32());
    drive_now[0] = sr;   /* the widths' sums (a dithered entry counts as lit): committed_dark() and lit_ms */
    drive_now[1] = sg;
    drive_now[2] = sb;
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
#endif

/* ---- The consume ------------------------------------------------------------- */

bool matrix_can_read(void) {
    return matrix_scanned;
}

/* CUSTOM_MATRIX = lite: quantum/matrix_common.c's matrix_scan() calls this
 * with raw_matrix and runs debounce on what it returns. */
bool matrix_scan_custom(matrix_row_t current_matrix[]) {
    if (!matrix_scanned) return false; /* Nothing to process until we have the matrix scanned */

    /* The XOR of the very snapshot copied, taken under the lock with the copy
     * (stage 3: the consume count made exact; before it the instrumentation
     * compared unlocked and could miss a scan that landed between its read
     * and the copy, raw_edges' undercount of one or two in a few hundred,
     * BACKLOG.md), and counted off the lock. The ISR writes shared_matrix on
     * every key-row sample, so the compare and copy exclude it or the loop
     * can consume a matrix that is part new and part old (the hazard the
     * flush's masked copy locked against until D9; the widths' halfword
     * stores need no lock). */
    matrix_row_t diff[MATRIX_ROWS];
    bool         changed = false;
    chSysLock();
    for (uint8_t i = 0; i < MATRIX_ROWS; i++) {
        diff[i] = (matrix_row_t)(current_matrix[i] ^ shared_matrix[i]);
        if (diff[i]) {
            current_matrix[i] = shared_matrix[i];
            changed           = true;
        }
    }
    matrix_scanned = false;
    chSysUnlock();
    input_note_consume(diff, MATRIX_ROWS);

    return changed;
}
