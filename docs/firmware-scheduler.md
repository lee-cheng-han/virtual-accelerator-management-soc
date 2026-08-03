# Firmware command scheduler

## Implemented pipeline

The Zephyr firmware owns a fixed pool of eight command objects. It performs no
command-path heap allocation. Four preemptible tasks transfer sole ownership
through bounded pointer message queues:

```text
receiver -> validation queue -> validator -> ready queue
         -> earliest-deadline scheduler -> completion queue -> publisher
```

The receiver reserves a slab object before acknowledging a portal submission,
captures all 64 descriptor bytes, reset generation, monotonic capture time, and
an increasing acceptance sequence, then changes `FREE` to `SUBMITTED`. The
validator applies the normative first-error rules and sends invalid work
directly to `COMPLETED_ERROR`; accepted work enters `QUEUED`.

The scheduler drains all currently ready objects and selects the earliest
absolute deadline. Acceptance sequence provides a stable FIFO tie-break. One
engine slot is modeled, so accepted authorization work transitions through
`RUNNING` to `COMPLETED`. The completion task is the only owner permitted to
write the firmware portal result and return an object to the slab.

## Executable invariants

Every transition supplies both the required old state and new state to a
central assertion. A wrong owner, duplicate queue delivery, or illegal state
therefore fails the firmware immediately. Publication additionally requires a
terminal state and a zero publication count; the count becomes one before the
object can return to `FREE`.

Structured transition records contain event type, acceptance sequence, command
ID, reset generation, and numeric old/new states. Publication records contain
the same identity plus the publication count. They never contain a DMA buffer,
native pointer, or payload byte.

| Value | State |
|---:|---|
| 0 | `FREE` |
| 1 | `SUBMITTED` |
| 2 | `VALIDATING` |
| 3 | `QUEUED` |
| 4 | `RUNNING` |
| 5 | `ABORTING` |
| 6 | `COMPLETED` |
| 7 | `COMPLETED_ERROR` |
| 8 | `CANCELLED` |

## Deadline and recovery behavior

`timeout_ms=0` selects 30 seconds. Other validated timeouts become absolute
firmware deadlines at capture. The scheduler rechecks generation and deadline
immediately before dispatch. Expired queued work follows
`QUEUED -> ABORTING -> COMPLETED_ERROR` and publishes
`TIMED_OUT/TIMEOUT` without authorizing QEMU payload access. A generation
mismatch becomes `CANCELLED` with `RESET/RESET`.

The test-only `CONFIG_VAMS_SCHEDULER_TEST_DELAY_MS` defaults to zero and is not
a host-visible capability. Its isolated build delays scheduler dispatch long
enough to exercise expiry deterministically.

```sh
make scheduler-recovery-smoke \
  CROSS_COMPILE=/path/to/riscv64-unknown-elf- \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32 \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
```

The dual-QEMU test requires the complete timeout state trace, exactly one
firmware publication, `TIMED_OUT/TIMEOUT` in the host CQ, and a clean following
NOP with its own exactly-once trace.

## Remaining recovery boundary

Firmware currently authorizes the hardware-model payload engine and receives
no running-command result event back from it. Consequently, persisted
mid-command state, firmware abort acknowledgment, disconnect reconciliation,
and final DMA telemetry remain future work. The QEMU endpoint independently
implements engine-only reset with a private epoch, terminal reset completion,
queued-work preservation, and stale-callback suppression. Queue/device reset
generation suppression and firmware queued timeout complete the currently
executable recovery hierarchy without claiming a firmware running-result
protocol.
