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
| DMA timeout: arm fault bit 0, submit MEM_COPY | Next payload DMA never completes until firmware deadline; RUNNING→ABORTING. | timed-out counter + timeout completion; abort or engine reset; following NOP succeeds. |
| Dropped completion interrupt: bit 1 | Next CQ write/tail occurs but MSI-X transition is suppressed once. | CQ status stays pending; driver polling completes original cookie; no reset; fault count increments. |
| Invalid descriptor version: submit version 2 | Validator rejects before payload DMA. | INVALID/UNSUPPORTED_VERSION, rejected counter; next valid command succeeds. Protocol trigger, no debug bit. |
| SQ full: publish depth-1 descriptors while consumption paused by test harness | Additional nonblocking submit returns `-EAGAIN`; no overwrite/doorbell. | head/tail remain legal, high-water observed; draining restores progress. Protocol trigger. |
| Reset during active transfer: bit 5, submit a payload command | Hardware starts selected-scope reset after DMA becomes active; old callback suppressed. | generation and reset reason change; request gets local reset error/no stale CQ; reinit + NOP succeeds. |
| Engine hang: bit 2, submit any engine opcode | Engine remains BUSY until deadline; abort deliberately fails. | engine HUNG/error, timeout counter; engine reset; subsequent work remains correct. |
| Firmware task hang: bit 3 with ARG selecting service | Selected task stops advancing its epoch. | heartbeat/task diagnostic stalls, watchdog reset count/reason increments; management reboot and host queue rebuild. |
| Corrupt mailbox: bit 4 then send management message | Next message checksum/type is made invalid; command rings unaffected. | mailbox CORRUPT + FW error/log; message discarded; mailbox resynchronized or management reset if persistent. |

Each row becomes a regression test in the phase that implements its trigger.
Tests must arm, confirm armed state, perform exactly one triggering action,
observe `FAULT_STATUS/COUNT`, validate recovery, and execute a clean NOP. A test
must fail if the fault does not trigger or triggers twice.

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
