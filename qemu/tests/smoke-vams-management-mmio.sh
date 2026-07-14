#!/bin/sh

set -eu

qemu=${QEMU_SYSTEM_RISCV32:-qemu-system-riscv32}
firmware=${VAMS_ZEPHYR_FIRMWARE:-build/firmware/zephyr/zephyr/zephyr.elf}
output=$(mktemp)
trap 'rm -f "$output"' EXIT HUP INT TERM

test -x "$qemu" || command -v "$qemu" >/dev/null 2>&1 || {
    echo "QEMU executable not found: $qemu" >&2
    exit 2
}
test -f "$firmware" || {
    echo "Zephyr image not found: $firmware" >&2
    exit 2
}

set +e
printf '%s\n' \
    'readl 0x10020000' \
    'readl 0x10010010' \
    'writel 0x10010004 0x1' \
    'readl 0x10010010' \
    'readl 0x10020028' \
    'writel 0x10010008 0x80000001' \
    'writel 0x1001000c 0x1' \
    'readl 0x10010010' \
    'readl 0x10010008' \
    'readl 0x1002002c' \
    'writel 0x10010010 0x2' \
    'readl 0x10010010' \
    'writel 0x10020000 0x3e8' \
    'readl 0x10020000' \
    'writel 0x10020018 0x2a' \
    'readl 0x10020018' \
    'writel 0x10020000 0x10' \
    'readl 0x10020034' \
    'writel 0x10020034 0x1' \
    'readl 0x10020034' \
    'readl 0x10020038' \
    'readb 0x10020000' | \
    timeout 2 "$qemu" \
        -M vams_riscv \
        -global vams-mgmt.test-message=1 \
        -display none \
        -serial none \
        -bios "$firmware" \
        -qtest stdio >"$output" 2>&1
status=$?
set -e

if [ "$status" -ne 124 ]; then
    cat "$output" >&2
    echo "unexpected qtest exit status: $status" >&2
    exit 1
fi

expected=$(printf '%s\n' \
    'OK 0x0000000000001388' \
    'OK 0x0000000000000001' \
    'OK 0x0000000000000000' \
    'OK 0x0000000000000001' \
    'OK 0x0000000000000002' \
    'OK 0x0000000080000001' \
    'OK 0x0000000000000001' \
    'OK 0x0000000000000000' \
    'OK 0x00000000000003e8' \
    'OK 0x000000000000002a' \
    'OK 0x0000000000000001' \
    'OK 0x0000000000000000' \
    'OK 0x00000000ffffffff' \
    'OK 0x0000000000000000')
actual=$(grep '^OK 0x' "$output")
if [ "$actual" != "$expected" ]; then
    cat "$output" >&2
    echo 'management MMIO read sequence did not match' >&2
    exit 1
fi

if grep -Fq 'FAIL ' "$output"; then
    cat "$output" >&2
    echo 'qtest command failed' >&2
    exit 1
fi

echo 'VAMS management MMIO qtest smoke test: PASS'
