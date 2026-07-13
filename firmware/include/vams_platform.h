/* SPDX-License-Identifier: MIT */
#ifndef VAMS_PLATFORM_H
#define VAMS_PLATFORM_H

#ifndef __ASSEMBLER__
#include <stdint.h>
#define VAMS_U32(value) UINT32_C(value)
#define VAMS_U8(value)  UINT8_C(value)
#else
#define VAMS_U32(value) value
#define VAMS_U8(value)  value
#endif

#define VAMS_ROM_BASE             VAMS_U32(0x00001000)
#define VAMS_ROM_SIZE             VAMS_U32(0x00010000)
#define VAMS_ACLINT_SWI_BASE      VAMS_U32(0x02000000)
#define VAMS_ACLINT_MTIMER_BASE   VAMS_U32(0x02004000)
#define VAMS_ACLINT_MTIMECMP      (VAMS_ACLINT_MTIMER_BASE + VAMS_U32(0x0000))
#define VAMS_ACLINT_MTIME         (VAMS_ACLINT_MTIMER_BASE + VAMS_U32(0x7ff8))
#define VAMS_UART_BASE            VAMS_U32(0x10000000)
#define VAMS_SRAM_BASE            VAMS_U32(0x80000000)
#define VAMS_SRAM_SIZE            VAMS_U32(0x00080000)

#define VAMS_UART_THR             VAMS_U32(0x0)
#define VAMS_UART_LSR             VAMS_U32(0x5)
#define VAMS_UART_LSR_THRE        VAMS_U8(0x20)

#define VAMS_MCAUSE_TIMER         VAMS_U32(0x80000007)
#define VAMS_MIE_MTIE             (VAMS_U32(1) << 7)
#define VAMS_MSTATUS_MIE          (VAMS_U32(1) << 3)

#endif
