// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Settings profiles (plans/SETTINGS-PROFILES-PLAN.md): eight slots of the
// menu's settings in the top 218 B of the emulated EEPROM, and HC_PROFILES,
// the host's backup and restore. P5a: the store and the command; P5b: what
// the menu's pages (menu.c) and the Next and Previous keys need; P5d (JD,
// 10-26: "8 feels about right"): eight slots, not four, and the list's Save
// new; Rename is P5c's. Everything it touches goes through
// menu_env.h, so the host tests run it on their fakes.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "menu_env.h"   /* env_rgb_t */

#define PROF_BLOCK_SIZE 218u
#define PROF_BASE       (1024u - PROF_BLOCK_SIZE)   /* 806: menu_glue.c asserts it's TOTAL_EEPROM_BYTE_COUNT - 218 */
#define PROF_SLOTS      8u
#define PROF_REC        27u
#define PROF_MAGIC_V2   0xA2u   /* P5d's eight slots; P5a's four were 0xA1 (hostagent/ak820profiles.py converts) */
#define PROF_PAGE       26u     /* HC_PROFILES' page: [6..31] of the report */
#define PROF_PAGES      9u      /* 8 x 26 + 10 */
#define PROF_NAME       12u

/* The block: the magic, the active slot (0xFF none), eight records. */
enum { PROF_OFF_MAGIC = 0, PROF_OFF_ACTIVE = 1, PROF_OFF_REC = 2 };
#define PROF_REC_OFF(slot) ((uint8_t)(PROF_OFF_REC + (slot) * PROF_REC))
_Static_assert(PROF_OFF_REC + PROF_SLOTS * PROF_REC == PROF_BLOCK_SIZE, "the block is its header and eight records");
_Static_assert(PROF_BLOCK_SIZE <= 255u, "the block's offsets are a byte");
_Static_assert((PROF_PAGES - 1u) * PROF_PAGE < PROF_BLOCK_SIZE && PROF_PAGES * PROF_PAGE >= PROF_BLOCK_SIZE, "nine pages cover it");

/* A record's bytes. The crc covers bytes 1-25, not the state, so a Delete or
 * a save's commit (the state alone) never invalidates it. */
enum {
    PR_STATE = 0, PR_FIELDS, PR_RGB_ON, PR_EFFECT, PR_HUE, PR_SAT, PR_VAL, PR_SPEED,
    PR_LCD, PR_LOCKS, PR_CHARGING, PR_CLOCK, PR_BVIEW, PR_POWER, PR_NAME, PR_CRC = PR_NAME + PROF_NAME
};
_Static_assert(PR_CRC == 26 && PR_CRC + 1 == PROF_REC, "the record is 27 B, its crc last");
enum { PS_EMPTY = 0x00, PS_SAVED_V1 = 0x01 };
/* The fields bits: a record without one leaves that setting alone at a load. */
enum {
    PF_LIGHTING = 1u << 0, PF_LCD = 1u << 1, PF_LOCKS = 1u << 2, PF_CHARGING = 1u << 3,
    PF_CLOCK = 1u << 4, PF_BVIEW = 1u << 5, PF_NAME = 1u << 6, PF_POWER = 1u << 7
};
#define PF_PROFILE 0x7Fu   /* a saved profile: everything but the power mode */
#define PF_LIVE    0xBFu   /* the live record (HC_PROFILES 4, 5): the power mode, no name */

/* A write's and HC_PROFILES' statuses. */
enum {
    PROF_OK = 0, PROF_BAD, PROF_TOKEN, PROF_BUSY, PROF_INVALID, PROF_WRITE_FAILED, PROF_INCOMPLETE,
    PROF_FORMAT, PROF_OK_EFFECT_SKIPPED
};

uint8_t prof_crc8(const uint8_t *p, uint8_t n);
bool    prof_rec_valid(const uint8_t *rec);         /* state 0x01, the crc, every present field in range */
bool    prof_formatted(void);
uint8_t prof_active(void);                          /* 0-3; 0xFF none, or the slot isn't valid. A pending choice counts */
uint8_t prof_active_rec(uint8_t *rec);              /* the same, its record in rec (PROF_REC) when valid */
bool    prof_slot(uint8_t slot, uint8_t *rec);      /* the slot's record; true when valid */
uint8_t prof_valid_mask(void);                      /* P5d: bit s set when slot s holds a valid record */
uint8_t prof_free_slot(void);                       /* P5d: the first slot without a valid record; 0xFF all saved */
void    prof_name(uint8_t slot, const uint8_t *rec, char *out);   /* PROF_NAME + 1: the name, or "Profile n" */
bool    prof_differs(const uint8_t *rec);           /* the live settings differ in a field it holds: the menu's `*` */
uint8_t prof_step(bool next, uint8_t *slot);        /* the Next or Previous key's load; PROF_INVALID: none saved */
bool    prof_take_changed(void);                    /* the host wrote the block since the last call: the menu repaints */
void    prof_capture(uint8_t *rec, uint8_t fields); /* the live settings as a record, state and crc set */
uint8_t prof_apply(const uint8_t *rec, bool live);  /* PROF_OK, or PROF_OK_EFFECT_SKIPPED; the power mode only `live` */
void    prof_preview(uint8_t slot, const env_rgb_t *base, uint8_t base_lcd);   /* P5e: unsaved, over the live settings */
uint8_t prof_save(uint8_t slot, bool *consolidated);   /* the live settings, keeping a valid slot's name; it becomes active */
uint8_t prof_delete(uint8_t slot);
uint8_t prof_rename(uint8_t slot, const char *name);   /* P5c: up to PROF_NAME chars, NUL-padded; "" for "Profile n" */
uint8_t prof_load(uint8_t slot);
bool    prof_restore_open(void);                    /* an HC_PROFILES restore: the menu's writes wait */
bool    prof_write_failed(void);                    /* a write failed: none more until a reboot (profiles.c) */
void    prof_tick(void);                            /* 10 Hz: a restore's 5 s without a page aborts it; the active slot's write */

/* HC_PROFILES (0x10 on the health channel), the whole 32 B report in place;
 * false for an unknown sub. Its framing is in profiles.c. */
bool prof_hid(uint8_t *data);
