#!/usr/bin/env python3
"""Verify deterministic CQ watermark hysteresis and overload throttling."""

import importlib.util
import os
import subprocess
import sys
from pathlib import Path


TEST_ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "vams_firmware_pcie", TEST_ROOT / "smoke-vams-firmware-pcie.py"
)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

CQ_WATERMARK = 0x220
CQ_HIGH_WATER = 0x224
CQ_BACKPRESSURE_COUNT = 0x228
CQ_STATUS = 0x21C
CQ_BACKPRESSURE = 1 << 3


def configure(qtest):
    qtest.out32(0xCF8, 0x80001010)
    qtest.out32(0xCFC, MODULE.BAR0)
    qtest.out32(0xCF8, 0x80001014)
    qtest.out32(0xCFC, 0xFEBE0000)
    qtest.out32(0xCF8, 0x80001004)
    qtest.out32(0xCFC, 0x6)
    for offset, value in (
        (0x100, MODULE.SQ_BASE), (0x104, 0), (0x108, 16),
        (0x200, MODULE.CQ_BASE), (0x204, 0), (0x208, 16),
        (CQ_WATERMARK, (4 << 16) | 2),
        (0x218, 1), (0x118, 1), (0x020, 1),
    ):
        qtest.write32(MODULE.BAR0 + offset, value)


def expect_state(qtest, sq_head, cq_tail, count, throttled=True):
    actual = (
        qtest.read32(MODULE.BAR0 + 0x10C),
        qtest.read32(MODULE.BAR0 + 0x210),
        qtest.read32(MODULE.BAR0 + CQ_BACKPRESSURE_COUNT),
    )
    expected = (sq_head, cq_tail, count)
    if actual != expected:
        raise AssertionError(f"queue state: expected {expected}, got {actual}")
    status = qtest.read32(MODULE.BAR0 + CQ_STATUS)
    if bool(status & CQ_BACKPRESSURE) != throttled:
        raise AssertionError(f"unexpected CQ backpressure status 0x{status:x}")


def main():
    executable = os.environ.get("QEMU_SYSTEM_X86_64", "qemu-system-x86_64")
    try:
        executable = MODULE.executable_path(executable)
    except FileNotFoundError as error:
        print(f"QEMU executable not found: {error}", file=sys.stderr)
        return 2

    qtest = MODULE.QTest(executable, None, subprocess.DEVNULL)
    try:
        configure(qtest)
        for slot in range(8):
            command_id = 0xC0B00000 + slot
            cookie = 0xC0B0000000000000 + slot
            qtest.write(
                MODULE.SQ_BASE + slot * MODULE.SUBMISSION.size,
                MODULE.submission(1, command_id, cookie),
            )
        qtest.write32(MODULE.BAR0 + 0x114, 8)

        expect_state(qtest, 4, 4, 1)
        qtest.write32(MODULE.BAR0 + 0x214, 1)
        expect_state(qtest, 4, 4, 1)
        qtest.write32(MODULE.BAR0 + 0x214, 2)
        expect_state(qtest, 6, 6, 2)
        qtest.write32(MODULE.BAR0 + 0x214, 4)
        expect_state(qtest, 8, 8, 3)

        if qtest.read32(MODULE.BAR0 + CQ_HIGH_WATER) != 4:
            raise AssertionError("CQ high-water evidence mismatch")
        for slot in range(8):
            MODULE.check_completion(
                qtest, slot,
                (0xC0B00000 + slot, 0, 0, 0, 0,
                 0xC0B0000000000000 + slot),
            )

        qtest.write32(MODULE.BAR0 + 0x214, 8)
        expect_state(qtest, 8, 8, 3, throttled=False)
    except (AssertionError, OSError, RuntimeError) as error:
        print(f"CQ backpressure QTest failed: {error}", file=sys.stderr)
        return 1
    finally:
        qtest.close()

    print("VAMS CQ watermark hysteresis and overload throttling: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
