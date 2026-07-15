# Linux PCI Driver

## Scope

`vams_pci.ko` is the deliberately thin Linux host driver for PCI vendor/device
`1b36:1100`. The current endpoint advertises MSI-X but not DMA, so the driver
binds in discovery mode. It does not allocate rings, expose a character device,
or claim that commands work. Queue transport is added only when the QEMU model
implements and advertises the DMA capability.

Probe performs the following ordered checks and acquisitions:

1. allocate driver state and enable the PCI memory function;
2. verify BAR0 is a memory resource of at least 4 KiB, reserve it, and map it;
3. mask and clear stale VAMS interrupt sources;
4. verify BAR identity, hardware-interface major 1, descriptor version 1,
   MSI-X capability, and READY without RESETTING or FATAL;
5. negotiate a coherent 64-bit DMA mask with a 32-bit fallback;
6. allocate exactly two MSI-X vectors and install the CQ and async handlers;
7. enable PCI bus mastering for MSI-X memory writes, then enable VAMS interrupt
   sources. Payload DMA remains prohibited until its capability is advertised.

Every error path unwinds acquired resources in reverse order. Remove first
masks the device sources and flushes the posted write, then disables bus
mastering, frees IRQs and vectors, unmaps/releases BAR0, disables the PCI
function, and frees private state.

Vector 0 handles the sticky CQ source. Vector 1 handles ERROR, FW_EVENT, and
RESET_DONE. Each handler returns `IRQ_NONE` when its assigned source is absent
and acknowledges only its assigned W1C bits. The async handler snapshots device
error state and updates the observed reset generation. CQ draining will be
inserted before CQ acknowledgment when queue support is implemented.

## Build

Build against an installed or prepared kernel tree:

```sh
make kernel KERNEL_BUILD=/path/to/linux/build
```

The build uses the kernel `W=1` warning set. The module metadata and modalias
allow normal PCI autoloading for `1b36:1100`.

## Disposable-guest validation

The test-only build enables deterministic probe failures after PCI enable, BAR
reservation, BAR mapping, DMA-mask negotiation, vector allocation, and each IRQ
registration. It also enables an opt-in probe self-test that forces and waits
for both MSI-X paths. These controls are excluded from the production module.

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
the guest it verifies PCI identity, all seven injected cleanup paths, successful
rebinding after failures, both MSI-X vectors, normal remove, and a second clean
probe/remove cycle. A test kernel and static BusyBox are external pinned test
dependencies; they are never committed as generated artifacts.

## Known limitations

- No queue, DMA payload, userspace, reset orchestration, or power-management
  interface exists yet.
- Firmware version zero is accepted because the PCIe shell is not connected to
  the RISC-V subsystem. READY and the hardware ABI remain mandatory.
- The driver has been designed for one or more independent VAMS functions, but
  multi-device concurrency is not stressed until command queues exist.
