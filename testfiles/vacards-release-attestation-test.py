#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Serial rejection tests for the unavailable production trust boundary.

These fixtures are deliberately NOT a proposed closure/authentication schema.
No production positive is possible until an independent boundary is provisioned.
The actual shell CLI must reject even apparently successful candidate records,
without invoking the executable or modifying evidence. Fixture directories are
retained for inspection, including on failure. No app builds or tests are run.
"""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
VERIFIER = ROOT / "packaging/macos/vacards/verify-release-attestation.sh"
PROVENANCE = ROOT / "packaging/vacards/packaging-provenance.py"


class ProductionBoundary(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixtures = Path(tempfile.mkdtemp(prefix="vacards-release-boundary-"))
        print(f"Retained synthetic fixtures: {cls.fixtures}", flush=True)

    def setUp(self):
        self.prefix = self.fixtures / self.id().rsplit(".", 1)[-1]
        (self.prefix / "bin").mkdir(parents=True)
        # An executable that must never be invoked by blocked verification.
        executable = self.prefix / "bin/inkscape"
        executable.write_text('#!/bin/sh\n: > "$0.executed"\nexit 97\n', encoding="utf-8")
        executable.chmod(0o755)

    def snapshot(self):
        return {str(p.relative_to(self.prefix)): p.read_bytes()
                for p in self.prefix.rglob("*") if p.is_file()}

    def reject(self, *, env=None):
        before = self.snapshot()
        binder = [sys.executable, "-B", str(PROVENANCE)]
        commands = [
            ["/bin/sh", str(VERIFIER), str(self.prefix)],
            ["/bin/sh", str(VERIFIER), "--candidate", str(self.prefix)],
            [*binder, "require-release-authorization"],
            [*binder, "verify-attestation", str(self.prefix)],
            [*binder, "verify-attestation", "--candidate", str(self.prefix)],
            [*binder, "restore-bundle", "--app", str(self.prefix),
             "--install", str(self.prefix), "--cairo-prefix", str(self.prefix)],
            [*binder, "finish-bundle", str(self.prefix)],
            [*binder, "verify-bundle", str(self.prefix)],
        ]
        for args in commands:
            with self.subTest(command=args):
                result = subprocess.run(args, cwd=ROOT, env={**os.environ, **(env or {})},
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("production release closure is unavailable", result.stderr)
                self.assertIn("fresh integrated full gate", result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertEqual(before, self.snapshot(), "verification changed retained evidence")

    def attest(self, text):
        for name in ("VACARDS-RELEASE-GATE.env", "VACARDS-RELEASE-GATE.pending.env"):
            (self.prefix / name).write_text(text, encoding="utf-8")

    def test_missing_authorization_rejected(self):
        self.reject()

    def test_legacy_format3_and_high_counts_cannot_authorize(self):
        self.attest("format=3\nscope=full\ncritical_test_count=999999\n")
        self.reject()

    def test_quick_scope_cannot_authorize(self):
        self.attest("format=3\nscope=quick\n")
        self.reject()

    def test_partial_closure_cannot_authorize(self):
        self.attest("format=3\nscope=full\n")
        (self.prefix / "closure.json").write_text('{"outcome":"passed",', encoding="utf-8")
        self.reject()

    def test_apparently_passed_structural_closure_is_not_trust(self):
        self.attest("format=3\nscope=full\n")
        (self.prefix / "closure.json").write_text(json.dumps({
            "schema_version": 1, "purpose": "verify-closure", "outcome": "passed",
            "suites": ["critical", "native_nesting", "actionable",
                       "portfolio_short", "portfolio_60s"],
        }), encoding="utf-8")
        self.reject()

    def test_stale_source_and_run_cannot_authorize(self):
        self.attest("format=3\nscope=full\nsource_commit=" + "a" * 40 + "\n")
        (self.prefix / "VACARDS-GATE-RUN.json").write_text(
            '{"source_commit":"' + "b" * 40 + '","run_id":"old-run"}', encoding="utf-8")
        self.reject()

    def test_tampered_hash_bindings_cannot_authorize(self):
        self.attest("format=3\nscope=full\nbinary_sha256=" + "0" * 64 + "\n")
        (self.prefix / "VACARDS-PACKAGING-INPUTS.json").write_text("tampered\n", encoding="utf-8")
        self.reject()

    def test_unknown_format_and_duplicate_fields_cannot_authorize(self):
        self.attest("format=3\nformat=4\nscope=quick\nscope=full\n")
        self.reject()

    def test_candidate_written_authentication_is_not_authority(self):
        self.attest("format=3\nscope=full\nauthenticated=true\nacceptance=passed\n")
        identity = self.prefix / "identity.json"
        identity.write_text(json.dumps({"schema_version": 1, "baseline_sha256": "a" * 64,
                                       "signature_sha256": "b" * 64,
                                       "verifier_identity": "candidate-self-assertion",
                                       "verifier_sha256": "c" * 64}), encoding="utf-8")
        self.reject(env={"VACARDS_BASELINE_IDENTITY": str(identity),
                         "VACARDS_RELEASE_AUTHENTICATED": "1",
                         "VACARDS_ALLOW_UNAUTHENTICATED": "1",
                         "VACARDS_GATE_TEST_MODE": "1"})

    def test_candidate_flag_does_not_fall_back_to_final_attestation(self):
        (self.prefix / "VACARDS-RELEASE-GATE.env").write_text(
            "format=3\nscope=full\n", encoding="utf-8")
        self.reject()

    def test_cli_has_no_trust_override(self):
        result = subprocess.run(["/bin/sh", str(VERIFIER), "--trust-candidate", str(self.prefix)],
                                cwd=ROOT, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 2)
        self.assertIn("usage:", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
