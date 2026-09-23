// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include_next <chconf.h>

/* chSysHalt() disables interrupts and spins; without this it never reaches
 * HardFault and the watchdog's record would name the interrupted operation as
 * if it had hung. The common chconf.h defines the hook unconditionally, so it
 * is replaced here. __builtin_return_address(0) expands inside chSysHalt():
 * its caller, the code that halted. See fault.c. */
#undef CH_CFG_SYSTEM_HALT_HOOK
#define CH_CFG_SYSTEM_HALT_HOOK(reason) do {                                \
    (void)(reason);                                                         \
    extern void watchdog_record_halt(uint32_t caller);                      \
    watchdog_record_halt((uint32_t)__builtin_return_address(0));            \
} while (0)
