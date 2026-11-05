#!/usr/bin/env python3
"""Inventory/copy a conservative, build-bound Scrap Mechanic server data pack.

No asset formats are decoded here. The selected files are candidates, not a
claim that the native server can yet consume them or that dependencies close.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile


EXPECTED_BUILD = "cf403cf9d3e465e05298c8e6082f4d664c32140ebcc4696a64200e367abb530d"
CONFIRMED = {".lua", ".json", ".obj", ".tile", ".tileson", ".shapeset", ".physicsmaterials"}
UNCERTAIN = {".mesh", ".export", ".rend"}
ROOTS = ("Data", "Survival", "ChallengeData", "Challenges", "Cache/Bundle")
MANIFEST = "manifest.json"


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def linked(path: Path) -> bool:
    return path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction())


def inside(path: Path, root: Path) -> bool:
    return path == root or root in path.parents


def checked_path(root: Path, relative: str) -> Path:
    """Reject traversal, Windows drive/ADS syntax and links on every platform."""
    parts = relative.replace("\\", "/").split("/")
    if not relative or any(p in ("", ".", "..") or ":" in p for p in parts):
        raise ValueError(f"unsafe relative path: {relative!r}")
    current = root
    for part in parts:
        current = current / part
        if linked(current):
            raise ValueError(f"symbolic link/junction is not allowed: {current}")
    if not inside(current.resolve(), root.resolve()):
        raise ValueError(f"path escapes root: {relative!r}")
    return current


def classify(relative: str) -> str | None:
    path = Path(relative)
    if relative.startswith("Cache/Bundle/"):
        return "physics-cache-candidate" if path.name.endswith("_physics.cbo") else None
    if path.suffix.lower() in CONFIRMED:
        return "server-data-candidate"
    if path.suffix.lower() in UNCERTAIN:
        return "dependency-uncertain"
    return None


def inventory(game: Path, allow_unknown_build: bool = False) -> dict:
    game = game.resolve(strict=True)
    exe = checked_path(game, "Release/ScrapMechanic.exe")
    build = digest(exe)
    if build != EXPECTED_BUILD and not allow_unknown_build:
        raise ValueError(f"unsupported game build {build}; expected {EXPECTED_BUILD}")
    entries = []
    names: set[str] = set()
    skipped_links = []
    for relative_root in ROOTS:
        root = checked_path(game, relative_root)
        if not root.exists():
            continue
        for directory, directories, files in os.walk(root, followlinks=False):
            safe_dirs = []
            for name in sorted(directories):
                child = Path(directory) / name
                if linked(child):
                    skipped_links.append(child.relative_to(game).as_posix())
                else:
                    safe_dirs.append(name)
            directories[:] = safe_dirs
            for name in sorted(files):
                source = Path(directory) / name
                relative = source.relative_to(game).as_posix()
                if linked(source):
                    skipped_links.append(relative)
                    continue
                kind = classify(relative)
                if kind is None:
                    continue
                checked_path(game, relative)
                folded = relative.casefold()
                if folded in names:
                    raise ValueError(f"case-insensitive path collision: {relative}")
                names.add(folded)
                size_before = source.stat().st_size
                sha = digest(source)
                if source.stat().st_size != size_before:
                    raise ValueError(f"source changed while hashing: {source}")
                entries.append({"path": relative, "bytes": size_before,
                                "sha256": sha, "classification": kind})
    entries.sort(key=lambda entry: entry["path"])
    return {"format": "scrapds-data-pack-v1", "game_exe_sha256": build,
            "known_build": build == EXPECTED_BUILD,
            "dependency_completeness": "unverified",
            "skipped_links": sorted(skipped_links), "files": entries}


def verify_output(output: Path, manifest: dict) -> None:
    """An existing pack must match exactly; never replace user files."""
    existing_manifest = checked_path(output, MANIFEST)
    if not existing_manifest.is_file():
        raise ValueError(f"nonempty output has no {MANIFEST}; refusing to overwrite")
    if json.loads(existing_manifest.read_text(encoding="utf-8")) != manifest:
        raise ValueError("existing pack differs from source inventory; choose a new output directory")
    expected = {entry["path"] for entry in manifest["files"]} | {MANIFEST}
    actual = set()
    for directory, directories, files in os.walk(output, followlinks=False):
        for name in directories + files:
            if linked(Path(directory) / name):
                raise ValueError("existing output contains a link/junction")
        for name in files:
            actual.add((Path(directory) / name).relative_to(output).as_posix())
    if actual != expected:
        raise ValueError("existing output has missing or untracked files; refusing to overwrite")
    for entry in manifest["files"]:
        path = checked_path(output, entry["path"])
        if path.stat().st_size != entry["bytes"] or digest(path) != entry["sha256"]:
            raise ValueError(f"existing pack file is modified: {entry['path']}")


def copy_pack(game: Path, output: Path, manifest: dict) -> str:
    game = game.resolve(strict=True)
    # Reject linked existing ancestors before resolving away their identity.
    candidate = output.absolute()
    for ancestor in (candidate, *candidate.parents):
        if linked(ancestor):
            raise ValueError(f"output contains a link/junction: {ancestor}")
    output = candidate.resolve()
    if inside(output, game) or inside(game, output):
        raise ValueError("output and game directories must not overlap")
    if output.exists():
        if not output.is_dir():
            raise ValueError("output is not a directory")
        if any(output.iterdir()):
            verify_output(output, manifest)
            return "verified-existing"
    output.parent.mkdir(parents=True, exist_ok=True)
    # Stage beside destination so completed packs become visible in one rename.
    with tempfile.TemporaryDirectory(prefix=".scrapds-import-", dir=output.parent) as temporary:
        stage = Path(temporary) / "pack"
        stage.mkdir()
        for entry in manifest["files"]:
            source = checked_path(game, entry["path"])
            target = checked_path(stage, entry["path"])
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.open("rb") as incoming, target.open("xb") as outgoing:
                shutil.copyfileobj(incoming, outgoing, 1024 * 1024)
            if target.stat().st_size != entry["bytes"] or digest(target) != entry["sha256"]:
                raise ValueError(f"source changed during copy: {entry['path']}")
        (stage / MANIFEST).write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        if output.exists():
            output.rmdir()  # Only succeeds when still empty; never recursively removes output.
        stage.rename(output)
    return "copied"


def summarize(manifest: dict) -> dict:
    by_class = Counter()
    by_extension = Counter()
    for entry in manifest["files"]:
        by_class[entry["classification"]] += entry["bytes"]
        by_extension[Path(entry["path"]).suffix.lower()] += entry["bytes"]
    return {"game_exe_sha256": manifest["game_exe_sha256"], "known_build": manifest["known_build"],
            "files": len(manifest["files"]), "bytes": sum(by_class.values()),
            "bytes_by_classification": dict(sorted(by_class.items())),
            "bytes_by_extension": dict(sorted(by_extension.items())),
            "skipped_links": manifest["skipped_links"], "dependency_completeness": "unverified"}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", required=True, type=Path, help="licensed installation root")
    parser.add_argument("--output", type=Path, help="new pack directory (required with --copy)")
    parser.add_argument("--copy", action="store_true", help="copy files; default inventories only")
    parser.add_argument("--allow-unknown-build", action="store_true", help="explicitly inventory/copy an unsupported build")
    args = parser.parse_args(argv)
    if args.copy and args.output is None:
        parser.error("--copy requires --output")
    try:
        manifest = inventory(args.game, args.allow_unknown_build)
        result = summarize(manifest)
        result["action"] = copy_pack(args.game, args.output, manifest) if args.copy else "inventory-only"
        if args.copy:
            result["output"] = str(args.output.resolve())
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except (OSError, ValueError) as error:
        print(f"import error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
