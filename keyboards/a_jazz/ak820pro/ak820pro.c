// Copyright 2026 Fernando Birra
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ak820pro.h"

#include "gpio.h"
#include "connection.h"

#include "graphics/display.h"
#include "graphics/lcd_bus.h"
#include "bluetooth/ch582f_ajazz.h"
#include "rtc/rtc.h"
#include "raw_hid.h"
#include "watchdog.h"
#include "health.h"
#ifdef USB_WAKEUP_ON_KEYPRESS
#    include "usb_main.h"   /* USB_DRIVER */
#endif

// Wireless-mode/slot UI lives in bt_ui.c; the persisted kb datablock in
// kb_eeconfig.c (phase-3 module split -- code moved verbatim).
#include "bt_ui.h"
#include "kb_eeconfig.h"
#include "consumer_mod.h"
#include "param_overlay.h"
#include "indicators.h"

void early_hardware_init_post(void) {
    // Configure SPI0 pins for the LCD panel. SEL0 is left UNMUXED: our bare-metal bus
    // (graphics/lcd_bus.c) drives CS (B8) as a plain GPIO and must hold it low across
    // a whole DMA frame, so B8 must be free of the SPI SEL function.
    SN_PFPA->SPI_b.MISO0 = 0b11;
    SN_PFPA->SPI_b.MOSI0 = 0b11;
    SN_PFPA->SPI_b.SCK0  = 0b11;

    // Configure UART2 pins for the CH582F wireless module
    SN_PFPA->UART_b.UTXD2 = 0b11;
    SN_PFPA->UART_b.URXD2 = 0b11;
}


 void keyboard_post_init_kb(void) {
#ifdef FORCE_EEPROM_RESET
    /* ONE-SHOT RECOVERY BUILD -- not for normal use.
     * An interrupted `flash write` erased the whole application region,
     * including the wear-levelling EEPROM store. A corrupt store makes QMK
     * rewrite it continually; every write stalls the main loop while the flash
     * array is busy, which presents as stuck and repeating keys. This wipes it
     * back to a known state. Flash the normal build immediately after. */
    eeconfig_init();
#endif

    // Windows Lock and Charging LEDs: outputs, off initially. update_leds() then
    // tracks their real state, writing only on a change.
    pwm_tick_init();

    gpio_set_pin_output(LED_WINLOCK_PIN);
    gpio_write_pin(LED_WINLOCK_PIN, false);
    gpio_set_pin_output(LED_CHARGING_PIN);
    gpio_write_pin(LED_CHARGING_PIN, false);

    // Set up GPIO pins for the charging status inputs
    gpio_set_pin_input_high(CHARGE_CHRG_PIN);   // input with pull-up
    gpio_set_pin_input_high(CHARGE_STDBY_PIN);  // input with pull-up

    // Restore persisted config, then re-select the saved BT slot if the
    // slider booted in the BT position (dip_switch_init ran before EEPROM
    // was read). Details in kb_eeconfig.c / bt_ui.c.
    kb_eeconfig_init();
    bt_ui_resume_saved_profile();

    // Bring up the clock: I2C, the SN32 1 Hz counter, and a first PCF8563 seed.
    rtc_init();

    // Initialize the display subsystem (painter, fonts, images, etc.) and draw the splash screen.
    display_init_kb();

    // Reset-cause bookkeeping (consecutive-WDT-reset counter, degraded-mode
    // decision) -- before the watchdog arms, after the console is plumbed.
    watchdog_boot_check();

    // Chain the user hook: overriding keyboard_post_init_kb() replaces QMK's
    // default, which is what normally calls keyboard_post_init_user().
    keyboard_post_init_user();

    // Arm the watchdog LAST: boot blocks deliberately (lcd_init's 240 ms of
    // wait_ms, the asset index read, the RTC seed), and none of that should
    // count against the timeout. See watchdog.c for the 12 s rationale.
    watchdog_start();
 }

 bool dip_switch_update_kb(uint8_t index, bool active) {
    // Let the keymap handle layer logic (Mac/Win, no_gui) first.
    if(!dip_switch_update_user(index, active)) {
        return false;
    }

    if (index == 0) {  // Mac/Windows switch -- icon only (layer set by keymap)
        if (active) display_draw_windows_logo();
        else        display_draw_mac_logo();
    } else if (index == 1 || index == 2) {
        bt_ui_mode_slider(index, active);   // tri-state slider; see bt_ui.c
    }
    return true;
 }

#ifdef USB_WAKEUP_ON_KEYPRESS
/* Ask a sleeping host to wake. QMK's own version is compiled out here -- see the
 * NO_USB_STARTUP_CHECK note in config.h -- and we cannot restore it, because it
 * wakes the host from inside a blocking `while (USB_SUSPENDED)` loop that would
 * stall the main loop and drop the wireless link.
 *
 * Gated on the host having ENABLED remote wakeup (bit 1 of the USB status): a
 * host that does not want to be woken by this device never sets it, so this is
 * self-limiting rather than something to police ourselves.
 *
 * Rate-limited because usbWakeupHost() sleeps SN32_USB_HOST_WAKEUP_DURATION (2 ms)
 * driving the K-state. Costs nothing in normal use -- it returns immediately
 * unless USB is actually suspended. */
#define USB_STATUS_REMOTE_WAKEUP 2U   /* USB_GETSTATUS_REMOTE_WAKEUP_ENABLED; that
                                       * define is private to chibios.c */
#define USB_WAKEUP_RETRY_MS      500

static void usb_wakeup_try(void) {
    if (USB_DRIVER.state != USB_SUSPENDED) return;
    if (!(USB_DRIVER.status & USB_STATUS_REMOTE_WAKEUP)) return;

    static uint16_t last_try = 0;
    static bool     tried    = false;
    if (tried && timer_elapsed(last_try) < USB_WAKEUP_RETRY_MS) return;
    last_try = timer_read();
    tried    = true;
    usbWakeupHost(&USB_DRIVER);
}
#endif

/* ---- Worst-case main-loop gap (Stage 2 instrument) ------------------------
 *
 * Measures the LONGEST interval between consecutive housekeeping passes, not
 * the average scan rate. A single 100 ms stall is invisible in a per-second
 * average -- 400 Hz drops to 360 and looks like ordinary jitter -- which is
 * exactly why the console blocking bug went unnoticed.
 *
 * Reported on the LCD, deliberately NOT over the console: the console path is
 * the thing under test, and printing to it would perturb the measurement.
 * Fires only when a NEW maximum is set, so it costs nothing while quiet.
 *
 * Remove with LOOPGAP_INSTRUMENT once the console work is validated. */
#ifdef LOOPGAP_INSTRUMENT

/* WHICH long operation was running when the loop stalled.
 *
 * The probe measured a 59 ms in-use stall -- easily enough to swallow a
 * keystroke, since a character arrives roughly every 125 ms at typing speed.
 * Magnitude alone does not say what to fix, and the three suspects need
 * completely different remedies, so each marks itself on the way in. */
volatile uint8_t loop_stall_mark = 0;   /* see LOOP_MARK_* in ak820pro.h */

static const char *loop_mark_name(uint8_t m) {
    switch (m) {
        case LOOP_MARK_FLASH: return "flash";   /* internal-flash program/erase */
        case LOOP_MARK_BLIT:  return "blit";    /* flash->LCD DMA wait */
        case LOOP_MARK_I2C:   return "i2c";     /* bit-banged PCF8563 */
        default:              return "?";
    }
}

static void loop_gap_task(void) {
    static uint32_t last = 0, report_at = 0;
    static uint16_t n_gaps = 0;
    static uint32_t worst = 0;
    static uint8_t  worst_mark = 0;

    if (timer_read32() < LOOPGAP_SETTLE_MS) {   /* boot blocks deliberately */
        last = timer_read32();
        return;
    }
    if (last) {
        uint32_t gap = timer_elapsed32(last);
        if (gap >= 4) {                          /* main loop is ~2.5 ms */
            n_gaps++;
            if (gap > worst) { worst = gap; worst_mark = loop_stall_mark; }
        }
    }
    last = timer_read32();
    loop_stall_mark = LOOP_MARK_NONE;

    /* Report at most once a second, and only when something happened.
     *
     * A MAX-HOLD WAS THE WRONG INSTRUMENT for the observed fault: typing goes
     * janky for a stretch, then recovers. Once a max latches, every later stall
     * is invisible, so a burst of thirty 12 ms stalls looks identical to
     * silence. The COUNT is what distinguishes a periodic storm from a one-off,
     * and the count is what the symptom actually is. */
    /* Report on the CONSOLE, never the panel.
     *
     * The previous version called display_set_param_status(), which draws ~12
     * glyphs, each a blocking DMA blit -- i.e. it reported by doing the exact
     * thing it was measuring. That fed back and ran away to a 154 ms stall and
     * an unusable board. An instrument must not touch the subsystem under test.
     *
     * Console writes are one line per second and the endpoint's sticky
     * timed_out flag bounds the worst case; that path was measured earlier and
     * exonerated. Also prints uptime, because the fault is WORST RIGHT AFTER
     * BOOT and settles over minutes -- so when it happens matters as much as
     * what. */
    if (n_gaps && timer_elapsed32(report_at) >= 1000) {
        report_at = timer_read32();
        dprintf("[stall] t=%lus %ux worst=%lums %s\n",
                (unsigned long)(timer_read32() / 1000), (unsigned)n_gaps,
                (unsigned long)worst, loop_mark_name(worst_mark));
        n_gaps = 0; worst = 0; worst_mark = LOOP_MARK_NONE;
    }
}
#endif

#ifdef CONSOLE_ENABLE
/* Key-event counter, to localise dropped keystrokes.
 *
 * Measured 2026-08-30: ~2.3% of characters lost on this board, wired, all pure
 * drops with no transpositions. That could be the matrix/debounce failing to
 * SEE the press, or the USB path failing to DELIVER it. Those need completely
 * different fixes, and counting is the only way to tell them apart:
 *
 *   firmware count == characters received  -> the matrix missed it
 *   firmware count >  characters received  -> the report was lost downstream
 *
 * Counted in process_record_kb, which is after debounce and after the matrix
 * has resolved the event, so it measures what the firmware BELIEVES it sent.
 *
 * Printed at most once a second and only when the count moves, i.e. one USB
 * write per second of typing rather than one per keystroke -- deliberately, so
 * the instrument cannot cause the drops it is measuring. */
static uint16_t key_press_count = 0;

static void key_stat_task(void) {
    static uint16_t last = 0;
    static uint32_t last_at = 0;
    if (key_press_count == last) return;
    if (timer_elapsed32(last_at) < 1000) return;
    last_at = timer_read32();
    dprintf("[keys] presses=%u\n", (unsigned)key_press_count);
    last = key_press_count;
}
#endif
bool process_record_kb(uint16_t keycode, keyrecord_t *record) {
#ifdef CONSOLE_ENABLE
    if (record->event.pressed) key_press_count++;
#endif
    rgb_repeat_process_record(keycode, record);   // arm/disarm hold-to-repeat

    if (!process_modified_consumer(keycode, record)) return false;

#ifdef USB_WAKEUP_ON_KEYPRESS
    if (record->event.pressed) usb_wakeup_try();   // typing should wake a sleeping host
#endif
    // Let the keymap handle/override keycodes first (e.g. Mac media keys).
    if (!process_record_user(keycode, record)) {
        return false;
    }

    switch (keycode) {
        case SCR_TOG:
            if (record->event.pressed) {
                display_toggle_power();
#ifdef PARAM_OVERLAY
                /* Only the "On" case is ever legible -- turning the panel off
                 * takes the message with it. Pushed anyway so the action is
                 * consistent with every other adjustment, and so switching back
                 * on confirms itself rather than just appearing. */
                if (display_get_power()) display_set_param_status("LCD On");
#endif
            }
            return false;
        case ANIM_TOG:
            if (record->event.pressed) anim_toggle();
            return false;
        case SCR_UP:
        case SCR_DN:
            if (record->event.pressed) {
                if (keycode == SCR_UP) display_brightness_up();
                else                   display_brightness_down();
#ifdef PARAM_OVERLAY
                /* Percentage of the LEVEL INDEX, not of the duty cycle. The 10
                 * levels are perceptually spaced, so duty runs 2/4/6/10/17/25/
                 * 38/56/100% and reads as erratic; the index divides evenly and
                 * answers the question actually being asked -- how far up the
                 * range am I. Buffer is oversized because the compiler cannot
                 * prove %u is short (-Werror=format-truncation); the slot
                 * truncates to the band width anyway. */
                char buf[24];
                uint8_t lvl = display_get_brightness();
                uint8_t mx  = display_get_brightness_max();
                snprintf(buf, sizeof(buf), "LCD    %3u%%",
                         (unsigned)(mx ? (lvl * 100u + mx / 2u) / mx : 0u));
                display_set_param_status(buf);
#endif
            }
            return false;
        case QK_BOOT:
            /* Draw the bootloader notice BEFORE handing off to QMK, which then
             * resets into the ROM bootloader.
             *
             * Bootloader mode is otherwise indistinguishable from a dead board
             * -- no RGB, dark LCD, no typing, no indicator of any kind -- which
             * has cost real diagnostic time. The GC9107 keeps its own GRAM
             * across the MCU reset, so the picture survives; whether the
             * BACKLIGHT does is the open question (PANEL_BKL goes high-Z on
             * reset). If it goes dark this is simply a no-op, which is why it
             * is worth trying in this form first.
             *
             * Returns TRUE so QMK still performs the reset -- we are only
             * decorating it. This covers Fn+Esc; holding ESC while plugging in
             * is handled by the ROM before any of our code runs, and cannot be
             * decorated. */
            /* Draw the notice, then let QMK do its normal magic-flag reset.
             *
             * ⚠️ DO NOT "improve" this by jumping straight to the ROM at
             * SN32_BOOTLOADER_ADDRESS. That DOES keep the splash lit -- GPIOs
             * retain state without a reset, so the backlight survives -- but it
             * hands the ROM a live machine (48 MHz clocks, SPI/PWM/UART/timers
             * configured, USB half torn down) and flashing then becomes
             * unreliable: sonixflasher stalls partway through the erase and
             * only a REPLUG clears the ROM's ISP session. Tried 2026-08-31 and
             * reverted. Entry via a clean power-on reset worked every time;
             * entry via the direct jump failed every time.
             *
             * The splash is therefore only visible for the moment before the
             * reset. A barely-visible message that flashes reliably beats a
             * readable one that does not. */
            if (record->event.pressed) {
                display_bootloader_splash();
                /* Hold it long enough to READ before QMK resets into the ROM.
                 * The reset drops PANEL_BKL to high-Z so the panel goes dark
                 * the instant it fires; without this pause the notice is
                 * present for a few milliseconds and effectively invisible.
                 * Costs 1.5 s on the way into the bootloader, which is nothing
                 * against a flash cycle. */
                wait_ms(1500);
            }
            return true;
        case KC_MEDIA_PLAY_PAUSE:
            /* Guess the new state so the icon flips instantly instead of after
             * the host's next poll. Returns TRUE: the keypress must still reach
             * the host, we are only decorating it. Works wherever the key is
             * bound, since process_record_kb sees the resolved keycode. */
            if (record->event.pressed) {
                display_toggle_play_icon();
                display_playback_key();   // freeze/resume the timer with it
            }
            return true;
#ifdef RGB_MATRIX_ENABLE
        // VIA-assignable RGB-matrix controls (see ak820pro.h). One step per press.
        case RGBM_TOG:  if (record->event.pressed) rgb_matrix_toggle();         return false;
        case RGBM_MOD:  if (record->event.pressed) rgb_matrix_step();           return false;
        case RGBM_RMOD: if (record->event.pressed) rgb_matrix_step_reverse();   return false;
        case RGBM_HUI:  if (record->event.pressed) rgb_matrix_increase_hue();   return false;
        case RGBM_HUD:  if (record->event.pressed) rgb_matrix_decrease_hue();   return false;
        case RGBM_SAI:  if (record->event.pressed) rgb_matrix_increase_sat();   return false;
        case RGBM_SAD:  if (record->event.pressed) rgb_matrix_decrease_sat();   return false;
        case RGBM_VAI:  if (record->event.pressed) rgb_matrix_increase_val();   return false;
        case RGBM_VAD:  if (record->event.pressed) rgb_matrix_decrease_val();   return false;
        case RGBM_SPI:  if (record->event.pressed) rgb_matrix_increase_speed(); return false;
        case RGBM_SPD:  if (record->event.pressed) rgb_matrix_decrease_speed(); return false;
#endif
        // BT slot / 2.4G / pair keys: tap-select + hold-to-pair, in bt_ui.c.
        case BT1:      // Fn+Q -> tap: select BT1 | hold: pair BT1
        case BT2:      // Fn+W -> tap: select BT2 | hold: pair BT2
        case BT3:      // Fn+E -> tap: select BT3 | hold: pair BT3
            return bt_ui_slot_key(keycode, record);
        case BT24G:    // Fn+R -> select 2.4G (2.4G mode only)
            return bt_ui_24g_key(record);
        case BT_PAIR:  // Fn+P (long press) -> pair, BT/2.4G modes only
            return bt_ui_pair_key(record);
        default:
            return true;
    }
}

// Apply a 7-byte time payload to the RTC:
//   [0]=year-2000 [1]=month [2]=day [3]=weekday [4]=hour [5]=min [6]=sec
// Sets both the PCF8563 (persist) and the live SN32 clock; the display picks it up
// within a second via rtc_get_time(). Returns the PCF (persistence) write status.
static bool rtc_apply_bytes(const uint8_t *p) {
    /* Validate BEFORE touching hardware: these bytes go into a BATTERY-BACKED
     * part, so garbage persists across power cycles -- and dec2bcd(sec >= 80)
     * would even set the PCF8563's VL (voltage-low) flag via bit 7 of the
     * seconds register. Any local HID-capable process can send this packet;
     * "the host script is well-behaved" is not a guard. (Audit finding
     * IV-1, hardening-plan/findings-input-validation.md.) */
    if (p[0] > 99 ||               /* year -- the PCF stores year %100 but
                                    * reads reconstruct 2000+yy, so 100..255
                                    * would set one year live and persist
                                    * another (Codex phase-2 review #1) */
        p[1] < 1 || p[1] > 12 ||   /* month   */
        p[2] < 1 || p[2] > 31 ||   /* day     */
        p[3] > 6 ||                /* weekday */
        p[4] > 23 ||               /* hours   */
        p[5] > 59 ||               /* minutes */
        p[6] > 59) {               /* seconds */
        return false;
    }
    rtc_time_t t = {
        .year    = (uint16_t)(2000 + p[0]),
        .month   = p[1],
        .day     = p[2],
        .weekday = p[3],
        .hours   = p[4],
        .minutes = p[5],
        .seconds = p[6],
    };
    return rtc_set_time(&t);
}

// Clock-set command framing, identical for VIA and non-VIA builds so the host
// set-clock utility speaks ONE protocol. It's VIA's custom-value layout:
//   [SET_VALUE, RTC_CHANNEL, RTC_SET_TIME, year-2000, month, day, weekday,
//    hour, min, sec]
// The reply echoes the packet: data[0] stays SET_VALUE when handled, or becomes
// UNHANDLED (0xFF) when rejected. SET_VALUE/UNHANDLED mirror VIA's
// id_custom_set_value / id_unhandled so the same bytes work against either build.
enum {
    RTC_SET_VALUE = 0x07, // == VIA id_custom_set_value
    RTC_UNHANDLED = 0xFF, // == VIA id_unhandled
    RTC_CHANNEL   = 0x10,
    RTC_SET_TIME  = 0x01,
    /* Read the live clock back. Exists so post-set phase error is MEASURABLE:
     * without it the only way to see the offset is to film the panel next to a
     * screen showing `date` and step frames. Reply reuses the request buffer:
     *   [SET_VALUE, RTC_CHANNEL, RTC_GET_TIME, ok, yy, mm, dd, wday, hh, mm, ss]
     * `ok` is 1 when rtc_get_time() succeeded.
     *
     * Whole seconds only, deliberately -- the sub-second phase is recovered by
     * POLLING this fast and watching for the increment, which needs no extra
     * protocol and no sub-second field on the wire. */
    RTC_GET_TIME  = 0x02,
};

// ---------------------------------------------------------------------------
// Flash provisioning channel (Stage D). Same VIA custom-value framing as the
// clock above, on its own channel, so one host tool speaks one protocol:
//   [SET_VALUE, FLASH_CHANNEL, cmd, payload...]
// Replies are written back into the same buffer (VIA echoes it); data[3] is a
// status byte, with any returned data from data[4].
//
// Nothing here ever blocks on the chip. A page program is ~1-3 ms and a sector
// erase 50-300 ms; waiting for either inside the HID callback would stall the
// matrix scan (measured: a blocking erase costs ~6% of one scan window). So a
// command that needs an idle chip returns FS_BUSY and the host re-sends.
enum {
    FLASH_CHANNEL   = 0x11,
    // commands
    FC_INFO         = 0x01,  // -> jedec[3], asset_base[3]
    FC_ERASE        = 0x02,  // addr[3]            (4K sector)
    FC_WRITE_BEGIN  = 0x03,  // addr[3]
    FC_WRITE_DATA   = 0x04,  // len[1], bytes...
    FC_WRITE_END    = 0x05,  // flush a partial page
    FC_CRC32        = 0x06,  // addr[3], len[3]  -> crc[4]
    FC_STATUS       = 0x07,  // -> busy[1]
    FC_UNLOCK       = 0x08,  // on[1]  (animation slots)
    FC_CRC_NEXT     = 0x09,  // continue a running CRC -> crc[4] when done
    // status codes returned in data[3]
    FS_OK           = 0x00,
    FS_BUSY         = 0x01,  // chip busy -- resend this packet
    FS_REFUSED      = 0x02,  // write floor / locked / animation owns the bus
    FS_BADARG       = 0x03,
    FS_MORE         = 0x04,  // CRC still running -- send FC_CRC_NEXT
};

// CRC is computed in slices. Reading a whole range inside one HID callback
// blocks the matrix scan for the entire read -- a 184 KB verify measured a drop
// from ~1396 Hz to ~300 Hz. Everything else in this channel is non-blocking, so
// the CRC must be too: each call folds at most CRC_SLICE bytes (~0.3 ms of SPI)
// and returns FS_MORE until the range is consumed.
#define CRC_SLICE 1024u
static uint32_t crc_addr, crc_left, crc_acc;

// Streaming write state. Bytes accumulate here until a 256-byte page boundary,
// because the chip WRAPS rather than continuing when a program crosses one.
static uint32_t fw_addr  = 0;      // flash address of pg[0]
static uint16_t fw_fill  = 0;      // bytes buffered
static uint8_t  fw_pg[256];
static bool     fw_open  = false;

static uint8_t flash_flush_page(void) {
    if (!fw_fill) return FS_OK;
    if (flash_busy()) return FS_BUSY;
    if (!flash_page_program(fw_addr, fw_pg, fw_fill)) return FS_REFUSED;
    fw_addr += fw_fill;
    fw_fill  = 0;
    return FS_OK;
}

static void flash_command(uint8_t *data, uint8_t length) {
    uint8_t  cmd = data[2];
    uint8_t *p   = &data[3];
    uint32_t a   = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];

    switch (cmd) {
        case FC_INFO: {
            uint32_t id = flash_jedec_id();
            data[3] = FS_OK;
            data[4] = (uint8_t)(id >> 16); data[5] = (uint8_t)(id >> 8); data[6] = (uint8_t)id;
            data[7] = (uint8_t)(FLASH_ASSET_BASE >> 16);
            data[8] = (uint8_t)(FLASH_ASSET_BASE >> 8);
            data[9] = (uint8_t)(FLASH_ASSET_BASE);
            return;
        }
        case FC_STATUS:
            data[3] = FS_OK;
            data[4] = flash_busy() ? 1 : 0;
            return;

        case FC_UNLOCK:
            flash_set_unlocked(p[0] != 0);
            data[3] = FS_OK;
            return;

        case FC_ERASE:
            if (flash_busy())               { data[3] = FS_BUSY;    return; }
            data[3] = flash_erase_sector(a) ? FS_OK : FS_REFUSED;
            return;

        case FC_WRITE_BEGIN:
            // A page-aligned start keeps every later flush inside one page.
            if (a & 0xFFu)                  { data[3] = FS_BADARG;  return; }
            if (!flash_writable(a, 1))      { data[3] = FS_REFUSED; return; }
            fw_addr = a; fw_fill = 0; fw_open = true;
            data[3] = FS_OK;
            return;

        case FC_WRITE_DATA: {
            if (!fw_open)                   { data[3] = FS_BADARG;  return; }
            uint8_t n = p[0];
            if (n == 0 || n > length - 4)   { data[3] = FS_BADARG;  return; }
            // Buffer, flushing whenever a full page is ready. On FS_BUSY nothing
            // is consumed, so the host simply re-sends the identical packet.
            for (uint8_t i = 0; i < n; i++) {
                fw_pg[fw_fill++] = p[1 + i];
                if (fw_fill == sizeof fw_pg) {
                    uint8_t st = flash_flush_page();
                    if (st != FS_OK) { fw_fill -= (uint16_t)(i + 1); data[3] = st; return; }
                }
            }
            data[3] = FS_OK;
            return;
        }

        case FC_WRITE_END:
            data[3] = flash_flush_page();
            if (data[3] == FS_OK) fw_open = false;
            return;

        case FC_CRC32: {
            uint32_t len = ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 8) | p[5];
            if (!len)                       { data[3] = FS_BADARG;  return; }
            crc_addr = a; crc_left = len; crc_acc = 0xFFFFFFFFu;
        }
        /* fall through: fold the first slice immediately */
        case FC_CRC_NEXT: {
            if (!crc_left)                  { data[3] = FS_BADARG;  return; }
            uint32_t n = crc_left < CRC_SLICE ? crc_left : CRC_SLICE;
            crc_acc   = flash_crc32_acc(crc_acc, crc_addr, n);
            crc_addr += n;
            crc_left -= n;
            if (crc_left) { data[3] = FS_MORE; return; }
            uint32_t c = ~crc_acc;
            data[3] = FS_OK;
            data[4] = (uint8_t)(c >> 24); data[5] = (uint8_t)(c >> 16);
            data[6] = (uint8_t)(c >> 8);  data[7] = (uint8_t)c;
            return;
        }
        default:
            data[0] = RTC_UNHANDLED;
            return;
    }
}

static inline bool is_flash_cmd(const uint8_t *data, uint8_t length) {
    return length >= 4 && data[0] == RTC_SET_VALUE && data[1] == FLASH_CHANNEL;
}

/* --- Host text channel ----------------------------------------------------
 *   [SET_VALUE, TEXT_CHANNEL, TEXT_SET,   icon, bytes...]   up to 12 bytes
 *   [SET_VALUE, TEXT_CHANNEL, TEXT_CLEAR]
 *
 * One packet carries the whole payload: the band fits 12 glyphs and a raw-HID
 * report has ~27 usable bytes, so there is no framing to design. The firmware
 * assigns no meaning to the text -- a host script decides what it says.
 * Platform-agnostic on purpose: a launchd agent on macOS and a Task Scheduler
 * script on Windows produce the same bytes, so moving machines needs no reflash. */
enum {
    TEXT_CHANNEL = 0x12,
    TEXT_SET     = 0x01,
    TEXT_CLEAR   = 0x02,
    /* Per-line set: [.., TEXT_SET_LINE, line, icon, ASCII...].
     * A second line does not fit in one packet -- 32 bytes leaves ~27 for text
     * after framing, and two 16-char lines is 32 -- so each line gets its own
     * report. Torn updates are harmless: the lines are independently meaningful
     * (title / artist) and the producer polls every 3 s. */
    TEXT_SET_LINE = 0x03,
    /* Playback position: [.., TEXT_PLAYBACK, state, pos_hi, pos_lo, dur_hi,
     * dur_lo]. Seconds, big-endian, 16 bits -> 18.2 h, well past any track.
     * state 0 hands the band back to the clock. */
    TEXT_PLAYBACK = 0x04,
};

static inline bool is_text_cmd(const uint8_t *data, uint8_t length) {
    return length >= 3 && data[0] == RTC_SET_VALUE && data[1] == TEXT_CHANNEL;
}

static void text_command(uint8_t *data, uint8_t length) {
    switch (data[2]) {
        case TEXT_SET:
            /* data[3] = icon id, data[4..] = ASCII. Length is whatever the host
             * sent; display_set_text() clamps and sanitises. */
            if (length >= 4) {
                display_set_text(data[3], (const char *)&data[4],
                                 (uint8_t)(length - 4));
            }
            break;
        case TEXT_SET_LINE:
            /* data[3] = line, data[4] = icon, data[5..] = ASCII. */
            if (length >= 5) {
                display_set_text_line(data[3], data[4], (const char *)&data[5],
                                      (uint8_t)(length - 5));
            }
            break;
        case TEXT_PLAYBACK:
            if (length >= 8) {
                display_set_playback(data[3],
                                     (uint16_t)((data[4] << 8) | data[5]),
                                     (uint16_t)((data[6] << 8) | data[7]));
            }
            break;
        case TEXT_CLEAR:
            display_clear_text();
            break;
        default:
            data[0] = RTC_UNHANDLED;
            break;
    }
}

// ---------------------------------------------------------------------------
// Health channel: read the unified health counters over raw HID (health.c).
// Same VIA custom-value framing as the channels above:
//   [SET_VALUE, HEALTH_CHANNEL, HC_GET] -> [.., .., HC_GET, version, 28 bytes]
// Raw HID is the PRIMARY health readout -- it exists in every build flavor,
// where the console exists only in instrumented ones. Replies route through
// the active host driver, so like ak820ctl this needs the dip switch in wired
// mode; the counters themselves accumulate in any mode.
enum {
    HEALTH_CHANNEL = 0x13,
    HC_GET         = 0x01,
#ifdef WDT_TEST_HOOKS
    /* Test-only, instrumented builds: deliberately wedge the main loop to
     * prove the watchdog resets the board and the boot accounting works.
     *   [SET_VALUE, HEALTH_CHANNEL, HC_STALL, mode]
     * mode 1: spin forever (interrupts still running -- the historical hang
     *         signature). mode 2: force a kb-eeconfig flash write, then spin,
     *         so the reset lands as close after a program cycle as this test
     *         can arrange. The reply never arrives, by design. */
    HC_STALL       = 0x7E,
#endif
};
#define HEALTH_PROTO_VERSION 1

static inline bool is_health_cmd(const uint8_t *data, uint8_t length) {
    return length >= 3 && data[0] == RTC_SET_VALUE && data[1] == HEALTH_CHANNEL;
}

static void health_command(uint8_t *data, uint8_t length) {
    switch (data[2]) {
        case HC_GET:
            if (length >= 32) {
                data[3] = HEALTH_PROTO_VERSION;
                health_fill(&data[4]);   /* 28 bytes: exactly fills the report */
            } else {
                data[0] = RTC_UNHANDLED;
            }
            break;
#ifdef WDT_TEST_HOOKS
        case HC_STALL:
            if (length >= 4) {
                if (data[3] == 2) {
                    kb_eeconfig_test_write();   /* a REAL flash program first */
                }
                for (;;) { /* wedge: the watchdog must get us out of here */ }
            }
            break;
#endif
        default:
            data[0] = RTC_UNHANDLED;
            break;
    }
}

static inline bool rtc_is_set_time_cmd(const uint8_t *data, uint8_t length) {
    return length >= 10 && data[0] == RTC_SET_VALUE &&
           data[1] == RTC_CHANNEL && data[2] == RTC_SET_TIME;
}

static inline bool rtc_is_get_time_cmd(const uint8_t *data, uint8_t length) {
    return length >= 3 && data[0] == RTC_SET_VALUE &&
           data[1] == RTC_CHANNEL && data[2] == RTC_GET_TIME;
}

/* Fill the reply in place with the live clock. */
static void rtc_read_into(uint8_t *data) {
    /* Zeroed up front: rtc_get_time() leaves t untouched when it fails, and the
     * fields below are serialized regardless of ok. Without this the reply
     * carries stack garbage -- nondeterministic, and a small leak of whatever
     * was on the stack. data[3] already tells the host the reading is invalid. */
    rtc_time_t t = {0};
    bool ok = rtc_get_time(&t);
    data[3] = ok ? 1 : 0;
    data[4] = (uint8_t)(t.year >= 2000 ? t.year - 2000 : 0);
    data[5] = t.month;
    data[6] = t.day;
    data[7] = t.weekday;
    data[8] = t.hours;
    data[9] = t.minutes;
    data[10] = t.seconds;
}

#if defined(VIA_ENABLE)

// VIA owns raw_hid_receive() and dispatches custom-value commands here. VIA echoes
// the buffer back itself -- do NOT call raw_hid_send().
void via_custom_value_command_kb(uint8_t *data, uint8_t length) {
    if (rtc_is_get_time_cmd(data, length)) {
        rtc_read_into(data);
        return;
    }
    if (rtc_is_set_time_cmd(data, length)) {
        // Honest reply: a rejected packet or failed I2C write must not echo
        // back as "handled" -- the host's next poll is its only truth.
        if (!rtc_apply_bytes(&data[3])) data[0] = RTC_UNHANDLED;
        return;
    }
    if (is_flash_cmd(data, length)) {
        flash_command(data, length);
        return;
    }
    if (is_text_cmd(data, length)) {
        text_command(data, length);
        return;
    }
    if (is_health_cmd(data, length)) {
        health_command(data, length);
        return;
    }
    data[0] = RTC_UNHANDLED;
}

#else // no VIA: handle the same packet directly and echo it back like VIA would.

void raw_hid_receive(uint8_t *data, uint8_t length) {
    if (rtc_is_get_time_cmd(data, length)) {
        rtc_read_into(data);
    } else if (rtc_is_set_time_cmd(data, length)) {
        if (!rtc_apply_bytes(&data[3])) data[0] = RTC_UNHANDLED;
    } else if (is_flash_cmd(data, length)) {
        flash_command(data, length);
    } else if (is_text_cmd(data, length)) {
        text_command(data, length);
    } else if (is_health_cmd(data, length)) {
        health_command(data, length);
    } else {
        data[0] = RTC_UNHANDLED;
    }
    raw_hid_send(data, length);
}

#endif



__attribute__((weak)) void display_housekeeping_task(void) {}


/* Health line for the never-started-DMA fault, printed ONLY when a failure
 * actually happens -- i.e. when timeouts or retries move.
 *
 * This used to print blits/s every second. That served its purpose: it measured
 * the blit rate (median 1/s idle, bursts to ~85) and killed the theory that the
 * two-line text slot and playback readout had raised the hang rate by raising
 * blit volume. Once answered it was a USB write per second for a number nobody
 * reads, so only the failure signal is left.
 *
 * i2c-overlap was dropped from the line too: it stayed 0 across hundreds of
 * pcf_read opportunities, so the RTC bus guard is confirmed to be doing nothing
 * for this fault. The guard STAYS -- it is correct per the contract documented
 * in lcd_bus.c -- but it does not need reporting.
 *
 * blits/s is still included as context on the failure line, where it is free. */
#ifdef CONSOLE_ENABLE
static void blit_stat_task(void) {
    static uint32_t last_at = 0;
    static uint16_t last_to = 0, last_re = 0;
    if (timer_elapsed32(last_at) < 1000) return;
    last_at = timer_read32();

    uint32_t rate = lcd_blit_count_take();
    uint16_t to   = lcd_blit_timeouts();
    uint16_t re   = lcd_blit_retries();
    if (to != last_to || re != last_re) {
        dprintf("[lcd] timeouts=%u retries=%u (blits/s=%lu)\n",
                (unsigned)to, (unsigned)re, (unsigned long)rate);
        last_to = to;
        last_re = re;
    }
}
#endif

void housekeeping_task_kb(void) {
    health_loop_tick();    // worst-gap max-hold; one timer read per pass
#ifdef LOOPGAP_INSTRUMENT
    loop_gap_task();
#endif
    rgb_repeat_task();
    display_blit_pump();   // one glyph per iteration; never waits on the DMA
#ifdef CONSOLE_ENABLE
    blit_stat_task();
#endif

    // Throttle the housekeeping to 10 Hz
    static uint32_t last_t = 0;
    if (timer_elapsed32(last_t) >= 100) {
        last_t = timer_read32();

        update_leds();
        bt_pair_hold_task();   // hold-to-pair fires under the finger, not on release
        modified_consumer_task();  // drop held mods once a knob spin stops
#ifdef PARAM_OVERLAY
        param_status_task();   // surface setting changes in the info band
#endif
#ifdef CONSOLE_ENABLE
        key_stat_task();       // dropped-keystroke localisation; see key_press_count
#endif

        if (!anim_active()) rtc_task();   // RTC I2C (port A) glitches the flash SPI1 pins (A12/A13) mid-DMA
        anim_task();                      // one animation frame per 100 ms
        display_housekeeping_task();
        health_task();                    // [health] console line, on change only
    }

    // Chain the user hook
    housekeeping_task_user();

    // Kick the watchdog LAST -- after the 10 Hz block and the user hook, so a
    // kick certifies a COMPLETED pass. A kick at entry would hand a freshly
    // wedged pass another full timeout and prove nothing about what follows.
    watchdog_kick();
}

/*
 * Defer eeconfig flushes away from LCD DMA activity.
 *
 * Confirmed failure: adjusting RGB writes eeconfig on every step, and an SN32 EFL
 * program/erase overlapping an in-flight flash->LCD DMA blit wedges the board --
 * LED row mux frozen mid-cycle, raw HID dead, USB still enumerated, recoverable
 * only by a power cycle. Console capture caught it ending on
 * "rgb matrix set hsv [EEPROM]" with the next line never arriving.
 *
 * efl_ramtext.diff (flash routines in SRAM) narrows this but does not close it,
 * and CORTEX_ENABLE_WFI_IDLE FALSE does not fix it either -- that only removed
 * the lost-wakeup half of the failure described in config.h; the missed SPI0 DMA
 * completion is the half that actually bites.
 *
 * So simply do not start a flash write while a blit is in flight. Paired with
 * RGB_MATRIX_EEPROM_WRITE_DELAY this also cuts the write rate from ~8/s while a
 * key is held to at most one per interval, which matters for flash wear too.
 *
 * Deliberately NOT gated on anim_active(): the animation player runs continuous
 * DMA, and blocking on it would mean RGB settings never persist while an
 * animation is on. Blits are short, so per-blit gating still finds gaps.
 */
/* Drain any flash->LCD DMA before the wear-levelling layer programs or erases
 * internal flash. See backing_store_pre_write_hook() in
 * platforms/chibios/drivers/wear_leveling/wear_leveling_efl.c.
 *
 * WHY THIS IS BROADER THAN rgb_matrix_eeprom_flush_allowed() BELOW: that hook
 * only gates RGB's own eeconfig flush, which was the trigger we happened to
 * notice first. VIA's dynamic-keymap writes take a completely different path and
 * were never covered -- assigning keys in VIA reproduced the hang (one LED row
 * stuck lit, board dead until a power cycle) on 2026-08-29. The bug was never
 * about RGB; it is about ANY internal-flash write overlapping the LCD DMA.
 *
 * Waiting is sufficient rather than merely narrowing the window: flash writes are
 * synchronous on the main loop, and blits are started from the main loop too, so
 * once the in-flight blit has drained no new one can begin before the write
 * completes.
 *
 * Bounded like every other blit wait here -- an unbounded spin would trade an
 * intermittent hang for a guaranteed one if a completion interrupt is ever
 * genuinely lost. */
void backing_store_pre_write_hook(void) {
#ifdef LOOPGAP_INSTRUMENT
    loop_stall_mark = LOOP_MARK_FLASH;   /* every internal-flash writer passes here */
#endif
    /* lcd_blit_wait() rather than a bare spin: a blit whose completion IRQ was
     * missed never clears, so the old loop burned its full second here on every
     * single flash write and left the bus asserted. See lcd_blit_wait(). */
    lcd_blit_wait();
}

bool rgb_matrix_eeprom_flush_allowed(void) {
    /* Two gates.
     *
     * 1. Never start an internal-flash write while a flash->LCD DMA blit is in
     *    flight -- the original reason this hook exists.
     *
     * 2. Wait until the RGB settings have actually SETTLED.
     *
     * QMK's eeconfig_flush_rgb_matrix_task() is a RATE LIMIT, not a debounce:
     *
     *     if (timer_elapsed(flush_timer) > timeout) { flush(); flush_timer = timer_read(); }
     *
     * so it fires every RGB_MATRIX_EEPROM_WRITE_DELAY ms for as long as the
     * config stays dirty -- including throughout a held adjust key. Each write
     * makes the flash array busy for milliseconds, during which the row ISR
     * cannot run to arm the next row and the matrix goes DARK. Observed on
     * hardware as a dark flash mid-hold while sweeping brightness.
     *
     * Holding the flush off until the values stop moving turns "a write every
     * 750 ms while adjusting" into "one write once you settle". That also cuts
     * flash wear, and cuts exposure to the still-unexplained RGB-adjust hang,
     * whose trigger is an internal-flash write.
     *
     * Cheap enough to run from rgb_task_sync: six byte compares and a timer
     * read. Self-contained on purpose -- it must not depend on PARAM_OVERLAY,
     * which is a removable nicety. */
    if (lcd_blit_busy()) return false;

    static uint8_t  last_mode = 0, last_h = 0, last_s = 0, last_v = 0, last_sp = 0;
    static bool     last_on = false, primed = false;
    static uint32_t settled_since = 0;

    uint8_t mode = rgb_matrix_get_mode();
    uint8_t h    = rgb_matrix_get_hue();
    uint8_t sa   = rgb_matrix_get_sat();
    uint8_t v    = rgb_matrix_get_val();
    uint8_t sp   = rgb_matrix_get_speed();
    bool    on   = rgb_matrix_is_enabled();

    if (!primed) {
        primed = true;
        last_mode = mode; last_h = h; last_s = sa; last_v = v; last_sp = sp; last_on = on;
        settled_since = timer_read32();
    } else if (mode != last_mode || h != last_h || sa != last_s ||
               v != last_v || sp != last_sp || on != last_on) {
        last_mode = mode; last_h = h; last_s = sa; last_v = v; last_sp = sp; last_on = on;
        settled_since = timer_read32();      // still moving -- restart the settle window
    }

    return timer_elapsed32(settled_since) >= RGB_SETTLE_MS;
}
