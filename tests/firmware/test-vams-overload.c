/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <vams_overload.h>

int main(void)
{
	assert(vams_counter_increment(0U) == 1U);
	assert(vams_counter_increment(UINT32_MAX - 1U) == UINT32_MAX);
	assert(vams_counter_increment(UINT32_MAX) == UINT32_MAX);
	assert(!vams_command_admission_allowed(false, 8U));
	assert(!vams_command_admission_allowed(true, 0U));
	assert(vams_command_admission_allowed(true, 1U));
	puts("VAMS firmware overload policy test: PASS");
	return 0;
}
