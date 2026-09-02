# Hardware-free demo contract

`make demo` is a noninteractive demonstration of the implemented accelerator
and real Zephyr control plane. It requires prepared QEMU and firmware artifacts,
but requires no PCIe card, FPGA, development board, root access, or network.

Prepare the exact QEMU source revision and patch series once:

```sh
make qemu-prepare
```

Build the firmware with the pinned Zephyr source and a compatible RV32 GCC
toolchain, then run the offline demo:

```sh
make zephyr-prepare
make zephyr CROSS_COMPILE=/path/to/riscv64-unknown-elf-
make demo
```

The demo fails during preflight unless both QEMU binaries identify the pinned
source revision and the Zephyr ELF is present. It then runs these fail-fast
stages with explicit process timeouts:

1. generated contracts, release inputs, host unit tests, and source hygiene;
2. PCI descriptors through the real RV32 Zephyr validation/scheduler service;
3. copy, fill, CRC32, and vector data integrity including the 16 MiB DMA path;
4. all deterministic PCI fault classes and clean recovery commands;
5. a development-sized queue/reset/endurance stress run.

Each stage writes its complete output under `build/demo/<UTC timestamp>/`.
`report.json` records the source revision and dirty state, exact dependency
pins, host details, artifact paths and SHA-256 hashes, commands, durations,
exit status, and log names. A failed stage preserves all earlier logs. Console
output ends in exactly one `VAMS DEMO: PASS` or `VAMS DEMO: FAIL` line.

Use `VAMS_DEMO_OUTPUT=/path` to select a result directory or override
`VAMS_DEMO_QEMU_RISCV32`, `VAMS_DEMO_QEMU_X86_64`, and
`VAMS_ZEPHYR_FIRMWARE` for verified artifacts in another location.

Example high-level output:

```text
source-contract: PASS
firmware-pcie: PASS
payload-integrity: PASS
fault-recovery: PASS
queue-stress: PASS
VAMS demo evidence: build/demo/20260811T120000Z
VAMS DEMO: PASS
```

## Remaining release demonstration work

The current orchestrator demonstrates the actual firmware-owned command path,
payload engine, fault controls, and recovery directly through QTest. It does not
yet package a pinned Linux guest image or run the public driver/userspace API in
the same invocation. `kernel-smoke` separately boots a disposable Linux guest,
loads `vams_pci.ko`, exercises MSI-X and polling completion, runs concurrent NOP
and payload requests, validates per-file mapping ownership and close during DMA,
and verifies cleanup.

The final release demo will compose that guest test with this orchestrator once
the guest kernel, matching headers, module, project-owned static init/client,
and initramfs recipe have reproducible artifact manifests. The separate guest
already demonstrates copy/fill/CRC/vector through `/dev/vamsN`; composition and
artifact pinning are the remaining release work.
