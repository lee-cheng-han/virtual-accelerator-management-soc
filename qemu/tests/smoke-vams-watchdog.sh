#!/bin/sh

set -eu

qemu=${QEMU_SYSTEM_RISCV32:-qemu-system-riscv32}
firmware=${VAMS_WATCHDOG_FIRMWARE:-build/firmware/zephyr-watchdog/zephyr/zephyr.elf}
output=$(mktemp)
trap 'rm -f "$output"' EXIT HUP INT TERM

test -x "$qemu" || command -v "$qemu" >/dev/null 2>&1 || {
    echo "QEMU executable not found: $qemu" >&2
    exit 2
}
test -f "$firmware" || {
    echo "watchdog test image not found: $firmware" >&2
    exit 2
}

set +e
timeout 4 "$qemu" \
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

test "$(grep -Fc 'Virtual Accelerator Management SoC Zephyr booting' "$output")" \
    -ge 2 || {
    cat "$output" >&2
    echo 'watchdog did not reboot the management CPU' >&2
    exit 1
}

for checkpoint in \
    'Watchdog test: withholding pet' \
    'Reset: reason=5 watchdog_count=1 generation=1' \
    'Recovery: watchdog reset observed' \
    'Telemetry: heartbeat=3 '
do
    grep -Fq "$checkpoint" "$output" || {
        cat "$output" >&2
        echo "missing watchdog checkpoint: $checkpoint" >&2
        exit 1
    }
done

if grep -Eq 'ASSERTION FAIL|mcause:' "$output"; then
    cat "$output" >&2
    echo 'watchdog recovery reported a fatal error' >&2
    exit 1
fi

echo 'VAMS watchdog reset and recovery smoke test: PASS'
