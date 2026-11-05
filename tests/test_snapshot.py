import hashlib
from contextlib import closing
import importlib.util
import pathlib
import sqlite3
import tempfile
import unittest


MODULE = pathlib.Path(__file__).resolve().parents[1] / "tools" / "world_snapshot.py"
SPEC = importlib.util.spec_from_file_location("world_snapshot", MODULE)
snapshot = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(snapshot)


class SnapshotTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.source = self.root / "world.db"
        with closing(sqlite3.connect(self.source)) as database, database:
            database.execute("CREATE TABLE Game(value BLOB)")
            database.execute("INSERT INTO Game VALUES (?)", (b"\x00\xffpayload",))

    def test_read_only_and_identical_contents(self):
        old_hash = hashlib.sha256(self.source.read_bytes()).hexdigest()
        old_mtime = self.source.stat().st_mtime_ns
        output = self.root / "subdir" / "снимок.db"
        result = snapshot.create_snapshot(self.source, output)
        self.assertEqual(result["quick_check"], "ok")
        self.assertEqual(old_hash, hashlib.sha256(self.source.read_bytes()).hexdigest())
        self.assertEqual(old_mtime, self.source.stat().st_mtime_ns)
        with closing(sqlite3.connect(output)) as database:
            self.assertEqual(database.execute("SELECT value FROM Game").fetchall(), [(b"\x00\xffpayload",)])
        self.assertEqual(result["sha256"], hashlib.sha256(output.read_bytes()).hexdigest())
        self.assertFalse(list(output.parent.glob(".scrapds-snapshot-*")))

    def test_existing_output_preserved(self):
        output = self.root / "existing.db"
        output.write_bytes(b"preserve")
        with self.assertRaises(FileExistsError):
            snapshot.create_snapshot(self.source, output)
        self.assertEqual(output.read_bytes(), b"preserve")

    def test_committed_wal_included(self):
        original = sqlite3.connect(self.source)
        self.addCleanup(original.close)
        original.execute("PRAGMA journal_mode=WAL")
        original.execute("PRAGMA wal_autocheckpoint=0")
        original.execute("INSERT INTO Game VALUES (?)", (b"wal-only",))
        original.commit()
        main_hash = hashlib.sha256(self.source.read_bytes()).hexdigest()
        output = self.root / "wal-snapshot.db"
        snapshot.create_snapshot(self.source, output)
        with closing(sqlite3.connect(output)) as database:
            self.assertEqual(database.execute("SELECT count(*) FROM Game").fetchone(), (2,))
        self.assertEqual(main_hash, hashlib.sha256(self.source.read_bytes()).hexdigest())

    def test_invalid_source_not_published(self):
        source = self.root / "broken.db"
        source.write_bytes(b"not sqlite")
        output = self.root / "result.db"
        with self.assertRaises(sqlite3.DatabaseError):
            snapshot.create_snapshot(source, output)
        self.assertFalse(output.exists())
        self.assertFalse(list(self.root.glob(".scrapds-snapshot-*")))

    def test_missing_source_not_created(self):
        missing = self.root / "absent.db"
        with self.assertRaises(FileNotFoundError):
            snapshot.create_snapshot(missing, self.root / "result.db")
        self.assertFalse(missing.exists())

    def test_output_race_does_not_overwrite(self):
        output = self.root / "racing.db"
        real_link = snapshot.os.link

        def create_then_link(source, destination):
            destination.write_bytes(b"racing writer")
            return real_link(source, destination)

        from unittest.mock import patch
        with patch.object(snapshot.os, "link", side_effect=create_then_link):
            with self.assertRaises(FileExistsError):
                snapshot.create_snapshot(self.source, output)
        self.assertEqual(output.read_bytes(), b"racing writer")
        self.assertFalse(list(self.root.glob(".scrapds-snapshot-*")))


if __name__ == "__main__":
    unittest.main()
