# Bounded payload DMA

## Execution contract

The QEMU engine processes every payload command with a fixed 64 KiB working
chunk. `MEM_COPY` alternates one source read and destination write per chunk.
`MEM_FILL` captures its single source byte once, constructs one chunk at a
time, and writes each chunk. `CRC32` retains the reflected IEEE CRC state
across source chunks. `VECTOR_ADD` uses two 64 KiB buffers so source and
destination operands are captured for a chunk before its result is written.

The largest temporary allocation is therefore 64 KiB for copy, fill, and CRC,
or 128 KiB for vector addition, independent of the legal command length.
Commands retain the v1 maximum transfer of 16 MiB.

## Completion and failure semantics

Successful commands still publish `bytes_processed=length` only after every
chunk completes. A directional DMA or allocation failure publishes zero bytes;
v1 does not expose partial progress, and destination contents are unspecified
after any failed operation. CRC mismatch continues to return the computed CRC
with zero processed bytes.

Chunk processing currently occurs inside one engine callback. The QEMU main
loop therefore cannot service an abort between chunks. Persisted chunk state,
Nth-chunk fault injection, and named cancellation checkpoints remain future
work. Engine-epoch reset cancels the whole callback before it executes and does
not claim between-chunk preemption.

## Hardware-free validation

```sh
make payload-throughput-smoke \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
```

The smoke writes a deterministic 16 MiB source, copies the architectural
maximum transfer, and independently verifies the destination with CRC32. Fill
and vector commands cross the 64 KiB boundary by non-round lengths and verify
all data, source preservation, and adjacent guards.

The command also prints elapsed host-clock throughput for the 16 MiB copy.
That value is useful as local QEMU regression evidence only: it is not a
physical accelerator result, has no release threshold, and is invalid if any
integrity or completion check fails.
