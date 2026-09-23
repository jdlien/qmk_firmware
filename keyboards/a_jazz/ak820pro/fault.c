// Copyright 2026 JD Lien
// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Terminal handlers: a hard fault, a vector nobody handles, a ChibiOS halt.
 *
 * Before this, ChibiOS's weak defaults spun silently (vectors.S), the
 * watchdog fired, and the breadcrumb named whatever operation happened to be
 * running -- a fault looked exactly like a hang. Now each writes a terminal
 * record (watchdog_record.c) and THEN spins, so the watchdog still resets
 * through the existing path and accounting (degraded mode included). None of
 * them resets the board itself: a handler that did would loop forever on a
 * deterministic boot-time fault.
 *
 * NMI IS NOT OURS: this ChibiOS port switches context through NMI and
 * NMI_Handler is strong in chcore.c. A fault inside NMI locks up without
 * reaching HardFault. (crash-hunt review, finding 4)
 */
#include <stdint.h>
#include "watchdog_record.h"

/* Private stack for the recorders. In .bss, above both stacks (MSP at
 * 0x20000000-0x20000400 and the main thread's PSP at 0x20000400-0x20000C00),
 * so neither can reach it by overflowing downward -- and an overflowed MSP is
 * exactly the case where the handler must not push onto MSP. The recorders
 * need a few words; 128 bytes leaves margin. (crash-hunt review, finding 1) */
uint32_t wdt_fault_stack[32] __attribute__((aligned(8), used));

void HardFault_Handler(void) __attribute__((naked, used));
void _unhandled_exception(void) __attribute__((naked, used));
void wdt_hard_fault_c(uintptr_t frame) __attribute__((noreturn, used));
void wdt_unhandled_c(void) __attribute__((noreturn, used));
void watchdog_record_halt(uint32_t caller);

#ifdef WDT_TEST_HOOKS
/* HC_FAULT mode 4: fault again INSIDE HardFault, after the record is written,
 * to learn whether the watchdog recovers a locked-up core on this part. */
volatile uint8_t wdt_test_fault_in_handler;
#endif

static uint8_t ipsr(void) {
    uint32_t v;
    __asm__ volatile("mrs %0, ipsr" : "=r"(v));
    return (uint8_t)(v & 0x3Fu);
}

/* Registers only until SP is ours: EXC_RETURN bit 2 says whether the frame
 * went to PSP (thread) or MSP (handler), and nothing touches a stack before
 * SP moves to wdt_fault_stack. ARMv6-M Thumb: register TST, no IT blocks.
 * .ltorg keeps the literal pool in reach of the LDR. */
void HardFault_Handler(void) {
    __asm__ volatile(
        "movs r0, #4                    \n"
        "mov  r1, lr                    \n"
        "tst  r0, r1                    \n"
        "beq  1f                        \n"
        "mrs  r0, psp                   \n"
        "b    2f                        \n"
        "1:                             \n"
        "mrs  r0, msp                   \n"
        "2:                             \n"
        "ldr  r1, =wdt_fault_stack+128  \n"
        "mov  sp, r1                    \n"
        "bl   wdt_hard_fault_c          \n"
        "3:                             \n"
        "b    3b                        \n"
        ".ltorg                         \n");
}

void wdt_hard_fault_c(uintptr_t frame) {
    watchdog_record_fault_frame(frame);
#ifdef WDT_TEST_HOOKS
    if (wdt_test_fault_in_handler) __asm__ volatile("udf #0");
#endif
    for (;;) {}
}

/* The weak target of every vector nothing claims. vectors.S reaches it with
 * BL, which has already replaced EXC_RETURN, so the frame (and the PC) cannot
 * be recovered here; the vector number is the evidence. */
void _unhandled_exception(void) {
    __asm__ volatile(
        "ldr  r1, =wdt_fault_stack+128  \n"
        "mov  sp, r1                    \n"
        "bl   wdt_unhandled_c           \n"
        "1:                             \n"
        "b    1b                        \n"
        ".ltorg                         \n");
}

void wdt_unhandled_c(void) {
    watchdog_record_stop(WDT_SITE_UNHANDLED_EXCEPTION, ipsr(), 0);
    for (;;) {}
}

/* CH_CFG_SYSTEM_HALT_HOOK (chconf.h): chSysHalt() has disabled interrupts
 * and spins afterwards without ever reaching HardFault. `caller` is
 * chSysHalt's return address -- who halted. With ChibiOS's checks and asserts
 * compiled out this is rarely reachable; it costs nothing. */
void watchdog_record_halt(uint32_t caller) {
    watchdog_record_stop(WDT_SITE_HALT, ipsr(), caller);
}
