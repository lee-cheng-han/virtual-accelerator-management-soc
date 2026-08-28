/* SPDX-License-Identifier: MIT */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <vams_overload.h>
#include <vams_retained.h>

static uint32_t vams_crc32_byte(uint32_t crc, uint8_t value)
{
	crc ^= value;
	for (uint32_t bit = 0U; bit < 8U; bit++) {
		const uint32_t mask = 0U - (crc & 1U);

		crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & mask);
	}
	return crc;
}

uint32_t vams_retained_crc32(const struct vams_retained_record *record)
{
	const uint8_t *bytes = (const uint8_t *)record;
	const size_t crc_offset = offsetof(struct vams_retained_record, crc32);
	uint32_t crc = UINT32_MAX;

	for (size_t index = 0U; index < sizeof(*record); index++) {
		const bool crc_byte = index >= crc_offset &&
			index < crc_offset + sizeof(record->crc32);

		crc = vams_crc32_byte(crc, crc_byte ? 0U : bytes[index]);
	}
	return ~crc;
}

bool vams_retained_validate(const struct vams_retained_record *record)
{
	return record->magic == VAMS_RETAINED_MAGIC &&
	       record->version == VAMS_RETAINED_VERSION &&
	       record->length == sizeof(*record) &&
	       record->crc32 == vams_retained_crc32(record);
}

void vams_retained_format(struct vams_retained_record *record,
			  uint32_t reset_reason, uint32_t generation)
{
	memset(record, 0, sizeof(*record));
	record->magic = VAMS_RETAINED_MAGIC;
	record->version = VAMS_RETAINED_VERSION;
	record->length = sizeof(*record);
	record->boot_count = 1U;
	record->reset_reason = reset_reason;
	record->reset_generation = generation;
	record->crc32 = vams_retained_crc32(record);
}

#ifdef __ZEPHYR__

#include <zephyr/kernel.h>

#include <vams_management.h>

static struct vams_retained_record retained_record;
static struct k_spinlock retained_lock;
static const struct device *retention_device;

BUILD_ASSERT(sizeof(retained_record) <= VAMS_MANAGEMENT_RETENTION_SIZE);
BUILD_ASSERT(sizeof(retained_record) % sizeof(uint32_t) == 0U);

static void vams_retained_commit(void)
{
	retained_record.crc32 = vams_retained_crc32(&retained_record);
	vams_management_retention_write(retention_device, &retained_record,
					 sizeof(retained_record));
}

bool vams_retained_boot(const struct device *management, uint32_t reset_reason,
			uint32_t generation)
{
	retention_device = management;
	vams_management_retention_read(retention_device, &retained_record,
					 sizeof(retained_record));
	const bool valid = vams_retained_validate(&retained_record);

	if (!valid) {
		vams_retained_format(&retained_record, reset_reason, generation);
		vams_management_retention_write(retention_device, &retained_record,
						 sizeof(retained_record));
		return false;
	}
	retained_record.boot_count =
		vams_counter_increment(retained_record.boot_count);
	retained_record.reset_reason = reset_reason;
	retained_record.reset_generation = generation;
	vams_retained_commit();
	return true;
}

void vams_retained_snapshot(struct vams_retained_record *record)
{
	k_spinlock_key_t key = k_spin_lock(&retained_lock);

	*record = retained_record;
	k_spin_unlock(&retained_lock, key);
}

void vams_retained_note_health(uint32_t stuck_task, uint32_t failures)
{
	k_spinlock_key_t key = k_spin_lock(&retained_lock);

	retained_record.last_stuck_task = stuck_task;
	retained_record.health_failure_count = failures;
	vams_retained_commit();
	k_spin_unlock(&retained_lock, key);
}

void vams_retained_note_command(uint32_t command_id, uint32_t generation)
{
	k_spinlock_key_t key = k_spin_lock(&retained_lock);

	retained_record.last_command_id = command_id;
	retained_record.last_command_generation = generation;
	vams_retained_commit();
	k_spin_unlock(&retained_lock, key);
}

void vams_retained_clear_command(void)
{
	k_spinlock_key_t key = k_spin_lock(&retained_lock);

	retained_record.last_command_id = 0U;
	retained_record.last_command_generation = 0U;
	vams_retained_commit();
	k_spin_unlock(&retained_lock, key);
}

void vams_retained_note_event(uint32_t event_id, uint32_t command_id,
			      uint32_t generation, uint32_t timestamp_ms)
{
	k_spinlock_key_t key = k_spin_lock(&retained_lock);
	struct vams_retained_event *event =
		&retained_record.events[retained_record.event_head];

	event->event_id = event_id;
	event->command_id = command_id;
	event->generation = generation;
	event->timestamp_ms = timestamp_ms;
	retained_record.event_head =
		(retained_record.event_head + 1U) % VAMS_RETAINED_EVENT_COUNT;
	if (retained_record.event_count < VAMS_RETAINED_EVENT_COUNT) {
		retained_record.event_count++;
	}
	vams_retained_commit();
	k_spin_unlock(&retained_lock, key);
}

#endif
