// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "quantum.h"

/* Adjust-key hold-to-repeat (always compiled) and the parameter readout
 * overlay (compiled under PARAM_OVERLAY -- a removable nicety, see
 * config.h). One module because they share the pressed-at-an-end-stop
 * bookkeeping (param_force_kc). */

/* Call at the top of process_record_kb for every event: arms/disarms the
 * repeat and notes the key for the end-stop readout. */
void param_repeat_process_record(uint16_t keycode, keyrecord_t *record);

/* Every main-loop pass: performs the repeat steps after the hold threshold. */
void param_repeat_task(void);

/* Push the LCD-brightness readout into the info band. Shared by the tap path in
 * process_record_kb and by the hold-repeat step, so a sweep keeps the readout
 * live instead of showing only the first value. Compiles to nothing without
 * PARAM_OVERLAY, so callers need no #ifdef. */
void param_show_lcd_brightness(void);

#ifdef PARAM_OVERLAY
/* 10 Hz housekeeping: surfaces RGB/NKRO changes in the info band. */
void param_status_task(void);
#endif
