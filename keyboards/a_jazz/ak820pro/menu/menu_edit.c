// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
#include "menu_edit.h"

#include "menu_env.h"

/* QMK's RGB keycode ranges (keycodes.h), as writers: RM_* 0x7840-0x784E,
 * flags included; UG_* 0x7820-0x782A, which process_underglow() runs into
 * the RGB-matrix setters (RGB_MATRIX_DISABLE_SHARED_KEYCODES isn't defined).
 * The RGB_MODE_* between them reach no setter here. The firmware checks the
 * numbers against QMK's names; the host tests can't include QMK. */
#define ME_RM_FIRST 0x7840u
#define ME_RM_LAST  0x784Eu
#define ME_UG_FIRST 0x7820u
#define ME_UG_LAST  0x782Au
#ifndef MENU_HOST_TEST
#    include "keycodes.h"
_Static_assert(ME_RM_FIRST == QK_RGB_MATRIX_ON && ME_RM_LAST == QK_RGB_MATRIX_FLAG_PREVIOUS &&
                   ME_UG_FIRST == QK_UNDERGLOW_TOGGLE && ME_UG_LAST == QK_UNDERGLOW_SPEED_DOWN,
               "QMK's RGB keycode ranges");
#endif

/* The one edit (C6): its field and its domain's snapshot, 9 B. */
static struct {
    uint8_t   field;   /* ME_NONE: none */
    uint8_t   level;   /* ME_LCD, ME_LOCKS, ME_CHG, ME_PROFILE: the level when it opened */
    env_rgb_t rgb;     /* an RGB field, ME_PROFILE: the config when it opened */
} ed;
static uint8_t  pending;     /* ME_PEND_* for menu_tick() */
static uint16_t ext_count;   /* external writes that cancelled an edit (op 27 sub 5) */

static bool    is_rgb(uint8_t f) { return (f >= ME_MODE && f <= ME_ENABLE) || f == ME_PROFILE; }
static bool    is_ind(uint8_t f) { return f == ME_LOCKS || f == ME_CHG; }
static uint8_t ind_of(uint8_t f) { return f == ME_LOCKS ? ENV_IND_LOCKS : ENV_IND_CHARGING; }
static uint8_t domain_of(uint8_t f) { return is_rgb(f) ? ME_DOM_RGB : f == ME_LCD ? ME_DOM_LCD : ME_DOM_IND; }
/* A writer of domain d conflicts with the edit: its own domain, and P5e's
 * profile preview the LCD's too. */
static bool in_domain(uint8_t f, uint8_t d) { return domain_of(f) == d || (f == ME_PROFILE && d == ME_DOM_LCD); }

/* A level field's live value (the LCD's, or an indicator group's). */
static uint8_t level_now(uint8_t f) { return f == ME_LCD ? env_lcd_level() : env_ind_level(ind_of(f)); }

static uint8_t field_of(const env_rgb_t *c, uint8_t f) {
    switch (f) {
        case ME_MODE:   return c->mode;
        case ME_VAL:    return c->v;
        case ME_HUE:    return c->h;
        case ME_SAT:    return c->s;
        case ME_SPEED:  return c->speed;
        case ME_ENABLE: return c->enable;
        default:        return 0;
    }
}

/* One field set, by the saving setter or not. QMK's mode and HSV setters do
 * nothing while the lights are off, so those run with the lights on, inside
 * this call, and the enable goes back last (C6); the enable changes are
 * _noeeprom, so a dirty flag the setter raised stays raised. A preview sets
 * the mode only when it changes, so an HSV or speed preview doesn't restart
 * the effect; Save always calls the saving setter (the flag is the point). */
static void set_field(uint8_t f, uint8_t v, bool save) {
    env_rgb_t c;
    env_rgb_get(&c);
    switch (f) {
        case ME_LCD:    env_lcd_set(v, save); return;
        case ME_LOCKS:
        case ME_CHG:    env_ind_set(ind_of(f), v, save); return;
        case ME_ENABLE: env_rgb_enable(v != 0, save); return;
        case ME_SPEED:  env_rgb_speed(v, save); return;
        case ME_MODE:
            if (!save && c.mode == v) return;
            break;
        default: break;
    }
    if (!c.enable) env_rgb_enable(true, false);
    if (f == ME_MODE) env_rgb_mode(v, save);
    else env_rgb_hsv(f == ME_HUE ? v : c.h, f == ME_SAT ? v : c.s, f == ME_VAL ? v : c.v, save);
    if (!c.enable) env_rgb_enable(false, false);
}

/* The snapshot back, every field _noeeprom, the enable last. */
static void restore(void) {
    if (ed.field == ME_LCD || is_ind(ed.field)) {
        set_field(ed.field, ed.level, false);
        return;
    }
    if (ed.field == ME_PROFILE) env_lcd_set(ed.level, false);   /* P5e: the LCD's level, then the RGB config */
    env_rgb_t c;
    env_rgb_get(&c);
    const env_rgb_t *s = &ed.rgb;
    bool on = c.enable;
    if (!on && (c.mode != s->mode || c.h != s->h || c.s != s->s || c.v != s->v)) {
        env_rgb_enable(true, false);
        on = true;
    }
    if (c.mode != s->mode) env_rgb_mode(s->mode, false);
    if (c.h != s->h || c.s != s->s || c.v != s->v) env_rgb_hsv(s->h, s->s, s->v, false);
    if (c.speed != s->speed) env_rgb_speed(s->speed, false);
    if (c.flags != s->flags) env_rgb_flags(s->flags);
    if (on != (s->enable != 0)) env_rgb_enable(s->enable != 0, false);
}

/* Every close comes here (Save and the rollback): an indicator edit's
 * preview stops lighting its LEDs, so the force can't outlive the edit. */
static void close_edit(void) {
    if (is_ind(ed.field)) env_ind_force(ind_of(ed.field), false);
    ed.field = ME_NONE;
}

void me_open(uint8_t field) {
    menu_edit_rollback();
    if (field == ME_NONE || field > ME_PROFILE) return;
    ed.field = field;
    if (field == ME_LCD || is_ind(field)) ed.level = level_now(field);
    else env_rgb_get(&ed.rgb);
    if (field == ME_PROFILE) ed.level = env_lcd_level();
    if (is_ind(field)) env_ind_force(ind_of(field), true);   /* the preview shows, whatever Caps is */
}

bool    me_active(void) { return ed.field != ME_NONE; }
uint8_t me_field(void) { return ed.field; }

uint8_t me_value(void) {
    if (ed.field == ME_LCD || is_ind(ed.field)) return level_now(ed.field);
    env_rgb_t c;
    env_rgb_get(&c);
    return field_of(&c, ed.field);
}

uint8_t me_saved(void) { return ed.field == ME_LCD || is_ind(ed.field) ? ed.level : field_of(&ed.rgb, ed.field); }

void me_preview(uint8_t value) {
    if (ed.field != ME_NONE && ed.field != ME_PROFILE) set_field(ed.field, value, false);   /* a profile's: prof_preview() */
}

void me_save(void) {
    if (ed.field == ME_NONE || ed.field == ME_PROFILE) return;
    set_field(ed.field, me_value(), true);
    close_edit();
}

void me_cancel(void) { menu_edit_rollback(); }

void me_keep(void) { close_edit(); }

void menu_edit_rollback(void) {
    if (ed.field == ME_NONE) return;
    restore();
    close_edit();
}

void menu_edit_external(uint8_t domain) {
    pending |= ME_PEND_CHANGED;
    if (ed.field == ME_NONE || !in_domain(ed.field, domain)) return;
    menu_edit_rollback();
    pending |= ME_PEND_CANCELLED;
    if (ext_count < 0xFFFFu) ext_count++;
}

uint8_t me_take_pending(void) {
    uint8_t p = pending;
    pending   = 0;
    return p;
}

uint16_t me_external_count(void) { return ext_count; }

bool menu_edit_rgb_open(void) { return is_rgb(ed.field); }

uint8_t me_key_domain(uint16_t keycode, bool pressed) {
    if (keycode >= ME_RM_FIRST && keycode <= ME_RM_LAST) return pressed ? 0 : ME_DOM_RGB;
    if (keycode >= ME_UG_FIRST && keycode <= ME_UG_LAST) return pressed ? ME_DOM_RGB : 0;
    return 0;
}
