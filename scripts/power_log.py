#!/usr/bin/env python3
"""Retrieve the persistent PaperS3 battery timeline without resetting the device."""
import argparse
import csv
import io
from pathlib import Path
import time

import serial


def fetch(port, timeout=60):
    connection = serial.Serial(port=None, baudrate=115200, timeout=0.25, write_timeout=5)
    connection.dtr = True
    connection.rts = False
    connection.port = port
    with connection:
        connection.write(b'CMD:POWERLOG\n')
        deadline = time.monotonic() + timeout
        started = False
        rows = []
        pending = bytearray()
        while time.monotonic() < deadline:
            pending.extend(connection.read_until(b'\n'))
            if not pending.endswith(b'\n'):
                continue
            line = bytes(pending).strip()
            pending.clear()
            if line == b'POWERLOG_BUSY':
                raise RuntimeError('Leave USB storage mode before retrieving the power log')
            if line == b'POWERLOG_BEGIN':
                started = True
                continue
            if not started:
                continue
            if line == b'POWERLOG_END':
                if not rows or not rows[0].startswith('session,utc,'):
                    raise RuntimeError('No valid power log header received')
                return '\n'.join(rows) + '\n'
            if line.startswith(b'[') or not line:
                continue  # Ordinary serial diagnostics between CSV records.
            text = line.decode('ascii')
            if len(next(csv.reader(io.StringIO(text)))) not in (17, 19):
                raise RuntimeError('Corrupt/interleaved power log; retry retrieval')
            rows.append(text)
            deadline = time.monotonic() + timeout
        raise RuntimeError('Timed out waiting for power log; ensure device is awake')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = fetch(args.port)
    args.output.write_text(result)
    print(f'Saved {len(result.splitlines()) - 1} records to {args.output}')
