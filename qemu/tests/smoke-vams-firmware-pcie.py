#!/usr/bin/env python3
"""Exercise PCI SQ/CQ DMA through the real Zephyr command service."""

import argparse
import binascii
import os
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import time


BAR0 = 0xFEBF0000
SQ_BASE = 0x100000
CQ_BASE = 0x110000
PAYLOAD_SOURCE = 0x120003
PAYLOAD_DESTINATION = 0x130005
PAYLOAD_LENGTH = 4097
FILL_SOURCE = 0x121001
FILL_DESTINATION = 0x140007
FILL_LENGTH = 4111
CRC_SOURCE = 0x150009
CRC_LENGTH = 4123
SUBMISSION = struct.Struct("<HBBIQQIIQIIQQ")
COMPLETION = struct.Struct("<IHHIIQQ")


class QTest:
    def __init__(self, executable, command_socket, stderr):
        self.process = subprocess.Popen(
            [
                executable,
                "-machine", "q35,accel=qtest",
                "-m", "64M",
                "-display", "none",
                "-nodefaults",
                "-chardev",
                f"socket,id=command,path={command_socket},server=off",
                "-device", "vams-pcie,addr=2,command-chardev=command",
                "-qtest", "stdio",
                "-qtest-log", os.devnull,
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=stderr,
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
        return int(self.command(f"readl 0x{address:x}").split()[1], 16)

    def out32(self, address, value):
        self.command(f"outl 0x{address:x} 0x{value:x}")

    def write(self, address, data):
        self.command(f"write 0x{address:x} {len(data)} 0x{data.hex()}")

    def read(self, address, length):
        reply = self.command(f"read 0x{address:x} {length}")
        return bytes.fromhex(reply.split()[1][2:])

    def close(self):
        stop_process(self.process)
        for stream in (self.process.stdin, self.process.stdout):
            if stream is not None:
                stream.close()


def stop_process(process):
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)


def executable_path(value):
    path = shutil.which(value) if os.path.sep not in value else value
    if not path or not os.path.isfile(path):
        raise FileNotFoundError(value)
    return path


def wait_for_path(path, process):
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        if os.path.exists(path):
            return
        if process.poll() is not None:
            raise RuntimeError("management QEMU exited before bridge startup")
        time.sleep(0.01)
    raise RuntimeError("management command bridge did not start")


def configure_queues(qtest):
    qtest.out32(0xCF8, 0x80001010)
    qtest.out32(0xCFC, BAR0)
    qtest.out32(0xCF8, 0x80001014)
    qtest.out32(0xCFC, 0xFEBE0000)
    qtest.out32(0xCF8, 0x80001004)
    qtest.out32(0xCFC, 0x6)
    for offset, value in (
        (0x100, SQ_BASE), (0x104, 0), (0x108, 16),
        (0x200, CQ_BASE), (0x204, 0), (0x208, 16),
        (0x218, 1), (0x118, 1), (0x020, 1),
    ):
        qtest.write32(BAR0 + offset, value)


def submission(version, command_id, cookie, opcode=0, source=0,
               destination=0, length=0, flags=0, expected_crc=0):
    return SUBMISSION.pack(
        version, opcode, flags, command_id, source, destination, length, 0,
        cookie, expected_crc, 0, 0, 0
    )


def wait_for_completion(qtest, expected_tail):
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        if qtest.read32(BAR0 + 0x210) == expected_tail:
            return
        time.sleep(0.01)
    raise RuntimeError(f"completion tail did not reach {expected_tail}")


def check_completion(qtest, slot, expected):
    raw = qtest.read(CQ_BASE + slot * COMPLETION.size, COMPLETION.size)
    command_id, status, error, bytes_done, crc, cookie, _ = \
        COMPLETION.unpack(raw)
    actual = (command_id, status, error, bytes_done, crc, cookie)
    if actual != expected:
        raise AssertionError(f"completion {slot}: expected {expected}, got {actual}")


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--qemu-riscv32",
        default=os.environ.get("QEMU_SYSTEM_RISCV32", "qemu-system-riscv32"),
    )
    parser.add_argument(
        "--qemu-x86_64",
        default=os.environ.get("QEMU_SYSTEM_X86_64", "qemu-system-x86_64"),
    )
    parser.add_argument(
        "--firmware",
        default=os.environ.get(
            "VAMS_ZEPHYR_FIRMWARE",
            "build/firmware/zephyr/zephyr/zephyr.elf",
        ),
    )
    return parser.parse_args()


def main():
    args = parse_args()
    try:
        riscv_qemu = executable_path(args.qemu_riscv32)
        x86_qemu = executable_path(args.qemu_x86_64)
    except FileNotFoundError as error:
        print(f"QEMU executable not found: {error}", file=sys.stderr)
        return 2
    if not os.path.isfile(args.firmware):
        print(f"Zephyr firmware not found: {args.firmware}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="vams-firmware-pcie-") as temp:
        command_socket = os.path.join(temp, "command.sock")
        firmware_log_path = os.path.join(temp, "firmware.log")
        pcie_log_path = os.path.join(temp, "pcie.log")
        with open(firmware_log_path, "wb") as firmware_log, \
                open(pcie_log_path, "wb") as pcie_log:
            management = subprocess.Popen(
                [
                    riscv_qemu,
                    "-machine", "vams_riscv",
                    "-display", "none",
                    "-monitor", "none",
                    "-serial", "stdio",
                    "-bios", args.firmware,
                    "-chardev",
                    f"socket,id=command,path={command_socket},server=on,wait=off",
                    "-global", "vams-mgmt.command-chardev=command",
                ],
                stdin=subprocess.DEVNULL,
                stdout=firmware_log,
                stderr=subprocess.STDOUT,
            )
            qtest = None
            try:
                wait_for_path(command_socket, management)
                qtest = QTest(x86_qemu, command_socket, pcie_log)
                configure_queues(qtest)

                first_cookie = 0x1122334455667788
                qtest.write(SQ_BASE, submission(1, 0x10203040, first_cookie))
                qtest.write32(BAR0 + 0x114, 1)
                wait_for_completion(qtest, 1)
                check_completion(
                    qtest, 0, (0x10203040, 0, 0, 0, 0, first_cookie)
                )
                qtest.write32(BAR0 + 0x214, 1)

                second_cookie = 0x8877665544332211
                qtest.write(
                    SQ_BASE + SUBMISSION.size,
                    submission(2, 0x50607080, second_cookie),
                )
                qtest.write32(BAR0 + 0x114, 2)
                wait_for_completion(qtest, 2)
                check_completion(
                    qtest, 1, (0x50607080, 1, 1, 0, 0, second_cookie)
                )
                qtest.write32(BAR0 + 0x214, 2)

                stale_cookie = 0xAABBCCDDEEFF0011
                qtest.write(
                    SQ_BASE + 2 * SUBMISSION.size,
                    submission(1, 0x90A0B0C0, stale_cookie),
                )
                qtest.write32(BAR0 + 0x114, 3)
                qtest.write32(BAR0 + 0x118, 2)
                time.sleep(0.1)
                if qtest.read32(BAR0 + 0x210) != 0:
                    raise AssertionError("pre-reset completion reached new CQ")

                qtest.write32(BAR0 + 0x218, 1)
                qtest.write32(BAR0 + 0x118, 1)
                qtest.write32(BAR0 + 0x020, 1)
                clean_cookie = 0x0F1E2D3C4B5A6978
                qtest.write(
                    SQ_BASE, submission(1, 0xD0E0F000, clean_cookie)
                )
                qtest.write32(BAR0 + 0x114, 1)
                wait_for_completion(qtest, 1)
                check_completion(
                    qtest, 0, (0xD0E0F000, 0, 0, 0, 0, clean_cookie)
                )
                qtest.write32(BAR0 + 0x214, 1)

                payload = bytes(
                    ((index * 37) + 11) & 0xFF
                    for index in range(PAYLOAD_LENGTH)
                )
                guard = bytes([0xA5]) * 16
                qtest.write(PAYLOAD_SOURCE, payload)
                qtest.write(
                    PAYLOAD_DESTINATION - len(guard),
                    guard + bytes(PAYLOAD_LENGTH) + guard,
                )
                copy_cookie = 0x13579BDF2468ACE0
                qtest.write(
                    SQ_BASE + SUBMISSION.size,
                    submission(
                        1, 0xC0DEC001, copy_cookie, opcode=1,
                        source=PAYLOAD_SOURCE,
                        destination=PAYLOAD_DESTINATION,
                        length=PAYLOAD_LENGTH,
                    ),
                )
                qtest.write32(BAR0 + 0x114, 2)
                wait_for_completion(qtest, 2)
                check_completion(
                    qtest, 1,
                    (0xC0DEC001, 0, 0, PAYLOAD_LENGTH, 0, copy_cookie),
                )
                copied = qtest.read(PAYLOAD_DESTINATION, PAYLOAD_LENGTH)
                before = qtest.read(PAYLOAD_DESTINATION - len(guard),
                                    len(guard))
                after = qtest.read(PAYLOAD_DESTINATION + PAYLOAD_LENGTH,
                                   len(guard))
                if copied != payload or before != guard or after != guard:
                    raise AssertionError("MEM_COPY data or guard mismatch")
                qtest.write32(BAR0 + 0x214, 2)

                overlap_cookie = 0x1029384756ABCDEF
                qtest.write(
                    SQ_BASE + 2 * SUBMISSION.size,
                    submission(
                        1, 0xC0DEC002, overlap_cookie, opcode=1,
                        source=PAYLOAD_SOURCE,
                        destination=PAYLOAD_SOURCE + 8,
                        length=64,
                    ),
                )
                qtest.write32(BAR0 + 0x114, 3)
                wait_for_completion(qtest, 3)
                check_completion(
                    qtest, 2, (0xC0DEC002, 1, 9, 0, 0, overlap_cookie)
                )
                qtest.write32(BAR0 + 0x214, 3)

                invalid_copies = (
                    (3, 4, 0xC0DEC003, 0x3000000000000003,
                     PAYLOAD_SOURCE, PAYLOAD_DESTINATION, 0, 6),
                    (4, 5, 0xC0DEC004, 0x4000000000000004,
                     (1 << 64) - 32, PAYLOAD_DESTINATION, 64, 8),
                    (5, 6, 0xC0DEC005, 0x5000000000000005,
                     0, PAYLOAD_DESTINATION, 64, 9),
                )
                for slot, tail, command_id, cookie, source, destination, \
                        length, error in invalid_copies:
                    qtest.write(
                        SQ_BASE + slot * SUBMISSION.size,
                        submission(
                            1, command_id, cookie, opcode=1,
                            source=source, destination=destination,
                            length=length,
                        ),
                    )
                    qtest.write32(BAR0 + 0x114, tail)
                    wait_for_completion(qtest, tail)
                    check_completion(
                        qtest, slot, (command_id, 1, error, 0, 0, cookie)
                    )
                    qtest.write32(BAR0 + 0x214, tail)

                dma_failures = (
                    (6, 7, 0xC0DEC006, 0x6000000000000006,
                     0x40000000, PAYLOAD_DESTINATION, 16),
                    (7, 8, 0xC0DEC007, 0x7000000000000007,
                     PAYLOAD_SOURCE, 0x40000000, 17),
                )
                for slot, tail, command_id, cookie, source, destination, \
                        error in dma_failures:
                    qtest.write(
                        SQ_BASE + slot * SUBMISSION.size,
                        submission(
                            1, command_id, cookie, opcode=1,
                            source=source, destination=destination, length=64,
                        ),
                    )
                    qtest.write32(BAR0 + 0x114, tail)
                    wait_for_completion(qtest, tail)
                    check_completion(
                        qtest, slot, (command_id, 2, error, 0, 0, cookie)
                    )
                    qtest.write32(BAR0 + 0x214, tail)

                fill_value = 0x6D
                fill_guard = bytes([0x3C]) * 16
                qtest.write(FILL_SOURCE, bytes([fill_value]))
                qtest.write(
                    FILL_DESTINATION - len(fill_guard),
                    fill_guard + bytes(FILL_LENGTH) + fill_guard,
                )
                fill_cookie = 0x8A7B6C5D4E3F2011
                qtest.write(
                    SQ_BASE + 8 * SUBMISSION.size,
                    submission(
                        1, 0xF1110001, fill_cookie, opcode=2,
                        source=FILL_SOURCE, destination=FILL_DESTINATION,
                        length=FILL_LENGTH,
                    ),
                )
                qtest.write32(BAR0 + 0x114, 9)
                wait_for_completion(qtest, 9)
                check_completion(
                    qtest, 8,
                    (0xF1110001, 0, 0, FILL_LENGTH, 0, fill_cookie),
                )
                filled = qtest.read(FILL_DESTINATION, FILL_LENGTH)
                before = qtest.read(
                    FILL_DESTINATION - len(fill_guard), len(fill_guard)
                )
                after = qtest.read(
                    FILL_DESTINATION + FILL_LENGTH, len(fill_guard)
                )
                if (filled != bytes([fill_value]) * FILL_LENGTH or
                        before != fill_guard or after != fill_guard):
                    raise AssertionError("MEM_FILL data or guard mismatch")
                qtest.write32(BAR0 + 0x214, 9)

                invalid_fills = (
                    (9, 10, 0xF1110002, 0x9000000000000009,
                     FILL_SOURCE, FILL_DESTINATION, 0, 6),
                    (10, 11, 0xF1110003, 0xA00000000000000A,
                     FILL_SOURCE, (1 << 64) - 32, 64, 8),
                    (11, 12, 0xF1110004, 0xB00000000000000B,
                     0, FILL_DESTINATION, 64, 9),
                )
                for slot, tail, command_id, cookie, source, destination, \
                        length, error in invalid_fills:
                    qtest.write(
                        SQ_BASE + slot * SUBMISSION.size,
                        submission(
                            1, command_id, cookie, opcode=2,
                            source=source, destination=destination,
                            length=length,
                        ),
                    )
                    qtest.write32(BAR0 + 0x114, tail)
                    wait_for_completion(qtest, tail)
                    check_completion(
                        qtest, slot, (command_id, 1, error, 0, 0, cookie)
                    )
                    qtest.write32(BAR0 + 0x214, tail)

                fill_dma_failures = (
                    (12, 13, 0xF1110005, 0xC00000000000000C,
                     0x40000000, FILL_DESTINATION, 16),
                    (13, 14, 0xF1110006, 0xD00000000000000D,
                     FILL_SOURCE, 0x40000000, 17),
                )
                for slot, tail, command_id, cookie, source, destination, \
                        error in fill_dma_failures:
                    qtest.write(
                        SQ_BASE + slot * SUBMISSION.size,
                        submission(
                            1, command_id, cookie, opcode=2,
                            source=source, destination=destination, length=64,
                        ),
                    )
                    qtest.write32(BAR0 + 0x114, tail)
                    wait_for_completion(qtest, tail)
                    check_completion(
                        qtest, slot, (command_id, 2, error, 0, 0, cookie)
                    )
                    qtest.write32(BAR0 + 0x214, tail)

                crc_payload = bytes(
                    ((index * 73) + 19) & 0xFF
                    for index in range(CRC_LENGTH)
                )
                expected_crc = binascii.crc32(crc_payload) & 0xFFFFFFFF
                qtest.write(CRC_SOURCE, crc_payload)

                crc_cases = (
                    (14, 15, 0xCC320001, 0xE00000000000000E,
                     0, 0, 0, CRC_LENGTH, expected_crc),
                    (15, 0, 0xCC320002, 0xF00000000000000F,
                     1, expected_crc, 0, CRC_LENGTH, expected_crc),
                    (0, 1, 0xCC320003, 0x0100000000000010,
                     1, expected_crc ^ 0xFFFFFFFF, 20, 0, expected_crc),
                )
                for slot, tail, command_id, cookie, flags, check_crc, \
                        error, bytes_done, result_crc in crc_cases:
                    qtest.write(
                        SQ_BASE + slot * SUBMISSION.size,
                        submission(
                            1, command_id, cookie, opcode=3,
                            source=CRC_SOURCE, length=CRC_LENGTH, flags=flags,
                            expected_crc=check_crc,
                        ),
                    )
                    qtest.write32(BAR0 + 0x114, tail)
                    wait_for_completion(qtest, tail)
                    check_completion(
                        qtest, slot,
                        (command_id, 2 if error else 0, error,
                         bytes_done, result_crc, cookie),
                    )
                    qtest.write32(BAR0 + 0x214, tail)

                invalid_crcs = (
                    (1, 2, 0xCC320004, 0x0200000000000011,
                     CRC_SOURCE, 0, CRC_LENGTH, 2, 0, 3),
                    (2, 3, 0xCC320005, 0x0300000000000012,
                     CRC_SOURCE, 0, CRC_LENGTH, 0, expected_crc, 4),
                    (3, 4, 0xCC320006, 0x0400000000000013,
                     CRC_SOURCE, FILL_DESTINATION, CRC_LENGTH, 0, 0, 9),
                    (4, 5, 0xCC320007, 0x0500000000000014,
                     CRC_SOURCE, 0, 0, 0, 0, 6),
                    (5, 6, 0xCC320008, 0x0600000000000015,
                     (1 << 64) - 32, 0, 64, 0, 0, 8),
                )
                for slot, tail, command_id, cookie, source, destination, \
                        length, flags, check_crc, error in invalid_crcs:
                    qtest.write(
                        SQ_BASE + slot * SUBMISSION.size,
                        submission(
                            1, command_id, cookie, opcode=3, source=source,
                            destination=destination, length=length,
                            flags=flags, expected_crc=check_crc,
                        ),
                    )
                    qtest.write32(BAR0 + 0x114, tail)
                    wait_for_completion(qtest, tail)
                    check_completion(
                        qtest, slot, (command_id, 1, error, 0, 0, cookie)
                    )
                    qtest.write32(BAR0 + 0x214, tail)

                crc_dma_cookie = 0x0700000000000016
                qtest.write(
                    SQ_BASE + 6 * SUBMISSION.size,
                    submission(
                        1, 0xCC320009, crc_dma_cookie, opcode=3,
                        source=0x40000000, length=64,
                    ),
                )
                qtest.write32(BAR0 + 0x114, 7)
                wait_for_completion(qtest, 7)
                check_completion(
                    qtest, 6, (0xCC320009, 2, 16, 0, 0, crc_dma_cookie)
                )
                qtest.write32(BAR0 + 0x214, 7)
            except Exception as error:
                print(f"firmware PCI bridge test failed: {error}", file=sys.stderr)
                return_code = 1
            else:
                return_code = 0
            finally:
                if qtest is not None:
                    qtest.close()
                stop_process(management)

        with open(firmware_log_path, "r", encoding="utf-8") as stream:
            firmware_output = stream.read()
        if return_code == 0:
            required = (
                "Command: id=0x10203040 status=0 error=0 "
                "cookie=0x1122334455667788",
                "Command: id=0x50607080 status=1 error=1 "
                "cookie=0x8877665544332211",
                "Command: id=0x90a0b0c0 status=0 error=0 "
                "cookie=0xaabbccddeeff0011",
                "Command: id=0xd0e0f000 status=0 error=0 "
                "cookie=0x0f1e2d3c4b5a6978",
                "Command: id=0xc0dec001 status=0 error=0 "
                "cookie=0x13579bdf2468ace0",
                "Command: id=0xc0dec002 status=1 error=9 "
                "cookie=0x1029384756abcdef",
                "Command: id=0xc0dec003 status=1 error=6 "
                "cookie=0x3000000000000003",
                "Command: id=0xc0dec004 status=1 error=8 "
                "cookie=0x4000000000000004",
                "Command: id=0xc0dec005 status=1 error=9 "
                "cookie=0x5000000000000005",
                "Command: id=0xc0dec006 status=0 error=0 "
                "cookie=0x6000000000000006",
                "Command: id=0xc0dec007 status=0 error=0 "
                "cookie=0x7000000000000007",
                "Command: id=0xf1110001 status=0 error=0 "
                "cookie=0x8a7b6c5d4e3f2011",
                "Command: id=0xf1110002 status=1 error=6 "
                "cookie=0x9000000000000009",
                "Command: id=0xf1110003 status=1 error=8 "
                "cookie=0xa00000000000000a",
                "Command: id=0xf1110004 status=1 error=9 "
                "cookie=0xb00000000000000b",
                "Command: id=0xf1110005 status=0 error=0 "
                "cookie=0xc00000000000000c",
                "Command: id=0xf1110006 status=0 error=0 "
                "cookie=0xd00000000000000d",
                "Command: id=0xcc320001 status=0 error=0 "
                "cookie=0xe00000000000000e",
                "Command: id=0xcc320002 status=0 error=0 "
                "cookie=0xf00000000000000f",
                "Command: id=0xcc320003 status=0 error=0 "
                "cookie=0x0100000000000010",
                "Command: id=0xcc320004 status=1 error=3 "
                "cookie=0x0200000000000011",
                "Command: id=0xcc320005 status=1 error=4 "
                "cookie=0x0300000000000012",
                "Command: id=0xcc320006 status=1 error=9 "
                "cookie=0x0400000000000013",
                "Command: id=0xcc320007 status=1 error=6 "
                "cookie=0x0500000000000014",
                "Command: id=0xcc320008 status=1 error=8 "
                "cookie=0x0600000000000015",
                "Command: id=0xcc320009 status=0 error=0 "
                "cookie=0x0700000000000016",
            )
            if not all(line in firmware_output for line in required):
                print(firmware_output, file=sys.stderr)
                print("firmware did not report both bridged commands",
                      file=sys.stderr)
                return_code = 1
        if return_code != 0:
            with open(pcie_log_path, "r", encoding="utf-8") as stream:
                print(stream.read(), file=sys.stderr)
            print(firmware_output, file=sys.stderr)
            return return_code

    print("VAMS firmware-owned MEM_COPY/MEM_FILL/CRC32: "
          "data=PASS validation=PASS DMA-errors=PASS")
    print("VAMS PCI DMA to Zephyr command bridge: firmware=26 host=25 PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
