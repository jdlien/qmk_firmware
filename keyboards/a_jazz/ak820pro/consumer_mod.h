// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "quantum.h"

/* Modified-consumer sequencing (e.g. LSA(KC_VOLU) on the knob): flush the
 * modifiers on their own endpoint before the consumer usage, hold them
 * across a spin burst, release on idle. See consumer_mod.c for the
 * endpoint-ordering race this exists to beat. */

/* Call early in process_record_kb; returns false when fully handled. */
bool process_modified_consumer(uint16_t keycode, keyrecord_t *record);

/* 10 Hz housekeeping: drop held modifiers once the spin stops. */
void modified_consumer_task(void);
