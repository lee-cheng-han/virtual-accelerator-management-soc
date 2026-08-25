#!/usr/bin/env python3
"""Capture and validate VAMS Zephyr resource high-water evidence."""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


RESOURCE_RE = re.compile(
    r"Resources: static_sram=(\d+)/(\d+) pool_high=(\d+)/(\d+) "
    r"validation_high=(\d+)/(\d+) ready_high=(\d+)/(\d+) "
    r"running_high=(\d+)/(\d+) "
    r"completion_high=(\d+)/(\d+) recovery_attempts=(\d+) "
    r"recovery_escalations=(\d+) admission_defers=(\d+) "
    r"heartbeat_drops=(\d+) queue_overloads=(\d+) portal_stalls=(\d+) "
    r"event_drops=(\d+)"
)
STACK_RE = re.compile(
    r"(producer|monitor|mailbox|receiver|validator|scheduler|recovery|completion|health|event)="
    r"(\d+)/(\d+)"
)
WATCHDOG_RE = re.compile(
    r"Watchdog margin: timeout_ms=(\d+) max_pet_interval_ms=(\d+) "
    r"margin_ms=(\d+)"
)


def executable_path(value):
    path = shutil.which(value) if os.path.sep not in value else value
    if not path or not os.path.isfile(path):
        raise FileNotFoundError(value)
    return path


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_pair(name, used, capacity):
    used = int(used)
    capacity = int(capacity)
    if used > capacity:
        raise AssertionError(f"{name} high-water {used} exceeds {capacity}")
    return {"high_water": used, "capacity": capacity,
            "margin": capacity - used}


def capture(qemu, firmware):
    process = subprocess.Popen(
        [qemu, "-M", "vams_riscv", "-display", "none", "-monitor", "none",
         "-serial", "stdio", "-bios", firmware],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        output, _ = process.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            output, _ = process.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            output, _ = process.communicate(timeout=2)
    resources = RESOURCE_RE.search(output)
    watchdog = WATCHDOG_RE.search(output)
    stack_line = next(
        (line for line in output.splitlines() if line.startswith("Stacks: ")),
        "",
    )
    stacks = STACK_RE.findall(stack_line)
    if resources is None or watchdog is None or len(stacks) != 10:
        raise AssertionError("firmware resource transcript is incomplete")

    values = resources.groups()
    static_sram = parse_pair("static SRAM", values[0], values[1])
    if static_sram["margin"] == 0:
        raise AssertionError("firmware image leaves no SRAM margin")
    queues = {
        "command_pool": parse_pair("command pool", values[2], values[3]),
        "validation": parse_pair("validation queue", values[4], values[5]),
        "ready": parse_pair("ready queue", values[6], values[7]),
        "running": parse_pair("running queue", values[8], values[9]),
        "completion": parse_pair("completion queue", values[10], values[11]),
    }
    stack_report = {
        name: parse_pair(f"{name} stack", used, capacity)
        for name, used, capacity in stacks
    }
    exhausted = [name for name, values in stack_report.items()
                 if values["margin"] == 0]
    if exhausted:
        raise AssertionError(f"stacks have no margin: {', '.join(exhausted)}")
    timeout_ms, maximum_ms, margin_ms = map(int, watchdog.groups())
    if maximum_ms + margin_ms != timeout_ms or margin_ms <= 0:
        raise AssertionError("watchdog margin is not positive and consistent")
    return {
        "schema": "vams-firmware-resources-v3",
        "result": "PASS",
        "firmware": {
            "path": firmware,
            "sha256": file_sha256(firmware),
        },
        "static_sram": static_sram,
        "stacks": stack_report,
        "queues": queues,
        "recovery": {
            "attempts": int(values[12]),
            "escalations": int(values[13]),
        },
        "overload": {
            "admission_defers": int(values[14]),
            "heartbeat_drops": int(values[15]),
            "queue_overloads": int(values[16]),
            "portal_stalls": int(values[17]),
            "event_drops": int(values[18]),
        },
        "watchdog": {
            "timeout_ms": timeout_ms,
            "max_pet_interval_ms": maximum_ms,
            "margin_ms": margin_ms,
        },
        "scope": "QEMU vams_riscv runtime high-water evidence",
    }


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--qemu",
        default=os.environ.get("QEMU_SYSTEM_RISCV32", "qemu-system-riscv32"),
    )
    parser.add_argument(
        "--firmware",
        default=os.environ.get(
            "VAMS_ZEPHYR_FIRMWARE",
            "build/firmware/zephyr/zephyr/zephyr.elf",
        ),
    )
    parser.add_argument("--json-output", type=Path)
    return parser.parse_args()


def main():
    args = parse_args()
    try:
        qemu = executable_path(args.qemu)
        if not os.path.isfile(args.firmware):
            raise FileNotFoundError(args.firmware)
        report = capture(qemu, args.firmware)
    except (FileNotFoundError, AssertionError, RuntimeError) as error:
        print(f"VAMS firmware resource qualification failed: {error}",
              file=sys.stderr)
        return 1
    rendered = json.dumps(report, indent=2, sort_keys=True)
    if args.json_output:
        args.json_output.parent.mkdir(parents=True, exist_ok=True)
        args.json_output.write_text(rendered + "\n", encoding="utf-8")
    print(rendered)
    return 0


if __name__ == "__main__":
    sys.exit(main())
