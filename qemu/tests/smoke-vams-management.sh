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
timeout 4 "$qemu" \
    -M vams_riscv \
    -global vams-mgmt.test-message=1 \
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
    'Services: mailbox, watchdog, reset telemetry' \
    'Mailbox: request=0x00000001 response=0x80000001' \
    'Telemetry: heartbeat=1 ' \
    'Telemetry: heartbeat=2 ' \
    'Telemetry: heartbeat=3 '
do
    grep -Fq "$checkpoint" "$output" || {
        cat "$output" >&2
        echo "missing management checkpoint: $checkpoint" >&2
        exit 1
    }
done

if grep -Eq 'ASSERTION FAIL|mcause:' "$output"; then
    cat "$output" >&2
    echo 'management firmware reported a fatal error' >&2
    exit 1
fi

echo 'VAMS mailbox, watchdog, and telemetry smoke test: PASS'
