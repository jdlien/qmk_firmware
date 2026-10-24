// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
/* The menu wired into the firmware: menu_backend.h and menu_env.h on
 * lcd_bus.c and display.c, the knob's identity, and the entry points the
 * board calls. Everything with a decision in it is menu_ops.c's, which the
 * host tests run. plans/CONFIG-MENU-BUILD-PLAN.md. */
#include "menu_glue.h"
#include "menu_backend.h"
#include "menu_env.h"
#include "ak820pro.h"
#include "graphics/display.h"
#include "graphics/lcd_bus.h"

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
bool mb_busy(void) { return lcd_blit_busy(); }
bool mb_lost_pending(void) { return lcd_blit_lost_pending(); }

/* --- menu_env.h ---------------------------------------------------------------- */

uint16_t env_po_event(po_event_t ev) { return display_po_event(ev); }
bool     env_wake_notice_take(void) { return display_wake_notice_take(); }
bool     env_wake_held(void) { return display_wake_held(); }
#ifdef AK820_DEV_HOOKS
void    env_lost_inject(void) { lcd_blit_lost_inject(); }
bool    env_batt_dirty(void) { return display_batt_dirty(); }
bool    env_logo_stale(void) { return display_logo_stale(); }
uint8_t env_queue_depths(void) { return display_queue_depths(); }
bool    env_probe_handoff(uint8_t *out4) { return display_probe_handoff(out4); }
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
bool menu_pre_process(uint16_t keycode, keyrecord_t *record) {
    (void)keycode;
    const keyevent_t *e = &record->event;
    mi_control_t      c;
    if (e->type == ENCODER_CW_EVENT && e->key.col == 0) c = MI_CW;
    else if (e->type == ENCODER_CCW_EVENT && e->key.col == 0) c = MI_CCW;
    else if (e->type == KEY_EVENT && e->key.row == KNOB_ROW && e->key.col == KNOB_COL) c = MI_KNOB;
    else return true;

    bool fn = (layer_state & (((layer_state_t)1 << WINFN) | ((layer_state_t)1 << MACFN))) != 0;
    return menu_physical(c, e->pressed, fn, timer_read32());
}

void menu_task(void) { menu_ops_task(timer_read32()); }

#ifdef AK820_DEV_HOOKS
bool menu_op27(uint8_t *data) { return menu_ops_op27(data, timer_read32()); }
#endif
