# Milestone roadmap

Each phase begins by auditing the prior gate, listing changed files and risks,
and ends with tests, exact commands/results, limitations, and a commit-message
suggestion. Commit messages describe the delivered engineering change and never
the phase or milestone number. A later phase may not make an earlier gate flaky.

| Phase | Deliverable and acceptance gate |
|---:|---|
| **0 — Specification** | This design package, skeleton, and `make check`; no implementation. |
| **1 — Minimal RISC-V VM** | QEMU subsystem, ROM/SRAM/UART/timer/IRQ, bare-metal image, expected four-line boot output, automated boot smoke. |
| **2 — Zephyr port** | `vams_riscv` board/devicetree/Kconfig, console/tick, two tasks communicate and heartbeat under test. |
| **3 — Management peripherals** | Mailbox, watchdog, reset, telemetry plus Zephyr drivers; structured event records, task progress epochs, mailbox and watchdog-reset integration tests. |
| **4 — PCIe endpoint** | Enumeration, BAR0 identity/capabilities, MSI-X test, thin `vams_pci` probe/remove and cleanup tests. |
| **5 — Command path** | One SQ/CQ, generated v1 ABI definitions, NOP validation/completion, explicit DMA-memory ordering, interrupt plus polling, reference queue model and ID/cookie round trip. |
| **6 — DMA/engine** | Copy, fill, CRC32, vector add; overflow/length/timeouts; state invariants, descriptor fuzz target, data-integrity and throughput smoke. |
| **7 — Scheduling/recovery** | Queued/running states, timeout, engine/queue/device reset, reset generation, structured errors, reset-race tests and stale-callback assertions; later commands remain sound after forced timeout. |
| **8 — Fault injection** | At least five deterministic debug faults plus named race-window checkpoints, counter, expected recovery, regression and clean post-fault command each. |
| **9 — Stress/performance** | Model/property queue tests, ring wrap, million-command and long-duration runs, repeated reset, histogram, throughput, stack/SRAM use, watchdog margin and queue high-water report. |
| **10 — CI/demo** | Pinned reproducible builds, compatibility matrix, coverage/static analysis/fuzzing and layered tests in CI; unified trace export; `make demo` boots, submits, verifies, faults, recovers, and reports PASS/FAIL. |

Current gate: the minimal RISC-V VM is implemented and accepted by the
firmware build, clean QEMU patch check, and automated boot smoke test. The
Zephyr port is the next implementation gate.

Release 1 is complete only after Phase 10. Multiple queues, management cores,
IOMMU emulation, signing/update schemes, SR-IOV, and power management require a
post-release design revision; they are not hidden Phase 0 commitments.

## Dependency order and risks

The QEMU RISC-V subsystem precedes Zephyr because its timer/interrupt contract
defines the board port. Management peripherals precede PCIe queues so firmware
health can be observed while the host path is brought up. NOP precedes payload
DMA so descriptor ordering and completion congestion can be validated alone.

Highest risks are maintaining a QEMU patch against a pinned upstream version,
correct asynchronous DMA cancellation on reset, Zephyr timer/IRQ compatibility,
and host-memory lifetime during process exit. Each receives a focused test at
the first phase where it exists. Toolchain and QEMU revisions will be pinned in
that phase rather than guessed in Phase 0.

## Release-quality engineering tracks

These are acceptance work, not optional feature expansion. Each track begins at
the first milestone that has enough implementation to test it and remains part
of every later regression gate.

### Generated contract and traceability

A machine-readable ABI source becomes authoritative before the command path is
implemented. It defines registers, fields, descriptors, opcodes, status values,
and errors, and generates project-owned C headers plus documentation tables for
QEMU, firmware, kernel, and userspace. Generated output is checked for a clean
working tree in CI. Every normative requirement receives a stable ID linked to
its design section, implementation owner, test case, CI result, and limitation.
The generator must preserve compile-time size/offset assertions and raw
little-endian layout tests; it must not hide handwritten behavioral logic.

### Executable state invariants

Debug/test builds assert that a command has exactly one owner, produces at most
one completion, and never appears in two firmware queues; indices never cross
producer ownership; CQ entries are never overwritten; payload DMA never begins
before complete validation; buffers remain mapped for device ownership; and a
pre-reset callback cannot publish into a later generation or engine epoch.
Invariant violations fail tests immediately and include enough state to replay
the sequence.

### Reference and property models

A small implementation-independent SQ/CQ state model is introduced with the
first NOP path. Generated sequences configure, enable, submit, consume,
complete, mask/unmask, poll, wrap, reset, and inject faults, comparing every
observable QEMU transition with the model. Failed randomized runs print and
retain a deterministic seed and minimized operation sequence.

### Memory-order verification

Tests independently delay and observe host release before SQ publication,
device acquire before fetch, descriptor capture before SQ-head advancement,
payload completion before result construction, CQ DMA visibility before tail,
tail before MSI-X, and host acquire before CQ consumption. Documentation and
code name the exact Linux/QEMU/firmware barriers used; a working interrupt must
not be accepted as proof that memory ordering is correct.

### Race-window fault control

In addition to one-shot faults, debug builds gain deterministic pause/release
checkpoints after descriptor fetch, before SQ-head update, after payload start,
after completion DMA, before CQ-tail publication, and before interrupt delivery.
Tests can fail the Nth DMA chunk or freeze a named firmware task at a named
progress epoch. Checkpoints are never production capabilities and tests use
explicit handshakes rather than timing sleeps.

### Unified observability

Firmware, QEMU, kernel, and userspace emit stable structured events containing
event ID, severity, command ID when valid, reset generation, engine epoch,
state transition, timestamp/clock domain, queue indices, DMA operation ID, and
numeric error context. `vamsctl trace` merges them into a command timeline and
can emit human-readable or JSON output. Logging remains nonblocking and reports
drops; no host buffer contents or native pointers enter logs.

### Resource and worst-case evidence

Measurements cover per-task stack high-water, static/runtime SRAM, command-pool
and queue occupancy, maximum queue and interrupt-to-thread latency, watchdog
margin, engine utilization, recovery duration, log drops, and telemetry
saturation. Every configured stack, pool, and control timeout is justified by
measured worst case plus stated margin, rather than average performance.

### Robustness, compatibility, and analysis

Fuzz targets cover raw descriptors, BAR access sequences, mailbox parsing,
doorbell/reset/interrupt sequences, and firmware state events. Every discovered
failure adds a fixed corpus case. GCC/Clang warning-clean builds, ASan/UBSan,
QTest, Zephyr ztest, kernel W=1/Sparse, targeted Coccinelle, branch coverage of
validation/recovery, and focused mutation testing become CI gates when their
component exists.

Compatibility testing covers new host with older firmware, v1 descriptors with
newer firmware, unknown capability bits, newer interface minor versions,
unsupported major versions, version zero during firmware boot, and reset during
negotiation. No command is automatically replayed across an uncertain reset.

### Reliability qualification

The release candidate must complete at least one million mixed commands with
queue wraparound, thousands of deterministic resets, concurrent-process and
process-exit stress, and an extended fault-injection run without stale/duplicate
completion, leaked DMA mapping, invariant violation, sanitizer finding, or
unexplained watchdog reset. Exact duration/counts, seeds, host configuration,
raw results, failures, and firmware resource high-water values are checked in
as machine-readable evidence; a correctness failure invalidates performance
results.

### Reproducibility and diagnostics

QEMU, Zephyr SDK, compilers, Linux kernel, guest image, and build environment
are pinned with artifact hashes. After dependencies are prepared, core builds
and tests run without network access. `vamsctl` eventually provides `info`,
`health`, `queue-status`, `submit`, `fault`, `reset`, `trace`, and `self-test`
commands with consistent text and JSON output.

## Post-release improvements

Only after every release gate is stable should the project consider
scatter/gather DMA, multiple queue pairs and MSI-X vectors, a local-memory BAR,
quiesced live migration, IOMMU-translated DMA testing, hardware performance
counters, firmware update/security lifecycle, SR-IOV/PASID isolation, and power
management. Each requires an ABI design revision and its own failure model; none
may be added merely to increase the feature count.

The strongest optional validation is a companion FPGA or RISC-V development
board implementation that reuses the same register/descriptor ABI and Zephyr
command firmware. The QEMU and hardware targets should run a shared conformance
suite, with physical DMA/interrupt timing and logic-analyzer evidence clearly
separated from virtual measurements. This is a post-release qualification goal,
not a dependency for completing the virtual platform.
