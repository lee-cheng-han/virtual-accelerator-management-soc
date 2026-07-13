# Milestone roadmap

Each phase begins by auditing the prior gate, listing changed files and risks,
and ends with tests, exact commands/results, limitations, and a commit-message
suggestion. A later phase may not make an earlier gate flaky.

| Phase | Deliverable and acceptance gate |
|---:|---|
| **0 — Specification** | This design package, skeleton, and `make check`; no implementation. |
| **1 — Minimal RISC-V VM** | QEMU subsystem, ROM/SRAM/UART/timer/IRQ, bare-metal image, expected three-line boot output, QTest smoke. |
| **2 — Zephyr port** | `vams_riscv` board/devicetree/Kconfig, console/tick, two tasks communicate and heartbeat under test. |
| **3 — Management peripherals** | Mailbox, watchdog, reset, telemetry plus Zephyr drivers; mailbox and watchdog-reset integration tests. |
| **4 — PCIe endpoint** | Enumeration, BAR0 identity/capabilities, MSI-X test, thin `vams_pci` probe/remove and cleanup tests. |
| **5 — Command path** | One SQ/CQ, v1 descriptors, NOP validation/completion, interrupt plus polling; ID/cookie round trip. |
| **6 — DMA/engine** | Copy, fill, CRC32, vector add; overflow/length/timeouts; data-integrity and throughput smoke. |
| **7 — Scheduling/recovery** | Queued/running states, timeout, engine/queue/device reset, reset generation, structured errors; later commands remain sound after forced timeout. |
| **8 — Fault injection** | At least five deterministic debug faults with counter, expected recovery, and regression each. |
| **9 — Stress/performance** | Ring wrap, long run, repeated reset, histogram, throughput, stack use, queue high-water report. |
| **10 — CI/demo** | Reproducible builds and layered tests in CI; `make demo` boots, submits, verifies, faults, recovers, and reports PASS/FAIL. |

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

