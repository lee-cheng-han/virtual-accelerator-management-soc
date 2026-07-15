#!/bin/sh

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
QEMU_SYSTEM_X86_64=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
VAMS_LINUX_IMAGE=${VAMS_LINUX_IMAGE:-}
VAMS_PCI_MODULE=${VAMS_PCI_MODULE:-$ROOT_DIR/kernel/vams_pci.ko}
BUSYBOX=${BUSYBOX:-busybox}

if [ -z "$VAMS_LINUX_IMAGE" ] || [ ! -f "$VAMS_LINUX_IMAGE" ]; then
	echo 'set VAMS_LINUX_IMAGE to a matching x86_64 Linux kernel image' >&2
	exit 2
fi
if [ ! -f "$VAMS_PCI_MODULE" ]; then
	echo "VAMS module not found: $VAMS_PCI_MODULE" >&2
	exit 2
fi
if ! command -v "$QEMU_SYSTEM_X86_64" >/dev/null 2>&1; then
	echo "QEMU binary not found: $QEMU_SYSTEM_X86_64" >&2
	exit 2
fi
if ! command -v "$BUSYBOX" >/dev/null 2>&1; then
	echo "static BusyBox not found: $BUSYBOX" >&2
	exit 2
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/root/bin" "$tmp/root/dev" "$tmp/root/proc" "$tmp/root/sys"
cp "$BUSYBOX" "$tmp/root/bin/busybox"
cp "$VAMS_PCI_MODULE" "$tmp/root/vams_pci.ko"
cp "$ROOT_DIR/kernel/tests/guest-init.sh" "$tmp/root/init"
chmod 0755 "$tmp/root/init"

(
	cd "$tmp/root"
	find . -print | "$BUSYBOX" cpio -o -H newc
) >"$tmp/initramfs.cpio" 2>"$tmp/cpio.log"
"$BUSYBOX" gzip -9 "$tmp/initramfs.cpio"

set +e
timeout 45 "$QEMU_SYSTEM_X86_64" \
	-machine q35,accel=tcg \
	-cpu max \
	-m 256M \
	-nodefaults \
	-no-reboot \
	-nographic \
	-monitor none \
	-serial stdio \
	-kernel "$VAMS_LINUX_IMAGE" \
	-initrd "$tmp/initramfs.cpio.gz" \
	-append 'console=ttyS0 panic=-1 rdinit=/init' \
	-device vams-pcie,addr=2 \
	-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	>"$tmp/console.log" 2>&1
status=$?
set -e

if [ "$status" -ne 1 ] && [ "$status" -ne 0 ]; then
	cat "$tmp/console.log"
	echo "QEMU guest exited unexpectedly with status $status" >&2
	exit 1
fi
if ! grep -q 'VAMS Linux PCI queue, NOP, MSI-X, and cleanup smoke test: PASS' \
	"$tmp/console.log"; then
	cat "$tmp/console.log"
	echo 'guest did not report a passing VAMS PCI driver test' >&2
	exit 1
fi

grep -E 'VAMS test device:|injecting probe failure|MSI-X self-test passed|NOP round trip passed|ready:|PASS' \
	"$tmp/console.log"
echo 'VAMS Linux PCI guest integration test: PASS'
