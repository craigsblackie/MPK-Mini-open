#!/usr/bin/env python3
"""Sanity-check an application image against what the resident loader accepts.

The loader (src/loader.c) refuses to branch into a slot whose first two
vectors do not look like a stack pointer and a Thumb entry point, and the
receiver refuses a length outside the slot. Checking the same things here
means a bad build is caught at the desk rather than by a keyboard that
quietly comes up in recovery.
"""
from pathlib import Path
import struct
import sys

APP_SLOT_BASE = 0x08008000
APP_SLOT_SIZE = 0x6800
APP_MIN_SIZE = 512
RAM_BASE = 0x20000000
RAM_SIZE = 20 * 1024

image = Path(sys.argv[1]).read_bytes()

if len(image) % 2:
    raise SystemExit(f"image is {len(image)} bytes, which is not a whole number of halfwords")
if not APP_MIN_SIZE <= len(image) <= APP_SLOT_SIZE:
    raise SystemExit(
        f"image is {len(image)} bytes, outside the {APP_MIN_SIZE}..{APP_SLOT_SIZE} the slot accepts"
    )

stack, entry = struct.unpack_from("<II", image)
if not RAM_BASE <= stack <= RAM_BASE + RAM_SIZE:
    raise SystemExit(f"initial stack pointer 0x{stack:08x} is not in SRAM")
if not APP_SLOT_BASE <= entry < APP_SLOT_BASE + len(image):
    raise SystemExit(f"reset vector 0x{entry:08x} is not inside the image")
if not entry & 1:
    raise SystemExit(f"reset vector 0x{entry:08x} has no Thumb bit")

free = APP_SLOT_SIZE - len(image)
print(f"application image: {len(image)} bytes, {free} free in the slot ({100 * len(image) // APP_SLOT_SIZE}% used)")
