/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <vams_health.h>

int main(void)
{
	uint32_t epochs[VAMS_HEALTH_TASK_COUNT] = { 0 };
	struct vams_health_tracker tracker;

	vams_health_tracker_init(&tracker, epochs, 2U, 2U);
	assert(vams_health_tracker_update(&tracker, epochs));
	assert(vams_health_tracker_update(&tracker, epochs));
	for (uint32_t index = 0U; index < VAMS_HEALTH_TASK_COUNT; index++) {
		epochs[index]++;
	}
	assert(vams_health_tracker_update(&tracker, epochs));
	for (uint32_t frozen = 0U; frozen < VAMS_HEALTH_TASK_COUNT; frozen++) {
		for (uint32_t index = 0U; index < VAMS_HEALTH_TASK_COUNT; index++) {
			if (index != frozen) {
				epochs[index]++;
			}
		}
		assert(vams_health_tracker_update(&tracker, epochs));
		for (uint32_t index = 0U; index < VAMS_HEALTH_TASK_COUNT; index++) {
			if (index != frozen) {
				epochs[index]++;
			}
		}
		assert(!vams_health_tracker_update(&tracker, epochs));
		assert(tracker.stuck_task == frozen + 1U);
		epochs[frozen]++;
		for (uint32_t index = 0U; index < VAMS_HEALTH_TASK_COUNT; index++) {
			epochs[index]++;
		}
		assert(vams_health_tracker_update(&tracker, epochs));
	}
	tracker.failure_count = UINT32_MAX;
	tracker.failure_latched = false;
	assert(vams_health_tracker_update(&tracker, epochs));
	assert(!vams_health_tracker_update(&tracker, epochs));
	assert(tracker.failure_count == UINT32_MAX);
	puts("VAMS per-task health policy test: PASS");
	return 0;
}
