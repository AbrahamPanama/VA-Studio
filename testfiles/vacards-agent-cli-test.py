#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""VACards agent CLI case runner.

Usage: vacards-agent-cli-test.py INKSCAPE CASE_DIR

Runs every ``*.json`` case in CASE_DIR (sorted) against the VACards agent CLI
of the given Inkscape binary.  Case format is described in
``testfiles/cli_tests/vacards-agent/README.md``.  Exit status:

* 77 -- CASE_DIR is missing or holds no ``*.json`` case (ctest "Skipped");
*  1 -- at least one case failed;
*  0 -- every case passed.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile

RESULT_SCHEMA = "va-studio.cli-result/1"
RECORD_PREFIX = "VASTUDIO-RESULT "
TAIL_CHARS = 3000


def _tokens(work_dir, source_testfiles):
    """Placeholder substitutions applied to every string of a case."""
    return (
        ("{WORK}", work_dir),
        ("{RESULT}", os.path.join(work_dir, "result.jsonl")),
        ("{TESTCASES}", os.path.join(source_testfiles, "cli_tests", "testcases")),
        ("{FIXTURES}", os.path.join(source_testfiles, "cli_tests", "vacards-agent", "fixtures")),
    )


def substitute(value, tokens):
    """Recursively str.replace the placeholders in every string of a JSON value."""
    if isinstance(value, str):
        for token, replacement in tokens:
            value = value.replace(token, replacement)
        return value
    if isinstance(value, list):
        return [substitute(item, tokens) for item in value]
    if isinstance(value, dict):
        return {key: substitute(item, tokens) for key, item in value.items()}
    return value


def match(expected, actual, path, failures):
    """Structural match: dicts are subset matches, lists are elementwise, else equal."""
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            failures.append("%s: expected an object, got %s" % (path, type(actual).__name__))
            return False
        ok = True
        for key, value in expected.items():
            if key not in actual:
                failures.append("%s.%s: missing key (actual keys: %s)" % (path, key, sorted(actual)))
                ok = False
                continue
            if not match(value, actual[key], "%s.%s" % (path, key), failures):
                ok = False
        return ok
    if isinstance(expected, list):
        if not isinstance(actual, list) or len(expected) != len(actual):
            shown = actual if isinstance(actual, list) else type(actual).__name__
            failures.append("%s: expected a list of %d, got %s" % (path, len(expected), shown))
            return False
        ok = True
        for index, (exp_item, act_item) in enumerate(zip(expected, actual)):
            if not match(exp_item, act_item, "%s[%d]" % (path, index), failures):
                ok = False
        return ok
    # A bool only matches a bool: type and value must agree, so true != 1. This is
    # checked before the numeric tolerance, which must never swallow a bool.
    if isinstance(expected, bool) or isinstance(actual, bool):
        if type(expected) is not type(actual) or expected != actual:
            failures.append("%s: expected %r, got %r" % (path, expected, actual))
            return False
        return True
    # Numbers (but not bools) compare with a relative tolerance so a value pinned
    # from one rounding does not fail on an equivalent floating-point result.
    if (
        isinstance(expected, (int, float))
        and not isinstance(expected, bool)
        and isinstance(actual, (int, float))
        and not isinstance(actual, bool)
    ):
        if abs(expected - actual) <= 1e-6 * max(1.0, abs(expected), abs(actual)):
            return True
        failures.append("%s: expected %r, got %r" % (path, expected, actual))
        return False
    if expected != actual:
        failures.append("%s: expected %r, got %r" % (path, expected, actual))
        return False
    return True


def gather_records(case, work_dir, stdout_text, stderr_text, failures):
    """Return (record_lines, records) for the selected result channel."""
    source = case["_records_from"]
    if source == "file":
        path = os.path.join(work_dir, "result.jsonl")
        lines = []
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                lines = [line for line in handle.read().splitlines() if line.strip()]
    else:
        stream = stdout_text if source == "stdout" else stderr_text
        lines = [
            line[len(RECORD_PREFIX):] for line in stream.splitlines() if line.startswith(RECORD_PREFIX)
        ]

    records = []
    for index, line in enumerate(lines):
        try:
            records.append(json.loads(line))
        except ValueError as error:
            failures.append("record %d: invalid JSON (%s): %s" % (index + 1, error, line[:200]))
    return lines, records


def check_records(records, case, failures):
    """Schema/seq invariants plus an optional expect_records comparison."""
    for index, record in enumerate(records):
        if not isinstance(record, dict):
            failures.append("record %d: expected a JSON object" % (index + 1))
            continue
        if record.get("schema") != RESULT_SCHEMA:
            failures.append("record %d: schema %r != %r" % (index + 1, record.get("schema"), RESULT_SCHEMA))
        if record.get("seq") != index + 1:
            failures.append("record %d: seq %r != %d" % (index + 1, record.get("seq"), index + 1))

    if "expect_records" in case:
        expected = case["expect_records"]
        if len(expected) != len(records):
            failures.append("expect_records: expected %d record(s), got %d" % (len(expected), len(records)))
        else:
            for index, (exp_item, act_item) in enumerate(zip(expected, records)):
                match(exp_item, act_item, "expect_records[%d]" % index, failures)


def check_contains(name, needles, haystack, failures):
    for needle in needles:
        if needle not in haystack:
            failures.append("%s: missing %r" % (name, needle))


def check_not_contains(name, needles, haystack, failures):
    for needle in needles:
        if needle in haystack:
            failures.append("%s: unexpectedly present %r" % (name, needle))


def check_files(work_dir, expected_files, failures):
    for rel_path, spec in expected_files.items():
        path = os.path.join(work_dir, rel_path)
        exists = os.path.exists(path)
        if "exists" in spec and exists != spec["exists"]:
            failures.append("expect_files[%s]: exists %s != %s" % (rel_path, exists, spec["exists"]))
        if not exists:
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            content = handle.read()
        check_contains("expect_files[%s].contains" % rel_path, spec.get("contains", []), content, failures)
        check_not_contains(
            "expect_files[%s].not_contains" % rel_path, spec.get("not_contains", []), content, failures
        )


def check_same_files(work_dir, pairs, failures):
    """Every [A, B] pair must name two existing files with identical bytes."""
    for pair in pairs:
        rel_a, rel_b = pair[0], pair[1]
        path_a = os.path.join(work_dir, rel_a)
        path_b = os.path.join(work_dir, rel_b)
        same = False
        if os.path.exists(path_a) and os.path.exists(path_b):
            with open(path_a, "rb") as handle_a, open(path_b, "rb") as handle_b:
                same = handle_a.read() == handle_b.read()
        if not same:
            failures.append("files differ: %s %s" % (rel_a, rel_b))


def run_case(case_path, inkscape, source_testfiles):
    with open(case_path, "r", encoding="utf-8") as handle:
        raw_case = json.load(handle)

    raw_actions = raw_case.get("actions", "")
    if not isinstance(raw_actions, str):
        raw_actions = ""

    work_dir = tempfile.mkdtemp(prefix="vacards-agent-")
    case = substitute(raw_case, _tokens(work_dir, source_testfiles))

    actions = case.get("actions", "")
    args = list(case.get("args", []))
    timeout = case.get("timeout", 120)
    expect_exit = case.get("expect_exit", 0)
    stdin_text = case.get("stdin")
    uses_result = "{RESULT}" in raw_actions or "{RESULT}" in (raw_case.get("stdin") or "")
    default_source = "file" if uses_result else "stderr"
    case["_records_from"] = case.get("records_from", default_source)

    input_copy = None
    if case.get("input"):
        source_path = case["input"]
        input_copy = os.path.join(work_dir, os.path.basename(source_path))
        shutil.copyfile(source_path, input_copy)

    cmd = [inkscape]
    if input_copy:
        cmd.append(input_copy)
    if "actions" in case:
        cmd.append("--actions=" + actions)
    cmd.extend(args)

    env = dict(os.environ)
    env["INKSCAPE_PROFILE_DIR"] = os.path.join(work_dir, "profile")
    env["INKSCAPE_APP_ID_TAG"] = "vacardsagent"
    env["LC_ALL"] = "C"

    failures = []
    timed_out = False
    stdout_text = ""
    stderr_text = ""
    returncode = None
    try:
        completed = subprocess.run(
            cmd, cwd=work_dir, env=env, capture_output=True, timeout=timeout, check=False,
            input=stdin_text.encode("utf-8") if stdin_text is not None else None
        )
        returncode = completed.returncode
        stdout_text = (completed.stdout or b"").decode("utf-8", errors="replace")
        stderr_text = (completed.stderr or b"").decode("utf-8", errors="replace")
    except subprocess.TimeoutExpired as error:
        timed_out = True
        failures.append("timed out after %s second(s)" % timeout)
        stdout_text = (error.stdout or b"").decode("utf-8", errors="replace")
        stderr_text = (error.stderr or b"").decode("utf-8", errors="replace")
        returncode = "timeout"

    if not timed_out and returncode != expect_exit:
        failures.append("exit code: expected %s, got %s" % (expect_exit, returncode))

    record_lines, records = gather_records(case, work_dir, stdout_text, stderr_text, failures)
    check_records(records, case, failures)

    record_text = "\n".join(record_lines)
    check_contains(
        "expect_records_text_contains", case.get("expect_records_text_contains", []), record_text, failures
    )
    check_contains("expect_stdout_contains", case.get("expect_stdout_contains", []), stdout_text, failures)
    check_contains("expect_stderr_contains", case.get("expect_stderr_contains", []), stderr_text, failures)
    check_not_contains(
        "expect_stderr_not_contains", case.get("expect_stderr_not_contains", []), stderr_text, failures
    )
    check_files(work_dir, case.get("expect_files", {}), failures)
    check_same_files(work_dir, case.get("expect_same_files", []), failures)

    return {
        "failures": failures,
        "cmd": cmd,
        "returncode": returncode,
        "stdout": stdout_text,
        "stderr": stderr_text,
        "records": records,
        "work_dir": work_dir,
    }


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("Usage: %s INKSCAPE CASE_DIR\n" % os.path.basename(argv[0]))
        return 2

    inkscape, case_dir = argv[1], argv[2]
    source_testfiles = os.path.dirname(os.path.abspath(__file__))

    case_files = []
    if os.path.isdir(case_dir):
        case_files = sorted(
            os.path.join(case_dir, name) for name in os.listdir(case_dir) if name.endswith(".json")
        )
    if not case_files:
        print("SKIP: no cases in %s" % case_dir)
        return 77

    any_failed = False
    for case_path in case_files:
        name = os.path.basename(case_path)
        try:
            result = run_case(case_path, inkscape, source_testfiles)
        except Exception as error:  # a runner-level problem is still a case failure
            any_failed = True
            print("FAIL %s: runner error: %s" % (name, error))
            continue

        if result["failures"]:
            any_failed = True
            print("FAIL %s: %s" % (name, result["failures"][0]))
            for failure in result["failures"][1:]:
                print("  %s" % failure)
            print("  command: %s" % " ".join(result["cmd"]))
            print("  exit code: %s" % result["returncode"])
            print("  stdout (last %d chars):\n%s" % (TAIL_CHARS, result["stdout"][-TAIL_CHARS:]))
            print("  stderr (last %d chars):\n%s" % (TAIL_CHARS, result["stderr"][-TAIL_CHARS:]))
            print("  records: %s" % json.dumps(result["records"], indent=2))
            print("  work dir: %s" % result["work_dir"])
        else:
            print("PASS %s" % name)
            shutil.rmtree(result["work_dir"], ignore_errors=True)

    return 1 if any_failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
