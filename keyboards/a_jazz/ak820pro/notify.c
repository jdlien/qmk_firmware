// Copyright 2026 quill4gen7
// SPDX-License-Identifier: GPL-2.0-or-later
/* Host notifications -- see notify.h.
 *
 * FRAME (identical on both transports)
 *
 *   [0] hdr   seq:2 (bits 7-6) | effect:3 (bits 5-3) | page:1 (bit 2) |
 *             kind:2 (bits 1-0): 0 SHOW, 1 CLOSE, 2 ASK, 3 AMBIENT
 *   [1] hue   0-255, QMK hue scale
 *   [2] dur   effect duration in 100 ms units; 0 = no lighting,
 *             255 = until the page is dismissed
 *   [3] gif   page mode: GIF slot 1..NOTIFY_GIF_SLOTS, 0 = none
 *   [4] len   text bytes that follow, <= NOTIFY_TEXT_MAX
 *   [5..]     ASCII; an optional '\n' forces a line break
 *   [5+len]   CRC-8 (poly 0x07, init 0x00) over bytes [0 .. 5+len)
 *
 * Without the page bit the text goes to the dashboard's two-line host-text
 * band. With it the notification takes the whole panel until any key is
 * pressed: the slot's GIF if it holds a valid one, else the text, word-wrapped.
 *
 * GIF SLOTS live in the always-writable asset region, above the asset image
 * (0xCE0000, ~180 KB today) with room for it to grow:
 *   slot n at NOTIFY_GIF_BASE + (n-1) * NOTIFY_GIF_STRIDE
 *   +0x000  "AKN1", frame count (1..15)
 *   +0x100  frames, 128x128 RGB565 lo-byte-first, 0x8000 apart
 * Written over the cable with `ak820ctl flash write`.
 *
 * CLOSE (kind 1) closes the page and ends an until-dismissed effect: the host
 * sends it when the notification was dealt with at the computer. [3] bit7 set
 * means "only question slot (bits 5-4)": that question is dropped and the next
 * one shown. A plain CLOSE leaves pending questions alone. len may be 0.
 *
 * ASK (kind 2) is a page with a question, answered from the board. Text is
 * title\ndetail...\nopt1\nopt2..., one row each; [3] is flags:
 *   bits1-0  number of detail lines after the title (0-2), shown as is
 *   bit2     PERM: a permission -- option 1 answers ALLOW, option 2 DENY
 *   bit3     MULTI: checkboxes -- Space toggles the option under the cursor,
 *            Enter sends ANSWER_CHOICE[i] for every checked option, one at a
 *            time, then ANSWER_ALLOW as the end marker
 *   bits5-4  SLOT 0-3: up to four questions wait at once, from different
 *            sessions. The title shows "n/m"; PgUp/PgDn (or Home/End) switch
 *            between them; answering one shows the next. Every answer is
 *            preceded by ANSWER_SLOT[slot] so each asker takes only its own.
 * Title, details and options share the 5 rows; a hint fills a spare row.
 * The arrows move the selection (Left/Up back, Right/Down forward), Space
 * ticks a box (MULTI), Enter confirms, Esc cancels and gives the keyboard back.
 * Layer keys (Fn) pass through, so Fn+U/Fn+O reach PgUp/PgDn. Every other key
 * is IGNORED while a question is up: one stray keystroke must not throw the
 * question away (the first version cancelled on any key, and while typing
 * that is the likeliest key to arrive). The answer goes back as one HID
 * consumer usage -- the only board-to-host path over BT/2.4G besides
 * keystrokes -- picked from AL usages nothing on the desktop binds:
 *   ANSWER_ALLOW / ANSWER_DENY for a permission, ANSWER_CHOICE[i] for option
 *   i of a choice, ANSWER_CANCEL for Esc. The host reads them from the
 *   receiver's "Consumer Control" input device.
 *
 * AMBIENT (kind 3) sets the screensaver: [3] = GIF slot (0 = off), [2] =
 * seconds without a key press before it starts (0 = 60). The GIF plays full
 * screen until any key, and that key is NOT swallowed -- it types as usual.
 * Notifications and questions always take the screen from it. The host picks
 * the slot by what it is doing (e.g. idle vs. at work). Default after boot:
 * slot 3 after 60 s, which shows nothing if that slot is empty.
 *
 * LED CHANNEL (BT/2.4G)
 *
 * The CH582F forwards only the host keyboard-LED bitmap, and the host only
 * sends that when it CHANGES, so every bit has to be a change. One bit per
 * report: toggling Num Lock sends a 0, toggling Scroll Lock sends a 1, MSB
 * first. A report where both flipped means reports were coalesced somewhere
 * (the host's LED worker, the dongle, the UART) -- the frame is dropped.
 * Losing two toggles of the same line is invisible here and is caught by the
 * length check or the CRC. A frame ends when no change arrives for
 * LC_GAP_MS; the host repeats each frame with the same seq and the repeat is
 * swallowed as a duplicate.
 *
 * Spurious changes are expected: the compositor rewrites all three lock LEDs
 * whenever the xkb lock state moves. One outside a frame opens a garbage frame
 * that fails the length/CRC check; one inside a frame corrupts that copy, and
 * the repeat carries the message. */
#include "quantum.h"
#include "notify.h"
#include "graphics/display.h"
#include "graphics/lcd_bus.h"

#define NOTIFY_TEXT_MAX 44
#define LC_GAP_MS       400u
#define LC_MAX_BYTES    (5 + NOTIFY_TEXT_MAX + 1)

#define NOTIFY_GIF_BASE   0xD80000u
#define NOTIFY_GIF_STRIDE 0x80000u
#define NOTIFY_GIF_SLOTS  5
#define NOTIFY_GIF_MAXF   ((NOTIFY_GIF_STRIDE - 0x100u) / 0x8000u)   /* 15 */
#define DUP_WINDOW_MS   15000u

#define LED_NUM    0x01u
#define LED_SCROLL 0x04u
#define LC_MASK    (LED_NUM | LED_SCROLL)

/* Answer usages (HID consumer page, AL range) and the Linux keys they map to. */
#define ANSWER_ALLOW  0x191   /* AL Finance       -> KEY_FINANCE */
#define ANSWER_DENY   0x1AB   /* AL Spell Check   -> KEY_SPELLCHECK */
#define ANSWER_CANCEL 0x1BD   /* AL Info          -> KEY_INFO */
/* Slot markers, sent before every answer. */
static const uint16_t ANSWER_SLOT[4] = {
    0x199,   /* AL Network Chat  -> KEY_CHAT */
    0x1A7,   /* AL Documents     -> KEY_DOCUMENTS */
    0x1AE,   /* AL Keyboard Layout -> KEY_KEYBOARD */
    0x18E,   /* AL Calendar      -> KEY_CALENDAR */
};
static const uint16_t ANSWER_CHOICE[4] = {
    0x1B6,   /* AL Image Browser -> KEY_IMAGES */
    0x1B7,   /* AL Audio Browser -> KEY_AUDIO */
    0x1B8,   /* AL Movie Browser -> KEY_VIDEO */
    0x1BC,   /* AL Instant Messaging -> KEY_MESSENGER */
};

enum { KIND_SHOW = 0, KIND_CLOSE = 1, KIND_ASK = 2, KIND_AMBIENT = 3 };
#define ASK_NDETAIL 0x03
#define ASK_PERM    0x04
#define ASK_MULTI   0x08
#define ASK_SLOTS   4
#define CLOSE_SLOT  0x80
#define ASK_MAX_OPTS 4
#define ASK_ROWS     5

enum notify_effect {
    FX_SOLID   = 0,
    FX_BLINK   = 1,
    FX_BREATHE = 2,
    FX_SWEEP   = 3,
};

/* --- CRC ------------------------------------------------------------------ */

static uint8_t crc8(const uint8_t *p, uint8_t n) {
    uint8_t c = 0;
    while (n--) {
        c ^= *p++;
        for (uint8_t i = 0; i < 8; i++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
    }
    return c;
}

/* --- Stats ---------------------------------------------------------------- */

static struct {
    uint16_t ok, crc_err, len_err, both_err, dup;
    uint32_t transitions;
    uint16_t last_bits, last_ms, min_gap_ms;
} st = {.min_gap_ms = 0xFFFF};

static void put16(uint8_t *o, uint16_t v) { o[0] = (uint8_t)(v >> 8); o[1] = (uint8_t)v; }

void notify_stats_fill(uint8_t *o) {
    put16(o + 0, st.ok);
    put16(o + 2, st.crc_err);
    put16(o + 4, st.len_err);
    put16(o + 6, st.both_err);
    put16(o + 8, st.dup);
    o[10] = (uint8_t)(st.transitions >> 24);
    o[11] = (uint8_t)(st.transitions >> 16);
    o[12] = (uint8_t)(st.transitions >> 8);
    o[13] = (uint8_t)st.transitions;
    put16(o + 14, st.last_bits);
    put16(o + 16, st.last_ms);
    put16(o + 18, st.min_gap_ms);
}

/* --- Effect state --------------------------------------------------------- */

static bool     fx_on        = false;
static uint8_t  fx_kind      = FX_SOLID;
static uint8_t  fx_hue       = 0;
static uint32_t fx_until     = 0;
static bool     fx_forced_on = false;   /* we enabled RGB just for this */

static void fx_stop(void) {
    fx_on = false;
    if (fx_forced_on) {
        rgb_matrix_disable_noeeprom();
        fx_forced_on = false;
    }
}

static void fx_start(uint8_t kind, uint8_t hue, uint8_t dur) {
    fx_kind  = kind;
    fx_hue   = hue;
    fx_until = (dur == 255) ? 0 : timer_read32() + (uint32_t)dur * 100u;
    if (!rgb_matrix_is_enabled()) {
        rgb_matrix_enable_noeeprom();
        fx_forced_on = true;
    }
    fx_on = true;
}

bool rgb_matrix_indicators_advanced_kb(uint8_t led_min, uint8_t led_max) {
    if (!rgb_matrix_indicators_advanced_user(led_min, led_max)) return false;
    if (!fx_on) return true;

    /* g_rgb_timer is the matrix's own frame clock: no extra timer reads here,
     * which on this MCU cost real time per call (see ak820pro.c). */
    uint32_t t = g_rgb_timer;
    uint8_t  v = 255;
    switch (fx_kind) {
        case FX_BLINK:   v = ((t / 180u) & 1u) ? 0 : 255; break;          /* ~2.8 Hz */
        case FX_BREATHE: {                                                 /* ~1.5 s triangle */
            uint8_t ph = (uint8_t)(t / 6u);
            v          = (uint8_t)(ph < 128u ? ph * 2u : (255u - ph) * 2u);
            break;
        }
        default: break;
    }

    for (uint8_t i = led_min; i < led_max; i++) {
        uint8_t vi = v;
        if (fx_kind == FX_SWEEP) {
            /* A bright band ~48 units wide travelling left to right. */
            int16_t pos = (int16_t)((t / 4u) % 288u);   /* 0..287, off the right edge at the end */
            int16_t d   = (int16_t)g_led_config.point[i].x - pos + 24;
            vi          = (d < 0 || d > 48) ? 24 : 255;
        }
        rgb_t c = hsv_to_rgb((hsv_t){fx_hue, 255, vi});
        rgb_matrix_set_color(i, c.r, c.g, c.b);
    }
    return true;
}

/* --- Frame execution ------------------------------------------------------ */

static uint8_t  last_seq    = 0xFF;
static uint32_t last_seq_at = 0;

/* --- Ambient (screensaver) ------------------------------------------------- */

static uint8_t  amb_slot    = 3;        /* GIF slot, 0 = off */
static uint16_t amb_after_s = 60;
static bool     amb_showing = false;
static uint32_t last_key_at = 0;

/* --- Ask page --------------------------------------------------------------- */

typedef struct {
    bool    used;
    uint8_t flags, ndet, nopt, sel, checked;   /* checked: MULTI, bit i = option i */
    char    title[13];
    char    det[2][13];
    char    opt[ASK_MAX_OPTS][11];             /* 12 columns less the "> " marker */
} ask_t;

static ask_t   asks[ASK_SLOTS];
static uint8_t ask_cur = 0;       /* the slot on screen */
static bool    ask_on  = false;   /* the page is showing questions */

/* Usages still to send, one per 10 Hz tick with a release in between, so
 * consecutive usages are never merged in one consumer report. A ring: answers
 * given in quick succession queue up behind each other. */
static uint16_t answer_q[16];
static uint8_t  answer_qh = 0, answer_qt = 0;
static bool     answer_release = false;

#define PAGE_COLS 12

static void copy_row(char *dst, const char *s, uint8_t n) {
    if (n > PAGE_COLS) n = PAGE_COLS;
    memcpy(dst, s, n);
    dst[n] = 0;
}

static uint8_t ask_count(void) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < ASK_SLOTS; i++) n += asks[i].used;
    return n;
}

/* The next used slot from `from`, stepping `dir` (+1/-1), excluding `from`
 * itself unless it is the only one. ASK_SLOTS if none. */
static uint8_t ask_step(uint8_t from, int8_t dir) {
    for (uint8_t k = 1; k <= ASK_SLOTS; k++) {
        uint8_t i = (uint8_t)((from + ASK_SLOTS + dir * k) % ASK_SLOTS);
        if (asks[i].used) return i;
    }
    return ASK_SLOTS;
}

/* Rebuild the page text for the question on screen. Every row is centred HERE
 * and padded to the full width, so the page's own centring leaves it alone and
 * nothing shifts as the selection marker moves. */
static void ask_render(bool fresh) {
    ask_t  *a = &asks[ask_cur];
    char    buf[ASK_ROWS * (PAGE_COLS + 1)];
    uint8_t n = 0, rows = 0;
    #define ROW(s) do { const char *_s = (s); uint8_t _l = (uint8_t)strlen(_s); \
        uint8_t _p = (uint8_t)((PAGE_COLS - _l) / 2), _k; \
        for (_k = 0; _k < _p; _k++) buf[n++] = ' '; \
        memcpy(buf + n, _s, _l); n += _l; \
        for (_k = (uint8_t)(_p + _l); _k < PAGE_COLS; _k++) buf[n++] = ' '; \
        buf[n++] = '\n'; rows++; } while (0)
    char row[PAGE_COLS + 1];
    uint8_t total = ask_count();
    if (total > 1) {
        /* "Title  2/3": the title cut to fit, the position right-aligned. */
        uint8_t pos = 1;
        for (uint8_t i = 0; i < ask_cur; i++) pos += asks[i].used;
        uint8_t tl = (uint8_t)strlen(a->title);
        if (tl > PAGE_COLS - 4) tl = PAGE_COLS - 4;
        memset(row, ' ', PAGE_COLS);
        memcpy(row, a->title, tl);
        row[PAGE_COLS - 3] = (char)('0' + pos);
        row[PAGE_COLS - 2] = '/';
        row[PAGE_COLS - 1] = (char)('0' + total);
        row[PAGE_COLS] = 0;
        ROW(row);
    } else {
        ROW(a->title);
    }
    for (uint8_t i = 0; i < a->ndet; i++) ROW(a->det[i]);
    /* "> label" padded to the longest label, so every option row has the same
     * width and they line up when centred. */
    bool    multi = a->flags & ASK_MULTI;
    uint8_t lead  = multi ? 3 : 2;   /* ">x " / "> " */
    uint8_t w = 0;
    for (uint8_t k = 0; k < a->nopt; k++) if (strlen(a->opt[k]) > w) w = (uint8_t)strlen(a->opt[k]);
    if (w > PAGE_COLS - lead) w = PAGE_COLS - lead;
    for (uint8_t i = 0; i < a->nopt; i++) {
        uint8_t l = (uint8_t)strlen(a->opt[i]);
        if (l > w) l = w;
        row[0] = (i == a->sel) ? '>' : ' ';
        if (multi) row[1] = (a->checked & (1u << i)) ? 'x' : 'o';
        row[lead - 1] = ' ';
        memcpy(row + lead, a->opt[i], l);
        memset(row + lead + l, ' ', (size_t)(w - l));
        row[lead + w] = 0;
        ROW(row);
    }
    if (rows < ASK_ROWS) ROW(multi ? "Spc Ent Esc" : "Enter / Esc");
    #undef ROW
    if (n) n--;   /* drop the trailing newline */
    if (fresh) display_notify_page_text(buf, n);
    else       display_notify_page_update(buf, n);
    ask_on = true;
}

static void ask_open(uint8_t flags, const char *txt, uint8_t len) {
    uint8_t slot = (flags >> 4) & 0x03;
    ask_t  *a    = &asks[slot];
    memset(a, 0, sizeof(*a));
    a->flags = flags;
    uint8_t want_det = flags & ASK_NDETAIL;
    if (want_det > 2) want_det = 2;
    uint8_t line = 0, start = 0;
    for (uint8_t i = 0; i <= len; i++) {
        if (i < len && txt[i] != '\n') continue;
        uint8_t l = (uint8_t)(i - start);
        if (line == 0)             copy_row(a->title, txt + start, l);
        else if (line <= want_det) copy_row(a->det[a->ndet++], txt + start, l);
        else if (a->nopt < ASK_MAX_OPTS && 1 + a->ndet + a->nopt < ASK_ROWS) {
            if (l > PAGE_COLS - 2) l = PAGE_COLS - 2;
            memcpy(a->opt[a->nopt], txt + start, l);
            a->opt[a->nopt++][l] = 0;
        }
        line++;
        start = (uint8_t)(i + 1);
    }
    /* A permission needs its two answers; a choice at least one option. */
    if (a->nopt < ((flags & ASK_PERM) ? 2 : 1)) return;
    a->used = true;
    if (ask_on && display_notify_page_active()) {
        ask_render(false);   /* another question on screen: only its "n/m" changes */
    } else {
        ask_cur = slot;
        ask_render(true);
    }
}

static void answer_push(uint16_t usage) {
    uint8_t next = (uint8_t)((answer_qt + 1) % (sizeof(answer_q) / sizeof(answer_q[0])));
    if (next == answer_qh) return;   /* full: cannot happen with 4 slots x 6 usages */
    answer_q[answer_qt] = usage;
    answer_qt = next;
}

/* Answer the question on screen and drop it. True if another question is
 * now showing, false if none is left (the caller closes the page). */
static bool ask_answer(uint16_t usage) {
    ask_t *a = &asks[ask_cur];
    answer_push(ANSWER_SLOT[ask_cur]);
    if (usage) {
        answer_push(usage);
    } else {   /* MULTI: the ticked options, then the end marker */
        for (uint8_t i = 0; i < a->nopt; i++)
            if (a->checked & (1u << i)) answer_push(ANSWER_CHOICE[i]);
        answer_push(ANSWER_ALLOW);
    }
    a->used = false;
    uint8_t next = ask_step(ask_cur, 1);
    if (next == ASK_SLOTS) { ask_on = false; return false; }
    ask_cur = next;
    ask_render(true);
    return true;
}

/* Drop a question the host withdrew (answered at the computer, timed out). */
static void ask_drop(uint8_t slot) {
    if (!asks[slot].used) return;
    asks[slot].used = false;
    if (!ask_on) return;
    if (slot == ask_cur) {
        uint8_t next = ask_step(slot, 1);
        if (next == ASK_SLOTS) {
            ask_on = false;
            display_notify_page_close();
            return;
        }
        ask_cur = next;
        ask_render(true);
    } else {
        ask_render(false);   /* the "n/m" changes */
    }
}

/* The slot's frame count, or 0 if it holds nothing valid. */
static uint8_t gif_frames(uint8_t slot) {
    if (slot == 0 || slot > NOTIFY_GIF_SLOTS) return 0;
    uint8_t h[5];
    flash_read_bytes(NOTIFY_GIF_BASE + (uint32_t)(slot - 1) * NOTIFY_GIF_STRIDE, h, sizeof(h));
    if (h[0] != 'A' || h[1] != 'K' || h[2] != 'N' || h[3] != '1') return 0;
    return (h[4] >= 1 && h[4] <= NOTIFY_GIF_MAXF) ? h[4] : 0;
}

bool notify_frame(const uint8_t *b, uint8_t n) {
    if (n < 6 || b[4] > NOTIFY_TEXT_MAX || n != (uint8_t)(6 + b[4])) {
        st.len_err++;
        return false;
    }
    if (crc8(b, (uint8_t)(n - 1)) != b[n - 1]) {
        st.crc_err++;
        return false;
    }

    uint8_t seq = b[0] >> 6;
    if (seq == last_seq && timer_elapsed32(last_seq_at) < DUP_WINDOW_MS) {
        st.dup++;
        return true;
    }
    last_seq    = seq;
    last_seq_at = timer_read32();
    st.ok++;

    const char *txt = (const char *)&b[5];
    uint8_t     len = b[4];
    bool        page = (b[0] >> 2) & 1;
    uint8_t     kind = b[0] & 0x03;

    if (kind == KIND_AMBIENT) {
        bool changed = b[3] != amb_slot;
        amb_slot    = b[3] <= NOTIFY_GIF_SLOTS ? b[3] : 0;
        amb_after_s = b[2] ? b[2] : 60;
        if (amb_showing && changed) {
            uint8_t frames = amb_slot ? gif_frames(amb_slot) : 0;
            if (frames) display_notify_page_gif(NOTIFY_GIF_BASE + (uint32_t)(amb_slot - 1) * NOTIFY_GIF_STRIDE, frames);
            else { amb_showing = false; display_notify_page_close(); }
        }
        return true;
    }
    if (kind == KIND_CLOSE) {
        if (b[3] & CLOSE_SLOT) {
            ask_drop((b[3] >> 4) & 0x03);
            if (!ask_count() && fx_on && !fx_until) fx_stop();
        } else if (!ask_count()) {   /* pending questions outlive a plain close */
            ask_on      = false;
            amb_showing = false;
            display_notify_page_close();
            if (fx_on && !fx_until) fx_stop();
        }
        return true;
    }
    if (kind == KIND_ASK) {
        amb_showing = false;   /* a question outranks the screensaver */
        ask_open(b[3], txt, len);
        if (b[2]) fx_start((uint8_t)((b[0] >> 3) & 0x07), b[1], b[2]);
        return true;
    }
    uint8_t     nl  = 0;
    while (nl < len && txt[nl] != '\n') nl++;
    if (page && ask_count()) {
        page = false;   /* questions are waiting: they keep the screen */
    }
    if (page) {
        ask_on      = false;
        amb_showing = false;
        uint8_t frames = gif_frames(b[3]);
        if (!(frames && display_notify_page_gif(NOTIFY_GIF_BASE + (uint32_t)(b[3] - 1) * NOTIFY_GIF_STRIDE, frames)))
            display_notify_page_text(txt, len);
    } else if (len) {
        /* Line 0 first: display_set_text_line(0) clears line 1. */
        display_set_text_line(0, DISPLAY_ICON_NONE, txt, nl);
        if (nl < len) display_set_text_line(1, DISPLAY_ICON_NONE, txt + nl + 1, (uint8_t)(len - nl - 1));
    }
    if (b[2]) fx_start((uint8_t)((b[0] >> 3) & 0x07), b[1], b[2]);
    return true;
}

/* --- LED-channel decoder -------------------------------------------------- */

/* QMK starts from an all-off LED state and calls led_update_kb only on a
 * change, so the first report after boot differs from 0, not from "unknown".
 * Treating it as a baseline instead swallowed the first bit of the first
 * frame after every reset (measured: 111 of 112 bits). */
static uint8_t  lc_prev  = 0;
static uint8_t  lc_buf[LC_MAX_BYTES];
static uint16_t lc_bits  = 0;
static bool     lc_bad   = false;  /* frame poisoned, swallow until the gap */
static uint32_t lc_first = 0, lc_last = 0;

static void lc_close(void) {
    if (lc_bits && !lc_bad) {
        st.last_bits = lc_bits;
        st.last_ms   = (uint16_t)(lc_last - lc_first);
        if (lc_bits % 8u) st.len_err++;
        else notify_frame(lc_buf, (uint8_t)(lc_bits / 8u));
    }
    lc_bits = 0;
    lc_bad  = false;
}

void notify_leds_changed(uint8_t raw) {
    uint8_t cur = raw & LC_MASK;
    uint8_t d = cur ^ lc_prev;
    lc_prev   = cur;
    if (!d) return;   /* Caps only */

    uint32_t now = timer_read32();
    if (lc_bits || lc_bad) {
        uint32_t gap = now - lc_last;
        if (gap >= LC_GAP_MS) lc_close();
        else if (gap < st.min_gap_ms) st.min_gap_ms = (uint16_t)gap;
    }
    st.transitions++;
    if (!lc_bits && !lc_bad) lc_first = now;
    lc_last = now;
    if (lc_bad) return;

    if (d == LC_MASK) {
        st.both_err++;
        lc_bad = true;
        return;
    }
    if (lc_bits >= LC_MAX_BYTES * 8u) {
        st.len_err++;
        lc_bad = true;
        return;
    }
    uint8_t bit  = (d == LED_SCROLL) ? 1 : 0;
    uint8_t byte = (uint8_t)(lc_bits / 8u);
    if (lc_bits % 8u == 0) lc_buf[byte] = 0;
    lc_buf[byte] |= (uint8_t)(bit << (7u - lc_bits % 8u));
    lc_bits++;
}

static void ambient_task(void) {
    if (amb_showing) {
        if (!display_notify_page_active()) amb_showing = false;   /* someone closed it */
        return;
    }
    if (!amb_slot || display_notify_page_active() || display_debug_active() || !display_get_power()) return;
    if (timer_elapsed32(last_key_at) < (uint32_t)amb_after_s * 1000u) return;
    uint8_t frames = gif_frames(amb_slot);
    if (frames && display_notify_page_gif(NOTIFY_GIF_BASE + (uint32_t)(amb_slot - 1) * NOTIFY_GIF_STRIDE, frames))
        amb_showing = true;
    else
        last_key_at = timer_read32();   /* nothing to show: do not retry every tick */
}

void notify_task(void) {
    ambient_task();
    if (answer_release) {
        host_consumer_send(0);
        answer_release = false;
    } else if (answer_qh != answer_qt) {
        host_consumer_send(answer_q[answer_qh]);
        answer_qh = (uint8_t)((answer_qh + 1) % (sizeof(answer_q) / sizeof(answer_q[0])));
        answer_release = true;
    }
    if ((lc_bits || lc_bad) && timer_elapsed32(lc_last) >= LC_GAP_MS) lc_close();
    if (fx_on && fx_until && (int32_t)(timer_read32() - fx_until) >= 0) fx_stop();
}

/* --- Keys -----------------------------------------------------------------
 * Any key press on a page is swallowed, release included, so it does not also
 * type. On an ask page only the arrows, Enter and Esc do anything (see ASK);
 * on a plain page any key dismisses. An until-dismissed effect ends with the
 * page. */
static bool     swallow_release = false;
static keypos_t swallow_key;

/* True while the page stays up after this key. */
static bool ask_key(uint16_t kc) {
    ask_t *a = &asks[ask_cur];
    switch (kc) {
        case KC_LEFT: case KC_UP:
            a->sel = (uint8_t)((a->sel + a->nopt - 1) % a->nopt);
            ask_render(false);
            return true;
        case KC_RIGHT: case KC_DOWN:
            a->sel = (uint8_t)((a->sel + 1) % a->nopt);
            ask_render(false);
            return true;
        case KC_PGUP: case KC_HOME:
        case KC_PGDN: case KC_END: {
            int8_t  dir  = (kc == KC_PGUP || kc == KC_HOME) ? -1 : 1;
            uint8_t next = ask_step(ask_cur, dir);
            if (next != ASK_SLOTS && next != ask_cur) {
                ask_cur = next;
                ask_render(true);
            }
            return true;
        }
        case KC_SPC:
            if (a->flags & ASK_MULTI) {
                a->checked ^= (uint8_t)(1u << a->sel);
                ask_render(false);
            }
            return true;
        case KC_ENT:
            if (a->flags & ASK_PERM)       return ask_answer(a->sel == 0 ? ANSWER_ALLOW : ANSWER_DENY);
            else if (a->flags & ASK_MULTI) return ask_answer(0);
            else                           return ask_answer(ANSWER_CHOICE[a->sel]);
        case KC_ESC:
            return ask_answer(ANSWER_CANCEL);
        default:
            return true;   /* ignored: the question waits for a real answer */
    }
}

bool notify_process_record(uint16_t keycode, keyrecord_t *record) {
    if (record->event.pressed) {
        last_key_at = timer_read32();
        if (amb_showing) {   /* wake: back to the dashboard, and the key still types */
            amb_showing = false;
            if (display_notify_page_active()) display_notify_page_close();
            return false;
        }
    }
    if (swallow_release && !record->event.pressed &&
        record->event.key.row == swallow_key.row && record->event.key.col == swallow_key.col) {
        swallow_release = false;
        return true;
    }
    if (!record->event.pressed || !display_notify_page_active()) return false;
    /* Layer keys pass, press and release: Fn+U / Fn+O must reach PgUp/PgDn. */
    if (ask_on && (IS_QK_MOMENTARY(keycode) || IS_QK_LAYER_TAP(keycode) || IS_QK_ONE_SHOT_LAYER(keycode)))
        return false;
    swallow_release = true;
    swallow_key     = record->event.key;
    if (ask_on && ask_key(keycode)) return true;
    ask_on = false;
    display_notify_page_close();
    if (fx_on && !fx_until) fx_stop();
    return true;
}

/* --- Raw HID staging -------------------------------------------------------- */
static uint8_t stage_buf[LC_MAX_BYTES];

bool notify_stage(uint8_t offset, const uint8_t *p, uint8_t n) {
    if (n > 28 || (uint16_t)offset + n > sizeof(stage_buf)) return false;
    memcpy(stage_buf + offset, p, n);
    return true;
}

bool notify_commit(uint8_t len) {
    return len <= sizeof(stage_buf) && notify_frame(stage_buf, len);
}
