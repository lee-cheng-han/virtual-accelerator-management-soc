#!/usr/bin/env python3
"""Run reproducible, hardware-free VAMS queue and payload qualification."""

import argparse
import binascii
import hashlib
import importlib.util
import json
import math
import os
import platform
import shutil
import statistics
import struct
import subprocess
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

DEPTH = 16
MAX_BATCH = DEPTH - 1
COMPLETION = struct.Struct("<IHHIIQQ")
RESET_GENERATION = SMOKE.BAR0 + 0x028
SQ_DOORBELL = SMOKE.BAR0 + 0x114
SQ_CONTROL = SMOKE.BAR0 + 0x118
CQ_TAIL = SMOKE.BAR0 + 0x210
CQ_DOORBELL = SMOKE.BAR0 + 0x214
CQ_CONTROL = SMOKE.BAR0 + 0x218
DEVICE_CONTROL = SMOKE.BAR0 + 0x020
SOURCE = 0x200000
DESTINATION = 0x210000
FILL_VALUE = 0x220000
PAYLOAD_LENGTH = 256


def executable_path(value):
    path = shutil.which(value) if os.path.sep not in value else value
    if not path or not os.path.isfile(path):
        raise FileNotFoundError(value)
    return path


def ring_segments(start, count, entry_size, base):
    first = min(count, DEPTH - start)
    yield base + start * entry_size, first
    if first != count:
        yield base, count - first


def percentile(values, percent):
    ordered = sorted(values)
    rank = max(0, math.ceil((percent / 100.0) * len(ordered)) - 1)
    return ordered[rank]


def latency_report(samples):
    return {
        "samples": len(samples),
        "min_us": round(min(samples) / 1_000, 3),
        "mean_us": round(statistics.fmean(samples) / 1_000, 3),
        "stddev_us": round(
            (statistics.pstdev(samples) if len(samples) > 1 else 0) / 1_000,
            3,
        ),
        "p50_us": round(percentile(samples, 50) / 1_000, 3),
        "p90_us": round(percentile(samples, 90) / 1_000, 3),
        "p99_us": round(percentile(samples, 99) / 1_000, 3),
        "p99_9_us": round(percentile(samples, 99.9) / 1_000, 3),
        "max_us": round(max(samples) / 1_000, 3),
    }


def command_output(command):
    return subprocess.check_output(
        command, cwd=ROOT, text=True, stderr=subprocess.STDOUT
    ).strip()


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class QualificationRing:
    def __init__(self, qtest):
        self.qtest = qtest
        self.sq_tail = 0
        self.cq_head = 0
        self.sequence = 0
        self.wraps = 0
        self.high_water = 0

    def next_identity(self):
        self.sequence += 1
        command_id = 0x51000000 + self.sequence
        cookie = 0x5100000000000000 + self.sequence
        return command_id, cookie

    def submit_nops(self, count):
        if not 1 <= count <= MAX_BATCH:
            raise ValueError(f"invalid batch size {count}")
        descriptors = []
        expected = []
        for _ in range(count):
            command_id, cookie = self.next_identity()
            descriptors.append(SMOKE.submission(1, command_id, cookie))
            expected.append((command_id, 0, 0, 0, 0, cookie))
        payload = b"".join(descriptors)
        consumed = 0
        for address, entries in ring_segments(
                self.sq_tail, count, SMOKE.SUBMISSION.size, SMOKE.SQ_BASE):
            length = entries * SMOKE.SUBMISSION.size
            self.qtest.write(address, payload[consumed:consumed + length])
            consumed += length
        next_tail = (self.sq_tail + count) & (DEPTH - 1)
        start_ns = time.perf_counter_ns()
        self.qtest.write32(SQ_DOORBELL, next_tail)
        if self.qtest.read32(CQ_TAIL) != next_tail:
            raise AssertionError("NOP batch did not complete synchronously")
        raw = bytearray()
        for address, entries in ring_segments(
                self.cq_head, count, COMPLETION.size, SMOKE.CQ_BASE):
            raw.extend(self.qtest.read(address, entries * COMPLETION.size))
        elapsed_ns = time.perf_counter_ns() - start_ns
        actual = [COMPLETION.unpack_from(raw, offset * COMPLETION.size)[:6]
                  for offset in range(count)]
        if actual != expected:
            for index, (wanted, got) in enumerate(zip(expected, actual)):
                if wanted != got:
                    raise AssertionError(
                        f"completion {index}: expected {wanted}, got {got}"
                    )
            raise AssertionError("completion count mismatch")
        old_tail = self.sq_tail
        self.sq_tail = next_tail
        self.cq_head = (self.cq_head + count) & (DEPTH - 1)
        self.qtest.write32(CQ_DOORBELL, self.cq_head)
        if old_tail + count >= DEPTH:
            self.wraps += 1
        self.high_water = max(self.high_water, count)
        return elapsed_ns

    def submit_payload(self, opcode):
        command_id, cookie = self.next_identity()
        source = SOURCE
        destination = DESTINATION
        expected_crc = 0
        flags = 0
        if opcode == 2:
            source = FILL_VALUE
        elif opcode == 3:
            destination = 0
            expected_crc = binascii.crc32(
                self.qtest.read(SOURCE, PAYLOAD_LENGTH)
            ) & 0xFFFFFFFF
            flags = 1
        descriptor = SMOKE.submission(
            1, command_id, cookie, opcode=opcode, source=source,
            destination=destination, length=PAYLOAD_LENGTH,
            flags=flags, expected_crc=expected_crc,
        )
        self.qtest.write(
            SMOKE.SQ_BASE + self.sq_tail * SMOKE.SUBMISSION.size,
            descriptor,
        )
        next_tail = (self.sq_tail + 1) & (DEPTH - 1)
        start_ns = time.perf_counter_ns()
        self.qtest.write32(SQ_DOORBELL, next_tail)
        self.qtest.clock_step(25_000_000)
        if self.qtest.read32(CQ_TAIL) != next_tail:
            raise AssertionError(f"payload opcode {opcode} did not complete")
        raw = self.qtest.read(
            SMOKE.CQ_BASE + self.cq_head * COMPLETION.size, COMPLETION.size
        )
        completion = COMPLETION.unpack(raw)
        expected_result_crc = expected_crc if opcode == 3 else 0
        expected = (
            command_id, 0, 0, PAYLOAD_LENGTH, expected_result_crc, cookie
        )
        if completion[:6] != expected:
            raise AssertionError(
                f"payload completion: expected {expected}, got {completion[:6]}"
            )
        self.sq_tail = next_tail
        self.cq_head = (self.cq_head + 1) & (DEPTH - 1)
        self.qtest.write32(CQ_DOORBELL, self.cq_head)
        return time.perf_counter_ns() - start_ns

    def reset(self, expected_generation):
        self.qtest.write32(SQ_CONTROL, 2)
        generation = self.qtest.read32(RESET_GENERATION)
        if generation != expected_generation:
            raise AssertionError(
                f"reset generation: expected {expected_generation}, got {generation}"
            )
        if self.qtest.read32(CQ_TAIL) != 0:
            raise AssertionError("queue reset left a stale completion")
        self.qtest.write32(CQ_CONTROL, 1)
        self.qtest.write32(SQ_CONTROL, 1)
        self.qtest.write32(DEVICE_CONTROL, 1)
        self.sq_tail = 0
        self.cq_head = 0


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--qemu",
        default=os.environ.get("QEMU_SYSTEM_X86_64", "qemu-system-x86_64"),
    )
    parser.add_argument("--commands", type=int, default=1_000_000)
    parser.add_argument("--resets", type=int, default=1_000)
    parser.add_argument("--payload-samples", type=int, default=256)
    parser.add_argument("--virtual-hours", type=int, default=24)
    parser.add_argument("--json-output", type=Path)
    args = parser.parse_args()
    if args.commands < args.payload_samples + 2:
        parser.error("commands must exceed payload-samples by at least two")
    if min(args.resets, args.payload_samples, args.virtual_hours) < 0:
        parser.error("counts and virtual-hours must not be negative")
    return args


def run(args, executable):
    transport_commands = args.commands - args.payload_samples - 1
    batch_latencies = []
    payload_latencies = []
    completed = 0
    resets_done = 0
    started_ns = time.perf_counter_ns()
    with tempfile.TemporaryFile() as qemu_log:
        qtest = SMOKE.QTest(executable, None, qemu_log)
        try:
            SMOKE.configure_queues(qtest)
            ring = QualificationRing(qtest)
            initial_generation = qtest.read32(RESET_GENERATION)
            while completed < transport_commands:
                count = min(MAX_BATCH, transport_commands - completed)
                batch_latencies.append(ring.submit_nops(count))
                completed += count
                due = (completed * args.resets) // max(transport_commands, 1)
                while resets_done < due:
                    resets_done += 1
                    ring.reset((initial_generation + resets_done) & 0xFFFFFFFF)

            source = bytes(((index * 37) + 11) & 0xFF
                           for index in range(PAYLOAD_LENGTH))
            destination = bytes((index * 3) & 0xFF
                                for index in range(PAYLOAD_LENGTH))
            vector_expected = b"".join(
                struct.pack(
                    "<I",
                    (struct.unpack_from("<I", source, offset)[0] +
                     struct.unpack_from("<I", destination, offset)[0]) &
                    0xFFFFFFFF,
                )
                for offset in range(0, PAYLOAD_LENGTH, 4)
            )
            qtest.write(SOURCE, source)
            qtest.write(DESTINATION, destination)
            qtest.write(FILL_VALUE, b"\x6d")
            payload_counts = {"mem_copy": 0, "mem_fill": 0,
                              "crc32": 0, "vector_add": 0}
            names = ("mem_copy", "mem_fill", "crc32", "vector_add")
            for sample in range(args.payload_samples):
                opcode = (sample & 3) + 1
                if opcode == 1:
                    qtest.write(SOURCE, source)
                elif opcode == 4:
                    qtest.write(SOURCE, source)
                    qtest.write(DESTINATION, destination)
                payload_latencies.append(ring.submit_payload(opcode))
                payload_counts[names[opcode - 1]] += 1
                if opcode == 1 and qtest.read(DESTINATION, PAYLOAD_LENGTH) != source:
                    raise AssertionError("MEM_COPY data mismatch")
                if opcode == 2 and qtest.read(DESTINATION, PAYLOAD_LENGTH) != \
                        bytes([0x6D]) * PAYLOAD_LENGTH:
                    raise AssertionError("MEM_FILL data mismatch")
                if opcode == 4 and qtest.read(DESTINATION, PAYLOAD_LENGTH) != \
                        vector_expected:
                    raise AssertionError("VECTOR_ADD data mismatch")
            qtest.clock_step(args.virtual_hours * 60 * 60 * 1_000_000_000)
            liveness_latency = ring.submit_nops(1)
            completed = args.commands
        except Exception:
            qemu_log.seek(0)
            sys.stderr.write(qemu_log.read().decode("utf-8", errors="replace"))
            raise
        finally:
            qtest.close()
    elapsed_ns = time.perf_counter_ns() - started_ns
    return {
        "schema": "vams-stress-qualification-v1",
        "result": "PASS",
        "environment": {
            "source_revision": command_output(["git", "rev-parse", "HEAD"]),
            "source_dirty": bool(command_output([
                "git", "status", "--porcelain", "--untracked-files=no"
            ])),
            "qemu_version": command_output([executable, "--version"]).splitlines()[0],
            "qemu_sha256": file_sha256(executable),
            "host": platform.platform(),
            "python": platform.python_version(),
        },
        "configuration": {
            "commands": args.commands,
            "payload_samples": args.payload_samples,
            "resets": args.resets,
            "virtual_hours": args.virtual_hours,
            "queue_depth": DEPTH,
            "workload_order": "deterministic",
        },
        "correctness": {
            "completed": completed,
            "completion_failures": 0,
            "stale_or_duplicate_completions": 0,
            "resets_completed": resets_done,
            "ring_wraps": ring.wraps,
            "queue_high_water": ring.high_water,
            "payload_commands": payload_counts,
            "post_endurance_liveness": "PASS",
        },
        "performance": {
            "host_elapsed_seconds": round(elapsed_ns / 1_000_000_000, 6),
            "commands_per_host_second": round(
                args.commands / (elapsed_ns / 1_000_000_000), 3
            ),
            "nop_batch_latency": latency_report(batch_latencies),
            "payload_latency": latency_report(payload_latencies)
            if payload_latencies else None,
            "post_endurance_liveness_us": round(liveness_latency / 1_000, 3),
        },
        "scope": (
            "QEMU host-clock regression evidence; not physical-device "
            "performance"
        ),
    }


def main():
    args = parse_args()
    try:
        executable = executable_path(args.qemu)
        report = run(args, executable)
    except (FileNotFoundError, AssertionError, RuntimeError, ValueError) as error:
        print(f"VAMS stress qualification failed: {error}", file=sys.stderr)
        return 1
    rendered = json.dumps(report, indent=2, sort_keys=True)
    if args.json_output:
        args.json_output.parent.mkdir(parents=True, exist_ok=True)
        args.json_output.write_text(rendered + "\n", encoding="utf-8")
    print(rendered)
    return 0


if __name__ == "__main__":
    sys.exit(main())
