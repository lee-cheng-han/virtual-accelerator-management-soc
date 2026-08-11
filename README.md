# Virtual Accelerator Management SoC

Virtual Accelerator Management SoC (VAMS) is a specification-first portfolio
project for a QEMU PCIe accelerator whose control plane runs real RTOS firmware
on an embedded RISC-V management CPU. The host submits versioned DMA
descriptors; firmware validates, schedules, monitors, and recovers work; a thin
Linux PCI driver only exposes the queues and lifecycle controls.

> Status: **Deterministic fault injection complete.**
> The
> custom QEMU machine runs bare-metal and Zephyr firmware with mailbox,
> watchdog recovery, telemetry, and a private command portal. Zephyr now
> validates generated-ABI NOP descriptors and publishes completions, while the
> host-facing endpoint and `vams_pci.ko` exercise coherent SQ/CQ DMA and MSI-X.
> A private dual-QEMU bridge now carries PCI-fetched descriptors through the
> real Zephyr command service and returns firmware-owned completions to the CQ.
> Firmware-validated `MEM_COPY`, `MEM_FILL`, `CRC32`, and `VECTOR_ADD` now
> perform checked PCI payload DMA with exact byte counts, guarded writes,
> verified CRC results, and little-endian vector arithmetic.
> Payload execution now crosses a deterministic virtual-time engine boundary;
> command deadlines and queue-reset cancellation are validated without
> wall-clock timing.
> Every payload operation uses a bounded 64 KiB DMA working chunk, including
> streaming CRC state and a full 16 MiB copy/integrity throughput smoke.
> Zephyr now schedules fixed-pool command objects through receiver, validator,
> earliest-deadline scheduler, and exactly-once completion tasks.
> Engine-only reset now terminates active work with a reset completion, advances
> a private engine epoch, preserves queued work, and suppresses stale callbacks.
> A property-gated debug block now injects six one-shot PCI faults, selects an
> Nth matching transaction, pauses two named race windows, preserves evidence,
> locks until cold reset, and requires a clean command after every recovery.

## Architecture

```text
Host userspace: libvams + vamsctl
                 |
Thin vams_pci driver (DMA queues, MSI-X, reset)
                 |
             PCIe endpoint
        +--------+---------+
        | BAR0 registers   |       host memory
        | queue controller |<----> SQ / CQ / data buffers
        | DMA + engine     |
        +--------+---------+
                 |
       RISC-V RV32 management subsystem
  ROM / SRAM / UART / timer / IRQ / watchdog
                 |
      Zephyr firmware control plane
 validate -> schedule -> execute -> recover -> report
```

Unlike a basic MMIO mailbox exercise, VAMS makes firmware the owner of command
policy and recovery. The host/device contract uses PCIe DMA submission and
completion rings, explicit ownership indices, MSI-X with polling fallback,
generation-tagged resets, and layered health management. The mailbox is an
internal management peripheral, not the host datapath.

## Current deliverables

- Normative scope and requirements
- Host, device, firmware, queue, interrupt, and recovery architecture
- 4 KiB BAR0 register contract and debug-only fault controls
- Little-endian 64-byte submission and 32-byte completion ABI
- Firmware task and command lifecycle design
- Verification, performance, demo, and milestone plans
- Out-of-tree QEMU `vams_riscv` RV32 machine patch
- Freestanding boot firmware with UART, SRAM, and machine-timer checks
- Automated QEMU boot-transcript smoke test
- Out-of-tree Zephyr `vams_riscv` board and SoC definitions
- Zephyr mailbox and management-control drivers
- Progress-epoch health supervision, watchdog recovery, and telemetry
- QTest MMIO, mailbox, and forced-watchdog-reset regressions
- QEMU `vams-pcie` processing-accelerator endpoint with BAR0 and two MSI-X vectors
- PCI enumeration, MMIO error, MSI-X pending, and asynchronous-reset QTest
- Thin `vams_pci` queue driver with ABI validation and 64/32-bit DMA-mask negotiation
- Two-vector MSI-X handling with reverse-order probe/remove cleanup
- Disposable Linux guest test covering nine injected probe failures, both IRQs, and rebinding
- Authoritative JSON v1 ABI with generated portable, QEMU, and kernel headers
- Generated firmware ABI plus a private descriptor/completion ownership portal
- Zephyr-owned valid and unsupported-version NOP completions
- Private PCI-to-RV32 command bridge with end-to-end Zephyr validation and
  stale-completion suppression across queue reset
- Firmware-owned `MEM_COPY`, `MEM_FILL`, `CRC32`, and `VECTOR_ADD` validation
  with QEMU PCI DMA execution
- Virtual-time engine BUSY state, absolute deadlines, timeout completion, and
  reset-generation callback suppression
- Bounded 64 KiB payload chunks with maximum-transfer integrity and
  virtual-model throughput reporting
- Fixed eight-object Zephyr command pool with asserted ownership transitions,
  queued deadlines, reset generations, and exactly-once publication
- Host-visible engine status/error/epoch state and engine-only reset with a
  terminal reset result, queued-work preservation, and stale-callback rejection
- Six debug-gated timeout/IRQ/hang/DMA/reset faults with Nth-match selection,
  engine-start and CQ-publication checkpoints, persistent evidence, and lockout
- Fail-fast queue/bridge/engine invariants plus replayable raw-descriptor and
  malformed-BAR mutation regressions
- One coherent SQ/CQ pair with checked doorbells, DMA ordering, and paired reset
- Successful and invalid NOP completions through QTest raw guest memory
- Linux guest NOP round trip through a real coherent ring and MSI-X interrupt
- Versioned `/dev/vamsN` UAPI with tracked concurrent NOP requests
- Lost-interrupt CQ polling fallback and bounded request cancellation
- Deterministic SQ/CQ reference-model comparison across randomized queue,
  wraparound, backpressure, interrupt, error, and reset sequences

The normative documents are under [`docs/`](docs/). When a summary here and a
normative document disagree, the normative document wins.

## Build and validation

Documentation checks need a POSIX shell, GNU Make, `grep`, and `find`:

```sh
make check
make tree
```

The minimal firmware additionally needs a bare-metal RISC-V GCC toolchain. Its
QEMU machine is carried as an out-of-tree patch; detailed integration and smoke
test commands are in the
[minimal RISC-V subsystem guide](docs/minimal-riscv-subsystem.md).

```sh
make firmware CROSS_COMPILE=riscv64-unknown-elf-
make smoke \
  CROSS_COMPILE=riscv64-unknown-elf- \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32
```

The RTOS target uses pinned Zephyr v4.4.0 source and a repository-local Python
environment:

```sh
make zephyr-prepare
make zephyr-smoke \
  CROSS_COMPILE=riscv64-unknown-elf- \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32
make management-mmio-smoke \
  CROSS_COMPILE=riscv64-unknown-elf- \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32
make management-smoke \
  CROSS_COMPILE=riscv64-unknown-elf- \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32
make watchdog-smoke \
  CROSS_COMPILE=riscv64-unknown-elf- \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32
make command-portal-smoke \
  CROSS_COMPILE=riscv64-unknown-elf- \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32
make firmware-command-smoke \
  CROSS_COMPILE=riscv64-unknown-elf- \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32
make firmware-pcie-smoke \
  CROSS_COMPILE=riscv64-unknown-elf- \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32 \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
make mem-copy-smoke \
  CROSS_COMPILE=riscv64-unknown-elf- \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32 \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
make mem-fill-smoke \
  CROSS_COMPILE=riscv64-unknown-elf- \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32 \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
make crc32-smoke \
  CROSS_COMPILE=riscv64-unknown-elf- \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32 \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
make vector-add-smoke \
  CROSS_COMPILE=riscv64-unknown-elf- \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32 \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
make async-engine-smoke \
  CROSS_COMPILE=riscv64-unknown-elf- \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32 \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
make fault-injection-smoke \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
make dma-engine-smoke \
  CROSS_COMPILE=riscv64-unknown-elf- \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32 \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
make scheduler-recovery-smoke \
  CROSS_COMPILE=/path/to/riscv64-unknown-elf- \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32 \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
make assurance-smoke \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
make pcie-smoke \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
make queue-model-smoke \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
make nop-smoke \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
make abi-check
make kernel KERNEL_BUILD=/path/to/linux/build
make kernel-smoke \
  KERNEL_BUILD=/path/to/linux/build \
  VAMS_LINUX_IMAGE=/path/to/matching/bzImage \
  BUSYBOX=/path/to/static/busybox \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
```

`make demo` reports that the full PCIe accelerator demo is not implemented.
The kernel smoke test builds a temporary initramfs and needs matching Linux
headers/image plus static BusyBox; it does not require a disk image.

## Specification map

| Document | Purpose |
|---|---|
| [Requirements](docs/requirements.md) | Scope, requirements, ownership, acceptance |
| [Architecture](docs/architecture.md) | Components, paths, memory, interrupts |
| [Roadmap](docs/roadmap.md) | Testable milestone gates |
| [Register map](docs/register-map.md) | BAR0 hardware contract |
| [Host/firmware ABI](docs/host-firmware-abi.md) | Queue protocol and compatibility |
| [Descriptors](docs/descriptor-format.md) | Exact binary layouts and validation |
| [Firmware architecture](docs/firmware-architecture.md) | RTOS tasks and concurrency |
| [Command lifecycle](docs/command-lifecycle.md) | States, transitions, exceptional paths |
| [Fault and recovery](docs/fault-recovery.md) | Deterministic faults and reset hierarchy |
| [Verification plan](docs/verification-plan.md) | Test layers and traceability |
| [Performance plan](docs/performance-plan.md) | Metrics and reproducible method |
| [Demo](docs/demo.md) | Current and final demo contracts |
| [Minimal RISC-V subsystem](docs/minimal-riscv-subsystem.md) | QEMU and firmware bring-up contract |
| [Zephyr board port](docs/zephyr-board-port.md) | RTOS board, timer, task IPC, and validation |
| [Management peripherals](docs/management-peripherals.md) | Mailbox, watchdog, reset, telemetry, and tests |
| [PCIe endpoint](docs/pcie-endpoint.md) | PCI identity, BAR0, MSI-X, reset, and QTest contract |
| [Linux PCI driver](docs/linux-pci-driver.md) | Probe/remove, ABI validation, IRQs, cleanup, and guest test |
| [Linux UAPI](docs/linux-uapi.md) | Versioning, device info, synchronous NOP, and lifetime rules |
| [NOP command path](docs/nop-command-path.md) | Generated ABI, coherent rings, ordering, NOP, and limitations |
| [MEM_COPY command path](docs/mem-copy-command-path.md) | Firmware validation, payload DMA, ordering, and integrity tests |
| [MEM_FILL command path](docs/mem-fill-command-path.md) | Firmware validation, write-only DMA, ordering, and integrity tests |
| [CRC32 command path](docs/crc32-command-path.md) | Firmware validation, IEEE CRC result checking, DMA errors, and tests |
| [VECTOR_ADD command path](docs/vector-add-command-path.md) | Firmware validation, little-endian arithmetic, DMA errors, and tests |
| [Asynchronous engine](docs/asynchronous-engine.md) | Virtual-time execution, deadlines, BUSY state, reset cancellation, and tests |
| [Bounded payload DMA](docs/chunked-dma.md) | Chunked working sets, maximum-transfer integrity, and throughput smoke |
| [Firmware scheduler](docs/firmware-scheduler.md) | Fixed-pool ownership pipeline, EDF dispatch, queued timeout, and exactly-once completion |
| [Assurance](docs/assurance.md) | Executable invariants, descriptor/BAR fuzzing, replay, and sanitizer use |

## Planned repository areas

`qemu/` holds out-of-tree device-model work and tests; `firmware/` holds the
`vams_riscv` firmware and future Zephyr board/application; `kernel/` stays a thin
`vams_pci` transport; `userspace/` holds `libvams`, `vamsctl`, and benchmarks;
and top-level `tests/` holds end-to-end suites. Unimplemented directories are
scaffolding and gain tracked files only when their components are built.

## Known limitations

- Target-specific QEMU binaries require the PCI endpoint and RV32 management
  subsystem to run as two processes joined by a private local bridge.
- All four v1 payload commands use a correctness-first virtual-time QEMU engine.
  Dispatch, deadlines, and reset cancellation are asynchronous, and payload
  working sets are bounded, but all chunks still execute within one callback.
  A host-requested engine reset is implemented; mid-command firmware abort
  acknowledgment and host telemetry are not.
- The endpoint retains a direct validator only for isolated QTests; integrated
  NOP and all four v1 payload commands use real Zephyr validation.
- Firmware scheduling covers capture through authorization publication. The
  payload engine does not yet return running-command events to firmware, so
  firmware-side abort acknowledgment and DMA-result telemetry remain
  unimplemented. QEMU independently provides engine-only recovery.
- Fault injection currently targets the PCI queue, engine, DMA, and interrupt
  model. Firmware-task hang and mailbox-corruption injection remain planned
  cross-subsystem extensions.
- The public host API currently exposes device information and synchronous NOP;
  payload mapping and asynchronous userspace submission remain future work.
- The provisional development PCI ID is not allocated for production use.
- One management CPU and one queue pair are deliberately fixed for release 1.
- No IOMMU model, SR-IOV, secure boot, signed update, or A/B firmware support is
  in release-1 scope.
- Performance numbers will not be published until the corresponding system and
  measurement harness exist.

## License

VAMS is available under the MIT License; see [LICENSE](LICENSE). Linux kernel
module sources under `kernel/` are GPL-2.0-only for kernel compatibility.
