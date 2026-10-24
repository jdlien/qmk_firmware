// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The menu's damage model (plans/CONFIG-MENU-BUILD-PLAN.md, C3): fixed slots
// per layout, a banded clear on a layout change, one bounded operation a pass.
// Pure logic over menu_backend.h.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MS_MAX_SLOTS 24

typedef enum { MS_EMPTY = 0, MS_INVALID, MS_PARTIAL, MS_COMMITTED } ms_state_t;

typedef struct {
    uint16_t sig;     /* COMMITTED: what's on the panel; PARTIAL: what's being painted */
    uint8_t  state;
    uint8_t  gen;
} ms_slot_t;
_Static_assert(sizeof(ms_slot_t) == 4, "C3: 4 B a slot");

/* A piece's outcome. */
typedef enum {
    MS_PAINT_BUSY = 0,   /* the bus was busy: nothing sent, the same piece next pass */
    MS_PAINT_DONE,       /* sent (a glyph: its DMA armed) */
    MS_PAINT_FAILED,     /* given up, a loss latched: the op ends here */
} ms_paint_t;

/* A layout: its slots' count, and for each its current target's signature,
 * its chunk count, a chunk's piece count, and one piece's paint. A chunk is
 * a logical op (C9 counts them); its pieces together paint the chunk's whole
 * share of the slot's footprint, background included, and each piece sends at
 * most MB_PASS_BYTES of CPU-fed pixels (C4). One piece is sent a pass. */
typedef struct {
    uint8_t n;
    uint16_t (*sig)(uint8_t slot);
    uint8_t (*chunks)(uint8_t slot);
    uint8_t (*pieces)(uint8_t slot, uint8_t chunk);
    ms_paint_t (*paint)(uint8_t slot, uint8_t chunk, uint8_t piece);
} ms_layout_t;

typedef enum {
    MS_STEP_IDLE = 0,   /* everything settled */
    MS_STEP_CLEAR,      /* a band of the clear (or its retry) */
    MS_STEP_PAINT,      /* a piece of a chunk */
    MS_STEP_BUSY,       /* the bus was busy: the same piece next pass */
    MS_STEP_WAIT,       /* a slot's last transfer, or a loss, still to resolve */
    MS_STEP_COMMIT,     /* a slot committed */
    MS_STEP_STOPPED,    /* op 27's partial stop */
} ms_step_t;

void      ms_init(void);
void      ms_set_layout(const ms_layout_t *l, bool fresh);   /* a layout change: every slot INVALID, and a
                                                              * banded clear (a running one finishes, unless fresh) */
void      ms_retarget(void);                     /* same layout, new targets: rule 2 restarts what changed */
void      ms_invalidate_all(void);               /* a loss under this owner */
ms_step_t ms_step(void);

bool      ms_clearing(void);
bool      ms_settled(void);                      /* pixels_done, but for the owner */
uint8_t   ms_unsettled(void);                    /* op 27 sub 5, byte 21 */
uint8_t   ms_ops(void);                          /* completed paint operations since the last target */
void      ms_stop_after(uint8_t n);              /* 0: no stop */
void      ms_stop_release(void);                 /* the next op 27 sub 0: a reached stop lets go */
bool      ms_stopped(void);
bool      ms_stop_cancelled_take(void);          /* a pending stop was disposed of by a new target */
const ms_slot_t *ms_slot(uint8_t i);

/* C10's measures (op 27 sub 5's bytes 26-27, sub 10). Ticks are MB_TICK_HZ. */
uint32_t ms_last_op_ticks(void);    /* the last ENDED op's latency: its first piece's start to its end */
uint32_t ms_op_max_ticks(void);     /* the largest, since ms_meas_reset() */
uint32_t ms_piece_max_ticks(void);  /* the largest single piece's paint call */
uint32_t ms_band_max_ticks(void);   /* the largest layout clear band */
uint32_t ms_repaint_max_ticks(void);   /* a target's creation to its settling */
uint8_t  ms_tgen(void);
uint8_t  ms_settled_count(void);    /* wrapping: a target settled for the first time */
uint8_t  ms_superseded_count(void); /* wrapping: a target replaced before it settled */
uint8_t  ms_committed_count(void);  /* wrapping: slots committed */
void     ms_meas_reset(void);       /* the maxima only (sub 10's arm) */
