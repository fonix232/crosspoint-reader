#!/usr/bin/env python3
"""Upload an X4 Pro app image over CrossPoint USB CDC (requires USB updater firmware)."""
import argparse
import hashlib
from pathlib import Path
import struct
import sys
import time


class UploadError(RuntimeError):
    pass


def validate_image(data):
    if len(data) < 65536 or data[0] != 0xE9:
        raise UploadError('Expected an ESP application firmware.bin, not a factory image.')
    if struct.unpack_from('<H', data, 12)[0] != 9:
        raise UploadError('This image is not for ESP32-S3.')
    if not 1 <= data[1] <= 16 or data[23] != 1:
        raise UploadError('Expected 1–16 segments and an appended SHA-256.')
    position, checksum = 24, 0xEF
    payloads = []
    for _ in range(data[1]):
        if position + 8 > len(data):
            raise UploadError('Truncated segment header.')
        length = struct.unpack_from('<I', data, position + 4)[0]
        position += 8
        if position + length > len(data):
            raise UploadError('Truncated segment.')
        payload = data[position:position + length]
        payloads.append(payload)
        for byte in payload:
            checksum ^= byte
        position += length
    padded = (position + 16) & ~15
    if padded + 32 != len(data) or data[padded - 1] != checksum:
        raise UploadError('Invalid image size or checksum.')
    if hashlib.sha256(data[:padded]).digest() != data[padded:]:
        raise UploadError('Invalid image SHA-256.')
    import re
    tags = re.findall(rb'CROSSPOINT-BOARD-V1:([^;\x00]{1,23});', b''.join(payloads))
    if not tags or any(tag != b'x4pro' for tag in tags):
        raise UploadError('Image must be tagged for x4pro only.')
    return data[-32:].hex()


def send(port, data):
    if port.write(data) != len(data):
        raise UploadError('Short USB write; upload aborted.')


def response(port, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = port.readline(512).decode('ascii', errors='replace').strip()
        if line.startswith('FW:ERROR '):
            raise UploadError(line[9:])
        if line.startswith('FW:'):
            return line
    raise UploadError('No updater response. Install the USB-updater firmware via SD first, '
                      'keep the reader awake, and return to Home.')


def expect(port, expected, timeout=15):
    actual = response(port, timeout)
    if actual != expected:
        raise UploadError(f'Expected {expected!r}, received {actual!r}.')


def probe(port):
    send(port, b'\nCMD:FW_INFO\n')
    line = response(port)
    fields = line.split()
    if len(fields) != 5 or fields[:3] != ['FW:INFO', '1', 'x4pro']:
        raise UploadError(f'Unsupported updater: {line}')
    try:
        limit = int(fields[3])
    except ValueError as exc:
        raise UploadError('Invalid partition size.') from exc
    return limit, fields[4]


def upload(port, data, verify_only=False):
    expected_boot_hash = validate_image(data)
    limit, _ = probe(port)
    if len(data) > limit:
        raise UploadError(f'Image is {len(data)} bytes; inactive partition holds {limit}.')
    digest = hashlib.sha256(data).hexdigest()
    send(port, f'CMD:FW_UPLOAD {len(data)} {digest}\n'.encode())
    ready = response(port, 60)
    if ready != 'FW:READY 128':
        raise UploadError(f'Unexpected upload handshake: {ready}')
    last_percent = -1
    for offset in range(0, len(data), 128):
        block = data[offset:offset + 128]
        send(port, block)
        received = offset + len(block)
        expect(port, f'FW:ACK {received}')
        percent = received * 100 // len(data)
        if percent != last_percent:
            print(f'\rUploading: {percent:3d}%', end='', flush=True)
            last_percent = percent
    print()
    expect(port, 'FW:VALIDATED', 120)
    if verify_only:
        send(port, b'CMD:FW_ABORT\n')
        try:
            response(port)
        except UploadError as exc:
            if str(exc) != 'CANCELLED':
                raise
        else:
            raise UploadError('Device did not acknowledge cancellation.')
        print('Image validated on the reader; no flash writes requested.')
        return expected_boot_hash
    send(port, b'CMD:FW_COMMIT\n')
    expect(port, 'FW:FLASHING')
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        status = response(port, min(30, max(0, deadline - time.monotonic())))
        if status == 'FW:DONE':
            print('\nFlash verified; reader is rebooting.')
            return expected_boot_hash
        if not status.startswith('FW:FLASH '):
            raise UploadError(f'Unexpected flash response: {status}')
        print(f'\rFlashing: {status[9:]}%', end='', flush=True)
    raise UploadError('Flash completion not confirmed. Do not retry until the reader state is checked.')


def open_port(serial_module, path):
    port = serial_module.Serial(port=None, baudrate=115200, timeout=0.5, write_timeout=10)
    port.dtr, port.rts = True, False
    port.port = path
    port.open()
    return port


def select_port(list_ports, requested=None, serial_number=None):
    if requested:
        return requested
    matches = [p.device for p in list_ports.comports()
               if p.product == 'CrossPoint_X4_Pro' and (not serial_number or p.serial_number == serial_number)]
    if len(matches) != 1:
        raise UploadError('Connect one awake CrossPoint X4 Pro, or specify --port explicitly.')
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('firmware', type=Path, nargs='?')
    parser.add_argument('--port', help='USB CDC port; otherwise discover CrossPoint_X4_Pro')
    parser.add_argument('--probe', action='store_true', help='Report updater and current image hash only')
    parser.add_argument('--verify-only', action='store_true', help='Upload and validate without flashing')
    args = parser.parse_args()
    if not args.probe and args.firmware is None:
        parser.error('firmware is required unless --probe is used')
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        parser.error('Install pyserial: python3 -m pip install pyserial')
    try:
        # Reject wrong or corrupt images before opening any device.
        data = None if args.probe else args.firmware.read_bytes()
        if data is not None:
            validate_image(data)
        path = select_port(list_ports, args.port)
        identity = next((p.serial_number for p in list_ports.comports() if p.device == path), None)
        with open_port(serial, path) as port:
            if args.probe:
                limit, digest = probe(port)
                print(f'X4 Pro USB updater v1; partition={limit}; running SHA-256={digest}')
                return 0
            expected = upload(port, data, args.verify_only)
        if args.verify_only:
            return 0
        time.sleep(2)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                new_path = select_port(list_ports, args.port if not identity else None, identity)
                with open_port(serial, new_path) as port:
                    _, running = probe(port)
                if running == expected:
                    print(f'Confirmed running uploaded firmware on {new_path}.')
                    return 0
            except (OSError, serial.SerialException, UploadError):
                pass
            time.sleep(1)
        raise UploadError('Upload completed, but the new running image could not be confirmed over USB.')
    except (OSError, serial.SerialException, UploadError) as exc:
        print(f'Upload failed: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
