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
timeout 5 "$qemu" \
    -M vams_riscv \
    -display none \
    -monitor none \
    -serial stdio \
    -bios "$firmware" >"$output" 2>&1
status=$?
set -e

if [ "$status" -ne 124 ]; then
    cat "$output" >&2
    echo "unexpected QEMU exit status: $status" >&2
    exit 1
fi

for checkpoint in \
    'Virtual Accelerator Management SoC Zephyr booting' \
    'Kernel: Zephyr 4.4.0' \
    'Tasks: producer -> message queue -> monitor' \
    'Heartbeat: sequence=1 ' \
    'Heartbeat: sequence=2 ' \
    'Heartbeat: sequence=3 '
do
    grep -Fq "$checkpoint" "$output" || {
        cat "$output" >&2
        echo "missing Zephyr checkpoint: $checkpoint" >&2
        exit 1
    }
done

echo 'VAMS Zephyr boot and IPC smoke test: PASS'
