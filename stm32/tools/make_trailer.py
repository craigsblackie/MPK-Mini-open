#!/usr/bin/env python3
"""Build the 16-byte trailer that marks an application image bootable.

Over the air the keyboard writes this itself, as the last step of a
transfer and only after checking the image against the CRC it was
promised -- that ordering is what makes an interrupted update safe.

On the bench, flashing an application over SWD skips that step, so the
resident loader finds no trailer and stays in recovery. This produces
the same 16 bytes to flash alongside it. See include/otamap.h for the
layout and the addresses.
"""
from pathlib import Path
import struct
import sys
import zlib

APP_SLOT_SIZE = 0x6800
APP_MIN_SIZE = 512
MAGIC = b"MPKA"

if len(sys.argv) != 3:
    raise SystemExit(f"usage: {sys.argv[0]} <application.bin> <trailer.bin>")

image = Path(sys.argv[1]).read_bytes()
if not APP_MIN_SIZE <= len(image) <= APP_SLOT_SIZE or len(image) % 2:
    raise SystemExit(
        f"image is {len(image)} bytes; the slot takes an even "
        f"{APP_MIN_SIZE}..{APP_SLOT_SIZE}"
    )

# zlib.crc32 is CRC-32/ISO-HDLC, which is what stm32/src/crc32.c computes
# and what the ESP32 bridge sends at commit.
crc = zlib.crc32(image) & 0xFFFFFFFF
trailer = MAGIC + struct.pack("<III", len(image), crc, (~crc) & 0xFFFFFFFF)
assert len(trailer) == 16

Path(sys.argv[2]).write_bytes(trailer)
print(f"trailer for {len(image)} bytes, crc32 0x{crc:08x}")
