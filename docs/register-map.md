# Host BAR0 register map

This is the normative release-1 host-visible register contract. It describes
the final release-1 values; an intermediate phase must read capabilities and
must not advertise a feature until it works.

The current QEMU PCIe model implements identification/device control, SQ/CQ,
and interrupt control. It advertises capability bits 0, 1, and 5: DMA, MSI-X,
and polling-safe CQ. MEM_COPY and MEM_FILL payload DMA are implemented without
the planned engine-control register block. Watchdog bridge, host telemetry
snapshot, engine registers, and debug blocks remain unimplemented and return
illegal-MMIO behavior. See the [NOP command-path guide](nop-command-path.md),
[MEM_COPY guide](mem-copy-command-path.md), and
[MEM_FILL guide](mem-fill-command-path.md) for the implemented subset.

## Global rules

BAR0 is a 4096-byte, non-prefetchable PCI memory BAR. Registers are
little-endian. Unless a row says otherwise, access width is exactly 32 bits,
reads have no side effects, writes have only the listed effects, and access is
safe during active DMA. Aligned 8-, 16-, or 64-bit accesses and accesses to an
unimplemented offset return all ones on read and are ignored on write while
setting `VAMS_ERR_ILLEGAL_MMIO`. QEMU must not crash or split an illegal access.

Reserved bits read zero and writes to them are ignored and set
`VAMS_ERR_RESERVED_WRITE`. RO writes are ignored and set
`VAMS_ERR_RO_WRITE`. W1C accepts any combination of defined bits: writing one
clears that bit, zero has no effect, and reserved ones follow the global rule.
W1S writes set defined bits. RW registers retain legal values. Illegal field
values leave the complete register unchanged and set `VAMS_ERR_BAD_CONFIG`.

`Cold` reset means PCI function creation/power cycle. `Device` reset means FLR
or `DEVICE_CONTROL.RESET`; `Mgmt` reset restarts only the RISC-V subsystem;
`Queue` reset affects SQ/CQ. Unless stated otherwise, the listed reset value is
applied by Cold and Device reset. Queue configuration is writable only while
its enable bit is zero and no reset is active. Base low is staged until base
high is written, then the aligned 64-bit pair commits atomically.

## Block allocation

| Range | Block |
|---|---|
| `0x000–0x0ff` | identification, capabilities, device state |
| `0x100–0x1ff` | submission queue |
| `0x200–0x2ff` | completion queue |
| `0x300–0x3ff` | interrupt control |
| `0x400–0x4ff` | host/firmware management mailbox |
| `0x500–0x5ff` | processing engine |
| `0x600–0x6ff` | watchdog and reset |
| `0x700–0x7ff` | health and telemetry |
| `0xf00–0xfff` | debug-only deterministic fault injection |

## Identification and device control

| Offset | Register | Reset | Access | Owner | Definition / write and reset effects |
|---:|---|---:|---|---|---|
| `000` | `VAMS_DEVICE_ID` | `11001b36` | RO | HW | `[15:0]=vendor 1b36`, `[31:16]=device 1100`. |
| `004` | `VAMS_HW_IF_VERSION` | `00010000` | RO | HW | ABI interface major 1 in `[31:16]`, minor 0 in `[15:0]`. |
| `008` | `VAMS_FW_VERSION` | `00000000` | RO | FW | Firmware semantic `major.minor.patch` as `[31:24].[23:16].[15:0]`; zero until boot publishes it; Mgmt reset clears it. |
| `00c` | `VAMS_DESC_VERSION` | `00000001` | RO | HW | Highest accepted descriptor version; release 1 accepts exactly 1. |
| `010` | `VAMS_CAPABILITIES` | `0000007f` | RO | HW | Bits 0 DMA, 1 MSI-X, 2 watchdog, 3 telemetry, 4 engine reset, 5 polling-safe CQ, 6 debug fault block. Feature bits read zero until implemented. |
| `014` | `VAMS_MAX_TRANSFER` | `01000000` | RO | HW | Maximum bytes in one command: 16 MiB. |
| `018` | `VAMS_QUEUE_LIMITS` | `04000010` | RO | HW | `[15:0]=minimum depth 16`, `[31:16]=maximum depth 1024`. |
| `01c` | `VAMS_DEVICE_STATUS` | `00000000` | RO | HW/FW | Bit 0 READY, 1 FW_RUNNING, 2 QUEUES_READY, 3 ENGINE_BUSY, 4 RESETTING, 5 FATAL, 6 CQ_STALLED. Live status; Device reset clears all, firmware/hardware set as state advances. |
| `020` | `VAMS_DEVICE_CONTROL` | `00000000` | RW/W1S | Host | Bit 0 ENABLE (RW), 1 RESET (self-clearing W1S), 2 QUIESCE (RW). ENABLE requires valid enabled SQ/CQ and bus mastering; otherwise unchanged + BAD_CONFIG. RESET starts Device reset. QUIESCE stops new SQ fetch but allows active work to finish. |
| `024` | `VAMS_ERROR_STATUS` | `00000000` | W1C | HW sets; host clears | Bits: 0 ILLEGAL_MMIO, 1 RESERVED_WRITE, 2 RO_WRITE, 3 BAD_CONFIG, 4 DMA, 5 QUEUE, 6 FW, 7 ENGINE, 8 WATCHDOG, 9 FATAL. Any combination may be cleared; zero no-op; Device reset clears except FATAL, which Cold reset clears. |
| `028` | `VAMS_RESET_GENERATION` | `00000000` | RO | HW | Incremented modulo 2^32 on every Queue, Mgmt, or Device reset; never written by FW. An engine-only reset uses a private engine epoch because queued commands survive. Preserved across resets, cleared only by Cold reset. Wrap is legal. |
| `02c` | `VAMS_LAST_FATAL` | `00000000` | RO | HW/FW | First fatal error code since Cold reset; subsequent fatal errors do not overwrite it. |

Device reset is asynchronous. RESET reads zero even while reset proceeds;
`STATUS.RESETTING` is the completion indication. ENABLE is cleared at reset
entry. Host must reread generation and reconfigure after READY returns.

## Submission queue

| Offset | Register | Reset | Access | Owner | Definition / write and reset effects |
|---:|---|---:|---|---|---|
| `100` | `VAMS_SQ_BASE_LO` | `00000000` | RW | Host | DMA base `[31:0]`; staged, 64-byte alignment required on pair commit. |
| `104` | `VAMS_SQ_BASE_HI` | `00000000` | RW | Host | DMA base `[63:32]`; write commits pair. |
| `108` | `VAMS_SQ_DEPTH` | `00000000` | RW | Host | Power of two 16–1024; zero means unconfigured. |
| `10c` | `VAMS_SQ_HEAD` | `00000000` | RO | FW | Consumer index `[9:0]`, always less than depth; advances only after local descriptor capture. |
| `110` | `VAMS_SQ_TAIL` | `00000000` | RO | HW | Last accepted producer index from doorbell. |
| `114` | `VAMS_SQ_DOORBELL` | `00000000` | WO | Host | Write proposed tail `[9:0]`. Value must be `< depth` and must not advance through/past head; illegal value rejected + QUEUE. A valid write publishes prior SQ memory and wakes firmware. Read returns zero. |
| `118` | `VAMS_SQ_CONTROL` | `00000000` | RW/W1S | Host | Bit 0 ENABLE, bit 1 RESET self-clearing. Enable requires committed base/depth and disabled device queue; reset clears indices/status and also resets CQ to prevent orphan completions. |
| `11c` | `VAMS_SQ_STATUS` | `00000000` | RO | HW/FW | Bit 0 ENABLED, 1 EMPTY, 2 FULL, 3 FETCH_ACTIVE, 4 ERROR. EMPTY becomes 1 once a valid enabled empty ring exists. |

Queue reset clears head/tail and queue status, retains base/depth, clears ENABLE,
cancels all accepted commands, increments generation, and requires both queues
to be re-enabled. Device reset also clears base/depth. SQ configuration and
doorbell are unsafe during active queue DMA except a RESET write.

## Completion queue

| Offset | Register | Reset | Access | Owner | Definition / write and reset effects |
|---:|---|---:|---|---|---|
| `200` | `VAMS_CQ_BASE_LO` | `00000000` | RW | Host | DMA base `[31:0]`; staged, 64-byte alignment required. |
| `204` | `VAMS_CQ_BASE_HI` | `00000000` | RW | Host | DMA base `[63:32]`; write commits pair. |
| `208` | `VAMS_CQ_DEPTH` | `00000000` | RW | Host | Power of two 16–1024; zero means unconfigured. |
| `20c` | `VAMS_CQ_HEAD` | `00000000` | RO | HW | Last accepted host consumer index. |
| `210` | `VAMS_CQ_TAIL` | `00000000` | RO | FW | Producer index; advances only after completion DMA is visible. |
| `214` | `VAMS_CQ_DOORBELL` | `00000000` | WO | Host | New head `[9:0]`, `< depth`, may advance only across produced entries. Valid write releases CQ slots and may resume SQ. Read zero. Illegal advancement rejected + QUEUE. |
| `218` | `VAMS_CQ_CONTROL` | `00000000` | RW/W1S | Host | Bit 0 ENABLE, bit 1 RESET self-clearing. RESET has the paired queue-reset behavior above. |
| `21c` | `VAMS_CQ_STATUS` | `00000000` | RO | HW/FW | Bit 0 ENABLED, 1 EMPTY, 2 FULL, 3 WRITE_ACTIVE, 4 ERROR. FULL asserts with one slot unused and stalls acceptance before internal capacity exhausts. |

## Interrupt control

| Offset | Register | Reset | Access | Owner | Definition / write and reset effects |
|---:|---|---:|---|---|---|
| `300` | `VAMS_INTR_STATUS` | `00000000` | W1C | HW/FW set; host clears | Bits 0 CQ, 1 ERROR, 2 FW_EVENT, 3 RESET_DONE. Sticky regardless of mask. Cleared combinations are supported; Device reset clears all except it sets RESET_DONE when complete. |
| `304` | `VAMS_INTR_MASK` | `0000000f` | RW | Host | One masks the corresponding status source. Masking does not clear status. Reserved bits illegal. Device reset masks all. |
| `308` | `VAMS_INTR_FORCE` | `00000000` | WO | Host/test | Defined bit writes set matching status for interrupt path testing; prohibited when `DEBUG_LOCK=1`, then ignored + BAD_CONFIG. Read zero. |
| `30c` | `VAMS_INTR_COALESCE` | `00000001` | RW | Host | `[15:0]` CQ completion threshold 1–256; `[31:16]` timeout in 10-us units, 0 disables timer. Illegal zero threshold rejected. Phase 5 may support only reset value and advertise no coalescing capability. |

An unmasked 0→1 pending transition triggers its assigned MSI-X vector. Vector 0
serves CQ; vector 1 serves other bits when available, otherwise all share vector
0. Clearing status while more CQ entries exist causes CQ status to be set again,
preventing a lost level. All registers are safe during active DMA.

## Host/firmware management mailbox

This block carries lifecycle notifications, never command payloads.

| Offset | Register | Reset | Access | Owner | Definition / write and reset effects |
|---:|---|---:|---|---|---|
| `400` | `VAMS_H2F_MESSAGE` | `00000000` | RW | Host produces; FW consumes | 32-bit management message staged before doorbell. |
| `404` | `VAMS_H2F_DOORBELL` | `00000000` | WO | Host | Writing 1 latches MESSAGE if not pending and signals FW; other defined value illegal. Pending collision ignored + QUEUE. |
| `408` | `VAMS_F2H_MESSAGE` | `00000000` | RO | FW produces; host consumes | Latched firmware management response/event. |
| `40c` | `VAMS_MAILBOX_STATUS` | `00000000` | W1C | HW | Bit 0 H2F_PENDING (FW clears internally), 1 F2H_PENDING (host W1C), 2 H2F_OVERFLOW, 3 CORRUPT. Host may W1C bits 1–3 but not bit 0; an attempt is RO_WRITE. Mgmt reset clears pending and sets CORRUPT if a message was lost. |

## Processing engine

| Offset | Register | Reset | Access | Owner | Definition / write and reset effects |
|---:|---|---:|---|---|---|
| `500` | `VAMS_ENGINE_STATUS` | `00000000` | RO | HW | Bit 0 BUSY, 1 DMA_ACTIVE, 2 HUNG, 3 ERROR. Engine reset clears all. |
| `504` | `VAMS_ENGINE_CONTROL` | `00000000` | W1S | FW via bridge | Bit 0 ABORT, 1 RESET. Host writes rejected as RO_WRITE. RESET blocks DMA, increments a private engine epoch, and clears engine state; it does not change host reset generation. |
| `508` | `VAMS_ENGINE_COMMAND_ID` | `00000000` | RO | FW/HW | Running command ID; zero when idle (ID zero remains legal, so STATUS determines validity). |
| `50c` | `VAMS_ENGINE_ERROR` | `00000000` | W1C | HW sets; FW clears | Bits 0 DMA_READ, 1 DMA_WRITE, 2 TIMEOUT, 3 ABORT_FAILED, 4 BAD_OPERATION. Device reset also clears. Host writes rejected. |

## Watchdog and reset

| Offset | Register | Reset | Access | Owner | Definition / write and reset effects |
|---:|---|---:|---|---|---|
| `600` | `VAMS_WDT_TIMEOUT_MS` | `00001388` | RW | Host policy; FW validates | Timeout 100–60000 ms; reset 5000. Writable only while WDT disabled. |
| `604` | `VAMS_WDT_CONTROL` | `00000000` | RW | Host/FW policy | Bit 0 ENABLE, bit 1 PAUSE_WHEN_DEBUGGED. Reserved bits illegal. Enabling starts countdown. Device reset disables; Mgmt reset preserves ENABLE and restarts countdown. |
| `608` | `VAMS_WDT_PET` | `00000000` | WO | FW | Magic write `0x56414d53` reloads; any other write sets FW error and does not reload. Host write rejected. Read zero. |
| `60c` | `VAMS_RESET_REQUEST` | `00000000` | W1S | Host | Bits 0 QUEUES, 1 ENGINE, 2 MGMT, 3 DEVICE. Exactly one bit is legal; multiple/zero rejected. Self-clears when accepted. |
| `610` | `VAMS_LAST_RESET_REASON` | `00000000` | RO | HW | Enum: 0 power-on, 1 host-device, 2 host-queue, 3 host-engine, 4 host-mgmt, 5 watchdog, 6 fatal, 7 QEMU migration. Updated at reset entry; preserved except Cold reset. |
| `614` | `VAMS_RESET_STATUS` | `00000000` | RO | HW | Bit 0 IN_PROGRESS; `[7:4]` scope enum. Cleared only after DMA is stopped and reset is complete. |

## Health and telemetry

Counters saturate at `UINT64_MAX` and never wrap. They are firmware-owned except
where noted. Writing `VAMS_TELEM_SNAPSHOT=1` atomically copies all live values
into the readable shadow bank; reads return the most recent snapshot. The first
snapshot after reset returns reset values. Snapshot is safe during DMA.

| Offset | Register | Reset | Access | Owner | Definition |
|---:|---|---:|---|---|---|
| `700` | `VAMS_TELEM_SNAPSHOT` | `00000000` | WO | Host | Write 1 requests atomic snapshot; other values illegal; read zero. |
| `704` | `VAMS_FW_HEARTBEAT` | `00000000` | RO | FW | 32-bit monotonic, wrapping heartbeat, updated at least once/second. |
| `708` | `VAMS_FW_UPTIME_LO` | `00000000` | RO | FW | Snapshot uptime in ms `[31:0]`. |
| `70c` | `VAMS_FW_UPTIME_HI` | `00000000` | RO | FW | Uptime `[63:32]`. |
| `710/714` | `VAMS_COMMANDS_ACCEPTED_LO/HI` | `0/0` | RO | FW | Commands passing validation. |
| `718/71c` | `VAMS_COMMANDS_REJECTED_LO/HI` | `0/0` | RO | FW | Error completions produced by validation. |
| `720/724` | `VAMS_COMMANDS_COMPLETED_LO/HI` | `0/0` | RO | FW | Successful completions. |
| `728/72c` | `VAMS_COMMANDS_TIMED_OUT_LO/HI` | `0/0` | RO | FW | Commands entering timeout abort. |
| `730/734` | `VAMS_DMA_BYTES_LO/HI` | `0/0` | RO | HW/FW | Successfully transferred payload bytes; descriptor/CQ traffic excluded. |
| `738/73c` | `VAMS_DMA_ERRORS_LO/HI` | `0/0` | RO | HW/FW | Failed DMA transactions. |
| `740` | `VAMS_QUEUE_HIGH_WATER` | `00000000` | RO | FW | Maximum simultaneous accepted commands `[15:0]`; upper bits zero. |
| `744` | `VAMS_WDT_RESET_COUNT` | `00000000` | RO | HW | Saturating watchdog reset count. |
| `748` | `VAMS_TELEM_LAST_RESET` | `00000000` | RO | HW | Snapshot of LAST_RESET_REASON. |
| `74c` | `VAMS_MAX_LATENCY_US` | `00000000` | RO | FW | Saturating maximum submit-observed to completion-published latency in us. |
| `750` | `VAMS_LAST_ERROR_CODE` | `00000000` | RO | FW/HW | Most recent structured error; reset zero. |
| `754` | `VAMS_TELEM_STATUS` | `00000000` | RO | HW | Bit 0 SNAPSHOT_VALID, bit 1 COUNTER_SATURATED. |
| `758` | `VAMS_TELEM_FW_VERSION` | `00000000` | RO | FW | Firmware version captured with the snapshot, encoded identically to `VAMS_FW_VERSION`; Mgmt reset clears until republished. |

Mgmt reset clears firmware uptime and heartbeat but preserves command, DMA,
watchdog, high-water, last-reset, latency, and error counters. Device and Cold
reset clear all telemetry except `WDT_RESET_COUNT` and last-reset fields, which
Cold reset alone clears. This persistence is virtual device state, not durable
storage across QEMU destruction.

## Debug-only fault injection

This block exists only when the QEMU property `x-vams-debug=true`; otherwise it
reads all ones and writes are illegal MMIO. It is never a production capability.

| Offset | Register | Reset | Access | Owner | Definition / side effects |
|---:|---|---:|---|---|---|
| `f00` | `VAMS_FAULT_CONTROL` | `00000000` | W1S | Test host | One-shot bits: 0 next DMA timeout, 1 drop next CQ interrupt, 2 hang next engine command, 3 hang firmware service, 4 corrupt next mailbox, 5 reset on next active transfer. Mutually exclusive; multiple bits rejected. Bit auto-clears when triggered. |
| `f04` | `VAMS_FAULT_ARG` | `00000000` | RW | Test host | Fault-specific argument; write only when CONTROL zero. Zero selects deterministic default. |
| `f08` | `VAMS_FAULT_STATUS` | `00000000` | W1C | HW | Bits mirror fault types that have triggered. Multiple defined bits may be cleared; zero no-op. Device reset preserves, Cold reset clears. |
| `f0c` | `VAMS_FAULT_COUNT` | `00000000` | RO | HW | Saturating number of triggered injections; Cold reset clears. |
| `f10` | `VAMS_DEBUG_LOCK` | `00000000` | W1S | Host | Write 1 permanently disables new faults and INTR_FORCE until Cold reset; reads bit 0. Zero no-op. |

Arming while another fault is armed is BAD_CONFIG. Fault effects and recovery
are specified in `fault-recovery.md`; injection never bypasses descriptor range
checks or grants access outside the PCI DMA address space.
