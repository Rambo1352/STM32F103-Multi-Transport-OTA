#!/usr/bin/env python3
"""Send app.bin to the board over TCP using the upgrade protocol."""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import time
import zlib
from pathlib import Path

from protocol_frame import (  # noqa: E402
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


def read_exact(sock: socket.socket, size: int, timeout_s: float) -> bytes:
    deadline = time.monotonic() + timeout_s
    data = bytearray()
    while len(data) < size:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        sock.settimeout(min(0.05, remaining))
        try:
            chunk = sock.recv(size - len(data))
        except socket.timeout:
            continue
        except OSError as exc:
            raise ConnectionError(f"socket error: {exc}") from exc
        if not chunk:
            raise ConnectionError("connection closed")
        data += chunk
    if len(data) != size:
        raise TimeoutError(f"timeout while reading {size} bytes, got {len(data)}")
    return bytes(data)


def read_ack(sock: socket.socket, timeout_s: float) -> tuple[int, int, int]:
    header = read_exact(sock, 4, timeout_s)
    cmd, length = struct.unpack("<HH", header)
    payload_and_tail = read_exact(sock, length + 6, timeout_s)
    frame = header + payload_and_tail

    if cmd != CMD_ACK:
        raise RuntimeError(f"unexpected response cmd 0x{cmd:04X}")

    expected_crc = struct.unpack("<H", frame[-2:])[0]
    actual_crc = crc16_modbus(frame[:-2])
    if actual_crc != expected_crc:
        raise RuntimeError(f"bad ack crc, actual 0x{actual_crc:04X}, expected 0x{expected_crc:04X}")

    request_cmd, status, detail = struct.unpack("<HHI", payload_and_tail[:8])
    return request_cmd, status, detail


def send_frame(sock: socket.socket, frame: bytes, request_cmd: int, timeout_s: float) -> int:
    sock.settimeout(timeout_s)
    sock.sendall(frame)
    ack_cmd, status, detail = read_ack(sock, timeout_s)
    if ack_cmd != request_cmd:
        raise RuntimeError(f"ack for 0x{ack_cmd:04X}, expected 0x{request_cmd:04X}")
    if status != 0:
        name = STATUS_TEXT.get(status, f"STATUS_{status}")
        raise RuntimeError(f"device returned {name}, detail={detail}")
    return detail


def send_named_frame(sock: socket.socket, name: str, frame: bytes, request_cmd: int, timeout_s: float) -> int:
    print(f"{name}...", flush=True)
    try:
        return send_frame(sock, frame, request_cmd, timeout_s)
    except Exception as exc:
        raise RuntimeError(f"{name} failed: {exc}") from exc


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("bin", type=Path, help="app .bin file")
    parser.add_argument("--host", default="192.168.1.88", help="board IP address")
    parser.add_argument("--port", type=int, default=5000, help="TCP upgrade port")
    parser.add_argument("--packet-size", type=int, default=512)
    parser.add_argument("--version", type=lambda value: int(value, 0), default=0x00010000)
    parser.add_argument("--slot", type=int, default=0, help="0 auto, 1 slot A, 2 slot B")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--frame-gap-ms", type=float, default=20.0,
                        help="delay after each ACK before sending the next frame")
    args = parser.parse_args()

    if args.packet_size <= 0 or (args.packet_size % 2) != 0:
        print("--packet-size must be a positive even number", file=sys.stderr)
        return 2

    image = args.bin.read_bytes()
    image_crc = crc32_file(args.bin)
    print(f"file: {args.bin}")
    print(f"size: {len(image)} bytes")
    print(f"crc32: 0x{image_crc:08X}")
    print(f"target: {args.host}:{args.port}")

    try:
        sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
    except OSError as exc:
        print(f"connect failed: {exc}", file=sys.stderr)
        print("Check the network before retrying:", file=sys.stderr)
        print(f"  1. Ping the target first: ping {args.host}", file=sys.stderr)
        print("  2. For Wi-Fi OTA, PC must connect to ESP32 AP dengziqi and board log must show WIFI OTA listen port=5001.", file=sys.stderr)
        print("  3. For W5500 OTA, PC must be in 192.168.1.x subnet and board log must show TCP OTA listen port=5000.", file=sys.stderr)
        print(f"  4. Test the port: Test-NetConnection {args.host} -Port {args.port}", file=sys.stderr)
        return 1

    with sock:
        try:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass

        time.sleep(1.0)
        start = time.monotonic()
        frame_gap_s = max(0.0, args.frame_gap_ms / 1000.0)

        send_named_frame(sock, "ENTER", build_frame(CMD_ENTER), CMD_ENTER, args.timeout)
        time.sleep(frame_gap_s)
        send_named_frame(sock, "BEGIN", build_begin(args.bin, args.version, args.slot), CMD_BEGIN, args.timeout)
        time.sleep(frame_gap_s)

        crc = 0
        for offset in range(0, len(image), args.packet_size):
            payload = image[offset : offset + args.packet_size]
            crc = zlib.crc32(payload, crc)
            detail = send_named_frame(
                sock,
                f"DATA offset={offset}",
                build_data(offset, payload),
                CMD_DATA,
                args.timeout,
            )
            time.sleep(frame_gap_s)
            percent = 100.0 * min(detail, len(image)) / len(image)
            print(f"\r{detail}/{len(image)} bytes {percent:5.1f}%", end="")

        print()
        if (crc & 0xFFFFFFFF) != image_crc:
            raise RuntimeError("host-side streaming CRC mismatch")

        send_named_frame(sock, "END", build_frame(CMD_END), CMD_END, args.timeout)
        elapsed = time.monotonic() - start
        speed = len(image) / elapsed if elapsed > 0 else 0
        print(f"upgrade sent, {speed:.0f} B/s")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
