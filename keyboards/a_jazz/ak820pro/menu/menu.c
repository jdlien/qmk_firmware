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
#include "profiles.h"   /* P5b: the settings profiles' pages */
#include "../graphics/res/menu_text_pages.h"
#include "../graphics/batt_text.h"   /* P4: the Battery choice previews the dashboard's text */

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
    N_BOOT,                                                     /* 34: the bootloader item (JD, 10-24) */
    N_IND, N_CONN,                                              /* 35-36: P2a (JD, 10-25) */
    N_LOCKS, N_CHGL,                                            /* 37-38: Indicators' rows */
    N_CONNINFO,                                                 /* 39: P3, Connection off BT */
    N_BT0, N_BT1, N_BT2,                                        /* 40-42: Connection's slots */
    N_PAIRNOW,                                                  /* 43: "Pair device to BT n", the current slot (JD, 10-25) */
    N_PAIRING,                                                  /* 44: the Pairing page, its slot in the frame's cursor */
    N_BATT, N_BV0, N_BV1,                                       /* 45-47: P4, Battery and its two readouts */
    N_CLK4, N_CLK5,                                             /* 48-49: the Clock's rows 5-6 (rows: clock_enum[]) */
    N_PROFILES,                                                 /* 50: P5b, the settings profiles (plans/SETTINGS-PROFILES-PLAN.md) */
    N_PSLOT,                                                    /* 51: a saved slot's row, its slot in val; as a frame
                                                                 * its page, its slot in pslot (P5d: one node, eight slots) */
    N_PNEW,                                                     /* 52: P5d, Profiles' last row: Save new */
    N_PLOAD, N_PSAVE, N_PDELETE,                                /* 53-55: its actions; as frames, Save's and Delete's
                                                                 * confirms, the slot in the cursor */
    N_COUNT
};
_Static_assert(N_COUNT <= 63, "a row's node is 6 bits");
_Static_assert(MENU_HELP_TOPICS == 4, "Help's four topics");
static bool is_pslot(uint8_t node) { return node == N_PSLOT; }

/* The top level's order (P4, JD 10-25): Battery under Clock, and
 * Diagnostics under Bootloader on page two. P5b (JD, 10-25): Profiles is
 * page one's last row, and Bootloader moves to the top of page two, with the
 * system items. */
static const uint8_t top_rows[]   = {N_LIGHT, N_SCREEN, N_CONN, N_POWER, N_IND,  N_CLOCK,
                                     N_BATT,  N_PROFILES, N_BOOT, N_DIAG, N_HELP, N_ABOUT, N_EXIT};
static const uint8_t light_rows[] = {N_BACK, N_EFFECT, N_BRIGHT, N_HUE, N_SAT, N_SPEED, N_LIGHTS};
static const uint8_t ind_rows[]   = {N_BACK, N_LOCKS, N_CHGL};
static const uint8_t conn_rows[]  = {N_BACK, N_BT0, N_BT1, N_BT2, N_PAIRNOW};   /* P3: on BT */
static const uint8_t diag_rows[]  = {N_BACK, N_HEALTH, N_CAMERA, N_RESET};
static const uint8_t help_rows[]  = {N_BACK, N_TOPIC0, N_TOPIC1, N_TOPIC2, N_TOPIC3};
static const uint8_t power_rows[] = {N_BACK, N_POW0, N_POW1, N_POW2, N_POW3};
static const uint8_t clock_rows[] = {N_BACK, N_CLK0, N_CLK1, N_CLK2, N_CLK3, N_CLK4, N_CLK5};
static const uint8_t batt_rows[]  = {N_BACK, N_BV0, N_BV1};   /* P4 */
/* P5d: Profiles' rows are Back, the saved slots in slot order, then Save
 * new (row_count(), row_node()), no empty slot shown (JD, 10-26: "just a
 * 'Save New' makes more sense than finite grayed out unused slots"). */
static const uint8_t pslot_rows[] = {N_BACK, N_PLOAD, N_PSAVE, N_PDELETE};
/* A slot page's summary: three lines under its actions (Decided 10), as the
 * Battery page places its notes. */
#define PSUM_ROW0  5u
#define PSUM_LINES 3u
_Static_assert(sizeof pslot_rows <= PSUM_ROW0 && PSUM_ROW0 + PSUM_LINES <= MENU_ROWS, "the summary under the actions");

static const char *const power_full[4]  = {"Always on", "Balanced", "Power saver", "Lights out"};
static const char *const power_short[4] = {"Always on", "Balanced", "Saver", "Lights out"};
/* The choice's rows in MENU-TEXT's order (10-25: the modes without seconds),
 * and the clock enum each maps to: 24h:ss = 24H (0), 24h = 24H_HM (4),
 * 12h:ss = 12H (1), 12h = 12H_HM (5), Date = DATE (3), Off = OFF (2). Each
 * row but Off shows a live example of its format (clock_example()). */
#define CLOCK_ROWS 6
static const char *const clock_full[CLOCK_ROWS] = {"24h:ss", "24h", "12h:ss", "12h", "Date", "Off"};   /* JD: as Fn+C names them */
static const uint8_t     clock_enum[CLOCK_ROWS] = {0, 4, 1, 5, 3, 2};
static const char *const clock_short_by_enum[CLOCK_ROWS] = {"24h:ss", "12h:ss", "Off", "Date", "24h", "12h"};
/* A Clock row's index (0-5), or -1. */
static int8_t clk_row(uint8_t node) {
    if (node >= N_CLK0 && node <= N_CLK3) return (int8_t)(node - N_CLK0);
    if (node == N_CLK4 || node == N_CLK5) return (int8_t)(4 + node - N_CLK4);
    return -1;
}
static const char *clock_example(uint8_t row, char *buf);   /* by row_sig(), below */
/* P4: the battery readout, by its row (0 percent, 1 hours), and the top
 * level's value. */
static const char *const batt_full[2]  = {"Percent", "Hours of use"};
static const char *const batt_short[2] = {"Percent", "Hours"};
/* P4 (JD, 10-25): the choice previews each readout as the dashboard shows it
 * on battery, and a note under it says why the hours don't show yet. Read
 * live: the rows' signatures follow the text (row_sig), so a change repaints
 * them without anything stored. */
static bool batt_hours_ready(const env_batt_t *b) {
    return b->on_battery && b->avg_minutes >= 60u && b->left_min != 0xFFFFu;
}
static const char *batt_preview(bool hours, char *buf) {   /* buf: BATT_TEXT_MAX + 1 at least */
    env_batt_t b;
    env_batt_facts(&b);
    if (b.pct > 100u) return "--";
    if (b.pct == 0u && b.on_battery) return "Low";
    if (hours && !batt_hours_ready(&b)) return "--";
    batt_level_text(buf, b.pct, b.on_battery, b.left_min, b.avg_minutes >= 60u, hours);
    return buf;
}
static void batt_note(uint8_t line, char *buf, size_t n) {
    env_batt_t b;
    env_batt_facts(&b);
    buf[0] = '\0';
    if (batt_hours_ready(&b) || line > 2u) return;
    /* On battery at the cut (Low) or with no level, the previews say so, and
     * no wait will bring the hours (codex). */
    if (b.on_battery && (b.pct == 0u || b.pct > 100u)) return;
    if (b.avg_minutes < 60u) {
        static const char *const warm[2] = {"Hours show after an", "hour on battery:"};
        if (line < 2u) snprintf(buf, n, "%s", warm[line]);
        else           snprintf(buf, n, "%u min to go.", (unsigned)(60u - b.avg_minutes));
    } else if (!b.on_battery && line < 2u) {
        static const char *const off[2] = {"Hours show only", "on battery."};
        snprintf(buf, n, "%s", off[line]);
    }
}
/* P3: the top level's Connection value by link code (0 USB, 1 2.4G, 2-4 BT
 * 1-3), and the current slot's link in six cells by ENV_LINK_* state. */
static const char *const link_names[5] = {"USB", "2.4G", "BT 1", "BT 2", "BT 3"};
static const char *const link_words[5] = {"", "Trying", "Pair", "Linked", "Failed"};
/* The current slot's icon in its check box's cells, by the same state (JD,
 * 10-25: "a green checkmark beside Linked ... red x in a circle for
 * Failed"; the box stays, green): idle, linking, advertising, linked,
 * failed. Its colour, fixed; RGB565. The box is green, as on every page. */
static const char *const link_icons[5]   = {MT_CHECK_BOX, MT_I_WAIT, MT_I_CONN, MT_CHECK_BOX, MT_I_FAIL};
static const uint16_t    link_colour[5]  = {MENU_SAVED, 0x9492u /* #909090 */, 0x3CFFu /* #3C9CFF */, MENU_SAVED,
                                            0xFACBu /* #FF5A5A */};
static const char *const slot_names[3] = {"Bluetooth 1", "Bluetooth 2", "Bluetooth 3"};
/* Pair a device's row, by the slot it pairs (JD, 10-25: "Pair (a )device to
 * BT N"; with the "a" it's 21 cells). */
static const char *const pair_labels[3] = {"Pair device to BT 1", "Pair device to BT 2", "Pair device to BT 3"};

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
        case N_BATT:   return "Battery";   /* P4 */
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
        case N_BOOT:   return "Bootloader";
        case N_IND:    return "Indicators";
        case N_CONN:   return "Connection";
        case N_LOCKS:  return "Lock lights";
        case N_CHGL:   return "Charging";   /* the mockup's row; its editor's title is "Charging light" */
        case N_PROFILES: return "Profiles";   /* P5b; a slot's row is its name (row_text) */
        case N_PLOAD:  return "Load";
        case N_PSAVE:  return "Save current here";
        case N_PDELETE: return "Delete";
        case N_PNEW:   return "+ Save new";   /* P5d: the `+` in the check box's cells, the names' column */
        default: break;
    }
    if (node >= N_BT0 && node <= N_BT2) return slot_names[node - N_BT0];
    if (node >= N_TOPIC0 && node <= N_TOPIC3) return menu_help_titles[node - N_TOPIC0];
    if (node >= N_POW0 && node <= N_POW3) return power_full[node - N_POW0];
    if (clk_row(node) >= 0) return clock_full[clk_row(node)];
    if (node >= N_BV0 && node <= N_BV1) return batt_full[node - N_BV0];
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
        case N_BOOT:  return MT_I_BOOT;
        case N_IND:   return MT_I_IND;    /* P2a: the sun (JD's pick) */
        case N_CONN:  return MT_I_CONN;   /* Bluetooth */
        case N_BATT:  return MT_I_BATT " ";   /* P4: a vertical battery */
        case N_PROFILES: return MT_I_PROF " ";   /* P5b: the folder */
        default:      return NULL;
    }
}

/* --- The palette ------------------------------------------------------------- */
/* Const on every build since the settings profiles (10-25): dev's live copy
 * (op 27 sub 13, menu_tune.py) gave its 14 B of RAM to them. JD: "I probably
 * don't need live colour tuning anymore." */
static const uint16_t pal[MP_N] = {MENU_LIT, MENU_DIM, MENU_LEAVE, MENU_WHITE, MENU_GREY, MENU_TRACK, MENU_CURSOR};
uint16_t menu_color(uint8_t i) { return i < MP_N ? pal[i] : 0; }

/* --- State ------------------------------------------------------------------ */

typedef struct { uint8_t node, cursor, top; } frame_t;
static frame_t  stack[4];   /* P5b: the top level, Profiles, a slot's page, its confirm */
static uint8_t  depth;
static uint8_t  model_power, model_clock;   /* the latched model (external changes: Fn+B, Fn+C) */
static uint8_t  title_id;                   /* the latched title: a screen's, a flash's or the hint */
static uint8_t  flash_id;                   /* 0 none */
static uint16_t flash_t0;                   /* 16-bit ms */
static uint8_t  confirm_page;               /* Reset counters: 0 the question, 1 done */
/* Page-scoped: About's snapshot, frozen while it shows, or the Clock list's
 * time, read once an RTC second (clock_now()). The two never show together,
 * and each is taken as its page opens. */
static union {
    env_about_t snap;
    struct {
        env_time_t t;
        uint16_t   tick;   /* env_rtc_tick() at the read */
        uint8_t    have;   /* not bools: About's bytes would be no bool's */
        uint8_t    st;     /* CK_* */
    } clk;
} pg;
enum { CK_OK = 1, CK_JUMP = 2 };   /* set; a read that wasn't the last's time on, for menu_tick() */
#define CLOCK_EXAMPLES 5u          /* the Clock page's rows 1-5 (Off has none) */
_Static_assert(1 + CLOCK_ROWS <= MENU_ROWS, "the Clock page is one screen: its examples are slots S_ROW0 + 1-5");
_Static_assert(sizeof pg == sizeof(env_about_t), "the Clock's time shares About's 12 B");
static uint32_t ft_last;                    /* P1b: the editor's last turn, ms (32-bit: the backstop is 180 s) */
/* P5b: the profiles as the rows show them. Each row read the store itself
 * at every scan, a crc or two a time: 1.8 ms a pass more on the top level
 * with a profile active (the board, 10-26). Read once a pass, it still cost
 * 0.55 ms a pass with every slot empty: this core's main loop has a few MHz
 * under the row ISR. So it's read at most every 128 ms, and at once after a
 * click or a host's write; a key's load or a live change shows within the
 * 128 ms. pfs (P5d's eight slots): bits 0-7 the valid slots, 8-11 the
 * active one (15 none), 12 its `*`. pfs_flags: PFS_STALE, PFS_FAILED
 * (prof_write_failed() at the last tick, so its rise says so) and, bits
 * 2-7, the 128 ms tick of the last read. A row's first need after it's
 * stale reads it afresh, so nothing is read on pages without these rows. */
enum { PFS_STALE = 1, PFS_FAILED = 2, PFS_TICKS = 0xFC };
static uint16_t pfs;
static uint8_t  pfs_flags = PFS_STALE;
static uint8_t  pslot;                      /* P5d: the open slot page's slot (the stack holds one at most) */
/* P5e: what the preview shows: a slot (its preview's edit open, or the
 * live settings already that slot's after a Load), PV_LIVE the live
 * settings, PV_HELD the live settings since an external write rolled the
 * preview back, until the knob moves (the write would hide under it). */
enum { PV_HELD = 0xFE, PV_LIVE = 0xFF };
static uint8_t  pv = PV_LIVE;
static bool     ft_have;                    /* a turn in this editor yet */
static uint8_t  model_link, model_pf;       /* P3: env_link() and env_bt_pair_facts(), latched as the model */

static frame_t *cur(void) { return &stack[depth - 1]; }

/* P3's link, from the latch. */
static uint8_t link_slot(void) { return (uint8_t)((model_link >> 2) & 3u); }
static uint8_t link_state(void) { return (uint8_t)((model_link >> 4) & 7u); }
static bool    on_bt(void) { return (model_link & 3u) == ENV_LINK_BT; }
static uint8_t link_code(void) {   /* the top level's value: link_names[] */
    uint8_t tr = model_link & 3u;
    return tr == ENV_LINK_BT ? (uint8_t)(2u + (link_slot() > 2u ? 2u : link_slot())) : tr == ENV_LINK_24G ? 1u : 0u;
}
/* The Pairing page's phase (the design's order): the latch and its slot.
 * Unsure: advertising as far as the driver knows (its optimistic PAIRING),
 * but the module never confirmed and nothing is being resent: a lost 5B 31
 * looks the same as a module that never started (codex), so it can still be
 * stopped. */
enum { PP_START = 0, PP_OPEN, PP_DONE, PP_ENDED, PP_FAILED, PP_UNSURE };
static uint8_t pair_phase(uint8_t slot) {
    if (!on_bt() || link_slot() != slot) return PP_ENDED;
    if (model_pf & ENV_PF_WAITING) return PP_START;
    uint8_t st = link_state();
    if (model_pf & ENV_PF_CONFIRMED) return st == ENV_LINK_PAIRING ? PP_OPEN : st == ENV_LINK_CONNECTED ? PP_DONE : PP_ENDED;
    if (model_pf & ENV_PF_RETRYING) return PP_START;
    return st == ENV_LINK_PAIRING ? PP_UNSURE : PP_FAILED;
}
static bool pair_stoppable(uint8_t phase) { return phase == PP_START || phase == PP_OPEN || phase == PP_UNSURE; }

static const uint8_t *rows_of(uint8_t node, uint8_t *n) {
    switch (node) {
        case N_TOP:   *n = sizeof top_rows;   return top_rows;
        case N_DIAG:  *n = sizeof diag_rows;  return diag_rows;
        case N_HELP:  *n = sizeof help_rows;  return help_rows;
        case N_POWER: *n = sizeof power_rows; return power_rows;
        case N_CLOCK: *n = sizeof clock_rows; return clock_rows;
        case N_BATT:  *n = sizeof batt_rows;  return batt_rows;
        case N_LIGHT: *n = sizeof light_rows; return light_rows;
        case N_IND:   *n = sizeof ind_rows;   return ind_rows;
        case N_CONN:  *n = sizeof conn_rows;  return conn_rows;
        case N_PSLOT: *n = sizeof pslot_rows; return pslot_rows;   /* P5b; Profiles' own: row_count(), row_node() */
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
static uint8_t pfs_saved(void);
static uint8_t pfs_nth(uint8_t k);
static uint8_t row_count(uint8_t node) {
    if (node == N_EFFECT) return (uint8_t)(1u + effects());
    if (node == N_LIGHTS) return 3;
    if (node == N_PROFILES) return (uint8_t)(2u + pfs_saved());   /* P5d: Back, the saved slots, Save new */
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
    if (node == N_PROFILES) {   /* P5d: row i the (i - 1)th saved slot, its slot in val; then Save new */
        if (!i) return N_BACK;
        uint8_t s = pfs_nth((uint8_t)(i - 1u));
        if (s < PROF_SLOTS) {
            *val = s;
            return N_PSLOT;
        }
        return i == 1u + pfs_saved() ? N_PNEW : N_NONE;
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
static bool is_list(uint8_t node) {
    return node == N_TOP || node == N_DIAG || node == N_HELP || node == N_LIGHT || node == N_IND || node == N_CONN ||
           node == N_PROFILES || is_pslot(node);
}
static bool is_bt_screen(uint8_t node) { return node == N_CONN || node == N_PAIRING; }   /* P3: on BT only */
static bool is_preview(uint8_t node) { return node == N_EFFECT || node == N_LIGHTS; }   /* P1b's previewing choices */
static bool is_choice(uint8_t node) { return node == N_POWER || node == N_CLOCK || node == N_BATT || is_preview(node); }
static bool is_option(uint8_t node) {   /* a row a check box marks: a choice's option, or a slot (P3; P5b's) */
    return (node >= N_POW0 && node <= N_CLK3) || node == N_EFFOPT || node == N_LTSOPT || (node >= N_BT0 && node <= N_BT2) ||
           (node >= N_BV0 && node <= N_BV1) || node == N_CLK4 || node == N_CLK5 || is_pslot(node);
}
static bool is_ind_editor(uint8_t node) { return node == N_LOCKS || node == N_CHGL; }   /* P2a */
static bool is_editor(uint8_t node) { return node == N_SCREEN || (node >= N_BRIGHT && node <= N_SPEED) || is_ind_editor(node); }
static bool is_confirm(uint8_t node) {   /* the confirm geometry's pages */
    return node == N_RESET || node == N_BOOT || node == N_CONNINFO || node == N_PAIRING || node == N_PSAVE ||
           node == N_PDELETE;
}
static bool is_prof_screen(uint8_t node) {   /* P5b: a host restore pops these */
    return node == N_PROFILES || is_pslot(node) || node == N_PSAVE || node == N_PDELETE;
}
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
        case N_LOCKS:  return ME_LOCKS;
        case N_CHGL:   return ME_CHG;
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
    if (node == N_BATT) return env_batt_hours() ? 2u : 1u;   /* P4 */
    for (uint8_t i = 0; i < CLOCK_ROWS; i++)
        if (clock_enum[i] == model_clock) return (uint8_t)(1u + i);
    return 1;
}

/* --- Titles ------------------------------------------------------------------ */
enum { T_TOP = 1, T_POWER, T_CLOCK, T_DIAG, T_HELP, T_ABOUT, T_TOPIC0, T_RESET = T_TOPIC0 + 4,
       T_SAVED = 16, T_CANCELLED, T_RESET_DONE, T_HINT, T_HINT_BACK,
       T_LIGHT, T_SCREEN, T_EFFECT, T_BRIGHT, T_HUE, T_SAT, T_SPEED, T_SECOND, T_LIGHTS,   /* 21-29: P1b */
       T_BOOT, T_BOOT_BUSY,                                                                 /* 30-31 */
       T_IND, T_CONN, T_LOCKS, T_CHGL,                                                      /* 32-35: P2a */
       T_PAIRING, T_NOW_BT1, T_NOW_BT2, T_NOW_BT3,                                          /* 36-39: P3 */
       T_ALREADY_BT1, T_ALREADY_BT2, T_ALREADY_BT3, T_PAIR_STOPPED,                         /* 40-43 */
       T_BATT,                                                                              /* 44: P4 */
       T_PROFILES, T_PSLOT0, T_PSAVE = T_PSLOT0 + PROF_SLOTS, T_PDELETE,                    /* 45-55: P5b; a slot's page's title is its name */
       T_LOADED, T_LOADED_NOFX, T_DELETED, T_PBUSY, T_SAVE_FAILED,                          /* 56-60 */
       T_PFULL };                                                                           /* 61: P5d */
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
        case N_BOOT:   return T_BOOT;
        case N_IND:    return T_IND;
        case N_CONN:
        case N_CONNINFO: return T_CONN;
        case N_LOCKS:  return T_LOCKS;
        case N_CHGL:   return T_CHGL;
        case N_PAIRING:  return T_PAIRING;
        case N_BATT:   return T_BATT;
        case N_PROFILES: return T_PROFILES;   /* P5b */
        case N_PSAVE:  return T_PSAVE;
        case N_PDELETE: return T_PDELETE;
        default:      return is_topic(n)   ? (uint8_t)(T_TOPIC0 + (n - N_TOPIC0))
                             : is_pslot(n) ? (uint8_t)(T_PSLOT0 + pslot)
                                           : T_TOP;
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
        case T_BOOT:       return "Bootloader";
        case T_BOOT_BUSY:  return "Try again";   /* a key's release didn't go over the radio */
        case T_IND:        return "Indicators";
        case T_CONN:       return "Connection";
        case T_LOCKS:      return "Lock lights";
        case T_CHGL:       return "Charging light";
        case T_PAIRING:    return "Pairing";
        case T_NOW_BT1:    return "Now on BT 1";
        case T_NOW_BT2:    return "Now on BT 2";
        case T_NOW_BT3:    return "Now on BT 3";
        case T_ALREADY_BT1: return "Already on BT 1";
        case T_ALREADY_BT2: return "Already on BT 2";
        case T_ALREADY_BT3: return "Already on BT 3";
        case T_PAIR_STOPPED: return "Pairing stopped";
        case T_BATT:       return "Battery";
        /* P5b's; the words wait for JD's text pass (MENU-TEXT.md) */
        case T_PROFILES:   return "Profiles";
        case T_PSAVE:      return "Save profile";
        case T_PDELETE:    return "Delete profile";
        case T_LOADED:     return "Loaded";
        case T_LOADED_NOFX: return "Loaded w/o effect";   /* its effect isn't in this build */
        case T_DELETED:    return "Deleted";
        case T_PBUSY:      return "Busy, try again";      /* a recovery, or a host restore open */
        case T_SAVE_FAILED: return "Save failed";         /* a write failed: none more until a reboot */
        case T_PFULL:      return "Full: delete one";     /* P5d: Save new, every slot saved */
        default: break;
    }
    if (id >= T_TOPIC0 && id < T_TOPIC0 + 4) return menu_help_titles[id - T_TOPIC0];
    return "";   /* a slot's page: its name, in l_paint() */
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
        case T_BOOT:   return icon_of(N_BOOT);
        case T_IND:
        case T_LOCKS:
        case T_CHGL:   return icon_of(N_IND);
        case T_CONN:
        case T_PAIRING: return icon_of(N_CONN);
        case T_BATT:   return icon_of(N_BATT);
        default:       return id >= T_LIGHT && id <= T_LIGHTS        ? icon_of(N_LIGHT)
                            : id >= T_PROFILES && id <= T_PDELETE ? icon_of(N_PROFILES)   /* P5b */
                                                                   : "";
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
            uint32_t m = pg.snap.uptime_s / 60u, d = m / 1440u, h = (m / 60u) % 24u;
            if (d) snprintf(out, sz, "Up %lud %luh %lum", (unsigned long)d, (unsigned long)h, (unsigned long)(m % 60u));
            else   snprintf(out, sz, "Up %luh %lum", (unsigned long)h, (unsigned long)(m % 60u));
            break;
        }
        case 3:
            if (pg.snap.batt_pm == ENV_UNKNOWN) snprintf(out, sz, "Battery -");
            else snprintf(out, sz, "Battery %u.%u%%", pg.snap.batt_pm / 10u, pg.snap.batt_pm % 10u);
            break;
        case 4:
            if (pg.snap.pack_mv == ENV_UNKNOWN) snprintf(out, sz, "Pack -");
            else snprintf(out, sz, "Pack %u mV", pg.snap.pack_mv);
            break;
        case 5:
            if (pg.snap.left_min == ENV_UNKNOWN) snprintf(out, sz, "Left -");
            else snprintf(out, sz, "Left %uh of use", pg.snap.left_min / 60u);
            break;
        case 6:
            snprintf(out, sz, "Mode %s", power_full[pg.snap.mode & 3u]);
            break;
        default: {
            uint8_t tr = pg.snap.link & 3u, slot = (pg.snap.link >> 2) & 3u, st = (pg.snap.link >> 4) & 7u;
            const char *s = st < 5 ? link_state[st] : "?";
            if (tr == ENV_LINK_USB) snprintf(out, sz, "Link USB");
            else if (tr == ENV_LINK_24G) snprintf(out, sz, "Link 2.4G %s", s);
            else snprintf(out, sz, "Link BT %u %s", (unsigned)(slot + 1u), s);
            break;
        }
    }
}

/* --- P5b: the profiles' snapshot ------------------------------------------ */

static uint16_t pfs_get(void) {
    if (pfs_flags & PFS_STALE) {
        uint8_t rec[PROF_REC], a = prof_active_rec(rec), v = prof_valid_mask();
        bool    star = a < PROF_SLOTS && prof_differs(rec);
        pfs        = (uint16_t)(v | ((uint16_t)(a < PROF_SLOTS ? a : 15u) << 8) | (star ? 0x1000u : 0u));
        pfs_flags &= (uint8_t)~PFS_STALE;
    }
    return pfs;
}
_Static_assert(PROF_SLOTS == 8u, "pfs: a byte of valid slots");
static bool    pfs_valid(uint8_t s) { return (pfs_get() >> s) & 1u; }
static uint8_t pfs_active(void) {   /* 0-7, or 0xFF none */
    uint8_t a = (uint8_t)((pfs_get() >> 8) & 15u);
    return a < PROF_SLOTS ? a : 0xFFu;
}
static bool pfs_star(void) { return (pfs_get() & 0x1000u) != 0u; }
/* P5d's list: how many are saved; the kth saved slot (0xFF none); a slot's
 * place among the saved ones before it, so a saved slot's row is 1 + that. */
static uint8_t pfs_saved(void) {
    uint8_t n = 0;
    for (uint8_t v = (uint8_t)pfs_get(); v; v &= (uint8_t)(v - 1u)) n++;
    return n;
}
static uint8_t pfs_nth(uint8_t k) {
    uint8_t v = (uint8_t)pfs_get();
    for (uint8_t s = 0; s < PROF_SLOTS; s++)
        if (((v >> s) & 1u) && !k--) return s;
    return 0xFFu;
}
static uint8_t pfs_rank(uint8_t slot) {
    uint8_t v = (uint8_t)(pfs_get() & ((1u << slot) - 1u)), n = 0;
    for (; v; v &= (uint8_t)(v - 1u)) n++;
    return n;
}
/* P5d: a Profiles frame's cursor on a row and its page the cursor's. The
 * list shrinks when the host deletes (the dev hooks), under the list or
 * under a slot's page or confirm above it (codex, P5d's review: past the
 * end, nothing was selected and a click did nothing; with top left at 8, a
 * blank page). Not Profiles: nothing. */
static void prof_list_fit(frame_t *f) {
    if (f->node != N_PROFILES) return;
    uint8_t n = row_count(N_PROFILES);
    if (f->cursor >= n) f->cursor = (uint8_t)(n - 1u);
    f->top = (uint8_t)(f->cursor / MENU_ROWS * MENU_ROWS);
}
/* P5e: the preview (JD, 10-26: "preload the profile as the user scrolls
 * over it"): a saved slot's lighting and LCD level, unsaved, in an
 * ME_PROFILE edit (menu_edit.c snapshots and restores the live settings,
 * and holds the RGB flush); PV_LIVE closes it, the live settings back. The
 * `*` reads the live settings, so the snapshot is read afresh. */
static void pv_show(uint8_t slot) {
    if (slot == pv) return;
    if (slot >= PROF_SLOTS) {
        me_cancel();
    } else {
        if (me_field() != ME_PROFILE) me_open(ME_PROFILE);
        prof_preview(slot);
    }
    pv = slot;
    pfs_flags |= PFS_STALE;
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
enum { RK_BLANK = 0, RK_ITEM, RK_TEXT, RK_ABOUT, RK_VALUE, RK_NOTE,   /* RK_NOTE: P4's note under Battery */
       RK_PNOTE };                                                    /* P5b: a slot's summary, node the slot's */

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
        case N_LOCKS:
        case N_CHGL:   return 7;   /* P2a: the two told apart by the variant bit (row_sig) */
        default:       return -1;
    }
}
static uint8_t live_value(uint8_t node) {
    if (node == N_SCREEN) return env_lcd_level();
    if (node == N_LOCKS) return env_ind_level(ENV_IND_LOCKS);
    if (node == N_CHGL) return env_ind_level(ENV_IND_CHARGING);
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
        case N_LOCKS:   /* P2a: the level is the percentage */
        case N_CHGL:
            if (v) snprintf(b, n, "%u%%", (unsigned)v);
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
    if (f->node == N_BATT && r >= 4u && r <= 6u) {   /* P4: the hours' note, under the choice; not selectable */
        x.kind = RK_NOTE;
        x.line = (uint8_t)(r - 4u);
        return x;
    }
    if (is_pslot(f->node) && r >= PSUM_ROW0 && r < PSUM_ROW0 + PSUM_LINES) {   /* P5b: the summary; not selectable */
        x.kind = RK_PNOTE;
        x.node = f->node;
        x.val  = pslot;
        x.line = (uint8_t)(r - PSUM_ROW0);
        return x;
    }
    if (i >= row_count(f->node)) return x;
    x.kind = RK_ITEM;
    x.node = row_node(f->node, i, &val);
    x.lit  = i == f->cursor;
    if (x.node == N_POWER) x.val = model_power & 3u;
    else if (x.node == N_CLOCK) x.val = model_clock < CLOCK_ROWS ? model_clock : 0u;
    else if (x.node == N_BATT) x.val = env_batt_hours() ? 1u : 0u;   /* P4: only the menu changes it, so no latch */
    else if (x.node == N_CONN) x.val = link_code();   /* P3: the link, link_names[] */
    else if (x.node == N_PAIRNOW) x.val = (uint8_t)(link_slot() > 2u ? 2u : link_slot());   /* the slot it pairs */
    else if (x.node >= N_BT0 && x.node <= N_BT2) {   /* the current slot: its check, and its link's word */
        x.bullet = on_bt() && x.node - N_BT0 == link_slot();
        x.val    = x.bullet ? (uint8_t)(link_state() < 5u ? link_state() : 0u) : 0u;
    }
    else if (x.node == N_PROFILES) {   /* P5b: the active slot (15 none) in val, its `*` in line (row_sig's bit 2) */
        uint8_t a = pfs_active();
        x.val  = a < PROF_SLOTS ? a : 15u;
        x.line = a < PROF_SLOTS && pfs_star() ? 1u : 0u;
    } else if (x.node == N_PSLOT) {   /* P5d: a saved slot, its slot in val (row_node()); the active one's check */
        x.val    = val;
        x.bullet = pfs_active() == val;
    } else if (x.node == N_PNEW) x.val = pfs_saved() >= PROF_SLOTS ? 1u : 0u;   /* P5d: 1 every slot saved, greyed */
    else if (x.node == N_EFFOPT || x.node == N_LTSOPT) x.val = val;
    else if (value_which(x.node) >= 0) {
        x.kind = RK_VALUE;
        x.val  = live_value(x.node);
    }
    if (is_choice(f->node) && i && i == saved_row(f->node)) x.bullet = true;
    return x;
}

/* --- P5b's text: a slot's name, the top level's value, a slot's summary ------ */

/* A slot's name, or "Profile n" for an empty slot (a deleted one's bytes
 * stay) or a nameless one. out: PROF_NAME + 1. */
static const char *pslot_label(uint8_t s, char *out) {
    uint8_t rec[PROF_REC];
    if (!prof_slot(s, rec)) rec[PR_FIELDS] = 0;
    prof_name(s, rec, out);
    return out;
}
/* The top level's value: the active slot's name in 9 cells, or 8 and its
 * `*`; a nameless slot's "Profile n" starred is "Prof. n*", its digit kept.
 * None with no active slot. buf: 24. */
static const char *prof_value(const row_t *x, char *buf) {
    if (x->val >= PROF_SLOTS) return "None";
    uint8_t rec[PROF_REC];
    (void)prof_slot(x->val, rec);
    if (x->line && !((rec[PR_FIELDS] & PF_NAME) && rec[PR_NAME])) {
        snprintf(buf, 24, "Prof. %u*", (unsigned)(x->val + 1u));
        return buf;
    }
    prof_name(x->val, rec, buf);
    size_t room = x->line ? 8u : 9u;
    if (strlen(buf) > room) buf[room] = '\0';
    if (x->line) strcat(buf, "*");
    return buf;
}
/* A slot's summary (Decided 6 and 10; the content is to be worked out on the
 * panel): the effect and its speed (an Alphas/Mods' second colour as its
 * offset); the hue, saturation and brightness as the editors show them; the
 * screen and the clock. out: 20 cells of spaces. */
static void prof_summary(uint8_t slot, uint8_t line, char *out) {
    uint8_t rec[PROF_REC];
    char    b[24];
    size_t  n;
    if (!prof_slot(slot, rec)) return;
    if (line == 0u) {
        memcpy(out, icon_of(N_LIGHT), 2);
        if (!(rec[PR_FIELDS] & PF_LIGHTING)) return;
        if (!rec[PR_RGB_ON]) {
            memcpy(out + 2, "Lights off", 10);
            return;
        }
        int8_t fx = -1;
        for (uint8_t k = 0; k < effects(); k++)
            if (env_effect_id(k) == rec[PR_EFFECT]) fx = (int8_t)k;
        if (fx >= 0 && env_effect_two_tone(env_effect_mode((uint8_t)fx)))
            snprintf(b, sizeof b, "+%u", (unsigned)((rec[PR_SPEED] * 360u) / 256u));
        else
            snprintf(b, sizeof b, "%u%%", (unsigned)((rec[PR_SPEED] * 100u + 127u) / 255u));
        size_t vn = strlen(b);
        memcpy(out + 20 - vn, b, vn);
        const char *name = fx >= 0 ? env_effect_name((uint8_t)fx) : "Unknown effect";
        n = strlen(name);
        if (n > 20u - 2u - vn - 1u) n = 20u - 2u - vn - 1u;
        memcpy(out + 2, name, n);
    } else if (line == 1u) {
        if (!(rec[PR_FIELDS] & PF_LIGHTING)) return;
        snprintf(b, sizeof b, "H:%u S:%u%% B:%u%%", (unsigned)((rec[PR_HUE] * 360u) / 256u),
                 (unsigned)((rec[PR_SAT] * 100u + 127u) / 255u), (unsigned)((rec[PR_VAL] * 100u + 127u) / 255u));
        n = strlen(b);
        memcpy(out + (n <= 18u ? 2u : 20u - n), b, n);   /* under the effect's name; all three at 100% need 19 */
    } else {
        memcpy(out, icon_of(N_SCREEN), 2);
        if (rec[PR_FIELDS] & PF_LCD) fmt_value(N_SCREEN, rec[PR_LCD], b, sizeof b);
        else snprintf(b, sizeof b, "--");
        memcpy(out + 2, b, strlen(b));
        memcpy(out + 8, icon_of(N_CLOCK), 2);
        const char *c = rec[PR_FIELDS] & PF_CLOCK ? clock_short_by_enum[rec[PR_CLOCK] % CLOCK_ROWS] : "--";
        memcpy(out + 10, c, strlen(c));
    }
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
    if (x->kind == RK_NOTE) {
        batt_note(x->line, buf, sizeof buf);
        size_t n = strlen(buf);
        memcpy(out, buf, n > 20 ? 20 : n);
        return;
    }
    if (x->kind == RK_PNOTE) {
        prof_summary(x->val, x->line, out);
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
    char        nm[PROF_NAME + 1];
    const char *label = x->node == N_EFFOPT   ? env_effect_name(x->val)
                      : x->node == N_LTSOPT   ? (x->val ? "On" : "Off")
                      : x->node == N_PAIRNOW  ? pair_labels[x->val % 3u]
                      : x->node == N_PSLOT    ? pslot_label(x->val, nm)   /* P5b */
                                              : label_of(x->node);
    const char *value = "";
    if (x->node == N_POWER) value = power_short[x->val & 3u];
    else if (x->node == N_CLOCK) value = clock_short_by_enum[x->val % CLOCK_ROWS];
    else if (clk_row(x->node) >= 0 && clk_row(x->node) < 5) value = clock_example((uint8_t)clk_row(x->node), buf);
    else if (x->node == N_BATT) value = batt_short[x->val & 1u];
    else if (x->node == N_CONN) value = link_names[x->val % 5u];          /* P3 */
    else if (x->node >= N_BT0 && x->node <= N_BT2) value = link_words[x->val % 5u];
    else if (x->node == N_BV0 || x->node == N_BV1) value = batt_preview(x->node == N_BV1, buf);   /* P4 */
    else if (x->node == N_PROFILES) value = prof_value(x, buf);           /* P5b */
    else if (x->node == N_PSLOT) value = MT_TRI_R;                        /* a saved slot opens its page */
    else if (x->node == N_PNEW) value = x->val ? "Full" : "";            /* P5d */
    else if (x->node == N_DIAG || x->node == N_HELP || x->node == N_ABOUT || x->node == N_HEALTH ||
             x->node == N_CAMERA || x->node == N_RESET || is_topic(x->node) || x->node == N_LIGHT || x->node == N_BOOT ||
             x->node == N_IND)
        value = MT_TRI_R;                            /* JD, 10-24: ▶ for > */
    size_t      at   = 0;
    const char *icon = icon_of(x->node);
    if (icon) {
        memcpy(out, icon, 2);
        at = 2;
    } else if (is_option(x->node)) {                       /* an option: the saved one's check in cells 0-1 */
        if (x->bullet)   /* JD, 10-24: the square; the circle "doesn't look good". P3: a slot's, by its link */
            memcpy(out, x->node >= N_BT0 && x->node <= N_BT2 ? link_icons[x->val % 5u] : MT_CHECK_BOX, 2);
        at = 2;                                            /* in line with "◀ Back" */
    }
    size_t ln = strlen(label), vn = strlen(value);
    if (at + ln > 20) ln = 20 - at;
    memcpy(out + at, label, ln);
    if (vn) memcpy(out + 20 - vn, value, vn);
}

/* What a Battery row's text is made of, as batt_preview() and batt_note()
 * decide it, without composing it: equal keys, equal text (the host test
 * checks every change repaints). */
static uint32_t batt_key(const row_t *x) {
    env_batt_t b;
    env_batt_facts(&b);
    bool ready = batt_hours_ready(&b);
    if (x->kind == RK_NOTE) {
        if (ready || x->line > 2u || (b.on_battery && (b.pct == 0u || b.pct > 100u))) return 0;
        if (b.avg_minutes < 60u) return x->line < 2u ? 1u : 2u | (uint32_t)(60u - b.avg_minutes) << 2;
        return !b.on_battery && x->line < 2u ? 3u : 0u;
    }
    if (b.pct > 100u) return 1;                    /* "--" */
    if (b.pct == 0u && b.on_battery) return 2;     /* "Low" */
    if (x->node == N_BV1) {                        /* "--", "<1h", "160h" (999 at most) */
        if (!ready) return 3;
        uint32_t h = b.left_min < 60u ? 0u : b.left_min / 60u;
        return 4u | (h > 999u ? 999u : h) << 3;
    }
    /* "85%", or under 10 h "18% 6h", "18% <1h" (batt_level_text()) */
    uint32_t h = ready && b.left_min < 600u ? (b.left_min < 60u ? 10u : b.left_min / 60u) : 15u;
    return 0x10000u | (uint32_t)b.pct << 4 | h;
}
/* b is a's time dt seconds on: a's date, or the next day's if that passed
 * midnight. */
static bool clock_follows(const env_time_t *a, const env_time_t *b, uint16_t dt) {
    static const uint8_t mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    uint32_t sa = (uint32_t)a->hours * 3600u + a->minutes * 60u + a->seconds;
    uint32_t sb = (uint32_t)b->hours * 3600u + b->minutes * 60u + b->seconds;
    if (sb != (sa + dt) % 86400u) return false;
    if (sa + dt < 86400u) return a->day == b->day && a->month == b->month && a->year == b->year;
    bool    leap = (a->year % 4u == 0u && a->year % 100u != 0u) || a->year % 400u == 0u;
    uint8_t last = (uint8_t)(mdays[(a->month + 11u) % 12u] + (a->month == 2u && leap ? 1u : 0u));
    if (a->day < last) return b->day == a->day + 1u && b->month == a->month && b->year == a->year;
    if (a->month < 12u) return b->day == 1u && b->month == a->month + 1u && b->year == a->year;
    return b->day == 1u && b->month == 1u && b->year == a->year + 1u;
}
/* The Clock list's time: one RTC read an RTC second. rtc_get_time() decodes
 * through newlib's localtime_r, costly on this core, and each scan called it
 * for every example row: passes of 23 ms on the board (22ec0d3d). A host's
 * set between seconds shows at the next. A read that doesn't follow the last
 * by the seconds counted (a set) is a jump: its 7-bit ticks could equal what
 * the rows show (codex: 02:07:59, a set, then 04:15:00 kept 02:07), so
 * menu_tick() repaints the examples whole (CK_JUMP holds until it does). */
static bool clock_now(env_time_t *t) {
    uint16_t k = env_rtc_tick();
    if (!pg.clk.have || k != pg.clk.tick) {
        env_time_t was = pg.clk.t;
        bool       had = pg.clk.have && (pg.clk.st & CK_OK);
        uint8_t    st  = (uint8_t)((env_clock_now(&pg.clk.t) ? CK_OK : 0u) | (pg.clk.have ? pg.clk.st & CK_JUMP : 0u));
        if (had && (st & CK_OK) && !clock_follows(&was, &pg.clk.t, (uint16_t)(k - pg.clk.tick))) st |= CK_JUMP;
        pg.clk.st   = st;
        pg.clk.tick = k;
        pg.clk.have = true;
    }
    *t = pg.clk.t;
    return pg.clk.st & CK_OK;
}
/* The Clock's examples (10-25): the format each row's mode shows, at the
 * RTC's time now. */
static const char *clock_example(uint8_t row, char *buf) {   /* buf: 24 */
    static const char *const months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    env_time_t t;
    uint8_t    mode   = clock_enum[row];
    bool       twelve = mode == 1u || mode == 5u, secs = mode == 0u || mode == 1u;
    if (!clock_now(&t)) return row == 4u ? "--- --, ----" : secs ? "--:--:--" : "--:--";
    unsigned hh = t.hours;
    if (twelve) {
        hh %= 12u;
        if (hh == 0u) hh = 12u;
    }
    const char *ap = t.hours >= 12u ? "PM" : "AM";
    if (row == 4u) snprintf(buf, 24, "%s %u, %u", months[(t.month + 11u) % 12u], (unsigned)t.day, (unsigned)t.year);
    else if (secs && twelve) snprintf(buf, 24, "%u:%02u:%02u %s", hh, (unsigned)t.minutes, (unsigned)t.seconds, ap);
    else if (secs) snprintf(buf, 24, "%02u:%02u:%02u", hh, (unsigned)t.minutes, (unsigned)t.seconds);
    else if (twelve) snprintf(buf, 24, "%u:%02u %s", hh, (unsigned)t.minutes, ap);
    else snprintf(buf, 24, "%02u:%02u", hh, (unsigned)t.minutes);
    return buf;
}
/* An example row's signature: 0x2000 | row << 10 | wide << 9 | tick << 2 |
 * bullet << 1 | lit. The tick (7 bits) is the second of the day for the rows
 * with seconds, the minute for the others, the day for Date; consecutive
 * ticks differ, so each repaints. `wide`: a 12h:ss time of 11 characters
 * (10-12 o'clock) reaches cell 9, the left half. */
static uint16_t clock_sig(const row_t *x, uint8_t row) {
    env_time_t t;
    uint8_t    mode = clock_enum[row];
    uint16_t   bits = (uint16_t)((x->bullet ? 2u : 0u) | (x->lit ? 1u : 0u));
    /* Unset: the row field 7, which no valid example's has, so the clock's
     * first valid time always repaints (codex: a fixed tick aliased one). */
    if (!clock_now(&t)) return (uint16_t)(0x2000u | (7u << 10) | ((uint16_t)row << 2) | bits);
    uint32_t tick;
    if (row == 4u) tick = (uint32_t)t.month * 32u + t.day;
    else if (mode == 0u || mode == 1u) tick = (uint32_t)t.hours * 3600u + t.minutes * 60u + t.seconds;
    else tick = (uint32_t)t.hours * 60u + t.minutes;
    bool wide = mode == 1u && (t.hours % 12u == 0u || t.hours % 12u >= 10u);
    return (uint16_t)(0x2000u | ((uint16_t)row << 10) | (wide ? 0x200u : 0u) | ((tick & 0x7Fu) << 2) | bits);
}

static uint16_t row_sig(const row_t *x) {
    /* P4: live text (the Battery choice's previews and note): a hash of
     * what the row shows, keyed by its inputs (batt_key(), no formatting:
     * the text cost 19 ms passes on the board), in 0x1000-0x1FFF (no other
     * kind's), with the bullet and the lit bit as RK_ITEM's. A collision only
     * delays a repaint to the text's next change. */
    if (x->kind == RK_NOTE || (x->kind == RK_ITEM && (x->node == N_BV0 || x->node == N_BV1))) {
        uint32_t k = batt_key(x) ^ ((uint32_t)x->node << 24) ^ ((uint32_t)x->line << 30);
        uint16_t h = 0x811Cu;
        for (uint8_t i = 0; i < 4u; i++, k >>= 8) h = (uint16_t)((h ^ (uint8_t)k) * 0x0193u);
        return (uint16_t)(0x1000u | (h & 0x0FFCu) | (x->bullet ? 2u : 0u) | (x->lit ? 1u : 0u));
    }
    if (x->kind == RK_ITEM && clk_row(x->node) >= 0 && clk_row(x->node) < 5) return clock_sig(x, (uint8_t)clk_row(x->node));
    switch (x->kind) {
        case RK_ITEM:   /* bit 2: P5b's `*` (line, 0 on every other item) */
            return (uint16_t)((1u << 14) | ((uint16_t)x->node << 8) | ((uint16_t)(x->val & 15u) << 4) |
                              ((uint16_t)(x->line & 1u) << 2) | (x->bullet ? 2u : 0u) | (x->lit ? 1u : 0u));
        case RK_PNOTE: {   /* P5b: 0xC800 | line << 9 | the record's crc << 1 (equal crc, equal text); 0xCE00 | line << 1
                            * empty. Bit 0 stays clear: a row's bit 0 is its lit bit, which the scheduler reads. The
                            * crc byte read raw: the snapshot vouched for the record this pass */
            uint8_t s = x->val, crc;
            if (!pfs_valid(s)) return (uint16_t)(0xCE00u | ((uint16_t)x->line << 1));
            env_prof_read((uint16_t)(PROF_BASE + PROF_REC_OFF(s) + PR_CRC), &crc, 1);
            return (uint16_t)(0xC800u | ((uint16_t)(x->line & 3u) << 9) | ((uint16_t)crc << 1));
        }
        case RK_TEXT:
            return (uint16_t)((2u << 14) | ((uint16_t)(x->node - N_TOPIC0) << 11) | ((uint16_t)x->line << 5));
        case RK_ABOUT:
            return (uint16_t)((3u << 14) | ((uint16_t)x->line << 8));
        case RK_VALUE: {   /* P1b: 0xE000 | which << 10 | variant << 9 | value << 1 | lit (bit 0 is lit) */
            /* The variant: Speed's Second color, or P2a's Charging light (which 7, with Lock lights). */
            uint16_t variant = ((x->node == N_SPEED && two_tone()) || x->node == N_CHGL) ? 1u : 0u;
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
    /* A list pages by 8, so its last page's top can pass count - 8 (nine rows:
     * 8); the thumb sits at the end then (codex: it was drawn off the panel). */
    if (*count > MENU_ROWS && *top > *count - MENU_ROWS) *top = (uint8_t)(*count - MENU_ROWS);
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
/* P3: a row whose cells 0-1 take a colour of their own (the top level's icons
 * in white; the saved option's green box; the current slot's status icon): 0
 * for none. Read from the row's content, so it follows the signature (the
 * icon is its node's, the state its val, the saved option or current slot its
 * bullet, the lit tint its lit bit). */
static uint16_t row_icon_colour(const row_t *x) {
    if (x->kind != RK_ITEM && x->kind != RK_VALUE) return 0u;
    /* JD, 10-25: white "so they pop"; on the selected row a whiter yellow than
     * its text ("feels weird when they don't change at all") */
    if (icon_of(x->node)) return x->lit ? MENU_ICON_LIT : pal[MP_WHITE];
    if (x->kind != RK_ITEM || !x->bullet || !is_option(x->node)) return 0u;
    return x->node >= N_BT0 && x->node <= N_BT2 ? link_colour[x->val % 5u] : MENU_SAVED;
}
static bool row_split(uint8_t s) {
    row_t x = row_at((uint8_t)(s - S_ROW0));
    return row_icon_colour(&x) != 0;
}
/* A row: its lead, then cells 0-9 and cells 10-19 with the trail; a split row
 * (P3) has cells 0-1 as a chunk of their own between the lead and cells 2-9. */
static uint8_t l_chunks(uint8_t s) { return (s == S_SEP || s == S_BAR) ? 1 : is_row(s) ? (row_split(s) ? 4 : 3) : 2; }
static uint8_t rows_per_piece(void) { return (uint8_t)(MB_PASS_BYTES / (CHUNK_MAX * 2u)); }   /* 4 */
/* A split row's cells 2-9 are narrower, so their pieces take 5 pixel rows: 3
 * pieces, not 4, which pays for the icon's own piece. Each piece costs about
 * a pass whatever its size: the top level's white icons (JD, 10-25) added 8
 * pieces, ~100 ms, to its repaint until this. */
#define SPLIT_W (HALF_W - 2u * MT_CELL_W)   /* 48 */
static uint8_t rows_per_split(void) { return (uint8_t)(MB_PASS_BYTES / (SPLIT_W * 2u)); }   /* 5 */
static uint8_t l_pieces(uint8_t s, uint8_t c) {
    if (s == S_SEP || s == S_BAR || (is_row(s) && c == 0)) return 1;   /* a row's lead: every pixel row at once */
    const bool split = is_row(s) && row_split(s);
    if (split && c == 1) return 1;                                     /* the icon's two cells: every row at once too */
    uint8_t r = split && c == 2 ? rows_per_split() : rows_per_piece();
    return (uint8_t)((MT_CELL_H + r - 1u) / r);
}
_Static_assert(MB_PASS_BYTES / (CHUNK_MAX * 2u) >= 1 && (MB_PASS_BYTES / (CHUNK_MAX * 2u)) * ((CHUNK_MAX + 7u) / 8u) <= BITS_MAX,
               "a half-row piece: whole pixel rows, its bits in the buffer");
_Static_assert(MB_PASS_BYTES / (SPLIT_W * 2u) <= MT_CELL_H && (MB_PASS_BYTES / (SPLIT_W * 2u)) * ((SPLIT_W + 7u) / 8u) <= BITS_MAX,
               "a split row's cells 2-9: whole pixel rows, its bits in the buffer");
_Static_assert(ROW_LEAD * MT_CELL_H * 2u <= MB_PASS_BYTES && ROW_LEAD <= 8u && MT_CELL_H <= BITS_MAX, "a row's lead: one piece");
_Static_assert(2u * MT_CELL_W * MT_CELL_H * 2u <= MB_PASS_BYTES && ((2u * MT_CELL_W + 7u) / 8u) * MT_CELL_H <= BITS_MAX,
               "a row's icon (two cells): one piece");

/* One piece of a 20-cell text slot at y: chunk c, piece p's pixel rows. A
 * row (`row`) has the 4 px lead, with `cursor` (a code, or 0) in it in
 * `cursor_fg`; the title and the confirm's lines a 1 px one. */
static ms_paint_t text_piece(const char *cells, uint16_t y, bool row, uint8_t cursor, uint16_t cursor_fg, uint8_t c,
                             uint8_t p, uint16_t fg, uint16_t icon_fg) {
    const uint8_t  lead  = row ? ROW_LEAD : 1u;
    mt_line_t      l     = {cells, 20, lead, 1, false, false, cursor};
    const bool     lc    = row && c == 0;              /* the row's lead */
    const bool     split = row && icon_fg != 0;        /* P3: cells 0-1 a chunk of their own */
    const bool     ic    = split && c == 1;            /* that chunk */
    const uint8_t  k     = split && c > 1 ? (uint8_t)(c - 1u) : c;   /* the halves' chunk, as unsplit */
    uint16_t       c0, w;                              /* the chunk's line columns, [c0, c0 + w) */
    if (!row) {                            /* [0, 61), [61, 122) */
        c0 = c ? (uint16_t)(lead + HALF_W) : 0u;
        w  = c ? (uint16_t)(HALF_W + 1u) : (uint16_t)(lead + HALF_W);
    } else if (lc) {                       /* [0, 4) */
        c0 = 0;
        w  = lead;
    } else if (ic) {                       /* [4, 16) */
        c0 = lead;
        w  = (uint16_t)(2u * MT_CELL_W);
    } else {                               /* [4, 64), [64, 125); split: [16, 64) */
        c0 = k == 1 ? (uint16_t)(lead + (split ? 2u * MT_CELL_W : 0u)) : (uint16_t)(lead + HALF_W);
        w  = k == 1 ? (uint16_t)(split ? SPLIT_W : HALF_W) : (uint16_t)(HALF_W + 1u);
    }
    const uint8_t  rpp  = split && c == 2 ? rows_per_split() : rows_per_piece();   /* as l_pieces() */
    const bool     all  = lc || ic;        /* one piece, every pixel row */
    const uint8_t  r0   = all ? 0 : (uint8_t)(p * rpp);
    const uint8_t  rows = all ? MT_CELL_H : (uint8_t)(r0 + rpp > MT_CELL_H ? MT_CELL_H - r0 : rpp);
    const uint8_t  st   = (uint8_t)((w + 7u) / 8u);
    uint8_t        bits[BITS_MAX];
    mt_bits(&l, c0, w, r0, rows, bits, st);
    return mb_mono(bits, st, (uint16_t)(TEXT_X - lead + c0), (uint16_t)(y + r0), w, rows,
                   lc ? cursor_fg : ic ? icon_fg : fg, 0)
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
        char        nm[PROF_NAME + 1];
        const char *ic = title_icon(title_id), *tt = title_text(title_id);
        if (title_id >= T_PSLOT0 && title_id < T_PSLOT0 + PROF_SLOTS) {   /* P5b: a slot's page, its name */
            uint8_t rec[PROF_REC], s = (uint8_t)(title_id - T_PSLOT0);
            if (!prof_slot(s, rec)) rec[PR_FIELDS] = 0;
            prof_name(s, rec, nm);
            ic = icon_of(N_PROFILES);
            tt = nm;
        }
        size_t      ni = strlen(ic), n = strlen(tt);
        memcpy(cells, ic, ni);
        memcpy(cells + ni, tt, n > 20 - ni ? 20 - ni : n);
        return text_piece(cells, 0, false, 0, 0, c, p, pal[MP_WHITE], 0);
    }
    row_t x = row_at((uint8_t)(s - S_ROW0));
    row_text(&x, cells);
    uint16_t fg = x.kind != RK_ITEM && x.kind != RK_VALUE ? pal[MP_WHITE]   /* a page's text, About's lines */
                : x.lit              ? pal[MP_LIT]
                : (x.node == N_BACK || x.node == N_EXIT) ? pal[MP_LEAVE]
                : x.node == N_PNEW && x.val ? pal[MP_GREY]   /* P5d: Save new, every slot saved */
                                     : pal[MP_DIM];
    /* The cursor (JD, 10-24): in the lit row's lead, in its own colour. */
    return text_piece(cells, (uint16_t)(ROW_Y0 + (s - S_ROW0) * ROW_H), true, x.lit ? (uint8_t)MT_CURSOR[0] : 0u,
                      pal[MP_CURSOR], c, p, fg, row_icon_colour(&x));
}

/* A Clock example's tick (10-25): the right half only, its cells 10-19 and
 * the trail, when nothing but the tick changed (clock_sig()); not Date,
 * whose text crosses the halves. A split row's right half is chunk 3. */
static uint8_t l_first(uint8_t s, uint16_t was, uint16_t now) {
    if (!is_row(s) || (was & 0xE000u) != 0x2000u || (now & 0xE000u) != 0x2000u) return 0;
    if (((was ^ now) & (uint16_t)~0x01FCu) != 0u || ((now >> 10) & 7u) == 4u) return 0;
    return row_split(s) ? 3u : 2u;
}
static const ms_layout_t L_LIST = {11, l_sig, l_chunks, l_pieces, l_paint, S_ROW0, MENU_ROWS, l_first};

/* --- The confirm geometry: Reset counters' two pages (the mockup: text at y
 * 22, 36, 72 and 86 under the title and the separator). */
static const uint8_t c_y[4] = {22, 36, 72, 86};
static const char *const c_lines[2][4] = {
    {"Clear the health", "counters?", "Click: reset", "Hold: cancel"},
    {"Counters reset.", "", "Click: back", ""},
};
/* The bootloader item's (JD, 10-24; the words wait for his text pass): the
 * question, and the refusal on the battery's reserve. */
static const char *const boot_lines[2][4] = {
    {"For a firmware", "update over USB.", "Click: enter", "Hold: cancel"},
    {"Plug in first:", "the battery is low.", "Click: back", ""},
};
/* P3, Connection off BT (JD, 10-25: "a screen saying 'Switch to BT mode to
 * change Bluetooth settings' or such"): fixed, its lines at y 22, 36, 50 and
 * 86 so the sentence isn't split by the confirm pages' gap. */
static const uint8_t     info_y[4]     = {22, 36, 50, 86};
static const char *const info_lines[4] = {"Switch to BT mode to", "change Bluetooth", "settings.", "Click: back"};
/* The Pairing page's lines by phase (PP_*); a NULL is the slot's line,
 * written below. The words wait for JD's text pass (MENU-TEXT.md). */
static const char *const pair_lines[6][3] = {
    {NULL, "Getting ready...", "Click or hold: stop"},
    {"On the device, add", NULL, "Click or hold: stop"},   /* JD, 10-25: a phone or an iPad too; the band's exact advertised name */
    {NULL, "connected.", "Click: back"},
    {"Pairing ended.", "", "Click: back"},
    {"Pairing didn't", "start. Try again.", "Click: back"},
    {"The radio didn't", "confirm pairing.", "Click or hold: stop"},
};
static void pair_line(uint8_t slot, uint8_t phase, uint8_t i, char *out, size_t sz) {
    const char *s = i < 3 ? pair_lines[phase][i] : "";
    if (s) snprintf(out, sz, "%s", s);
    else if (phase == PP_OPEN) snprintf(out, sz, "AK820 5.1-%u", (unsigned)(slot + 1u));
    else snprintf(out, sz, "Bluetooth %u%s", (unsigned)(slot + 1u), phase == PP_DONE ? " is" : "");
}
/* P5b's confirms, the slot's name in the question (the words wait for JD's
 * text pass). "Replace " or "Delete " and "?" leave 12 for the name. */
static void pconfirm_line(uint8_t node, uint8_t slot, uint8_t i, char *out, size_t sz) {
    static const char *const tail[2][3] = {{"with these settings?", "Click: replace", "Hold: cancel"},
                                           {"", "Click: delete", "Hold: cancel"}};
    const bool del = node == N_PDELETE;
    if (i) {
        snprintf(out, sz, "%s", tail[del][(i - 1u) % 3u]);
        return;
    }
    char    nm[PROF_NAME + 1];
    uint8_t rec[PROF_REC];
    if (!prof_slot(slot, rec)) rec[PR_FIELDS] = 0;
    prof_name(slot, rec, nm);
    snprintf(out, sz, del ? "Delete %s?" : "Replace %s", nm);
}
static uint16_t c_sig(uint8_t s) {
    if (s == 0) return title_id;
    if (s == 1) return 1;
    uint8_t n = cur()->node;
    if (n == N_CONNINFO) return (uint16_t)(0x8000u | (uint16_t)(s - 2u));
    if (n == N_PSAVE || n == N_PDELETE)   /* P5b: the slot's (its name repaints at the layout change) */
        return (uint16_t)((n == N_PSAVE ? 0x300u : 0x400u) | ((uint16_t)cur()->cursor << 4) | (uint16_t)(s - 2u));
    if (n == N_PAIRING) {
        uint8_t slot = cur()->cursor;
        return (uint16_t)(0xA000u | ((uint16_t)slot << 8) | ((uint16_t)pair_phase(slot) << 4) | (uint16_t)(s - 2u));
    }
    return (uint16_t)((n == N_BOOT ? 0x200u : 0x100u) | ((uint16_t)confirm_page << 4) | (uint16_t)(s - 2u));
}
static uint8_t c_chunks(uint8_t s) { return s == 1 ? 1 : 2; }
static uint8_t c_pieces(uint8_t s, uint8_t c) { return l_pieces(s == 1 ? S_SEP : S_TITLE, c); }
static ms_paint_t c_paint(uint8_t s, uint8_t c, uint8_t p) {
    if (s <= 1) return l_paint(s, c, p);
    char    cells[21], buf[24];
    uint8_t node = cur()->node, i = (uint8_t)(s - 2u);
    memset(cells, ' ', 20);
    cells[20] = '\0';
    if (node == N_CONNINFO) snprintf(buf, sizeof buf, "%s", info_lines[i]);
    else if (node == N_PAIRING) pair_line(cur()->cursor, pair_phase(cur()->cursor), i, buf, sizeof buf);
    else if (node == N_PSAVE || node == N_PDELETE) pconfirm_line(node, cur()->cursor, i, buf, sizeof buf);
    else snprintf(buf, sizeof buf, "%s", (node == N_BOOT ? boot_lines : c_lines)[confirm_page & 1u][i]);
    size_t n = strlen(buf);
    memcpy(cells, buf, n > 20 ? 20 : n);
    return text_piece(cells, (node == N_CONNINFO ? info_y : c_y)[i], false, 0, 0, c, p, pal[MP_WHITE], 0);
}
static const ms_layout_t L_CONFIRM = {6, c_sig, c_chunks, c_pieces, c_paint, 0, 0, 0};

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
static uint8_t e_range_max(void) {
    uint8_t n = cur()->node;
    return n == N_SCREEN ? (uint8_t)MENU_LCD_MAX : is_ind_editor(n) ? (uint8_t)ENV_IND_MAX : 255u;
}

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
/* A hue at full saturation and value, RGB565, by QMK's own arithmetic
 * (quantum/color.c's hsv_to_rgb_impl() with s = v = 255), so the strip's
 * colours are the keys' (codex, P1b's code review). */
static uint16_t hue565(uint8_t h) {
    uint8_t region = (uint8_t)(h * 6u / 255u), rem = (uint8_t)((h * 2 - region * 85) * 3);
    uint8_t q = (uint8_t)((255u * (255u - ((255u * rem) >> 8))) >> 8);
    uint8_t t = (uint8_t)((255u * (255u - ((255u * (255u - rem)) >> 8))) >> 8);
    uint8_t r, g, b;
    switch (region) {
        case 6:
        case 0:  r = 255; g = t; b = 0; break;
        case 1:  r = q; g = 255; b = 0; break;
        case 2:  r = 0; g = 255; b = t; break;
        case 3:  r = 0; g = q; b = 255; break;
        case 4:  r = t; g = 0; b = 255; break;
        default: r = 255; g = 0; b = q; break;
    }
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/* The bands' rows, filled a row at a time in panel order (MB_PANEL): a
 * precomputed row copied, then the few pixels that differ set. */
typedef struct { uint8_t x0, edge; uint16_t grey, white; } bar_ctx_t;
static void bar_row(const void *v, uint16_t row, uint16_t *px, uint16_t w) {
    const bar_ctx_t *c = v;
    const uint16_t   y = (uint16_t)(BAR_YT + row);
    for (uint16_t i = 0; i < w; i++) {
        const uint16_t x = (uint16_t)(c->x0 + i);
        px[i] = (y == BAR_YT || y == BAR_YB || x == BAR_X0 || x == BAR_X1) ? c->grey : x < c->edge ? c->white : 0u;
    }
}
typedef struct { uint8_t x0, mx; uint16_t white, tri; uint16_t col[ENV_BW]; } env_ctx_t;
static void env_row(const void *v, uint16_t row, uint16_t *px, uint16_t w) {
    const env_ctx_t *c = v;
    const int16_t    y = (int16_t)(ENV_Y0 + row), rel = (int16_t)(c->mx - c->x0);
    if (y >= STRIP_YT && y <= STRIP_YB) memcpy(px, c->col, (size_t)w * 2u);   /* the strip; 0 outside it */
    else memset(px, 0, (size_t)w * 2u);
    if (y >= 38) {   /* the triangle: 5, 3, 1 */
        int16_t half = (int16_t)(2 - (y - 38));
        for (int16_t d = (int16_t)-half; d <= half; d++)
            if (rel + d >= 0 && rel + d < (int16_t)w) px[rel + d] = c->tri;
    } else if (rel >= 0 && rel < (int16_t)w)
        px[rel] = c->white;   /* the line, y 20-37 */
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
        bar_ctx_t c = {(uint8_t)(BAR_X0 + k * BAR_BW), bar_edge(), MB_PANEL(pal[MP_GREY]), MB_PANEL(pal[MP_WHITE])};
        uint16_t  w = k + 1 == BAR_N ? (uint16_t)(BAR_X1 - c.x0 + 1u) : BAR_BW;
        return mb_rows(c.x0, BAR_YT, w, BAR_YB - BAR_YT + 1u, bar_row, &c) ? MS_PAINT_DONE : MS_PAINT_FAILED;
    }
    env_ctx_t c = {(uint8_t)(k * ENV_BW), marker_x(), MB_PANEL(pal[MP_WHITE]), MB_PANEL(pal[MP_LIT]), {0}};
    for (uint8_t i = 0; i < ENV_BW; i++) {
        uint16_t x = (uint16_t)(c.x0 + i);
        if (x >= STRIP_X0 && x <= STRIP_X1) c.col[i] = MB_PANEL(hue565((uint8_t)(((x - STRIP_X0) * 256u + 60u) / 120u)));
    }
    return mb_rows(c.x0, ENV_Y0, ENV_BW, ENV_Y1 - ENV_Y0 + 1u, env_row, &c) ? MS_PAINT_DONE : MS_PAINT_FAILED;
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
    return text_piece(cells, (uint16_t)(HINT_Y0 + (s - vs - 1u) * ROW_H), false, 0, 0, c, p, pal[MP_WHITE], 0);
}
static const ms_layout_t L_RANGE = {2 + BAR_N + 1 + 3, e_sig, e_chunks, e_pieces, e_paint, 0, 0, 0};
static const ms_layout_t L_HUE   = {2 + ENV_N + 1 + 3, e_sig, e_chunks, e_pieces, e_paint, 0, 0, 0};
_Static_assert(2 + ENV_N + 1 + 3 <= MS_MAX_SLOTS, "the hue editor's slots");
_Static_assert(ENV_BW * (ENV_Y1 - ENV_Y0 + 1) * 2 <= MB_PASS_BYTES && BAR_BW * (BAR_YB - BAR_YT + 1) * 2 <= MB_PASS_BYTES,
               "a band is one piece");

/* --- Navigation -------------------------------------------------------------- */

static void retitle(uint32_t now);
static bool sync_model(void);
static uint32_t last_idle;   /* menu_tick's latest idle time, for retitle() between ticks */

static void push(uint8_t node, uint8_t cursor) {
    if (depth >= sizeof stack / sizeof stack[0]) return;   /* four deep at most: P5b's confirms */
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
    model_link   = env_link();
    model_pf     = env_bt_pair_facts();
    (void)me_take_pending();   /* changes from before the session aren't its targets */
    pv = PV_LIVE;              /* P5e: the panel's release rolled any preview back */
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
    model_link   = env_link();
    model_pf     = env_bt_pair_facts();
    (void)me_take_pending();
    pv = PV_LIVE;
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
    env_about(&pg.snap);
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
    if (from == N_PROFILES) pv_show(PV_LIVE);                /* P5e: out of Profiles, the live settings */
    depth--;
    if (cur()->node == N_PSLOT) pv_show(pslot);              /* P5e: back from a confirm, its profile again */
    /* P5d: back on its row, wherever a host's write moved it; in range either way */
    if (from == N_PSLOT && pfs_valid(pslot)) cur()->cursor = (uint8_t)(1u + pfs_rank(pslot));
    prof_list_fit(cur());
    if (is_editor(from)) {   /* back to its list: a layout change, both ways (ms_set_layout's clear) */
        retitle(now);
        ms_set_layout(&L_LIST, false);
    } else
        show(false, is_confirm(from), now);
    return MENU_NONE;
}

/* P5b: a profile action's outcome, as the title's flash. */
static void prof_outcome(uint8_t st, uint8_t ok, uint32_t now) {
    flash(st == PROF_OK                  ? ok
          : st == PROF_OK_EFFECT_SKIPPED ? T_LOADED_NOFX
          : st == PROF_BUSY              ? T_PBUSY
                                         : T_SAVE_FAILED,
          now);
}
/* A page that shows names, which the rows' signatures don't carry: entered,
 * those slots repaint whatever their signature, as About's do. */
static void enter_named(uint8_t first, uint8_t n, uint32_t now) {
    retitle(now);
    ms_invalidate_rows(S_TITLE, 1);
    ms_invalidate_rows(first, n);
    ms_retarget();
}

/* P1b: an editor opens on its field's edit, its own layout. */
static void open_editor(uint8_t node, uint32_t now) {
    me_open(field_of(node));
    ft_have = false;
    push(node, 0);
    retitle(now);
    ms_set_layout(is_hue_editor(node) ? &L_HUE : &L_RANGE, false);
}

/* A 0-255 value as its percentage (fmt_value's rounding), and the value
 * that shows a percentage: pct_of(val_of_pct(p)) == p for every p. */
static int16_t pct_of(int16_t v) { return (int16_t)((v * 100 + 127) / 255); }
static int16_t val_of_pct(int16_t p) { return (int16_t)((p * 255 + 50) / 100); }

/* A turn in an editor: the field's next value, previewed. Fast (the mockup's
 * rule, codex's predicate): two or more detents in one input, or one within
 * 90 ms of the editor's last turn. A range clamps; a hue wraps. A slow
 * detent is the finest step the value shows (JD, 10-25: "micro-adjustments
 * by one value/percent"): a hue's unit, a level, or the next percentage of
 * a 0-255 value; a fast one is 8% (as fast as the old 20 of 255) or 12 hue
 * units. The Screen's levels are 0-100 since P2b, as the indicators'. */
static void edit_turn(int8_t turns, uint32_t now) {
    bool fast = turns >= 2 || turns <= -2 || (ft_have && (uint32_t)(now - ft_last) < 90u);
    ft_have   = true;
    ft_last   = now;
    uint8_t node = cur()->node;
    int16_t v    = me_value();
    if (is_hue_editor(node))
        v = (int16_t)((v + turns * (fast ? 12 : 1)) & 255);
    else if (node == N_SCREEN || is_ind_editor(node)) {   /* levels 0-100: the Screen's, the indicators' */
        int16_t max = e_range_max();
        v = (int16_t)(v + turns * (fast ? 8 : 1));
        if (v < 0) v = 0;
        if (v > max) v = max;
    } else {   /* a 0-255 value shown as a percentage */
        int16_t p = (int16_t)(pct_of(v) + turns * (fast ? 8 : 1));
        if (p < 0) p = 0;
        if (p > 100) p = 100;
        v = val_of_pct(p);
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
        else if (f->node == N_BATT) env_batt_hours_set(i == 1u);   /* P4 */
        else { env_clock_set(clock_enum[i]); model_clock = clock_enum[i]; }
        flash(T_SAVED, now);
        return pop(now);
    }
    switch (node) {
        case N_POWER:
        case N_CLOCK:
        case N_BATT:
            if (node == N_CLOCK) {
                /* About's bytes: read afresh. Its examples repaint whole:
                 * rows a quick re-entry left committed could alias (codex). */
                pg.clk.have = false;
                ms_invalidate_rows(S_ROW0 + 1u, CLOCK_EXAMPLES);
            }
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
        case N_LOCKS:
        case N_CHGL:
            open_editor(node, now);
            return MENU_NONE;
        case N_DIAG:
        case N_HELP:
        case N_LIGHT:
        case N_IND:
            push(node, 1);
            show(false, false, now);
            return MENU_NONE;
        case N_CONN:   /* P3: the slots on BT, the cursor on the current one; off it, JD's page */
            if (on_bt()) {
                push(N_CONN, (uint8_t)(1u + (link_slot() > 2u ? 0u : link_slot())));
                show(false, false, now);
            } else {
                push(N_CONNINFO, 0);
                show(true, false, now);
            }
            return MENU_NONE;
        case N_BT0:
        case N_BT1:
        case N_BT2: {   /* the tap's select; nothing for the current slot while linked or linking, on a live select */
            uint8_t slot = (uint8_t)(node - N_BT0), st = link_state();
            /* A lost select re-selects whatever the state reads: a stale
             * CONNECTED can stand over one that never got through (codex). */
            if (slot == link_slot() && !(model_pf & ENV_PF_SEL_LOST) &&
                (st == ENV_LINK_CONNECTED || st == ENV_LINK_LINKING)) {
                env_bt_pair_cancel();   /* nothing sent, but a pending pair or a key's hold is superseded (codex) */
                (void)sync_model();
                flash((uint8_t)(T_ALREADY_BT1 + slot), now);
            } else {
                env_bt_select(slot);
                (void)sync_model();
                flash((uint8_t)(T_NOW_BT1 + slot), now);
            }
            retitle(now);
            ms_retarget();
            return MENU_NONE;
        }
        case N_PAIRNOW: {   /* the current slot (JD, 10-25), straight to its page; Connection's cursor on its row */
            uint8_t slot = link_slot() > 2u ? 2u : link_slot();
            env_bt_pair(slot);
            (void)sync_model();
            f->cursor = (uint8_t)(1u + slot);
            push(N_PAIRING, slot);
            retitle(now);
            ms_set_layout(&L_CONFIRM, false);
            return MENU_NONE;
        }
        case N_ABOUT:
            open_about(now);
            return MENU_NONE;
        case N_PROFILES: {   /* P5b: the cursor on the active slot; not while a host restore is open */
            if (prof_restore_open()) {
                flash(T_PBUSY, now);
                retitle(now);
                ms_retarget();
                return MENU_NONE;
            }
            uint8_t a = pfs_active();   /* P5d: on the active slot's row, else the first (Save new, none saved) */
            push(N_PROFILES, (uint8_t)(1u + (a < PROF_SLOTS ? pfs_rank(a) : 0u)));
            enter_named(S_ROW0 + 1u, MENU_ROWS - 1u, now);
            return MENU_NONE;
        }
        case N_PSLOT: {   /* a saved slot: its page, on Load */
            uint8_t rec[PROF_REC];
            if (!prof_slot(val, rec)) {   /* emptied since the snapshot (the host's): the list as it now is */
                retitle(now);
                ms_retarget();
                return MENU_NONE;
            }
            pslot = val;
            pv_show(val);   /* P5e: at once, not after a rest: a click is deliberate */
            push(N_PSLOT, 1);
            enter_named(S_ROW0 + PSUM_ROW0, PSUM_LINES, now);
            return MENU_NONE;
        }
        case N_PNEW: {   /* P5d: the live settings into the first free slot as Profile n, which becomes the active one */
            pv_show(PV_LIVE);   /* P5e: the live settings, not a preview's */
            uint8_t s = prof_free_slot();
            if (s >= PROF_SLOTS) {
                flash(T_PFULL, now);
            } else {
                uint8_t st = prof_save(s, NULL);
                prof_outcome(st, T_SAVED, now);
                pfs_flags |= PFS_STALE;
                if (st == PROF_OK) {   /* the cursor on its row; the rows below it move down one */
                    f->cursor = (uint8_t)(1u + pfs_rank(s));
                    f->top    = (uint8_t)(f->cursor / MENU_ROWS * MENU_ROWS);
                }
            }
            retitle(now);
            ms_retarget();
            return MENU_NONE;
        }
        case N_PLOAD: {   /* commits like a choice: back to Profiles, where the check moves */
            uint8_t st = prof_load(pslot);
            if (st == PROF_OK || st == PROF_OK_EFFECT_SKIPPED) {
                me_keep();   /* P5e: the preview's values are now saved, so its snapshot goes */
                pv          = pslot;
                model_clock = env_clock_mode();   /* the latched model, as a choice's commit sets it */
                prof_outcome(st, T_LOADED, now);
                return pop(now);
            }
            if (st == PROF_INVALID) return pop(now);   /* emptied meanwhile (the host's) */
            prof_outcome(st, T_LOADED, now);
            retitle(now);
            ms_retarget();
            return MENU_NONE;
        }
        case N_PSAVE:
        case N_PDELETE:   /* overwriting and deleting ask first (Decided 5) */
            if (node == N_PSAVE) pv_show(PV_LIVE);   /* P5e: Save's confirm shows what it would save */
            push(node, pslot);
            show(true, false, now);
            return MENU_NONE;
        case N_EXIT:
            return MENU_CLOSE;
        case N_HEALTH: return MENU_DIAG_HEALTH;
        case N_CAMERA: return MENU_DIAG_CAMERA;
        case N_RESET:
        case N_BOOT:
            confirm_page = 0;
            push(node, 0);
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

/* The latched model, read fresh (Fn+B, Fn+C; P3: the link and the pairing
 * facts). True: it changed. */
static bool sync_model(void) {
    uint8_t p = env_power_mode(), c = env_clock_mode(), l = env_link(), f = env_bt_pair_facts();
    if (p == model_power && c == model_clock && l == model_link && f == model_pf) return false;
    model_power = p;
    model_clock = c;
    model_link  = l;
    model_pf    = f;
    return true;
}

/* P5b: a confirm's click. Save: back to the slot's page, its summary the
 * new record's. Delete: back past the slot's page to Profiles. Refused (a
 * recovery; a write failed): back to the slot's page, saying so. */
static menu_result_t pconfirm_click(uint32_t now) {
    frame_t *f = cur();
    if (f->node == N_PSAVE) {
        prof_outcome(prof_save(f->cursor, NULL), T_SAVED, now);
        return pop(now);
    }
    uint8_t st = prof_delete(f->cursor);
    prof_outcome(st, T_DELETED, now);
    if (st != PROF_OK) return pop(now);
    pv_show(PV_LIVE);                /* P5e: its profile gone, the live settings */
    depth = (uint8_t)(depth - 2u);   /* the confirm and the slot's page */
    prof_list_fit(cur());            /* P5d: the deleted row's index, now the next row's */
    retitle(now);
    ms_set_layout(&L_LIST, false);
    return MENU_NONE;
}

/* P3: leaving the Pairing page, a click or a hold: while it's starting,
 * advertising or unsure, that stops it. */
static menu_result_t pair_leave(uint32_t now) {
    if (pair_stoppable(pair_phase(cur()->cursor))) {
        env_bt_pair_stop();
        (void)sync_model();
        flash(T_PAIR_STOPPED, now);
    }
    return pop(now);
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
    pfs_flags   |= PFS_STALE;   /* P5b: a click may write a slot; the store is read afresh */
    if (pv == PV_HELD) pv = PV_LIVE;   /* P5e: the knob moved: the rest previews again */
    /* An editor or a previewing choice whose edit an external write has
     * already rolled back takes no more input: menu_tick() pops it with
     * "Cancelled" (codex, P1b's code review: a click queued behind the write
     * showed "Saved" for an edit that no longer existed). */
    if ((is_editor(cur()->node) || is_preview(cur()->node)) && !me_active()) return MENU_NONE;
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
        if (f->node == N_BOOT) {   /* enter it, unless the battery is in its reserve (flash.sh's rule) */
            if (confirm_page == 0) {
                if (!env_reserve()) return MENU_BOOTLOADER;
                confirm_page = 1;
                ms_retarget();
                return MENU_NONE;
            }
            return pop(now);
        }
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
        if (f->node == N_PAIRING) return pair_leave(now);
        if (f->node == N_PSAVE || f->node == N_PDELETE) return pconfirm_click(now);   /* P5b */
        return pop(now);   /* a topic, About, Connection off BT: back */
    }
    if (latch == MI_LATCH_BACK) {   /* pop() cancels an editor's or a previewing choice's edit */
        if (f->node == N_PAIRING) return pair_leave(now);   /* the same as a click (the mockup's "Click or hold: stop") */
        if (is_choice(f->node) || is_editor(f->node) || ((f->node == N_RESET || f->node == N_BOOT) && confirm_page == 0) ||
            f->node == N_PSAVE || f->node == N_PDELETE)
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
    uint8_t pend  = me_take_pending();
    bool    moved = sync_model();   /* Fn+B, Fn+C, while the menu shows */
    if ((pend & ME_PEND_CANCELLED) && pv < PROF_SLOTS && me_field() != ME_PROFILE) {
        pv         = PV_HELD;   /* P5e: an external write rolled the preview back: the live settings until the knob moves */
        pfs_flags |= PFS_STALE;
    }
    if ((pend & ME_PEND_CANCELLED) && (is_editor(cur()->node) || is_preview(cur()->node))) {
        flash(T_CANCELLED, now);
        (void)pop(now);   /* its target is the new one (one target, not two: codex) */
        moved = false;
    } else if (pend)
        moved = true;
    /* P5b: the host wrote the block (a restore, the dev hooks): the names
     * repaint, which the rows' signatures don't carry. Here, outside the
     * scheduler's scans, as the Clock's jump below. */
    if (prof_take_changed()) {
        ms_invalidate_all();
        pfs_flags |= PFS_STALE;
        for (uint8_t i = 0; i < depth; i++) prof_list_fit(&stack[i]);   /* P5d: a host's delete shrinks the list */
        moved = true;
    }
    uint8_t tk = (uint8_t)((now >> 7) << 2) & PFS_TICKS;   /* the profiles' snapshot: every 128 ms */
    if ((pfs_flags & PFS_TICKS) != tk) pfs_flags = (uint8_t)((pfs_flags & ~PFS_TICKS) | tk | PFS_STALE);
    /* A profile write failed where no click shows it (the active slot's,
     * deferred to kb_eeconfig's settle; codex): "Save failed" when the latch
     * rises, here or at the next open. A click's own failure flashed it too. */
    bool failed = prof_write_failed();
    if (failed && !(pfs_flags & PFS_FAILED)) flash(T_SAVE_FAILED, now);
    pfs_flags = (uint8_t)(failed ? pfs_flags | PFS_FAILED : pfs_flags & ~PFS_FAILED);
    /* P3: the Connection screens follow the slider. Its BT screens off BT, or
     * the off-BT page on BT: back to the top level, the cursor on Connection,
     * whose value shows the new link. P5b: a host restore pops the profile
     * pages the same way, the cursor on Profiles (their writes would be
     * refused). */
    uint8_t n = cur()->node, row = N_NONE;
    if ((is_bt_screen(n) && !on_bt()) || (n == N_CONNINFO && on_bt())) row = N_CONN;
    else if (is_prof_screen(n) && prof_restore_open()) row = N_PROFILES;
    if (row != N_NONE) {
        pv_show(PV_LIVE);   /* P5e: a restore's pop, out of Profiles */
        depth           = 1;
        stack[0].cursor = row_index(N_TOP, row);
        stack[0].top    = (uint8_t)(stack[0].cursor / MENU_ROWS * MENU_ROWS);
        retitle(now);
        if (is_confirm(n)) ms_set_layout(&L_LIST, false);
        else ms_retarget();
        moved = false;
    }
    /* P5e: the preview, once the knob has rested on Profiles' row MENU_PREVIEW_MS:
     * a saved slot's, or the live settings on Back and Save new. */
    if (cur()->node == N_PROFILES && pv != PV_HELD && idle_ms >= MENU_PREVIEW_MS) {
        uint8_t v, node = row_node(N_PROFILES, cur()->cursor, &v);
        pv_show(node == N_PSLOT ? v : PV_LIVE);
    }
    /* The Clock list: a jump (clock_now()) repaints the examples whole, here,
     * outside the scheduler's scans (invalidating from a signature would
     * clear the slot ms_step() is painting): an INVALID row repaints
     * whatever its signature. */
    if (cur()->node == N_CLOCK) {
        env_time_t now_t;
        (void)clock_now(&now_t);
        if (pg.clk.st & CK_JUMP) {
            pg.clk.st &= (uint8_t)~CK_JUMP;
            ms_invalidate_rows(S_ROW0 + 1u, CLOCK_EXAMPLES);
        }
    }
    uint8_t t = want_title(now, idle_ms);
    if (t != title_id) {
        title_id = t;
        moved    = true;
    }
    if (moved) ms_retarget();
}

void menu_bootloader_refused(uint32_t now) {
    if (!depth) return;
    flash(T_BOOT_BUSY, now);
    retitle(now);
    ms_retarget();
}

uint8_t menu_screen_id(void) { return depth ? (uint8_t)(16u + cur()->node) : 0; }
uint8_t menu_cursor(void) {
    if (!depth) return 0xFF;
    frame_t *f = cur();
    if (is_topic(f->node)) return f->top;
    if (f->node == N_PAIRING) return (uint8_t)(f->cursor << 4 | pair_phase(f->cursor));   /* P3: the slot and the phase */
    if (f->node == N_CONNINFO) return 0;   /* one page: not Reset's or Bootloader's confirm_page (codex) */
    if (f->node == N_PSAVE || f->node == N_PDELETE) return f->cursor;   /* P5b: the slot */
    if (f->node == N_PSLOT) return (uint8_t)(pslot << 4 | f->cursor);  /* P5d: the slot and the row */
    if (is_confirm(f->node)) return confirm_page;
    if (is_editor(f->node)) return me_value();   /* P1b: an editor's value */
    return f->cursor;
}

/* P3: the Pairing page holds the menu to the backstop, not the idle timeout:
 * adding a device on a computer takes longer than 24 s, and the idle time
 * dates from the last knob input, so a page that turned connected after that
 * would close the moment it did (codex). A close leaves pairing running. */
bool menu_long_wait(void) { return depth && cur()->node == N_PAIRING; }
