#!/usr/bin/env python3
"""Patch the checksum expected by the retained stock MPK mini updater."""
from pathlib import Path
import sys

APP_SIZE = 0x5800

source = Path(sys.argv[1]).read_bytes()
if len(source) != APP_SIZE:
    raise SystemExit(f"application image is {len(source)} bytes, expected {APP_SIZE}")

image = bytearray(source)
checksum = (-sum(image[:-2])) & 0xFFFF
image[-2] = checksum & 0xFF
image[-1] = checksum >> 8
Path(sys.argv[2]).write_bytes(image)

if (sum(image[:-2]) + int.from_bytes(image[-2:], "little")) & 0xFFFF:
    raise SystemExit("internal checksum verification failed")
print(f"stock updater checksum: 0x{checksum:04x}")
