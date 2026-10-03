#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""VerifyPhase0 metadata tests with an inert archive, never runtime ABI proof.

Set VACARDS_TEST_RUSTC to the existing pinned rustc when it is not on PATH.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
BRIDGE = ROOT / "src/3rdparty/vacards-nesting-rs"
VERIFIER = BRIDGE / "cmake/VerifyPhase0.cmake"


class Phase0MetadataTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rustc = os.environ.get("VACARDS_TEST_RUSTC") or shutil.which("rustc")
        if not cls.rustc:
            raise RuntimeError("Set VACARDS_TEST_RUSTC to an existing Rust 1.88.0 executable")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="VACards phase0 á ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source fixture"
        for name in ("Cargo.toml", "Cargo.lock", "rust-toolchain.toml", ".cargo/config.toml",
                     "vendor/jagua-rs/.cargo-checksum.json", "LICENSES/jagua-rs-MPL-2.0.txt",
                     "include/vacards_nesting.h", "src/job.rs"):
            target = self.source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(BRIDGE / name, target)
        self.manifest = self.root / "dependency fixture.env"
        original = (ROOT / "VACARDS-DEPENDENCIES.env").read_text()
        self.manifest.write_text(original)
        self.archive = self.root / "inert archive.a"
        self.archive.write_bytes(b"metadata test only; not executable Rust evidence\n")
        self.output = self.root / "metadata.env"

    def verify(self, success, reason=None):
        result = subprocess.run(["cmake", f"-DSOURCE_DIR={self.source.as_posix()}",
                                 f"-DDEPENDENCY_MANIFEST={self.manifest.as_posix()}",
                                 f"-DRUSTC_EXECUTABLE={Path(self.rustc).as_posix()}",
                                 f"-DRUST_ARCHIVE={self.archive.as_posix()}",
                                 f"-DOUTPUT_FILE={self.output.as_posix()}",
                                 "-P", VERIFIER.as_posix()], cwd=self.root, capture_output=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("nesting_ffi_api_version=3\n", self.output.read_text())
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse(self.output.exists(), "failed validation issued metadata")
            if reason:
                self.assertIn(reason.encode(), result.stderr)

    def test_matching_manifest_header_and_rust(self):
        self.verify(True)

    def test_crlf_manifest(self):
        self.manifest.write_bytes(self.manifest.read_bytes().replace(b"\n", b"\r\n"))
        self.verify(True)

    def test_wrong_manifest_abi(self):
        self.manifest.write_text(self.manifest.read_text().replace("nesting_ffi_api_version=3",
                                                                 "nesting_ffi_api_version=1"))
        self.verify(False, "Public header ABI differs")

    def test_invalid_or_duplicate_fields(self):
        original = self.manifest.read_text()
        for contents in (original + "nesting_ffi_api_version=3\n", original + "unexpected=value\n",
                         original.replace("nesting_ffi_api_version=3\n", ""),
                         original.replace("nesting_ffi_api_version=3", "nesting_ffi_api_version=bad"),
                         original.replace("libcdr_commit=", "libcdr_commit=bad")):
            with self.subTest(contents=contents):
                self.manifest.write_text(contents)
                self.verify(False, "Invalid nesting dependency metadata")

    def test_header_mismatch_missing_and_duplicate(self):
        path = self.source / "include/vacards_nesting.h"
        original = path.read_text()
        for contents in (original.replace("VERSION 3u", "VERSION 1u"),
                         original.replace("#define VAC_NESTING_API_VERSION 3u", ""),
                         original + "\n#define VAC_NESTING_API_VERSION 3u\n"):
            with self.subTest(contents=contents):
                path.write_text(contents)
                self.verify(False, "Public header ABI differs" if "VERSION 1u" in contents
                            else "Expected one numeric VAC_NESTING_API_VERSION declaration")

    def test_rust_mismatch_missing_and_duplicate(self):
        path = self.source / "src/job.rs"
        original = path.read_text()
        declaration = "pub const API_VERSION: u32 = 3;"
        for contents in (original.replace(declaration, declaration.replace("= 3", "= 1")),
                         original.replace(declaration, ""), original + "\n" + declaration + "\n"):
            with self.subTest(contents=contents):
                path.write_text(contents)
                self.verify(False, "Rust ABI differs" if "API_VERSION: u32 = 1;" in contents
                            else "Expected one numeric Rust API_VERSION declaration")


if __name__ == "__main__":
    unittest.main()
