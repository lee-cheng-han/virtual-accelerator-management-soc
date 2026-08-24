# PCIe Endpoint

## Implemented scope

The QEMU type `vams-pcie` is a PCI Express processing-accelerator endpoint. It
provides the host-visible identity/control foundation plus one coherent command
queue. The private dual-QEMU bridge and all firmware-owned v1 payload operations
now work in integration tests. A virtual-time engine supplies BUSY state,
deadline completion, bounded payload DMA, reset-safe callback cancellation, and
engine-only recovery, firmware-issued abort control, management-reset terminal
notification, bounded reset acknowledgment, and CQ watermark throttling. Host
payload UAPI remains
unavailable.

The optional `x-vams-debug=true` property exposes deterministic fault and
checkpoint registers for QTest only. Without it, capabilities remain
`0x000000b3`, the block reads as unimplemented, and accesses set illegal-MMIO.
Debug-enabled test instances advertise `0x000000f3`; this is not a production
feature contract.

| PCI field | Value |
|---|---:|
| Vendor ID | `0x1b36` |
| Device ID | `0x1100` |
| Revision | `0x00` |
| Class code | `0x120000` (processing accelerator) |
| Subsystem vendor/device | `0x1b36` / `0x1100` |

BAR0 is the normative 4 KiB non-prefetchable register BAR. BAR1 is a separate
4 KiB QEMU-managed MSI-X table/PBA BAR with two vectors. BAR1 contains only
standard PCI MSI-X structures and is not part of the VAMS register ABI.

## Implemented BAR0 registers

The following registers implement the behavior defined by the
[host register map](register-map.md):

| Offset | Register | Current value or behavior |
|---:|---|---|
| `0x000` | `VAMS_DEVICE_ID` | `0x11001b36` |
| `0x004` | `VAMS_HW_IF_VERSION` | `0x00010000` |
| `0x008` | `VAMS_FW_VERSION` | Zero until firmware integration |
| `0x00c` | `VAMS_DESC_VERSION` | Descriptor format 1 |
| `0x010` | `VAMS_CAPABILITIES` | `0x000000b3`, DMA, MSI-X, engine reset, polling-safe CQ, CQ watermarks |
| `0x014` | `VAMS_MAX_TRANSFER` | 16 MiB architectural limit |
| `0x018` | `VAMS_QUEUE_LIMITS` | Depth range 16–1024 |
| `0x01c` | `VAMS_DEVICE_STATUS` | READY or RESETTING |
| `0x020` | `VAMS_DEVICE_CONTROL` | QUIESCE storage, RESET request; ENABLE is rejected |
| `0x024` | `VAMS_ERROR_STATUS` | Defined W1C error bits |
| `0x028` | `VAMS_RESET_GENERATION` | Increments on device-control reset |
| `0x02c` | `VAMS_LAST_FATAL` | Preserved until cold reset |
| `0x220` | `VAMS_CQ_WATERMARK` | Configurable high/low occupancy thresholds |
| `0x224` | `VAMS_CQ_HIGH_WATER` | Peak CQ occupancy since cold reset |
| `0x228` | `VAMS_CQ_BACKPRESSURE_COUNT` | Saturating throttle-entry count |
| `0x300` | `VAMS_INTR_STATUS` | Four sticky W1C sources |
| `0x304` | `VAMS_INTR_MASK` | Four source masks |
| `0x308` | `VAMS_INTR_FORCE` | Deterministic source assertion |
| `0x30c` | `VAMS_INTR_COALESCE` | Reset value 1 only |
| `0x500` | `VAMS_ENGINE_STATUS` | BUSY plus sticky engine-error summary |
| `0x504` | `VAMS_ENGINE_CONTROL` | Reads zero; host writes rejected |
| `0x508` | `VAMS_ENGINE_COMMAND_ID` | Active command ID, zero when idle |
| `0x50c` | `VAMS_ENGINE_ERROR` | Read-only DMA direction/timeout evidence |
| `0x510` | `VAMS_ENGINE_EPOCH` | Private cancellation epoch |
| `0x60c` | `VAMS_RESET_REQUEST` | Engine-only reset request implemented |
| `0x610` | `VAMS_LAST_RESET_REASON` | Power-on/device/queue/engine reason |
| `0x614` | `VAMS_RESET_STATUS` | Zero for synchronous engine reset |
| `0xf00–0xf10` | Fault controls/evidence/lock | Present only with `x-vams-debug=true` |
| `0xf14–0xf1c` | Named checkpoint controls | Engine-start and pre-CQ pause/release |

The SQ block at `0x100–0x11f` and CQ block at `0x200–0x22b` implement the
normative base, depth, index, doorbell, control, and status registers. BAR0
offsets outside the listed subset are deliberately unimplemented. They return all ones,
ignore writes, and set `VAMS_ERR_ILLEGAL_MMIO`. Accesses that are not aligned
32-bit operations behave the same way. RO writes, reserved-bit writes, and
invalid configuration requests set their distinct normative error bits.

Device ENABLE requires configured enabled SQ/CQ registers and PCI bus mastering.
The DMA capability covers descriptor/completion transport and the implemented
copy, fill, CRC32, and vector-add payload operations.

## Interrupt and reset behavior

MSI-X vector 0 serves the CQ source. Vector 1 serves ERROR, FW_EVENT, and
RESET_DONE. VAMS source masking is independent from PCI MSI-X masking: a source
remains sticky, and QEMU records a pending MSI-X table bit when the PCI vector
cannot be delivered. Unmasking a pending VAMS source issues the corresponding
notification.

Writing `DEVICE_CONTROL.RESET` begins an asynchronous device reset. RESETTING
asserts immediately, ENABLE/QUIESCE clear, the generation increments, and the
interrupt sources become masked. One millisecond of virtual time later READY
asserts and RESET_DONE becomes pending. Error state clears except for the
architecturally persistent fatal bit; last-fatal state remains preserved until
cold reset.

The PCI configuration, MSI-X table/PBA, BAR0 state, in-progress reset timer, and
active engine command/timer/deadline/generation are migration state. Live
migration is not an accepted platform feature until an end-to-end migration
regression exists.

Migration state version 11 includes fault state, bridge control, CQ watermark
configuration/evidence, the reset-acknowledgment timer/counter, debug lock,
engine-hung state, and checkpoints. A destination still
must be launched with a compatible debug property; live migration remains
outside the accepted feature set.

Writing the engine bit in `RESET_REQUEST` synchronously cancels active work,
publishes `RESET/RESET`, increments the private engine epoch, records
host-engine as the reset reason, and preserves the queue generation and pending
descriptors. A stale callback cannot modify payload or publish another CQ entry.

If management firmware or its watchdog resets while it owns a request, the
management model emits `RESET/RESET` before clearing portal state. The PCIe
endpoint accepts that terminal notification during validation or active engine
execution, cancels the engine epoch, and publishes exactly one CQ entry.

The CQ high and low watermarks form a hysteresis pair. Reaching the high value
sets `CQ_STATUS.BACKPRESSURE`, increments a saturating counter, and stops new SQ
consumption. Draining to the low value clears throttling and resumes work. The
Linux driver programs 12/8 for its depth-16 queues; the compatibility default
uses 15/14.

Queue and device reset can transfer an active engine result back to firmware
after host-visible queue state has already been invalidated. The endpoint waits
100 ms of virtual time for firmware's terminal acknowledgment. An on-time reply
cancels the timer; expiry clears the private bridge owner, increments
`RESET_ACK_TIMEOUT_COUNT`, sets firmware and queue errors, and permits later
reconfiguration. The timer and evidence are migration state, and older incoming
state with an outstanding discard receives a fresh bounded deadline.

## Validation

Build QEMU with `x86_64-softmmu`, then run:

```sh
make pcie-smoke \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
make fault-injection-smoke \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
make cq-backpressure-smoke \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
```

The QTest smoke uses a Q35 PCIe root bus and verifies:

- vendor/device, revision/class, BAR sizes, and PCI capability chaining;
- the two-vector MSI-X table and PBA layout;
- every currently implemented BAR0 reset value;
- source-to-vector mapping for vectors 0 and 1 while PCI MSI-X is masked;
- W1C, RO-write, reserved-write, invalid-width, and unimplemented-offset errors;
- rejection of ENABLE before valid queues and bus mastering are configured; and
- asynchronous RESETTING → READY behavior, generation increment, and
  RESET_DONE publication.

The CQ regression configures 4/2 high/low thresholds, submits eight NOPs, and
requires three exact throttle/resume transitions, a peak occupancy of four,
and no early SQ consumption or lost/duplicate completion.

The fault regression separately verifies production gating, rejected
multi-fault arms, six one-shot triggers, Nth-match selection, MSI-X suppression
against a normal PBA transition, timeout/hang/DMA/reset recovery, two paused
race windows, persistent evidence, lockout, and a clean command after each
fault.

The Linux `vams_pci` driver now validates this contract, installs both MSI-X
handlers, and is exercised by a disposable-guest probe/failure/remove test. See
the [Linux PCI driver guide](linux-pci-driver.md). `nop-smoke` separately
verifies coherent DMA ordering, descriptor validation, completion bytes,
doorbells, interrupts, and queue reset.
