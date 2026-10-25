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

/* The palette: these defaults, and on dev builds op 27 sub 13 changes one
 * live (JD tunes colours on the panel without a flash a shade). */
enum { MP_LIT = 0, MP_DIM, MP_LEAVE, MP_WHITE, MP_GREY, MP_TRACK, MP_CURSOR, MP_N };
uint16_t menu_color(uint8_t i);
#if defined(AK820_DEV_HOOKS) || defined(MENU_HOST_TEST)
bool     menu_set_color(uint8_t i, uint16_t rgb565);   /* false: no such colour */
#endif

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
#define MENU_HINT_FROM 4000u     /* the hints after 4 s idle: "Turn + Click Knob" 1 s, */
#define MENU_HINT_EVERY 6000u    /* then "Hold to Go Back" 1 s (JD, 10-24), every 6 s */
