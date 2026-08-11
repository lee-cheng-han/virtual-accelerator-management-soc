# Demo contract

## Current component demonstrations

The complete Linux-guest orchestration is not implemented yet, but the
accelerator, real Zephyr control plane, DMA operations, recovery, and fault
controls have executable hardware-free demonstrations:

```sh
make check
make firmware-pcie-smoke \
  CROSS_COMPILE=/path/to/riscv64-unknown-elf- \
  QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32 \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
make fault-injection-smoke \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
```

These commands validate documentation/ABI/source hygiene, carry PCI descriptors
through real firmware, verify all payload operations and reset recovery, then
exercise six debug-only faults and two race checkpoints. They do not yet claim
the final guest-driver/userspace workflow or physical performance.

## Incremental demonstrations

Each phase adds one observable proof: Phase 1 prints the RV32 boot
banner; Phase 2 boots Zephyr and task heartbeat; Phase 3 exercises mailbox and
watchdog; Phase 4 enumerates/probes and raises a test MSI-X; Phase 5 round-trips
NOP ID/cookie; Phase 6 verifies data operations; Phase 7 recovers a timeout;
Phase 8 demonstrates deterministic faults; all demonstrations through Phase 8
are implemented. Phase 9 will add the reproducible qualification report.

## Final `make demo` acceptance

Phase 10's noninteractive command must build pinned QEMU/firmware/driver/tools,
boot a prepared Linux guest and RISC-V firmware, wait with explicit timeouts,
load `vams_pci.ko`, display hardware/firmware/descriptor versions, run NOP/copy/
fill/CRC/vector commands with data verification, snapshot telemetry, inject one
DMA timeout or dropped interrupt, prove recovery with a clean NOP, shut down,
and print exactly one final PASS or FAIL summary.

The script captures QEMU, guest kernel, firmware UART, and test logs under a
timestamped result directory. Failure at any step preserves logs, exits nonzero,
and identifies the failed stage. It must not require interactive input, root on
the host beyond documented virtualization access, or network access after
dependencies/images are prepared. A `VAMS_KEEP_VM=1` diagnostic option may keep
the failed VM alive, but default CI behavior always cleans up.

Expected high-level output (future, not implemented):

```text
VAMS hardware interface 1.0, firmware <built-version>
queue pair: depth=<n>, MSI-X: ready
NOP: PASS  COPY: PASS  FILL: PASS  CRC32: PASS  VECTOR_ADD: PASS
fault: <name> triggered; recovery generation <old> -> <new>
post-recovery NOP: PASS
VAMS DEMO: PASS
```
