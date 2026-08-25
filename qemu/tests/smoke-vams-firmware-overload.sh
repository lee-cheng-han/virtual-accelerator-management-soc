#!/bin/sh

set -eu

qemu=${QEMU_SYSTEM_RISCV32:-qemu-system-riscv32}
firmware=${VAMS_OVERLOAD_FIRMWARE:-build/firmware/zephyr-overload/zephyr/zephyr.elf}
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
timeout 5 "$qemu" -M vams_riscv -display none -monitor none \
    -serial stdio -bios "$firmware" >"$output" 2>&1
status=$?
set -e

if [ "$status" -ne 124 ]; then
    cat "$output" >&2
    echo "unexpected QEMU exit status: $status" >&2
    exit 1
fi

grep -Eq 'Resources: .*heartbeat_drops=4 .*event_drops=32' "$output" || {
    cat "$output" >&2
    echo 'deterministic overload counters do not match' >&2
    exit 1
}
grep -Fq 'Heartbeat: sequence=1 ' "$output" || {
    cat "$output" >&2
    echo 'firmware did not make progress after overload' >&2
    exit 1
}
if grep -Eq 'ASSERTION FAIL|mcause:' "$output"; then
    cat "$output" >&2
    echo 'firmware overload test reported a fatal error' >&2
    exit 1
fi

echo 'VAMS firmware deterministic overload smoke test: PASS'
