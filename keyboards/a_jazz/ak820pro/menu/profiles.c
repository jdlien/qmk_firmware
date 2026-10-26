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

/* A restore's transaction (3 B): the host's token; the pages received (bits
 * 0-4), the open bit, and the write-failure latch; a countdown in 10 Hz
 * ticks, 5 s without a page aborting it.
 * The latch (codex, P5a's review): wear_leveling_write() updates its RAM
 * cache BEFORE it programs the flash, so after a failed write the cache
 * holds bytes the flash may not. A retry of the same bytes would then be
 * skipped as unchanged and commit a slot whose body never landed. So the
 * first failure refuses every later profile write until a reboot reloads
 * the cache from the flash, which then shows the truth. Aborts keep it. */
#define TX_OPEN   0x80u
#define TX_FAILED 0x40u
#define TX_PAGES  0x1Fu
#define TX_TICKS  50u
static uint8_t tx_token, tx_state, tx_left;

/* CRC-8, polynomial 0x07, initial 0xFF, MSB first (hostagent/ak820profiles.py
 * computes the same). */
uint8_t prof_crc8(const uint8_t *p, uint8_t n) {
    uint8_t c = 0xFFu;
    while (n--) {
        c ^= *p++;
        for (uint8_t i = 0; i < 8u; i++) c = (uint8_t)((c & 0x80u) ? (uint8_t)(c << 1) ^ 0x07u : (uint8_t)(c << 1));
    }
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
    if ((f & PF_BVIEW) && rec[PR_BVIEW] != PROF_BVIEW_PERCENT && rec[PR_BVIEW] != PROF_BVIEW_HOURS) return false;
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

bool prof_formatted(void) { return rd8(PROF_OFF_MAGIC) == PROF_MAGIC_V1; }

bool prof_slot(uint8_t slot, uint8_t *rec) {
    if (slot >= PROF_SLOTS || !prof_formatted()) {
        memset(rec, 0, PROF_REC);
        return false;
    }
    rd(PROF_REC_OFF(slot), rec, PROF_REC);
    return prof_rec_valid(rec);
}

uint8_t prof_active(void) {
    uint8_t rec[PROF_REC], a = prof_formatted() ? rd8(PROF_OFF_ACTIVE) : 0xFFu;
    return a < PROF_SLOTS && prof_slot(a, rec) ? a : 0xFFu;
}

/* A fresh flash leaves the block unformatted. The first write formats it:
 * the states, the active byte, then the magic last, so the first save shows
 * and a stale byte from before the flash never reads as a record. */
static bool format(bool *consolidated) {
    for (uint8_t s = 0; s < PROF_SLOTS; s++)
        if (!wr8(PROF_REC_OFF(s) + PR_STATE, PS_EMPTY, consolidated)) return false;
    return wr8(PROF_OFF_ACTIVE, 0xFFu, consolidated) && wr8(PROF_OFF_MAGIC, PROF_MAGIC_V1, consolidated);
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

void prof_capture(uint8_t *rec, uint8_t fields) {
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
    rec[PR_BVIEW]    = env_batt_hours() ? PROF_BVIEW_HOURS : PROF_BVIEW_PERCENT;
    if (fields & PF_POWER) rec[PR_POWER] = env_power_mode();
    rec[PR_CRC] = body_crc(rec);
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
    if (f & PF_BVIEW) env_batt_hours_set(rec[PR_BVIEW] == PROF_BVIEW_HOURS);
    if (live && (f & PF_POWER)) env_power_set(rec[PR_POWER]);   /* never a profile's (Decided 2) */
    return st;
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
             * a mix of old and new a crc8 could pass. */
            uint8_t o = PROF_REC_OFF(slot);
            if (!wr8(o + PR_STATE, PS_EMPTY, &c) || !wr(o + PR_FIELDS, &rec[PR_FIELDS], PROF_REC - 1u, &c) ||
                !wr8(o + PR_STATE, PS_SAVED_V1, &c) || !wr8(PROF_OFF_ACTIVE, slot, &c))
                st = PROF_WRITE_FAILED;
        }
    }
    if (consolidated) *consolidated = c;
    return st;
}

uint8_t prof_delete(uint8_t slot) {
    bool    c  = false;
    uint8_t st = slot < PROF_SLOTS ? writable() : PROF_BAD;
    if (st != PROF_OK || !prof_formatted()) return st;
    bool was_active = rd8(PROF_OFF_ACTIVE) == slot;
    if (!wr8(PROF_REC_OFF(slot) + PR_STATE, PS_EMPTY, &c) || (was_active && !wr8(PROF_OFF_ACTIVE, 0xFFu, &c)))
        return PROF_WRITE_FAILED;
    return PROF_OK;
}

uint8_t prof_load(uint8_t slot) {
    bool    c = false;
    uint8_t rec[PROF_REC], st = writable();
    if (st != PROF_OK) return st;
    if (!prof_slot(slot, rec)) return PROF_INVALID;
    st = prof_apply(rec, false);
    return wr8(PROF_OFF_ACTIVE, slot, &c) ? st : PROF_WRITE_FAILED;
}

/* --- HC_PROFILES -------------------------------------------------------------- */

bool prof_restore_open(void) { return (tx_state & TX_OPEN) != 0u; }

/* A restore's end: closed, its pages forgotten, the failure latch kept. */
static void tx_close(void) { tx_state &= TX_FAILED; }

void prof_tick(void) {
    if (prof_restore_open() && --tx_left == 0u) tx_close();   /* aborted: the block stays unformatted */
}

#ifdef MENU_HOST_TEST
void prof_test_reset(void) { tx_token = tx_state = tx_left = 0u; }
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
    if ((tx_state & TX_PAGES) != TX_PAGES) return PROF_INCOMPLETE;
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
    return wr8(PROF_OFF_ACTIVE, a, &c) && wr8(PROF_OFF_MAGIC, PROF_MAGIC_V1, &c) ? PROF_OK : PROF_WRITE_FAILED;
}

/* The report in place: [0] SET_VALUE, [1] 0x13, [2] 0x10, [3] the sub; VIA
 * echoes it back. A block page is 26 B at [6..31], so the block is five.
 *   0  read page p:   [4] p                -> [5] status, [6..31] its bytes (zero past the block's end)
 *   1  begin:         [5] token            -> [6] status: aborts an open restore, writes the magic to 0 (a torn
 *                                             restore reads as four empty slots), and opens one
 *   2  write page p:  [4] p, [5] token, [6..31] -> [6] status: straight into the block, the magic's byte
 *                                             masked (the host never writes it); the page marked received
 *   3  commit:        [5] token            -> [6] status (commit(), above); the transaction closes either way
 *   4  the live record                     -> [4..30] it (fields PF_LIVE), [31] status
 *   5  apply one:     [4..30] a record     -> [31] status (PROF_OK_EFFECT_SKIPPED: an effect this build lacks)
 *   6  flush                               -> [4] status: kb_eeconfig's and the RGB config's deferred writes now
 * 4, 5 and 6 cancel an open preview first. Dev builds: 0x40 save the live
 * settings to slot [4] -> [5] status, [6] 1 when the wear-leveling layer
 * consolidated; 0x41 delete slot [4] -> [5] status. */
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
            return true;
        case 1:
            tx_close();
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
            tx_state = TX_OPEN;   /* the latch is clear here */
            tx_left  = TX_TICKS;
            d[6]     = PROF_OK;
            return true;
        case 2: {
            if (!prof_restore_open() || d[5] != tx_token) {
                d[6] = PROF_TOKEN;
                return true;
            }
            if (p >= PROF_PAGES || env_recovering()) {
                d[6] = p >= PROF_PAGES ? PROF_BAD : PROF_BUSY;
                return true;
            }
            uint8_t skip = p == 0u ? 1u : 0u;   /* the magic: the firmware writes it, at the commit */
            if (!wr((uint8_t)(p * PROF_PAGE + skip), &d[6 + skip], (uint8_t)(page_len(p) - skip), &c)) {
                tx_close();
                d[6] = PROF_WRITE_FAILED;
                return true;
            }
            tx_state |= (uint8_t)(1u << p);
            tx_left   = TX_TICKS;
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
            d[4] = PROF_OK;
            return true;
#if defined(AK820_DEV_HOOKS) || defined(MENU_HOST_TEST)
        case 0x40:
            d[5] = prof_save(p, &c);
            d[6] = c ? 1u : 0u;
            return true;
        case 0x41:
            d[5] = prof_delete(p);
            return true;
#endif
        default:
            return false;
    }
}
