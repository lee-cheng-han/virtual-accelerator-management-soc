# System architecture

## Component view

```text
 process -> libvams/vamsctl -> ioctl/mmap/poll -> vams_pci
                                               | coherent SQ/CQ
                                               | mapped payload DMA
                                               v
 +--------------------------- vams-pcie ----------------------------+
 | PCI config | BAR0 | MSI-X | queue front end | PCI DMA requester  |
 |                                      |                            |
 |                              command/event fabric                 |
 |                              |                 |                  |
 |                       processing engine     telemetry             |
 |                              |                 |                  |
 | +--------------------- management subsystem -------------------+ |
 | | RV32IMAC | ROM | SRAM | UART | timer/IRQ | mailbox | WDT/RST | |
 | |                    Zephyr firmware                           | |
 | | receive -> validate -> schedule -> DMA/engine -> complete    | |
 | +--------------------------------------------------------------+ |
 +-----------------------------------------------------------------+
```

The QEMU integration is maintained as a small out-of-tree patch series until
interfaces stabilize. The implemented `vams-pcie` shell connects to QEMU's
PCIe bus with vendor `0x1b36`, device `0x1100`, revision `0x00`, and class
`0x120000` (processing accelerator). It currently implements BAR0 identity,
control/error handling, and MSI-X. QEMU system binaries are target-specific, so
an x86 system process cannot instantiate the RV32 CPU model directly. The
hardware-free co-simulation therefore runs `vams-pcie` and the `vams_riscv`
management subsystem in separate QEMU processes joined by a private chardev.
Only the PCI function is host-visible. The bridge carries fixed generated-ABI
descriptors and completions; Zephyr still owns validation and completion
policy. The development identity is provisional and is not an allocated
production ID.

The implemented `vams_pci` driver validates the endpoint identity and interface,
negotiates a coherent DMA mask, owns BAR0, installs both MSI-X handlers, and
allocates one coherent SQ/CQ pair when capability bit 0 is set. The current NOP
transport retains an in-process validator for isolated endpoint tests. In the
integrated configuration, the endpoint DMA-fetches one descriptor, stages it
over the private bridge, waits for real Zephyr firmware to complete it, and
then DMA-publishes the returned completion. An implementation-independent
executable model compares queue indices, status, errors, interrupts, generation
changes, and completion contents with the endpoint after every operation in
deterministic randomized sequences. Queue or device reset marks an in-flight
bridge result for discard, preventing a pre-reset firmware completion from
entering the new CQ generation.

For `MEM_COPY`, `MEM_FILL`, and `CRC32`, Zephyr validates version, opcode, flags,
reserved fields, timeout, length, overflow, and required addresses before
authorizing execution. Copy additionally rejects overlapping ranges. The PCI
model rechecks the hardware safety conditions and constructs a bounded private
payload buffer. Copy and fill DMA-write their destination; CRC32 returns the
IEEE result in the completion and can compare it with a caller-provided value.
Copy and CRC32 read the full source; fill reads exactly one source byte.

## Address spaces

Host BAR0 is one 4 KiB non-prefetchable register BAR. A later optional BAR2 may
expose local accelerator memory, but release-1 commands use DMA addresses and do
not require BAR2. PCI DMA uses QEMU's bus-master address space and is disabled
until PCI command memory-space and bus-master enables are set.

The management subsystem initially uses this private physical map:

| Address range | Size | Function |
|---|---:|---|
| `0x0000_1000–0x0001_0fff` | 64 KiB | immutable boot ROM |
| `0x0200_0000–0x0200_ffff` | 64 KiB | interrupt controller/timer window |
| `0x1000_0000–0x1000_0fff` | 4 KiB | UART |
| `0x1001_0000–0x1001_0fff` | 4 KiB | mailbox/doorbell bridge |
| `0x1002_0000–0x1002_0fff` | 4 KiB | watchdog/reset/telemetry bridge |
| `0x1003_0000–0x1003_0fff` | 4 KiB | private firmware command portal |
| `0x8000_0000–0x8007_ffff` | 512 KiB | firmware SRAM |

The internal peripheral registers are normative in the
[management-peripheral contract](management-peripherals.md). The BAR0 contract
is normative in the register map. QEMU restores the ELF-backed SRAM image on a
management reset before the reset ROM branches to it, while the peripheral
retains reset-cause telemetry. Secure boot is out of scope.

## Host stack and queues

The initial `/dev/vamsN` API provides versioned device information and tracked
synchronous NOP submission. `libvams`, `vamsctl`, and the benchmark will build
on it as payload ownership is implemented. The thin `vams_pci` driver owns PCI enable,
64-bit DMA mask negotiation, coherent ring allocation, payload mapping, MSI-X,
backpressure, reset serialization, and process cleanup. It does not validate
opcode-specific policy or schedule commands.

The host configures one SQ and CQ while disabled, then enables queues and the
device. The rings use producer/consumer indices modulo depth. One slot is kept
empty, making usable capacity `depth - 1`; this makes full and empty
unambiguous without phase bits. Firmware captures a submission before advancing
SQ head. Completion congestion stops SQ consumption before firmware's bounded
internal command pool can overflow. The driver rejects new submissions while
resetting and wakes blocked submitters when SQ head advances.

## Command and DMA path

1. The driver maps payloads and writes a 64-byte SQ entry.
2. It publishes SQ tail with a release barrier and rings `SQ_DOORBELL`.
3. Hardware notifies firmware; the receiver DMA-fetches one entry.
4. Firmware validates it, allocates command state, and advances SQ head.
5. The scheduler dispatches to the DMA/processing engine. The v1 engine is
   single-issue; vector add is element-wise little-endian `uint32_t` addition
   modulo 2^32.
6. Firmware creates a 32-byte completion. Hardware DMA-writes it, publishes CQ
   tail, and raises MSI-X if unmasked.
7. The driver drains CQ, returns cookies/results, advances CQ head, and unmaps
   buffers after device ownership ends.

DMA and descriptor fetches are asynchronous QEMU operations. The device checks
`address + length - 1` for 64-bit overflow before any access. Reset first blocks
new DMA, drains/cancels model-owned transactions, increments `RESET_GENERATION`,
and only then exposes READY. Generation is internal command metadata, not a v1
descriptor field; it prevents a pre-reset callback from producing a stale CQ
entry.

## Interrupt flow

Vector 0 signals CQ availability; vector 1 may signal fatal/error state. A CQ
write becomes globally visible before status, tail, and MSI-X. The host ISR
acknowledges/masks only as needed and schedules threaded/NAPI-style draining.
Firmware peripheral ISRs acknowledge the device source and enqueue an event;
they never parse descriptors or perform DMA. Interrupt status remains sticky
until W1C acknowledgment. A lost MSI-X is not data loss: driver polling of CQ
tail and a watchdog timer provide fallback.

## Recovery hierarchy

Recovery escalates only as far as needed: reject/abort one command, reset queue
indices, reset DMA/engine, restart management firmware, then PCI function reset.
Every reset increments a generation and records a reason. No queued or running
command survives queue-level or stronger reset in release 1; the driver returns
reset/cancel status to known request owners and userspace decides whether retry
is safe. See the fault/recovery document for preservation details.

## Differentiation

BAR writes carry configuration and doorbells, not command payloads. The useful
path is PCIe bus-master DMA through versioned rings, controlled by executable
RISC-V RTOS firmware with task boundaries, scheduling, watchdog supervision,
telemetry, and layered recovery. The internal mailbox is only an interrupt/event
bridge between modeled hardware and firmware. Thus neither a platform MMIO
device nor a Linux character driver is the architectural center.
