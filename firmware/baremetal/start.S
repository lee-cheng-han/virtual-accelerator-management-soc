/* SPDX-License-Identifier: MIT */

#include "vams_platform.h"

    .section .text.start, "ax", @progbits
    .globl _start
    .type _start, @function
    .align 2
_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    la sp, __stack_top

    la t0, __bss_start
    la t1, __bss_end
.Lbss_loop:
    bgeu t0, t1, .Lbss_done
    sw zero, 0(t0)
    addi t0, t0, 4
    j .Lbss_loop
.Lbss_done:
    call vams_main

.Lhalt:
    wfi
    j .Lhalt
    .size _start, . - _start

    .globl vams_trap_entry
    .type vams_trap_entry, @function
    .align 2
vams_trap_entry:
    addi sp, sp, -8
    sw t0, 0(sp)
    sw t1, 4(sp)

    csrr t0, mcause
    li t1, VAMS_MCAUSE_TIMER
    bne t0, t1, .Ltrap_fault

    la t0, vams_timer_fired
    li t1, 1
    sw t1, 0(t0)

    li t0, VAMS_ACLINT_MTIMECMP
    li t1, -1
    sw t1, 0(t0)
    sw t1, 4(t0)

    lw t0, 0(sp)
    lw t1, 4(sp)
    addi sp, sp, 8
    mret

.Ltrap_fault:
    wfi
    j .Ltrap_fault
    .size vams_trap_entry, . - vams_trap_entry
