/* SPDX-License-Identifier: MIT */

#ifndef VAMS_MAILBOX_H
#define VAMS_MAILBOX_H

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#define VAMS_MAILBOX_PING UINT32_C(0x00000001)
#define VAMS_MAILBOX_PING_RESPONSE UINT32_C(0x80000001)
#define VAMS_MAILBOX_UNSUPPORTED UINT32_C(0xffffffff)

int vams_mailbox_receive(const struct device *dev, uint32_t *message,
			 k_timeout_t timeout);
int vams_mailbox_respond(const struct device *dev, uint32_t response);

#endif /* VAMS_MAILBOX_H */
