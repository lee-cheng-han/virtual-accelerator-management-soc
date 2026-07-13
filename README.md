# Virtual Accelerator Management SoC

Virtual Accelerator Management SoC (VAMS) is a specification-first portfolio
project for a QEMU PCIe accelerator whose control plane runs real RTOS firmware
on an embedded RISC-V management CPU. The host submits versioned DMA
descriptors; firmware validates, schedules, monitors, and recovers work; a thin
Linux PCI driver only exposes the queues and lifecycle controls.

> Status: **Phase 0 — Complete (specification only).** No hardware, firmware,
> kernel driver, or working demo is implemented yet. Phase 1 is planned.

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

## Phase 0 deliverables

- Normative scope and requirements
- Host, device, firmware, queue, interrupt, and recovery architecture
- 4 KiB BAR0 register contract and debug-only fault controls
- Little-endian 64-byte submission and 32-byte completion ABI
- Firmware task and command lifecycle design
- Verification, performance, demo, and milestone plans

The normative documents are under [`docs/`](docs/). When a summary here and a
normative document disagree, the normative document wins.

## Build and validation

Phase 0 needs a POSIX shell, GNU Make, `grep`, and `find`:

```sh
make check
make tree
```

There is nothing to compile yet. `make demo` intentionally reports that the
full demo is unavailable until Phase 10. Future toolchain dependencies include
QEMU build prerequisites, a RISC-V cross compiler, Zephyr SDK/west, and Linux
kernel headers; exact supported versions will be pinned when introduced.

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

## Planned repository areas

`qemu/` holds out-of-tree device-model work and tests; `firmware/` holds the
`vams_riscv` Zephyr board and management application; `kernel/` stays a thin
`vams_pci` transport; `userspace/` holds `libvams`, `vamsctl`, and benchmarks;
and top-level `tests/` holds end-to-end suites. Empty Phase 1 directories are
present locally as scaffolding and gain tracked files only when implemented.

## Known limitations

- All behavior described beyond Phase 0 is a design target, not a claim of
  implementation.
- The provisional development PCI ID is not allocated for production use.
- One management CPU and one queue pair are deliberately fixed for release 1.
- No IOMMU model, SR-IOV, secure boot, signed update, or A/B firmware support is
  in release-1 scope.
- Performance numbers will not be published until the corresponding system and
  measurement harness exist.

## License

VAMS is available under the MIT License; see [LICENSE](LICENSE).

