/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VAMS_REGS_H
#define VAMS_REGS_H

#include <linux/bits.h>

#define VAMS_BAR0_SIZE                 0x1000U

#define VAMS_REG_DEVICE_ID             0x000U
#define VAMS_REG_HW_IF_VERSION         0x004U
#define VAMS_REG_FW_VERSION            0x008U
#define VAMS_REG_DESC_VERSION          0x00cU
#define VAMS_REG_CAPABILITIES          0x010U
#define VAMS_REG_MAX_TRANSFER          0x014U
#define VAMS_REG_QUEUE_LIMITS          0x018U
#define VAMS_REG_DEVICE_STATUS         0x01cU
#define VAMS_REG_ERROR_STATUS          0x024U
#define VAMS_REG_RESET_GENERATION      0x028U
#define VAMS_REG_LAST_FATAL            0x02cU

#define VAMS_REG_INTR_STATUS           0x300U
#define VAMS_REG_INTR_MASK             0x304U
#define VAMS_REG_INTR_FORCE            0x308U

#define VAMS_DEVICE_ID_VALUE           0x11001b36U
#define VAMS_HW_IF_MAJOR_SUPPORTED     1U
#define VAMS_DESC_VERSION_SUPPORTED    1U

#define VAMS_CAP_DMA                   BIT(0)
#define VAMS_CAP_MSIX                  BIT(1)
#define VAMS_CAP_WATCHDOG              BIT(2)
#define VAMS_CAP_TELEMETRY             BIT(3)
#define VAMS_CAP_ENGINE_RESET          BIT(4)
#define VAMS_CAP_POLLING_CQ            BIT(5)
#define VAMS_CAP_DEBUG_FAULT           BIT(6)
#define VAMS_CAP_KNOWN                 GENMASK(6, 0)

#define VAMS_STATUS_READY              BIT(0)
#define VAMS_STATUS_FW_RUNNING         BIT(1)
#define VAMS_STATUS_RESETTING          BIT(4)
#define VAMS_STATUS_FATAL              BIT(5)

#define VAMS_INTR_CQ                   BIT(0)
#define VAMS_INTR_ERROR                BIT(1)
#define VAMS_INTR_FW_EVENT             BIT(2)
#define VAMS_INTR_RESET_DONE           BIT(3)
#define VAMS_INTR_ASYNC                (VAMS_INTR_ERROR | VAMS_INTR_FW_EVENT | \
					 VAMS_INTR_RESET_DONE)
#define VAMS_INTR_ALL                  (VAMS_INTR_CQ | VAMS_INTR_ASYNC)

#define VAMS_MSIX_VECTORS              2
#define VAMS_MSIX_CQ_VECTOR            0
#define VAMS_MSIX_ASYNC_VECTOR         1

#endif /* VAMS_REGS_H */
