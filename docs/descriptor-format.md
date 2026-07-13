# Descriptor format

This document is normative for descriptor version 1. All fields are
little-endian integers in host DMA memory. Software must use endian conversion;
C layout alone does not define wire encoding.

## Submission descriptor

```c
#include <stdint.h>

#define VAMS_DESC_VERSION_1 1U

struct vams_submission {
    uint16_t version;          /* 0x00 */
    uint8_t  opcode;           /* 0x02 */
    uint8_t  flags;            /* 0x03 */
    uint32_t command_id;       /* 0x04 */
    uint64_t source_dma;       /* 0x08 */
    uint64_t destination_dma;  /* 0x10 */
    uint32_t length;           /* 0x18 */
    uint32_t timeout_ms;       /* 0x1c */
    uint64_t user_cookie;      /* 0x20 */
    uint32_t expected_crc;     /* 0x28 */
    uint32_t reserved0;        /* 0x2c */
    uint64_t reserved1;        /* 0x30 */
    uint64_t reserved2;        /* 0x38 */
};

_Static_assert(sizeof(struct vams_submission) == 64,
               "vams_submission must be 64 bytes");
```

Each ring entry and ring base is 64-byte aligned. Fields must remain at the
listed offsets; producers zero-initialize the whole descriptor. `command_id` is
opaque and may be zero, but the host driver must keep it unique among accepted,
uncompleted commands. A duplicate outstanding ID is rejected. IDs may be reused
after completion or reset. `user_cookie` is never interpreted or dereferenced;
every descriptor-derived completion returns all 64 bits unchanged, including
for validation errors, timeout, and cancellation.

`source_dma` and `destination_dma` are PCI DMA addresses, never CPU pointers.
For every used range, `base + length - 1` must not exceed `UINT64_MAX`.
`timeout_ms=0` selects the device default of 30000 ms; other legal values are
1–60000 ms. Time starts when firmware accepts the descriptor, includes queue
wait and execution, and is measured by the management monotonic clock. The
timeout does not include time waiting in host software before SQ publication.

Version 1 defines flag bit 0, `VAMS_SUB_F_VERIFY_CRC`, only for CRC32. It asks
firmware to compare the result with `expected_crc`. Bits 1–7 must be zero.
`expected_crc` must be zero unless that flag is set. All reserved fields must be
zero. Nonzero reserved data is an error, never silently ignored.

## Completion descriptor

```c
struct vams_completion {
    uint32_t command_id;       /* 0x00 */
    uint16_t status;           /* 0x04 */
    uint16_t error_code;       /* 0x06 */
    uint32_t bytes_processed;  /* 0x08 */
    uint32_t result_crc;       /* 0x0c */
    uint64_t user_cookie;      /* 0x10 */
    uint64_t device_timestamp; /* 0x18 */
};

_Static_assert(sizeof(struct vams_completion) == 32,
               "vams_completion must be 32 bytes");
```

The CQ base is 64-byte aligned; individual 32-byte entries are naturally
32-byte aligned. `device_timestamp` is firmware monotonic microseconds since the
current management boot. It may restart after management reset and is not a
wall clock. Successful commands have `status=SUCCESS`, `error_code=NONE`, and
`bytes_processed=length` (NOP uses zero). Validation errors have zero bytes and
CRC. Release 1 does not report partial completion: failures and cancellations
set `bytes_processed=0`, even if an engine touched data before abort. Callers
must treat destination contents as unspecified on any non-success completion.

```c
enum vams_completion_status {
    VAMS_STATUS_SUCCESS   = 0x0000,
    VAMS_STATUS_INVALID   = 0x0001,
    VAMS_STATUS_FAILED    = 0x0002,
    VAMS_STATUS_TIMED_OUT = 0x0003,
    VAMS_STATUS_CANCELLED = 0x0004,
    VAMS_STATUS_RESET     = 0x0005,
};

enum vams_error_code {
    VAMS_ERR_NONE                = 0x0000,
    VAMS_ERR_UNSUPPORTED_VERSION = 0x0001,
    VAMS_ERR_INVALID_OPCODE      = 0x0002,
    VAMS_ERR_INVALID_FLAGS       = 0x0003,
    VAMS_ERR_RESERVED_NONZERO    = 0x0004,
    VAMS_ERR_DUPLICATE_ID        = 0x0005,
    VAMS_ERR_INVALID_LENGTH      = 0x0006,
    VAMS_ERR_INVALID_ALIGNMENT   = 0x0007,
    VAMS_ERR_ADDRESS_OVERFLOW    = 0x0008,
    VAMS_ERR_INVALID_ADDRESS     = 0x0009,
    VAMS_ERR_INVALID_TIMEOUT     = 0x000a,
    VAMS_ERR_DMA_READ            = 0x0010,
    VAMS_ERR_DMA_WRITE           = 0x0011,
    VAMS_ERR_ENGINE              = 0x0012,
    VAMS_ERR_TIMEOUT             = 0x0013,
    VAMS_ERR_CRC_MISMATCH        = 0x0014,
    VAMS_ERR_RESET               = 0x0015,
    VAMS_ERR_QUEUE_RESET         = 0x0016,
    VAMS_ERR_DEVICE_FATAL        = 0x0017,
};
```

Unknown status/error values must be treated by newer host software as a generic
I/O failure while preserving the raw value for diagnostics.

## Opcode contract

```c
enum vams_opcode {
    VAMS_OP_NOP        = 0x00,
    VAMS_OP_MEM_COPY   = 0x01,
    VAMS_OP_MEM_FILL   = 0x02,
    VAMS_OP_CRC32      = 0x03,
    VAMS_OP_VECTOR_ADD = 0x04,
};
```

| Opcode | Buffers and alignment | Length/result | Errors and partial completion |
|---|---|---|---|
| NOP | Both addresses zero. | Length and expected CRC zero; completion bytes/CRC zero. | Invalid fields only; never partial. Still obeys timeout/reset. |
| MEM_COPY | Source and destination nonzero, byte-aligned, distinct non-overlapping numeric ranges. | 1–16 MiB bytes; exact byte copy. CRC zero. | Read/write DMA, overflow, timeout, reset; never partial. |
| MEM_FILL | Source points to one readable byte containing fill value; destination byte-aligned and nonzero. | 1–16 MiB destination bytes. CRC zero. | Source range is exactly 1 byte; write DMA, timeout, reset; never partial. |
| CRC32 | Source nonzero and byte-aligned; destination must be zero. | 1–16 MiB. IEEE CRC-32: reflected polynomial `0xedb88320`, initial/final XOR `0xffffffff`; result in `result_crc`. | Read DMA; optional mismatch returns FAILED/CRC_MISMATCH and computed CRC; never partial. |
| VECTOR_ADD | Source and destination nonzero and 4-byte aligned. Destination initially contains operand B and is overwritten by source A + B. Numeric ranges must not overlap. | 4–16 MiB, multiple of 4; little-endian `uint32_t` addition modulo 2^32. CRC zero. | Read/write DMA, alignment, timeout, reset; never partial. |

Except CRC32 with VERIFY_CRC, flags are zero. DMA addresses that QEMU cannot
access produce `INVALID_ADDRESS` if rejected before dispatch or the directional
DMA error after dispatch. No opcode permits scatter/gather in version 1.

## Validation order

Firmware captures all 64 bytes into private SRAM, then validates without
touching payload memory in this fixed order:

1. `version == 1`; otherwise UNSUPPORTED_VERSION.
2. opcode is defined; otherwise INVALID_OPCODE.
3. flags are legal for that opcode; otherwise INVALID_FLAGS.
4. reserved fields and conditionally unused expected CRC are zero; otherwise
   RESERVED_NONZERO.
5. command ID is not currently outstanding; otherwise DUPLICATE_ID.
6. timeout is 0 or 1–60000; otherwise INVALID_TIMEOUT.
7. opcode-specific length is within the rules; otherwise INVALID_LENGTH.
8. required/unused addresses and alignment are valid; otherwise
   INVALID_ADDRESS or INVALID_ALIGNMENT.
9. every used address range passes unsigned overflow checks; otherwise
   ADDRESS_OVERFLOW.
10. command resources are reserved and payload DMA may begin.

The first failure wins and generates one INVALID completion containing captured
ID and cookie. A descriptor-fetch DMA failure cannot safely recover ID/cookie;
it sets queue/DMA error and invokes queue recovery instead of fabricating a
completion. Lack of CQ space is backpressure, not a validation error.

## Version and reset policy

Major descriptor versions are incompatible layouts. Hardware exposes its
highest version, and release 1 accepts only exactly 1. A future compatible
extension must use currently reserved fields only after negotiating a new
version; v1 producers keep them zero and v1 consumers reject them nonzero.
Field meaning never changes within a version. Firmware must continue accepting
v1 if a future advertised version claims v1 compatibility through a capability
defined by that future specification.

Queue or stronger reset stops descriptor acceptance. Firmware attempts an
error completion for command-only recovery, but queue/engine/management/device
reset does not rely on CQ availability: accepted commands receive RESET status
only when the CQ remains operational; otherwise the driver completes its local
requests with reset error. Reset generation is the authoritative boundary.
Pre-reset asynchronous callbacks are discarded and may not publish entries.

