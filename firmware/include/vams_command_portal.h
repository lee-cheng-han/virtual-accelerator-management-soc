/* SPDX-License-Identifier: MIT */
#ifndef VAMS_COMMAND_PORTAL_H
#define VAMS_COMMAND_PORTAL_H

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <vams_abi.h>

bool vams_command_pending(const struct device *dev);
int vams_command_receive(const struct device *dev,
			 struct vams_submission *submission,
			 k_timeout_t timeout);
int vams_command_complete(const struct device *dev,
			  const struct vams_completion *completion);
int vams_command_abort(const struct device *dev,
		       const struct vams_completion *request);
int vams_command_result_receive(const struct device *dev,
				struct vams_completion *result,
				k_timeout_t timeout);

#endif /* VAMS_COMMAND_PORTAL_H */
