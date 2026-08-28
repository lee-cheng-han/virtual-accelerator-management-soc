/* SPDX-License-Identifier: MIT */
#ifndef VAMS_HEALTH_H
#define VAMS_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

#include <vams_overload.h>

enum vams_health_task_id {
	VAMS_HEALTH_TASK_NONE = 0,
	VAMS_HEALTH_TASK_PRODUCER = 1,
	VAMS_HEALTH_TASK_MONITOR = 2,
	VAMS_HEALTH_TASK_MAILBOX = 3,
	VAMS_HEALTH_TASK_RECEIVER = 4,
	VAMS_HEALTH_TASK_VALIDATOR = 5,
	VAMS_HEALTH_TASK_SCHEDULER = 6,
	VAMS_HEALTH_TASK_RECOVERY = 7,
	VAMS_HEALTH_TASK_COMPLETION = 8,
	VAMS_HEALTH_TASK_COUNT = 8,
};

struct vams_health_tracker {
	uint32_t last_epoch[VAMS_HEALTH_TASK_COUNT];
	uint32_t missed[VAMS_HEALTH_TASK_COUNT];
	uint32_t failure_count;
	uint32_t stuck_task;
	uint32_t startup_grace;
	uint32_t miss_limit;
	bool failure_latched;
};

void vams_health_tracker_init(struct vams_health_tracker *tracker,
			      const uint32_t epochs[VAMS_HEALTH_TASK_COUNT],
			      uint32_t startup_grace, uint32_t miss_limit);
bool vams_health_tracker_update(
	struct vams_health_tracker *tracker,
	const uint32_t epochs[VAMS_HEALTH_TASK_COUNT]);

#endif /* VAMS_HEALTH_H */
