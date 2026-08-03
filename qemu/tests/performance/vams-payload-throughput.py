#!/usr/bin/env python3
"""Verify chunk-boundary payload integrity and report virtual-model throughput."""

import argparse
import binascii
import importlib.util
import os
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

DMA_CHUNK_SIZE = 64 * 1024
MAX_TRANSFER = 16 * 1024 * 1024
U32_SIZE = 4
SOURCE = 0x200000
DESTINATION = 0x1400000
FILL_SOURCE = 0x2600000
FILL_DESTINATION = 0x2700000
VECTOR_SOURCE = 0x2800000
VECTOR_DESTINATION = 0x2900000
COMPLETION = struct.Struct("<IHHIIQQ")


def executable_path(value):
    path = shutil.which(value) if os.path.sep not in value else value
    if not path or not os.path.isfile(path):
        raise FileNotFoundError(value)
    return path


class CommandRing:
    def __init__(self, qtest):
        self.qtest = qtest
        self.sq_tail = 0
        self.cq_head = 0

    def submit(self, descriptor):
        slot = self.sq_tail
        next_tail = (slot + 1) & 15
        self.qtest.write(
            SMOKE.SQ_BASE + slot * SMOKE.SUBMISSION.size, descriptor
        )
        start_ns = time.perf_counter_ns()
        self.qtest.write32(SMOKE.BAR0 + 0x114, next_tail)
        SMOKE.wait_for_completion(self.qtest, next_tail)
        elapsed_ns = time.perf_counter_ns() - start_ns
        raw = self.qtest.read(
            SMOKE.CQ_BASE + self.cq_head * COMPLETION.size, COMPLETION.size
        )
        completion = COMPLETION.unpack(raw)
        self.cq_head = (self.cq_head + 1) & 15
        self.qtest.write32(SMOKE.BAR0 + 0x214, self.cq_head)
        self.sq_tail = next_tail
        return completion, elapsed_ns


def expect_success(completion, command_id, length, cookie):
    expected = (command_id, 0, 0, length, 0, cookie)
    if completion[:6] != expected:
        raise AssertionError(
            f"completion mismatch: expected {expected}, got {completion[:6]}"
        )


def write_max_transfer_source(qtest):
    pattern = bytes(
        ((index * 37) + 11) & 0xFF for index in range(DMA_CHUNK_SIZE)
    )
    crc = 0
    for offset in range(0, MAX_TRANSFER, DMA_CHUNK_SIZE):
        qtest.write(SOURCE + offset, pattern)
        crc = binascii.crc32(pattern, crc)
    return crc & 0xFFFFFFFF


def verify_fill(qtest, ring):
    length = DMA_CHUNK_SIZE + 37
    value = 0x6D
    guard = bytes([0xA7]) * 16
    qtest.write(FILL_SOURCE, bytes([value]))
    qtest.write(FILL_DESTINATION - len(guard), guard)
    qtest.write(FILL_DESTINATION + length, guard)
    command_id = 0xC4A00003
    cookie = 0xC4A0000000000003
    completion, _ = ring.submit(SMOKE.submission(
        1, command_id, cookie, opcode=2, source=FILL_SOURCE,
        destination=FILL_DESTINATION, length=length,
    ))
    expect_success(completion, command_id, length, cookie)
    first = qtest.read(FILL_DESTINATION, DMA_CHUNK_SIZE)
    remainder = qtest.read(
        FILL_DESTINATION + DMA_CHUNK_SIZE, length - DMA_CHUNK_SIZE
    )
    if first != bytes([value]) * len(first) or \
            remainder != bytes([value]) * len(remainder):
        raise AssertionError("MEM_FILL chunk-boundary data mismatch")
    if qtest.read(FILL_DESTINATION - len(guard), len(guard)) != guard or \
            qtest.read(FILL_DESTINATION + length, len(guard)) != guard:
        raise AssertionError("MEM_FILL crossed a destination guard")


def verify_vector_add(qtest, ring):
    length = DMA_CHUNK_SIZE + 68
    elements = length // U32_SIZE
    source = bytearray(length)
    destination = bytearray(length)
    expected = bytearray(length)
    for index in range(elements):
        source_value = (index * 0x10203041) & 0xFFFFFFFF
        destination_value = (0xFFFFFFFF - index * 0x01010101) & 0xFFFFFFFF
        struct.pack_into("<I", source, index * 4, source_value)
        struct.pack_into("<I", destination, index * 4, destination_value)
        struct.pack_into(
            "<I", expected, index * 4,
            (source_value + destination_value) & 0xFFFFFFFF,
        )
    guard = bytes([0x5C]) * 16
    qtest.write(VECTOR_SOURCE, source)
    qtest.write(VECTOR_DESTINATION - len(guard), guard + destination + guard)
    command_id = 0xC4A00004
    cookie = 0xC4A0000000000004
    completion, _ = ring.submit(SMOKE.submission(
        1, command_id, cookie, opcode=4, source=VECTOR_SOURCE,
        destination=VECTOR_DESTINATION, length=length,
    ))
    expect_success(completion, command_id, length, cookie)
    if qtest.read(VECTOR_DESTINATION, length) != expected:
        raise AssertionError("VECTOR_ADD chunk-boundary arithmetic mismatch")
    if qtest.read(VECTOR_SOURCE, length) != source:
        raise AssertionError("VECTOR_ADD modified its source")
    if qtest.read(VECTOR_DESTINATION - len(guard), len(guard)) != guard or \
            qtest.read(VECTOR_DESTINATION + length, len(guard)) != guard:
        raise AssertionError("VECTOR_ADD crossed a destination guard")


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--qemu",
        default=os.environ.get("QEMU_SYSTEM_X86_64", "qemu-system-x86_64"),
    )
    return parser.parse_args()


def main():
    args = parse_args()
    try:
        executable = executable_path(args.qemu)
    except FileNotFoundError as error:
        print(f"QEMU executable not found: {error}", file=sys.stderr)
        return 2

    with tempfile.TemporaryFile() as qemu_log:
        qtest = SMOKE.QTest(executable, None, qemu_log)
        try:
            SMOKE.configure_queues(qtest)
            ring = CommandRing(qtest)
            expected_crc = write_max_transfer_source(qtest)

            copy_id = 0xC4A00001
            copy_cookie = 0xC4A0000000000001
            copy_completion, elapsed_ns = ring.submit(SMOKE.submission(
                1, copy_id, copy_cookie, opcode=1, source=SOURCE,
                destination=DESTINATION, length=MAX_TRANSFER,
            ))
            expect_success(
                copy_completion, copy_id, MAX_TRANSFER, copy_cookie
            )

            crc_id = 0xC4A00002
            crc_cookie = 0xC4A0000000000002
            crc_completion, _ = ring.submit(SMOKE.submission(
                1, crc_id, crc_cookie, opcode=3, source=DESTINATION,
                length=MAX_TRANSFER, flags=1, expected_crc=expected_crc,
            ))
            expected_completion = (
                crc_id, 0, 0, MAX_TRANSFER, expected_crc, crc_cookie
            )
            if crc_completion[:6] != expected_completion:
                raise AssertionError(
                    "maximum-transfer CRC mismatch: "
                    f"expected {expected_completion}, got {crc_completion[:6]}"
                )

            verify_fill(qtest, ring)
            verify_vector_add(qtest, ring)
        except Exception as error:
            print(f"chunked DMA smoke failed: {error}", file=sys.stderr)
            qemu_log.seek(0)
            sys.stderr.write(
                qemu_log.read().decode("utf-8", errors="replace")
            )
            return 1
        finally:
            qtest.close()

    seconds = elapsed_ns / 1_000_000_000
    throughput = (MAX_TRANSFER / (1024 * 1024)) / seconds
    print(
        "VAMS chunked DMA: chunk=64KiB max-transfer=16MiB "
        "copy=PASS crc=PASS fill=PASS vector=PASS"
    )
    print(
        f"VAMS virtual-model throughput: opcode=MEM_COPY bytes={MAX_TRANSFER} "
        f"host_ms={elapsed_ns / 1_000_000:.3f} "
        f"mib_per_second={throughput:.3f} PASS"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
