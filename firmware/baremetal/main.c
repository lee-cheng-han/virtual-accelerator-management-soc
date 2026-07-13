/* SPDX-License-Identifier: MIT */
#include <stdint.h>

#include "vams_platform.h"

extern uint8_t __sram_start[];
extern uint8_t __sram_end[];
extern void vams_trap_entry(void);

volatile uint32_t vams_timer_fired;
static volatile uint32_t vams_sram_probe;

static uint8_t vams_mmio_read8(uint32_t address)
{
    return *(volatile uint8_t *)(uintptr_t)address;
}

static void vams_mmio_write8(uint32_t address, uint8_t value)
{
    *(volatile uint8_t *)(uintptr_t)address = value;
}

static uint32_t vams_mmio_read32(uint32_t address)
{
    return *(volatile uint32_t *)(uintptr_t)address;
}

static void vams_mmio_write32(uint32_t address, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)address = value;
}

static void vams_uart_putc(char value)
{
    while ((vams_mmio_read8(VAMS_UART_BASE + VAMS_UART_LSR) &
            VAMS_UART_LSR_THRE) == 0U) {
    }
    vams_mmio_write8(VAMS_UART_BASE + VAMS_UART_THR, (uint8_t)value);
}

static void vams_uart_puts(const char *text)
{
    while (*text != '\0') {
        if (*text == '\n') {
            vams_uart_putc('\r');
        }
        vams_uart_putc(*text);
        ++text;
    }
}

static uint64_t vams_mtime_read(void)
{
    uint32_t high_before;
    uint32_t low;
    uint32_t high_after;

    do {
        high_before = vams_mmio_read32(VAMS_ACLINT_MTIME + 4U);
        low = vams_mmio_read32(VAMS_ACLINT_MTIME);
        high_after = vams_mmio_read32(VAMS_ACLINT_MTIME + 4U);
    } while (high_before != high_after);

    return ((uint64_t)high_after << 32) | (uint64_t)low;
}

static void vams_mtimecmp_write(uint64_t value)
{
    vams_mmio_write32(VAMS_ACLINT_MTIMECMP + 4U, UINT32_MAX);
    vams_mmio_write32(VAMS_ACLINT_MTIMECMP, (uint32_t)value);
    vams_mmio_write32(VAMS_ACLINT_MTIMECMP + 4U, (uint32_t)(value >> 32));
}

static void vams_timer_interrupt_test(void)
{
    const uint32_t mtie = VAMS_MIE_MTIE;
    const uint32_t global_mie = VAMS_MSTATUS_MIE;
    const uintptr_t trap_address = (uintptr_t)&vams_trap_entry;
    const uint64_t deadline = vams_mtime_read() + UINT64_C(10000);

    vams_timer_fired = 0U;
    __asm__ volatile("csrw mtvec, %0" : : "r"(trap_address) : "memory");
    vams_mtimecmp_write(deadline);
    __asm__ volatile("csrs mie, %0" : : "r"(mtie) : "memory");
    __asm__ volatile("csrs mstatus, %0" : : "r"(global_mie) : "memory");

    while (vams_timer_fired == 0U) {
        __asm__ volatile("wfi");
    }

    __asm__ volatile("csrc mie, %0" : : "r"(mtie) : "memory");
}

static uint32_t vams_sram_test(void)
{
    const uintptr_t start = (uintptr_t)__sram_start;
    const uintptr_t end = (uintptr_t)__sram_end;

    if ((start != (uintptr_t)VAMS_SRAM_BASE) ||
        ((end - start) != (uintptr_t)VAMS_SRAM_SIZE)) {
        return 0U;
    }

    vams_sram_probe = UINT32_C(0xa5c35a3c);
    if (vams_sram_probe != UINT32_C(0xa5c35a3c)) {
        return 0U;
    }
    vams_sram_probe = 0U;
    return 1U;
}

void vams_main(void)
{
    vams_uart_puts("Virtual Accelerator Management SoC firmware booting\n");
    vams_uart_puts("CPU: RV32\n");

    if (vams_sram_test() == 0U) {
        vams_uart_puts("SRAM: error\n");
        for (;;) {
            __asm__ volatile("wfi");
        }
    }

    vams_uart_puts("SRAM: detected\n");
    vams_timer_interrupt_test();
    vams_uart_puts("UART: ready\n");

    for (;;) {
        __asm__ volatile("wfi");
    }
}
