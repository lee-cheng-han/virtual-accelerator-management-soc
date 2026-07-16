# NOP command path

## Implemented scope

The current `vams-pcie` model implements one host-memory submission queue and
one completion queue with depth 16–1024. The Linux driver chooses depth 16,
allocates both rings with `dma_alloc_coherent()`, programs their 64-bit DMA
addresses, enables CQ before SQ, enables the device, and drains completions from
MSI-X vector 0. Capability bits DMA, MSI-X, and polling-safe CQ are advertised.

The PCI path remains a transport/reference implementation: QEMU captures and
validates its NOP descriptors directly because the standalone `vams_riscv`
firmware harness has not yet been embedded into the PCI function. Separately,
the private management command portal now stages the same generated descriptor
for Zephyr, which owns the normative validation order and completion policy.
The final requirement is not satisfied until PCI DMA and queue events use that
firmware path. No payload opcode is implemented or advertised as working.

## Generated ABI

[`abi/vams-v1.json`](../abi/vams-v1.json) is authoritative for descriptor
version, opcodes, status/error values, field order, and structure sizes.
`scripts/gen-vams-abi.py` deterministically produces separate portable,
firmware, Linux, and QEMU headers. `make abi-check` fails if any generated output is stale,
compiles exact size/offset assertions with GCC and Clang, and checks fixed raw
little-endian submission/completion byte strings.

## Queue ordering

The host zeroes and fills one 64-byte submission entry, executes `dma_wmb()`,
then writes `SQ_DOORBELL`. QEMU's `pci_dma_read()` executes the DMA barrier
before capturing all 64 bytes and advances SQ head only after a successful
read. It validates the private copy without touching payload memory.

QEMU builds a private 32-byte completion, calls `pci_dma_write()`, and advances
CQ tail only after that write returns `MEMTX_OK`. It then sets sticky CQ status
and notifies MSI-X vector 0 when unmasked. The driver observes CQ tail, executes
`dma_rmb()`, copies each completion, publishes CQ head, and only then W1C-clears
the interrupt source. Clearing CQ interrupt status while entries remain causes
the model to reassert it.

Both rings reserve one empty entry. Doorbells are checked as forward modulo
advances and cannot pass the opposite owner index. CQ full stops SQ processing
without overwriting a completion; a legal CQ-head doorbell resumes processing.
Descriptor-read or completion-write failure marks the relevant queue and sets
DMA plus queue error status.

`make queue-model-smoke` runs an implementation-independent SQ/CQ state model
against the live endpoint. Four fixed seeds generate 1,200 operations covering
valid and invalid NOPs, wraparound, CQ backpressure, deferred SQ processing,
queue disable/re-enable, quiesce, illegal doorbells, sticky interrupt
reassertion, error clearing, and paired queue reset. Every observable queue
register and every outstanding completion is compared after each operation.
Failures report the seed and shortest observed failing operation prefix; custom
seeds and operation counts can be replayed directly with the Python runner.

## NOP behavior

NOP requires descriptor version 1, opcode zero, flags/reserved/CRC fields zero,
a legal timeout, zero length, and zero source/destination addresses. The first
failed rule produces exactly one INVALID completion with the captured command
ID and cookie. A valid NOP produces SUCCESS/NONE with zero bytes and CRC.

The Linux test-only `probe_nop_selftest=1` path submits command ID `0x56414d53`,
waits at most one second for the real CQ interrupt, and checks ID, cookie,
status, error, and bytes processed. It is compiled out of the production
module. The production module configures and owns the queues but exposes no
payload interface. Its versioned `/dev/vamsN` API tracks synchronous NOP
requests; reset control and payload-buffer ownership remain deferred.

## Reset and limitations

Queue reset disables both rings and device processing, clears indices and queue
errors, drops pending CQ interrupt state, preserves configured bases/depths,
and increments reset generation. Device reset also clears queue configuration.
Migration state includes all queue configuration, indices, and errors, but
end-to-end migration with coherent guest memory remains unsupported.

The private portal uses explicit host-submit, firmware-ack, firmware-complete,
and host-ack ownership transitions. Its QTest covers overwrite rejection,
sticky overflow/protocol errors, counters, and exact completion bytes; a Zephyr
test covers valid and unsupported-version NOPs. The Linux driver now adds
tracked concurrent NOPs, CQ polling fallback, and a versioned host API. The next
work is to connect PCI queue events and DMA service to the firmware portal. The
reference model now covers ownership, backpressure, interrupt, and reset
sequences, but the two sides remain tested foundations rather than one
firmware-owned PCI command plane.
