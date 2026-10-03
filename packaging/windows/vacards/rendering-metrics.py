#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline aggregate metrics for VACards rendering-diagnostics schema-4 CSV and
raw per-frame stderr timing lines.

Read-only with respect to its inputs; writes only ``--out``. It does not launch
the application, capture frames, measure CPU/GPU, verify a stored hash against an
expectation, or produce display-present/FPS numbers. Python standard library
only.

Contract sources
----------------
* ``src/ui/widget/canvas/rendering-stats.h`` — schema version 4 and the exact
  70-column header/field order.
* ``src/ui/widget/canvas.cpp:2250-2278`` — every CSV row already contains
  ``current - previous`` interval deltas for the event counters and for the
  timing ``_count`` / ``_total_us`` fields, plus the cumulative
  ``_lifetime_max_us``. Adjacent rows must NOT be differenced again.
* ``doc/vacards/RENDERING_DIAGNOSTICS.md`` — interval interpretation, overlapping
  stages, ``gtk_cycles = -1`` means unavailable, zero-count stages have no timing
  samples, canvas paint counts are not display FPS.

Boundary labels used here (do not relabel)
------------------------------------------
* CSV ``pixels`` is the sum of tile ``ImageSurface`` areas: repeated raster of the
  same window pixels and outline passes are counted again. Report it as
  ``rastered-tile-Mpix/s``; it is not the unique damaged-region union and not the
  pixels copied to the window.
* stderr ``vacards_frame_paint_us`` is the GTK ``before-paint -> after-paint``
  processing proxy. It is NOT GDI completion, NOT display present and NOT FPS.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import math
import os
import re
import sys
from pathlib import Path

CSV_SCHEMA_VERSION = 4
GTK_RENDERER = "GskCairoRenderer"
CANVAS_BACKEND = "Cairo"

# rendering-stats.h enum order (do not reorder).
EVENT_NAMES = (
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
)

TIMING_NAMES = (
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
)

BASE_COLUMNS = (
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
)

TIMING_SUFFIXES = ("_count", "_total_us", "_lifetime_max_us")


def _expected_header() -> list[str]:
    header = list(BASE_COLUMNS)
    header.extend(EVENT_NAMES)
    for name in TIMING_NAMES:
        for suffix in TIMING_SUFFIXES:
            header.append(name + suffix)
    return header


EXPECTED_HEADER = _expected_header()

# The application writes only plain integers/floats with stream formatting.
# Reject Python-only spellings such as ``1_0`` and non-finite tokens before the
# numeric conversion so ``nan``/``inf`` cannot pass through ``float()``.
INTEGER_RE = re.compile(r"^[+-]?[0-9]+$")
FLOAT_RE = re.compile(
    r"^[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?$"
)

FRAME_LABELS = {
    "frame_paint_us": "vacards_frame_paint_us",
    "frame_wait_us": "vacards_frame_wait_us",
    "input_latency_us": "vacards_input_latency_us",
}

FRAME_LABEL_TEXT = {
    "frame_paint_us": (
        "GTK before-paint -> after-paint processing proxy. "
        "NOT GDI completion. NOT display present. NOT FPS."
    ),
    "frame_wait_us": (
        "GTK frame-clock interval (previous after-paint -> next before-paint); "
        "refresh-throttled, not FPS."
    ),
    "input_latency_us": (
        "pointer motion -> first tile painted (first tile only); "
        "not input-to-photon latency."
    ),
}


class MetricsError(Exception):
    """A fail-closed input or usage error."""


def _parse_int(field: str, value: str, where: str, minimum: int | None = None) -> int:
    if not INTEGER_RE.match(value):
        raise MetricsError(f"{where}: {field}={value!r} is not an integer")
    number = int(value, 10)
    if minimum is not None and number < minimum:
        raise MetricsError(f"{where}: {field}={number} is below minimum {minimum}")
    return number


def _parse_float(
    field: str, value: str, where: str, minimum: float | None = None
) -> float:
    if not FLOAT_RE.match(value):
        raise MetricsError(f"{where}: {field}={value!r} is not a finite number")
    number = float(value)
    if not math.isfinite(number):
        raise MetricsError(f"{where}: {field}={value!r} is not finite")
    if minimum is not None and number < minimum:
        raise MetricsError(f"{where}: {field}={number} is below minimum {minimum}")
    return number


def _read_bytes(path: Path, role: str) -> bytes:
    try:
        return path.read_bytes()
    except OSError as error:
        raise MetricsError(f"cannot read {role} {path}: {error}") from error


def _same_file(first: Path, second: Path) -> bool:
    """True when two path spellings resolve to the same file."""
    try:
        if first.resolve() == second.resolve():
            return True
    except OSError:
        pass
    try:
        return os.path.samefile(first, second)
    except OSError:
        return False


def _reject_output_overwrites_input(
    csv_path: Path, stderr_path: Path | None, out_path: Path
) -> None:
    """Refuse an --out that is one of the inputs, before anything is written."""
    if _same_file(out_path, csv_path):
        raise MetricsError(
            f"--out {out_path} is the same file as --csv {csv_path}; "
            "refusing to overwrite an input"
        )
    if stderr_path is not None and _same_file(out_path, stderr_path):
        raise MetricsError(
            f"--out {out_path} is the same file as --stderr {stderr_path}; "
            "refusing to overwrite an input"
        )


def parse_csv(path: Path) -> dict:
    """Parse and validate one schema-4 CSV, returning typed records + metadata."""
    raw = _read_bytes(path, "CSV")
    sha256 = hashlib.sha256(raw).hexdigest()
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError as error:
        raise MetricsError(f"CSV {path} is not valid UTF-8: {error}") from error

    reader = csv.reader(io.StringIO(text, newline=""))
    raw_rows = [row for row in reader if any(cell.strip() != "" for cell in row)]
    if not raw_rows:
        raise MetricsError(f"CSV {path} has no header row")
    header = raw_rows[0]
    if header != EXPECTED_HEADER:
        mismatch = _header_mismatch(header)
        raise MetricsError(
            f"CSV {path}: unexpected header ({mismatch}); schema 4 required"
        )

    data_rows = raw_rows[1:]
    warnings: list[str] = []
    # No row is ever dropped: a wrong-width row, including an interrupted final
    # write with no trailing newline, is rejected below like any other bad row.
    if not data_rows:
        raise MetricsError(
            f"CSV {path} has a header but no complete data rows"
        )

    records = []
    for index, row in enumerate(data_rows, start=1):
        where = f"CSV {path} row {index}"
        if len(row) != len(EXPECTED_HEADER):
            raise MetricsError(
                f"{where}: expected {len(EXPECTED_HEADER)} columns, found {len(row)}"
            )
        cells = dict(zip(EXPECTED_HEADER, row))

        gtk_renderer = cells["gtk_renderer"].strip()
        if gtk_renderer != GTK_RENDERER:
            raise MetricsError(
                f"{where}: gtk_renderer={gtk_renderer!r} is not {GTK_RENDERER!r}"
            )
        canvas_backend = cells["canvas_backend"].strip()
        if canvas_backend != CANVAS_BACKEND:
            raise MetricsError(
                f"{where}: canvas_backend={canvas_backend!r} is not {CANVAS_BACKEND!r}"
            )
        schema_version = _parse_int(
            "schema_version", cells["schema_version"], where
        )
        if schema_version != CSV_SCHEMA_VERSION:
            raise MetricsError(
                f"{where}: schema_version={schema_version} is not {CSV_SCHEMA_VERSION}"
            )

        interval_ms = _parse_float("interval_ms", cells["interval_ms"], where)
        if interval_ms <= 0:
            raise MetricsError(f"{where}: interval_ms={interval_ms} must be positive")
        scale = _parse_float("scale", cells["scale"], where)
        if scale <= 0:
            raise MetricsError(f"{where}: scale={scale} must be positive")

        record = {
            "sample_us": _parse_int("sample_us", cells["sample_us"], where, minimum=0),
            "interval_ms": interval_ms,
            "canvas_id": _parse_int("canvas_id", cells["canvas_id"], where, minimum=0),
            "gtk_renderer": gtk_renderer,
            "canvas_backend": canvas_backend,
            "logical_width": _parse_int(
                "logical_width", cells["logical_width"], where, minimum=0
            ),
            "logical_height": _parse_int(
                "logical_height", cells["logical_height"], where, minimum=0
            ),
            "scale": scale,
            "threads": _parse_int("threads", cells["threads"], where, minimum=1),
            "gtk_cycles": _parse_int(
                "gtk_cycles", cells["gtk_cycles"], where, minimum=-1
            ),
            "schema_version": schema_version,
            "render_mode": _parse_int(
                "render_mode", cells["render_mode"], where, minimum=0
            ),
            "split_mode": _parse_int(
                "split_mode", cells["split_mode"], where, minimum=0
            ),
            "dragging": _parse_int("dragging", cells["dragging"], where, minimum=0),
            "events": {},
            "timings": {},
        }
        if record["dragging"] not in (0, 1):
            raise MetricsError(
                f"{where}: dragging={record['dragging']} must be 0 or 1"
            )
        for name in EVENT_NAMES:
            record["events"][name] = _parse_int(name, cells[name], where, minimum=0)
        for name in TIMING_NAMES:
            record["timings"][name] = {
                "count": _parse_int(
                    name + "_count", cells[name + "_count"], where, minimum=0
                ),
                "total_us": _parse_int(
                    name + "_total_us", cells[name + "_total_us"], where, minimum=0
                ),
                "lifetime_max_us": _parse_int(
                    name + "_lifetime_max_us",
                    cells[name + "_lifetime_max_us"],
                    where,
                    minimum=0,
                ),
            }
        records.append(record)

    if not records:
        raise MetricsError(f"CSV {path} has a header but no data rows")
    return {
        "sha256": sha256,
        "records": records,
        "warnings": warnings,
    }


def _header_mismatch(header: list[str]) -> str:
    """Return a short, content-free description of how a header is wrong."""
    if len(header) != len(EXPECTED_HEADER):
        return f"found {len(header)} columns, expected {len(EXPECTED_HEADER)}"
    for index, (found, expected) in enumerate(zip(header, EXPECTED_HEADER)):
        if found != expected:
            return (
                f"column {index + 1} is {found!r}, expected {expected!r}"
            )
    return "unknown mismatch"


def check_monotonic(records: list[dict]) -> None:
    """Require sample_us to be strictly increasing within each canvas_id."""
    groups: dict[int, list[dict]] = {}
    for record in records:
        groups.setdefault(record["canvas_id"], []).append(record)
    for canvas_id, group in groups.items():
        for previous, current in zip(group, group[1:]):
            if current["sample_us"] <= previous["sample_us"]:
                raise MetricsError(
                    f"canvas_id {canvas_id}: sample_us is not strictly increasing "
                    f"({previous['sample_us']} -> {current['sample_us']})"
                )


def select_and_trim(
    records: list[dict], canvas_id: int | None, warmup_rows: int
) -> tuple[list[dict], list[int]]:
    """Select one canvas and drop the first warmup_rows CSV rows."""
    groups: dict[int, list[dict]] = {}
    for record in records:
        groups.setdefault(record["canvas_id"], []).append(record)
    canvases = sorted(groups)

    if canvas_id is not None:
        if canvas_id not in groups:
            raise MetricsError(
                f"--canvas-id {canvas_id} is not present; file has canvases {canvases}"
            )
        selected = groups[canvas_id]
    else:
        if len(groups) > 1:
            raise MetricsError(
                "mixed canvases in one CSV "
                f"({canvases}); pass --canvas-id to select one explicitly"
            )
        selected = groups[canvases[0]]

    if warmup_rows >= len(selected):
        raise MetricsError(
            f"--warmup-rows {warmup_rows} leaves no data rows "
            f"(selected canvas has {len(selected)} rows)"
        )
    return selected[warmup_rows:], canvases


def parse_frame_metrics(path: Path | None) -> dict:
    """Parse raw per-frame stderr lines; values are microseconds."""
    values: dict[str, list[int]] = {key: [] for key in FRAME_LABELS}
    sha256 = None

    if path is not None:
        raw = _read_bytes(path, "stderr log")
        sha256 = hashlib.sha256(raw).hexdigest()
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            text = raw.decode("utf-8", errors="replace")
        prefixes = {label + "=": key for key, label in FRAME_LABELS.items()}
        for line_number, line in enumerate(text.splitlines(), start=1):
            stripped = line.strip()
            for prefix, key in prefixes.items():
                if not stripped.startswith(prefix):
                    continue
                value = stripped[len(prefix):].strip()
                where = f"stderr {path} line {line_number}"
                if not INTEGER_RE.match(value):
                    raise MetricsError(
                        f"{where}: {FRAME_LABELS[key]}={value!r} is not an integer"
                    )
                number = int(value, 10)
                if number < 0:
                    raise MetricsError(
                        f"{where}: {FRAME_LABELS[key]}={number} must not be negative"
                    )
                values[key].append(number)
                break
    return {"sha256": sha256, "values": values}


def nearest_rank(values: list[int], percentile: float) -> int:
    """Nearest-rank percentile (1-based rank ceil(p/100 * n))."""
    ordered = sorted(values)
    n = len(ordered)
    rank = math.ceil(percentile * n / 100.0)
    rank = max(1, min(n, rank))
    return ordered[rank - 1]


def frame_section(path: Path | None, parsed: dict) -> dict:
    metrics = {}
    warnings: list[str] = []
    provided = path is not None
    if not provided:
        warnings.append(
            "no --stderr provided; frame timing metrics are null/unavailable"
        )
    for key, label in FRAME_LABELS.items():
        samples = parsed["values"][key]
        if not samples:
            metrics[key] = {
                "label": FRAME_LABEL_TEXT[key],
                "raw_key": label,
                "n": 0,
                "p50_ms": None,
                "p95_ms": None,
                "min_ms": None,
                "max_ms": None,
                "status": "unavailable",
            }
            warnings.append(
                f"frame metric '{label}' absent from the provided stderr log; "
                "reported null/unavailable"
            )
            continue
        metrics[key] = {
            "label": FRAME_LABEL_TEXT[key],
            "raw_key": label,
            "n": len(samples),
            "p50_ms": nearest_rank(samples, 50.0) / 1000.0,
            "p95_ms": nearest_rank(samples, 95.0) / 1000.0,
            "min_ms": min(samples) / 1000.0,
            "max_ms": max(samples) / 1000.0,
            "status": "available",
        }
    return {
        "provided": provided,
        "scope": "whole-provided-log",
        "warmup_applied": False,
        "percentile_method": "nearest-rank",
        "note": (
            "stderr lines are not timestamped; --warmup-rows applies only to CSV "
            "rows and cannot trim these samples"
        ),
        "log_sha256": parsed["sha256"],
        "metrics": metrics,
        "warnings": warnings,
    }


def aggregate(
    csv_path: Path, stderr_path: Path | None, canvas_id: int | None, warmup_rows: int
) -> dict:
    parsed_csv = parse_csv(csv_path)
    records = parsed_csv["records"]
    check_monotonic(records)
    used, canvases = select_and_trim(records, canvas_id, warmup_rows)

    warnings: list[str] = list(parsed_csv["warnings"])
    duration_ms = sum(record["interval_ms"] for record in used)
    duration_s = duration_ms / 1000.0

    events_total = {
        name: sum(record["events"][name] for record in used) for name in EVENT_NAMES
    }
    events_section = {}
    for name in EVENT_NAMES:
        total = events_total[name]
        entry = {"total": total, "per_s": total / duration_s}
        if name == "pixels":
            entry.update(
                {
                    "rastered_tile_mpix_s": total / (duration_s * 1e6),
                    "label": "rastered-tile-Mpix/s",
                    "basis": (
                        "sum of tile ImageSurface areas; repeated raster of the same "
                        "window pixels and outline passes are counted again; not the "
                        "unique damaged-region union and not the pixels copied to the "
                        "window"
                    ),
                }
            )
        events_section[name] = entry

    stages = {}
    for name in TIMING_NAMES:
        count = sum(record["timings"][name]["count"] for record in used)
        total_us = sum(record["timings"][name]["total_us"] for record in used)
        lifetime_max_us = max(
            record["timings"][name]["lifetime_max_us"] for record in used
        )
        if count > 0:
            weighted_mean_ms = total_us / count / 1000.0
            reason = None
        else:
            weighted_mean_ms = None
            reason = (
                "no timing samples in the selected rows; mean unavailable (null), "
                "not zero"
            )
            if total_us != 0:
                reason += (
                    f"; interval total_us={total_us} with count 0 "
                    "(non-transactional concurrent snapshot)"
                )
            warnings.append(
                f"stage '{name}': zero interval events; weighted mean is null "
                "(unavailable), not 0"
            )
        stages[name] = {
            "total_count": count,
            "sum_total_us": total_us,
            "weighted_mean_ms": weighted_mean_ms,
            "weighted_mean_basis": "sum(total_us)/sum(count)/1000",
            "lifetime_max_us": lifetime_max_us,
            "lifetime_max_ms": lifetime_max_us / 1000.0,
            "lifetime_label": "lifetime",
            "mean_unavailable_reason": reason,
        }

    if all(record["gtk_cycles"] == -1 for record in used):
        warnings.append("gtk_cycles unavailable (-1) in every selected row")

    parsed_stderr = parse_frame_metrics(stderr_path)
    frame = frame_section(stderr_path, parsed_stderr)
    warnings.extend(frame["warnings"])

    inputs = [
        {
            "role": "csv",
            "path": str(csv_path),
            "sha256": parsed_csv["sha256"],
        }
    ]
    if stderr_path is not None:
        inputs.append(
            {
                "role": "stderr",
                "path": str(stderr_path),
                "sha256": parsed_stderr["sha256"],
            }
        )

    return {
        "tool": "rendering-metrics",
        "tool_version": 1,
        "contract": {
            "csv_schema_version": CSV_SCHEMA_VERSION,
            "gtk_renderer": GTK_RENDERER,
            "canvas_backend": CANVAS_BACKEND,
            "csv_source": "src/ui/widget/canvas.cpp:2250-2278",
            "header_source": "src/ui/widget/canvas/rendering-stats.h",
        },
        "inputs": inputs,
        "scope": {
            "canvas_id": used[0]["canvas_id"],
            "gtk_renderer": used[0]["gtk_renderer"],
            "canvas_backend": used[0]["canvas_backend"],
            "canvases_in_file": canvases,
            "rows_in_file": len(records),
            "rows_selected": len(used) + warmup_rows,
            "warmup_rows": warmup_rows,
            "rows_used": len(used),
            "duration_s": duration_s,
            "interval_ms_sum": duration_ms,
            "first_sample_us": used[0]["sample_us"],
            "last_sample_us": used[-1]["sample_us"],
            "csv_deltas_already_interval": True,
            "rows_differenced_again": False,
        },
        "events": events_section,
        "stages": stages,
        "frame_timing": frame,
        "labels": {
            "is_display_fps": False,
            "is_display_present": False,
            "is_gdi_completion": False,
            "process_cpu_claims_used": False,
            "overlapping_stages_summed": False,
            "note": (
                "Canvas paint/tile work is not display FPS; stages overlap and must "
                "not be summed; frame paint is a GTK processing proxy only."
            ),
        },
        "warnings": warnings,
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="rendering-metrics.py",
        description=(
            "Offline aggregate of VACards rendering schema-4 CSV interval deltas "
            "and raw per-frame stderr timings. Read-only; writes only --out."
        ),
    )
    parser.add_argument("--csv", required=True, help="schema-4 rendering CSV path")
    parser.add_argument(
        "--stderr",
        default=None,
        help="raw application stderr log with vacards_frame_* lines (optional)",
    )
    parser.add_argument("--out", required=True, help="JSON output path")
    parser.add_argument(
        "--canvas-id",
        type=int,
        default=None,
        help="select one canvas_id from a mixed-canvas CSV",
    )
    parser.add_argument(
        "--warmup-rows",
        type=int,
        default=0,
        help="discard this many leading CSV rows before aggregating (default 0)",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.warmup_rows < 0:
            raise MetricsError("--warmup-rows must not be negative")
        csv_path = Path(args.csv)
        stderr_path = Path(args.stderr) if args.stderr is not None else None
        out_path = Path(args.out)
        _reject_output_overwrites_input(csv_path, stderr_path, out_path)
        result = aggregate(csv_path, stderr_path, args.canvas_id, args.warmup_rows)
        payload = json.dumps(result, indent=2, sort_keys=True, allow_nan=False) + "\n"
        try:
            out_path.write_text(payload, encoding="utf-8")
        except OSError as error:
            raise MetricsError(f"cannot write --out {args.out}: {error}") from error
    except MetricsError as error:
        print(f"rendering-metrics: error: {error}", file=sys.stderr)
        return 2
    print(
        f"rendering-metrics: canvas {result['scope']['canvas_id']}, "
        f"{result['scope']['rows_used']} rows, "
        f"{result['scope']['duration_s']:.3f} s -> {args.out}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
