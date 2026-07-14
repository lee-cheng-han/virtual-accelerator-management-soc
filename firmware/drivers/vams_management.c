/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT vams_management

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#include <vams_management.h>

#define VAMS_CONTROL_WDT_TIMEOUT_MS 0x00U
#define VAMS_CONTROL_WDT_CONTROL 0x04U
#define VAMS_CONTROL_WDT_PET 0x08U
#define VAMS_CONTROL_RESET_REQUEST 0x0cU
#define VAMS_CONTROL_LAST_RESET_REASON 0x10U
#define VAMS_CONTROL_WDT_RESET_COUNT 0x14U
#define VAMS_CONTROL_FW_HEARTBEAT 0x18U
#define VAMS_CONTROL_FW_UPTIME_LO 0x1cU
#define VAMS_CONTROL_FW_UPTIME_HI 0x20U
#define VAMS_CONTROL_FW_VERSION 0x24U
#define VAMS_CONTROL_MAILBOX_RX_COUNT 0x28U
#define VAMS_CONTROL_MAILBOX_TX_COUNT 0x2cU
#define VAMS_CONTROL_RESET_GENERATION 0x30U
#define VAMS_CONTROL_STATUS 0x34U

#define VAMS_WDT_ENABLE BIT(0)
#define VAMS_WDT_PET_MAGIC UINT32_C(0x56414d53)
#define VAMS_RESET_REQUEST_MANAGEMENT BIT(0)

struct vams_management_config {
	uintptr_t base;
};

int vams_management_watchdog_start(const struct device *dev,
				   uint32_t timeout_ms)
{
	const struct vams_management_config *config;
	uint32_t control;

	if ((dev == NULL) || !device_is_ready(dev)) {
		return -EINVAL;
	}

	config = dev->config;
	control = sys_read32(config->base + VAMS_CONTROL_WDT_CONTROL);
	if ((control & VAMS_WDT_ENABLE) != 0U) {
		return sys_read32(config->base + VAMS_CONTROL_WDT_TIMEOUT_MS) ==
		       timeout_ms ? 0 : -EBUSY;
	}

	sys_write32(timeout_ms, config->base + VAMS_CONTROL_WDT_TIMEOUT_MS);
	if (sys_read32(config->base + VAMS_CONTROL_WDT_TIMEOUT_MS) != timeout_ms) {
		return -EINVAL;
	}
	sys_write32(VAMS_WDT_ENABLE,
		    config->base + VAMS_CONTROL_WDT_CONTROL);
	return 0;
}

int vams_management_watchdog_pet(const struct device *dev)
{
	const struct vams_management_config *config;

	if ((dev == NULL) || !device_is_ready(dev)) {
		return -EINVAL;
	}

	config = dev->config;
	if ((sys_read32(config->base + VAMS_CONTROL_WDT_CONTROL) &
	     VAMS_WDT_ENABLE) == 0U) {
		return -EACCES;
	}

	sys_write32(VAMS_WDT_PET_MAGIC, config->base + VAMS_CONTROL_WDT_PET);
	return 0;
}

void vams_management_publish(const struct device *dev, uint32_t heartbeat,
			     uint64_t uptime_ms, uint32_t firmware_version)
{
	const struct vams_management_config *config = dev->config;

	sys_write32(heartbeat, config->base + VAMS_CONTROL_FW_HEARTBEAT);
	sys_write32((uint32_t)uptime_ms,
		    config->base + VAMS_CONTROL_FW_UPTIME_LO);
	sys_write32((uint32_t)(uptime_ms >> 32),
		    config->base + VAMS_CONTROL_FW_UPTIME_HI);
	sys_write32(firmware_version, config->base + VAMS_CONTROL_FW_VERSION);
}

void vams_management_snapshot(const struct device *dev,
			      struct vams_management_snapshot *snapshot)
{
	const struct vams_management_config *config = dev->config;

	snapshot->reset_reason =
		sys_read32(config->base + VAMS_CONTROL_LAST_RESET_REASON);
	snapshot->watchdog_reset_count =
		sys_read32(config->base + VAMS_CONTROL_WDT_RESET_COUNT);
	snapshot->reset_generation =
		sys_read32(config->base + VAMS_CONTROL_RESET_GENERATION);
	snapshot->mailbox_rx_count =
		sys_read32(config->base + VAMS_CONTROL_MAILBOX_RX_COUNT);
	snapshot->mailbox_tx_count =
		sys_read32(config->base + VAMS_CONTROL_MAILBOX_TX_COUNT);
	snapshot->status = sys_read32(config->base + VAMS_CONTROL_STATUS);
	snapshot->watchdog_enabled =
		(sys_read32(config->base + VAMS_CONTROL_WDT_CONTROL) &
		 VAMS_WDT_ENABLE) != 0U;
}

void vams_management_reset(const struct device *dev)
{
	const struct vams_management_config *config = dev->config;

	sys_write32(VAMS_RESET_REQUEST_MANAGEMENT,
		    config->base + VAMS_CONTROL_RESET_REQUEST);
}

static int vams_management_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static const struct vams_management_config vams_management_config_0 = {
	.base = DT_INST_REG_ADDR(0),
};

DEVICE_DT_INST_DEFINE(0, vams_management_init, NULL, NULL,
		      &vams_management_config_0, POST_KERNEL,
		      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);
