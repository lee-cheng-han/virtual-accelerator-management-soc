/* SPDX-License-Identifier: MIT */

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#include <vams_scheduler.h>

#define VAMS_DEFAULT_TIMEOUT_MS UINT32_C(30000)

void vams_scheduler_transition(struct vams_command_object *command,
			       enum vams_command_state expected,
			       enum vams_command_state next)
{
	__ASSERT_NO_MSG(command != NULL);
	__ASSERT_NO_MSG(command->state == expected);
	__ASSERT_NO_MSG(next <= VAMS_COMMAND_CANCELLED);
	printk("Scheduler: event=transition sequence=%" PRIu32
	       " command=0x%08" PRIx32 " generation=%" PRIu32
	       " from=%u to=%u\n",
	       command->sequence,
	       sys_le32_to_cpu(command->submission.command_id),
	       command->generation, (unsigned int)command->state,
	       (unsigned int)next);
	command->state = (uint8_t)next;
}

void vams_scheduler_capture(struct vams_command_object *command,
			    uint32_t generation, uint32_t sequence,
			    uint64_t captured_at_ms)
{
	uint32_t timeout_ms;

	__ASSERT_NO_MSG(command != NULL);
	__ASSERT_NO_MSG(command->state == VAMS_COMMAND_FREE);
	timeout_ms = sys_le32_to_cpu(command->submission.timeout_ms);
	if (timeout_ms == 0U) {
		timeout_ms = VAMS_DEFAULT_TIMEOUT_MS;
	}
	command->generation = generation;
	command->sequence = sequence;
	command->captured_at_ms = captured_at_ms;
	command->deadline_ms = captured_at_ms + timeout_ms;
	command->publication_count = 0U;
	vams_scheduler_transition(command, VAMS_COMMAND_FREE,
				  VAMS_COMMAND_SUBMITTED);
}

bool vams_scheduler_deadline_expired(const struct vams_command_object *command,
				     uint64_t now_ms)
{
	__ASSERT_NO_MSG(command != NULL);
	return now_ms >= command->deadline_ms;
}

void vams_scheduler_timeout(struct vams_command_object *command,
			    uint64_t timestamp_ms)
{
	__ASSERT_NO_MSG(command != NULL);
	__ASSERT_NO_MSG(command->state == VAMS_COMMAND_ABORTING);
	command->completion.status = sys_cpu_to_le16(VAMS_STATUS_TIMED_OUT);
	command->completion.error_code = sys_cpu_to_le16(VAMS_ERR_TIMEOUT);
	command->completion.bytes_processed = 0U;
	command->completion.result_crc = 0U;
	command->completion.device_timestamp = sys_cpu_to_le64(timestamp_ms);
	vams_scheduler_transition(command, VAMS_COMMAND_ABORTING,
				  VAMS_COMMAND_COMPLETED_ERROR);
}

void vams_scheduler_cancel(struct vams_command_object *command,
			   uint64_t timestamp_ms)
{
	__ASSERT_NO_MSG(command != NULL);
	__ASSERT_NO_MSG(command->state == VAMS_COMMAND_QUEUED);
	command->completion.status = sys_cpu_to_le16(VAMS_STATUS_RESET);
	command->completion.error_code = sys_cpu_to_le16(VAMS_ERR_RESET);
	command->completion.bytes_processed = 0U;
	command->completion.result_crc = 0U;
	command->completion.device_timestamp = sys_cpu_to_le64(timestamp_ms);
	vams_scheduler_transition(command, VAMS_COMMAND_QUEUED,
				  VAMS_COMMAND_CANCELLED);
}

void vams_scheduler_mark_published(struct vams_command_object *command)
{
	__ASSERT_NO_MSG(command != NULL);
	__ASSERT_NO_MSG(command->state == VAMS_COMMAND_COMPLETED ||
			command->state == VAMS_COMMAND_COMPLETED_ERROR ||
			command->state == VAMS_COMMAND_CANCELLED);
	__ASSERT_NO_MSG(command->publication_count == 0U);
	command->publication_count = 1U;
	printk("Scheduler: event=published sequence=%" PRIu32
	       " command=0x%08" PRIx32 " generation=%" PRIu32
	       " count=%u\n",
	       command->sequence,
	       sys_le32_to_cpu(command->submission.command_id),
	       command->generation, (unsigned int)command->publication_count);
}
