// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// P1b's edit transaction (C6; plans/CONFIG-MENU-P1-DESIGN.md, "P1b"): one
// previewing edit at most, of an RGB field, the LCD level or an indicator
// level (P2a, which lights its LEDs while it's open). Pure logic over
// menu_env.h's primitives, so the host tests run this sequencing, not a copy.
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The fields an edit can preview. */
enum {
    ME_NONE = 0,
    ME_MODE,     /* the effect */
    ME_VAL,      /* brightness */
    ME_HUE,
    ME_SAT,
    ME_SPEED,    /* speed, or Alphas/Mods' second colour's hue offset */
    ME_ENABLE,   /* Lights */
    ME_LCD,      /* the Screen's level */
    ME_LOCKS,    /* P2a: the lock lights' level, 0-100 */
    ME_CHG,      /* the charging light's */
};

/* The conflict domains of an external writer. ME_DOM_IND has none today:
 * only the menu sets an indicator level. */
enum { ME_DOM_RGB = 1, ME_DOM_LCD = 2, ME_DOM_IND = 3 };

/* The pending bits menu_tick() consumes (me_take_pending()). */
enum {
    ME_PEND_CHANGED   = 1u << 0,   /* an external write: a new target */
    ME_PEND_CANCELLED = 1u << 1,   /* and it cancelled the open edit: pop it, "Cancelled" */
};

void    me_open(uint8_t field);       /* snapshots the domain; any open edit is rolled back first */
bool    me_active(void);
uint8_t me_field(void);               /* ME_NONE when none */
uint8_t me_value(void);               /* the field's live value (the preview) */
uint8_t me_saved(void);               /* the field's value when the edit opened: the check, and Cancel's */
void    me_preview(uint8_t value);
void    me_save(void);                /* the live value, by the saving setter; closes */
void    me_cancel(void);              /* the snapshot back; closes */

/* C6's rollback: the snapshot back and the transaction closed, nothing else.
 * Idempotent. panel_owner.c calls it before the panel leaves the menu. */
void menu_edit_rollback(void);

/* An external writer of `domain`, called just before it applies: rolls back
 * an open edit of that domain, and marks the change for menu_tick(). */
void    menu_edit_external(uint8_t domain);
uint8_t me_take_pending(void);        /* ME_PEND_* since the last take; clears them */
uint16_t me_external_count(void);     /* op 27 sub 5's count of external writes that cancelled an edit */

/* The board's flush hold: an RGB edit is open (its preview must not reach
 * EEPROM). */
bool menu_edit_rgb_open(void);

/* QMK's own RGB keycodes as writers (C6): RM_* act on release (no
 * RGB_TRIGGER_ON_KEYDOWN), UG_* on press. ME_DOM_RGB, or 0. */
uint8_t me_key_domain(uint16_t keycode, bool pressed);
