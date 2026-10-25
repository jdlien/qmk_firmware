// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
/* Who owns the panel, the menu's session and the dashboard's restore: C1 of
 * plans/CONFIG-MENU-BUILD-PLAN.md, row for row. Every change of owner or
 * session goes through po_event(); nothing else writes them. */
#include "panel_owner.h"
#include "menu_edit.h"

static panel_owner_t   owner;
static menu_session_t  session;
static restore_phase_t phase;
static uint16_t        gen;
static bool            damaged;
static uint8_t         step;
static debug_page_t    dpage;
static bool            to_diag;          /* DEBUG entered from Diagnostics */
static bool            close_on_release; /* a wake while OPEN: close when the hold lifts */
static uint32_t        last_repair;
static bool            repaired_once;
static uint16_t        fails;

void po_init(void) {
    owner            = PANEL_DASH;
    session          = SESSION_NONE;
    phase            = RESTORE_SPLASH;
    gen              = 0;
    damaged          = false;
    step             = 0;
    dpage            = DEBUG_PAGE_HEALTH;
    to_diag          = false;
    close_on_release = false;
    last_repair      = 0;
    repaired_once    = false;
    fails            = 0;
}

panel_owner_t   po_owner(void) { return owner; }
menu_session_t  po_session(void) { return session; }
restore_phase_t po_phase(void) { return phase; }
uint16_t        po_gen(void) { return gen; }
bool            po_damaged(void) { return damaged; }
debug_page_t    po_debug_page(void) { return dpage; }
bool            po_return_to_diag(void) { return to_diag; }
uint16_t        po_invariant_fail(void) { return fails; }
void            po_note_fail(uint16_t bit) { fails |= bit; }

bool po_restoring(void) {
    return phase == RESTORE_CLEAR || phase == RESTORE_STAGES || phase == RESTORE_RESIDUAL;
}
/* PENDING keeps the owner DASH, so the dashboard goes on painting under it. */
bool po_dash_paints(void) { return owner == PANEL_DASH && session != SESSION_OPEN && phase == RESTORE_DONE; }
bool po_owner_band_busy(void) { return owner != PANEL_DASH || session != SESSION_NONE || po_restoring(); }
bool po_panel_lit_for_owner(void) { return owner == PANEL_DEBUG || session == SESSION_OPEN; }
bool po_anim_admissible(void) { return owner == PANEL_DASH && session == SESSION_NONE && phase == RESTORE_DONE; }
bool po_restored(uint16_t g) { return owner == PANEL_DASH && g == gen && phase == RESTORE_DONE; }
bool po_visible(void) { return session == SESSION_OPEN; }
bool po_sleep_blocked(void) { return session != SESSION_NONE; }

/* Leaving DASH invalidates the dashboard, from DONE too. */
static uint16_t set_owner(panel_owner_t o) {
    if (o == owner) return 0;
    if (owner == PANEL_DASH) {
        phase = RESTORE_WAITING;
        step  = 0;
    }
    owner = o;
    return PO_ACT_QUIESCE_DROP;
}

static uint16_t restore_start(void) {
    gen++;
    phase   = RESTORE_CLEAR;
    step    = 1;
    damaged = false;
    return PO_ACT_RESTORE_START;
}

/* Starts under DASH (the session NONE or PENDING); waits under anyone else. */
static uint16_t restore_request(void) {
    if (owner == PANEL_DASH) return restore_start();
    phase = RESTORE_WAITING;
    step  = 0;
    return 0;
}

static uint16_t do_open(void) {
    uint16_t a = set_owner(PANEL_MENU);
    session    = SESSION_OPEN;
    to_diag    = false;
    return (uint16_t)(a | PO_ACT_RESET_BUFFERS | PO_ACT_OVERLAYS_OFF | PO_ACT_REPEAT_CANCEL |
                      PO_ACT_MENU_CLEAR | PO_ACT_INPUT_CLEAR);
}

/* P1b (C6): the two transitions that take the panel from the menu roll an
 * open edit back FIRST, before the owner and the session change (their
 * PO_ACT_EDIT_CANCEL is applied only afterwards, so it stays the record). The
 * rollback restores values and nothing else; with no edit it does nothing. */
static uint16_t do_close(void) {
    menu_edit_rollback();
    uint16_t a = (uint16_t)(PO_ACT_EDIT_CANCEL | PO_ACT_INPUT_CLEAR | PO_ACT_OVERLAYS_REPRIME);
    a |= set_owner(PANEL_DASH);
    session          = SESSION_NONE;
    to_diag          = false;
    close_on_release = false;
    return (uint16_t)(a | restore_start());
}

static uint16_t cancel_pending(void) {
    session = SESSION_NONE;
    return (uint16_t)(PO_ACT_OVERLAYS_REPRIME | PO_ACT_INPUT_CLEAR);
}

static uint16_t enter_debug(debug_page_t page, bool from_menu) {
    menu_edit_rollback();   /* above: the debug page's handoff, dev op 1's included */
    uint16_t a = set_owner(PANEL_DEBUG);
    dpage      = page;
    to_diag    = from_menu;
    a |= PO_ACT_RESET_BUFFERS | PO_ACT_DEBUG_ENTER;
    if (from_menu) a |= PO_ACT_EDIT_CANCEL | PO_ACT_INPUT_CLEAR;
    return a;
}

static uint16_t back_to_menu(void) {
    uint16_t a = set_owner(PANEL_MENU);
    return (uint16_t)(a | PO_ACT_RESET_BUFFERS | PO_ACT_MENU_CLEAR | PO_ACT_INPUT_CLEAR);
}

uint16_t po_event(po_event_t ev, bool wake_held, uint32_t now) {
    (void)now;
    switch (ev) {
        case PO_EV_OPENER:
            if (session == SESSION_PENDING) return cancel_pending();
            if (session == SESSION_OPEN) return do_close();
            if (owner == PANEL_ANIM) return PO_ACT_REFUSED;
            if (owner == PANEL_DASH && wake_held) {
                session = SESSION_PENDING;
                return (uint16_t)(PO_ACT_OVERLAYS_OFF | PO_ACT_INPUT_CLEAR);
            }
            return do_open();   /* from DASH, or a handoff from a standalone DEBUG */

        case PO_EV_WAKE_RELEASED:
            if (session == SESSION_PENDING) return do_open();
            if (session == SESSION_OPEN && close_on_release) return do_close();
            return 0;

        case PO_EV_DBG_PAGE:
            if (session == SESSION_PENDING || owner == PANEL_ANIM) return PO_ACT_REFUSED;
            if (session == SESSION_OPEN) {
                if (owner == PANEL_MENU) return enter_debug(DEBUG_PAGE_HEALTH, true);
                if (dpage == DEBUG_PAGE_HEALTH) {
                    dpage = DEBUG_PAGE_CAMERA;
                    return PO_ACT_DEBUG_CAMERA;
                }
                return back_to_menu();
            }
            if (owner == PANEL_DASH) return enter_debug(DEBUG_PAGE_HEALTH, false);
            if (dpage == DEBUG_PAGE_HEALTH) {   /* DEBUG, standalone */
                dpage = DEBUG_PAGE_CAMERA;
                return PO_ACT_DEBUG_CAMERA;
            }
            return (uint16_t)(set_owner(PANEL_DASH) | restore_start());

        case PO_EV_DIAG_HEALTH:
        case PO_EV_DIAG_CAMERA:
            if (session != SESSION_OPEN || owner != PANEL_MENU) return PO_ACT_REFUSED;
            return enter_debug(ev == PO_EV_DIAG_CAMERA ? DEBUG_PAGE_CAMERA : DEBUG_PAGE_HEALTH, true);

        case PO_EV_DEBUG_CLICK:
            if (session != SESSION_OPEN || owner != PANEL_DEBUG) return PO_ACT_REFUSED;
            return back_to_menu();

        case PO_EV_CLOSE:
            if (session == SESSION_PENDING) return cancel_pending();
            if (session == SESSION_OPEN) return do_close();
            return PO_ACT_REFUSED;

        case PO_EV_ANIM_START:
            if (!po_anim_admissible()) return PO_ACT_REFUSED;
            return (uint16_t)(set_owner(PANEL_ANIM) | PO_ACT_ANIM_START);

        case PO_EV_ANIM_STOP:
            /* Always allowed: a fresh restore from band 1 subsumes any request
             * that waited under the animation. */
            if (owner != PANEL_ANIM) return PO_ACT_REFUSED;
            return (uint16_t)(PO_ACT_ANIM_STOP | set_owner(PANEL_DASH) | restore_start());

        case PO_EV_STARTUP:
            return restore_request();

        case PO_EV_WAKE_WHILE_OPEN:
            fails |= PO_FAIL_WAKE_OPEN;
            if (session == SESSION_OPEN) close_on_release = true;
            return 0;
    }
    return PO_ACT_REFUSED;
}

uint8_t po_restore_step(void) {
    return (phase == RESTORE_CLEAR || phase == RESTORE_STAGES) ? step : 0;
}

void po_restore_advance(void) {
    if (phase != RESTORE_CLEAR && phase != RESTORE_STAGES) return;
    step++;
    if (step > PO_CLEAR_BANDS + PO_STAGES) {
        phase = RESTORE_RESIDUAL;
        step  = 0;
    } else if (step > PO_CLEAR_BANDS) {
        phase = RESTORE_STAGES;
    }
}

po_residual_t po_residual_next(const po_residual_in_t *in, uint32_t now) {
    if (phase != RESTORE_RESIDUAL) return PO_RES_NONE;
    if (in->busy) return PO_RES_RECOVER;
    if (in->lq) return PO_RES_LQ;
    if (in->gq) return PO_RES_GQ;
    if (in->batt) return PO_RES_BATT;
    if (in->logo_os) return PO_RES_LOGO_OS;
    if (in->logo_conn) return PO_RES_LOGO_CONN;
    /* A loss not yet taken: the pump's top takes it next pass and marks the
     * generation damaged. Never DONE over it. */
    if (in->lost) return PO_RES_WAIT;
    if (damaged) {
        /* Only the repair is rate-limited, never the remembering. */
        if (repaired_once && (uint32_t)(now - last_repair) < PO_REPAIR_COOLDOWN_MS) return PO_RES_WAIT;
        repaired_once = true;
        last_repair   = now;
        restore_start();
        return PO_RES_RESTART;
    }
    phase = RESTORE_DONE;
    return PO_RES_DONE;
}

uint16_t po_loss(uint32_t now) {
    (void)now;
    switch (owner) {
        case PANEL_DASH:
            if (phase == RESTORE_DONE) {
                /* Recorded at once: display_restored() is false from this pass,
                 * and RESIDUAL starts the repair when the cooldown allows. */
                phase   = RESTORE_RESIDUAL;
                step    = 0;
                damaged = true;
            } else if (po_restoring()) {
                damaged = true;
            }
            return 0;
        case PANEL_MENU:  return PO_ACT_RESET_BUFFERS;
        case PANEL_DEBUG: return PO_ACT_DEBUG_ENTER;
        case PANEL_ANIM:  return PO_ACT_ANIM_STOP;
    }
    return 0;
}
