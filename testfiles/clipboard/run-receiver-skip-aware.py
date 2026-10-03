#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""CTest wrapper for gtest binaries whose skip must not become a pass.

Used for `test_text-paste-external` (skips in SetUp when VACARDS_CLIP_SESSION is
unset) and for the in-process `test_text-paste` suite (its live-clipboard fixture
skips unless VACARDS_CLIP_ALLOW_LIVE=1, while its six clipboard-free fragment
tests always run). A plain `ctest` run of an all-skipped binary exits 0 and CTest
would report the test as Passed even though no product behavior was exercised.
The generic TEST_SOURCES loop in testfiles/CMakeLists.txt cannot express that, so
this wrapper:

  * runs the binary with `--gtest_output=json:<path>` (stdout/stderr are passed
    through, so gtest's own `[  SKIPPED ]` line still reaches CTest);
  * returns 77 when the binary exited 0 but nothing was executed (all tests
    skipped, or zero tests), which CTest maps with SKIP_RETURN_CODE 77;
  * returns the binary's own exit code when it is non-zero (a real failure);
  * returns 2 (never 0) when the binary exited 0 but its gtest JSON is missing,
    unreadable, or reports tests without per-test results: the wrapper cannot
    prove a test executed, so it must not let the case look green.

This script never touches the clipboard: it only spawns the test process. The
receiver skips before any clipboard access when no driver session is configured,
and the in-process suite skips before the first clipboard read without the
explicit disposable-clipboard opt-in; the wrapper passes no VACARDS_CLIP_*
variables of its own.

Usage:
  run-receiver-skip-aware.py --receiver PATH [--gtest-output PATH] [-- BINARY_ARGS...]

Exit codes: 0 executed without failure, 77 nothing executed, 2 wrapper could not
verify evidence, otherwise the receiver's exit code.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

EXIT_OK = 0
EXIT_UNVERIFIED = 2
EXIT_SKIP = 77


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Run a gtest binary and map an all-skipped exit 0 to 77.")
    parser.add_argument("--receiver", required=True, help="gtest binary under test")
    parser.add_argument("--gtest-output", default=None,
                        help="path for the binary's --gtest_output=json report (default: temporary file)")
    parser.add_argument("receiver_args", nargs="*",
                        help="extra arguments passed to the binary (use '--' before them)")
    return parser.parse_args(argv)


def _iter_testcases(report: dict):
    """Yield testcase dicts from the gtest JSON report layouts.

    Real gtest writes `{"testsuites": [{"testsuite": [{"name": ...}]}]}`; the
    flat `{"testsuite": [{...}]}` shape is tolerated for older/other producers.
    """
    def from_suite(suite):
        nested = suite.get("testsuite") if isinstance(suite, dict) else None
        if isinstance(nested, list):
            for entry in nested:
                if not isinstance(entry, dict):
                    continue
                if isinstance(entry.get("testsuite"), (list, dict)):
                    yield from from_suite(entry)
                else:
                    yield entry
        elif isinstance(nested, dict):
            yield from from_suite(nested)

    top = report.get("testsuites")
    if isinstance(top, list):
        for suite in top:
            yield from from_suite(suite)
    elif isinstance(top, dict):
        yield from from_suite(top)
    flat = report.get("testsuite")
    if isinstance(flat, list):
        for entry in flat:
            if not isinstance(entry, dict):
                continue
            if isinstance(entry.get("testsuite"), (list, dict)):
                yield from from_suite(entry)
            else:
                yield entry
    elif isinstance(flat, dict):
        yield from from_suite(flat)


def count_tests(report: dict):
    """Return (total, skipped, per_test_details) from a gtest JSON report.

    A test counts as skipped when either the `status` or the `result` field says
    SKIPPED (gtest versions differ); `per_test_details` is the number of testcase
    entries found, so a report with a test count but no per-test detail can be
    rejected as unverifiable instead of assumed passing.
    """
    testcases = list(_iter_testcases(report))
    total = report.get("tests")
    if not isinstance(total, int) or isinstance(total, bool) or total < 0:
        total = len(testcases)
    total = max(total, len(testcases))
    skipped = sum(1 for testcase in testcases
                  if testcase.get("status") == "SKIPPED" or testcase.get("result") == "SKIPPED")
    return total, min(skipped, total), len(testcases)


def main(argv) -> int:
    args = parse_args(argv)
    receiver = Path(args.receiver).expanduser()
    if not receiver.is_file():
        sys.stderr.write(f"run-receiver-skip-aware: receiver not found: {receiver}\n")
        return EXIT_UNVERIFIED

    temporary_report = None
    if args.gtest_output:
        gtest_path = Path(args.gtest_output).expanduser()
    else:
        handle, temporary_report = tempfile.mkstemp(prefix="vacards-receiver-gtest-", suffix=".json")
        os.close(handle)
        gtest_path = Path(temporary_report)
    try:
        command = [str(receiver)] + list(args.receiver_args) + [f"--gtest_output=json:{gtest_path}"]
        try:
            completed = subprocess.run(command, check=False)
        except OSError as error:
            sys.stderr.write(f"run-receiver-skip-aware: cannot run receiver: {error}\n")
            return EXIT_UNVERIFIED
        if completed.returncode != 0:
            return completed.returncode

        if not gtest_path.is_file():
            sys.stderr.write(
                "run-receiver-skip-aware: receiver exited 0 but produced no gtest JSON at "
                f"{gtest_path}; refusing to report a pass\n")
            return EXIT_UNVERIFIED
        try:
            with gtest_path.open("r", encoding="utf-8") as handle:
                report = json.load(handle)
        except Exception as error:
            sys.stderr.write(
                f"run-receiver-skip-aware: unreadable gtest JSON at {gtest_path}: {error}; "
                "refusing to report a pass\n")
            return EXIT_UNVERIFIED
        if not isinstance(report, dict):
            sys.stderr.write(
                f"run-receiver-skip-aware: gtest JSON at {gtest_path} is not an object; "
                "refusing to report a pass\n")
            return EXIT_UNVERIFIED

        total, skipped, per_test_details = count_tests(report)
        if total == 0:
            sys.stderr.write(
                "run-receiver-skip-aware: the receiver ran zero tests; mapping to exit "
                f"{EXIT_SKIP} (SKIP_RETURN_CODE) instead of a false pass\n")
            return EXIT_SKIP
        if per_test_details == 0:
            sys.stderr.write(
                f"run-receiver-skip-aware: gtest JSON at {gtest_path} reports {total} test(s) but no "
                "per-test results; cannot prove anything executed, refusing to report a pass\n")
            return EXIT_UNVERIFIED
        executed = total - skipped
        if executed == 0:
            sys.stderr.write(
                "run-receiver-skip-aware: nothing executed "
                f"({skipped}/{total} tests skipped); mapping to exit {EXIT_SKIP} "
                "(SKIP_RETURN_CODE) instead of a false pass\n")
            return EXIT_SKIP
        return EXIT_OK
    finally:
        if temporary_report:
            try:
                Path(temporary_report).unlink()
            except OSError:
                pass


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
