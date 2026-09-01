// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "quantum.h"

/* RGB adjust-key hold-to-repeat (always compiled) and the parameter readout
 * overlay (compiled under PARAM_OVERLAY -- a removable nicety, see
 * config.h). One module because they share the pressed-at-an-end-stop
 * bookkeeping (param_force_kc). */

/* Call at the top of process_record_kb for every event: arms/disarms the
 * repeat and notes the key for the end-stop readout. */
void rgb_repeat_process_record(uint16_t keycode, keyrecord_t *record);

/* Every main-loop pass: performs the repeat steps after the hold threshold. */
void rgb_repeat_task(void);

#ifdef PARAM_OVERLAY
/* 10 Hz housekeeping: surfaces RGB/NKRO changes in the info band. */
void param_status_task(void);
#endif
