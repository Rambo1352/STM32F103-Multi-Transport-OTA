#!/usr/bin/env python3
"""Send app.bin to the board over UART using the upgrade protocol."""

from __future__ import annotations

import argparse
import struct
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from protocol_frame import (
    CMD_BEGIN,
    CMD_DATA,
    CMD_END,
    CMD_ENTER,
    build_begin,
    build_data,
    build_frame,
    crc16_modbus,
    crc32_file,
)

CMD_ACK = 0x8000
STATUS_TEXT = {
    0: "OK",
    1: "CRC_ERROR",
    2: "INVALID_CMD",
    3: "INVALID_STATE",
    4: "FLASH_ERROR",
    5: "VERIFY_FAILED",
    6: "UNSUPPORTED",
}


def read_exact(ser, size: int, timeout_s: float) -> bytes:
    deadline = time.monotonic() + timeout_s
    data = bytearray()
    while len(data) < size and time.monotonic() < deadline:
        chunk = ser.read(size - len(data))
        if chunk:
            data += chunk
    if len(data) != size:
        raise TimeoutError(f"timeout while reading {size} bytes, got {len(data)}")
    return bytes(data)


def read_ack(ser, timeout_s: float) -> tuple[int, int, int]:
    header = read_exact(ser, 4, timeout_s)
    cmd, length = struct.unpack("<HH", header)
    payload_and_tail = read_exact(ser, length + 6, timeout_s)
    frame = header + payload_and_tail

    if cmd != CMD_ACK:
        raise RuntimeError(f"unexpected response cmd 0x{cmd:04X}")

    expected_crc = struct.unpack("<H", frame[-2:])[0]
    actual_crc = crc16_modbus(frame[:-2])
    if actual_crc != expected_crc:
        raise RuntimeError(f"bad ack crc, actual 0x{actual_crc:04X}, expected 0x{expected_crc:04X}")

    request_cmd, status, detail = struct.unpack("<HHI", payload_and_tail[:8])
    return request_cmd, status, detail


def send_frame(ser, frame: bytes, request_cmd: int, retries: int, timeout_s: float) -> int:
    last_error: Exception | None = None
    for _ in range(retries + 1):
        ser.reset_input_buffer()
        ser.write(frame)
        ser.flush()
        try:
            ack_cmd, status, detail = read_ack(ser, timeout_s)
            if ack_cmd != request_cmd:
                raise RuntimeError(f"ack for 0x{ack_cmd:04X}, expected 0x{request_cmd:04X}")
            if status != 0:
                name = STATUS_TEXT.get(status, f"STATUS_{status}")
                raise RuntimeError(f"device returned {name}, detail={detail}")
            return detail
        except Exception as exc:
            last_error = exc
            time.sleep(0.05)
    raise RuntimeError(f"frame 0x{request_cmd:04X} failed after retries: {last_error}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("bin", type=Path, help="app .bin file")
    parser.add_argument("--port", required=True, help="serial port, for example COM3")
    parser.add_argument("--baudrate", type=int, default=9600)
    parser.add_argument("--packet-size", type=int, default=512)
    parser.add_argument("--version", type=lambda value: int(value, 0), default=0x00010000)
    parser.add_argument("--slot", type=int, default=0, help="0 auto, 1 slot A, 2 slot B")
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--retries", type=int, default=3)
    args = parser.parse_args()

    if args.packet_size <= 0 or (args.packet_size % 2) != 0:
        print("--packet-size must be a positive even number", file=sys.stderr)
        return 2

    try:
        import serial
    except ImportError:
        print("pyserial is required: python -m pip install pyserial", file=sys.stderr)
        return 2

    image = args.bin.read_bytes()
    image_crc = crc32_file(args.bin)
    print(f"file: {args.bin}")
    print(f"size: {len(image)} bytes")
    print(f"crc32: 0x{image_crc:08X}")

    with serial.Serial(args.port, args.baudrate, timeout=0.05) as ser:
        send_frame(ser, build_frame(CMD_ENTER), CMD_ENTER, args.retries, args.timeout)
        send_frame(ser, build_begin(args.bin, args.version, args.slot), CMD_BEGIN, args.retries, args.timeout)

        crc = 0
        start = time.monotonic()
        for offset in range(0, len(image), args.packet_size):
            payload = image[offset : offset + args.packet_size]
            crc = zlib.crc32(payload, crc)
            detail = send_frame(
                ser,
                build_data(offset, payload),
                CMD_DATA,
                args.retries,
                args.timeout,
            )
            percent = 100.0 * min(detail, len(image)) / len(image)
            print(f"\r{detail}/{len(image)} bytes {percent:5.1f}%", end="")

        print()
        if (crc & 0xFFFFFFFF) != image_crc:
            raise RuntimeError("host-side streaming CRC mismatch")

        send_frame(ser, build_frame(CMD_END), CMD_END, args.retries, args.timeout)
        elapsed = time.monotonic() - start
        speed = len(image) / elapsed if elapsed > 0 else 0
        print(f"upgrade sent, {speed:.0f} B/s")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
