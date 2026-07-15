/* SPDX-License-Identifier: MIT */
#ifndef VAMS_COMMAND_H
#define VAMS_COMMAND_H

#include <stdint.h>

#include <vams_abi.h>

void vams_command_execute(const struct vams_submission *submission,
			  struct vams_completion *completion,
			  uint64_t timestamp);

#endif /* VAMS_COMMAND_H */
