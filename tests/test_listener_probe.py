import ctypes as C
import importlib.util
from pathlib import Path
import struct
import sys
import unittest

directory = Path(__file__).resolve().parents[1] / "tools/steam"
sys.path.insert(0, str(directory))
spec = importlib.util.spec_from_file_location("listener_probe", directory / "listener_probe.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class ListenerTests(unittest.TestCase):
    def test_layout(self):
        probe.validate_layouts()
        self.assertEqual(probe.Message.data.offset, 0)
        self.assertEqual(probe.Message.size.offset, 8)
        self.assertEqual(probe.Message.connection.offset, 12)
        self.assertEqual(probe.Message.release.offset, 184)
        self.assertEqual(probe.ConnectionStatus.old_state.offset, 704)

    def test_callback_binary_parse(self):
        payload = bytearray(712)
        struct.pack_into("<I", payload, 0, 123)
        1)
        struct.pack_into("<I", payload, 8 + 144, 7)
        struct.pack_into("<ii", payload, 8 + 176, 1, 0)
        struct.pack_into("<i", payload, 704, 0)
        event = probe.parse_status(payload)
        self.assertEqual(event["connection"], 123)
        self.assertEqual(event["steam_id"], "1")
        self.assertEqual(event["listen_socket"], 7)
        self.assertEqual(event["state"], 1)

    def test_unknown_identity_cannot_be_authorized(self):
        peer = probe.NetworkIdentity()
        peer.kind = 1
        peer.size = 8
        self.assertEqual(peer.steam_id(), 0)
        peer.kind = 16
        peer.size = 128
        self.assertEqual(peer.steam_id(), 0)

    def test_truncated_callback_rejected(self):
        for length in [0, 4, 8, 704, 711, 713]:
            with self.assertRaises(ValueError):
                probe.parse_status(bytes(length))


if __name__ == "__main__":
    unittest.main()
