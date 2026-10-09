#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Focused regression for the Windows internal-test packager stage environment.

Run with: python3 -B testfiles/vacards-packager-env-test.py [-v]

This exercises the real ``stage_env`` helper from
``packaging/windows/vacards/create-internal-test-installer.py`` against a
synthetic UCRT64/MSYS2 directory tree. It proves the approved dependency order,
that ``usr/bin/core_perl`` (which holds ``shasum``) is on the restricted PATH,
that no inherited PATH entry leaks in, that ``LANG``/``LC_ALL`` are pinned to
``C`` for the child only, and that a real child process can discover the
synthetic checksum tool there.

The packager targets native Windows and builds a ``;``-joined PATH. The POSIX
child below is a synthetic shim that reinterprets those entries with the host
separator so the discovery path can run on Linux/macOS; it is not Windows
qualification. On an actual Windows host the child uses native PATH/PATHEXT
semantics and the execution step is skipped as unavailable.
"""

import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
PACKAGER = ROOT / "packaging/windows/vacards/create-internal-test-installer.py"

spec = importlib.util.spec_from_file_location("vacards_create_internal_test_installer", PACKAGER)
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)

# The packager targets native Windows; POSIX can only run a synthetic stand-in.
REQUIRES_POSIX_EXECUTION = unittest.skipUnless(
    os.name != "nt", "extensionless MSYS2 shasum execution is unavailable off POSIX")

# A child that receives the packager env and locates the synthetic tool. It
# splits the Windows-form PATH on ';' explicitly (the packager's native form)
# and rejoins with the host separator so a synthetic POSIX shim can resolve.
CHILD = r"""
import os, shutil, subprocess, sys
entries = os.environ["PATH"].split(";")
tool = shutil.which("shasum", path=os.pathsep.join(entries))
if not tool:
    print("NOT_FOUND")
    sys.exit(0)
print(tool)
if os.name != "nt":
    result = subprocess.run([tool], capture_output=True, text=True)
    print("EXEC:" + result.stdout.strip())
"""


class PackagerStageEnvironment(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="vacards packager env ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cairo = self.root / "cairo prefix"
        self.cdr = self.root / "libcdr prefix"
        self.pango = self.root / "pango prefix"
        self.ucrt = self.root / "msys64/ucrt64"
        self.system_root = self.root / "Windows"
        for directory in (self.pango / "bin", self.cairo / "bin", self.cdr / "bin", self.ucrt / "bin",
                          self.ucrt.parent / "usr/bin",
                          self.ucrt.parent / "usr/bin/core_perl",
                          self.system_root / "System32"):
            directory.mkdir(parents=True)
        self.shasum = self.write_shasum()
        # An inherited PATH that must not survive and a locale that must not
        # leak into the child-process environment.
        self.ambient = {"SystemRoot": str(self.system_root), "HOME": str(self.root),
                        "PATH": "C:\\unsafe-host-tools;C:\\msys64\\ucrt64\\bin",
                        "PATHEXT": ".COM;.EXE;.BAT;.CMD",
                        "LANG": "en_US.UTF-8", "LC_ALL": "en_US.UTF-8", "KEEP": "yes"}

    def write_shasum(self):
        core_perl = self.ucrt.parent / "usr/bin/core_perl"
        if os.name == "nt":
            # Native Windows resolution needs a PATHEXT extension; extensionless
            # MSYS2 tools are only reachable through bash and are not qualified here.
            path = core_perl / "shasum.bat"
            path.write_text("@echo synthetic-shasum\r\n", encoding="utf-8")
            return path
        path = core_perl / "shasum"
        path.write_text("#!/bin/sh\necho synthetic-shasum\n", encoding="utf-8", newline="\n")
        path.chmod(0o755)
        return path

    def approved(self):
        return [str(self.pango / "bin"), str(self.cairo / "bin"), str(self.cdr / "bin"), str(self.ucrt / "bin"),
                str(self.ucrt.parent / "usr/bin"), str(self.ucrt.parent / "usr/bin/core_perl"),
                str(self.system_root / "System32"), str(self.system_root)]

    def test_approved_dependency_order_includes_core_perl(self):
        env = packager.stage_env(self.cairo, self.cdr, self.pango, self.ucrt, environ=self.ambient)
        self.assertEqual(env["PATH"].split(";"), self.approved())
        self.assertEqual(len(self.approved()), len(set(self.approved())))
        self.assertEqual(Path(env["PATH"].split(";")[5]), self.ucrt.parent / "usr/bin/core_perl")

    def test_inherited_unsafe_path_entries_are_excluded(self):
        env = packager.stage_env(self.cairo, self.cdr, self.pango, self.ucrt, environ=self.ambient)
        entries = env["PATH"].split(";")
        for inherited in self.ambient["PATH"].split(";"):
            self.assertNotIn(inherited, entries)

    def test_locale_pinned_and_ambient_mapping_unchanged(self):
        ambient = dict(self.ambient)
        env = packager.stage_env(self.cairo, self.cdr, self.pango, self.ucrt, environ=ambient)
        self.assertEqual((env["LANG"], env["LC_ALL"]), ("C", "C"))
        self.assertIsNot(env, ambient)
        self.assertEqual(ambient, self.ambient)
        self.assertEqual(env["KEEP"], "yes")
        self.assertEqual(env["SystemRoot"], str(self.system_root))

    def test_live_os_environ_not_mutated(self):
        with patch.dict(os.environ, {"SystemRoot": str(self.system_root),
                                     "PATH": "C:\\unsafe-host-tools",
                                     "LANG": "en_US.UTF-8", "LC_ALL": "en_US.UTF-8"}):
            before = dict(os.environ)
            env = packager.stage_env(self.cairo, self.cdr, self.pango, self.ucrt)
            self.assertEqual(dict(os.environ), before)
        self.assertEqual((env["LANG"], env["LC_ALL"]), ("C", "C"))
        self.assertNotIn("unsafe-host-tools", env["PATH"].split(";"))

    def test_synthetic_checksum_tool_discoverable_by_child(self):
        env = packager.stage_env(self.cairo, self.cdr, self.pango, self.ucrt, environ=self.ambient)
        # Keep the real Windows loader root for the child; the synthetic PATH
        # still determines which checksum tool is found.
        if os.name == "nt":
            env["SystemRoot"] = os.environ["SystemRoot"]
        result = subprocess.run([sys.executable, "-B", "-c", CHILD], env=env,
                                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertNotEqual(lines[0], "NOT_FOUND", result.stdout)
        self.assertEqual(Path(lines[0]).resolve(), self.shasum.resolve())
        self.assertEqual(Path(lines[0]).parent.resolve(),
                         (self.ucrt.parent / "usr/bin/core_perl").resolve())
        if os.name != "nt":
            self.assertIn("EXEC:synthetic-shasum", lines)

    @REQUIRES_POSIX_EXECUTION
    def test_posix_shim_executes_from_core_perl(self):
        env = packager.stage_env(self.cairo, self.cdr, self.pango, self.ucrt, environ=self.ambient)
        found = subprocess.run([sys.executable, "-B", "-c",
                                "import os,shutil;print(shutil.which('shasum') or '')"],
                               env=dict(env, PATH=os.pathsep.join(env["PATH"].split(";"))),
                               text=True, capture_output=True, timeout=30)
        self.assertEqual(Path(found.stdout.strip()).resolve(), self.shasum.resolve())


if __name__ == "__main__":
    unittest.main(verbosity=2)
