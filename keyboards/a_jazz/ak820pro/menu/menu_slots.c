// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
/* C3 of plans/CONFIG-MENU-BUILD-PLAN.md. Nothing moves within a layout, so
 * no old rectangle is ever remembered: a layout change clears the panel in
 * bands, and within a layout every slot repaints its whole footprint. */
#include "menu_slots.h"
#include "menu_backend.h"

#define NO_CURSOR 0xFFu

static const ms_layout_t *layout;
static ms_slot_t slots[MS_MAX_SLOTS];
static uint8_t   tgen;            /* the target's generation */
static uint8_t   cursor;          /* the one slot that may be PARTIAL */
static uint8_t   chunk;           /* its current chunk (a logical op) */
static uint8_t   piece;           /* the chunk's next piece */
static uint8_t   clear_band;      /* 1..MB_BANDS while clearing */
static uint8_t   ops;             /* completed paint operations since the last target */
static bool      ops_inflight;    /* the last operation's DMA, counted when it ends */
static uint8_t   stop_n;          /* op 27's partial stop: pending while > 0 and not stopped */
static bool      stopped;
static bool      stop_cancelled;  /* a pending stop disposed of by a retarget or layout change */

/* C10's measures. */
static bool      op_started;      /* the current op's first piece has been tried */
static bool      op_untimed;      /* ...and a retarget cut it: it counts when it ends, untimed (revision 13) */
static uint32_t  op_t0;
static uint32_t  dma_piece_t0;    /* an in-flight glyph's piece: its start */
static bool      dma_untimed;     /* ...and a retarget came while it flew */
static uint32_t  last_op_ticks, op_max_ticks, piece_max_ticks, band_max_ticks, repaint_max_ticks;
static bool      target_live;     /* a target exists to settle or be superseded */
static bool      target_settled;  /* ...and it has settled */
static uint32_t  target_t0;
static uint8_t   settled_count, superseded_count, committed_count;

void ms_init(void) {
    layout = 0;
    for (uint8_t i = 0; i < MS_MAX_SLOTS; i++) slots[i] = (ms_slot_t){0, MS_EMPTY, 0};
    tgen = 0;
    cursor = NO_CURSOR;
    chunk = piece = 0;
    clear_band = 0;
    ops = 0;
    ops_inflight = false;
    stop_n = 0;
    stopped = stop_cancelled = false;
    op_started = op_untimed = dma_untimed = target_live = target_settled = false;
    op_t0 = target_t0 = 0;
    last_op_ticks = 0;
    settled_count = superseded_count = committed_count = 0;
    ms_meas_reset();
}

void ms_meas_reset(void) {
    op_max_ticks = piece_max_ticks = band_max_ticks = repaint_max_ticks = 0;
}

/* An op's end: counted once, and a stop it reaches latched in the same step
 * (C4, revision 9). Before, a synchronous op counted at once but its stop
 * latched only on the next ms_step(), so navigation in between could cancel
 * a stop that had been reached. */
static void end_op_at(uint32_t end) {
    if (ops < 0xFFu) ops++;
    if (stop_n && ops >= stop_n && !mb_busy()) stopped = true;
    if (op_started) {
        op_started = false;
        if (!op_untimed) {
            last_op_ticks = end - op_t0;
            if (last_op_ticks > op_max_ticks) op_max_ticks = last_op_ticks;
        }
        op_untimed = false;
    }
}
static void end_op(void) { end_op_at(mb_now_ticks()); }

/* An asynchronous op ends when its transfer has ended (completed or given
 * up), timed at the bus endpoint, not when this notices; a synchronous one
 * when its last piece returns. */
static void settle_ops(void) {
    if (ops_inflight && !mb_busy()) {
        ops_inflight = false;
        uint32_t end = mb_dma_end_ticks();
        if (!dma_untimed && end - dma_piece_t0 > piece_max_ticks) piece_max_ticks = end - dma_piece_t0;
        dma_untimed = false;
        end_op_at(end);
    }
}

/* A pending stop's work is disposed of by a new target: its sequence ends
 * (ms_stop_cancelled_take). A stop already reached holds until
 * ms_stop_release(), the next op 27 sub 0. The op that just ended is settled
 * first, so a stop it reached is latched, not cancelled. */
static void new_target(void) {
    settle_ops();
    if (target_live && !target_settled) superseded_count++;
    target_live    = true;
    target_settled = false;
    target_t0      = mb_now_ticks();
    /* An op in progress may resume (its slot's signature unchanged, revision
     * 13): it still counts once when it ends, in the new target, but isn't
     * timed -- the part before the retarget isn't this target's. A glyph
     * still flying is kept, so its end is counted. If its slot restarts
     * instead, the abandoned op never ends (ms_step). */
    if (op_started) op_untimed = true;
    if (ops_inflight) dma_untimed = op_untimed = true;
    tgen++;
    ops = 0;
    if (stop_n && !stopped) {
        stop_n         = 0;
        stop_cancelled = true;
    }
}

void ms_set_layout(const ms_layout_t *l, bool fresh) {
    layout = l;
    /* A clear in progress is never restarted by navigation: it finishes its
     * remaining bands. Opening, or coming back from another owner, sweeps
     * the panel afresh. */
    if (fresh || !clear_band) clear_band = 1;
    for (uint8_t i = 0; i < MS_MAX_SLOTS; i++)
        slots[i].state = (l && i < l->n) ? MS_INVALID : MS_EMPTY;
    cursor = NO_CURSOR;
    new_target();
    /* A reached stop holds through navigation, a layout change included,
     * until the next op 27 sub 0. Opening, or coming back from another
     * owner, is a new screen: that lets it go. */
    if (fresh) {
        stopped = false;
        stop_n  = 0;
    }
}

/* A target already on the panel (the same pattern again, eight rows round)
 * settles at once, not on a later read. */
void ms_retarget(void) {
    new_target();
    (void)ms_settled();
}

void ms_invalidate_all(void) {
    settle_ops();   /* an op that ended latches its stop before the loss resets anything */
    op_started = op_untimed = false;
    if (!layout) return;
    for (uint8_t i = 0; i < layout->n; i++) slots[i].state = MS_INVALID;
    cursor = NO_CURSOR;
}

static bool settled(uint8_t i) {
    return slots[i].state == MS_COMMITTED && slots[i].sig == layout->sig(i);
}

/* C10: a target settles when pixels_done first becomes true for it, which
 * is the moment its last slot commits with no clear running. It's noted
 * there, at IDLE, and whenever ms_settled() finds it so, whichever comes
 * first. A loss's repaint of the same target doesn't settle it again. */
static void note_settled(void) {
    if (!target_live || target_settled) return;
    target_settled = true;
    settled_count++;
    uint32_t dt = mb_now_ticks() - target_t0;
    if (dt > repaint_max_ticks) repaint_max_ticks = dt;
}

/* One piece. The chunk's op ends with its last piece: at once for a fill,
 * when its DMA ends for a glyph. A failed piece ends the op there (counted,
 * its stop latched) and the slot sends nothing more: its PARTIAL state
 * waits for the owner's loss handler, which repaints (C4). */
static ms_step_t paint_piece(void) {
    uint32_t t = mb_now_ticks();
    if (!op_started) {   /* a new op; a resumed one keeps its (untimed) start */
        op_started = true;
        op_untimed = false;
        op_t0      = t;
    }
    ms_paint_t r = layout->paint(cursor, chunk, piece);
    if (r == MS_PAINT_BUSY) return MS_STEP_BUSY;   /* the same piece next pass */
    if (mb_busy()) {
        dma_piece_t0 = t;   /* a glyph: its piece ends with its DMA (settle_ops) */
    } else {
        uint32_t dt = mb_now_ticks() - t;
        if (dt > piece_max_ticks) piece_max_ticks = dt;
    }
    if (r == MS_PAINT_FAILED) {
        piece = 0;
        chunk = layout->chunks(cursor);
        end_op();
        return MS_STEP_PAINT;
    }
    if (++piece < layout->pieces(cursor, chunk)) return MS_STEP_PAINT;
    piece = 0;
    chunk++;
    if (mb_busy()) ops_inflight = true;   /* a DMA armed: the op ends when it does */
    else end_op();
    return MS_STEP_PAINT;
}

ms_step_t ms_step(void) {
    if (!layout) return MS_STEP_IDLE;
    settle_ops();

    /* A reached stop holds everything, a pending layout clear included. */
    if (stopped) return MS_STEP_STOPPED;

    /* A clear completes only when every band succeeded: a failed band is
     * retried, the cursor not advanced. */
    if (clear_band) {
        uint32_t t  = mb_now_ticks();
        bool     ok = mb_clear_band((uint8_t)(clear_band - 1u));
        uint32_t dt = mb_now_ticks() - t;
        if (dt > band_max_ticks) band_max_ticks = dt;
        if (ok && ++clear_band > MB_BANDS) clear_band = 0;
        return MS_STEP_CLEAR;
    }

    if (stop_n && ops >= stop_n) {
        /* After operation n's transfer has completed or failed. */
        if (mb_busy()) return MS_STEP_WAIT;
        stopped = true;
        return MS_STEP_STOPPED;
    }

    if (cursor != NO_CURSOR) {
        ms_slot_t *s = &slots[cursor];
        /* Resumed while its own signature is unchanged (revision 13): the
         * signature is the slot's whole target, so the pieces already sent
         * are still right. A retarget that only moved other slots no longer
         * restarts it -- with ops of several pieces that starved a knob spin
         * (the S1 gate, 10-24). A layout change or a loss invalidates every
         * slot, and those restart. */
        if (s->state == MS_PARTIAL && s->sig == layout->sig(cursor)) {
            if (chunk < layout->chunks(cursor)) return paint_piece();
            /* Every chunk sent: committed only with the last transfer done and
             * no loss waiting to be taken. */
            if (mb_busy() || mb_lost_pending()) return MS_STEP_WAIT;
            s->state = MS_COMMITTED;
            cursor   = NO_CURSOR;
            committed_count++;
            (void)ms_settled();   /* the last commit: its target settles now */
            return MS_STEP_COMMIT;
        }
        /* Its target changed, or a loss invalidated it: it restarts from its
         * first chunk and piece when it's next chosen. */
        if (s->state == MS_PARTIAL) s->state = MS_INVALID;
        op_started = op_untimed = false;   /* its op is abandoned: it never ends */
        cursor = NO_CURSOR;
    }

    for (uint8_t i = 0; i < layout->n; i++) {
        if (settled(i)) continue;
        slots[i].state = MS_PARTIAL;
        slots[i].sig   = layout->sig(i);
        slots[i].gen   = tgen;
        cursor         = i;
        chunk = piece  = 0;
        return paint_piece();
    }
    note_settled();   /* nothing left for this target */
    return MS_STEP_IDLE;
}

bool ms_clearing(void) { return clear_band != 0; }

bool ms_settled(void) {
    if (!layout || clear_band) return false;
    for (uint8_t i = 0; i < layout->n; i++)
        if (!settled(i)) return false;
    if (mb_busy()) return false;
    note_settled();
    return true;
}

uint8_t ms_unsettled(void) {
    if (!layout) return 0;
    uint8_t n = 0;
    for (uint8_t i = 0; i < layout->n; i++)
        if (!settled(i)) n++;
    return n;
}

uint8_t ms_ops(void) {
    settle_ops();
    return ops;
}
void ms_stop_after(uint8_t n) {
    stop_n         = n;
    stopped        = false;
    stop_cancelled = false;
}
void ms_stop_release(void) {
    stop_n  = 0;
    stopped = false;
}
bool ms_stopped(void) { return stopped; }
bool ms_stop_cancelled_take(void) {
    bool c         = stop_cancelled;
    stop_cancelled = false;
    return c;
}
const ms_slot_t *ms_slot(uint8_t i) { return i < MS_MAX_SLOTS ? &slots[i] : 0; }

uint32_t ms_last_op_ticks(void)     { return last_op_ticks; }
uint32_t ms_op_max_ticks(void)      { return op_max_ticks; }
uint32_t ms_piece_max_ticks(void)   { return piece_max_ticks; }
uint32_t ms_band_max_ticks(void)    { return band_max_ticks; }
uint32_t ms_repaint_max_ticks(void) { return repaint_max_ticks; }
uint8_t  ms_tgen(void)              { return tgen; }
uint8_t  ms_settled_count(void)     { return settled_count; }
uint8_t  ms_superseded_count(void)  { return superseded_count; }
uint8_t  ms_committed_count(void)   { return committed_count; }
