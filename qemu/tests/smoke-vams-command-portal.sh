#!/bin/sh

set -eu

qemu=${QEMU_SYSTEM_RISCV32:-qemu-system-riscv32}
firmware=${VAMS_FIRMWARE:-build/firmware/baremetal/vams-riscv-fw.elf}
output=$(mktemp)
invalid_output=$(mktemp)
trap 'rm -f "$output" "$invalid_output"' EXIT HUP INT TERM

test -x "$qemu" || command -v "$qemu" >/dev/null 2>&1 || {
    echo "QEMU executable not found: $qemu" >&2
    exit 2
}
test -f "$firmware" || {
    echo "firmware image not found: $firmware" >&2
    exit 2
}

set +e
"$qemu" -M vams_riscv -global vams-mgmt.test-command=3 \
    -display none -serial none -bios "$firmware" >"$invalid_output" 2>&1
invalid_status=$?
set -e
if [ "$invalid_status" -eq 0 ] ||
   ! grep -Fq 'test-command must be 0, 1, or 2' "$invalid_output"; then
    cat "$invalid_output" >&2
    echo 'invalid command injection property was not rejected' >&2
    exit 1
fi

set +e
printf '%s\n' \
    'writel 0x10030100 0x1' \
    'writel 0x10030104 0x56414d53' \
    'writel 0x10030120 0x55667788' \
    'writel 0x10030124 0x11223344' \
    'writel 0x10030004 0x1' \
    'readl 0x10030000' \
    'readl 0x10030014' \
    'writel 0x10030100 0x2' \
    'readl 0x10030000' \
    'writel 0x10030000 0x4' \
    'readl 0x10030100' \
    'writel 0x10030008 0x1' \
    'readl 0x10030000' \
    'writel 0x10030200 0x56414d53' \
    'writel 0x10030204 0x0' \
    'writel 0x10030210 0x55667788' \
    'writel 0x10030214 0x11223344' \
    'writel 0x1003000c 0x1' \
    'readl 0x10030000' \
    'readl 0x10030018' \
    'read 0x10030200 32' \
    'writel 0x10030010 0x1' \
    'readl 0x10030000' \
    'writel 0x10030010 0x1' \
    'readl 0x10030000' \
    'writel 0x10030000 0x10' \
    'readl 0x10030000' | \
    timeout 2 "$qemu" \
        -M vams_riscv \
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
    'OK 0x0000000000000001' \
    'OK 0x0000000000000001' \
    'OK 0x0000000000000005' \
    'OK 0x0000000000000001' \
    'OK 0x0000000000000000' \
    'OK 0x0000000000000002' \
    'OK 0x0000000000000001' \
    'OK 0x534d415600000000000000000000000088776655443322110000000000000000' \
    'OK 0x0000000000000000' \
    'OK 0x0000000000000010' \
    'OK 0x0000000000000000')
actual=$(grep '^OK 0x' "$output")
if [ "$actual" != "$expected" ]; then
    cat "$output" >&2
    echo 'command portal ownership, overflow, or completion state did not match' >&2
    exit 1
fi

if grep -Fq 'FAIL ' "$output"; then
    cat "$output" >&2
    echo 'qtest command failed' >&2
    exit 1
fi

echo 'VAMS firmware command portal QTest: PASS'
