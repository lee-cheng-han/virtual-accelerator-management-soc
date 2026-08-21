# Firmware command scheduler

## Implemented pipeline

The Zephyr firmware owns a fixed pool of eight command objects. It performs no
command-path heap allocation. Five preemptible tasks transfer sole ownership
through bounded pointer message queues:

```text
receiver -> validation queue -> validator -> ready queue
         -> earliest-deadline scheduler -> running queue -> recovery manager
         -> completion queue -> publisher
```

The receiver reserves a slab object before acknowledging a portal submission,
captures all 64 descriptor bytes, reset generation, monotonic capture time, and
an increasing acceptance sequence, then changes `FREE` to `SUBMITTED`. The
validator applies the normative first-error rules and sends invalid work
directly to `COMPLETED_ERROR`; accepted work enters `QUEUED`.

The scheduler drains all currently ready objects and selects the earliest
absolute deadline. Acceptance sequence provides a stable FIFO tie-break. NOP
work completes locally. For payload work, the scheduler publishes an engine
authorization and transfers the still-`RUNNING` object to the single-entry
running queue. The recovery manager acknowledges the modeled engine result,
validates command ID and cookie, applies the terminal transition, and transfers
the object to the completion task. The completion task is the only owner
permitted to publish a terminal host result and return an object to the slab.

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

For running payload work, the recovery manager waits only until the absolute
command deadline. Expiry follows `RUNNING -> ABORTING`, publishes an explicit
`TIMED_OUT/TIMEOUT` abort request, and waits at most 100 ms for the engine
result. A valid acknowledgment completes with the returned terminal status. A
missing or mismatched acknowledgment increments a saturating escalation counter
and completes `FAILED/ENGINE`; neither path can retain the command object
indefinitely.

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

`make firmware-ownership-smoke` independently exercises result and abort
framing, firmware-final publication, engine-reset reconciliation, and
bridge-disconnect terminal recovery without requiring the Zephyr compiler.
It also verifies that management/watchdog-style reset notification cancels an
active modeled engine command and produces exactly one `RESET/RESET` CQ entry.

## Remaining recovery boundary

Firmware now owns payload commands through modeled-engine result validation and
terminal publication. Engine, queue, and device reset results are acknowledged;
the PCI endpoint discards reset-scope terminal replies after ownership is
reconciled. A bridge disconnect synthesizes one terminal host failure and
cancels modeled work. The high-priority recovery manager now owns bounded
running abort and escalation. Management/watchdog resets reconcile an active
portal command, and the host CQ has tested high/low watermark hysteresis.
Remaining work is an explicit firmware reset-acknowledgment deadline plus
bounded overload policy for the firmware slab, task queues, telemetry, and logs.
