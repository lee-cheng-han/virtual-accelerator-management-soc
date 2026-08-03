/* SPDX-License-Identifier: MIT */
#ifndef VAMS_SCHEDULER_H
#define VAMS_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

#include <vams_abi.h>

#define VAMS_COMMAND_POOL_SIZE 8U

enum vams_command_state {
	VAMS_COMMAND_FREE = 0,
	VAMS_COMMAND_SUBMITTED = 1,
	VAMS_COMMAND_VALIDATING = 2,
	VAMS_COMMAND_QUEUED = 3,
	VAMS_COMMAND_RUNNING = 4,
	VAMS_COMMAND_ABORTING = 5,
	VAMS_COMMAND_COMPLETED = 6,
	VAMS_COMMAND_COMPLETED_ERROR = 7,
	VAMS_COMMAND_CANCELLED = 8,
};

struct vams_command_object {
	struct vams_submission submission;
	struct vams_completion completion;
	uint64_t captured_at_ms;
	uint64_t deadline_ms;
	uint32_t generation;
	uint32_t sequence;
	uint8_t state;
	uint8_t publication_count;
	uint8_t reserved[2];
};

void vams_scheduler_capture(struct vams_command_object *command,
			    uint32_t generation, uint32_t sequence,
			    uint64_t captured_at_ms);
void vams_scheduler_transition(struct vams_command_object *command,
			       enum vams_command_state expected,
			       enum vams_command_state next);
bool vams_scheduler_deadline_expired(const struct vams_command_object *command,
				     uint64_t now_ms);
static inline bool vams_scheduler_precedes(
	const struct vams_command_object *first,
	const struct vams_command_object *second)
{
	return first->deadline_ms < second->deadline_ms ||
	       (first->deadline_ms == second->deadline_ms &&
		first->sequence < second->sequence);
}
void vams_scheduler_timeout(struct vams_command_object *command,
			    uint64_t timestamp_ms);
void vams_scheduler_cancel(struct vams_command_object *command,
			   uint64_t timestamp_ms);
void vams_scheduler_mark_published(struct vams_command_object *command);

#endif /* VAMS_SCHEDULER_H */
