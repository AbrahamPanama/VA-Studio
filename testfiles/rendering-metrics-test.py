#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Outcome tests for packaging/windows/vacards/rendering-metrics.py.

The 70-column schema-4 header and the expected percentile/rate values are
written independently here (not imported from the tool) so a mistaken
assumption shared with the implementation cannot make a case pass.

Runs the tool as a subprocess, inspects exit code, output presence and parsed
JSON, prints ``CASE <name> PASS|FAIL`` and exits 0 only when every case passes.
No external Python dependency, no network, no writes outside the temp dir.

Usage: python3 testfiles/rendering-metrics-test.py
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

TOOL = (
    Path(__file__).resolve().parents[1]
    / "packaging"
    / "windows"
    / "vacards"
    / "rendering-metrics.py"
)

# Independently written from src/ui/widget/canvas/rendering-stats.h.
BASE = [
    "sample_us",
    "interval_ms",
    "canvas_id",
    "gtk_renderer",
    "canvas_backend",
    "logical_width",
    "logical_height",
    "scale",
    "threads",
    "gtk_cycles",
    "schema_version",
    "render_mode",
    "split_mode",
    "dragging",
]
EVENTS = [
    "tiles",
    "pixels",
    "redraws",
    "timeouts",
    "store_recreated",
    "store_shifted",
    "redraw_requests",
    "coalesced_requests",
    "idle_callbacks",
    "instant_callbacks",
    "motion_received",
    "motion_ignored",
    "button_motion",
    "button_presses",
    "button_releases",
    "paint_launches",
    "deadline_callbacks",
]
TIMINGS = [
    "raster",
    "buffer_wait",
    "commit",
    "paint",
    "queue",
    "update",
    "redraw",
    "dispatch",
    "snapshot",
    "callback_wait",
    "redraw_context",
    "motion_handler",
    "event_handler",
]
HEADER = list(BASE) + list(EVENTS)
for _timing in TIMINGS:
    for _suffix in ("_count", "_total_us", "_lifetime_max_us"):
        HEADER.append(_timing + _suffix)


def row(
    sample_us,
    interval_ms,
    *,
    canvas_id=0,
    events=None,
    counts=None,
    totals=None,
    lifetimes=None,
    gtk_renderer="GskCairoRenderer",
    canvas_backend="Cairo",
    schema_version=4,
    gtk_cycles=10,
    interval_override=None,
):
    """Build one schema-4 CSV row from named fields; unset fields are 0."""
    events = events or {}
    counts = counts or {}
    totals = totals or {}
    lifetimes = lifetimes or {}
    values = {
        "sample_us": sample_us,
        "interval_ms": interval_ms,
        "canvas_id": canvas_id,
        "gtk_renderer": gtk_renderer,
        "canvas_backend": canvas_backend,
        "logical_width": 1920,
        "logical_height": 1080,
        "scale": 1,
        "threads": 8,
        "gtk_cycles": gtk_cycles,
        "schema_version": schema_version,
        "render_mode": 0,
        "split_mode": 0,
        "dragging": 0,
    }
    for name in EVENTS:
        values[name] = events.get(name, 0)
    for name in TIMINGS:
        values[name + "_count"] = counts.get(name, 0)
        values[name + "_total_us"] = totals.get(name, 0)
        values[name + "_lifetime_max_us"] = lifetimes.get(name, 0)
    if interval_override is not None:
        values["interval_ms"] = interval_override
    return ",".join(str(values[column]) for column in HEADER)


def csv_text(*rows, header=None):
    lines = [",".join(header) if header is not None else ",".join(HEADER)]
    lines.extend(rows)
    return "\n".join(lines) + "\n"


class Cli:
    def __init__(self, returncode, stdout, stderr, payload, csv_bytes, stderr_bytes):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr
        self.payload = payload
        self.csv_bytes = csv_bytes
        self.stderr_bytes = stderr_bytes


def invoke(csv_body, stderr_body=None, extra=()):
    """Run the tool on a fresh temp dir; return exit data and the parsed output."""
    with tempfile.TemporaryDirectory(prefix="vacards-rendering-metrics-") as directory:
        csv_path = Path(directory) / "rendering.csv"
        csv_path.write_text(csv_body, encoding="utf-8")
        command = [sys.executable, str(TOOL), "--csv", str(csv_path)]
        stderr_path = None
        if stderr_body is not None:
            stderr_path = Path(directory) / "app-stderr.log"
            stderr_path.write_text(stderr_body, encoding="utf-8")
            command += ["--stderr", str(stderr_path)]
        out_path = Path(directory) / "metrics.json"
        command += ["--out", str(out_path)]
        command += list(extra)
        completed = subprocess.run(
            command, capture_output=True, text=True, cwd=directory
        )
        payload = None
        if out_path.exists():
            payload = json.loads(out_path.read_text(encoding="utf-8"))
        return Cli(
            completed.returncode,
            completed.stdout,
            completed.stderr,
            payload,
            csv_path.read_bytes(),
            stderr_path.read_bytes() if stderr_path is not None else None,
        )


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def expect_rejected(result, case):
    check(
        result.returncode != 0,
        f"{case}: expected nonzero exit, got {result.returncode}",
    )
    check(
        result.payload is None,
        f"{case}: expected no --out file on rejection (fail-closed)",
    )


CASES = []


def case(function):
    CASES.append(function)
    return function


# --------------------------------------------------------------------------
# 1. Independently known nearest-rank percentiles.
# --------------------------------------------------------------------------
@case
def known_percentiles():
    # Sorted paint values (us): 1000,2000,3000,4000. n=4.
    # p50 rank=ceil(0.50*4)=2 -> 2000us=2.0ms; p95 rank=ceil(0.95*4)=4 -> 4000us=4.0ms.
    stderr = "\n".join(
        f"vacards_frame_paint_us={value}" for value in (3000, 1000, 4000, 2000)
    ) + "\n"
    stderr += "vacards_frame_wait_us=500\n"
    stderr += "unrelated log line\n"
    result = invoke(
        csv_text(row(1000, 500.0, events={"tiles": 1, "pixels": 10})),
        stderr,
    )
    check(result.returncode == 0, result.stderr)
    metrics = result.payload["frame_timing"]["metrics"]
    paint = metrics["frame_paint_us"]
    check(paint["n"] == 4, f"paint n={paint['n']}")
    check(paint["p50_ms"] == 2.0, f"paint p50={paint['p50_ms']}")
    check(paint["p95_ms"] == 4.0, f"paint p95={paint['p95_ms']}")
    check(paint["min_ms"] == 1.0, f"paint min={paint['min_ms']}")
    check(paint["max_ms"] == 4.0, f"paint max={paint['max_ms']}")
    wait = metrics["frame_wait_us"]
    check(wait["n"] == 1 and wait["p50_ms"] == 0.5 and wait["p95_ms"] == 0.5, wait)
    absent = metrics["input_latency_us"]
    check(absent["n"] == 0 and absent["p50_ms"] is None, absent)


# --------------------------------------------------------------------------
# 2. Interval-weighted rates from rows that are already deltas.
# --------------------------------------------------------------------------
@case
def interval_weighted_rates_without_double_difference():
    # Rows already hold interval deltas. Weighted paint mean must be
    # (1000+2000)/ (2+3) /1000 = 0.6 ms exactly, and lifetime is the max 9000us.
    body = csv_text(
        row(
            1000,
            500.0,
            events={"tiles": 10, "pixels": 1_000_000},
            counts={"paint": 2},
            totals={"paint": 1000},
            lifetimes={"paint": 5000},
        ),
        row(
            1500,
            500.0,
            events={"tiles": 30, "pixels": 3_000_000},
            counts={"paint": 3},
            totals={"paint": 2000},
            lifetimes={"paint": 9000},
        ),
    )
    result = invoke(body, None)
    check(result.returncode == 0, result.stderr)
    scope = result.payload["scope"]
    check(scope["duration_s"] == 1.0, f"duration={scope['duration_s']}")
    check(scope["rows_differenced_again"] is False, scope)
    events = result.payload["events"]
    check(events["pixels"]["total"] == 4_000_000, events["pixels"])
    check(events["pixels"]["rastered_tile_mpix_s"] == 4.0, events["pixels"])
    check(events["pixels"]["label"] == "rastered-tile-Mpix/s", events["pixels"])
    check(events["tiles"]["per_s"] == 40.0, events["tiles"])
    paint = result.payload["stages"]["paint"]
    check(paint["total_count"] == 5, paint)
    check(paint["sum_total_us"] == 3000, paint)
    check(paint["weighted_mean_ms"] == 0.6, paint)
    check(paint["lifetime_max_us"] == 9000, paint)
    check(paint["lifetime_max_ms"] == 9.0, paint)
    check(paint["lifetime_label"] == "lifetime", paint)
    # If adjacent rows had been differenced again, weight would use totals like
    # (2000-1000)=1000 over counts (3-2)=1 -> 1.0 ms, which is not 0.6.
    check(paint["weighted_mean_ms"] != 1.0, "double-difference not detected")


# --------------------------------------------------------------------------
# 3. Zero-count stage: mean is null, never 0.
# --------------------------------------------------------------------------
@case
def zero_count_stage_mean_is_null():
    body = csv_text(
        row(
            1000,
            500.0,
            counts={"raster": 0, "paint": 4},
            totals={"raster": 0, "paint": 800},
            lifetimes={"raster": 0, "paint": 800},
        )
    )
    result = invoke(body, None)
    check(result.returncode == 0, result.stderr)
    raster = result.payload["stages"]["raster"]
    check(raster["total_count"] == 0, raster)
    check(raster["weighted_mean_ms"] is None, f"raster mean={raster['weighted_mean_ms']!r}")
    check(raster["weighted_mean_ms"] != 0, "zero mean must not be reported as 0")
    check(
        any("raster" in warning and "null" in warning for warning in result.payload["warnings"]),
        result.payload["warnings"],
    )
    # A genuinely measured zero-pixel interval keeps a real 0.0 rate, not null.
    check(result.payload["events"]["pixels"]["rastered_tile_mpix_s"] == 0.0, result.payload["events"]["pixels"])


# --------------------------------------------------------------------------
# 4. Missing frame lines and absent stderr.
# --------------------------------------------------------------------------
@case
def missing_frame_lines_are_unavailable():
    body = csv_text(row(1000, 500.0, events={"pixels": 10}))
    result = invoke(body, "some other line\nvacards_doc_update_us=5 more=0\n")
    check(result.returncode == 0, result.stderr)
    metrics = result.payload["frame_timing"]["metrics"]
    for key in ("frame_paint_us", "frame_wait_us", "input_latency_us"):
        check(metrics[key]["n"] == 0, metrics[key])
        check(metrics[key]["p50_ms"] is None, metrics[key])
        check(metrics[key]["status"] == "unavailable", metrics[key])


@case
def absent_stderr_option_is_allowed_and_null():
    body = csv_text(row(1000, 500.0, events={"pixels": 10}))
    result = invoke(body, None)
    check(result.returncode == 0, result.stderr)
    frame = result.payload["frame_timing"]
    check(frame["provided"] is False, frame)
    check(frame["warmup_applied"] is False, frame)
    for key in ("frame_paint_us", "frame_wait_us", "input_latency_us"):
        check(frame["metrics"][key]["n"] == 0, frame["metrics"][key])
    # No stderr input means no stderr hash entry.
    roles = [entry["role"] for entry in result.payload["inputs"]]
    check(roles == ["csv"], roles)


# --------------------------------------------------------------------------
# 5. Malformed header / schema / backend / width / nan / negative / interval.
# --------------------------------------------------------------------------
@case
def malformed_header_rejected():
    header = list(HEADER)
    header[0] = "time_us"
    body = csv_text(row(1000, 500.0), header=header)
    expect_rejected(invoke(body, None), "malformed_header")


@case
def wrong_schema_version_rejected():
    body = csv_text(row(1000, 500.0, schema_version=3))
    expect_rejected(invoke(body, None), "wrong_schema")


@case
def wrong_renderer_rejected():
    body = csv_text(row(1000, 500.0, gtk_renderer="GskGLRenderer"))
    expect_rejected(invoke(body, None), "wrong_renderer")


@case
def wrong_backend_rejected():
    body = csv_text(row(1000, 500.0, canvas_backend="OpenGL"))
    expect_rejected(invoke(body, None), "wrong_backend")


@case
def short_row_rejected():
    # 69 fields and a trailing newline: a complete malformed row, not a
    # truncated final write, so it must be rejected.
    body = ",".join(HEADER) + "\n" + ",".join(["1"] * (len(HEADER) - 1)) + "\n"
    expect_rejected(invoke(body, None), "short_row")


@case
def long_row_rejected():
    body = csv_text(row(1000, 500.0) + ",0")
    expect_rejected(invoke(body, None), "long_row")


@case
def truncated_final_row_rejected():
    # An interrupted/unflushed log can end mid-row without a trailing newline.
    # That is still a wrong-width row: it must be rejected, never dropped.
    body = ",".join(HEADER) + "\n" + row(1000, 500.0, events={"pixels": 42}) + "\n"
    body += "1,2,3"
    result = invoke(body, None)
    expect_rejected(result, "truncated_final_row")
    check(
        "expected 70 columns, found 3" in result.stderr,
        f"truncated_final_row: unclear error: {result.stderr!r}",
    )


@case
def truncated_only_data_row_rejected():
    # Same fail-closed rule when the partial write is the only data row.
    body = ",".join(HEADER) + "\n" + "478549091615,514.938,1,GskCairoRenderer,Cairo"
    expect_rejected(invoke(body, None), "truncated_only_row")


@case
def complete_final_row_without_trailing_newline_accepted():
    # A fully complete row is valid even when the file does not end in a newline.
    body = ",".join(HEADER) + "\n" + row(1000, 500.0, events={"pixels": 42})
    result = invoke(body, None)
    check(result.returncode == 0, result.stderr)
    check(result.payload["scope"]["rows_used"] == 1, result.payload["scope"])
    check(result.payload["events"]["pixels"]["total"] == 42, result.payload["events"])


@case
def nan_interval_rejected():
    body = csv_text(row(1000, 500.0, interval_override="nan"))
    expect_rejected(invoke(body, None), "nan_interval")


@case
def negative_pixels_rejected():
    body = csv_text(row(1000, 500.0, events={"pixels": -1}))
    expect_rejected(invoke(body, None), "negative_pixels")


@case
def zero_interval_rejected():
    body = csv_text(row(1000, 0.0))
    expect_rejected(invoke(body, None), "zero_interval")


@case
def negative_interval_rejected():
    body = csv_text(row(1000, -5.0))
    expect_rejected(invoke(body, None), "negative_interval")


@case
def negative_frame_value_rejected():
    body = csv_text(row(1000, 500.0))
    expect_rejected(invoke(body, "vacards_frame_paint_us=-7\n"), "negative_frame")


# --------------------------------------------------------------------------
# 6. Mixed canvases and non-monotonic samples.
# --------------------------------------------------------------------------
@case
def mixed_canvases_rejected_unless_selected():
    body = csv_text(
        row(1000, 500.0, canvas_id=0, events={"pixels": 100}),
        row(1000, 500.0, canvas_id=1, events={"pixels": 200}),
    )
    expect_rejected(invoke(body, None), "mixed_canvases_without_selection")
    selected = invoke(body, None, extra=["--canvas-id", "1"])
    check(selected.returncode == 0, selected.stderr)
    check(selected.payload["scope"]["canvas_id"] == 1, selected.payload["scope"])
    check(selected.payload["scope"]["rows_used"] == 1, selected.payload["scope"])
    check(selected.payload["events"]["pixels"]["total"] == 200, selected.payload["events"])
    check(selected.payload["scope"]["canvases_in_file"] == [0, 1], selected.payload["scope"])


@case
def non_monotonic_sample_us_rejected():
    body = csv_text(row(2000, 500.0), row(1000, 500.0))
    expect_rejected(invoke(body, None), "non_monotonic")


# --------------------------------------------------------------------------
# 7. Warmup trimming, and warmup must not pretend to trim stderr.
# --------------------------------------------------------------------------
@case
def warmup_rows_trim_csv_only():
    body = csv_text(
        row(1000, 1000.0, events={"pixels": 1_000_000}),
        row(2000, 500.0, events={"pixels": 1_000_000}),
        row(3000, 500.0, events={"pixels": 3_000_000}),
    )
    stderr = "vacards_frame_paint_us=1000\nvacards_frame_paint_us=2000\n"
    result = invoke(body, stderr, extra=["--warmup-rows", "1"])
    check(result.returncode == 0, result.stderr)
    scope = result.payload["scope"]
    check(scope["warmup_rows"] == 1, scope)
    check(scope["rows_used"] == 2, scope)
    check(scope["duration_s"] == 1.0, scope)
    check(result.payload["events"]["pixels"]["rastered_tile_mpix_s"] == 4.0, result.payload["events"])
    # Frame samples are from the whole log, not the trimmed CSV interval.
    check(result.payload["frame_timing"]["metrics"]["frame_paint_us"]["n"] == 2, result.payload["frame_timing"])
    check(result.payload["frame_timing"]["warmup_applied"] is False, result.payload["frame_timing"])


@case
def warmup_larger_than_rows_rejected():
    body = csv_text(row(1000, 500.0), row(2000, 500.0))
    expect_rejected(invoke(body, None, extra=["--warmup-rows", "2"]), "warmup_too_large")


@case
def negative_warmup_rejected():
    body = csv_text(row(1000, 500.0))
    expect_rejected(invoke(body, None, extra=["--warmup-rows", "-1"]), "negative_warmup")


@case
def canvas_id_not_present_rejected():
    body = csv_text(row(1000, 500.0, canvas_id=0))
    expect_rejected(invoke(body, None, extra=["--canvas-id", "7"]), "canvas_not_present")


# --------------------------------------------------------------------------
# 8. SHA-256 of the provided inputs is emitted with the paths.
# --------------------------------------------------------------------------
@case
def input_sha256_and_paths_emitted():
    body = csv_text(row(1000, 500.0, events={"pixels": 10}))
    stderr = "vacards_frame_paint_us=1000\n"
    result = invoke(body, stderr)
    check(result.returncode == 0, result.stderr)
    expected_csv = hashlib.sha256(result.csv_bytes).hexdigest()
    expected_stderr = hashlib.sha256(result.stderr_bytes).hexdigest()
    entries = {entry["role"]: entry for entry in result.payload["inputs"]}
    check(entries["csv"]["sha256"] == expected_csv, entries["csv"])
    check(entries["stderr"]["sha256"] == expected_stderr, entries["stderr"])
    check(entries["csv"]["path"].endswith("rendering.csv"), entries["csv"])
    check(entries["stderr"]["path"].endswith("app-stderr.log"), entries["stderr"])


# --------------------------------------------------------------------------
# 9. --out must never be the same file as --csv or --stderr.
# --------------------------------------------------------------------------
@case
def out_must_not_overwrite_csv():
    body = csv_text(row(1000, 500.0, events={"pixels": 10}))
    with tempfile.TemporaryDirectory(prefix="vacards-rendering-metrics-") as directory:
        csv_path = Path(directory) / "rendering.csv"
        csv_path.write_text(body, encoding="utf-8")
        before = csv_path.read_bytes()
        completed = subprocess.run(
            [sys.executable, str(TOOL), "--csv", str(csv_path), "--out", str(csv_path)],
            capture_output=True,
            text=True,
            cwd=directory,
        )
        check(
            completed.returncode != 0,
            f"out_overwrites_csv: expected nonzero exit, got {completed.returncode}",
        )
        check(
            "same file as --csv" in completed.stderr,
            f"out_overwrites_csv: unclear error: {completed.stderr!r}",
        )
        check(
            csv_path.read_bytes() == before,
            "out_overwrites_csv: --csv input was overwritten",
        )


@case
def out_must_not_overwrite_stderr():
    body = csv_text(row(1000, 500.0, events={"pixels": 10}))
    with tempfile.TemporaryDirectory(prefix="vacards-rendering-metrics-") as directory:
        csv_path = Path(directory) / "rendering.csv"
        csv_path.write_text(body, encoding="utf-8")
        stderr_path = Path(directory) / "app-stderr.log"
        stderr_path.write_text("vacards_frame_paint_us=1000\n", encoding="utf-8")
        before = stderr_path.read_bytes()
        completed = subprocess.run(
            [
                sys.executable,
                str(TOOL),
                "--csv",
                str(csv_path),
                "--stderr",
                str(stderr_path),
                "--out",
                str(stderr_path),
            ],
            capture_output=True,
            text=True,
            cwd=directory,
        )
        check(
            completed.returncode != 0,
            f"out_overwrites_stderr: expected nonzero exit, got {completed.returncode}",
        )
        check(
            "same file as --stderr" in completed.stderr,
            f"out_overwrites_stderr: unclear error: {completed.stderr!r}",
        )
        check(
            stderr_path.read_bytes() == before,
            "out_overwrites_stderr: --stderr input was overwritten",
        )


def main():
    failures = 0
    for function in CASES:
        name = function.__name__
        try:
            function()
        except Exception as error:  # noqa: BLE001 - test harness reports any failure
            failures += 1
            print(f"CASE {name} FAIL: {type(error).__name__}: {error}")
        else:
            print(f"CASE {name} PASS")
    print(f"SUMMARY {len(CASES) - failures}/{len(CASES)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
