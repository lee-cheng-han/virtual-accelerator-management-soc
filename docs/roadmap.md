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

Current gate: reproducible CI and integrated demonstration. Deterministic fault
injection and core stress qualification are complete.
In addition to coherent PCI
SQ/CQ DMA and the Linux guest NOP round trip, the standalone management harness
now has a private ownership portal and generated firmware ABI. Zephyr captures,
validates, and completes valid and unsupported-version NOPs. The Linux driver
now exposes per-file registered DMA buffers, every v1 payload command,
asynchronous submit/wait, `poll`, concurrent request tracking, interrupt
fallback, reset-generation cancellation, and safe process-exit ownership. A deterministic
reference model compares QEMU ownership, backpressure, wraparound, interrupt,
error, completion, and reset state after every randomized operation. The
dual-QEMU integration now DMA-stages PCI submissions through the private portal,
runs real Zephyr validation, and DMA-publishes firmware completions. Basic
queue-reset stale-result suppression is tested. Firmware now validates every v1
payload opcode, and the QEMU engine performs exact checked PCI payload DMA with
guarded copy/fill/vector data integrity and independently verified CRC coverage.
Virtual-time asynchronous dispatch now exposes BUSY, enforces command deadlines
before payload access, and cancels reset-generation callbacks without stale CQ
publication. Executable queue/bridge/engine invariants and replayable raw
descriptor/BAR mutation regressions now cover the implemented QEMU surface.
Payload DMA uses bounded 64 KiB chunks, and a hardware-free smoke verifies
the full 16 MiB transfer with an independent CRC while reporting explicitly
virtual-model throughput. Zephyr owns an eight-object fixed pool and
four-task command pipeline with asserted states, earliest-deadline dispatch,
queued expiry, generation cancellation, exactly-once publication, and clean
post-timeout recovery. QEMU engine-only reset terminates active work exactly
once, advances a private epoch without changing host queue generation, preserves
queued work, and suppresses the cancelled callback. Queue and device recovery
remain generation-scoped. Firmware now acknowledges and validates running
results, owns terminal publication, reconciles engine reset, and converts bridge
disconnect into a terminal failure. The recovery manager now bounds result
waits, requests abort, waits 100 ms for acknowledgment, and counts/escalates a
missing result. Queue/device reset terminal ownership now has its own 100 ms
virtual-time acknowledgment deadline with saturating timeout evidence, error
reporting, migration state, and verified clean progress after expiry. Six debug-gated one-shot
faults now cover forced timeout, CQ-notification suppression, engine hang,
directional payload DMA failures, and reset after engine activation. Nth-match
selection, sticky/saturating evidence, a cold-reset-only lock, engine-start and
pre-CQ pause/release checkpoints, and a clean command after every recovery are
validated. Firmware-task and mailbox faults remain cross-subsystem extensions;
The hardware-free qualification now completes one million mixed commands,
1,000 deterministic queue resets, 61,650 ring wraps at 15/16 occupancy, and 24
hours of virtual time with no stale or duplicate completion. It publishes
host-clock latency distributions and machine-readable environment/evidence.
Firmware resource telemetry now measures linked SRAM, every service stack,
fixed-pool/queue high-water, and watchdog margin. The rebuilt image and loaded
dual-QEMU workload now have a recorded development baseline; worst-case runs on
the pinned release compiler remain qualification follow-up work. QEMU and
Zephyr revisions are now centralized;
the QEMU patch and resulting source transformations are content verified. CI
checks source/contracts and clean patch application on every change, while
main/manual runs build QEMU and execute model, fuzz, fault, and stress suites.
The offline `make demo` verifies pinned QEMU identities and then runs the real
firmware bridge, all payload operations, deterministic recovery, and queue
stress with per-stage logs and JSON evidence. Pinned reproducible Linux guest
artifacts, public cancel/reset control, unified trace export, and the fully
composed guest demo remain before Release 1 completion.

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

## Improvement implementation program

The following work is part of the roadmap rather than an informal wish list.
Assurance is applied when a component exists; future-component rows remain
explicitly planned and are not counted as implemented.

| Improvement | Applied to completed work | Remaining acceptance work |
|---|---|---|
| Executable state invariants | QEMU queue/engine invariants, active engine-epoch checks, firmware fixed-pool transitions, and Linux per-file mapping/request references and generation-tagged cancellation enforce fail-fast ownership. | Kernel-debugging validation of host invariants and recovery-manager escalation invariants. |
| Descriptor/register fuzzing | Replayable 4,096-case raw-descriptor and malformed-BAR regressions with seeds and failure traces. | Coverage-guided fuzzing, mailbox parser, interrupt/reset sequences, and permanent corpus minimization. |
| Real firmware scheduler | Zephyr uses an eight-object slab with receiver, validator, EDF scheduler, recovery-manager, and completion tasks connected by bounded queues. Payload commands remain firmware-owned through the terminal result. Descriptor capture defers without acknowledgment when the slab is full; internal handoffs are nonblocking capacity invariants; completion publication, queue/device reset acknowledgment, management/watchdog reset serialization, and recovery waits are bounded. The Linux driver programs tested CQ high/low watermarks and retains command/mapping ownership through asynchronous completion. | Add explicit public cancellation and scheduler telemetry. |
| Timeout and recovery | QEMU engine deadlines, queue/device generation cancellation, engine-only epoch reset, firmware queued deadlines/generation cancellation, result/abort acknowledgment, 100 ms abort, completion-publication, and reset-acknowledgment bounds, saturating escalation/overload counters, bridge-disconnect reconciliation, serialized management/watchdog terminal reset notification, and host generation-tagged request cancellation have executable paths. | Public reset control and hot-remove stress with active DMA. |
| Chunked DMA | Every payload opcode uses 64 KiB chunks; maximum-transfer copy/CRC, cross-boundary fill/vector, directional errors, guards, and v1 completion reporting are tested. | Persisted cancellation points, Nth-chunk faults, and memory-pressure evidence. |
| Deterministic faults | Six debug-gated PCI faults provide one-shot/Nth-match timeout, IRQ, hang, DMA-read, DMA-write, and active-reset injection; test-only injection independently freezes all eight essential firmware tasks; two named checkpoints, evidence persistence, lockout, recovery, and post-fault commands are tested. | Mailbox corruption, Nth-chunk failure, and additional ordering checkpoints. |
| Thin Linux payload API | Versioned info/NOP plus per-file long-term pinned DMA mappings, all payload opcodes, asynchronous submit/wait, multiple outstanding requests, `poll`/`epoll`, compat-safe layouts, reset cancellation, process-exit retention, and guest cleanup tests exist. | Public cancel/reset controls, scatter/gather mappings, hot-removal races, tracepoints/debugfs, and `libvams`. |
| Memory-order verification | Queue transport documents host/device ownership and QEMU DMA ordering; model and integration tests exercise the normal publication chain. | Independently delayed release/acquire checkpoints from descriptor write through CQ visibility, interrupt, and host consumption. |
| Unified observability | Firmware heartbeat/reset telemetry and stable command ID/cookie results exist. | Cross-layer structured events, bounded drop reporting, merged trace, and JSON CLI. |
| Stress/performance | Queue model plus deterministic qualification cover one million mixed commands, 1,000 resets, queue wrap/high-water, 24 virtual hours, integrity, liveness, and host-clock latency distributions. The rebuilt firmware passed the 41-firmware/39-host dual-QEMU suite and recorded 53,860/524,288 bytes SRAM, 748 ms watchdog margin, all task stack margins, loaded pool/queue high-water values, acknowledged abort, escalation, and post-recovery progress. | Establish a pinned-runner baseline and add worst-case concurrent-process, process-exit, memory-pressure, cross-scope recovery, and extended-fault qualification. |
| Security and isolation | Descriptor validation rejects malformed ranges, overlap, alignment, flags, and unsupported operations before payload access; the Linux API enforces opaque per-file handles, bounds, device read/write permissions, busy unmap, and cross-file rejection; debug faults are property-gated, absent from production capabilities, and lockable until cold reset. | Modeled IOMMU faults, privilege policy, hostile-parser coverage, and adversarial hot-removal isolation tests. |
| Requirement traceability | Generated ABI artifacts and normative documents define the current cross-layer contract. | Stable requirement IDs linked to design, implementation, test, CI evidence, and explicit limitation in a generated compliance matrix. |
| Reproducible CI and demo | Central pins, QEMU patch/source hashes, compatibility data, release-evidence validation, clean patch CI, main/manual virtual regressions, an offline firmware/payload/fault/stress demo, and a diskless Linux guest payload-UAPI test with a project-owned static init are implemented. Ubuntu's RISC-V GCC 14.2.0 successfully performs development rebuilds. | Acquire and hash-lock the pinned GCC 13.4.0 release toolchain and Linux artifacts; add sanitizer/static-analysis/coverage jobs, unified traces, and the Linux guest test to the main demo. |

### Near-term implementation order

1. Finish the Linux host surface with explicit cancel/reset controls,
   scatter/gather support, hot-remove fault tests, kernel tracepoints/debugfs,
   and a stable `libvams` wrapper.
2. Add independently controlled memory-order tests and the merged structured
   trace needed to diagnose failures across firmware, QEMU, kernel, and
   userspace.
3. Extend the million-command/reset/endurance qualification with
   concurrent-process, process-exit, memory-pressure, and long-duration
   watchdog runs while collecting worst-case recovery evidence and enforcing
   regression floors against the new rebuilt-image resource baseline.
4. Finish hash-locking the build matrix, enable sanitizers/static analysis and
   coverage-guided fuzzing, generate the requirements-to-evidence matrix, and
   compose the reproducible Linux guest/public UAPI into the existing
   hardware-free `make demo` report.

## Complete industry improvement backlog

This backlog is the authoritative implementation plan for closing the gap
between the current portfolio-quality virtual platform and an industry-grade
firmware product. Items are ordered by dependency and risk. Adding accelerator
opcodes does not take priority over ownership, isolation, lifecycle safety, or
reproducibility. Every required gate remains hardware-free.

### Work package A — Firmware command and recovery ownership

Implemented foundation: the high-priority recovery manager owns the running
queue, result identity validation, deadline-bounded abort request, 100 ms abort
acknowledgment, escalation counters, terminal transition/publication, reset
result reconciliation, disconnect terminal completion, management/watchdog
terminal reset notification, and CQ watermark hysteresis with persistent
high-water/backpressure evidence. Queue/device reset terminal acknowledgment is
bounded at 100 ms with persistent timeout evidence and clean post-expiry
progress. Management/watchdog reset now holds the RV32 warm reset until the
bridge acknowledges the exact tagged terminal result or its independent 100 ms
deadline expires; concurrent requests coalesce with watchdog reason priority.
Firmware capture defers before acknowledgment when its slab is full,
all owned-command queue handoffs are nonblocking capacity invariants, heartbeat
and diagnostic saturation is explicitly counted, and completion-portal stalls
request reset after 100 ms with the watchdog as a backstop.

Acceptance requires firmware ownership from descriptor capture through terminal
acknowledgment, bounded recovery at every scope, no command with two owners, no
lost or duplicate result, and clean progress after engine timeout, queue reset,
device reset, watchdog reset, CQ saturation, and bridge disconnect.

### Work package B — Firmware resource, health, and crash engineering

Development baseline captured on the rebuilt GCC 14.2.0 image: the complete
dual-QEMU suite passed 41 firmware-visible and 39 host-visible commands. Linked
SRAM is 53,860/524,288 bytes; peak command pool use is 3/8; validation, ready,
running, and completion queues peak at 1/8, 1/8, 1/1, and 1/8; the watchdog
margin is 748 ms. Peak task stack use is producer 184/1024, monitor 412/1024,
mailbox 220/1024, receiver 352/1536, validator 428/2048, scheduler 476/1536,
recovery 400/1536, completion 444/1536, and health 540/1024 bytes. This is
a development baseline, not yet the pinned-runner worst-case acceptance record.

- Run worst-case command, fault, overload, and recovery workloads; capture all
  task, ISR, main, and idle stack high-water values; justify each configured
  stack with a documented safety margin and enforce a regression floor in CI.
- Per-task health deadlines, startup grace, stable stuck-task identity,
  consecutive-failure thresholds, and saturating episode counters are
  implemented and tested by independently freezing each of eight essential
  tasks. Pet-jitter qualification remains.
- Add versioned, bounded, nonblocking structured firmware events with severity,
  command identity, generation, engine epoch, state transition, timestamp/clock
  domain, queue indices, error context, and explicit drop accounting.
- A 128-byte QEMU retained-SRAM window now preserves a versioned CRC record with
  boot/reset state, stuck-task evidence, last active command, assertion/stack
  fields, and four recent events across warm reset. Portable tests reject
  partial, corrupt, wrong-length, and old-version records. Assertion and stack
  fault capture into the reserved fields remains.
- Add Zephyr `ztest` or portable host tests for validation precedence, timeout
  wraparound, EDF/FIFO order, ownership transitions, recovery escalation,
  watchdog policy, mailbox parsing, telemetry encoding, and persistent records.

Acceptance requires positive measured stack/watchdog/SRAM margins under the
qualification workload, deterministic diagnosis of a deliberately stalled task,
and useful crash evidence after every modeled fatal reset without payload or
native-address disclosure.

### Work package C — Complete Linux payload and asynchronous API

- Add registered/pinned userspace DMA buffers and public copy, fill, CRC32, and
  vector operations instead of exposing raw guest addresses.
- Add asynchronous submit/wait, multiple outstanding requests, `poll`/`epoll`,
  cancellation, timeouts, reset control, and a stable `libvams` wrapper.
- Track commands and mappings per file descriptor across completion, reset,
  close, process exit, device removal, and module unload; define 32-bit
  compatibility behavior.
- Harden close during DMA, reset during completion, remove during ioctl/IRQ,
  unknown or duplicate completion, concurrent reset, and completion after file
  teardown.
- Add non-ABI `debugfs` state and kernel tracepoints for queues, requests,
  interrupts, polling, DMA bytes, timeout, cancellation, reset, and removal.

Acceptance requires all v1 operations through `/dev/vamsN`, concurrent
processes, asynchronous completion, safe exit/removal with work outstanding,
and zero leaked mappings, requests, or use-after-free reports under kernel
debugging configurations.

Implemented so far: opaque per-file mappings with bounds and permissions, every
v1 opcode, multiple outstanding requests, submit/wait, `poll`/`epoll`, compat
layouts, busy unmap, reset-generation cancellation, and close/process-exit
retention are exercised in the disposable QEMU guest. Explicit cancellation,
reset control, hot-remove stress, kernel-debugging runs, observability, and the
userspace library remain for this work package's complete acceptance gate.

### Work package D — DMA security and process isolation

- Enforce a per-file DMA aperture using opaque mapping handles, bounds,
  read/write permissions, lifetime reference counts, and safe unmap semantics.
- Reject cross-process, stale, overflowed, permission-incompatible, or
  reset-invalid mappings before payload access.
- Add a modeled IOMMU translation/fault path and adversarial tests proving a
  command cannot touch unrelated guest or process memory.
- Write a threat model covering malicious descriptors, compromised userspace or
  guest kernel, image tampering, rollback, debug abuse, reset abuse, denial of
  service, persistent-state corruption, and diagnostic leakage; state the
  virtual device's trust boundaries and non-goals.

Acceptance requires hostile multi-process and reset/removal tests with no DMA
outside an authorized mapping and documented residual risk for threats that the
virtual device cannot mitigate.

### Work package E — Device-model concurrency and ordering

- Add independently controllable checkpoints for descriptor write/release,
  SQ-tail publication, device acquire/fetch, capture before SQ-head, payload
  completion, CQ DMA, CQ-tail publication, MSI-X delivery, and host acquire.
- Extend deterministic faults with Nth-chunk failure, descriptor-fetch pause,
  pre-SQ-head pause, post-completion-DMA pause, pre-interrupt pause, firmware
  task freeze, mailbox corruption, bridge loss, QEMU termination, CQ-full reset,
  and reset between result publication and acknowledgment.
- Execute payload DMA as asynchronous chunks with cancellation points,
  configurable latency/bandwidth, bounded outstanding operations, channel
  contention, internal partial progress, byte accounting, and utilization
  counters. Partial internal progress must never become a false success.
- Exercise queue depths 16, 64, 256, and 1024, repeated wrap at each depth,
  saturation, invalid configurations, post-reset reconfiguration, and MSI-X
  mappings. Multiple queue pairs remain an explicitly versioned extension.
- Add quiesced migration state serialization or explicit safe rejection while
  busy, with compatibility tests for registers, indices, generations, timers,
  engine state, and production/debug differences.

Acceptance requires executable ordering proofs at every ownership boundary,
deterministic replay for every new race, cancellation between any two DMA
chunks, and no stale visibility across reset or migration.

### Work package F — Full Linux guest demonstration

- Pin and hash a Linux source/configuration, matching headers and kernel image,
  module, project-owned static init/client, initramfs recipe, and userspace tools.
- Extend `make demo` to boot management firmware and the Linux guest, load the
  driver, report hardware/firmware/ABI/UAPI versions, run every command through
  the public API, independently verify output, inject a fault, prove recovery,
  collect telemetry/traces, and shut down cleanly.
- Preserve firmware UART, QEMU, guest kernel, driver, userspace, and test logs
  plus artifact hashes and a single aggregate JSON result.

Acceptance is one offline, noninteractive, unprivileged command ending in
exactly one PASS/FAIL summary and demonstrating the same public interface an
application would use. Direct QTest remains lower-layer evidence, not a
substitute for this gate.

### Work package G — Security boot and update lifecycle

- Add a modeled immutable root of trust, signed manifest verification, firmware
  identity measurement, version/compatibility policy, anti-rollback state, and
  atomic A/B update metadata.
- Inject interruption at every erase, write, verify, and metadata transition;
  retain either the prior valid image or a constrained authenticated recovery
  path. Test wrong key, corruption, rollback, incompatible image, metadata
  damage, key rotation, and recovery abuse.
- Define development, provisioning, production, recovery, and decommissioning
  key/debug states. Keep signing keys outside source and artifacts, and prove
  production builds omit or permanently gate test fault controls.

Acceptance requires that untrusted firmware never executes, the last bootable
image is never destroyed by an interrupted update, rollback policy is enforced,
and production-mode debug cannot be enabled by normal guest actions.

### Work package H — Verification depth and traceability

- Assign stable requirement IDs and generate a compliance matrix linking each
  requirement to design, implementation owner, test, CI job, evidence artifact,
  result, and limitation.
- Add coverage-guided descriptor, MMIO, queue/reset/interrupt, mailbox, firmware
  event, and persistent-record fuzzers with seed corpora, minimization, replay,
  and permanent regression inputs for every discovered defect.
- Run ASan, UBSan, LeakSanitizer, Clang static analysis, GCC `-fanalyzer`,
  CodeQL, branch coverage, and focused mutation tests on host/QEMU components.
- Run the driver with `W=1`/`W=2`, Sparse, Smatch, Coccinelle, KUnit, Lockdep,
  KASAN, UBSAN, DMA API debugging, forced allocation failures, and repeated
  bind/unbind/load/unload.

Acceptance requires no unexplained sanitizer/static-analysis findings,
traceable evidence for every release requirement, and measured coverage of all
security-, validation-, ownership-, timeout-, and recovery-relevant branches.

### Work package I — Reliability and performance qualification

- Extend qualification to several million mixed commands, thousands of every
  reset scope, extended fault sequences, concurrent users, process/QEMU exit,
  memory pressure, CQ backpressure, repeated driver and firmware restart, clock
  variation, telemetry saturation, and controlled host load.
- On a pinned runner, baseline commands/s, payload throughput by size/opcode,
  p50/p90/p99/p99.9/max latency, recovery duration, engine utilization,
  interrupts/command, polling wakeups, CPU use, queue occupancy, firmware stack
  and SRAM high-water, and watchdog margin.
- Store raw machine-readable evidence, seeds, resource constraints, failure
  disposition, and confidence/regression bands. Any correctness failure
  invalidates the affected timing result and restarts its endurance run.

Acceptance requires zero unexplained hang, corruption, stale/duplicate
completion, mapping leak, invariant violation, watchdog reset, or security-policy
bypass for the documented duration and workload.

### Work package J — Hermetic builds and layered CI

- Hash-lock the RISC-V compiler, Zephyr Python wheels, host container, QEMU
  dependencies, Linux kernel/headers and initramfs generator, guest filesystem,
  CI actions, and generated artifact manifest.
- Provide a container, Nix, or Guix environment with fixed locale/timestamps,
  no host-tool leakage, offline rebuild after acquisition, compiler/linker
  identity, SBOM, provenance, and repeated-build hash comparison.
- Layer CI into contract/docs, GCC/Clang units, QEMU apply/build, QTest/model,
  sanitizers, fuzz smoke/extended fuzz, Zephyr build/smoke/resources, kernel
  matrix, guest integration, full demo, nightly million-command, scheduled
  endurance, and release reproducibility jobs.

Acceptance requires a clean-host build from the manifest, offline validation,
and reproducible release artifacts or a documented and bounded source of any
remaining nondeterminism.

### Work package K — Operator tools and unified observability

- Implement `vamsctl info`, `health`, `queue-status`, `submit`, `wait`, `reset`,
  `fault`, `trace`, `self-test`, and `benchmark` with consistent text and JSON.
- Correlate firmware, QEMU, kernel, and userspace events by command ID, cookie,
  reset generation, engine epoch, DMA operation, and named clock domain.
- Generate a static HTML qualification dashboard containing artifact identity,
  test results, command/reset counts, latency/throughput distributions, resource
  margins, fault coverage, failures, and known limitations.
- Document operator procedures for health, update, rollback, safe reset,
  degraded operation, diagnostics, recovery, and decommissioning.

Acceptance requires a single-command timeline for a normal, timed-out, and
reset command, stable machine-readable CLI output, bounded drop reporting, and
diagnostics that expose neither payloads, secrets, nor native pointers.

### Work package L — Maintainability and generated engineering contracts

- Adopt documented C/C++/Python/shell rules for conversions, endianness, MMIO,
  DMA ownership, timeout arithmetic, lock ordering, errors, assertions, reset,
  logging, allocation, and generated code.
- Pin and apply `clang-format`, Python lint/format, ShellCheck, Markdown lint,
  and kernel-style checks without introducing unrelated churn.
- Extend the ABI schema to generate registers, bit fields, layouts, status/error
  values, documentation tables, Python decoders, trace names, compatibility
  metadata, and compile-time assertions while keeping behavior handwritten.
- Record architecture decisions for firmware policy ownership, the dual-QEMU
  bridge, ring semantics, heap-free operation, EDF, reset domains, and the
  distinction between virtual timing and physical performance.
- Define supported revisions, compatibility promises, release/rollback,
  long-term support, backport, vulnerability response, and end-of-life policy.

Acceptance requires automated style/contract checks, no hand-copied divergent
ABI constants, reviewable architectural rationale, and a documented support
policy for every released interface.

### Optional post-release capability backlog

Only after work packages A–L meet their release gates, evaluate scatter/gather
DMA, multiple queue pairs and MSI-X vectors, a local-memory BAR, IOMMU-translated
DMA, performance-monitoring registers, quiesced live migration, persistent
virtual flash, multiple management cores, SR-IOV/PASID isolation, and modeled
power/thermal states. Each feature requires a new or compatible ABI design,
threat analysis, resource budget, compatibility plan, deterministic faults, and
its own acceptance evidence. An FPGA or development-board port remains optional
and never becomes a prerequisite for the virtual release.

### Execution sequence

1. Work packages A and B establish complete firmware ownership and measurable
   recovery/resource behavior.
2. Work packages C and D expose all payload commands safely through Linux while
   enforcing process-scoped DMA isolation.
3. Work package E proves ordering and cancellation under adversarial races.
4. Work package F composes the public Linux path into the offline demo.
5. Work packages H and J make verification traceable, analyzable, and hermetic.
6. Work packages I and K produce release qualification and usable diagnostics.
7. Work packages G and L close the security lifecycle and long-term maintenance
   requirements.
8. Optional post-release capabilities begin only after formal release sign-off.

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
