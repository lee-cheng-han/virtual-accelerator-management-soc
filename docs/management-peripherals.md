# Management Peripherals

## Scope

The `vams-mgmt` QEMU device provides three firmware-facing 4 KiB MMIO regions:
a mailbox at `0x10010000`, a watchdog/reset/telemetry block at `0x10020000`,
and a private command portal at `0x10030000`. All defined registers are
naturally aligned, little-endian 32-bit accesses. Other widths, unaligned
accesses, and undefined offsets are invalid.

This is the internal management contract. It is separate from the future
host-visible PCI BAR contract in [the register map](register-map.md).

## Mailbox registers

| Offset | Name | Access | Meaning |
|---:|---|---|---|
| `0x00` | `MESSAGE` | RO | Current host-to-firmware message |
| `0x04` | `ACK` | WO | Write `1` after capturing `MESSAGE` |
| `0x08` | `RESPONSE` | RW | Firmware response payload |
| `0x0c` | `RESPONSE_DOORBELL` | WO | Write `1` to publish `RESPONSE` |
| `0x10` | `STATUS` | RO/W1C | Pending and overflow state |

`STATUS` bit 0 is host-to-firmware pending, bit 1 is firmware-to-host
pending, and bit 2 is host-to-firmware overflow. The mailbox asserts the
hart's machine-external interrupt while bit 0 is set. The ISR captures the
message, acknowledges the source, and enqueues only the 32-bit value; response
selection runs in the mailbox service task. A response doorbell while bit 1 is
already set is rejected and records a bad write.

The current model's `vams-mgmt.test-message` QOM property injects one nonzero
message at cold boot. Message `0x00000001` is a ping and returns
`0x80000001`; unsupported values return `0xffffffff`. The PCIe integration uses
a separate private command bridge and does not reuse this mailbox test path.

## Private command portal

The portal is an internal ownership boundary, not a second host ABI. It stages
one generated 64-byte submission and one 32-byte completion so the standalone
RISC-V harness can execute the same validation policy that will eventually sit
behind the PCI queue front end.

| Offset | Name | Access | Meaning |
|---:|---|---|---|
| `0x00` | `STATUS` | RO/W1C | Pending, overflow, and protocol-error bits |
| `0x04` | `HOST_SUBMIT` | WO | Write `1` after staging a descriptor |
| `0x08` | `FW_ACK` | WO | Write `1` after capturing the descriptor |
| `0x0c` | `FW_COMPLETE` | WO | Write `1` after staging a completion |
| `0x10` | `HOST_ACK` | WO | Write `1` after capturing the completion |
| `0x14` | `SUBMIT_COUNT` | RO | Saturating accepted-submission count |
| `0x18` | `COMPLETE_COUNT` | RO | Saturating published-completion count |
| `0x1c` | `RESULT_ACK` | WO | Write `1` after capturing an engine result |
| `0x20` | `RESULT_COUNT` | RO | Saturating accepted-engine-result count |
| `0x24` | `FW_ABORT` | WO | Write `1` after staging a firmware abort request |
| `0x28` | `ABORT_COUNT` | RO | Saturating accepted-abort-request count |
| `0x100–0x13c` | `SUBMISSION[0..15]` | RW | 64-byte descriptor staging window |
| `0x200–0x21c` | `COMPLETION[0..7]` | RW | 32-byte completion staging window |
| `0x300–0x31c` | `RESULT[0..7]` | RO | 32-byte modeled-engine result window |

`STATUS` bit 0 is host-to-firmware pending, bit 1 firmware-to-host pending,
bits 2 and 3 report attempted overwrite of the respective staging window, bit
4 reports an invalid ownership transition, and bit 5 reports a pending engine
result. Firmware captures all descriptor
words, executes a full memory fence, and only then acknowledges bit 0. It fills
all completion words, executes a full fence, and only then publishes bit 1.
Overwrite and protocol bits are sticky W1C diagnostics.

The Zephyr command task polls for bit 0 and validates NOP or any v1 payload
opcode using the normative first-error order. Payload authorization causes the
portal to accept one engine result; the recovery manager acknowledges bit 5,
validates ID and cookie, and publishes the terminal completion. At deadline it
stages `TIMED_OUT/TIMEOUT`, rings `FW_ABORT`, and waits a bounded 100 ms for the
result before escalation. The
`vams-mgmt.test-command` property injects a valid
NOP with value `1` or an
unsupported-version NOP with value `2` at cold boot. This is deterministic test
injection only. The optional `command-chardev` property now drives the same
ownership registers from the PCI queue controller in the dual-QEMU test.
The test-only `x-drop-abort-result` property drops one abort result to verify
the bounded escalation path; it defaults off.

## Watchdog, reset, and telemetry registers

| Offset | Name | Access | Meaning |
|---:|---|---|---|
| `0x00` | `WDT_TIMEOUT_MS` | RW | Deadline in milliseconds |
| `0x04` | `WDT_CONTROL` | RW | Bit 0 enables the watchdog |
| `0x08` | `WDT_PET` | WO | Reload on magic `0x56414d53` |
| `0x0c` | `RESET_REQUEST` | WO | Write bit 0 for management reset |
| `0x10` | `LAST_RESET_REASON` | RO | `0` power-on, `4` firmware, `5` watchdog |
| `0x14` | `WDT_RESET_COUNT` | RO | Saturating watchdog reset count |
| `0x18` | `FW_HEARTBEAT` | RW | Latest healthy firmware epoch |
| `0x1c` | `FW_UPTIME_LO` | RW | Firmware uptime bits 31:0 |
| `0x20` | `FW_UPTIME_HI` | RW | Firmware uptime bits 63:32 |
| `0x24` | `FW_VERSION` | RW | Packed firmware version |
| `0x28` | `MAILBOX_RX_COUNT` | RO | Saturating acknowledged-message count |
| `0x2c` | `MAILBOX_TX_COUNT` | RO | Saturating published-response count |
| `0x30` | `RESET_GENERATION` | RO | Management-reset generation |
| `0x34` | `STATUS` | RO/W1C | Bit 0 records a rejected write |
| `0x38` | `RESET_NOTIFY_COUNT` | RO | Accepted active-command reset notifications |
| `0x3c` | `RESET_NOTIFY_FAIL_COUNT` | RO | Failed active-command reset notifications |
| `0x40` | `RESET_ACK_COUNT` | RO | Accepted management/watchdog terminal acknowledgments |
| `0x44` | `RESET_ACK_TIMEOUT_COUNT` | RO | Expired 100 ms terminal-acknowledgment deadlines |
| `0x48` | `RESET_PENDING` | RO | A management/watchdog reset owns the terminal handshake |
| `0x4c` | `RESET_COALESCE_COUNT` | RO | Concurrent reset requests merged into a pending reset |
| `0x50` | `TEST_FREEZE_TASK` | RO | Test-only essential-task selector, zero by default |
| `0x100–0x17c` | `RETENTION[0..31]` | RW | 128-byte warm-reset-retained firmware crash record window |

The timeout defaults to 5000 ms and accepts values from 100 through 60000 ms
only while disabled. The firmware configures 1000 ms. Its health task samples
eight essential task epochs every 100 ms, applies two startup-grace samples,
and requires two consecutive misses. It publishes telemetry and pets the
watchdog only while every required task meets its deadline.

A management or watchdog reset first sends a privately tagged `RESET/RESET` for
any command still owned by the portal. The RV32 reset remains pending until the
PCI bridge returns the exact command ID, cookie, status, and error, or until a
100 ms virtual-time deadline expires. Concurrent reset requests coalesce and a
watchdog reason takes precedence. Success, timeout, and coalescing counters
survive the warm reset and are reported by the next firmware boot. The reset
then restores the ELF-backed SRAM
image and resets the CPU,
ACLINT, and UART. It clears volatile mailbox and firmware telemetry, while
preserving reset reason, reset generation, watchdog reset count, mailbox
counters, watchdog configuration, and diagnostic status. This makes a
watchdog reboot observable to the next firmware instance without retaining
mutated RAM-only kernel objects. The separate retention window survives warm
reset and migration but clears on cold reset; firmware stores a versioned,
CRC-protected bounded crash record there.

## Validation

Six focused tests cover the contract:

```sh
make management-mmio-smoke QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32
make management-smoke QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32
make watchdog-smoke QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32
make command-portal-smoke QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32
make firmware-command-smoke QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32
make firmware-health-smoke QEMU_SYSTEM_RISCV32=/path/to/qemu-system-riscv32
```

The QTest smoke checks reset values, W1C acknowledgment, writable timeout, and
telemetry storage. The runtime mailbox test checks ISR-to-task delivery,
response publication, health epochs, and repeated telemetry. The watchdog test
withholds the first pet, requires a second boot with reason 5/count 1, and then
requires continued healthy telemetry with no assertion or trap report. The
portal QTest checks ownership, overwrite rejection, sticky diagnostics,
counters, and exact completion bytes. The firmware test requires exact success
and unsupported-version completions from Zephyr. The health test freezes each
of eight essential tasks, checks exact stuck-task evidence, forces watchdog
recovery, validates the retained record on the second boot, and requires
post-reset progress.

## Current limitations

- The mailbox and polling UART share the hart's direct machine-external input;
  a PLIC is deferred until the PCIe interrupt topology needs it.
- There is one four-entry mailbox receive queue and one outstanding response.
- The command portal holds one descriptor, engine result, and completion; it is
  intentionally polling-only.
- Telemetry reads are individually atomic 32-bit operations; the split uptime
  value does not yet provide a multiword snapshot protocol.
- Management state is migration version 8 while earlier snapshots remain
  loadable with empty result state. End-to-end live migration is not yet an
  accepted or tested platform capability.
