// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
#include "text_fit.h"

#include <stddef.h>

uint8_t text_fit_budget(uint8_t face, uint8_t line) {
    if (line > 1u) return 0;
    if (face == TEXT_FACE_SMALL) return line ? TEXT_FIT_SMALL_L1 : TEXT_FIT_SMALL_L0;
    return line ? TEXT_FIT_LARGE_L1 : TEXT_FIT_LARGE_L0;
}

uint8_t text_fit_len(uint8_t face, uint8_t line, const char *s) {
    if (!s) return 0;
    const uint8_t max = text_fit_budget(face, line);
    uint8_t       n   = 0;
    while (n < max && s[n]) n++;   /* never reads past the budget */
    return n;
}
