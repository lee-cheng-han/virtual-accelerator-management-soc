# Assurance for implemented components

## Executable invariants

The current QEMU endpoint fails immediately when a critical ownership or queue
condition is violated. Checks run around bridge ownership transfer, engine
scheduling/completion/cancellation, CQ publication, queue/device reset, status
observation, and cold reset.

The implemented invariant set requires:

- configured SQ/CQ depths remain supported powers of two and their indices
  remain strictly within those depths;
- firmware-bridge ownership, stale-result draining, and engine ownership are
  mutually exclusive;
- `DEVICE_STATUS.ENGINE_BUSY` is equivalent to engine ownership;
- an active engine callback carries the current reset generation and positive
  deadline/finish timestamps;
- reset cancellation clears engine ownership and BUSY before a new generation
  can accept work.

These checks cover the command states that exist today. Exactly-once completion
counters across a future multi-object firmware scheduler, buffer-mapping
lifetime assertions, and engine-epoch checks will be added with those
components.

## Descriptor fuzz regression

`qemu/tests/fuzz/vams-descriptor-fuzz.py` writes raw 64-byte descriptors through
the real SQ DMA path and checks returned ID, cookie, status, error, byte count,
and CRC. Its structured mutations cover all currently implemented first-error
classes across NOP, MEM_COPY, MEM_FILL, CRC32, and VECTOR_ADD:

- version, opcode, legal flags, conditional expected CRC, and every reserved
  field;
- timeout and opcode-specific length;
- zero, unused, misaligned, overflowing, and overlapping addresses.

The default run executes 4,096 cases across 28 categories. Failure output
contains the seed, iteration, category, complete descriptor hex, expected and
actual result, and QEMU diagnostics.

## BAR sequence fuzz regression

`qemu/tests/fuzz/vams-bar-fuzz.py` generates malformed byte, word, dword, and
qword reads/writes across the complete 4 KiB BAR. It mixes aligned and unaligned
offsets, reserved fields, queue controls, doorbells, interrupt registers, and
reset requests. Every 32 operations it verifies immutable identity, evaluates
the device-status invariants, and advances virtual time.

Failure output includes the seed, failing iteration, QEMU diagnostics, and the
last 32 operations as a directly replayable sequence. The current target is a
deterministic generational regression rather than a coverage-guided libFuzzer
binary; coverage-guided descriptor and MMIO harnesses remain CI work.

## Commands and sanitizers

```sh
make assurance-smoke \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
```

Seeds and counts are configurable:

```sh
make fuzz-smoke \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64 \
  VAMS_DESCRIPTOR_FUZZ_SEED=0xd35c0123 \
  VAMS_BAR_FUZZ_SEED=0xba4f0223 \
  VAMS_FUZZ_ITERATIONS=4096
```

The same targets accept an ASan/UBSan-built QEMU binary and inherit sanitizer
environment variables:

```sh
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
make fuzz-smoke QEMU_SYSTEM_X86_64=/path/to/sanitized/qemu-system-x86_64
```

`make source-check` compiles every Python test/helper, syntax-checks repository
shell tests, and rejects whitespace errors. `make check` retains generated ABI,
raw little-endian layout, and warning-clean GCC/Clang compile checks.
