#!/bin/sh

set -eu

qemu=${QEMU_SYSTEM_RISCV32:-qemu-system-riscv32}
firmware=${VAMS_HEALTH_FIRMWARE:-build/firmware/zephyr-health/zephyr/zephyr.elf}
output=$(mktemp)
trap 'rm -f "$output"' EXIT HUP INT TERM

test -x "$qemu" || command -v "$qemu" >/dev/null 2>&1 || {
    echo "QEMU executable not found: $qemu" >&2
    exit 2
}
test -f "$firmware" || {
    echo "health test image not found: $firmware" >&2
    exit 2
}

for task in 1 2 3 4 5 6 7 8; do
    set +e
    timeout 4 "$qemu" -M vams_riscv \
        -global "vams-mgmt.x-test-freeze-task=$task" \
        -display none -monitor none -serial stdio -bios "$firmware" \
        >"$output" 2>&1
    status=$?
    set -e
    if [ "$status" -ne 124 ]; then
        cat "$output" >&2
        echo "unexpected QEMU exit status for frozen task $task: $status" >&2
        exit 1
    fi
    grep -Fq "Health: stuck_task=$task failures=1" "$output" || {
        cat "$output" >&2
        echo "missing diagnosis for frozen task $task" >&2
        exit 1
    }
    grep -Eq "Retained: valid=1 boots=2 .*stuck_task=$task health_failures=1" \
        "$output" || {
        cat "$output" >&2
        echo "missing retained evidence for frozen task $task" >&2
        exit 1
    }
    grep -Fq 'Reset: reason=5 watchdog_count=1 generation=1' "$output" || {
        cat "$output" >&2
        echo "missing watchdog recovery for frozen task $task" >&2
        exit 1
    }
    grep -Fq 'Telemetry: heartbeat=3 ' "$output" || {
        cat "$output" >&2
        echo "missing post-reset progress for frozen task $task" >&2
        exit 1
    }
    if grep -Eq 'ASSERTION FAIL|mcause:' "$output"; then
        cat "$output" >&2
        echo "fatal error while testing frozen task $task" >&2
        exit 1
    fi
done

echo 'VAMS per-task health diagnosis and retained recovery: PASS'
