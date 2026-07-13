/* SPDX-License-Identifier: MIT */

#include <inttypes.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/version.h>

#define VAMS_TASK_STACK_SIZE 1024
#define VAMS_TASK_PRIORITY   5
#define VAMS_HEARTBEAT_PERIOD K_MSEC(250)

struct vams_heartbeat {
	uint32_t sequence;
	int64_t uptime_ms;
};

K_MSGQ_DEFINE(vams_heartbeat_queue, sizeof(struct vams_heartbeat), 4, 4);
K_THREAD_STACK_DEFINE(vams_producer_stack, VAMS_TASK_STACK_SIZE);
K_THREAD_STACK_DEFINE(vams_monitor_stack, VAMS_TASK_STACK_SIZE);

static struct k_thread vams_producer_thread;
static struct k_thread vams_monitor_thread;

static void vams_producer(void *unused1, void *unused2, void *unused3)
{
	uint32_t sequence = 1U;

	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	for (;;) {
		const struct vams_heartbeat heartbeat = {
			.sequence = sequence,
			.uptime_ms = k_uptime_get(),
		};
		int status;

		status = k_msgq_put(&vams_heartbeat_queue, &heartbeat, K_FOREVER);
		__ASSERT_NO_MSG(status == 0);
		sequence++;
		k_sleep(VAMS_HEARTBEAT_PERIOD);
	}
}

static void vams_monitor(void *unused1, void *unused2, void *unused3)
{
	struct vams_heartbeat heartbeat;

	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	for (;;) {
		const int status =
			k_msgq_get(&vams_heartbeat_queue, &heartbeat, K_FOREVER);

		__ASSERT_NO_MSG(status == 0);
		printk("Heartbeat: sequence=%" PRIu32 " uptime_ms=%" PRId64 "\n",
		       heartbeat.sequence, heartbeat.uptime_ms);
	}
}

int main(void)
{
	int status;

	printk("Virtual Accelerator Management SoC Zephyr booting\n");
	printk("Kernel: Zephyr %s\n", KERNEL_VERSION_STRING);

	(void)k_thread_create(&vams_monitor_thread, vams_monitor_stack,
			      K_THREAD_STACK_SIZEOF(vams_monitor_stack),
			      vams_monitor, NULL, NULL, NULL,
			      VAMS_TASK_PRIORITY, 0, K_FOREVER);
	status = k_thread_name_set(&vams_monitor_thread, "vams_monitor");
	__ASSERT_NO_MSG(status == 0);

	(void)k_thread_create(&vams_producer_thread, vams_producer_stack,
			      K_THREAD_STACK_SIZEOF(vams_producer_stack),
			      vams_producer, NULL, NULL, NULL,
			      VAMS_TASK_PRIORITY, 0, K_FOREVER);
	status = k_thread_name_set(&vams_producer_thread, "vams_producer");
	__ASSERT_NO_MSG(status == 0);

	printk("Tasks: producer -> message queue -> monitor\n");
	k_thread_start(&vams_monitor_thread);
	k_thread_start(&vams_producer_thread);

	return 0;
}
