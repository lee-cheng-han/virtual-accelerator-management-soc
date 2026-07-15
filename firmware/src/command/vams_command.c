/* SPDX-License-Identifier: MIT */

#include <stdint.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>

#include <vams_command.h>

#define VAMS_MAX_TIMEOUT_MS 60000U

static uint16_t vams_validate_nop(const struct vams_submission *submission)
{
	if (sys_le16_to_cpu(submission->version) != VAMS_DESC_VERSION_1) {
		return VAMS_ERR_UNSUPPORTED_VERSION;
	}
	if (submission->opcode != VAMS_OP_NOP) {
		return VAMS_ERR_INVALID_OPCODE;
	}
	if (submission->flags != 0U) {
		return VAMS_ERR_INVALID_FLAGS;
	}
	if (sys_le32_to_cpu(submission->expected_crc) != 0U ||
	    sys_le32_to_cpu(submission->reserved0) != 0U ||
	    sys_le64_to_cpu(submission->reserved1) != 0U ||
	    sys_le64_to_cpu(submission->reserved2) != 0U) {
		return VAMS_ERR_RESERVED_NONZERO;
	}
	if (sys_le32_to_cpu(submission->timeout_ms) > VAMS_MAX_TIMEOUT_MS) {
		return VAMS_ERR_INVALID_TIMEOUT;
	}
	if (sys_le32_to_cpu(submission->length) != 0U) {
		return VAMS_ERR_INVALID_LENGTH;
	}
	if (sys_le64_to_cpu(submission->source_dma) != 0U ||
	    sys_le64_to_cpu(submission->destination_dma) != 0U) {
		return VAMS_ERR_INVALID_ADDRESS;
	}
	return VAMS_ERR_NONE;
}

void vams_command_execute(const struct vams_submission *submission,
			  struct vams_completion *completion,
			  uint64_t timestamp)
{
	const uint16_t error = vams_validate_nop(submission);

	memset(completion, 0, sizeof(*completion));
	completion->command_id = submission->command_id;
	completion->status = sys_cpu_to_le16(error == VAMS_ERR_NONE ?
					       VAMS_STATUS_SUCCESS :
					       VAMS_STATUS_INVALID);
	completion->error_code = sys_cpu_to_le16(error);
	completion->user_cookie = submission->user_cookie;
	completion->device_timestamp = sys_cpu_to_le64(timestamp);
}
