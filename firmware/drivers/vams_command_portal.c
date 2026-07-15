/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT vams_command_portal

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/sys_io.h>

#include <vams_command_portal.h>

#define VAMS_COMMAND_STATUS 0x00U
#define VAMS_COMMAND_FW_ACK 0x08U
#define VAMS_COMMAND_FW_COMPLETE 0x0cU
#define VAMS_COMMAND_SUBMISSION 0x100U
#define VAMS_COMMAND_COMPLETION 0x200U

#define VAMS_COMMAND_H2F_PENDING BIT(0)
#define VAMS_COMMAND_F2H_PENDING BIT(1)
#define VAMS_COMMAND_WORDS (VAMS_SUBMISSION_SIZE / sizeof(uint32_t))
#define VAMS_COMPLETION_WORDS (VAMS_COMPLETION_SIZE / sizeof(uint32_t))

struct vams_command_portal_config {
	mm_reg_t base;
};

int vams_command_receive(const struct device *dev,
			 struct vams_submission *submission,
			 k_timeout_t timeout)
{
	const struct vams_command_portal_config *config = dev->config;
	k_timepoint_t deadline = sys_timepoint_calc(timeout);
	uint32_t words[VAMS_COMMAND_WORDS];

	if (submission == NULL) {
		return -EINVAL;
	}

	while ((sys_read32(config->base + VAMS_COMMAND_STATUS) &
		VAMS_COMMAND_H2F_PENDING) == 0U) {
		if (sys_timepoint_expired(deadline)) {
			return -EAGAIN;
		}
		k_sleep(K_MSEC(1));
	}

	for (size_t index = 0U; index < ARRAY_SIZE(words); index++) {
		words[index] = sys_read32(config->base + VAMS_COMMAND_SUBMISSION +
					  index * sizeof(uint32_t));
	}
	barrier_dmem_fence_full();
	memcpy(submission, words, sizeof(*submission));
	sys_write32(1U, config->base + VAMS_COMMAND_FW_ACK);
	return 0;
}

int vams_command_complete(const struct device *dev,
			  const struct vams_completion *completion)
{
	const struct vams_command_portal_config *config = dev->config;
	uint32_t words[VAMS_COMPLETION_WORDS];

	if (completion == NULL) {
		return -EINVAL;
	}
	if (sys_read32(config->base + VAMS_COMMAND_STATUS) &
	    VAMS_COMMAND_F2H_PENDING) {
		return -EBUSY;
	}

	memcpy(words, completion, sizeof(*completion));
	for (size_t index = 0U; index < ARRAY_SIZE(words); index++) {
		sys_write32(words[index], config->base + VAMS_COMMAND_COMPLETION +
					 index * sizeof(uint32_t));
	}
	barrier_dmem_fence_full();
	sys_write32(1U, config->base + VAMS_COMMAND_FW_COMPLETE);
	return 0;
}

static int vams_command_portal_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	BUILD_ASSERT(sizeof(struct vams_submission) == VAMS_SUBMISSION_SIZE);
	BUILD_ASSERT(sizeof(struct vams_completion) == VAMS_COMPLETION_SIZE);
	return 0;
}

static const struct vams_command_portal_config vams_command_portal_config_0 = {
	.base = DT_INST_REG_ADDR(0),
};

DEVICE_DT_INST_DEFINE(0, vams_command_portal_init, NULL, NULL,
		      &vams_command_portal_config_0, POST_KERNEL,
		      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);
