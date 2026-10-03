#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the specific missing-export-ID failure without accepting other errors."""

import os
import subprocess
import sys


OUTPUT_FILENAME = "export-id-not-exist.png"
EXPECTED_DIAGNOSTIC = (
    b'InkFileExport::do_export_png: Object with id="not-exist" '
    b"was not found in the document. Skipping."
)


def report(status, stdout, stderr):
    print(f"Measured exit status: {status}", flush=True)
    print("stdout:\n" + stdout.decode("utf-8", errors="replace"), flush=True)
    print("stderr:\n" + stderr.decode("utf-8", errors="replace"), flush=True)


def main():
    if len(sys.argv) != 3:
        raise RuntimeError("Expected the built Inkscape executable and theta.svg paths")
    if os.path.lexists(OUTPUT_FILENAME):
        raise RuntimeError(f"Output path already exists before execution: {OUTPUT_FILENAME}")

    command = [
        sys.argv[1],
        "--export-id=not-exist",
        "--export-type=png",
        "--export-png-use-dithering=false",
        "--export-filename=" + OUTPUT_FILENAME,
        "--export-make-paths",
        sys.argv[2],
    ]
    print(f"Command: {command!r}", flush=True)
    try:
        result = subprocess.run(command, capture_output=True, timeout=60)
    except subprocess.TimeoutExpired as error:
        report("timeout; no completed exit status", error.stdout or b"", error.stderr or b"")
        print(f"Output path exists after timeout: {os.path.lexists(OUTPUT_FILENAME)}", flush=True)
        raise RuntimeError("Missing-export-ID command exceeded 60 seconds") from error

    report(result.returncode, result.stdout, result.stderr)
    output_exists = os.path.lexists(OUTPUT_FILENAME)
    print(f"Output path exists after execution: {output_exists}", flush=True)
    if result.returncode != 1:
        raise RuntimeError(f"Expected exit status 1, got {result.returncode}")
    if EXPECTED_DIAGNOSTIC not in result.stderr.splitlines():
        raise RuntimeError("Exact missing-export-ID diagnostic was not emitted")
    if output_exists:
        raise RuntimeError(f"Missing export ID produced an output path: {OUTPUT_FILENAME}")


if __name__ == "__main__":
    main()
