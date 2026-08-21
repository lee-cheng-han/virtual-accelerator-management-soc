#!/usr/bin/env python3
"""Verify deterministic VAMS PCI fault injection and clean recovery."""

import argparse
import importlib.util
import os
import shutil
import struct
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

BAR0 = SMOKE.BAR0
BAR1 = 0xFEBE0000
FAULT_CONTROL = BAR0 + 0xF00
FAULT_ARG = BAR0 + 0xF04
FAULT_STATUS = BAR0 + 0xF08
FAULT_COUNT = BAR0 + 0xF0C
DEBUG_LOCK = BAR0 + 0xF10
CHECKPOINT_CONTROL = BAR0 + 0xF14
CHECKPOINT_STATUS = BAR0 + 0xF18
CHECKPOINT_RELEASE = BAR0 + 0xF1C
ENGINE_STATUS = BAR0 + 0x500
RESET_REQUEST = BAR0 + 0x60C
LAST_RESET_REASON = BAR0 + 0x610
SOURCE = 0x200000
DESTINATION = 0x210000
LENGTH = 64
COMPLETION = struct.Struct("<IHHIIQQ")

DMA_TIMEOUT = 1 << 0
DROP_CQ_INTERRUPT = 1 << 1
ENGINE_HANG = 1 << 2
DMA_READ = 1 << 3
DMA_WRITE = 1 << 4
RESET_ACTIVE = 1 << 5
ALL_FAULTS = (1 << 6) - 1


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
        self.sequence = 0

    def submit_async(self, descriptor):
        slot = self.sq_tail
        next_tail = (slot + 1) & 15
        self.qtest.write(
            SMOKE.SQ_BASE + slot * SMOKE.SUBMISSION.size, descriptor
        )
        self.qtest.write32(BAR0 + 0x114, next_tail)
        self.sq_tail = next_tail
        return next_tail

    def finish(self, expected_tail):
        SMOKE.wait_for_completion(self.qtest, expected_tail)
        raw = self.qtest.read(
            SMOKE.CQ_BASE + self.cq_head * COMPLETION.size, COMPLETION.size
        )
        completion = COMPLETION.unpack(raw)
        self.cq_head = (self.cq_head + 1) & 15
        self.qtest.write32(BAR0 + 0x214, self.cq_head)
        self.qtest.write32(BAR0 + 0x300, 1)
        return completion

    def submit(self, descriptor):
        return self.finish(self.submit_async(descriptor))

    def descriptor(self, opcode=0, timeout_ms=0):
        self.sequence += 1
        return SMOKE.submission(
            1, 0xFA170000 + self.sequence,
            0xFA17000000000000 + self.sequence,
            opcode=opcode,
            source=SOURCE if opcode else 0,
            destination=DESTINATION if opcode else 0,
            length=LENGTH if opcode else 0,
            timeout_ms=timeout_ms,
        )

    def clean_nop(self):
        completion = self.submit(self.descriptor())
        expected_id = 0xFA170000 + self.sequence
        expected_cookie = 0xFA17000000000000 + self.sequence
        expect_completion(
            completion, expected_id, 0, 0, 0, expected_cookie
        )

    def reset_indices(self):
        self.sq_tail = 0
        self.cq_head = 0


def expect_completion(completion, command_id, status, error, bytes_done,
                      cookie):
    expected = (command_id, status, error, bytes_done, 0, cookie)
    if completion[:6] != expected:
        raise AssertionError(
            f"completion mismatch: expected {expected}, got {completion[:6]}"
        )


def arm(qtest, fault, argument=0):
    qtest.write32(FAULT_ARG, argument)
    qtest.write32(FAULT_CONTROL, fault)
    if qtest.read32(FAULT_CONTROL) != fault:
        raise AssertionError(f"fault 0x{fault:x} did not arm")


def expect_fault_state(qtest, status, count):
    actual = (
        qtest.read32(FAULT_CONTROL),
        qtest.read32(FAULT_STATUS),
        qtest.read32(FAULT_COUNT),
    )
    expected = (0, status, count)
    if actual != expected:
        raise AssertionError(
            f"fault state mismatch: expected {expected}, got {actual}"
        )


def prepare_payload(qtest, destination_value=0xA5):
    source = bytes(((index * 13) + 7) & 0xFF for index in range(LENGTH))
    destination = bytes([destination_value]) * LENGTH
    qtest.write(SOURCE, source)
    qtest.write(DESTINATION, destination)
    return source, destination


def verify_debug_gate(executable):
    with tempfile.TemporaryFile() as log:
        qtest = SMOKE.QTest(executable, None, log)
        try:
            SMOKE.configure_queues(qtest)
            if qtest.read32(BAR0 + 0x010) != 0xB3:
                raise AssertionError("production capabilities changed")
            if qtest.read32(FAULT_CONTROL) != 0xFFFFFFFF:
                raise AssertionError("fault block visible without debug property")
            if not (qtest.read32(BAR0 + 0x024) & 1):
                raise AssertionError("disabled fault access lacked MMIO error")
        finally:
            qtest.close()


def run_faults(executable):
    status = 0
    count = 0
    with tempfile.TemporaryFile() as log:
        qtest = SMOKE.QTest(
            executable, None, log, "x-vams-debug=true"
        )
        try:
            SMOKE.configure_queues(qtest)
            ring = CommandRing(qtest)
            if qtest.read32(BAR0 + 0x010) != 0xF3:
                raise AssertionError("debug capability was not advertised")

            qtest.write32(FAULT_CONTROL, DMA_TIMEOUT | ENGINE_HANG)
            if qtest.read32(FAULT_CONTROL) != 0 or \
                    not (qtest.read32(BAR0 + 0x024) & (1 << 3)):
                raise AssertionError("multi-fault arm was not rejected")
            qtest.write32(BAR0 + 0x024, 0x3FF)

            _, untouched = prepare_payload(qtest)
            arm(qtest, DMA_TIMEOUT)
            completion = ring.submit(ring.descriptor(opcode=1, timeout_ms=25))
            expect_completion(
                completion, 0xFA170001, 3, 19, 0, 0xFA17000000000001
            )
            if qtest.read(DESTINATION, LENGTH) != untouched:
                raise AssertionError("timeout fault modified payload")
            status |= DMA_TIMEOUT
            count += 1
            expect_fault_state(qtest, status, count)
            ring.clean_nop()

            qtest.out32(0xCF8, 0x80001040)
            qtest.out32(0xCFC, 0x80018011)
            qtest.write32(BAR0 + 0x300, 0xF)
            qtest.write32(BAR0 + 0x304, 0xE)
            arm(qtest, DROP_CQ_INTERRUPT)
            completion = ring.submit(ring.descriptor())
            expect_completion(
                completion, 0xFA170003, 0, 0, 0, 0xFA17000000000003
            )
            if qtest.read32(BAR1 + 0x800) != 0:
                raise AssertionError("dropped CQ notification reached MSI-X PBA")
            status |= DROP_CQ_INTERRUPT
            count += 1
            expect_fault_state(qtest, status, count)
            ring.clean_nop()
            if not (qtest.read32(BAR1 + 0x800) & 1):
                raise AssertionError("clean CQ notification did not reach MSI-X PBA")
            qtest.write32(BAR1 + 0x00C, 0)
            qtest.write32(BAR1 + 0x00C, 1)

            _, untouched = prepare_payload(qtest, 0xB6)
            arm(qtest, ENGINE_HANG)
            tail = ring.submit_async(ring.descriptor(opcode=1))
            if qtest.read32(ENGINE_STATUS) != 0xD:
                raise AssertionError("engine hang did not assert BUSY/HUNG/ERROR")
            qtest.clock_step(100_000_000)
            if qtest.read32(BAR0 + 0x210) == tail:
                raise AssertionError("hung engine completed without recovery")
            qtest.write32(RESET_REQUEST, 1 << 1)
            completion = ring.finish(tail)
            expect_completion(
                completion, 0xFA170005, 5, 21, 0,
                0xFA17000000000005,
            )
            if qtest.read(DESTINATION, LENGTH) != untouched:
                raise AssertionError("engine-hang fault modified payload")
            status |= ENGINE_HANG
            count += 1
            expect_fault_state(qtest, status, count)
            ring.clean_nop()

            source, untouched = prepare_payload(qtest, 0xC7)
            arm(qtest, DMA_READ, argument=2)
            completion = ring.submit(ring.descriptor(opcode=1))
            expect_completion(
                completion, 0xFA170007, 0, 0, LENGTH,
                0xFA17000000000007,
            )
            if qtest.read(DESTINATION, LENGTH) != source:
                raise AssertionError("first read before Nth fault failed")
            if qtest.read32(FAULT_CONTROL) != DMA_READ or \
                    qtest.read32(FAULT_COUNT) != count:
                raise AssertionError("Nth read fault triggered too early")
            _, untouched = prepare_payload(qtest, 0xC7)
            completion = ring.submit(ring.descriptor(opcode=1))
            expect_completion(
                completion, 0xFA170008, 2, 16, 0,
                0xFA17000000000008,
            )
            if qtest.read(DESTINATION, LENGTH) != untouched:
                raise AssertionError("read fault modified destination")
            status |= DMA_READ
            count += 1
            expect_fault_state(qtest, status, count)
            ring.clean_nop()

            _, untouched = prepare_payload(qtest, 0xD8)
            arm(qtest, DMA_WRITE)
            completion = ring.submit(ring.descriptor(opcode=1))
            expect_completion(
                completion, 0xFA17000A, 2, 17, 0,
                0xFA1700000000000A,
            )
            if qtest.read(DESTINATION, LENGTH) != untouched:
                raise AssertionError("write fault modified destination")
            status |= DMA_WRITE
            count += 1
            expect_fault_state(qtest, status, count)
            ring.clean_nop()

            _, untouched = prepare_payload(qtest, 0xE9)
            generation = qtest.read32(BAR0 + 0x028)
            arm(qtest, RESET_ACTIVE)
            ring.submit_async(ring.descriptor(opcode=1))
            if qtest.read32(BAR0 + 0x028) != (generation + 1) & 0xFFFFFFFF:
                raise AssertionError("active-transfer reset missed generation")
            if qtest.read32(LAST_RESET_REASON) != 2:
                raise AssertionError("active-transfer reset reason mismatch")
            if qtest.read32(BAR0 + 0x118) or qtest.read32(BAR0 + 0x218):
                raise AssertionError("active-transfer reset left queues enabled")
            qtest.clock_step(100_000_000)
            if qtest.read(DESTINATION, LENGTH) != untouched:
                raise AssertionError("active-transfer reset modified payload")
            status |= RESET_ACTIVE
            count += 1
            expect_fault_state(qtest, status, count)
            ring.reset_indices()
            SMOKE.configure_queues(qtest)
            ring.clean_nop()

            source, untouched = prepare_payload(qtest, 0x9A)
            qtest.write32(CHECKPOINT_CONTROL, 1)
            tail = ring.submit_async(ring.descriptor(opcode=1))
            if qtest.read32(CHECKPOINT_STATUS) != 1 or \
                    not (qtest.read32(ENGINE_STATUS) & 1):
                raise AssertionError("engine-start checkpoint did not pause")
            qtest.clock_step(100_000_000)
            if qtest.read32(BAR0 + 0x210) == tail or \
                    qtest.read(DESTINATION, LENGTH) != untouched:
                raise AssertionError("engine-start checkpoint leaked progress")
            qtest.write32(CHECKPOINT_RELEASE, 1)
            completion = ring.finish(tail)
            expect_completion(
                completion, 0xFA17000E, 0, 0, LENGTH,
                0xFA1700000000000E,
            )
            if qtest.read(DESTINATION, LENGTH) != source:
                raise AssertionError("released engine-start work was incorrect")

            source, _ = prepare_payload(qtest, 0xAB)
            qtest.write32(CHECKPOINT_CONTROL, 2)
            tail = ring.submit_async(ring.descriptor(opcode=1))
            for _ in range(20):
                if qtest.read32(CHECKPOINT_STATUS) == 2:
                    break
                qtest.clock_step(2_000_000)
            else:
                raise AssertionError("CQ-publication checkpoint did not pause")
            if qtest.read32(BAR0 + 0x210) == tail:
                raise AssertionError("CQ tail advanced through checkpoint")
            if qtest.read(DESTINATION, LENGTH) != source:
                raise AssertionError("payload was incomplete at CQ checkpoint")
            qtest.write32(CHECKPOINT_RELEASE, 1)
            completion = ring.finish(tail)
            expect_completion(
                completion, 0xFA17000F, 0, 0, LENGTH,
                0xFA1700000000000F,
            )
            ring.clean_nop()

            qtest.write32(DEBUG_LOCK, 1)
            qtest.write32(FAULT_ARG, 0)
            qtest.write32(FAULT_CONTROL, DMA_TIMEOUT)
            qtest.write32(CHECKPOINT_CONTROL, 1)
            qtest.write32(BAR0 + 0x308, 1)
            if qtest.read32(DEBUG_LOCK) != 1 or \
                    qtest.read32(FAULT_CONTROL) != 0 or \
                    qtest.read32(CHECKPOINT_CONTROL) != 0 or \
                    not (qtest.read32(BAR0 + 0x024) & (1 << 3)):
                raise AssertionError("debug lock did not reject control writes")
            qtest.write32(BAR0 + 0x020, 1 << 1)
            qtest.clock_step(1_000_000)
            if qtest.read32(DEBUG_LOCK) != 1 or \
                    qtest.read32(FAULT_STATUS) != status or \
                    qtest.read32(FAULT_COUNT) != count:
                raise AssertionError("device reset lost debug evidence or lock")
        except Exception:
            log.seek(0)
            sys.stderr.write(log.read().decode("utf-8", errors="replace"))
            raise
        finally:
            qtest.close()


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
        verify_debug_gate(executable)
        run_faults(executable)
    except Exception as error:
        print(f"fault injection smoke failed: {error}", file=sys.stderr)
        return 1
    print(
        "VAMS deterministic faults: timeout=PASS drop-irq=PASS hang=PASS "
        "dma-read=PASS dma-write=PASS reset-active=PASS"
    )
    print(
        "VAMS fault recovery: one-shot=PASS nth-match=PASS post-fault=PASS "
        "checkpoints=PASS debug-lock=PASS evidence-persistence=PASS"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
