#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the existing host suite, then reload its session in a fresh process."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


CASE = "SavedSessionSurvivesProcessRestart"


def main():
    binary = Path(sys.argv[1]).resolve(strict=True)
    evidence = Path(tempfile.mkdtemp(prefix="vacards-library-session-")).resolve()
    profile = evidence / "profile"
    print(f"Library session evidence (retained): {evidence}", flush=True)
    # Never reset an existing profile or invoke an installed application. The
    # first process must create preferences.xml; the second must reuse it.
    env = dict(os.environ, INKSCAPE_PROFILE_DIR=str(profile))
    for name in ("VACARDS_LIBRARY_SESSION_VERIFY", "GTEST_FILTER",
                 "GTEST_TOTAL_SHARDS", "GTEST_SHARD_INDEX"):
        env.pop(name, None)
    for phase, selection, timeout in (("write", "*", 120),
                                      ("reopen", f"LibraryHostTest.{CASE}", 30)):
        if phase == "reopen":
            if not (profile / "preferences.xml").is_file():
                raise RuntimeError("First process did not persist preferences")
            env["VACARDS_LIBRARY_SESSION_VERIFY"] = "1"
        report = evidence / f"{phase}.xml"
        result = subprocess.run(
            [str(binary), f"--gtest_filter={selection}",
             f"--gtest_output=xml:{report}"], env=env, timeout=timeout)
        if result.returncode:
            return result.returncode
        tree = ET.parse(report)
        cases = [case for case in tree.iter("testcase")
                 if case.get("classname") == "LibraryHostTest" and case.get("name") == CASE]
        if (len(cases) != 1 or cases[0].get("status") != "run"
                or cases[0].get("result") != "completed"
                or cases[0].find("failure") is not None or cases[0].find("skipped") is not None):
            raise RuntimeError(f"{phase}: required persistence case did not run and pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
