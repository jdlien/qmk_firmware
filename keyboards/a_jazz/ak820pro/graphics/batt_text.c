// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
#include "batt_text.h"

#include <stdio.h>
#include <string.h>

void batt_level_text(char *out, uint8_t pct, bool on_battery, uint16_t left_min, bool avg_ready, bool hours) {
    char wide[16];   /* the compiler cannot prove 7 fit: composed wide, then copied */
    bool ready = on_battery && avg_ready && left_min != 0xFFFFu;
    if (ready && hours) {
        /* JD, 10-25: "160 h ... all the time there" */
        if (left_min < 60u) snprintf(wide, sizeof wide, "<1h");
        else                snprintf(wide, sizeof wide, "%uh", (unsigned)(left_min / 60u));
    } else if (ready && left_min < 600u) {
        /* Hours of USE beside the level once they matter: under 10 h on
         * battery, with a mature average (plans/DEEP-SLEEP-BUILD-PLAN.md,
         * section 7): "18% 6h", "99% <1h". */
        if (left_min < 60u) snprintf(wide, sizeof wide, "%u%% <1h", pct);
        else                snprintf(wide, sizeof wide, "%u%% %uh", pct, (unsigned)(left_min / 60u));
    } else {
        snprintf(wide, sizeof wide, "%u%%", pct);
    }
    size_t n = strlen(wide);
    if (n > BATT_TEXT_MAX) n = BATT_TEXT_MAX;
    memcpy(out, wide, n);
    out[n] = '\0';
}
