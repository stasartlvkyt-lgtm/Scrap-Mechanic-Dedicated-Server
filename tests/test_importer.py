import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / "tools/importer/import_data.py"
SPEC = importlib.util.spec_from_file_location("import_data", SOURCE)
importer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(importer)


class ImporterTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.game = self.root / "game"
        self.write("Release/ScrapMechanic.exe", b"fixture-exe")
        self.write("Data/Scripts/server.lua", b"return 1\n")
        self.write("Survival/terrain.tile", b"tile")
        self.write("Data/collision.mesh", b"mesh")
        self.write("Data/texture.png", b"excluded")
        self.write("Cache/Bundle/game_shape_physics.cbo", b"physics")
        self.write("Cache/Bundle/core_materials.cbo", b"excluded")
        self.write("Save/world.db", b"excluded")

    def write(self, relative, data):
        target = self.game / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        return target

    def manifest(self):
        return importer.inventory(self.game, True)

    def test_unknown_build_rejected_and_candidates_classified(self):
        with self.assertRaisesRegex(ValueError, "unsupported game build"):
            importer.inventory(self.game)
        manifest = self.manifest()
        self.assertFalse(manifest["known_build"])
        entries = {e["path"]: e for e in manifest["files"]}
        self.assertEqual(len(entries), 4)
        self.assertEqual(entries["Data/collision.mesh"]["classification"], "dependency-uncertain")
        self.assertEqual(entries["Data/Scripts/server.lua"]["sha256"], importer.digest(self.game / "Data/Scripts/server.lua"))

    def test_copy_idempotence_and_modified_file_protection(self):
        manifest = self.manifest()
        output = self.root / "output"
        self.assertEqual(importer.copy_pack(self.game, output, manifest), "copied")
        self.assertEqual(json.loads((output / "manifest.json").read_text()), manifest)
        self.assertEqual(importer.copy_pack(self.game, output, manifest), "verified-existing")
        modified = output / "Data/Scripts/server.lua"
        modified.write_bytes(b"user data")
        with self.assertRaisesRegex(ValueError, "modified"):
            importer.copy_pack(self.game, output, manifest)
        self.assertEqual(modified.read_bytes(), b"user data")

    def test_source_change_and_untracked_files_refused(self):
        manifest = self.manifest()
        output = self.root / "output"
        self.write("Data/Scripts/server.lua", b"changed")
        with self.assertRaisesRegex(ValueError, "source changed"):
            importer.copy_pack(self.game, output, manifest)
        self.assertFalse(output.exists())
        output.mkdir()
        (output / "unrelated.txt").write_text("mine")
        with self.assertRaisesRegex(ValueError, "no manifest"):
            importer.copy_pack(self.game, output, self.manifest())
        self.assertEqual((output / "unrelated.txt").read_text(), "mine")

    def test_untracked_file_inside_valid_pack_is_preserved(self):
        manifest = self.manifest()
        output = self.root / "output"
        importer.copy_pack(self.game, output, manifest)
        extra = output / "notes.txt"
        extra.write_text("local notes")
        with self.assertRaisesRegex(ValueError, "untracked"):
            importer.copy_pack(self.game, output, manifest)
        self.assertEqual(extra.read_text(), "local notes")

    def test_traversal_overlaps_and_links(self):
        for path in ("../outside", "/absolute", "C:/absolute", "Data/../x", "Data/x:stream"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                importer.checked_path(self.game, path)
        for output in (self.game, self.game / "pack", self.root):
            with self.subTest(output=output), self.assertRaisesRegex(ValueError, "overlap"):
                importer.copy_pack(self.game, output, self.manifest())
        target = self.root / "outside.lua"
        target.write_text("private")
        link = self.game / "Data/link.lua"
        try:
            link.symlink_to(target)
        except OSError:
            self.skipTest("OS does not permit symbolic links in this session")
        manifest = self.manifest()
        self.assertIn("Data/link.lua", manifest["skipped_links"])
        self.assertNotIn("Data/link.lua", {e["path"] for e in manifest["files"]})


if __name__ == "__main__":
    unittest.main()
