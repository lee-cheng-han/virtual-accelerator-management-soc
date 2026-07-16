#!/usr/bin/env python3
"""Compare the VAMS PCIe SQ/CQ implementation with an independent model."""

import argparse
import os
import random
import select
import shutil
import struct
import subprocess
import sys
import time


BAR0 = 0xFEBF0000
SQ_BASE = 0x100000
CQ_BASE = 0x110000
DEPTH = 16

REG_DEVICE_STATUS = 0x01C
REG_DEVICE_CONTROL = 0x020
REG_ERROR_STATUS = 0x024
REG_RESET_GENERATION = 0x028
REG_SQ_HEAD = 0x10C
REG_SQ_TAIL = 0x110
REG_SQ_DOORBELL = 0x114
REG_SQ_CONTROL = 0x118
REG_SQ_STATUS = 0x11C
REG_CQ_HEAD = 0x20C
REG_CQ_TAIL = 0x210
REG_CQ_DOORBELL = 0x214
REG_CQ_CONTROL = 0x218
REG_CQ_STATUS = 0x21C
REG_INTR_STATUS = 0x300

CONTROL_ENABLE = 1 << 0
CONTROL_QUIESCE = 1 << 2
QUEUE_ENABLE = 1 << 0
QUEUE_RESET = 1 << 1
STATUS_READY = 1 << 0
STATUS_QUEUES_READY = 1 << 2
ERROR_QUEUE = 1 << 5
INTR_CQ = 1 << 0
INTR_ERROR = 1 << 1

SUBMISSION = struct.Struct("<HBBIQQIIQIIQQ")
COMPLETION = struct.Struct("<IHHIIQQ")


class QTest:
    def __init__(self, executable):
        self.process = subprocess.Popen(
            [
                executable,
                "-machine", "q35,accel=qtest",
                "-m", "64M",
                "-display", "none",
                "-nodefaults",
                "-device", "vams-pcie,addr=2",
                "-qtest", "stdio",
                "-qtest-log", os.devnull,
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        self.output = b""

    def command(self, command):
        if self.process.stdin is None or self.process.stdout is None:
            raise RuntimeError("qtest pipes are unavailable")
        self.process.stdin.write((command + "\n").encode("ascii"))
        self.process.stdin.flush()
        deadline = time.monotonic() + 2.0
        while True:
            while b"\n" not in self.output:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise RuntimeError(f"qtest timed out after {command!r}")
                ready, _, _ = select.select(
                    [self.process.stdout], [], [], remaining
                )
                if not ready:
                    raise RuntimeError(f"qtest timed out after {command!r}")
                chunk = os.read(self.process.stdout.fileno(), 4096)
                if not chunk:
                    raise RuntimeError(f"qtest stopped after {command!r}")
                self.output += chunk
            raw_reply, self.output = self.output.split(b"\n", 1)
            reply = raw_reply.decode("ascii").strip()
            if reply.startswith("IRQ "):
                continue
            if not reply.startswith("OK"):
                raise RuntimeError(f"qtest rejected {command!r}: {reply}")
            return reply

    def write32(self, address, value):
        self.command(f"writel 0x{address:x} 0x{value:x}")

    def read32(self, address):
        reply = self.command(f"readl 0x{address:x}")
        return int(reply.split()[1], 16)

    def out32(self, address, value):
        self.command(f"outl 0x{address:x} 0x{value:x}")

    def write(self, address, data):
        self.command(f"write 0x{address:x} {len(data)} 0x{data.hex()}")

    def read(self, address, length):
        reply = self.command(f"read 0x{address:x} {length}")
        return bytes.fromhex(reply.split()[1][2:])

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        for stream in (self.process.stdin, self.process.stdout,
                       self.process.stderr):
            if stream is not None:
                stream.close()


class QueueModel:
    def __init__(self):
        self.sq_head = 0
        self.sq_tail = 0
        self.cq_head = 0
        self.cq_tail = 0
        self.sq_enabled = False
        self.cq_enabled = False
        self.device_control = 0
        self.sq_error = False
        self.cq_error = False
        self.error_status = 0
        self.intr_status = 0
        self.generation = 0
        self.submissions = [None] * DEPTH
        self.completions = [None] * DEPTH
        self.sq_wrapped = False
        self.cq_wrapped = False

    def configure(self):
        self.cq_enabled = True
        self.sq_enabled = True
        self.device_control = CONTROL_ENABLE
        self.process()

    def queue_ready(self):
        return self.sq_enabled and self.cq_enabled

    def cq_full(self):
        return (self.cq_tail + 1) % DEPTH == self.cq_head

    def sq_full(self):
        return (self.sq_tail + 1) % DEPTH == self.sq_head

    def process(self):
        if not (self.device_control & CONTROL_ENABLE):
            return
        if self.device_control & CONTROL_QUIESCE:
            return
        while self.sq_head != self.sq_tail and not self.cq_full():
            descriptor = self.submissions[self.sq_head]
            if descriptor is None:
                raise AssertionError("model consumed an uninitialized SQ slot")
            self.submissions[self.sq_head] = None
            self.sq_head = (self.sq_head + 1) % DEPTH
            if self.sq_head == 0:
                self.sq_wrapped = True
            command_id, cookie, valid = descriptor
            self.completions[self.cq_tail] = (
                command_id,
                0 if valid else 1,
                0 if valid else 1,
                cookie,
            )
            self.cq_tail = (self.cq_tail + 1) % DEPTH
            if self.cq_tail == 0:
                self.cq_wrapped = True
            self.intr_status |= INTR_CQ

    def submit(self, command_id, cookie, valid):
        if self.sq_full():
            return False
        self.submissions[self.sq_tail] = (command_id, cookie, valid)
        self.sq_tail = (self.sq_tail + 1) % DEPTH
        self.process()
        return True

    def consume(self, count):
        produced = (self.cq_tail - self.cq_head) % DEPTH
        if count > produced:
            raise AssertionError("model consumed beyond CQ tail")
        for _ in range(count):
            self.completions[self.cq_head] = None
            self.cq_head = (self.cq_head + 1) % DEPTH
        self.process()

    def set_quiesce(self, quiesced):
        self.device_control = CONTROL_ENABLE
        if quiesced:
            self.device_control |= CONTROL_QUIESCE
        self.process()

    def set_queue(self, submission, enabled):
        if submission:
            self.sq_enabled = enabled
        else:
            self.cq_enabled = enabled
        if not self.queue_ready():
            self.device_control = 0
        self.process()

    def reset_queues(self):
        self.device_control = 0
        self.sq_head = 0
        self.sq_tail = 0
        self.cq_head = 0
        self.cq_tail = 0
        self.sq_enabled = False
        self.cq_enabled = False
        self.sq_error = False
        self.cq_error = False
        self.intr_status &= ~INTR_CQ
        self.generation += 1
        self.submissions = [None] * DEPTH
        self.completions = [None] * DEPTH

    def invalid_sq_doorbell(self):
        self.sq_error = True
        self.error_status |= ERROR_QUEUE
        self.intr_status |= INTR_ERROR

    def invalid_cq_doorbell(self):
        self.cq_error = True
        self.error_status |= ERROR_QUEUE
        self.intr_status |= INTR_ERROR

    def ack_interrupt(self, bits):
        self.intr_status &= ~bits
        if bits & INTR_CQ and self.cq_head != self.cq_tail:
            self.intr_status |= INTR_CQ

    def sq_status(self):
        status = QUEUE_ENABLE if self.sq_enabled else 0
        if self.sq_enabled and self.sq_head == self.sq_tail:
            status |= 1 << 1
        if self.sq_enabled and self.sq_full():
            status |= 1 << 2
        if self.sq_error:
            status |= 1 << 4
        return status

    def cq_status(self):
        status = QUEUE_ENABLE if self.cq_enabled else 0
        if self.cq_enabled and self.cq_head == self.cq_tail:
            status |= 1 << 1
        if self.cq_enabled and self.cq_full():
            status |= 1 << 2
        if self.cq_error:
            status |= 1 << 4
        return status

    def registers(self):
        device_status = STATUS_READY
        if self.queue_ready():
            device_status |= STATUS_QUEUES_READY
        return {
            "device_status": device_status,
            "device_control": self.device_control,
            "error_status": self.error_status,
            "generation": self.generation,
            "sq_head": self.sq_head,
            "sq_tail": self.sq_tail,
            "sq_control": QUEUE_ENABLE if self.sq_enabled else 0,
            "sq_status": self.sq_status(),
            "cq_head": self.cq_head,
            "cq_tail": self.cq_tail,
            "cq_control": QUEUE_ENABLE if self.cq_enabled else 0,
            "cq_status": self.cq_status(),
            "intr_status": self.intr_status,
        }


class ComparisonRun:
    REGISTER_MAP = {
        "device_status": REG_DEVICE_STATUS,
        "device_control": REG_DEVICE_CONTROL,
        "error_status": REG_ERROR_STATUS,
        "generation": REG_RESET_GENERATION,
        "sq_head": REG_SQ_HEAD,
        "sq_tail": REG_SQ_TAIL,
        "sq_control": REG_SQ_CONTROL,
        "sq_status": REG_SQ_STATUS,
        "cq_head": REG_CQ_HEAD,
        "cq_tail": REG_CQ_TAIL,
        "cq_control": REG_CQ_CONTROL,
        "cq_status": REG_CQ_STATUS,
        "intr_status": REG_INTR_STATUS,
    }

    def __init__(self, executable, seed):
        self.qtest = QTest(executable)
        self.model = QueueModel()
        self.random = random.Random(seed)
        self.seed = seed
        self.trace = []
        self.coverage = set()
        self.next_command_id = 1

    def write_reg(self, offset, value):
        self.qtest.write32(BAR0 + offset, value)

    def configure_hardware(self):
        self.qtest.out32(0xCF8, 0x80001010)
        self.qtest.out32(0xCFC, BAR0)
        self.qtest.out32(0xCF8, 0x80001014)
        self.qtest.out32(0xCFC, 0xFEBE0000)
        self.qtest.out32(0xCF8, 0x80001004)
        self.qtest.out32(0xCFC, 0x6)
        self.write_reg(0x100, SQ_BASE)
        self.write_reg(0x104, 0)
        self.write_reg(0x108, DEPTH)
        self.write_reg(0x200, CQ_BASE)
        self.write_reg(0x204, 0)
        self.write_reg(0x208, DEPTH)
        self.write_reg(REG_CQ_CONTROL, QUEUE_ENABLE)
        self.write_reg(REG_SQ_CONTROL, QUEUE_ENABLE)
        self.write_reg(REG_DEVICE_CONTROL, CONTROL_ENABLE)
        self.model.configure()

    def descriptor_bytes(self, command_id, cookie, valid):
        return SUBMISSION.pack(
            1 if valid else 2, 0, 0, command_id,
            0, 0, 0, 0, cookie, 0, 0, 0, 0,
        )

    def submit(self):
        if self.model.sq_full():
            return False
        command_id = self.next_command_id
        self.next_command_id += 1
        cookie = 0x56414D5300000000 | command_id
        valid = self.random.randrange(5) != 0
        slot = self.model.sq_tail
        self.qtest.write(
            SQ_BASE + slot * SUBMISSION.size,
            self.descriptor_bytes(command_id, cookie, valid),
        )
        next_tail = (slot + 1) % DEPTH
        self.write_reg(REG_SQ_DOORBELL, next_tail)
        self.model.submit(command_id, cookie, valid)
        if next_tail == 0:
            self.coverage.add("sq-tail-wrap")
        self.trace.append(f"submit id={command_id} valid={int(valid)}")
        return True

    def consume(self):
        produced = (self.model.cq_tail - self.model.cq_head) % DEPTH
        if not produced:
            return False
        count = self.random.randint(1, produced)
        new_head = (self.model.cq_head + count) % DEPTH
        self.write_reg(REG_CQ_DOORBELL, new_head)
        self.model.consume(count)
        self.trace.append(f"consume count={count}")
        return True

    def toggle_quiesce(self):
        quiesced = not bool(self.model.device_control & CONTROL_QUIESCE)
        value = CONTROL_ENABLE | (CONTROL_QUIESCE if quiesced else 0)
        self.write_reg(REG_DEVICE_CONTROL, value)
        self.model.set_quiesce(quiesced)
        self.coverage.add("quiesce")
        self.trace.append(f"quiesce value={int(quiesced)}")

    def toggle_queue(self):
        submission = bool(self.random.getrandbits(1))
        offset = REG_SQ_CONTROL if submission else REG_CQ_CONTROL
        self.write_reg(offset, 0)
        self.model.set_queue(submission, False)
        self.write_reg(offset, QUEUE_ENABLE)
        self.model.set_queue(submission, True)
        self.write_reg(REG_DEVICE_CONTROL, CONTROL_ENABLE)
        self.model.device_control = CONTROL_ENABLE
        self.model.process()
        self.coverage.add("queue-toggle")
        self.trace.append("toggle sq" if submission else "toggle cq")

    def reset_queues(self):
        self.write_reg(REG_SQ_CONTROL, QUEUE_RESET)
        self.model.reset_queues()
        self.write_reg(REG_CQ_CONTROL, QUEUE_ENABLE)
        self.write_reg(REG_SQ_CONTROL, QUEUE_ENABLE)
        self.write_reg(REG_DEVICE_CONTROL, CONTROL_ENABLE)
        self.model.configure()
        self.coverage.add("queue-reset")
        self.trace.append("queue reset")

    def random_action(self):
        choice = self.random.randrange(100)
        if choice < 52 and self.submit():
            return
        if choice < 70 and self.consume():
            return
        if choice < 76:
            self.toggle_quiesce()
        elif choice < 80:
            self.write_reg(REG_SQ_DOORBELL, DEPTH)
            self.model.invalid_sq_doorbell()
            self.coverage.add("invalid-sq-doorbell")
            self.trace.append("invalid sq doorbell")
        elif choice < 84:
            self.write_reg(REG_CQ_DOORBELL, DEPTH)
            self.model.invalid_cq_doorbell()
            self.coverage.add("invalid-cq-doorbell")
            self.trace.append("invalid cq doorbell")
        elif choice < 88:
            self.write_reg(REG_INTR_STATUS, INTR_CQ)
            self.model.ack_interrupt(INTR_CQ)
            self.trace.append("ack cq interrupt")
        elif choice < 91:
            self.write_reg(REG_INTR_STATUS, INTR_ERROR)
            self.model.ack_interrupt(INTR_ERROR)
            self.trace.append("ack error interrupt")
        elif choice < 94:
            self.write_reg(REG_ERROR_STATUS, 0x3FF)
            self.model.error_status = 0
            self.trace.append("clear errors")
        elif choice < 97:
            self.toggle_queue()
        else:
            self.reset_queues()

    def verify_completions(self):
        slot = self.model.cq_head
        while slot != self.model.cq_tail:
            expected = self.model.completions[slot]
            if expected is None:
                raise AssertionError(f"model CQ slot {slot} has no completion")
            raw = self.qtest.read(CQ_BASE + slot * COMPLETION.size,
                                  COMPLETION.size)
            command_id, status, error, bytes_done, crc, cookie, _ = \
                COMPLETION.unpack(raw)
            actual = (command_id, status, error, cookie)
            if bytes_done or crc or actual != expected:
                raise AssertionError(
                    f"CQ slot {slot}: expected {expected}, got {actual}, "
                    f"bytes={bytes_done} crc={crc}"
                )
            slot = (slot + 1) % DEPTH

    def verify(self):
        if self.model.cq_full():
            self.coverage.add("cq-backpressure")
        if self.model.sq_full():
            self.coverage.add("sq-full")
        if self.model.sq_head != self.model.sq_tail:
            self.coverage.add("deferred-sq")
        if self.model.sq_wrapped:
            self.coverage.add("sq-head-wrap")
        if self.model.cq_wrapped:
            self.coverage.add("cq-tail-wrap")
        expected = self.model.registers()
        for name, offset in self.REGISTER_MAP.items():
            actual = self.qtest.read32(BAR0 + offset)
            if actual != expected[name]:
                raise AssertionError(
                    f"{name}: expected 0x{expected[name]:08x}, "
                    f"got 0x{actual:08x}"
                )
        self.verify_completions()

    def run(self, operations):
        try:
            self.configure_hardware()
            self.trace.append("configure")
            self.verify()
            for _ in range(operations):
                self.random_action()
                self.verify()
            return self.coverage
        except Exception as error:
            print(f"FAIL seed={self.seed} operation={len(self.trace)}: {error}",
                  file=sys.stderr)
            print("Shortest observed failing prefix:", file=sys.stderr)
            for index, operation in enumerate(self.trace, 1):
                print(f"  {index}: {operation}", file=sys.stderr)
            raise
        finally:
            self.qtest.close()


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--qemu",
        default=os.environ.get("QEMU_SYSTEM_X86_64", "qemu-system-x86_64"),
        help="QEMU system executable",
    )
    parser.add_argument("--operations", type=int, default=300)
    parser.add_argument(
        "--seed",
        action="append",
        type=lambda value: int(value, 0),
        help="deterministic seed; may be supplied more than once",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    executable = shutil.which(args.qemu) if os.path.sep not in args.qemu \
        else args.qemu
    if not executable or not os.path.isfile(executable):
        print(f"QEMU executable not found: {args.qemu}", file=sys.stderr)
        return 2
    if args.operations < 1:
        print("--operations must be positive", file=sys.stderr)
        return 2

    seeds = args.seed or [0x56414D53, 0x10203040, 0xC001D00D, 0x5EED1234]
    coverage = set()
    for seed in seeds:
        coverage.update(ComparisonRun(executable, seed).run(args.operations))
        print(f"VAMS queue model: seed=0x{seed:08x} "
              f"operations={args.operations} PASS")
    required = {
        "cq-backpressure", "cq-tail-wrap", "deferred-sq",
        "invalid-cq-doorbell", "invalid-sq-doorbell", "queue-reset",
        "queue-toggle", "quiesce", "sq-full", "sq-head-wrap",
        "sq-tail-wrap",
    }
    if args.seed is None and args.operations == 300:
        missing = required - coverage
        if missing:
            print("coverage requirements not reached: " +
                  ", ".join(sorted(missing)), file=sys.stderr)
            return 1
    print("VAMS queue model coverage: " + ", ".join(sorted(coverage)))
    print(f"VAMS SQ/CQ reference-model comparison: seeds={len(seeds)} PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
