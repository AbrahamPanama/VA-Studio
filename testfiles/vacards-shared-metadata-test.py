#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared CLI contracts; no application build or platform dependency fixtures."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1]


class SharedMetadataTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="VACards metadata á ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "relocated source"
        self.shared = self.root / "packaging/vacards"
        self.legacy = self.root / "packaging/macos/vacards"
        self.shared.mkdir(parents=True)
        self.legacy.mkdir(parents=True)
        for name in ("vacards-version.sh", "vacards-dependencies.sh"):
            shutil.copy2(SOURCE / "packaging/vacards" / name, self.shared / name)
            shutil.copy2(SOURCE / "packaging/macos/vacards" / name, self.legacy / name)
        self.version = self.shared / "VERSION.env"
        self.dependency = self.root / "VACARDS-DEPENDENCIES.env"
        shutil.copy2(SOURCE / "packaging/vacards/VERSION.env", self.version)
        shutil.copy2(SOURCE / "VACARDS-DEPENDENCIES.env", self.dependency)

    def run_tool(self, kind, *args, legacy=False):
        tool = (self.legacy if legacy else self.shared) / f"vacards-{kind}.sh"
        # Native Windows Python must pass forward-slash paths to MSYS sh.
        arguments = [arg.as_posix() if isinstance(arg, Path) else str(arg) for arg in args]
        return subprocess.run(["sh", tool.as_posix(), *arguments],
                              cwd=self.temporary.name, capture_output=True)

    def test_default_paths_wrappers_and_relocation(self):
        for kind, options in (("version", ("--base", "--build", "--edition", "--release",
                                           "--tag", "--env", "--pe", "--msi")),
                              ("dependencies", ("--env", "--validate", "--sha256"))):
            for option in options:
                with self.subTest(kind=kind, option=option):
                    shared = self.run_tool(kind, option)
                    legacy = self.run_tool(kind, option, legacy=True)
                    self.assertEqual(shared.returncode, 0, shared.stderr)
                    self.assertEqual((legacy.returncode, legacy.stdout), (0, shared.stdout))
        self.assertFalse((self.legacy / "VERSION.env").exists())
        # MSYS may report /c/... while Python reports C:/...; the default must
        # still point to this relocated root, as verified by its unique fields.
        self.assertTrue(self.run_tool("dependencies", "--validate").stdout.strip().endswith(
                        b"/relocated source/VACARDS-DEPENDENCIES.env"))
        self.version.write_text("format=1\nbase_version=2.3.4\nedition=vacards\nbuild_number=42\n")
        self.assertEqual(self.run_tool("version", "--release", legacy=True).stdout,
                         b"2.3.4-vacards.42\n")

    def test_windows_mappings_and_boundaries(self):
        for base, build, pe, msi in (("1.5.0", "6", "1,5,0,6", "1.5.6"),
                                     ("255.255.65535", "65535", "255,255,65535,65535", "255.255.65535"),
                                     ("0.0.0", "1", "0,0,0,1", "0.0.1")):
            self.version.write_text(f"format=1\nbase_version={base}\nedition=vacards\nbuild_number={build}\n")
            for option, expected in (("--pe", pe), ("--msi", msi)):
                result = self.run_tool("version", option)
                self.assertEqual((result.returncode, result.stdout.strip()), (0, expected.encode()))
        for base, build in (("256.0.0", "1"), ("0.256.0", "1"), ("0.0.65536", "1"),
                            ("1.5.0", "65536"), ("1.5.0", "9" * 100),
                            ("9" * 100 + ".0.0", "1")):
            self.version.write_text(f"format=1\nbase_version={base}\nedition=vacards\nbuild_number={build}\n")
            self.assertNotEqual(self.run_tool("version", "--env").returncode, 0)

    def test_strict_records_in_both_manifests(self):
        for kind, path in (("version", self.version), ("dependencies", self.dependency)):
            original = path.read_text()
            lines = original.splitlines()
            invalid = [original + suffix for suffix in
                       ("\n", "# comment\n", "=value\n", "extra=value\n", "bare_key\n")]
            for line in lines:
                key = line.split("=", 1)[0]
                invalid.extend((original.replace(line + "\n", ""), original + line + "\n",
                                original.replace(line, key + "="),
                                original.replace(line, key + "=<PLACEHOLDER>"),
                                original.replace(line, " " + line),
                                original.replace(line, line + "=extra")))
            for contents in invalid:
                with self.subTest(kind=kind, contents=contents):
                    path.write_text(contents)
                    for legacy in (False, True):
                        result = self.run_tool(kind, "--env", legacy=legacy)
                        self.assertNotEqual(result.returncode, 0)
                        self.assertEqual(result.stdout, b"")
            path.unlink()
            self.assertNotEqual(self.run_tool(kind, "--env").returncode, 0)
            path.write_text(original)

    def test_crlf_explicit_file_and_binary_hash(self):
        for kind, path in (("version", self.version), ("dependencies", self.dependency)):
            original = path.read_bytes()
            expected = self.run_tool(kind, "--file", path, "--env").stdout
            external = Path(self.temporary.name) / f"{kind} override.env"
            external.write_bytes(original.replace(b"\n", b"\r\n"))
            result = self.run_tool(kind, "--file", external, "--env", legacy=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.replace(b"\r\n", b"\n"), expected)
            if kind == "dependencies":
                digest = self.run_tool(kind, "--file", external, "--sha256")
                self.assertEqual(digest.stdout.strip(), hashlib.sha256(external.read_bytes()).hexdigest().encode())

    def test_dependency_values_and_feature_collisions(self):
        original = self.dependency.read_text()
        for line in original.splitlines():
            key, value = line.split("=", 1)
            result = self.run_tool("dependencies", "--get", key)
            self.assertEqual((result.returncode, result.stdout.strip()), (0, value.encode()))
        for field, value in (("required_cmake_features", "WITH_LIBCDR,WITH_LIBCDR"),
                             ("disabled_cmake_features", "WITH_LIBCDR"),
                             ("libcdr_ref", "refs/../escape"),
                             ("nesting_rust_toolchain", "stable"),
                             ("nesting_ffi_api_version", "0")):
            contents = "\n".join(field + "=" + value if line.startswith(field + "=") else line
                                 for line in original.splitlines()) + "\n"
            self.dependency.write_text(contents)
            self.assertNotEqual(self.run_tool("dependencies", "--validate").returncode, 0)

    def test_control_bytes_and_no_final_newline(self):
        for kind, path in (("version", self.version), ("dependencies", self.dependency)):
            original = path.read_bytes()
            for control in (b"\x00", b"\x01", b"\x7f", b"\r"):
                with self.subTest(kind=kind, control=control):
                    corrupted = original.replace(b"format=", b"format=" + control, 1)
                    self.assertNotEqual(corrupted, original)
                    path.write_bytes(corrupted)
                    self.assertNotEqual(self.run_tool(kind, "--env").returncode, 0)
            path.write_bytes(original.rstrip(b"\n"))
            self.assertEqual(self.run_tool(kind, "--env").returncode, 0)

    def test_invalid_cli(self):
        for kind in ("version", "dependencies"):
            for args in ((), ("--unknown",), ("--env", "extra"), ("--file",),
                         ("--file", "missing file", "--env")):
                result = self.run_tool(kind, *args)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b"")
        self.assertNotEqual(self.run_tool("dependencies", "--get", "unknown").returncode, 0)


if __name__ == "__main__":
    unittest.main()
