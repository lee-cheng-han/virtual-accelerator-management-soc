/* SPDX-License-Identifier: MIT */

#ifndef VAMS_MANAGEMENT_H
#define VAMS_MANAGEMENT_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#define VAMS_RESET_REASON_POWER_ON UINT32_C(0)
#define VAMS_RESET_REASON_FIRMWARE UINT32_C(4)
#define VAMS_RESET_REASON_WATCHDOG UINT32_C(5)

struct vams_management_snapshot {
	uint32_t reset_reason;
	uint32_t watchdog_reset_count;
	uint32_t reset_generation;
	uint32_t mailbox_rx_count;
	uint32_t mailbox_tx_count;
	uint32_t status;
	bool watchdog_enabled;
};

int vams_management_watchdog_start(const struct device *dev,
				   uint32_t timeout_ms);
int vams_management_watchdog_pet(const struct device *dev);
void vams_management_publish(const struct device *dev, uint32_t heartbeat,
			     uint64_t uptime_ms, uint32_t firmware_version);
void vams_management_snapshot(const struct device *dev,
			      struct vams_management_snapshot *snapshot);
void vams_management_reset(const struct device *dev);

#endif /* VAMS_MANAGEMENT_H */
