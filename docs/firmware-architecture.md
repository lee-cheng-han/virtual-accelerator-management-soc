# Firmware architecture

The release-1 firmware is a Zephyr application for `vams_riscv` on one
RV32IMAC hart. It is the command-policy owner. Hardware owns DMA mechanics and
register side effects; the host owns buffer mapping and ring production.

The current firmware captures generated-ABI descriptors into an eight-object
slab and transfers sole ownership through receiver, validator,
earliest-deadline scheduler, recovery-manager, and completion tasks. It validates
every v1 opcode in fixed first-error order, publishes payload authorization,
validates the returned command ID and cookie, applies the terminal state, and
publishes the only host-visible result exactly once. The PCI model performs
authorized payload work and returns byte count, CRC, error, and reset status.

The current PCI model supplies the asynchronous execution boundary after
firmware authorization. It captures the command deadline and reset generation,
exposes BUSY, and returns success, failure, timeout, or reset through the result
portal. The recovery manager bounds waits by the command deadline, issues an
abort, waits 100 ms for acknowledgment, and escalates a missing or mismatched
result to a terminal engine failure.

## Boot and steady state

Boot ROM establishes stack/trap state and enters an SRAM image. Firmware clears
volatile state, reads reset reason/generation, initializes peripheral drivers,
creates fixed-size command/event objects, publishes firmware version, then sets
FW_RUNNING/READY. Dynamic allocation is forbidden after initialization. Before
READY, host doorbells remain latched or are rejected without DMA.

Stacks remain deliberately conservative. The health task now reports initialized
stack-scan high-water values for every running service, linked static SRAM,
command-pool and queue occupancy, and watchdog margin. Those values are retained
as qualification evidence before any stack is reduced. Zephyr cooperative
priority is negative and preemptible priority is nonnegative; lower numbers run
first.

| Task | Priority | Stack | May block on | Responsibility |
|---|---:|---:|---|---|
| Recovery manager | preemptible 0 | 1536 B | running queue; result/abort portal | Validate results; enforce deadlines; request abort; count escalation. |
| Command receiver | preemptible 1 | 1536 B | doorbell semaphore, descriptor DMA completion | Fetch complete descriptors; advance SQ head only after capture. |
| Validator | preemptible 2 | 2048 B | command-input queue, output capacity | Apply fixed validation order; build rejection or accepted command. |
| Scheduler | preemptible 3 | 1536 B | ready queue, engine slot | EDF by absolute timeout, FIFO tie-break; dispatch one command. |
| Completion service | preemptible 2 | 1536 B | result queue, CQ-space semaphore | DMA-write completion, publish CQ tail, request interrupt. |
| Watchdog service | preemptible 4 | 1024 B | periodic timer | Check task progress epochs and pet hardware watchdog. |
| Telemetry service | preemptible 5 | 1536 B | periodic timer/snapshot request | Aggregate counters and atomically publish snapshots. |
| Logging service | preemptible 7 | 1536 B | bounded log queue/UART | Export structured records; drop rather than block producers. |

Telemetry and logging rows describe the target decomposition; the
current build combines telemetry with watchdog health and uses synchronous UART
logging. The recovery manager and completion task keep the validator and
scheduler out of the terminal publication path.

## Objects and ownership

Firmware allocates a fixed pool of eight command
objects. Each object has exactly one owning task at a time; transfer through a
Zephyr `k_msgq` moves ownership. Receiver owns captured bytes, validator owns
VALIDATING, scheduler owns QUEUED, the recovery manager owns RUNNING/ABORTING while
waiting for the portal, and completion service owns terminal result
publication. Recovery ownership across reset/watchdog scope now uses terminal
portal notifications rather than taking other owners' locks;
management/watchdog reset is held pending until the bridge acknowledges the
tagged terminal result or a separate 100 ms deadline expires. Only then does
the reset generation advance.

| Shared object | Synchronization | Rule |
|---|---|---|
| ISR event bits | atomic bits + `k_sem_give` | ISR sets/acks; corresponding task atomically exchanges bits. |
| Task queues | bounded `k_msgq` | Thread-only; producer never holds another lock while posting. |
| Command pool | Zephyr `k_mem_slab` | Receiver allocates, completion frees; eight fixed objects and no heap allocation. |
| SQ/CQ indices | single writer + atomic/read barrier | Receiver alone writes SQ head; completion alone writes CQ tail. |
| Engine registers | DMA manager alone | Recovery requests abort through an event; direct emergency reset is hardware-mediated. |
| Telemetry live counters | atomics for ISR/HW values; service-owned otherwise | Saturating update; snapshot seqlock prevents torn 64-bit values on RV32. |
| UART log queue | lock-free/bounded Zephyr queue | Log calls never wait; drop counter increments on overflow. |

## Interrupt path

Peripheral ISRs read source, W1C only handled bits, capture minimal status into
atomic event bits, give the owning task semaphore, and return. They may access
only interrupt status/ack registers, atomic event words, timestamp counter, and
ISR-safe semaphore APIs. They may not touch descriptors, command objects, DMA
buffers, mutexes, UART, or CQ memory. Level sources are masked by their task if
draining might be prolonged and unmasked after state is consistent.

Receiver/DMA waits are event-driven with absolute timeout rechecks, so a stale
or spurious event is harmless. Interrupt-to-thread latency contributes to the
command timeout and telemetry.

## Scheduling and timeout

One engine slot makes execution nonpreemptive except abort. Accepted commands
are ordered by earliest absolute deadline, with acceptance sequence as stable
tie-break. This avoids a short-timeout command waiting behind later long-deadline
work but cannot preempt a running DMA. Validator rejects a command whose legal
timeout already cannot be represented by the monotonic timer.

Scheduler checks expiry before dispatch. DMA manager arms an absolute deadline,
then issues the operation. Timeout posts recovery; recovery requests abort and
waits a fixed 100 ms abort grace. An acknowledged abort yields a timed-out
completion. Failure escalates to engine reset. Payload output is unspecified on
failure and firmware never reports partial bytes.

## Locking and priority safety

Thread lock order is lifecycle gate → command-pool mutex → telemetry snapshot
lock. Queue operations occur outside those locks. Engine ownership is message
based, not locked. No task waits for DMA, CQ space, a queue, or UART while
holding a mutex. Zephyr mutex priority inheritance protects the short pool and
snapshot critical sections. Recovery never waits for a lower-priority task
while preventing it from running; it posts quiesce, then waits with a bounded
timeout and yields.

The receiver inspects portal ownership before allocating and never acknowledges
a descriptor unless a command object is available. A full slab therefore
defers capture without transferring ownership. Internal command handoffs use
nonblocking puts: because every downstream queue can hold the entire eight-item
command pool, failure violates a capacity invariant and fails fast instead of
deadlocking. Completion publication has a 100 ms bound and requests management
reset on expiry; watchdog expiry is the one-second reset backstop. The PCI queue
controller additionally applies host-programmed high/low CQ watermark
hysteresis before the host ring becomes full.

## Watchdog and logging

Every essential task advances a progress epoch at a defined loop boundary.
The health policy checks producer, monitor, mailbox, receiver, validator,
scheduler, recovery, and completion epochs every 100 ms after startup grace.
Two consecutive misses latch the stable lowest stuck-task ID, increment a
saturating failure-episode counter, emit a structured event, and withhold the
watchdog pet. A test-only selector can freeze each epoch independently.
Hardware expiry performs management reset, increments generation/reset counter,
and causes the driver to discard prior-generation requests.

Before warm reset, firmware mirrors a bounded record into 128 bytes of modeled
retained SRAM. Its magic, version, length, and IEEE CRC protect boot/reset
metadata, stuck-task evidence, last command/generation, reserved assertion and
stack-failure fields, and four recent events. Invalid, partial, corrupt, or old
records are reformatted without trusting their contents.

Runtime diagnostic producers write fixed-size events to a bounded queue with
`K_NO_WAIT`; a lower-priority logger owns UART formatting. Queue saturation
drops only diagnostics and increments a saturating counter, so logging is never
part of command correctness. Events contain an ID, command ID when valid,
generation, timestamp, and numeric context, with no host buffer data or raw
pointers. The later unified-observability work adds severity, engine epoch,
clock-domain metadata, and cross-layer schema/versioning.

Heartbeat samples use the same nonblocking/drop-accounted policy. Management
telemetry is written directly to fixed MMIO registers and allocates no queue or
buffer. Resource evidence reports admission deferrals, heartbeat and event
drops, internal queue overloads, and completion-portal stalls as saturating
counters.
