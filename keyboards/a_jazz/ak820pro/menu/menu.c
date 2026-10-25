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
#include "menu_edit.h"
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
    N_LIGHT, N_SCREEN,                                          /* 24-25: P1b */
    N_EFFECT, N_BRIGHT, N_HUE, N_SAT, N_SPEED, N_LIGHTS,        /* 26-31: Lighting's rows */
    N_EFFOPT, N_LTSOPT,                                         /* 32-33: an Effect or Lights option, its index in val */
    N_COUNT
};
_Static_assert(N_COUNT <= 63, "a row's node is 6 bits");
_Static_assert(MENU_HELP_TOPICS == 4, "Help's four topics");

static const uint8_t top_rows[]   = {N_LIGHT, N_SCREEN, N_POWER, N_CLOCK, N_DIAG, N_HELP, N_ABOUT, N_EXIT};
static const uint8_t light_rows[] = {N_BACK, N_EFFECT, N_BRIGHT, N_HUE, N_SAT, N_SPEED, N_LIGHTS};
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

/* The live effect uses speed as its second colour (Alphas/Mods): Lighting's
 * row and editor are Second color then, not Speed (P1b). */
static bool two_tone(void) {
    env_rgb_t c;
    env_rgb_get(&c);
    return env_effect_two_tone(c.mode);
}

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
        case N_BACK:   return MT_TRI_L " Back";   /* JD, 10-24: ◀ for ‹ */
        case N_LIGHT:  return "Lighting";
        case N_SCREEN: return "Screen";
        case N_EFFECT: return "Effect";
        case N_BRIGHT: return "Brightness";
        case N_HUE:    return "Hue";
        case N_SAT:    return "Saturation";
        case N_SPEED:  return two_tone() ? "Second color" : "Speed";   /* Alphas/Mods' speed is its second colour */
        case N_LIGHTS: return "Lights";
        default: break;
    }
    if (node >= N_TOPIC0 && node <= N_TOPIC3) return menu_help_titles[node - N_TOPIC0];
    if (node >= N_POW0 && node <= N_POW3) return power_full[node - N_POW0];
    if (node >= N_CLK0 && node <= N_CLK3) return clock_full[node - N_CLK0];
    return "";
}

/* The top level's icons (JD, 10-24, the icon picker): one cell and a space,
 * or two cells whose last five columns are blank, so every label starts at
 * cell 2. */
static const char *icon_of(uint8_t node) {
    switch (node) {
        case N_POWER: return MT_I_POWER " ";
        case N_CLOCK: return MT_I_CLOCK;
        case N_DIAG:  return MT_I_DIAG " ";
        case N_HELP:  return MT_I_HELP;
        case N_ABOUT: return MT_I_ABOUT;
        case N_EXIT:  return MT_I_EXIT " ";
        case N_LIGHT: return MT_I_LIGHT " ";   /* P1b */
        case N_SCREEN: return MT_I_SCREEN;
        default:      return NULL;
    }
}

/* --- The palette ------------------------------------------------------------- */
#if defined(AK820_DEV_HOOKS) || defined(MENU_HOST_TEST)
static uint16_t pal[MP_N] = {MENU_LIT, MENU_DIM, MENU_LEAVE, MENU_WHITE, MENU_GREY, MENU_TRACK, MENU_CURSOR};
bool menu_set_color(uint8_t i, uint16_t c) {
    if (i >= MP_N) return false;
    pal[i] = c;
    return true;
}
#else
static const uint16_t pal[MP_N] = {MENU_LIT, MENU_DIM, MENU_LEAVE, MENU_WHITE, MENU_GREY, MENU_TRACK, MENU_CURSOR};
#endif
uint16_t menu_color(uint8_t i) { return i < MP_N ? pal[i] : 0; }

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
static uint32_t ft_last;                    /* P1b: the editor's last turn, ms (32-bit: the backstop is 180 s) */
static bool     ft_have;                    /* a turn in this editor yet */

static frame_t *cur(void) { return &stack[depth - 1]; }

static const uint8_t *rows_of(uint8_t node, uint8_t *n) {
    switch (node) {
        case N_TOP:   *n = sizeof top_rows;   return top_rows;
        case N_DIAG:  *n = sizeof diag_rows;  return diag_rows;
        case N_HELP:  *n = sizeof help_rows;  return help_rows;
        case N_POWER: *n = sizeof power_rows; return power_rows;
        case N_CLOCK: *n = sizeof clock_rows; return clock_rows;
        case N_LIGHT: *n = sizeof light_rows; return light_rows;
        default:      *n = 0;                 return 0;
    }
}
/* P1b's choices list their options from the effect table: a list's or a
 * choice's row count, and row i's node (an option's index in *val). An
 * option's index is 4 bits of its signature: at most 15 effects. */
static uint8_t effects(void) {
    uint8_t n = env_effects();
    return n > 15 ? 15 : n;
}
static uint8_t row_count(uint8_t node) {
    if (node == N_EFFECT) return (uint8_t)(1u + effects());
    if (node == N_LIGHTS) return 3;
    uint8_t n;
    (void)rows_of(node, &n);
    return n;
}
static uint8_t row_node(uint8_t node, uint8_t i, uint8_t *val) {
    *val = 0;
    if (node == N_EFFECT || node == N_LIGHTS) {
        if (!i) return N_BACK;
        *val = (uint8_t)(i - 1u);
        return node == N_EFFECT ? N_EFFOPT : N_LTSOPT;
    }
    uint8_t        n;
    const uint8_t *r = rows_of(node, &n);
    return r && i < n ? r[i] : N_NONE;
}
static uint8_t row_index(uint8_t list, uint8_t node) {
    for (uint8_t i = 0, v; i < row_count(list); i++)
        if (row_node(list, i, &v) == node) return i;
    return 0;
}
static bool is_topic(uint8_t node) { return node >= N_TOPIC0 && node <= N_TOPIC3; }
static bool is_list(uint8_t node) { return node == N_TOP || node == N_DIAG || node == N_HELP || node == N_LIGHT; }
static bool is_preview(uint8_t node) { return node == N_EFFECT || node == N_LIGHTS; }   /* P1b's previewing choices */
static bool is_choice(uint8_t node) { return node == N_POWER || node == N_CLOCK || is_preview(node); }
static bool is_editor(uint8_t node) { return node == N_SCREEN || (node >= N_BRIGHT && node <= N_SPEED); }
static bool is_hue_editor(uint8_t node) { return node == N_HUE || (node == N_SPEED && two_tone()); }
static uint8_t field_of(uint8_t node) {
    switch (node) {
        case N_EFFECT: return ME_MODE;
        case N_BRIGHT: return ME_VAL;
        case N_HUE:    return ME_HUE;
        case N_SAT:    return ME_SAT;
        case N_SPEED:  return ME_SPEED;
        case N_LIGHTS: return ME_ENABLE;
        case N_SCREEN: return ME_LCD;
        default:       return ME_NONE;
    }
}
static int8_t effect_index(uint8_t mode) {
    for (uint8_t i = 0; i < effects(); i++)
        if (env_effect_mode(i) == mode) return (int8_t)i;
    return -1;
}
/* An option's value: the effect's mode, or Lights' enable. */
static uint8_t option_value(uint8_t node, uint8_t val) { return node == N_EFFOPT ? env_effect_mode(val) : val; }
static uint8_t topic_lines(uint8_t node) {
    uint8_t t = (uint8_t)(node - N_TOPIC0);
    return (uint8_t)(menu_help_start[t + 1] - menu_help_start[t]);
}
/* A choice's saved option, by its row (1-4; a previewing choice's from the
 * edit's snapshot, not the live preview; 0, Back, for an effect the table
 * doesn't list). */
static uint8_t saved_row(uint8_t node) {
    if (node == N_POWER) return (uint8_t)(1u + (model_power & 3u));
    if (node == N_EFFECT) return (uint8_t)(effect_index(me_saved()) + 1);
    if (node == N_LIGHTS) return me_saved() ? 2u : 1u;
    for (uint8_t i = 0; i < 4; i++)
        if (clock_enum[i] == model_clock) return (uint8_t)(1u + i);
    return 1;
}

/* --- Titles ------------------------------------------------------------------ */
enum { T_TOP = 1, T_POWER, T_CLOCK, T_DIAG, T_HELP, T_ABOUT, T_TOPIC0, T_RESET = T_TOPIC0 + 4,
       T_SAVED = 16, T_CANCELLED, T_RESET_DONE, T_HINT, T_HINT_BACK,
       T_LIGHT, T_SCREEN, T_EFFECT, T_BRIGHT, T_HUE, T_SAT, T_SPEED, T_SECOND, T_LIGHTS };   /* 21-29: P1b */
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
        case N_LIGHT:  return T_LIGHT;
        case N_SCREEN: return T_SCREEN;
        case N_EFFECT: return T_EFFECT;
        case N_BRIGHT: return T_BRIGHT;
        case N_HUE:    return T_HUE;
        case N_SAT:    return T_SAT;
        case N_SPEED:  return two_tone() ? T_SECOND : T_SPEED;
        case N_LIGHTS: return T_LIGHTS;
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
        case T_HINT_BACK:  return "Hold to Go Back";   /* JD, 10-24: the hold isn't obvious */
        case T_LIGHT:      return "Lighting";
        case T_SCREEN:     return "Screen";
        case T_EFFECT:     return "Effect";
        case T_BRIGHT:     return "Brightness";
        case T_HUE:        return "Hue";
        case T_SAT:        return "Saturation";
        case T_SPEED:      return "Speed";
        case T_SECOND:     return "Second color";
        case T_LIGHTS:     return "Lights";
        default: break;
    }
    if (id >= T_TOPIC0 && id < T_TOPIC0 + 4) return menu_help_titles[id - T_TOPIC0];
    return "";
}
/* A heading's icon, before its text (JD, 10-24): the wrench on the top level,
 * and a section's own on every screen inside it ("keep the consistency").
 * The flashes and the hints are messages, not headings: none. */
static const char *title_icon(uint8_t id) {
    if (id == T_TOP) return MT_WRENCH " ";   /* it inks all 12 columns: a space */
    if (id >= T_TOPIC0 && id < T_TOPIC0 + 4) return icon_of(N_HELP);
    switch (id) {
        case T_POWER: return icon_of(N_POWER);
        case T_CLOCK: return icon_of(N_CLOCK);
        case T_DIAG:
        case T_RESET: return icon_of(N_DIAG);
        case T_HELP:  return icon_of(N_HELP);
        case T_ABOUT: return icon_of(N_ABOUT);
        case T_SCREEN: return icon_of(N_SCREEN);
        default:      return id >= T_LIGHT && id <= T_LIGHTS ? icon_of(N_LIGHT) : "";
    }
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
 * only after all the text, a second or two). A row is one slot in half rows
 * (JD: by efficiency). Every text slot's 20 cells are x 4-123 and its 1 px
 * trail x 124; its lead is x 3 for the title and the confirm's lines, and x
 * 0-3 for a list row: the cursor's three columns and its 1 px gap (JD, 10-24:
 * x 0-2 were black, so nothing moved). The title's chunks: the lead and cells
 * 0-9, then cells 10-19 and the trail. A row's: its lead first, alone (the
 * cursor has its own colour: JD, 10-24, it "kinda blends into" a lit icon;
 * one piece, 112 B), then cells 0-9, then cells 10-19 and the trail. */
#define S_TITLE 0
#define S_SEP   1
#define S_BAR   2
#define S_ROW0  3
#define ROW_Y0  15
#define ROW_H   14
#define TEXT_X  4
#define ROW_LEAD 4
#define HALF_W  (10u * MT_CELL_W)   /* ten cells */
#define CHUNK_MAX (HALF_W + 1u)     /* 61: a lead or a trail pixel with them */
#define BAR_X   125
#define BAR_Y0  15
#define BAR_H   (MENU_ROWS * ROW_H)
#define BITS_MAX 64

/* A row's content: what the slot shows, for its signature and its painter. */
typedef struct { uint8_t kind; uint8_t node; uint8_t val; bool bullet, lit; uint8_t line; } row_t;
enum { RK_BLANK = 0, RK_ITEM, RK_TEXT, RK_ABOUT, RK_VALUE };

/* P1b's value rows (Lighting's, and the top level's Screen): which one, for
 * the signature, and the live value, read as it composes (not latched: a
 * value changing between pieces changes the signature, which restarts the
 * slot). */
static int8_t value_which(uint8_t node) {
    switch (node) {
        case N_EFFECT: return 0;
        case N_BRIGHT: return 1;
        case N_HUE:    return 2;
        case N_SAT:    return 3;
        case N_SPEED:  return 4;
        case N_LIGHTS: return 5;
        case N_SCREEN: return 6;
        default:       return -1;
    }
}
static uint8_t live_value(uint8_t node) {
    if (node == N_SCREEN) return env_lcd_level();
    env_rgb_t c;
    env_rgb_get(&c);
    switch (node) {
        case N_EFFECT: return c.mode;
        case N_BRIGHT: return c.v;
        case N_HUE:    return c.h;
        case N_SAT:    return c.s;
        case N_SPEED:  return c.speed;
        default:       return c.enable;
    }
}
/* As the Fn readout writes them (param_overlay.c): percentages with its
 * rounding, the hue in degrees, the second colour as its offset. */
static void fmt_value(uint8_t node, uint8_t v, char *b, size_t n) {
    switch (node) {
        case N_EFFECT: {
            int8_t i = effect_index(v);
            if (i >= 0) snprintf(b, n, "%s", env_effect_name((uint8_t)i));
            else snprintf(b, n, "Mode %u", (unsigned)v);
            break;
        }
        case N_HUE:    snprintf(b, n, "%u", (unsigned)((v * 360u) / 256u)); break;
        case N_LIGHTS: snprintf(b, n, "%s", v ? "On" : "Off"); break;
        case N_SCREEN:
            if (v) snprintf(b, n, "%u%%", (unsigned)((v * 100u + MENU_LCD_MAX / 2u) / MENU_LCD_MAX));
            else snprintf(b, n, "Off");
            break;
        case N_SPEED:   /* Alphas/Mods' second colour: its offset; otherwise a percentage */
            if (two_tone()) snprintf(b, n, "+%u", (unsigned)((v * 360u) / 256u));
            else snprintf(b, n, "%u%%", (unsigned)((v * 100u + 127u) / 255u));
            break;
        default: snprintf(b, n, "%u%%", (unsigned)((v * 100u + 127u) / 255u)); break;
    }
}

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
    uint8_t i = (uint8_t)(f->top + r), val;
    if (i >= row_count(f->node)) return x;
    x.kind = RK_ITEM;
    x.node = row_node(f->node, i, &val);
    x.lit  = i == f->cursor;
    if (x.node == N_POWER) x.val = model_power & 3u;
    else if (x.node == N_CLOCK) x.val = model_clock & 3u;
    else if (x.node == N_EFFOPT || x.node == N_LTSOPT) x.val = val;
    else if (value_which(x.node) >= 0) {
        x.kind = RK_VALUE;
        x.val  = live_value(x.node);
    }
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
    if (x->kind == RK_VALUE) {
        size_t      at   = 0;
        const char *icon = icon_of(x->node);   /* the top level's Screen */
        if (icon) {
            memcpy(out, icon, 2);
            at = 2;
        }
        const char *label = label_of(x->node);
        size_t      ln    = strlen(label);
        memcpy(out + at, label, ln);
        fmt_value(x->node, x->val, buf, sizeof buf);
        size_t vn = strlen(buf), room = at + ln + 1u < 20u ? 20u - at - ln - 1u : 0u;
        if (vn > room) vn = room;
        memcpy(out + 20 - vn, buf, vn);
        return;
    }
    if (x->kind != RK_ITEM) return;
    const char *label = x->node == N_EFFOPT   ? env_effect_name(x->val)
                      : x->node == N_LTSOPT   ? (x->val ? "On" : "Off")
                                              : label_of(x->node);
    const char *value = "";
    if (x->node == N_POWER) value = power_short[x->val & 3u];
    else if (x->node == N_CLOCK) value = clock_short_by_enum[x->val & 3u];
    else if (x->node == N_DIAG || x->node == N_HELP || x->node == N_ABOUT || x->node == N_HEALTH ||
             x->node == N_CAMERA || x->node == N_RESET || is_topic(x->node) || x->node == N_LIGHT)
        value = MT_TRI_R;                            /* JD, 10-24: ▶ for > */
    size_t      at   = 0;
    const char *icon = icon_of(x->node);
    if (icon) {
        memcpy(out, icon, 2);
        at = 2;
    } else if ((x->node >= N_POW0 && x->node <= N_CLK3) || x->node == N_EFFOPT || x->node == N_LTSOPT) {   /* an option: the saved one's check in cells 0-1 */
        if (x->bullet) memcpy(out, MT_CHECK_BOX, 2);       /* JD, 10-24: the square; the circle "doesn't look good" */
        at = 2;                                            /* in line with "◀ Back" */
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
        case RK_VALUE: {   /* P1b: 0xE000 | which << 10 | variant << 9 | value << 1 | lit (bit 0 is lit) */
            uint16_t variant = (x->node == N_SPEED && two_tone()) ? 1u : 0u;
            return (uint16_t)(0xE000u | ((uint16_t)value_which(x->node) << 10) | (variant << 9) |
                              ((uint16_t)x->val << 1) | (x->lit ? 1u : 0u));
        }
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
    else *count = row_count(f->node);
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
static bool    is_row(uint8_t s) { return s >= S_ROW0; }
static uint8_t l_chunks(uint8_t s) { return (s == S_SEP || s == S_BAR) ? 1 : is_row(s) ? 3 : 2; }
static uint8_t rows_per_piece(void) { return (uint8_t)(MB_PASS_BYTES / (CHUNK_MAX * 2u)); }   /* 4 */
static uint8_t l_pieces(uint8_t s, uint8_t c) {
    if (s == S_SEP || s == S_BAR || (is_row(s) && c == 0)) return 1;   /* a row's lead: every pixel row at once */
    uint8_t r = rows_per_piece();
    return (uint8_t)((MT_CELL_H + r - 1u) / r);
}
_Static_assert(MB_PASS_BYTES / (CHUNK_MAX * 2u) >= 1 && (MB_PASS_BYTES / (CHUNK_MAX * 2u)) * ((CHUNK_MAX + 7u) / 8u) <= BITS_MAX,
               "a half-row piece: whole pixel rows, its bits in the buffer");
_Static_assert(ROW_LEAD * MT_CELL_H * 2u <= MB_PASS_BYTES && ROW_LEAD <= 8u && MT_CELL_H <= BITS_MAX, "a row's lead: one piece");

/* One piece of a 20-cell text slot at y: chunk c, piece p's pixel rows. A
 * row (`row`) has the 4 px lead, with `cursor` (a code, or 0) in it in
 * `cursor_fg`; the title and the confirm's lines a 1 px one. */
static ms_paint_t text_piece(const char *cells, uint16_t y, bool row, uint8_t cursor, uint16_t cursor_fg, uint8_t c,
                             uint8_t p, uint16_t fg) {
    const uint8_t  lead = row ? ROW_LEAD : 1u;
    mt_line_t      l    = {cells, 20, lead, 1, false, false, cursor};
    const bool     lc   = row && c == 0;   /* the row's lead */
    uint16_t       c0, w;                  /* the chunk's line columns, [c0, c0 + w) */
    if (!row) {                            /* [0, 61), [61, 122) */
        c0 = c ? (uint16_t)(lead + HALF_W) : 0u;
        w  = c ? (uint16_t)(HALF_W + 1u) : (uint16_t)(lead + HALF_W);
    } else if (lc) {                       /* [0, 4) */
        c0 = 0;
        w  = lead;
    } else {                               /* [4, 64), [64, 125) */
        c0 = c == 1 ? lead : (uint16_t)(lead + HALF_W);
        w  = c == 1 ? (uint16_t)HALF_W : (uint16_t)(HALF_W + 1u);
    }
    const uint8_t  rpp  = rows_per_piece();
    const uint8_t  r0   = lc ? 0 : (uint8_t)(p * rpp);
    const uint8_t  rows = lc ? MT_CELL_H : (uint8_t)(r0 + rpp > MT_CELL_H ? MT_CELL_H - r0 : rpp);
    const uint8_t  st   = (uint8_t)((w + 7u) / 8u);
    uint8_t        bits[BITS_MAX];
    mt_bits(&l, c0, w, r0, rows, bits, st);
    return mb_mono(bits, st, (uint16_t)(TEXT_X - lead + c0), (uint16_t)(y + r0), w, rows, lc ? cursor_fg : fg, 0)
               ? MS_PAINT_DONE : MS_PAINT_FAILED;
}

static ms_paint_t l_paint(uint8_t s, uint8_t c, uint8_t p) {
    char cells[21];
    if (s == S_SEP) return mb_fill(4, 14, 123, 14, pal[MP_GREY]) ? MS_PAINT_DONE : MS_PAINT_FAILED;
    if (s == S_BAR) {
        uint8_t count, top;
        bar_state(&count, &top);
        if (count <= MENU_ROWS) return mb_fill(BAR_X, BAR_Y0, BAR_X, BAR_Y0 + BAR_H - 1, 0) ? MS_PAINT_DONE : MS_PAINT_FAILED;
        /* The mockup's: a track, and a thumb at least 6 px, placed by the top. */
        uint16_t th = (uint16_t)((BAR_H * MENU_ROWS + count / 2u) / count);
        if (th < 6) th = 6;
        uint16_t ty = (uint16_t)(BAR_Y0 + ((uint32_t)(BAR_H - th) * top + (count - MENU_ROWS) / 2u) / (count - MENU_ROWS));
        if (!mb_fill(BAR_X, BAR_Y0, BAR_X, BAR_Y0 + BAR_H - 1, pal[MP_TRACK])) return MS_PAINT_FAILED;
        return mb_fill(BAR_X, ty, BAR_X, (uint16_t)(ty + th - 1u), pal[MP_WHITE]) ? MS_PAINT_DONE : MS_PAINT_FAILED;
    }
    if (s == S_TITLE) {
        memset(cells, ' ', 20);
        cells[20]      = '\0';
        const char *ic = title_icon(title_id), *tt = title_text(title_id);
        size_t      ni = strlen(ic), n = strlen(tt);
        memcpy(cells, ic, ni);
        memcpy(cells + ni, tt, n > 20 - ni ? 20 - ni : n);
        return text_piece(cells, 0, false, 0, 0, c, p, pal[MP_WHITE]);
    }
    row_t x = row_at((uint8_t)(s - S_ROW0));
    row_text(&x, cells);
    uint16_t fg = x.kind != RK_ITEM && x.kind != RK_VALUE ? pal[MP_WHITE]   /* a page's text, About's lines */
                : x.lit              ? pal[MP_LIT]
                : (x.node == N_BACK || x.node == N_EXIT) ? pal[MP_LEAVE]
                                     : pal[MP_DIM];
    /* The cursor (JD, 10-24): in the lit row's lead, in its own colour. */
    return text_piece(cells, (uint16_t)(ROW_Y0 + (s - S_ROW0) * ROW_H), true, x.lit ? (uint8_t)MT_CURSOR[0] : 0u,
                      pal[MP_CURSOR], c, p, fg);
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
    return text_piece(cells, c_y[s - 2u], false, 0, 0, c, p, pal[MP_WHITE]);
}
static const ms_layout_t L_CONFIRM = {6, c_sig, c_chunks, c_pieces, c_paint, 0, 0};

/* --- The editors (P1b; C8's geometry) ----------------------------------------
 * Range: the title, the separator, the bar as 7 column bands, the value, the
 * three hints. Hue: the title, the separator, the envelope as 16 column
 * bands, the value, the hints. The bands come first, so the bar or the marker
 * moves at once and the number follows (as the scrollbar leads the rows).
 * Each band is one piece (<= 336 B) of generated pixels; its signature names
 * every pixel it paints. */
#define BAR_X0  13          /* the outline: x 13-114, y 51-60 */
#define BAR_X1  114
#define BAR_YT  51
#define BAR_YB  60
#define BAR_BW  16          /* bands of 16 columns, the last 6 */
#define BAR_N   7
#define ENV_Y0  20          /* the envelope: x 0-127, y 20-40 */
#define ENV_Y1  40
#define ENV_BW  8
#define ENV_N   16
#define STRIP_X0 4          /* the strip: x 4-123, y 22-35 */
#define STRIP_X1 123
#define STRIP_YT 22
#define STRIP_YB 35
#define VAL_X   14          /* the value: 10 big cells, centred */
#define VAL_CELLS 10
#define HINT_Y0 72
static const char *const hint_lines[3] = {"Turn: change", "Click: save", "Hold: cancel"};

static uint8_t e_bands(void) { return is_hue_editor(cur()->node) ? ENV_N : BAR_N; }
static uint8_t e_value_slot(void) { return (uint8_t)(2u + e_bands()); }
static uint8_t e_range_max(void) { return cur()->node == N_SCREEN ? (uint8_t)MENU_LCD_MAX : 255u; }

/* The bar's fill edge: the first column not filled, 14 (empty) to 114 (full). */
static uint8_t bar_edge(void) {
    uint16_t max = e_range_max();
    return (uint8_t)(BAR_X0 + 1u + (me_value() * 100u + max / 2u) / max);
}
/* The marker's column for the hue it shows: Second color's lands at hue +
 * speed. 4 + round(a * 120 / 256): 4-124. */
static uint8_t marker_x(void) {
    uint8_t a = me_value();
    if (cur()->node == N_SPEED) {
        env_rgb_t c;
        env_rgb_get(&c);
        a = (uint8_t)(c.h + a);
    }
    return (uint8_t)(STRIP_X0 + (a * 120u + 128u) / 256u);
}
/* A hue at full saturation and value, RGB565 (QMK's sextant method). */
static uint16_t hue565(uint8_t h) {
    uint8_t region = (uint8_t)(h / 43u), rem = (uint8_t)((h - region * 43u) * 6u);
    uint8_t r, g, b;
    switch (region) {
        case 0:  r = 255; g = rem; b = 0; break;
        case 1:  r = (uint8_t)(255u - rem); g = 255; b = 0; break;
        case 2:  r = 0; g = 255; b = rem; break;
        case 3:  r = 0; g = (uint8_t)(255u - rem); b = 255; break;
        case 4:  r = rem; g = 0; b = 255; break;
        default: r = 255; g = 0; b = (uint8_t)(255u - rem); break;
    }
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

typedef struct { uint8_t x0, edge; uint16_t grey, white; } bar_ctx_t;
static uint16_t bar_px(const void *v, uint16_t col, uint16_t row) {
    const bar_ctx_t *c = v;
    uint16_t         x = (uint16_t)(c->x0 + col), y = (uint16_t)(BAR_YT + row);
    if (x == BAR_X0 || x == BAR_X1 || y == BAR_YT || y == BAR_YB) return c->grey;
    return x < c->edge ? c->white : 0;
}
typedef struct { uint8_t x0, mx; uint16_t white, tri; uint16_t col[ENV_BW]; } env_ctx_t;
static uint16_t env_px(const void *v, uint16_t col, uint16_t row) {
    const env_ctx_t *c = v;
    int16_t          x = (int16_t)(c->x0 + col), y = (int16_t)(ENV_Y0 + row), d = (int16_t)(x - c->mx);
    if (y >= 38) return (d < 0 ? -d : d) <= 2 - (y - 38) ? c->tri : 0;   /* the triangle: 5, 3, 1 */
    if (d == 0) return c->white;                                         /* the line, y 20-37 */
    if (y >= STRIP_YT && y <= STRIP_YB && x >= STRIP_X0 && x <= STRIP_X1) return c->col[col];
    return 0;
}

/* A band's signature: the bar's, where the edge falls in it (the outline and
 * the background are fixed); the envelope's, the marker's centre relative to
 * it, -2..9 (the triangle reaches two columns either side), or none. */
static uint16_t band_sig(uint8_t k) {
    if (!is_hue_editor(cur()->node)) {
        int16_t x0 = (int16_t)(BAR_X0 + k * BAR_BW), w = k + 1 == BAR_N ? (int16_t)(BAR_X1 - x0 + 1) : BAR_BW;
        int16_t st = (int16_t)(bar_edge() - x0);
        if (st < 0) st = 0;
        if (st > w) st = w;
        return (uint16_t)(0x4000u | ((uint16_t)k << 5) | (uint16_t)st);
    }
    int16_t rel = (int16_t)(marker_x() - k * ENV_BW);
    return (uint16_t)(0x8000u | ((uint16_t)k << 5) | (rel >= -2 && rel <= 9 ? (uint16_t)(rel + 3) : 0u));
}
static ms_paint_t band_paint(uint8_t k) {
    if (!is_hue_editor(cur()->node)) {
        bar_ctx_t c = {(uint8_t)(BAR_X0 + k * BAR_BW), bar_edge(), pal[MP_GREY], pal[MP_WHITE]};
        uint16_t  w = k + 1 == BAR_N ? (uint16_t)(BAR_X1 - c.x0 + 1u) : BAR_BW;
        return mb_pixels(c.x0, BAR_YT, w, BAR_YB - BAR_YT + 1u, bar_px, &c) ? MS_PAINT_DONE : MS_PAINT_FAILED;
    }
    env_ctx_t c = {(uint8_t)(k * ENV_BW), marker_x(), pal[MP_WHITE], pal[MP_LIT], {0}};
    for (uint8_t i = 0; i < ENV_BW; i++) {
        uint16_t x = (uint16_t)(c.x0 + i);
        if (x >= STRIP_X0 && x <= STRIP_X1) c.col[i] = hue565((uint8_t)(((x - STRIP_X0) * 256u + 60u) / 120u));
    }
    return mb_pixels(c.x0, ENV_Y0, ENV_BW, ENV_Y1 - ENV_Y0 + 1u, env_px, &c) ? MS_PAINT_DONE : MS_PAINT_FAILED;
}

/* The value, centred in its 10 big cells. */
static void value_cells(char *out) {
    char b[12];
    fmt_value(cur()->node, me_value(), b, sizeof b);
    size_t n = strlen(b), at = n < VAL_CELLS ? (VAL_CELLS - n) / 2u : 0u;
    memset(out, ' ', VAL_CELLS);
    out[VAL_CELLS] = '\0';
    memcpy(out + at, b, n > VAL_CELLS ? VAL_CELLS : n);
}

static uint16_t e_sig(uint8_t s) {
    if (s == S_TITLE) return title_id;
    if (s == S_SEP) return 1;
    uint8_t vs = e_value_slot();
    if (s < vs) return band_sig((uint8_t)(s - 2u));
    if (s == vs) return (uint16_t)(((uint16_t)field_of(cur()->node) << 8) | me_value());
    return (uint16_t)(0x100u | (s - vs));   /* a hint: fixed */
}
static uint8_t e_chunks(uint8_t s) {
    if (s == S_SEP) return 1;
    uint8_t vs = e_value_slot();
    if (s >= 2 && s < vs) return 1;
    return s == vs ? VAL_CELLS : 2;   /* the value: a glyph a chunk; the title and the hints: half rows */
}
static uint8_t e_pieces(uint8_t s, uint8_t c) {
    uint8_t vs = e_value_slot();
    if (s == S_SEP || (s >= 2 && s <= vs)) return 1;
    return l_pieces(S_TITLE, c);
}
static ms_paint_t e_paint(uint8_t s, uint8_t c, uint8_t p) {
    if (s <= S_SEP) return l_paint(s, c, p);
    uint8_t vs = e_value_slot();
    if (s < vs) return band_paint((uint8_t)(s - 2u));
    if (s == vs) {
        char cells[VAL_CELLS + 1];
        value_cells(cells);
        uint16_t y = is_hue_editor(cur()->node) ? 44u : 22u;
        return mb_glyph_big(cells[c], (uint16_t)(VAL_X + c * MB_BIG_W), y) ? MS_PAINT_DONE : MS_PAINT_BUSY;
    }
    char        cells[21];
    const char *ln = hint_lines[s - vs - 1u];
    memset(cells, ' ', 20);
    cells[20] = '\0';
    memcpy(cells, ln, strlen(ln));
    return text_piece(cells, (uint16_t)(HINT_Y0 + (s - vs - 1u) * ROW_H), false, 0, 0, c, p, pal[MP_WHITE]);
}
static const ms_layout_t L_RANGE = {2 + BAR_N + 1 + 3, e_sig, e_chunks, e_pieces, e_paint, 0, 0};
static const ms_layout_t L_HUE   = {2 + ENV_N + 1 + 3, e_sig, e_chunks, e_pieces, e_paint, 0, 0};
_Static_assert(2 + ENV_N + 1 + 3 <= MS_MAX_SLOTS, "the hue editor's slots");
_Static_assert(ENV_BW * (ENV_Y1 - ENV_Y0 + 1) * 2 <= MB_PASS_BYTES && BAR_BW * (BAR_YB - BAR_YT + 1) * 2 <= MB_PASS_BYTES,
               "a band is one piece");

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
    (void)me_take_pending();   /* changes from before the session aren't its targets */
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
    (void)me_take_pending();
    push(N_TOP, row_index(N_TOP, N_DIAG));   /* Diagnostics' row, wherever it is */
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
    uint8_t from = cur()->node;
    if (is_editor(from) || is_preview(from)) me_cancel();   /* never leave an edit open (a no-op once saved) */
    depth--;
    if (is_editor(from)) {   /* back to its list: a layout change, both ways (ms_set_layout's clear) */
        retitle(now);
        ms_set_layout(&L_LIST, false);
    } else
        show(false, from == N_RESET, now);
    return MENU_NONE;
}

/* P1b: an editor opens on its field's edit, its own layout. */
static void open_editor(uint8_t node, uint32_t now) {
    me_open(field_of(node));
    ft_have = false;
    push(node, 0);
    retitle(now);
    ms_set_layout(is_hue_editor(node) ? &L_HUE : &L_RANGE, false);
}

/* A turn in an editor: the field's next value, previewed. Fast (the mockup's
 * rule, codex's predicate): two or more detents in one input, or one within
 * 90 ms of the editor's last turn. A range clamps; a hue wraps. */
static void edit_turn(int8_t turns, uint32_t now) {
    bool fast = turns >= 2 || turns <= -2 || (ft_have && (uint32_t)(now - ft_last) < 90u);
    ft_have   = true;
    ft_last   = now;
    uint8_t node = cur()->node;
    int16_t v    = me_value();
    if (is_hue_editor(node))
        v = (int16_t)((v + turns * (fast ? 12 : 3)) & 255);
    else {
        int16_t max = e_range_max(), step = node == N_SCREEN ? 1 : (fast ? 20 : 5);
        v = (int16_t)(v + turns * step);
        if (v < 0) v = 0;
        if (v > max) v = max;
    }
    me_preview((uint8_t)v);
    ms_retarget();
}

static menu_result_t activate(uint32_t now) {
    frame_t *f = cur();
    uint8_t  val, node = row_node(f->node, f->cursor, &val);
    if (node == N_NONE) return MENU_NONE;
    if (node == N_BACK) {
        if (is_choice(f->node)) flash(T_CANCELLED, now);   /* a previewing choice's: pop() cancels it */
        return pop(now);
    }
    if (is_preview(f->node)) {   /* P1b: save the option under the cursor (already previewed) */
        me_preview(option_value(node, val));
        me_save();
        flash(T_SAVED, now);
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
        case N_EFFECT:
        case N_LIGHTS:   /* P1b: a previewing choice opens its edit, then on the saved option */
            me_open(field_of(node));
            push(node, saved_row(node));
            show(false, false, now);
            return MENU_NONE;
        case N_SCREEN:
        case N_BRIGHT:
        case N_HUE:
        case N_SAT:
        case N_SPEED:
            open_editor(node, now);
            return MENU_NONE;
        case N_DIAG:
        case N_HELP:
        case N_LIGHT:
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
        if (is_editor(f->node))
            edit_turn(turns, now);
        else if (is_list(f->node) || is_choice(f->node)) {
            uint8_t n = row_count(f->node);
            int16_t c = (int16_t)((f->cursor + turns) % (int16_t)n);
            if (c < 0) c += n;
            f->cursor = (uint8_t)c;
            f->top    = (uint8_t)(f->cursor / MENU_ROWS * MENU_ROWS);   /* lists page by 8 */
            if (is_preview(f->node)) {   /* P1b: the option under the cursor shows at once; on Back, the old value */
                uint8_t val, node = row_node(f->node, f->cursor, &val);
                me_preview(node == N_BACK ? me_saved() : option_value(node, val));
            }
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
        if (is_editor(f->node)) {   /* P1b: save */
            me_save();
            flash(T_SAVED, now);
            return pop(now);
        }
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
    if (latch == MI_LATCH_BACK) {   /* pop() cancels an editor's or a previewing choice's edit */
        if (is_choice(f->node) || is_editor(f->node) || (f->node == N_RESET && confirm_page == 0))
            flash(T_CANCELLED, now);
        return pop(now);
    }
    if (changed && ms_tgen() == g0) ms_retarget();   /* nothing navigated: the model's change is the target */
    return MENU_NONE;
}

/* The title the screen wants now: a flash, the hint (lists only), or its own. */
static uint8_t want_title(uint32_t now, uint32_t idle_ms) {
    if (flash_id && (uint16_t)((uint16_t)now - flash_t0) < MENU_FLASH_MS) return flash_id;
    flash_id = 0;
    if (is_list(cur()->node) && idle_ms >= MENU_HINT_FROM) {
        uint32_t ph = idle_ms % MENU_HINT_EVERY;   /* 4 s the title, 1 s each hint */
        if (ph >= MENU_HINT_FROM) return ph < MENU_HINT_FROM + 1000u ? T_HINT : T_HINT_BACK;
    }
    return screen_title();
}
static void retitle(uint32_t now) { title_id = want_title(now, last_idle); }

void menu_tick(uint32_t now, uint32_t idle_ms) {
    if (!depth) return;
    last_idle = idle_ms;
    /* P1b: an external write is a new target (codex, round two); one that
     * cancelled the open edit also pops it, with "Cancelled". */
    uint8_t pend = me_take_pending();
    if ((pend & ME_PEND_CANCELLED) && (is_editor(cur()->node) || is_preview(cur()->node))) {
        flash(T_CANCELLED, now);
        (void)pop(now);
    }
    bool moved = sync_model() || pend;   /* Fn+B, Fn+C, while the menu shows */
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
    if (is_editor(f->node)) return me_value();   /* P1b: an editor's value */
    return f->cursor;
}
