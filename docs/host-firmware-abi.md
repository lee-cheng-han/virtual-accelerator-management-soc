# Host–firmware ABI

The ABI consists of BAR0, one host-produced submission ring, one
firmware-produced completion ring, MSI-X notifications, and the descriptor
formats. The management mailbox is for lifecycle events only.

## Initialization handshake

1. The driver enables the PCI function, negotiates a 64-bit DMA mask (falling
   back to 32-bit only if all allocations/mappings comply), requests MSI-X, and
   leaves device ENABLE clear.
2. It verifies HW interface major 1, descriptor version support, capabilities,
   and READY/FW_RUNNING. A major mismatch fails probe; a newer minor is accepted
   only using known capability bits.
3. It allocates zeroed coherent SQ/CQ memory at legal depth/alignment, programs
   base low then high and depth while disabled, and initializes both indices to
   zero by paired queue reset.
4. It enables CQ, then SQ, clears stale interrupt status, configures/unmasks
   interrupts, and sets DEVICE ENABLE.
5. It rereads RESET_GENERATION. Any change restarts initialization.

Ring depth is a power of two 16–1024. Indices are offsets, not monotonically
growing counters. Empty is `head == tail`; full is `next(tail) == head`, so one
entry is deliberately unused. A doorbell may move only forward over entries the
producer owns and must never leap over the consumer.

## Submission protocol

The driver owns a free SQ entry. It maps/pins all payload buffers for the entire
device-ownership interval, fills and endian-converts the entry, then executes a
DMA write/release barrier. It computes the next tail and writes SQ_DOORBELL.
Once published, neither the descriptor nor payload needed as device input may
be modified until completion/reset. Firmware/hardware acquires before fetching.

Firmware copies the descriptor into SRAM. Only after a successful full fetch
may it advance SQ head, allowing host reuse. This does not release payload
buffers: command completion or reset does. Firmware validates and either queues
the command or prepares an error completion. If internal resources or CQ
capacity reach their bounded threshold, it stops consuming SQ entries. The host
observes full and returns `-EAGAIN` for nonblocking submission or sleeps on a
driver wait queue for blocking submission.

## Completion protocol

Firmware waits for CQ space, fills a private completion, and requests one 32-byte
DMA write. Hardware finishes that write before updating CQ tail and setting
interrupt status. The driver reads tail, performs a DMA read/acquire barrier,
and drains all entries through that tail. It matches command ID to driver-owned
state, preserves the returned cookie/result, unmaps payloads, publishes the new
head with CQ_DOORBELL, and wakes submitters/waiters.

An unknown or duplicate completion ID is a queue-corruption event: stop new
submissions and request queue/device recovery. Userspace cannot choose kernel
pointer cookies; the UAPI cookie is opaque user data copied through a separate
driver tracking object.

MSI-X is a hint. The driver also examines CQ tail during submit/wait operations
and runs a low-rate completion poll while requests are outstanding. Thus the
dropped-interrupt fault increases latency but cannot strand work. Interrupt
status is cleared only after draining; if work races with the clear, the device
reasserts it.

## Concurrency and process lifetime

The driver serializes queue publication with a submit lock and CQ consumption
with a completion lock; reset takes a lifecycle mutex outside both. Lock order
is lifecycle → submit → completion and no wait occurs while holding a spinlock.
Multiple processes may submit. Driver-generated IDs are unique across all open
files. Per-file close marks its requests abandoned but does not reuse/unmap
their DMA memory until completion or successful reset cancellation; completions
are consumed and discarded for an exited process.

Module removal first rejects submissions, masks interrupts, quiesces, waits a
bounded interval, resets if needed, cancels requests, frees IRQs, then frees
rings and PCI resources. The management firmware never depends on a userspace
process remaining alive.

## Failure and reset contract

- Descriptor validation produces an error completion and advances normally.
- SQ full causes host backpressure; CQ full stalls device consumption and sets
  health/status, never overwrites entries.
- Command timeout begins at firmware acceptance. Firmware aborts the engine;
  successful abort produces TIMED_OUT. Failed abort escalates to engine reset.
- DMA failure produces FAILED when CQ is usable and may escalate by scope.
- A host reset rejects new submissions, masks interrupts, and records the old
  generation. When reset completes, the driver locally completes every request
  from that generation with reset error and rebuilds queues.
- Firmware restart invalidates every accepted command. It never reconstructs
  work from host rings because head may already have advanced.

The driver must not automatically replay commands: copy/fill/vector operations
are not universally idempotent from the caller's perspective. Userspace may
retry after receiving an explicit reset result and reestablishing buffers.

## Compatibility

The HW interface version governs BAR0; the descriptor version governs ring
entries; the Linux UAPI will have its own structure-size/version fields. Hosts
ignore unknown capability bits and reserved RO register bits, but never write
unknown bits. Major interface mismatch fails safely. Minor additions must keep
existing offsets and semantics. Queue memory is not migration-stable in release
1; QEMU migration is permitted only while the function is quiesced with no
accepted commands, otherwise migration must fail visibly.

