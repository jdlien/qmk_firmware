// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
/* The menu's pure modules (panel_owner.c, menu_input.c, menu_slots.c,
 * menu_fixture.c) wired into the firmware. S0 of
 * plans/CONFIG-MENU-BUILD-PLAN.md: the MENU owner's screen is the fixture, and
 * normal firmware never captures Fn plus the knob (MENU_ENABLE off) unless a
 * dev build's op 27 arms it. */
#include "menu_glue.h"

#include <string.h>

#include "menu_backend.h"
#include "menu_fixture.h"
#include "menu_input.h"
#include "menu_slots.h"
#include "ak820pro.h"
#include "graphics/display.h"
#include "graphics/lcd_bus.h"

/* --- The backend: menu_backend.h on lcd_bus.c --------------------------------- */

bool mb_clear_band(uint8_t band) {
    return lcd_clear_rect_ok(0, (uint16_t)(band * MB_BAND_H), MB_W, MB_BAND_H);
}
void mb_fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t rgb565) {
    lcd_fill_rect(x0, y0, x1, y1, rgb565);
}
bool mb_glyph(char c, uint16_t x, uint16_t y) {
    return lcd_draw_flash_glyph_try(ASSET_IOSEVKA_MEDIUM_13, c, x, y);   /* FONT_SMALL: Cozette 6x14 */
}
bool mb_busy(void) { return lcd_blit_busy(); }
bool mb_lost_pending(void) { return lcd_blit_lost_pending(); }

/* --- Capture -------------------------------------------------------------------- */

#ifdef AK820_DEV_HOOKS
static bool armed = false;   /* op 27 sub 6: off at boot, never persisted */
#endif
static uint8_t losses_seen;

static bool menu_enabled(void) {
#if defined(MENU_ENABLE)
    return true;
#elif defined(AK820_DEV_HOOKS)
    return armed;
#else
    return false;
#endif
}

#define OWNER_MENU_OPEN() (po_owner() == PANEL_MENU && po_session() == SESSION_OPEN)

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
    switch (mi_physical(c, e->pressed, fn, menu_enabled(), po_session(), timer_read32())) {
        case MI_FORWARD:  return true;
        case MI_OPENER:   (void)display_po_event(PO_EV_OPENER); return false;
        case MI_CONSUMED: return false;
    }
    return true;
}

/* --- The screen (the fixture, in S0) ------------------------------------------- */

void menu_screen_open(bool from_debug) {
    if (from_debug) fx_return_from_diag();
    else            fx_open();
    mi_note_input(timer_read32());   /* the idle timeout starts at the opening */
}

void menu_screen_invalidate(void) { fx_invalidate(); }
void menu_input_clear(void) { mi_clear(); }
void menu_note_loss(void) {
    if (losses_seen < 0xFFu) losses_seen++;
}

void menu_pump(void) {
    if (ms_step() == MS_STEP_COMMIT && (lcd_blit_busy() || lcd_blit_lost_pending()))
        po_note_fail(PO_FAIL_EARLY_COMMIT);   /* the sticky check, from fresh reads */
}

void menu_loss_dropped(void) {
    if (mi_outstanding() == MI_KIND_LOSS) mi_finish(MI_ST_SUPERSEDED);
}

/* op 27's mutations that end on a later pass. */
static void op27_poll(void) {
    switch (mi_outstanding()) {
        case MI_KIND_PATTERN_STOP:
            if (!OWNER_MENU_OPEN()) mi_finish(MI_ST_SUPERSEDED);
            else if (ms_stopped()) mi_finish(MI_ST_DONE);
            break;
        case MI_KIND_LOSS:
            if (!lcd_blit_lost_pending()) mi_finish(MI_ST_DONE);   /* the owner's handler took it */
            break;
        default:
            break;
    }
}

/* Each pass, in housekeeping_task_kb, before display_blit_pump(), independent of
 * the gated display housekeeping (C2). */
void menu_task(void) {
    uint32_t now = timer_read32();
    if (display_wake_notice_take()) (void)display_po_event(PO_EV_WAKE_RELEASED);
    mi_tick(now);
    op27_poll();
    if (po_session() != SESSION_OPEN) return;

    int8_t     turns;
    mi_latch_t latch;
    if (mi_take(&turns, &latch)) {
        if (latch == MI_LATCH_CLOSE) {
            (void)display_po_event(PO_EV_CLOSE);
            return;
        }
        if (po_owner() == PANEL_DEBUG) {
            /* Diagnostics: a click or a hold goes back to the menu. */
            if (latch != MI_LATCH_NONE) (void)display_po_event(PO_EV_DEBUG_CLICK);
            return;
        }
        switch (fx_input(turns, latch)) {
            case FX_DIAG:  (void)display_po_event(PO_EV_DIAG_HEALTH); return;
            case FX_CLOSE: (void)display_po_event(PO_EV_CLOSE); return;
            case FX_NONE:  break;
        }
    }
    /* 24 s with no knob input closes it (C7: hint repaints, host traffic and
     * typing don't restart this). The fixture has no edit, so no backstop. */
    if ((uint32_t)(now - mi_last_input()) >= MI_IDLE_MS) (void)display_po_event(PO_EV_CLOSE);
}

/* --- Op 27 (dev builds) ------------------------------------------------------- */
#ifdef AK820_DEV_HOOKS

static bool open_session(void) { return po_session() == SESSION_OPEN; }

/* A mutation the state can't take answers 2, one while another is
 * outstanding 1; neither touches the outstanding one. */
static uint8_t begin(mi_kind_t k, uint8_t *seq) { return mi_begin(k, seq); }

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void op27_read(uint8_t *d) {
    memset(&d[5], 0, 27);
    d[5]  = MI_OK;
    d[6]  = mi_last_seq();
    d[7]  = (uint8_t)mi_last_state();
    d[8]  = (uint8_t)po_owner();
    d[9]  = (uint8_t)po_session();
    put16(&d[10], po_gen());
    d[12] = (uint8_t)po_phase();
    d[13] = po_damaged();
    d[14] = OWNER_MENU_OPEN() ? fx_screen() : 0;
    d[15] = fx_cursor();
    uint8_t f = 0;
    if (armed) f |= 1u << 0;
    /* bit 1, an edit open: none in S0 */
    if (po_owner() == PANEL_MENU && ms_settled()) f |= 1u << 2;   /* pixels_done: the fixture's only */
    if (lcd_blit_busy()) f |= 1u << 3;
    if (po_owner() == PANEL_MENU && ms_clearing()) f |= 1u << 4;
    if (display_wake_held()) f |= 1u << 5;
    if (display_batt_dirty()) f |= 1u << 6;
    if (display_logo_stale()) f |= 1u << 7;
    d[16] = f;
    d[17] = display_queue_depths();
    d[18] = (uint8_t)mi_turns();
    d[19] = (uint8_t)mi_latch();
    d[20] = mi_dropped();
    d[21] = po_owner() == PANEL_MENU ? ms_unsettled() : 0;
    d[22] = losses_seen;
    d[23] = mi_capture_bits();
    put16(&d[24], po_invariant_fail());
    /* 26-27: the last paint's time (S1); 28: the pairing phase (P3) */
    d[29] = ms_ops();
    d[30] = ms_stopped() ? 1u : 0u;
}

bool menu_op27(uint8_t *d) {
    const uint8_t sub = d[4];
    const uint32_t now = timer_read32();
    uint8_t st = MI_OK, seq = 0;

    if (sub == 5) {
        op27_read(d);
        return true;
    }
    switch (sub) {
        case 0: {   /* the fixture's paint */
            uint8_t p = d[5], n = d[6];
            bool    f1 = OWNER_MENU_OPEN() && fx_screen() == FX_SCREEN_F1;
            if (!OWNER_MENU_OPEN() || p > 3) { st = MI_BAD; break; }
            if (p != 2 && n != 0) { st = MI_BAD; break; }
            if ((p == 0 || p == 1) && !f1) { st = MI_BAD; break; }
            if (p == 2 && (!f1 || n < 1 || n >= FX_F1_OPS || fx_pattern() != 0 || !ms_settled())) { st = MI_BAD; break; }
            st = begin(p == 2 ? MI_KIND_PATTERN_STOP : MI_KIND_PATTERN, &seq);
            if (st != MI_OK) break;
            if (p == 3) fx_other_layout();
            else fx_set_pattern(p == 0 ? 0 : 1);
            if (p == 2) ms_stop_after(n);
            else mi_finish(MI_ST_DONE);   /* the model has its new target */
            break;
        }
        case 1: {   /* turns */
            int8_t n = (int8_t)d[5];
            if (!open_session() || n == 0 || n > 16 || n < -16) { st = MI_BAD; break; }
            st = begin(MI_KIND_TURNS, &seq);
            if (st != MI_OK) break;
            mi_syn_turns(n);
            mi_note_input(now);
            break;
        }
        case 2:     /* a click */
            if (!open_session()) { st = MI_BAD; break; }
            st = begin(MI_KIND_CLICK, &seq);
            if (st != MI_OK) break;
            mi_syn_click();
            mi_note_input(now);
            break;
        case 3: {   /* a hold */
            uint16_t ms = (uint16_t)(d[5] | (d[6] << 8));
            if (!ms) ms = 650;
            if (!open_session() || ms < 50 || ms > 5000) { st = MI_BAD; break; }
            if (mi_knob_down() || mi_syn_hold_active()) { st = MI_BUSY; break; }
            st = begin(MI_KIND_HOLD, &seq);
            if (st != MI_OK) break;
            mi_syn_hold(ms, now);
            mi_note_input(now);
            break;
        }
        case 4:     /* Fn plus a click */
            st = begin(MI_KIND_OPENER, &seq);
            if (st != MI_OK) break;
            mi_finish(display_po_event(open_session() ? PO_EV_CLOSE : PO_EV_OPENER) & PO_ACT_REFUSED
                          ? MI_ST_REFUSED : MI_ST_DONE);
            break;
        case 6:     /* arm or disarm */
            if (d[5] > 1) { st = MI_BAD; break; }
            st = begin(MI_KIND_ARM, &seq);
            if (st != MI_OK) break;
            armed = d[5] != 0;
            /* Disarming closes a session; the capture bits still owed are kept. */
            if (!armed && po_session() != SESSION_NONE) (void)display_po_event(PO_EV_CLOSE);
            mi_finish(MI_ST_DONE);
            break;
        case 7:     /* a restore request, as the startup callback makes */
            st = begin(MI_KIND_RESTORE, &seq);
            if (st != MI_OK) break;
            (void)display_po_event(PO_EV_STARTUP);
            mi_finish(MI_ST_DONE);   /* installed: watch the phase and generation */
            break;
        case 8:     /* a loss, as an exhausted recovery marks one */
            st = begin(MI_KIND_LOSS, &seq);
            if (st != MI_OK) break;
            lcd_blit_lost_inject();
            break;
        default:
            return false;
    }
    memset(&d[5], 0, 27);
    d[5] = st;
    d[6] = st == MI_OK ? seq : 0;
    return true;
}
#endif
