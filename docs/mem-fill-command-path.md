# MEM_FILL command path

## Implemented scope

`MEM_FILL` is the first write-only payload operation. The source DMA address
names one readable byte containing the fill value. The destination names a
byte-aligned range from 1 byte through 16 MiB. Real Zephyr firmware validates
the captured descriptor before the host-facing QEMU endpoint accesses either
payload address.

Both addresses must be nonzero and the destination range must not overflow the
64-bit DMA address space. Source and destination may overlap because the engine
captures the single fill byte before writing the destination. Flags,
`expected_crc`, and reserved fields must be zero.

## Execution and completion

After successful firmware authorization, the endpoint independently validates
the descriptor, allocates a bounded private buffer, DMA-reads exactly one source
byte, fills the private buffer, and DMA-writes exactly `length` destination
bytes. The destination write completes before the completion is DMA-written,
the CQ tail advances, or MSI-X is raised.

Success reports `SUCCESS/NONE` with `bytes_processed=length`. Allocation or
independent validation failure reports `FAILED/ENGINE`, an inaccessible source
reports `FAILED/DMA_READ`, and an inaccessible destination reports
`FAILED/DMA_WRITE`. Every failure reports zero processed bytes and destination
contents are unspecified, matching the v1 all-or-nothing reporting contract.

The engine remains synchronous and single-command. Chunked asynchronous work,
deadlines, abort, and Linux userspace payload submission remain future work.

## Validation

```sh
make mem-fill-smoke \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32 \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
```

The dual-QEMU test fills an unaligned 4,111-byte destination, verifies every
byte and both adjacent guard regions, and checks the exact completion byte
count. It also covers zero length, destination overflow, a zero source, an
unmapped source, and an unmapped destination while retaining all NOP, reset, and
MEM_COPY regression coverage.
