#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Outcome-based synthetic tests for reduce_observer.py.

These tests are independent of the reducer implementation: they build synthetic
Pan input-summary / observer frames.csv / observer-metadata.json fixtures, run
the reducer as a subprocess, and assert hardcoded report fields and exit codes
derived by hand from the measurement rules.

Run:  python3 test_reduce_observer.py
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REDUCER = os.path.join(HERE, "reduce_observer.py")

FREQ = 10_000_000  # QPC ticks per second
START = 1000
END = 2000

CSV_HEADER = (
    "seq,phase,acquired_qpc,qpc_before,qpc_after,last_present_time,"
    "last_present_sec,last_mouse_update_time,accumulated_frames,copied,"
    "cursor_overlap,map_start_qpc,map_end_qpc,rows_scanned,rows_with_run,"
    "max_runs_in_row,left,right,width,width_drift,valid,scan_reason,"
    "content_changed,signature_changed,signature,stale,pointer_only,error"
)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        h.update(fh.read())
    return h.hexdigest()


def make_row(seq, lp, left, right, **kw):
    row = {
        "seq": seq,
        "phase": "measure",
        "acquired_qpc": lp,
        "qpc_before": lp - 10,
        "qpc_after": lp,
        "last_present_time": lp,
        "last_present_sec": lp / FREQ,
        "last_mouse_update_time": 0,
        "accumulated_frames": 1,
        "copied": 1,
        "cursor_overlap": 0,
        "map_start_qpc": lp - 5,
        "map_end_qpc": lp - 4,
        "rows_scanned": 4,
        "rows_with_run": 4,
        "max_runs_in_row": 1,
        "left": left,
        "right": right,
        "width": right - left + 1,
        "width_drift": 0,
        "valid": 1,
        "scan_reason": "ok",
        "content_changed": 0,
        "signature_changed": 0,
        "signature": "0x%016X" % (0xA000 + seq),
        "stale": 0,
        "pointer_only": 0,
        "error": "",
    }
    row.update(kw)
    if row["pointer_only"]:
        row["valid"] = 0
        row["copied"] = 0
        row["scan_reason"] = ""
    return row


def write_frames(path, rows):
    cols = CSV_HEADER.split(",")
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(CSV_HEADER + "\n")
        for row in rows:
            fh.write(",".join(str(row.get(c, "")) for c in cols) + "\n")


def build_metadata(rows, freq=FREQ):
    counts = {
        "acquires": len(rows),
        "timeouts": 0,
        "pointer_only": sum(1 for r in rows if r["pointer_only"]),
        "skipped": sum(1 for r in rows if not r["valid"]),
        "merged": sum(1 for r in rows if r["accumulated_frames"] > 1),
        "valid": sum(1 for r in rows if r["valid"] and r["phase"] != "warmup"),
        "missing": sum(1 for r in rows if r["scan_reason"] == "no_target_pixels"),
        "ambiguous": sum(1 for r in rows if (not r["valid"]) and r["scan_reason"] in (
            "multiple_runs", "partial_rows", "inconsistent_edges", "width_drift", "invalid_strip")),
        "cursor_overlap": sum(1 for r in rows if r["cursor_overlap"]),
        "stale": sum(1 for r in rows if r["stale"]),
        "errors": sum(1 for r in rows if (not r["valid"]) and (not r["copied"]) and not r["pointer_only"]),
        "content_changes": sum(1 for r in rows if r["content_changed"]),
        "signature_changes": sum(1 for r in rows if r["signature_changed"]),
        "frames_written": len(rows),
    }
    return {
        "schema": "vacards.screen-observer/1",
        "observer_version": "next4-observer-1",
        "qpc_frequency": freq,
        "qpc_start": START - 5000,
        "qpc_ready": START - 100,
        "qpc_end": END + 5000,
        "exit_code": 0,
        "exit_reason": "stop_file",
        "fatal": False,
        "roi": [0, 0, 64, 16],
        "expected_width": 20,
        "counts": counts,
        "note": "synthetic test metadata",
    }


def build_summary(rows, frames_sha, metadata_sha, start=START, end=END, freq=FREQ,
                  ready_qpc=None, exit_code=0, exited=True, forced_kill=False,
                  timed_out=False, requested=True, complete=True, observed_freq=None):
    if ready_qpc is None:
        ready_qpc = start - 100
    content_changes = sum(1 for r in rows if r["content_changed"])
    return {
        "helper": "Invoke-AppPan.ps1",
        "workload": "object-drag",
        "protocol_identity": "continuous-v2",
        "qpc_frequency": freq,
        "input_start_qpc": start,
        "input_end_qpc": end,
        "window_duration_seconds": (end - start) / freq,
        "phase_boundaries": {
            "schema": "vacards-app-pan-phase-boundaries/1",
            "complete": complete,
            "status": "complete" if complete else "partial",
            "qpc_frequency": freq,
            "input_start_qpc": start if complete else None,
            "input_end_qpc": end if complete else None,
        },
        "observer": {
            "schema": "vacards-app-pan-observer/1",
            "requested": requested,
            "exe_actual_sha256": "a" * 64,
            "expected_exe_sha256": "a" * 64,
            "ready_seen": True,
            "ready_qpc": ready_qpc,
            "stopped_qpc": end + 1000,
            "exited": exited,
            "exit_code": exit_code,
            "forced_kill": forced_kill,
            "timed_out": timed_out,
            "frames_written": len(rows),
            "valid_frames": sum(1 for r in rows if r["valid"] and r["phase"] != "warmup"),
            "missing_frames": sum(1 for r in rows if r["scan_reason"] == "no_target_pixels"),
            "merged_frames": sum(1 for r in rows if r["accumulated_frames"] > 1),
            "ambiguous_frames": sum(1 for r in rows if (not r["valid"]) and r["scan_reason"] in (
                "multiple_runs", "partial_rows", "inconsistent_edges", "width_drift", "invalid_strip")),
            "content_changes": content_changes,
            "frames_csv_sha256": frames_sha,
            "metadata_sha256": metadata_sha,
            "roi_config_actual_sha256": "c" * 64,
            "expected_roi_config_sha256": "c" * 64,
            "errors": [],
        },
    }


class ReducerCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="reduce-observer-test-")
        self.frames = os.path.join(self.tmp, "frames.csv")
        self.metadata = os.path.join(self.tmp, "observer-metadata.json")
        self.summary = os.path.join(self.tmp, "input-summary.json")
        self.out = os.path.join(self.tmp, "report.json")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_case(self, rows, summary_kw=None, metadata_freq=FREQ,
                 frames_sha_override=None, metadata_sha_override=None):
        write_frames(self.frames, rows)
        with open(self.metadata, "w", encoding="utf-8") as fh:
            json.dump(build_metadata(rows, freq=metadata_freq), fh)
        frames_sha = frames_sha_override or sha256_file(self.frames)
        metadata_sha = metadata_sha_override or sha256_file(self.metadata)
        summary = build_summary(rows, frames_sha, metadata_sha, **(summary_kw or {}))
        with open(self.summary, "w", encoding="utf-8") as fh:
            json.dump(summary, fh)
        proc = subprocess.run(
            [sys.executable, REDUCER, "--input-summary", self.summary,
             "--frames", self.frames, "--metadata", self.metadata, "--out", self.out],
            capture_output=True, text=True,
        )
        with open(self.out, "r", encoding="utf-8") as fh:
            report = json.load(fh)
        return proc.returncode, report

    # ------------------------------------------------------------------
    def test_window_and_motion_sequence(self):
        """warmup A, beforeinput B, B, C, C, D, postinput E -> 2 updates B->C, C->D."""
        rows = [
            make_row(1, 100, 10, 29, phase="warmup"),
            make_row(2, 900, 40, 59, content_changed=1),   # before-input B
            make_row(3, 1100, 40, 59),                     # input B (prior reference)
            make_row(4, 1200, 20, 39, content_changed=1),  # B -> C
            make_row(5, 1300, 20, 39),                     # C
            make_row(6, 1400, 50, 69, content_changed=1),  # C -> D
            make_row(7, 2100, 0, 9, content_changed=1),    # post-input E
        ]
        code, report = self.run_case(rows)
        self.assertEqual(code, 0, report.get("errors"))
        self.assertEqual(report["status"], "valid")
        self.assertEqual(report["counts"]["valid_in_window"], 4)
        self.assertEqual(report["counts"]["valid_before_window"], 2)
        self.assertEqual(report["counts"]["valid_after_window"], 1)
        self.assertEqual(report["recomputed"]["target_motion_updates"], 2)
        self.assertEqual(report["recomputed"]["total_duration_seconds"], 0.0001)
        self.assertEqual(report["recomputed"]["composed_target_updates_per_second"], 20000.0)
        intervals = report["recomputed"]["interupdate_intervals"]
        self.assertEqual(intervals["count"], 1)
        self.assertAlmostEqual(intervals["p50_ms"], 0.02, places=9)
        self.assertAlmostEqual(intervals["p95_ms"], 0.02, places=9)
        self.assertAlmostEqual(intervals["max_ms"], 0.02, places=9)
        self.assertEqual(intervals["quantile_method"], "linear_interpolation_type7")
        span = report["recomputed"]["longest_no_update_span"]
        self.assertEqual(span["type"], "trailing_censored")
        self.assertAlmostEqual(span["seconds"], 0.00006, places=12)
        self.assertAlmostEqual(report["recomputed"]["leading_no_update_span_seconds"], 0.00002, places=12)
        self.assertAlmostEqual(report["recomputed"]["trailing_no_update_span_seconds"], 0.00006, places=12)
        self.assertTrue(report["emitted_vs_recomputed"]["match"])

    def test_background_signature_change_is_not_motion(self):
        rows = [
            make_row(1, 100, 10, 29, phase="warmup"),
            make_row(2, 1100, 10, 29, signature_changed=1),
            make_row(3, 1200, 10, 29, signature_changed=1),
        ]
        code, report = self.run_case(rows)
        self.assertEqual(code, 0, report.get("errors"))
        self.assertEqual(report["recomputed"]["target_motion_updates"], 0)
        self.assertEqual(report["recomputed"]["composed_target_updates_per_second"], 0.0)
        self.assertTrue(report["recomputed"]["zero_motion_valid0cadence"])
        self.assertEqual(report["recomputed"]["longest_no_update_span"]["type"], "whole_window")
        self.assertAlmostEqual(report["recomputed"]["leading_no_update_span_seconds"], 0.0001, places=12)
        self.assertAlmostEqual(report["recomputed"]["trailing_no_update_span_seconds"], 0.0001, places=12)

    def test_invalid_sample_does_not_advance_previous_edges(self):
        rows = [
            make_row(1, 100, 10, 29, phase="warmup"),
            make_row(2, 1100, 40, 59, valid=0, scan_reason="multiple_runs",
                     content_changed=0),  # would match B if it wrongly advanced prev
            make_row(3, 1200, 40, 59, content_changed=1),
        ]
        code, report = self.run_case(rows)
        self.assertEqual(code, 0, report.get("errors"))
        self.assertEqual(report["recomputed"]["target_motion_updates"], 1)
        self.assertEqual(report["counts"]["primary_reason"]["ambiguous_multiple_runs"], 1)
        self.assertFalse(report["recomputed"]["zero_motion_valid0cadence"])
        self.assertTrue(report["recomputed"]["observed_cadence_is_lower_bound"])

    def test_pointer_only_rows_are_skipped(self):
        rows = [
            make_row(1, 100, 10, 29, phase="warmup"),
            make_row(2, 0, 999, 1028, pointer_only=1, acquired_qpc=1050,
                     qpc_before=1040, qpc_after=1050),
            make_row(3, 1100, 10, 29),
        ]
        code, report = self.run_case(rows)
        self.assertEqual(code, 0, report.get("errors"))
        self.assertEqual(report["recomputed"]["target_motion_updates"], 0)
        self.assertEqual(report["counts"]["flags"]["pointer_only"], 1)
        self.assertTrue(report["recomputed"]["zero_motion_valid0cadence"])

    def test_strict_window_boundaries_excluded(self):
        rows = [
            make_row(1, START, 10, 29),                 # == input_start: excluded
            make_row(2, START + 100, 40, 59, content_changed=1),
            make_row(3, END, 70, 99, content_changed=1),  # == input_end: excluded
        ]
        code, report = self.run_case(rows)
        self.assertEqual(code, 0, report.get("errors"))
        self.assertEqual(report["counts"]["valid_in_window"], 1)
        self.assertEqual(report["counts"]["valid_before_window"], 1)
        self.assertEqual(report["counts"]["valid_after_window"], 1)
        self.assertEqual(report["recomputed"]["target_motion_updates"], 1)
        self.assertTrue(report["emitted_vs_recomputed"]["match"])

    def test_no_valid_in_window_is_invalid(self):
        rows = [
            make_row(1, 100, 10, 29, phase="warmup"),
            make_row(2, 2100, 40, 59, content_changed=1),
        ]
        code, report = self.run_case(rows)
        self.assertEqual(code, 3)
        self.assertEqual(report["status"], "invalid")
        self.assertTrue(any("no valid frames" in e for e in report["errors"]))

    def test_merged_frames_retained_and_lower_bound(self):
        rows = [
            make_row(1, 100, 10, 29, phase="warmup"),
            make_row(2, 1100, 40, 59, accumulated_frames=3, content_changed=1),
            make_row(3, 1200, 40, 59),
        ]
        code, report = self.run_case(rows)
        self.assertEqual(code, 0, report.get("errors"))
        self.assertEqual(report["recomputed"]["target_motion_updates"], 1)
        self.assertEqual(report["counts"]["flags"]["merged"], 1)
        self.assertEqual(report["counts"]["accumulated_lost_captures_lowerbound"], 2)
        self.assertTrue(report["recomputed"]["observed_cadence_is_lower_bound"])
        self.assertIn("merged", report["recomputed"]["lower_bound_reasons"])

    def test_frequency_mismatch_fails(self):
        rows = [make_row(1, 1100, 10, 29, content_changed=1)]
        code, report = self.run_case(rows, metadata_freq=FREQ * 2)
        self.assertEqual(code, 3)
        self.assertEqual(report["status"], "invalid")
        self.assertFalse(report["clock"]["match"])
        self.assertTrue(any("qpc_frequency mismatch" in e for e in report["errors"]))

    def test_frames_hash_mismatch_fails(self):
        rows = [make_row(1, 1100, 10, 29, content_changed=1)]
        code, report = self.run_case(rows, frames_sha_override="d" * 64)
        self.assertEqual(code, 3)
        self.assertFalse(report["hash_verification"]["frames_match"])
        self.assertTrue(any("frames.csv sha256" in e for e in report["errors"]))

    def test_ready_after_input_start_fails(self):
        rows = [make_row(1, 1100, 10, 29, content_changed=1)]
        code, report = self.run_case(rows, summary_kw={"ready_qpc": START + 1})
        self.assertEqual(code, 3)
        self.assertFalse(report["window"]["observer_ready_before_input_start"])
        self.assertTrue(any("ready_qpc" in e for e in report["errors"]))

    def test_abnormal_observer_exit_fails(self):
        rows = [make_row(1, 1100, 10, 29, content_changed=1)]
        code, report = self.run_case(rows, summary_kw={"exit_code": 4, "exited": True})
        self.assertEqual(code, 3)
        self.assertTrue(any("exit_code" in e for e in report["errors"]))

    def test_nonmonotonic_acquired_qpc_fails(self):
        rows = [
            make_row(1, 1100, 10, 29, content_changed=1),
            make_row(2, 1000, 40, 59, content_changed=1, acquired_qpc=500),
        ]
        code, report = self.run_case(rows)
        self.assertEqual(code, 3)
        self.assertFalse(report["row_validation"]["acquired_qpc_monotonic"])

    def test_observer_off_is_excluded_not_zero_fps(self):
        rows = [make_row(1, 1100, 10, 29, content_changed=1)]
        code, report = self.run_case(rows, summary_kw={"requested": False, "complete": True})
        self.assertEqual(code, 0)
        self.assertEqual(report["status"], "excluded")
        self.assertEqual(report["excluded_reason"], "observer_off")
        self.assertNotIn("recomputed", report)

    def test_incomplete_phase_boundaries_fail(self):
        rows = [make_row(1, 1100, 10, 29, content_changed=1)]
        code, report = self.run_case(rows, summary_kw={"complete": False})
        self.assertEqual(code, 3)
        self.assertTrue(any("complete" in e for e in report["errors"]))

    def test_emitted_flag_mismatch_fails(self):
        rows = [
            make_row(1, 100, 10, 29, phase="warmup"),
            make_row(2, 1100, 40, 59, content_changed=0),  # wrong: edge moved
        ]
        code, report = self.run_case(rows)
        self.assertEqual(code, 3)
        self.assertFalse(report["emitted_vs_recomputed"]["match"])
        self.assertTrue(any("disagrees" in e for e in report["errors"]))

    def test_exclusion_counts_visible(self):
        rows = [
            make_row(1, 100, 10, 29, phase="warmup"),
            make_row(2, 1100, 40, 59, content_changed=1),
            make_row(3, 1200, 999, 1028, valid=0, scan_reason="no_target_pixels"),
            make_row(4, 1300, 40, 59, cursor_overlap=1, valid=0, content_changed=0),
            make_row(5, 1400, 40, 59, stale=1, valid=0, error="stale_present"),
            make_row(6, 1500, 0, 0, pointer_only=1),
        ]
        code, report = self.run_case(rows)
        self.assertEqual(code, 0, report.get("errors"))
        reasons = report["counts"]["primary_reason"]
        self.assertEqual(reasons["missing"], 1)
        self.assertEqual(reasons["cursor_overlap"], 1)
        self.assertEqual(reasons["stale_present"], 1)
        self.assertEqual(reasons["pointer_only"], 1)
        self.assertEqual(report["counts"]["invalid_rows"], 4)
        self.assertEqual(report["counts"]["valid_rows"], 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
