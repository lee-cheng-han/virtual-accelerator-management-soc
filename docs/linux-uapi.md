# Linux host API

## Device and versioning

Every bound VAMS PCI function registers one misc character device named
`/dev/vamsN`. The public header is
[`kernel/include/uapi/linux/vams.h`](../kernel/include/uapi/linux/vams.h).
All structures use fixed-width Linux UAPI types and begin with `size` and
`version`; release 1 requires the exact structure size and
`VAMS_UAPI_VERSION == 1`. Unknown ioctl numbers return `ENOTTY`. Unknown flags,
reserved values, sizes, or versions return `EINVAL` before queue ownership
changes.

`VAMS_IOCTL_GET_INFO` returns the hardware-interface and firmware versions,
known capabilities, active queue depth, and observed reset generation. It does
not expose kernel pointers, DMA addresses, or implementation-private state.

## Synchronous NOP

`VAMS_IOCTL_NOP` accepts flags zero, a timeout from 0 through 60000 ms, and an
opaque 64-bit user cookie. The driver allocates a nonzero command ID, inserts a
tracking object before publishing the descriptor, and waits up to one second
for the reference NOP transport. On success it returns command ID, completion
status/error, bytes processed, result CRC, and device timestamp. The cookie is
copied through the device unchanged.

Multiple threads and processes may issue NOP concurrently. SQ publication is
mutex-serialized while CQ lookup uses an IRQ-safe XArray and completion lock.
If the ring is momentarily full, the ioctl returns `EAGAIN`. A signal returns
the normal restart error and a host wait timeout returns `ETIMEDOUT`; neither
case frees the device-owned tracking object early. PCI removal completes any
remaining waiter with `ENODEV`.

MSI-X is an optimization, not the only completion path. A delayed worker checks
CQ every 10 ms while tracked requests exist. The test-only module masks the CQ
interrupt and requires this worker to complete a real NOP before restoring the
mask.

## Validation

`kernel/tests/vams-uapi-test.c` is compiled as a static guest executable. It
checks device information, rejects an unsupported UAPI version, then submits 32
NOPs concurrently from four independent file descriptors. Every request must
return success, a unique nonzero ID, zero processed bytes, and its exact cookie.

```sh
make kernel-uapi-test
make kernel-smoke \
  KERNEL_BUILD=/path/to/linux/build \
  VAMS_LINUX_IMAGE=/path/to/bzImage \
  BUSYBOX=/path/to/static/busybox \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
```

The current API is intentionally narrow. Payload-buffer mapping, asynchronous
submission/reaping, reset ioctls, telemetry snapshots, and a stable `libvams`
wrapper will be added only with their corresponding ownership and cancellation
tests.
