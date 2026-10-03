#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Runner-contract tests for testfiles/macos/run-gtk-menu-probe.py.

These are NOT GUI or candidate-behavior evidence. They exercise only the small
local runner's classification and output-safety contract, with tools mocked:

  * classify(): malformed/duplicate/missing case rows, lying or disagreeing
    summary counters, fail rows, setup errors, and the exact all-PASS + rc 0
    success gate;
  * main(): a pre-existing output directory is never overwritten, NaN/infinite
    timeouts are rejected, a pkg-config prefix that does not match the supplied
    prefix is rejected, and a failed compile is preserved and never run.

No product patch, no GUI, no real compiler, no network. Run:

    python3 testfiles/macos/gtk-menu-runner-test.py
"""

from __future__ import annotations

import importlib.util
import json
import os
import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent
RUNNER_PATH = HERE / "run-gtk-menu-probe.py"


def load_runner():
    spec = importlib.util.spec_from_file_location("gtk_menu_probe_runner",
                                                  str(RUNNER_PATH))
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


runner = load_runner()


def case(number: int, result: str, name: str = "c") -> dict:
    return {"case": number, "name": "%s%d" % (name, number), "result": result,
            "detail": "test"}


def summary(total: int, passed: int, failed: int, setup_errors: int = 0) -> dict:
    return {"total": total, "passed": passed, "failed": failed,
            "setup_errors": setup_errors}


def build_log(cases: list, summaries: list) -> str:
    lines = []
    for c in cases:
        lines.append("PROBE_CASE case=%d name=%s result=%s detail=%s"
                     % (c["case"], c["name"], c["result"], c["detail"]))
    for s in summaries:
        lines.append("PROBE_SUMMARY total=%d passed=%d failed=%d setup_errors=%d"
                     % (s["total"], s["passed"], s["failed"], s["setup_errors"]))
    return "\n".join(lines) + "\n"


def classify_log(log: str, probe_rc: int) -> dict:
    parsed = runner.parse_probe_log(log)
    return runner.classify(parsed["cases"], parsed["summaries"], probe_rc)


def six(result: str) -> list:
    return [case(i, result) for i in range(1, 7)]


class ClassifyTests(unittest.TestCase):
    def test_exact_six_pass_rc0_is_candidate_pass(self):
        result = classify_log(build_log(six("PASS"), [summary(6, 6, 0)]), 0)
        self.assertEqual(result["status"], "candidate_pass")
        self.assertEqual(result["exit_code"], 0)
        self.assertTrue(result["pass_"])
        self.assertTrue(result["row_counts_agree"])
        self.assertEqual(result["case_pass_rows"], 6)
        self.assertEqual(result["case_fail_rows"], 0)

    def test_all_fail_rows_rc3_is_baseline_failure(self):
        result = classify_log(build_log(six("FAIL"), [summary(6, 0, 6)]), 3)
        self.assertEqual(result["status"], "baseline_failure")
        self.assertEqual(result["exit_code"], 3)
        self.assertFalse(result["pass_"])

    def test_partial_fail_rows_rc3_is_baseline_failure(self):
        cases = [case(1, "FAIL")] + [case(i, "PASS") for i in range(2, 7)]
        result = classify_log(build_log(cases, [summary(6, 5, 1)]), 3)
        self.assertEqual(result["status"], "baseline_failure")
        self.assertFalse(result["pass_"])

    def test_lying_summary_cannot_pass(self):
        # One actual FAIL row, but the summary claims 6 passed / 0 failed.
        cases = [case(1, "FAIL")] + [case(i, "PASS") for i in range(2, 7)]
        result = classify_log(build_log(cases, [summary(6, 6, 0)]), 0)
        self.assertNotEqual(result["status"], "candidate_pass")
        self.assertFalse(result["pass_"])
        self.assertFalse(result["row_counts_agree"])
        self.assertEqual(result["status"], "incomplete_summary")

    def test_all_pass_rows_but_rc_nonzero_never_passes(self):
        result = classify_log(build_log(six("PASS"), [summary(6, 6, 0)]), 1)
        self.assertEqual(result["status"], "unexpected_failure")
        self.assertFalse(result["pass_"])

    def test_summary_count_disagrees_is_incomplete(self):
        result = classify_log(build_log(six("PASS"), [summary(6, 5, 1)]), 0)
        self.assertEqual(result["status"], "incomplete_summary")
        self.assertFalse(result["pass_"])

    def test_duplicate_summaries_rejected(self):
        result = classify_log(
            build_log(six("PASS"), [summary(6, 6, 0), summary(6, 6, 0)]), 0)
        self.assertEqual(result["status"], "incomplete_summary")
        self.assertFalse(result["pass_"])
        self.assertEqual(result["summary_count"], 2)

    def test_duplicate_case_ids_rejected(self):
        cases = [case(1, "PASS"), case(1, "PASS")] + [case(i, "PASS") for i in (3, 4, 5, 6)]
        result = classify_log(build_log(cases, [summary(6, 6, 0)]), 0)
        self.assertEqual(result["status"], "incomplete_summary")
        self.assertFalse(result["pass_"])

    def test_missing_case_row_rejected(self):
        cases = [case(i, "PASS") for i in range(1, 6)]  # no case 6
        result = classify_log(build_log(cases, [summary(5, 5, 0)]), 0)
        self.assertEqual(result["status"], "incomplete_summary")
        self.assertFalse(result["pass_"])

    def test_out_of_range_case_id_rejected(self):
        cases = [case(7, "PASS")] + [case(i, "PASS") for i in range(1, 6)]
        result = classify_log(build_log(cases, [summary(6, 6, 0)]), 0)
        self.assertEqual(result["status"], "incomplete_summary")
        self.assertFalse(result["pass_"])

    def test_malformed_log_is_incomplete(self):
        log = ("noise line\n"
               "PROBE_CASE case=1 name=x result=MAYBE detail=oops\n"
               "PROBE_SUMMARY total=?\n")
        result = classify_log(log, 0)
        self.assertEqual(result["status"], "incomplete_summary")
        self.assertFalse(result["pass_"])
        self.assertEqual(result["case_count"], 0)
        self.assertEqual(result["summary_count"], 0)

    def test_no_summary_is_incomplete(self):
        result = classify_log(build_log(six("PASS"), []), 0)
        self.assertEqual(result["status"], "incomplete_summary")
        self.assertFalse(result["pass_"])

    def test_setup_errors_fail_setup(self):
        result = classify_log(build_log(six("PASS"), [summary(6, 6, 0, setup_errors=1)]), 0)
        self.assertEqual(result["status"], "setup_failure")
        self.assertEqual(result["exit_code"], 2)
        self.assertFalse(result["pass_"])

    def test_probe_rc2_fail_setup(self):
        result = classify_log(build_log(six("PASS"), [summary(6, 6, 0)]), 2)
        self.assertEqual(result["status"], "setup_failure")
        self.assertEqual(result["exit_code"], 2)
        self.assertFalse(result["pass_"])


def make_prefix(root: pathlib.Path):
    prefix = root / "gtk-prefix"
    (prefix / "lib" / "pkgconfig").mkdir(parents=True)
    lib = root / "libdir"
    lib.mkdir()
    (lib / runner.DYLD_NAME).write_bytes(b"fake dylib")
    return prefix, lib


class FakeToolRun:
    """subprocess.run stand-in: pkg-config, clang and probe are all faked."""

    def __init__(self, pkgconfig_path, clang_path, binary_path,
                 prefix_output, compile_rc=1, write_binary=True,
                 probe_rc=0, probe_log=None):
        self.pkgconfig_path = pkgconfig_path
        self.clang_path = clang_path
        self.binary_path = binary_path
        self.prefix_output = prefix_output
        self.compile_rc = compile_rc
        self.write_binary = write_binary
        self.probe_rc = probe_rc
        self.probe_log = probe_log
        self.pkgconfig_calls = []
        self.clang_calls = []
        self.probe_calls = []

    def __call__(self, argv, **kwargs):
        exe = argv[0]
        if exe == self.pkgconfig_path:
            self.pkgconfig_calls.append(list(argv))
            if "--modversion" in argv:
                out = "4.22.4\n"
            elif "--variable=prefix" in argv:
                out = self.prefix_output + "\n"
            else:
                out = ""
            return subprocess.CompletedProcess(argv, 0, stdout=out, stderr="")
        if exe == self.clang_path:
            self.clang_calls.append(list(argv))
            if self.write_binary:
                with open(self.binary_path, "wb") as f:
                    f.write(b"partial compiler output")
            return subprocess.CompletedProcess(argv, self.compile_rc,
                                               stdout="", stderr="clang error")
        self.probe_calls.append(list(argv))
        stream = kwargs.get("stdout")
        if stream is not None and self.probe_log is not None:
            stream.write(self.probe_log)
        return subprocess.CompletedProcess(argv, self.probe_rc, stdout="", stderr="")


class MainTests(unittest.TestCase):
    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._td.name)
        self.prefix, self.lib = make_prefix(self.root)

    def tearDown(self):
        self._td.cleanup()

    def args(self, out, timeout="10"):
        return ["--gtk-prefix", str(self.prefix), "--lib-dir", str(self.lib),
                "--output", str(out), "--timeout=%s" % timeout]

    def fake_which(self, name):
        return "/usr/bin/pkg-config" if name == "pkg-config" else "/usr/bin/clang"

    def test_preexisting_output_is_never_overwritten(self):
        out = self.root / "evidence"
        out.mkdir()
        sentinel = out / "exit.json"
        original = '{"sentinel": "prior-evidence"}\n'
        sentinel.write_text(original, encoding="utf-8")
        with mock.patch.object(runner.platform, "system", return_value="Darwin"):
            rc = runner.main(self.args(out))
        self.assertEqual(rc, 1)
        self.assertEqual(sentinel.read_text(encoding="utf-8"), original)
        self.assertEqual(sorted(p.name for p in out.iterdir()), ["exit.json"])

    def test_nan_and_infinite_timeout_rejected(self):
        for bad in ("nan", "inf", "-inf", "0", "-5"):
            with self.subTest(timeout=bad):
                out = self.root / ("evidence-" + bad.replace("-", "neg"))
                with mock.patch.object(runner.platform, "system", return_value="Darwin"):
                    rc = runner.main(self.args(out, timeout=bad))
                self.assertEqual(rc, 1)
                self.assertFalse(out.exists())

    def test_pkgconfig_prefix_mismatch_rejected(self):
        out = self.root / "evidence"
        fake = FakeToolRun("/usr/bin/pkg-config", "/usr/bin/clang",
                           out / runner.BINARY_NAME,
                           prefix_output="/different/gtk/prefix")
        with mock.patch.object(runner.platform, "system", return_value="Darwin"), \
             mock.patch.object(runner.shutil, "which", side_effect=self.fake_which), \
             mock.patch.object(runner.subprocess, "run", side_effect=fake):
            rc = runner.main(self.args(out))
        self.assertEqual(rc, 1)
        record = json.loads((out / "exit.json").read_text(encoding="utf-8"))
        self.assertEqual(record["status"], "tooling_failure")
        self.assertIn("resolves prefix", record["error"])
        self.assertEqual(fake.clang_calls, [])
        self.assertEqual(fake.probe_calls, [])

    def test_compile_failure_preserves_output_and_never_runs_probe(self):
        out = self.root / "evidence"
        binary = out / runner.BINARY_NAME
        fake = FakeToolRun("/usr/bin/pkg-config", "/usr/bin/clang", binary,
                           prefix_output=str(self.prefix),
                           compile_rc=1, write_binary=True)
        with mock.patch.object(runner.platform, "system", return_value="Darwin"), \
             mock.patch.object(runner.shutil, "which", side_effect=self.fake_which), \
             mock.patch.object(runner.subprocess, "run", side_effect=fake):
            rc = runner.main(self.args(out))
        self.assertEqual(rc, 1)
        self.assertEqual(len(fake.clang_calls), 1)
        self.assertEqual(fake.probe_calls, [], "probe binary must not run on compile failure")
        self.assertFalse((out / "run.log").exists())
        self.assertTrue(binary.is_file(), "failed compiler output must be preserved")
        self.assertEqual(binary.read_bytes(), b"partial compiler output")
        record = json.loads((out / "exit.json").read_text(encoding="utf-8"))
        self.assertEqual(record["status"], "compile_failure")
        self.assertTrue(record["probe_binary_preserved"])

    def test_compile_success_runs_probe_once_and_classifies(self):
        out = self.root / "evidence"
        binary = out / runner.BINARY_NAME
        probe_log = build_log(six("PASS"), [summary(6, 6, 0)])
        fake = FakeToolRun("/usr/bin/pkg-config", "/usr/bin/clang", binary,
                           prefix_output=str(self.prefix),
                           compile_rc=0, write_binary=True,
                           probe_rc=0, probe_log=probe_log)
        with mock.patch.object(runner.platform, "system", return_value="Darwin"), \
             mock.patch.object(runner.shutil, "which", side_effect=self.fake_which), \
             mock.patch.object(runner.subprocess, "run", side_effect=fake):
            rc = runner.main(self.args(out))
        self.assertEqual(rc, 0)
        self.assertEqual(fake.probe_calls, [[str(binary)]])
        record = json.loads((out / "exit.json").read_text(encoding="utf-8"))
        self.assertEqual(record["status"], "candidate_pass")
        self.assertTrue(record["pass_"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
