// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
/* The menu's task each pass, op 27, and the menu's side of an owner event.
 * S0 of plans/CONFIG-MENU-BUILD-PLAN.md: the MENU owner's screen is the
 * fixture, and normal firmware never captures Fn plus the knob (MENU_ENABLE
 * off) unless a dev build's op 27 arms it. Pure logic: the host tests run it. */
#include "menu_ops.h"

#include <string.h>

#include "menu.h"
#include "menu_backend.h"
#include "menu_env.h"
#include "menu_fixture.h"
#include "menu_slots.h"

#define OWNER_MENU_OPEN() (po_owner() == PANEL_MENU && po_session() == SESSION_OPEN)

static bool    armed = false;   /* op 27 sub 6: off at boot, never persisted */
static uint8_t losses_seen;

/* The session's kind (P1a; plans/CONFIG-MENU-P1-DESIGN.md): installed when an
 * opener starts a session from NONE, before the opening's callbacks run, kept
 * through PENDING and DEBUG/OPEN, cleared once the session is NONE again (a
 * close or a PENDING cancel). The physical opener opens the menu, or the
 * fixture while op 27's arming is set (the S0/S1 scripts); op 27 sub 4 the
 * fixture, sub 11 the menu. */
enum { MK_NONE = 0, MK_FIXTURE, MK_MENU };
static uint8_t menu_kind   = MK_NONE;
static uint8_t report_kind = MK_FIXTURE;   /* op 27 sub 5's: the last session's, kept after it closes */
/* Returns the reporting kind to put back if the opener is refused (codex:
 * a refused opener left sub 5 reporting the refused kind). */
static uint8_t kind_for_new_session(uint8_t k) {
    uint8_t prev = report_kind;
    if (po_session() == SESSION_NONE) menu_kind = report_kind = k;
    return prev;
}
static void kind_refused(uint8_t prev) {
    report_kind = prev;
    if (po_session() == SESSION_NONE) menu_kind = MK_NONE;
}

bool menu_enabled(void) {
#if defined(MENU_ENABLE)
    return true;
#else
    return armed;   /* only op 27 (dev builds) ever sets it */
#endif
}

bool menu_physical(mi_control_t c, bool pressed, bool fn, uint32_t now) {
    switch (mi_physical(c, pressed, fn, menu_enabled(), po_session(), now)) {
        case MI_FORWARD:  return true;
        case MI_OPENER: {
            uint8_t prev = kind_for_new_session(armed ? MK_FIXTURE : MK_MENU);
            if (env_po_event(PO_EV_OPENER) & PO_ACT_REFUSED) kind_refused(prev);
            return false;
        }
        case MI_CONSUMED: return false;
    }
    return true;
}

static void screen_open(bool from_debug, uint32_t now) {
    if (menu_kind == MK_MENU) {
        if (from_debug) menu_return_from_diag(po_debug_page() == DEBUG_PAGE_CAMERA ? 1 : 0, now);
        else            menu_open(now);
    } else {
        if (from_debug) fx_return_from_diag();
        else            fx_open();
    }
    mi_note_input(now);   /* the idle timeout starts at the opening */
}

void menu_ops_apply(uint16_t acts, bool loss_dropped, uint32_t now) {
    if (acts & PO_ACT_REFUSED) return;
    if (loss_dropped && mi_outstanding() == MI_KIND_LOSS) mi_finish(MI_ST_SUPERSEDED);
    if (acts & PO_ACT_INPUT_CLEAR) mi_clear();
    if (acts & PO_ACT_MENU_CLEAR) screen_open(po_return_to_diag(), now);
}

void menu_screen_invalidate(void) {
    if (menu_kind == MK_MENU) menu_invalidate();
    else fx_invalidate();
}

void menu_note_loss(void) {
    if (losses_seen < 0xFFu) losses_seen++;
}

void menu_pump(void) {
    if (ms_step() == MS_STEP_COMMIT && (mb_busy() || mb_lost_pending()))
        po_note_fail(PO_FAIL_EARLY_COMMIT);   /* the sticky check, from fresh reads */
}

/* op 27's mutations that end on a later pass. */
static void op27_poll(void) {
    switch (mi_outstanding()) {
        case MI_KIND_PATTERN_STOP:
            if (!OWNER_MENU_OPEN() || ms_stop_cancelled_take()) mi_finish(MI_ST_SUPERSEDED);
            else if (ms_stopped()) mi_finish(MI_ST_DONE);
            break;
        case MI_KIND_LOSS:
            if (!mb_lost_pending()) mi_finish(MI_ST_DONE);   /* the owner's handler took it */
            break;
        default:
            break;
    }
}

/* Each pass, in housekeeping_task_kb, before display_blit_pump(), independent of
 * the gated display housekeeping (C2). */
void menu_ops_task(uint32_t now) {
    (void)ms_ops();   /* a DMA op that ended is settled within a pass, under any owner (C10) */
    if (env_wake_notice_take()) (void)env_po_event(PO_EV_WAKE_RELEASED);
    mi_tick(now);
    op27_poll();
    if (po_session() == SESSION_NONE) menu_kind = MK_NONE;   /* closed, or a PENDING cancelled */
    if (po_session() != SESSION_OPEN) return;

    int8_t     turns;
    mi_latch_t latch;
    if (mi_take(&turns, &latch)) {
        if (latch == MI_LATCH_CLOSE) {
            (void)env_po_event(PO_EV_CLOSE);
            return;
        }
        if (po_owner() == PANEL_DEBUG) {
            /* Diagnostics: a click or a hold goes back to the menu. */
            if (latch != MI_LATCH_NONE) (void)env_po_event(PO_EV_DEBUG_CLICK);
            return;
        }
        if (menu_kind == MK_MENU) {
            switch (menu_input(turns, latch, now)) {
                case MENU_DIAG_HEALTH: (void)env_po_event(PO_EV_DIAG_HEALTH); return;
                case MENU_DIAG_CAMERA: (void)env_po_event(PO_EV_DIAG_CAMERA); return;
                case MENU_CLOSE:       (void)env_po_event(PO_EV_CLOSE); return;
                case MENU_NONE:        break;
            }
        } else {
            switch (fx_input(turns, latch)) {
                case FX_DIAG:  (void)env_po_event(PO_EV_DIAG_HEALTH); return;
                case FX_CLOSE: (void)env_po_event(PO_EV_CLOSE); return;
                case FX_NONE:  break;
            }
        }
    }
    /* The menu's latched model (Fn+B, Fn+C) and its title (the flash, the
     * hint), each pass while it shows. */
    if (menu_kind == MK_MENU && po_owner() == PANEL_MENU) menu_tick(now, now - mi_last_input());
    /* 24 s with no knob input closes it (C7: hint repaints, host traffic and
     * typing don't restart this). The fixture has no edit, so no backstop. */
    /* Signed: an input stamped a little after `now` (an event read its own
     * timer) is no idle at all, never a wrapped 49 days. */
    if ((int32_t)(now - mi_last_input()) >= (int32_t)MI_IDLE_MS) (void)env_po_event(PO_EV_CLOSE);
}

/* --- Op 27 ------------------------------------------------------------------ */
#if defined(AK820_DEV_HOOKS) || defined(MENU_HOST_TEST)

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* MB_TICK_HZ ticks to whole us or ms, saturating at a u16. */
static uint16_t ticks_to_us16(uint32_t t) {
    uint64_t us = (uint64_t)t * 1000000u / MB_TICK_HZ;
    return us > 0xFFFFu ? 0xFFFFu : (uint16_t)us;
}
static uint16_t ticks_to_ms16(uint32_t t) {
    uint64_t ms = (uint64_t)t * 1000u / MB_TICK_HZ;
    return ms > 0xFFFFu ? 0xFFFFu : (uint16_t)ms;
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
    if (report_kind == MK_MENU) {   /* 16 + the node showing; the cursor, a topic's top, the confirm's page */
        d[14] = OWNER_MENU_OPEN() ? menu_screen_id() : 0;
        d[15] = po_owner() == PANEL_MENU ? menu_cursor() : 0xFFu;
    } else {
        d[14] = OWNER_MENU_OPEN() ? fx_screen() : 0;
        d[15] = fx_cursor();
    }
    uint8_t f = 0;
    if (armed) f |= 1u << 0;
    /* bit 1, an edit open: none in S0 */
    if (po_owner() == PANEL_MENU && ms_settled()) f |= 1u << 2;   /* pixels_done: the fixture's only */
    if (mb_busy()) f |= 1u << 3;
    if (po_owner() == PANEL_MENU && ms_clearing()) f |= 1u << 4;
    if (env_wake_held()) f |= 1u << 5;
    if (env_batt_dirty()) f |= 1u << 6;
    if (env_logo_stale()) f |= 1u << 7;
    d[16] = f;
    d[17] = env_queue_depths();
    d[18] = (uint8_t)mi_turns();
    d[19] = (uint8_t)mi_latch();
    d[20] = mi_dropped();
    d[21] = po_owner() == PANEL_MENU ? ms_unsettled() : 0;
    d[22] = losses_seen;
    d[23] = mi_capture_bits();
    put16(&d[24], po_invariant_fail());
    d[29] = ms_ops();   /* settles an ended DMA first, so its latency is in this reply */
    put16(&d[26], ticks_to_us16(ms_last_op_ticks()));   /* the last ended op's latency (C10) */
    /* 28: the pairing phase (P3) */
    d[30] = ms_stopped() ? 1u : 0u;
    d[31] = env_debug_done() ? 1u : 0u;
}

/* Sub 10 (C10): timing and targets, read at once. arm 1 starts an interval
 * after the reply: the maxima and counts zeroed, a new session token. */
static void op27_meas(uint8_t *d) {
    bool arm = d[5] != 0;
    env_meas_t m;
    env_meas(&m);
    uint32_t band = ms_band_max_ticks() > m.band_max_ticks ? ms_band_max_ticks() : m.band_max_ticks;
    memset(&d[5], 0, 27);
    d[5] = MI_OK;
    put16(&d[6], ticks_to_us16(ms_piece_max_ticks()));
    put16(&d[8], ticks_to_us16(ms_op_max_ticks()));
    put16(&d[10], ticks_to_us16(m.pass_max_ticks));
    put16(&d[12], m.pass_over15);
    put16(&d[14], ticks_to_ms16(ms_repaint_max_ticks()));
    d[16] = ms_tgen();
    d[17] = ms_settled_count();
    d[18] = ms_superseded_count();
    uint16_t bms = ticks_to_ms16(band);
    d[19] = bms > 0xFFu ? 0xFFu : (uint8_t)bms;
    d[20] = m.page;
    d[21] = m.dbg_gen;
    d[22] = m.dbg_done_gen;
    d[23] = ms_committed_count();
    put16(&d[24], m.passes);
    put16(&d[26], m.token);
    d[28] = m.health_gen;
    d[29] = m.dbg_sec;
    d[30] = m.rtc_sec;
    d[31] = m.aliased ? 1u : 0u;   /* the interval's ticks are ambiguous: the gate fails it */
    if (arm) {   /* the reply above is the old interval's; the next sub 10 reads the new token */
        ms_meas_reset();
        (void)env_meas_arm();
    }
}

/* A mutation the state can't take answers 2, one while another is
 * outstanding 1; neither touches the outstanding one. */
bool menu_ops_op27(uint8_t *d, uint32_t now) {
    const uint8_t sub = d[4];
    uint8_t       st = MI_OK, seq = 0;

    if (sub == 5) {
        op27_read(d);
        return true;
    }
    if (sub == 10) {
        if (d[5] > 1) {
            memset(&d[5], 0, 27);
            d[5] = MI_BAD;
            return true;
        }
        op27_meas(d);
        return true;
    }
    if (sub == 12) {   /* a menu text piece's time, in us: [6] window, [8] expansion, [10] sends, [12] the whole, [14] composition; arg 1 resets */
        bool     reset = d[5] == 1;
        uint16_t t[4];
        env_mono_timing(t, reset);
        uint32_t comp = menu_compose_max_ticks(reset);
        memset(&d[5], 0, 27);
        d[5] = MI_OK;
        for (uint8_t i = 0; i < 4; i++) put16(&d[6 + 2 * i], ticks_to_us16(t[i]));
        put16(&d[14], ticks_to_us16(comp));
        return true;
    }
    if (sub == 13) {   /* a menu colour, live: [5] the palette index, [6..7] RGB565; the menu repaints */
        bool ok = menu_set_color(d[5], (uint16_t)(d[6] | (d[7] << 8)));
        if (ok && OWNER_MENU_OPEN() && menu_kind == MK_MENU) {
            menu_invalidate();
            ms_retarget();
        }
        memset(&d[5], 0, 27);
        d[5] = ok ? MI_OK : MI_BAD;
        return true;
    }
    switch (sub) {
        case 0: {   /* the fixture's paint */
            uint8_t p = d[5], n = d[6];
            bool    fx   = OWNER_MENU_OPEN() && menu_kind == MK_FIXTURE;   /* the fixture's only (P1a) */
            bool    f1   = fx && fx_screen() == FX_SCREEN_F1;
            bool    mono = p >= FX_MONO_FIRST && p < FX_MONO_FIRST + FX_MONO_STYLES;   /* S1: F3 */
            if (!fx || (p > 3 && !mono)) { st = MI_BAD; break; }
            if (p != 2 && n != 0) { st = MI_BAD; break; }
            if ((p == 0 || p == 1) && !f1) { st = MI_BAD; break; }
            if (p == 2 && (!f1 || n < 1 || n >= FX_F1_OPS || fx_pattern() != 0 || !ms_settled())) { st = MI_BAD; break; }
            st = mi_begin(p == 2 ? MI_KIND_PATTERN_STOP : MI_KIND_PATTERN, &seq);
            if (st != MI_OK) break;
            ms_stop_release();   /* a reached stop holds until the next sub 0 */
            if (p == 3) fx_other_layout();
            else if (mono) fx_set_mono((uint8_t)(p - FX_MONO_FIRST));
            else fx_set_pattern(p == 0 ? 0 : 1);
            if (p == 2) ms_stop_after(n);
            else mi_finish(MI_ST_DONE);   /* the model has its new target */
            break;
        }
        case 1: {   /* turns */
            int8_t n = (int8_t)d[5];
            if (po_session() != SESSION_OPEN) { st = MI_BAD; break; }
            if (n == 0 || n > 16 || n < -16) { st = MI_BAD; break; }
            st = mi_begin(MI_KIND_TURNS, &seq);
            if (st != MI_OK) break;
            mi_syn_turns(n);
            mi_note_input(now);
            break;
        }
        case 2:     /* a click */
            if (po_session() != SESSION_OPEN) { st = MI_BAD; break; }
            st = mi_begin(MI_KIND_CLICK, &seq);
            if (st != MI_OK) break;
            mi_syn_click();
            mi_note_input(now);
            break;
        case 3: {   /* a hold */
            uint16_t ms = (uint16_t)(d[5] | (d[6] << 8));
            if (!ms) ms = 650;
            if (po_session() != SESSION_OPEN || ms < 50 || ms > 5000) { st = MI_BAD; break; }
            if (mi_knob_down() || mi_syn_hold_active()) { st = MI_BUSY; break; }
            st = mi_begin(MI_KIND_HOLD, &seq);
            if (st != MI_OK) break;
            mi_syn_hold(ms, now);
            mi_note_input(now);
            break;
        }
        case 4:     /* Fn plus a click: the opener, or the closer -- the fixture */
        case 11:    /* the same, the menu (P1a): sub 4 exactly but the kind */
            st = mi_begin(MI_KIND_OPENER, &seq);
            if (st != MI_OK) break;
        {
            uint8_t  prev = kind_for_new_session(sub == 4 ? MK_FIXTURE : MK_MENU);
            uint16_t a    = env_po_event(po_session() == SESSION_OPEN ? PO_EV_CLOSE : PO_EV_OPENER);
            if (a & PO_ACT_REFUSED) kind_refused(prev);
            mi_finish(a & PO_ACT_REFUSED ? MI_ST_REFUSED : MI_ST_DONE);
            break;
        }
        case 6:     /* arm or disarm */
            if (d[5] > 1) { st = MI_BAD; break; }
            st = mi_begin(MI_KIND_ARM, &seq);
            if (st != MI_OK) break;
            armed = d[5] != 0;
            /* Disarming closes a session; the capture bits still owed are kept. */
            if (!armed && po_session() != SESSION_NONE) (void)env_po_event(PO_EV_CLOSE);
            mi_finish(MI_ST_DONE);
            break;
        case 7:     /* a restore request, as the startup callback makes */
            st = mi_begin(MI_KIND_RESTORE, &seq);
            if (st != MI_OK) break;
            (void)env_po_event(PO_EV_STARTUP);
            mi_finish(MI_ST_DONE);   /* installed: watch the phase and generation */
            break;
        case 8:     /* a loss, as an exhausted recovery marks one */
            st = mi_begin(MI_KIND_LOSS, &seq);
            if (st != MI_OK) break;
            env_lost_inject();
            break;
        case 9: {   /* the handoff probe (the board's A3): bytes 7-10 */
            uint8_t q[4] = {0};
            st = mi_begin(MI_KIND_PROBE, &seq);
            if (st != MI_OK) break;
            mi_finish(env_probe_handoff(q) ? MI_ST_DONE : MI_ST_REFUSED);
            memset(&d[5], 0, 27);
            d[5] = st;
            d[6] = seq;
            memcpy(&d[7], q, 4);
            return true;
        }
        default:
            return false;
    }
    memset(&d[5], 0, 27);
    d[5] = st;
    d[6] = st == MI_OK ? seq : 0;
    return true;
}
#endif
