# Verification plan

Testing grows with implementation; Phase 0 provides requirements and executable
documentation checks only. Planned tests are not reported as passing.

## Layers

| Layer | Primary method | Required coverage |
|---|---|---|
| Register/unit | QEMU unit/QTest | every reset value, RO/W1C/reserved/width behavior, illegal offsets, enable prerequisites |
| RISC-V subsystem | QTest + console smoke | ROM→SRAM boot, UART bytes, timer/IRQ routing, reset |
| Firmware logic | Zephyr ztest/native unit tests where portable | validation order, overflow, state transitions, EDF/FIFO tie, timeout, counter saturation, mailbox parser, watchdog epochs |
| PCI/queue model | QTest | config/BAR/MSI-X, doorbells, barriers, wrap/full/empty, DMA boundaries, CQ-before-interrupt ordering, generation callbacks |
| Kernel | KUnit + fault-injected probe paths, Sparse | probe/remove cleanup, masks, IRQ/poll, concurrency, close/unload/reset |
| End to end | guest VM scripts | command data integrity, errors, resets, lost IRQ, wrap, repeated stress |
| Robustness | libFuzzer/QEMU fuzz target when stable | MMIO sequences and 64-byte descriptor parser with bounded deterministic corpus |

## Requirement traceability

| Requirement group | Minimum gate evidence |
|---|---|
| SYS-01/02 | boot console golden output plus timer/interrupt/reset QTests |
| SYS-03 | Zephyr boot, task synchronization, mailbox/telemetry, and watchdog-reset tests |
| PCI-01 | config-space/BAR/MSI-X QTest plus guest enumeration, IRQ, probe/remove, and rebind smoke |
| ABI-01/02 | generated descriptor byte-layout compile/raw tests, NOP queue QTest, portal ownership QTest, Zephyr valid/invalid completion test, and deterministic reference-model comparison |
| CMD/DMA | golden buffers/CRC/vector results plus zero/max/overflow/alignment matrices |
| REC/HLT | each recovery scope with pre/post generations and telemetry assertions |
| FLT-01 | one-shot trigger, expected evidence, clean NOP after every fault |
| LNX-01 | nine forced probe cleanup points, tracked IRQ/poll completion, version rejection, and 32 concurrent guest NOP ioctls; payload/reset lifetime cases remain |

## Required cases

Register tests cover all defined bits individually and combined W1C, writing
zero, reserved ones, wrong access sizes/alignment, access during DMA, and each
reset scope. Queue tests cover depths 16 and 1024, every wrap boundary, one-slot
empty rule, malicious backward/leaping doorbells, CQ backpressure, interrupt
mask/reassert, and reset with a pending async DMA callback.

Descriptor tests construct raw little-endian byte arrays, not only C structs.
For every opcode they cover legal minimum/maximum, one below/above, misalignment,
zero/unused addresses, `UINT64_MAX` range overflow, every reserved field,
unsupported version/opcode/flag, duplicate ID, default/min/max/invalid timeout,
cookie extremes, and first-error precedence. Successful data commands compare
all output bytes; failed commands treat destination as unspecified but verify no
out-of-range access using guard pages/canaries.

Current driver tests force failure after PCI enable, BAR reservation/map, DMA
mask negotiation, ring allocation, vector allocation, each IRQ registration,
and character-device registration, followed by a
successful bind/remove/rebind cycle. Queue driver tests will add forced ring
allocation failure; cleanup must remain leak-free. They cover blocking and
nonblocking backpressure, close with requests outstanding, unknown/duplicate
completion, lost interrupt polling, reset serialization, concurrent users,
remove during idle, and unload after bounded cancellation.

## Tool policy and CI

Host/QEMU helper code uses GCC and Clang with `-Wall -Wextra -Wpedantic` and
warnings as errors for project-owned code. ASan/UBSan run unit tests. Firmware
uses Zephyr's supported warning/static-analysis path. Kernel builds use W=1,
Sparse, and later targeted Coccinelle rules. Formatting and docs checks run only
after their configuration is committed. Fuzzing is added when a stable parser
exists, with corpus/crash artifacts retained by CI.

CI jobs will pin QEMU, Zephyr SDK, compiler, and kernel versions when those
dependencies enter the project. Fast unit/QTest jobs run on each change; the VM
integration job may be separate but remains required for release. Random stress
always logs and accepts a seed so a failure is reproducible.

## Current validation

```sh
make check
git diff --check
```

`make check` verifies required documents, descriptor assertions as text, the
status label, and whitespace. The executable management subsystem additionally
uses `zephyr-smoke`, `management-mmio-smoke`, `management-smoke`,
`command-portal-smoke`, `firmware-command-smoke`, `firmware-pcie-smoke`, and
`watchdog-smoke`. PCI validation uses `pcie-smoke`, `nop-smoke`,
`queue-model-smoke`, and `kernel-smoke`. The model test compares every exposed
queue transition and completion across four deterministic randomized seeds.
The kernel test boots a disposable Linux initramfs and exercises coherent
queues, interrupt and polling completion, version rejection, 32 concurrent NOP
ioctls, module binding, and cleanup.
`firmware-pcie-smoke` separately proves valid and invalid PCI submissions cross
the private bridge into real Zephyr, and that an in-flight pre-reset completion
is discarded before a clean post-reset command completes. The same integration
test, exposed as `mem-copy-smoke`, `mem-fill-smoke`, and `crc32-smoke`, verifies
odd-length patterned copy and fill operations, an independently computed IEEE
CRC32, matching and mismatching expected results, exact byte counts,
destination guards, zero length, 64-bit range overflow, zero addresses, copy
overlap rejection, invalid CRC flags, and directional DMA failures.
`abi-check` verifies generated headers plus compiled and raw-byte layouts.
