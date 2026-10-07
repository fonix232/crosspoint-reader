#!/usr/bin/env python3
"""Unattended CrossPoint input and screenshots. Requires only pyserial."""

import argparse
import json
from pathlib import Path
import secrets
import struct
import sys
import time
import zlib


class ProtocolError(RuntimeError):
    pass


class Client:
    def __init__(self, serial_port, timeout=40):
        self.serial = serial_port
        self.timeout = timeout

    def lines(self, prefix, deadline):
        pending = bytearray()
        while time.monotonic() < deadline:
            chunk = self.serial.read_until(b"\n", 1024)
            if not chunk:
                continue
            pending.extend(chunk)
            if len(pending) > 1024:
                raise ProtocolError("Oversized serial line")
            # A serial read timeout can split a valid line during rendering.
            if not pending.endswith(b"\n"):
                continue
            line = bytes(pending)
            pending.clear()
            # Device logs may be interspersed, but never interpret them as replies.
            if not line.startswith(prefix):
                continue
            try:
                yield line[len(prefix):].strip().decode("ascii")
            except UnicodeDecodeError as exc:
                raise ProtocolError("Non-ASCII reply") from exc
        raise ProtocolError("Timed out waiting for the awake reader")

    def command(self, command, screenshot=False):
        request_id = secrets.randbelow(2147483647) + 1
        wire = f"CMD:DEBUG {request_id} {command}\n".encode("ascii")
        if self.serial.write(wire) != len(wire):
            raise ProtocolError("Incomplete command write")
        replies = self.lines(f"DBG:{request_id} ".encode(), time.monotonic() + self.timeout)
        if screenshot:
            return self.read_screenshot(replies)
        for reply in replies:
            if reply.startswith("ERROR "):
                raise ProtocolError(reply)
            if reply == "OK" or reply == "OK RESET":
                return {"status": "accepted"}
            if reply.startswith("POWER "):
                fields = reply.split()
                if len(fields) < 2 or fields[1] not in ("1", "2") or len(fields) != (13 if fields[1] == "1" else 14):
                    raise ProtocolError("Unsupported POWER format")
                names = ("millivolts", "estimated_percent", "displayed_percent", "cpu_mhz", "wifi_mode",
                         "wifi_connected", "usb_connected", "charging", "epd_power", "epd_output_enable")
                names += (("usb_millivolts",) if fields[1] == "2" else ()) + ("uptime_ms",)
                return dict(zip(names, map(int, fields[2:])))
            if reply.startswith("INFO "):
                fields = reply.split()
                if len(fields) != 9 or fields[1] != "1":
                    raise ProtocolError("Unsupported INFO format")
                values = [int(value) for value in fields[2:]]
                return dict(zip(("width", "height", "orientation", "touch", "home_key", "usb_storage", "uptime_ms"), values))
            raise ProtocolError(f"Unexpected reply: {reply}")

    @staticmethod
    def read_screenshot(replies):
        metadata = None
        data = bytearray()
        for reply in replies:
            if reply.startswith("ERROR "):
                raise ProtocolError(reply)
            fields = reply.split()
            if not fields:
                raise ProtocolError("Empty screenshot reply")
            if fields[0] == "SCREENSHOT":
                if metadata or len(fields) != 6:
                    raise ProtocolError("Invalid screenshot header")
                metadata = tuple(int(value) for value in fields[1:])
                width, height, stride, orientation, size = metadata
                if not (0 < width <= 4096 and 0 < height <= 4096 and stride == (width + 7) // 8
                        and orientation in range(4) and size == stride * height and size <= 2 * 1024 * 1024):
                    raise ProtocolError("Invalid screenshot dimensions")
            elif fields[0] == "DATA":
                if not metadata or len(fields) != 3 or int(fields[1]) != len(data):
                    raise ProtocolError("Missing, duplicate, or out-of-order screenshot chunk")
                chunk = bytes.fromhex(fields[2])
                if not 0 < len(chunk) <= 64 or len(data) + len(chunk) > metadata[4]:
                    raise ProtocolError("Invalid screenshot chunk length")
                data.extend(chunk)
            elif fields[0] == "END":
                if (not metadata or len(fields) != 2 or len(data) != metadata[4]
                        or zlib.crc32(data) != int(fields[1], 16)):
                    raise ProtocolError("Screenshot length/checksum mismatch")
                return metadata, bytes(data)
            else:
                raise ProtocolError(f"Unexpected screenshot reply: {reply}")
        raise ProtocolError("Incomplete screenshot")

    def usb(self, action):
        wire = f"CMD:USB {action}\n".encode("ascii")
        if self.serial.write(wire) != len(wire):
            raise ProtocolError("Incomplete command write")
        for reply in self.lines(b"USB:", time.monotonic() + self.timeout):
            if reply.startswith("ERROR"):
                raise ProtocolError(reply)
            return {"usb": reply}


def oriented_pixels(metadata, data):
    """Match GfxRenderer::tapToLogical; output PNG grayscale bits (1 = white)."""
    width, height, stride, orientation, _ = metadata
    out_width, out_height = (height, width) if orientation in (0, 2) else (width, height)
    out_stride = (out_width + 7) // 8
    result = bytearray(out_stride * out_height)
    for py in range(height):
        for px in range(width):
            if orientation == 0:
                x, y = height - 1 - py, px
            elif orientation == 1:
                x, y = width - 1 - px, height - 1 - py
            elif orientation == 2:
                x, y = py, width - 1 - px
            else:
                x, y = px, py
            if data[py * stride + px // 8] & (0x80 >> (px % 8)):
                result[y * out_stride + x // 8] |= 0x80 >> (x % 8)
    return out_width, out_height, out_stride, result


def png_bytes(metadata, data):
    width, height, stride, pixels = oriented_pixels(metadata, data)

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))

    rows = b"".join(b"\x00" + pixels[y * stride:(y + 1) * stride] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 1, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--port", help="Explicit serial port; required when multiple readers are connected")
    result.add_argument("--timeout", type=float, default=40)
    result.add_argument("--settle", type=float, default=0.6, help="Seconds after accepted input (default: 0.6)")
    sub = result.add_subparsers(dest="operation", required=True)
    sub.add_parser("info")
    sub.add_parser("power", help="Read battery voltage and current power state; -1 means unavailable")
    sub.add_parser("reset", help="Cancel all synthetic contacts without generating clicks")
    shot = sub.add_parser("screenshot")
    shot.add_argument("output", type=Path, help="PNG output, written only after length and CRC verification")
    button = sub.add_parser("button")
    button.add_argument("name", choices=("back", "confirm", "left", "right", "up", "down", "power", "home"))
    button.add_argument("action", nargs="?", default="click", choices=("click", "down", "up"))
    button.add_argument("--duration", type=int, default=120, help="Click/hold duration in milliseconds (1–5000)")
    for action in ("tap", "swipe", "down", "move", "up"):
        touch = sub.add_parser(action, help=f"Touch {action} using current logical screen pixels")
        if action != "up":
            touch.add_argument("x", type=int)
            touch.add_argument("y", type=int)
        if action == "swipe":
            touch.add_argument("end_x", type=int)
            touch.add_argument("end_y", type=int)
        if action in ("tap", "swipe"):
            touch.add_argument("--duration", type=int, default=120 if action == "tap" else 300)
    usb = sub.add_parser("usb")
    usb.add_argument("action", choices=("on", "off", "status"))
    return result


def command_text(args):
    operation = args.operation
    if operation in ("info", "power", "reset", "screenshot"):
        return operation.upper()
    if operation == "button":
        command = f"BUTTON {args.name.upper()} {args.action.upper()}"
        if args.action == "click":
            command += f" {args.duration}"
        return command
    command = f"TOUCH {operation.upper()}"
    if operation != "up":
        command += f" {args.x} {args.y}"
    if operation == "swipe":
        command += f" {args.end_x} {args.end_y}"
    if operation in ("tap", "swipe"):
        command += f" {args.duration}"
    return command


def main():
    args = parser().parse_args()
    if args.timeout <= 0 or args.settle < 0:
        raise ProtocolError("Timeout must be positive and settle must be nonnegative")
    import serial
    from serial.tools import list_ports

    port = args.port
    if port is None:
        candidates = [p.device for p in list_ports.comports() if p.vid == 0x303A or "CrossPoint" in (p.product or "")]
        if len(candidates) != 1:
            raise ProtocolError("Specify --port; candidates: " + (", ".join(candidates) or "none"))
        port = candidates[0]
    connection = serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=2)
    connection.dtr = True
    connection.rts = False
    connection.port = port
    if sys.platform != "win32":
        connection.exclusive = True
    connection.open()
    with connection:
        client = Client(connection, args.timeout)
        if args.operation == "usb":
            reply = client.usb(args.action.upper())
        elif args.operation == "screenshot":
            deadline = time.monotonic() + args.timeout
            while True:
                try:
                    metadata, data = client.command("SCREENSHOT", screenshot=True)
                    break
                except ProtocolError as error:
                    if str(error) not in ("ERROR BUSY_RENDER", "ERROR INPUT_ACTIVE") or time.monotonic() >= deadline:
                        raise
                    time.sleep(0.1)
            payload = png_bytes(metadata, data)
            # Avoid leaving a misleading partial capture after a interrupted transfer/write.
            temporary = args.output.with_name(args.output.name + ".tmp")
            try:
                temporary.write_bytes(payload)
                temporary.replace(args.output)
            finally:
                temporary.unlink(missing_ok=True)
            reply = {"output": str(args.output), "bytes": len(payload), "crc32": f"{zlib.crc32(data):08x}"}
        else:
            reply = client.command(command_text(args))
            duration = getattr(args, "duration", 0) / 1000
            if args.operation not in ("info", "power", "reset"):
                time.sleep(duration + args.settle)
        print(json.dumps(reply))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, ProtocolError, ImportError) as error:
        print(f"wired debug: {error}", file=sys.stderr)
        sys.exit(1)
