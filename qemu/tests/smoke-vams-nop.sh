#!/bin/sh

set -eu

qemu=${QEMU_SYSTEM_X86_64:-qemu-system-x86_64}
output=$(mktemp)
trap 'rm -f "$output"' EXIT HUP INT TERM

test -x "$qemu" || command -v "$qemu" >/dev/null 2>&1 || {
    echo "QEMU executable not found: $qemu" >&2
    exit 2
}

# Raw little-endian v1 descriptors. The second intentionally uses version 2.
valid_nop=01000000443322110000000000000000000000000000000000000000000000008877665544332211000000000000000000000000000000000000000000000000
invalid_nop=02000000ddccbbaa000000000000000000000000000000000000000000000000efcdab8967452301000000000000000000000000000000000000000000000000

set +e
{
    printf '%s\n' \
        'outl 0xcf8 0x80001010' \
        'outl 0xcfc 0xfebf0000' \
        'outl 0xcf8 0x80001014' \
        'outl 0xcfc 0xfebe0000' \
        'outl 0xcf8 0x80001004' \
        'outl 0xcfc 0x6'
    printf 'write 0x100000 64 0x%s\n' "$valid_nop"
    printf 'write 0x100040 64 0x%s\n' "$invalid_nop"
    printf '%s\n' \
        'writel 0xfebf0100 0x100000' \
        'writel 0xfebf0104 0' \
        'writel 0xfebf0108 16' \
        'writel 0xfebf0200 0x110000' \
        'writel 0xfebf0204 0' \
        'writel 0xfebf0208 16' \
        'writel 0xfebf0218 1' \
        'writel 0xfebf0118 1' \
        'readl 0xfebf0010' \
        'readl 0xfebf001c' \
        'writel 0xfebf0020 1' \
        'writel 0xfebf0304 0xe' \
        'writel 0xfebf0114 1' \
        'readl 0xfebf010c' \
        'readl 0xfebf0210' \
        'readl 0xfebf0300' \
        'read 0x110000 32' \
        'writel 0xfebf0214 1' \
        'writel 0xfebf0300 1' \
        'readl 0xfebf0300' \
        'readl 0xfebf021c' \
        'writel 0xfebf0114 2' \
        'readl 0xfebf010c' \
        'readl 0xfebf0210' \
        'readl 0xfebf0300' \
        'read 0x110020 32' \
        'writel 0xfebf0214 2' \
        'writel 0xfebf0300 1' \
        'writel 0xfebf0118 2' \
        'readl 0xfebf0028' \
        'readl 0xfebf0118' \
        'readl 0xfebf0218' \
        'readl 0xfebf001c' \
        'writel 0xfebf0108 15' \
        'writel 0xfebf0114 1' \
        'readl 0xfebf0024'
} | timeout 2 "$qemu" \
    -machine q35,accel=qtest \
    -m 64M \
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
    'OK 0x0000000000000023' \
    'OK 0x0000000000000005' \
    'OK 0x0000000000000001' \
    'OK 0x0000000000000001' \
    'OK 0x0000000000000001' \
    'OK 0x4433221100000000000000000000000088776655443322110000000000000000' \
    'OK 0x0000000000000000' \
    'OK 0x0000000000000003' \
    'OK 0x0000000000000002' \
    'OK 0x0000000000000002' \
    'OK 0x0000000000000001' \
    'OK 0xddccbbaa010001000000000000000000efcdab89674523010000000000000000' \
    'OK 0x0000000000000001' \
    'OK 0x0000000000000000' \
    'OK 0x0000000000000000' \
    'OK 0x0000000000000001' \
    'OK 0x0000000000000028')
actual=$(grep '^OK 0x' "$output")
if [ "$actual" != "$expected" ]; then
    cat "$output" >&2
    echo 'NOP queue, completion, validation, or reset sequence did not match' >&2
    exit 1
fi

if grep -Fq 'FAIL ' "$output"; then
    cat "$output" >&2
    echo 'qtest command failed' >&2
    exit 1
fi

echo 'VAMS SQ/CQ DMA and NOP completion smoke test: PASS'
