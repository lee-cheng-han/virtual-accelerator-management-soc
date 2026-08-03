/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <vams_scheduler.h>

int main(void)
{
	const struct vams_command_object earliest = {
		.deadline_ms = UINT64_C(99),
		.sequence = UINT32_C(9),
	};
	const struct vams_command_object first_tie = {
		.deadline_ms = UINT64_C(100),
		.sequence = UINT32_C(10),
	};
	const struct vams_command_object second_tie = {
		.deadline_ms = UINT64_C(100),
		.sequence = UINT32_C(11),
	};

	assert(VAMS_COMMAND_POOL_SIZE == 8U);
	assert(VAMS_COMMAND_FREE == 0);
	assert(VAMS_COMMAND_CANCELLED == 8);
	assert(vams_scheduler_precedes(&earliest, &first_tie));
	assert(!vams_scheduler_precedes(&first_tie, &earliest));
	assert(vams_scheduler_precedes(&first_tie, &second_tie));
	assert(!vams_scheduler_precedes(&second_tie, &first_tie));
	assert(!vams_scheduler_precedes(&first_tie, &first_tie));
	puts("VAMS firmware scheduler EDF ordering test: PASS");
	return 0;
}
