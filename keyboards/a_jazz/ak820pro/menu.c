// Copyright 2026 quill4gen7
// SPDX-License-Identifier: GPL-2.0-or-later
/* Settings menu -- see menu.h.
 *
 * ITEMS
 *   Ask On/Off     Claude Code questions on the board. "Off" turns every
 *                  question into a notice (GIF slot 2) that answers "cancel"
 *                  at once, so the question is asked at the terminal and the
 *                  host closes the notice when it is answered there (notify.c,
 *                  NOTICE). Persisted (kb_eeconfig).
 *   Exit           close.
 *
 * The rows are padded to the full 12 columns here, so the page's own centring
 * leaves them alone and the ">" column stays put as it moves (as ask_render). */
#include "quantum.h"
#include "menu.h"
#include "ak820pro.h"
#include "notify.h"
#include "kb_eeconfig.h"
#include "graphics/display.h"

#define MENU_COLS    12
#define MENU_ROWS    5
#define MENU_IDLE_MS 20000u
#define KNOB_ROW     0      /* the knob's press, matrix [0,14] in keyboard.json */
#define KNOB_COL     14

enum { ITEM_QUESTIONS, ITEM_EXIT, ITEM_COUNT };

static bool     menu_on  = false;
static uint8_t  menu_sel = 0;
static uint32_t menu_at  = 0;

static void add_row(char *buf, uint8_t *n, const char *s) {
    uint8_t l = (uint8_t)strlen(s);
    if (l > MENU_COLS) l = MENU_COLS;
    memcpy(buf + *n, s, l);
    memset(buf + *n + l, ' ', (size_t)(MENU_COLS - l));
    *n = (uint8_t)(*n + MENU_COLS);
    buf[(*n)++] = '\n';
}

static void menu_render(bool fresh) {
    char    buf[MENU_ROWS * (MENU_COLS + 1)];
    char    line[MENU_COLS + 1];
    uint8_t n = 0;
    add_row(buf, &n, "  Settings");
    for (uint8_t i = 0; i < ITEM_COUNT; i++) {
        char mark = (i == menu_sel) ? '>' : ' ';
        switch (i) {
            case ITEM_QUESTIONS:
                snprintf(line, sizeof(line), "%c Ask    %s", mark, kb_eeconfig_get_notify_notice() ? "Off" : "On");
                break;
            default:
                snprintf(line, sizeof(line), "%c Exit", mark);
                break;
        }
        add_row(buf, &n, line);
    }
    for (uint8_t r = 1 + ITEM_COUNT; r < MENU_ROWS - 1; r++) add_row(buf, &n, "");
    add_row(buf, &n, "Turn & press");
    n--;   /* the trailing newline */
    if (fresh) display_notify_page_text(buf, n);
    else       display_notify_page_update(buf, n);
}

static void menu_open(void) {
    if (notify_page_busy() || display_debug_active()) return;
    if (display_notify_page_active()) notify_yield_page();   /* a plain page: the menu replaces it */
    menu_sel = 0;
    menu_render(true);
    menu_on = display_notify_page_active();
    menu_at = timer_read32();
}

static void menu_close(void) {
    menu_on = false;
    if (display_notify_page_active()) display_notify_page_close();
    notify_note_key();   /* the screensaver counts from here */
}

static void menu_move(int8_t dir) {
    menu_sel = (uint8_t)((menu_sel + ITEM_COUNT + dir) % ITEM_COUNT);
    menu_render(false);
}

static void menu_activate(void) {
    switch (menu_sel) {
        case ITEM_QUESTIONS:
            kb_eeconfig_set_notify_notice(!kb_eeconfig_get_notify_notice());
            menu_render(false);
            break;
        default:
            menu_close();
            break;
    }
}

bool menu_process_record(uint16_t keycode, keyrecord_t *record) {
    bool enc = IS_ENCODEREVENT(record->event);
    if (!menu_on) {
        if (!enc || !record->event.pressed) return false;
        if (!IS_LAYER_ON(WINFN) && !IS_LAYER_ON(MACFN)) return false;
        menu_open();
        return menu_on;   /* not opened (a question is up): the knob does what it did */
    }
    menu_at = timer_read32();
    if (enc) {   /* press and release of every click; one step per click */
        if (record->event.pressed) menu_move(record->event.type == ENCODER_CW_EVENT ? 1 : -1);
        return true;
    }
    /* Releases pass: one of a key held from before the menu must not stick,
     * and one whose press the menu took is a no-op. Layer keys pass too. */
    if (!record->event.pressed) return false;
    if (IS_QK_MOMENTARY(keycode) || IS_QK_LAYER_TAP(keycode) || IS_QK_ONE_SHOT_LAYER(keycode)) return false;
    if (record->event.key.row == KNOB_ROW && record->event.key.col == KNOB_COL) {
        menu_activate();
        return true;
    }
    switch (keycode) {
        case KC_ENT: case KC_SPC:  menu_activate(); break;
        case KC_UP:  case KC_LEFT: menu_move(-1);   break;
        case KC_DOWN: case KC_RGHT: menu_move(1);   break;
        case KC_ESC:               menu_close();    break;
        default:                   break;   /* ignored */
    }
    return true;
}

void menu_task(void) {
    if (!menu_on) return;
    if (!display_notify_page_active()) { menu_on = false; return; }   /* closed under us */
    if (timer_elapsed32(menu_at) >= MENU_IDLE_MS) menu_close();
}

void menu_lost(void) {
    menu_on = false;
}
