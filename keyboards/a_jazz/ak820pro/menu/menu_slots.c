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
static uint8_t   chunk;           /* its next chunk */
static uint8_t   clear_band;      /* 1..MB_BANDS while clearing */
static uint8_t   ops;
static uint8_t   stop_n;
static bool      stopped;

void ms_init(void) {
    layout = 0;
    for (uint8_t i = 0; i < MS_MAX_SLOTS; i++) slots[i] = (ms_slot_t){0, MS_EMPTY, 0};
    tgen = 0;
    cursor = NO_CURSOR;
    chunk = 0;
    clear_band = 0;
    ops = 0;
    stop_n = 0;
    stopped = false;
}

static void new_target(void) {
    tgen++;
    ops     = 0;
    stop_n  = 0;
    stopped = false;
}

void ms_set_layout(const ms_layout_t *l) {
    layout     = l;
    clear_band = 1;   /* never superseded: a later target change only retargets */
    for (uint8_t i = 0; i < MS_MAX_SLOTS; i++)
        slots[i].state = (l && i < l->n) ? MS_INVALID : MS_EMPTY;
    cursor = NO_CURSOR;
    new_target();
}

void ms_retarget(void) { new_target(); }

void ms_invalidate_all(void) {
    if (!layout) return;
    for (uint8_t i = 0; i < layout->n; i++) slots[i].state = MS_INVALID;
    cursor = NO_CURSOR;
}

static bool settled(uint8_t i) {
    return slots[i].state == MS_COMMITTED && slots[i].sig == layout->sig(i);
}

static ms_step_t paint_chunk(void) {
    if (!layout->paint(cursor, chunk)) return MS_STEP_BUSY;   /* the same chunk next pass */
    chunk++;
    if (ops < 0xFFu) ops++;
    return MS_STEP_PAINT;
}

ms_step_t ms_step(void) {
    if (!layout) return MS_STEP_IDLE;

    /* A clear completes only when every band succeeded: a failed band is
     * retried, the cursor not advanced. */
    if (clear_band) {
        if (mb_clear_band((uint8_t)(clear_band - 1u))) {
            if (++clear_band > MB_BANDS) clear_band = 0;
        }
        return MS_STEP_CLEAR;
    }

    if (stopped) return MS_STEP_STOPPED;
    if (stop_n && ops >= stop_n) {
        /* After operation n's transfer has completed or failed. */
        if (mb_busy()) return MS_STEP_WAIT;
        stopped = true;
        return MS_STEP_STOPPED;
    }

    if (cursor != NO_CURSOR) {
        ms_slot_t *s = &slots[cursor];
        if (s->state == MS_PARTIAL && s->sig == layout->sig(cursor) && s->gen == tgen) {
            if (chunk < layout->chunks(cursor)) return paint_chunk();
            /* Every chunk sent: committed only with the last transfer done and
             * no loss waiting to be taken. */
            if (mb_busy() || mb_lost_pending()) return MS_STEP_WAIT;
            s->state = MS_COMMITTED;
            cursor   = NO_CURSOR;
            return MS_STEP_COMMIT;
        }
        /* Its target changed, or a loss invalidated it: it restarts from chunk
         * 0 when it's next chosen. */
        if (s->state == MS_PARTIAL) s->state = MS_INVALID;
        cursor = NO_CURSOR;
    }

    for (uint8_t i = 0; i < layout->n; i++) {
        if (settled(i)) continue;
        slots[i].state = MS_PARTIAL;
        slots[i].sig   = layout->sig(i);
        slots[i].gen   = tgen;
        cursor         = i;
        chunk          = 0;
        return paint_chunk();
    }
    return MS_STEP_IDLE;
}

bool ms_clearing(void) { return clear_band != 0; }

bool ms_settled(void) {
    if (!layout || clear_band) return false;
    for (uint8_t i = 0; i < layout->n; i++)
        if (!settled(i)) return false;
    return !mb_busy();
}

uint8_t ms_unsettled(void) {
    if (!layout) return 0;
    uint8_t n = 0;
    for (uint8_t i = 0; i < layout->n; i++)
        if (!settled(i)) n++;
    return n;
}

uint8_t ms_ops(void) { return ops; }
void    ms_stop_after(uint8_t n) { stop_n = n; stopped = false; }
bool    ms_stopped(void) { return stopped; }
const ms_slot_t *ms_slot(uint8_t i) { return i < MS_MAX_SLOTS ? &slots[i] : 0; }
