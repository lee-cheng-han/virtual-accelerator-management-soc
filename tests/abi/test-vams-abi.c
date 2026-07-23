#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "vams_abi.h"

_Static_assert(VAMS_SUB_F_VERIFY_CRC == 1U, "CRC verification flag");
_Static_assert(sizeof(struct vams_submission) == 64, "submission size");
_Static_assert(offsetof(struct vams_submission, version) == 0x00, "version");
_Static_assert(offsetof(struct vams_submission, command_id) == 0x04, "id");
_Static_assert(offsetof(struct vams_submission, source_dma) == 0x08, "source");
_Static_assert(offsetof(struct vams_submission, destination_dma) == 0x10,
               "destination");
_Static_assert(offsetof(struct vams_submission, length) == 0x18, "length");
_Static_assert(offsetof(struct vams_submission, timeout_ms) == 0x1c, "timeout");
_Static_assert(offsetof(struct vams_submission, user_cookie) == 0x20, "cookie");
_Static_assert(offsetof(struct vams_submission, expected_crc) == 0x28, "crc");
_Static_assert(offsetof(struct vams_submission, reserved2) == 0x38, "reserved2");

_Static_assert(sizeof(struct vams_completion) == 32, "completion size");
_Static_assert(offsetof(struct vams_completion, command_id) == 0x00, "cq id");
_Static_assert(offsetof(struct vams_completion, status) == 0x04, "status");
_Static_assert(offsetof(struct vams_completion, error_code) == 0x06, "error");
_Static_assert(offsetof(struct vams_completion, bytes_processed) == 0x08,
               "processed");
_Static_assert(offsetof(struct vams_completion, result_crc) == 0x0c,
               "result crc");
_Static_assert(offsetof(struct vams_completion, user_cookie) == 0x10,
               "cq cookie");
_Static_assert(offsetof(struct vams_completion, device_timestamp) == 0x18,
               "timestamp");

int main(void)
{
    puts("VAMS ABI compile-time layout test: PASS");
    return 0;
}
