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

Current gate: command transport is in progress. The generated v1 ABI, coherent
SQ/CQ allocation, checked doorbells, QEMU PCI DMA ordering, NOP validation and
completion, MSI-X draining, paired queue reset, and Linux guest round trip are
implemented. The gate remains open until queue events and validation are owned
by the embedded RISC-V firmware, driver request tracking/polling exists, and the
public host API completes an ID/cookie round trip.

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

## Industry-grade extension roadmap

The virtual Release 1 demonstrates the architecture and its failure handling.
The following tracks turn that reference platform into a secure, reproducible,
and supportable virtual firmware product. Every required acceptance gate runs
on QEMU or ordinary host-side test infrastructure; access to an FPGA, physical
RISC-V target, PCIe card, or manufacturing fixture is never required. Work
should proceed in the order below unless a documented risk assessment justifies
an exception.

### Virtual platform completeness and portability

Keep platform-independent firmware separate from QEMU-specific board and device
integration. Complete the virtual PCIe endpoint, DMA, interrupt, timer, reset,
watchdog, telemetry, and persistent-flash models, and ensure that all externally
observable behavior is defined by the versioned ABI rather than host timing or
QEMU implementation details.

The conformance suite covers enumeration, every normative register, descriptor
byte order, DMA visibility, interrupt masking, reset generation, watchdog
recovery, malformed inputs, persistent-state corruption, and version
negotiation. Deterministic test controls model cold/warm boot, power loss,
brownout-equivalent interruption, delayed DMA, interrupt races, clock variation,
and persistent-storage failures without relying on wall-clock sleeps.

Acceptance requires reproducible firmware and guest images, pinned tool
versions, documented virtual-machine topology, and repeatable clean recovery
from a forced command timeout, firmware crash, PCIe reset, QEMU termination, and
host-process exit. Performance results are explicitly labeled as virtual-model
measurements and are used for regression detection, not physical-device claims.

### Secure boot and firmware-update lifecycle

Start with an explicit threat model covering malicious descriptors and DMA,
host compromise, image tampering, rollback, interrupted update, debug access,
key exposure, denial of service, and recovery abuse. Define trust boundaries and
which threats the device can and cannot resist before selecting cryptography or
changing the ABI.

Add an immutable or protected root of trust, authenticated boot, signed firmware
manifests, image version and compatibility checks, measured image identity,
anti-rollback state, and an atomic A/B or equivalent update scheme. An update
must survive power loss at every erase/write/metadata transition and either boot
the prior valid image or enter a constrained authenticated recovery path. Failed
authentication must never execute untrusted code or destroy the last bootable
image.

Define development, provisioning, production, recovery, and decommissioning key
states. Private signing keys remain outside source control and build artifacts;
per-instance secrets use an auditable provisioning process. Production-mode
debug access is disabled or authenticated, sensitive material is zeroized where
possible, and reset/decommission flows erase user and device credentials.
CI adds image-tamper, wrong-key, rollback, compatibility, interrupted-update,
metadata-corruption, key-rotation, and recovery-abuse tests. Releases include a
signed manifest, SBOM, provenance, reproducible-build evidence, vulnerability
response policy, and documented security-support lifetime.

### Provisioning and service diagnostics

Provide a provisioning mode that is cryptographically gated and cannot be
entered accidentally from normal guest operation. It assigns and verifies a
unique virtual-device identity, records device-model and firmware revisions,
provisions keys and policy, locks production-mode configuration, and emits a
signed or otherwise tamper-evident result tied to the instance identity.

Implement deterministic built-in tests for ROM/RAM, nonvolatile storage,
firmware image integrity, PCIe link and configuration space, DMA paths,
interrupts, queues, watchdog/reset sources, clocks, telemetry sensors, and every
implemented engine. Tests must report stable numeric failure codes and preserve
enough bounded evidence for diagnosis without exposing secrets. Host-side
loopback and known-answer tests replace external fixtures.

Provisioning and support tooling must provide batch-safe text/JSON output, host
and software version capture, retry policy, audit logs, pass/fail limits, and
result-database export. Golden-image checks and deliberately faulty device-model
configurations validate detection of missing devices, corrupt memory, broken
interrupts, failed storage, and provisioning errors. Normal self-test remains
non-destructive and clearly separate from privileged provisioning operations.

### Production reliability and qualification

Promote the existing virtual stress evidence into a written qualification plan
with run counts, duration, resource limits, pass/fail criteria, issue disposition,
and reproducible raw results. The plan covers long-duration mixed traffic, queue
saturation, reset storms, repeated firmware updates, simulated power loss during
boot/update/DMA, host crashes, surprise removal, clock variation, corrupted and
exhausted persistent storage, memory pressure, and watchdog recovery. Tests
record QEMU and firmware revisions, machine topology, host, kernel, toolchain,
CPU/memory limits, seeds, and timing source.

Release candidates require zero unexplained hangs, corruptions, security-policy
bypasses, stale/duplicate completions, leaked DMA ownership, or unrecoverable
updates. Every failure receives a reproducible test or a documented rationale
for why one is impossible. Correctness failures restart the affected endurance
run; performance averages cannot hide outliers or recovery failures. Publish
latency distributions, throughput, reset/update recovery times, watchdog margin,
resource high-water marks, failure counts, host-load sensitivity, and known
limits. Maintain a requirements-to-evidence matrix and formal release sign-off
by firmware, device-model, driver, verification, security, and release owners.
The project makes no electrical, environmental, EMC, safety, or physical-device
reliability claims.

### Coding assurance and maintainability

Adopt documented C/C++ and HDL coding rules appropriate to the target market,
with mandatory review for security boundaries, reset paths, DMA ownership, and
update logic. Keep warning-clean GCC and Clang builds, static analysis, sanitizers
where executable, stack analysis, coverage of safety- and security-relevant
branches, dependency/license scanning, and justified suppression baselines.
Critical parsers and state machines receive fuzzing, property tests, and focused
manual review; generated code remains traceable to its schema and generator.

Define supported virtual-device revisions, ABI compatibility guarantees,
firmware and driver version matrices, release/rollback procedures,
long-term-support branches, backport policy, end-of-life policy, and reproducible
release archives. CI must build every supported combination or explicitly mark
it unsupported.

### Field operation and supportability

Add bounded crash records and reset-cause persistence, health counters, update
history, wear indicators, and privacy-reviewed telemetry. Diagnostic collection
must be rate-limited, versioned, safe under partial corruption, and unable to
leak host payloads, keys, native pointers, or unrelated memory. Operators need
documented procedures for health checks, update, rollback, safe reset, degraded
operation, log collection, RMA preparation, and disaster recovery.

Validate upgrade and downgrade paths across every supported release pair, and
run fleet-like canary, staged-rollout, rollback, and incompatible-host tests in
the lab. Define service-level targets for detection and recovery, security
advisory handling, critical fixes, and data needed to diagnose a returned unit.

### Advanced platform capabilities

After the virtual platform, security, provisioning, and qualification
foundations are stable, consider scatter/gather DMA, multiple queue pairs and
MSI-X vectors, a local-memory BAR, quiesced live migration, IOMMU-translated DMA,
modeled performance counters, SR-IOV/PASID isolation, and modeled power
management. Each item requires an ABI design revision, threat analysis, resource
budget, compatibility plan, deterministic failure injection, and its own
acceptance tests. Feature count alone is not a release criterion.

### Optional physical-target validation

An FPGA or development-board port may later reuse the ABI and firmware as a
separate project. If added, it should run the same conformance suite and clearly
separate physical DMA, interrupt, power, and timing evidence from virtual
measurements. It is optional evidence and is not on the critical path, is not a
release gate, and does not change the completion standard below.

### Completion standard

The industry-grade virtual extension is complete when the released firmware,
QEMU device model, Linux driver, and host tools have traceable requirements,
full virtual conformance, authenticated and interruption-safe update, controlled
provisioning, useful service diagnostics, reproducible long-duration
qualification evidence, and a maintained support policy. All required evidence
must be obtainable on ordinary development and CI hosts; physical hardware is
explicitly not required.
