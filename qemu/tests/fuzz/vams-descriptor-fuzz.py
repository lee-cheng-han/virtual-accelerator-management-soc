#!/usr/bin/env python3
"""Deterministically mutate raw VAMS descriptors and check first-error results."""

import argparse
import importlib.util
import os
import random
import shutil
import struct
import sys
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SMOKE_PATH = ROOT / "qemu" / "tests" / "smoke-vams-firmware-pcie.py"
SPEC = importlib.util.spec_from_file_location("vams_firmware_pcie", SMOKE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load {SMOKE_PATH}")
SMOKE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SMOKE)

SUBMISSION = struct.Struct("<HBBIQQIIQIIQQ")
COMPLETION = struct.Struct("<IHHIIQQ")
SOURCE = 0x200000
DESTINATION = 0x210000


def executable_path(value):
    path = shutil.which(value) if os.path.sep not in value else value
    if not path or not os.path.isfile(path):
        raise FileNotFoundError(value)
    return path


def base_descriptor(command_id, cookie):
    return {
        "version": 1,
        "opcode": 0,
        "flags": 0,
        "command_id": command_id,
        "source": 0,
        "destination": 0,
        "length": 0,
        "timeout_ms": 0,
        "cookie": cookie,
        "expected_crc": 0,
        "reserved0": 0,
        "reserved1": 0,
        "reserved2": 0,
    }


def payload_descriptor(command_id, cookie, opcode):
    descriptor = base_descriptor(command_id, cookie)
    descriptor["opcode"] = opcode
    descriptor["source"] = SOURCE
    descriptor["length"] = 64
    if opcode in (1, 2, 4):
        descriptor["destination"] = DESTINATION
    return descriptor


def mutate(category, command_id, cookie):
    descriptor = base_descriptor(command_id, cookie)
    status = 1

    if category == "valid-nop":
        return descriptor, 0, 0
    if category == "version":
        descriptor.update(version=2, opcode=0xFF, flags=0xFF, reserved2=1)
        return descriptor, status, 1
    if category == "opcode":
        descriptor.update(opcode=0xFF, flags=0xFF, reserved1=1)
        return descriptor, status, 2
    if category == "flags-non-crc":
        descriptor.update(flags=1, reserved0=1)
        return descriptor, status, 3
    if category == "flags-crc":
        descriptor = payload_descriptor(command_id, cookie, 3)
        descriptor.update(flags=2, reserved0=1)
        return descriptor, status, 3
    if category == "expected-crc":
        descriptor["expected_crc"] = 0x12345678
        return descriptor, status, 4
    if category == "reserved0":
        descriptor["reserved0"] = 1
        return descriptor, status, 4
    if category == "reserved1":
        descriptor["reserved1"] = 1
        return descriptor, status, 4
    if category == "reserved2":
        descriptor["reserved2"] = 1
        return descriptor, status, 4
    if category == "timeout":
        descriptor.update(timeout_ms=60001, length=1, source=1)
        return descriptor, status, 10
    if category == "nop-length":
        descriptor.update(length=1, source=1)
        return descriptor, status, 6
    if category == "nop-address":
        descriptor["source"] = SOURCE
        return descriptor, status, 9

    opcode_name, fault = category.split("-", 1)
    opcode = {"copy": 1, "fill": 2, "crc": 3, "vector": 4}[opcode_name]
    descriptor = payload_descriptor(command_id, cookie, opcode)
    if fault == "length":
        descriptor["length"] = 2 if opcode == 4 else 0
        return descriptor, status, 6
    if fault == "zero":
        descriptor["source"] = 0
        return descriptor, status, 9
    if fault == "overflow":
        descriptor["source"] = (1 << 64) - 32
        descriptor["length"] = 64
        return descriptor, status, 8
    if fault == "destination-overflow":
        descriptor["destination"] = (1 << 64) - 32
        descriptor["length"] = 64
        return descriptor, status, 8
    if fault == "destination":
        descriptor["destination"] = DESTINATION
        return descriptor, status, 9
    if fault == "alignment":
        descriptor["source"] += 1
        return descriptor, status, 7
    if fault == "overlap":
        descriptor["destination"] = descriptor["source"] + 4
        return descriptor, status, 9
    raise AssertionError(category)


def pack_descriptor(descriptor):
    return SUBMISSION.pack(
        descriptor["version"],
        descriptor["opcode"],
        descriptor["flags"],
        descriptor["command_id"],
        descriptor["source"],
        descriptor["destination"],
        descriptor["length"],
        descriptor["timeout_ms"],
        descriptor["cookie"],
        descriptor["expected_crc"],
        descriptor["reserved0"],
        descriptor["reserved1"],
        descriptor["reserved2"],
    )


def wait_for_tail(qtest, expected):
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        if qtest.read32(SMOKE.BAR0 + 0x210) == expected:
            return
    raise RuntimeError(f"CQ tail did not reach {expected}")


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--qemu",
        default=os.environ.get("QEMU_SYSTEM_X86_64", "qemu-system-x86_64"),
    )
    parser.add_argument("--seed", type=lambda value: int(value, 0),
                        default=0xD35C0123)
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

    categories = (
        "valid-nop", "version", "opcode", "flags-non-crc", "flags-crc",
        "expected-crc", "reserved0", "reserved1", "reserved2", "timeout",
        "nop-length", "nop-address", "copy-length", "copy-zero",
        "copy-overflow", "copy-overlap", "fill-length", "fill-zero",
        "fill-destination-overflow", "crc-length", "crc-zero",
        "crc-overflow", "crc-destination", "vector-length", "vector-zero",
        "vector-alignment", "vector-overflow", "vector-overlap",
    )
    rng = random.Random(args.seed)
    coverage = set()

    with tempfile.TemporaryFile() as qemu_log:
        qtest = SMOKE.QTest(executable, None, qemu_log)
        try:
            SMOKE.configure_queues(qtest)
            producer = 0
            for iteration in range(args.iterations):
                category = categories[iteration % len(categories)]
                command_id = rng.getrandbits(32)
                cookie = rng.getrandbits(64)
                descriptor, expected_status, expected_error = mutate(
                    category, command_id, cookie
                )
                raw = pack_descriptor(descriptor)
                next_producer = (producer + 1) & 15
                try:
                    qtest.write(
                        SMOKE.SQ_BASE + producer * SUBMISSION.size, raw
                    )
                    qtest.write32(SMOKE.BAR0 + 0x114, next_producer)
                    wait_for_tail(qtest, next_producer)
                    completion_raw = qtest.read(
                        SMOKE.CQ_BASE + producer * COMPLETION.size,
                        COMPLETION.size,
                    )
                    actual = COMPLETION.unpack(completion_raw)
                    expected = (
                        command_id, expected_status, expected_error,
                        0, 0, cookie,
                    )
                    if actual[:6] != expected:
                        raise AssertionError(
                            f"expected {expected}, got {actual[:6]}"
                        )
                    qtest.write32(SMOKE.BAR0 + 0x214, next_producer)
                except Exception as error:
                    print(
                        f"descriptor fuzz failed: seed=0x{args.seed:x} "
                        f"iteration={iteration} category={category} "
                        f"descriptor={raw.hex()} error={error}",
                        file=sys.stderr,
                    )
                    qemu_log.seek(0)
                    sys.stderr.write(qemu_log.read().decode(
                        "utf-8", errors="replace"
                    ))
                    return 1
                coverage.add(category)
                producer = next_producer
        finally:
            qtest.close()

    missing = set(categories) - coverage
    if missing:
        print(f"descriptor fuzz coverage missing: {sorted(missing)}",
              file=sys.stderr)
        return 1
    print(
        f"VAMS descriptor fuzz: seed=0x{args.seed:x} "
        f"iterations={args.iterations} categories={len(coverage)} PASS"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
