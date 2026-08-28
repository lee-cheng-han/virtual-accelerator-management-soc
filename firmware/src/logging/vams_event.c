/* SPDX-License-Identifier: MIT */

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <vams_event.h>
#include <vams_overload.h>
#include <vams_retained.h>

#define VAMS_EVENT_QUEUE_DEPTH 32U

K_MSGQ_DEFINE(vams_event_queue, sizeof(struct vams_event),
	      VAMS_EVENT_QUEUE_DEPTH, 4);

static atomic_t event_drop_count;

static void vams_event_record_drop(void)
{
	atomic_val_t previous = atomic_get(&event_drop_count);

	while ((uint32_t)previous != UINT32_MAX &&
	       !atomic_cas(&event_drop_count, previous,
			   (atomic_val_t)vams_counter_increment(
				   (uint32_t)previous))) {
		previous = atomic_get(&event_drop_count);
	}
}

void vams_event_emit(enum vams_event_id id, uint32_t command_id,
		     uint32_t generation, uint32_t arg0, uint32_t arg1,
		     uint32_t arg2, uint64_t value)
{
	const struct vams_event event = {
		.timestamp_ms = (uint64_t)k_uptime_get(),
		.value = value,
		.command_id = command_id,
		.generation = generation,
		.arg0 = arg0,
		.arg1 = arg1,
		.arg2 = arg2,
		.id = (uint16_t)id,
	};

	vams_retained_note_event((uint32_t)id, command_id, generation,
				 (uint32_t)event.timestamp_ms);

	if (k_msgq_put(&vams_event_queue, &event, K_NO_WAIT) != 0) {
		vams_event_record_drop();
	}
}

int vams_event_receive(struct vams_event *event, k_timeout_t timeout)
{
	return k_msgq_get(&vams_event_queue, event, timeout);
}

uint32_t vams_event_drop_count(void)
{
	return (uint32_t)atomic_get(&event_drop_count);
}
