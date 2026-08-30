// Copyright 2026 Fernando Birra
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Bare-metal dashboard: renders directly through graphics/lcd_bus.c (no Quantum
// Painter). QP's asset blobs (qgf images, qff fonts) are decoded by lcd_bus.

#include "graphics/display.h"
#include "lcd_bus.h"

#include <string.h>
#include <stdio.h>
#include "quantum.h"
#include "gpio.h"
#include "rtc/rtc.h"



#define PANEL_BKL       A16

#define PANEL_WIDTH     128
#define PANEL_HEIGHT    128

#define COL_BG          0x0000   // black background
#define COL_FG          0xFFFF   // white text

// Font blobs (Iosevka, mono 1bpp).
// Fonts and images now live in external flash and are DMA-drawn; these are
// ids into the index that flash_assets_init() reads at boot. The full 95-glyph
// atlases are used, so EMBED_CHARSET and its "letters silently draw nothing"
// trap are gone.
#define FONT_CLOCK      ASSET_IOSEVKA_REGULAR_30   // big clock
#define FONT_STATUS     ASSET_IOSEVKA_MEDIUM_20    // small status text

// Bottom row: y position of the wireless status line.
#define STATUS_Y 106

static bool display_powered = true;
static bool display_paused  = false;   // true while the flash-animation player owns the bus
static bool splash_cleared = false;
static bool mac_mode = false;

uint32_t display_redraw_dashboard(uint32_t trigger_time, void *cb_arg);
void display_set_paused(bool paused) {
    display_paused = paused;
    if (!paused) display_redraw_dashboard(0, NULL);   // resume: full repaint
}

enum {
    CONN_MODE_WIRED = 0,
    CONN_MODE_BLUETOOTH,
    CONN_MODE_2_4G
};
static uint8_t connection_mode = CONN_MODE_WIRED;

bool display_get_power(void) {
    return display_powered;
}

// Backlight is event-driven: written here whenever the power state changes.
/* --- Backlight brightness -------------------------------------------------
 *
 * PANEL_BKL (A16) is a plain GPIO. Hardware PWM on it is IMPOSSIBLE, not merely
 * unused: the SN32F299 datasheet gives P0.16 the single alternate function
 * CT16B5_CAP0, a capture INPUT. So brightness is software PWM.
 *
 * The tick is CT16B3 (GPTD4) at a MEASURED 20000 Hz -- a dedicated timer, set up
 * in pwm_tick_init() in ak820pro.c. It used to run from a weak hook in the RGB
 * RGB row-scan ISR), which welded the switching rate to RGB_MATRIX_SPD_STEP:
 * retuning the LED field rate for the rainbow artifact silently retuned the
 * backlight into or out of flicker. THAT COUPLING IS GONE -- SPD_STEP no longer
 * affects this at all.
 *
 * Period is BKL_PWM_TICKS: 20000/48 = 417 Hz switching, floor 1/48 = 2.1%.
 * A dimmer floor needs a LONGER period, which lowers the switching rate; the
 * tick rate is now independent, so that trade is purely period-vs-floor.
 *
 * The levels are spaced perceptually, not linearly. Brightness perception is
 * roughly a power law, so an even duty spread wastes most of its steps at the
 * top where they are indistinguishable and gives nothing usable at the bottom,
 * which is the end that matters in a dark room. */
#define BKL_PWM_TICKS 48   /* 20000/48 = 417 Hz, floor 1/48 = 2.1%. Sized when
                            * the tick was losing 23% of its ticks to a priority
                            * inversion (an effective 15385 Hz -> 320 Hz). The
                            * tick is now a steady 20000, so there is headroom to
                            * lengthen this for a dimmer floor if wanted. */
static const uint8_t bkl_duty[] = { 0, 1, 2, 3, 5, 8, 12, 18, 27, 48 };
#define BKL_MAX_LEVEL ((uint8_t)(sizeof(bkl_duty) / sizeof(bkl_duty[0])) - 1)

#ifndef DISPLAY_BRIGHTNESS_DEFAULT
#    define DISPLAY_BRIGHTNESS_DEFAULT BKL_MAX_LEVEL
#endif

static volatile uint8_t bkl_level = DISPLAY_BRIGHTNESS_DEFAULT;
static volatile uint8_t bkl_phase = 0;

/* ISR context, called from pwm_tick_cb() in ak820pro.c -- the CT16B3/GPTD4 20 kHz
 * tick, which ak820pro.c owns because the indicator LEDs share it. Full and zero
 * duty are handled without toggling so a static backlight is not switched
 * needlessly. */
void display_backlight_tick(void) {
    uint8_t duty = display_powered ? bkl_duty[bkl_level] : 0;

    if (duty >= BKL_PWM_TICKS) {
        gpio_write_pin(PANEL_BKL, 1);
        return;
    }
    if (duty == 0) {
        gpio_write_pin(PANEL_BKL, 0);
        return;
    }

    uint8_t phase = bkl_phase + 1;
    if (phase >= BKL_PWM_TICKS) phase = 0;
    bkl_phase = phase;

    gpio_write_pin(PANEL_BKL, phase < duty);
}

uint8_t display_get_brightness_max(void) {
    return BKL_MAX_LEVEL;
}

uint8_t display_get_brightness(void) {
    return bkl_level;
}

void display_set_brightness(uint8_t level) {
    if (level > BKL_MAX_LEVEL) level = BKL_MAX_LEVEL;
    bkl_level = level;
    /* Deliberately not persisted. Every write to the kb eeconfig block is an
     * internal-flash program/erase, which is exactly what wedges this board
     * (see rgb_matrix_eeprom_flush_allowed in ak820pro.c). Set
     * DISPLAY_BRIGHTNESS_DEFAULT in config.h once a level is settled on. */
}

void display_brightness_up(void) {
    if (bkl_level < BKL_MAX_LEVEL) display_set_brightness(bkl_level + 1);
}

void display_brightness_down(void) {
    if (bkl_level > 0) display_set_brightness(bkl_level - 1);
}

void display_set_power(bool on) {
    display_powered = on;
    /* The ISR hook drives the pin from here on; write it once so power-off takes
     * effect immediately rather than at the next tick. */
    gpio_write_pin(PANEL_BKL, on && (bkl_duty[bkl_level] > 0));
}

void display_toggle_power(void) {
    display_set_power(!display_powered);
}

static bool display_backlight_init(void) {
    gpio_set_pin_output(PANEL_BKL);
    gpio_write_pin(PANEL_BKL, display_powered); // initial state (on)
    return true;
}

// y position of the big clock (top of the glyphs).
#define CLOCK_Y 49

// Clock format: 1 = HH:MM:SS (per-second redraw of the changed cells), 0 = HH:MM.
#ifndef DISPLAY_CLOCK_SHOW_SECONDS
#    define DISPLAY_CLOCK_SHOW_SECONDS TRUE
#endif

// Forces a full clock+date repaint after the background is cleared.
static bool clock_force_repaint = true;

static void draw_status(bool force); // CH582F status: battery + channel digit
static void draw_conn_row(void);      // three-transport strip, top row

void draw_clock(void) {
    rtc_time_t shown;
    bool valid = rtc_get_time(&shown);
    if (!valid) memset(&shown, 0, sizeof(shown));

    // Time HH:MM:SS -- redraw only the character cells that changed (usually just the
    // seconds). The clock font is monospace, so every cell has the same width.
    static char last_time[12] = {0};
    if (clock_force_repaint) memset(last_time, 0, sizeof(last_time)); // invalidate -> full repaint
    char time_str[12];
#if DISPLAY_CLOCK_SHOW_SECONDS
    snprintf(time_str, sizeof(time_str), "%02u:%02u:%02u",
             (unsigned)shown.hours, (unsigned)shown.minutes, (unsigned)shown.seconds);
#else
    snprintf(time_str, sizeof(time_str), "%02u:%02u",
             (unsigned)shown.hours, (unsigned)shown.minutes);
#endif
    if (strcmp(time_str, last_time) != 0) {
        uint8_t  n       = (uint8_t)strlen(time_str);        // 8 (HH:MM:SS) or 5 (HH:MM)
        uint16_t total_w = lcd_flash_text_width(FONT_CLOCK, time_str);
        int16_t  x0      = (PANEL_WIDTH - total_w) / 2;
        int16_t  cw      = total_w / n;                      // monospace cell width
        for (uint8_t i = 0; i < n; i++) {
            if (time_str[i] != last_time[i]) {
                int16_t cx = x0 + i * cw;
                char ch[2] = {time_str[i], 0};
                lcd_draw_flash_text(FONT_CLOCK, cx, CLOCK_Y, ch);
            }
        }
        strcpy(last_time, time_str);
    }
    clock_force_repaint = false; // consumed by the time above
}

uint32_t display_redraw_dashboard(uint32_t trigger_time, void *cb_arg) {
    splash_cleared = true;

    // Clear background.
    lcd_clear_rect(0, 0, PANEL_WIDTH, PANEL_HEIGHT);

    // Full repaint: force the clock and date to redraw over the cleared screen.
    clock_force_repaint = true;

    // Mac/Windows icon (top-left).
    lcd_draw_flash_image(mac_mode ? ASSET_APPLE_ICON_24X24 : ASSET_WINDOWS_ICON_24X24, 0, 0);

    // Connection icon.
    draw_conn_row();

    draw_clock();
    draw_status(true);   // battery + channel digit over the cleared screen

    return 0; // one-shot
}

bool display_init_kb(void) {
    lcd_init();          // GC9107 bring-up, rotation 270

    // Splash logo, held until the deferred dashboard repaint below.
    lcd_clear_rect(0, 0, PANEL_WIDTH, PANEL_HEIGHT);
    // All art lives in external flash now, so nothing can be drawn until the
    // index is read. An unprovisioned keyboard therefore shows a BLANK panel and
    // says so on the console -- there is no embedded fallback left to draw with,
    // which is the whole point (it reclaimed ~32KB of firmware). Provision with:
    //   ak820ctl flash write 0x0CE0000 graphics/res/flash_assets.bin
    if (flash_assets_init()) {
        dprintf("[assets] index ok, %u entries\n", flash_assets_count());
        lcd_draw_flash_image(ASSET_SONIXQMK, 0, 0);
    } else {
        dprintf("[assets] NO VALID INDEX at 0x%06lX -- panel stays blank.\n"
                "[assets] provision with: ak820ctl flash write 0x%06lX flash_assets.bin\n",
                (unsigned long)FLASH_ASSET_BASE, (unsigned long)FLASH_ASSET_BASE);
    }

    display_backlight_init();

    bool res = display_init_user();
    if (res)
        defer_exec(1500, display_redraw_dashboard, NULL);

    return true;
}

__attribute__((weak)) bool display_init_user(void) {
    return true;
}

__attribute__((weak)) bool display_housekeeping_task_user(void) {
    return true;
}

#include "bluetooth/ch582f_ajazz.h"   // connection state enum + getters
extern uint8_t ch582_get_battery(void);

// General blink phase: true during the first half of each period_ms cycle, so any
// element can gate its visibility on it. period_ms == 0 means always on (solid).
bool display_blink(uint16_t period_ms) {
    if (!period_ms) return true;
    return (timer_read32() % period_ms) < (period_ms / 2u);
}

// Connection-digit blink rates.
#define CONN_BLINK_PAIRING_MS 200   // pairing: fast
#define CONN_BLINK_LINKING_MS 700   // linking/reconnecting: slow
/* Failed: a lazy pulse that reads as DORMANT rather than busy. The slot is still
 * the one you selected, so blanking the digit threw away real information -- but
 * solid already means "connected", so it cannot be reused here. Confirmed on
 * hardware: a connect to an unreachable slot gives `5B 33 33 36`, and the
 * following `5B 23` (idle) is ignored by the parser, so REJECTED persists
 * indefinitely and the digit stayed gone for good. */
#define CONN_BLINK_FAILED_MS 2000   // failed: very slow, "stopped" not "trying"

/* Connection strip: all three transports shown at once, ordered LEFT-TO-RIGHT TO
 * MATCH THE PHYSICAL SWITCH (2.4G / wired / BT as seen from the top of the
 * board), with the inactive two dimmed. You read it the way you read the switch
 * under your fingers instead of decoding a single glyph.
 *
 * It is ONE 76x24 asset per state, not three 24x24 icons composited at runtime,
 * because flash assets are RGB565 with colour baked in -- lcd_draw_flash_image()
 * has no tint parameter, so "dim" has to be authored, not computed. Baking whole
 * rows also means one blit and no alignment arithmetic. Generated by the snippet
 * in assets/ from the original three icons.
 *
 * This replaced the date, which was redundant next to a computer clock. */
#define CONN_ICON_X   29   /* 5px clear of the OS logo: it shows a different kind of thing */
#define CONN_ICON_W   79   /* 3x24 icons + gaps of 3 and 4 (BT nudged 1px right for optical balance) */
#define CONN_ICON_H   24   /* was implicit in CONN_ICON_W while the icon was square */
#define CONN_NUM_X    (CONN_ICON_X + CONN_ICON_W + 1)
#define CONN_NUM_W    12

// Paints the whole three-transport strip for the current mode.
static void draw_conn_row(void) {
    uint16_t id = (connection_mode == CONN_MODE_BLUETOOTH) ? ASSET_CONN_ROW_BT
                : (connection_mode == CONN_MODE_2_4G)      ? ASSET_CONN_ROW_24G
                                                           : ASSET_CONN_ROW_CABLE;
    lcd_draw_flash_image(id, CONN_ICON_X, 0);
}

// Channel digit next to the connection icon, blinking to show link state:
//   connected -> solid digit;  linking/reconnecting -> slow blink;
//   pairing -> fast blink;  failed -> very slow pulse;  idle / USB -> no digit.
// Driven every housekeeping tick (~10 Hz) so the blink animates; the redraw is
// self-guarded so it only touches the panel on a visible transition.
static void draw_conn_number(bool force) {
    uint8_t slot  = ch582_get_target_slot();               // 1-3, or 0
    char    digit = (slot >= 1 && slot <= 3) ? (char)('0' + slot) : 0;

    char     c      = 0;   // the digit this state wants to show (0 = none)
    uint16_t period = 0;   // blink period; 0 = solid
    switch (ch582_get_conn_state()) {
        case CH582_CONN_CONNECTED: c = digit; period = 0;                     break;
        case CH582_CONN_PAIRING:   c = digit; period = CONN_BLINK_PAIRING_MS; break;
        case CH582_CONN_LINKING:   c = digit; period = CONN_BLINK_LINKING_MS; break;
        case CH582_CONN_REJECTED:  c = digit; period = CONN_BLINK_FAILED_MS;  break;
        default:                   c = 0;                                     break;
    }
    char shown = (c && display_blink(period)) ? c : 0;     // current blink-phase visibility

    static char last_shown = -1; // force the first paint
    if (!force && shown == last_shown) return;
    last_shown = shown;

    lcd_clear_rect(CONN_NUM_X, 0, CONN_NUM_W + 1, CONN_ICON_H);
    if (shown) {
        char s[2] = {shown, 0};
        lcd_draw_flash_text(FONT_STATUS, CONN_NUM_X, 2, s);
    }
}

// Battery icon, bottom-left. Body + terminal nub, with a proportional fill.
// Geometry is inclusive coordinates (lcd_fill_rect takes x0,y0,x1,y1).
#define BATT_X0    5   /* was 2: the LCD is recessed, so the bezel clips the outermost pixels */
#define BATT_Y0    (STATUS_Y + 5)
#define BATT_W     24
#define BATT_H     12
#define BATT_X1    (BATT_X0 + BATT_W - 1)
#define BATT_Y1    (BATT_Y0 + BATT_H - 1)
#define BATT_NUB_W 2
#define BATT_NUB_INSET 4          // nub is shorter than the body, top and bottom
// Inner (fillable) area, one pixel inside the 1px outline.
#define BATT_IN_X0 (BATT_X0 + 2)
#define BATT_IN_Y0 (BATT_Y0 + 2)
#define BATT_IN_X1 (BATT_X1 - 2)
#define BATT_IN_Y1 (BATT_Y1 - 2)
#define BATT_IN_W  (BATT_IN_X1 - BATT_IN_X0 + 1)

#define COL_BATT_OK    0x07E0   // green
#define COL_BATT_WARN  0xFFE0   // amber
#define COL_BATT_LOW   0xF800   // red
#define COL_BOLT       0xFFE0   // yellow, matching the padlock

extern bool charge_is_charging(void);   // ak820pro.c

/* 9x14 charging bolt, sat just right of the battery so the two read as one
 * unit. Drawn from horizontal runs of a rasterised polygon -- a hand-placed
 * zigzag of rectangles came out looking like the digit "4"; the diagonals only
 * read as a bolt once they step a pixel at a time.
 *
 * Colour already signals charging (the icon goes cyan), but that is a subtle cue
 * if you do not know the code, and the gap between icon and percentage was dead
 * space anyway. */
#define BOLT_X  (BATT_X0 + BATT_W + BATT_NUB_W + 4)
#define BOLT_Y  (BATT_Y0 - 1)

static void draw_bolt(uint16_t x, uint16_t y, uint16_t col) {
    lcd_fill_rect(x + 6, y +  0, x + 6, y +  0, col);
    lcd_fill_rect(x + 5, y +  1, x + 6, y +  2, col);
    lcd_fill_rect(x + 4, y +  3, x + 5, y +  4, col);
    lcd_fill_rect(x + 3, y +  5, x + 5, y +  5, col);
    lcd_fill_rect(x + 3, y +  6, x + 8, y +  6, col);
    lcd_fill_rect(x + 2, y +  7, x + 7, y +  7, col);
    lcd_fill_rect(x + 4, y +  8, x + 7, y +  8, col);
    lcd_fill_rect(x + 4, y +  9, x + 6, y +  9, col);
    lcd_fill_rect(x + 4, y + 10, x + 5, y + 10, col);
    lcd_fill_rect(x + 3, y + 11, x + 4, y + 12, col);
    lcd_fill_rect(x + 3, y + 13, x + 3, y + 13, col);
}

// Bottom row: battery icon (left) + percentage (right-aligned).
static void draw_battery(bool force) {
    static uint8_t last_batt = 0xFE;
    static bool    last_chrg = false;

    uint8_t batt = ch582_get_battery();
    bool    chrg = charge_is_charging();

    // Charging state changes the icon colour, so it has to retrigger a redraw
    // as well -- the level alone is not enough.
    if (!force && batt == last_batt && chrg == last_chrg) return;
    last_batt = batt;
    last_chrg = chrg;

    // Clear the bottom strip.
    lcd_clear_rect(0, STATUS_Y, PANEL_WIDTH, PANEL_HEIGHT - STATUS_Y);

    /* Outline stays white and the fill stays level-coloured even while
     * charging: the bolt carries that signal, so the two are orthogonal. The
     * earlier version recoloured the whole icon cyan, which hid the level
     * exactly when "15% and charging" is the most useful thing to know. */
    uint16_t outline = COL_FG;

    // Body outline: four 1px edges rather than a filled rect, so the interior
    // stays background and the fill below can be drawn independently.
    lcd_fill_rect(BATT_X0, BATT_Y0, BATT_X1, BATT_Y0, outline);   // top
    lcd_fill_rect(BATT_X0, BATT_Y1, BATT_X1, BATT_Y1, outline);   // bottom
    lcd_fill_rect(BATT_X0, BATT_Y0, BATT_X0, BATT_Y1, outline);   // left
    lcd_fill_rect(BATT_X1, BATT_Y0, BATT_X1, BATT_Y1, outline);   // right
    // Terminal nub on the right.
    lcd_fill_rect(BATT_X1 + 1, BATT_Y0 + BATT_NUB_INSET,
                  BATT_X1 + BATT_NUB_W, BATT_Y1 - BATT_NUB_INSET, outline);

    if (chrg) draw_bolt(BOLT_X, BOLT_Y, COL_BOLT);

    if (batt <= 100) {
        // Proportional fill. Round up so any nonzero charge shows at least one
        // column -- an empty-looking icon at 3% would read as "dead".
        uint16_t fw = (uint16_t)(((uint32_t)batt * BATT_IN_W + 99u) / 100u);
        if (fw > BATT_IN_W) fw = BATT_IN_W;

        if (fw > 0) {
            uint16_t col = (batt <= 20) ? COL_BATT_LOW
                         : (batt <= 50) ? COL_BATT_WARN
                                        : COL_BATT_OK;
            lcd_fill_rect(BATT_IN_X0, BATT_IN_Y0, BATT_IN_X0 + fw - 1, BATT_IN_Y1, col);
        }

        char bbuf[8];
        snprintf(bbuf, sizeof(bbuf), "%u%%", batt);
        uint16_t w = lcd_flash_text_width(FONT_STATUS, bbuf);
        /* PANEL_WIDTH - 4, not - 1: viewed from the right the bezel hides the
         * last couple of columns, which was clipping the '%'. */
        lcd_draw_flash_text(FONT_STATUS, PANEL_WIDTH - 4 - w, STATUS_Y, bbuf);
    }
}

// --- Lock indicator band ---------------------------------------------------
//
// The clock (Regular-30, 34 tall at CLOCK_Y 49) ends at y82 and the battery row
// starts at STATUS_Y 106, leaving exactly 23px -- which is the Medium-20 cell
// height, so one row of status text fits precisely.
//
// A single padlock marks "something is locked"; fixed-position labels say which.
// Labels never reflow, so their positions are learnable. The band is left black
// whenever nothing is locked -- this board lives on a desk in a dark room, and a
// permanently-lit row of words would defeat the point.
//
// Labels are white because lcd_draw_flash_text() has no colour parameter: the
// atlases are RGB565 with colour baked in, and a glyph blit paints its whole
// cell including the black background (no transparency). Colour is therefore
// only available from lcd_fill_rect, which is what draws the padlock.
//
// Labelled "WIN" even though the underlying flag is keymap_config.no_gui (which
// technically disables Cmd on a Mac). Deliberate: GUI-lock exists so the Windows
// key cannot yank you out of a fullscreen game. On macOS, locking Cmd would
// disable most hotkeys and nobody would enable it on purpose -- so the label is
// for the only context where the feature is meaningful. The padlock conveys
// "disabled"; the label says which key.
#define LOCK_Y      83
#define LOCK_PAD_X  4
#define LOCK_PAD_Y  (LOCK_Y + 3)          // 16px glyph centred in the 23px band
#define LOCK_CAP_X  20                    // "CAPS" 4 glyphs = 40px -> 20..59
#define LOCK_WIN_X  64                    // "WIN"  3 glyphs = 30px -> 64..93
#define LOCK_SLOT3_X 96                   // shared: "FN" 20px, "SCR" 30px -> 96..125
#define COL_PADLOCK 0xFFE0                // yellow

extern bool lock_state_caps(void);        // ak820pro.c
extern bool lock_state_gui(void);
extern bool lock_state_fn(void);
extern bool lock_state_scroll(void);

// 13x16 padlock. The first attempt was 11x14 with a 5px-wide flat-topped
// shackle over an 11px body, which read as a blob -- too narrow relative to the
// body and visibly square on top. This widens the shackle to 9px and steps the
// crown in two rows (5 wide, then 7) so it reads as an arch at this size; a
// single-pixel step is all the rounding 16px affords.
static void draw_padlock(uint16_t x, uint16_t y, uint16_t col) {
    lcd_fill_rect(x + 4, y + 0, x + 8,  y + 0,  col);   // crown, narrowest
    lcd_fill_rect(x + 3, y + 1, x + 9,  y + 1,  col);   // crown, stepping out
    lcd_fill_rect(x + 2, y + 2, x + 3,  y + 7,  col);   // shackle left post
    lcd_fill_rect(x + 9, y + 2, x + 10, y + 7,  col);   // shackle right post
    lcd_fill_rect(x + 0, y + 7, x + 12, y + 15, col);   // body
}

static void draw_locks(bool force) {
    bool caps = lock_state_caps();
    bool gui  = lock_state_gui();
    bool fn   = lock_state_fn();
    bool scr  = lock_state_scroll();

    uint8_t state = (caps ? 1u : 0u) | (gui ? 2u : 0u)
                  | (fn ? 4u : 0u)   | (scr ? 8u : 0u);

    static uint8_t last_state = 0xFFu;
    if (!force && state == last_state) return;
    last_state = state;

    lcd_clear_rect(0, LOCK_Y, PANEL_WIDTH, STATUS_Y - LOCK_Y);
    if (state == 0) return;               // nothing locked -> leave the band dark

    /* The padlock means "locked", so it tracks the lock states only. A held Fn
     * layer is not a lock -- it lights its slot without one. That stays correct
     * if Fn is ever made a toggle (TG/TT) rather than momentary. */
    if (caps || gui || scr) draw_padlock(LOCK_PAD_X, LOCK_PAD_Y, COL_PADLOCK);
    if (caps) lcd_draw_flash_text(FONT_STATUS, LOCK_CAP_X, LOCK_Y, "CAPS");
    if (gui)  lcd_draw_flash_text(FONT_STATUS, LOCK_WIN_X, LOCK_Y, "WIN");

    /* Third slot is shared. Scroll Lock wins when actually set -- this board has
     * no Scroll Lock key and macOS effectively never sets it, so in practice the
     * slot is Fn, but the state is not thrown away on the rare occasion some
     * other host or app does set it. */
    if (scr)      lcd_draw_flash_text(FONT_STATUS, LOCK_SLOT3_X, LOCK_Y, "SCR");
    else if (fn)  lcd_draw_flash_text(FONT_STATUS, LOCK_SLOT3_X, LOCK_Y, "FN");
}


// --- Host text slot --------------------------------------------------------
//
// One line pushed from the host over raw HID, drawn in the 24px band between
// the top row and the clock (y25..48). The firmware assigns no meaning to the
// text -- a script decides what it says and how to abbreviate it.
//
// Deliberately NOT scrolling in v1. A marquee would redraw at ~10 Hz and keep
// the flash->LCD DMA busy far more than the current roughly-once-a-second
// cadence, and DMA contention is exactly what the eeconfig-write freeze was
// about (see rgb_matrix_eeprom_flush_allowed in ak820pro.c). Truncating to what
// fits is also just less distracting to sit next to.
//
// The whole payload fits one raw-HID packet: 12 characters at a 10px advance
// fills the 128px band, against ~27 usable bytes per packet. So there is no
// framing, no offsets and no partial-render window to design around.
#define TEXT_Y        25
#define TEXT_H        (CLOCK_Y - TEXT_Y)     // 24px, clock starts at 49
#define TEXT_ICON_W   12
#define TEXT_ICON_GAP 4
#define TEXT_X        (TEXT_ICON_W + TEXT_ICON_GAP)

// Blank after this long with no update. Without it the panel would happily show
// last night's track forever if the host agent dies, the machine sleeps, or the
// cable is pulled -- and stale data presented confidently is worse than none.
#ifndef DISPLAY_TEXT_TIMEOUT_MS
#    define DISPLAY_TEXT_TIMEOUT_MS 180000u   /* 3 minutes */
#endif

#define COL_ICON_PLAY  0x07E0                 // green
#define COL_ICON_PAUSE 0xFFE0                 // amber
#define COL_ICON_STOP  0xF800                 // red

static char     text_buf[DISPLAY_TEXT_MAX + 1] = {0};
static uint8_t  text_icon    = DISPLAY_ICON_NONE;
static uint32_t text_stamp   = 0;
static bool     text_present = false;
static bool     text_dirty   = false;

void display_set_text(uint8_t icon, const char *s, uint8_t len) {
    if (len > DISPLAY_TEXT_MAX) len = DISPLAY_TEXT_MAX;

    uint8_t n = 0;
    for (uint8_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '\0') break;
        // The atlases carry printable ASCII only; anything else would index off
        // the end of the glyph table. Substitute rather than drop so the text
        // keeps its shape.
        text_buf[n++] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    text_buf[n] = '\0';

    text_icon    = (icon <= DISPLAY_ICON_STOP) ? icon : DISPLAY_ICON_NONE;
    text_stamp   = timer_read32();
    text_present = (n > 0) || (text_icon != DISPLAY_ICON_NONE);
    text_dirty   = true;
}

/* Optimistic local flip when the user presses play/pause.
 *
 * The host poll is the authority, but it only runs every few seconds, and the
 * one moment the delay is annoying is when YOU pressed the key and are looking
 * at the screen. So guess immediately; the next host push overwrites it with
 * the truth, and a wrong guess self-corrects within one poll interval. That is
 * the crucial difference from tracking playback locally: this always converges.
 *
 * Deliberately does NOT touch text_stamp. The flip is a guess, not evidence the
 * host agent is still alive, so it must not extend the staleness window -- an
 * agent that died should still let the slot expire on schedule. */
void display_toggle_play_icon(void) {
    if (text_icon == DISPLAY_ICON_PLAY) {
        text_icon = DISPLAY_ICON_PAUSE;
    } else if (text_icon == DISPLAY_ICON_PAUSE) {
        text_icon = DISPLAY_ICON_PLAY;
    } else {
        return;                 /* nothing playing -> nothing to guess about */
    }
    text_dirty = true;
}

void display_clear_text(void) {
    text_buf[0]  = '\0';
    text_icon    = DISPLAY_ICON_NONE;
    text_present = false;
    text_dirty   = true;
}

// 12x12 transport glyphs from rectangles -- colour is only available here, not
// from the font atlases (their colour is baked in and a glyph blit paints its
// whole cell). Play is a stepped triangle, the same trick as the padlock crown.
static void draw_text_icon(uint16_t x, uint16_t y, uint8_t icon) {
    switch (icon) {
        case DISPLAY_ICON_PLAY:
            for (uint16_t i = 0; i < 6; i++)
                lcd_fill_rect(x + i * 2, y + i, x + i * 2 + 1, y + 11 - i, COL_ICON_PLAY);
            break;
        case DISPLAY_ICON_PAUSE:
            lcd_fill_rect(x + 1, y, x + 4,  y + 11, COL_ICON_PAUSE);
            lcd_fill_rect(x + 7, y, x + 10, y + 11, COL_ICON_PAUSE);
            break;
        case DISPLAY_ICON_STOP:
            lcd_fill_rect(x + 1, y, x + 10, y + 11, COL_ICON_STOP);
            break;
        default:
            break;
    }
}

/* --- Wireless status overlay ----------------------------------------------
 *
 * The icon strip says WHICH link and the channel digit says which slot, but
 * neither says what the link is DOING -- a blinking digit is only legible if you
 * already know that ~200 ms means pairing and ~700 ms means connecting. This
 * puts that in words.
 *
 * It borrows the host text band rather than adding a row: there is no spare
 * vertical space (top strip 0..24, this band 25..48, clock 49..82, locks 83..105,
 * battery 106..). Link events are transient and actionable while a track title
 * is ambient, so the overlay OUTRANKS host text while active and hands the band
 * straight back afterwards -- a title is displaced for seconds, never lost.
 *
 * CONNECTED is deliberately a brief confirmation, not a steady readout: the
 * solid digit already says "connected" indefinitely, so holding the words there
 * would permanently cost the music slot to say nothing new.
 *
 * ELEVEN characters, not DISPLAY_TEXT_MAX (12). Text is drawn from TEXT_X (16)
 * to leave the icon gutter, so 12 glyphs at a 10px advance would end at x=136 on
 * a 128px panel -- and the bezel clips the outermost columns besides. */
#define CONN_STATUS_HOLD_MS 3000u

/* The overlay draws no icon, so unlike the host slot it need not start at
 * TEXT_X (16). Drawing from 4 gives TWELVE glyphs (4 + 12*10 = 124) inside the
 * 128px panel while keeping the same 4px margin the battery row uses -- the LCD
 * is recessed and the bezel clips the outermost columns. */
#define CONN_STATUS_X   4
#define CONN_STATUS_MAX 12

/* Full period of the symptom/remedy alternation: half of it on each message.
 * Slow enough to finish reading, fast enough not to look frozen. */
#define CONN_STATUS_ALT_MS 3000

/* <0 = not holding; 0-100 = how far through BT_PAIR_HOLD_MS the hold is. */
static int16_t     pair_hint_pct = -1;
#define PAIR_BAR_CELLS 6
static char        conn_status_buf[CONN_STATUS_MAX + 1] = {0};

void display_set_pair_hint(int16_t pct) {
    pair_hint_pct = pct;
}

#ifdef PARAM_OVERLAY
static char     param_status_buf[CONN_STATUS_MAX + 1] = {0};
static uint32_t param_status_since                    = 0;
static bool     param_status_on                       = false;

void display_set_param_status(const char *s) {
    if (!s) {
        if (param_status_on) { param_status_on = false; text_dirty = true; }
        return;
    }
    strncpy(param_status_buf, s, CONN_STATUS_MAX);
    param_status_buf[CONN_STATUS_MAX] = '\0';
    param_status_since = timer_read32();
    param_status_on    = true;
    text_dirty       = true;
}
#endif
static const char *conn_status_str   = NULL;   /* NULL = host text owns the band */
static uint32_t    conn_status_since = 0;

static void conn_status_update(void) {
    /* In wired mode the CH582F is bypassed entirely, so its state is stale --
     * report nothing rather than whatever the last wireless session left. */
    uint8_t st = (connection_mode == CONN_MODE_WIRED)
                     ? (uint8_t)CH582_CONN_IDLE
                     : (uint8_t)ch582_get_conn_state();
    uint8_t slot = ch582_get_target_slot();

    static uint8_t last_st   = 0xFF;  /* 0xFF: force evaluation on the first tick */
    static uint8_t last_slot = 0xFF;

    /* `changed` means "conn_status_buf may now hold different text". Both the
     * state and the slot feed it: the state selects WHICH string is formatted
     * and the slot is embedded IN it. Tracking only the slot was a latent bug --
     * switching REJECTED -> PAIRING on the same slot rewrites the buffer while
     * its POINTER is unchanged, so a pointer compare alone would have left the
     * previous message on the panel. */
    bool changed = false;
    if (st != last_st) {
        last_st           = st;
        conn_status_since = timer_read32();
        changed           = true;
    }
    if (slot != last_slot) {
        last_slot = slot;
        changed   = true;
    }

    const char *want = NULL;

#ifdef PARAM_OVERLAY
    /* Sits below the pair hint but above the link state: adjusting a setting is
     * an active gesture and wants immediate feedback, while a link state is
     * passive and will still be there in two seconds. */
    if (param_status_on) {
        if (timer_elapsed32(param_status_since) >= PARAM_OVERLAY_HOLD_MS) {
            param_status_on = false;
            text_dirty    = true;      /* expired: fall through and repaint */
        } else if (pair_hint_pct < 0) {
            if (conn_status_str != param_status_buf) {
                conn_status_str = param_status_buf;
                text_dirty      = true;
            }
            return;
        }
    }
#endif

    /* Outranks every link state: during the hold the link state is whatever the
     * press kicked off (LINKING, or REJECTED if the slot is unreachable), and
     * showing "Link failed" while the user is mid-gesture is both wrong and
     * discouraging. The hold is what matters until it resolves. */
    if (pair_hint_pct >= 0) {
        /* A filling bar, not a static label: at 2 s a motionless message reads as
         * "nothing is happening" and invites letting go early, which is the exact
         * frustration a longer hold would otherwise cost. Progress makes the wait
         * legible, which is what makes 2 s strictly better than 1 s rather than
         * merely safer. */
        uint8_t filled = (uint8_t)((pair_hint_pct * PAIR_BAR_CELLS) / 100);
        if (filled > PAIR_BAR_CELLS) filled = PAIR_BAR_CELLS;
        char bar[CONN_STATUS_MAX + 1];
        int  n = 0;
        bar[n++] = 'P'; bar[n++] = 'a'; bar[n++] = 'i'; bar[n++] = 'r';
        bar[n++] = ':'; bar[n++] = ' ';
        for (uint8_t i = 0; i < PAIR_BAR_CELLS; i++)
            bar[n++] = (i < filled) ? '=' : ' ';
        bar[n] = '\0';

        if (conn_status_str != conn_status_buf || strcmp(conn_status_buf, bar) != 0) {
            strcpy(conn_status_buf, bar);
            conn_status_str = conn_status_buf;
            text_dirty      = true;
        }
        return;
    }

    switch (st) {
        case CH582_CONN_PAIRING:
            if (connection_mode == CONN_MODE_2_4G) {
                want = "Pairing 2.4G";     /* the dongle pairs; no advertised name */
            } else {
                /* Show the EXACT advertised name, slot digit included, because
                 * the whole point is telling you what to look for in the phone's
                 * list. The module appends the slot to a stored prefix, giving
                 * "AK820 5.1-1". The "5.1" is Bluetooth-version marketing and
                 * means nothing -- but it IS what the host displays, so putting
                 * a tidier name here would be a lie.
                 *
                 * To actually rename it you would have to send 0xA9 (which this
                 * port never does) with framing documented only as "AK820 5.1-$",
                 * writing to module storage THAT CANNOT BE READ BACK. Verify the
                 * framing on the wire before attempting it. */
                if (display_blink(CONN_STATUS_ALT_MS)) {
                    /* The name alone is cryptic; the verb makes the two halves
                     * read as one sentence: "Pair with:" -> "AK820 5.1-2". */
                    want = "Pair with:";
                } else {
                    if (slot >= 1 && slot <= 3)
                        snprintf(conn_status_buf, sizeof(conn_status_buf), "AK820 5.1-%u", slot);
                    else
                        snprintf(conn_status_buf, sizeof(conn_status_buf), "AK820 5.1");
                    want = conn_status_buf;
                }
            }
            break;
        case CH582_CONN_LINKING:   want = "Connecting"; break;
        /* 0x36 is NOT in CH582F_PROTOCOL.md's 5B state table -- the "host
         * refused" reading is disassembly guesswork, never seen on the wire. So
         * describe the OBSERVABLE (no working link) and claim nothing about the
         * cause: "Refused" or "Not paired" would both assert more than is known.
         * This state also draws no digit, so without a word here the panel says
         * nothing at all. */
        case CH582_CONN_REJECTED:
            /* Alternate the symptom with the REMEDY. A failed link is the one
             * state where the panel can say something actionable, and we know
             * WHICH slot failed -- so name the single key rather than listing
             * Fn+Q/W/E and making the reader pick (which is 13 chars anyway,
             * one over budget).
             *
             * Deliberately NOT done for a long-lived LINKING. That state is
             * ambiguous: it is equally a dropped `5B 32` on a link that is
             * actually UP, and telling someone to hold the slot key there would
             * tear down a working connection to fix a display bug.
             *
             * The advice survives even if the 0x36 reading is wrong: re-pairing
             * the slot is a sane remedy for any stuck wireless state, which is
             * more than can be said for the label. */
            if (display_blink(CONN_STATUS_ALT_MS)) {
                want = "Link failed";
            } else {
                /* Stock keymap: Fn+Q/W/E select BT slots 1-3, Fn+R the dongle. */
                char key = (connection_mode == CONN_MODE_2_4G) ? 'R'
                         : (slot == 1) ? 'Q' : (slot == 2) ? 'W' : (slot == 3) ? 'E' : 0;
                if (key) {
                    /* Name the duration: the hold is BT_PAIR_HOLD_MS (2 s) and
                     * now fires under the finger, so "2s" is a promise the
                     * firmware actually keeps. 12 chars exactly. */
                    snprintf(conn_status_buf, sizeof(conn_status_buf), "Hold Fn+%c 2s", key);
                } else {
                    snprintf(conn_status_buf, sizeof(conn_status_buf), "Fn+Q/W/E");
                }
                want = conn_status_buf;
            }
            break;
        case CH582_CONN_CONNECTED:
            if (timer_elapsed32(conn_status_since) < CONN_STATUS_HOLD_MS)
                want = "Connected";
            break;
        default: break;
    }

    /* Pointer compare covers the literals; `changed` covers the formatted
     * buffer, whose pointer is stable while its contents are not. */
    if (want != conn_status_str || (changed && want == conn_status_buf)) {
        conn_status_str = want;
        text_dirty      = true;   /* overlay appeared, changed, or was released */
    }
}

static void draw_text_slot(bool force) {
    // Expire before drawing, so a stale slot blanks itself without the host
    // having to send anything.
    if (text_present && timer_elapsed32(text_stamp) > DISPLAY_TEXT_TIMEOUT_MS) {
        text_present = false;
        text_dirty   = true;
    }
    if (!force && !text_dirty) return;
    text_dirty = false;

    lcd_clear_rect(0, TEXT_Y, PANEL_WIDTH, TEXT_H);

    /* Wireless status transiently outranks the host text slot. No icon: the
     * icon IDs are media transports and would misdescribe a link event. */
    if (conn_status_str) {
        lcd_draw_flash_text(FONT_STATUS, CONN_STATUS_X, TEXT_Y, conn_status_str);
        return;
    }

    if (!text_present) return;

    if (text_icon != DISPLAY_ICON_NONE)
        draw_text_icon(0, TEXT_Y + (TEXT_H - 12) / 2, text_icon);
    if (text_buf[0])
        lcd_draw_flash_text(FONT_STATUS, TEXT_X, TEXT_Y, text_buf);
}

static void draw_status(bool force) {
    draw_conn_number(force);
    draw_battery(force);
    draw_locks(force);
    draw_text_slot(force);
}

void display_housekeeping_task(void) {
    if (!display_housekeeping_task_user())
        return;

    if (display_paused) return;   // animation owns the bus

    if (splash_cleared) {
        // Connection digit every tick (~10 Hz) so its blink animates; self-guarded.
        draw_conn_number(false);

        // Wireless status overlay: recompute before the slot is drawn so an
        // appearing/releasing overlay repaints on this same tick.
        conn_status_update();

        // Host text slot: self-guards, and expires itself on the same tick.
        draw_text_slot(false);

        // Lock band: self-guards, only redraws when a lock state actually
        // changes. Needs the ~10 Hz tick rather than the 1 Hz clock path so a
        // Caps press shows up immediately.
        draw_locks(false);

        // Clock + battery only need refreshing once per RTC second.
        static uint32_t last_shown_sec = UINT32_MAX;
        uint32_t sec = rtc_get_seconds();
        if (sec != last_shown_sec) {
            last_shown_sec = sec;
            draw_clock();
            draw_battery(false); // self-guards, only draws on change
        }
    }
}

void display_draw_mac_logo(void) {
    mac_mode = true;
    if (splash_cleared && !display_paused)
        lcd_draw_flash_image(ASSET_APPLE_ICON_24X24, 0, 0);
}

void display_draw_windows_logo(void) {
    mac_mode = false;
    if (splash_cleared && !display_paused)
        lcd_draw_flash_image(ASSET_WINDOWS_ICON_24X24, 0, 0);
}

void display_draw_usb_logo(void) {
    connection_mode = CONN_MODE_WIRED;
    if (splash_cleared && !display_paused)
        draw_conn_row();
}

void display_draw_bluetooth_logo(void) {
    connection_mode = CONN_MODE_BLUETOOTH;
    if (splash_cleared && !display_paused)
        draw_conn_row();
}

void display_draw_2_4_g_logo(void) {
    connection_mode = CONN_MODE_2_4G;
    if (splash_cleared && !display_paused)
        draw_conn_row();
}
