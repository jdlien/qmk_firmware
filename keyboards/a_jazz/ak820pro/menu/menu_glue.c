// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
/* The menu wired into the firmware: menu_backend.h and menu_env.h on
 * lcd_bus.c and display.c, the knob's identity, and the entry points the
 * board calls. Everything with a decision in it is menu_ops.c's, which the
 * host tests run. plans/CONFIG-MENU-BUILD-PLAN.md. */
#include "menu_glue.h"
#include "menu_backend.h"
#include "menu_env.h"
#include "menu_meas.h"
#include "ak820pro.h"
#include "health.h"
#include "rtc/rtc.h"
#include "graphics/display.h"
#include "graphics/lcd_bus.h"
#include "battery.h"
#include "power.h"
#include "kb_eeconfig.h"
#include "bluetooth/ch582f_ajazz.h"
#include "rgb_effects.h"

/* --- menu_backend.h ------------------------------------------------------------ */

bool mb_clear_band(uint8_t band) {
    return lcd_clear_rect_ok(0, (uint16_t)(band * MB_BAND_H), MB_W, MB_BAND_H);
}
bool mb_fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t rgb565) {
    return lcd_fill_rect_ok(x0, y0, x1, y1, rgb565);
}
bool mb_glyph(char c, uint16_t x, uint16_t y) {
    return lcd_draw_flash_glyph_try(ASSET_IOSEVKA_MEDIUM_13, c, x, y);   /* FONT_SMALL: Cozette 6x14 */
}
bool mb_glyph_big(char c, uint16_t x, uint16_t y) {
    return lcd_draw_flash_glyph_try(ASSET_IOSEVKA_MEDIUM_20, c, x, y);   /* FONT_STATUS: 10x23 */
}
bool mb_rows(uint16_t x, uint16_t y, uint16_t w, uint16_t h, mb_row_fn fn, const void *ctx) {
    return lcd_blit_rows(x, y, w, h, fn, ctx);
}
bool mb_mono(const uint8_t *bits, uint8_t stride, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t fg, uint16_t bg) {
    return lcd_blit_mono(bits, stride, x, y, w, h, fg, bg);
}
bool mb_busy(void) { return lcd_blit_busy(); }
bool mb_lost_pending(void) { return lcd_blit_lost_pending(); }
uint32_t mb_now_ticks(void) { return meas_ticks(); }
uint32_t mb_dma_end_ticks(void) { return meas_ticks_at16(lcd_blit_end_tick16()); }

/* --- menu_env.h ---------------------------------------------------------------- */

/* P1a's settings: the same calls as the keys that change them (Fn+B's mode
 * without its overlay, power_set_mode; Fn+C's pair for the clock). */
uint8_t env_power_mode(void) { return (uint8_t)power_mode(); }
void    env_power_set(uint8_t m) { power_set_mode((power_mode_t)m); }
uint8_t env_clock_mode(void) { return display_get_clock_mode(); }
void    env_clock_set(uint8_t m) {
    display_set_clock_mode(m);
    kb_eeconfig_set_clock_mode(m);
}
void env_health_reset(void) {
    health_reset();
    display_debug_refresh();
}

/* P1b's primitives (menu_edit.c sequences them): one QMK or board call each. */
void env_rgb_get(env_rgb_t *c) {
    c->mode   = rgb_matrix_get_mode();
    c->h      = rgb_matrix_get_hue();
    c->s      = rgb_matrix_get_sat();
    c->v      = rgb_matrix_get_val();
    c->speed  = rgb_matrix_get_speed();
    c->enable = rgb_matrix_is_enabled() ? 1u : 0u;
    c->flags  = rgb_matrix_get_flags();
}
void env_rgb_mode(uint8_t mode, bool save) { save ? rgb_matrix_mode(mode) : rgb_matrix_mode_noeeprom(mode); }
void env_rgb_hsv(uint8_t h, uint8_t s, uint8_t v, bool save) {
    save ? rgb_matrix_sethsv(h, s, v) : rgb_matrix_sethsv_noeeprom(h, s, v);
}
void env_rgb_speed(uint8_t speed, bool save) { save ? rgb_matrix_set_speed(speed) : rgb_matrix_set_speed_noeeprom(speed); }
void env_rgb_flags(uint8_t flags) { rgb_matrix_set_flags_noeeprom(flags); }
void env_rgb_enable(bool on, bool save) {
    if (on) save ? rgb_matrix_enable() : rgb_matrix_enable_noeeprom();
    else save ? rgb_matrix_disable() : rgb_matrix_disable_noeeprom();
}
uint8_t env_lcd_level(void) { return display_get_brightness(); }
void    env_lcd_set(uint8_t level, bool save) {
    display_set_brightness(level);
    if (save) kb_eeconfig_set_lcd_brightness(level);
}
uint8_t     env_effects(void) { return rgb_effects_n; }
uint8_t     env_effect_mode(uint8_t i) { return i < rgb_effects_n ? rgb_effects[i].mode : 0; }
const char *env_effect_name(uint8_t i) { return i < rgb_effects_n ? rgb_effects[i].name : ""; }
/* The bootloader item: the raw-HID jump's preparation (hid_protocol.c,
 * HC_BOOTLOADER; codex on the item: over BT clear_keyboard()'s releases are
 * frames the main loop pumps, so pump them and see them ACKed, or a key stays
 * down at the host through the flash), then exactly Fn+Esc's path (ak820pro.c,
 * QK_BOOT): the notice, held long enough to read, then QMK's clean reset into
 * the ROM. Never the direct jump to SN32_BOOTLOADER_ADDRESS: it made flashing
 * unreliable (the comment at QK_BOOT, 2026-08-31). The raw-HID path's 2 s
 * idle guard isn't copied: the knob's click is input activity. */
bool env_bootloader(void) {
    clear_keyboard();
    bool queued = ch582_is_usb() || ch582_send_releases();   /* fresh, on every attempt */
    if (!queued || !ch582_tx_drain(500)) return false;
    display_bootloader_splash();
    wait_ms(1500);
    reset_keyboard();   /* does not return */
    return true;
}
/* flash.sh's rule exactly: on battery AND the RGB cut fired (the cut stays
 * latched a few seconds after power arrives). */
bool env_reserve(void) { return battery_on_battery() && power_lights_cut(); }
bool        env_effect_two_tone(uint8_t mode) {
#ifdef ENABLE_RGB_MATRIX_ALPHAS_MODS
    return mode == RGB_MATRIX_ALPHAS_MODS;   /* alpha_mods_anim.h: hsv.h += speed */
#else
    (void)mode;
    return false;
#endif
}

/* About's snapshot (P1a): compact facts, frozen while the page shows. */
void env_about(env_about_t *a) {
    a->uptime_s = rtc_seconds_since_boot();   /* slept seconds included; timer_read32 stops asleep */
    a->batt_pm  = battery_level_permille();   /* 0xFFFF unknown */
    uint16_t mv = battery_pack_mv();
    a->pack_mv  = mv ? mv : ENV_UNKNOWN;       /* its unknown is 0 */
    a->left_min = battery_time_left_min();    /* 0xFFFF none */
    a->mode     = (uint8_t)power_mode();
    uint8_t tr  = ch582_is_usb() ? ENV_LINK_USB : ch582_is_24g() ? ENV_LINK_24G : ENV_LINK_BT;
    uint8_t sl  = ch582_get_target_slot();     /* 1-3 BT, 0 none */
    uint8_t st  = (uint8_t)ch582_get_conn_state();
    a->link     = (uint8_t)(tr | ((sl ? (uint8_t)(sl - 1u) & 3u : 0u) << 2) | ((st & 7u) << 4));
}

#ifndef AK820_BUILD_HASH
#    define AK820_BUILD_HASH 0u   /* a build without build.sh */
#endif
#ifndef AK820_BUILD_DIRTY
#    define AK820_BUILD_DIRTY 1
#endif
#ifndef AK820_BUILD_TOKEN
#    define AK820_BUILD_TOKEN 0u
#endif
uint32_t env_build_hash(void) { return AK820_BUILD_HASH; }
bool     env_build_dirty(void) { return AK820_BUILD_DIRTY != 0; }
uint32_t env_build_token(void) { return AK820_BUILD_TOKEN; }
const char *env_build_flavour(void) {
#if defined(AK820_DEVFAULT)
    return "fault";
#elif defined(LOOPGAP_INSTRUMENT)
    return "instr";
#elif defined(AK820_DEV_HOOKS)
    return "dev";
#elif defined(AK820PRO_LCD_VARIANT_FPB)
    return "fpb";
#else
    return "daily";
#endif
}

uint16_t env_po_event(po_event_t ev) { return display_po_event(ev); }
bool     env_wake_notice_take(void) { return display_wake_notice_take(); }
bool     env_wake_held(void) { return display_wake_held(); }
#ifdef AK820_DEV_HOOKS
void    env_lost_inject(void) { lcd_blit_lost_inject(); }
bool    env_batt_dirty(void) { return display_batt_dirty(); }
bool    env_logo_stale(void) { return display_logo_stale(); }
uint8_t env_queue_depths(void) { return display_queue_depths(); }
bool    env_probe_handoff(uint8_t *out4) { return display_probe_handoff(out4); }

void env_meas(env_meas_t *m) {
    m->pass_max_ticks = meas_pass_max_ticks();
    m->pass_over15    = meas_pass_over15();
    m->passes         = meas_passes();
    m->band_max_ticks = meas_band_max_ticks();
    m->token          = meas_token();
    m->aliased        = meas_aliased();
    m->health_gen     = health_reset_generation();
    display_debug_meas(&m->page, &m->dbg_gen, &m->dbg_done_gen, &m->dbg_sec);
    rtc_time_t t;
    m->rtc_sec = rtc_get_time(&t) ? t.seconds : 0xFFu;
}
uint16_t env_meas_arm(void) { return meas_arm(); }
bool     env_debug_done(void) { return display_debug_done(); }
#endif

/* --- The knob -------------------------------------------------------------------- */

#define KNOB_ROW 0
#define KNOB_COL 14   /* keyboard.json: the encoder's press */

/* By physical identity, never by keycode, so a VIA-remapped knob still drives
 * the menu and KC_MUTE or the volume codes are never matched. Fn is a WINFN or
 * MACFN bit already active at the press: a layer-tap whose layer isn't on yet
 * isn't Fn. Runs from pre_process_record_kb, after QMK has cleared the weak
 * mods and done the layer lookup, before combos and tapping, which never see a
 * captured event. */
static bool detent_swallowed;   /* the last encoder half this dropped */

bool menu_pre_process(uint16_t keycode, keyrecord_t *record) {
    (void)keycode;
    const keyevent_t *e = &record->event;
    mi_control_t      c;
    if (e->type == ENCODER_CW_EVENT && e->key.col == 0) c = MI_CW;
    else if (e->type == ENCODER_CCW_EVENT && e->key.col == 0) c = MI_CCW;
    else if (e->type == KEY_EVENT && e->key.row == KNOB_ROW && e->key.col == KNOB_COL) c = MI_KNOB;
    else return true;

    bool fn  = (layer_state & (((layer_state_t)1 << WINFN) | ((layer_state_t)1 << MACFN))) != 0;
    bool fwd = menu_physical(c, e->pressed, fn, timer_read32());
    if (c != MI_KNOB) detent_swallowed = !fwd;
    return fwd;
}

#ifdef ENCODER_MAP_ENABLE
/* quantum/encoder.c waits ENCODER_MAP_KEY_DELAY after each half of a detent,
 * to pace the host's reports. A half the menu swallowed sent none, so a
 * captured spin paid two 1 ms waits a detent of blocked main loop for
 * nothing. Read once per half, right after its action_exec(), and cleared,
 * so a half that never reached here keeps the wait. */
bool encoder_map_delay_wanted_kb(void) {
    bool swallowed   = detent_swallowed;
    detent_swallowed = false;
    return !swallowed;
}
#endif

/* C10's pass timing, and the tick's extension: first thing in every pass,
 * beside health's own gap, so an interval is one whole pass (codex, 10-24:
 * from menu_task() it spanned the previous pass's pump and this one's second
 * edge). */
void menu_pass_begin(void) { meas_pass(timer_read32()); }

void menu_task(void) { menu_ops_task(timer_read32()); }

#ifdef AK820_DEV_HOOKS
bool menu_op27(uint8_t *data) { return menu_ops_op27(data, timer_read32()); }
#endif
