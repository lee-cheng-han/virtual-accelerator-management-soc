# Linux host API

## Device and versioning

Every bound VAMS PCI function registers one misc character device named
`/dev/vamsN`. The public contract is
[`kernel/include/uapi/linux/vams.h`](../kernel/include/uapi/linux/vams.h).
All structures use fixed-width Linux UAPI types, contain no native pointers,
and begin with `size` and `version`. Version 1 requires the exact structure size
and `VAMS_UAPI_VERSION == 1`, so the same layouts are used by native and compat
processes. Unknown ioctl numbers return `ENOTTY`; unknown flags, nonzero
reserved fields, sizes, or versions return `EINVAL` before ownership changes.

`VAMS_IOCTL_GET_INFO` reports the hardware-interface and firmware versions,
known capabilities, active queue depth, and observed reset generation. It does
not expose kernel pointers, DMA addresses, or implementation-private state.

## Registered buffers

`VAMS_IOCTL_BUFFER_REGISTER` converts a userspace range of 1 byte through
16 MiB into a per-file opaque handle. `VAMS_BUFFER_READ` permits the device to
read the range and `VAMS_BUFFER_WRITE` permits it to write the range; both may
be requested. Registration validates the entire range, long-term pins its
pages, creates a streaming DMA mapping, and currently requires the resulting
mapping to be one contiguous DMA segment. No DMA address crosses the UAPI.

Handles belong to the file description that created them. A handle from a
separate open returns `ENOENT`, including when the numeric value happens to be
the same. Unregister returns `EBUSY` while a request holds the mapping and
`ENOENT` for an absent or already removed handle. Writable pages are dirtied
when their final reference is released.

## Asynchronous commands

`VAMS_IOCTL_SUBMIT` publishes one command and returns a nonzero command ID.
NOP uses no handles. `MEM_COPY` reads `length` bytes from source and writes the
destination. `MEM_FILL` reads one byte at the source offset and fills `length`
destination bytes. `CRC32` reads the source and optionally checks
`expected_crc`. `VECTOR_ADD` adds little-endian 32-bit source elements into a
read/write destination range. The driver rejects zero or excessive lengths,
misaligned vector lengths, overflowing handle ranges, permission mismatches,
and overlapping copy/vector DMA ranges before queue publication.

Multiple requests may be outstanding on one or many open files. SQ publication
is mutex-serialized; global command IDs and per-file request tables prevent one
process from waiting on another process's work. `EAGAIN` means the SQ is full.
Each request retains its mappings and file context until the device completes
or the driver gives it a terminal cancellation result.

`VAMS_IOCTL_WAIT` names one command ID. A zero timeout is nonblocking and
returns `EAGAIN` until terminal; a nonzero timeout waits for at most 60 seconds.
Signal interruption and host wait timeout do not reap the request. A successful
wait copies the device result, cookie, timestamp, and `driver_status`, then
reaps that command exactly once. Reset cancellation is reported as
`driver_status == -ECANCELED`; removal uses `-ENODEV`. A second waiter racing
the successful reap receives `EALREADY` or `ENOENT`.

`poll`/`epoll` reports readable when at least one owned request is terminal,
writable when an SQ entry appears available, and error/hangup during teardown.
The application still selects a command ID with `VAMS_IOCTL_WAIT`; readiness
does not reorder or implicitly reap completions.

Closing a file removes its userspace-visible handles and requests immediately.
In-flight requests retain their pinned mappings privately until terminal
completion, so close or process exit cannot release pages still owned by DMA.
Reset-generation handling is ordered against SQ doorbells and cancels only
requests published against an older generation.

## Synchronous compatibility command

`VAMS_IOCTL_NOP` remains as the simple blocking smoke and compatibility path.
It accepts a timeout field from 0 through 60000 ms and an opaque cookie, then
waits up to one second for transport completion. The result contains the
generated command ID, status/error, bytes processed, CRC, timestamp, and exact
cookie. Interrupted or timed-out callers release only their host reference;
the tracked request remains valid until completion or teardown.

MSI-X is an optimization rather than the sole completion path. A delayed
worker drains CQ every 10 ms while requests remain, so a lost or masked CQ
interrupt increases latency but does not strand ownership.

## Validation

`kernel/tests/vams-uapi-test.c` is a static guest executable. It validates
structure/version rejection, cross-file handle isolation, permissions and busy
unregister, every v1 payload opcode with guarded data, readiness polling, eight
outstanding asynchronous NOPs, 32 concurrent synchronous NOPs across four file
descriptors, close during an active DMA copy, and device reset with eight
submitted copies. The reset test requires at least one cancellation, a changed
generation, terminal reaping, successful unmap, and disabled stale queues. The
guest then unloads and rebinds the module to exercise cleanup.

```sh
make kernel-smoke \
  KERNEL_BUILD=/path/to/linux/build \
  VAMS_LINUX_IMAGE=/path/to/matching/bzImage \
  GEN_INIT_CPIO=/path/to/linux/build/usr/gen_init_cpio \
  QEMU_SYSTEM_X86_64=/path/to/qemu-system-x86_64
```

The test constructs a minimal initramfs from project-owned static executables;
BusyBox and a guest disk image are not required. Cancellation/reset-control
ioctls, multi-segment DMA mappings, a stable `libvams`, and hot-remove stress
remain follow-up work.
