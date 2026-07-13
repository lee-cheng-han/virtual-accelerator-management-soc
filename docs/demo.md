# Demo contract

## Current Phase 0 demo

There is no executable accelerator yet. The honest Phase 0 demonstration is:

```sh
make check
make tree
make demo
```

The first command must report `Phase 0 documentation checks: PASS`; the second
shows the scaffold; the third explains that the full demo arrives in Phase 10.
No boot, PCI, firmware, driver, or performance claim is made.

## Incremental demonstrations

Each phase adds one observable proof: Phase 1 prints the three-line RV32 boot
banner; Phase 2 boots Zephyr and task heartbeat; Phase 3 exercises mailbox and
watchdog; Phase 4 enumerates/probes and raises a test MSI-X; Phase 5 round-trips
NOP ID/cookie; Phase 6 verifies data operations; Phase 7 recovers a timeout;
Phase 8 demonstrates deterministic faults; Phase 9 emits a reproducible report.

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

