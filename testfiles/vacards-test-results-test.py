#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic suite-evidence tests only: no CMake, CTest, app, Git or signatures.

The fake authentication receipt is a trusted-caller fixture, NOT a signature
verification test or proof that any release baseline has been approved. The
cross-helper test requires gate-run.py beside test-results.py, or its explicit
path in VACARDS_GATE_RUN_HELPER during integration. It uses a real claim/verify
on a temporary Git fixture with inert compiler/Ninja files; no app is built.
"""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import uuid
import xml.etree.ElementTree as ET


HELPER = Path(__file__).resolve().parents[1] / "packaging/vacards/test-results.py"
sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("vacards_test_results", HELPER)
POLICY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(POLICY)
MISSING_FILE_ERROR = "[WinError 2]" if os.name == "nt" else "[Errno 2]"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def set_hash(values):
    return sha(("\n".join(values) + "\n").encode("utf-8"))


def identity(directory):
    info = directory.stat()
    return {"device": info.st_dev, "inode": info.st_ino}


class ResultsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="vacards test results ")
        self.addCleanup(self.temporary.cleanup)
        # Resolve macOS /var -> /private/var once; production rejects aliases.
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / "source with spaces"
        self.docs = self.source / "doc/vacards"
        self.docs.mkdir(parents=True)
        self.build = self.root / "build"
        self.evidence = self.build / "vacards-release-evidence"
        self.evidence.mkdir(parents=True)
        self.ledger_path = self.build / "VACARDS-GATE-RUN.json"
        self.identity_path = self.root / "authenticated-identity.json"
        self.baseline_path = self.docs / "WINDOWS_PORT_TEST_BASELINE.json"
        self.signature_path = self.docs / "WINDOWS_PORT_TEST_BASELINE.json.p7s"
        self.signature_path.write_bytes(b"SYNTHETIC detached signature, not authenticated")
        self.critical = [f"critical-{n:02}" for n in range(48)]
        self.native = list(POLICY.NATIVE)
        self.automated = sorted(self.critical + self.native)
        self.actionable = sorted(self.automated + ["extra-upstream-case"])
        self.baseline = {"schema_version": 1, "observed_source_commit": "a" * 40}
        for prefix, names in (("critical", self.critical), ("native_nesting", self.native),
                              ("preport_actionable", self.actionable), ("required_automated", self.automated)):
            self.baseline.update({prefix + "_test_names": names, prefix + "_test_count": len(names),
                                  prefix + "_test_name_set_sha256": set_hash(names)})
        ids = [f"VA-A{n:04}" for n in range(1, len(self.automated) + 1)]
        mappings = [{"acceptance_id": identifier, "feature_id": "synthetic-feature",
                     "test_name": name, "registration_file": "testfiles/CMakeLists.txt",
                     "source_file": "testfiles/synthetic.cpp", "predicate": "synthetic assertion",
                     "status": "active"} for identifier, name in zip(ids, self.automated)]
        self.baseline.update(windows_actionable_exclusions=[], windows_actionable_exclusion_count=0,
                             windows_actionable_exclusion_set_sha256=sha(b"[]"), required_acceptance_ids=ids,
                             required_acceptance_id_set_sha256=set_hash(ids), focused_case_mappings=mappings,
                             focused_case_mappings_sha256=sha(encoded(mappings)))
        self.auth = {"schema_version": 1, "baseline_sha256": "0" * 64,
                     "signature_sha256": sha(self.signature_path.read_bytes()),
                     "verifier_identity": "SYNTHETIC external verifier", "verifier_sha256": "b" * 64}
        self.save_baseline()
        self.ledger = {
            "schema_version": 1, "purpose": "qualification-input-reservation",
            "run_uuid": str(uuid.uuid4()), "created_utc": "2026-09-04T12:00:00Z",
            "root_ninja_log_at_claim": "absent",
            "inputs": {"source": {"path": str(self.source), "head": "c" * 40},
                       "build": str(self.build), "build_directory": identity(self.build),
                       "mode": "full", "install": str(self.root / "stage")},
            "outputs": {"evidence": {"path": str(self.evidence), **identity(self.evidence)}, "install": None},
        }
        self.save_ledger()
        self.environment = {key: value for key, value in os.environ.items()
                            if key not in POLICY.OPT_INS.values()}
        self.environment["PYTHONDONTWRITEBYTECODE"] = "1"

    def save_baseline(self):
        self.baseline_path.write_bytes(encoded(self.baseline))
        self.auth["baseline_sha256"] = sha(self.baseline_path.read_bytes())
        self.save_auth()

    def save_auth(self):
        self.identity_path.write_bytes(encoded(self.auth))

    def save_ledger(self):
        def ledger_json(value):
            return json.dumps(value, sort_keys=True, ensure_ascii=True, separators=(",", ":")).encode("ascii")
        self.ledger["configuration_sha256"] = sha(ledger_json(self.ledger["inputs"]))
        self.ledger["record_sha256"] = sha(ledger_json({k: v for k, v in self.ledger.items() if k != "record_sha256"}))
        self.ledger_path.write_bytes(ledger_json(self.ledger) + b"\n")

    def file(self, suite, suffix):
        return self.evidence / (POLICY.SUITES[suite] + suffix)

    def expected(self, suite):
        return {"critical": self.critical, "native_nesting": self.native,
                "actionable": self.actionable, **POLICY.PORTFOLIOS}[suite]

    def enumerate(self, suite="critical", values=None):
        if values is None:
            values = self.expected(suite)
        data = {"kind": "ctestInfo", "version": {"major": 1, "minor": 0},
                "backtraceGraph": {"commands": [], "files": [], "nodes": []},
                "tests": [{"name": value, "command": ["/fixture/test", "--some-option"],
                           "properties": []} for value in values]}
        self.file(suite, ".json").write_bytes(encoded(data))
        return data

    def invoke(self, command, suite="critical", *, error=None, status=0):
        args = [sys.executable, str(HELPER), command, "--ledger", str(self.ledger_path),
                "--baseline-identity", str(self.identity_path), "--suite", suite]
        if command == "freeze-selection":
            args += ["--enumeration", str(self.file(suite, ".json"))]
        else:
            args += ["--ctest-exit-code", str(status)]
        result = subprocess.run(args, cwd=self.root, env=self.environment,
                                capture_output=True, text=True, check=False)
        if error is None:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("no release attestation", result.stdout)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Rejected:", result.stderr)
            self.assertIn(error, result.stderr)
            self.assertNotIn("Traceback", result.stderr)
        self.assertFalse((self.evidence / "VACARDS-RELEASE-GATE.env").exists())
        return result

    def freeze(self, suite="critical", values=None):
        self.enumerate(suite, values)
        self.invoke("freeze-selection", suite)

    def outputs(self, suite="critical", values=None):
        if values is None:
            values = self.expected(suite)
        root = ET.Element("testsuite", tests=str(len(values)), failures="0", disabled="0", skipped="0")
        for value in values:
            case = ET.SubElement(root, "testcase", name=value, status="run", time="0.125")
            ET.SubElement(case, "system-out").text = "synthetic passed output"
        self.save_xml(root, suite)
        self.file(suite, ".log").write_text("SYNTHETIC CTest log\n", encoding="utf-8")
        return root

    def save_xml(self, root, suite="critical"):
        self.file(suite, ".xml").write_bytes(ET.tostring(root, encoding="utf-8", xml_declaration=True))

    def record(self, suite="critical", suffix=".result.json"):
        return json.loads(self.file(suite, suffix).read_text(encoding="utf-8"))

    def test_positive_all_five_suites_and_intentional_cross_suite_repeats(self):
        for suite in POLICY.SUITES:
            with self.subTest(suite=suite):
                if suite in POLICY.OPT_INS:
                    self.environment[POLICY.OPT_INS[suite]] = "1"
                self.freeze(suite)
                self.outputs(suite)
                self.invoke("verify-result", suite)
                result = self.record(suite)
                self.assertEqual(result["outcome"], "passed")
                self.assertEqual(result["test_count"], len(self.expected(suite)))
                self.assertEqual(result["run_id"], self.ledger["run_uuid"])
                self.assertEqual(result["ledger_sha256"], sha(self.ledger_path.read_bytes()))
                self.assertEqual(result["junit_sha256"], sha(self.file(suite, ".xml").read_bytes()))
                self.assertEqual(result["baseline_identity"]["signature_sha256"], self.auth["signature_sha256"])
                self.assertTrue(all(row["attempt"] == 1 for row in result["executions"]))
        keysets = [{(row["run_id"], row["suite_id"], row["test_name"]) for row in self.record(suite)["executions"]}
                   for suite in POLICY.SUITES]
        self.assertEqual(len(set.union(*keysets)), sum(map(len, keysets)))

    def test_real_gate_run_claim_and_verify_integration(self):
        helper = Path(os.environ.get("VACARDS_GATE_RUN_HELPER", str(HELPER.with_name("gate-run.py"))))
        self.assertTrue(helper.is_absolute() and helper.is_file(),
                        "cross-helper check requires gate-run.py or explicit VACARDS_GATE_RUN_HELPER; do not skip")
        environment = {key: value for key, value in self.environment.items() if not key.startswith("GIT_")}
        environment.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull, GIT_TERMINAL_PROMPT="0")

        def run(command):
            result = subprocess.run(command, cwd=self.source, env=environment,
                                    capture_output=True, text=True, timeout=30, check=False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result

        # Only this disposable repository is initialized/committed. The actual
        # worktree and helper's source are never changed by this test.
        run(["git", "init", "-q"])
        run(["git", "config", "user.name", "Synthetic gate fixture"])
        run(["git", "config", "user.email", "fixture@example.invalid"])
        run(["git", "config", "commit.gpgsign", "false"])
        run(["git", "add", "doc"])
        run(["git", "commit", "-q", "-m", "Disposable synthetic baseline fixture"])
        self.build = self.root / "real claim build"
        self.build.mkdir()
        compiler = self.root / "inert compiler"
        compiler.write_bytes(b"Synthetic compiler identity: never execute\n")
        compiler.chmod(0o755)
        cache = {"CMAKE_HOME_DIRECTORY": ("INTERNAL", str(self.source)),
                 "CMAKE_CACHEFILE_DIR": ("INTERNAL", str(self.build)),
                 "CMAKE_GENERATOR": ("INTERNAL", "Ninja"), "CMAKE_BUILD_TYPE": ("STRING", "Release"),
                 "CMAKE_C_COMPILER": ("FILEPATH", str(compiler)),
                 "CMAKE_CXX_COMPILER": ("FILEPATH", str(compiler)),
                 "VACARDS_NESTING_RUSTC_EXECUTABLE": ("FILEPATH", str(compiler)),
                 "VACARDS_NESTING_RUST_TARGET": ("STRING", "aarch64-apple-darwin")}
        (self.build / "CMakeCache.txt").write_text(
            "".join(f"{key}:{kind}={value}\n" for key, (kind, value) in cache.items()), encoding="utf-8")
        (self.build / "build.ninja").write_bytes(b"# Synthetic; never execute\n")
        run([sys.executable, str(helper), "claim", "--source", str(self.source), "--build", str(self.build),
             "--mode", "full", "--install", str(self.root / "claimed stage")])
        self.evidence = self.build / "vacards-release-evidence"
        self.ledger_path = self.build / "VACARDS-GATE-RUN.json"
        self.ledger = json.loads(self.ledger_path.read_bytes())
        original_ledger = self.ledger_path.read_bytes()
        self.assertEqual(set(self.ledger["outputs"]["evidence"]), {"path", "device", "inode"})
        run([sys.executable, str(helper), "verify", "--build", str(self.build)])
        self.freeze()
        self.outputs()
        run([sys.executable, str(helper), "verify", "--build", str(self.build)])
        self.invoke("verify-result")
        self.assertEqual(self.record()["source_commit"], self.ledger["inputs"]["source"]["head"])
        self.assertEqual(self.record()["run_id"], self.ledger["run_uuid"])
        self.assertEqual(self.ledger_path.read_bytes(), original_ledger)
        run([sys.executable, str(helper), "verify", "--build", str(self.build)])

    def test_positive_actionable_additions_are_recorded_not_substitutions(self):
        values = self.actionable + ["new-reviewed-later-test"]
        self.freeze("actionable", values)
        self.outputs("actionable", values)
        self.invoke("verify-result", "actionable")
        self.assertEqual(self.record("actionable")["additions"], ["new-reviewed-later-test"])

    def test_positive_quick_has_only_critical_and_native(self):
        self.ledger["inputs"]["mode"] = "quick"
        self.save_ledger()
        for suite in ("critical", "native_nesting"):
            self.freeze(suite)
            self.outputs(suite)
            self.invoke("verify-result", suite)
        for suite in ("actionable", "portfolio_short", "portfolio_60s"):
            self.enumerate(suite)
            self.invoke("freeze-selection", suite, error="quick mode cannot")

    def test_missing_critical(self):
        self.enumerate(values=self.critical[:-1])
        self.invoke("freeze-selection", error="missing critical")

    def test_critical_same_count_substitution(self):
        self.enumerate(values=self.critical[:-1] + ["replacement-test"])
        self.invoke("freeze-selection", error="missing critical")

    def test_missing_native(self):
        self.enumerate("native_nesting", self.native[:-1])
        self.invoke("freeze-selection", "native_nesting", error="missing native_nesting")

    def test_extra_critical_is_not_auto_accepted(self):
        self.enumerate(values=self.critical + ["new-test"])
        self.invoke("freeze-selection", error="unexpected critical")

    def test_actionable_rename(self):
        self.enumerate("actionable", self.actionable[:-1] + ["renamed-actionable"])
        self.invoke("freeze-selection", "actionable", error="missing actionable")

    def test_actionable_cannot_apply_windows_exclusions(self):
        exclusions = [{"test_name": self.actionable[0], "reason": "synthetic", "owner": "fixture",
                       "platform_predicate": "windows", "replacement_coverage": "none"}]
        self.baseline.update(windows_actionable_exclusions=exclusions, windows_actionable_exclusion_count=1,
                             windows_actionable_exclusion_set_sha256=sha(encoded(exclusions)))
        self.save_baseline()
        self.enumerate("actionable", self.actionable[1:])
        self.invoke("freeze-selection", "actionable", error="missing actionable")

    def test_actionable_excludes_portfolios(self):
        self.enumerate("actionable", self.actionable + POLICY.PORTFOLIOS["portfolio_short"])
        self.invoke("freeze-selection", "actionable", error="must exclude portfolio")

    def test_missing_short_portfolio(self):
        self.environment[POLICY.OPT_INS["portfolio_short"]] = "1"
        self.enumerate("portfolio_short", POLICY.PORTFOLIOS["portfolio_short"][:-1])
        self.invoke("freeze-selection", "portfolio_short", error="missing portfolio_short")

    def test_missing_long_portfolio(self):
        self.environment[POLICY.OPT_INS["portfolio_60s"]] = "1"
        self.enumerate("portfolio_60s", [])
        self.invoke("freeze-selection", "portfolio_60s", error="empty/invalid CTest selection")

    def test_extra_portfolio(self):
        self.environment[POLICY.OPT_INS["portfolio_short"]] = "1"
        self.enumerate("portfolio_short", POLICY.PORTFOLIOS["portfolio_short"] + POLICY.PORTFOLIOS["portfolio_60s"])
        self.invoke("freeze-selection", "portfolio_short", error="unexpected portfolio_short")

    def test_portfolio_requires_literal_optin(self):
        for suite in POLICY.OPT_INS:
            self.environment[POLICY.OPT_INS[suite]] = "true"
            self.enumerate(suite)
            self.invoke("freeze-selection", suite, error="requires literal")

    def test_portfolio_optin_cannot_disappear_during_execution(self):
        suite = "portfolio_60s"
        self.environment[POLICY.OPT_INS[suite]] = "1"
        self.freeze(suite)
        self.outputs(suite)
        del self.environment[POLICY.OPT_INS[suite]]
        self.invoke("verify-result", suite, error="requires literal")

    def test_disabled_enumeration(self):
        data = self.enumerate()
        data["tests"][0]["properties"] = [{"name": "DISABLED", "value": True}]
        self.file("critical", ".json").write_bytes(encoded(data))
        self.invoke("freeze-selection", error="disabled test")

    def test_missing_test_executable(self):
        data = self.enumerate()
        del data["tests"][0]["command"]
        self.file("critical", ".json").write_bytes(encoded(data))
        self.invoke("freeze-selection", error="missing executable command")

    def test_duplicate_enumeration(self):
        self.enumerate(values=self.critical + [self.critical[0]])
        self.invoke("freeze-selection", error="duplicate/normalized alias")

    def test_normalized_alias(self):
        self.enumerate("actionable", self.actionable + [self.critical[0].upper()])
        self.invoke("freeze-selection", "actionable", error="duplicate/normalized alias")

    def test_malformed_enumeration_name(self):
        self.enumerate(values=self.critical[:-1] + [True])
        self.invoke("freeze-selection", error="invalid enumerated string")

    def test_boolean_enumeration_version(self):
        data = self.enumerate()
        data["version"]["major"] = True
        self.file("critical", ".json").write_bytes(encoded(data))
        self.invoke("freeze-selection", error="unsupported CTest enumeration version")

    def test_missing_signature_is_explicit_release_blocker(self):
        self.signature_path.unlink()
        self.enumerate()
        self.invoke("freeze-selection", error="release blocked: approved baseline/signature unavailable")

    def test_signature_bytes_changed(self):
        self.signature_path.write_bytes(b"another signature")
        self.enumerate()
        self.invoke("freeze-selection", error="signature identity mismatch")

    def test_baseline_changed_without_authentication(self):
        self.baseline_path.write_bytes(self.baseline_path.read_bytes() + b"\n")
        self.enumerate()
        self.invoke("freeze-selection", error="baseline identity mismatch")

    def test_noncanonical_authenticated_baseline(self):
        self.baseline_path.write_bytes(json.dumps(self.baseline, indent=2).encode())
        self.auth["baseline_sha256"] = sha(self.baseline_path.read_bytes())
        self.save_auth()
        self.enumerate()
        self.invoke("freeze-selection", error="strict RFC 8785")

    def test_boolean_baseline_version(self):
        self.baseline["schema_version"] = True
        self.save_baseline()
        self.enumerate()
        self.invoke("freeze-selection", error="invalid baseline schema_version")

    def test_boolean_baseline_count(self):
        self.baseline["critical_test_count"] = True
        self.save_baseline()
        self.enumerate()
        self.invoke("freeze-selection", error="critical count mismatch")

    def test_positive_unicode_canonical_baseline(self):
        self.baseline["focused_case_mappings"][0]["predicate"] = "Synthetic café assertion 🧪"
        self.baseline["focused_case_mappings_sha256"] = sha(encoded(self.baseline["focused_case_mappings"]))
        self.save_baseline()
        self.freeze()
        self.outputs()
        self.invoke("verify-result")

    def test_boolean_identity_version(self):
        self.auth["schema_version"] = True
        self.save_auth()
        self.enumerate()
        self.invoke("freeze-selection", error="invalid baseline identity schema_version")

    def test_invalid_verifier_identity(self):
        self.auth["verifier_identity"] = []
        self.save_auth()
        self.enumerate()
        self.invoke("freeze-selection", error="invalid verifier identity string")

    def test_baseline_exact_top_level_fields(self):
        self.baseline["candidate_expectations"] = []
        self.save_baseline()
        self.enumerate()
        self.invoke("freeze-selection", error="invalid WP-00.6 baseline fields")

    def test_baseline_set_hash_mismatch(self):
        self.baseline["critical_test_name_set_sha256"] = "0" * 64
        self.save_baseline()
        self.enumerate()
        self.invoke("freeze-selection", error="critical set hash mismatch")

    def test_union_cannot_omit_native(self):
        self.baseline["required_automated_test_names"] = self.critical
        self.baseline["required_automated_test_count"] = len(self.critical)
        self.baseline["required_automated_test_name_set_sha256"] = set_hash(self.critical)
        self.save_baseline()
        self.enumerate()
        self.invoke("freeze-selection", error="union mismatch")

    def test_missing_focused_mapping(self):
        self.baseline["focused_case_mappings"].pop()
        self.save_baseline()
        self.enumerate()
        self.invoke("freeze-selection", error="mapped acceptance ID")

    def test_duplicate_json_keys(self):
        data = self.enumerate()
        self.file("critical", ".json").write_bytes(b'{"kind":"ctestInfo",' + encoded(data)[1:])
        self.invoke("freeze-selection", error="duplicate JSON field")

    def test_ledger_tampering(self):
        self.ledger["inputs"]["source"]["head"] = "d" * 40
        self.ledger_path.write_bytes(encoded(self.ledger))
        self.enumerate()
        self.invoke("freeze-selection", error="ledger checksum mismatch")

    def test_boolean_ledger_schema(self):
        self.ledger["schema_version"] = True
        self.save_ledger()
        self.enumerate()
        self.invoke("freeze-selection", error="invalid run ledger schema_version")

    def test_evidence_directory_replaced(self):
        self.evidence.rename(self.build / "previous-evidence")
        self.evidence.mkdir()
        self.enumerate()
        self.invoke("freeze-selection", error="evidence directory replaced")

    def test_fixed_ledger_path(self):
        self.ledger_path.rename(self.build / "alternate-ledger.json")
        self.ledger_path = self.build / "alternate-ledger.json"
        self.enumerate()
        self.invoke("freeze-selection", error="fixed build path")

    def test_symlink_enumeration_rejected(self):
        self.enumerate()
        original = self.file("critical", ".json")
        original.rename(self.root / "enumeration.json")
        original.symlink_to(self.root / "enumeration.json")
        self.invoke("freeze-selection", error="symlink/reparse")

    def test_stale_outputs_before_freeze(self):
        self.enumerate()
        self.outputs()
        self.invoke("freeze-selection", error="stale output")

    def test_freeze_retry_rejected_and_original_preserved(self):
        self.freeze()
        original = self.file("critical", ".selection.json").read_bytes()
        self.invoke("freeze-selection", error="suite already consumed")
        self.assertEqual(self.file("critical", ".selection.json").read_bytes(), original)

    def test_verification_retry_rejected_after_failure(self):
        self.freeze()
        self.outputs()
        self.invoke("verify-result", error="CTest exited nonzero", status=8)
        original = self.file("critical", ".result.json").read_bytes()
        self.invoke("verify-result", error="suite already consumed")
        self.assertEqual(self.file("critical", ".result.json").read_bytes(), original)
        self.assertEqual(self.record()["outcome"], "rejected")

    def test_verification_retry_rejected_after_success(self):
        self.freeze()
        self.outputs()
        self.invoke("verify-result")
        self.invoke("verify-result", error="suite already consumed")

    def test_missing_result_consumes_verification(self):
        self.freeze()
        self.invoke("verify-result", error=MISSING_FILE_ERROR)
        self.outputs()
        self.invoke("verify-result", error="suite already consumed")

    def test_wrong_run_rejected(self):
        self.freeze()
        self.outputs()
        self.ledger["run_uuid"] = str(uuid.uuid4())
        self.save_ledger()
        self.invoke("verify-result", error="binding changed")

    def test_wrong_source_rejected(self):
        self.freeze()
        self.outputs()
        self.ledger["inputs"]["source"]["head"] = "d" * 40
        self.save_ledger()
        self.invoke("verify-result", error="binding changed")

    def test_enumeration_command_drift(self):
        self.freeze()
        self.outputs()
        data = json.loads(self.file("critical", ".json").read_text())
        data["tests"][0]["command"] = ["/different/test"]
        self.file("critical", ".json").write_bytes(encoded(data))
        self.invoke("verify-result", error="binding changed")

    def test_authenticated_identity_drift(self):
        self.freeze()
        self.outputs()
        self.auth["verifier_identity"] = "another synthetic verifier"
        self.save_auth()
        self.invoke("verify-result", error="binding changed")

    def test_valid_but_different_authenticated_baseline_after_freeze(self):
        self.freeze()
        self.outputs()
        self.baseline["observed_source_commit"] = "d" * 40
        self.save_baseline()
        self.invoke("verify-result", error="binding changed")

    def test_boolean_selection_version_cannot_equal_integer_one(self):
        self.freeze()
        self.outputs()
        selected = self.record(suffix=".selection.json")
        selected["schema_version"] = True
        self.file("critical", ".selection.json").write_bytes(encoded(selected) + b"\n")
        self.invoke("verify-result", error="binding changed")

    def test_junit_skip_with_exit_zero(self):
        self.freeze()
        root = self.outputs()
        ET.SubElement(root[0], "skipped", message="opt-in absent")
        self.save_xml(root)
        self.invoke("verify-result", error="failed/skipped/error/retry")

    def test_junit_disabled_status(self):
        self.freeze()
        root = self.outputs()
        root[0].set("status", "disabled")
        self.save_xml(root)
        self.invoke("verify-result", error="status is not run")

    def test_junit_notrun_status(self):
        self.freeze()
        root = self.outputs()
        root[0].set("status", "notrun")
        self.save_xml(root)
        self.invoke("verify-result", error="status is not run")

    def test_junit_missing_status(self):
        self.freeze()
        root = self.outputs()
        del root[0].attrib["status"]
        self.save_xml(root)
        self.invoke("verify-result", error="status is not run")

    def test_junit_failure_even_exit_zero(self):
        self.freeze()
        root = self.outputs()
        ET.SubElement(root[0], "failure").text = "failed"
        self.save_xml(root)
        self.invoke("verify-result", error="failed/skipped/error/retry")

    def test_junit_error_even_exit_zero(self):
        self.freeze()
        root = self.outputs()
        ET.SubElement(root[0], "error").text = "timeout"
        self.save_xml(root)
        self.invoke("verify-result", error="failed/skipped/error/retry")

    def test_junit_retry_element(self):
        self.freeze()
        root = self.outputs()
        ET.SubElement(root[0], "flakyFailure")
        self.save_xml(root)
        self.invoke("verify-result", error="failed/skipped/error/retry")

    def test_junit_duplicate_name(self):
        self.freeze()
        self.outputs(values=self.critical[:-1] + [self.critical[0]])
        self.invoke("verify-result", error="duplicate/normalized alias")

    def test_junit_missing_case(self):
        self.freeze()
        self.outputs(values=self.critical[:-1])
        self.invoke("verify-result", error="names/cardinality differ")

    def test_junit_extra_case(self):
        self.freeze()
        self.outputs(values=self.critical + ["extra-test"])
        self.invoke("verify-result", error="names/cardinality differ")

    def test_junit_summary_mismatch(self):
        self.freeze()
        root = self.outputs()
        root.set("tests", "49")
        self.save_xml(root)
        self.invoke("verify-result", error="tests count mismatch")

    def test_junit_hidden_summary_skip(self):
        self.freeze()
        root = self.outputs()
        root.set("skipped", "1")
        self.save_xml(root)
        self.invoke("verify-result", error="skipped count mismatch/nonzero")

    def test_junit_invalid_duration(self):
        self.freeze()
        root = self.outputs()
        root[0].set("time", "nan")
        self.save_xml(root)
        self.invoke("verify-result", error="invalid JUnit duration")

    def test_junit_wrong_run_property(self):
        self.freeze()
        root = self.outputs()
        properties = ET.SubElement(root[0], "properties")
        ET.SubElement(properties, "property", name="run_id", value=str(uuid.uuid4()))
        self.save_xml(root)
        self.invoke("verify-result", error="mixed JUnit run_id")

    def test_junit_malformed(self):
        self.freeze()
        self.outputs()
        self.file("critical", ".xml").write_bytes(b"<testsuite")
        self.invoke("verify-result", error="unclosed token")

    def test_junit_entities_rejected(self):
        self.freeze()
        self.outputs()
        self.file("critical", ".xml").write_bytes(b'<!DOCTYPE testsuite [<!ENTITY a "x">]><testsuite/>')
        self.invoke("verify-result", error="DTD/entity")

    def test_utf16_cannot_hide_junit_entities(self):
        self.freeze()
        self.outputs()
        self.file("critical", ".xml").write_bytes(
            '<!DOCTYPE testsuite [<!ENTITY a "x">]><testsuite/>'.encode("utf-16"))
        self.invoke("verify-result", error="utf-8")

    def test_positive_multi_suite_junit_container(self):
        self.freeze()
        first = self.outputs(values=self.critical[:24])
        second = self.outputs(values=self.critical[24:])
        root = ET.Element("testsuites", tests="48", failures="0")
        root.extend([first, second])
        self.save_xml(root)
        self.invoke("verify-result")

    def test_empty_log_rejected(self):
        self.freeze()
        self.outputs()
        self.file("critical", ".log").write_bytes(b"")
        self.invoke("verify-result", error="empty CTest log")

    def test_failed_suite_cannot_be_repaired_by_cross_suite_success(self):
        self.freeze()
        self.outputs()
        self.invoke("verify-result", error="CTest exited nonzero", status=8)
        self.freeze("actionable")
        self.outputs("actionable")
        self.invoke("verify-result", "actionable")
        self.assertEqual(self.record()["outcome"], "rejected")
        self.assertEqual(self.record("actionable")["outcome"], "passed")

    def prepare_closure(self, mode="quick"):
        self.ledger["inputs"]["mode"] = mode
        self.save_ledger()
        scheduled = tuple(POLICY.SUITES) if mode == "full" else ("critical", "native_nesting")
        for suite in scheduled:
            if suite in POLICY.OPT_INS:
                self.environment[POLICY.OPT_INS[suite]] = "1"
            self.freeze(suite)
            self.outputs(suite)
            self.invoke("verify-result", suite)

    def closure(self, error=None):
        before = {p.name: p.read_bytes() for p in self.evidence.iterdir() if p.is_file()}
        result = subprocess.run([sys.executable, str(HELPER), "verify-closure", "--ledger", str(self.ledger_path),
                                 "--baseline-identity", str(self.identity_path)], cwd=self.root,
                                env=self.environment, capture_output=True, text=True, check=False)
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.evidence.iterdir() if p.is_file()})
        self.assertFalse((self.evidence / "VACARDS-RELEASE-GATE.env").exists())
        if error:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(error, result.stderr)
            self.assertEqual(result.stdout, "")
            return None
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        record = json.loads(result.stdout)
        self.assertEqual(record["authority"], "structural-suite-evidence-only")
        self.assertEqual(record["helper_sha256"], sha(HELPER.read_bytes()))
        self.assertEqual(record["ledger_sha256"], sha(self.ledger_path.read_bytes()))
        return record

    def test_closure_quick_exact_two_read_only(self):
        self.prepare_closure()
        result = self.closure()
        self.assertEqual([row["suite_id"] for row in result["suites"]], ["critical", "native_nesting"])

    def test_closure_full_exact_five_with_cross_suite_repeats(self):
        self.prepare_closure("full")
        # Closure rechecks recorded opt-ins, not current ambient settings.
        self.environment = {k: v for k, v in self.environment.items() if k not in POLICY.OPT_INS.values()}
        result = self.closure()
        self.assertEqual([row["suite_id"] for row in result["suites"]], list(POLICY.SUITES))

    def test_closure_rejects_missing_record(self):
        self.prepare_closure()
        self.file("native_nesting", ".result.json").unlink()
        self.closure("exactly the scheduled")

    def test_closure_rejects_extra_record(self):
        self.prepare_closure()
        (self.evidence / "unexpected.result.json").write_bytes(b"{}")
        self.closure("exactly the scheduled")

    def test_closure_rejects_selected_only(self):
        self.prepare_closure()
        self.file("critical", ".result.json").write_bytes(self.file("critical", ".selection.json").read_bytes())
        self.closure("result rejected")

    def test_closure_rejects_mutated_raw_files(self):
        self.prepare_closure()
        for suffix in (".json", ".selection.json", ".xml", ".log"):
            with self.subTest(suffix=suffix):
                target = self.file("critical", suffix)
                original = target.read_bytes()
                target.write_bytes(original + b"\n")
                self.closure("closure")
                target.write_bytes(original)

    def test_closure_rejects_mixed_stale_rejected_or_unknown_records(self):
        self.prepare_closure()
        original = self.record()
        for field, value in (("schema_version", 2), ("outcome", "rejected"), ("run_id", str(uuid.uuid4())),
                             ("source_commit", "f" * 40), ("ledger_sha256", "f" * 64),
                             ("helper_sha256", "f" * 64), ("ctest_exit_code", 8),
                             ("baseline_identity", {}), ("suite_id", "native_nesting")):
            with self.subTest(field=field):
                self.file("critical", ".result.json").write_bytes(POLICY.record_bytes({**original, field: value}))
                self.closure("result rejected")

    def test_closure_rejects_empty_partial_or_duplicate_json_record(self):
        self.prepare_closure()
        for raw in (b"", b'{"schema_version":', b'{"schema_version":1,"schema_version":1}'):
            self.file("critical", ".result.json").write_bytes(raw)
            self.closure("result rejected")

    def test_closure_rejects_missing_or_aliased_raw_file(self):
        self.prepare_closure()
        target = self.file("critical", ".xml")
        original = target.read_bytes()
        target.unlink()
        self.closure(MISSING_FILE_ERROR)
        other = self.root / "alias.xml"
        other.write_bytes(original)
        target.symlink_to(other)
        self.closure("symlink")

    def test_result_keeps_exit_code_when_junit_missing(self):
        self.freeze()
        self.file("critical", ".log").write_text("CTest crashed before JUnit")
        self.invoke("verify-result", status=9, error=MISSING_FILE_ERROR)
        self.assertEqual(self.record()["ctest_exit_code"], 9)

    def test_comparator_retention_survives_repeat_and_detects_retained_tampering(self):
        self.ledger["inputs"]["mode"] = "quick"
        self.save_ledger()
        self.freeze()
        self.outputs()
        self.invoke("verify-result")
        data = self.enumerate("native_nesting")
        output = self.build / "differential-output"
        output.mkdir()
        for test in data["tests"]:
            if test["name"] == "vacards-nesting-differential-selfcheck":
                test["command"] += ["-DOUTPUT_DIR=" + str(output)]
        self.file("native_nesting", ".json").write_bytes(encoded(data))
        self.invoke("freeze-selection", "native_nesting")
        self.outputs("native_nesting")
        for filename in ("baseline.tsv", "portfolio.tsv", "comparison.tsv"):
            (output / filename).write_bytes(b"first suite raw report")
        self.invoke("verify-result", "native_nesting")
        (output / "baseline.tsv").write_bytes(b"later actionable suite overwrites original")
        record = self.closure()["suites"][1]
        self.assertEqual(len(record["comparator_files"]), 3)
        retained = self.evidence / record["comparator_files"][0]["retained"]
        self.assertEqual(retained.read_bytes(), b"first suite raw report")
        retained.write_bytes(b"tampered")
        self.closure("result rejected")

    def test_failed_portfolio_preserves_partial_comparator_reports(self):
        suite = "portfolio_short"
        self.environment[POLICY.OPT_INS[suite]] = "1"
        data = self.enumerate(suite)
        output = self.build / "failed-100ms"
        output.mkdir()
        next(test for test in data["tests"] if test["name"] ==
             "vacards-nesting-portfolio-differential-100ms")["command"] += ["-DOUTPUT_DIR=" + str(output)]
        self.file(suite, ".json").write_bytes(encoded(data))
        self.invoke("freeze-selection", suite)
        self.outputs(suite)
        (output / "baseline.tsv").write_bytes(b"partial failed output")
        self.invoke("verify-result", suite, status=8, error="CTest exited nonzero")
        records = self.record(suite)["comparator_files"]
        self.assertEqual(len(records), 5)
        self.assertEqual(records[0]["sha256"], sha(b"partial failed output"))
        self.assertTrue(all(row["sha256"] is None for row in records[1:]))


if __name__ == "__main__":
    unittest.main()
