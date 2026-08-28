/* SPDX-License-Identifier: MIT */
#ifndef VAMS_EVENT_H
#define VAMS_EVENT_H

#include <stdint.h>

#include <zephyr/kernel.h>

enum vams_event_id {
	VAMS_EVENT_TRANSITION = 1,
	VAMS_EVENT_PUBLISHED,
	VAMS_EVENT_HEARTBEAT,
	VAMS_EVENT_MAILBOX,
	VAMS_EVENT_ABORT_REQUEST,
	VAMS_EVENT_ABORT_ESCALATED,
	VAMS_EVENT_RESULT_MISMATCH,
	VAMS_EVENT_COMMAND_COMPLETE,
	VAMS_EVENT_TELEMETRY,
	VAMS_EVENT_WATCHDOG_WITHHELD,
	VAMS_EVENT_OVERLOAD_TEST,
	VAMS_EVENT_HEALTH_STUCK,
};

struct vams_event {
	uint64_t timestamp_ms;
	uint64_t value;
	uint32_t command_id;
	uint32_t generation;
	uint32_t arg0;
	uint32_t arg1;
	uint32_t arg2;
	uint16_t id;
	uint16_t reserved;
};

void vams_event_emit(enum vams_event_id id, uint32_t command_id,
		     uint32_t generation, uint32_t arg0, uint32_t arg1,
		     uint32_t arg2, uint64_t value);
int vams_event_receive(struct vams_event *event, k_timeout_t timeout);
uint32_t vams_event_drop_count(void);

#endif /* VAMS_EVENT_H */
