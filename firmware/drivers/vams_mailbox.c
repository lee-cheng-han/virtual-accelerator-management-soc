/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT vams_mailbox

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#include <vams_mailbox.h>

#define VAMS_MAILBOX_MESSAGE 0x00U
#define VAMS_MAILBOX_ACK 0x04U
#define VAMS_MAILBOX_RESPONSE 0x08U
#define VAMS_MAILBOX_RESPONSE_DOORBELL 0x0cU
#define VAMS_MAILBOX_STATUS 0x10U

#define VAMS_MAILBOX_H2F_PENDING BIT(0)
#define VAMS_MAILBOX_F2H_PENDING BIT(1)

struct vams_mailbox_config {
	uintptr_t base;
};

static char vams_mailbox_rx_buffer[sizeof(uint32_t) * 4] __aligned(4);
static struct k_msgq vams_mailbox_rx_queue;

static void vams_mailbox_isr(const void *arg)
{
	const struct device *dev = arg;
	const struct vams_mailbox_config *config = dev->config;
	const uint32_t status = sys_read32(config->base + VAMS_MAILBOX_STATUS);
	uint32_t message;

	if ((status & VAMS_MAILBOX_H2F_PENDING) == 0U) {
		return;
	}

	message = sys_read32(config->base + VAMS_MAILBOX_MESSAGE);
	sys_write32(1U, config->base + VAMS_MAILBOX_ACK);
	(void)k_msgq_put(&vams_mailbox_rx_queue, &message, K_NO_WAIT);
}

int vams_mailbox_receive(const struct device *dev, uint32_t *message,
			 k_timeout_t timeout)
{
	if ((dev == NULL) || (message == NULL) || !device_is_ready(dev)) {
		return -EINVAL;
	}

	return k_msgq_get(&vams_mailbox_rx_queue, message, timeout);
}

int vams_mailbox_respond(const struct device *dev, uint32_t response)
{
	const struct vams_mailbox_config *config;

	if ((dev == NULL) || !device_is_ready(dev)) {
		return -EINVAL;
	}

	config = dev->config;
	if ((sys_read32(config->base + VAMS_MAILBOX_STATUS) &
	     VAMS_MAILBOX_F2H_PENDING) != 0U) {
		return -EBUSY;
	}

	sys_write32(response, config->base + VAMS_MAILBOX_RESPONSE);
	sys_write32(1U, config->base + VAMS_MAILBOX_RESPONSE_DOORBELL);
	return 0;
}

static int vams_mailbox_init(const struct device *dev)
{
	k_msgq_init(&vams_mailbox_rx_queue, vams_mailbox_rx_buffer,
		     sizeof(uint32_t), 4);

	IRQ_CONNECT(DT_INST_IRQN(0), 0, vams_mailbox_isr,
		    DEVICE_DT_INST_GET(0), 0);
	irq_enable(DT_INST_IRQN(0));
	/* Also consume a request that was posted before the CPU IRQ was wired. */
	vams_mailbox_isr(dev);
	return 0;
}

static const struct vams_mailbox_config vams_mailbox_config_0 = {
	.base = DT_INST_REG_ADDR(0),
};

DEVICE_DT_INST_DEFINE(0, vams_mailbox_init, NULL, NULL,
		      &vams_mailbox_config_0, POST_KERNEL,
		      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);
