#!/usr/bin/env python3
"""Exercise PCI SQ/CQ DMA through the real Zephyr command service."""

import argparse
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


def submission(version, command_id, cookie):
    return SUBMISSION.pack(
        version, 0, 0, command_id, 0, 0, 0, 0, cookie, 0, 0, 0, 0
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

    print("VAMS PCI DMA to Zephyr command bridge: firmware=4 host=3 PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
