// Copyright 2026 quill4gen7
// SPDX-License-Identifier: GPL-2.0-or-later
/* Host notifications -- see notify.h.
 *
 * FRAME (identical on both transports)
 *
 *   [0] hdr   seq:2 (bits 7-6) | effect:3 (bits 5-3) | page:1 (bit 2) | 0:2
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
    uint8_t     nl  = 0;
    while (nl < len && txt[nl] != '\n') nl++;
    if (page) {
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

void notify_task(void) {
    if ((lc_bits || lc_bad) && timer_elapsed32(lc_last) >= LC_GAP_MS) lc_close();
    if (fx_on && fx_until && (int32_t)(timer_read32() - fx_until) >= 0) fx_stop();
}

/* --- Dismiss ---------------------------------------------------------------
 * Any key press closes the page and is swallowed, release included, so the
 * key that dismisses does not also type. An until-dismissed effect ends too. */
static bool     swallow_release = false;
static keypos_t swallow_key;

bool notify_process_record(keyrecord_t *record) {
    if (swallow_release && !record->event.pressed &&
        record->event.key.row == swallow_key.row && record->event.key.col == swallow_key.col) {
        swallow_release = false;
        return true;
    }
    if (!record->event.pressed || !display_notify_page_active()) return false;
    display_notify_page_close();
    if (fx_on && !fx_until) fx_stop();
    swallow_release = true;
    swallow_key     = record->event.key;
    return true;
}
