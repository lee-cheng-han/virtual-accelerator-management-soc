# Reproducible virtual-platform builds

`tools/versions.env` is the single source for the QEMU, patch-series, patched
source, Zephyr, and expected RISC-V GCC revisions. `make check` cross-validates
those pins against the compatibility matrix and checked-in qualification
evidence.

## QEMU

`make qemu-prepare` clones the exact detached QEMU commit when necessary,
verifies the ordered patch-series SHA-256, applies it only to a recognized clean
tree, verifies the resulting source-state SHA-256, configures only the RV32 and
x86-64 system targets, and builds them. A marker in the ignored build directory
prevents accidental double application while still rejecting changed pins.

The preparation step may access the network to clone QEMU and obtain host build
dependencies not already available. Re-running against prepared source uses the
local tree. The resulting executables identify the pinned upstream commit, and
the demo records their content hashes.

## Zephyr firmware

`make zephyr-prepare` clones Zephyr v4.4.0 at its exact commit, creates the
repository-local Python environment, and initializes west. `make zephyr`
requires an RV32-capable bare-metal GCC prefix and does not modify the Zephyr
source. The expected compiler release is recorded with the other pins; a future
container/toolchain archive must add a content hash before builds can be called
bit-for-bit reproducible across hosts.

## Offline validation and evidence

After QEMU and firmware artifacts exist, `make demo` performs no downloads. It
checks the QEMU embedded version before execution and records source state,
pins, artifact hashes, host details, stage commands, durations, results, and
logs. `make stress-qualification` and `make firmware-resource-report` emit
separate JSON evidence under `build/reports/`.

The lightweight CI job always validates generated contracts, source hygiene,
release inputs, and host tests. A second job applies the patch series to a clean
pinned QEMU checkout. Main-branch pushes and manual runs additionally build the
virtual platform and execute model, fuzz, fault, and stress regressions, then
archive their evidence.

## Current reproducibility boundary

The source inputs and QEMU transformation are content-pinned. Host packages,
the RISC-V compiler binary, Zephyr Python wheels, Linux kernel/header pair, and
BusyBox guest artifact are not yet all stored in a hash-locked manifest. Until
they are, builds are repeatable from exact sources but are not claimed to be
bit-identical across distributions. The runtime demo remains deterministic in
behavior and records enough identity data to reproduce its environment.
