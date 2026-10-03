#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Portable evidence manifest unit/CLI tests; no external Python dependencies."""
import hashlib
import importlib.util
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import shutil
import subprocess
import sys
import tempfile
import unittest


TOOL = Path(__file__).resolve().parents[1] / "packaging/vacards/hash-evidence.py"
SPEC = importlib.util.spec_from_file_location("hash_evidence", TOOL)
HASHER = importlib.util.module_from_spec(SPEC)
# Do not generate bytecode under the source checkout.
sys.dont_write_bytecode = True
SPEC.loader.exec_module(HASHER)


class HashEvidenceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="VACards evidence á ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "evidence tree"
        self.root.mkdir()
        (self.root / "z binary.bin").write_bytes(b"\x00\r\n\xff")
        (self.root / "sub dir").mkdir()
        (self.root / "sub dir/a.txt").write_bytes(b"abc")

    def cli(self, *args, env=None):
        return subprocess.run([sys.executable, str(TOOL), "--root", str(self.root), *args],
                              cwd=self.temporary.name, capture_output=True, env=env)

    def link(self, path, target, directory=False):
        try:
            target = str(Path(target))
            # MSYS2 Python can format WindowsPath with "/" when MSYSTEM is set.
            # Relative reparse targets are stored verbatim and need backslashes.
            if os.name == "nt":
                target = target.replace("/", "\\")
            path.symlink_to(target, target_is_directory=directory)
        except OSError as error:
            self.fail(f"symlink tests require symlink support/Windows Developer Mode: {error}")

    def test_known_hashes_sorted_paths_and_deduplicated_selections(self):
        expected = (f"{hashlib.sha256(b'abc').hexdigest()}  sub dir/a.txt\n"
                    f"{hashlib.sha256(bytes([0, 13, 10, 255])).hexdigest()}  z binary.bin\n").encode()
        self.assertEqual(HASHER.manifest_bytes(self.root), expected)
        self.assertEqual(HASHER.manifest_bytes(self.root,
                         ["z binary.bin", "sub dir\\a.txt", "./sub dir//a.txt", "sub dir"]), expected)

    def test_relocation_and_creation_order_do_not_change_bytes(self):
        original = HASHER.manifest_bytes(self.root)
        moved = Path(self.temporary.name) / "another location"
        moved.mkdir()
        (moved / "sub dir").mkdir()
        shutil.copy2(self.root / "sub dir/a.txt", moved / "sub dir/a.txt")
        shutil.copy2(self.root / "z binary.bin", moved / "z binary.bin")
        self.assertEqual(HASHER.manifest_bytes(moved), original)

    def test_output_excluded_and_repeatable(self):
        output = self.root / "SHA256SUMS"
        result = self.cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        original = output.read_bytes()
        self.assertEqual(self.cli().returncode, 0)
        self.assertEqual(output.read_bytes(), original)
        self.assertNotIn(b"SHA256SUMS", original)
        self.assertEqual(self.cli("--output", "sub dir/hashes.txt", "sub dir").returncode, 0)
        self.assertNotIn(b"hashes.txt", (self.root / "sub dir/hashes.txt").read_bytes())

    def test_traversal_absolute_drives_and_unc_rejected(self):
        for invalid in ("../outside", "sub dir/../../outside", "sub dir/../z binary.bin",
                        "..\\outside", "/absolute", "C:\\outside", "C:relative",
                        "\\\\server\\share\\file", "\\rooted"):
            with self.subTest(path=invalid):
                self.assertNotEqual(self.cli(invalid).returncode, 0)
                self.assertNotEqual(self.cli("--output", invalid).returncode, 0)
        self.assertFalse((self.root / "SHA256SUMS").exists())

    def test_hardlinked_output_rejected_repeatably_without_publication(self):
        output = self.root / "SHA256SUMS"
        output.write_bytes(b"previous manifest\n")
        alias = self.root / "alias.txt"
        os.link(output, alias)
        original = sorted(path.name for path in self.root.iterdir())
        for _ in range(2):
            result = self.cli()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"hard links", result.stderr)
            self.assertEqual(output.read_bytes(), b"previous manifest\n")
            self.assertEqual(alias.read_bytes(), b"previous manifest\n")
            self.assertTrue(os.path.samefile(output, alias))
            self.assertEqual(sorted(path.name for path in self.root.iterdir()), original)

    def test_prefix_collisions_on_every_host(self):
        # Pure registry tests also exercise case-sensitive fixtures on hosts
        # whose filesystem cannot create the conflicting names simultaneously.
        for first, second in (("item", "ITEM/child.txt"),
                              ("folder/a.txt", "FOLDER/b.txt"),
                              ("café/a.txt", "cafe\u0301/b.txt"),
                              ("café", "cafe\u0301/child.txt")):
            for left, right in ((first, second), (second, first)):
                with self.subTest(left=left, right=right):
                    registry = {}
                    HASHER.register_name(Path(left), registry)
                    with self.assertRaisesRegex(HASHER.EvidenceError, "collision"):
                        HASHER.register_name(Path(right), registry)

    def test_explicit_paths_register_unvisited_parents(self):
        upper = self.root / "PREFIX"
        upper.mkdir()
        (upper / "a.txt").write_bytes(b"upper")
        lower = self.root / "prefix"
        if lower.exists():
            # Even on a case-insensitive filesystem, an explicit alias spelling
            # must not silently create a nonportable pair of directory names.
            (upper / "b.txt").write_bytes(b"lower")
        else:
            lower.mkdir()
            (lower / "b.txt").write_bytes(b"lower")
        result = self.cli("PREFIX/a.txt", "prefix/b.txt")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"collision", result.stderr)
        self.assertFalse((self.root / "SHA256SUMS").exists())

    def test_windows_path_equality_cannot_hide_case_collisions(self):
        self.assertEqual(PureWindowsPath("item"), PureWindowsPath("ITEM"))
        registry = {}
        HASHER.register_name(PureWindowsPath("item"), registry)
        with self.assertRaisesRegex(HASHER.EvidenceError, "collision"):
            HASHER.register_name(PureWindowsPath("ITEM/child.txt"), registry)
        self.assertEqual(registry["item"], "item")

    def test_unicode_output_repeatability_with_host_aliases(self):
        directory = self.root / "re\u0301sultats"
        directory.mkdir()
        output = directory / "somme\u0301.sha"
        output.write_bytes(b"old manifest")
        # On normalization-insensitive macOS these are one physical entry.
        # Other hosts use their actual decomposed name, plus the pure registry
        # collision tests above which cover distinct Unicode entries.
        composed = self.root / "résultats/sommé.sha"
        argument = "résultats/sommé.sha" if composed.exists() else "re\u0301sultats/somme\u0301.sha"
        (directory / "data.txt").write_bytes(b"sample")
        # Reproduce Windows' redirected legacy encoding on every test host.
        env = {**os.environ, "PYTHONIOENCODING": "cp1252"}
        first = self.cli("--output", argument, env=env)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(first.stdout.decode("utf-8").strip(), str(self.root.resolve() / argument))
        expected = output.read_bytes()
        second = self.cli("--output", argument, env=env)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(output.read_bytes(), expected)
        self.assertNotIn("sommé.sha".encode(), expected)

    def test_reserved_superscript_devices_rejected_as_inputs_and_outputs(self):
        for name in ("COM¹", "COM².txt", "com³.log", "LPT¹", "LPT².txt", "lpt³.log"):
            with self.subTest(name=name):
                with self.assertRaises(HASHER.EvidenceError):
                    HASHER.normalized_name(PurePosixPath(name))
                result = self.cli("--output", name)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b"reserved Windows filename", result.stderr)
        self.assertEqual(HASHER.normalized_name(Path("COM10.txt")), "COM10.txt")
        self.assertEqual(HASHER.normalized_name(Path("LPT0.txt")), "LPT0.txt")

    def test_file_symlink_escape_rejected_preserves_output(self):
        outside = Path(self.temporary.name) / "outside.txt"
        outside.write_text("outside")
        output = self.root / "SHA256SUMS"
        output.write_bytes(b"previous manifest\n")
        self.link(self.root / "escape", outside)
        result = self.cli()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"escapes evidence root", result.stderr)
        self.assertEqual(output.read_bytes(), b"previous manifest\n")

    def test_directory_symlink_escape_rejected(self):
        outside = Path(self.temporary.name) / "outside directory"
        outside.mkdir()
        self.link(self.root / "escape", outside, directory=True)
        self.assertNotEqual(self.cli().returncode, 0)

    def test_internal_symlinks_hash_logical_paths_and_exclude_output_alias(self):
        self.link(self.root / "alias.txt", "sub dir/a.txt")
        self.link(self.root / "directory alias", "sub dir", directory=True)
        manifest = HASHER.manifest_bytes(self.root)
        self.assertIn(b"  alias.txt\n", manifest)
        self.assertIn(b"  directory alias/a.txt\n", manifest)
        (self.root / "SHA256SUMS").write_bytes(b"stale")
        self.link(self.root / "output alias", "SHA256SUMS")
        self.assertEqual(HASHER.manifest_bytes(self.root), manifest)

    def test_broken_links_and_cycles_fail(self):
        for target in ("missing", ".", "bad link"):
            with self.subTest(target=target):
                link = self.root / "bad link"
                self.link(link, target, directory=target == ".")
                self.assertNotEqual(self.cli().returncode, 0)
                link.unlink()

    def test_output_symlinks_and_output_parent_escapes_fail(self):
        outside = Path(self.temporary.name) / "outside"
        outside.write_bytes(b"untouched")
        link = self.root / "SHA256SUMS"
        for target in (outside, self.root / "z binary.bin"):
            self.link(link, target)
            self.assertNotEqual(self.cli().returncode, 0)
            link.unlink()
        self.link(self.root / "outside dir", Path(self.temporary.name), directory=True)
        self.assertNotEqual(self.cli("--output", "outside dir/new").returncode, 0)
        self.assertEqual(outside.read_bytes(), b"untouched")

    def test_nfc_and_case_collisions(self):
        (self.root / "cafe\u0301.txt").write_bytes(b"accent")
        self.assertIn("  café.txt\n".encode(), HASHER.manifest_bytes(self.root))
        # macOS/Windows may alias these names. Pure normalization must still be
        # identical; collision rejection is exercised on case-sensitive hosts.
        self.assertEqual(HASHER.normalized_name(Path("café.txt")),
                         HASHER.normalized_name(Path("cafe\u0301.txt")))
        (self.root / "Case.txt").write_bytes(b"upper")
        if not (self.root / "case.txt").exists():
            (self.root / "case.txt").write_bytes(b"lower")
            self.assertNotEqual(self.cli().returncode, 0)

    def test_nonportable_names_special_files_and_missing_selections(self):
        for name in ("bad\nname", "bad\rname", "literal\\slash", "CON", "NUL.txt", "COM1",
                     "bad:", "trailing.", "trailing "):
            with self.subTest(name=name), self.assertRaises(HASHER.EvidenceError):
                HASHER.normalized_name(PurePosixPath(name))
        self.assertNotEqual(self.cli("missing").returncode, 0)
        self.assertNotEqual(self.cli("--output", ".").returncode, 0)
        if hasattr(os, "mkfifo"):
            os.mkfifo(self.root / "fifo")
            self.assertNotEqual(self.cli().returncode, 0)


if __name__ == "__main__":
    unittest.main()
