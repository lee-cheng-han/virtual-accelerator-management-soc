# Linux PCI Driver

## Scope

`vams_pci.ko` is the deliberately thin Linux host driver for PCI vendor/device
`1b36:1100`. When DMA is advertised, it allocates and configures one coherent
SQ/CQ pair at depth 16. It does not expose a character device or public command
API yet; production builds own the transport but do not originate commands.

Probe performs the following ordered checks and acquisitions:

1. allocate driver state and enable the PCI memory function;
2. verify BAR0 is a memory resource of at least 4 KiB, reserve it, and map it;
3. mask and clear stale VAMS interrupt sources;
4. verify BAR identity, hardware-interface major 1, descriptor version 1,
   MSI-X capability, and READY without RESETTING or FATAL;
5. negotiate a coherent 64-bit DMA mask with a 32-bit fallback;
6. allocate zeroed coherent SQ/CQ rings when DMA is advertised;
7. allocate exactly two MSI-X vectors and install the CQ and async handlers;
8. enable PCI bus mastering, program CQ then SQ, enable the device, and unmask
   VAMS interrupt sources.

Every error path unwinds acquired resources in reverse order. Remove first
masks the device sources and flushes the posted write, then disables bus
mastering, frees IRQs and vectors, unmaps/releases BAR0, disables the PCI
function, and frees private state.

Vector 0 handles the sticky CQ source. It reads CQ tail, executes `dma_rmb()`,
copies completions, publishes CQ head, and only then acknowledges its assigned
W1C source. Vector 1 handles ERROR, FW_EVENT, and RESET_DONE. Each handler
returns `IRQ_NONE` when its source is absent; the async handler snapshots device
error state and updates the observed reset generation.

## Build

Build against an installed or prepared kernel tree:

```sh
make kernel KERNEL_BUILD=/path/to/linux/build
```

The build uses the kernel `W=1` warning set. The module metadata and modalias
allow normal PCI autoloading for `1b36:1100`.

## Disposable-guest validation

The test-only build enables deterministic probe failures after PCI enable, BAR
reservation, BAR mapping, DMA-mask negotiation, coherent-ring allocation,
vector allocation, and each IRQ registration. It also enables opt-in probe
self-tests that force both MSI-X paths and submit one generated-ABI NOP through
the coherent rings. These controls are excluded from the production module.

```sh
make kernel-test-build \
  KERNEL_BUILD=/path/to/linux/build

make kernel-smoke \
  KERNEL_BUILD=/path/to/linux/build \
  VAMS_LINUX_IMAGE=/path/to/matching/bzImage \
  BUSYBOX=/path/to/static/busybox \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
```

The smoke test creates a temporary initramfs and does not need a disk image. In
the guest it verifies PCI identity, all eight injected cleanup paths, successful
rebinding after failures, both MSI-X vectors, a NOP ID/cookie round trip, normal
remove, and a second clean probe/remove cycle. A test kernel and static BusyBox
are external pinned dependencies; they are never committed as generated
artifacts.

## Known limitations

- No public submission API, payload DMA, reset orchestration, polling worker, or
  power-management interface exists yet.
- Firmware version zero is accepted because the PCIe shell is not connected to
  the RISC-V subsystem. READY and the hardware ABI remain mandatory.
- The driver has been designed for one or more independent VAMS functions, but
  multi-device concurrency is not stressed until command queues exist.
