# Asynchronous engine execution

## Implemented scope

Firmware authorization and payload execution are now separate ownership states.
After Zephyr returns a successful authorization for a payload opcode, the QEMU
endpoint captures the descriptor, completion template, absolute command
deadline, reset generation, and modeled engine finish time. It asserts
`DEVICE_STATUS.ENGINE_BUSY` and schedules a virtual-time callback instead of
executing payload DMA in the command-bridge receive path.

The correctness model uses a deterministic two-millisecond engine interval. It
is not a physical-device latency or performance claim. QTests advance the
virtual clock explicitly, so deadline and reset behavior does not depend on
host scheduling or sleeps.

## Deadlines and completion

`timeout_ms=0` selects the v1 default of 30,000 ms; explicit values retain their
validated 1–60,000 ms range. The absolute deadline begins when the PCI endpoint
captures the submission. If the deadline precedes the modeled finish time, the
timer publishes `TIMED_OUT/TIMEOUT` with zero bytes and CRC without touching
payload memory. Otherwise the selected payload operation executes and its
normal completion is published.

Only one payload command owns the engine. While it is active, SQ consumption
stops but host doorbells may publish additional descriptors within normal ring
capacity. Clearing BUSY and constructing the terminal result happen before CQ
DMA, tail publication, and interrupt delivery.

## Reset and migration safety

Queue and device reset synchronously delete the engine timer, clear BUSY, and
increment the reset generation while discarding the old command. The callback
also compares its captured generation immediately before execution and before
CQ publication. It cannot touch payload memory or publish a completion into a
later queue generation.

The engine timer, captured submission/completion, absolute deadline, finish
time, generation, and in-flight bit are included in QEMU migration state version
5. Live migration remains unsupported until a dedicated end-to-end migration
test exists, but serialized device state is not silently incomplete.

## Validation

```sh
make async-engine-smoke \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32 \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
```

The test submits a one-millisecond MEM_COPY against the two-millisecond engine
interval and requires a timeout with an untouched destination. It then submits
another copy, waits for BUSY without advancing virtual time, performs queue
reset, advances beyond the old timer, and proves that neither payload nor CQ was
modified. After queue reconfiguration, a clean firmware-owned NOP must complete.
