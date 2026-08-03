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

run_case()
{
    command=$1
    expected=$2

    set +e
    timeout 2 "$qemu" \
        -M vams_riscv \
        -global vams-mgmt.test-command="$command" \
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
    grep -Fq "$expected" "$output" || {
        cat "$output" >&2
        echo "missing firmware command result: $expected" >&2
        exit 1
    }
    for transition in 'from=0 to=1' 'from=1 to=2'; do
        grep -Fq "$transition" "$output" || {
            cat "$output" >&2
            echo "missing scheduler transition: $transition" >&2
            exit 1
        }
    done
    if [ "$command" -eq 1 ]; then
        transitions='from=2 to=3
from=3 to=4
from=4 to=6
from=6 to=0'
    else
        transitions='from=2 to=7
from=7 to=0'
    fi
    echo "$transitions" | while IFS= read -r transition; do
        grep -Fq "$transition" "$output" || {
            cat "$output" >&2
            echo "missing scheduler transition: $transition" >&2
            exit 1
        }
    done
    grep -Fq 'Scheduler: event=published sequence=1 command=0x56414d53 generation=0 count=1' \
        "$output" || {
        cat "$output" >&2
        echo 'missing exactly-once scheduler publication' >&2
        exit 1
    }
    if grep -Eq 'ASSERTION FAIL|mcause:' "$output"; then
        cat "$output" >&2
        echo 'firmware command service reported a fatal error' >&2
        exit 1
    fi
}

run_case 1 'Command: id=0x56414d53 status=0 error=0 cookie=0x1122334455667788'
run_case 2 'Command: id=0x56414d53 status=1 error=1 cookie=0x1122334455667788'

echo 'VAMS firmware-owned NOP validation and completion smoke test: PASS'
