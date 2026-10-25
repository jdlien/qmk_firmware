// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The knob, captured for the menu (plans/CONFIG-MENU-BUILD-PLAN.md, C2), and
// op 27's synthetic input and single flight. Pure logic: the caller passes the
// event, the time and the session; the host tests run this file as it is.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "panel_owner.h"

#define MI_HOLD_MS     600u
#define MI_TURNS_MAX   64
#define MI_IDLE_MS     24000u
#define MI_BACKSTOP_MS 180000u   /* P1b: while a previewing edit is open, the 3-minute backstop instead (C8) */

/* The physical controls, by identity, never by keycode. */
typedef enum { MI_KNOB = 0, MI_CW, MI_CCW, MI_CONTROLS } mi_control_t;

/* The latch, in rising precedence (op 27 sub 5, byte 19). */
typedef enum { MI_LATCH_NONE = 0, MI_LATCH_CLICK, MI_LATCH_BACK, MI_LATCH_CLOSE } mi_latch_t;

/* What the hook should do with a physical event. */
typedef enum {
    MI_FORWARD = 0,   /* not ours: pre_process_record_user() */
    MI_CONSUMED,      /* captured, nothing else to do */
    MI_OPENER,        /* captured: call po_event(PO_EV_OPENER) */
} mi_verdict_t;

void mi_init(void);

/* A physical knob event. `fn` is WINFN or MACFN active at a press; `enabled` is
 * MENU_ENABLE or the armed test mode. */
mi_verdict_t mi_physical(mi_control_t c, bool pressed, bool fn, bool enabled,
                         menu_session_t session, uint32_t now);

/* Once a pass, before the screen acts: the hold's timer and a synthetic hold's
 * release. Then the screen takes the input. */
void       mi_tick(uint32_t now);
bool       mi_take(int8_t *turns, mi_latch_t *latch);   /* false when there's nothing */
void       mi_clear(void);          /* PO_ACT_INPUT_CLEAR: at close and every handoff */
void       mi_session_closed(void); /* the gesture and the hold timer, never the capture bits */
uint32_t   mi_last_input(void);     /* knob input only: the idle timeout's start */
void       mi_note_input(uint32_t now);

int8_t     mi_turns(void);
mi_latch_t mi_latch(void);
uint8_t    mi_dropped(void);
uint8_t    mi_capture_bits(void);
bool       mi_knob_down(void);      /* the physical knob, captured or forwarded */

/* --- op 27: synthetic input and its single flight ------------------------- */
typedef enum { MI_ST_NONE = 0, MI_ST_PENDING, MI_ST_DONE, MI_ST_CANCELLED, MI_ST_SUPERSEDED, MI_ST_REFUSED } mi_state_t;
typedef enum {
    MI_KIND_NONE = 0, MI_KIND_TURNS, MI_KIND_CLICK, MI_KIND_HOLD, MI_KIND_OPENER,
    MI_KIND_PATTERN, MI_KIND_PATTERN_STOP, MI_KIND_RESTORE, MI_KIND_LOSS, MI_KIND_ARM, MI_KIND_PROBE,
} mi_kind_t;

/* Status values (reply byte 5). */
enum { MI_OK = 0, MI_BUSY = 1, MI_BAD = 2 };

/* Begin a mutation: MI_BUSY while one is outstanding, else a fresh seq (never
 * 0) in *seq and MI_OK. A busy reply leaves the outstanding one alone. */
uint8_t    mi_begin(mi_kind_t kind, uint8_t *seq);
void       mi_finish(mi_state_t st);   /* the outstanding one ends */
mi_kind_t  mi_outstanding(void);       /* MI_KIND_NONE when nothing is */
uint8_t    mi_last_seq(void);
mi_state_t mi_last_state(void);

/* The synthetic gestures. Each is begun by the caller first. */
uint8_t    mi_syn_turns(int8_t n);
uint8_t    mi_syn_click(void);
uint8_t    mi_syn_hold(uint16_t ms, uint32_t now);
bool       mi_syn_hold_active(void);

/* A boundary that throws input away (mi_clear, a close): an outstanding
 * synthetic turn, click or hold ends cancelled. */
