#!/usr/bin/env python3
"""Validate pinned inputs, compatibility data, and checked-in evidence."""

import hashlib
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def load_pins():
    pins = {}
    for raw_line in (ROOT / "tools/versions.env").read_text().splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        key, value = line.split("=", 1)
        if not re.fullmatch(r"VAMS_[A-Z0-9_]+", key) or not value:
            raise AssertionError(f"invalid pin: {raw_line}")
        pins[key] = value
    return pins


def main():
    pins = load_pins()
    patch_manifest = "".join(
        f"{hashlib.sha256(path.read_bytes()).hexdigest()}  "
        f"{path.relative_to(ROOT)}\n"
        for path in sorted((ROOT / "qemu" / "patches").glob("*.patch"))
    ).encode("ascii")
    if hashlib.sha256(patch_manifest).hexdigest() != \
            pins["VAMS_QEMU_PATCH_SHA256"]:
        raise AssertionError("QEMU patch-series hash does not match its pin")
    for name in ("VAMS_QEMU_COMMIT", "VAMS_ZEPHYR_COMMIT",
                 "VAMS_QEMU_PATCH_SHA256", "VAMS_QEMU_SOURCE_SHA256"):
        if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", pins[name]):
            raise AssertionError(f"invalid hexadecimal pin: {name}")
    compatibility = json.loads(
        (ROOT / "tools/compatibility.json").read_text(encoding="utf-8")
    )
    platform_data = compatibility["virtual_platform"]
    expected = {
        "qemu_commit": pins["VAMS_QEMU_COMMIT"],
        "qemu_version": pins["VAMS_QEMU_VERSION"],
        "zephyr_commit": pins["VAMS_ZEPHYR_COMMIT"],
        "zephyr_version": pins["VAMS_ZEPHYR_VERSION"],
    }
    if platform_data != expected:
        raise AssertionError("compatibility matrix and version pins disagree")
    supported = compatibility["interfaces"]
    if len(supported) != 1 or supported[0]["status"] != "supported":
        raise AssertionError("release interface support is ambiguous")
    if any(supported[0][field] != 1 for field in (
            "descriptor_major", "firmware_major",
            "hardware_interface_major", "linux_uapi")):
        raise AssertionError("release interface major must be 1")

    evidence = json.loads(
        (ROOT / "docs/evidence/stress-qualification.json").read_text(
            encoding="utf-8"
        )
    )
    correctness = evidence["correctness"]
    configuration = evidence["configuration"]
    if evidence["result"] != "PASS" or configuration["commands"] < 1_000_000:
        raise AssertionError("stress evidence does not meet the command gate")
    if configuration["resets"] < 1_000 or \
            correctness["stale_or_duplicate_completions"] != 0 or \
            correctness["completion_failures"] != 0:
        raise AssertionError("stress evidence does not meet recovery gates")
    if correctness["post_endurance_liveness"] != "PASS":
        raise AssertionError("stress evidence lacks post-endurance liveness")
    print("VAMS release inputs and evidence: PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, KeyError, ValueError, json.JSONDecodeError) as error:
        print(f"VAMS release input check failed: {error}", file=sys.stderr)
        sys.exit(1)
