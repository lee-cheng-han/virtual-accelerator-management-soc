#!/usr/bin/env python3
"""Run the noninteractive hardware-free VAMS demonstration."""

import argparse
import datetime
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_pins():
    pins = {}
    for raw_line in (ROOT / "tools" / "versions.env").read_text().splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        key, value = line.split("=", 1)
        if not re.fullmatch(r"[A-Z0-9_]+", key) or not value:
            raise ValueError(f"invalid version pin: {raw_line}")
        pins[key] = value
    return pins


def executable(value):
    candidate = shutil.which(value) if os.path.sep not in value else value
    if not candidate or not os.path.isfile(candidate) or not os.access(candidate, os.X_OK):
        raise FileNotFoundError(value)
    return str(Path(candidate).resolve())


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--qemu-riscv32",
        default=os.environ.get(
            "QEMU_SYSTEM_RISCV32",
            str(ROOT / "build/qemu-src/build-vams/qemu-system-riscv32"),
        ),
    )
    parser.add_argument(
        "--qemu-x86-64",
        default=os.environ.get(
            "QEMU_SYSTEM_X86_64",
            str(ROOT / "build/qemu-src/build-vams/qemu-system-x86_64"),
        ),
    )
    parser.add_argument(
        "--firmware",
        default=os.environ.get(
            "VAMS_ZEPHYR_FIRMWARE",
            str(ROOT / "build/firmware/zephyr/zephyr/zephyr.elf"),
        ),
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(os.environ["VAMS_DEMO_OUTPUT"])
        if os.environ.get("VAMS_DEMO_OUTPUT") else None,
    )
    parser.add_argument("--stress-commands", type=int, default=10_000)
    parser.add_argument("--stress-resets", type=int, default=32)
    return parser.parse_args()


class Demo:
    def __init__(self, args):
        timestamp = datetime.datetime.now(datetime.UTC).strftime("%Y%m%dT%H%M%SZ")
        self.output = (args.output_dir or ROOT / "build" / "demo" / timestamp).resolve()
        self.output.mkdir(parents=True, exist_ok=False)
        self.args = args
        self.pins = load_pins()
        self.stages = []
        self.qemu_riscv32 = ""
        self.qemu_x86_64 = ""
        self.firmware = Path(args.firmware).resolve()

    def preflight(self):
        self.qemu_riscv32 = executable(self.args.qemu_riscv32)
        self.qemu_x86_64 = executable(self.args.qemu_x86_64)
        if not self.firmware.is_file():
            raise FileNotFoundError(self.firmware)
        for path in (self.qemu_riscv32, self.qemu_x86_64):
            version = subprocess.check_output(
                [path, "--version"], text=True, stderr=subprocess.STDOUT
            ).splitlines()[0]
            if self.pins["VAMS_QEMU_VERSION"] not in version or \
                    self.pins["VAMS_QEMU_COMMIT"][:10] not in version:
                raise RuntimeError(f"QEMU does not match pinned revision: {version}")

    def run_stage(self, name, command, extra_env=None, timeout=120):
        log_path = self.output / f"{len(self.stages) + 1:02d}-{name}.log"
        env = os.environ.copy()
        if extra_env:
            env.update(extra_env)
        started = datetime.datetime.now(datetime.UTC)
        with log_path.open("w", encoding="utf-8") as log:
            result = subprocess.run(
                command, cwd=ROOT, env=env, stdout=log,
                stderr=subprocess.STDOUT, text=True, timeout=timeout,
                check=False,
            )
        elapsed = (datetime.datetime.now(datetime.UTC) - started).total_seconds()
        stage = {
            "name": name,
            "command": command,
            "elapsed_seconds": round(elapsed, 3),
            "exit_code": result.returncode,
            "log": log_path.name,
            "result": "PASS" if result.returncode == 0 else "FAIL",
        }
        self.stages.append(stage)
        if result.returncode:
            raise RuntimeError(f"{name} exited with status {result.returncode}")
        print(f"{name}: PASS ({elapsed:.2f}s)")

    def report(self, result, failure=None):
        git_revision = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip()
        report = {
            "schema": "vams-demo-v1",
            "result": result,
            "failure": failure,
            "source": {
                "revision": git_revision,
                "dirty": bool(subprocess.check_output(
                    ["git", "status", "--porcelain"], cwd=ROOT, text=True
                ).strip()),
            },
            "pins": self.pins,
            "artifacts": {
                "qemu_riscv32": {
                    "path": self.qemu_riscv32,
                    "sha256": sha256(Path(self.qemu_riscv32)),
                } if self.qemu_riscv32 else None,
                "qemu_x86_64": {
                    "path": self.qemu_x86_64,
                    "sha256": sha256(Path(self.qemu_x86_64)),
                } if self.qemu_x86_64 else None,
                "firmware": {
                    "path": str(self.firmware),
                    "sha256": sha256(self.firmware),
                } if self.firmware.is_file() else None,
            },
            "host": {
                "platform": platform.platform(),
                "python": platform.python_version(),
            },
            "stages": self.stages,
        }
        (self.output / "report.json").write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    def run(self):
        self.preflight()
        common = {
            "QEMU_SYSTEM_RISCV32": self.qemu_riscv32,
            "QEMU_SYSTEM_X86_64": self.qemu_x86_64,
            "VAMS_ZEPHYR_FIRMWARE": str(self.firmware),
        }
        self.run_stage("source-contract", ["make", "check", "source-check"])
        self.run_stage(
            "firmware-pcie",
            [str(ROOT / "qemu/tests/smoke-vams-firmware-pcie.py")], common,
        )
        self.run_stage(
            "payload-integrity",
            [str(ROOT / "qemu/tests/performance/vams-payload-throughput.py")],
            common,
        )
        self.run_stage(
            "fault-recovery",
            [str(ROOT / "qemu/tests/fault/vams-fault-injection.py")], common,
        )
        self.run_stage(
            "queue-stress",
            [str(ROOT / "qemu/tests/stress/vams-stress-qualification.py"),
             "--commands", str(self.args.stress_commands),
             "--resets", str(self.args.stress_resets),
             "--payload-samples", "16", "--virtual-hours", "1"],
            common,
        )


def main():
    args = parse_args()
    demo = None
    try:
        demo = Demo(args)
        demo.run()
        demo.report("PASS")
    except (FileNotFoundError, ValueError, RuntimeError,
            subprocess.SubprocessError) as error:
        if demo is not None:
            demo.report("FAIL", str(error))
            output = demo.output
        else:
            output = "unavailable"
        print(f"VAMS DEMO: FAIL ({error}; evidence={output})")
        return 1
    print(f"VAMS demo evidence: {demo.output}")
    print("VAMS DEMO: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
