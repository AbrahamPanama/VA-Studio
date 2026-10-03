#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the real release-gate shell and reservation helper on tiny CMake trees.

Real: Git worktrees, LANGUAGES NONE CMake, Ninja stamps, CTest, exclusive
reservation and drift verification. Fake: application, compiler identity,
Mach-O/dependency/packaging/attestation checks. No application is built or run;
fixture attestations are NOT release evidence. Only disposable gate copies
replace the two unconditional production guards with synthetic assumptions.
The actual CLI has no authentication bypass. Exact suite/result checks use the
real helper and synthetic baseline; external signature/acceptance is not proven.

Default helper: packaging/vacards/gate-run.py. Before its separate integration,
pass --gate-run-helper /absolute/path/to/Volta/packaging/vacards/gate-run.py.
That override affects only test fixture copies, never the production gate.
"""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "packaging/vacards/gate-run.py"
RESULT_SPEC = importlib.util.spec_from_file_location("wiring_results", ROOT / "packaging/vacards/test-results.py")
RESULTS = importlib.util.module_from_spec(RESULT_SPEC)
with mock.patch.object(sys, "dont_write_bytecode", True):
    RESULT_SPEC.loader.exec_module(RESULTS)

AUTH_FIXTURE = 'authenticated_identity_file="$source_root/synthetic-identity.json" # FIXTURE ONLY'
ACCEPTANCE_FIXTURE = ': # FIXTURE ONLY: synthetic acceptance, no release proof'

# The fake probe only exercises control flow around the real reservation.
PROBE = r'''#!/usr/bin/env python3
import json, os, pathlib, subprocess, sys
root = pathlib.Path(os.environ["WIRING_ROOT"])
source = pathlib.Path(os.environ["WIRING_SOURCE"])
build = pathlib.Path(os.environ["WIRING_BUILD"])
phase, *args = sys.argv[1:]
def event(name):
    with (root / "events.log").open("a") as stream: stream.write(name + "\n")
def drift(name):
    if os.environ.get("WIRING_DRIFT") != name: return
    target = os.environ.get("WIRING_DRIFT_TARGET", "source")
    path = {"source": source / "payload.txt", "cache": build / "CMakeCache.txt",
            "ninja": build / "build.ninja", "tool": root / "tools/compiler"}[target]
    with path.open("a") as stream: stream.write("\n# injected identity drift\n")
def reserved():
    record = json.loads((build / "VACARDS-GATE-RUN.json").read_text())
    assert (build / "vacards-release-evidence").is_dir()
    if record["inputs"]["mode"] == "full":
        assert pathlib.Path(record["inputs"]["install"]).is_dir()
    return record
if phase in ("enumeration", "critical-build", "full-test-build", "full-build", "install"):
    record = reserved(); event(phase); drift(phase)
    if phase == "install" and os.environ.get("WIRING_PRIOR_FAILURE"):
        for name in ("VACARDS-RELEASE-GATE.failed.env", "VACARDS-RELEASE-GATE.failed.env.prior"):
            (pathlib.Path(record["inputs"]["install"]) / name).write_bytes(b"prior failed record\x00")
    if phase == "install" and os.environ.get("WIRING_EXISTING_PENDING"):
        pending = pathlib.Path(record["inputs"]["install"]) / "VACARDS-RELEASE-GATE.pending.env"
        if os.environ["WIRING_EXISTING_PENDING"] == "directory": pending.mkdir()
        else: pending.write_bytes(b"prior pending record\x00")
elif phase in ("critical-tests", "native-tests", "actionable-tests", "portfolio-short", "portfolio-60s"):
    reserved()
    if phase.startswith("portfolio-"):
        required = "VACARDS_NESTING_RUN_PORTFOLIO_DIFFERENTIAL" + ("_60S" if phase == "portfolio-60s" else "")
        assert os.environ.get(required) == "1"
        other = "VACARDS_NESTING_RUN_PORTFOLIO_DIFFERENTIAL" + ("" if phase == "portfolio-60s" else "_60S")
        assert other not in os.environ
        event(phase)
        if phase == "portfolio-60s" and os.environ.get("WIRING_CLOSURE_TAMPER"):
            target = build / "vacards-release-evidence/critical-tests.result.json"
            data = json.loads(target.read_text()); data["outcome"] = "rejected"
            target.write_text(json.dumps(data))
    if not args or args[0] == "1":
        event(phase); drift(phase)
        if os.environ.get("WIRING_FAIL") == phase: sys.exit(9)
elif phase == "app":
    event("fake-app")
    head = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    print("Fake Inkscape --version; no native app: " + head[:10])
    # Always supply fake loader output: macOS may strip DYLD_* at the shell
    # interpreter boundary. This is explicitly NOT a runtime-linking test.
    print(str(root / "libcdr/lib/libcdr.0.dylib"))
    print(str(root / "cairo/lib/libcairo.2.dylib"))
elif phase == "verify":
    name = args[0]; event(name)
    if name == "verify-release-attestation.sh":
        record = reserved()
        prefix = pathlib.Path(record["inputs"]["install"])
        assert args[1:] == ["--candidate", str(prefix)]
        pending = prefix / "VACARDS-RELEASE-GATE.pending.env"
        final = prefix / "VACARDS-RELEASE-GATE.env"
        assert pending.is_file() and not os.path.lexists(final)
        assert not (build / "vacards-release-evidence/VACARDS-RELEASE-GATE.env").exists()
        (root / "validated-candidate.bytes").write_bytes(pending.read_bytes())
        drift("final-validator")
        late_tool = os.environ.get("WIRING_FAIL_LATE_TOOL")
        if late_tool:
            (root / "late-tools-armed").touch()
            command = ["mktemp", str(root / "failure.XXXXXX")] if late_tool == "mktemp" else ["mv", str(pending), str(root / "moved")]
            subprocess.run(command, check=True)
        target = os.environ.get("WIRING_PUBLICATION_TARGET")
        if os.environ.get("WIRING_PUBLICATION_EVIDENCE"):
            final = build / "vacards-release-evidence/VACARDS-RELEASE-GATE.env"
        if target == "file": final.write_bytes(b"prior non-authorizing destination\x00")
        elif target == "directory": final.mkdir()
        elif target == "symlink":
            sentinel = root / "symlink-destination"; sentinel.write_bytes(b"prior non-authorizing destination\x00")
            final.symlink_to(sentinel)
        if os.environ.get("WIRING_PUBLICATION_IO"):
            destination = build / "vacards-release-evidence" if os.environ["WIRING_PUBLICATION_IO"] == "evidence" else prefix
            destination.chmod(0o500)
    if os.environ.get("WIRING_FAIL") == name: sys.exit(8)
elif phase == "packaging":
    event("fake-packaging-" + args[0])
    if args[0] == "verify-attestation":
        assert args[1] == "--candidate"
        prefix = pathlib.Path(args[2])
        assert (prefix / "VACARDS-RELEASE-GATE.pending.env").is_file()
        assert not (prefix / "VACARDS-RELEASE-GATE.env").exists()
    if args[0] == "capture":
        reserved()
        output = pathlib.Path(args[args.index("--output") + 1])
        schemas = pathlib.Path(args[args.index("--schema-dir") + 1]); schemas.mkdir()
        (schemas / "gschemas.compiled").write_text("fake schema bytes")
        output.write_text(json.dumps({"fixture_only": True}))
else: raise AssertionError(phase)
'''

TOOLS = r'''#!/usr/bin/env python3
import os, pathlib, subprocess, sys
name = pathlib.Path(sys.argv[0]).name
root = pathlib.Path(os.environ["WIRING_ROOT"])
if name == "uname":
    print("Darwin" if sys.argv[1:] == ["-s"] else "arm64")
elif name == "lipo": print("arm64")
elif name == "vtool": print("    minos 26.0")
elif name == "pkg-config": print("1.0.0")
elif name in ("mktemp", "mv"):
    if (root / "late-tools-armed").exists() and os.environ.get("WIRING_FAIL_LATE_TOOL") == name:
        with (root / "events.log").open("a") as stream: stream.write("injected-" + name + "-failure\n")
        sys.exit(97)
    os.execv({"mktemp": "@REAL_MKTEMP@", "mv": "@REAL_MV@"}[name], [name, *sys.argv[1:]])
elif name == "compiler":
    assert sys.argv[1:] == ["--version"], "a compiler was unexpectedly invoked"
    if os.environ.get("WIRING_DRIFT") == "final-provenance":
        with (pathlib.Path(os.environ["WIRING_BUILD"]) / "CMakeCache.txt").open("a") as stream:
            stream.write("\n# late provenance drift\n")
    print("Fake compiler identity; never compiles")
else: raise AssertionError(name)
'''

CMAKE = r'''cmake_minimum_required(VERSION 3.20)
project(GateWiring LANGUAGES NONE)
enable_testing()
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${CMAKE_SOURCE_DIR}/enumerate.cmake")
set(WIRING_CRITICAL_COUNT 48 CACHE STRING "Fixture critical test count")
set(BUILD_TESTING ON CACHE BOOL "")
set(VACARDS_NESTING_BUILD_PHASE0_TESTS ON CACHE BOOL "")
set(VACARDS_NESTING_BUILD_PHASE2_TESTS ON CACHE BOOL "")
set(TESTS_WITH_ASAN OFF CACHE BOOL "")
set(WITH_LIBCDR ON CACHE BOOL "")
set(VACARDS_REQUIRE_MODERN_CDR ON CACHE BOOL "")
set(VACARDS_REQUIRE_PATCHED_CAIRO ON CACHE BOOL "")
set(CMAKE_OSX_DEPLOYMENT_TARGET 26.0 CACHE STRING "" FORCE)
set(VACARDS_NESTING_RUST_TARGET aarch64-apple-darwin CACHE STRING "")
file(WRITE "${CMAKE_BINARY_DIR}/VACARDS-CMAKE-FEATURES.env" "@FEATURES@")
configure_file("${CMAKE_SOURCE_DIR}/app.in" "${CMAKE_BINARY_DIR}/bin/inkscape" @ONLY)
file(CHMOD "${CMAKE_BINARY_DIR}/bin/inkscape" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
add_custom_command(OUTPUT critical.stamp
  COMMAND "@PYTHON@" "${CMAKE_SOURCE_DIR}/probe.py" critical-build
  COMMAND "${CMAKE_COMMAND}" -E touch critical.stamp VERBATIM)
add_custom_target(build-vacards-critical DEPENDS critical.stamp)
foreach(target vacards_nesting_abi_smoke_c vacards_nesting_abi_smoke_cpp vacards_nesting_job_contract_c vacards_nesting_cpp_contract_test)
  add_custom_target(${target})
endforeach()
add_custom_target(tests COMMAND "@PYTHON@" "${CMAKE_SOURCE_DIR}/probe.py" full-test-build VERBATIM)
add_custom_target(unit_tests COMMAND "${CMAKE_COMMAND}" -E true)
add_custom_target(installable ALL COMMAND "@PYTHON@" "${CMAKE_SOURCE_DIR}/probe.py" full-build VERBATIM)
foreach(number RANGE 1 ${WIRING_CRITICAL_COUNT})
  add_test(NAME fixture-critical-${number} COMMAND "@PYTHON@" "${CMAKE_SOURCE_DIR}/probe.py" critical-tests ${number})
  set_tests_properties(fixture-critical-${number} PROPERTIES LABELS vacards-critical)
endforeach()
foreach(name @NATIVE@)
  add_test(NAME ${name} COMMAND "@PYTHON@" "${CMAKE_SOURCE_DIR}/probe.py" native-tests)
endforeach()
foreach(budget 100 1000 5000 60000)
  set(phase portfolio-short)
  if(budget EQUAL 60000)
    set(phase portfolio-60s)
  endif()
  add_test(NAME vacards-nesting-portfolio-differential-${budget}ms COMMAND "@PYTHON@" "${CMAKE_SOURCE_DIR}/probe.py" ${phase} ${budget})
  set_tests_properties(vacards-nesting-portfolio-differential-${budget}ms PROPERTIES LABELS vacards-nesting-opt-in)
endforeach()
add_test(NAME fixture-actionable COMMAND "@PYTHON@" "${CMAKE_SOURCE_DIR}/probe.py" actionable-tests)
install(PROGRAMS "${CMAKE_BINARY_DIR}/bin/inkscape" DESTINATION bin)
install(CODE "execute_process(COMMAND \"@PYTHON@\" \"${CMAKE_SOURCE_DIR}/probe.py\" install COMMAND_ERROR_IS_FATAL ANY)")
'''


class GateWiring(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for tool in ("git", "cmake", "ninja", "ctest", "python3", "shasum"):
            if not shutil.which(tool): raise RuntimeError("missing fixture tool: " + tool)
        cls.helper_bytes = HELPER.read_bytes()
        print("REAL gate-run helper SHA256: " + hashlib.sha256(cls.helper_bytes).hexdigest(), flush=True)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gate-wiring-á-", dir=os.environ.get("TMPDIR"))
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / "source space"
        self.source.mkdir()
        self.build = self.root / "fresh build"
        self.install = self.root / "installs/new prefix"
        self.install.parent.mkdir()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith(("GIT_", "DYLD_", "WIRING_"))}
        self.env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull, GIT_TERMINAL_PROMPT="0",
                        WIRING_ROOT=str(self.root), WIRING_SOURCE=str(self.source), WIRING_BUILD=str(self.build),
                        PYTHONDONTWRITEBYTECODE="1", VACARDS_GATE_JOBS="2")
        self.env.pop("VACARDS_GATE_ALLOW_DIRTY", None)
        for relative in ("packaging/macos/vacards/run-release-gate.sh", "packaging/macos/vacards/vacards-version.sh",
                         "packaging/macos/vacards/vacards-dependencies.sh", "packaging/vacards/vacards-version.sh",
                         "packaging/vacards/vacards-dependencies.sh", "packaging/vacards/VERSION.env",
                         "VACARDS-DEPENDENCIES.env"):
            dest = self.source / relative; dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, dest)
            if dest.suffix == ".sh": dest.chmod(0o755)
        (self.source / "packaging/vacards/gate-run.py").write_bytes(self.helper_bytes)
        shutil.copyfile(ROOT / "packaging/vacards/test-results.py", self.source / "packaging/vacards/test-results.py")
        (self.source / "payload.txt").write_text("frozen fixture source\n")
        (self.source / "probe.py").write_text(PROBE)
        (self.source / "enumerate.cmake").write_text(
            f'execute_process(COMMAND "{sys.executable}" "{self.source}/probe.py" enumeration COMMAND_ERROR_IS_FATAL ANY)\n')
        self.gate = self.source / "packaging/macos/vacards/run-release-gate.sh"
        # Only this disposable, committed source copy replaces the blockers.
        # No production runtime argument, environment flag or file selects trust.
        gate_text = self.gate.read_text()
        self.assertEqual(gate_text.count("\nrequire_authenticated_baseline\n"), 1)
        self.assertEqual(gate_text.count("\nrequire_external_acceptance\n"), 1)
        self.gate.write_text(gate_text.replace("\nrequire_authenticated_baseline\n", "\n" + AUTH_FIXTURE + "\n")
                             .replace("\nrequire_external_acceptance\n", "\n" + ACCEPTANCE_FIXTURE + "\n"))
        self.synthetic_baseline()
        for name in ("verify-vacards-libcdr.sh", "verify-vacards-cairo-prefix.sh", "verify-release-attestation.sh"):
            stub = self.gate.parent / name
            stub.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{self.source}/probe.py" verify {name} "$@"\n')
            stub.chmod(0o755)
        stub = self.source / "packaging/vacards/packaging-provenance.py"
        stub.write_text(f'import os, sys\nos.execv({sys.executable!r}, [{sys.executable!r}, {str(self.source / "probe.py")!r}, "packaging", *sys.argv[1:]])\n')
        (self.source / "app.in").write_text(f'#!/bin/sh\nexec "{sys.executable}" "{self.source}/probe.py" app "$@"\n')
        tools = self.root / "tools"; tools.mkdir()
        tool_source = TOOLS.replace("#!/usr/bin/env python3", "#!" + sys.executable)
        # Keep real utility paths available behind narrowly armed failure probes.
        for name in ("mktemp", "mv"):
            tool_source = tool_source.replace("@REAL_" + name.upper() + "@", shutil.which(name))
        for name in ("uname", "lipo", "vtool", "pkg-config", "compiler", "mktemp", "mv"):
            tool = tools / name; tool.write_text(tool_source); tool.chmod(0o755)
        self.env["PATH"] = str(tools) + os.pathsep + self.env["PATH"]
        manifest = self.source / "VACARDS-DEPENDENCIES.env"
        self.pins = dict(line.split("=", 1) for line in manifest.read_text().splitlines() if "=" in line)
        features = "format=1\\n" + "".join(f"{key}=ON\\n" for key in self.pins["required_cmake_features"].split(","))
        features += "".join(f"{key}=OFF\\n" for key in self.pins["disabled_cmake_features"].split(","))
        (self.source / "CMakeLists.txt").write_text(CMAKE.replace("@PYTHON@", sys.executable).replace("@FEATURES@", features)
                                                   .replace("@NATIVE@", " ".join(RESULTS.NATIVE)))
        for dependency in ("cairo", "libcdr"):
            (self.root / dependency / "lib").mkdir(parents=True)
        (self.root / "libcdr/VACARDS-LIBCDR.env").write_text(
            "source_commit=" + self.pins["libcdr_commit"] + "\nlibrary_sha256=" + "0" * 64 + "\n"
            + "pkgconfig_version=" + self.pins["libcdr_pkgconfig_version"] + "\n")
        (self.root / "cairo/VACARDS-CAIRO.txt").write_text("synthetic dependency, not a real runtime\n")
        self.git("init", "-q")
        self.git("config", "user.name", "Gate wiring fixture")
        self.git("config", "user.email", "gate-fixture@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        self.commit()
        baseline = self.git("rev-parse", "HEAD").stdout.strip()
        manifest.write_text("\n".join("source_baseline_commit=" + baseline if line.startswith("source_baseline_commit=")
                                     else line for line in manifest.read_text().splitlines()) + "\n")
        self.commit()
        args = ["cmake", "-S", self.source, "-B", self.build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                "-DCMAKE_INSTALL_PREFIX=" + str(self.root / "unused cache prefix"),
                "-DVACARDS_RESOLVED_LIBCDR_PREFIX=" + str(self.root / "libcdr"),
                "-DVACARDS_RESOLVED_LIBCDR_VERSION=" + self.pins["libcdr_pkgconfig_version"],
                "-DVACARDS_RESOLVED_CAIRO_PREFIX=" + str(self.root / "cairo"), "-DVACARDS_RESOLVED_CAIRO_VERSION=1.18.4"]
        for key in ("CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "VACARDS_NESTING_RUSTC_EXECUTABLE"):
            args.append(f"-D{key}:FILEPATH={tools / 'compiler'}")
        self.command(args, good=True)
        self.ledger = self.build / "VACARDS-GATE-RUN.json"
        self.evidence = self.build / "vacards-release-evidence"

    def command(self, args, *, good=None, env=None, cwd=None):
        result = subprocess.run([str(a) for a in args], env=env or self.env, cwd=cwd or self.root,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        if good is True: self.assertEqual(result.returncode, 0, result.stdout)
        if good is False: self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def synthetic_baseline(self):
        critical = sorted(f"fixture-critical-{n}" for n in range(1, 49))
        native = RESULTS.NATIVE
        combined = sorted(critical + native)
        baseline = {"schema_version": 1, "observed_source_commit": "a" * 40}
        for prefix, names in (("critical", critical), ("native_nesting", native),
                              ("required_automated", combined),
                              ("preport_actionable", sorted(combined + ["fixture-actionable"]))):
            baseline.update({prefix + "_test_names": names, prefix + "_test_count": len(names),
                             prefix + "_test_name_set_sha256": RESULTS.name_hash(names)})
        ids = [f"VA-A{n:04}" for n in range(1, len(combined) + 1)]
        mappings = [{"acceptance_id": identifier, "feature_id": "synthetic", "test_name": name,
                     "registration_file": "CMakeLists.txt", "source_file": "probe.py",
                     "predicate": "synthetic only", "status": "active"} for identifier, name in zip(ids, combined)]
        baseline.update(windows_actionable_exclusions=[], windows_actionable_exclusion_count=0,
                        windows_actionable_exclusion_set_sha256=RESULTS.digest(b"[]"),
                        required_acceptance_ids=ids, required_acceptance_id_set_sha256=RESULTS.name_hash(ids),
                        focused_case_mappings=mappings, focused_case_mappings_sha256=RESULTS.digest(RESULTS.canonical(mappings)))
        docs = self.source / "doc/vacards"; docs.mkdir(parents=True)
        raw = RESULTS.canonical(baseline)
        (docs / "WINDOWS_PORT_TEST_BASELINE.json").write_bytes(raw)
        signature = b"SYNTHETIC not a verified signature"
        (docs / "WINDOWS_PORT_TEST_BASELINE.json.p7s").write_bytes(signature)
        (self.source / "synthetic-identity.json").write_bytes(RESULTS.canonical({"schema_version": 1,
            "baseline_sha256": RESULTS.digest(raw), "signature_sha256": RESULTS.digest(signature),
            "verifier_identity": "SYNTHETIC fixture assumption", "verifier_sha256": "b" * 64}))

    def git(self, *args):
        return self.command(["git", "-C", self.source, *args], good=True)

    def commit(self):
        self.git("add", ".")
        self.git("commit", "-q", "-m", "Temporary compiler-free fixture only")

    def run_gate(self, mode="quick", *, install=None, good=True, changes=None, gate=None):
        args = ["/bin/sh", gate or self.gate, "--" + mode, self.build]
        if mode == "full": args.append(self.install if install is None else install)
        return self.command(args, good=good, env=dict(self.env, **(changes or {})))

    def assert_unclaimed(self):
        self.assertFalse(self.ledger.exists())
        self.assertFalse((self.build / ".ninja_log").exists())

    def assert_no_authorization(self):
        self.assertFalse((self.evidence / "VACARDS-RELEASE-GATE.env").exists())
        self.assertFalse((self.install / "VACARDS-RELEASE-GATE.env").exists())

    def assert_candidate_intact(self):
        self.assertEqual((self.install / "VACARDS-RELEASE-GATE.pending.env").read_bytes(),
                         (self.root / "validated-candidate.bytes").read_bytes())

    def test_quick_real_reservation_and_ctest(self):
        result = self.run_gate()
        record = json.loads(self.ledger.read_text())
        self.assertEqual(record["inputs"]["mode"], "quick")
        self.assertIsNone(record["inputs"]["install"])
        self.assertEqual(record["root_ninja_log_at_claim"], "absent")
        self.assertTrue((self.build / ".ninja_log").is_file())
        self.assertIn("100% tests passed", (self.evidence / "critical-tests.log").read_text())
        self.assertGreaterEqual(result.stdout.count("verify: "), 10)
        events = (self.root / "events.log").read_text().splitlines()
        self.assertLess(events.index("critical-build"), events.index("enumeration"))
        self.assertIn("native-tests", events)
        self.assertNotIn("portfolio-short", events)
        self.assertNotIn("portfolio-60s", events)
        self.assertNotIn("install", events)
        closure = json.loads((self.evidence / "test-suite-closure.json").read_text())
        self.assertEqual([row["suite_id"] for row in closure["suites"]], ["critical", "native_nesting"])
        self.assert_no_authorization()

    def test_full_reserves_before_build_and_installs_explicit_override(self):
        self.run_gate("full")
        record = json.loads(self.ledger.read_text())
        self.assertEqual(record["inputs"]["install"], str(self.install))
        self.assertTrue((self.install / "bin/inkscape").is_file())
        self.assertTrue((self.install / "VACARDS-RELEASE-GATE.env").is_file())
        self.assert_candidate_intact()
        self.assertTrue(os.path.samefile(self.install / "VACARDS-RELEASE-GATE.pending.env",
                                        self.install / "VACARDS-RELEASE-GATE.env"))
        self.assertTrue(os.path.samefile(self.evidence / "VACARDS-RELEASE-GATE.pending.env",
                                        self.evidence / "VACARDS-RELEASE-GATE.env"))
        self.assertEqual((self.evidence / "VACARDS-RELEASE-GATE.env").read_bytes(),
                         (self.install / "VACARDS-RELEASE-GATE.env").read_bytes())
        self.assertFalse((self.root / "unused cache prefix").exists())
        self.assertIn("actionable-tests", (self.root / "events.log").read_text())
        events = (self.root / "events.log").read_text().splitlines()
        self.assertEqual(events.count("portfolio-short"), 3)
        self.assertEqual(events.count("portfolio-60s"), 1)
        closure = json.loads((self.evidence / "test-suite-closure.json").read_text())
        self.assertEqual([row["suite_id"] for row in closure["suites"]], list(RESULTS.SUITES))
        for copy in (self.install / "VACARDS-GATE-RUN.json", self.evidence / "VACARDS-GATE-RUN.json"):
            self.assertEqual(copy.read_bytes(), self.ledger.read_bytes())
        self.assertIn("gate_run_sha256=" + hashlib.sha256(self.ledger.read_bytes()).hexdigest(),
                      (self.install / "VACARDS-BUILD-PROVENANCE.env").read_text())
        provenance_sha256 = hashlib.sha256((self.install / "VACARDS-BUILD-PROVENANCE.env").read_bytes()).hexdigest()
        self.assertIn("build_provenance_sha256=" + provenance_sha256,
                      (self.install / "VACARDS-RELEASE-GATE.env").read_text())

    def test_missing_critical_name_consumes_claim_without_execution(self):
        self.command(["cmake", "-S", self.source, "-B", self.build, "-DWIRING_CRITICAL_COUNT=47"], good=True)
        result = self.run_gate(good=False)
        self.assertIn("missing critical tests", result.stdout)
        self.assertTrue(self.ledger.exists())
        self.assertTrue(self.evidence.is_dir())
        events = (self.root / "events.log").read_text().splitlines()
        self.assertIn("enumeration", events)
        self.assertIn("critical-build", events)
        self.assertNotIn("critical-tests", events)
        self.assertTrue((self.build / ".ninja_log").exists())
        ledger = self.ledger.read_bytes()
        self.run_gate(good=False)
        self.assertEqual(self.ledger.read_bytes(), ledger)
        self.assertEqual((self.root / "events.log").read_text().splitlines().count("enumeration"), events.count("enumeration"))
        self.assert_no_authorization()

    def test_real_prior_ninja_build_rejected_without_evidence(self):
        self.command(["cmake", "--build", self.build, "--parallel", "2", "--target", "unit_tests"], good=True)
        self.assertTrue((self.build / ".ninja_log").exists())
        before = (self.build / ".ninja_log").read_bytes()
        result = self.run_gate(good=False)
        self.assertIn("existing .ninja_log", result.stdout)
        self.assertFalse(self.evidence.exists())
        self.assertFalse(self.ledger.exists())
        self.assertEqual((self.build / ".ninja_log").read_bytes(), before)

    def test_existing_evidence_even_empty_rejected(self):
        self.evidence.mkdir()
        result = self.run_gate(good=False)
        self.assertIn("existing vacards-release-evidence", result.stdout)
        self.assert_unclaimed()
        self.assertEqual(list(self.evidence.iterdir()), [])

    def test_existing_evidence_never_erased(self):
        self.evidence.mkdir()
        sentinel = self.evidence / "VACARDS-RELEASE-GATE.env"; sentinel.write_bytes(b"prior raw evidence\x00")
        self.run_gate(good=False)
        self.assertEqual(sentinel.read_bytes(), b"prior raw evidence\x00")
        self.assert_unclaimed()

    def test_existing_install_even_empty_rejected_before_evidence(self):
        self.install.mkdir()
        self.run_gate("full", good=False)
        self.assert_unclaimed()
        self.assertFalse(self.evidence.exists())
        self.assertEqual(list(self.install.iterdir()), [])

    def test_relative_install_rejected_before_evidence(self):
        result = self.run_gate("full", install="relative-prefix", good=False)
        self.assertIn("must be absolute", result.stdout)
        self.assert_unclaimed()
        self.assertFalse(self.evidence.exists())

    def test_missing_install_parent_not_created(self):
        result = self.run_gate("full", install=self.root / "missing/child", good=False)
        self.assertIn("parent must already exist", result.stdout)
        self.assertFalse((self.root / "missing").exists())
        self.assert_unclaimed()

    def test_install_inside_build_rejected(self):
        self.run_gate("full", install=self.build / "install", good=False)
        self.assert_unclaimed()
        self.assertFalse(self.evidence.exists())

    def test_same_sha_other_worktree_rejected(self):
        other = self.root / "other worktree"
        self.git("worktree", "add", "--detach", other, "HEAD")
        result = self.run_gate(gate=other / "packaging/macos/vacards/run-release-gate.sh", good=False)
        self.assertIn("different source tree", result.stdout)
        self.assert_unclaimed()
        self.assertFalse(self.evidence.exists())

    def test_preflight_failure_does_not_reserve(self):
        self.run_gate(changes={"WIRING_FAIL": "verify-vacards-cairo-prefix.sh"}, good=False)
        self.assert_unclaimed()
        self.assertFalse(self.evidence.exists())

    def test_missing_helper_fails_closed(self):
        (self.source / "packaging/vacards/gate-run.py").unlink()
        result = self.run_gate(good=False)
        self.assertIn("missing qualification reservation helper", result.stdout)
        self.assert_unclaimed()

    def test_failed_critical_tests_preserved_and_retry_rejected(self):
        self.run_gate(good=False, changes={"WIRING_FAIL": "critical-tests"})
        before = {p.name: p.read_bytes() for p in self.evidence.iterdir() if p.is_file()}
        ledger = self.ledger.read_bytes()
        self.assertIn(b"Failed", before["critical-tests.log"])
        rejected = json.loads(before["critical-tests.result.json"])
        self.assertEqual(rejected["outcome"], "rejected")
        self.assertEqual(rejected["ctest_exit_code"], 8)
        self.run_gate(good=False)
        self.assertEqual(ledger, self.ledger.read_bytes())
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.evidence.iterdir() if p.is_file()})
        self.assert_no_authorization()

    def reconfigure(self):
        self.commit()
        self.command(["cmake", "-S", self.source, "-B", self.build], good=True)

    def test_production_cli_rejects_synthetic_receipts_and_ambient_flags_before_claim(self):
        self.gate.write_bytes((ROOT / "packaging/macos/vacards/run-release-gate.sh").read_bytes())
        self.commit()
        for mode in ("quick", "full"):
            result = self.run_gate(mode, good=False, changes={
                "VACARDS_GATE_ALLOW_DIRTY": "1", "VACARDS_BASELINE_IDENTITY": str(self.source / "synthetic-identity.json"),
                "VACARDS_GATE_AUTHENTICATED": "1", "VACARDS_GATE_SKIP_AUTH": "1"})
            self.assertIn("production baseline authentication is not provisioned", result.stdout)
            self.assert_unclaimed()
            self.assertFalse(self.evidence.exists())
            self.assertFalse(self.install.exists())
            self.assert_no_authorization()

    def test_production_cli_missing_signature_stops_before_claim(self):
        self.gate.write_bytes((ROOT / "packaging/macos/vacards/run-release-gate.sh").read_bytes())
        (self.source / "doc/vacards/WINDOWS_PORT_TEST_BASELINE.json.p7s").unlink()
        self.commit()
        result = self.run_gate(good=False)
        self.assertIn("approved tracked WP-00.6 baseline/signature", result.stdout)
        self.assert_unclaimed()
        self.assert_no_authorization()

    def test_full_structural_pass_cannot_replace_external_acceptance(self):
        self.gate.write_text(self.gate.read_text().replace(ACCEPTANCE_FIXTURE, "require_external_acceptance"))
        self.commit()
        result = self.run_gate("full", good=False)
        self.assertIn("production acceptance verification is not provisioned", result.stdout)
        self.assertTrue((self.evidence / "test-suite-closure.json").is_file())
        self.assertNotIn("install", (self.root / "events.log").read_text().splitlines())
        self.assert_no_authorization()

    def test_missing_native_name_rejected(self):
        cmake = self.source / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("vacards-nesting-abi-smoke-c ", ""))
        self.reconfigure()
        result = self.run_gate(good=False)
        self.assertIn("missing native_nesting tests", result.stdout)
        self.assertTrue((self.evidence / "critical-tests.result.json").exists())
        self.assert_no_authorization()

    def test_actionable_rename_rejected(self):
        cmake = self.source / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("NAME fixture-actionable ", "NAME fixture-renamed-actionable "))
        self.reconfigure()
        result = self.run_gate("full", good=False)
        self.assertIn("missing actionable tests", result.stdout)
        self.assert_no_authorization()

    def test_equal_critical_count_with_wrong_names_rejected(self):
        cmake = self.source / "CMakeLists.txt"
        cmake.write_text(cmake.read_text().replace("fixture-critical-${number}", "alias-critical-${number}"))
        self.reconfigure()
        result = self.run_gate(good=False)
        self.assertIn("missing critical tests", result.stdout)
        self.assert_no_authorization()

    def test_exit_zero_skip_still_emits_rejected_result(self):
        cmake = self.source / "CMakeLists.txt"
        cmake.write_text(cmake.read_text() + '\nset_tests_properties(fixture-critical-1 PROPERTIES SKIP_RETURN_CODE 9)\n')
        self.reconfigure()
        result = self.run_gate(good=False, changes={"WIRING_FAIL": "critical-tests"})
        self.assertIn("result rejected", result.stdout)
        record = json.loads((self.evidence / "critical-tests.result.json").read_text())
        self.assertEqual(record["ctest_exit_code"], 0)
        self.assertEqual(record["outcome"], "rejected")
        self.assert_no_authorization()

    def test_missing_short_opt_in_rejected_before_portfolio_execution(self):
        self.gate.write_text(self.gate.read_text().replace("export VACARDS_NESTING_RUN_PORTFOLIO_DIFFERENTIAL=1",
                                                          ": # FIXTURE missing short opt-in"))
        self.commit()
        result = self.run_gate("full", good=False)
        self.assertIn("requires literal VACARDS_NESTING_RUN_PORTFOLIO_DIFFERENTIAL=1", result.stdout)
        self.assertNotIn("portfolio-short", (self.root / "events.log").read_text())
        self.assert_no_authorization()

    def test_missing_long_opt_in_rejected_before_long_execution(self):
        self.gate.write_text(self.gate.read_text().replace("export VACARDS_NESTING_RUN_PORTFOLIO_DIFFERENTIAL_60S=1",
                                                          ": # FIXTURE missing long opt-in"))
        self.commit()
        result = self.run_gate("full", good=False)
        self.assertIn("requires literal VACARDS_NESTING_RUN_PORTFOLIO_DIFFERENTIAL_60S=1", result.stdout)
        self.assertNotIn("portfolio-60s", (self.root / "events.log").read_text())
        self.assert_no_authorization()

    def test_late_rejected_record_blocks_closure_and_install(self):
        result = self.run_gate("full", good=False, changes={"WIRING_CLOSURE_TAMPER": "1"})
        self.assertIn("closure critical result rejected", result.stdout)
        self.assertEqual((self.evidence / "test-suite-closure.json").read_bytes(), b"")
        self.assertNotIn("install", (self.root / "events.log").read_text().splitlines())
        self.assert_no_authorization()

    def test_critical_build_drift_rejected_before_tests(self):
        result = self.run_gate(good=False, changes={"WIRING_DRIFT": "critical-build", "WIRING_DRIFT_TARGET": "cache"})
        self.assertIn("after critical build", result.stdout)
        self.assertNotIn("critical-tests", (self.root / "events.log").read_text())
        self.assertTrue(self.ledger.exists())
        self.assert_no_authorization()

    def test_critical_test_source_drift_rejected(self):
        result = self.run_gate(good=False, changes={"WIRING_DRIFT": "critical-tests"})
        self.assertIn("after critical tests", result.stdout)
        self.assert_no_authorization()

    def test_full_build_tool_drift_rejected_before_actionable(self):
        result = self.run_gate("full", good=False, changes={"WIRING_DRIFT": "full-build", "WIRING_DRIFT_TARGET": "tool"})
        self.assertIn("after full builds", result.stdout)
        self.assertNotIn("actionable-tests", (self.root / "events.log").read_text())
        self.assert_no_authorization()

    def test_actionable_ninja_drift_rejected_before_install(self):
        result = self.run_gate("full", good=False, changes={"WIRING_DRIFT": "actionable-tests", "WIRING_DRIFT_TARGET": "ninja"})
        self.assertIn("before install", result.stdout)
        self.assertFalse((self.install / "bin").exists())
        self.assert_no_authorization()

    def test_install_drift_rejected_and_failed_outputs_preserved(self):
        result = self.run_gate("full", good=False, changes={"WIRING_DRIFT": "install", "WIRING_DRIFT_TARGET": "cache"})
        self.assertIn("after install", result.stdout)
        self.assertTrue((self.install / "bin/inkscape").is_file())
        self.assert_no_authorization()

    def test_late_provenance_drift_rejected_before_attestation(self):
        result = self.run_gate("full", good=False, changes={"WIRING_DRIFT": "final-provenance"})
        self.assertIn("before final attestation", result.stdout)
        self.assert_no_authorization()

    def test_final_validator_drift_never_publishes_candidate(self):
        result = self.run_gate("full", good=False, changes={"WIRING_DRIFT": "final-validator"})
        self.assertIn("after candidate validation / before publication", result.stdout)
        self.assert_candidate_intact()
        self.assert_no_authorization()

    def test_final_validator_failure_never_overwrites_prior_failure(self):
        self.run_gate("full", good=False, changes={"WIRING_FAIL": "verify-release-attestation.sh", "WIRING_PRIOR_FAILURE": "1"})
        for name in ("VACARDS-RELEASE-GATE.failed.env", "VACARDS-RELEASE-GATE.failed.env.prior"):
            self.assertEqual((self.install / name).read_bytes(), b"prior failed record\x00")
        self.assertEqual(len(list(self.install.glob("VACARDS-RELEASE-GATE.failed.env.*"))), 1)
        self.assert_candidate_intact()
        self.assert_no_authorization()

    def test_existing_candidate_never_reused(self):
        result = self.run_gate("full", good=False, changes={"WIRING_EXISTING_PENDING": "file"})
        self.assertIn("could not exclusively write pending", result.stdout)
        self.assertEqual((self.install / "VACARDS-RELEASE-GATE.pending.env").read_bytes(), b"prior pending record\x00")
        self.assert_no_authorization()

    def test_candidate_write_io_failure_never_authorizes(self):
        result = self.run_gate("full", good=False, changes={"WIRING_EXISTING_PENDING": "directory"})
        self.assertIn("could not exclusively write pending", result.stdout)
        self.assertEqual(list((self.install / "VACARDS-RELEASE-GATE.pending.env").iterdir()), [])
        self.assert_no_authorization()

    def test_validator_mktemp_failure_never_authorizes(self):
        self.run_gate("full", good=False, changes={"WIRING_FAIL_LATE_TOOL": "mktemp"})
        self.assertIn("injected-mktemp-failure", (self.root / "events.log").read_text())
        self.assert_candidate_intact()
        self.assert_no_authorization()

    def test_validator_mv_failure_never_authorizes(self):
        self.run_gate("full", good=False, changes={"WIRING_FAIL_LATE_TOOL": "mv"})
        self.assertIn("injected-mv-failure", (self.root / "events.log").read_text())
        self.assert_candidate_intact()
        self.assert_no_authorization()

    def test_publication_io_failure_preserves_candidate_without_authority(self):
        try:
            result = self.run_gate("full", good=False, changes={"WIRING_PUBLICATION_IO": "1"})
            self.assertIn("could not exclusively publish", result.stdout)
            self.assert_candidate_intact()
            self.assertFalse((self.install / "VACARDS-RELEASE-GATE.env").exists())
            # Evidence may remain published: it has no installable binary or
            # complete installed provenance and is not a packaging prefix.
            self.assertEqual((self.evidence / "VACARDS-RELEASE-GATE.env").read_bytes(),
                             (self.install / "VACARDS-RELEASE-GATE.pending.env").read_bytes())
            self.assertFalse((self.evidence / "bin/inkscape").exists())
        finally:
            if self.install.exists(): self.install.chmod(0o700)

    def test_late_evidence_link_io_failure_never_publishes_install(self):
        try:
            result = self.run_gate("full", good=False, changes={"WIRING_PUBLICATION_IO": "evidence"})
            self.assertIn("could not exclusively publish", result.stdout)
            self.assert_candidate_intact()
            self.assert_no_authorization()
            self.assertEqual((self.evidence / "VACARDS-RELEASE-GATE.pending.env").read_bytes(),
                             (self.install / "VACARDS-RELEASE-GATE.pending.env").read_bytes())
        finally:
            if self.evidence.exists(): self.evidence.chmod(0o700)

    def publication_collision(self, kind):
        result = self.run_gate("full", good=False, changes={"WIRING_PUBLICATION_TARGET": kind})
        self.assertIn("could not exclusively publish", result.stdout)
        self.assert_candidate_intact()
        self.assertFalse((self.evidence / "VACARDS-RELEASE-GATE.env").exists())
        return self.install / "VACARDS-RELEASE-GATE.env"

    def test_publication_file_collision_never_overwrites(self):
        self.assertEqual(self.publication_collision("file").read_bytes(), b"prior non-authorizing destination\x00")

    def test_publication_directory_collision_never_links_inside(self):
        self.assertEqual(list(self.publication_collision("directory").iterdir()), [])

    def test_publication_symlink_collision_never_follows(self):
        final = self.publication_collision("symlink")
        self.assertTrue(final.is_symlink())
        self.assertEqual(final.read_bytes(), b"prior non-authorizing destination\x00")

    def test_evidence_publication_collision_rejects_before_install_publication(self):
        result = self.run_gate("full", good=False, changes={"WIRING_PUBLICATION_TARGET": "file", "WIRING_PUBLICATION_EVIDENCE": "1"})
        self.assertIn("could not exclusively publish", result.stdout)
        self.assertFalse((self.install / "VACARDS-RELEASE-GATE.env").exists())
        self.assertEqual((self.evidence / "VACARDS-RELEASE-GATE.env").read_bytes(), b"prior non-authorizing destination\x00")
        self.assert_candidate_intact()

    def test_real_shell_candidate_mode_and_python_flag_wiring(self):
        verifier = self.gate.parent / "verify-release-attestation.sh"
        shutil.copyfile(ROOT / "packaging/macos/vacards/verify-release-attestation.sh", verifier)
        self.commit()
        result = self.run_gate("full")
        self.assertIn("Validated VACards pending candidate (not packaging authorization)", result.stdout)
        self.assertIn("fake-packaging-verify-attestation", (self.root / "events.log").read_text())

    def test_real_shell_default_rejects_pending_only(self):
        verifier = self.gate.parent / "verify-release-attestation.sh"
        shutil.copyfile(ROOT / "packaging/macos/vacards/verify-release-attestation.sh", verifier)
        self.install.mkdir()
        (self.install / "VACARDS-RELEASE-GATE.pending.env").write_text("format=3\nscope=full\n")
        result = self.command(["/bin/sh", verifier, self.install], good=False)
        self.assertIn("missing " + str(self.install / "VACARDS-RELEASE-GATE.env"), result.stdout)
        self.assert_no_authorization()


class CandidateModes(unittest.TestCase):
    """Real basename/CLI/hash plumbing; packaging input schema is stubbed only."""

    def setUp(self):
        spec = importlib.util.spec_from_file_location("candidate_provenance", ROOT / "packaging/vacards/packaging-provenance.py")
        self.provenance = importlib.util.module_from_spec(spec)
        with mock.patch.object(sys, "dont_write_bytecode", True):
            spec.loader.exec_module(self.provenance)
        # Preserve lower-level binding/publication assertions when the release
        # worker integrates the production guard. Only this in-process fixture
        # module is patched; real CLI rejection has independent coverage.
        guard = mock.patch.object(self.provenance, "require_production_authorization", create=True)
        guard.start()
        self.addCleanup(guard.stop)

    def test_python_candidate_basename_never_falls_back_to_final(self):
        p = self.provenance
        with tempfile.TemporaryDirectory(prefix="candidate-mode-", dir=os.environ.get("TMPDIR")) as temporary:
            prefix = Path(temporary)
            (prefix / p.INPUTS).write_bytes(b"synthetic packaging inputs")
            (prefix / "VACARDS-CAIRO.txt").write_bytes(b"synthetic Cairo marker")
            (prefix / "VACARDS-LIBCDR.env").write_bytes(b"synthetic libcdr marker")
            ledger = prefix / "VACARDS-GATE-RUN.json"
            ledger.write_bytes(b'{"fixture_only": "opaque local reservation bytes"}\n')
            provenance = prefix / "VACARDS-BUILD-PROVENANCE.env"
            provenance.write_text("packaging_inputs_sha256=" + p.digest(prefix / p.INPUTS)
                                  + "\ngate_run_sha256=" + p.digest(ledger) + "\n")
            libcdr_hash = p.digest(prefix / "VACARDS-LIBCDR.env")
            data = {"files": {"cairo_marker": {"sha256": p.digest(prefix / "VACARDS-CAIRO.txt")},
                              "libcdr_manifest": {"sha256": libcdr_hash}},
                    "roles": {"libcdr": "/synthetic/libcdr"},
                    "libraries": [{"file": {"realpath": "/synthetic/libcdr", "sha256": "0" * 64}}]}
            candidate = prefix / "VACARDS-RELEASE-GATE.pending.env"
            final = prefix / "VACARDS-RELEASE-GATE.env"
            candidate.write_text("format=3\nscope=full\nbuild_provenance_sha256=" + p.digest(provenance)
                                 + "\nlibcdr_manifest_sha256=" + libcdr_hash + "\nlibcdr_library_sha256=" + "0" * 64 + "\n")
            with mock.patch.object(p, "load_inputs", return_value=data):
                self.assertEqual(p.attest(prefix, candidate=True)[0], data)
                with self.assertRaises(FileNotFoundError): p.attest(prefix)
                final.write_text("format=invalid\n")
                self.assertEqual(p.attest(prefix, candidate=True)[0], data)
                with self.assertRaises(p.Rejected): p.attest(prefix)
                final.write_bytes(candidate.read_bytes())
                candidate.write_text("format=invalid\n")
                self.assertEqual(p.attest(prefix)[0], data)
                with self.assertRaises(p.Rejected): p.attest(prefix, candidate=True)

    def test_python_candidate_cli_is_explicitly_non_authorizing(self):
        p = self.provenance
        with mock.patch.object(p, "attest") as attest, mock.patch.object(sys, "argv", [
                "packaging-provenance.py", "verify-attestation", "--candidate", "/synthetic/prefix"]):
            output = io.StringIO()
            with contextlib.redirect_stdout(output): p.main()
            attest.assert_called_once_with("/synthetic/prefix", candidate=True)
            self.assertIn("not packaging authorization", output.getvalue())

    def test_python_cli_rejects_custom_basename_and_bundle_candidate_flag(self):
        for args in (["verify-attestation", "/synthetic/prefix", "--candidate=../escape.env"],
                     ["restore-bundle", "--app", "/synthetic/app", "--install", "/synthetic/prefix",
                      "--cairo-prefix", "/synthetic/cairo", "--candidate"],
                     ["verify-bundle", "/synthetic/app", "--candidate"]):
            with self.subTest(args=args), mock.patch.object(sys, "argv", ["packaging-provenance.py", *args]):
                with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                    self.provenance.main()
                self.assertEqual(error.exception.code, 2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--gate-run-helper", type=Path, default=HELPER)
    options, remaining = parser.parse_known_args()
    HELPER = options.gate_run_helper.resolve(strict=True)
    print("COMPILER-FREE WIRING: real CMake/Ninja/CTest/reservation; fake app/provenance, no release proof", flush=True)
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
