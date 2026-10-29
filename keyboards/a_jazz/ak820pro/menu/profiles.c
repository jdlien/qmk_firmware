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
/* 10-28: the menu's face, in PR_BVIEW beside the readout (bits 0-1) under
 * PF_BVIEW: bit 2 Small, bit 3 Regular, neither Large (Terminus Bold), as
 * kb_eeconfig's byte. Every record carries it since JD's "profiles respect
 * the font size setting": the capture, the `*`'s compare, the range check
 * and the apply (menu_font_set(), the one way to change it). A record saved
 * before has the bits clear and loads as Bold, the default. */
#define PROF_BVIEW_VIEW    0x03u
#define PROF_BVIEW_SMALL   0x04u
#define PROF_BVIEW_REG     0x08u
#define PROF_BVIEW_FACE    (PROF_BVIEW_SMALL | PROF_BVIEW_REG)
static uint8_t face_bits(uint8_t font) {
    return font == ENV_FONT_SMALL ? PROF_BVIEW_SMALL : font == ENV_FONT_REGULAR ? PROF_BVIEW_REG : 0u;
}
static uint8_t face_of(uint8_t bview) {
    uint8_t f = bview & PROF_BVIEW_FACE;
    return f == PROF_BVIEW_SMALL ? ENV_FONT_SMALL : f == PROF_BVIEW_REG ? ENV_FONT_REGULAR : ENV_FONT_LARGE;
}

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
/* The active byte's choice still to be written (P5b), 1 B, the pair as it
 * will be, while SD_PEND is set. The menu's Load, Save and Delete, the Next
 * and Previous keys and the switch's flips all set it here, one writer and
 * one rule: it rides kb_eeconfig's settle (env_settings_touch()) and
 * prof_tick() writes it once that write is done, so a run of presses costs
 * one write and a deep sleep waits on it (DSB_FLASH). The records' writes
 * stay at the click. */
static uint8_t pend;
_Static_assert(PROF_SLOTS <= 0xFu, "a side's slot in its nibble, 0xF none");
/* The Win/Mac switch (10-27), 1 B: the side it reads (SD_MAC), whether the
 * boot's reading came (SD_KNOWN), the side the profiles go by (SD_SETTLED:
 * the Mac's when set), the pair pending, a flip's countdown in 10 Hz ticks,
 * and the boot's check still to run (SD_BOOT, prof_tick()). The side
 * settles once the switch has rested SD_REST ticks, so a bouncing contact
 * loads once and a bounce back loads nothing; until then a Load or a Save
 * goes to the side it was on. */
#define SD_MAC     0x01u
#define SD_KNOWN   0x02u
#define SD_SETTLED 0x04u
#define SD_PEND    0x08u
#define SD_TICKS   0x70u
#define SD_TICK    0x10u
#define SD_BOOT    0x80u
#define SD_REST    3u   /* 200-300 ms */
static uint8_t side;

static uint8_t cur_side(void) { return (side & SD_SETTLED) ? PROF_SIDE_MAC : PROF_SIDE_WIN; }
static uint8_t nib(uint8_t pair, uint8_t sd) { return (uint8_t)((pair >> (sd * 4u)) & 0x0Fu); }
/* The pair with each side that remembers slot s (or a nibble past the
 * profiles, s >= PROF_USER: the last record's too) set to none. */
static uint8_t drop(uint8_t pair, uint8_t s) {
    for (uint8_t sd = PROF_SIDE_WIN; sd <= PROF_SIDE_MAC; sd++) {
        uint8_t n = nib(pair, sd);
        if (n == s || (s >= PROF_USER && n >= PROF_USER)) pair |= (uint8_t)(0x0Fu << (sd * 4u));
    }
    return pair;
}

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

/* The crc and every present field in range, whatever the state. */
static bool body_ok(const uint8_t *rec) {
    if (rec[PR_CRC] != body_crc(rec)) return false;
    uint8_t f = rec[PR_FIELDS];
    if ((f & PF_LIGHTING) && rec[PR_RGB_ON] > 1u) return false;
    if ((f & PF_LCD) && rec[PR_LCD] > MENU_LCD_MAX) return false;
    if ((f & PF_LOCKS) && rec[PR_LOCKS] > ENV_IND_MAX) return false;
    if ((f & PF_CHARGING) && rec[PR_CHARGING] > ENV_IND_MAX) return false;
    if ((f & PF_CLOCK) && rec[PR_CLOCK] >= PROF_CLOCK_MODES) return false;
    if (f & PF_BVIEW) {   /* the readout 1-3, one face or none, nothing else */
        uint8_t b = rec[PR_BVIEW], v = (uint8_t)(b & PROF_BVIEW_VIEW);
        if (v < PROF_BVIEW_PERCENT || v > PROF_BVIEW_BOTH || (b & ~(PROF_BVIEW_VIEW | PROF_BVIEW_FACE)) ||
            (b & PROF_BVIEW_FACE) == PROF_BVIEW_FACE)
            return false;
    }
    if ((f & PF_POWER) && rec[PR_POWER] >= PROF_POWER_MODES) return false;
    if ((f & PF_NAME) && !name_ok(&rec[PR_NAME])) return false;
    return true;
}
bool prof_rec_valid(const uint8_t *rec) { return rec[PR_STATE] == PS_SAVED_V1 && body_ok(rec); }
/* The last record, PS_LAST | a side, a profile's settings and nothing else. */
static bool last_valid(const uint8_t *rec) {
    return (rec[PR_STATE] & ~1u) == PS_LAST && !(rec[PR_FIELDS] & ~PF_SIDE) && body_ok(rec);
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

bool prof_formatted(void) { return rd8(PROF_OFF_MAGIC) == PROF_MAGIC_V4; }

bool prof_slot(uint8_t slot, uint8_t *rec) {
    if (slot >= PROF_USER || !prof_formatted()) {
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
    for (uint8_t s = 0; s < PROF_USER; s++) {
        rd(PROF_REC_OFF(s), rec, PROF_REC);
        if (prof_rec_valid(rec)) v |= (uint8_t)(1u << s);
    }
    return v;
}
_Static_assert(PROF_SLOTS <= 8u, "a mask's byte");

uint8_t prof_free_slot(void) {
    uint8_t v = prof_valid_mask();
    for (uint8_t s = 0; s < PROF_USER; s++)
        if (!((v >> s) & 1u)) return s;
    return 0xFFu;
}

/* The active byte as it will be: the pending pair, else the block's. */
static uint8_t active_byte(void) { return (side & SD_PEND) ? pend : rd8(PROF_OFF_ACTIVE); }
static void    set_pair(uint8_t pair) {
    pend = pair;
    side |= SD_PEND;
    env_settings_touch();
}
/* The side the profiles go by: its slot a (0xFF none). */
static void set_active(uint8_t a) {
    uint8_t sh = (uint8_t)(cur_side() * 4u);
    set_pair((uint8_t)((active_byte() & ~(0x0Fu << sh)) | ((a & 0x0Fu) << sh)));
}

uint8_t prof_active_rec(uint8_t *rec) {
    uint8_t a = prof_formatted() ? nib(active_byte(), cur_side()) : 0xFFu;
    return a < PROF_USER && prof_slot(a, rec) ? a : 0xFFu;
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
    side &= (uint8_t)~SD_PEND;   /* a fresh block's pair is none, none */
    for (uint8_t s = 0; s < PROF_SLOTS; s++)
        if (!wr8(PROF_REC_OFF(s) + PR_STATE, PS_EMPTY, consolidated)) return false;
    return wr8(PROF_OFF_ACTIVE, 0xFFu, consolidated) && wr8(PROF_OFF_MAGIC, PROF_MAGIC_V4, consolidated);
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

/* --- The last record (10-27) --------------------------------------------------
 * The eighth record holds the settings the side not in use had as it was
 * left (PS_LAST | that side), so a flip back brings them back, the `*`
 * included: JD, 10-27, "don't lose your settings when switching between
 * mac/windows". Only a side with a profile keeps its settings. */

/* The last record into rec: true when it holds side sd's. */
static bool last_of(uint8_t sd, uint8_t *rec) {
    if (!prof_formatted()) return false;
    rd(PROF_REC_OFF(PROF_LAST), rec, PROF_REC);
    return last_valid(rec) && rec[PR_STATE] == (uint8_t)(PS_LAST | sd);
}
/* rec as the last record, in a save's order: the state empty, the body and
 * crc, the state in its own write (a cut leaves it empty). */
static void last_put(const uint8_t *rec) {
    bool    c = false;
    uint8_t o = PROF_REC_OFF(PROF_LAST);
    if (wr8(o + PR_STATE, PS_EMPTY, &c) && wr(o + PR_FIELDS, &rec[PR_FIELDS], PROF_REC - 1u, &c))
        (void)wr8(o + PR_STATE, rec[PR_STATE], &c);
}
static void last_clear(void) {
    bool    c = false;
    uint8_t o = PROF_REC_OFF(PROF_LAST);
    if (prof_formatted() && rd8(o + PR_STATE) != PS_EMPTY) (void)wr8(o + PR_STATE, PS_EMPTY, &c);
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
    rec[PR_BVIEW]    = (uint8_t)(env_batt_view() | face_bits(env_font()));   /* 10-28: the face beside it */
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
    if (f & PF_BVIEW) {
        env_batt_view_set(rec[PR_BVIEW] & PROF_BVIEW_VIEW);
        menu_font_set(face_of(rec[PR_BVIEW]));   /* 10-28: re-lays out an open menu, redraws the song text */
    }
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
    uint8_t st = slot < PROF_USER ? writable() : PROF_BAD;
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
                !wr8(o + PR_STATE, PS_SAVED_V1, &c)) {
                st = PROF_WRITE_FAILED;
            } else {
                set_active(slot);
                /* 10-27: the other side's settings as left came from this
                 * profile, so they'd bring the old one back: it loads anew. */
                uint8_t o2 = (uint8_t)(cur_side() ^ 1u);
                if (nib(active_byte(), o2) == slot && last_of(o2, rec)) last_clear();
            }
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
    uint8_t rec[PROF_REC], st = slot < PROF_USER ? writable() : PROF_BAD;
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
    uint8_t st = slot < PROF_USER ? writable() : PROF_BAD;
    if (st != PROF_OK || !prof_formatted()) return st;
    if (!wr8(PROF_REC_OFF(slot) + PR_STATE, PS_EMPTY, &c)) return PROF_WRITE_FAILED;
    uint8_t a = active_byte(), b = drop(a, slot), last[PROF_REC];   /* either side's */
    if (b != a) set_pair(b);
    /* A side without a profile keeps nothing of its own (10-27): its
     * settings as left go with it. */
    for (uint8_t sd = PROF_SIDE_WIN; sd <= PROF_SIDE_MAC; sd++)
        if (nib(a, sd) == slot && last_of(sd, last)) last_clear();
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
    for (uint8_t i = 1; i <= PROF_USER; i++) {
        uint8_t s = a >= PROF_USER ? (uint8_t)(next ? i - 1u : PROF_USER - i)
                                    : (uint8_t)((a + (next ? i : PROF_USER - i)) % PROF_USER);
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
    side &= (uint8_t)~SD_PEND;
    if (prof_formatted()) (void)wr8(PROF_OFF_ACTIVE, a, &c);
}

/* A flip settled. A side with a profile keeps its settings (10-27): the side
 * left goes into the last record as it was, and the side arrived at comes
 * back from it as it was left, else its profile loads, as the Next and
 * Previous keys load one; either named (env_prof_switched()). A side
 * without one keeps nothing of its own: arriving at it changes nothing. A
 * bounce back to the side it was on changes nothing either. Refused by the
 * latch: nothing applied or kept, the side still moves.
 * Codex (10-27): an open preview goes back before anything is kept, so the
 * side's own settings are, never a preview's. And the side arrived at's
 * settings are saved at once, before the side left's record: the change of
 * computers that flips this switch often flips the slider too, which browns
 * the board out, and a cut in the settings' deferred write (5 s) would find
 * the record moved on while the saved settings were still the old side's.
 * A cut between the two writes loses the side left's changes only. */
static void settle_side(void) {
    uint8_t now = (side & SD_MAC) ? SD_SETTLED : 0u;
    if ((side & SD_SETTLED) == now) return;
    uint8_t keep[PROF_REC], rec[PROF_REC];
    uint8_t x     = cur_side();
    bool    kept  = prof_active_rec(rec) < PROF_USER;
    side          = (uint8_t)((side & ~SD_SETTLED) | now);
    uint8_t s     = prof_active_rec(rec), st = writable();
    bool    taken = false;
    if (st == PROF_OK && (kept || s < PROF_USER)) {
        cancel_previews();
        if (kept) {   /* the side left, as it is now */
            prof_capture(keep, PF_SIDE);
            keep[PR_STATE] = (uint8_t)(PS_LAST | x);
        }
        if (s < PROF_USER) {
            taken = last_of(cur_side(), rec);   /* read before the side left's goes in */
            st    = taken ? prof_apply(rec, false) : prof_load(s);
            env_settings_flush();
        }
        if (kept)
            last_put(keep);
        else if (taken)
            last_clear();   /* used, and nothing to keep: no stale copy */
    }
    if (s < PROF_USER) env_prof_switched(s, st);
}

void prof_switch(bool mac) {
    uint8_t m = mac ? SD_MAC : 0u;
    if (!(side & SD_KNOWN)) {   /* the boot's (dip_switch_init(), before the EEPROM is read): no load, a check at the first tick */
        side = (uint8_t)((side & SD_PEND) | SD_KNOWN | SD_BOOT | m | (mac ? SD_SETTLED : 0u));
        return;
    }
    if ((side & SD_MAC) == m) return;   /* a reading again, not a change */
    side = (uint8_t)((side & ~(SD_MAC | SD_TICKS)) | m | (SD_REST * SD_TICK));
}

void prof_tick(void) {
    if (prof_restore_open() && --tx_left == 0u) tx_close();   /* aborted: the block stays unformatted */
    /* The boot's check (10-27): the last record holding this side's settings
     * means the switch moved while the board was off, so the live settings
     * are the other side's: settled as that side, then the flip's swap. A
     * boot that didn't move it (the slider's flips reboot the board) finds
     * the other side's there, and changes nothing. */
    if ((side & SD_BOOT) && writable() != PROF_BUSY) {
        uint8_t rec[PROF_REC];
        side &= (uint8_t)~SD_BOOT;
        if (prof_active_rec(rec) < PROF_USER && last_of(cur_side(), rec)) {
            side ^= SD_SETTLED;
            settle_side();
        }
    }
    /* A flip's countdown; at its end the load waits out a recovery or a
     * restore, as the keys' would be refused. */
    if ((side & SD_TICKS) && !((side & SD_TICKS) == SD_TICK && writable() == PROF_BUSY)) {
        side = (uint8_t)(side - SD_TICK);
        if (!(side & SD_TICKS)) settle_side();
    }
    /* After kb_eeconfig's settled write (it runs just before this in the
     * 10 Hz block), never in a recovery or a restore. */
    if ((side & SD_PEND) && !env_settings_pending() && writable() == PROF_OK) write_active();
}

#ifdef MENU_HOST_TEST
void prof_test_reset(void) { tx_token = tx_state = tx_left = pend = side = 0u; }
#endif

/* An open edit's preview back first, as VIA's lighting reads do
 * (hid_protocol.c's via_command_kb()): a backup never holds one, an apply
 * replaces it, and RGB's forced flush would otherwise write it. */
static void cancel_previews(void) {
    menu_edit_external(ME_DOM_RGB);
    menu_edit_external(ME_DOM_LCD);
    menu_edit_external(ME_DOM_IND);
    menu_edit_external(ME_DOM_FONT);   /* 10-28: the Font page's; an apply carries the face */
}

/* The page p's span of the block. */
static uint8_t page_len(uint8_t p) {
    uint8_t off = (uint8_t)(p * PROF_PAGE);
    return (uint8_t)(PROF_BLOCK_SIZE - off < PROF_PAGE ? PROF_BLOCK_SIZE - off : PROF_PAGE);
}

/* The restore's commit: every page received; each record empty or valid; the
 * active byte normalized, each side's; the magic last, in its own write. */
static uint8_t commit(void) {
    bool    c = false;
    uint8_t rec[PROF_REC];
    if ((tx_state & TX_PAGES) != PROF_PAGES) return PROF_INCOMPLETE;
    uint8_t a = drop(rd8(PROF_OFF_ACTIVE), PROF_USER);   /* a nibble past the profiles */
    for (uint8_t s = 0; s < PROF_SLOTS; s++) {
        rd(PROF_REC_OFF(s), rec, PROF_REC);
        if (rec[PR_STATE] == PS_EMPTY) {
            a = drop(a, s);
        } else if (s < PROF_USER ? !prof_rec_valid(rec) : !last_valid(rec)) {   /* the eighth, the last record */
            return PROF_INVALID;
        }
    }
    return wr8(PROF_OFF_ACTIVE, a, &c) && wr8(PROF_OFF_MAGIC, PROF_MAGIC_V4, &c) ? PROF_OK : PROF_WRITE_FAILED;
}

/* --- Reset settings (10-27) ------------------------------------------------- */

uint8_t prof_reset(void) {
    /* Not writable()'s answer: under the latch it says WRITE_FAILED before
     * it looks at a recovery (codex, 10-27). */
    if (env_recovering() || prof_restore_open()) return PROF_BUSY;
    uint8_t        st;
    env_defaults_t d;
    uint8_t        rec[PROF_REC];
    env_defaults(&d);
    memset(rec, 0, PROF_REC);
    rec[PR_STATE]    = PS_SAVED_V1;
    rec[PR_FIELDS]   = PF_LIVE;
    rec[PR_RGB_ON]   = d.rgb.enable ? 1u : 0u;
    rec[PR_EFFECT]   = effect_id_of(d.rgb.mode);
    rec[PR_HUE]      = d.rgb.h;
    rec[PR_SAT]      = d.rgb.s;
    rec[PR_VAL]      = d.rgb.v;
    rec[PR_SPEED]    = d.rgb.speed;
    rec[PR_LCD]      = d.lcd;
    rec[PR_LOCKS]    = d.locks;
    rec[PR_CHARGING] = d.charging;
    rec[PR_CLOCK]    = d.clock;
    rec[PR_BVIEW]    = d.bview;
    rec[PR_POWER]    = d.power;
    cancel_previews();
    st = prof_apply(rec, true);
    /* Both sides forget their profile, and the last record goes: a flip
     * changes nothing until a profile is loaded or saved again. Under the
     * latch the pair still changes in RAM, so until a reboot nothing loads. */
    if (prof_formatted()) {
        if (active_byte() != 0xFFu) set_pair(0xFFu);
        last_clear();
    }
    return st;
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
 *   4  the live record                     -> [4..30] it (fields PF_LIVE), [31] status. 10-28: PR_BVIEW's bit 2
 *                                             the face, set Small, the crc taken over it
 *   5  apply one:     [4..30] a record     -> [31] status (PROF_OK_EFFECT_SKIPPED: an effect this build lacks).
 *                                             10-28: the crc checked as it came; a record of exactly PF_LIVE
 *                                             has PR_BVIEW's bit 2 taken out before the checks and sets the
 *                                             face once applied (clear, as an earlier firmware's: Large); any
 *                                             other record leaves the face alone
 *   6  flush                               -> [4] status: kb_eeconfig's and the RGB config's deferred writes now,
 *                                             and the pending active slot's (P5b): WRITE_FAILED if it failed, or
 *                                             while the latch holds (any profile write failed since the boot)
 *   7  the geometry (P5d)                  -> [4] slots, [5] a record's bytes, [6] pages, [7] a page's bytes,
 *                                             [8] the magic, [9] the side the profiles go by (10-27: 1
 *                                             Windows, 2 the Mac, 0 before the switch's first reading; the
 *                                             builds before, 0). P5a's and P5b's builds answer it unhandled:
 *                                             the host takes them for four slots, five pages, magic 0xA1
 * 4, 5 and 6 cancel an open preview first. Dev builds: 0x40 save the live
 * settings to slot [4] -> [5] status, [6] 1 when the wear-leveling layer
 * consolidated; 0x41 delete slot [4] -> [5] status; 0x42 the switch read as
 * [4] (0 Windows, 1 the Mac), as a flip would read it -> [5] status (10-27:
 * the board's checks; put it back after). The subs that write the block
 * (1, 2, 3, 0x40, 0x41) mark it changed for the menu. */
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
            if (p == 0u && (side & SD_PEND)) d[6 + PROF_OFF_ACTIVE] = active_byte();   /* a backup has the choice the board will write */
            return true;
        case 1:
            tx_close();
            side &= (uint8_t)~SD_PEND;   /* the restore's active byte wins */
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
            prof_capture(&d[4], PF_LIVE);   /* the face in it, as every record's (10-28) */
            d[31] = PROF_OK;
            return true;
        case 5: {
            /* 10-28: the face is the record's own field (PR_BVIEW's bits
             * 2-3), checked and applied as any other: no unpacking. */
            uint8_t *r = &d[4];
            if (r[PR_STATE] != PS_SAVED_V1) {
                d[31] = PROF_FORMAT;
            } else if (!prof_rec_valid(r)) {
                d[31] = PROF_INVALID;
            } else if (env_recovering()) {
                d[31] = PROF_BUSY;
            } else {
                cancel_previews();   /* the Font page's preview too: "Cancelled", the restore's face shows */
                d[31] = prof_apply(r, true);
            }
            return true;
        }
        case 6:
            cancel_previews();
            memset(&d[4], 0, 28);
            if (env_recovering()) {
                d[4] = PROF_BUSY;
                return true;
            }
            env_settings_flush();
            if ((side & SD_PEND) && writable() == PROF_OK) write_active();   /* the choice durable too */
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
            d[8] = PROF_MAGIC_V4;
            d[9] = (side & SD_KNOWN) ? (uint8_t)(1u + cur_side()) : 0u;
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
        case 0x42:
            prof_switch(p != 0u);
            d[5] = PROF_OK;
            return true;
#endif
        default:
            return false;
    }
}
