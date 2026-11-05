// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Whether a flush has anything to convert (D9 revised), apart so the battery
// simulator can test the rule itself. The render's bytes changing is one reason. An
// encoding change is another (the mode, a raw pattern), and in a wide mode so is a
// value change, even with no byte moved: the chroma of a dim Solid is the chroma of
// a bright one, and a cap change reaches the LEDs only this way.
#pragma once

#include <stdbool.h>
#include <stdint.h>

static inline bool rowisr_flush_dirty(bool bytes_changed, bool wide, bool last_wide, uint8_t v, uint8_t last_v) {
    return bytes_changed || wide != last_wide || (wide && v != last_v);
}
