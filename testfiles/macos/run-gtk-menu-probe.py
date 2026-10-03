#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build and run the standalone in-process macOS NSMenu protocol probe.

This is a small, local-only runner for testfiles/macos/gtk-menu-probe.m. It:

  * requires an explicit GTK prefix (headers/pkgconfig) and an explicit runtime
    library directory that actually contains libgtk-4.1.dylib;
  * compiles with clang argv assembled from ``shlex.split`` of the pkg-config
    ``gtk4`` cflags/libs -- no shell strings, no hardcoded local paths;
  * writes all evidence (compile.log, run.log, exit.json, the probe binary)
    into a fresh out-of-source --output directory;
  * refuses to run a stale binary: a nonzero compile aborts before the run;
  * enforces the probe's six-case contract and never reports success from a
    missing/zero/mismatched case set.

GUI caveat: the probe activates NSMenu items in-process via
``-[NSMenu performActionForItemAtIndex:]``. That is protocol evidence, not a real
menu-bar click through MenuBarAgent / the native tracking run loop / WindowServer.
A pass here does not satisfy a real native-click GUI case.

Darwin only. No installation, no HOME/global-settings mutation.

Exit codes:
  0  candidate_pass        six cases, all PASS, zero setup errors
  1  compile_failure / unexpected_failure / usage / tooling
  2  setup_failure         probe reported setup errors (harness/menu construction)
  3  baseline_failure      six cases, zero setup errors, >=1 case FAIL (not a pass)
  4  timeout               wall-clock --timeout exceeded
  5  incomplete_summary    six-case summary not met (never a pass)
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import math
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
from typing import Any, Dict, List, Optional

# --- classification / runner exit codes -------------------------------------
EXIT_CANDIDATE_PASS = 0
EXIT_TOOLING = 1
EXIT_SETUP_FAILURE = 2
EXIT_BASELINE_FAILURE = 3
EXIT_TIMEOUT = 4
EXIT_INCOMPLETE = 5
EXIT_UNEXPECTED = 1

DEFAULT_TIMEOUT = 60.0
PROBE_SRC_NAME = "gtk-menu-probe.m"
BINARY_NAME = "gtk-menu-probe"
DYLD_NAME = "libgtk-4.1.dylib"
REQUIRED_CASE_NUMBERS = [1, 2, 3, 4, 5, 6]

SUMMARY_RE = re.compile(
    r"^PROBE_SUMMARY total=(?P<total>\d+) passed=(?P<passed>\d+) "
    r"failed=(?P<failed>\d+) setup_errors=(?P<setup_errors>\d+)\s*$"
)
CASE_RE = re.compile(
    r"^PROBE_CASE case=(?P<case>\d+) name=(?P<name>\S+) "
    r"result=(?P<result>PASS|FAIL) detail=(?P<detail>.*)$"
)

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE_SRC = os.path.join(HERE, PROBE_SRC_NAME)


def utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def sha256_file(path: str) -> Optional[str]:
    try:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return None


def run_capture(argv: List[str], env: Dict[str, str], stdout_path: str,
                timeout: Optional[float] = None) -> int:
    """Run argv verbatim, merging stdout+stderr into stdout_path."""
    with open(stdout_path, "w", encoding="utf-8", errors="replace") as out:
        proc = subprocess.run(argv, stdout=out, stderr=subprocess.STDOUT,
                              env=env, timeout=timeout)
    return proc.returncode


def write_exit_json(output_dir: str, record: Dict[str, Any]) -> None:
    record.setdefault("timestamp_utc", utc_now())
    path = os.path.join(output_dir, "exit.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(record, f, indent=2, sort_keys=True)
        f.write("\n")


def fail_early(message: str, record: Dict[str, Any]) -> int:
    """Write exit.json only into an output dir this run created, else stderr.

    A pre-existing output directory may hold a prior run's evidence. A rejected
    invocation must never overwrite (or create files in) it, so evidence is
    written only when ``output_dir_created_by_runner`` is set -- i.e. only after
    this run created a fresh directory and owns it.
    """
    record["status"] = record.get("status", "tooling_failure")
    record["error"] = message
    print("RUNNER_ERROR: %s" % message, file=sys.stderr)
    out = record.get("output_dir")
    if record.get("output_dir_created_by_runner") and out and os.path.isdir(out):
        write_exit_json(out, record)
    return EXIT_TOOLING


def parse_probe_log(log_text: str) -> Dict[str, Any]:
    cases: List[Dict[str, Any]] = []
    summaries: List[Dict[str, int]] = []
    for line in log_text.splitlines():
        m = CASE_RE.match(line.strip())
        if m:
            cases.append({"case": int(m.group("case")),
                          "name": m.group("name"),
                          "result": m.group("result"),
                          "detail": m.group("detail")})
            continue
        m = SUMMARY_RE.match(line.strip())
        if m:
            summaries.append({k: int(v) for k, v in m.groupdict().items()})
    return {"cases": cases, "summaries": summaries}


def classify(cases: List[Dict[str, Any]], summaries: List[Dict[str, int]],
             probe_rc: int) -> Dict[str, Any]:
    """Return a classification dict; never yields a pass without six valid cases.

    The verdict is derived from the actual ``PROBE_CASE`` result rows, not from
    the summary counters: there must be exactly one ``PROBE_SUMMARY`` line, the
    six case ids must be exactly 1..6, the summary's total/passed/failed must
    equal the row counts, and every row must be PASS for a candidate pass (with
    probe rc 0 and zero setup errors).
    """
    case_numbers = sorted(c["case"] for c in cases)
    pass_rows = sum(1 for c in cases if c["result"] == "PASS")
    fail_rows = sum(1 for c in cases if c["result"] == "FAIL")
    summary = summaries[0] if len(summaries) == 1 else None
    setup_errors = summary["setup_errors"] if summary is not None else -1

    ids_ok = len(cases) == 6 and case_numbers == REQUIRED_CASE_NUMBERS
    row_counts_agree = (
        summary is not None
        and summary["total"] == len(cases)
        and summary["passed"] == pass_rows
        and summary["failed"] == fail_rows
    )
    six_ok = len(summaries) == 1 and ids_ok and row_counts_agree

    result: Dict[str, Any] = {
        "case_count": len(cases),
        "case_numbers": case_numbers,
        "case_pass_rows": pass_rows,
        "case_fail_rows": fail_rows,
        "summary_count": len(summaries),
        "row_counts_agree": row_counts_agree,
        "summary": summary,
        "six_case_contract_met": six_ok,
        "setup_errors": setup_errors,
    }

    if not six_ok:
        result.update(status="incomplete_summary", exit_code=EXIT_INCOMPLETE,
                      pass_=False,
                      reason="need exactly one summary with case ids 1..6 and "
                             "row counts agreeing with the summary; never a pass")
        return result
    if probe_rc == 2 or setup_errors > 0:
        result.update(status="setup_failure", exit_code=EXIT_SETUP_FAILURE,
                      pass_=False,
                      reason="probe reported setup errors; investigate harness")
        return result
    if probe_rc == 0:
        if pass_rows == 6 and fail_rows == 0 and setup_errors == 0:
            result.update(status="candidate_pass", exit_code=EXIT_CANDIDATE_PASS,
                          pass_=True,
                          reason="all six case rows PASS with zero setup errors")
        else:
            result.update(status="unexpected_failure", exit_code=EXIT_UNEXPECTED,
                          pass_=False,
                          reason="probe exit 0 but case rows are not all PASS")
        return result
    if probe_rc == 3:
        result.update(status="baseline_failure", exit_code=EXIT_BASELINE_FAILURE,
                      pass_=False,
                      reason="six valid cases, >=1 FAIL; baseline synchronous behavior, not a pass")
        return result
    result.update(status="unexpected_failure", exit_code=EXIT_UNEXPECTED,
                  pass_=False, reason="unexpected probe exit code %d" % probe_rc)
    return result


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="run-gtk-menu-probe.py",
        description="Build and run the in-process macOS NSMenu protocol probe "
                    "(protocol evidence only, not a real menu-bar click).",
    )
    p.add_argument("--gtk-prefix", required=True,
                   help="GTK prefix containing include/ and lib/pkgconfig (headers/pkgconfig)")
    p.add_argument("--lib-dir", required=True,
                   help="exact runtime dylib directory containing %s" % DYLD_NAME)
    p.add_argument("--output", required=True,
                   help="fresh out-of-source evidence directory (must not already exist)")
    p.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                   help="probe wall-clock timeout seconds (default %g)" % DEFAULT_TIMEOUT)
    return p


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    record: Dict[str, Any] = {
        "runner": "run-gtk-menu-probe.py",
        "checkout_probe_src": PROBE_SRC,
        "gtk_prefix": args.gtk_prefix,
        "lib_dir": args.lib_dir,
        "output_dir": args.output,
        "timeout_seconds": args.timeout,
        "gui_caveat": ("in-process NSMenu performActionForItemAtIndex: is protocol "
                       "evidence, not an actual menu-bar click"),
    }

    if platform.system() != "Darwin":
        return fail_early("Darwin only (platform=%s)" % platform.system(), record)

    # --- cheap precondition checks before creating the output directory ------
    if not os.path.isfile(PROBE_SRC):
        return fail_early("missing probe source: %s" % PROBE_SRC, record)
    if not os.path.isdir(args.gtk_prefix):
        return fail_early("GTK prefix is not a directory: %s" % args.gtk_prefix, record)
    pkgconfig_dir = os.path.join(args.gtk_prefix, "lib", "pkgconfig")
    if not os.path.isdir(pkgconfig_dir):
        return fail_early("GTK prefix has no lib/pkgconfig: %s" % pkgconfig_dir, record)
    if not os.path.isdir(args.lib_dir):
        return fail_early("lib dir is not a directory: %s" % args.lib_dir, record)
    dylib_path = os.path.join(args.lib_dir, DYLD_NAME)
    if not os.path.isfile(dylib_path):
        return fail_early("no %s in lib dir %s (candidate runtime bundle required)"
                          % (DYLD_NAME, args.lib_dir), record)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        return fail_early("--timeout must be a finite positive number", record)

    if os.path.exists(args.output):
        return fail_early("output dir already exists; pass a fresh path: %s" % args.output,
                          record)
    try:
        os.makedirs(args.output, exist_ok=False)
    except OSError as exc:
        record["output_dir"] = None
        return fail_early("cannot create output dir %s: %s" % (args.output, exc), record)
    # From here the runner owns a fresh output dir and may write evidence into it.
    record["output_dir_created_by_runner"] = True

    env = os.environ.copy()
    env["PKG_CONFIG_PATH"] = (pkgconfig_dir + os.pathsep + env["PKG_CONFIG_PATH"]
                              if env.get("PKG_CONFIG_PATH") else pkgconfig_dir)

    pkg_config = shutil.which("pkg-config")
    clang = shutil.which("clang")
    record["pkg_config"] = pkg_config
    record["clang"] = clang
    record["gtk_prefix_pkgconfig"] = pkgconfig_dir
    if pkg_config is None:
        return fail_early("pkg-config not found on PATH", record)
    if clang is None:
        return fail_early("clang not found on PATH", record)

    # --- resolve pkg-config gtk4 --------------------------------------------
    try:
        modversion = subprocess.run([pkg_config, "--modversion", "gtk4"],
                                    capture_output=True, text=True, env=env, check=True)
        prefix_raw = subprocess.run([pkg_config, "--variable=prefix", "gtk4"],
                                    capture_output=True, text=True, env=env, check=True)
        cflags_raw = subprocess.run([pkg_config, "--cflags", "gtk4"],
                                    capture_output=True, text=True, env=env, check=True)
        libs_raw = subprocess.run([pkg_config, "--libs", "gtk4"],
                                  capture_output=True, text=True, env=env, check=True)
    except subprocess.CalledProcessError as exc:
        return fail_early("pkg-config gtk4 failed under PKG_CONFIG_PATH=%s: rc=%s %s"
                          % (pkgconfig_dir, exc.returncode, (exc.stderr or "").strip()),
                          record)
    cflags = shlex.split(cflags_raw.stdout)
    libs = shlex.split(libs_raw.stdout)
    resolved_prefix = prefix_raw.stdout.strip()
    record["gtk4_version"] = modversion.stdout.strip()
    record["pkg_config_cflags"] = cflags
    record["pkg_config_libs"] = libs
    record["gtk_prefix_resolved_by_pkgconfig"] = resolved_prefix

    # The headers/libs must come from the supplied prefix, not some other
    # pkg-config entry on PATH or a stale .pc that points elsewhere.
    supplied_prefix = os.path.realpath(args.gtk_prefix)
    if not resolved_prefix:
        return fail_early("pkg-config gtk4 has no prefix variable under PKG_CONFIG_PATH=%s"
                          % pkgconfig_dir, record)
    if os.path.realpath(resolved_prefix) != supplied_prefix:
        return fail_early("pkg-config gtk4 resolves prefix %s, not supplied prefix %s"
                          % (resolved_prefix, supplied_prefix), record)

    # --- compile -------------------------------------------------------------
    binary_path = os.path.join(args.output, BINARY_NAME)
    compile_argv = ([clang, "-g", "-O0", "-fno-objc-arc", "-o", binary_path, PROBE_SRC]
                    + cflags
                    + ["-L" + args.lib_dir] + libs
                    + ["-Wl,-rpath," + args.lib_dir,
                       "-framework", "AppKit", "-framework", "Foundation"])
    record["compile_argv"] = compile_argv
    compile_log = os.path.join(args.output, "compile.log")
    try:
        compile_rc = run_capture(compile_argv, env, compile_log)
    except OSError as exc:
        compile_rc = -1
        with open(compile_log, "a", encoding="utf-8") as f:
            f.write("runner compile OSError: %s\n" % exc)
    record["compile_returncode"] = compile_rc

    if compile_rc != 0 or not os.path.isfile(binary_path):
        # Never run a stale/half-written binary from a failed compile. Preserve
        # whatever clang wrote as compile evidence; the fresh output dir already
        # rules out a stale binary from an earlier run.
        record["probe_binary_preserved"] = os.path.isfile(binary_path)
        record.update(status="compile_failure", exit_code=EXIT_TOOLING, pass_=False,
                      reason="clang returned %s; probe not run" % compile_rc)
        write_exit_json(args.output, record)
        print("COMPILE_FAILED rc=%s see %s" % (compile_rc, compile_log), file=sys.stderr)
        return EXIT_TOOLING

    record["probe_binary_sha256"] = sha256_file(binary_path)
    record["dylib_path"] = dylib_path
    record["dylib_sha256"] = sha256_file(dylib_path)

    # --- run -----------------------------------------------------------------
    run_env = dict(env)
    prev_dyld = run_env.get("DYLD_LIBRARY_PATH", "")
    run_env["DYLD_LIBRARY_PATH"] = (args.lib_dir + os.pathsep + prev_dyld
                                    if prev_dyld else args.lib_dir)
    run_env.setdefault("VACARDS_MACOS_MENU_TRACE", "1")
    run_log = os.path.join(args.output, "run.log")
    record["run_argv"] = [binary_path]
    record["run_env_dyld_library_path"] = run_env["DYLD_LIBRARY_PATH"]
    record["run_env_menu_trace"] = run_env.get("VACARDS_MACOS_MENU_TRACE")

    timed_out = False
    try:
        probe_rc = run_capture([binary_path], run_env, run_log, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        probe_rc = None
        with open(run_log, "a", encoding="utf-8") as f:
            f.write("\nRUNNER: TIMEOUT after %gs\n" % args.timeout)
    record["run_returncode"] = probe_rc

    if timed_out:
        record.update(status="timeout", exit_code=EXIT_TIMEOUT, pass_=False,
                      reason="probe exceeded %gs; not a pass" % args.timeout)
        write_exit_json(args.output, record)
        print("TIMEOUT after %gs (see %s)" % (args.timeout, run_log), file=sys.stderr)
        return EXIT_TIMEOUT

    with open(run_log, "r", encoding="utf-8", errors="replace") as f:
        log_text = f.read()
    parsed = parse_probe_log(log_text)
    record["cases"] = parsed["cases"]
    record["summaries"] = parsed["summaries"]
    result = classify(parsed["cases"], parsed["summaries"], probe_rc)
    record.update(result)
    write_exit_json(args.output, record)

    print("status=%s probe_exit=%s cases=%d summary=%s"
          % (result["status"], probe_rc, result["case_count"],
             result["summary"] if result["summary"] else "<none>"))
    print("evidence=%s" % args.output)
    return int(result["exit_code"])


if __name__ == "__main__":
    sys.exit(main())
