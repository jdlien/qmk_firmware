// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Settings profiles (plans/SETTINGS-PROFILES-PLAN.md): four slots of the
// menu's settings in the top 110 B of the emulated EEPROM, and HC_PROFILES,
// the host's backup and restore. P5a: the store and the command; the menu's
// pages are P5b's, Rename P5c's. Everything it touches goes through
// menu_env.h, so the host tests run it on their fakes.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PROF_BLOCK_SIZE 110u
#define PROF_BASE       (1024u - PROF_BLOCK_SIZE)   /* 914: menu_glue.c asserts it's TOTAL_EEPROM_BYTE_COUNT - 110 */
#define PROF_SLOTS      4u
#define PROF_REC        27u
#define PROF_MAGIC_V1   0xA1u
#define PROF_PAGE       26u   /* HC_PROFILES' page: [6..31] of the report */
#define PROF_PAGES      5u    /* 4 x 26 + 6 */
#define PROF_NAME       12u

/* The block: the magic, the active slot (0xFF none), four records. */
enum { PROF_OFF_MAGIC = 0, PROF_OFF_ACTIVE = 1, PROF_OFF_REC = 2 };
#define PROF_REC_OFF(slot) ((uint8_t)(PROF_OFF_REC + (slot) * PROF_REC))
_Static_assert(PROF_OFF_REC + PROF_SLOTS * PROF_REC == PROF_BLOCK_SIZE, "the block is its header and four records");
_Static_assert((PROF_PAGES - 1u) * PROF_PAGE < PROF_BLOCK_SIZE && PROF_PAGES * PROF_PAGE >= PROF_BLOCK_SIZE, "five pages cover it");

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
uint8_t prof_active(void);                          /* 0-3; 0xFF none, or the slot isn't valid */
bool    prof_slot(uint8_t slot, uint8_t *rec);      /* the slot's record; true when valid */
void    prof_capture(uint8_t *rec, uint8_t fields); /* the live settings as a record, state and crc set */
uint8_t prof_apply(const uint8_t *rec, bool live);  /* PROF_OK, or PROF_OK_EFFECT_SKIPPED; the power mode only `live` */
uint8_t prof_save(uint8_t slot, bool *consolidated);   /* the live settings, keeping a valid slot's name; it becomes active */
uint8_t prof_delete(uint8_t slot);
uint8_t prof_load(uint8_t slot);
bool    prof_restore_open(void);                    /* an HC_PROFILES restore: the menu's writes wait */
void    prof_tick(void);                            /* 10 Hz: a restore's 5 s without a page aborts it */

/* HC_PROFILES (0x10 on the health channel), the whole 32 B report in place;
 * false for an unknown sub. Its framing is in profiles.c. */
bool prof_hid(uint8_t *data);
