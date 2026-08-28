/* SPDX-License-Identifier: MIT */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <vams_health.h>

void vams_health_tracker_init(struct vams_health_tracker *tracker,
			      const uint32_t epochs[VAMS_HEALTH_TASK_COUNT],
			      uint32_t startup_grace, uint32_t miss_limit)
{
	memset(tracker, 0, sizeof(*tracker));
	memcpy(tracker->last_epoch, epochs, sizeof(tracker->last_epoch));
	tracker->startup_grace = startup_grace;
	tracker->miss_limit = miss_limit;
}

bool vams_health_tracker_update(
	struct vams_health_tracker *tracker,
	const uint32_t epochs[VAMS_HEALTH_TASK_COUNT])
{
	uint32_t stuck_task = VAMS_HEALTH_TASK_NONE;

	if (tracker->startup_grace > 0U) {
		tracker->startup_grace--;
		memcpy(tracker->last_epoch, epochs, sizeof(tracker->last_epoch));
		return true;
	}

	for (uint32_t index = 0U; index < VAMS_HEALTH_TASK_COUNT; index++) {
		if (epochs[index] != tracker->last_epoch[index]) {
			tracker->last_epoch[index] = epochs[index];
			tracker->missed[index] = 0U;
		} else {
			tracker->missed[index] =
				vams_counter_increment(tracker->missed[index]);
			if (stuck_task == VAMS_HEALTH_TASK_NONE &&
			    tracker->missed[index] >= tracker->miss_limit) {
				stuck_task = index + 1U;
			}
		}
	}

	tracker->stuck_task = stuck_task;
	if (stuck_task == VAMS_HEALTH_TASK_NONE) {
		tracker->failure_latched = false;
		return true;
	}
	if (!tracker->failure_latched) {
		tracker->failure_count =
			vams_counter_increment(tracker->failure_count);
		tracker->failure_latched = true;
	}
	return false;
}
