#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise Rust/C++ target policy without claiming cross-platform linking."""

import pathlib
import subprocess
import tempfile
import unittest


POLICY = (pathlib.Path(__file__).resolve().parents[1] /
          "src/3rdparty/vacards-nesting-rs/cmake/RustTarget.cmake")


class RustTargetPolicyTest(unittest.TestCase):
    def resolve(self, *, system="Windows", processor="AMD64", pointer="8",
                compiler="GNU", host="x86_64-pc-windows-msvc", architectures="",
                requested="", expected=None, error=None):
        with tempfile.TemporaryDirectory(prefix="vacards target policy ") as root:
            script = pathlib.Path(root) / "policy.cmake"
            args = [host, system, processor, pointer, compiler, architectures, requested]
            script.write_text(
                f'include("{POLICY.as_posix()}")\n' +
                'vacards_nesting_rust_target(' +
                ' '.join(f'"{value}"' for value in args) + ' result)\n' +
                'message(STATUS "resolved=${result}")\n', encoding="utf-8")
            result = subprocess.run(["cmake", "-P", str(script)], text=True,
                                    capture_output=True, check=False)
            output = result.stdout + result.stderr
            if error:
                self.assertNotEqual(result.returncode, 0, output)
                self.assertIn(error, " ".join(output.split()))
            else:
                self.assertEqual(result.returncode, 0, output)
                self.assertIn(f"resolved={expected}", output)

    def test_gnu_compiler_does_not_use_msvc_rust_host(self):
        self.resolve(expected="x86_64-pc-windows-gnu")

    def test_msvc_compiler_does_not_use_gnu_rust_host(self):
        self.resolve(compiler="MSVC", host="x86_64-pc-windows-gnu",
                     expected="x86_64-pc-windows-msvc")

    def test_clang_gnu_frontend(self):
        self.resolve(compiler="Clang", expected="x86_64-pc-windows-gnu")

    def test_matching_explicit_target(self):
        self.resolve(requested="x86_64-pc-windows-gnu",
                     expected="x86_64-pc-windows-gnu")

    def test_mixed_windows_abi_rejected(self):
        self.resolve(requested="x86_64-pc-windows-msvc", error="does not match")

    def test_unsupported_windows_architecture_rejected(self):
        self.resolve(processor="ARM64", error="require x86-64")
        self.resolve(pointer="4", error="require x86-64")

    def test_unsupported_compiler_rejected(self):
        self.resolve(compiler="unknown", error="Unsupported VACards nesting Windows compiler")

    def test_apple_silicon(self):
        self.resolve(system="Darwin", processor="arm64", compiler="AppleClang",
                     expected="aarch64-apple-darwin")

    def test_intel_macos_cross_build_uses_requested_architecture(self):
        self.resolve(system="Darwin", processor="arm64", compiler="AppleClang",
                     architectures="x86_64", expected="x86_64-apple-darwin")

    def test_multi_arch_build_cannot_silently_link_one_rust_archive(self):
        self.resolve(system="Darwin", processor="arm64",
                     architectures="arm64;x86_64", error="single macOS architecture")

    def test_macos_target_mismatch_rejected(self):
        self.resolve(system="Darwin", processor="arm64",
                     requested="x86_64-apple-darwin", error="does not match")

    def test_native_linux_remains_supported(self):
        self.resolve(system="Linux", processor="x86_64", host="x86_64-unknown-linux-gnu",
                     expected="x86_64-unknown-linux-gnu")

    def test_explicit_other_platform_target(self):
        self.resolve(system="Linux", processor="aarch64", requested="aarch64-unknown-linux-gnu",
                     expected="aarch64-unknown-linux-gnu")

    def test_linux_foreign_os_rejected(self):
        self.resolve(system="Linux", processor="x86_64",
                     host="x86_64-unknown-linux-gnu", requested="x86_64-pc-windows-gnu",
                     error="does not match Linux")

    def test_linux_foreign_architecture_rejected(self):
        self.resolve(system="Linux", processor="x86_64",
                     host="x86_64-unknown-linux-gnu", requested="aarch64-unknown-linux-gnu",
                     error="does not match Linux")

    def test_target_cannot_escape_output_directory(self):
        self.resolve(system="Linux", requested="../bad", error="Invalid VACards nesting Rust target")

    def test_missing_host_rejected(self):
        self.resolve(system="Linux", host="", error="does not match Linux")

    def test_stdlib_probe_outcomes(self):
        with tempfile.TemporaryDirectory(prefix="vacards standard library ") as root:
            root = pathlib.Path(root)
            present = root / "lib with spaces"
            present.mkdir()
            (present / "libstd-fixture.rlib").write_bytes(b"existence probe fixture, not compiled Rust")
            empty = root / "empty"
            empty.mkdir()
            for status, directory, succeeds in [
                ("0", present, True), ("1", present, False),
                ("No such file or directory", present, False),
                ("0", empty, False), ("0", root / "missing", False),
            ]:
                with self.subTest(status=status, directory=directory):
                    script = root / "probe.cmake"
                    script.write_text(f'include("{POLICY.as_posix()}")\n'
                                      f'vacards_nesting_require_stdlib("{status}" '
                                      f'"{directory.as_posix()}" "test-target")\n', encoding="utf-8")
                    result = subprocess.run(["cmake", "-P", str(script)], text=True,
                                            capture_output=True, check=False)
                    output = result.stdout + result.stderr
                    self.assertEqual(result.returncode == 0, succeeds, output)
                    if not succeeds:
                        self.assertIn("Rust standard library for test-target is unavailable", output)


if __name__ == "__main__":
    unittest.main()
