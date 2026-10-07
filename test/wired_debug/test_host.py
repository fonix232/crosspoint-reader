import importlib.util
from pathlib import Path
import struct
import unittest
import zlib

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("wired_debug", root / "scripts/wired_debug.py")
debug = importlib.util.module_from_spec(spec)
spec.loader.exec_module(debug)


class HostTests(unittest.TestCase):
    def test_screenshot_crc_and_framing(self):
        data = b"\x80\x40"
        replies = ["SCREENSHOT 8 2 1 3 2", "DATA 0 8040", f"END {zlib.crc32(data):08x}"]
        metadata, actual = debug.Client.read_screenshot(iter(replies))
        self.assertEqual(data, actual)
        self.assertEqual(metadata, (8, 2, 1, 3, 2))
        for bad in ([replies[0], "DATA 1 8040", replies[2]],
                    [replies[0], replies[1], "END 00000000"],
                    [replies[0], "DATA 0 80", replies[2]],
                    ["SCREENSHOT 8 2 2 3 4"], ["DATA 0 80"], [replies[0]],
                    ["SCREENSHOT 1000000 2 1 3 2"], ["ERROR USB_STORAGE_ACTIVE"], [""]):
            with self.assertRaises(debug.ProtocolError):
                debug.Client.read_screenshot(iter(bad))

    def test_serial_line_split_across_timeouts(self):
        class Serial:
            def write(self, data):
                request_id = data.split()[1]
                self.replies = iter([b"[LOG] rend", b"", b"ering\n", b"DBG:",
                                     request_id, b" O", b"", b"K\n"])
                return len(data)

            def read_until(self, *_):
                return next(self.replies)

        self.assertEqual(debug.Client(Serial()).command("RESET"), {"status": "accepted"})

    def test_orientation_all_four_corners(self):
        # Physical 8x2 screen: a white top-left pixel, all other pixels black.
        expected = ((1, 0), (7, 1), (0, 7), (0, 0))
        for orientation in range(4):
            width, height, stride, data = debug.oriented_pixels((8, 2, 1, orientation, 2), b"\x80\x00")
            white = [(x, y) for y in range(height) for x in range(width)
                     if data[y * stride + x // 8] & (0x80 >> (x % 8))]
            self.assertEqual(white, [expected[orientation]])

    def test_png_headers_and_chunks(self):
        result = debug.png_bytes((8, 2, 1, 3, 2), b"\x80\x40")
        self.assertEqual(result[:8], b"\x89PNG\r\n\x1a\n")
        offset = 8
        while offset < len(result):
            length = struct.unpack(">I", result[offset:offset + 4])[0]
            kind = result[offset + 4:offset + 8]
            payload = result[offset + 8:offset + 8 + length]
            checksum = struct.unpack(">I", result[offset + 8 + length:offset + 12 + length])[0]
            self.assertEqual(checksum, zlib.crc32(kind + payload))
            if kind == b"IDAT":
                self.assertEqual(zlib.decompress(payload), b"\x00\x80\x00\x40")
            offset += length + 12

    def test_commands_and_correlated_logs(self):
        class Serial:
            def write(self, data):
                request_id = data.split()[1]
                self.replies = iter([b"[LOG] unrelated\n", b"DBG:0 OK\n", b"DBG:" + request_id + b" OK\n"])
                return len(data)

            def read_until(self, *_):
                return next(self.replies)

        self.assertEqual(debug.Client(Serial()).command("RESET"), {"status": "accepted"})
        args = debug.parser().parse_args(["swipe", "1", "2", "3", "4", "--duration", "300"])
        self.assertEqual(debug.command_text(args), "TOUCH SWIPE 1 2 3 4 300")
        args = debug.parser().parse_args(["button", "home"])
        self.assertEqual(debug.command_text(args), "BUTTON HOME CLICK 120")


if __name__ == "__main__":
    unittest.main()
