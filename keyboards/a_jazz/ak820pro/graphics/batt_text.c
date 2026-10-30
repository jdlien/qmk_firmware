// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
#include "batt_text.h"

#include <stdio.h>
#include <string.h>

void batt_level_text(char *out, uint8_t pct, bool on_battery, uint16_t left_min, bool avg_ready, uint8_t view) {
    char wide[24];   /* the compiler cannot prove 8 fit: composed wide, then copied */
    char hrs[12];
    bool ready = on_battery && avg_ready && left_min != 0xFFFFu;
    unsigned h = left_min / 60u;
    if (h > 999u) h = 999u;   /* three digits: the estimate's ceiling is 1092 h, and nothing awake draws that little */
    if (left_min < 60u) snprintf(hrs, sizeof hrs, "<1h");
    else                snprintf(hrs, sizeof hrs, "%uh", h);
    if (view == BATT_VIEW_ICON) {
        wide[0] = '\0';
    } else if (ready && view == BATT_VIEW_HOURS) {
        /* JD, 10-25: "160 h ... all the time there" */
        snprintf(wide, sizeof wide, "%s", hrs);
    } else if (ready && (view == BATT_VIEW_BOTH || left_min < 600u)) {
        /* Both (JD, 10-26: "show hours & percent"), always; Percent, the
         * hours of USE beside the level once they matter: under 10 h on
         * battery, with a mature average (plans/DEEP-SLEEP-BUILD-PLAN.md,
         * section 7): "18% 6h", "99% <1h". */
        snprintf(wide, sizeof wide, "%u%% %s", pct, hrs);
        /* "100% 160h" is 9 cells, past even the closed-up space's 8: the
         * level alone (JD: "only ever show time if we're under 99% if it
         * gets too tight"). An hour's use from FULL leaves 100% only under
         * 0.5% an hour, so it is rare. */
        if (strlen(wide) > BATT_TEXT_MAX) snprintf(wide, sizeof wide, "%u%%", pct);
    } else {
        snprintf(wide, sizeof wide, "%u%%", pct);
    }
    size_t n = strlen(wide);
    if (n > BATT_TEXT_MAX) n = BATT_TEXT_MAX;
    memcpy(out, wide, n);
    out[n] = '\0';
}
