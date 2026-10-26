// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The configuration menu's tree, P1a (plans/CONFIG-MENU-P1-DESIGN.md): the
// lists, the committing choices, Help's topics, About, Reset counters and
// Diagnostics' handoff, on the slot scheduler (C3) in lit text and half rows.
// Pure: settings and facts come through menu_env.h, the panel through
// menu_backend.h, so the host tests run it.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "menu_input.h"

typedef enum {
    MENU_NONE = 0,
    MENU_DIAG_HEALTH,   /* hand off to the Health page (C1) */
    MENU_DIAG_CAMERA,   /* hand off to the Camera page */
    MENU_CLOSE,         /* Exit, or Back at the top */
    MENU_BOOTLOADER,    /* the bootloader item: enter it, as Fn+Esc does */
} menu_result_t;

void          menu_open(uint32_t now);                    /* the top level, row 0, a fresh layout */
void          menu_return_from_diag(uint8_t page, uint32_t now);   /* 0 Health, 1 Camera: Diagnostics, the page's row */
void          menu_invalidate(void);                      /* a loss under the menu */
menu_result_t menu_input(int8_t turns, mi_latch_t latch, uint32_t now);
void          menu_tick(uint32_t now, uint32_t idle_ms);  /* each pass: the latched model and the title */
uint8_t       menu_screen_id(void);                       /* op 27 sub 5, byte 14: 16 + the node showing */
uint8_t       menu_cursor(void);                          /* byte 15: the cursor, a topic's top line, the confirm's page */
void          menu_bootloader_refused(uint32_t now);      /* env_bootloader() couldn't release the keys: "Try again" */
bool          menu_long_wait(void);                       /* P3: the Pairing page starting or advertising: the backstop */

/* P5c, Rename (plans/SETTINGS-PROFILES-PLAN.md, "Rename"): the one page that
 * takes typing. menu_key() sees every key event but the knob's, before QMK
 * does: true, Rename swallowed it. menu_rename_task() runs each pass, under
 * any owner: the capture's moves (WAIT to CAPTURE once the keys are quiet,
 * CAPTURE to DRAIN when the page goes, DRAIN to off once they're up). */
bool menu_key(uint8_t row, uint8_t col, bool pressed, uint32_t now);
bool menu_renaming(void);                                 /* WAIT or CAPTURE: the knob reads Fn as up */
void menu_rename_task(bool showing, uint32_t now);
/* QMK's basic keycodes Rename reads (HID usages; menu_glue.c checks them). */
#define MENU_KC_A        0x04u   /* to Z, 0x1D */
#define MENU_KC_1        0x1Eu   /* to 0, 0x27 */
#define MENU_KC_ENTER    0x28u
#define MENU_KC_ESCAPE   0x29u
#define MENU_KC_BSPC     0x2Au
#define MENU_KC_SPACE    0x2Cu
#define MENU_KC_MINUS    0x2Du   /* to the slash, 0x38 */
#define MENU_KC_KP_ENTER 0x58u
#define MENU_KC_LSHIFT   0xE1u
#define MENU_KC_RSHIFT   0xE5u

/* The palette: these colours, const on every build (dev's live tuning, op 27
 * sub 13, retired 10-25 for the settings profiles' RAM). */
enum { MP_LIT = 0, MP_DIM, MP_LEAVE, MP_WHITE, MP_GREY, MP_TRACK, MP_CURSOR, MP_N };
uint16_t menu_color(uint8_t i);

/* For the host tests' expected frames. */
#define MENU_ROWS      8
#define MENU_LCD_MAX   100u      /* the Screen's top level, 0-100 since P2b (display.c checks it against BKL_MAX_LEVEL) */
#define MENU_LIT       0xFF94u   /* the selected row, #FFF2A0 (JD, 10-24: lit text) */
#define MENU_DIM       0xCE59u   /* the other rows, #C8C8C8 */
#define MENU_LEAVE     0xFCD3u   /* "◀ Back" and Exit, not selected: light red, #FF9A9A (JD, 10-24: #E08080 too dim) */
#define MENU_WHITE     0xFFFFu   /* titles, pages */
#define MENU_GREY      0x632Cu   /* the separator, #646464 */
#define MENU_TRACK     0x4228u   /* the scrollbar's track, #464646 */
#define MENU_CURSOR    0xA514u   /* the cursor, #A5A2A5: a neutral grey a shade under the dim rows (JD, 10-24: in the lit colour it "blends into" a lit icon) */
#define MENU_SAVED     0x468Cu   /* the saved option's check box, #40D060, on every page (JD, 10-25: "green everywhere") */
#define MENU_ICON_LIT  0xFFB8u   /* the selected row's top-level icon, #FFF7C6: a whiter yellow than its text (JD, 10-25; the others' are white). Not in the palette: a slot there cost the dev heap 8 B */
#define MENU_FLASH_MS  1300u
#define MENU_PREVIEW_MS 300u     /* P5e: a profile's preview once the knob rests on its row this long (JD, 10-26:
                                  * "between 0.3s to a second ... so the board doesn't go crazy as you scroll";
                                  * 600 ms first, then "about half what it is now") */
#define MENU_HINT_FROM 4000u     /* the hints after 4 s idle: "Turn + Click Knob" 1 s, */
#define MENU_HINT_EVERY 6000u    /* then "Hold to Go Back" 1 s (JD, 10-24), every 6 s */
