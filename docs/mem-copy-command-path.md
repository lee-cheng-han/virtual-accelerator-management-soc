# MEM_COPY command path

## Implemented scope

`MEM_COPY` is the first payload operation. The host-facing QEMU endpoint
DMA-fetches the 64-byte submission and sends it through the private dual-QEMU
bridge. Real Zephyr firmware applies the normative validation order and returns
an authorization result. Only a successful firmware result permits the PCI
model to touch payload memory.

The current engine is deliberately synchronous and single-command. It supports
lengths from 1 byte through 16 MiB and byte-aligned, nonzero, distinct,
non-overlapping source and destination ranges. Vector add,
scatter/gather, concurrent engine work, and Linux userspace payload submission
are not implemented.

## Validation and ownership

Firmware validates descriptor version, implemented opcode, flags, reserved
fields, timeout, length, 64-bit range overflow, nonzero addresses, and numeric
range overlap without reading host payload memory. The endpoint independently
rechecks the command before DMA as a hardware safety boundary.

After firmware authorization, the endpoint owns the command and its captured
descriptor. It allocates at most the advertised 16 MiB transfer limit,
DMA-reads the entire source into a private buffer, then DMA-writes the entire
destination. The private buffer is released before CQ publication. The source
and destination addresses are fixed-width DMA values; neither firmware nor an
ABI structure contains a native pointer.

Successful completion reports `SUCCESS/NONE` and `bytes_processed=length` only
after the destination DMA write succeeds. Allocation failure reports
`FAILED/ENGINE`; source DMA failure reports `FAILED/DMA_READ`; and destination
DMA failure reports `FAILED/DMA_WRITE`. Failed operations report zero processed
bytes. As specified by the ABI, destination contents after a failed write are
unspecified, but no partial success is reported.

## Ordering and reset

SQ DMA capture completes before SQ-head advancement. Firmware authorization
precedes all payload access. Source DMA completes before destination DMA begins,
and destination DMA completes before the completion is written, CQ tail is
advanced, or MSI-X is raised. Existing queue-reset logic discards a late
firmware result from the prior generation before accepting new bridge work.

The synchronous implementation is a correctness baseline, not a throughput
claim. Later engine work will introduce chunked asynchronous DMA, deadlines,
abort, telemetry, and deterministic race checkpoints without changing the v1
descriptor contract.

## Validation

```sh
make mem-copy-smoke \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32 \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
```

The test uses a 4,097-byte deterministic pattern and unaligned byte addresses,
checks the exact destination, verifies guards immediately before and after the
range, and requires the exact completion byte count. It also proves rejection
of zero length, 64-bit range overflow, a zero address, and overlapping ranges.
Unmapped source and destination cases require directional `DMA_READ` and
`DMA_WRITE` failures with zero processed bytes. The same run retains
valid/invalid NOP and stale-completion reset coverage.
