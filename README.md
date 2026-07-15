# Virtual Accelerator Management SoC

Virtual Accelerator Management SoC (VAMS) is a specification-first portfolio
project for a QEMU PCIe accelerator whose control plane runs real RTOS firmware
on an embedded RISC-V management CPU. The host submits versioned DMA
descriptors; firmware validates, schedules, monitors, and recovers work; a thin
Linux PCI driver only exposes the queues and lifecycle controls.

> Status: **PCIe endpoint and Linux discovery driver implemented.** The
> custom QEMU machine runs bare-metal and Zephyr firmware with mailbox,
> watchdog recovery, and telemetry. The host-facing `vams-pcie` endpoint now
> enumerates with BAR0 and two MSI-X vectors, while `vams_pci.ko` validates and
> binds it with tested cleanup paths. DMA queues and the full demo remain
> planned.

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
- Thin `vams_pci` discovery driver with ABI validation and 64/32-bit DMA-mask negotiation
- Two-vector MSI-X handling with reverse-order probe/remove cleanup
- Disposable Linux guest test covering seven injected probe failures, both IRQs, and rebinding

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
make pcie-smoke \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
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

## Planned repository areas

`qemu/` holds out-of-tree device-model work and tests; `firmware/` holds the
`vams_riscv` firmware and future Zephyr board/application; `kernel/` stays a thin
`vams_pci` transport; `userspace/` holds `libvams`, `vamsctl`, and benchmarks;
and top-level `tests/` holds end-to-end suites. Unimplemented directories are
scaffolding and gain tracked files only when their components are built.

## Known limitations

- The RISC-V management subsystem and PCIe endpoint shell are executable, but
  they are not connected into one private device instance yet.
- The endpoint advertises only MSI-X. The Linux driver binds in discovery mode;
  queue, DMA, engine, firmware, telemetry, and debug capabilities remain clear
  until their host-facing paths work.
- The provisional development PCI ID is not allocated for production use.
- One management CPU and one queue pair are deliberately fixed for release 1.
- No IOMMU model, SR-IOV, secure boot, signed update, or A/B firmware support is
  in release-1 scope.
- Performance numbers will not be published until the corresponding system and
  measurement harness exist.

## License

VAMS is available under the MIT License; see [LICENSE](LICENSE). Linux kernel
module sources under `kernel/` are GPL-2.0-only for kernel compatibility.
