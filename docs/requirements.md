# Requirements and scope

This document is normative for VAMS release 1. “Must” is required for a phase
gate; “should” requires a documented exception.

## Product objective

VAMS must model a QEMU PCIe accelerator with one embedded RV32 management CPU
running Zephyr. Firmware, rather than the Linux driver, must validate and
schedule DMA descriptor work, manage health, and drive recovery. Each milestone
must leave a reproducible, testable result and preserve earlier normal paths.

## Functional requirements

| ID | Requirement | Target phase |
|---|---|---:|
| SYS-01 | One RV32IMAC management CPU executes code from boot ROM and SRAM. | 1 |
| SYS-02 | UART, timer, interrupt routing, reset, watchdog, mailbox, and telemetry peripherals are modeled. | 1–3 |
| SYS-03 | Zephyr board `vams_riscv` boots and runs separately communicating tasks. | 2 |
| PCI-01 | `vams-pcie` enumerates as PCIe with BAR0 and at least one MSI/MSI-X vector. | 4 |
| ABI-01 | Exactly one submission and one completion ring use the v1 descriptors. | 5 |
| ABI-02 | Firmware validates descriptors in the normative order before execution. | 5 |
| CMD-01 | NOP, memory copy, memory fill, CRC32, and vector add have stable semantics. | 5–6 |
| DMA-01 | All hardware-visible addresses are 64-bit DMA addresses; no native pointers enter the ABI. | 5 |
| DMA-02 | Length, range overflow, alignment, DMA failure, timeout, and reset are handled. | 6–7 |
| REC-01 | Command, queue, engine, firmware, and full-device recovery levels exist. | 7 |
| HLT-01 | Heartbeat, uptime, command, byte, error, reset, queue, and latency telemetry is host-readable. | 3–7 |
| FLT-01 | At least five deterministic, debug-only injected faults have regression tests. | 8 |
| LNX-01 | `vams_pci` handles PCI, coherent rings, DMA mapping, interrupt/poll completion, and reset only. | 4–7 |
| TST-01 | QTest, firmware tests, driver tests, and end-to-end regression are automated as introduced. | all |

## Release-1 constraints

- One management CPU, one submission queue, one completion queue, one command
  execution slot, and one completion MSI-X vector (plus an optional async/error
  vector) are sufficient.
- Rings have power-of-two depth from 16 through 1024 and 64-byte-aligned bases.
- Maximum command transfer length is 16 MiB (`0x01000000`).
- Hardware/ABI multibyte values are little-endian.
- BAR0 is 4 KiB. Queue descriptors and payloads live in host DMA memory.
- The initial engine implements correctness, not representative accelerator
  arithmetic performance.

Out of scope: multiple management CPUs, more than one queue pair, IOMMU
emulation, SR-IOV, secure boot, production signing, A/B update, complex power
management, protocol stacks, and a GUI.

## Ownership invariants

| Resource | Host | Firmware | Hardware model |
|---|---|---|---|
| Submission entry | Writes before tail publish; does not modify while owned | Reads and validates after tail observation | DMA fetches; orders doorbell observation |
| SQ tail | Produces monotonically modulo depth | Observes | Stores/exposes |
| SQ head | Reads for space | Advances after descriptor capture | Stores/exposes |
| Completion entry | Reads after tail observation | Builds/publishes | DMA writes |
| CQ head | Advances after consumption | Observes for space | Stores/exposes |
| CQ tail | Reads | Advances after DMA write | Stores/exposes |
| Command policy/state | No direct access | Sole owner outside ISR event flags | Executes requested primitives |
| DMA translation/data movement | Maps/unmaps buffers | Requests and monitors | Performs PCI DMA and enforces range checks |
| Heartbeat/health | Reads | Updates | Exposes and atomically snapshots counters |
| Queue/firmware/device reset | Requests and reinitializes host state | Quiesces if responsive | Enforces and increments host generation |
| Fault controls | Test software writes | Responds to effects | Injects deterministic behavior |

The host must issue a release barrier before publishing SQ tail. The device must
complete descriptor/data reads before advancing SQ head. Firmware/device must
complete the CQ DMA write before publishing CQ tail and interrupting. The host
uses an acquire barrier after observing CQ tail.

## Quality and acceptance

Hardware and ABI structures use fixed-width types, explicit padding, static
size assertions, and compile-time endianness conversion. Reserved values and
reset effects must be tested. New code should build under GCC and Clang with
strict warnings as errors where upstream environments permit. A feature is
“implemented” only when its phase acceptance test passes and documentation is
updated. Known failures must be visible, not hidden by the demo.

Phase 0 acceptance is `make check`, a complete internal consistency review, and
absence of implementation code. Later acceptance criteria are in the roadmap
and verification plan.
