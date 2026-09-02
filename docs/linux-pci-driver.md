# Linux PCI driver

## Scope

`vams_pci.ko` is the thin Linux transport for PCI vendor/device `1b36:1100`.
When DMA is advertised it configures one coherent depth-16 SQ/CQ pair,
registers `/dev/vamsN`, and exposes versioned information, buffer registration,
asynchronous NOP/copy/fill/CRC32/vector operations, wait, and `poll` interfaces.
Policy, descriptor validation, scheduling, and payload execution remain owned
by firmware and the endpoint.

Probe acquires and validates resources in this order:

1. enable the PCI memory function and validate/map the 4 KiB BAR0;
2. mask stale sources and validate identity, ABI versions, capabilities, and
   READY state;
3. negotiate a coherent 64-bit DMA mask with a 32-bit fallback;
4. allocate aligned coherent SQ/CQ rings;
5. allocate exactly two MSI-X vectors and register CQ and asynchronous handlers;
6. enable bus mastering, configure queues and CQ watermarks, then enable IRQs;
7. register the reference-counted misc character device.

Every probe failure unwinds the acquired prefix in reverse order. Remove first
rejects new operations and removes the device node, stops CQ/reset work, masks
and frees IRQs, terminates tracked requests with `-ENODEV`, then releases
vectors, rings, BAR0, and the PCI function.

## Queue and completion ownership

SQ publication is serialized by `submit_lock`. The driver inserts a
reference-counted request into both the device command-ID table and its owning
file table before applying `dma_wmb()` and ringing the SQ doorbell. Vector 0
drains CQ after `dma_rmb()`, removes the global request exactly once, synchronizes
streaming mappings for the CPU, wakes waiters, publishes CQ head, and then
acknowledges its W1C source. A delayed worker performs the same drain every
10 ms while requests exist, providing lost-interrupt fallback.

Vector 1 handles ERROR, FW_EVENT, and RESET_DONE. Reset generation updates are
ordered against SQ doorbell publication with a spinlock. Serialized reset work
cancels only requests tagged with an older generation, avoiding both stale DMA
ownership and accidental cancellation of requests submitted after reset.

Each open file owns two XArrays: opaque mappings and reapable requests.
Registered buffers are long-term pinned and streaming-DMA mapped with explicit
device read/write permissions. Requests take mapping references before
publication. Unregister therefore cannot unpin an active mapping, and file
release may hide all objects immediately while device-owned references keep
pages and driver state alive through terminal completion. Open files and
requests hold `kref` references through PCI removal to prevent state
use-after-free.

The UAPI uses only fixed-width fields and routes compat ioctls through the same
layout. Its exact validation and result contract is in
[the Linux UAPI guide](linux-uapi.md).

## Build

Build against a configured kernel tree with the kernel `W=1` warning set:

```sh
make kernel KERNEL_BUILD=/path/to/linux/build
```

The module metadata and PCI alias support normal autoloading for `1b36:1100`.

## Disposable-guest validation

The test-only module can fail probe after each of nine acquisitions and can
force both MSI-X vectors, a generated-ABI NOP, and CQ polling while the CQ
interrupt is masked. Those controls are excluded from production builds.

```sh
make kernel-smoke \
  KERNEL_BUILD=/path/to/linux/build \
  VAMS_LINUX_IMAGE=/path/to/matching/bzImage \
  GEN_INIT_CPIO=/path/to/linux/build/usr/gen_init_cpio \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
```

The script creates a temporary initramfs containing a project-owned static init
and UAPI test; it requires neither BusyBox nor a disk image. In one boot it
checks all probe unwind points and clean rebind, both IRQs, interrupt polling,
all payload opcodes, mapping isolation and lifetime, asynchronous readiness and
reaping, concurrent synchronous requests, close during DMA, active device-reset
cancellation, normal unload, and a second clean probe/remove cycle.

## Known limitations

- Registered ranges must DMA-map as one contiguous segment; scatter/gather
  descriptor chaining is not implemented.
- Public cancel and reset-control ioctls, hot-remove-during-DMA stress,
  debugfs/tracepoints, and a stable userspace library remain planned.
- Firmware version zero is accepted because the standalone PCIe shell is not
  connected to the RISC-V subsystem in this single-QEMU guest test.
- Independent functions receive distinct device nodes, but multi-device and
  memory-pressure qualification are not yet part of the guest suite.
