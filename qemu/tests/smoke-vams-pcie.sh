#!/bin/sh

set -eu

qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
output=$(mktemp)
trap 'rm -f "$output"' EXIT HUP INT TERM

test -x "$qemu" || command -v "$qemu" >/dev/null 2>&1 || {
    echo "QEMU executable not found: $qemu" >&2
    exit 2
}

set +e
printf '%s\n' \
    'outl 0xcf8 0x80001000' \
    'inl 0xcfc' \
    'outl 0xcf8 0x80001008' \
    'inl 0xcfc' \
    'outl 0xcf8 0x80001010' \
    'outl 0xcfc 0xffffffff' \
    'inl 0xcfc' \
    'outl 0xcfc 0xfebf0000' \
    'outl 0xcf8 0x80001014' \
    'outl 0xcfc 0xffffffff' \
    'inl 0xcfc' \
    'outl 0xcfc 0xfebe0000' \
    'outl 0xcf8 0x80001004' \
    'outl 0xcfc 0x2' \
    'outl 0xcf8 0x80001034' \
    'inl 0xcfc' \
    'outl 0xcf8 0x80001040' \
    'inl 0xcfc' \
    'outl 0xcf8 0x80001044' \
    'inl 0xcfc' \
    'outl 0xcf8 0x80001048' \
    'inl 0xcfc' \
    'readl 0xfebf0000' \
    'readl 0xfebf0004' \
    'readl 0xfebf000c' \
    'readl 0xfebf0010' \
    'readl 0xfebf0014' \
    'readl 0xfebf0018' \
    'readl 0xfebf001c' \
    'readl 0xfebf0304' \
    'readl 0xfebf030c' \
    'readq 0xfebe0800' \
    'writel 0xfebf0304 0xd' \
    'writel 0xfebf0308 0x2' \
    'readl 0xfebf0300' \
    'readq 0xfebe0800' \
    'writel 0xfebf0304 0xc' \
    'writel 0xfebf0308 0x1' \
    'readl 0xfebf0300' \
    'readq 0xfebe0800' \
    'writel 0xfebf0300 0xf' \
    'readl 0xfebf0300' \
    'readl 0xfebf0024' \
    'readb 0xfebf0000' \
    'readl 0xfebf0024' \
    'writel 0xfebf0024 0x1' \
    'writel 0xfebf0000 0x0' \
    'readl 0xfebf0024' \
    'writel 0xfebf0024 0x4' \
    'writel 0xfebf0304 0x10' \
    'readl 0xfebf0024' \
    'readl 0xfebf0100' \
    'readl 0xfebf0024' \
    'writel 0xfebf0024 0x3ff' \
    'writel 0xfebf0300 0xf' \
    'writel 0xfebf0020 0x1' \
    'readl 0xfebf0024' \
    'readl 0xfebf0020' \
    'writel 0xfebf0024 0x8' \
    'writel 0xfebf0300 0xf' \
    'writel 0xfebf0020 0x2' \
    'readl 0xfebf001c' \
    'readl 0xfebf0028' \
    'readl 0xfebf0304' \
    'readq 0xfebe0800' \
    'clock_step 1000000' \
    'readl 0xfebf001c' \
    'readl 0xfebf0028' \
    'readl 0xfebf0300' | \
    timeout 2 "$qemu" \
        -machine q35,accel=qtest \
        -display none \
        -nodefaults \
        -device vams-pcie,addr=2 \
        -qtest stdio >"$output" 2>&1
status=$?
set -e

if [ "$status" -ne 124 ]; then
    cat "$output" >&2
    echo "unexpected qtest exit status: $status" >&2
    exit 1
fi

expected=$(printf '%s\n' \
    'OK 0x11001b36' \
    'OK 0x12000000' \
    'OK 0xfffff000' \
    'OK 0xfffff000' \
    'OK 0x0040' \
    'OK 0x18011' \
    'OK 0x0001' \
    'OK 0x0801' \
    'OK 0x0000000011001b36' \
    'OK 0x0000000000010000' \
    'OK 0x0000000000000001' \
    'OK 0x0000000000000002' \
    'OK 0x0000000001000000' \
    'OK 0x0000000004000010' \
    'OK 0x0000000000000001' \
    'OK 0x000000000000000f' \
    'OK 0x0000000000000001' \
    'OK 0x0000000000000000' \
    'OK 0x0000000000000002' \
    'OK 0x0000000000000002' \
    'OK 0x0000000000000003' \
    'OK 0x0000000000000003' \
    'OK 0x0000000000000000' \
    'OK 0x0000000000000000' \
    'OK 0x00000000000000ff' \
    'OK 0x0000000000000001' \
    'OK 0x0000000000000004' \
    'OK 0x0000000000000002' \
    'OK 0x00000000ffffffff' \
    'OK 0x0000000000000003' \
    'OK 0x0000000000000008' \
    'OK 0x0000000000000000' \
    'OK 0x0000000000000010' \
    'OK 0x0000000000000001' \
    'OK 0x000000000000000f' \
    'OK 0x0000000000000000' \
    'OK 0x0000000000000001' \
    'OK 0x0000000000000001' \
    'OK 0x0000000000000008')
actual=$(grep '^OK 0x' "$output")
if [ "$actual" != "$expected" ]; then
    cat "$output" >&2
    echo 'PCIe configuration, BAR, MSI-X, or reset sequence did not match' >&2
    exit 1
fi

if grep -Fq 'FAIL ' "$output"; then
    cat "$output" >&2
    echo 'qtest command failed' >&2
    exit 1
fi

echo 'VAMS PCIe enumeration, BAR, MSI-X, and reset smoke test: PASS'
