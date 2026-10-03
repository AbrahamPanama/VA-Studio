#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stdlib qualification reservation tests; temp Git repos/fake compiler bytes.

Run with Python 3.10+, Git, CMake and Ninja on PATH. The real CMake probe uses
LANGUAGES NONE, fake compiler bytes, a touch-only Ninja rule and CMake-only
CTest scripts. No compiler or application build is executed.
Native Windows runs exercise NtCreateFile; link tests require Developer Mode
or permission to create symlinks. Skips must remain visible in runner evidence.
"""
import copy
from concurrent.futures import ThreadPoolExecutor
from contextlib import nullcontext
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


sys.dont_write_bytecode = True
TOOL = Path(__file__).resolve().parents[1] / "packaging/vacards/gate-run.py"
SPEC = importlib.util.spec_from_file_location("gate_run", TOOL)
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


def child():
    """Real subprocess barriers/faults, kept out of the production helper."""
    phase, source, build, mode, install = sys.argv[2:]
    install = None if install == "-" else install
    original_open, original_mkdir = GATE.Directory.open_file, GATE.Directory.mkdir
    original_fdopen = os.fdopen

    def pause():
        print("ready", flush=True)
        sys.stdin.readline()

    def open_file(self, name, *, create=False):
        if create and phase == "race":
            pause()
        return original_open(self, name, create=create)

    def mkdir(self, name):
        if phase == "before-evidence" and name == GATE.EVIDENCE:
            pause()
        result = original_mkdir(self, name)
        if ((phase == "after-evidence" and name == GATE.EVIDENCE) or
                (phase == "after-install" and install and name == Path(install).name)):
            pause()
        return result

    class PartialWriter:
        def __init__(self, stream):
            self.stream = stream
        def __enter__(self):
            return self
        def __exit__(self, *args):
            return self.stream.__exit__(*args)
        def __getattr__(self, key):
            return getattr(self.stream, key)
        def write(self, data):
            self.stream.write(data[:41])
            self.stream.flush()
            os.fsync(self.stream.fileno())
            pause()
            return self.stream.write(data[41:])

    def fdopen(fd, mode):
        stream = original_fdopen(fd, mode)
        return PartialWriter(stream) if mode == "wb" and phase == "partial-ledger" else stream

    with mock.patch.object(GATE.Directory, "open_file", open_file), \
            mock.patch.object(GATE.Directory, "mkdir", mkdir), mock.patch.object(os, "fdopen", fdopen):
        try:
            result = GATE.claim(source, build, mode, install)
            print(result["run_uuid"])
        except (ValueError, OSError) as error:
            print(str(error), file=sys.stderr)
            return 1
    return 0


class GateRunTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="VACards gate á ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / "source with spaces"
        self.source.mkdir()
        self.environment = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
        self.environment.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
                                GIT_TERMINAL_PROMPT="0")
        self.git("init", "-q")
        self.git("config", "user.name", "Gate fixture")
        self.git("config", "user.email", "gate-fixture@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        (self.source / "source.c").write_bytes(b"int fixture = 1;\n")
        (self.source / ".gitignore").write_bytes(b"ignored/\n")
        self.git("add", ".")
        self.git("commit", "-q", "-m", "Temporary fixture only")
        self.tools = self.root / "fake compiler directory"
        self.tools.mkdir()
        self.compilers = {}
        for key, name in zip(GATE.COMPILERS, ("cc.exe", "cxx.exe", "rustc.exe")):
            compiler = self.tools / name
            compiler.write_bytes(b"NOT EXECUTABLE CODE: " + key.encode())
            compiler.chmod(0o755)
            self.compilers[key] = compiler
        self.build = self.new_build("quick build")
        self.install_parent = self.root / "installs"
        self.install_parent.mkdir()
        self.install = self.install_parent / "fresh full install"
        self.ledger = self.build / GATE.LEDGER

    def git(self, *args, source=None):
        result = subprocess.run(["git", "-C", str(source or self.source), *args],
                                env=self.environment, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def new_build(self, name, *, source=None, build_type="Release", features=True):
        build = self.root / name
        build.mkdir()
        fields = {
            "CMAKE_HOME_DIRECTORY": ("INTERNAL", (source or self.source).as_posix()),
            "CMAKE_CACHEFILE_DIR": ("INTERNAL", build.as_posix()),
            "CMAKE_GENERATOR": ("INTERNAL", "Ninja"), "CMAKE_BUILD_TYPE": ("STRING", build_type),
            "CMAKE_INSTALL_PREFIX": ("PATH", (self.root / "cache default unused").as_posix()),
            "VACARDS_NESTING_RUST_TARGET": ("STRING", "aarch64-apple-darwin"),
            "CMAKE_C_FLAGS": ("STRING", "-O2 -DFIXTURE=1"),
            "CMAKE_CXX_FLAGS": ("STRING", "-O2 -Wall"), "WITH_VACARDS_NESTING": ("BOOL", "ON"),
            **{key: ("FILEPATH", value.as_posix()) for key, value in self.compilers.items()},
        }
        data = "# Fake configured cache; no compiler has run\n" + "".join(
            f"{key}:{kind}={value}\n" for key, (kind, value) in fields.items())
        (build / "CMakeCache.txt").write_bytes(data.encode())
        (build / "build.ninja").write_bytes(b"# inert fixture: never execute\n")
        if features:
            (build / GATE.FEATURES).write_bytes(b"format=1\nWITH_VACARDS_NESTING=ON\n")
        return build

    def cli(self, *args, environment=None):
        return subprocess.run([sys.executable, str(TOOL), *map(str, args)], cwd=self.root,
                              env=environment, capture_output=True, timeout=20)

    def claim(self, *, build=None, source=None, mode="quick", install=None):
        args = ["claim", "--source", source or self.source, "--build", build or self.build, "--mode", mode]
        if install is not None:
            args += ["--install", install]
        result = self.cli(*args)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"no success attestation", result.stdout)
        return json.loads(((build or self.build) / GATE.LEDGER).read_bytes())

    def reject_claim(self, *, build=None, source=None, mode="quick", install=None, reason=None):
        target = build or self.build
        before = {name: os.path.lexists(target / name) for name in (GATE.LEDGER, GATE.EVIDENCE)}
        args = ["claim", "--source", source or self.source, "--build", target, "--mode", mode]
        if install is not None:
            args += ["--install", install]
        result = self.cli(*args)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertNotIn(b"Traceback", result.stderr)
        if reason:
            self.assertIn(reason.encode(), result.stderr)
        self.assertEqual(before, {name: os.path.lexists(target / name) for name in before},
                         "static validation failure created an output")

    def reject_verify(self, build=None, reason=None):
        target = build or self.build
        before = (target / GATE.LEDGER).read_bytes()
        result = self.cli("verify", "--build", target)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertNotIn(b"Traceback", result.stderr)
        if reason:
            self.assertIn(reason.encode(), result.stderr)
        self.assertEqual((target / GATE.LEDGER).read_bytes(), before)

    def rewrite(self, record, *, checksum=True):
        if checksum:
            record["record_sha256"] = GATE.digest(GATE.canonical_json(
                {key: value for key, value in record.items() if key != "record_sha256"}))
        self.writable_fixture(self.ledger)
        self.ledger.write_bytes(GATE.canonical_json(record) + b"\n")

    def writable_fixture(self, path):
        if os.name != "nt":
            path.chmod(0o600)
            return
        # Unseal only our temporary tamper-test file. The native UCRT Python
        # runtime can report successful chmod without clearing READONLY.
        # Production sealing and all subsequent rejection checks stay intact.
        import ctypes as ct
        kernel = ct.WinDLL("kernel32", use_last_error=True)
        get_attributes = kernel.GetFileAttributesW
        get_attributes.argtypes, get_attributes.restype = [ct.c_wchar_p], ct.c_uint32
        set_attributes = kernel.SetFileAttributesW
        set_attributes.argtypes, set_attributes.restype = [ct.c_wchar_p, ct.c_uint32], ct.c_int
        attributes = get_attributes(str(path))
        if attributes == 0xFFFFFFFF:
            raise ct.WinError(ct.get_last_error())
        if attributes & stat.FILE_ATTRIBUTE_READONLY:
            writable = attributes & ~stat.FILE_ATTRIBUTE_READONLY
            if not set_attributes(str(path), writable or stat.FILE_ATTRIBUTE_NORMAL):
                raise ct.WinError(ct.get_last_error())
        self.assertFalse(get_attributes(str(path)) & stat.FILE_ATTRIBUTE_READONLY)

    def link(self, link, target, *, directory=False):
        try:
            link.symlink_to(target, target_is_directory=directory)
        except OSError as error:
            if os.name == "nt" and getattr(error, "winerror", None) == 1314:
                self.skipTest("Windows symlink tests require Developer Mode or symlink privilege")
            raise

    def start_child(self, phase, *, build=None, mode="quick", install=None):
        process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "--child", phase,
                                    str(self.source), str(build or self.build), mode,
                                    str(install) if install else "-"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        def cleanup():
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=10)
        self.addCleanup(cleanup)
        return process

    def ready(self, process):
        with ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(process.stdout.readline)
            try:
                output = future.result(timeout=20)
            except TimeoutError:
                process.kill()
                raise
        if output.rstrip(b"\r\n") != b"ready":
            _, error = process.communicate(timeout=10)
            self.fail(f"child failed before barrier: {output!r} {error!r}")

    def test_fresh_quick_claim_and_verify_preserve_record_and_failure_logs(self):
        record = self.claim()
        before = self.ledger.read_bytes()
        self.assertEqual(record["inputs"]["source"]["head"], self.git("rev-parse", "HEAD").decode().strip())
        self.assertEqual(record["inputs"]["source"]["git_common_dir"], str(self.source / ".git"))
        self.assertEqual(record["root_ninja_log_at_claim"], "absent")
        log = self.build / GATE.EVIDENCE / "failure.log"
        log.write_bytes(b"raw failure\x00\r\n")
        (self.build / ".ninja_log").write_bytes(b"# ninja log v5\n")
        self.assertEqual(self.cli("verify", "--build", self.build).returncode, 0)
        self.assertEqual(self.ledger.read_bytes(), before)
        self.assertEqual(log.read_bytes(), b"raw failure\x00\r\n")
        self.assertFalse((self.build / GATE.EVIDENCE / "VACARDS-RELEASE-GATE.env").exists())
        self.reject_claim(reason="existing VACARDS-GATE-RUN.json")
        self.assertEqual(self.ledger.read_bytes(), before)

    def test_fresh_full_reserves_explicit_install_override(self):
        record = self.claim(mode="full", install=self.install)
        self.assertEqual(record["inputs"]["install"], str(self.install))
        self.assertEqual(list(self.install.iterdir()), [])
        (self.install / "installed.txt").write_bytes(b"fixture installation")
        self.assertEqual(self.cli("verify", "--build", self.build).returncode, 0)
        self.reject_claim(mode="full", install=self.install)

    def test_quick_then_full_require_independent_builds(self):
        quick = self.claim()
        self.reject_claim(mode="full", install=self.install)
        full_build = self.new_build("full build")
        full = self.claim(build=full_build, mode="full", install=self.install)
        self.assertNotEqual(quick["run_uuid"], full["run_uuid"])
        self.assertNotEqual(quick["configuration_sha256"], full["configuration_sha256"])
        self.assertEqual(self.cli("verify", "--build", full_build).returncode, 0)

    def test_nested_probe_log_does_not_block_claim(self):
        probe = self.build / "CMakeFiles/CMakeScratch/TryCompile-123"
        probe.mkdir(parents=True)
        (probe / ".ninja_log").write_bytes(b"compiler probe")
        self.claim()

    def test_absence_of_log_is_only_an_observation(self):
        (self.build / "old-executable").write_bytes(b"unknown prior history")
        record = self.claim()
        self.assertNotIn("never_built", record)
        self.assertEqual(record["root_ninja_log_at_claim"], "absent")

    def test_existing_root_log_even_empty_blocks_claim(self):
        (self.build / ".ninja_log").touch()
        self.reject_claim(reason="existing .ninja_log")

    def test_existing_evidence_empty_or_failed_is_immutable(self):
        evidence = self.build / GATE.EVIDENCE
        evidence.mkdir()
        self.reject_claim(reason="existing vacards-release-evidence")
        (evidence / "failed.log").write_bytes(b"original failure")
        self.reject_claim()
        self.assertEqual((evidence / "failed.log").read_bytes(), b"original failure")

    def test_existing_partial_ledger_never_reused(self):
        for data in (b"", b'{"schema_version":1,'):
            with self.subTest(data=data):
                self.ledger.write_bytes(data)
                self.reject_claim(reason="existing VACARDS-GATE-RUN.json")
                self.reject_verify()
                self.assertEqual(self.ledger.read_bytes(), data)

    def test_mode_install_mismatches_and_existing_install(self):
        self.reject_claim(mode="full", reason="requires an install prefix")
        self.reject_claim(install=self.install, reason="quick mode")
        self.reject_claim(mode="dev")
        self.install.mkdir()
        self.reject_claim(mode="full", install=self.install, reason="already exists")
        self.assertEqual(list(self.install.iterdir()), [])

    def test_install_requires_existing_parent_and_disjoint_trees(self):
        for target in (self.root / "missing/child", self.build / "stage", self.source / "stage", self.root):
            with self.subTest(target=target):
                self.reject_claim(mode="full", install=target)

    def test_missing_cache_fields_and_duplicate_keys(self):
        cache = self.build / "CMakeCache.txt"
        original = cache.read_text(encoding="utf-8")
        for key in sorted(GATE.REQUIRED):
            with self.subTest(key=key, case="missing"):
                cache.write_text("\n".join(line for line in original.splitlines() if not line.startswith(key + ":")), encoding="utf-8")
                self.reject_claim(reason="missing cache field")
            for kind in ("STRING", "INTERNAL"):
                with self.subTest(key=key, case=kind):
                    cache.write_text(original + f'"{key}":{kind}=duplicated\n', encoding="utf-8")
                    self.reject_claim(reason="duplicate cache field")

    def test_malformed_cache_and_empty_fields(self):
        cache = self.build / "CMakeCache.txt"
        original = cache.read_bytes()
        for suffix in (b"bare-key\n", b"oops:UNKNOWN=value\n", b"=value\n", b"oops:STRING=\0bad\n",
                       b"oops:STRING=bad\rvalue\n"):
            with self.subTest(suffix=suffix):
                cache.write_bytes(original + suffix)
                self.reject_claim()
        cache.write_bytes(original.replace(b"CMAKE_GENERATOR:INTERNAL=Ninja", b"CMAKE_GENERATOR:INTERNAL="))
        self.reject_claim(reason="missing cache field")

    def test_crlf_cache_and_feature_absence_are_recorded(self):
        cache = self.build / "CMakeCache.txt"
        cache.write_bytes(cache.read_bytes().replace(b"\n", b"\r\n"))
        (self.build / GATE.FEATURES).unlink()
        record = self.claim()
        self.assertIsNone(record["inputs"]["effective_features"])
        self.assertEqual(self.cli("verify", "--build", self.build).returncode, 0)
        (self.build / GATE.FEATURES).write_bytes(b"format=1\n")
        self.reject_verify(reason="identity drift")

    def test_ninja_generator_and_configuration_tree_required(self):
        cache = self.build / "CMakeCache.txt"
        original = cache.read_text(encoding="utf-8")
        cache.write_text(original.replace("=Ninja\n", "=Unix Makefiles\n"), encoding="utf-8")
        self.reject_claim(reason="Ninja generator")
        cache.write_text(original.replace(self.build.as_posix(), self.root.as_posix()), encoding="utf-8")
        self.reject_claim(reason="different build tree")
        cache.write_text(original, encoding="utf-8")
        (self.build / "build.ninja").unlink()
        self.reject_claim()

    def test_same_sha_other_worktree_is_rejected(self):
        other = self.root / "other worktree"
        self.git("worktree", "add", "--detach", str(other), "HEAD")
        self.assertEqual(self.git("rev-parse", "HEAD"), self.git("rev-parse", "HEAD", source=other))
        self.reject_claim(source=other, reason="different source worktree")

    def test_same_sha_other_configuration_or_copied_ledger_is_rejected(self):
        first = self.claim()
        second_build = self.new_build("different config", build_type="RelWithDebInfo")
        second = self.claim(build=second_build)
        self.assertEqual(first["inputs"]["source"]["head"], second["inputs"]["source"]["head"])
        self.assertNotEqual(first["configuration_sha256"], second["configuration_sha256"])
        target = second_build / GATE.LEDGER
        self.writable_fixture(target)
        target.write_bytes(self.ledger.read_bytes())
        self.reject_verify(second_build, reason="identity drift")

    def test_source_dirty_diff_and_untracked_content_drift(self):
        (self.source / "source.c").write_bytes(b"int fixture = 2;\n")
        (self.source / "untracked.txt").write_bytes(b"one")
        record = self.claim()
        self.assertTrue(record["inputs"]["source"]["untracked"])
        (self.source / "source.c").write_bytes(b"int fixture = 3;\n")
        self.reject_verify(reason="identity drift")
        (self.source / "source.c").write_bytes(b"int fixture = 2;\n")
        (self.source / "untracked.txt").write_bytes(b"two")
        self.reject_verify(reason="identity drift")

    def test_source_commit_staging_and_assume_unchanged_drift(self):
        self.git("update-index", "--assume-unchanged", "source.c")
        self.claim()
        (self.source / "source.c").write_bytes(b"int fixture = 7;\n")
        self.reject_verify(reason="identity drift")
        self.git("update-index", "--no-assume-unchanged", "source.c")
        self.git("add", "source.c")
        self.reject_verify(reason="identity drift")
        self.git("commit", "-q", "-m", "Fixture next revision")
        self.reject_verify(reason="identity drift")

    def test_missing_source_file_and_new_submodule_drift(self):
        self.claim()
        original = (self.source / "source.c").read_bytes()
        (self.source / "source.c").unlink()
        self.reject_verify(reason="identity drift")
        (self.source / "source.c").write_bytes(original)
        self.git("update-index", "--add", "--cacheinfo", "160000," +
                 self.git("rev-parse", "HEAD").decode().strip() + ",missing-submodule")
        self.reject_verify(reason="identity drift")

    def test_cache_ninja_and_feature_bytes_drift(self):
        self.claim()
        for filename in ("CMakeCache.txt", "build.ninja", GATE.FEATURES):
            with self.subTest(filename=filename):
                path = self.build / filename
                original = path.read_bytes()
                path.write_bytes(original + b"# changed bytes\n")
                self.reject_verify(reason="identity drift")
                path.write_bytes(original)
        (self.build / GATE.FEATURES).unlink()
        self.reject_verify(reason="identity drift")

    def test_every_compiler_content_is_hashed_not_executed(self):
        record = self.claim()
        for key, compiler in self.compilers.items():
            with self.subTest(key=key):
                original, times = compiler.read_bytes(), compiler.stat()
                self.assertEqual(record["inputs"]["executables"][key]["sha256"], GATE.digest(original))
                compiler.write_bytes(b"X" * len(original))
                os.utime(compiler, ns=(times.st_atime_ns, times.st_mtime_ns))
                self.reject_verify(reason="identity drift")
                compiler.write_bytes(original)

    def test_cache_strings_are_not_shell_evaluated(self):
        cache = self.build / "CMakeCache.txt"
        marker = self.root / "DO_NOT_EXECUTE"
        with cache.open("a", encoding="utf-8") as stream:
            stream.write(f"CMAKE_CXX_FLAGS_RELEASE:STRING=$(touch '{marker}') ; arbitrary=bytes\n")
        self.claim()
        self.assertFalse(marker.exists())

    def test_compiler_missing_or_command_instead_of_absolute_path(self):
        cache = self.build / "CMakeCache.txt"
        original = cache.read_text(encoding="utf-8")
        compiler = self.compilers["CMAKE_C_COMPILER"]
        for value in ("cc --version", (self.root / "missing compiler").as_posix()):
            cache.write_text(original.replace(compiler.as_posix(), value), encoding="utf-8")
            self.reject_claim()

    def test_environment_flags_drift_and_git_overrides_ignored(self):
        self.claim()
        environment = dict(os.environ, RUSTFLAGS="-C opt-level=1")
        result = self.cli("verify", "--build", self.build, environment=environment)
        self.assertNotEqual(result.returncode, 0)
        environment = dict(os.environ, GIT_DIR=str(self.root / "not-a-repository"), GIT_WORK_TREE=str(self.root))
        result = self.cli("verify", "--build", self.build, environment=environment)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_compiler_cargo_environment_added_changed_removed(self):
        keys = ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "COMPILER_PATH", "LIBRARY_PATH",
                "CARGO_BUILD_RUSTFLAGS", "CARGO_BUILD_RUSTC_WRAPPER", "CARGO_BUILD_TARGET",
                "CARGO_TARGET_AARCH64_APPLE_DARWIN_LINKER", "CARGO_PROFILE_RELEASE_LTO",
                "RUSTC_WRAPPER", "RUSTC_WORKSPACE_WRAPPER", "RUSTDOCFLAGS", "CFLAGS_aarch64_apple_darwin",
                "INCLUDE", "LIBPATH", "CL", "_CL_", "PATH")
        # Exercise absence, addition, mutation, and removal without running any
        # override as a command. Do not copy credential values into the ledger.
        with mock.patch.dict(os.environ, {key: "baseline override with spaces" for key in keys if key != "PATH"}):
            GATE.claim(str(self.source), str(self.build), "quick")
            GATE.verify(str(self.build))
            for key in keys:
                if key == "PATH":
                    continue
                with self.subTest(removed=key):
                    previous = os.environ.pop(key)
                    with self.assertRaisesRegex(GATE.GateError, "identity drift"):
                        GATE.verify(str(self.build))
                    os.environ[key] = previous
        # PATH must remain usable for the helper's read-only Git inspection.
        build = self.new_build("environment drift")
        self.claim(build=build)
        for key in keys:
            with self.subTest(key=key):
                environment = dict(os.environ)
                environment[key] = (os.environ["PATH"] + os.pathsep + str(self.root)
                                    if key == "PATH" else "changed override with spaces")
                result = self.cli("verify", "--build", build, environment=environment)
                self.assertNotEqual(result.returncode, 0, (key, result.stdout))
                self.assertIn(b"identity drift", result.stderr)

    def test_environment_value_capture_no_credentials(self):
        environment = {"CPATH": "", "CARGO_BUILD_RUSTFLAGS": "-C opt-level=1",
                       "RUSTC_WRAPPER": "/fake wrapper with spaces",
                       "CARGO_REGISTRIES_FIXTURE_TOKEN": "synthetic-test-placeholder"}
        with mock.patch.dict(os.environ, environment):
            recorded = GATE.build_environment()
            self.assertEqual(recorded["CPATH"], "")
            self.assertEqual(recorded["CARGO_BUILD_RUSTFLAGS"], "-C opt-level=1")
            self.assertEqual(recorded["RUSTC_WRAPPER"], "/fake wrapper with spaces")
            self.assertNotIn("CARGO_REGISTRIES_FIXTURE_TOKEN", recorded)
            for key in ("CPATH", "CARGO_BUILD_RUSTFLAGS", "RUSTC_WRAPPER"):
                with self.subTest(key=key):
                    value = os.environ.pop(key)
                    self.assertNotEqual(GATE.build_environment(), recorded)
                    os.environ[key] = value

    def test_nested_control_closure_and_extensionless_inputs(self):
        nested = self.build / "nested controls"
        nested.mkdir()
        (self.build / "build.ninja").write_text("part = nested$ controls/one\ninclude $part\n", encoding="utf-8")
        (nested / "one").write_text("subninja nested$ controls/two\n", encoding="utf-8")
        (nested / "two").write_text("# deepest Ninja include\n", encoding="utf-8")
        (self.build / "CTestTestfile.cmake").write_text('subdirs("nested controls")\n', encoding="utf-8")
        (nested / "CTestTestfile.cmake").write_text('include("${CMAKE_CURRENT_LIST_DIR}/no extension")\n', encoding="utf-8")
        (nested / "no extension").write_text('# included CTest commands\n', encoding="utf-8")
        (nested / "command response.rsp").write_text("original response flags\n", encoding="utf-8")
        record = self.claim()
        names = [item["path"] for item in record["inputs"]["configuration_files"]]
        self.assertEqual(names, sorted(names))
        for path in nested.iterdir():
            with self.subTest(path=path.name):
                original = path.read_bytes()
                path.write_bytes(original + b"# tamper\n")
                self.reject_verify(reason="identity drift")
                path.write_bytes(original)
        self.assertEqual(self.cli("verify", "--build", self.build).returncode, 0)

    def test_new_removed_and_escaping_controls(self):
        nested = self.build / "nested"
        nested.mkdir()
        script = nested / "probe.cmake"
        script.write_text("# original\n", encoding="utf-8")
        self.claim()
        script.unlink()
        self.reject_verify(reason="identity drift")
        script.write_text("# original\n", encoding="utf-8")
        for name in ("CTestCustom.cmake", "nested/new.ninja", "nested/new.py", "nested/new.rsp"):
            path = self.build / name
            path.write_text("# unexpected new control\n", encoding="utf-8")
            self.reject_verify(reason="identity drift")
            path.unlink()
        # New ordinary output bytes and test logs are deliberately not input attestations.
        (self.build / "output.o").write_bytes(b"new binary output")
        self.link(self.build / "output library", self.build / "output.o")
        self.assertEqual(self.cli("verify", "--build", self.build).returncode, 0)
        script.unlink()
        external = self.root / "external.cmake"
        external.write_text("# must not follow\n", encoding="utf-8")
        self.link(script, external)
        self.reject_verify()

    def test_control_include_validation_creates_no_outputs(self):
        root = self.build / "build.ninja"
        external = self.root / "external control"
        external.write_text("# outside configured trees\n", encoding="utf-8")
        for content in ("include missing\n", "include $unknown\n", "include ../external\n",
                        "include " + external.as_posix().replace(" ", "$ ") + "\n",
                        "include build.ninja\n"):
            with self.subTest(content=content):
                root.write_text(content, encoding="utf-8")
                self.reject_claim()
        root.write_text("# inert\n", encoding="utf-8")
        testfile = self.build / "CTestTestfile.cmake"
        for content in ('include("${UNKNOWN}/file")\n', 'include("../external")\n',
                        'include("' + external.as_posix() + '")\n', 'include(UnknownModule)\n',
                        'include("relative.cmake")\n', 'include("${CMAKE_BINARY_DIR}/relative.cmake")\n',
                        'add_test(probe cmake -P relative.cmake)\n'):
            with self.subTest(content=content):
                testfile.write_text(content, encoding="utf-8")
                self.reject_claim()

    def test_include_grammar_scopes_ignored_source_and_optional_absence(self):
        ignored = self.source / "ignored"
        ignored.mkdir()
        leaf = ignored / "included control"
        leaf.write_text("# ignored Git bytes must still be bound when included\n", encoding="utf-8")
        controls = self.build / "control files"
        controls.mkdir()
        (self.build / "build.ninja").write_bytes(
            b"prefix = control$ files\r\ninclude $\r\n  ${prefix}/global\r\n"
            b"subninja ${prefix}/child\r\ninclude ${prefix}/$which\r\n")
        (controls / "global").write_text("which = one\n", encoding="utf-8")
        (controls / "child").write_text("which = two\ninclude ${prefix}/$which\n", encoding="utf-8")
        for name in ("one", "two"):
            (controls / name).write_text("# " + name + "\n", encoding="utf-8")
        testfile = self.build / "CTestTestfile.cmake"
        testfile.write_text('#[=[ include("${DONT_EVALUATE}/ignored") ]=]\n'
                            '# include("${COMMENT}/ignored")\n'
                            'include([=[' + leaf.as_posix() + ']=])\n'
                            'include("${CMAKE_CURRENT_LIST_DIR}/optional.cmake" OPTIONAL)\n', encoding="utf-8")
        record = self.claim()
        closure = {entry["path"]: entry for entry in record["inputs"]["control_includes"]}
        for path in (controls / "one", controls / "two", leaf):
            self.assertIn(str(path), closure)
        self.assertIsNone(closure[str(self.build / "optional.cmake")]["sha256"])
        original = leaf.read_bytes()
        leaf.write_bytes(original + b"# changed\n")
        self.reject_verify(reason="identity drift")
        leaf.write_bytes(original)
        (self.build / "optional.cmake").write_text("# appeared later\n", encoding="utf-8")
        self.reject_verify(reason="identity drift")

    def test_internal_parent_reference_and_link_parent_escape(self):
        nested = self.build / "nested"
        nested.mkdir()
        leaf = self.build / "install leaf"
        leaf.write_text("# install include\n", encoding="utf-8")
        install = self.build / "cmake_install.cmake"
        install.write_text('include("' + (nested / "../install leaf").as_posix() + '")\n', encoding="utf-8")
        self.claim()
        leaf.write_text("# changed install include\n", encoding="utf-8")
        self.reject_verify(reason="identity drift")
        other = self.new_build("link parent build")
        self.link(other / "alias", self.root, directory=True)
        (other / "CTestTestfile.cmake").write_text(
            'include("' + (other / "alias/../install leaf").as_posix() + '")\n', encoding="utf-8")
        self.reject_claim(build=other)

    def test_literal_ninja_cwd_forms_without_shell_execution(self):
        # Parse both generated shell spellings on every host. This is not a
        # native Windows execution claim: the fake commands are never run.
        for index, prefix in enumerate(('cd "{cwd}" && cmake -P script',
                                        'cmd.exe /C "cd /D "{cwd}" && cmake -P script"')):
            build = self.new_build("literal cwd " + str(index))
            nested = build / "nested scripts"
            nested.mkdir()
            script = nested / "script"
            script.write_text("# extensionless invoked script\n", encoding="utf-8")
            (build / "build.ninja").write_text("# inert grammar fixture\n  COMMAND = " +
                                               prefix.format(cwd=nested.as_posix()) + "\n", encoding="utf-8")
            record = self.claim(build=build)
            self.assertIn(str(script), [entry["path"] for entry in record["inputs"]["control_includes"]])
            script.write_text("# altered\n", encoding="utf-8")
            self.reject_verify(build, reason="identity drift")

    def test_configuration_inventory_and_include_record_strictness(self):
        original = self.claim()
        for name in ("configuration_files", "control_includes"):
            for change in (lambda x: [], lambda x: x + x, lambda x: [{**x[0], "path": "../escape"}],
                           lambda x: [{**x[0], "sha256": "0" * 64}],
                           lambda x: [{**x[0], "unknown": True}]):
                with self.subTest(field=name, change=change):
                    record = copy.deepcopy(original)
                    record["inputs"][name] = change(record["inputs"][name])
                    self.rewrite(record)
                    self.reject_verify(reason="identity drift")

    def real_cmake_probe(self):
        cmake, ninja, ctest = (shutil.which(name) for name in ("cmake", "ninja", "ctest"))
        if not all((cmake, ninja, ctest)):
            self.skipTest("real compiler-free probe requires CMake, Ninja and CTest")
        (self.source / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.20)
project(GateControlProbe LANGUAGES NONE)
enable_testing()
include(CPack)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/source.c" DESTINATION share/gate-probe)
add_custom_command(OUTPUT probe.stamp
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/probe.stamp"
  VERBATIM)
add_custom_target(probe DEPENDS probe.stamp)
add_subdirectory(nested)
''', encoding="utf-8")
        nested = self.source / "nested"
        nested.mkdir()
        (nested / "CMakeLists.txt").write_text('''file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/leaf control" "message(STATUS original)\\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/test script"
  "include(\\"${CMAKE_CURRENT_BINARY_DIR}/leaf control\\")\\n")
add_test(NAME script-probe COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/test script")
''', encoding="utf-8")
        build = self.root / "real CMake build"
        command = [cmake, "-S", str(self.source), "-B", str(build), "-G", "Ninja",
                   "-DCMAKE_BUILD_TYPE:STRING=Release", "-DVACARDS_NESTING_RUST_TARGET:STRING=fixture-only",
                   "-DCMAKE_MAKE_PROGRAM:FILEPATH=" + ninja]
        command += [f"-D{key}:FILEPATH={path.as_posix()}" for key, path in self.compilers.items()]
        result = subprocess.run(command, cwd=self.root, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((build / ".ninja_log").exists())
        self.assertFalse(list(build.glob("CMakeFiles/**/CMakeCCompiler.cmake")))
        self.assertFalse(list(build.glob("CMakeFiles/**/CMakeCXXCompiler.cmake")))
        self.assertIn("${CPACK_PROPERTIES_FILE}", (build / "CPackConfig.cmake").read_text(encoding="utf-8"))
        return build, cmake, ninja, ctest

    def test_real_cmake_ninja_and_ctest_control_tamper(self):
        build, cmake, ninja, ctest = self.real_cmake_probe()
        self.claim(build=build)
        for command in ([cmake, "--build", str(build), "--target", "probe"],
                        [ctest, "--test-dir", str(build), "--output-on-failure"]):
            result = subprocess.run(command, cwd=self.root, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertTrue((build / "probe.stamp").exists())
        result = self.cli("verify", "--build", build)
        self.assertEqual(result.returncode, 0, result.stderr)
        mutations = {
            "CMakeFiles/rules.ninja": lambda s: s.replace("command = $COMMAND", "command = " + cmake + " -E false"),
            "CTestTestfile.cmake": lambda s: s.replace('subdirs("nested")', '# dropped nested tests'),
            "nested/CTestTestfile.cmake": lambda s: s.replace("script-probe", "renamed-probe"),
            "nested/test script": lambda s: 'message(FATAL_ERROR tampered)\n' + s,
            "nested/leaf control": lambda s: s.replace("STATUS original", "FATAL_ERROR tampered"),
            "cmake_install.cmake": lambda s: s + "\nmessage(FATAL_ERROR tampered)\n",
            "CPackConfig.cmake": lambda s: s + "\n# configuration tampered\n",
        }
        for name, change in mutations.items():
            with self.subTest(control=name):
                path = build / name
                original = path.read_bytes()
                altered = change(original.decode()).encode()
                self.assertNotEqual(altered, original)
                path.write_bytes(altered)
                if name == "CMakeFiles/rules.ninja":
                    # Ninja's read-only command inventory proves these bytes
                    # control execution, without executing the failing command.
                    result = subprocess.run([ninja, "-C", str(build), "-t", "commands", "probe"],
                                            capture_output=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn(b"-E false", result.stdout)
                if name == "CTestTestfile.cmake":
                    result = subprocess.run([ctest, "--test-dir", str(build), "--show-only=json-v1"],
                                            capture_output=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(json.loads(result.stdout)["tests"], [])
                self.reject_verify(build, reason="identity drift")
                path.write_bytes(original)
        self.assertEqual(self.cli("verify", "--build", build).returncode, 0)

    def test_symlinked_inputs_and_dangling_outputs_rejected(self):
        for name in ("CMakeCache.txt", "build.ninja", GATE.FEATURES):
            with self.subTest(name=name):
                path = self.build / name
                original = path.read_bytes()
                external = self.root / name
                external.write_bytes(original)
                path.unlink()
                self.link(path, external)
                self.reject_claim()
                self.assertEqual(external.read_bytes(), original)
                path.unlink()
                path.write_bytes(original)
        for name in (GATE.LEDGER, GATE.EVIDENCE, ".ninja_log"):
            with self.subTest(name=name):
                path = self.build / name
                self.link(path, self.root / "does not exist")
                self.reject_claim()
                path.unlink()

    def test_symlinked_source_build_install_parent_rejected(self):
        source_alias, build_alias = self.root / "source alias", self.root / "build alias"
        self.link(source_alias, self.source, directory=True)
        self.link(build_alias, self.build, directory=True)
        self.reject_claim(source=source_alias)
        self.reject_claim(build=build_alias)
        parent = self.root / "install alias"
        self.link(parent, self.install_parent, directory=True)
        self.reject_claim(mode="full", install=parent / "prefix")
        self.assertEqual(list(self.install_parent.iterdir()), [])

    def test_compiler_symlink_identity_changes_even_with_same_bytes(self):
        compiler = self.compilers["CMAKE_C_COMPILER"]
        original = compiler.read_bytes()
        target = self.tools / "compiler real.exe"
        target.write_bytes(original)
        target.chmod(0o755)
        compiler.unlink()
        self.link(compiler, target)
        self.claim()
        other = self.tools / "compiler another.exe"
        other.write_bytes(original)
        other.chmod(0o755)
        compiler.unlink()
        self.link(compiler, other)
        self.reject_verify(reason="identity drift")

    def test_output_replacement_and_symlink_escape_rejected(self):
        self.claim(mode="full", install=self.install)
        for directory in (self.build / GATE.EVIDENCE, self.install):
            with self.subTest(directory=directory):
                original = directory.with_name(directory.name + " preserved")
                directory.rename(original)
                try:
                    directory.mkdir()
                    self.reject_verify(reason="reserved outputs changed")
                    directory.rmdir()
                    self.link(directory, original, directory=True)
                    self.reject_verify()
                finally:
                    # A missing native symlink privilege skips this subcase;
                    # restore its owned output before checking the next one.
                    if directory.is_symlink():
                        directory.unlink()
                    elif directory.exists():
                        directory.rmdir()
                    original.rename(directory)

    def test_ledger_strict_missing_unknown_duplicate_and_types(self):
        original = self.claim()
        for key in original:
            with self.subTest(missing=key):
                record = copy.deepcopy(original)
                del record[key]
                self.rewrite(record, checksum=False)
                self.reject_verify(reason="invalid ledger fields")
        for location in ((), ("inputs",), ("inputs", "source"), ("outputs", "evidence")):
            with self.subTest(unknown=location):
                record = copy.deepcopy(original)
                obj = record
                for key in location:
                    obj = obj[key]
                obj["unknown"] = "unexpected"
                self.rewrite(record)
                self.reject_verify()
        for location in (("inputs",), ("inputs", "source"), ("outputs",),
                         ("outputs", "evidence"), ("inputs", "executables", "CMAKE_C_COMPILER")):
            baseline = original
            for key in location:
                baseline = baseline[key]
            for field in baseline:
                with self.subTest(missing_nested=(location, field)):
                    record = copy.deepcopy(original)
                    obj = record
                    for key in location:
                        obj = obj[key]
                    del obj[field]
                    self.rewrite(record)
                    self.reject_verify()
        for field, value in (("schema_version", True), ("run_uuid", "not-a-uuid"),
                             ("created_utc", "tomorrow"), ("purpose", "success-attestation"),
                             ("configuration_sha256", "0" * 64), ("inputs", [])):
            record = copy.deepcopy(original)
            record[field] = value
            self.rewrite(record)
            self.reject_verify()
        for text in (GATE.canonical_json(original).replace(b'"schema_version":1', b'"schema_version":1,"schema_version":1'),
                     GATE.canonical_json(original).replace(b'"mode":"full"', b'"mode":"full","mode":"full"')
                     .replace(b'"mode":"quick"', b'"mode":"quick","mode":"quick"')):
            self.ledger.write_bytes(text)
            self.reject_verify(reason="duplicate JSON field")

    def test_recorded_mode_install_and_output_path_mismatches(self):
        original = self.claim()
        for mode, install in (("full", None), ("quick", str(self.install)), ("dev", None)):
            record = copy.deepcopy(original)
            record["inputs"]["mode"], record["inputs"]["install"] = mode, install
            self.rewrite(record)
            self.reject_verify()
        record = copy.deepcopy(original)
        record["outputs"]["evidence"]["path"] = str(self.root / "outside")
        self.rewrite(record)
        self.reject_verify(reason="reserved outputs changed")

    def test_submodule_checkout_and_dirty_bytes_are_bound(self):
        dependency = self.root / "dependency"
        dependency.mkdir()
        self.git("init", "-q", source=dependency)
        (dependency / "data.txt").write_bytes(b"dependency")
        self.git("add", ".", source=dependency)
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                 "commit", "-q", "-m", "Fixture dependency", source=dependency)
        self.git("-c", "protocol.file.allow=always", "submodule", "add", "-q", str(dependency), "dependency")
        self.git("commit", "-q", "-am", "Fixture submodule")
        record = self.claim()
        self.assertEqual(len(record["inputs"]["source"]["submodules"]), 1)
        (self.source / "dependency/data.txt").write_bytes(b"tampered dependency")
        self.reject_verify(reason="identity drift")

    def test_concurrent_claims_have_exactly_one_winner(self):
        children = [self.start_child("race") for _ in range(6)]
        for process in children:
            self.ready(process)
        for process in children:
            process.stdin.write(b"go\n")
            process.stdin.flush()
        results = []
        for process in children:
            output, error = process.communicate(timeout=20)
            results.append(process.returncode)
            self.assertIn(process.returncode, (0, 1), (output, error))
        self.assertEqual(results.count(0), 1, results)
        self.assertEqual(self.cli("verify", "--build", self.build).returncode, 0)

    def test_competing_full_builds_cannot_share_install_prefix(self):
        other = self.new_build("other full build")
        children = [self.start_child("race", build=build, mode="full", install=self.install)
                    for build in (self.build, other)]
        for process in children:
            self.ready(process)
        for process in children:
            process.stdin.write(b"go\n")
            process.stdin.flush()
        for process in children:
            process.communicate(timeout=20)
        self.assertEqual([p.returncode for p in children].count(0), 1)
        self.assertTrue(self.ledger.exists())
        self.assertTrue((other / GATE.LEDGER).exists(), "losing reservation must remain consumed")

    def test_terminated_reservations_are_consumed_and_preserve_partial_bytes(self):
        for phase in ("before-evidence", "after-evidence", "after-install", "partial-ledger"):
            with self.subTest(phase=phase):
                build = self.new_build("interrupted " + phase)
                install = self.install_parent / phase
                process = self.start_child(phase, build=build, mode="full", install=install)
                self.ready(process)
                process.terminate()
                process.communicate(timeout=10)
                ledger = build / GATE.LEDGER
                self.assertTrue(ledger.exists())
                before = ledger.read_bytes()
                if phase == "partial-ledger":
                    self.assertEqual(len(before), 41)
                self.reject_verify(build)
                self.reject_claim(build=build, mode="full", install=install)
                self.assertEqual(ledger.read_bytes(), before)

    def test_mkdir_and_write_failures_consume_run_without_cleanup(self):
        for phase in ("evidence", "install", "write"):
            with self.subTest(phase=phase):
                build = self.new_build("I O failure " + phase)
                prefix = self.install_parent / phase
                original = GATE.Directory.mkdir
                def mkdir(directory, name):
                    if (phase == "evidence" and name == GATE.EVIDENCE) or (phase == "install" and name == phase):
                        raise PermissionError("injected output failure")
                    return original(directory, name)
                fault = (mock.patch.object(GATE.Directory, "readonly", side_effect=OSError("injected seal failure"))
                         if phase == "write" else nullcontext())
                with mock.patch.object(GATE.Directory, "mkdir", mkdir), fault:
                    with self.assertRaises(OSError):
                        GATE.claim(str(self.source), str(build), "full", str(prefix))
                self.assertTrue((build / GATE.LEDGER).exists())
                self.reject_claim(build=build, mode="full", install=prefix)

    def test_configuration_race_retains_consumed_reservation(self):
        original = GATE.Directory.mkdir
        def mkdir(directory, name):
            result = original(directory, name)
            if name == GATE.EVIDENCE:
                with (self.build / "CMakeCache.txt").open("ab") as stream:
                    stream.write(b"# concurrent reconfiguration\n")
            return result
        with mock.patch.object(GATE.Directory, "mkdir", mkdir):
            with self.assertRaisesRegex(GATE.GateError, "inputs changed during reservation"):
                GATE.claim(str(self.source), str(self.build), "quick")
        self.assertEqual(self.ledger.read_bytes(), b"")
        self.reject_verify()
        self.reject_claim()

    def test_ninja_attempt_race_blocks_claim(self):
        original = GATE.Directory.mkdir
        def mkdir(directory, name):
            result = original(directory, name)
            (self.build / ".ninja_log").write_bytes(b"concurrent build attempt")
            return result
        with mock.patch.object(GATE.Directory, "mkdir", mkdir):
            with self.assertRaisesRegex(GATE.GateError, "Ninja log appeared during claim"):
                GATE.claim(str(self.source), str(self.build), "quick")
        self.assertEqual(self.ledger.read_bytes(), b"")
        self.assertEqual((self.build / ".ninja_log").read_bytes(), b"concurrent build attempt")

    def test_existing_output_appearing_after_preflight_is_not_overwritten(self):
        original = GATE.Directory.open_file
        def open_file(directory, name, *, create=False):
            if create:
                evidence = self.build / GATE.EVIDENCE
                evidence.mkdir()
                (evidence / "failure.log").write_bytes(b"another producer's output")
            return original(directory, name, create=create)
        with mock.patch.object(GATE.Directory, "open_file", open_file):
            with self.assertRaises(OSError):
                GATE.claim(str(self.source), str(self.build), "quick")
        self.assertEqual((self.build / GATE.EVIDENCE / "failure.log").read_bytes(), b"another producer's output")
        self.assertEqual(self.ledger.read_bytes(), b"")

    def test_reserved_output_replacement_during_claim_fails(self):
        original = GATE.observe
        calls = 0
        def observe(*args):
            nonlocal calls
            calls += 1
            if calls == 2:
                evidence = self.build / GATE.EVIDENCE
                evidence.rename(self.build / "preserved evidence")
                evidence.mkdir()
            return original(*args)
        with mock.patch.object(GATE, "observe", observe):
            with self.assertRaisesRegex(GATE.GateError, "output replaced during claim"):
                GATE.claim(str(self.source), str(self.build), "quick")
        self.assertEqual(self.ledger.read_bytes(), b"")

    def test_nonregular_input_never_blocks_or_creates_output(self):
        (self.build / GATE.FEATURES).unlink()
        (self.build / GATE.FEATURES).mkdir()
        self.reject_claim()
        (self.build / GATE.FEATURES).rmdir()
        if hasattr(os, "mkfifo"):
            os.mkfifo(self.build / GATE.FEATURES)
            self.reject_claim()

    def test_wrong_source_root_and_missing_cache_create_nothing(self):
        subdir = self.source / "subdir"
        subdir.mkdir()
        self.reject_claim(source=subdir)
        (self.build / "CMakeCache.txt").unlink()
        self.reject_claim()

    def test_build_path_swap_cannot_redirect_exclusive_write(self):
        outside = self.root / "outside"
        outside.mkdir()
        moved = self.root / "moved original build"
        with GATE.Directory(self.build) as directory:
            self.build.rename(moved)
            self.link(self.build, outside, directory=True)
            # Simulate the narrow interval AFTER check() but BEFORE open().
            with mock.patch.object(directory, "check"):
                fd = directory.open_file(GATE.LEDGER, create=True)
                os.write(fd, b"retained in original directory")
                os.close(fd)
            with self.assertRaises((GATE.GateError, OSError)):
                directory.check()
        self.assertFalse((outside / GATE.LEDGER).exists())
        self.assertEqual((moved / GATE.LEDGER).read_bytes(), b"retained in original directory")

    def test_windows_path_rules_are_tested_on_every_host(self):
        for name in ("..", "C:stream", "file:stream", "CON", "LPT1", "NUL.txt", "tail.", "tail ", "x/y", "x\\y"):
            with self.subTest(name=name), self.assertRaises(GATE.GateError):
                GATE.windows_name(name)
        GATE.windows_name("evidence á with spaces")

    @unittest.skipUnless(os.name == "nt", "native Windows API execution requires a Windows runner")
    def test_native_windows_handles_and_readonly_ledger(self):
        with GATE.Directory(self.build) as directory:
            self.assertIsInstance(directory.windows, GATE.WindowsFiles)
        self.claim(mode="full", install=self.install)
        self.assertTrue(self.ledger.stat().st_file_attributes & stat.FILE_ATTRIBUTE_READONLY)
        self.assertEqual(self.cli("verify", "--build", self.build).returncode, 0)


def ctest_methods():
    return unittest.defaultTestLoader.getTestCaseNames(GateRunTest)


def check_ctest_inventory(inventory):
    """Fail on stale, missing, duplicated or miswired method registrations."""
    prefix = "vacards-gate-run-test."
    methods = ctest_methods()
    expected = {prefix + method: method for method in methods}
    expected[prefix + "inventory"] = None
    actual = [test for test in inventory["tests"] if test["name"].startswith(prefix)]
    names = [test["name"] for test in actual]
    if not methods or len(names) != len(set(names)) or set(names) != set(expected):
        raise AssertionError(f"Gate CTest coverage mismatch: expected {sorted(expected)}, got {names}")
    if any(test["name"] == "vacards-gate-run-test" for test in inventory["tests"]):
        raise AssertionError("Stale monolithic gate registration remains")
    for test in actual:
        properties = {p["name"]: p["value"] for p in test["properties"]}
        if (properties.get("TIMEOUT") != 180 or not properties.get("RUN_SERIAL")
                or "vacards-critical" not in properties.get("LABELS", [])):
            raise AssertionError(f"Gate CTest bounds/critical label mismatch: {test['name']}")
        method = expected[test["name"]]
        command = test["command"]
        if Path(command[1]).resolve() != Path(__file__).resolve():
            raise AssertionError(f"Wrong fixture: {test['name']}")
        if method is not None and command[2:] != ["GateRunTest." + method, "-v"]:
            raise AssertionError(f"Wrong method selection: {test['name']}")
    return len(methods)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--child":
        sys.exit(child())
    if sys.argv[1:] == ["--list-ctest-methods"]:
        print("\n".join(ctest_methods()))
        sys.exit(0)
    if len(sys.argv) == 4 and sys.argv[1] == "--check-ctest-inventory":
        inventory = subprocess.run([sys.argv[2], "--test-dir", sys.argv[3], "--show-only=json-v1"],
                                   check=True, stdout=subprocess.PIPE, timeout=30)
        count = check_ctest_inventory(json.loads(inventory.stdout))
        print(f"PASS: all {count} gate methods registered exactly once with original bounds and critical labels")
        sys.exit(0)
    unittest.main()
