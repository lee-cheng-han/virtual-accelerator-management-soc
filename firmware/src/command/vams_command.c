/* SPDX-License-Identifier: MIT */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>

#include <vams_command.h>

#define VAMS_MAX_TIMEOUT_MS 60000U
#define VAMS_MAX_TRANSFER_SIZE UINT32_C(0x01000000)

static bool vams_range_overflows(uint64_t address, uint32_t length)
{
	return address > UINT64_MAX - ((uint64_t)length - 1U);
}

static bool vams_ranges_overlap(uint64_t first, uint64_t second,
				uint32_t length)
{
	const uint64_t first_end = first + length - 1U;
	const uint64_t second_end = second + length - 1U;

	return first <= second_end && second <= first_end;
}

static uint16_t vams_validate_command(const struct vams_submission *submission)
{
	const uint8_t opcode = submission->opcode;
	const uint32_t length = sys_le32_to_cpu(submission->length);
	const uint64_t source = sys_le64_to_cpu(submission->source_dma);
	const uint64_t destination =
		sys_le64_to_cpu(submission->destination_dma);

	if (sys_le16_to_cpu(submission->version) != VAMS_DESC_VERSION_1) {
		return VAMS_ERR_UNSUPPORTED_VERSION;
	}
	if (opcode != VAMS_OP_NOP && opcode != VAMS_OP_MEM_COPY &&
	    opcode != VAMS_OP_MEM_FILL && opcode != VAMS_OP_CRC32) {
		return VAMS_ERR_INVALID_OPCODE;
	}
	if ((opcode == VAMS_OP_CRC32 &&
	     (submission->flags & ~VAMS_SUB_F_VERIFY_CRC) != 0U) ||
	    (opcode != VAMS_OP_CRC32 && submission->flags != 0U)) {
		return VAMS_ERR_INVALID_FLAGS;
	}
	if ((opcode != VAMS_OP_CRC32 ||
	     (submission->flags & VAMS_SUB_F_VERIFY_CRC) == 0U) &&
	    sys_le32_to_cpu(submission->expected_crc) != 0U) {
		return VAMS_ERR_RESERVED_NONZERO;
	}
	if (sys_le32_to_cpu(submission->reserved0) != 0U ||
	    sys_le64_to_cpu(submission->reserved1) != 0U ||
	    sys_le64_to_cpu(submission->reserved2) != 0U) {
		return VAMS_ERR_RESERVED_NONZERO;
	}
	if (sys_le32_to_cpu(submission->timeout_ms) > VAMS_MAX_TIMEOUT_MS) {
		return VAMS_ERR_INVALID_TIMEOUT;
	}
	if (opcode == VAMS_OP_NOP && length != 0U) {
		return VAMS_ERR_INVALID_LENGTH;
	}
	if (opcode == VAMS_OP_NOP && (source != 0U || destination != 0U)) {
		return VAMS_ERR_INVALID_ADDRESS;
	}
	if (opcode == VAMS_OP_MEM_COPY) {
		if (length == 0U || length > VAMS_MAX_TRANSFER_SIZE) {
			return VAMS_ERR_INVALID_LENGTH;
		}
		if (vams_range_overflows(source, length) ||
		    vams_range_overflows(destination, length)) {
			return VAMS_ERR_ADDRESS_OVERFLOW;
		}
		if (source == 0U || destination == 0U ||
		    vams_ranges_overlap(source, destination, length)) {
			return VAMS_ERR_INVALID_ADDRESS;
		}
	}
	if (opcode == VAMS_OP_MEM_FILL) {
		if (length == 0U || length > VAMS_MAX_TRANSFER_SIZE) {
			return VAMS_ERR_INVALID_LENGTH;
		}
		if (vams_range_overflows(destination, length)) {
			return VAMS_ERR_ADDRESS_OVERFLOW;
		}
		if (source == 0U || destination == 0U) {
			return VAMS_ERR_INVALID_ADDRESS;
		}
	}
	if (opcode == VAMS_OP_CRC32) {
		if (length == 0U || length > VAMS_MAX_TRANSFER_SIZE) {
			return VAMS_ERR_INVALID_LENGTH;
		}
		if (vams_range_overflows(source, length)) {
			return VAMS_ERR_ADDRESS_OVERFLOW;
		}
		if (source == 0U || destination != 0U) {
			return VAMS_ERR_INVALID_ADDRESS;
		}
	}
	return VAMS_ERR_NONE;
}

void vams_command_execute(const struct vams_submission *submission,
			  struct vams_completion *completion,
			  uint64_t timestamp)
{
	const uint16_t error = vams_validate_command(submission);

	memset(completion, 0, sizeof(*completion));
	completion->command_id = submission->command_id;
	completion->status = sys_cpu_to_le16(error == VAMS_ERR_NONE ?
					       VAMS_STATUS_SUCCESS :
					       VAMS_STATUS_INVALID);
	completion->error_code = sys_cpu_to_le16(error);
	completion->user_cookie = submission->user_cookie;
	completion->device_timestamp = sys_cpu_to_le64(timestamp);
}
