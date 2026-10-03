#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""reduce_observer.py — stdlib reducer for the VACards app-screen-observer.

Consumes a Pan ``input-summary.json`` (produced by Invoke-AppPan.ps1) plus the
observer ``frames.csv`` / ``observer-metadata.json`` and emits one JSON report.

This is an independent recomputation of target-motion cadence. It never claims
input-to-frame latency, nearest-input association, photons, display refresh, or
unoccluded/perfect cursor exclusion.

Exit codes:
    0  report produced, data valid OR trial excluded (observer off)
    2  usage error or input-summary unreadable/unparseable
    3  report produced, data invalid (fail closed)

No third-party imports. Python 3.8+.
"""

import argparse
import csv
import hashlib
import json
import math
import os
import sys

SCHEMA = "vacards-observer-cadence-reducer/1"
QUANTILE_METHOD = "linear_interpolation_type7"

# Strict window membership: input_start < LastPresentTime < input_end.
WINDOW_CONVENTION = "input_start_qpc < last_present_time < input_end_qpc"

AMBIGUOUS_REASONS = (
    "multiple_runs",
    "partial_rows",
    "inconsistent_edges",
    "width_drift",
    "invalid_strip",
)

CSV_REQUIRED_COLUMNS = (
    "seq",
    "phase",
    "acquired_qpc",
    "qpc_before",
    "qpc_after",
    "last_present_time",
    "accumulated_frames",
    "copied",
    "cursor_overlap",
    "left",
    "right",
    "width",
    "width_drift",
    "valid",
    "scan_reason",
    "content_changed",
    "signature_changed",
    "stale",
    "pointer_only",
    "error",
)


class UsageError(Exception):
    pass


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------
def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def as_int(value, default=None):
    if value is None:
        return default
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        if math.isnan(value) or math.isinf(value):
            return default
        return int(value)
    text = str(value).strip()
    if text == "" or text.lower() in ("null", "none", "nan"):
        return default
    try:
        return int(text, 0)
    except ValueError:
        try:
            return int(float(text))
        except ValueError:
            return default


def as_float(value, default=None):
    if value is None:
        return default
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return float(value)
    text = str(value).strip()
    if text == "" or text.lower() in ("null", "none", "nan"):
        return default
    try:
        return float(text)
    except ValueError:
        return default


def as_bool01(value):
    if value is None:
        return False
    if isinstance(value, bool):
        return value
    text = str(value).strip().lower()
    return text in ("1", "true", "yes")


def dig(obj, *keys, default=None):
    cur = obj
    for key in keys:
        if not isinstance(cur, dict) or key not in cur:
            return default
        cur = cur[key]
    return cur


def quantile_type7(values, p):
    """Hyndman-Fan type 7 / linear interpolation quantile (explicit)."""
    if not values:
        return None
    ordered = sorted(values)
    n = len(ordered)
    if n == 1:
        return ordered[0]
    h = (n - 1) * p
    lo = int(math.floor(h))
    hi = min(lo + 1, n - 1)
    frac = h - lo
    return ordered[lo] + frac * (ordered[hi] - ordered[lo])


def round_or_none(value, digits=9):
    if value is None:
        return None
    return round(value, digits)


def write_report(report, out_path):
    text = json.dumps(report, indent=2, sort_keys=False)
    if out_path:
        with open(out_path, "w", encoding="utf-8") as fh:
            fh.write(text + "\n")
    else:
        sys.stdout.write(text + "\n")


# ---------------------------------------------------------------------------
# Path resolution
# ---------------------------------------------------------------------------
def resolve_local_path(candidate, summary_path, observer_dir, label, errors):
    """Resolve an observer artifact path that may be a Windows path from the
    summary. Tries the literal path, the basename beside the input summary,
    beside --observer-dir, and in the cwd."""
    tried = []
    candidates = []
    if candidate:
        candidates.append(candidate)
        base = os.path.basename(str(candidate).replace("\\", "/"))
        if base:
            if summary_path:
                candidates.append(os.path.join(os.path.dirname(os.path.abspath(summary_path)), base))
            if observer_dir:
                candidates.append(os.path.join(observer_dir, base))
            candidates.append(os.path.join(os.getcwd(), base))
    for cand in candidates:
        if not cand:
            continue
        norm = os.path.abspath(cand)
        tried.append(norm)
        if os.path.isfile(norm):
            return norm, tried
    errors.append("%s not found; tried: %s" % (label, ", ".join(tried) if tried else "(none)"))
    return None, tried


# ---------------------------------------------------------------------------
# CSV loading / validation
# ---------------------------------------------------------------------------
def load_frames(path, errors):
    rows = []
    with open(path, "r", encoding="utf-8-sig", newline="") as fh:
        reader = csv.DictReader(fh)
        if reader.fieldnames is None:
            errors.append("frames.csv has no header")
            return None
        missing = [c for c in CSV_REQUIRED_COLUMNS if c not in reader.fieldnames]
        if missing:
            errors.append("frames.csv missing required columns: " + ", ".join(missing))
            return None
        for lineno, raw in enumerate(reader, start=2):
            row = {
                "line": lineno,
                "phase": (raw.get("phase") or "").strip(),
                "acquired_qpc": as_int(raw.get("acquired_qpc")),
                "qpc_before": as_int(raw.get("qpc_before")),
                "qpc_after": as_int(raw.get("qpc_after")),
                "last_present_time": as_int(raw.get("last_present_time")),
                "accumulated_frames": as_int(raw.get("accumulated_frames"), 0),
                "copied": as_bool01(raw.get("copied")),
                "cursor_overlap": as_bool01(raw.get("cursor_overlap")),
                "left": as_int(raw.get("left")),
                "right": as_int(raw.get("right")),
                "width": as_int(raw.get("width")),
                "width_drift": as_bool01(raw.get("width_drift")),
                "valid": as_bool01(raw.get("valid")),
                "scan_reason": (raw.get("scan_reason") or "").strip(),
                "content_changed": as_bool01(raw.get("content_changed")),
                "signature_changed": as_bool01(raw.get("signature_changed")),
                "stale": as_bool01(raw.get("stale")),
                "pointer_only": as_bool01(raw.get("pointer_only")),
                "error": (raw.get("error") or "").strip(),
                "seq": as_int(raw.get("seq")),
            }
            if row["seq"] is None:
                row["seq"] = lineno - 1
            rows.append(row)
    if not rows:
        errors.append("frames.csv contains no data rows")
        return None
    return rows


def validate_timing(rows, errors):
    """Monotonic sequence and timing; no duplicate seq counting."""
    duplicate_seq = False
    nonmonotonic_seq = False
    nonmonotonic_acquired = False
    bracket_violation = False
    last_present_monotonic = True
    seen = set()
    prev_seq = None
    prev_acquired = None
    prev_lp_measure = None
    for row in rows:
        seq = row["seq"]
        if seq in seen:
            duplicate_seq = True
        seen.add(seq)
        if prev_seq is not None and seq <= prev_seq:
            nonmonotonic_seq = True
        prev_seq = seq
        acquired = row["acquired_qpc"]
        if acquired is not None and prev_acquired is not None and acquired < prev_acquired:
            nonmonotonic_acquired = True
        if acquired is not None:
            prev_acquired = acquired
        qb, qa = row["qpc_before"], row["qpc_after"]
        if qb is not None and qa is not None and qb > qa:
            bracket_violation = True
        # last_present monotonic for measured, content-eligible, non-stale rows
        if (
            row["phase"] != "warmup"
            and not row["pointer_only"]
            and not row["stale"]
            and row["last_present_time"] is not None
            and row["last_present_time"] > 0
        ):
            lp = row["last_present_time"]
            if prev_lp_measure is not None and lp < prev_lp_measure:
                last_present_monotonic = False
            prev_lp_measure = lp
    result = {
        "duplicate_seq": duplicate_seq,
        "seq_strictly_increasing": not duplicate_seq and not nonmonotonic_seq,
        "acquired_qpc_monotonic": not nonmonotonic_acquired,
        "qpc_bracket_sane": not bracket_violation,
        "last_present_time_monotonic_measured": last_present_monotonic,
    }
    if duplicate_seq:
        errors.append("frames.csv has duplicate seq values")
    if nonmonotonic_seq:
        errors.append("frames.csv seq is not strictly increasing")
    if nonmonotonic_acquired:
        errors.append("frames.csv acquired_qpc is not monotonic")
    if bracket_violation:
        errors.append("frames.csv has qpc_before > qpc_after")
    if not last_present_monotonic:
        errors.append("frames.csv last_present_time regressed on a measured content row")
    return result


# ---------------------------------------------------------------------------
# Classification
# ---------------------------------------------------------------------------
def stale_reason(error_text):
    text = (error_text or "").lower()
    for token in ("nonpositive_present", "nonmonotonic_present", "future_present", "stale_present"):
        if token in text:
            return token
    return "stale_other"


def classify_row(row):
    if row["pointer_only"]:
        return "pointer_only"
    if row["stale"]:
        return stale_reason(row["error"])
    if row["cursor_overlap"]:
        return "cursor_overlap"
    if not row["copied"]:
        return "scan_error"
    if row["valid"]:
        return "valid"
    reason = row["scan_reason"]
    if reason == "no_target_pixels":
        return "missing"
    if reason in AMBIGUOUS_REASONS:
        return "ambiguous_" + reason
    if reason == "":
        return "invalid_unknown"
    return "ambiguous_" + reason


# ---------------------------------------------------------------------------
# Core reduction
# ---------------------------------------------------------------------------
def reduce_frames(rows, freq, start_qpc, end_qpc):
    counts = {
        "total_rows": len(rows),
        "valid_rows": 0,
        "valid_in_window": 0,
        "valid_before_window": 0,
        "valid_after_window": 0,
        "invalid_rows": 0,
        "phases": {},
        "primary_reason": {},
        "flags": {
            "pointer_only": 0,
            "stale": 0,
            "cursor_overlap": 0,
            "merged": 0,
            "width_drift": 0,
            "content_changed_emitted": 0,
            "signature_changed_emitted": 0,
        },
    }
    lost_captures = 0
    lost_captures_in_window = 0
    prev_edges = None
    updates = []
    mismatch_all = []
    mismatch_in_window = []
    emitted_in_window = 0
    recomputed_in_window = 0
    in_window_exclusions = {}
    in_window_merged = 0

    for row in rows:
        reason = classify_row(row)
        is_valid = reason == "valid"
        counts["phases"][row["phase"]] = counts["phases"].get(row["phase"], 0) + 1
        counts["primary_reason"][reason] = counts["primary_reason"].get(reason, 0) + 1
        if row["pointer_only"]:
            counts["flags"]["pointer_only"] += 1
        if row["stale"]:
            counts["flags"]["stale"] += 1
        if row["cursor_overlap"]:
            counts["flags"]["cursor_overlap"] += 1
        merged_row = row["accumulated_frames"] is not None and row["accumulated_frames"] > 1
        if merged_row:
            counts["flags"]["merged"] += 1
            lost_captures += row["accumulated_frames"] - 1
        if row["width_drift"]:
            counts["flags"]["width_drift"] += 1
        if row["content_changed"]:
            counts["flags"]["content_changed_emitted"] += 1
        if row["signature_changed"]:
            counts["flags"]["signature_changed_emitted"] += 1

        lp = row["last_present_time"]
        in_window = lp is not None and start_qpc < lp < end_qpc
        if is_valid:
            counts["valid_rows"] += 1
            if in_window:
                counts["valid_in_window"] += 1
            elif lp is not None and lp <= start_qpc:
                counts["valid_before_window"] += 1
            else:
                counts["valid_after_window"] += 1

            motion = prev_edges is not None and (row["left"], row["right"]) != prev_edges
            if motion != row["content_changed"]:
                entry = {"seq": row["seq"], "line": row["line"],
                         "last_present_time": lp, "recomputed": motion,
                         "emitted": row["content_changed"],
                         "left": row["left"], "right": row["right"]}
                mismatch_all.append(entry)
                if in_window:
                    mismatch_in_window.append(entry)
            if in_window:
                emitted_in_window += 1 if row["content_changed"] else 0
                if motion:
                    recomputed_in_window += 1
                    updates.append(lp)
                if merged_row:
                    in_window_merged += 1
                    lost_captures_in_window += row["accumulated_frames"] - 1
            prev_edges = (row["left"], row["right"])
        else:
            counts["invalid_rows"] += 1
            if in_window and not row["pointer_only"]:
                in_window_exclusions[reason] = in_window_exclusions.get(reason, 0) + 1

    return {
        "counts": counts,
        "lost_captures": lost_captures,
        "lost_captures_in_window": lost_captures_in_window,
        "updates": updates,
        "mismatch_all": mismatch_all,
        "mismatch_in_window": mismatch_in_window,
        "emitted_in_window": emitted_in_window,
        "recomputed_in_window": recomputed_in_window,
        "in_window_exclusions": in_window_exclusions,
        "in_window_merged": in_window_merged,
    }


def build_cadence(updates, start_qpc, end_qpc, freq):
    duration_seconds = (end_qpc - start_qpc) / float(freq)
    n_updates = len(updates)
    cadence = (n_updates / duration_seconds) if duration_seconds > 0 else None

    interval_qpc = [updates[i + 1] - updates[i] for i in range(len(updates) - 1)]
    interval_seconds = [v / float(freq) for v in interval_qpc]
    interval_ms = [v * 1000.0 for v in interval_seconds]
    stats = {
        "count": len(interval_ms),
        "quantile_method": QUANTILE_METHOD,
        "p50_ms": round_or_none(quantile_type7(interval_ms, 0.50)),
        "p95_ms": round_or_none(quantile_type7(interval_ms, 0.95)),
        "max_ms": round_or_none(max(interval_ms)) if interval_ms else None,
        "p50_seconds": round_or_none(quantile_type7(interval_seconds, 0.50), 12),
        "p95_seconds": round_or_none(quantile_type7(interval_seconds, 0.95), 12),
        "max_seconds": round_or_none(max(interval_seconds), 12) if interval_seconds else None,
    }

    # No-update spans include the censored window edges.
    edges = [start_qpc] + list(updates) + [end_qpc]
    spans = []
    for i in range(len(edges) - 1):
        if n_updates == 0:
            label = "whole_window"
        elif i == 0:
            label = "leading_censored"
        elif i == len(edges) - 2:
            label = "trailing_censored"
        else:
            label = "interupdate"
        spans.append({
            "start_qpc": edges[i],
            "end_qpc": edges[i + 1],
            "seconds": (edges[i + 1] - edges[i]) / float(freq),
            "type": label,
        })
    longest = max(spans, key=lambda s: s["seconds"]) if spans else None
    if n_updates == 0 and spans:
        # A whole window with no update is simultaneously the leading and the
        # trailing censored span.
        leading = spans[0]
        trailing = spans[0]
    else:
        leading = next((s for s in spans if s["type"] == "leading_censored"), None)
        trailing = next((s for s in spans if s["type"] == "trailing_censored"), None)
    internal = [s for s in spans if s["type"] == "interupdate"]

    return {
        "total_duration_seconds": round_or_none(duration_seconds, 12),
        "target_motion_updates": n_updates,
        "composed_target_updates_per_second": round_or_none(cadence, 9),
        "interupdate_intervals": stats,
        "longest_no_update_span": {
            "type": longest["type"] if longest else None,
            "seconds": round_or_none(longest["seconds"], 12) if longest else None,
            "start_qpc": longest["start_qpc"] if longest else None,
            "end_qpc": longest["end_qpc"] if longest else None,
        },
        "leading_no_update_span_seconds": round_or_none(leading["seconds"], 12) if leading else None,
        "trailing_no_update_span_seconds": round_or_none(trailing["seconds"], 12) if trailing else None,
        "longest_internal_no_update_span_seconds": round_or_none(max(s["seconds"] for s in internal), 12) if internal else None,
    }


# ---------------------------------------------------------------------------
# Top-level driver
# ---------------------------------------------------------------------------
def reduce_trial(summary_path, frames_path, metadata_path, out_path):
    errors = []
    warnings = []
    report = {
        "schema": SCHEMA,
        "status": "invalid",
        "ok": False,
        "excluded_reason": None,
        "errors": errors,
        "warnings": warnings,
    }

    try:
        with open(summary_path, "r", encoding="utf-8-sig") as fh:
            summary = json.load(fh)
    except Exception as exc:  # noqa: BLE001 - report and fail closed
        raise UsageError("cannot read input-summary %s: %s" % (summary_path, exc))
    if not isinstance(summary, dict):
        raise UsageError("input-summary is not a JSON object")

    observer = summary.get("observer") if isinstance(summary.get("observer"), dict) else None
    report["inputs"] = {
        "input_summary_path": os.path.abspath(summary_path),
        "input_summary_sha256": sha256_file(summary_path),
        "frames_path": os.path.abspath(frames_path) if frames_path else None,
        "metadata_path": os.path.abspath(metadata_path) if metadata_path else None,
        "workload": summary.get("workload"),
        "protocol_identity": summary.get("protocol_identity"),
    }

    # --- observer-off trials are excluded (not 0 FPS) ----------------------
    if observer is None or not as_bool01(observer.get("requested")):
        report["status"] = "excluded"
        report["ok"] = True
        report["excluded_reason"] = "observer_off"
        warnings.append("observer was not requested/recorded; trial excluded, not a 0-FPS cadence")
        write_report(report, out_path)
        return 0, report

    report["observer"] = {
        "schema": observer.get("schema"),
        "exe_actual_sha256": observer.get("exe_actual_sha256"),
        "expected_exe_sha256": observer.get("expected_exe_sha256"),
        "ready_qpc": as_int(observer.get("ready_qpc")),
        "ready_seen": as_bool01(observer.get("ready_seen")),
        "started_qpc": as_int(observer.get("started_qpc")),
        "stopped_qpc": as_int(observer.get("stopped_qpc")),
        "exited": observer.get("exited"),
        "exit_code": as_int(observer.get("exit_code")),
        "forced_kill": as_bool01(observer.get("forced_kill")),
        "timed_out": as_bool01(observer.get("timed_out")),
        "frames_written": as_int(observer.get("frames_written")),
        "valid_frames": as_int(observer.get("valid_frames")),
        "missing_frames": as_int(observer.get("missing_frames")),
        "merged_frames": as_int(observer.get("merged_frames")),
        "ambiguous_frames": as_int(observer.get("ambiguous_frames")),
        "content_changes": as_int(observer.get("content_changes")),
        "errors": observer.get("errors"),
        "metadata_sha256_declared": observer.get("metadata_sha256"),
        "frames_csv_sha256_declared": observer.get("frames_csv_sha256"),
    }

    # --- phase boundaries / window -----------------------------------------
    pb = summary.get("phase_boundaries") if isinstance(summary.get("phase_boundaries"), dict) else {}
    start_qpc = as_int(pb.get("input_start_qpc"), as_int(summary.get("input_start_qpc")))
    end_qpc = as_int(pb.get("input_end_qpc"), as_int(summary.get("input_end_qpc")))
    pb_freq = as_int(pb.get("qpc_frequency"))
    summary_freq = as_int(summary.get("qpc_frequency"))
    if as_int(pb.get("complete")) != 1 and not as_bool01(pb.get("complete")):
        errors.append("phase_boundaries.complete is not true")
    if pb.get("status") != "complete":
        errors.append("phase_boundaries.status is not complete (got %r)" % (pb.get("status"),))
    if start_qpc is None or end_qpc is None:
        errors.append("input window boundaries are missing")
    elif not (start_qpc < end_qpc):
        errors.append("input_start_qpc is not strictly before input_end_qpc")

    # --- metadata -----------------------------------------------------------
    meta = None
    if metadata_path:
        try:
            with open(metadata_path, "r", encoding="utf-8-sig") as fh:
                meta = json.load(fh)
        except Exception as exc:  # noqa: BLE001
            errors.append("cannot parse observer-metadata.json: %s" % exc)
        if meta is not None and not isinstance(meta, dict):
            errors.append("observer-metadata.json is not a JSON object")
            meta = None
    else:
        errors.append("observer-metadata.json path could not be resolved")

    meta_freq = as_int(meta.get("qpc_frequency")) if isinstance(meta, dict) else None
    freq_candidates = [f for f in (pb_freq, summary_freq, meta_freq) if f is not None]
    if not freq_candidates or any(f <= 0 for f in freq_candidates):
        errors.append("qpc_frequency is missing or non-positive")
        freq = None
    else:
        freq = freq_candidates[0]
    freq_match = bool(freq_candidates) and all(f == freq for f in freq_candidates)
    if not freq_match:
        errors.append("qpc_frequency mismatch across input/metadata: %r" % (freq_candidates,))
    report["clock"] = {
        "input_qpc_frequency": summary_freq,
        "phase_boundaries_qpc_frequency": pb_freq,
        "metadata_qpc_frequency": meta_freq,
        "selected_qpc_frequency": freq,
        "match": freq_match,
    }

    # --- hash verification (where provided) --------------------------------
    hash_block = {
        "frames_sha256_declared": observer.get("frames_csv_sha256"),
        "frames_sha256_actual": None,
        "frames_match": None,
        "metadata_sha256_declared": observer.get("metadata_sha256"),
        "metadata_sha256_actual": None,
        "metadata_match": None,
        "exe_sha256_actual": observer.get("exe_actual_sha256"),
        "exe_sha256_expected": observer.get("expected_exe_sha256"),
        "exe_match": None,
        "roi_config_sha256_actual": observer.get("roi_config_actual_sha256"),
        "roi_config_sha256_expected": observer.get("expected_roi_config_sha256"),
        "roi_config_match": None,
        "checks": [],
    }
    if frames_path and os.path.isfile(frames_path):
        hash_block["frames_sha256_actual"] = sha256_file(frames_path)
        declared = hash_block["frames_sha256_declared"]
        if declared:
            hash_block["frames_match"] = declared.lower() == hash_block["frames_sha256_actual"].lower()
            hash_block["checks"].append("frames.csv")
            if not hash_block["frames_match"]:
                errors.append("frames.csv sha256 does not match the declared observer hash")
    if metadata_path and os.path.isfile(metadata_path):
        hash_block["metadata_sha256_actual"] = sha256_file(metadata_path)
        declared = hash_block["metadata_sha256_declared"]
        if declared:
            hash_block["metadata_match"] = declared.lower() == hash_block["metadata_sha256_actual"].lower()
            hash_block["checks"].append("observer-metadata.json")
            if not hash_block["metadata_match"]:
                errors.append("observer-metadata.json sha256 does not match the declared observer hash")
    if hash_block["exe_sha256_expected"]:
        actual = hash_block["exe_sha256_actual"]
        hash_block["exe_match"] = bool(actual) and actual.lower() == hash_block["exe_sha256_expected"].lower()
        hash_block["checks"].append("observer-exe")
        if not hash_block["exe_match"]:
            errors.append("observer exe sha256 actual does not match expected")
    if hash_block["roi_config_sha256_expected"]:
        actual = hash_block["roi_config_sha256_actual"]
        hash_block["roi_config_match"] = bool(actual) and actual.lower() == hash_block["roi_config_sha256_expected"].lower()
        hash_block["checks"].append("roi-config")
        if not hash_block["roi_config_match"]:
            errors.append("roi config sha256 actual does not match expected")
    report["hash_verification"] = hash_block

    # --- load and validate frames ------------------------------------------
    if not frames_path or not os.path.isfile(frames_path):
        errors.append("frames.csv path could not be resolved")
        rows = None
    else:
        try:
            rows = load_frames(frames_path, errors)
        except Exception as exc:  # noqa: BLE001
            errors.append("cannot parse frames.csv: %s" % exc)
            rows = None

    # --- observer normal exit and ready ordering ---------------------------
    ready_qpc = report["observer"]["ready_qpc"]
    ready_ok = ready_qpc is not None and start_qpc is not None and ready_qpc <= start_qpc
    if not ready_ok:
        errors.append("observer.ready_qpc is missing or not <= input_start_qpc")
    if not as_bool01(observer.get("ready_seen")):
        errors.append("observer.ready_seen is not true")
    if not as_bool01(observer.get("exited")):
        errors.append("observer did not exit normally (exited is false)")
    if report["observer"]["exit_code"] != 0:
        errors.append("observer exit_code was %r" % (report["observer"]["exit_code"],))
    if report["observer"]["forced_kill"]:
        errors.append("observer had to be force-killed")
    if report["observer"]["timed_out"]:
        errors.append("observer stop timed out")

    report["window"] = {
        "input_start_qpc": start_qpc,
        "input_end_qpc": end_qpc,
        "window_seconds": round_or_none((end_qpc - start_qpc) / float(freq), 12) if (start_qpc is not None and end_qpc is not None and freq) else None,
        "membership_convention": WINDOW_CONVENTION,
        "observer_ready_qpc": ready_qpc,
        "observer_ready_before_input_start": ready_ok,
        "observer_started_qpc": report["observer"]["started_qpc"],
        "observer_stopped_qpc": report["observer"]["stopped_qpc"],
    }
    if isinstance(meta, dict):
        mstart, mend = as_int(meta.get("qpc_start")), as_int(meta.get("qpc_end"))
        report["observer_metadata"] = {
            "schema": meta.get("schema"),
            "observer_version": meta.get("observer_version"),
            "qpc_start": mstart,
            "qpc_ready": as_int(meta.get("qpc_ready")),
            "qpc_end": mend,
            "span_seconds": round_or_none((mend - mstart) / float(freq), 12) if (mstart is not None and mend is not None and freq) else None,
            "exit_code": as_int(meta.get("exit_code")),
            "exit_reason": meta.get("exit_reason"),
            "fatal": as_bool01(meta.get("fatal")),
            "counts": meta.get("counts"),
        }

    if rows is None or freq is None or start_qpc is None or end_qpc is None or not (start_qpc < end_qpc):
        report["status"] = "invalid"
        report["ok"] = False
        write_report(report, out_path)
        return 3, report

    timing = validate_timing(rows, errors)
    report["row_validation"] = timing

    reduction = reduce_frames(rows, freq, start_qpc, end_qpc)
    counts = reduction["counts"]

    # --- no valid frames inside the input window => invalid ----------------
    if counts["valid_in_window"] == 0:
        errors.append("no valid frames with LastPresentTime strictly inside the input window")

    # --- emitted-vs-recomputed flag comparison -----------------------------
    if reduction["mismatch_all"]:
        errors.append("recomputed motion flag disagrees with emitted content_changed on %d row(s)"
                      % len(reduction["mismatch_all"]))

    cadence = build_cadence(reduction["updates"], start_qpc, end_qpc, freq)

    lower_bound = bool(
        reduction["in_window_merged"]
        or reduction["in_window_exclusions"]
    )
    zero_motion_complete = (
        cadence["target_motion_updates"] == 0
        and counts["valid_in_window"] > 0
        and not reduction["in_window_exclusions"]
        and reduction["in_window_merged"] == 0
    )

    report["counts"] = {
        "total_rows": counts["total_rows"],
        "valid_rows": counts["valid_rows"],
        "valid_in_window": counts["valid_in_window"],
        "valid_before_window": counts["valid_before_window"],
        "valid_after_window": counts["valid_after_window"],
        "invalid_rows": counts["invalid_rows"],
        "phases": counts["phases"],
        "primary_reason": counts["primary_reason"],
        "flags": counts["flags"],
        "in_window_exclusions": reduction["in_window_exclusions"],
        "in_window_content_exclusions_total": sum(reduction["in_window_exclusions"].values()),
        "in_window_merged": reduction["in_window_merged"],
        "accumulated_lost_captures_lowerbound": reduction["lost_captures"],
        "accumulated_lost_captures_lowerbound_in_window": reduction["lost_captures_in_window"],
        "observer_metadata_counts": meta.get("counts") if isinstance(meta, dict) else None,
    }
    report["recomputed"] = cadence
    report["recomputed"]["observed_cadence_is_lower_bound"] = lower_bound
    report["recomputed"]["lower_bound_reasons"] = sorted(
        list(reduction["in_window_exclusions"].keys())
        + (["merged"] if reduction["in_window_merged"] else []))
    report["recomputed"]["zero_motion_valid0cadence"] = zero_motion_complete
    report["emitted_vs_recomputed"] = {
        "in_window_valid_rows": counts["valid_in_window"],
        "emitted_content_changes_in_window": reduction["emitted_in_window"],
        "recomputed_motion_updates_in_window": reduction["recomputed_in_window"],
        "match": len(reduction["mismatch_all"]) == 0,
        "mismatch_count": len(reduction["mismatch_all"]),
        "mismatch_in_window_count": len(reduction["mismatch_in_window"]),
        "mismatches": reduction["mismatch_all"][:50],
    }
    report["input_identity"] = {
        "input_start_qpc": start_qpc,
        "input_end_qpc": end_qpc,
        "qpc_frequency": freq,
        "window_identity_sha256": hashlib.sha256(
            ("%d:%d:%d" % (start_qpc, end_qpc, freq)).encode("ascii")
        ).hexdigest(),
    }

    # metadata count reconciliation, visible but only frames_written is fatal
    meta_counts = meta.get("counts") if isinstance(meta, dict) else None
    if isinstance(meta_counts, dict):
        fw = as_int(meta_counts.get("frames_written"))
        if fw is not None and fw != counts["total_rows"]:
            errors.append("observer metadata counts.frames_written=%d != frames.csv rows=%d"
                          % (fw, counts["total_rows"]))
        comparisons = [
            ("valid", as_int(meta_counts.get("valid")), sum(
                1 for r in rows if r["valid"] and r["phase"] != "warmup")),
            ("missing", as_int(meta_counts.get("missing")), counts["primary_reason"].get("missing", 0)),
            ("merged", as_int(meta_counts.get("merged")), counts["flags"]["merged"]),
            ("ambiguous", as_int(meta_counts.get("ambiguous")), sum(
                v for k, v in counts["primary_reason"].items() if k.startswith("ambiguous_"))),
            ("content_changes", as_int(meta_counts.get("content_changes")), counts["flags"]["content_changed_emitted"]),
        ]
        report["metadata_consistency"] = []
        for name, declared, derived in comparisons:
            entry = {"name": name, "metadata": declared, "csv_derived": derived, "match": declared == derived}
            report["metadata_consistency"].append(entry)
            if declared is not None and declared != derived:
                warnings.append("metadata counts.%s=%r != csv-derived %r" % (name, declared, derived))

    report["notes"] = [
        "LastPresentTime is compositor-observed software presentation in QPC units; not photons and not display refresh.",
        "No input-to-frame latency or nearest-input association is computed.",
        "ROI signature changes are never counted as motion; only immediate-prior-valid edge displacement counts.",
        "Merged/invalid observations make the observed cadence a lower bound; no values are interpolated.",
        "Cursor exclusion is conservative, not perfect.",
        "Observer-off trials are excluded (not 0 FPS); perturbation controls stay separate.",
        "Whole input window is the primary measure even when the input contains repeated loops.",
    ]

    status_valid = len(errors) == 0
    report["status"] = "valid" if status_valid else "invalid"
    report["ok"] = status_valid
    write_report(report, out_path)
    return (0 if status_valid else 3), report


def build_parser():
    parser = argparse.ArgumentParser(
        description="Reduce Pan input-summary + observer frames into a cadence JSON report (stdlib only).")
    parser.add_argument("--input-summary", required=True, help="Path to Pan input-summary.json")
    parser.add_argument("--frames", help="Path to observer frames.csv")
    parser.add_argument("--metadata", help="Path to observer-metadata.json")
    parser.add_argument("--observer-dir", help="Directory containing frames.csv and observer-metadata.json")
    parser.add_argument("--out", help="Write JSON report to this path (default stdout)")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    errors = []
    summary_path = args.input_summary
    if not os.path.isfile(summary_path):
        sys.stderr.write("input-summary not found: %s\n" % summary_path)
        return 2
    try:
        with open(summary_path, "r", encoding="utf-8-sig") as fh:
            summary = json.load(fh)
    except Exception as exc:  # noqa: BLE001
        sys.stderr.write("cannot parse input-summary: %s\n" % exc)
        return 2

    observer = summary.get("observer") if isinstance(summary.get("observer"), dict) else None

    frames_path = args.frames
    metadata_path = args.metadata
    if not frames_path and observer and observer.get("frames_csv_path"):
        frames_path, _ = resolve_local_path(observer.get("frames_csv_path"), summary_path,
                                            args.observer_dir, "frames.csv", errors)
    elif not frames_path and args.observer_dir:
        cand = os.path.join(args.observer_dir, "frames.csv")
        frames_path = cand if os.path.isfile(cand) else None
    if not metadata_path and observer and observer.get("metadata_path"):
        metadata_path, _ = resolve_local_path(observer.get("metadata_path"), summary_path,
                                              args.observer_dir, "observer-metadata.json", errors)
    elif not metadata_path and args.observer_dir:
        cand = os.path.join(args.observer_dir, "observer-metadata.json")
        metadata_path = cand if os.path.isfile(cand) else None

    # A missing artifact only matters for observer-on trials; defer to reduce.
    for msg in errors:
        sys.stderr.write("warning: %s\n" % msg)

    try:
        code, _ = reduce_trial(summary_path, frames_path, metadata_path, args.out)
        return code
    except UsageError as exc:
        sys.stderr.write("%s\n" % exc)
        return 2


if __name__ == "__main__":
    sys.exit(main())
