import ctypes
import importlib.util
from pathlib import Path
import struct
import unittest

path = Path(__file__).resolve().parents[1] / "tools/steam/identity_probe.py"
spec = importlib.util.spec_from_file_location("identity_probe", path)
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class IdentityProbeTests(unittest.TestCase):
    def test_x64_callback_layout(self):
        if ctypes.sizeof(ctypes.c_void_p) != 8:
            self.skipTest("x64 layout")
        self.assertEqual(ctypes.sizeof(probe.CallbackMsg), 24)
        self.assertEqual(probe.CallbackMsg.parameter.offset, 8)
        self.assertEqual(probe.CallbackMsg.size.offset, 16)

    def test_connect_failure_struct(self):
        event = probe.decode_callback(102, struct.pack("<i?3x", 16, True))
        self.assertEqual(event["name"], "SteamServerConnectFailure_t")
        self.assertEqual(event["eresult"], 16)
        self.assertTrue(event["still_retrying"])

    def test_disconnect_struct(self):
        event = probe.decode_callback(103, struct.pack("<i", 3))
        self.assertEqual(event["eresult"], 3)

    def test_truncated_callback_not_decoded(self):
        self.assertNotIn("eresult", probe.decode_callback(102, b"\x01\x00"))

    def test_unknown_payload_not_copied(self):
        result = probe.decode_callback(999, b"private payload")
        self.assertEqual(result, {"id": 999, "payload_bytes": 15})


if __name__ == "__main__":
    unittest.main()
