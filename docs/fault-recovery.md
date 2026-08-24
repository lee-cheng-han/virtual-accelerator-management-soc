# Fault injection and recovery

Fault injection is a debug/test interface, gated by the `x-vams-debug` QEMU
property and lockable until cold reset. It must not be enabled or described as a
production PCI capability. One one-shot fault may be armed at a time.

## Recovery hierarchy

| Level | Trigger | Preserve | Discard | Host result / responsibility |
|---|---|---|---|---|
| Command | invalid descriptor, clean DMA error, successful timeout abort | Rings, other commands, engine | One command output | Consume error completion; decide retry. |
| Queue | illegal indices, descriptor fetch/CQ write failure, persistent CQ fault | Firmware, healthy engine, telemetry, configured bases/depth | Both ring indices and every accepted command | Stop submit, cancel generation, reset/re-enable pair. |
| Engine | hang or failed abort | Firmware, ring configuration, telemetry | Running command and engine-local state | Observe timeout/reset result; wait READY before continuing queued work. |
| Firmware | watchdog/task deadlock, corrupt internal state | PCI config, telemetry persistence, host allocations | SRAM state and every accepted command | Cancel old generation and rebuild queues after boot. |
| Device | fatal PCI/DMA state or lower-level recovery failure | PCI identity, persistent reset fields | BAR config state, queues, firmware, engine | Reinitialize as probe; fail device if READY deadline expires. |

Before queue or stronger reset, hardware blocks doorbells and new DMA, marks
RESETTING, tags/cancels asynchronous callbacks, and waits a bounded model-level
drain. It increments host-visible generation before exposing READY. Release 1
does not let queued commands survive queue, firmware, or device reset. An
engine-only reset instead increments a private engine epoch, cancels the running
callback, and preserves not-yet-dispatched commands after the scheduler checks
their host generations and deadlines; otherwise recovery escalates.

## Deterministic scenarios

| Scenario / trigger | Expected behavior | Evidence and recovery |
|---|---|---|
| DMA timeout: arm fault bit 0, submit MEM_COPY | Modeled finish is forced beyond its otherwise-successful deadline; payload is untouched. | `TIMED_OUT/TIMEOUT`, sticky evidence and count; following NOP succeeds. |
| Dropped completion interrupt: bit 1 | Next CQ write/tail occurs but MSI-X transition is suppressed once. | CQ status stays pending; driver polling completes original cookie; no reset; fault count increments. |
| Invalid descriptor version: submit version 2 | Validator rejects before payload DMA. | INVALID/UNSUPPORTED_VERSION, rejected counter; next valid command succeeds. Protocol trigger, no debug bit. |
| SQ full: publish depth-1 descriptors while consumption paused by test harness | Additional nonblocking submit returns `-EAGAIN`; no overwrite/doorbell. | head/tail remain legal, high-water observed; draining restores progress. Protocol trigger. |
| Engine hang: bit 2, submit any engine opcode | Engine remains BUSY/HUNG with no timer callback. | engine error evidence; engine-only reset returns RESET and subsequent work remains correct. |
| Payload DMA read failure: bit 3 | Selected payload read returns a modeled directional failure before memory access. | FAILED/DMA_READ, destination unchanged, next NOP succeeds. |
| Payload DMA write failure: bit 4 | Selected payload write returns a modeled directional failure before the write. | FAILED/DMA_WRITE, destination unchanged, next NOP succeeds. |
| Reset during active transfer: bit 5, submit a payload command | Queue reset begins immediately after BUSY asserts; callback is cancelled. | generation/reason change, no old CQ or payload write, reconfigure + NOP succeeds. |

Every implemented row is a regression test. Tests must arm, confirm armed
state, perform exactly one triggering action,
observe `FAULT_STATUS/COUNT`, validate recovery, and execute a clean NOP. A test
must fail if the fault does not trigger or triggers twice.

The asynchronous-engine baseline covers a natural ten-millisecond deadline
against a deterministic twenty-millisecond operation and queue reset after BUSY
assertion. It proves timeout-before-payload, timer cancellation, generation
suppression, and clean NOP recovery. Engine-only reset now provides a second
recovery path: it completes active work as `RESET/RESET`, advances a private
epoch without changing queue generation, preserves queued work, and rejects the
cancelled callback after its original deadline.

The debug-gated QEMU endpoint now implements all six PCI-local one-shot faults
above. `FAULT_ARG=N` selects the Nth matching event, with zero selecting the
next; only the triggering match changes sticky status and the saturating count.
The regression contrasts a suppressed CQ notification with a normal MSI-X PBA
transition, recovers a hung engine, proves directional DMA failures preserve the
destination, and reconfigures after forced queue reset. It executes a clean NOP
after every fault. Evidence and the debug lock survive device reset, while cold
reset clears both.

Two explicit checkpoints pause after engine activation but before execution,
and after payload completion but before CQ publication. Tests advance virtual
time while paused to prove forbidden progress, then release and validate the
original command. Firmware-task hang and mailbox-corruption injection remain
future cross-subsystem extensions rather than falsely advertised PCI faults.

The firmware scheduler additionally has a test-only dispatch delay that forces
a legal one-millisecond command to expire while `QUEUED`. The regression
requires `QUEUED -> ABORTING -> COMPLETED_ERROR`, exactly one timeout
publication, and a clean following NOP. This is deterministic queued recovery;
it does not replace the unimplemented running-command firmware abort protocol.

## Escalation policy

Recovery manager records the first cause and scope. A command abort receives
100 ms; engine reset receives 500 ms; firmware READY receives 5 s; device READY
receives 10 s. These are control-plane recovery limits, independent of command
timeout. Failure at a level escalates one step. Device recovery failure sets
FATAL, preserves first fatal code, masks bus mastering/interrupts, and requires
QEMU device recreation or operator action.

Stale completions are prevented by generation checks at every asynchronous
callback and before CQ publication. Telemetry records accepted/rejected,
timeouts, DMA errors, watchdog resets, last reason/error, high water, and
latency. Reset itself must never clear the evidence needed to diagnose why it
occurred, except cold destruction as specified in the register map.

Queue/device reset terminal transfer to firmware is independently bounded at
100 ms of virtual time. Firmware acknowledgment cancels the deadline. If it is
silent, the endpoint releases private bridge ownership, increments a saturating
timeout counter, raises firmware and queue errors, and accepts clean work after
queue reconfiguration. The test withholds the reply and advances the virtual
clock to 99 ms and then exactly to the deadline; it uses no wall-clock race.
