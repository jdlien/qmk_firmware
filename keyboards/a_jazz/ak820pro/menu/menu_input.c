// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
/* The knob, captured for the menu: C2 of plans/CONFIG-MENU-BUILD-PLAN.md.
 * The idea of a menu on the knob, with every other key left alone and a held
 * key never stuck, is quill4gen7's (menu.c on the quill-personal branch of
 * github.com/quill4gen7/qmk_firmware); this is a separate implementation.
 *
 * Capture is by physical control and never depends on room: a press that is
 * captured has its release consumed, whatever the session is by then, and a
 * press that was forwarded has its release forwarded. The input is coalesced
 * into a turn count and one latch, because a physical event can't be told
 * "busy". */
#include "menu_input.h"

static uint8_t    capture;        /* bit per mi_control_t: its press was captured */
static bool       knob_down;      /* the physical knob, whoever has it */
static bool       gesture;        /* a session's knob press, timing a click or a hold */
static bool       hold_fired;     /* its Back has been latched */
static uint32_t   hold_start;
static int8_t     turns;
static mi_latch_t latch;
static uint8_t    dropped;
static uint32_t   last_input;

static bool       syn_hold;       /* a synthetic hold in progress */
static bool       syn_fired;
static uint32_t   syn_start;
static uint16_t   syn_ms;
static bool       owed;           /* the outstanding synthetic action is in the input */
static mi_latch_t syn_latch;      /* the latch it set (none for turns) */
static bool       syn_lost;       /* its effect was discarded: a higher latch, or a close */
/* Its Back (at 600 ms) was taken by the screen before the release. The
 * fixture's Back closed the session, which cancelled the hold first; the
 * menu's goes up a level and the session stays OPEN, so at the release the
 * latch is empty because it was delivered, not because it was lost (P1a). */
static bool       syn_back_taken;

static mi_kind_t  out_kind;
static uint8_t    last_seq;
static mi_state_t last_state;

void mi_init(void) {
    capture = 0;
    knob_down = gesture = hold_fired = false;
    hold_start = 0;
    turns = 0;
    latch = MI_LATCH_NONE;
    dropped = 0;
    last_input = 0;
    syn_hold = syn_fired = owed = syn_lost = syn_back_taken = false;
    syn_latch = MI_LATCH_NONE;
    syn_start = 0;
    syn_ms = 0;
    out_kind = MI_KIND_NONE;
    last_seq = 0;
    last_state = MI_ST_NONE;
}

static void drop(void) {
    if (dropped < 0xFFu) dropped++;
}

/* Close outranks Back, which outranks a click; a lower one after a higher one
 * is dropped. A synthetic action whose latch is replaced, or whose turns a
 * close will discard, ends superseded when the batch is taken. */
static void set_latch(mi_latch_t l) {
    if (l > latch) {
        if (owed && (syn_latch != MI_LATCH_NONE || l == MI_LATCH_CLOSE)) syn_lost = true;
        latch = l;
    } else {
        drop();
    }
}

/* A latch freezes the turns before it: later ones are dropped and counted. */
static void add_turns(int8_t n) {
    if (latch != MI_LATCH_NONE) {
        drop();
        return;
    }
    int16_t t = (int16_t)(turns + n);
    if (t > MI_TURNS_MAX) { t = MI_TURNS_MAX; drop(); }
    if (t < -MI_TURNS_MAX) { t = -MI_TURNS_MAX; drop(); }
    turns = (int8_t)t;
}

void mi_note_input(uint32_t now) { last_input = now; }
uint32_t mi_last_input(void) { return last_input; }

static void syn_cancel(void) {
    if (!syn_hold) return;
    syn_hold = syn_fired = syn_back_taken = false;
    if (out_kind == MI_KIND_HOLD) mi_finish(MI_ST_CANCELLED);
}

mi_verdict_t mi_physical(mi_control_t c, bool pressed, bool fn, bool enabled,
                         menu_session_t session, uint32_t now) {
    const uint8_t bit = (uint8_t)(1u << c);

    /* Releases first, before any check of the session or of `enabled`. */
    if (!pressed) {
        if (c == MI_KNOB) knob_down = false;
        if (!(capture & bit)) return MI_FORWARD;
        capture &= (uint8_t)~bit;
        if (c == MI_KNOB && gesture) {
            gesture = false;
            if (!hold_fired) set_latch((uint32_t)(now - hold_start) < MI_HOLD_MS ? MI_LATCH_CLICK : MI_LATCH_BACK);
            hold_fired = false;
            last_input = now;
        }
        return MI_CONSUMED;
    }

    if (c == MI_KNOB) {
        knob_down = true;
        syn_cancel();   /* a physical press cancels a synthetic hold: no Back */
    }

    switch (session) {
        case SESSION_NONE:
            if (c == MI_KNOB && enabled && fn) {
                capture |= bit;   /* refused under the animation, but still ours */
                return MI_OPENER;
            }
            return MI_FORWARD;

        case SESSION_PENDING:
            capture |= bit;
            if (c == MI_KNOB && fn) return MI_OPENER;   /* a second opener cancels */
            return MI_CONSUMED;                         /* nothing buffered for a screen not open */

        case SESSION_OPEN:
            capture |= bit;
            last_input = now;
            if (c == MI_KNOB) {
                if (fn) set_latch(MI_LATCH_CLOSE);   /* not a click: its release is just consumed */
                else {
                    gesture    = true;
                    hold_fired = false;
                    hold_start = now;
                }
            } else {
                add_turns(c == MI_CW ? 1 : -1);
            }
            return MI_CONSUMED;
    }
    return MI_FORWARD;
}

void mi_tick(uint32_t now) {
    if (gesture && !hold_fired && (uint32_t)(now - hold_start) >= MI_HOLD_MS) {
        hold_fired = true;
        set_latch(MI_LATCH_BACK);
    }
    if (syn_hold) {
        uint32_t held = (uint32_t)(now - syn_start);
        if (!syn_fired && held >= MI_HOLD_MS) {
            syn_fired = true;
            set_latch(MI_LATCH_BACK);
        }
        if (held >= syn_ms) {   /* the scheduled release */
            mi_latch_t l         = syn_fired ? MI_LATCH_BACK : MI_LATCH_CLICK;
            bool       delivered = syn_fired && syn_back_taken;
            if (!syn_fired) set_latch(MI_LATCH_CLICK);
            syn_hold = syn_fired = syn_back_taken = false;
            owed      = true;   /* ends once the screen has taken the batch */
            syn_latch = l;
            syn_lost  = !delivered && latch != l;
            last_input = now;
        }
    }
}

/* True when there's a batch to act on: turns, a latch, or an outstanding
 * synthetic action -- even one whose turns a physical turn cancelled to a net
 * zero, which still ends here. */
bool mi_take(int8_t *t, mi_latch_t *l) {
    if (!turns && latch == MI_LATCH_NONE && !owed) return false;
    *l = latch;
    *t = latch == MI_LATCH_CLOSE ? 0 : turns;   /* close discards the turns */
    if (syn_hold && syn_fired && latch == MI_LATCH_BACK) syn_back_taken = true;
    turns = 0;
    latch = MI_LATCH_NONE;
    if (owed) {
        bool lost = syn_lost;
        owed      = false;
        syn_lost  = false;
        syn_latch = MI_LATCH_NONE;
        if (out_kind == MI_KIND_TURNS || out_kind == MI_KIND_CLICK || out_kind == MI_KIND_HOLD)
            mi_finish(lost ? MI_ST_SUPERSEDED : MI_ST_DONE);
    }
    return true;
}

void mi_session_closed(void) {
    gesture = hold_fired = false;
    syn_cancel();
}

void mi_clear(void) {
    turns = 0;
    latch = MI_LATCH_NONE;
    mi_session_closed();
    if (owed) {
        owed      = false;
        syn_lost  = false;
        syn_latch = MI_LATCH_NONE;
        if (out_kind == MI_KIND_TURNS || out_kind == MI_KIND_CLICK || out_kind == MI_KIND_HOLD)
            mi_finish(MI_ST_CANCELLED);
    }
}

int8_t     mi_turns(void) { return turns; }
mi_latch_t mi_latch(void) { return latch; }
uint8_t    mi_dropped(void) { return dropped; }
uint8_t    mi_capture_bits(void) { return capture; }
bool       mi_knob_down(void) { return knob_down; }

/* --- op 27 ------------------------------------------------------------------ */

uint8_t mi_begin(mi_kind_t kind, uint8_t *seq) {
    if (out_kind != MI_KIND_NONE) return MI_BUSY;
    last_seq = (uint8_t)(last_seq + 1u);
    if (!last_seq) last_seq = 1;   /* never 0 */
    out_kind   = kind;
    last_state = MI_ST_PENDING;
    *seq       = last_seq;
    return MI_OK;
}

void mi_finish(mi_state_t st) {
    if (out_kind == MI_KIND_NONE) return;
    out_kind   = MI_KIND_NONE;
    last_state = st;
    owed       = false;
    syn_lost   = false;
    syn_latch  = MI_LATCH_NONE;
}

mi_kind_t  mi_outstanding(void) { return out_kind; }
uint8_t    mi_last_seq(void) { return last_seq; }
mi_state_t mi_last_state(void) { return last_state; }

uint8_t mi_syn_turns(int8_t n) {
    if (latch != MI_LATCH_NONE) {   /* frozen behind a latch: dropped */
        drop();
        mi_finish(MI_ST_SUPERSEDED);
        return MI_OK;
    }
    add_turns(n);
    owed      = true;
    syn_latch = MI_LATCH_NONE;
    syn_lost  = false;
    return MI_OK;
}

uint8_t mi_syn_click(void) {
    set_latch(MI_LATCH_CLICK);
    if (latch != MI_LATCH_CLICK) {   /* a higher latch was already set */
        mi_finish(MI_ST_SUPERSEDED);
        return MI_OK;
    }
    owed      = true;
    syn_latch = MI_LATCH_CLICK;
    syn_lost  = false;
    return MI_OK;
}

uint8_t mi_syn_hold(uint16_t ms, uint32_t now) {
    if (knob_down || syn_hold) return MI_BUSY;
    syn_hold  = true;
    syn_fired = syn_back_taken = false;
    syn_start = now;
    syn_ms    = ms;
    return MI_OK;
}

bool mi_syn_hold_active(void) { return syn_hold; }
