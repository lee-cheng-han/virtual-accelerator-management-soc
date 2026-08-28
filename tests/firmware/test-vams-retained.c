/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vams_retained.h>

int main(void)
{
	struct vams_retained_record record;
	struct vams_retained_record partial;

	vams_retained_format(&record, 5U, 7U);
	assert(vams_retained_validate(&record));
	assert(record.boot_count == 1U);
	assert(record.reset_reason == 5U);
	assert(record.reset_generation == 7U);

	partial = record;
	partial.crc32 ^= UINT32_C(0x1);
	assert(!vams_retained_validate(&partial));
	partial = record;
	partial.version++;
	partial.crc32 = vams_retained_crc32(&partial);
	assert(!vams_retained_validate(&partial));
	partial = record;
	partial.length--;
	partial.crc32 = vams_retained_crc32(&partial);
	assert(!vams_retained_validate(&partial));
	memset(&partial, 0xa5, sizeof(partial) / 2U);
	assert(!vams_retained_validate(&partial));
	puts("VAMS retained crash-record validation test: PASS");
	return 0;
}
