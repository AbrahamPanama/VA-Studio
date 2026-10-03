#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise real shell/CMake policy with synthetic UCRT64 tool responses.

This deliberately does not establish PE linkability, loaded DLL identity, or
execution of the C clipping regression. Those require the actual Windows lane.
"""
import hashlib
import io
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
DLLS = ("libcairo-2.dll", "libcairo-gobject-2.dll", "libcairo-script-interpreter-2.dll")
MODULES = ("cairo", "cairo-gobject", "cairo-script-interpreter")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(args, env, cwd):
    if os.name == "nt" and Path(str(args[0])).stem.lower() == "bash" and env.get("CAIRO_TEST_REAL_CYGPATH"):
        # Bash policies prepend colon-separated POSIX paths. Convert only at
        # this boundary; native CMake retains its native path-list environment.
        env = dict(env)
        for key in ("PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR"):
            if env.get(key):
                converted = subprocess.run([env["CAIRO_TEST_REAL_CYGPATH"], "-u", "-p", env[key]],
                                           env=dict(env, LC_ALL="C.UTF-8"), check=True,
                                           stdout=subprocess.PIPE, text=True, encoding="utf-8")
                env[key] = converted.stdout.strip()
    return subprocess.run([str(a) for a in args], cwd=cwd, env=env,
                          text=True, encoding="utf-8", stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


# Each fake executable is a private fixture in the test's disposable /ucrt64.
# Real bash, patch, tar, shasum, pkg-config and CMake run the policies under test.
MOCK = r'''#!PYTHON
import json, os, pathlib, subprocess, sys
sys.stdout.reconfigure(encoding="utf-8")
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
base = pathlib.Path(os.environ["CAIRO_TEST_ROOT"])
if name == "uname":
    print("x86_64" if args == ["-m"] else "MINGW64_NT-10.0-22631")
elif name == "cygpath":
    value = args[-1]
    if value == "/ucrt64": value = str(base / "ucrt64")
    if os.name == "nt":
        # Only the synthetic UCRT prefix is virtual; all other paths retain
        # the real MSYS mount/drive/Unicode conversion semantics.
        result = subprocess.run([os.environ["CAIRO_TEST_REAL_CYGPATH"], *args[:-1], value],
                                stdout=subprocess.PIPE, check=True)
        sys.stdout.buffer.write(result.stdout)
        sys.exit(0)
    if value.startswith("C:\\"): value = value[2:].replace("\\", "/")
    print("C:" + value.replace("/", "\\") if args[0] == "-w" else value)
elif name in ("gcc", "g++"):
    if args == ["-dumpmachine"]:
        print(os.environ.get("CAIRO_TEST_TRIPLE", "x86_64-w64-mingw32"))
    elif args == ["--version"]: print("synthetic UCRT64 GCC (policy test, no compilation)")
    else:
        out = pathlib.Path(args[args.index("-o") + 1])
        out.write_text("#!/bin/sh\necho 'Synthetic clipping runner; no Windows execution'\nexit " + os.environ.get("CAIRO_TEST_CLIP_EXIT", "0") + "\n", encoding="utf-8", newline="\n")
        out.chmod(0o755)
elif name == "objdump":
    if args == ["--version"]: print("synthetic GNU objdump")
    else:
        data = pathlib.Path(args[-1]).read_bytes()
        if args[0] == "-p":
            if data.startswith(b"SYNTHETIC_PE64_EXE"):
                print("Characteristics 0x22\n\texecutable\n\tlarge address aware")
            elif data.startswith((b"SYNTHETIC_PE64", b"SYNTHETIC_PE32")):
                print("Characteristics 0x2022\n\texecutable\n\tlarge address aware\n\tDLL")
            else: sys.exit(1)
        elif data.startswith(b"SYNTHETIC_PE64"):
            print("fixture: file format pei-x86-64\narchitecture: i386:x86-64, flags 0x0:")
        elif data.startswith(b"SYNTHETIC_PE32"):
            print("fixture: file format pei-i386\narchitecture: i386, flags 0x0:")
        else: sys.exit(1)
elif name == "meson":
    if args == ["--version"]: print("synthetic Meson"); sys.exit(0)
    if os.environ.get("CAIRO_TEST_MESON_FAIL") == args[0]: sys.exit(9)
    if args[0] == "setup":
        assert "--default-library" in args and "shared" in args
        assert "--wrap-mode" in args and "nodownload" in args
        build = pathlib.Path(args[1]); build.mkdir()
        (build / "fixture.json").write_text(json.dumps({"prefix": args[args.index("--prefix") + 1]}), encoding="utf-8", newline="\n")
    else:
        build = pathlib.Path(args[args.index("-C") + 1])
        if args[0] == "compile":
            assert args[args.index("-j") + 1] == "2"
        elif args[0] == "install":
            assert "--no-rebuild" in args
            prefix = pathlib.Path(json.loads((build / "fixture.json").read_text(encoding="utf-8"))["prefix"])
            for directory in ("bin", "lib/pkgconfig", "include/cairo"):
                (prefix / directory).mkdir(parents=True, exist_ok=True)
            for module in ("cairo", "cairo-gobject", "cairo-script-interpreter"):
                (prefix / "bin" / ("lib" + module + "-2.dll")).write_bytes(b"SYNTHETIC_PE64:" + module.encode())
                (prefix / "lib" / ("lib" + module + ".dll.a")).write_text("synthetic import " + module, encoding="utf-8", newline="\n")
                (prefix / "lib/pkgconfig" / (module + ".pc")).write_text(
                    "prefix=" + prefix.as_posix() + "\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\n"
                    "Name: " + module + "\nDescription: synthetic policy fixture\nVersion: 1.18.4\n"
                    "Libs: -L${libdir} -l" + module + "\nCflags: -I${includedir}/cairo\n", encoding="utf-8", newline="\n")
            for header in ("cairo.h", "cairo-version.h"):
                (prefix / "include/cairo" / header).write_text("/* synthetic header */\n", encoding="utf-8", newline="\n")
else: sys.exit(99)
'''


class WindowsCairoPolicy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("bash"):
            raise RuntimeError("missing policy-test tool: bash")
        # These tools run in Bash. Native Windows PATHEXT lookup cannot find
        # extensionless Perl tools such as shasum even when they are on PATH.
        tools = ("pkg-config", "cmake", "ninja", "patch", "shasum", "curl")
        check = run(["bash", "-c", 'for tool; do command -v "$tool" >/dev/null || '
                     '{ echo "missing policy-test tool: $tool"; exit 1; }; done',
                     "policy-prerequisites", *tools], os.environ, ROOT)
        if check.returncode:
            raise RuntimeError(check.stdout)
        cls.temporary = tempfile.TemporaryDirectory(prefix="vacards-cairo-policy-", dir=os.environ.get("TMPDIR"))
        cls.base = Path(cls.temporary.name).resolve()
        cls.repo = cls.base / "source space ü"
        for relative in ("VACARDS-DEPENDENCIES.env", "CMakeLists.txt",
                         "CMakeScripts/DefineDependsandFlags.cmake",
                         "packaging/macos/vacards/vacards-dependencies.sh",
                         "packaging/macos/vacards/cairo-1.18.4-clip-all.patch",
                         "packaging/windows/vacards/build-patched-cairo.sh",
                         "packaging/windows/vacards/verify-vacards-cairo-prefix.sh",
                         "testfiles/vacards-windows-cairo-test.c"):
            destination = cls.repo / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        # Newer revisions retain the macOS entry point as a thin wrapper.
        # Include its canonical parser when testing such a source revision.
        shared = ROOT / "packaging/vacards/vacards-dependencies.sh"
        if shared.is_file():
            destination = cls.repo / "packaging/vacards/vacards-dependencies.sh"
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(shared, destination)
        cls.builder = cls.repo / "packaging/windows/vacards/build-patched-cairo.sh"
        cls.verifier = cls.repo / "packaging/windows/vacards/verify-vacards-cairo-prefix.sh"
        cls.common_patch = cls.repo / "packaging/macos/vacards/cairo-1.18.4-clip-all.patch"
        cls.patch_bytes = cls.common_patch.read_bytes()
        cls.archive = cls.base / "synthetic-source.tar.xz"
        old_lines = [line[1:] for line in cls.patch_bytes.decode().splitlines()
                     if line.startswith("-") and not line.startswith("---")]
        content = ("/* synthetic padding */\n" * 339 + "\n".join(old_lines) + "\n").encode()
        with tarfile.open(cls.archive, "w:xz") as archive:
            member = tarfile.TarInfo("cairo-1.18.4/src/cairo-clip.c")
            member.size = len(content)
            archive.addfile(member, io.BytesIO(content))
        manifest = cls.repo / "VACARDS-DEPENDENCIES.env"
        manifest.write_text("\n".join(
            "cairo_release_sha256=" + digest(cls.archive) if line.startswith("cairo_release_sha256=") else line
            for line in manifest.read_text(encoding="utf-8").splitlines()) + "\n", encoding="utf-8", newline="\n")
        tools = cls.base / "ucrt64/bin"
        tools.mkdir(parents=True)
        for name in ("uname", "cygpath", "gcc", "g++", "objdump", "meson"):
            executable = tools / name
            if os.name == "nt" and name == "cygpath":
                converter = shutil.which("cygpath")
                if not converter:
                    raise RuntimeError("missing policy-test tool: cygpath")
                real = shlex.quote(Path(converter).as_posix())
                virtual = shlex.quote((cls.base / "ucrt64").as_posix())
                executable.write_text("#!/bin/sh\nexport LC_ALL=C.UTF-8\n"
                                      'if [ "$#" -eq 2 ] && [ "$2" = /ucrt64 ]; then\n'
                                      f'  exec {real} "$1" {virtual}\nfi\n'
                                      f'exec {real} "$@"\n', encoding="utf-8", newline="\n")
                executable.chmod(0o755)
                continue
            # A single-file shell/Python trampoline works with spaces and with
            # native Windows Python; Bash cannot use a C:\\... Python shebang.
            interpreter = shlex.quote(Path(sys.executable).as_posix())
            script = shlex.quote(executable.as_posix())
            command = f"{interpreter} {script}"
            if os.name == "nt":
                # Policy uses LC_ALL=C for bytewise checks. The synthetic
                # native-Python boundary must decode MSYS UTF-8 arguments,
                # rather than interpreting those bytes with the ANSI codepage.
                command = "env LC_ALL=C.UTF-8 " + command
            trampoline = "#!/bin/sh\n'''exec' " + command + ' "$@"\n\' \'\'\''
            executable.write_text(MOCK.replace("#!PYTHON", trampoline), encoding="utf-8", newline="\n")
            executable.chmod(0o755)
        cls.packages = cls.base / "packages"
        cls.packages.mkdir()
        for module in ("pixman-1", "fontconfig", "freetype2", "glib-2.0", "gobject-2.0", "libpng", "zlib"):
            (cls.packages / (module + ".pc")).write_text(
                "Name: " + module + "\nDescription: synthetic dependency\nVersion: 1.0\n", encoding="utf-8", newline="\n")
        cls.env = dict(os.environ, MSYSTEM="UCRT64", CAIRO_TEST_ROOT=str(cls.base),
                       PATH=str(tools) + os.pathsep + os.environ["PATH"],
                       PKG_CONFIG_PATH=str(cls.packages), PKG_CONFIG_LIBDIR=str(cls.packages), DESTDIR="")
        if os.name == "nt":
            cls.env["CAIRO_TEST_REAL_CYGPATH"] = shutil.which("cygpath") or ""
            if not cls.env["CAIRO_TEST_REAL_CYGPATH"]:
                raise RuntimeError("missing policy-test tool: cygpath")
        cls.env.pop("PKG_CONFIG_SYSROOT_DIR", None)
        cls.env.pop("PKG_CONFIG", None)
        cls.output = cls.base / "build space ü"
        result = run(["bash", cls.builder, cls.output, "--archive", cls.archive], cls.env, cls.repo)
        if result.returncode:
            raise RuntimeError("synthetic builder positive control failed:\n" + result.stdout)
        cls.prefix = cls.output / "install"
        cls.seed = cls.base / "seed"
        shutil.copytree(cls.prefix, cls.seed)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        shutil.rmtree(self.prefix)
        shutil.copytree(self.seed, self.prefix)
        self.common_patch.write_bytes(self.patch_bytes)
        self.test_env = dict(self.env, PKG_CONFIG_PATH=str(self.prefix / "lib/pkgconfig") + os.pathsep + str(self.packages))
        self.marker = self.prefix / "VACARDS-CAIRO.env"
        self.inventory = self.prefix / "VACARDS-CAIRO.sha256"

    def verify(self, *args, env=None, good=True, reason=None):
        result = run(["bash", self.verifier, self.prefix, *args], env or self.test_env, self.repo)
        if good:
            self.assertEqual(result.returncode, 0, result.stdout)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertNotIn("Verified VACards Windows Cairo", result.stdout)
            if reason: self.assertIn(reason, result.stdout)
        return result

    def marker_set(self, key, value):
        self.marker.write_text("\n".join(key + "=" + value if line.startswith(key + "=") else line
                                         for line in self.marker.read_text(encoding="utf-8").splitlines()) + "\n", encoding="utf-8", newline="\n")

    def refresh_inventory(self):
        paths = [line.split("  ", 1)[1] for line in self.inventory.read_text(encoding="utf-8").splitlines()]
        self.inventory.write_text("".join(digest(self.prefix / path) + "  " + path + "\n" for path in paths), encoding="utf-8", newline="\n")
        self.marker_set("inventory_sha256", digest(self.inventory))

    def test_valid_prefix_and_read_only_verification(self):
        before = {str(p.relative_to(self.prefix)): digest(p) for p in self.prefix.rglob("*") if p.is_file()}
        self.verify()
        after = {str(p.relative_to(self.prefix)): digest(p) for p in self.prefix.rglob("*") if p.is_file()}
        self.assertEqual(before, after)

    def test_stock_same_version_rejected(self):
        self.marker.unlink()
        self.verify(good=False, reason="provenance")

    def test_dll_mutation_rejected(self):
        with (self.prefix / "bin" / DLLS[0]).open("ab") as stream: stream.write(b"tampered")
        self.verify(good=False, reason="checksum changed")

    def test_wrong_pe_arch_even_with_matching_hash(self):
        (self.prefix / "bin" / DLLS[1]).write_bytes(b"SYNTHETIC_PE32")
        self.refresh_inventory()
        self.verify(good=False, reason="PE x86-64")

    def test_executable_renamed_dll_rejected(self):
        (self.prefix / "bin" / DLLS[0]).write_bytes(b"SYNTHETIC_PE64_EXE")
        self.refresh_inventory()
        self.verify(good=False, reason="PE file is not a DLL")

    def test_unknown_dll_rejected(self):
        (self.prefix / "bin/libcairo-surprise.dll").write_bytes(b"SYNTHETIC_PE64")
        self.verify(good=False, reason="DLL inventory")

    def test_missing_dll_rejected(self):
        (self.prefix / "bin" / DLLS[2]).unlink()
        self.verify(good=False, reason="missing regular file")

    def test_archive_bytes_not_just_label(self):
        (self.prefix / "share/vacards-cairo/source.tar.xz").write_bytes(b"replacement archive")
        self.refresh_inventory()
        self.verify(good=False, reason="source archive bytes")

    def test_copied_patch_bytes_rejected(self):
        with (self.prefix / "share/vacards-cairo/clip-all.patch").open("ab") as stream: stream.write(b"\nchanged")
        self.refresh_inventory()
        self.verify(good=False, reason="actual patch bytes")

    def test_current_common_patch_changed_same_fix_label(self):
        self.common_patch.write_bytes(self.patch_bytes + b"\nchanged shared patch")
        self.verify(good=False, reason="patch_sha256")

    def test_import_library_mutation_rejected(self):
        (self.prefix / "lib/libcairo.dll.a").write_bytes(b"wrong import")
        self.verify(good=False, reason="checksum changed")

    def test_changed_toolchain_ledger_rejected(self):
        (self.prefix / "share/vacards-cairo/toolchain.txt").write_text("other compiler", encoding="utf-8", newline="\n")
        self.verify(good=False, reason="checksum changed")

    def test_duplicate_and_unknown_marker_fields_rejected(self):
        original = self.marker.read_text(encoding="utf-8")
        for extra in ("format=1\n", "unknown_key=value\n"):
            with self.subTest(extra=extra):
                self.marker.write_text(original + extra, encoding="utf-8", newline="\n")
                self.verify(good=False, reason="invalid provenance keys")

    def test_unsafe_inventory_path_rejected(self):
        self.inventory.write_text("0" * 64 + "  ../outside.dll\n", encoding="utf-8", newline="\n")
        self.marker_set("inventory_sha256", digest(self.inventory))
        self.verify(good=False, reason="unsafe/unexpected")

    def test_unrecorded_header_rejected(self):
        (self.prefix / "include/cairo/unrecorded.h").write_text("unexpected header", encoding="utf-8", newline="\n")
        self.verify(good=False, reason="unrecorded header")

    def test_symlinked_dll_rejected(self):
        dll = self.prefix / "bin" / DLLS[0]
        dll.unlink()
        target = self.seed / "bin" / DLLS[0]
        spelling = str(target)
        dll.symlink_to(spelling.replace("/", "\\") if os.name == "nt" else spelling)
        self.assertTrue(dll.samefile(target))
        self.assertEqual(dll.read_bytes(), target.read_bytes())
        self.verify(good=False, reason="missing regular file")

    def test_pkgconfig_search_path_swap_rejected(self):
        other = self.base / "other-pc"
        other.mkdir(exist_ok=True)
        for module in MODULES: shutil.copyfile(self.prefix / "lib/pkgconfig" / (module + ".pc"), other / (module + ".pc"))
        self.verify(env=dict(self.test_env, PKG_CONFIG_PATH=str(other)), good=False, reason="pkg-config file")

    def test_pkgconfig_prefix_spoof_rejected(self):
        pc = self.prefix / "lib/pkgconfig/cairo.pc"
        original = pc.read_text(encoding="utf-8")
        changed = original.replace("prefix=" + self.prefix.as_posix(), "prefix=" + self.base.as_posix())
        self.assertNotEqual(original, changed, "prefix-spoof fixture did not change the selected .pc file")
        pc.write_text(changed, encoding="utf-8", newline="\n")
        self.refresh_inventory()
        env = self.test_env
        if os.name == "nt":
            # Windows pkgconf otherwise replaces the mutated prefix with the
            # .pc file's install root, hiding this intended negative fixture.
            wrapper = self.base / "pkgconf-no-relocation"
            real = shlex.quote(Path(shutil.which("pkg-config")).as_posix())
            wrapper.write_text(f'#!/bin/sh\nexec {real} --dont-define-prefix "$@"\n',
                               encoding="utf-8", newline="\n")
            wrapper.chmod(0o755)
            env = dict(env, PKG_CONFIG=wrapper.as_posix())
            query = run(["bash", wrapper, "--variable=prefix", "cairo"], env, self.repo)
            self.assertEqual(query.returncode, 0, query.stdout)
            self.assertEqual(query.stdout.strip().replace("\\ ", " "), self.base.as_posix())
        self.verify(env=env, good=False, reason="outside the expected prefix")

    def test_saved_gate_rejects_independently_valid_replacement(self):
        saved = self.base / "gate.env"
        shutil.copyfile(self.marker, saved)
        with (self.prefix / "bin" / DLLS[0]).open("ab") as stream: stream.write(b"different valid build")
        self.refresh_inventory()
        self.verify()
        self.verify("--expected-record", saved, good=False, reason="saved gate record")

    def test_runtime_copy_matches_saved_bytes_and_rejects_replacement(self):
        saved = self.base / "runtime-gate.env"
        shutil.copyfile(self.marker, saved)
        with tempfile.TemporaryDirectory(dir=self.base) as directory:
            stage = Path(directory)
            for dll in DLLS: shutil.copyfile(self.prefix / "bin" / dll, stage / dll)
            self.verify("--expected-record", saved, "--runtime-dir", stage)
            self.verify("--runtime-dir", stage, good=False, reason="requires --expected-record")
            with (stage / DLLS[0]).open("ab") as stream: stream.write(b"replacement")
            self.verify("--expected-record", saved, "--runtime-dir", stage, good=False, reason="tested bytes")

    def test_builder_rejects_existing_output(self):
        result = run(["bash", self.builder, self.output, "--archive", self.archive], self.env, self.repo)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must be empty", result.stdout)

    def test_wrong_shell_and_compiler_rejected(self):
        for changes in ({"MSYSTEM": "MINGW64"}, {"CAIRO_TEST_TRIPLE": "i686-w64-mingw32"}):
            result = run(["bash", self.builder, self.base / "never-built", "--archive", self.archive], dict(self.env, **changes), self.repo)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((self.base / "never-built").exists())

    def test_bad_archive_fails_before_configuration(self):
        corrupt = self.base / "bad.tar.xz"; corrupt.write_bytes(b"bad source")
        output = self.base / "bad-archive-build"
        result = run(["bash", self.builder, output, "--archive", corrupt], self.env, self.repo)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source archive checksum", result.stdout)
        self.assertFalse((output / "build").exists())
        self.assertFalse((output / "install/VACARDS-CAIRO.env").exists())

    def test_failed_clip_test_does_not_issue_record(self):
        output = self.base / "failed-clip-build"
        result = run(["bash", self.builder, output, "--archive", self.archive], dict(self.env, CAIRO_TEST_CLIP_EXIT="8"), self.repo)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((output / "install/VACARDS-CAIRO.env").exists())

    def test_failed_meson_does_not_issue_record(self):
        for phase in ("setup", "compile", "install"):
            with self.subTest(phase=phase):
                output = self.base / ("failed-meson-" + phase)
                result = run(["bash", self.builder, output, "--archive", self.archive],
                             dict(self.env, CAIRO_TEST_MESON_FAIL=phase), self.repo)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((output / "install/VACARDS-CAIRO.env").exists())

    def test_cmake_windows_default_verifier_and_import_binding(self):
        root_cmake = (self.repo / "CMakeLists.txt").read_text(encoding="utf-8")
        option_start = root_cmake.index("if(APPLE OR WIN32)")
        option_block = root_cmake[option_start:root_cmake.index("endif()", option_start) + len("endif()")]
        definitions = (self.repo / "CMakeScripts/DefineDependsandFlags.cmake").read_text(encoding="utf-8")
        def block(name):
            return definitions.split("# BEGIN VACARDS WINDOWS CAIRO " + name)[1].split("# END VACARDS WINDOWS CAIRO " + name)[0]
        with tempfile.TemporaryDirectory(dir=self.base) as directory:
            project = Path(directory)
            native_converter = (f'set(VACARDS_CAIRO_CYGPATH_EXECUTABLE "{Path(self.env["CAIRO_TEST_REAL_CYGPATH"]).as_posix()}")'
                                if os.name == "nt" else "")
            cmake = f'''cmake_minimum_required(VERSION 3.24)
project(CairoPolicy NONE)
set(WIN32 TRUE)
set(APPLE FALSE)
set(MINGW TRUE)
set(CMAKE_SIZEOF_VOID_P 8)
{option_block}
if(NOT VACARDS_REQUIRE_PATCHED_CAIRO)
  message(FATAL_ERROR "Windows default did not fail closed")
endif()
set(CMAKE_SOURCE_DIR "{self.repo.as_posix()}")
set(PKG_CONFIG_EXECUTABLE "{Path(shutil.which('pkg-config')).as_posix()}")
find_package(PkgConfig REQUIRED)
pkg_get_variable(VACARDS_RESOLVED_CAIRO_PREFIX cairo prefix)
{native_converter}
{block('PREFIX CHECK')}
add_library(PkgConfig::GTK INTERFACE IMPORTED)
set_property(TARGET PkgConfig::GTK PROPERTY INTERFACE_LINK_LIBRARIES "/stock/lib/libcairo.dll.a;cairo-gobject;-lcairo-script-interpreter;/stock/lib/libpango.dll.a")
set_property(TARGET PkgConfig::GTK PROPERTY INTERFACE_INCLUDE_DIRECTORIES "/stock/include/cairo;/stock/include/gtk-3.0")
{block('LINK CHECK')}
get_target_property(actual PkgConfig::GTK INTERFACE_LINK_LIBRARIES)
set(expected "{self.prefix.as_posix()}/lib/libcairo.dll.a;{self.prefix.as_posix()}/lib/libcairo-gobject.dll.a;{self.prefix.as_posix()}/lib/libcairo-script-interpreter.dll.a;/stock/lib/libpango.dll.a")
if(NOT actual STREQUAL expected)
  message(FATAL_ERROR "Wrong Cairo import binding: ${{actual}}")
endif()
get_target_property(actual_includes PkgConfig::GTK INTERFACE_INCLUDE_DIRECTORIES)
set(expected_includes "{self.prefix.as_posix()}/include/cairo;/stock/include/gtk-3.0")
if(NOT actual_includes STREQUAL expected_includes)
  message(FATAL_ERROR "Wrong Cairo header binding: ${{actual_includes}}")
endif()
'''
            (project / "CMakeLists.txt").write_text(cmake, encoding="utf-8", newline="\n")
            result = run(["cmake", "-S", project, "-B", project / "good"], self.test_env, self.repo)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.marker.unlink()
            result = run(["cmake", "-S", project, "-B", project / "stock"], self.test_env, self.repo)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Windows Cairo provenance verification failed", result.stdout)


if __name__ == "__main__":
    print("SYNTHETIC POLICY TESTS: no Windows compilation, linking or runtime proof", flush=True)
    unittest.main(verbosity=2)
