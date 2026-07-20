# Firmware architecture

The release-1 firmware is a Zephyr application for `vams_riscv` on one
RV32IMAC hart. It is the command-policy owner. Hardware owns DMA mechanics and
register side effects; the host owns buffer mapping and ring production.

The current standalone harness implements the first policy slice as one polling
command-service task: it captures a generated-ABI descriptor from the private
portal, acknowledges ownership, validates NOP, MEM_COPY, or MEM_FILL in fixed
first-error order, and publishes an authorization completion. The PCI model
performs the authorized payload operation and finalizes its byte count. The task
decomposition below remains the target for asynchronous scheduling, engine
monitoring, and recovery.

## Boot and steady state

Boot ROM establishes stack/trap state and enters an SRAM image. Firmware clears
volatile state, reads reset reason/generation, initializes peripheral drivers,
creates fixed-size command/event objects, publishes firmware version, then sets
FW_RUNNING/READY. Dynamic allocation is forbidden after initialization. Before
READY, host doorbells remain latched or are rejected without DMA.

Provisional stacks are deliberately conservative and must be replaced by
measured Phase 9 high-water data. Zephyr cooperative priority is negative and
preemptible priority is nonnegative; lower numbers run first.

| Task | Priority | Stack | May block on | Responsibility |
|---|---:|---:|---|---|
| Recovery manager | cooperative -1 | 1536 B | recovery event queue; bounded hardware ack | Serialize abort/reset and generation changes. |
| Command receiver | preemptible 1 | 1536 B | doorbell semaphore, descriptor DMA completion | Fetch complete descriptors; advance SQ head only after capture. |
| Validator | preemptible 2 | 2048 B | command-input queue, output capacity | Apply fixed validation order; build rejection or accepted command. |
| Scheduler | preemptible 3 | 1536 B | ready queue, engine slot | EDF by absolute timeout, FIFO tie-break; dispatch one command. |
| DMA manager | preemptible 2 | 2048 B | DMA event semaphore/timer | Program, monitor, abort engine; build result event. |
| Completion service | preemptible 2 | 1536 B | result queue, CQ-space semaphore | DMA-write completion, publish CQ tail, request interrupt. |
| Watchdog service | preemptible 4 | 1024 B | periodic timer | Check task progress epochs and pet hardware watchdog. |
| Telemetry service | preemptible 5 | 1536 B | periodic timer/snapshot request | Aggregate counters and atomically publish snapshots. |
| Logging service | preemptible 7 | 1536 B | bounded log queue/UART | Export structured records; drop rather than block producers. |

This adds a completion task to the minimum requested decomposition so CQ
backpressure cannot block validator or recovery. “DMA manager” owns both DMA and
the simple processing-engine transaction in release 1.

## Objects and ownership

Firmware allocates a fixed pool of at most `min(SQ usable capacity, 64)` command
objects. Each object has exactly one owning task at a time; transfer through a
Zephyr `k_msgq` moves ownership. Receiver owns captured bytes, validator owns
VALIDATING, scheduler owns QUEUED, DMA manager owns RUNNING/ABORTING, and
completion service owns terminal result publication. Only recovery may freeze
all owners, using events and acknowledgments rather than taking their locks.

| Shared object | Synchronization | Rule |
|---|---|---|
| ISR event bits | atomic bits + `k_sem_give` | ISR sets/acks; corresponding task atomically exchanges bits. |
| Task queues | bounded `k_msgq` | Thread-only; producer never holds another lock while posting. |
| Command pool | `k_mutex` | Receiver allocates, completion/recovery frees. Never used in ISR. |
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

Potential inversion remains when completion is blocked by a full CQ while
commands occupy the pool. The receiver applies a reserved-capacity watermark:
it stops SQ capture early enough that every accepted command has a command
object and a result slot. Watchdog/log/telemetry cannot consume these slots.

## Watchdog and logging

Every essential task advances a progress epoch at a defined loop boundary.
Watchdog pets only if required epochs changed within their service interval or
the task is in a declared bounded wait. A debug fault can freeze one epoch.
Hardware expiry performs management reset, increments generation/reset counter,
and causes the driver to discard prior-generation requests.

Structured logs contain event ID, severity, command ID when valid, generation,
timestamp, and two numeric arguments. They contain no host buffer data or raw
pointers. Logging is diagnostic and never part of correctness.
