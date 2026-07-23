# CRC32 command path

## Implemented scope

`CRC32` computes the IEEE CRC-32 of a byte-aligned source range from 1 byte
through 16 MiB. The reflected polynomial is `0xedb88320`, with initial and final
XOR values of `0xffffffff`. The destination address must be zero because the
result is returned in the completion rather than written to payload memory.

Real Zephyr firmware validates the captured descriptor before the QEMU endpoint
accesses host memory. Source must be nonzero, its range must not overflow the
64-bit DMA address space, reserved fields must be zero, and the only permitted
flag is generated ABI bit `VAMS_SUB_F_VERIFY_CRC`.

## Result verification

Without the verification flag, `expected_crc` must be zero. A successful command
reports `SUCCESS/NONE`, `bytes_processed=length`, and the computed value in
`result_crc`.

With `VAMS_SUB_F_VERIFY_CRC`, firmware authorizes a nonzero or zero expected
value and the engine compares it after computation. A match has the ordinary
success result. A mismatch reports `FAILED/CRC_MISMATCH`, zero processed bytes,
and still returns the computed CRC for diagnosis. No payload contents enter logs
or ABI-owned state.

## DMA and ordering

The endpoint independently revalidates the firmware-authorized descriptor,
allocates at most 16 MiB, and DMA-reads the complete source before computing the
CRC. The buffer is released before completion publication. An allocation or
independent-validation failure reports `FAILED/ENGINE`; an inaccessible source
reports `FAILED/DMA_READ`. Failures other than CRC mismatch return zero CRC.

The implementation is synchronous and single-command. Chunked asynchronous
calculation, deadlines, abort, hardware acceleration, and performance claims
remain future work.

## Validation

```sh
make crc32-smoke \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32 \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
```

The dual-QEMU test compares a 4,123-byte deterministic input with Python's
independent standard-library CRC32, then covers matching and mismatching
expected values. It also checks unknown flags, unused expected CRC, nonzero
destination, zero length, source overflow, and unmapped source DMA while
retaining all prior NOP, reset, copy, and fill regressions.
