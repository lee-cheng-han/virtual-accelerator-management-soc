# VECTOR_ADD command path

## Implemented scope

`VECTOR_ADD` treats source as operand A and the initial destination contents as
operand B. Both buffers contain little-endian `uint32_t` elements. The engine
overwrites destination with `A + B` modulo 2³² while leaving source unchanged.

Length is 4 bytes through 16 MiB and must be a multiple of four. Both DMA
addresses must be nonzero and four-byte aligned, neither range may overflow the
64-bit address space, and the numeric ranges must not overlap. Flags,
`expected_crc`, and reserved fields must be zero.

## Ownership and execution

Real Zephyr firmware validates the complete captured descriptor before payload
access. After authorization, the QEMU endpoint independently validates it,
allocates two bounded private buffers, and DMA-reads the complete source and
destination operands. Only after both snapshots succeed does it calculate the
result and DMA-write the destination.

The implementation uses explicit little-endian loads and stores. Addition is
performed with `uint32_t`, making overflow defined modulo 2³². It never casts DMA
bytes to a native array or places native pointers in the ABI.

Success reports `SUCCESS/NONE` and `bytes_processed=length` after the destination
write completes. Allocation failure reports `FAILED/ENGINE`; failure reading
either operand reports `FAILED/DMA_READ`; destination publication failure
reports `FAILED/DMA_WRITE`. All failures report zero processed bytes, and
destination contents are unspecified after failure.

The engine remains synchronous and single-command. The current QTest memory map
does not expose a region that succeeds on DMA read but independently fails a
later write, so VECTOR_ADD directly covers both operand-read failures while the
existing copy/fill tests retain directional destination-write failure coverage.

## Validation

```sh
make vector-add-smoke \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32 \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
```

The dual-QEMU test processes 1,025 elements, including unsigned wraparound,
compares every result against an independent Python calculation, verifies the
source is unchanged, and checks destination guards. It also covers invalid
length, zero and misaligned addresses, overflow, overlapping ranges, and
unmapped source and destination operands while retaining every earlier command
and reset regression.
