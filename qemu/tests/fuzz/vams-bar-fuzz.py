#!/usr/bin/env python3
"""Drive replayable malformed BAR access sequences against vams-pcie."""

import argparse
import importlib.util
import os
import random
import shutil
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SMOKE_PATH = ROOT / "qemu" / "tests" / "smoke-vams-firmware-pcie.py"
SPEC = importlib.util.spec_from_file_location("vams_firmware_pcie", SMOKE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load {SMOKE_PATH}")
SMOKE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SMOKE)


def executable_path(value):
    path = shutil.which(value) if os.path.sep not in value else value
    if not path or not os.path.isfile(path):
        raise FileNotFoundError(value)
    return path


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--qemu",
        default=os.environ.get("QEMU_SYSTEM_X86_64", "qemu-system-x86_64"),
    )
    parser.add_argument("--seed", type=lambda value: int(value, 0),
                        default=0xBA4F0223)
    parser.add_argument("--iterations", type=int, default=4096)
    return parser.parse_args()


def main():
    args = parse_args()
    if args.iterations <= 0:
        print("--iterations must be positive", file=sys.stderr)
        return 2
    try:
        executable = executable_path(args.qemu)
    except FileNotFoundError as error:
        print(f"QEMU executable not found: {error}", file=sys.stderr)
        return 2

    rng = random.Random(args.seed)
    reads = ("readb", "readw", "readl", "readq")
    writes = ("writeb", "writew", "writel", "writeq")
    operations = []

    with tempfile.TemporaryFile() as qemu_log:
        qtest = SMOKE.QTest(executable, None, qemu_log)
        try:
            SMOKE.configure_queues(qtest)
            for iteration in range(args.iterations):
                width_index = rng.randrange(4)
                offset = rng.randrange(0x1000)
                address = SMOKE.BAR0 + offset
                if rng.getrandbits(1):
                    operation = f"{reads[width_index]} 0x{address:x}"
                else:
                    bits = 8 << width_index
                    value = rng.getrandbits(bits)
                    operation = (
                        f"{writes[width_index]} 0x{address:x} 0x{value:x}"
                    )
                operations.append(operation)
                if len(operations) > 32:
                    operations.pop(0)
                try:
                    qtest.command(operation)
                    if iteration % 32 == 0:
                        identity = qtest.read32(SMOKE.BAR0)
                        if identity != 0x11001B36:
                            raise AssertionError(
                                f"identity changed to 0x{identity:08x}"
                            )
                        qtest.read32(SMOKE.BAR0 + 0x01C)
                        qtest.clock_step(1_000_000)
                except Exception as error:
                    print(
                        f"BAR fuzz failed: seed=0x{args.seed:x} "
                        f"iteration={iteration} error={error}",
                        file=sys.stderr,
                    )
                    for replay in operations:
                        print(f"  {replay}", file=sys.stderr)
                    qemu_log.seek(0)
                    sys.stderr.write(qemu_log.read().decode(
                        "utf-8", errors="replace"
                    ))
                    return 1
        finally:
            qtest.close()

    print(
        f"VAMS BAR fuzz: seed=0x{args.seed:x} "
        f"iterations={args.iterations} PASS"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
