// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
/* The configuration menu's tree, P1a: plans/CONFIG-MENU-P1-DESIGN.md (the
 * tree, the transitions, the signatures, one lit row at most), on C3/C4's
 * slots, C8's list geometry, lit text and half rows. The words are the
 * mockup's (plans/config-menu-mockup.html). */
#include "menu.h"

#include <stdio.h>
#include <string.h>

#include "menu_backend.h"
#include "menu_env.h"
#include "menu_slots.h"
#include "menu_text.h"
#include "../graphics/res/menu_text_pages.h"

/* --- The tree ---------------------------------------------------------------
 * Every row a node of its own (options included: the top level's "Clock 24h"
 * and the choice's "24 hour" never share a signature). At most 63. */
enum {
    N_NONE = 0,
    N_TOP, N_POWER, N_CLOCK, N_DIAG, N_HELP, N_ABOUT, N_EXIT,   /* 1-7 */
    N_HEALTH, N_CAMERA, N_RESET,                                /* 8-10 */
    N_TOPIC0, N_TOPIC1, N_TOPIC2, N_TOPIC3,                     /* 11-14 */
    N_BACK,                                                     /* 15: "‹ Back" */
    N_POW0, N_POW1, N_POW2, N_POW3,                             /* 16-19 */
    N_CLK0, N_CLK1, N_CLK2, N_CLK3,                             /* 20-23 */
    N_COUNT
};
_Static_assert(N_COUNT <= 63, "a row's node is 6 bits");
_Static_assert(MENU_HELP_TOPICS == 4, "Help's four topics");

static const uint8_t top_rows[]   = {N_POWER, N_CLOCK, N_DIAG, N_HELP, N_ABOUT, N_EXIT};
static const uint8_t diag_rows[]  = {N_BACK, N_HEALTH, N_CAMERA, N_RESET};
static const uint8_t help_rows[]  = {N_BACK, N_TOPIC0, N_TOPIC1, N_TOPIC2, N_TOPIC3};
static const uint8_t power_rows[] = {N_BACK, N_POW0, N_POW1, N_POW2, N_POW3};
static const uint8_t clock_rows[] = {N_BACK, N_CLK0, N_CLK1, N_CLK2, N_CLK3};

static const char *const power_full[4]  = {"Always on", "Balanced", "Power saver", "Lights out"};
static const char *const power_short[4] = {"Always on", "Balanced", "Saver", "Lights out"};
/* The choice's rows in the mockup's order, and the clock enum each maps to:
 * 24 hour = 24H (0), 12 hour = 12H (1), Date = DATE (3), Off = OFF (2). */
static const char *const clock_full[4]  = {"24 hour", "12 hour", "Date", "Off"};
static const uint8_t     clock_enum[4]  = {0, 1, 3, 2};
static const char *const clock_short_by_enum[4] = {"24h", "12h", "Off", "Date"};

static const char *label_of(uint8_t node) {
    switch (node) {
        case N_POWER:  return "Power";
        case N_CLOCK:  return "Clock";
        case N_DIAG:   return "Diagnostics";
        case N_HELP:   return "Help";
        case N_ABOUT:  return "About";
        case N_EXIT:   return "Exit";
        case N_HEALTH: return "Health page";
        case N_CAMERA: return "Camera page";
        case N_RESET:  return "Reset counters";
        case N_BACK:   return MT_LSAQUO " Back";
        default: break;
    }
    if (node >= N_TOPIC0 && node <= N_TOPIC3) return menu_help_titles[node - N_TOPIC0];
    if (node >= N_POW0 && node <= N_POW3) return power_full[node - N_POW0];
    if (node >= N_CLK0 && node <= N_CLK3) return clock_full[node - N_CLK0];
    return "";
}

/* --- State ------------------------------------------------------------------ */

typedef struct { uint8_t node, cursor, top; } frame_t;
static frame_t  stack[3];
static uint8_t  depth;
static uint8_t  model_power, model_clock;   /* the latched model (external changes: Fn+B, Fn+C) */
static uint8_t  title_id;                   /* the latched title: a screen's, a flash's or the hint */
static uint8_t  flash_id;                   /* 0 none */
static uint16_t flash_t0;                   /* 16-bit ms */
static uint8_t  confirm_page;               /* Reset counters: 0 the question, 1 done */
static env_about_t snap;                    /* About's, frozen while it shows */

static frame_t *cur(void) { return &stack[depth - 1]; }

static const uint8_t *rows_of(uint8_t node, uint8_t *n) {
    switch (node) {
        case N_TOP:   *n = sizeof top_rows;   return top_rows;
        case N_DIAG:  *n = sizeof diag_rows;  return diag_rows;
        case N_HELP:  *n = sizeof help_rows;  return help_rows;
        case N_POWER: *n = sizeof power_rows; return power_rows;
        case N_CLOCK: *n = sizeof clock_rows; return clock_rows;
        default:      *n = 0;                 return 0;
    }
}
static bool is_topic(uint8_t node) { return node >= N_TOPIC0 && node <= N_TOPIC3; }
static bool is_list(uint8_t node) { return node == N_TOP || node == N_DIAG || node == N_HELP; }
static bool is_choice(uint8_t node) { return node == N_POWER || node == N_CLOCK; }
static uint8_t topic_lines(uint8_t node) {
    uint8_t t = (uint8_t)(node - N_TOPIC0);
    return (uint8_t)(menu_help_start[t + 1] - menu_help_start[t]);
}
/* A choice's saved option, by its row (1-4). */
static uint8_t saved_row(uint8_t node) {
    if (node == N_POWER) return (uint8_t)(1u + (model_power & 3u));
    for (uint8_t i = 0; i < 4; i++)
        if (clock_enum[i] == model_clock) return (uint8_t)(1u + i);
    return 1;
}

/* --- Titles ------------------------------------------------------------------ */
enum { T_TOP = 1, T_POWER, T_CLOCK, T_DIAG, T_HELP, T_ABOUT, T_TOPIC0, T_RESET = T_TOPIC0 + 4,
       T_SAVED = 16, T_CANCELLED, T_RESET_DONE, T_HINT };
_Static_assert(T_RESET == 11, "the title ids: the design's table");

static uint8_t screen_title(void) {
    uint8_t n = cur()->node;
    switch (n) {
        case N_TOP:   return T_TOP;
        case N_POWER: return T_POWER;
        case N_CLOCK: return T_CLOCK;
        case N_DIAG:  return T_DIAG;
        case N_HELP:  return T_HELP;
        case N_ABOUT: return T_ABOUT;
        case N_RESET: return T_RESET;
        default:      return is_topic(n) ? (uint8_t)(T_TOPIC0 + (n - N_TOPIC0)) : T_TOP;
    }
}
static const char *title_text(uint8_t id) {
    switch (id) {
        case T_TOP:        return "Configuration";
        case T_POWER:      return "Power";
        case T_CLOCK:      return "Clock";
        case T_DIAG:       return "Diagnostics";
        case T_HELP:       return "Help";
        case T_ABOUT:      return "About";
        case T_RESET:      return "Reset counters";
        case T_SAVED:      return "Saved";
        case T_CANCELLED:  return "Cancelled";
        case T_RESET_DONE: return "Counters reset";
        case T_HINT:       return "Turn + Click Knob";
        default: break;
    }
    if (id >= T_TOPIC0 && id < T_TOPIC0 + 4) return menu_help_titles[id - T_TOPIC0];
    return "";
}

/* --- About ------------------------------------------------------------------ */

static void about_line(uint8_t line, char *out, size_t sz) {
    static const char *const link_state[5] = {"idle", "linking", "pairing", "connected", "rejected"};
    switch (line) {
        case 0:
            snprintf(out, sz, "Firmware %08lx%s", (unsigned long)env_build_hash(), env_build_dirty() ? "+" : "");
            break;
        case 1:
            snprintf(out, sz, "%s, 0x%08lx", env_build_flavour(), (unsigned long)env_build_token());
            break;
        case 2: {
            uint32_t m = snap.uptime_s / 60u, d = m / 1440u, h = (m / 60u) % 24u;
            if (d) snprintf(out, sz, "Up %lud %luh %lum", (unsigned long)d, (unsigned long)h, (unsigned long)(m % 60u));
            else   snprintf(out, sz, "Up %luh %lum", (unsigned long)h, (unsigned long)(m % 60u));
            break;
        }
        case 3:
            if (snap.batt_pm == ENV_UNKNOWN) snprintf(out, sz, "Battery -");
            else snprintf(out, sz, "Battery %u.%u%%", snap.batt_pm / 10u, snap.batt_pm % 10u);
            break;
        case 4:
            if (snap.pack_mv == ENV_UNKNOWN) snprintf(out, sz, "Pack -");
            else snprintf(out, sz, "Pack %u mV", snap.pack_mv);
            break;
        case 5:
            if (snap.left_min == ENV_UNKNOWN) snprintf(out, sz, "Left -");
            else snprintf(out, sz, "Left %uh of use", snap.left_min / 60u);
            break;
        case 6:
            snprintf(out, sz, "Mode %s", power_full[snap.mode & 3u]);
            break;
        default: {
            uint8_t tr = snap.link & 3u, slot = (snap.link >> 2) & 3u, st = (snap.link >> 4) & 7u;
            const char *s = st < 5 ? link_state[st] : "?";
            if (tr == ENV_LINK_USB) snprintf(out, sz, "Link USB");
            else if (tr == ENV_LINK_24G) snprintf(out, sz, "Link 2.4G %s", s);
            else snprintf(out, sz, "Link BT %u %s", (unsigned)(slot + 1u), s);
            break;
        }
    }
}

/* --- The list geometry (C8): 0 the title, 1 the separator, 2 the scrollbar,
 * 3-10 the rows. The scrollbar's slot comes before the rows so a page step
 * shows the new position first, then the text flows in (JD, 10-24: it moved
 * only after all the text, a second or two). A row is one slot of two chunks
 * (half rows, JD: by efficiency); every text slot is x 3-124 (a pad pixel,
 * 20 cells, x 124). */
#define S_TITLE 0
#define S_SEP   1
#define S_BAR   2
#define S_ROW0  3
#define ROW_Y0  15
#define ROW_H   14
#define TEXT_W  122
#define HALF_W  61
#define BAR_X   125
#define BAR_Y0  15
#define BAR_H   (MENU_ROWS * ROW_H)
#define BITS_MAX 64

/* A row's content: what the slot shows, for its signature and its painter. */
typedef struct { uint8_t kind; uint8_t node; uint8_t val; bool bullet, lit; uint8_t line; } row_t;
enum { RK_BLANK = 0, RK_ITEM, RK_TEXT, RK_ABOUT };

static row_t row_at(uint8_t r) {
    row_t    x = {RK_BLANK, 0, 15, false, false, 0};
    frame_t *f = cur();
    if (is_topic(f->node)) {
        uint8_t i = (uint8_t)(f->top + r);
        if (i < topic_lines(f->node)) { x.kind = RK_TEXT; x.line = i; x.node = f->node; }
        return x;
    }
    if (f->node == N_ABOUT) {
        x.kind = RK_ABOUT;
        x.line = r;
        return x;
    }
    uint8_t        n;
    const uint8_t *rows = rows_of(f->node, &n);
    uint8_t        i    = (uint8_t)(f->top + r);
    if (!rows || i >= n) return x;
    x.kind = RK_ITEM;
    x.node = rows[i];
    x.lit  = i == f->cursor;
    if (x.node == N_POWER) x.val = model_power & 3u;
    else if (x.node == N_CLOCK) x.val = model_clock & 3u;
    if (is_choice(f->node) && i && i == saved_row(f->node)) x.bullet = true;
    return x;
}

/* The row's 20 cells. */
static void row_text(const row_t *x, char *out) {
    memset(out, ' ', 20);
    out[20] = '\0';
    char buf[24];
    if (x->kind == RK_TEXT) {
        const char *s = menu_help_lines[menu_help_start[x->node - N_TOPIC0] + x->line];
        size_t      n = strlen(s);
        memcpy(out, s, n > 20 ? 20 : n);
        return;
    }
    if (x->kind == RK_ABOUT) {
        about_line(x->line, buf, sizeof buf);
        size_t n = strlen(buf);
        memcpy(out, buf, n > 20 ? 20 : n);
        return;
    }
    if (x->kind != RK_ITEM) return;
    const char *label = label_of(x->node);
    const char *value = "";
    if (x->node == N_POWER) value = power_short[x->val & 3u];
    else if (x->node == N_CLOCK) value = clock_short_by_enum[x->val & 3u];
    else if (x->node == N_DIAG || x->node == N_HELP || x->node == N_ABOUT || x->node == N_HEALTH ||
             x->node == N_CAMERA || x->node == N_RESET || is_topic(x->node))
        value = ">";
    size_t at = 0;
    if (x->node >= N_POW0 && x->node <= N_CLK3) {   /* an option: the saved one's bullet in cell 0, a space after it */
        out[0] = x->bullet ? MT_BULLET[0] : ' ';
        at     = 2;                                  /* JD, 10-24: the dot needed room; in line with "‹ Back" */
    }
    size_t ln = strlen(label), vn = strlen(value);
    if (at + ln > 20) ln = 20 - at;
    memcpy(out + at, label, ln);
    if (vn) memcpy(out + 20 - vn, value, vn);
}

static uint16_t row_sig(const row_t *x) {
    switch (x->kind) {
        case RK_ITEM:
            return (uint16_t)((1u << 14) | ((uint16_t)x->node << 8) | ((uint16_t)(x->val & 15u) << 4) |
                              (x->bullet ? 2u : 0u) | (x->lit ? 1u : 0u));
        case RK_TEXT:
            return (uint16_t)((2u << 14) | ((uint16_t)(x->node - N_TOPIC0) << 11) | ((uint16_t)x->line << 5));
        case RK_ABOUT:
            return (uint16_t)((3u << 14) | ((uint16_t)x->line << 8));
        default:
            return 0;
    }
}

/* The scrollbar: blank when everything fits, else the count and the top. */
static void bar_state(uint8_t *count, uint8_t *top) {
    frame_t *f = cur();
    *top       = f->top;
    if (is_topic(f->node)) *count = topic_lines(f->node);
    else if (f->node == N_ABOUT) *count = 8;
    else (void)rows_of(f->node, count);
}
static uint16_t bar_sig(void) {
    uint8_t count, top;
    bar_state(&count, &top);
    if (count <= MENU_ROWS) return 0xFFFFu;
    return (uint16_t)(((uint16_t)(count & 63u) << 8) | (top & 63u));
}

static uint16_t l_sig(uint8_t s) {
    if (s == S_TITLE) return title_id;
    if (s == S_SEP) return 1;
    if (s == S_BAR) return bar_sig();
    row_t x = row_at((uint8_t)(s - S_ROW0));
    return row_sig(&x);
}
static uint8_t l_chunks(uint8_t s) { return (s == S_SEP || s == S_BAR) ? 1 : 2; }
static uint8_t rows_per_piece(void) { return (uint8_t)(MB_PASS_BYTES / (HALF_W * 2u)); }   /* 4 */
static uint8_t l_pieces(uint8_t s, uint8_t c) {
    (void)c;
    if (s == S_SEP || s == S_BAR) return 1;
    uint8_t r = rows_per_piece();
    return (uint8_t)((MT_CELL_H + r - 1u) / r);
}
_Static_assert(MB_PASS_BYTES / (HALF_W * 2u) >= 1 && (MB_PASS_BYTES / (HALF_W * 2u)) * ((HALF_W + 7u) / 8u) <= BITS_MAX,
               "a half-row piece: whole pixel rows, its bits in the buffer");

/* One piece of a 20-cell text slot at y: chunk c's half, piece p's rows. */
static ms_paint_t text_piece(const char *cells, uint16_t y, uint8_t c, uint8_t p, uint16_t fg) {
    mt_line_t      l    = {cells, 20, 1, 1, false, false};
    const uint8_t  rpp  = rows_per_piece();
    const uint8_t  r0   = (uint8_t)(p * rpp);
    const uint8_t  rows = (uint8_t)(r0 + rpp > MT_CELL_H ? MT_CELL_H - r0 : rpp);
    const uint16_t c0   = (uint16_t)(c * HALF_W);
    uint8_t        bits[BITS_MAX];
    mt_bits(&l, c0, HALF_W, r0, rows, bits, (HALF_W + 7u) / 8u);
    return mb_mono(bits, (HALF_W + 7u) / 8u, (uint16_t)(3u + c0), (uint16_t)(y + r0), HALF_W, rows, fg, 0)
               ? MS_PAINT_DONE : MS_PAINT_FAILED;
}

static ms_paint_t l_paint(uint8_t s, uint8_t c, uint8_t p) {
    char cells[21];
    if (s == S_SEP) return mb_fill(4, 14, 123, 14, MENU_GREY) ? MS_PAINT_DONE : MS_PAINT_FAILED;
    if (s == S_BAR) {
        uint8_t count, top;
        bar_state(&count, &top);
        if (count <= MENU_ROWS) return mb_fill(BAR_X, BAR_Y0, BAR_X, BAR_Y0 + BAR_H - 1, 0) ? MS_PAINT_DONE : MS_PAINT_FAILED;
        /* The mockup's: a track, and a thumb at least 6 px, placed by the top. */
        uint16_t th = (uint16_t)((BAR_H * MENU_ROWS + count / 2u) / count);
        if (th < 6) th = 6;
        uint16_t ty = (uint16_t)(BAR_Y0 + ((uint32_t)(BAR_H - th) * top + (count - MENU_ROWS) / 2u) / (count - MENU_ROWS));
        if (!mb_fill(BAR_X, BAR_Y0, BAR_X, BAR_Y0 + BAR_H - 1, MENU_TRACK)) return MS_PAINT_FAILED;
        return mb_fill(BAR_X, ty, BAR_X, (uint16_t)(ty + th - 1u), MENU_WHITE) ? MS_PAINT_DONE : MS_PAINT_FAILED;
    }
    if (s == S_TITLE) {
        memset(cells, ' ', 20);
        cells[20]      = '\0';
        const char *tt = title_text(title_id);
        size_t      n  = strlen(tt);
        memcpy(cells, tt, n > 20 ? 20 : n);
        return text_piece(cells, 0, c, p, MENU_WHITE);
    }
    row_t x = row_at((uint8_t)(s - S_ROW0));
    row_text(&x, cells);
    uint16_t fg = x.kind != RK_ITEM ? MENU_WHITE
                : x.lit              ? MENU_LIT
                : (x.node == N_BACK || x.node == N_EXIT) ? MENU_LEAVE
                                     : MENU_DIM;
    return text_piece(cells, (uint16_t)(ROW_Y0 + (s - S_ROW0) * ROW_H), c, p, fg);
}

static const ms_layout_t L_LIST = {11, l_sig, l_chunks, l_pieces, l_paint, S_ROW0, MENU_ROWS};

/* --- The confirm geometry: Reset counters' two pages (the mockup: text at y
 * 22, 36, 72 and 86 under the title and the separator). */
static const uint8_t c_y[4] = {22, 36, 72, 86};
static const char *const c_lines[2][4] = {
    {"Clear the health", "counters?", "Click: reset", "Hold: cancel"},
    {"Counters reset.", "", "Click: back", ""},
};
static uint16_t c_sig(uint8_t s) {
    if (s == 0) return title_id;
    if (s == 1) return 1;
    return (uint16_t)(0x100u | ((uint16_t)confirm_page << 4) | (uint16_t)(s - 2u));
}
static uint8_t c_chunks(uint8_t s) { return s == 1 ? 1 : 2; }
static uint8_t c_pieces(uint8_t s, uint8_t c) { return l_pieces(s == 1 ? S_SEP : S_TITLE, c); }
static ms_paint_t c_paint(uint8_t s, uint8_t c, uint8_t p) {
    if (s <= 1) return l_paint(s, c, p);
    char cells[21];
    memset(cells, ' ', 20);
    cells[20]      = '\0';
    const char *ln = c_lines[confirm_page & 1u][s - 2u];
    memcpy(cells, ln, strlen(ln));
    return text_piece(cells, c_y[s - 2u], c, p, MENU_WHITE);
}
static const ms_layout_t L_CONFIRM = {6, c_sig, c_chunks, c_pieces, c_paint, 0, 0};

/* --- Navigation -------------------------------------------------------------- */

static void retitle(uint32_t now);
static uint32_t last_idle;   /* menu_tick's latest idle time, for retitle() between ticks */

static void push(uint8_t node, uint8_t cursor) {
    if (depth >= 3) return;   /* the tree is three deep at most */
    stack[depth++] = (frame_t){node, cursor, (uint8_t)(cursor / MENU_ROWS * MENU_ROWS)};
}
static void flash(uint8_t id, uint32_t now) {
    flash_id = id;
    flash_t0 = (uint16_t)now;
}

void menu_open(uint32_t now) {
    depth        = 0;
    last_idle    = 0;   /* the opening is knob input */
    flash_id     = 0;
    confirm_page = 0;
    model_power  = env_power_mode();
    model_clock  = env_clock_mode();
    push(N_TOP, 0);
    title_id = T_TOP;
    retitle(now);
    ms_set_layout(&L_LIST, true);
}

void menu_return_from_diag(uint8_t page, uint32_t now) {
    depth        = 0;
    last_idle    = 0;
    flash_id     = 0;
    confirm_page = 0;
    model_power  = env_power_mode();
    model_clock  = env_clock_mode();
    push(N_TOP, 2);   /* Diagnostics' row */
    push(N_DIAG, page ? 2 : 1);
    title_id = T_DIAG;
    retitle(now);
    ms_set_layout(&L_LIST, true);
}

void menu_invalidate(void) { ms_invalidate_all(); }

/* A screen change within the list geometry: a retarget. Into or out of
 * Reset counters: a layout change (its clear). */
static void show(bool to_confirm, bool from_confirm, uint32_t now) {
    retitle(now);
    if (to_confirm) ms_set_layout(&L_CONFIRM, false);
    else if (from_confirm) ms_set_layout(&L_LIST, false);
    else ms_retarget();
}

static void open_about(uint32_t now) {
    env_about(&snap);
    push(N_ABOUT, 0);
    retitle(now);
    /* A new snapshot: About's rows repaint whatever their signature (a
     * committed row from an earlier visit could equal it). No clear. */
    ms_invalidate_rows(S_ROW0, MENU_ROWS);
    ms_retarget();
}

static menu_result_t pop(uint32_t now) {
    if (depth <= 1) return MENU_CLOSE;
    bool from_confirm = cur()->node == N_RESET;
    depth--;
    show(false, from_confirm, now);
    return MENU_NONE;
}

static menu_result_t activate(uint32_t now) {
    frame_t       *f = cur();
    uint8_t        n;
    const uint8_t *rows = rows_of(f->node, &n);
    if (!rows || f->cursor >= n) return MENU_NONE;
    uint8_t node = rows[f->cursor];
    if (node == N_BACK) {
        if (is_choice(f->node)) flash(T_CANCELLED, now);
        return pop(now);
    }
    if (is_choice(f->node)) {   /* commit the option under the cursor */
        uint8_t i = (uint8_t)(f->cursor - 1u);
        if (f->node == N_POWER) { env_power_set(i); model_power = i; }
        else { env_clock_set(clock_enum[i]); model_clock = clock_enum[i]; }
        flash(T_SAVED, now);
        return pop(now);
    }
    switch (node) {
        case N_POWER:
        case N_CLOCK:
            push(node, saved_row(node));
            show(false, false, now);
            return MENU_NONE;
        case N_DIAG:
        case N_HELP:
            push(node, 1);
            show(false, false, now);
            return MENU_NONE;
        case N_ABOUT:
            open_about(now);
            return MENU_NONE;
        case N_EXIT:
            return MENU_CLOSE;
        case N_HEALTH: return MENU_DIAG_HEALTH;
        case N_CAMERA: return MENU_DIAG_CAMERA;
        case N_RESET:
            confirm_page = 0;
            push(N_RESET, 0);
            show(true, false, now);
            return MENU_NONE;
        default:
            if (is_topic(node)) {
                push(node, 0);
                cur()->top = 0;
                show(false, false, now);
            }
            return MENU_NONE;
    }
}

/* The latched model, read fresh (Fn+B, Fn+C). True: it changed. */
static bool sync_model(void) {
    uint8_t p = env_power_mode(), c = env_clock_mode();
    if (p == model_power && c == model_clock) return false;
    model_power = p;
    model_clock = c;
    return true;
}

menu_result_t menu_input(int8_t turns, mi_latch_t latch, uint32_t now) {
    if (!depth) return MENU_NONE;
    /* Before navigation reads it (codex, P1a's code review: a choice opened
     * on the previous pass's saved value, and a click committed it); and the
     * input is knob input, so the hint's idle time restarts here, not a tick
     * later (no target for the hint between). An open choice's cursor stays
     * where it is: only its bullet follows the model. */
    bool changed = sync_model();
    last_idle    = 0;
    uint8_t g0   = ms_tgen();
    frame_t *f = cur();
    if (turns) {
        if (is_list(f->node) || is_choice(f->node)) {
            uint8_t        n;
            (void)rows_of(f->node, &n);
            int16_t c = (int16_t)((f->cursor + turns) % (int16_t)n);
            if (c < 0) c += n;
            f->cursor = (uint8_t)c;
            f->top    = (uint8_t)(f->cursor / MENU_ROWS * MENU_ROWS);   /* lists page by 8 */
            ms_retarget();
        } else if (is_topic(f->node)) {
            int16_t lines = topic_lines(f->node), maxtop = lines > MENU_ROWS ? (int16_t)(lines - MENU_ROWS) : 0;
            int16_t t     = (int16_t)(f->top + (turns > 0 ? MENU_ROWS : -MENU_ROWS));
            if (t < 0) t = 0;
            if (t > maxtop) t = maxtop;
            if (t != f->top) {
                f->top = (uint8_t)t;
                ms_retarget();
            }
        }
    }
    if (latch == MI_LATCH_CLICK) {
        if (is_list(f->node) || is_choice(f->node)) return activate(now);
        if (f->node == N_RESET) {
            if (confirm_page == 0) {
                env_health_reset();
                confirm_page = 1;
                flash(T_RESET_DONE, now);
                retitle(now);
                ms_retarget();
                return MENU_NONE;
            }
            return pop(now);
        }
        return pop(now);   /* a topic, About: back */
    }
    if (latch == MI_LATCH_BACK) {
        if (is_choice(f->node) || (f->node == N_RESET && confirm_page == 0)) flash(T_CANCELLED, now);
        return pop(now);
    }
    if (changed && ms_tgen() == g0) ms_retarget();   /* nothing navigated: the model's change is the target */
    return MENU_NONE;
}

/* The title the screen wants now: a flash, the hint (lists only), or its own. */
static uint8_t want_title(uint32_t now, uint32_t idle_ms) {
    if (flash_id && (uint16_t)((uint16_t)now - flash_t0) < MENU_FLASH_MS) return flash_id;
    flash_id = 0;
    if (is_list(cur()->node) && idle_ms >= MENU_HINT_FROM && idle_ms % MENU_HINT_EVERY >= MENU_HINT_FROM) return T_HINT;
    return screen_title();
}
static void retitle(uint32_t now) { title_id = want_title(now, last_idle); }

void menu_tick(uint32_t now, uint32_t idle_ms) {
    if (!depth) return;
    last_idle  = idle_ms;
    bool moved = sync_model();   /* Fn+B, Fn+C, while the menu shows */
    uint8_t t = want_title(now, idle_ms);
    if (t != title_id) {
        title_id = t;
        moved    = true;
    }
    if (moved) ms_retarget();
}

uint8_t menu_screen_id(void) { return depth ? (uint8_t)(16u + cur()->node) : 0; }
uint8_t menu_cursor(void) {
    if (!depth) return 0xFF;
    frame_t *f = cur();
    if (is_topic(f->node)) return f->top;
    if (f->node == N_RESET) return confirm_page;
    return f->cursor;
}
