/* SPDX-License-Identifier: MIT */
#ifndef VAMS_RETAINED_H
#define VAMS_RETAINED_H

#include <stdbool.h>
#include <stdint.h>

#define VAMS_RETAINED_MAGIC UINT32_C(0x56414d52)
#define VAMS_RETAINED_VERSION 1U
#define VAMS_RETAINED_EVENT_COUNT 4U

struct vams_retained_event {
	uint32_t event_id;
	uint32_t command_id;
	uint32_t generation;
	uint32_t timestamp_ms;
};

struct vams_retained_record {
	uint32_t magic;
	uint16_t version;
	uint16_t length;
	uint32_t crc32;
	uint32_t boot_count;
	uint32_t reset_reason;
	uint32_t reset_generation;
	uint32_t last_stuck_task;
	uint32_t health_failure_count;
	uint32_t last_command_id;
	uint32_t last_command_generation;
	uint32_t last_assertion;
	uint32_t stack_failure;
	uint32_t event_head;
	uint32_t event_count;
	struct vams_retained_event events[VAMS_RETAINED_EVENT_COUNT];
};

uint32_t vams_retained_crc32(const struct vams_retained_record *record);
bool vams_retained_validate(const struct vams_retained_record *record);
void vams_retained_format(struct vams_retained_record *record,
			  uint32_t reset_reason, uint32_t generation);

#ifdef __ZEPHYR__
#include <zephyr/device.h>
bool vams_retained_boot(const struct device *management, uint32_t reset_reason,
			uint32_t generation);
void vams_retained_snapshot(struct vams_retained_record *record);
void vams_retained_note_health(uint32_t stuck_task, uint32_t failures);
void vams_retained_note_command(uint32_t command_id, uint32_t generation);
void vams_retained_clear_command(void);
void vams_retained_note_event(uint32_t event_id, uint32_t command_id,
			      uint32_t generation, uint32_t timestamp_ms);
#endif

#endif /* VAMS_RETAINED_H */
