/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/version.h>

#include <vams_mailbox.h>
#include <vams_management.h>

#define VAMS_TASK_STACK_SIZE 1024
#define VAMS_TASK_PRIORITY 5
#define VAMS_HEALTH_PRIORITY 4
#define VAMS_HEARTBEAT_PERIOD K_MSEC(250)
#define VAMS_MAILBOX_POLL_PERIOD K_MSEC(100)
#define VAMS_WATCHDOG_TIMEOUT_MS 1000U
#define VAMS_FIRMWARE_VERSION UINT32_C(0x00010000)

struct vams_heartbeat {
	uint32_t sequence;
	int64_t uptime_ms;
};

static char vams_heartbeat_buffer[sizeof(struct vams_heartbeat) * 4]
	__aligned(4);
static struct k_msgq vams_heartbeat_queue;
K_THREAD_STACK_DEFINE(vams_producer_stack, VAMS_TASK_STACK_SIZE);
K_THREAD_STACK_DEFINE(vams_monitor_stack, VAMS_TASK_STACK_SIZE);
K_THREAD_STACK_DEFINE(vams_mailbox_stack, VAMS_TASK_STACK_SIZE);
K_THREAD_STACK_DEFINE(vams_health_stack, VAMS_TASK_STACK_SIZE);

static const struct device *const mailbox = DEVICE_DT_GET(DT_NODELABEL(mailbox0));
static const struct device *const management =
	DEVICE_DT_GET(DT_NODELABEL(management0));
static struct k_thread vams_producer_thread;
static struct k_thread vams_monitor_thread;
static struct k_thread vams_mailbox_thread;
static struct k_thread vams_health_thread;
static atomic_t producer_epoch;
static atomic_t monitor_epoch;
static atomic_t mailbox_epoch;

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
		atomic_inc(&producer_epoch);
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
		atomic_inc(&monitor_epoch);
		printk("Heartbeat: sequence=%" PRIu32 " uptime_ms=%" PRId64 "\n",
		       heartbeat.sequence, heartbeat.uptime_ms);
	}
}

static void vams_mailbox_service(void *unused1, void *unused2, void *unused3)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	for (;;) {
		uint32_t message;
		int status;

		status = vams_mailbox_receive(mailbox, &message,
					      VAMS_MAILBOX_POLL_PERIOD);
		atomic_inc(&mailbox_epoch);
		if (status == -EAGAIN) {
			continue;
		}
		__ASSERT_NO_MSG(status == 0);

		const uint32_t response = message == VAMS_MAILBOX_PING ?
			VAMS_MAILBOX_PING_RESPONSE : VAMS_MAILBOX_UNSUPPORTED;

		status = vams_mailbox_respond(mailbox, response);
		__ASSERT_NO_MSG(status == 0);
		printk("Mailbox: request=0x%08" PRIx32
		       " response=0x%08" PRIx32 "\n", message, response);
	}
}

static void vams_health_monitor(void *arg1, void *unused2, void *unused3)
{
	const struct vams_management_snapshot *boot_snapshot = arg1;
	atomic_val_t last_producer = atomic_get(&producer_epoch);
	atomic_val_t last_monitor = atomic_get(&monitor_epoch);
	atomic_val_t last_mailbox = atomic_get(&mailbox_epoch);
	uint32_t heartbeat = 0U;
	bool expiry_announced = false;

	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	for (;;) {
		atomic_val_t current_producer;
		atomic_val_t current_monitor;
		atomic_val_t current_mailbox;
		uint64_t uptime_ms;
		bool healthy;

		k_sleep(VAMS_HEARTBEAT_PERIOD);
		uptime_ms = k_uptime_get();
		current_producer = atomic_get(&producer_epoch);
		current_monitor = atomic_get(&monitor_epoch);
		current_mailbox = atomic_get(&mailbox_epoch);
		healthy = (current_producer != last_producer) &&
			  (current_monitor != last_monitor) &&
			  (current_mailbox != last_mailbox);

		if (healthy) {
			heartbeat++;
			vams_management_publish(management, heartbeat, uptime_ms,
						VAMS_FIRMWARE_VERSION);
			printk("Telemetry: heartbeat=%" PRIu32
			       " uptime_ms=%" PRIu64 " healthy=1\n",
			       heartbeat, uptime_ms);
		}

		if (IS_ENABLED(CONFIG_VAMS_WATCHDOG_EXPIRY_TEST) &&
		    (boot_snapshot->watchdog_reset_count == 0U)) {
			if (!expiry_announced) {
				printk("Watchdog test: withholding pet\n");
				expiry_announced = true;
			}
		} else if (healthy) {
			const int status = vams_management_watchdog_pet(management);

			__ASSERT_NO_MSG(status == 0);
		}

		last_producer = current_producer;
		last_monitor = current_monitor;
		last_mailbox = current_mailbox;
	}
}

int main(void)
{
	static struct vams_management_snapshot boot_snapshot;
	int status;

	printk("Virtual Accelerator Management SoC Zephyr booting\n");
	printk("Kernel: Zephyr %s\n", KERNEL_VERSION_STRING);
	k_msgq_init(&vams_heartbeat_queue, vams_heartbeat_buffer,
		     sizeof(struct vams_heartbeat), 4);
	__ASSERT_NO_MSG(device_is_ready(mailbox));
	__ASSERT_NO_MSG(device_is_ready(management));

	vams_management_snapshot(management, &boot_snapshot);
	printk("Reset: reason=%" PRIu32 " watchdog_count=%" PRIu32
	       " generation=%" PRIu32 "\n", boot_snapshot.reset_reason,
	       boot_snapshot.watchdog_reset_count,
	       boot_snapshot.reset_generation);
	if (boot_snapshot.reset_reason == VAMS_RESET_REASON_WATCHDOG) {
		printk("Recovery: watchdog reset observed\n");
	}

	status = vams_management_watchdog_start(management,
						VAMS_WATCHDOG_TIMEOUT_MS);
	__ASSERT_NO_MSG(status == 0);

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

	(void)k_thread_create(&vams_mailbox_thread, vams_mailbox_stack,
			      K_THREAD_STACK_SIZEOF(vams_mailbox_stack),
			      vams_mailbox_service, NULL, NULL, NULL,
			      VAMS_TASK_PRIORITY, 0, K_FOREVER);
	status = k_thread_name_set(&vams_mailbox_thread, "vams_mailbox");
	__ASSERT_NO_MSG(status == 0);

	(void)k_thread_create(&vams_health_thread, vams_health_stack,
			      K_THREAD_STACK_SIZEOF(vams_health_stack),
			      vams_health_monitor, &boot_snapshot, NULL, NULL,
			      VAMS_HEALTH_PRIORITY, 0, K_FOREVER);
	status = k_thread_name_set(&vams_health_thread, "vams_health");
	__ASSERT_NO_MSG(status == 0);

	printk("Tasks: producer -> message queue -> monitor\n");
	printk("Services: mailbox, watchdog, reset telemetry\n");
	k_thread_start(&vams_monitor_thread);
	k_thread_start(&vams_producer_thread);
	k_thread_start(&vams_mailbox_thread);
	k_thread_start(&vams_health_thread);

	return 0;
}
