// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Who owns the panel, the menu's session, and the dashboard's restore
// (plans/CONFIG-MENU-BUILD-PLAN.md, C1). Pure logic: no ChibiOS, no LCD. The
// display executes what these functions decide; the host tests
// (scripts/menu_s0_test.c) run this file as the firmware does.
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Declaration order is op 27's wire value (sub 5, bytes 8-9 and 12). */
typedef enum { PANEL_DASH = 0, PANEL_DEBUG, PANEL_MENU, PANEL_ANIM } panel_owner_t;
typedef enum { SESSION_NONE = 0, SESSION_PENDING, SESSION_OPEN } menu_session_t;
typedef enum {
    RESTORE_SPLASH = 0,   /* boot, until the startup callback */
    RESTORE_WAITING,      /* the dashboard is invalid; another owner holds the panel */
    RESTORE_CLEAR,        /* the banded clear */
    RESTORE_STAGES,       /* the components, one a pass */
    RESTORE_RESIDUAL,     /* the queues, the battery, the logos: po_residual_next() */
    RESTORE_DONE,
} restore_phase_t;
typedef enum { DEBUG_PAGE_HEALTH = 0 } debug_page_t;   /* 10-28: the camera page (1) gone */

/* The restore's cursor: steps 1..PO_CLEAR_BANDS are the clear's bands, the
 * PO_STAGES after them the components (display.c's debug_restore_step). */
#define PO_CLEAR_BANDS 8
#define PO_STAGES      6
#define PO_REPAIR_COOLDOWN_MS 5000u

/* What an event asks the display to do, in this order. */
enum {
    PO_ACT_REFUSED          = 1u << 0,   /* nothing changed */
    PO_ACT_QUIESCE_DROP     = 1u << 1,   /* the owner changes: quiesce, drop a latched loss */
    PO_ACT_RESET_BUFFERS    = 1u << 2,   /* dbg_text, dbg_shown, the damage state */
    PO_ACT_OVERLAYS_OFF     = 1u << 3,   /* suppress, and clear one showing */
    PO_ACT_REPEAT_CANCEL    = 1u << 4,   /* the param repeat */
    PO_ACT_MENU_CLEAR       = 1u << 5,   /* the menu's opening clear and first screen */
    PO_ACT_EDIT_CANCEL      = 1u << 6,
    PO_ACT_INPUT_CLEAR      = 1u << 7,   /* the coalesced input and the gesture */
    PO_ACT_RESTORE_START    = 1u << 8,   /* the display discards its queues: stage 1 */
    PO_ACT_OVERLAYS_REPRIME = 1u << 9,
    PO_ACT_DEBUG_ENTER      = 1u << 10,  /* po_debug_page() says which */
    /* 1u << 11 was PO_ACT_DEBUG_CAMERA, Health to Camera in place: retired 10-28 */
    PO_ACT_ANIM_START       = 1u << 12,
    PO_ACT_ANIM_STOP        = 1u << 13,
};

typedef enum {
    PO_EV_OPENER = 0,      /* Fn plus the knob's press, the session closed or pending */
    PO_EV_WAKE_RELEASED,   /* display_wake_release()'s notice */
    PO_EV_DBG_PAGE,        /* the DBG_PAGE key or op 1 */
    PO_EV_DIAG_HEALTH,     /* Diagnostics > Health (the fixture's row 7 in S0) */
    PO_EV_DIAG_CAMERA,     /* retired 10-28 with the camera page: refused; the number kept for the events after it */
    PO_EV_DEBUG_CLICK,     /* a knob click while DEBUG holds an open session */
    PO_EV_CLOSE,           /* Exit, Fn plus a click, the timeout, the backstop, a disarm */
    PO_EV_ANIM_START,      /* the header already validated */
    PO_EV_ANIM_STOP,
    PO_EV_STARTUP,         /* the startup callback, or op 27 sub 7 */
    PO_EV_WAKE_WHILE_OPEN, /* not expected: DSB_MENU blocks deep sleep */
} po_event_t;

void po_init(void);
uint16_t po_event(po_event_t ev, bool wake_held, uint32_t now);

panel_owner_t   po_owner(void);
menu_session_t  po_session(void);
restore_phase_t po_phase(void);
uint16_t        po_gen(void);
bool            po_damaged(void);
debug_page_t    po_debug_page(void);
bool            po_return_to_diag(void);   /* DEBUG under an open session goes back to MENU */

/* The predicates (C1). */
bool po_dash_paints(void);        /* without today's splash, bus and pass gates: the caller adds them */
bool po_restoring(void);
bool po_owner_band_busy(void);    /* display_band_busy()'s first test */
bool po_panel_lit_for_owner(void);
bool po_anim_admissible(void);
bool po_restored(uint16_t gen);
bool po_visible(void);            /* the session OPEN: C7's override */
bool po_sleep_blocked(void);      /* the session not NONE: DSB_MENU */

/* The restore's cursor, for display.c's stages. */
uint8_t po_restore_step(void);           /* 1..PO_CLEAR_BANDS+PO_STAGES while CLEAR or STAGES */
void    po_restore_advance(void);        /* a band succeeded, or a stage ran */

/* RESIDUAL's scheduler: what this pass does. */
typedef struct {
    bool busy;        /* lcd_blit_busy() */
    bool lq, gq;      /* a queued label or glyph */
    bool batt;        /* batt_dirty || batt_bulb_stale() */
    bool logo_os, logo_conn;
    bool lost;        /* a loss latched and not yet taken (lcd_blit_lost_pending) */
} po_residual_in_t;
typedef enum {
    PO_RES_NONE = 0,  /* not in RESIDUAL */
    PO_RES_RECOVER,   /* pump_bus_busy */
    PO_RES_LQ, PO_RES_GQ, PO_RES_BATT, PO_RES_LOGO_OS, PO_RES_LOGO_CONN,
    PO_RES_WAIT,      /* a damaged generation waiting out the cooldown */
    PO_RES_RESTART,   /* the cooldown passed: a new generation was started (PO_ACT_RESTORE_START's work) */
    PO_RES_DONE,      /* the phase is now DONE */
} po_residual_t;
po_residual_t po_residual_next(const po_residual_in_t *in, uint32_t now);

/* A loss, taken once a pass at the pump's top. Returns the actions for the
 * current owner: PO_ACT_MENU_CLEAR (MENU: every slot invalid), PO_ACT_DEBUG_ENTER
 * with po_debug_page() (DEBUG: the page showing invalidated, its clear again),
 * PO_ACT_ANIM_STOP (ANIM), or 0 (DASH: recorded as damage). */
uint16_t po_loss(uint32_t now);

/* Sticky invariant failures (op 27 sub 5, bytes 24-25). */
enum {
    PO_FAIL_EARLY_COMMIT = 1u << 0,
    PO_FAIL_EARLY_DONE   = 1u << 1,
    PO_FAIL_WAKE_OPEN    = 1u << 2,
    PO_FAIL_SCRATCH      = 1u << 3,   /* lcd_bus.c, dev builds: the shared scratch's rules broken */
    PO_FAIL_STREAM       = 1u << 4,   /* a nested, overlong or short panel-order stream */
    PO_FAIL_SPI0_POLL    = 1u << 5,   /* lcd_bus.c, dev builds: a polled bulk send gave up */
};
uint16_t po_invariant_fail(void);
void     po_note_fail(uint16_t bit);
