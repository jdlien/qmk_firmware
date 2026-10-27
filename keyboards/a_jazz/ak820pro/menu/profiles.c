// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
#include "profiles.h"

#include <string.h>

#include "menu.h"        /* MENU_LCD_MAX */
#include "menu_edit.h"   /* menu_edit_external(): an open preview's rollback */
#include "menu_env.h"

/* The ranges a record's fields are checked against. menu_glue.c asserts
 * each is the firmware's own. */
#define PROF_CLOCK_MODES   6u   /* DISPLAY_CLOCK_MODE_COUNT */
#define PROF_POWER_MODES   4u   /* POWER_MODE_COUNT */
#define PROF_BVIEW_PERCENT 1u   /* KB_BATT_VIEW_PERCENT */
#define PROF_BVIEW_HOURS   2u   /* KB_BATT_VIEW_HOURS */
#define PROF_BVIEW_BOTH    3u   /* KB_BATT_VIEW_BOTH (JD, 10-26) */

/* A restore's transaction (3 B): the host's token; the pages received, in
 * order (bits 0-4, a count: P5d's nine pages outgrew P5a's five bits of a
 * mask, and RAM has no byte to spare), the open bit, and the write-failure
 * latch; a countdown in 10 Hz ticks, 5 s without a page aborting it.
 * The latch (codex, P5a's review): wear_leveling_write() updates its RAM
 * cache BEFORE it programs the flash, so after a failed write the cache
 * holds bytes the flash may not. A retry of the same bytes would then be
 * skipped as unchanged and commit a slot whose body never landed. So the
 * first failure refuses every later profile write until a reboot reloads
 * the cache from the flash, which then shows the truth. Aborts keep it.
 * TX_CHANGED (P5b): the host wrote the block, so the menu repaints the names
 * its signatures don't carry (prof_take_changed()). Closes keep it too. */
#define TX_OPEN    0x80u
#define TX_FAILED  0x40u
#define TX_CHANGED 0x20u
#define TX_PAGES   0x1Fu   /* the count of pages received in order */
#define TX_TICKS   50u
_Static_assert(PROF_PAGES <= TX_PAGES, "the pages' count fits its five bits");
static uint8_t tx_token, tx_state, tx_left;
/* The active slot's choice still to be written (P5b), 1 B: 0 none, else
 * 0x80 | the slot (0-7), or 0xFF for no slot. The menu's Load, Save and
 * Delete and the Next and Previous keys all set it here, one writer and one
 * rule: it rides kb_eeconfig's settle (env_settings_touch()) and prof_tick()
 * writes it once that write is done, so a run of presses costs one write and
 * a deep sleep waits on it (DSB_FLASH). The records' writes stay at the
 * click. */
static uint8_t pend;
_Static_assert(PROF_SLOTS <= 0x7Fu, "the pending slot's 7 bits");

/* CRC-8, polynomial 0x07, initial 0xFF, MSB first (hostagent/ak820profiles.py
 * computes the same), a byte a step from a table (P5b): bit at a time, the
 * menu's scans of the slots cost passes ~1.8 ms on the board, under the row
 * ISR's share of this core. */
static const uint8_t crc_tab[256] = {
    0x00, 0x07, 0x0E, 0x09, 0x1C, 0x1B, 0x12, 0x15, 0x38, 0x3F, 0x36, 0x31, 0x24, 0x23, 0x2A, 0x2D,
    0x70, 0x77, 0x7E, 0x79, 0x6C, 0x6B, 0x62, 0x65, 0x48, 0x4F, 0x46, 0x41, 0x54, 0x53, 0x5A, 0x5D,
    0xE0, 0xE7, 0xEE, 0xE9, 0xFC, 0xFB, 0xF2, 0xF5, 0xD8, 0xDF, 0xD6, 0xD1, 0xC4, 0xC3, 0xCA, 0xCD,
    0x90, 0x97, 0x9E, 0x99, 0x8C, 0x8B, 0x82, 0x85, 0xA8, 0xAF, 0xA6, 0xA1, 0xB4, 0xB3, 0xBA, 0xBD,
    0xC7, 0xC0, 0xC9, 0xCE, 0xDB, 0xDC, 0xD5, 0xD2, 0xFF, 0xF8, 0xF1, 0xF6, 0xE3, 0xE4, 0xED, 0xEA,
    0xB7, 0xB0, 0xB9, 0xBE, 0xAB, 0xAC, 0xA5, 0xA2, 0x8F, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9D, 0x9A,
    0x27, 0x20, 0x29, 0x2E, 0x3B, 0x3C, 0x35, 0x32, 0x1F, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0D, 0x0A,
    0x57, 0x50, 0x59, 0x5E, 0x4B, 0x4C, 0x45, 0x42, 0x6F, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7D, 0x7A,
    0x89, 0x8E, 0x87, 0x80, 0x95, 0x92, 0x9B, 0x9C, 0xB1, 0xB6, 0xBF, 0xB8, 0xAD, 0xAA, 0xA3, 0xA4,
    0xF9, 0xFE, 0xF7, 0xF0, 0xE5, 0xE2, 0xEB, 0xEC, 0xC1, 0xC6, 0xCF, 0xC8, 0xDD, 0xDA, 0xD3, 0xD4,
    0x69, 0x6E, 0x67, 0x60, 0x75, 0x72, 0x7B, 0x7C, 0x51, 0x56, 0x5F, 0x58, 0x4D, 0x4A, 0x43, 0x44,
    0x19, 0x1E, 0x17, 0x10, 0x05, 0x02, 0x0B, 0x0C, 0x21, 0x26, 0x2F, 0x28, 0x3D, 0x3A, 0x33, 0x34,
    0x4E, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5C, 0x5B, 0x76, 0x71, 0x78, 0x7F, 0x6A, 0x6D, 0x64, 0x63,
    0x3E, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2C, 0x2B, 0x06, 0x01, 0x08, 0x0F, 0x1A, 0x1D, 0x14, 0x13,
    0xAE, 0xA9, 0xA0, 0xA7, 0xB2, 0xB5, 0xBC, 0xBB, 0x96, 0x91, 0x98, 0x9F, 0x8A, 0x8D, 0x84, 0x83,
    0xDE, 0xD9, 0xD0, 0xD7, 0xC2, 0xC5, 0xCC, 0xCB, 0xE6, 0xE1, 0xE8, 0xEF, 0xFA, 0xFD, 0xF4, 0xF3,
};
uint8_t prof_crc8(const uint8_t *p, uint8_t n) {
    uint8_t c = 0xFFu;
    while (n--) c = crc_tab[c ^ *p++];
    return c;
}

static uint8_t body_crc(const uint8_t *rec) { return prof_crc8(&rec[PR_FIELDS], PR_CRC - PR_FIELDS); }

/* ASCII 0x20-0x7E, NUL-padded: nothing after the first NUL. */
static bool name_ok(const uint8_t *n) {
    bool end = false;
    for (uint8_t i = 0; i < PROF_NAME; i++) {
        if (n[i] == 0u)
            end = true;
        else if (end || n[i] < 0x20u || n[i] > 0x7Eu)
            return false;
    }
    return true;
}

bool prof_rec_valid(const uint8_t *rec) {
    if (rec[PR_STATE] != PS_SAVED_V1 || rec[PR_CRC] != body_crc(rec)) return false;
    uint8_t f = rec[PR_FIELDS];
    if ((f & PF_LIGHTING) && rec[PR_RGB_ON] > 1u) return false;
    if ((f & PF_LCD) && rec[PR_LCD] > MENU_LCD_MAX) return false;
    if ((f & PF_LOCKS) && rec[PR_LOCKS] > ENV_IND_MAX) return false;
    if ((f & PF_CHARGING) && rec[PR_CHARGING] > ENV_IND_MAX) return false;
    if ((f & PF_CLOCK) && rec[PR_CLOCK] >= PROF_CLOCK_MODES) return false;
    if ((f & PF_BVIEW) && (rec[PR_BVIEW] < PROF_BVIEW_PERCENT || rec[PR_BVIEW] > PROF_BVIEW_BOTH)) return false;
    if ((f & PF_POWER) && rec[PR_POWER] >= PROF_POWER_MODES) return false;
    if ((f & PF_NAME) && !name_ok(&rec[PR_NAME])) return false;
    return true;
}

/* --- The block, through the EEPROM's RAM cache and wear_leveling_write() --- */

static void rd(uint8_t off, void *dst, uint8_t n) { env_prof_read((uint16_t)(PROF_BASE + off), dst, n); }
static uint8_t rd8(uint8_t off) {
    uint8_t b;
    rd(off, &b, 1);
    return b;
}
/* One write of exactly those bytes: false when it failed. */
static bool wr(uint8_t off, const void *src, uint8_t n, bool *consolidated) {
    if (tx_state & TX_FAILED) return false;
    uint8_t s = env_prof_write((uint16_t)(PROF_BASE + off), src, n);
    if (s == ENV_WR_CONSOLIDATED) *consolidated = true;
    if (s == ENV_WR_FAILED) tx_state |= TX_FAILED;
    return s != ENV_WR_FAILED;
}
static bool wr8(uint8_t off, uint8_t v, bool *consolidated) { return wr(off, &v, 1, consolidated); }

bool prof_formatted(void) { return rd8(PROF_OFF_MAGIC) == PROF_MAGIC_V2; }

bool prof_slot(uint8_t slot, uint8_t *rec) {
    if (slot >= PROF_SLOTS || !prof_formatted()) {
        memset(rec, 0, PROF_REC);
        return false;
    }
    rd(PROF_REC_OFF(slot), rec, PROF_REC);
    return prof_rec_valid(rec);
}

/* P5d: a bit a valid slot, the magic read once (the menu's snapshot reads
 * this every 128 ms: prof_slot()'s own magic read a slot made it 18 reads). */
uint8_t prof_valid_mask(void) {
    uint8_t rec[PROF_REC], v = 0;
    if (!prof_formatted()) return 0;
    for (uint8_t s = 0; s < PROF_SLOTS; s++) {
        rd(PROF_REC_OFF(s), rec, PROF_REC);
        if (prof_rec_valid(rec)) v |= (uint8_t)(1u << s);
    }
    return v;
}
_Static_assert(PROF_SLOTS <= 8u, "a mask's byte");

uint8_t prof_free_slot(void) {
    uint8_t v = prof_valid_mask();
    for (uint8_t s = 0; s < PROF_SLOTS; s++)
        if (!((v >> s) & 1u)) return s;
    return 0xFFu;
}

/* The active byte as it will be: the pending choice, else the block's. */
static uint8_t active_byte(void) { return pend ? (pend == 0xFFu ? 0xFFu : (uint8_t)(pend & 0x7Fu)) : rd8(PROF_OFF_ACTIVE); }
static void    set_active(uint8_t a) {
    pend = (uint8_t)(0x80u | a);   /* 0xFF stays 0xFF */
    env_settings_touch();
}

uint8_t prof_active_rec(uint8_t *rec) {
    uint8_t a = prof_formatted() ? active_byte() : 0xFFu;
    return a < PROF_SLOTS && prof_slot(a, rec) ? a : 0xFFu;
}
uint8_t prof_active(void) {
    uint8_t rec[PROF_REC];
    return prof_active_rec(rec);
}

void prof_name(uint8_t slot, const uint8_t *rec, char *out) {
    if ((rec[PR_FIELDS] & PF_NAME) && rec[PR_NAME]) {
        memcpy(out, &rec[PR_NAME], PROF_NAME);
        out[PROF_NAME] = '\0';
    } else {
        memcpy(out, "Profile ", 8);
        out[8] = (char)('1' + (slot & 7u));
        out[9] = '\0';
    }
}

/* A fresh flash leaves the block unformatted. The first write formats it:
 * the states, the active byte, then the magic last, so the first save shows
 * and a stale byte from before the flash never reads as a record. */
static bool format(bool *consolidated) {
    for (uint8_t s = 0; s < PROF_SLOTS; s++)
        if (!wr8(PROF_REC_OFF(s) + PR_STATE, PS_EMPTY, consolidated)) return false;
    return wr8(PROF_OFF_ACTIVE, 0xFFu, consolidated) && wr8(PROF_OFF_MAGIC, PROF_MAGIC_V2, consolidated);
}

/* Never inside a deep sleep's recovery (a menu can be open in its first
 * passes: sleep.c's wake releases the display first), nor while a restore
 * is open. */
static uint8_t writable(void) {
    if (tx_state & TX_FAILED) return PROF_WRITE_FAILED;
    return env_recovering() || prof_restore_open() ? PROF_BUSY : PROF_OK;
}

bool prof_write_failed(void) { return (tx_state & TX_FAILED) != 0u; }

/* --- The effect's stable id (rgb_effects.c, append-only) ---------------------- */

/* Id 0 is reserved, never assigned: QMK clamps mode 0 (RGB_MATRIX_NONE)
 * to 1 (codex), so it can't be set, and a record holding 0 loads without
 * its effect, as an unknown id does. */
static uint8_t effect_id_of(uint8_t mode) {
    for (uint8_t i = 0; i < env_effects(); i++)
        if (env_effect_mode(i) == mode) return env_effect_id(i);
    return 0xFFu;   /* a mode the table doesn't name: a load skips it */
}
static bool mode_of_id(uint8_t id, uint8_t *mode) {
    if (id == 0u) return false;
    for (uint8_t i = 0; i < env_effects(); i++)
        if (env_effect_id(i) == id) {
            *mode = env_effect_mode(i);
            return true;
        }
    return false;
}

/* --- Capture, apply, and the slots' writes ------------------------------------ */

/* The live settings as a record's body, its crc not yet set. */
static void capture_body(uint8_t *rec, uint8_t fields) {
    env_rgb_t c;
    env_rgb_get(&c);
    memset(rec, 0, PROF_REC);
    rec[PR_STATE]    = PS_SAVED_V1;
    rec[PR_FIELDS]   = fields;
    rec[PR_RGB_ON]   = c.enable ? 1u : 0u;
    rec[PR_EFFECT]   = effect_id_of(c.mode);
    rec[PR_HUE]      = c.h;
    rec[PR_SAT]      = c.s;
    rec[PR_VAL]      = c.v;
    rec[PR_SPEED]    = c.speed;
    rec[PR_LCD]      = env_lcd_level();
    rec[PR_LOCKS]    = env_ind_level(ENV_IND_LOCKS);
    rec[PR_CHARGING] = env_ind_level(ENV_IND_CHARGING);
    rec[PR_CLOCK]    = env_clock_mode();
    rec[PR_BVIEW]    = env_batt_view();
    if (fields & PF_POWER) rec[PR_POWER] = env_power_mode();
}
void prof_capture(uint8_t *rec, uint8_t fields) {
    capture_body(rec, fields);
    rec[PR_CRC] = body_crc(rec);
}

/* The menu's `*`: a compare of the fields the record holds, not a hash, so
 * no collisions. The ladder's caps never touch these values, so they never
 * make it; any change does, Fn keys and VIA included. */
bool prof_differs(const uint8_t *rec) {
    static const uint8_t field_at[6][2] = {
        {PF_LCD, PR_LCD}, {PF_LOCKS, PR_LOCKS}, {PF_CHARGING, PR_CHARGING}, {PF_CLOCK, PR_CLOCK}, {PF_BVIEW, PR_BVIEW},
        {PF_POWER, PR_POWER},
    };
    uint8_t live[PROF_REC], f = rec[PR_FIELDS];
    capture_body(live, PF_LIVE);   /* no crc: only the fields are compared */
    if ((f & PF_LIGHTING) && memcmp(&live[PR_RGB_ON], &rec[PR_RGB_ON], PR_SPEED - PR_RGB_ON + 1u)) return true;
    for (uint8_t i = 0; i < 6u; i++)
        if ((f & field_at[i][0]) && live[field_at[i][1]] != rec[field_at[i][1]]) return true;
    return false;
}

uint8_t prof_apply(const uint8_t *rec, bool live) {
    uint8_t f = rec[PR_FIELDS], st = PROF_OK;
    if (f & PF_LIGHTING) {
        /* The edit's restore order (menu_edit.c): QMK's mode and HSV setters
         * return early while the lights are off, so on first, unsaved, and
         * the enable last at the record's value, saved. */
        uint8_t mode;
        env_rgb_enable(true, false);
        if (mode_of_id(rec[PR_EFFECT], &mode))
            env_rgb_mode(mode, true);
        else
            st = PROF_OK_EFFECT_SKIPPED;
        env_rgb_hsv(rec[PR_HUE], rec[PR_SAT], rec[PR_VAL], true);
        env_rgb_speed(rec[PR_SPEED], true);
        env_rgb_enable(rec[PR_RGB_ON] != 0u, true);
    }
    if (f & PF_LCD) env_lcd_set(rec[PR_LCD], true);
    if (f & PF_LOCKS) env_ind_set(ENV_IND_LOCKS, rec[PR_LOCKS], true);
    if (f & PF_CHARGING) env_ind_set(ENV_IND_CHARGING, rec[PR_CHARGING], true);
    if (f & PF_CLOCK) env_clock_set(rec[PR_CLOCK]);
    if (f & PF_BVIEW) env_batt_view_set(rec[PR_BVIEW]);
    if (live && (f & PF_POWER)) env_power_set(rec[PR_POWER]);   /* never a profile's (Decided 2) */
    return st;
}

/* P5e: the menu's preview, inside its ME_PROFILE edit (menu_edit.c holds the
 * live settings and the RGB flush): the lighting and the LCD level only,
 * the two that show with the menu up; nothing saved. The target is the
 * live settings the edit opened on (base, base_lcd) with the record's
 * fields over them, so a field the record lacks, or an effect this build
 * lacks, shows the live settings' and never an earlier preview's, which a
 * Load would then save (codex, P5e's review). Set in prof_apply()'s order
 * (the lights on while the mode and HSV change, the enable last), and only
 * what differs from what shows: a mode set restarts its animation. */
void prof_preview(uint8_t slot, const env_rgb_t *base, uint8_t base_lcd) {
    uint8_t   rec[PROF_REC], mode, lcd = base_lcd;
    env_rgb_t t = *base, c;
    if (prof_slot(slot, rec)) {
        if (rec[PR_FIELDS] & PF_LIGHTING) {
            if (mode_of_id(rec[PR_EFFECT], &mode)) t.mode = mode;
            t.h      = rec[PR_HUE];
            t.s      = rec[PR_SAT];
            t.v      = rec[PR_VAL];
            t.speed  = rec[PR_SPEED];
            t.enable = rec[PR_RGB_ON] ? 1u : 0u;
        }
        if (rec[PR_FIELDS] & PF_LCD) lcd = rec[PR_LCD];
    }
    env_rgb_get(&c);
    bool on = c.enable;
    if (!on && (c.mode != t.mode || c.h != t.h || c.s != t.s || c.v != t.v)) {
        env_rgb_enable(true, false);
        on = true;
    }
    if (c.mode != t.mode) env_rgb_mode(t.mode, false);
    if (c.h != t.h || c.s != t.s || c.v != t.v) env_rgb_hsv(t.h, t.s, t.v, false);
    if (c.speed != t.speed) env_rgb_speed(t.speed, false);
    if (on != (t.enable != 0u)) env_rgb_enable(t.enable != 0u, false);
    if (lcd != env_lcd_level()) env_lcd_set(lcd, false);
}

uint8_t prof_save(uint8_t slot, bool *consolidated) {
    bool    c  = false;
    uint8_t st = slot < PROF_SLOTS ? writable() : PROF_BAD;
    if (st == PROF_OK) {
        uint8_t rec[PROF_REC], name[PROF_NAME];
        memset(name, 0, sizeof name);
        if (prof_slot(slot, rec) && (rec[PR_FIELDS] & PF_NAME)) memcpy(name, &rec[PR_NAME], PROF_NAME);
        if (!prof_formatted() && !format(&c)) {
            st = PROF_WRITE_FAILED;
        } else {
            prof_capture(rec, PF_PROFILE);
            memcpy(&rec[PR_NAME], name, PROF_NAME);
            rec[PR_CRC] = body_crc(rec);
            /* The slot empty first, then the body and crc, then the state in
             * its own write: a cut between them leaves the slot empty, never
             * a mix of old and new a crc8 could pass. The active slot after,
             * deferred (set_active()). */
            uint8_t o = PROF_REC_OFF(slot);
            if (!wr8(o + PR_STATE, PS_EMPTY, &c) || !wr(o + PR_FIELDS, &rec[PR_FIELDS], PROF_REC - 1u, &c) ||
                !wr8(o + PR_STATE, PS_SAVED_V1, &c))
                st = PROF_WRITE_FAILED;
            else
                set_active(slot);
        }
    }
    if (consolidated) *consolidated = c;
    return st;
}

/* P5c: the name, and the crc; the state empty first and saved last, each in
 * its own write, as a save's (a cut between leaves the slot empty, never a
 * name the crc doesn't cover). The fields byte too when it lacked the name
 * bit (a restored record's). The active slot doesn't move. */
uint8_t prof_rename(uint8_t slot, const char *name) {
    bool    c = false;
    uint8_t rec[PROF_REC], st = slot < PROF_SLOTS ? writable() : PROF_BAD;
    if (st != PROF_OK) return st;
    if (!prof_slot(slot, rec)) return PROF_INVALID;
    bool had = (rec[PR_FIELDS] & PF_NAME) != 0u;
    memset(&rec[PR_NAME], 0, PROF_NAME);
    for (uint8_t i = 0; i < PROF_NAME && name[i]; i++) rec[PR_NAME + i] = (uint8_t)name[i];
    rec[PR_FIELDS] |= PF_NAME;
    rec[PR_CRC] = body_crc(rec);
    if (!prof_rec_valid(rec)) return PROF_INVALID;   /* a character outside 0x20-0x7E */
    uint8_t o = PROF_REC_OFF(slot), from = had ? PR_NAME : PR_FIELDS;
    if (!wr8(o + PR_STATE, PS_EMPTY, &c) || !wr((uint8_t)(o + from), &rec[from], (uint8_t)(PROF_REC - from), &c) ||
        !wr8(o + PR_STATE, PS_SAVED_V1, &c))
        return PROF_WRITE_FAILED;
    return PROF_OK;
}

uint8_t prof_delete(uint8_t slot) {
    bool    c  = false;
    uint8_t st = slot < PROF_SLOTS ? writable() : PROF_BAD;
    if (st != PROF_OK || !prof_formatted()) return st;
    if (!wr8(PROF_REC_OFF(slot) + PR_STATE, PS_EMPTY, &c)) return PROF_WRITE_FAILED;
    if (active_byte() == slot) set_active(0xFFu);
    return PROF_OK;
}

uint8_t prof_load(uint8_t slot) {
    uint8_t rec[PROF_REC], st = writable();
    if (st != PROF_OK) return st;
    if (!prof_slot(slot, rec)) return PROF_INVALID;
    st = prof_apply(rec, false);
    set_active(slot);
    return st;
}

static void cancel_previews(void);   /* below, with HC_PROFILES' */

uint8_t prof_step(bool next, uint8_t *slot) {
    /* From the active slot, in slot order, skipping empties and wrapping
     * round to it again (one saved slot: a press reloads it, clearing its
     * `*`); from none, Next the first saved and Previous the last. */
    uint8_t rec[PROF_REC], a = prof_active(), st = writable();
    if (st != PROF_OK) return st;
    for (uint8_t i = 1; i <= PROF_SLOTS; i++) {
        uint8_t s = a >= PROF_SLOTS ? (uint8_t)(next ? i - 1u : PROF_SLOTS - i)
                                    : (uint8_t)((a + (next ? i : PROF_SLOTS - i)) % PROF_SLOTS);
        if (prof_slot(s, rec)) {
            *slot = s;
            cancel_previews();   /* an editor's preview, as any external write (`Cancelled`) */
            return prof_load(s);
        }
    }
    return PROF_INVALID;
}

/* --- HC_PROFILES -------------------------------------------------------------- */

bool prof_restore_open(void) { return (tx_state & TX_OPEN) != 0u; }

/* A restore's end: closed, its pages forgotten, the failure latch and the
 * changed bit kept. */
static void tx_close(void) { tx_state &= TX_FAILED | TX_CHANGED; }

bool prof_take_changed(void) {
    bool c = (tx_state & TX_CHANGED) != 0u;
    tx_state &= (uint8_t)~TX_CHANGED;
    return c;
}

/* The pending active slot, written now: dropped if the block isn't
 * formatted (an aborted restore left it so; the first save formats it). A
 * failure latches as any profile write's (wr()): the menu says "Save
 * failed" when it sees the latch rise, and the flush reports it (codex,
 * P5b's review: it was silent). */
static void write_active(void) {
    bool    c = false;
    uint8_t a = active_byte();
    pend      = 0u;
    if (prof_formatted()) (void)wr8(PROF_OFF_ACTIVE, a, &c);
}

void prof_tick(void) {
    if (prof_restore_open() && --tx_left == 0u) tx_close();   /* aborted: the block stays unformatted */
    /* After kb_eeconfig's settled write (it runs just before this in the
     * 10 Hz block), never in a recovery or a restore. */
    if (pend && !env_settings_pending() && writable() == PROF_OK) write_active();
}

#ifdef MENU_HOST_TEST
void prof_test_reset(void) { tx_token = tx_state = tx_left = pend = 0u; }
#endif

/* An open edit's preview back first, as VIA's lighting reads do
 * (hid_protocol.c's via_command_kb()): a backup never holds one, an apply
 * replaces it, and RGB's forced flush would otherwise write it. */
static void cancel_previews(void) {
    menu_edit_external(ME_DOM_RGB);
    menu_edit_external(ME_DOM_LCD);
    menu_edit_external(ME_DOM_IND);
}

/* The page p's span of the block. */
static uint8_t page_len(uint8_t p) {
    uint8_t off = (uint8_t)(p * PROF_PAGE);
    return (uint8_t)(PROF_BLOCK_SIZE - off < PROF_PAGE ? PROF_BLOCK_SIZE - off : PROF_PAGE);
}

/* The restore's commit: every page received; each record empty or valid; the
 * active byte normalized; the magic last, in its own write. */
static uint8_t commit(void) {
    bool    c = false;
    uint8_t rec[PROF_REC];
    if ((tx_state & TX_PAGES) != PROF_PAGES) return PROF_INCOMPLETE;
    uint8_t a = rd8(PROF_OFF_ACTIVE);
    for (uint8_t s = 0; s < PROF_SLOTS; s++) {
        rd(PROF_REC_OFF(s), rec, PROF_REC);
        if (rec[PR_STATE] == PS_EMPTY) {
            if (a == s) a = 0xFFu;
        } else if (!prof_rec_valid(rec)) {
            return PROF_INVALID;
        }
    }
    if (a >= PROF_SLOTS) a = 0xFFu;
    return wr8(PROF_OFF_ACTIVE, a, &c) && wr8(PROF_OFF_MAGIC, PROF_MAGIC_V2, &c) ? PROF_OK : PROF_WRITE_FAILED;
}

/* The report in place: [0] SET_VALUE, [1] 0x13, [2] 0x10, [3] the sub; VIA
 * echoes it back. A block page is 26 B at [6..31], so the block is nine.
 *   0  read page p:   [4] p                -> [5] status, [6..31] its bytes (zero past the block's end); the
 *                                             active byte as it will be, a pending choice included (P5b)
 *   1  begin:         [5] token            -> [6] status: aborts an open restore, writes the magic to 0 (a torn
 *                                             restore reads as eight empty slots), and opens one
 *   2  write page p:  [4] p, [5] token, [6..31] -> [6] status: straight into the block, the magic's byte
 *                                             masked (the host never writes it). In order (P5d): page p
 *                                             only once 0 to p - 1 are in (PROF_BAD before), a page again
 *                                             as often as it likes
 *   3  commit:        [5] token            -> [6] status (commit(), above); the transaction closes either way
 *   4  the live record                     -> [4..30] it (fields PF_LIVE), [31] status
 *   5  apply one:     [4..30] a record     -> [31] status (PROF_OK_EFFECT_SKIPPED: an effect this build lacks)
 *   6  flush                               -> [4] status: kb_eeconfig's and the RGB config's deferred writes now,
 *                                             and the pending active slot's (P5b): WRITE_FAILED if it failed, or
 *                                             while the latch holds (any profile write failed since the boot)
 *   7  the geometry (P5d)                  -> [4] slots, [5] a record's bytes, [6] pages, [7] a page's bytes,
 *                                             [8] the magic. P5a's and P5b's builds answer it unhandled: the
 *                                             host takes them for four slots, five pages, magic 0xA1
 * 4, 5 and 6 cancel an open preview first. Dev builds: 0x40 save the live
 * settings to slot [4] -> [5] status, [6] 1 when the wear-leveling layer
 * consolidated; 0x41 delete slot [4] -> [5] status. The subs that write the
 * block (1, 2, 3, 0x40, 0x41) mark it changed for the menu. */
bool prof_hid(uint8_t *d) {
    uint8_t p = d[4];
    bool    c = false;
    switch (d[3]) {
        case 0:
            memset(&d[5], 0, 27);
            if (p >= PROF_PAGES) {
                d[5] = PROF_BAD;
                return true;
            }
            rd((uint8_t)(p * PROF_PAGE), &d[6], page_len(p));
            if (p == 0u && pend) d[6 + PROF_OFF_ACTIVE] = active_byte();   /* a backup has the choice the board will write */
            return true;
        case 1:
            tx_close();
            pend = 0u;   /* the restore's active byte wins */
            tx_state |= TX_CHANGED;
            if (tx_state & TX_FAILED) {
                d[6] = PROF_WRITE_FAILED;
                return true;
            }
            if (env_recovering()) {
                d[6] = PROF_BUSY;
                return true;
            }
            if (!wr8(PROF_OFF_MAGIC, 0x00u, &c)) {
                d[6] = PROF_WRITE_FAILED;
                return true;
            }
            tx_token = d[5];
            tx_state = TX_OPEN | TX_CHANGED;   /* the latch is clear here */
            tx_left  = TX_TICKS;
            d[6]     = PROF_OK;
            return true;
        case 2: {
            if (!prof_restore_open() || d[5] != tx_token) {
                d[6] = PROF_TOKEN;
                return true;
            }
            uint8_t got = tx_state & TX_PAGES;
            if (p >= PROF_PAGES || p > got || env_recovering()) {
                d[6] = p >= PROF_PAGES || p > got ? PROF_BAD : PROF_BUSY;
                return true;
            }
            uint8_t skip = p == 0u ? 1u : 0u;   /* the magic: the firmware writes it, at the commit */
            tx_state |= TX_CHANGED;
            if (!wr((uint8_t)(p * PROF_PAGE + skip), &d[6 + skip], (uint8_t)(page_len(p) - skip), &c)) {
                tx_close();
                d[6] = PROF_WRITE_FAILED;
                return true;
            }
            if (p == got) tx_state++;   /* the count, bits 0-4: the next page in order */
            tx_left = TX_TICKS;
            d[6]      = PROF_OK;
            return true;
        }
        case 3:
            if (!prof_restore_open() || d[5] != tx_token) {
                d[6] = PROF_TOKEN;
                return true;
            }
            if (env_recovering()) {   /* the transaction stays open: the host retries */
                d[6] = PROF_BUSY;
                return true;
            }
            d[6] = commit();
            tx_close();
            tx_state |= TX_CHANGED;
            return true;
        case 4:
            cancel_previews();
            memset(&d[4], 0, 28);
            prof_capture(&d[4], PF_LIVE);
            d[31] = PROF_OK;
            return true;
        case 5:
            if (d[4 + PR_STATE] != PS_SAVED_V1) {
                d[31] = PROF_FORMAT;
            } else if (!prof_rec_valid(&d[4])) {
                d[31] = PROF_INVALID;
            } else if (env_recovering()) {
                d[31] = PROF_BUSY;
            } else {
                cancel_previews();
                d[31] = prof_apply(&d[4], true);
            }
            return true;
        case 6:
            cancel_previews();
            memset(&d[4], 0, 28);
            if (env_recovering()) {
                d[4] = PROF_BUSY;
                return true;
            }
            env_settings_flush();
            if (pend && writable() == PROF_OK) write_active();   /* the choice durable too */
            /* WRITE_FAILED while the latch holds: this write's failure, or any
             * profile write's since the boot, a deferred one included, whose
             * pending choice is gone (codex, both rounds). The cache can't
             * vouch for the flash until a reboot. */
            d[4] = prof_write_failed() ? PROF_WRITE_FAILED : PROF_OK;
            return true;
        case 7:
            memset(&d[4], 0, 28);
            d[4] = PROF_SLOTS;
            d[5] = PROF_REC;
            d[6] = PROF_PAGES;
            d[7] = PROF_PAGE;
            d[8] = PROF_MAGIC_V2;
            return true;
#if defined(AK820_DEV_HOOKS) || defined(MENU_HOST_TEST)
        case 0x40:
            cancel_previews();   /* P5e: the live settings, never a preview's */
            d[5] = prof_save(p, &c);
            d[6] = c ? 1u : 0u;
            tx_state |= TX_CHANGED;
            return true;
        case 0x41:
            d[5] = prof_delete(p);
            tx_state |= TX_CHANGED;
            return true;
#endif
        default:
            return false;
    }
}
