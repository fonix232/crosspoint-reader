import hashlib
import importlib.util
import io
from pathlib import Path
import struct
import unittest
from contextlib import redirect_stdout

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('uploader', root/'scripts/usb_firmware_upload.py')
uploader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(uploader)


def image(board=b'x4pro'):
    header = bytearray(24)
    header[0], header[1], header[12], header[23] = 0xE9, 1, 9, 1
    payload = (b'CROSSPOINT-BOARD-V1:' + board + b';').ljust(65536, b'\x00')
    data = header + struct.pack('<II', 0x3c000020, len(payload)) + payload
    checksum = 0xEF
    for value in payload:
        checksum ^= value
    data += bytes(((len(data)+16)&~15)-len(data)-1) + bytes([checksum])
    return bytes(data + hashlib.sha256(data).digest())


class Port:
    def __init__(self, data, bad_ack=False, validation_error=False):
        self.data, self.bad_ack, self.validation_error = data, bad_ack, validation_error
        self.lines, self.writes = [], []
        self.offset = 0
    def write(self, data):
        self.writes.append(data)
        if data == b'\nCMD:FW_INFO\n':
            self.lines += [b'[123] log noise\n', b'FW:INFO 1 x4pro 6553600 -\n']
        elif data.startswith(b'CMD:FW_UPLOAD '):
            self.lines += [b'FW:READY 128\n']
        elif data == b'CMD:FW_COMMIT\n':
            self.lines += [b'FW:FLASHING\n', b'FW:FLASH 100\n', b'FW:DONE\n']
        elif data == b'CMD:FW_ABORT\n':
            self.lines += [b'FW:ERROR CANCELLED\n']
        else:
            assert data == self.data[self.offset:self.offset+len(data)]
            self.offset += len(data)
            self.lines += [f'FW:ACK {self.offset + int(self.bad_ack)}\n'.encode()]
            if self.offset == len(self.data):
                self.lines += [b'FW:ERROR BAD_SHA\n' if self.validation_error else b'FW:VALIDATED\n']
        return len(data)
    def readline(self, _):
        return self.lines.pop(0) if self.lines else b''


class Tests(unittest.TestCase):
    def test_image_checks(self):
        data = image()
        self.assertEqual(uploader.validate_image(data), data[-32:].hex())
        for invalid in (image(b'papers3'), data[:-1], data + b'x', bytes([0])+data[1:], data[:-1]+bytes([data[-1]^1])):
            with self.assertRaises(uploader.UploadError): uploader.validate_image(invalid)
    def test_upload(self):
        data = image()
        for verify_only in (False, True):
            port = Port(data)
            with redirect_stdout(io.StringIO()): uploader.upload(port, data, verify_only)
            self.assertEqual(b'CMD:FW_COMMIT\n' in port.writes, not verify_only)
    def test_failures_never_commit(self):
        data = image()
        for kwargs in ({'bad_ack':True}, {'validation_error':True}):
            port = Port(data, **kwargs)
            with redirect_stdout(io.StringIO()), self.assertRaises(uploader.UploadError): uploader.upload(port, data)
            self.assertNotIn(b'CMD:FW_COMMIT\n', port.writes)
    def test_unknown_protocol(self):
        port = Port(image())
        port.lines = [b'FW:INFO 2 papers3 100 -\n']
        with self.assertRaises(uploader.UploadError): uploader.probe(port)

if __name__ == '__main__': unittest.main()
