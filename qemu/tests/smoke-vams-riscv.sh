#!/bin/sh

set -eu

qemu=${QEMU_SYSTEM_RISCV32:-qemu-system-riscv32}
firmware=${VAMS_FIRMWARE:-build/firmware/baremetal/vams-riscv-fw.elf}
output=$(mktemp)
trap 'rm -f "$output"' EXIT HUP INT TERM

test -x "$qemu" || command -v "$qemu" >/dev/null 2>&1 || {
    echo "QEMU executable not found: $qemu" >&2
    exit 2
}
test -f "$firmware" || {
    echo "firmware image not found: $firmware" >&2
    exit 2
}

"$qemu" -machine help | grep -q '^vams_riscv '

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
    'Virtual Accelerator Management SoC firmware booting' \
    'CPU: RV32' \
    'SRAM: detected' \
    'UART: ready'
do
    grep -Fq "$checkpoint" "$output" || {
        cat "$output" >&2
        echo "missing boot checkpoint: $checkpoint" >&2
        exit 1
    }
done

if grep -Fq 'SRAM: error' "$output"; then
    cat "$output" >&2
    echo 'firmware reported an SRAM failure' >&2
    exit 1
fi

echo 'VAMS RISC-V boot smoke test: PASS'
