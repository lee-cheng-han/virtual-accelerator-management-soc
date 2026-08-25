/* SPDX-License-Identifier: MIT */
#ifndef VAMS_OVERLOAD_H
#define VAMS_OVERLOAD_H

#include <stdbool.h>
#include <stdint.h>

static inline uint32_t vams_counter_increment(uint32_t value)
{
	return value == UINT32_MAX ? UINT32_MAX : value + 1U;
}

static inline bool vams_command_admission_allowed(bool portal_pending,
						   uint32_t pool_free)
{
	return portal_pending && pool_free > 0U;
}

#endif /* VAMS_OVERLOAD_H */
