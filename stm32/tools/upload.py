#!/usr/bin/env python3
"""Upload or download an application image over USB MIDI.

The web editor is the ordinary way to do this. This is the path that does
not need the bridge: it talks the same 'f' SysEx command (see
include/otaproto.h) straight down the keyboard's own USB MIDI port, which
is useful on a bench, when the ESP32 is not fitted, and when the bridge
itself is what is being worked on.

  ./upload.py /dev/snd/midiC3D0 write build/mpk-mini-open.bin
  ./upload.py /dev/snd/midiC3D0 read  backup.bin
  ./upload.py /dev/snd/midiC3D0 query
"""
import glob
import os
import re
import select
import struct
import sys
import time
import zlib

CMD = 0x66  # 'f'
SUB_QUERY, SUB_BEGIN, SUB_DATA, SUB_COMMIT, SUB_ABORT, SUB_READ = range(6)
CHUNK = 112
STATUS = {
    0: "ok", 1: "out of sequence", 2: "out of range", 3: "flash error",
    4: "checksum mismatch", 5: "bad encoding", 6: "wrong chunk offset",
    7: "restarting into recovery",
}
REBOOTING = 7


def find_port(preferred=None):
    """Locate the keyboard's raw MIDI node, which moves when it re-enumerates."""
    if preferred and os.path.exists(preferred):
        return preferred
    try:
        cards = open("/proc/asound/cards").read()
    except OSError:
        cards = ""
    for index in re.findall(r"^\s*(\d+)\s+\[.*MPK", cards, re.M | re.I):
        node = f"/dev/snd/midiC{index}D0"
        if os.path.exists(node):
            return node
    nodes = sorted(glob.glob("/dev/snd/midiC*D0"))
    return nodes[0] if nodes else None


def digits(value, count):
    return bytes((value >> (7 * i)) & 0x7F for i in reversed(range(count)))


def undigits(data):
    value = 0
    for b in data:
        value = (value << 7) | (b & 0x7F)
    return value


def encode7(raw):
    """7-in-8 packing: one MSB byte then up to seven low-7-bit bytes."""
    out = bytearray()
    for i in range(0, len(raw), 7):
        group = raw[i:i + 7]
        out.append(sum((1 << k) for k, b in enumerate(group) if b & 0x80))
        out.extend(b & 0x7F for b in group)
    return bytes(out)


def decode7(wire):
    out = bytearray()
    i = 0
    while i < len(wire):
        group = wire[i + 1:i + 8]
        msbs = wire[i]
        out.extend(b | (0x80 if msbs & (1 << k) else 0) for k, b in enumerate(group))
        i += len(group) + 1
    return bytes(out)


class Link:
    def __init__(self, path):
        self.path = path
        self.fd = os.open(path, os.O_RDWR)

    def reopen(self, timeout=20.0):
        """Reconnect after the keyboard resets and re-enumerates.

        The node can reappear before the device behind it is ready, and
        it does not always come back with the same card number, so this
        re-resolves the path each time and proves the result by reading
        the port's own descriptor rather than trusting the open.
        """
        try:
            os.close(self.fd)
        except OSError:
            pass
        deadline = time.time() + timeout
        while time.time() < deadline:
            time.sleep(0.5)
            node = find_port(None)
            if node is None:
                continue
            try:
                fd = os.open(node, os.O_RDWR)
                os.write(fd, b"\xfe")  # active sensing: harmless, proves it is alive
            except OSError:
                continue
            self.fd = fd
            self.path = node
            return True
        return False

    def exchange(self, sub, payload=b"", timeout=3.0):
        """One request and its reply.

        The device node can go away underneath this: the keyboard resets
        after a commit and after handing a transfer to recovery, and the
        node is briefly present but dead while it re-enumerates. Reads
        and writes then fail with ENODEV, which is a reconnect to be
        waited out rather than an error to report.
        """
        length = 1 + len(payload)
        message = bytes([0xF0, 0x47, 0x00, 0x7C, CMD,
                         (length >> 7) & 0x7F, length & 0x7F, sub]) + payload + b"\xf7"
        try:
            os.write(self.fd, message)
        except OSError:
            if not self.reopen():
                raise SystemExit("the keyboard went away and did not come back")
            os.write(self.fd, message)

        reply = bytearray()
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not select.select([self.fd], [], [], deadline - time.time())[0]:
                break
            try:
                chunk = os.read(self.fd, 256)
            except OSError:
                if not self.reopen():
                    raise SystemExit("the keyboard went away and did not come back")
                break  # the request went to a device that is no longer there
            for b in chunk:
                if b == 0xF0:
                    reply = bytearray([b])
                elif reply:
                    reply.append(b)
                    if b == 0xF7:
                        # Ignore anything that is not an answer to this command.
                        if len(reply) >= 9 and reply[4] == CMD and reply[7] == sub:
                            return reply[8], bytes(reply[9:-1])
                        reply = bytearray()
        raise SystemExit(f"no reply to sub-command {sub} within {timeout}s")

    def check(self, sub, payload=b"", timeout=3.0, attempts=3):
        """Exchange, retrying a message that gets no answer at all.

        A lost message is not the same as a refused one. The USB MIDI
        endpoint can be left briefly unable to receive, and anything the
        host was delivering at that moment is discarded; the keyboard
        recovers on its own but the request is gone. Since every
        sub-command here is either idempotent or resynchronises from the
        offset the keyboard echoes back, simply asking again is correct.
        """
        for attempt in range(attempts):
            try:
                status, extra = self.exchange(sub, payload, timeout)
            except SystemExit:
                if attempt + 1 == attempts:
                    raise
                continue
            if status != 0:
                raise SystemExit(f"keyboard refused: {STATUS.get(status, status)}")
            return extra
        raise SystemExit("unreachable")


def query(link):
    extra = link.check(SUB_QUERY)
    info = {
        "protocol": extra[0],
        "mode": "recovery" if extra[1] else "application",
        "version": f"{extra[2]}.{extra[3]}.{extra[4]}",
        "slot": undigits(extra[5:8]),
        "chunk": undigits(extra[8:10]),
    }
    if len(extra) >= 18:
        info["installed"] = undigits(extra[10:13])
        info["crc32"] = undigits(extra[13:18])
    return info


def write_image(link, path):
    image = open(path, "rb").read()
    info = query(link)
    if len(image) % 2 or not 512 <= len(image) <= info["slot"]:
        raise SystemExit(f"{path} is {len(image)} bytes; the slot takes an even 512..{info['slot']}")
    stack, entry = struct.unpack_from("<II", image)
    if not 0x20000000 <= stack <= 0x20005000:
        raise SystemExit("that file does not start with a keyboard vector table")

    print(f"{info['mode']} v{info['version']}; sending {len(image)} bytes")

    status, _ = link.exchange(SUB_BEGIN, digits(len(image), 4))
    if status == REBOOTING:
        # The application runs from the slot an update erases, so it
        # cannot do the transfer. It gives up its own bootability and
        # restarts into recovery, which can. Follow it there.
        print("  keyboard is restarting into recovery to accept the update…")
        if not link.reopen():
            raise SystemExit("the keyboard did not come back after restarting")
        for _ in range(20):
            try:
                if query(link)["mode"] == "recovery":
                    break
            except SystemExit:
                pass
            time.sleep(0.5)
        else:
            raise SystemExit("the keyboard did not come back up in recovery")
        status, _ = link.exchange(SUB_BEGIN, digits(len(image), 4))
    if status != 0:
        raise SystemExit(f"keyboard refused: {STATUS.get(status, status)}")
    for offset in range(0, len(image), CHUNK):
        part = image[offset:offset + CHUNK]
        try:
            extra = link.check(SUB_DATA, digits(offset, 4) + encode7(part))
            received = undigits(extra[:4])
        except SystemExit:
            # A retry of a chunk the keyboard already wrote is refused as
            # out of sequence, and the refusal carries how far it got --
            # which is how the sender finds its place again.
            status, extra = link.exchange(SUB_DATA, digits(offset, 4) + encode7(part))
            received = undigits(extra[:4]) if len(extra) >= 4 else 0
            if status not in (0, 6) or received != offset + len(part):
                raise SystemExit(f"lost the transfer at offset {offset}")
        if received != offset + len(part):
            raise SystemExit("the keyboard disagrees about how much arrived")
        print(f"\r  {offset + len(part)}/{len(image)}", end="", flush=True)
    print()
    link.check(SUB_COMMIT, digits(zlib.crc32(image) & 0xFFFFFFFF, 5), timeout=6.0)
    print("committed; the keyboard is restarting")


def read_image(link, path):
    info = query(link)
    installed = info.get("installed", 0)
    if not installed:
        raise SystemExit("no application installed to download")

    out = bytearray()
    while len(out) < installed:
        want = min(CHUNK, installed - len(out))
        extra = link.check(SUB_READ, digits(len(out), 4) + digits(want, 2))
        if undigits(extra[:4]) != len(out):
            raise SystemExit("the keyboard sent the wrong part of the image")
        out.extend(decode7(extra[4:]))
        print(f"\r  {len(out)}/{installed}", end="", flush=True)
    print()
    if zlib.crc32(bytes(out)) & 0xFFFFFFFF != info["crc32"]:
        raise SystemExit("downloaded image did not match the keyboard's checksum")
    open(path, "wb").write(bytes(out))
    print(f"wrote {len(out)} bytes to {path}, checksum verified")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    port = sys.argv[1] if sys.argv[1] != "auto" else find_port()
    if port is None:
        raise SystemExit("no MPK mini raw MIDI device found")
    link = Link(port)
    action = sys.argv[2]
    if action == "query":
        for key, value in query(link).items():
            print(f"  {key}: {value}")
    elif action == "write":
        write_image(link, sys.argv[3])
    elif action == "read":
        read_image(link, sys.argv[3])
    else:
        raise SystemExit(__doc__)
