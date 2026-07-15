/* SPDX-License-Identifier: MIT */
#ifndef VAMS_COMMAND_PORTAL_H
#define VAMS_COMMAND_PORTAL_H

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <vams_abi.h>

int vams_command_receive(const struct device *dev,
			 struct vams_submission *submission,
			 k_timeout_t timeout);
int vams_command_complete(const struct device *dev,
			  const struct vams_completion *completion);

#endif /* VAMS_COMMAND_PORTAL_H */
