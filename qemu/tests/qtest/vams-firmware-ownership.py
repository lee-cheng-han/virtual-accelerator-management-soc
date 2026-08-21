#!/usr/bin/env python3
"""Validate firmware-owned terminal completion and disconnect recovery."""

import importlib.util
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


TEST_ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "vams_firmware_pcie", TEST_ROOT / "smoke-vams-firmware-pcie.py"
)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ManagementQTest(MODULE.QTest):
    def __init__(self, executable, command_socket, firmware):
        self.process = subprocess.Popen(
            [
                executable,
                "-machine", "vams_riscv",
                "-display", "none",
                "-monitor", "none",
                "-serial", "none",
                "-bios", firmware,
                "-chardev",
                f"socket,id=command,path={command_socket},server=on,wait=off",
                "-global", "vams-mgmt.command-chardev=command",
                "-qtest", "stdio",
                "-qtest-log", os.devnull,
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            bufsize=0,
        )
        self.output = b""


def receive_exact(connection, size):
    data = bytearray()
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            raise RuntimeError("firmware bridge closed during transfer")
        data.extend(chunk)
    return bytes(data)


def wait_for_status(qtest, mask):
    for _ in range(100):
        if qtest.read32(0x10030000) & mask:
            return
    raise RuntimeError(f"portal status bit 0x{mask:x} did not assert")


def check_management_portal(executable, firmware, temp):
    socket_path = os.path.join(temp, "management.sock")
    qtest = ManagementQTest(executable, socket_path, firmware)
    connection = None
    try:
        MODULE.wait_for_path(socket_path, qtest.process)
        connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        connection.settimeout(3)
        deadline = time.monotonic() + 3
        while True:
            try:
                connection.connect(socket_path)
                break
            except ConnectionRefusedError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.01)
        command_id = 0xBEEFF001
        cookie = 0x0123456789ABCDEF
        raw_submission = MODULE.submission(
            1, command_id, cookie, opcode=1, source=0x1000,
            destination=0x2000, length=64,
        )
        connection.sendall(raw_submission)
        wait_for_status(qtest, 1)
        if qtest.read(0x10030100, len(raw_submission)) != raw_submission:
            raise AssertionError("portal submission staging mismatch")
        qtest.write32(0x10030008, 1)

        authorization = MODULE.COMPLETION.pack(
            command_id, 0, 0, 0, 0, cookie, 0
        )
        qtest.write(0x10030200, authorization)
        qtest.write32(0x1003000C, 1)
        if receive_exact(connection, len(authorization)) != authorization:
            raise AssertionError("portal authorization mismatch")

        result = MODULE.COMPLETION.pack(
            command_id, 0, 0, 64, 0, cookie, 1234
        )
        connection.sendall(result)
        wait_for_status(qtest, 1 << 5)
        if qtest.read(0x10030300, len(result)) != result:
            raise AssertionError("portal engine-result staging mismatch")
        if qtest.read32(0x10030020) != 1:
            raise AssertionError("portal result counter did not advance")
        qtest.write32(0x1003001C, 1)

        qtest.write(0x10030200, result)
        qtest.write32(0x1003000C, 1)
        if receive_exact(connection, len(result)) != result:
            raise AssertionError("portal terminal completion mismatch")

        next_id = command_id + 1
        next_cookie = cookie + 1
        next_submission = MODULE.submission(
            1, next_id, next_cookie, opcode=1, source=0x3000,
            destination=0x4000, length=64,
        )
        connection.sendall(next_submission)
        wait_for_status(qtest, 1)
        if qtest.read(0x10030100, len(next_submission)) != next_submission:
            raise AssertionError("portal did not return to submission mode")
        qtest.write32(0x10030008, 1)
        next_authorization = MODULE.COMPLETION.pack(
            next_id, 0, 0, 0, 0, next_cookie, 0
        )
        qtest.write(0x10030200, next_authorization)
        qtest.write32(0x1003000C, 1)
        if receive_exact(connection, MODULE.COMPLETION.size) != \
                next_authorization:
            raise AssertionError("second portal authorization mismatch")

        abort = MODULE.COMPLETION.pack(
            next_id, 3, 19, 0, 0, next_cookie, 2000
        )
        qtest.write(0x10030200, abort)
        qtest.write32(0x10030024, 1)
        if receive_exact(connection, MODULE.COMPLETION.size) != abort:
            raise AssertionError("portal abort request mismatch")
        if qtest.read32(0x10030028) != 1:
            raise AssertionError("portal abort counter did not advance")
        connection.sendall(abort)
        wait_for_status(qtest, 1 << 5)
        if qtest.read(0x10030300, len(abort)) != abort:
            raise AssertionError("portal abort result mismatch")
        if qtest.read32(0x10030020) != 2:
            raise AssertionError("portal result counter mismatch after abort")
        qtest.write32(0x1003001C, 1)
        qtest.write(0x10030200, abort)
        qtest.write32(0x1003000C, 1)
        if receive_exact(connection, MODULE.COMPLETION.size) != abort:
            raise AssertionError("portal abort completion mismatch")

        reset_id = command_id + 2
        reset_cookie = cookie + 2
        reset_submission = MODULE.submission(
            1, reset_id, reset_cookie, opcode=1, source=0x5000,
            destination=0x6000, length=64,
        )
        connection.sendall(reset_submission)
        wait_for_status(qtest, 1)
        qtest.write32(0x10030008, 1)
        reset_authorization = MODULE.COMPLETION.pack(
            reset_id, 0, 0, 0, 0, reset_cookie, 0
        )
        qtest.write(0x10030200, reset_authorization)
        qtest.write32(0x1003000C, 1)
        if receive_exact(connection, MODULE.COMPLETION.size) != \
                reset_authorization:
            raise AssertionError("reset-test authorization mismatch")
        qtest.write32(0x1002000C, 1)
        reset_notice = MODULE.COMPLETION.unpack(
            receive_exact(connection, MODULE.COMPLETION.size)
        )
        if reset_notice[:6] != (reset_id, 5, 21, 0, 0, reset_cookie):
            raise AssertionError(f"reset notification mismatch: {reset_notice}")
        if qtest.read32(0x10020038) != 1 or \
                qtest.read32(0x1002003C) != 0:
            raise AssertionError("reset notification evidence mismatch")
    finally:
        if connection is not None:
            connection.close()
        qtest.close()


def main():
    executable = os.environ.get(
        "QEMU_SYSTEM_X86_64", "qemu-system-x86_64"
    )
    management_executable = os.environ.get(
        "QEMU_SYSTEM_RISCV32", "qemu-system-riscv32"
    )
    firmware = os.environ.get(
        "VAMS_ZEPHYR_FIRMWARE",
        "build/firmware/zephyr/zephyr/zephyr.elf",
    )
    try:
        executable = MODULE.executable_path(executable)
        management_executable = MODULE.executable_path(management_executable)
        if not os.path.isfile(firmware):
            raise FileNotFoundError(firmware)
    except FileNotFoundError as error:
        print(f"QEMU executable not found: {error}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="vams-ownership-") as temp:
        try:
            check_management_portal(management_executable, firmware, temp)
        except (AssertionError, OSError, RuntimeError) as error:
            print(f"management portal QTest failed: {error}", file=sys.stderr)
            return 1
        socket_path = os.path.join(temp, "command.sock")
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(socket_path)
        listener.listen(1)
        qtest = MODULE.QTest(
            executable, socket_path, subprocess.DEVNULL,
            device_options="x-vams-debug=on",
        )
        connection = None
        try:
            connection, _ = listener.accept()
            connection.settimeout(3)
            MODULE.configure_queues(qtest)

            source = 0x120000
            destination = 0x130000
            payload = bytes(range(64))
            command_id = 0xA11CE001
            cookie = 0x1122334455667788
            qtest.write(source, payload)
            qtest.write(destination, bytes(len(payload)))

            errors = []

            def firmware_service():
                try:
                    raw = receive_exact(connection, MODULE.SUBMISSION.size)
                    fields = MODULE.SUBMISSION.unpack(raw)
                    if fields[3] != command_id or fields[8] != cookie:
                        raise AssertionError("submission identity mismatch")
                    authorization = MODULE.COMPLETION.pack(
                        command_id, 0, 0, 0, 0, cookie, 0
                    )
                    connection.sendall(authorization)
                    result = receive_exact(connection, MODULE.COMPLETION.size)
                    result_fields = MODULE.COMPLETION.unpack(result)
                    if result_fields[:6] != (
                        command_id, 0, 0, len(payload), 0, cookie
                    ):
                        raise AssertionError(
                            f"unexpected engine result: {result_fields[:6]}"
                        )
                    connection.sendall(result)
                except Exception as error:  # Propagate thread failures.
                    errors.append(error)

            service = threading.Thread(target=firmware_service)
            service.start()
            qtest.write(
                MODULE.SQ_BASE,
                MODULE.submission(
                    1, command_id, cookie, opcode=1, source=source,
                    destination=destination, length=len(payload),
                ),
            )
            qtest.write32(MODULE.BAR0 + 0x114, 1)
            MODULE.wait_for_completion(qtest, 1)
            service.join(timeout=3)
            if service.is_alive():
                raise RuntimeError("firmware service did not finish")
            if errors:
                raise errors[0]
            MODULE.check_completion(
                qtest, 0, (command_id, 0, 0, len(payload), 0, cookie)
            )
            if qtest.read(destination, len(payload)) != payload:
                raise AssertionError("payload engine did not copy data")
            qtest.write32(MODULE.BAR0 + 0x214, 1)

            reset_id = 0xA11CE002
            reset_cookie = 0x7766554433221100
            errors.clear()

            def reset_service():
                try:
                    receive_exact(connection, MODULE.SUBMISSION.size)
                    connection.sendall(MODULE.COMPLETION.pack(
                        reset_id, 0, 0, 0, 0, reset_cookie, 0
                    ))
                    result = receive_exact(connection, MODULE.COMPLETION.size)
                    fields = MODULE.COMPLETION.unpack(result)
                    if fields[:6] != (
                        reset_id, 5, 21, 0, 0, reset_cookie
                    ):
                        raise AssertionError(
                            f"unexpected reset result: {fields[:6]}"
                        )
                    connection.sendall(result)
                except Exception as error:  # Propagate thread failures.
                    errors.append(error)

            service = threading.Thread(target=reset_service)
            service.start()
            qtest.write32(MODULE.BAR0 + 0xF14, 1)
            qtest.write(
                MODULE.SQ_BASE + MODULE.SUBMISSION.size,
                MODULE.submission(
                    1, reset_id, reset_cookie, opcode=1, source=source,
                    destination=destination, length=len(payload),
                ),
            )
            qtest.write32(MODULE.BAR0 + 0x114, 2)
            MODULE.wait_for_engine_busy(qtest)
            qtest.write32(MODULE.BAR0 + 0x60C, 2)
            MODULE.wait_for_completion(qtest, 2)
            service.join(timeout=3)
            if service.is_alive():
                raise RuntimeError("reset service did not finish")
            if errors:
                raise errors[0]
            MODULE.check_completion(
                qtest, 1, (reset_id, 5, 21, 0, 0, reset_cookie)
            )
            qtest.write32(MODULE.BAR0 + 0x214, 2)

            abort_id = 0xA11CE003
            abort_cookie = 0x66554433221100FF
            errors.clear()

            def abort_service():
                try:
                    receive_exact(connection, MODULE.SUBMISSION.size)
                    connection.sendall(MODULE.COMPLETION.pack(
                        abort_id, 0, 0, 0, 0, abort_cookie, 0
                    ))
                    request = MODULE.COMPLETION.pack(
                        abort_id, 3, 19, 0, 0, abort_cookie, 0
                    )
                    connection.sendall(request)
                    result = receive_exact(connection, MODULE.COMPLETION.size)
                    fields = MODULE.COMPLETION.unpack(result)
                    if fields[:6] != (
                        abort_id, 3, 19, 0, 0, abort_cookie
                    ):
                        raise AssertionError(
                            f"unexpected abort result: {fields[:6]}"
                        )
                    connection.sendall(result)
                except Exception as error:  # Propagate thread failures.
                    errors.append(error)

            service = threading.Thread(target=abort_service)
            service.start()
            qtest.write(
                MODULE.SQ_BASE + 2 * MODULE.SUBMISSION.size,
                MODULE.submission(
                    1, abort_id, abort_cookie, opcode=1, source=source,
                    destination=destination, length=len(payload),
                ),
            )
            qtest.write32(MODULE.BAR0 + 0x114, 3)
            MODULE.wait_for_completion(qtest, 3)
            service.join(timeout=3)
            if service.is_alive():
                raise RuntimeError("abort service did not finish")
            if errors:
                raise errors[0]
            MODULE.check_completion(
                qtest, 2, (abort_id, 3, 19, 0, 0, abort_cookie)
            )
            qtest.write32(MODULE.BAR0 + 0x214, 3)

            management_reset_id = 0xA11CE004
            management_reset_cookie = 0x8877665544332211
            reset_ready = threading.Event()
            errors.clear()

            def management_reset_service():
                try:
                    receive_exact(connection, MODULE.SUBMISSION.size)
                    connection.sendall(MODULE.COMPLETION.pack(
                        management_reset_id, 0, 0, 0, 0,
                        management_reset_cookie, 0
                    ))
                    if not reset_ready.wait(timeout=3):
                        raise RuntimeError("management reset was not released")
                    connection.sendall(MODULE.COMPLETION.pack(
                        management_reset_id, 5, 21, 0, 0,
                        management_reset_cookie, 0
                    ))
                except Exception as error:  # Propagate thread failures.
                    errors.append(error)

            service = threading.Thread(target=management_reset_service)
            service.start()
            qtest.write32(MODULE.BAR0 + 0xF14, 1)
            qtest.write(
                MODULE.SQ_BASE + 3 * MODULE.SUBMISSION.size,
                MODULE.submission(
                    1, management_reset_id, management_reset_cookie, opcode=1,
                    source=source, destination=destination,
                    length=len(payload),
                ),
            )
            qtest.write32(MODULE.BAR0 + 0x114, 4)
            MODULE.wait_for_engine_busy(qtest)
            reset_ready.set()
            MODULE.wait_for_completion(qtest, 4)
            service.join(timeout=3)
            if service.is_alive():
                raise RuntimeError("management reset service did not finish")
            if errors:
                raise errors[0]
            MODULE.check_completion(
                qtest, 3,
                (management_reset_id, 5, 21, 0, 0,
                 management_reset_cookie),
            )
            qtest.write32(MODULE.BAR0 + 0x214, 4)

            lost_id = 0xA11CE005
            lost_cookie = 0x9988776655443322
            qtest.write(
                MODULE.SQ_BASE + 4 * MODULE.SUBMISSION.size,
                MODULE.submission(1, lost_id, lost_cookie),
            )
            qtest.write32(MODULE.BAR0 + 0x114, 5)
            receive_exact(connection, MODULE.SUBMISSION.size)
            connection.close()
            connection = None
            MODULE.wait_for_completion(qtest, 5)
            MODULE.check_completion(
                qtest, 4, (lost_id, 2, 18, 0, 0, lost_cookie)
            )
        except (AssertionError, OSError, RuntimeError) as error:
            print(f"firmware ownership QTest failed: {error}", file=sys.stderr)
            return 1
        finally:
            if connection is not None:
                connection.close()
            listener.close()
            qtest.close()

    print("VAMS firmware-owned portal, cross-reset, abort, and disconnect: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
