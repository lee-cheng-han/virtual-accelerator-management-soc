#!/usr/bin/env python3
"""Verify firmware queued timeout and clean post-timeout scheduling."""

import argparse
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
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
            "VAMS_SCHEDULER_FIRMWARE",
            "build/firmware/zephyr-scheduler/zephyr/zephyr.elf",
        ),
    )
    return parser.parse_args()


def require_transitions(output, command_id, transitions):
    command = f"command=0x{command_id:08x}"
    lines = [line for line in output.splitlines() if command in line]
    for source, destination in transitions:
        marker = f"from={source} to={destination}"
        if not any(marker in line for line in lines):
            raise AssertionError(
                f"command 0x{command_id:08x} missing transition {marker}"
            )
    published = [
        line for line in lines
        if "event=published" in line and "count=1" in line
    ]
    if len(published) != 1:
        raise AssertionError(
            f"command 0x{command_id:08x} publication count is {len(published)}"
        )


def main():
    args = parse_args()
    try:
        riscv_qemu = executable_path(args.qemu_riscv32)
        x86_qemu = executable_path(args.qemu_x86_64)
    except FileNotFoundError as error:
        print(f"QEMU executable not found: {error}", file=sys.stderr)
        return 2
    if not os.path.isfile(args.firmware):
        print(f"scheduler firmware not found: {args.firmware}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="vams-scheduler-") as temp:
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
            return_code = 0
            try:
                SMOKE.wait_for_path(command_socket, management)
                qtest = SMOKE.QTest(x86_qemu, command_socket, pcie_log)
                SMOKE.configure_queues(qtest)

                timeout_id = 0x5CED0001
                timeout_cookie = 0x5CED000000000001
                qtest.write(
                    SMOKE.SQ_BASE,
                    SMOKE.submission(
                        1, timeout_id, timeout_cookie, timeout_ms=1
                    ),
                )
                qtest.write32(SMOKE.BAR0 + 0x114, 1)
                SMOKE.wait_for_completion(qtest, 1)
                SMOKE.check_completion(
                    qtest, 0,
                    (timeout_id, 3, 19, 0, 0, timeout_cookie),
                )
                qtest.write32(SMOKE.BAR0 + 0x214, 1)

                recovery_id = 0x5CED0002
                recovery_cookie = 0x5CED000000000002
                qtest.write(
                    SMOKE.SQ_BASE + SMOKE.SUBMISSION.size,
                    SMOKE.submission(1, recovery_id, recovery_cookie),
                )
                qtest.write32(SMOKE.BAR0 + 0x114, 2)
                SMOKE.wait_for_completion(qtest, 2)
                SMOKE.check_completion(
                    qtest, 1,
                    (recovery_id, 0, 0, 0, 0, recovery_cookie),
                )
                qtest.write32(SMOKE.BAR0 + 0x214, 2)
            except Exception as error:
                print(f"scheduler recovery smoke failed: {error}", file=sys.stderr)
                return_code = 1
            finally:
                if qtest is not None:
                    qtest.close()
                SMOKE.stop_process(management)

        firmware_output = Path(firmware_log_path).read_text(
            encoding="utf-8", errors="replace"
        )
        if return_code == 0:
            try:
                require_transitions(
                    firmware_output, timeout_id,
                    ((0, 1), (1, 2), (2, 3), (3, 5), (5, 7), (7, 0)),
                )
                require_transitions(
                    firmware_output, recovery_id,
                    ((0, 1), (1, 2), (2, 3), (3, 4), (4, 6), (6, 0)),
                )
            except Exception as error:
                print(f"scheduler trace validation failed: {error}",
                      file=sys.stderr)
                return_code = 1
        if return_code != 0:
            print(re.sub(r"(?:Telemetry|Heartbeat):[^\r\n]*(?:\r?\n|$)",
                         "", firmware_output), file=sys.stderr)
            print(Path(pcie_log_path).read_text(
                encoding="utf-8", errors="replace"), file=sys.stderr)
            return return_code

    print("VAMS firmware scheduler: queued-timeout=PASS exactly-once=PASS "
          "recovery=PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
