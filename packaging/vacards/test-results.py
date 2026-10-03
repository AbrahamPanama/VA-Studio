#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Freeze/verify CTest suite evidence; never run tests or issue an attestation.

Caller prerequisites: independently authenticate the fixed, tracked WP-00.6
baseline/signature, and run gate-run.py verify before both operations. Supply
--baseline-identity JSON with exactly schema_version=1, baseline_sha256,
signature_sha256, verifier_identity, verifier_sha256. This is a TRUSTED CALLER
INPUT, not a signature or an authentication mechanism. This helper cannot prove
who supplied it, Git tracking, or approved signer/governance ancestry.

freeze-selection --ledger BUILD/VACARDS-GATE-RUN.json --baseline-identity FILE
                 --suite SUITE --enumeration EVIDENCE/STEM.json
verify-result    --ledger BUILD/VACARDS-GATE-RUN.json --baseline-identity FILE
                 --suite SUITE --ctest-exit-code INTEGER
verify-closure   --ledger BUILD/VACARDS-GATE-RUN.json --baseline-identity FILE

verify-closure is read-only, revalidates exactly the mode's scheduled records,
and prints a versioned JSON structural closure. It does not authenticate the
caller or supply WP-08 acceptance/predicate proof; it never authorizes packaging.

STEM is critical-tests, native-nesting-tests, actionable-tests,
portfolio-differential, or portfolio-differential-60s. The existing run ledger
owns EVIDENCE=BUILD/vacards-release-evidence. Freeze consumes the fixed
STEM.selection.json exclusively, before CTest writes STEM.xml and STEM.log.
Verify consumes STEM.result.json exclusively, even on rejection. Never remove
or rewrite these files: interruption/failure consumes that suite in that run.
Results bind the existing run UUID, source SHA, ledger/configuration hashes,
baseline identity, enumeration and raw output hashes. No second run ledger.

The caller MUST execute exactly once between freeze/verify, serially, without
CTest repeat/rerun-failed options, with the same selection and opt-in environment.
JUnit does not authenticate execution history: replacing outputs before verify
cannot be detected here. Protected orchestration/retention remains necessary.
All scheduled results must pass; another suite cannot repair a failed result.
Cross-suite repeats are intentional, keyed by (run_id, suite_id, test_name).

This implements the macOS WP-01.4 actionable MINIMUM plus observed additions;
it does not implement Windows authenticated additions/exclusions. It also does
not equate grouped CTest success with executed VA-A assertions, or emit the
WP-08 acceptance-ID/randomized/portfolio-predicate inventories. Those require
separate trusted assertion/comparator evidence. Missing approved baseline
signatures remain a release blocker, never a reason to learn a baseline here.
Python 3.10+, standard library only. Keep all input/output paths quiescent.
"""

import argparse
from datetime import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat
import sys
import unicodedata
import uuid
import xml.etree.ElementTree as ET


SUITES = {
    "critical": "critical-tests",
    "native_nesting": "native-nesting-tests",
    "actionable": "actionable-tests",
    "portfolio_short": "portfolio-differential",
    "portfolio_60s": "portfolio-differential-60s",
}
NATIVE = sorted([
    "vacards-nesting-abi-smoke-c", "vacards-nesting-abi-smoke-cpp",
    "vacards-nesting-cpp-contract", "vacards-nesting-differential-selfcheck",
    "vacards-nesting-job-contract-c", "vacards-nesting-randomized-feasibility",
    "vacards-nesting-rust-unit",
])
PORTFOLIOS = {suite: sorted(f"vacards-nesting-portfolio-differential-{n}ms" for n in budgets)
              for suite, budgets in (("portfolio_short", (100, 1000, 5000)),
                                     ("portfolio_60s", (60000,)))}
OPT_INS = {"portfolio_short": "VACARDS_NESTING_RUN_PORTFOLIO_DIFFERENTIAL",
           "portfolio_60s": "VACARDS_NESTING_RUN_PORTFOLIO_DIFFERENTIAL_60S"}
SETS = ("critical", "native_nesting", "preport_actionable", "required_automated")
BASELINE_FIELDS = {
    "schema_version", "observed_source_commit", "windows_actionable_exclusions",
    "windows_actionable_exclusion_count", "windows_actionable_exclusion_set_sha256",
    "required_acceptance_ids", "required_acceptance_id_set_sha256",
    "focused_case_mappings", "focused_case_mappings_sha256",
} | {prefix + suffix for prefix in SETS
     for suffix in ("_test_names", "_test_count", "_test_name_set_sha256")}
IDENTITY_FIELDS = {"schema_version", "baseline_sha256", "signature_sha256",
                   "verifier_identity", "verifier_sha256"}
LEDGER_FIELDS = {"schema_version", "purpose", "run_uuid", "created_utc", "inputs",
                 "root_ninja_log_at_claim", "configuration_sha256", "outputs", "record_sha256"}


class Rejected(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise Rejected(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def canonical(value, *, ledger=False):
    # Baseline schema only has strings, arrays, objects and bounded integers;
    # this is NOT a general RFC 8785 floating-point serializer.
    def ordered(item):
        if type(item) is dict:
            return {k: ordered(item[k]) for k in sorted(item, key=lambda k: k.encode("utf-16-be"))}
        if type(item) is list:
            return [ordered(x) for x in item]
        require(type(item) in (str, int, bool, type(None)), "unsupported canonical JSON value")
        if type(item) is int:
            require(abs(item) <= 9007199254740991, "integer exceeds exact JSON range")
        return item
    if ledger:
        return json.dumps(value, sort_keys=True, ensure_ascii=True,
                          separators=(",", ":"), allow_nan=False).encode("ascii")
    return json.dumps(ordered(value), ensure_ascii=False, separators=(",", ":"),
                      allow_nan=False).encode("utf-8")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON field: {key}")
        result[key] = value
    return result


def parse_json(data):
    return json.loads(data.decode("utf-8"), object_pairs_hook=unique_object,
                      parse_constant=reject_constant)


def reject_constant(value):
    raise Rejected(f"non-finite JSON number: {value}")


def fields(value, expected, label):
    require(type(value) is dict and set(value) == expected, f"invalid {label} fields")


def version(value, label):
    require(type(value) is int and value == 1, f"invalid {label} schema_version")


def string(value, label):
    require(type(value) is str and value and value == value.strip()
            and not any(ord(c) < 32 or ord(c) == 127 for c in value), f"invalid {label} string")
    value.encode("utf-8")  # Reject lone surrogates before hashing or path use.
    return value


def sha(value, label, length=64):
    require(type(value) is str and re.fullmatch(r"[0-9a-f]{%d}" % length, value),
            f"invalid {label} hash")


def names(values, label, *, sorted_input=False, allow_empty=False):
    require(type(values) is list and (values or allow_empty), f"empty/invalid {label} names")
    aliases = set()
    for value in values:
        string(value, label)
        alias = unicodedata.normalize("NFKC", value).casefold()
        require(alias not in aliases, f"duplicate/normalized alias in {label}: {value}")
        aliases.add(alias)
    result = sorted(values, key=lambda s: s.encode("utf-8"))
    if sorted_input:
        require(values == result, f"unsorted {label} names")
    return result


def name_hash(values):
    return digest(("\n".join(values) + "\n").encode("utf-8"))


def path(value, *, directory=False):
    string(str(value), "path")
    result = Path(value)
    require(result.is_absolute() and ".." not in result.parts, "paths must be absolute without traversal")
    for part in (result, *result.parents):
        info = part.lstat()
        require(not stat.S_ISLNK(info.st_mode) and not getattr(info, "st_file_attributes", 0) & 0x400,
                f"symlink/reparse path rejected: {part}")
    info = result.stat()
    require(stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode),
            f"wrong path type: {result}")
    if not directory:
        require(info.st_nlink == 1, f"hard-linked evidence/input rejected: {result}")
    return result


def read(value):
    return path(value).read_bytes()


def file_identity(value):
    info = value.stat()
    return {"device": info.st_dev, "inode": info.st_ino}


def load_run(ledger_path, suite):
    ledger_path = path(ledger_path)
    raw = read(ledger_path)
    record = parse_json(raw)
    fields(record, LEDGER_FIELDS, "run ledger")
    version(record["schema_version"], "run ledger")
    require(record["purpose"] == "qualification-input-reservation"
            and record["root_ninja_log_at_claim"] == "absent", "invalid ledger purpose/history")
    run = uuid.UUID(string(record["run_uuid"], "run UUID"))
    require(run.version == 4 and str(run) == record["run_uuid"], "invalid run UUID")
    created = string(record["created_utc"], "creation time")
    require(datetime.strptime(created, "%Y-%m-%dT%H:%M:%SZ").strftime("%Y-%m-%dT%H:%M:%SZ") == created,
            "invalid ledger creation time")
    require(record["record_sha256"] == digest(canonical(
        {k: v for k, v in record.items() if k != "record_sha256"}, ledger=True)), "ledger checksum mismatch")
    inputs = record["inputs"]
    require(type(inputs) is dict and type(inputs.get("source")) is dict, "invalid ledger inputs/source")
    require(record["configuration_sha256"] == digest(canonical(inputs, ledger=True)),
            "ledger configuration checksum mismatch")
    build = path(string(inputs.get("build"), "build"), directory=True)
    require(ledger_path == build / "VACARDS-GATE-RUN.json", "ledger must use fixed build path")
    require(inputs.get("build_directory") == file_identity(build), "ledger build directory replaced")
    require(inputs.get("mode") in ("quick", "full"), "invalid ledger mode")
    require(inputs["mode"] == "full" or suite in ("critical", "native_nesting"),
            "quick mode cannot select actionable/portfolio suites")
    source = path(string(inputs["source"].get("path"), "source"), directory=True)
    commit = inputs["source"].get("head")
    sha(commit, "source commit", 40)
    evidence = path(build / "vacards-release-evidence", directory=True)
    require(type(record["outputs"]) is dict
            and record["outputs"].get("evidence") == {"path": str(evidence), **file_identity(evidence)},
            "ledger evidence directory replaced")
    binding = {"run_id": str(run), "source_commit": commit, "mode": inputs["mode"],
               "ledger_sha256": digest(raw), "configuration_sha256": record["configuration_sha256"]}
    return source, evidence, binding


def load_baseline(source, identity_path):
    identity_raw = read(identity_path)
    identity = parse_json(identity_raw)
    fields(identity, IDENTITY_FIELDS, "authenticated baseline identity")
    version(identity["schema_version"], "baseline identity")
    string(identity["verifier_identity"], "verifier identity")
    for field in ("baseline_sha256", "signature_sha256", "verifier_sha256"):
        sha(identity[field], field)
    baseline_path = source / "doc/vacards/WINDOWS_PORT_TEST_BASELINE.json"
    signature_path = baseline_path.with_suffix(".json.p7s")
    require(baseline_path.exists() and signature_path.exists(),
            "release blocked: approved baseline/signature unavailable at fixed source paths; "
            "obtain independently authenticated WP-00.6 inputs, never generate expectations from candidate tests")
    raw, signature = read(baseline_path), read(signature_path)
    require(signature and digest(signature) == identity["signature_sha256"], "baseline signature identity mismatch")
    require(digest(raw) == identity["baseline_sha256"], "baseline identity mismatch")
    baseline = parse_json(raw)
    fields(baseline, BASELINE_FIELDS, "WP-00.6 baseline")
    version(baseline["schema_version"], "baseline")
    sha(baseline["observed_source_commit"], "baseline source commit", 40)
    require(raw == canonical(baseline), "baseline must be strict RFC 8785 canonical JSON")
    for prefix in SETS:
        values = names(baseline[prefix + "_test_names"], prefix, sorted_input=True)
        count = baseline[prefix + "_test_count"]
        require(type(count) is int and count == len(values), f"{prefix} count mismatch")
        require(baseline[prefix + "_test_name_set_sha256"] == name_hash(values), f"{prefix} set hash mismatch")
    critical, native = baseline["critical_test_names"], baseline["native_nesting_test_names"]
    require(len(critical) >= 48, "approved baseline critical floor is below 48")
    require(native == NATIVE, "baseline must contain exactly the seven native tests")
    combined = names(critical + native, "critical/native union")
    require(combined == baseline["required_automated_test_names"], "baseline critical/native union mismatch")
    require(set(combined) <= set(baseline["preport_actionable_test_names"]),
            "baseline actionable floor omits required automated tests")
    exclusions = baseline["windows_actionable_exclusions"]
    require(type(exclusions) is list and all(type(row) is dict for row in exclusions), "invalid Windows exclusions")
    require(type(baseline["windows_actionable_exclusion_count"]) is int
            and baseline["windows_actionable_exclusion_count"] == len(exclusions), "Windows exclusion count mismatch")
    require(baseline["windows_actionable_exclusion_set_sha256"] == digest(canonical(exclusions)),
            "Windows exclusion hash mismatch")
    # Exclusion row semantics belong to the Windows verifier. No exclusion is
    # applied to this macOS minimum, regardless of its reason/platform.
    ids = names(baseline["required_acceptance_ids"], "acceptance IDs", sorted_input=True)
    require(all(re.fullmatch(r"VA-A[0-9]{4}", value) for value in ids), "invalid acceptance ID")
    require(baseline["required_acceptance_id_set_sha256"] == name_hash(ids), "acceptance ID hash mismatch")
    mappings = baseline["focused_case_mappings"]
    require(type(mappings) is list, "invalid focused case mappings")
    for row in mappings:
        fields(row, {"acceptance_id", "feature_id", "test_name", "registration_file", "source_file",
                     "predicate", "status"}, "focused case mapping")
        for key, value in row.items():
            string(value, f"mapping {key}")
        require(row["status"] in ("active", "tombstone"), "invalid mapping status")
    require([row["acceptance_id"] for row in mappings] == ids, "missing/duplicate/unsorted mapped acceptance ID")
    require(baseline["focused_case_mappings_sha256"] == digest(canonical(mappings)), "focused mapping hash mismatch")
    require(set(combined) <= {row["test_name"] for row in mappings if row["status"] == "active"},
            "required test has no active baseline mapping")
    return baseline, {**identity, "identity_file_sha256": digest(identity_raw)}


def enumeration(raw):
    data = parse_json(raw)
    require(type(data) is dict and data.get("kind") == "ctestInfo", "invalid CTest enumeration kind")
    version_data = data.get("version")
    require(type(version_data) is dict and type(version_data.get("major")) is int
            and version_data["major"] == 1 and type(version_data.get("minor")) is int
            and version_data["minor"] == 0, "unsupported CTest enumeration version")
    tests = data.get("tests")
    require(type(tests) is list and tests, "empty/invalid CTest selection")
    values = []
    for test in tests:
        require(type(test) is dict, "invalid enumerated test")
        values.append(test.get("name"))
        command = test.get("command")
        require(type(command) is list and command and all(type(arg) is str for arg in command),
                f"missing executable command for {test.get('name')}")
        properties = test.get("properties", [])
        require(type(properties) is list, "invalid CTest properties")
        seen = set()
        for prop in properties:
            require(type(prop) is dict and type(prop.get("name")) is str and "value" in prop,
                    "invalid CTest property")
            require(prop["name"] not in seen, "duplicate CTest property")
            seen.add(prop["name"])
            if prop["name"] == "DISABLED":
                require(prop["value"] is False, f"disabled test: {test.get('name')}")
    return names(values, "enumerated")


def selected_names(baseline, suite, actual):
    if suite in PORTFOLIOS:
        required = PORTFOLIOS[suite]
    elif suite == "actionable":
        required = baseline["preport_actionable_test_names"]
    else:
        required = baseline[suite + "_test_names"]
    missing, extra = sorted(set(required) - set(actual)), sorted(set(actual) - set(required))
    require(not missing, f"missing {suite} tests: {missing}")
    if suite == "actionable":
        require(not set(actual).intersection(PORTFOLIOS["portfolio_short"] + PORTFOLIOS["portfolio_60s"]),
                "actionable selection must exclude portfolio opt-ins")
        # Reject extras which merely alias a frozen name rather than adding one.
        names(required + extra, "actionable minimum/additions")
    else:
        require(not extra, f"unexpected {suite} tests: {extra}")
    return extra


def opt_in(suite):
    if suite in OPT_INS:
        key = OPT_INS[suite]
        require(os.environ.get(key) == "1", f"{suite} requires literal {key}=1 for freeze, execution and verify")
        return {key: "1"}
    return {}


def comparator_files(evidence, stem, raw, *, retain=False):
    """Preserve available raw reports before a later suite can overwrite them.

    Missing reports are recorded explicitly, including on failed invocations.
    These are raw bytes, not a claim that the external predicates passed.
    """
    rows = []
    for test in parse_json(raw)["tests"]:
        outputs = [arg[len("-DOUTPUT_DIR="):] for arg in test["command"]
                   if arg.startswith("-DOUTPUT_DIR=")]
        if not outputs:
            continue
        require(len(outputs) == 1, "ambiguous comparator output directory")
        if retain and not rows:
            (evidence / (stem + ".comparators")).mkdir()
        original = Path(outputs[0])
        build = evidence.parent
        require(original.is_absolute() and ".." not in original.parts
                and original.is_relative_to(build) and not original.is_relative_to(evidence),
                "comparator output must be inside build, outside evidence")
        filenames = ["baseline.tsv", "portfolio.tsv", "comparison.tsv"]
        if test["name"] == "vacards-nesting-portfolio-differential-100ms":
            filenames += ["portfolio-reversed.tsv", "portfolio-input-order.tsv"]
        for filename in filenames:
            # Index destinations rather than interpolating untrusted test names.
            retained = evidence / (stem + ".comparators") / (str(len(rows)) + ".tsv")
            source = original / filename
            row = {"test_name": test["name"], "source": str(source),
                   "retained": str(retained.relative_to(evidence)), "sha256": None}
            if retain and os.path.lexists(source):
                data = read(source)
                path(retained.parent, directory=True)
                with retained.open("xb") as stream:
                    stream.write(data)
            if os.path.lexists(retained):
                row["sha256"] = digest(read(retained))
            rows.append(row)
    return rows


def junit(raw, expected, binding):
    # CTest writes UTF-8. Restrict the encoding before inspecting declarations,
    # so UTF-16 cannot hide a DTD from the declaration check.
    text = raw.decode("utf-8")
    require("\x00" not in text, "JUnit must be UTF-8 without NUL bytes")
    require("<!DOCTYPE" not in text.upper() and "<!ENTITY" not in text.upper(), "DTD/entity JUnit rejected")
    root = ET.fromstring(text)
    require(root.tag in ("testsuite", "testsuites"), "invalid JUnit root")
    suites = [root] if root.tag == "testsuite" else list(root)
    require(suites and all(node.tag == "testsuite" for node in suites), "invalid/empty JUnit suites")
    rows = []
    for node in suites:
        require(all(child.tag in ("testcase", "properties", "system-out", "system-err") for child in node),
                "unexpected/nested/retry JUnit node")
        cases = node.findall("testcase")
        summary(node, len(cases), required=True)
        for case in cases:
            name = string(case.get("name"), "JUnit test name")
            require(case.get("status") == "run", f"JUnit status is not run: {name}")
            require(case.get("result", "completed") == "completed", f"incomplete JUnit result: {name}")
            require(not set(case.attrib).intersection({"disabled", "skipped", "notrun", "retry", "retries", "attempt", "attempts"}),
                    f"unexpected attempt/disabled marker: {name}")
            require(all(child.tag in ("system-out", "system-err", "properties") for child in case),
                    f"failed/skipped/error/retry JUnit case: {name}")
            for prop in case.findall("./properties/property"):
                key = prop.get("name")
                if key in ("run_id", "source_commit", "suite_id"):
                    require(prop.get("value") == binding[key], f"mixed JUnit {key}: {name}")
            duration = float(case.get("time", "nan"))
            require(math.isfinite(duration) and duration >= 0, f"invalid JUnit duration: {name}")
            output = "".join((child.text or "") for child in case if child.tag in ("system-out", "system-err"))
            rows.append({"run_id": binding["run_id"], "suite_id": binding["suite_id"], "test_name": name,
                         "source_commit": binding["source_commit"], "attempt": 1, "enabled": True,
                         "outcome": "passed", "duration_seconds": duration,
                         "output_sha256": digest(output.encode("utf-8"))})
    if root.tag == "testsuites":
        summary(root, len(rows), required=False)
    actual = names([row["test_name"] for row in rows], "JUnit")
    require(actual == expected, f"JUnit names/cardinality differ from selection: missing={sorted(set(expected) - set(actual))}, "
            f"extra={sorted(set(actual) - set(expected))}")
    return sorted(rows, key=lambda row: row["test_name"].encode("utf-8"))


def summary(node, count, *, required):
    for field in ("tests", "failures", "errors", "disabled", "skipped"):
        value = node.get(field)
        if value is None and not (required and field == "tests"):
            continue
        require(type(value) is str and re.fullmatch(r"0|[1-9][0-9]*", value), f"invalid JUnit {field} count")
        require(int(value) == (count if field == "tests" else 0), f"JUnit {field} count mismatch/nonzero")


def record_bytes(value):
    # Result durations are JSON numbers, not claimed to be RFC 8785 records.
    return json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(",", ":"),
                      allow_nan=False).encode("utf-8") + b"\n"


def write_record(stream, value):
    stream.write(record_bytes(value))
    stream.flush()
    os.fsync(stream.fileno())


def execute(args):
    source, evidence, binding = load_run(args.ledger, args.suite)
    stem = SUITES[args.suite]
    binding["suite_id"] = args.suite
    selection_path = evidence / (stem + ".selection.json")
    output_path = evidence / (stem + (".selection.json" if args.command == "freeze-selection" else ".result.json"))
    # Exclusive fixed output is the suite/verification reservation. Preserve a
    # partial file if interrupted. Never offer --force or an alternate output.
    try:
        stream = output_path.open("xb")
    except FileExistsError:
        raise Rejected(f"suite already consumed; retry/reused output rejected: {output_path}") from None
    record = {"schema_version": 1, "purpose": args.command, **binding, "outcome": "rejected",
              "helper_sha256": digest(read(Path(__file__).resolve()))}
    with stream:
        try:
            baseline, identity = load_baseline(source, args.baseline_identity)
            record["baseline_identity"] = identity
            record["opt_in_environment"] = opt_in(args.suite)
            enum_path = evidence / (stem + ".json")
            xml_path, log_path = evidence / (stem + ".xml"), evidence / (stem + ".log")
            if args.command == "freeze-selection":
                require(path(args.enumeration) == enum_path, "enumeration must use fixed suite evidence path")
                require(not os.path.lexists(xml_path) and not os.path.lexists(log_path),
                        "result/log already exists before selection; stale output rejected")
            raw = read(enum_path)
            actual = enumeration(raw)
            extras = selected_names(baseline, args.suite, actual)
            record.update(test_names=actual, test_count=len(actual), test_name_set_sha256=name_hash(actual),
                          additions=extras, additions_count=len(extras), additions_name_set_sha256=name_hash(extras),
                          enumeration_sha256=digest(raw))
            if args.command == "freeze-selection":
                record["outcome"] = "selected"
            else:
                selection_raw = read(selection_path)
                expected_selection = {**record, "purpose": "freeze-selection", "outcome": "selected"}
                require(selection_raw == record_bytes(expected_selection),
                        "selection/run/baseline/enumeration binding changed or selection rejected")
                record["selection_sha256"] = digest(selection_raw)
                record["ctest_exit_code"] = args.ctest_exit_code
                record["comparator_files"] = comparator_files(evidence, stem, raw, retain=True)
                xml, log = read(xml_path), read(log_path)
                record.update(junit_sha256=digest(xml), log_sha256=digest(log), ctest_exit_code=args.ctest_exit_code)
                require(type(args.ctest_exit_code) is int and args.ctest_exit_code == 0, "CTest exited nonzero")
                require(log.strip(), "empty CTest log")
                record["executions"] = junit(xml, actual, binding)
                record["outcome"] = "passed"
        except (ValueError, OSError, TypeError, KeyError, AttributeError, RecursionError, ET.ParseError) as error:
            record["outcome"] = "rejected"
            record["error"] = str(error)
            write_record(stream, record)
            raise Rejected(str(error)) from None
        write_record(stream, record)
    return output_path


def verify_closure(args):
    source, evidence, binding = load_run(args.ledger, "critical")
    baseline, identity = load_baseline(source, args.baseline_identity)
    scheduled = tuple(SUITES) if binding["mode"] == "full" else ("critical", "native_nesting")
    expected_files = {SUITES[suite] + suffix for suite in scheduled
                      for suffix in (".selection.json", ".result.json")}
    actual_files = {item.name for item in evidence.iterdir()
                    if item.name.endswith((".selection.json", ".result.json"))}
    require(actual_files == expected_files, "closure requires exactly the scheduled selection/result records")
    suites = []
    for suite in scheduled:
        stem = SUITES[suite]
        raw = read(evidence / (stem + ".json"))
        actual = enumeration(raw)
        extras = selected_names(baseline, suite, actual)
        selected = {"schema_version": 1, "purpose": "freeze-selection", **binding,
                    "helper_sha256": digest(read(Path(__file__).resolve())),
                    "suite_id": suite, "outcome": "selected", "baseline_identity": identity,
                    "opt_in_environment": {OPT_INS[suite]: "1"} if suite in OPT_INS else {},
                    "test_names": actual, "test_count": len(actual), "test_name_set_sha256": name_hash(actual),
                    "additions": extras, "additions_count": len(extras), "additions_name_set_sha256": name_hash(extras),
                    "enumeration_sha256": digest(raw)}
        selection = read(evidence / (stem + ".selection.json"))
        require(selection == record_bytes(selected), "closure selection/run/baseline binding changed")
        xml, log = read(evidence / (stem + ".xml")), read(evidence / (stem + ".log"))
        require(log.strip(), "empty CTest log at closure")
        expected = {**selected, "purpose": "verify-result", "outcome": "passed",
                    "selection_sha256": digest(selection), "junit_sha256": digest(xml),
                    "log_sha256": digest(log), "ctest_exit_code": 0,
                    "comparator_files": comparator_files(evidence, stem, raw),
                    "executions": junit(xml, actual, {**binding, "suite_id": suite})}
        result = read(evidence / (stem + ".result.json"))
        require(result == record_bytes(expected),
                f"closure {suite} result rejected, stale, mismatched or unsupported")
        suites.append({"suite_id": suite, "result_file": stem + ".result.json",
                       "result_sha256": digest(result),
                       **{key: expected[key] for key in (
                           "test_names", "test_count", "test_name_set_sha256", "additions",
                           "additions_count", "additions_name_set_sha256", "enumeration_sha256",
                           "selection_sha256", "junit_sha256", "log_sha256", "comparator_files")}})
    return {"schema_version": 1, "purpose": "verify-closure", "outcome": "passed",
            "authority": "structural-suite-evidence-only", **binding,
            "baseline_identity": identity, "helper_sha256": digest(read(Path(__file__).resolve())),
            "suites": suites}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    for command in ("freeze-selection", "verify-result"):
        sub = commands.add_parser(command)
        sub.add_argument("--ledger", required=True)
        sub.add_argument("--baseline-identity", required=True)
        sub.add_argument("--suite", required=True, choices=tuple(SUITES))
        if command == "freeze-selection":
            sub.add_argument("--enumeration", required=True)
        else:
            sub.add_argument("--ctest-exit-code", required=True, type=int)
    closure = commands.add_parser("verify-closure")
    closure.add_argument("--ledger", required=True)
    closure.add_argument("--baseline-identity", required=True)
    args = parser.parse_args(argv)
    try:
        result = verify_closure(args) if args.command == "verify-closure" else execute(args)
    except (ValueError, OSError, TypeError, KeyError, AttributeError, RecursionError, ET.ParseError) as error:
        parser.exit(1, f"Rejected: {error}\n")
    if args.command == "verify-closure":
        sys.stdout.buffer.write(record_bytes(result))
    else:
        print(f"{args.command}: {result} (suite evidence only; no release attestation)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
