# Zephyr Board Port

## Implemented target

`vams_riscv` is an out-of-tree Zephyr board and SoC definition for the VAMS
QEMU management subsystem. It is validated against Zephyr v4.4.0 at commit
`684c9e8f32e4373a21098559f748f06915f950c9`.

The port provides:

- an RV32IMAC CPU description with Zicsr and Zifencei;
- a 512 KiB SRAM-only linker layout beginning at `0x80000000`;
- an NS16550 polling console at `0x10000000`;
- the RISC-V machine timer using `mtime` at `0x0200bff8` and `mtimecmp` at
  `0x02004000`;
- direct machine timer and external interrupt descriptions without a PLIC;
- a strict-warning Zephyr application with assertions and stack sentinels;
- producer and monitor tasks communicating through a bounded message queue;
- a timer-driven heartbeat and an automated QEMU transcript test.

The application uses two 1024-byte task stacks, a four-entry heartbeat queue,
and 250 ms heartbeat periods. The validated build uses 29,984 bytes of the
512 KiB SRAM region. These values are initial bring-up settings, not final
worst-case stack-sizing evidence.

## Dependency preparation

Preparation downloads the pinned Zephyr source into the ignored `build/`
directory, creates a repository-local Python virtual environment, installs
Zephyr's build requirements, and initializes the local west workspace:

```sh
make zephyr-prepare
```

The Zephyr source revision is exact. Python dependencies currently follow the
version constraints in Zephyr's official v4.4.0 requirements file rather than
a project-owned hash lock; dependency hash locking remains reproducibility
work for the CI milestone.

## Build

The board is discovered through `BOARD_ROOT` and the SoC through `SOC_ROOT`;
no Zephyr source files are modified. A standard bare-metal RISC-V GCC prefix is
required:

```sh
make zephyr \
  CROSS_COMPILE=riscv64-unknown-elf-
```

The build disables optional `ccache` use so it behaves consistently in
isolated environments. Zephyr's RAM-only RISC-V link uses an OMAGIC load
segment, so the application explicitly suppresses only binutils'
`--warn-rwx-segments` diagnostic; all other compiler and linker warnings remain
fatal. The current QEMU SRAM is deliberately executable and writable.

The resulting image is:

```text
build/firmware/zephyr/zephyr/zephyr.elf
```

## Automated validation

```sh
make zephyr-smoke \
  CROSS_COMPILE=riscv64-unknown-elf- \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32
```

The smoke test requires three consecutive heartbeat records. Reaching the
second and third heartbeat proves that the machine timer interrupt advances
Zephyr time, wakes the producer task, and lets the monitor task consume the
message queue repeatedly.

Expected console prefix:

```text
Virtual Accelerator Management SoC Zephyr booting
Kernel: Zephyr 4.4.0
Tasks: producer -> message queue -> monitor
Heartbeat: sequence=1 uptime_ms=...
Heartbeat: sequence=2 uptime_ms=...
Heartbeat: sequence=3 uptime_ms=...
```

## Deliberate limitations

- The UART console is polling-only; UART interrupt traffic is not exercised.
- No PLIC exists. The UART output is wired directly to machine external IRQ,
  matching the minimal QEMU topology.
- There is no mailbox, watchdog, reset controller, telemetry block, PCIe
  endpoint, DMA engine, or host command path yet.
- The application demonstrates RTOS scheduling and IPC, not the final firmware
  task topology or priorities.
- Stack sentinel and initialized-stack checks are enabled, but measured
  per-thread high-water evidence has not yet been collected.
