#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Local-only policy guard; never contacts GitHub or executes a workflow.

Retains the vacards-workflow-test CTest name with a deliberately changed contract:
reject executable Actions definitions instead of checking the retired cloud recipe.
Server-level Actions settings, subscriptions and storage billing are separate.
"""

import os
from pathlib import Path
import stat
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def policy_errors(source):
    workflows = Path(source) / ".github/workflows"
    try:
        mode = workflows.lstat().st_mode
    except FileNotFoundError:
        return []
    except OSError as error:
        return ["cannot inspect workflow path: " + str(error)]
    if stat.S_ISLNK(mode):
        return ["workflow directory must not be a symlink"]
    if not stat.S_ISDIR(mode):
        return ["workflow path is not a directory"]
    errors = []
    pending = [workflows]
    try:
        while pending:
            with os.scandir(pending.pop()) as entries:
                for entry in entries:
                    if entry.is_symlink() or Path(entry.name).suffix.casefold() in (".yml", ".yaml"):
                        errors.append("local-only policy forbids workflow or link: " + entry.path)
                    elif entry.is_dir(follow_symlinks=False):
                        pending.append(Path(entry.path))
    except OSError as error:
        # Path.rglob() suppresses scan errors; an incomplete inspection must fail.
        errors.append("cannot enumerate workflows: " + str(error))
    return errors


class LocalOnlyPolicy(unittest.TestCase):
    def test_repository_has_no_executable_workflows(self):
        self.assertEqual(policy_errors(ROOT), [])

    def test_local_gate_and_native_test_sources_are_preserved(self):
        for relative in (
            "packaging/macos/vacards/run-release-gate.sh",
            "packaging/vacards/gate-run.py",
            "packaging/vacards/test-results.py",
            "src/3rdparty/vacards-nesting-rs/CMakeLists.txt",
            "src/nesting/tests/nesting-ffi-test.cpp",
            "doc/public/BUILDING.md",
        ):
            with self.subTest(path=relative):
                self.assertTrue((ROOT / relative).is_file())

    def test_absent_workflow_directory_is_allowed(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.assertEqual(policy_errors(temporary), [])

    def test_readme_and_disabled_recipe_are_not_workflows(self):
        with tempfile.TemporaryDirectory() as temporary:
            workflows = Path(temporary) / ".github/workflows"
            workflows.mkdir(parents=True)
            for name in ("README.md", "example.yml.disabled"):
                (workflows / name).write_text("Documentation only", encoding="utf-8")
            self.assertEqual(policy_errors(temporary), [])

    def test_yaml_definitions_are_rejected_regardless_of_trigger_or_runner(self):
        for filename in ("build.yml", "build.yaml", "BUILD.YML", "nested/build.yaml"):
            for content in ("", "on: workflow_dispatch\njobs: {}\n",
                            "jobs:\n  test:\n    runs-on: self-hosted\n"):
                with self.subTest(filename=filename, content=content):
                    with tempfile.TemporaryDirectory() as temporary:
                        candidate = Path(temporary) / ".github/workflows" / filename
                        candidate.parent.mkdir(parents=True)
                        candidate.write_text(content, encoding="utf-8")
                        self.assertEqual(len(policy_errors(temporary)), 1)

    def test_workflow_directory_symlink_is_rejected(self):
        # Portable metadata simulation; no Windows symlink privilege required.
        with patch.object(Path, "lstat") as metadata:
            metadata.return_value.st_mode = stat.S_IFLNK
            self.assertEqual(policy_errors("unused"), ["workflow directory must not be a symlink"])

    def test_metadata_failure_is_rejected(self):
        with patch.object(Path, "lstat", side_effect=PermissionError("denied")):
            self.assertEqual(policy_errors("unused"), ["cannot inspect workflow path: denied"])

    def test_enumeration_failure_is_rejected(self):
        real_scandir = os.scandir
        for target in ("workflows", "nested"):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as temporary:
                workflows = Path(temporary) / ".github/workflows"
                (workflows / "nested").mkdir(parents=True)

                def scan(path):
                    if Path(path).name == target:
                        raise PermissionError("denied")
                    return real_scandir(path)

                with patch.object(os, "scandir", side_effect=scan):
                    self.assertEqual(policy_errors(temporary), ["cannot enumerate workflows: denied"])

    def test_non_directory_workflow_path_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            workflows = Path(temporary) / ".github/workflows"
            workflows.parent.mkdir()
            workflows.write_text("unexpected file", encoding="utf-8")
            self.assertEqual(policy_errors(temporary), ["workflow path is not a directory"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
