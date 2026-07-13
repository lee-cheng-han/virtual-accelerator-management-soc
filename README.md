# Virtual Accelerator Management SoC

Virtual Accelerator Management SoC (VAMS) is a specification-first portfolio
project for a QEMU PCIe accelerator whose control plane runs real RTOS firmware
on an embedded RISC-V management CPU. The host submits versioned DMA
descriptors; firmware validates, schedules, monitors, and recovers work; a thin
Linux PCI driver only exposes the queues and lifecycle controls.

> Status: **Specifications complete; minimal RV32 subsystem implemented.** The
> QEMU machine and bare-metal firmware boot through UART, SRAM, and timer IRQ
> checks. PCIe, DMA, Zephyr, the kernel driver, and the full demo remain planned.

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

`make demo` reports that the full PCIe accelerator demo is not implemented.
Future dependencies include the Zephyr SDK/west and Linux kernel headers; exact
supported versions will be pinned when those components are introduced.

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

## Planned repository areas

`qemu/` holds out-of-tree device-model work and tests; `firmware/` holds the
`vams_riscv` firmware and future Zephyr board/application; `kernel/` stays a thin
`vams_pci` transport; `userspace/` holds `libvams`, `vamsctl`, and benchmarks;
and top-level `tests/` holds end-to-end suites. Unimplemented directories are
scaffolding and gain tracked files only when their components are built.

## Known limitations

- Only the minimal RISC-V subsystem described in its bring-up guide is currently
  executable; the wider accelerator architecture remains a design target.
- The provisional development PCI ID is not allocated for production use.
- One management CPU and one queue pair are deliberately fixed for release 1.
- No IOMMU model, SR-IOV, secure boot, signed update, or A/B firmware support is
  in release-1 scope.
- Performance numbers will not be published until the corresponding system and
  measurement harness exist.

## License

VAMS is available under the MIT License; see [LICENSE](LICENSE).
