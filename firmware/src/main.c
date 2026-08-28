/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/version.h>

#include <vams_command.h>
#include <vams_command_portal.h>
#include <vams_event.h>
#include <vams_health.h>
#include <vams_mailbox.h>
#include <vams_management.h>
#include <vams_overload.h>
#include <vams_retained.h>
#include <vams_scheduler.h>

#define VAMS_TASK_STACK_SIZE 1024
#define VAMS_TASK_PRIORITY 5
#define VAMS_HEALTH_PRIORITY 4
#define VAMS_RECEIVER_PRIORITY 1
#define VAMS_VALIDATOR_PRIORITY 2
#define VAMS_COMPLETION_PRIORITY 2
#define VAMS_RECOVERY_PRIORITY 0
#define VAMS_SCHEDULER_PRIORITY 3
#define VAMS_COMMAND_STACK_SIZE 1536
#define VAMS_VALIDATOR_STACK_SIZE 2048
#define VAMS_HEARTBEAT_PERIOD K_MSEC(250)
#define VAMS_MAILBOX_POLL_PERIOD K_MSEC(100)
#define VAMS_WATCHDOG_TIMEOUT_MS 1000U
#define VAMS_ABORT_ACK_TIMEOUT_MS 100U
#define VAMS_COMPLETION_PUBLISH_TIMEOUT_MS 100U
#define VAMS_SERVICE_POLL_MS 100U
#define VAMS_HEALTH_STARTUP_GRACE 2U
#define VAMS_HEALTH_MISS_LIMIT 2U
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
K_THREAD_STACK_DEFINE(vams_receiver_stack, VAMS_COMMAND_STACK_SIZE);
K_THREAD_STACK_DEFINE(vams_validator_stack, VAMS_VALIDATOR_STACK_SIZE);
K_THREAD_STACK_DEFINE(vams_scheduler_stack, VAMS_COMMAND_STACK_SIZE);
K_THREAD_STACK_DEFINE(vams_recovery_stack, VAMS_COMMAND_STACK_SIZE);
K_THREAD_STACK_DEFINE(vams_completion_stack, VAMS_COMMAND_STACK_SIZE);
K_THREAD_STACK_DEFINE(vams_health_stack, VAMS_TASK_STACK_SIZE);
K_THREAD_STACK_DEFINE(vams_event_stack, VAMS_TASK_STACK_SIZE);
K_MEM_SLAB_DEFINE(vams_command_pool, sizeof(struct vams_command_object),
		  VAMS_COMMAND_POOL_SIZE, 4);
K_MSGQ_DEFINE(vams_validation_queue, sizeof(struct vams_command_object *),
	      VAMS_COMMAND_POOL_SIZE, 4);
K_MSGQ_DEFINE(vams_ready_queue, sizeof(struct vams_command_object *),
	      VAMS_COMMAND_POOL_SIZE, 4);
K_MSGQ_DEFINE(vams_running_queue, sizeof(struct vams_command_object *), 1, 4);
K_MSGQ_DEFINE(vams_completion_queue, sizeof(struct vams_command_object *),
	      VAMS_COMMAND_POOL_SIZE, 4);

static const struct device *const command_portal =
	DEVICE_DT_GET(DT_NODELABEL(command0));
static const struct device *const mailbox = DEVICE_DT_GET(DT_NODELABEL(mailbox0));
static const struct device *const management =
	DEVICE_DT_GET(DT_NODELABEL(management0));
static struct k_thread vams_producer_thread;
static struct k_thread vams_monitor_thread;
static struct k_thread vams_mailbox_thread;
static struct k_thread vams_receiver_thread;
static struct k_thread vams_validator_thread;
static struct k_thread vams_scheduler_thread;
static struct k_thread vams_recovery_thread;
static struct k_thread vams_completion_thread;
static struct k_thread vams_health_thread;
static struct k_thread vams_event_thread;
static atomic_t producer_epoch;
static atomic_t monitor_epoch;
static atomic_t mailbox_epoch;
static atomic_t receiver_epoch;
static atomic_t validator_epoch;
static atomic_t scheduler_epoch;
static atomic_t recovery_epoch;
static atomic_t completion_epoch;
static atomic_t command_objects_in_use;
static atomic_t command_pool_high_water;
static atomic_t validation_queue_high_water;
static atomic_t ready_queue_high_water;
static atomic_t running_queue_high_water;
static atomic_t completion_queue_high_water;
static atomic_t recovery_attempt_count;
static atomic_t recovery_escalation_count;
static atomic_t admission_defer_count;
static atomic_t heartbeat_drop_count;
static atomic_t queue_overload_count;
static atomic_t portal_stall_count;
static atomic_t overload_reset_requested;
static atomic_t freeze_triggered;
static atomic_t health_failure_count;
static atomic_t health_stuck_task;
static uint32_t command_generation;
static uint32_t command_sequence;
static uint32_t test_freeze_task;
static uint32_t boot_reset_generation;

extern char _image_ram_start[];
extern char _image_ram_end[];

static void vams_record_high_water(atomic_t *high_water, atomic_val_t value)
{
	atomic_val_t previous = atomic_get(high_water);

	while (value > previous && !atomic_cas(high_water, previous, value)) {
		previous = atomic_get(high_water);
	}
}

static void vams_saturating_atomic_inc(atomic_t *counter)
{
	atomic_val_t previous = atomic_get(counter);

	while ((uint32_t)previous != UINT32_MAX &&
	       !atomic_cas(counter, previous,
			   (atomic_val_t)vams_counter_increment(
				   (uint32_t)previous))) {
		previous = atomic_get(counter);
	}
}

static void vams_task_progress(atomic_t *epoch,
			       enum vams_health_task_id task_id)
{
	atomic_inc(epoch);
	if (IS_ENABLED(CONFIG_VAMS_HEALTH_TEST) &&
	    boot_reset_generation == 0U && test_freeze_task == task_id &&
	    atomic_cas(&freeze_triggered, 0, 1)) {
		k_sleep(K_FOREVER);
	}
}

static void vams_health_epochs(
	uint32_t epochs[VAMS_HEALTH_TASK_COUNT])
{
	epochs[VAMS_HEALTH_TASK_PRODUCER - 1U] =
		(uint32_t)atomic_get(&producer_epoch);
	epochs[VAMS_HEALTH_TASK_MONITOR - 1U] =
		(uint32_t)atomic_get(&monitor_epoch);
	epochs[VAMS_HEALTH_TASK_MAILBOX - 1U] =
		(uint32_t)atomic_get(&mailbox_epoch);
	epochs[VAMS_HEALTH_TASK_RECEIVER - 1U] =
		(uint32_t)atomic_get(&receiver_epoch);
	epochs[VAMS_HEALTH_TASK_VALIDATOR - 1U] =
		(uint32_t)atomic_get(&validator_epoch);
	epochs[VAMS_HEALTH_TASK_SCHEDULER - 1U] =
		(uint32_t)atomic_get(&scheduler_epoch);
	epochs[VAMS_HEALTH_TASK_RECOVERY - 1U] =
		(uint32_t)atomic_get(&recovery_epoch);
	epochs[VAMS_HEALTH_TASK_COMPLETION - 1U] =
		(uint32_t)atomic_get(&completion_epoch);
}

static int vams_queue_put(struct k_msgq *queue, const void *data,
			  atomic_t *high_water)
{
	const uint32_t used_before = k_msgq_num_used_get(queue);
	const uint32_t capacity = used_before + k_msgq_num_free_get(queue);
	const int status = k_msgq_put(queue, data, K_NO_WAIT);

	if (status == 0) {
		vams_record_high_water(high_water,
				       (atomic_val_t)MIN(used_before + 1U, capacity));
	}
	else {
		vams_saturating_atomic_inc(&queue_overload_count);
	}
	return status;
}

static void vams_event_logger(void *unused1, void *unused2, void *unused3)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	for (;;) {
		struct vams_event event;
		const int status = vams_event_receive(&event, K_FOREVER);

		__ASSERT_NO_MSG(status == 0);
		switch ((enum vams_event_id)event.id) {
		case VAMS_EVENT_TRANSITION:
			printk("Scheduler: event=transition sequence=%" PRIu32
			       " command=0x%08" PRIx32 " generation=%" PRIu32
			       " from=%" PRIu32 " to=%" PRIu32 "\n",
			       event.arg0, event.command_id, event.generation,
			       event.arg1, event.arg2);
			break;
		case VAMS_EVENT_PUBLISHED:
			printk("Scheduler: event=published sequence=%" PRIu32
			       " command=0x%08" PRIx32 " generation=%" PRIu32
			       " count=%" PRIu32 "\n",
			       event.arg0, event.command_id, event.generation,
			       event.arg1);
			break;
		case VAMS_EVENT_HEARTBEAT:
			printk("Heartbeat: sequence=%" PRIu32
			       " uptime_ms=%" PRIu64 "\n",
			       event.arg0, event.value);
			break;
		case VAMS_EVENT_MAILBOX:
			printk("Mailbox: request=0x%08" PRIx32
			       " response=0x%08" PRIx32 "\n",
			       event.arg0, event.arg1);
			break;
		case VAMS_EVENT_ABORT_REQUEST:
			printk("Recovery: event=abort-request command=0x%08" PRIx32
			       " attempt=%" PRIu32 "\n",
			       event.command_id, event.arg0);
			break;
		case VAMS_EVENT_ABORT_ESCALATED:
			printk("Recovery: event=abort-escalated command=0x%08" PRIx32
			       " count=%" PRIu32 "\n",
			       event.command_id, event.arg0);
			break;
		case VAMS_EVENT_RESULT_MISMATCH:
			printk("Recovery: rejected mismatched engine result"
			       " command=0x%08" PRIx32 "\n", event.command_id);
			break;
		case VAMS_EVENT_COMMAND_COMPLETE:
			printk("Command: id=0x%08" PRIx32 " status=%" PRIu32
			       " error=%" PRIu32 " cookie=0x%016" PRIx64 "\n",
			       event.command_id, event.arg0, event.arg1, event.value);
			break;
		case VAMS_EVENT_TELEMETRY:
			printk("Telemetry: heartbeat=%" PRIu32
			       " uptime_ms=%" PRIu64 " healthy=1\n",
			       event.arg0, event.value);
			break;
		case VAMS_EVENT_WATCHDOG_WITHHELD:
			printk("Watchdog test: withholding pet\n");
			break;
		case VAMS_EVENT_OVERLOAD_TEST:
			printk("Overload test: event=%" PRIu32 "\n", event.arg0);
			break;
		case VAMS_EVENT_HEALTH_STUCK:
			printk("Health: stuck_task=%" PRIu32
			       " failures=%" PRIu32 "\n",
			       event.arg0, event.arg1);
			break;
		default:
			break;
		}
	}
}

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

		status = k_msgq_put(&vams_heartbeat_queue, &heartbeat, K_NO_WAIT);
		if (status != 0) {
			vams_saturating_atomic_inc(&heartbeat_drop_count);
		}
		vams_task_progress(&producer_epoch, VAMS_HEALTH_TASK_PRODUCER);
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
		const int status = k_msgq_get(&vams_heartbeat_queue, &heartbeat,
					      K_MSEC(VAMS_SERVICE_POLL_MS));

		vams_task_progress(&monitor_epoch, VAMS_HEALTH_TASK_MONITOR);
		if (status == -EAGAIN) {
			continue;
		}
		__ASSERT_NO_MSG(status == 0);
		vams_event_emit(VAMS_EVENT_HEARTBEAT, 0U, command_generation,
				heartbeat.sequence, 0U, 0U,
				(uint64_t)heartbeat.uptime_ms);
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
		vams_task_progress(&mailbox_epoch, VAMS_HEALTH_TASK_MAILBOX);
		if (status == -EAGAIN) {
			continue;
		}
		__ASSERT_NO_MSG(status == 0);

		const uint32_t response = message == VAMS_MAILBOX_PING ?
			VAMS_MAILBOX_PING_RESPONSE : VAMS_MAILBOX_UNSUPPORTED;

		status = vams_mailbox_respond(mailbox, response);
		__ASSERT_NO_MSG(status == 0);
		vams_event_emit(VAMS_EVENT_MAILBOX, 0U, command_generation,
				message, response, 0U, 0U);
	}
}

static void vams_command_receiver(void *unused1, void *unused2, void *unused3)
{
	bool overload_test_complete = false;

	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	for (;;) {
		struct vams_command_object *command;
		int status;

		vams_task_progress(&receiver_epoch, VAMS_HEALTH_TASK_RECEIVER);

		if (IS_ENABLED(CONFIG_VAMS_OVERLOAD_TEST) &&
		    !overload_test_complete &&
		    vams_command_pending(command_portal)) {
			void *held[VAMS_COMMAND_POOL_SIZE];

			for (size_t index = 0U; index < ARRAY_SIZE(held); index++) {
				status = k_mem_slab_alloc(&vams_command_pool,
							  &held[index], K_NO_WAIT);
				__ASSERT_NO_MSG(status == 0);
				vams_record_high_water(&command_pool_high_water,
					atomic_inc(&command_objects_in_use) + 1);
			}
			for (uint32_t index = 0U; index < VAMS_COMMAND_POOL_SIZE;
			     index++) {
				__ASSERT_NO_MSG(!vams_command_admission_allowed(
					vams_command_pending(command_portal),
					k_mem_slab_num_free_get(&vams_command_pool)));
				vams_saturating_atomic_inc(&admission_defer_count);
				k_sleep(K_MSEC(1));
			}
			for (size_t index = 0U; index < ARRAY_SIZE(held); index++) {
				k_mem_slab_free(&vams_command_pool, held[index]);
				atomic_dec(&command_objects_in_use);
			}
			overload_test_complete = true;
			continue;
		}

		if (!vams_command_admission_allowed(
				vams_command_pending(command_portal),
				k_mem_slab_num_free_get(&vams_command_pool))) {
			if (vams_command_pending(command_portal)) {
				vams_saturating_atomic_inc(&admission_defer_count);
			}
			k_sleep(K_MSEC(1));
			continue;
		}
		status = k_mem_slab_alloc(&vams_command_pool, (void **)&command,
					  K_NO_WAIT);
		if (status != 0) {
			vams_saturating_atomic_inc(&admission_defer_count);
			k_sleep(K_MSEC(1));
			continue;
		}
		vams_record_high_water(&command_pool_high_water,
				       atomic_inc(&command_objects_in_use) + 1);
		memset(command, 0, sizeof(*command));
		status = vams_command_receive(command_portal, &command->submission,
					      K_NO_WAIT);
		if (status != 0) {
			k_mem_slab_free(&vams_command_pool, command);
			atomic_dec(&command_objects_in_use);
			continue;
		}
		command_sequence++;
		vams_scheduler_capture(command, command_generation,
				       command_sequence, k_uptime_get());
		vams_retained_note_command(
			sys_le32_to_cpu(command->submission.command_id),
			command_generation);
		status = vams_queue_put(&vams_validation_queue, &command,
					&validation_queue_high_water);
		__ASSERT_NO_MSG(status == 0);
	}
}

static void vams_command_validator(void *unused1, void *unused2, void *unused3)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	for (;;) {
		struct vams_command_object *command;
		int status;

		status = k_msgq_get(&vams_validation_queue, &command,
				    K_MSEC(VAMS_SERVICE_POLL_MS));
		vams_task_progress(&validator_epoch, VAMS_HEALTH_TASK_VALIDATOR);
		if (status == -EAGAIN) {
			continue;
		}
		__ASSERT_NO_MSG(status == 0);
		vams_scheduler_transition(command, VAMS_COMMAND_SUBMITTED,
					  VAMS_COMMAND_VALIDATING);
		vams_command_execute(&command->submission, &command->completion,
				     k_uptime_get());
		if (sys_le16_to_cpu(command->completion.status) !=
		    VAMS_STATUS_SUCCESS) {
			vams_scheduler_transition(command, VAMS_COMMAND_VALIDATING,
						  VAMS_COMMAND_COMPLETED_ERROR);
			status = vams_queue_put(&vams_completion_queue, &command,
						&completion_queue_high_water);
		} else {
			vams_scheduler_transition(command, VAMS_COMMAND_VALIDATING,
						  VAMS_COMMAND_QUEUED);
			status = vams_queue_put(&vams_ready_queue, &command,
						&ready_queue_high_water);
		}
		__ASSERT_NO_MSG(status == 0);
	}
}

static size_t vams_scheduler_select(
	struct vams_command_object *const ready[VAMS_COMMAND_POOL_SIZE],
	size_t count)
{
	size_t selected = 0U;

	for (size_t index = 1U; index < count; index++) {
		if (vams_scheduler_precedes(ready[index], ready[selected])) {
			selected = index;
		}
	}
	return selected;
}

static bool vams_is_payload_opcode(uint8_t opcode)
{
	return opcode == VAMS_OP_MEM_COPY || opcode == VAMS_OP_MEM_FILL ||
	       opcode == VAMS_OP_CRC32 || opcode == VAMS_OP_VECTOR_ADD;
}

static int vams_publish_portal_completion(
	const struct vams_completion *completion)
{
	const int64_t deadline =
		k_uptime_get() + VAMS_COMPLETION_PUBLISH_TIMEOUT_MS;
	int status;

	do {
		status = vams_command_complete(command_portal, completion);
		if (status == -EBUSY) {
			if (k_uptime_get() >= deadline) {
				vams_saturating_atomic_inc(&portal_stall_count);
				return -ETIMEDOUT;
			}
			k_sleep(K_MSEC(1));
		}
	} while (status == -EBUSY);
	return status;
}

static void vams_request_overload_reset(void)
{
	atomic_set(&overload_reset_requested, 1);
	vams_management_reset(management);
	for (;;) {
		/* The watchdog is a one-second backstop if reset delivery fails. */
		k_sleep(K_MSEC(10));
	}
}

static void vams_command_scheduler(void *unused1, void *unused2, void *unused3)
{
	struct vams_command_object *ready[VAMS_COMMAND_POOL_SIZE];
	size_t ready_count = 0U;

	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	for (;;) {
		struct vams_command_object *command;
		size_t selected;
		int status;

		vams_task_progress(&scheduler_epoch, VAMS_HEALTH_TASK_SCHEDULER);

		if (ready_count == 0U) {
			status = k_msgq_get(&vams_ready_queue, &ready[0],
					    K_MSEC(VAMS_SERVICE_POLL_MS));
			if (status == -EAGAIN) {
				continue;
			}
			__ASSERT_NO_MSG(status == 0);
			ready_count = 1U;
		}
		while (ready_count < ARRAY_SIZE(ready) &&
		       k_msgq_get(&vams_ready_queue, &ready[ready_count],
				    K_NO_WAIT) == 0) {
			ready_count++;
		}
		selected = vams_scheduler_select(ready, ready_count);
		command = ready[selected];
		ready_count--;
		ready[selected] = ready[ready_count];

		if (CONFIG_VAMS_SCHEDULER_TEST_DELAY_MS > 0) {
			k_sleep(K_MSEC(CONFIG_VAMS_SCHEDULER_TEST_DELAY_MS));
		}
		if (command->generation != command_generation) {
			vams_scheduler_cancel(command, k_uptime_get());
		} else if (vams_scheduler_deadline_expired(command,
							 k_uptime_get())) {
			vams_scheduler_transition(command, VAMS_COMMAND_QUEUED,
						  VAMS_COMMAND_ABORTING);
			vams_scheduler_timeout(command, k_uptime_get());
		} else {
			vams_scheduler_transition(command, VAMS_COMMAND_QUEUED,
						  VAMS_COMMAND_RUNNING);
			if (vams_is_payload_opcode(command->submission.opcode)) {
				status = vams_publish_portal_completion(
					&command->completion);
				if (status == -ETIMEDOUT) {
					vams_request_overload_reset();
				}
				__ASSERT_NO_MSG(status == 0);
				status = vams_queue_put(&vams_running_queue, &command,
							&running_queue_high_water);
				__ASSERT_NO_MSG(status == 0);
				continue;
			}
			vams_scheduler_transition(command, VAMS_COMMAND_RUNNING,
						  VAMS_COMMAND_COMPLETED);
		}
		status = vams_queue_put(&vams_completion_queue, &command,
					&completion_queue_high_water);
		__ASSERT_NO_MSG(status == 0);
	}
}

static void vams_recovery_manager(void *unused1, void *unused2, void *unused3)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	for (;;) {
		struct vams_command_object *command;
		struct vams_completion result;
		uint64_t now_ms;
		int status;

		status = k_msgq_get(&vams_running_queue, &command,
				    K_MSEC(VAMS_SERVICE_POLL_MS));
		vams_task_progress(&recovery_epoch, VAMS_HEALTH_TASK_RECOVERY);
		if (status == -EAGAIN) {
			continue;
		}
		__ASSERT_NO_MSG(status == 0);
		now_ms = k_uptime_get();
		do {
			const uint64_t remaining_ms =
				command->deadline_ms > now_ms ?
				command->deadline_ms - now_ms : 0U;
			const uint64_t poll_ms =
				MIN(remaining_ms, VAMS_SERVICE_POLL_MS);

			status = vams_command_result_receive(
				command_portal, &result,
				poll_ms > 0U ? K_MSEC(poll_ms) : K_NO_WAIT);
			vams_task_progress(&recovery_epoch,
					   VAMS_HEALTH_TASK_RECOVERY);
			now_ms = k_uptime_get();
		} while (status == -EAGAIN && now_ms < command->deadline_ms);
		if (status == -EAGAIN) {
			vams_saturating_atomic_inc(&recovery_attempt_count);
			vams_scheduler_begin_abort(command, k_uptime_get());
			vams_event_emit(VAMS_EVENT_ABORT_REQUEST,
				sys_le32_to_cpu(command->submission.command_id),
				command->generation,
				(uint32_t)atomic_get(&recovery_attempt_count),
				0U, 0U, 0U);
			status = vams_command_abort(command_portal,
						    &command->completion);
			if (status == 0) {
				status = vams_command_result_receive(
					command_portal, &result,
					K_MSEC(VAMS_ABORT_ACK_TIMEOUT_MS));
			} else if (status == -EBUSY) {
				status = vams_command_result_receive(
					command_portal, &result, K_NO_WAIT);
			}
			if (status == -EAGAIN) {
				vams_saturating_atomic_inc(&recovery_escalation_count);
				vams_scheduler_escalate(command, k_uptime_get());
				vams_event_emit(VAMS_EVENT_ABORT_ESCALATED,
					sys_le32_to_cpu(
						command->submission.command_id),
					command->generation,
					(uint32_t)atomic_get(
						&recovery_escalation_count),
					0U, 0U, 0U);
				goto publish;
			}
		}
		__ASSERT_NO_MSG(status == 0);
		status = vams_scheduler_apply_result(command, &result);
		if (status == -EPROTO) {
			vams_saturating_atomic_inc(&recovery_escalation_count);
			vams_scheduler_escalate(command, k_uptime_get());
			vams_event_emit(VAMS_EVENT_RESULT_MISMATCH,
				sys_le32_to_cpu(command->submission.command_id),
				command->generation, 0U, 0U, 0U, 0U);
		} else {
			__ASSERT_NO_MSG(status == 0);
		}
	publish:
		status = vams_queue_put(&vams_completion_queue, &command,
					&completion_queue_high_water);
		__ASSERT_NO_MSG(status == 0);
	}
}

static void vams_command_completion(void *unused1, void *unused2, void *unused3)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	for (;;) {
		struct vams_command_object *command;
		enum vams_command_state terminal_state;
		int status;

		status = k_msgq_get(&vams_completion_queue, &command,
				    K_MSEC(VAMS_SERVICE_POLL_MS));
		vams_task_progress(&completion_epoch,
				   VAMS_HEALTH_TASK_COMPLETION);
		if (status == -EAGAIN) {
			continue;
		}
		__ASSERT_NO_MSG(status == 0);
		terminal_state = command->state;
		status = vams_publish_portal_completion(&command->completion);
		if (status == -ETIMEDOUT) {
			vams_request_overload_reset();
		}
		__ASSERT_NO_MSG(status == 0);
		vams_scheduler_mark_published(command);
		vams_event_emit(VAMS_EVENT_COMMAND_COMPLETE,
			sys_le32_to_cpu(command->completion.command_id),
			command->generation,
			sys_le16_to_cpu(command->completion.status),
			sys_le16_to_cpu(command->completion.error_code), 0U,
			sys_le64_to_cpu(command->completion.user_cookie));
		vams_scheduler_transition(command, terminal_state, VAMS_COMMAND_FREE);
		vams_retained_clear_command();
		memset(command, 0, sizeof(*command));
		k_mem_slab_free(&vams_command_pool, command);
		atomic_dec(&command_objects_in_use);
	}
}

static size_t vams_stack_used(struct k_thread *thread, size_t capacity)
{
	size_t unused = 0U;
	const int status = k_thread_stack_space_get(thread, &unused);

	__ASSERT_NO_MSG(status == 0);
	return capacity - unused;
}

static void vams_report_resources(uint64_t watchdog_max_interval_ms)
{
	const size_t static_sram =
		(size_t)((uintptr_t)_image_ram_end - (uintptr_t)_image_ram_start);
	const uint64_t watchdog_margin_ms =
		watchdog_max_interval_ms < VAMS_WATCHDOG_TIMEOUT_MS ?
		VAMS_WATCHDOG_TIMEOUT_MS - watchdog_max_interval_ms : 0U;

	printk("Resources: static_sram=%zu/%u pool_high=%" PRId32 "/%u"
	       " validation_high=%" PRId32 "/%u ready_high=%" PRId32 "/%u"
	       " running_high=%" PRId32 "/1 completion_high=%" PRId32 "/%u"
	       " recovery_attempts=%" PRId32
	       " recovery_escalations=%" PRId32
	       " admission_defers=%" PRIu32 " heartbeat_drops=%" PRIu32
	       " queue_overloads=%" PRIu32 " portal_stalls=%" PRIu32
	       " event_drops=%" PRIu32 " health_failures=%" PRIu32
	       " stuck_task=%" PRIu32 "\n",
	       static_sram, CONFIG_SRAM_SIZE * 1024U,
	       (int32_t)atomic_get(&command_pool_high_water),
	       VAMS_COMMAND_POOL_SIZE,
	       (int32_t)atomic_get(&validation_queue_high_water),
	       VAMS_COMMAND_POOL_SIZE,
	       (int32_t)atomic_get(&ready_queue_high_water),
	       VAMS_COMMAND_POOL_SIZE,
	       (int32_t)atomic_get(&running_queue_high_water),
	       (int32_t)atomic_get(&completion_queue_high_water),
	       VAMS_COMMAND_POOL_SIZE,
	       (int32_t)atomic_get(&recovery_attempt_count),
	       (int32_t)atomic_get(&recovery_escalation_count),
	       (uint32_t)atomic_get(&admission_defer_count),
	       (uint32_t)atomic_get(&heartbeat_drop_count),
	       (uint32_t)atomic_get(&queue_overload_count),
	       (uint32_t)atomic_get(&portal_stall_count),
	       vams_event_drop_count(),
	       (uint32_t)atomic_get(&health_failure_count),
	       (uint32_t)atomic_get(&health_stuck_task));
	printk("Stacks: producer=%zu/%zu monitor=%zu/%zu mailbox=%zu/%zu"
	       " receiver=%zu/%zu validator=%zu/%zu scheduler=%zu/%zu"
	       " recovery=%zu/%zu completion=%zu/%zu health=%zu/%zu"
	       " event=%zu/%zu\n",
	       vams_stack_used(&vams_producer_thread,
			       K_THREAD_STACK_SIZEOF(vams_producer_stack)),
	       K_THREAD_STACK_SIZEOF(vams_producer_stack),
	       vams_stack_used(&vams_monitor_thread,
			       K_THREAD_STACK_SIZEOF(vams_monitor_stack)),
	       K_THREAD_STACK_SIZEOF(vams_monitor_stack),
	       vams_stack_used(&vams_mailbox_thread,
			       K_THREAD_STACK_SIZEOF(vams_mailbox_stack)),
	       K_THREAD_STACK_SIZEOF(vams_mailbox_stack),
	       vams_stack_used(&vams_receiver_thread,
			       K_THREAD_STACK_SIZEOF(vams_receiver_stack)),
	       K_THREAD_STACK_SIZEOF(vams_receiver_stack),
	       vams_stack_used(&vams_validator_thread,
			       K_THREAD_STACK_SIZEOF(vams_validator_stack)),
	       K_THREAD_STACK_SIZEOF(vams_validator_stack),
	       vams_stack_used(&vams_scheduler_thread,
			       K_THREAD_STACK_SIZEOF(vams_scheduler_stack)),
	       K_THREAD_STACK_SIZEOF(vams_scheduler_stack),
	       vams_stack_used(&vams_recovery_thread,
			       K_THREAD_STACK_SIZEOF(vams_recovery_stack)),
	       K_THREAD_STACK_SIZEOF(vams_recovery_stack),
	       vams_stack_used(&vams_completion_thread,
			       K_THREAD_STACK_SIZEOF(vams_completion_stack)),
	       K_THREAD_STACK_SIZEOF(vams_completion_stack),
	       vams_stack_used(&vams_health_thread,
			       K_THREAD_STACK_SIZEOF(vams_health_stack)),
	       K_THREAD_STACK_SIZEOF(vams_health_stack),
	       vams_stack_used(&vams_event_thread,
			       K_THREAD_STACK_SIZEOF(vams_event_stack)),
	       K_THREAD_STACK_SIZEOF(vams_event_stack));
	printk("Watchdog margin: timeout_ms=%u max_pet_interval_ms=%" PRIu64
	       " margin_ms=%" PRIu64 "\n",
	       VAMS_WATCHDOG_TIMEOUT_MS, watchdog_max_interval_ms,
	       watchdog_margin_ms);
}

static void vams_health_monitor(void *arg1, void *unused2, void *unused3)
{
	const struct vams_management_snapshot *boot_snapshot = arg1;
	uint32_t epochs[VAMS_HEALTH_TASK_COUNT];
	struct vams_health_tracker tracker;
	uint32_t heartbeat = 0U;
	uint32_t reported_failure_count = 0U;
	bool expiry_announced = false;
	bool resources_reported = false;
	uint64_t last_pet_ms = k_uptime_get();
	uint64_t watchdog_max_interval_ms = 0U;

	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);
	vams_health_epochs(epochs);
	vams_health_tracker_init(&tracker, epochs, VAMS_HEALTH_STARTUP_GRACE,
				 VAMS_HEALTH_MISS_LIMIT);

	for (;;) {
		uint64_t uptime_ms;
		bool healthy;

		k_sleep(VAMS_HEARTBEAT_PERIOD);
		uptime_ms = k_uptime_get();
		vams_health_epochs(epochs);
		healthy = vams_health_tracker_update(&tracker, epochs);
		atomic_set(&health_failure_count,
			   (atomic_val_t)tracker.failure_count);
		atomic_set(&health_stuck_task, (atomic_val_t)tracker.stuck_task);
		if (!healthy && tracker.failure_count != reported_failure_count) {
			reported_failure_count = tracker.failure_count;
			vams_retained_note_health(tracker.stuck_task,
						  tracker.failure_count);
			vams_event_emit(VAMS_EVENT_HEALTH_STUCK, 0U,
					command_generation, tracker.stuck_task,
					tracker.failure_count, 0U, 0U);
		}

		if (healthy) {
			heartbeat++;
			vams_management_publish(management, heartbeat, uptime_ms,
						VAMS_FIRMWARE_VERSION);
			vams_event_emit(VAMS_EVENT_TELEMETRY, 0U,
					command_generation, heartbeat, 0U, 0U,
					uptime_ms);
		}

		if (IS_ENABLED(CONFIG_VAMS_WATCHDOG_EXPIRY_TEST) &&
		    (boot_snapshot->watchdog_reset_count == 0U)) {
			if (!expiry_announced) {
				vams_event_emit(VAMS_EVENT_WATCHDOG_WITHHELD, 0U,
						command_generation, 0U, 0U, 0U, 0U);
				expiry_announced = true;
			}
		} else if (healthy && atomic_get(&overload_reset_requested) == 0) {
			const int status = vams_management_watchdog_pet(management);
			const uint64_t pet_interval_ms = uptime_ms - last_pet_ms;

			__ASSERT_NO_MSG(status == 0);
			last_pet_ms = uptime_ms;
			if (pet_interval_ms > watchdog_max_interval_ms) {
				watchdog_max_interval_ms = pet_interval_ms;
			}
		}

		if (!resources_reported && heartbeat >= 4U) {
			vams_report_resources(watchdog_max_interval_ms);
			resources_reported = true;
		}
	}
}

int main(void)
{
	static struct vams_management_snapshot boot_snapshot;
	struct vams_retained_record retained_snapshot;
	bool retained_valid;
	int status;

	printk("Virtual Accelerator Management SoC Zephyr booting\n");
	printk("Kernel: Zephyr %s\n", KERNEL_VERSION_STRING);
	k_msgq_init(&vams_heartbeat_queue, vams_heartbeat_buffer,
		     sizeof(struct vams_heartbeat), 4);
	__ASSERT_NO_MSG(device_is_ready(mailbox));
	__ASSERT_NO_MSG(device_is_ready(management));
	__ASSERT_NO_MSG(device_is_ready(command_portal));

	vams_management_snapshot(management, &boot_snapshot);
	command_generation = boot_snapshot.reset_generation;
	boot_reset_generation = boot_snapshot.reset_generation;
	test_freeze_task = boot_snapshot.test_freeze_task;
	retained_valid = vams_retained_boot(management, boot_snapshot.reset_reason,
					    boot_snapshot.reset_generation);
	vams_retained_snapshot(&retained_snapshot);
	printk("Reset: reason=%" PRIu32 " watchdog_count=%" PRIu32
	       " generation=%" PRIu32 " notifications=%" PRIu32
	       " notification_failures=%" PRIu32 " reset_acks=%" PRIu32
	       " reset_ack_timeouts=%" PRIu32 " reset_coalesced=%" PRIu32
	       "\n",
	       boot_snapshot.reset_reason,
	       boot_snapshot.watchdog_reset_count,
	       boot_snapshot.reset_generation, boot_snapshot.reset_notify_count,
	       boot_snapshot.reset_notify_fail_count,
	       boot_snapshot.reset_ack_count,
	       boot_snapshot.reset_ack_timeout_count,
	       boot_snapshot.reset_coalesce_count);
	printk("Retained: valid=%u boots=%" PRIu32 " reset_reason=%" PRIu32
	       " generation=%" PRIu32 " stuck_task=%" PRIu32
	       " health_failures=%" PRIu32 " last_command=0x%08" PRIx32
	       " events=%" PRIu32 " crc=0x%08" PRIx32 "\n",
	       retained_valid ? 1U : 0U, retained_snapshot.boot_count,
	       retained_snapshot.reset_reason, retained_snapshot.reset_generation,
	       retained_snapshot.last_stuck_task,
	       retained_snapshot.health_failure_count,
	       retained_snapshot.last_command_id, retained_snapshot.event_count,
	       retained_snapshot.crc32);
	if (boot_snapshot.reset_reason == VAMS_RESET_REASON_WATCHDOG) {
		printk("Recovery: watchdog reset observed\n");
	}

	status = vams_management_watchdog_start(management,
						VAMS_WATCHDOG_TIMEOUT_MS);
	__ASSERT_NO_MSG(status == 0);

	(void)k_thread_create(&vams_event_thread, vams_event_stack,
			      K_THREAD_STACK_SIZEOF(vams_event_stack),
			      vams_event_logger, NULL, NULL, NULL,
			      VAMS_TASK_PRIORITY + 1, 0, K_FOREVER);
	status = k_thread_name_set(&vams_event_thread, "vams_event");
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

	(void)k_thread_create(&vams_completion_thread, vams_completion_stack,
			      K_THREAD_STACK_SIZEOF(vams_completion_stack),
			      vams_command_completion, NULL, NULL, NULL,
			      VAMS_COMPLETION_PRIORITY, 0, K_FOREVER);
	status = k_thread_name_set(&vams_completion_thread, "vams_completion");
	__ASSERT_NO_MSG(status == 0);

	(void)k_thread_create(&vams_recovery_thread, vams_recovery_stack,
			      K_THREAD_STACK_SIZEOF(vams_recovery_stack),
			      vams_recovery_manager, NULL, NULL, NULL,
			      VAMS_RECOVERY_PRIORITY, 0, K_FOREVER);
	status = k_thread_name_set(&vams_recovery_thread, "vams_recovery");
	__ASSERT_NO_MSG(status == 0);

	(void)k_thread_create(&vams_scheduler_thread, vams_scheduler_stack,
			      K_THREAD_STACK_SIZEOF(vams_scheduler_stack),
			      vams_command_scheduler, NULL, NULL, NULL,
			      VAMS_SCHEDULER_PRIORITY, 0, K_FOREVER);
	status = k_thread_name_set(&vams_scheduler_thread, "vams_scheduler");
	__ASSERT_NO_MSG(status == 0);

	(void)k_thread_create(&vams_validator_thread, vams_validator_stack,
			      K_THREAD_STACK_SIZEOF(vams_validator_stack),
			      vams_command_validator, NULL, NULL, NULL,
			      VAMS_VALIDATOR_PRIORITY, 0, K_FOREVER);
	status = k_thread_name_set(&vams_validator_thread, "vams_validator");
	__ASSERT_NO_MSG(status == 0);

	(void)k_thread_create(&vams_receiver_thread, vams_receiver_stack,
			      K_THREAD_STACK_SIZEOF(vams_receiver_stack),
			      vams_command_receiver, NULL, NULL, NULL,
			      VAMS_RECEIVER_PRIORITY, 0, K_FOREVER);
	status = k_thread_name_set(&vams_receiver_thread, "vams_receiver");
	__ASSERT_NO_MSG(status == 0);

	(void)k_thread_create(&vams_health_thread, vams_health_stack,
			      K_THREAD_STACK_SIZEOF(vams_health_stack),
			      vams_health_monitor, &boot_snapshot, NULL, NULL,
			      VAMS_HEALTH_PRIORITY, 0, K_FOREVER);
	status = k_thread_name_set(&vams_health_thread, "vams_health");
	__ASSERT_NO_MSG(status == 0);

	printk("Tasks: producer -> message queue -> monitor\n");
	printk("Services: fixed-pool command scheduler, mailbox, watchdog, reset telemetry\n");
	if (IS_ENABLED(CONFIG_VAMS_OVERLOAD_TEST)) {
		const struct vams_heartbeat test_heartbeat = { 0 };

		for (uint32_t index = 0U; index < 64U; index++) {
			vams_event_emit(VAMS_EVENT_OVERLOAD_TEST, 0U,
					command_generation, index, 0U, 0U, 0U);
		}
		for (uint32_t index = 0U; index < 8U; index++) {
			if (k_msgq_put(&vams_heartbeat_queue, &test_heartbeat,
				       K_NO_WAIT) != 0) {
				vams_saturating_atomic_inc(&heartbeat_drop_count);
			}
		}
	}
	k_thread_start(&vams_event_thread);
	if (IS_ENABLED(CONFIG_VAMS_OVERLOAD_TEST)) {
		k_sleep(K_MSEC(1));
	}
	k_thread_start(&vams_monitor_thread);
	if (IS_ENABLED(CONFIG_VAMS_OVERLOAD_TEST)) {
		k_sleep(K_MSEC(1));
	}
	k_thread_start(&vams_producer_thread);
	k_thread_start(&vams_mailbox_thread);
	k_thread_start(&vams_completion_thread);
	k_thread_start(&vams_recovery_thread);
	k_thread_start(&vams_scheduler_thread);
	k_thread_start(&vams_validator_thread);
	k_thread_start(&vams_receiver_thread);
	k_thread_start(&vams_health_thread);

	return 0;
}
