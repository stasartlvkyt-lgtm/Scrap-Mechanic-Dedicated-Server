#!/usr/bin/env python3
"""Create a verified SQLite save snapshot without changing the source world.

Uses SQLite's online backup API so committed WAL state is included. Publication
uses a same-directory hard link: an existing output is never overwritten, even
if another process creates it while the backup is running.
"""

from __future__ import annotations

import argparse
from contextlib import closing
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import tempfile
import time


def create_snapshot(source: Path, output: Path, timeout: float = 60.0) -> dict:
    source = Path(source).resolve(strict=True)
    output = Path(output).absolute()
    if not source.is_file():
        raise ValueError("Source must be a regular SQLite file")
    if os.path.lexists(output):
        raise FileExistsError(f"Output already exists: {output}")
    if timeout <= 0:
        raise ValueError("Timeout must be positive")
    output.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + timeout

    def progress(status: int, remaining: int, total: int) -> None:
        if time.monotonic() >= deadline:
            raise TimeoutError("SQLite backup timed out")

    fd, temporary_name = tempfile.mkstemp(prefix=".scrapds-snapshot-", suffix=".db", dir=output.parent)
    os.close(fd)
    temporary = Path(temporary_name)
    try:
        with closing(sqlite3.connect(source.as_uri() + "?mode=ro", uri=True, timeout=min(timeout, 5.0))) as original:
            original.execute("PRAGMA query_only=ON")
            with closing(sqlite3.connect(temporary)) as snapshot:
                original.backup(snapshot, pages=256, progress=progress, sleep=0.05)
                snapshot.set_progress_handler(lambda: int(time.monotonic() >= deadline), 10000)
                rows = snapshot.execute("PRAGMA quick_check").fetchall()
                if rows != [("ok",)]:
                    raise ValueError(f"Snapshot quick_check failed: {rows[:10]}")
                snapshot.set_progress_handler(None, 0)
        with temporary.open("rb+") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
            os.fsync(stream.fileno())
        byte_count = temporary.stat().st_size
        os.link(temporary, output)  # Atomic, same filesystem, fails if output exists.
        return {
            "format": "scrapds.sqlite-snapshot.v1",
            "source": str(source),
            "output": str(output),
            "bytes": byte_count,
            "sha256": digest,
            "quick_check": "ok",
            "method": "sqlite-online-backup",
        }
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        print(json.dumps(create_snapshot(arguments.source, arguments.output), ensure_ascii=False, indent=2))
        return 0
    except (OSError, ValueError, sqlite3.Error) as error:
        parser.exit(1, f"snapshot error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
