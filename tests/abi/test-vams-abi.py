#!/usr/bin/env python3
"""Check schema sizes and exact little-endian descriptor bytes."""

import json
import pathlib
import struct


ROOT = pathlib.Path(__file__).resolve().parents[2]
schema = json.loads((ROOT / "abi" / "vams-v1.json").read_text(encoding="utf-8"))

submission_format = "<HBBIQQIIQIIQQ"
completion_format = "<IHHIIQQ"
assert struct.calcsize(submission_format) == schema["submission_size"]
assert struct.calcsize(completion_format) == schema["completion_size"]

nop = struct.pack(
    submission_format,
    schema["descriptor_version"],
    schema["opcodes"]["NOP"],
    0,
    0x11223344,
    0,
    0,
    0,
    0,
    0x1122334455667788,
    0,
    0,
    0,
    0,
)
expected = bytes.fromhex(
    "0100000044332211"
    "00000000000000000000000000000000"
    "0000000000000000"
    "8877665544332211"
    "000000000000000000000000000000000000000000000000"
)
assert nop == expected

completion = struct.pack(
    completion_format,
    0x11223344,
    schema["completion_status"]["SUCCESS"],
    schema["error_codes"]["NONE"],
    0,
    0,
    0x1122334455667788,
    0x0102030405060708,
)
assert completion.hex() == (
    "44332211000000000000000000000000"
    "88776655443322110807060504030201"
)

print("VAMS ABI raw little-endian layout test: PASS")
