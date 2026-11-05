import importlib.util
from pathlib import Path
import tempfile
import unittest
import struct

SPEC = importlib.util.spec_from_file_location("read_capture", Path(__file__).parents[1] / "tools/probe/read_capture.py")
capture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(capture)


class CaptureTests(unittest.TestCase):
    def header(self):
        return capture.FILE_HEADER.pack(b"SDSCAP01", 1, 96, 10_000_000, 42, capture.EXPECTED_SHA256.encode())

    def record(self, kind, source, output, sequence=0, flags=0, size=None):
        return capture.RECORD_HEADER.pack(0x31434552, 72, 1000, sequence, 0x1234, kind, 5,
                                          len(source) if size is None else size, 100,
                                          len(output), len(source), len(output), flags) + source + output

    def inspect(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.bin"
            path.write_bytes(data)
            return capture.inspect(path)

    def test_directions_and_absent_raw_message_id(self):
        report = self.inspect(self.header() + self.record(1, b"\x1dhello", b"\x1dpacked") +
                              self.record(3, b"packed", b"hello", sequence=1))
        self.assertEqual(report["record_count"], 2)
        self.assertEqual(report["records"][0]["message_id"], 0x1d)
        self.assertIsNone(report["records"][1]["message_id"])
        self.assertEqual(report["records"][0]["input"]["prefix_hex"], "1d68656c6c6f")

    def test_truncation_is_an_error(self):
        data = self.header() + self.record(1, b"hello", b"packed")
        for count in (1, 100, len(data) - 1):
            with self.subTest(count=count), self.assertRaisesRegex(ValueError, "truncated"):
                self.inspect(data[:count])

    def test_untrusted_length_does_not_allocate(self):
        record = capture.RECORD_HEADER.pack(0x31434552, 72, 1, 0, 0, 1, 1,
                                            2**40, 2**40, 1, 2**31, 0, 0)
        with self.assertRaisesRegex(ValueError, "payload sizes"):
            self.inspect(self.header() + record)

    def test_duplicate_sequence_rejected(self):
        with self.assertRaisesRegex(ValueError, "sequence"):
            self.inspect(self.header() + self.record(1, b"x", b"y") + self.record(2, b"x", b"y"))

    def test_incomplete_copy_is_explicit(self):
        report = self.inspect(self.header() + self.record(1, b"", b"x", flags=1, size=100))
        self.assertEqual(report["records"][0]["flags"], 1)
        self.assertEqual(report["records"][0]["source_size"], 100)

    def test_physics_arguments_are_not_packet_id(self):
        source = struct.pack("<Qfif", 0x1234, 0.025, 2, 0.025)
        record = capture.RECORD_HEADER.pack(0x31434552, 72, 10, 0, 0, 4, 1, 20, 0, 1, 20, 0, 0) + source
        report = self.inspect(self.header() + record)
        self.assertIsNone(report["records"][0]["message_id"])
        physics = report["records"][0]["physics"]
        self.assertEqual(physics["world_address"], "0x1234")
        self.assertAlmostEqual(physics["time_step"], 0.025)
        self.assertEqual(physics["max_substeps"], 2)
        self.assertEqual(physics["returned_substep_count"], 1)


if __name__ == "__main__":
    unittest.main()
