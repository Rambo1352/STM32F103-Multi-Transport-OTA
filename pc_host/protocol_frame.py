#!/usr/bin/env python3
"""Small helper for building and checking upgrade protocol frames."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


CMD_QUERY_INFO = 0x0001
CMD_ENTER = 0x0002
CMD_BEGIN = 0x0003
CMD_DATA = 0x0004
CMD_END = 0x0005


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


def crc32_file(path: Path) -> int:
    import zlib

    crc = 0
    with path.open("rb") as f:
        while True:
            chunk = f.read(4096)
            if not chunk:
                break
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF


def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    body = struct.pack("<HH", cmd, len(payload)) + payload + struct.pack("<I", 0)
    return body + struct.pack("<H", crc16_modbus(body))


def build_begin(path: Path, version: int, slot: int) -> bytes:
    payload = struct.pack("<IIIB", path.stat().st_size, crc32_file(path), version, slot)
    return build_frame(CMD_BEGIN, payload)


def build_data(offset: int, payload: bytes) -> bytes:
    return build_frame(CMD_DATA, struct.pack("<IH", offset, len(payload)) + payload)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("bin", type=Path, help="app binary path")
    parser.add_argument("--version", type=lambda value: int(value, 0), default=0x00010000)
    parser.add_argument("--slot", type=int, default=0, help="0 auto, 1 slot A, 2 slot B")
    parser.add_argument("--packet-size", type=int, default=512)
    parser.add_argument("--out", type=Path, default=Path("upgrade_frames.bin"))
    args = parser.parse_args()

    if args.packet_size <= 0 or (args.packet_size % 2) != 0:
        raise SystemExit("--packet-size must be a positive even number")

    image = args.bin.read_bytes()
    frames = bytearray()
    frames += build_frame(CMD_ENTER)
    frames += build_begin(args.bin, args.version, args.slot)

    for offset in range(0, len(image), args.packet_size):
        frames += build_data(offset, image[offset : offset + args.packet_size])

    frames += build_frame(CMD_END)
    args.out.write_bytes(frames)

    print(f"image size: {len(image)}")
    print(f"image crc32: 0x{crc32_file(args.bin):08X}")
    print(f"frames file: {args.out}")


if __name__ == "__main__":
    main()
