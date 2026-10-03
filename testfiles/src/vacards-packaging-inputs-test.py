#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic S3 contract tests; no app build, real signing, or package creation.

Run with python3 -X utf8 -B testfiles/src/vacards-packaging-inputs-test.py [-v].
Mach-O inspection/rewrites and schema compilation use explicit synthetic
adapters. Hashing, inventory capture, copies, receipt checks, CLI errors, and
the Cairo builder's shell orchestration execute the production code. An explicit
in-process mock isolates retained packaging integrity checks from the unavailable
production authorization boundary. It is never available to CLI subprocesses;
these tests do not authenticate or qualify a release.
"""

import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "packaging/vacards/packaging-provenance.py"
spec = importlib.util.spec_from_file_location("packaging_provenance", HELPER)
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)

# The canonical internal-test packager is exercised through its pure
# canonicalization helpers; main() still owns all native I/O.
DMG = ROOT / "packaging/macos/vacards/create-internal-test-dmg.py"
dmg_spec = importlib.util.spec_from_file_location("create_internal_test_dmg", DMG)
dmg = importlib.util.module_from_spec(dmg_spec)
dmg_spec.loader.exec_module(dmg)


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value, encoding="utf-8", newline="\n")
    return path


def tool_path(directory, name):
    # These files are inspected, not executed: run_tool supplies their output.
    # Native Windows discovery still needs an executable suffix.
    return directory / (name + (".exe" if os.name == "nt" else ""))


def dyld_path(path):
    # dyld emits slash-rooted paths, even when its synthetic files live on a
    # Windows test host. setUp pins cwd to that same drive; prove the spelling
    # resolves to the very same file rather than mocking the production parser.
    path = Path(path).resolve()
    value = path.as_posix()
    if os.name == "nt":
        value = value[len(path.drive):]
        if Path(value).resolve() != path:
            raise AssertionError(f"dyld fixture path does not round-trip: {path}")
    return value


def symlink(path, target):
    # MSYS2's native Python can use forward slashes in WindowsPath.__str__.
    # Windows stores symlink targets verbatim, so use native separators.
    target = str(target)
    path.symlink_to(target.replace("/", "\\") if os.name == "nt" else target)


def macho(path, links=(), ident=None, payload="tested A", rpaths=()):
    return write(path, json.dumps({"id": str(path) if ident is None else ident,
                                  "links": list(map(str, links)), "payload": payload,
                                  "rpaths": list(map(str, rpaths))}))


class PackagingInputs(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="vacards packaging ñ ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        previous_cwd = Path.cwd()
        os.chdir(self.root)
        self.addCleanup(os.chdir, previous_cwd)
        self.source = self.root / "source with spaces"
        self.tools = self.root / "tools"
        self.deps = self.root / "deps"
        self.cairo = self.deps / "cairo"
        self.libcdr = self.deps / "libcdr"
        self.librevenge = self.deps / "librevenge"
        self.gtk = self.deps / "gtk"
        self.pc_gtk = self.gtk
        self.glib = macho(self.gtk / "lib/libglib-2.0.0.dylib")
        self.ca = macho(self.cairo / "lib/libcairo.2.dylib", [self.glib])
        self.cg = macho(self.cairo / "lib/libcairo-gobject.2.dylib", [self.ca, self.glib])
        self.lr = macho(self.librevenge / "lib/librevenge-0.0.0.dylib", [self.glib])
        self.cd = macho(self.libcdr / "lib/libcdr-0.1.1.dylib", [self.glib, self.lr])
        self.gt = macho(self.gtk / "lib/libgtk-4.1.dylib", [self.cg])
        self.loader = macho(self.gtk / "lib/libpixbufloader_svg.so", [self.glib, self.ca], ident="")
        self.tiff_loader = macho(self.gtk / "lib/libpixbufloader-tiff.so", [self.glib], ident="")
        self.extra_loaders = []
        self.runtime = [self.ca, self.cg, self.cd, self.lr, self.gt, self.glib]
        self.exe = macho(self.root / "build/bin/inkscape", self.runtime, ident="")
        self.schema_source = self.gtk / p.SCHEMA_DEST
        write(self.schema_source / "org.gtk.gtk4.Settings.FileChooser.gschema.xml", "<schemalist/>")
        write(self.schema_source / "org.gtk.gtk4.Settings.Debug.gschema.xml", "<schemalist/>")
        self.patch_file = self.source / "packaging/macos/vacards/cairo-1.18.4-clip-all.patch"
        write(self.patch_file, "approved patch A\n")
        write(self.cairo / "VACARDS-CAIRO.txt",
              "Cairo release: 1.18.4\nUpstream fix: unchanged\n"
              f"Patch SHA-256: {p.digest(self.patch_file)}\n"
              f"Library SHA-256: {p.digest(self.ca)}\n"
              f"GObject Library SHA-256: {p.digest(self.cg)}\n")
        write(self.libcdr / "VACARDS-LIBCDR.env", f"format=1\nlibrary_sha256={p.digest(self.cd)}\n")
        write(self.librevenge / "VACARDS-LIBREVENGE.env",
              f"format=1\nlibrary_relative_path=lib/librevenge-0.0.0.dylib\n"
              f"library_sha256={p.digest(self.lr)}\n")
        for name in (*p.TOOLS, "pkg-config", "glib-compile-schemas", "gdk-pixbuf-query-loaders", "c++"):
            executable = write(tool_path(self.tools, name), f"synthetic tool {name} A\n")
            executable.chmod(0o755)
        self.cache = write(self.root / "build/CMakeCache.txt", f"CMAKE_CXX_COMPILER:FILEPATH={tool_path(self.tools, 'c++')}\n")
        self.evidence = self.root / "evidence"
        self.evidence.mkdir()
        self.inputs = self.evidence / p.INPUTS
        self.stage = self.evidence / "schemas"
        fake_helper = self.source / "packaging/vacards/packaging-provenance.py"
        write(fake_helper, HELPER.read_text())
        write(self.source / "packaging/macos/vacards/bundle-vacards-app.sh", "synthetic reviewed bundler\n")
        for mocker in (patch.object(p, "require_production_authorization", return_value=None),
                       patch.object(p, "__file__", str(fake_helper)),
                       patch.object(p, "run", side_effect=self.run_tool),
                       patch.dict(os.environ, {"PATH": str(self.tools) + os.pathsep + os.environ.get("PATH", "")})):
            mocker.start()
            self.addCleanup(mocker.stop)

    def run_tool(self, args, env=None):
        args = list(map(str, args))
        name = Path(args[0]).name
        if os.name == "nt" and name.lower().endswith(".exe"):
            name = name[:-4]
        if name == "pkg-config":
            return (str(self.pc_gtk) if "--variable=prefix" in args else "4.20.0") + "\n", ""
        if name == "gdk-pixbuf-query-loaders":
            # Real query-loaders discovers only modules physically present; the
            # fixtures unlink a module to exercise the missing-loader guard.
            found = [path for path in (self.loader, self.tiff_loader, *self.extra_loaders)
                     if Path(path).exists()]
            return "".join(f'"{path}"\n' for path in found), ""
        if name == "glib-compile-schemas":
            if "--version" in args:
                return "2.86.0\n", f"dyld[11]: <uuid> {dyld_path(self.glib)}\n"
            directory = Path(args[-1])
            payload = "".join(x.name + x.read_text() for x in sorted(directory.iterdir()))
            write(directory / "gschemas.compiled", payload)
            return "", ""
        if name == "inkscape":
            return "Inkscape synthetic\n", "".join(f"dyld[11]: <uuid> {dyld_path(x)}\n" for x in self.runtime)
        if name == "otool":
            binary = Path(args[-1])
            data = json.loads(binary.read_text())
            if args[1] == "-D":
                return f"{binary}:\n{data['id']}\n", ""
            if args[1] == "-l":
                return f"{binary}:\n" + "".join(
                    f"Load command {n}\n          cmd LC_RPATH\n      cmdsize 64\n"
                    f"         path {value} (offset 12)\n"
                    for n, value in enumerate(data.get("rpaths", []))), ""
            dependencies = ([data["id"]] if data["id"] else []) + data["links"]
            return f"{binary}:\n" + "".join(f"\t{x} (compatibility version 1.0.0, current version 1.0.0)\n" for x in dependencies), ""
        if name == "install_name_tool":
            binary = Path(args[-1])
            data = json.loads(binary.read_text())
            if args[1] == "-change":
                data["links"] = [args[3] if x == args[2] else x for x in data["links"]]
            else:
                data["id"] = args[2]
            write(binary, json.dumps(data))
            return "", ""
        raise AssertionError(f"unexpected synthetic command: {args}")

    def capture(self):
        p.capture(argparse.Namespace(cache=str(self.cache), executable=str(self.exe),
                  cairo_prefix=str(self.cairo), libcdr_prefix=str(self.libcdr),
                  output=str(self.inputs), schema_dir=str(self.stage)))
        return p.load_inputs(self.inputs)

    def capture_librevenge(self):
        p.capture(argparse.Namespace(cache=str(self.cache), executable=str(self.exe),
                  cairo_prefix=str(self.cairo), libcdr_prefix=str(self.libcdr),
                  librevenge_prefix=str(self.librevenge), output=str(self.inputs),
                  schema_dir=str(self.stage)))
        return p.load_inputs(self.inputs)

    def install(self):
        data = self.capture()
        install = self.root / "install"
        write(install / "bin/inkscape", self.exe.read_text())
        for name, source in ((p.INPUTS, self.inputs), ("VACARDS-CAIRO.txt", self.cairo / "VACARDS-CAIRO.txt"),
                             ("VACARDS-LIBCDR.env", self.libcdr / "VACARDS-LIBCDR.env")):
            shutil.copyfile(source, install / name)
        shutil.copytree(self.stage, install / p.SCHEMAS)
        # Opaque synthetic reservation bytes: this fixture tests the existing
        # gate/provenance hash chain, not run authentication or live re-observe.
        reservation = write(install / "VACARDS-GATE-RUN.json", '{"fixture_only":"full reservation A"}\n')
        provenance = write(install / "VACARDS-BUILD-PROVENANCE.env",
                           f"format=1\npackaging_inputs_sha256={p.digest(self.inputs)}\n"
                           f"gate_run_sha256={p.digest(reservation)}\n")
        write(install / "VACARDS-RELEASE-GATE.env",
              f"format=3\nscope=full\nbuild_provenance_sha256={p.digest(provenance)}\n"
              f"binary_sha256={p.digest(self.exe)}\nlibcdr_manifest_sha256={p.digest(self.libcdr / 'VACARDS-LIBCDR.env')}\n"
              f"libcdr_library_sha256={p.digest(self.cd)}\n")
        return data, install

    def app(self):
        data, install = self.install()
        app = self.root / "App ñ.app"
        resources = app / "Contents/Resources"
        resources.mkdir(parents=True)
        for source in install.iterdir():
            if source.is_file():
                shutil.copyfile(source, resources / source.name)
        write(app / "Contents/MacOS/inkscape-bin", self.exe.read_text())
        for record in data["libraries"]:
            if record["loader"]:
                name = next(module for module in p.REQUIRED_LOADERS if module in record["aliases"])
                target = resources / p.loader_relative(name)
            else:
                target = resources / ("lib/" + Path(record["file"]["path"]).name)
            # Reproduce dylibbundler having copied a valid but different build.
            binary = json.loads(Path(record["file"]["path"]).read_text())
            binary["payload"] = "discovered replacement B"
            write(target, json.dumps(binary))
        p.copy_schemas(argparse.Namespace(inputs=self.inputs, source=install / p.SCHEMAS,
                                         destination=resources / p.SCHEMA_DEST))
        return data, install, app

    def sign(self, app):
        # Explicit synthetic signing: no real codesign/ldid or package build.
        for binary in p.payload_files(app):
            data = json.loads(binary.read_text())
            data["signature"] = "synthetic ad-hoc signature"
            write(binary, json.dumps(data))

    def packaged(self):
        data, install, app = self.app()
        p.restore_bundle(argparse.Namespace(app=str(app), install=str(install), cairo_prefix=str(self.cairo)))
        self.sign(app)
        p.finish_bundle(argparse.Namespace(app=str(app)))
        return data, install, app

    def test_unchanged_inputs_and_compiled_schema_cache(self):
        data = self.capture()
        p.verify_inputs(data, str(self.cairo), runtime=True)
        self.assertEqual(set(data["roles"]), set(p.ROLES))
        self.assertEqual(data["schema_cache"], p.digest(self.stage / "gschemas.compiled"))

    def test_copied_gate_reservation_mutation_rejected(self):
        _, install = self.install()
        write(install / "VACARDS-GATE-RUN.json", '{"fixture_only":"valid reservation B"}\n')
        with self.assertRaisesRegex(p.Rejected, "reservation differs"):
            p.attest(install)

    def test_pending_candidate_and_default_real_cli_reject_missing_authority(self):
        data, install = self.install()
        final = install / "VACARDS-RELEASE-GATE.env"
        final.rename(install / "VACARDS-RELEASE-GATE.pending.env")
        self.assertEqual(p.attest(install, candidate=True)[0], data)
        command = [sys.executable, "-B", str(HELPER), "verify-attestation"]
        result = subprocess.run([*command, "--candidate", str(install)], text=True, capture_output=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("production release closure is unavailable", result.stderr)
        self.assertEqual(result.stdout, "")
        result = subprocess.run([*command, str(install)], text=True, capture_output=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("production release closure is unavailable", result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertFalse(final.exists())

    def test_pending_candidate_still_requires_exact_reservation(self):
        _, install = self.install()
        final = install / "VACARDS-RELEASE-GATE.env"
        final.rename(install / "VACARDS-RELEASE-GATE.pending.env")
        write(install / "VACARDS-GATE-RUN.json", "replacement reservation")
        with self.assertRaisesRegex(p.Rejected, "reservation differs"):
            p.attest(install, candidate=True)
        self.assertFalse(final.exists())

    def test_copied_gate_reservation_missing_rejected(self):
        _, install = self.install()
        (install / "VACARDS-GATE-RUN.json").unlink()
        with self.assertRaisesRegex(p.Rejected, "reservation is missing or linked"):
            p.attest(install)

    def test_copied_gate_reservation_symlink_rejected(self):
        _, install = self.install()
        reservation = install / "VACARDS-GATE-RUN.json"
        outside = self.root / "outside-ledger.json"
        reservation.rename(outside)
        symlink(reservation, outside)
        with self.assertRaisesRegex(p.Rejected, "reservation is missing or linked"):
            p.attest(install)

    def test_bundle_gate_reservation_mutation_rejected(self):
        _, _, app = self.packaged()
        write(app / "Contents/Resources/VACARDS-GATE-RUN.json", "replacement")
        with self.assertRaisesRegex(p.Rejected, "reservation differs"):
            p.verify_bundle(argparse.Namespace(app=str(app)))

    def test_valid_replacement_cairo_build_is_rejected(self):
        data = self.capture()
        old_hash = p.digest(self.ca)
        value = json.loads(self.ca.read_text())
        value["payload"] = "valid separately built Cairo B"
        write(self.ca, json.dumps(value))
        marker = self.cairo / "VACARDS-CAIRO.txt"
        write(marker, marker.read_text().replace(old_hash, p.digest(self.ca)))
        self.assertIn("Library SHA-256: " + p.digest(self.ca), marker.read_text())
        with self.assertRaisesRegex(p.Rejected, "tested input changed"):
            p.verify_inputs(data)

    def test_valid_replacement_libcdr_with_updated_manifest_is_rejected(self):
        data = self.capture()
        macho(self.cd, [self.glib], payload="valid separate build B")
        write(self.libcdr / "VACARDS-LIBCDR.env", f"format=1\nlibrary_sha256={p.digest(self.cd)}\n")
        with self.assertRaisesRegex(p.Rejected, "tested input changed"):
            p.verify_inputs(data)

    def test_another_cairo_prefix_is_rejected(self):
        data = self.capture()
        other = self.root / "other valid cairo"
        shutil.copytree(self.cairo, other)
        old_hash = p.digest(other / "lib/libcairo.2.dylib")
        macho(other / "lib/libcairo.2.dylib", [self.glib], payload="valid build B")
        marker = other / "VACARDS-CAIRO.txt"
        write(marker, marker.read_text().replace(old_hash, p.digest(other / "lib/libcairo.2.dylib")))
        with self.assertRaisesRegex(p.Rejected, "Cairo prefix differs"):
            p.verify_inputs(data, str(other))

    def test_gobject_gtk_compiler_patch_and_asset_drift(self):
        data = self.capture()
        for path in (self.cg, self.gt, tool_path(self.tools, "glib-compile-schemas"), tool_path(self.tools, "c++"), self.patch_file):
            with self.subTest(path=path.name):
                original = path.read_bytes()
                path.write_bytes(original + b"drift")
                with self.assertRaisesRegex(p.Rejected, "tested input changed"):
                    p.verify_inputs(data)
                path.write_bytes(original)
        self.ca.unlink()
        with self.assertRaisesRegex(p.Rejected, "required input is missing"):
            p.verify_inputs(data)

    def test_ambient_gtk_prefix_switch(self):
        data = self.capture()
        self.pc_gtk = self.root / "other GTK"
        self.pc_gtk.mkdir()
        with self.assertRaisesRegex(p.Rejected, "GTK pkg-config inputs differ"):
            p.verify_inputs(data)

    def test_explicit_gtk_capture_replaces_transitive_stock_library(self):
        stock = macho(self.root / "stock GTK/libgtk-4.1.dylib", [self.cg], payload="stock")
        wrapper = macho(self.gtk / "lib/libgtkmm-4.0.dylib", [stock])
        self.runtime.append(wrapper)
        p.capture(argparse.Namespace(cache=self.cache, executable=self.exe,
                  cairo_prefix=self.cairo, libcdr_prefix=self.libcdr, gtk_prefix=self.gtk,
                  output=self.inputs, schema_dir=self.stage))
        data = p.load_inputs(self.inputs)
        self.assertNotIn(str(stock.resolve()), {r["file"]["realpath"] for r in data["libraries"]})
        p.verify_inputs(data, runtime=True, gtk_prefix=self.gtk)
        self.assertEqual(p.runtime_env(self.cairo, self.gtk)["DYLD_LIBRARY_PATH"],
                         os.pathsep.join(map(str, (self.gtk / "lib", self.cairo / "lib"))))
        self.assertEqual(p.runtime_env(self.cairo)["DYLD_LIBRARY_PATH"], str(self.cairo / "lib"))
        with self.assertRaisesRegex(p.Rejected, "requested GTK prefix differs"):
            p.verify_inputs(data, runtime=True, gtk_prefix=stock.parent.parent)

    def test_runtime_env_two_arg_backcompat_and_ambient_dyld_removal(self):
        self.assertEqual(p.runtime_env(self.cairo)["DYLD_LIBRARY_PATH"],
                         str(self.cairo / "lib"))
        self.assertEqual(p.runtime_env(self.cairo, self.gtk)["DYLD_LIBRARY_PATH"],
                         os.pathsep.join(map(str, (self.gtk / "lib", self.cairo / "lib"))))
        with patch.dict(os.environ, {"DYLD_LIBRARY_PATH": "/ambient/search",
                                     "DYLD_FALLBACK_LIBRARY_PATH": "/ambient/fallback",
                                     "DYLD_INSERT_LIBRARIES": "/ambient/inject"}):
            env = p.runtime_env(self.cairo, self.gtk)
        self.assertEqual(env["DYLD_LIBRARY_PATH"],
                         os.pathsep.join(map(str, (self.gtk / "lib", self.cairo / "lib"))))
        self.assertNotIn("DYLD_FALLBACK_LIBRARY_PATH", env)
        self.assertNotIn("DYLD_INSERT_LIBRARIES", env)

    def test_runtime_env_explicit_libcdr_prefix_with_spaces(self):
        spaced = self.root / "pinned libcdr prefix with spaces"
        spaced.mkdir()
        env = p.runtime_env(self.cairo, self.gtk, spaced)
        self.assertEqual(env["DYLD_LIBRARY_PATH"],
                         os.pathsep.join(map(str, (self.gtk / "lib", self.cairo / "lib", spaced / "lib"))))
        self.assertIn(str(spaced / "lib"), env["DYLD_LIBRARY_PATH"])
        self.assertEqual(p.runtime_env(self.cairo, libcdr_prefix=spaced)["DYLD_LIBRARY_PATH"],
                         os.pathsep.join(map(str, (self.cairo / "lib", spaced / "lib"))))

    def test_runtime_paths_forwards_explicit_libcdr_prefix(self):
        seen = []
        real = p.runtime_env

        def spy(cairo_prefix, gtk_prefix=None, libcdr_prefix=None):
            seen.append(libcdr_prefix)
            return real(cairo_prefix, gtk_prefix, libcdr_prefix)

        with patch.object(p, "runtime_env", side_effect=spy):
            paths = p.runtime_paths(self.exe, self.cairo, self.gtk, self.libcdr)
        self.assertEqual(seen, [self.libcdr])
        self.assertIn(str(self.cd.resolve()), {str(Path(value).resolve()) for value in paths})

    def test_runtime_env_prepends_paired_librevenge(self):
        env = p.runtime_env(self.cairo, self.gtk, self.libcdr, self.librevenge)
        self.assertEqual(env["DYLD_LIBRARY_PATH"],
                         os.pathsep.join(map(str, (self.librevenge / "lib", self.gtk / "lib",
                                                   self.cairo / "lib", self.libcdr / "lib"))))
        # Omitting the new trailing argument preserves the previous value.
        self.assertEqual(p.runtime_env(self.cairo, self.gtk, self.libcdr)["DYLD_LIBRARY_PATH"],
                         os.pathsep.join(map(str, (self.gtk / "lib", self.cairo / "lib", self.libcdr / "lib"))))

    def test_explicit_librevenge_capture_accepts_paired_runtime(self):
        data = self.capture_librevenge()
        p.verify_inputs(data, runtime=True, gtk_prefix=self.gtk,
                        librevenge_prefix=self.librevenge)
        self.assertIn(str(self.lr.resolve()), {r["file"]["realpath"] for r in data["libraries"]})

    def test_explicit_librevenge_capture_rejects_stale_library_hash(self):
        macho(self.lr, [self.glib], payload="stock rebuild with same release")
        with self.assertRaisesRegex(p.Rejected, "librevenge library differs"):
            self.capture_librevenge()
        self.assertFalse(self.inputs.exists())

    def test_explicit_librevenge_capture_rejects_missing_manifest(self):
        (self.librevenge / "VACARDS-LIBREVENGE.env").unlink()
        with self.assertRaisesRegex(p.Rejected, "missing librevenge manifest"):
            self.capture_librevenge()
        self.assertFalse(self.inputs.exists())

    def test_explicit_librevenge_capture_rejects_foreign_runtime_library(self):
        stock = macho(self.root / "stock librevenge/librevenge-0.0.0.dylib", [self.glib])
        self.runtime.remove(self.lr)
        self.runtime.append(stock)
        with self.assertRaisesRegex(p.Rejected, "outside the attested prefix"):
            self.capture_librevenge()
        self.assertFalse(self.inputs.exists())

    def test_capture_and_runtime_verification_use_recorded_libcdr_prefix(self):
        probes = []
        real_paths = p.runtime_paths

        def spy(executable, cairo_prefix, gtk_prefix=None, libcdr_prefix=None):
            probes.append((Path(executable).name, libcdr_prefix))
            return real_paths(executable, cairo_prefix, gtk_prefix, libcdr_prefix)

        with patch.object(p, "runtime_paths", side_effect=spy):
            data = self.capture()
            p.verify_inputs(data, runtime=True)
        self.assertEqual(data["prefixes"]["libcdr"], str(self.libcdr.resolve()))
        inkscape_probes = [libcdr for name, libcdr in probes if name == "inkscape"]
        self.assertTrue(inkscape_probes)
        self.assertEqual(set(inkscape_probes), {data["prefixes"]["libcdr"]})

    def test_wrong_libcdr_prefix_is_rejected_at_capture(self):
        other = self.root / "other valid libcdr"
        shutil.copytree(self.libcdr, other)
        with self.assertRaisesRegex(p.Rejected, "outside the tested prefix"):
            p.capture(argparse.Namespace(cache=str(self.cache), executable=str(self.exe),
                      cairo_prefix=str(self.cairo), libcdr_prefix=str(other),
                      output=str(self.inputs), schema_dir=str(self.stage)))
        self.assertFalse(self.inputs.exists())

    def test_runtime_verification_rejects_libcdr_drift(self):
        data = self.capture()
        macho(self.cd, [self.glib], payload="drifted libcdr")
        with self.assertRaisesRegex(p.Rejected, "tested input changed"):
            p.verify_inputs(data, runtime=True)

    def test_explicit_gtk_capture_rejects_ambient_prefix(self):
        with self.assertRaisesRegex(p.Rejected, "pkg-config does not resolve the requested GTK"):
            p.capture(argparse.Namespace(cache=self.cache, executable=self.exe,
                      cairo_prefix=self.cairo, libcdr_prefix=self.libcdr,
                      gtk_prefix=self.root / "other GTK", output=self.inputs, schema_dir=self.stage))
        self.assertFalse(self.inputs.exists())

    def test_schema_add_remove_override_and_changed_bytes(self):
        data = self.capture()
        for name in ("extra.gschema.xml", "extra.gschema.override"):
            extra = write(self.schema_source / name, "new input")
            with self.assertRaisesRegex(p.Rejected, "schema source set changed"):
                p.verify_inputs(data)
            extra.unlink()
        schema = self.schema_source / "org.gtk.gtk4.Settings.Debug.gschema.xml"
        original = schema.read_text()
        schema.unlink()
        with self.assertRaisesRegex(p.Rejected, "schema source set changed"):
            p.verify_inputs(data)
        write(schema, original + "changed")
        with self.assertRaisesRegex(p.Rejected, "schema source set changed"):
            p.verify_inputs(data)

    def test_compiled_schema_change(self):
        data = self.capture()
        write(self.stage / "gschemas.compiled", "replacement")
        with self.assertRaisesRegex(p.Rejected, "compiled schema cache changed"):
            p.verify_inputs(data)

    def test_switched_schema_compiler_on_path(self):
        data = self.capture()
        alternate = write(tool_path(self.root / "alternate", "glib-compile-schemas"), "another compiler")
        alternate.chmod(0o755)
        with patch.dict(os.environ, {"PATH": str(alternate.parent) + os.pathsep + os.environ["PATH"]}):
            with self.assertRaisesRegex(p.Rejected, "schema compiler differs"):
                p.verify_inputs(data)

    def test_duplicate_runtime_cairo(self):
        self.runtime.append(macho(self.root / "foreign/libcairo.2.dylib"))
        with self.assertRaisesRegex(p.Rejected, "exactly one cairo"):
            self.capture()
        self.assertFalse(self.inputs.exists())

    def test_foreign_direct_cairo_link_is_rejected(self):
        foreign = macho(self.root / "foreign/libcairo.2.dylib")
        macho(self.exe, [foreign, self.cg, self.cd, self.gt], ident="")
        with self.assertRaisesRegex(p.Rejected, "directly links Cairo outside"):
            self.capture()
        self.assertFalse(self.inputs.exists())

    def test_freezes_loaded_cairo_not_gtk_recorded_stock_path(self):
        stock = macho(self.root / "stock Cairo/libcairo.2.dylib", [self.glib], payload="stock B")
        macho(self.gt, [stock, self.cg])
        data, _, app = self.packaged()
        self.assertNotIn(str(stock.resolve()), {r["file"]["realpath"] for r in data["libraries"]})
        p.verify_bundle(argparse.Namespace(app=str(app)))

    def test_svg_loader_rpath_closure_not_in_startup_trace(self):
        # Real Homebrew layout/LC_RPATH spellings from the failing SVG module.
        svg = macho(self.gtk / "lib/librsvg-2.2.dylib", [self.ca, self.glib])
        self.loader = macho(self.gtk / "lib/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader_svg.so",
                            ["@rpath/librsvg-2.2.dylib"], ident="",
                            rpaths=["@loader_path/../lib", "@loader_path/../../.."])
        self.assertNotIn(svg, self.runtime)
        data, _, app = self.packaged()
        record = next(r for r in data["libraries"] if r["file"]["realpath"] == str(svg.resolve()))
        self.assertEqual(record["file"]["sha256"], p.digest(svg))
        p.verify_bundle(argparse.Namespace(app=str(app)))

    def test_declared_rpath_order_and_missing_first_directory(self):
        first = macho(self.root / "first choice/libunique.dylib")
        second = macho(self.root / "second choice/libunique.dylib")
        macho(self.loader, ["@rpath/libunique.dylib"], ident="",
              rpaths=[self.root / "missing", first.parent, second.parent])
        data = self.capture()
        files = {r["file"]["realpath"] for r in data["libraries"]}
        self.assertIn(str(first.resolve()), files)
        self.assertNotIn(str(second.resolve()), files)

    def test_loader_path_and_executable_path_are_distinct(self):
        local = macho(self.loader.parent / "private/liblocal.dylib")
        main = macho(self.exe.parent / "private/libmain.dylib")
        macho(self.loader, ["@loader_path/private/liblocal.dylib", "@executable_path/private/libmain.dylib"], ident="")
        data = self.capture()
        files = {r["file"]["realpath"] for r in data["libraries"]}
        self.assertTrue({str(local.resolve()), str(main.resolve())} <= files)

    def test_loader_relative_id_in_unrelated_runtime_image_does_not_override(self):
        relative = "@loader_path/private/liblocal.dylib"
        unrelated = macho(self.root / "unrelated/liblocal.dylib", ident=relative)
        self.runtime.append(unrelated)
        local = macho(self.loader.parent / "private/liblocal.dylib")
        macho(self.loader, [relative], ident="")
        files = {r["file"]["realpath"] for r in self.capture()["libraries"]}
        self.assertIn(str(local.resolve()), files)

    def test_main_executable_rpath_is_inherited(self):
        leaf = macho(self.exe.parent / "extra/libinherited.dylib")
        macho(self.exe, self.runtime, ident="", rpaths=["@executable_path/extra"])
        macho(self.loader, ["@rpath/libinherited.dylib"], ident="")
        data = self.capture()
        self.assertIn(str(leaf.resolve()), {r["file"]["realpath"] for r in data["libraries"]})

    def test_transitive_parent_loader_rpath_is_not_child_relative(self):
        leaf = macho(self.root / "module/private/libleaf.dylib")
        child = macho(self.root / "module/private/libchild.dylib", ["@rpath/libleaf.dylib"])
        parent = macho(self.root / "module/libparent.dylib", ["@rpath/libchild.dylib"],
                       rpaths=["@loader_path/private"])
        macho(self.loader, [parent], ident="")
        data = self.capture()
        files = {r["file"]["realpath"] for r in data["libraries"]}
        self.assertTrue({str(parent.resolve()), str(child.resolve()), str(leaf.resolve())} <= files)

    def test_child_rpaths_precede_ancestor_rpaths(self):
        local = macho(self.root / "module/child-local/libleaf.dylib")
        other = macho(self.root / "module/parent-local/libleaf.dylib")
        child = macho(self.root / "module/libchild.dylib", ["@rpath/libleaf.dylib"],
                      rpaths=["@loader_path/child-local"])
        parent = macho(self.root / "module/libparent.dylib", [child], rpaths=["@loader_path/parent-local"])
        macho(self.loader, [parent], ident="")
        files = {r["file"]["realpath"] for r in self.capture()["libraries"]}
        self.assertIn(str(local.resolve()), files)
        self.assertNotIn(str(other.resolve()), files)

    def test_missing_rpath_does_not_guess_ambient_or_same_basename(self):
        ambient = macho(self.root / "ambient/libmissing.dylib")
        self.runtime.append(ambient)
        macho(self.loader, ["@rpath/libmissing.dylib"], ident="", rpaths=[self.root / "absent"])
        with patch.dict(os.environ, {"DYLD_FALLBACK_LIBRARY_PATH": str(ambient.parent)}):
            with self.assertRaisesRegex(p.Rejected, "ambient dependency-prefix guesses are not permitted"):
                self.capture()
        self.assertFalse(self.inputs.exists())

    def test_unrelated_image_rpaths_are_not_a_global_search_list(self):
        leaf = macho(self.root / "unrelated/libmissing.dylib")
        unrelated = macho(self.root / "other/libunrelated.dylib", rpaths=[leaf.parent])
        self.runtime.append(unrelated)
        macho(self.loader, ["@rpath/libmissing.dylib"], ident="")
        with self.assertRaisesRegex(p.Rejected, "cannot freeze unresolved dependency"):
            self.capture()
        self.assertFalse(self.inputs.exists())

    def test_relative_rpath_rejected_without_cwd_guess(self):
        macho(self.loader, ["@rpath/libmissing.dylib"], ident="", rpaths=["relative/lib"])
        with self.assertRaisesRegex(p.Rejected, "unsupported relative dyld path"):
            self.capture()
        self.assertFalse(self.inputs.exists())

    def test_symlinked_loader_uses_real_image_directory(self):
        leaf = macho(self.root / "real module/private/libleaf.dylib")
        real_loader = macho(self.root / "real module/libpixbufloader_svg.so", ["@rpath/libleaf.dylib"],
                            ident="", rpaths=["@loader_path/private"])
        self.loader.unlink()
        symlink(self.loader, real_loader)
        files = {r["file"]["realpath"] for r in self.capture()["libraries"]}
        self.assertIn(str(leaf.resolve()), files)

    def test_changed_compiler_runtime_is_rejected(self):
        data = self.capture()
        self.glib.write_bytes(self.glib.read_bytes() + b"new compiler dependency")
        with self.assertRaisesRegex(p.Rejected, "tested input changed"):
            p.verify_inputs(data)

    def test_loaded_libcdr_must_match_its_provenance_before_testing(self):
        macho(self.cd, [self.glib], payload="unattested rebuild")
        with self.assertRaisesRegex(p.Rejected, "loaded libcdr bytes differ"):
            self.capture()
        self.assertFalse(self.inputs.exists())

    def test_patch_changed_with_same_release_label(self):
        write(self.patch_file, "patch B same version")
        with self.assertRaisesRegex(p.Rejected, "patch bytes differ"):
            self.capture()
        self.assertFalse(self.inputs.exists())

    def test_old_cairo_marker_has_actionable_migration(self):
        write(self.cairo / "VACARDS-CAIRO.txt", "Cairo release: 1.18.4\n")
        with self.assertRaisesRegex(p.Rejected, "rerun run-release-gate.sh --full"):
            self.capture()
        self.assertFalse(self.inputs.exists())

    def test_duplicate_and_unsafe_inventory_fields(self):
        data = self.capture()
        write(self.inputs, self.inputs.read_text().replace('"format": 1', '"format": 1, "format": 1'))
        with self.assertRaisesRegex(p.Rejected, "duplicate provenance field"):
            p.load_inputs(self.inputs)
        data["schemas"]["../escape.gschema.xml"] = next(iter(data["schemas"].values()))
        with self.assertRaisesRegex(p.Rejected, "unsafe schema filename"):
            p.validate_inputs(data)

    def test_input_format_requires_integer_one(self):
        data = self.capture()
        for invalid in (True, False, 1.0, "1", None, 0, 2):
            with self.subTest(format=invalid):
                malformed = copy.deepcopy(data)
                malformed["format"] = invalid
                with self.assertRaisesRegex(p.Rejected, "format must be integer 1"):
                    p.validate_inputs(malformed)

    def test_required_strings_rejected_before_file_or_tool_use(self):
        data = self.capture()
        paths = [("prefixes", key) for key in data["prefixes"]]
        paths += [("roles", key) for key in data["roles"]]
        paths += [("schema_stage",), ("gtk", "prefix")]
        versions = [("gtk", "version"), ("schema_compiler", "version")]
        for keys in paths + versions:
            invalid_values = [None, True, 1, [], {}, "", " \t", "bad\0value"]
            if keys in paths:
                invalid_values.append("relative/path")
            for invalid in invalid_values:
                with self.subTest(field=".".join(keys), value=invalid):
                    malformed = copy.deepcopy(data)
                    parent = malformed
                    for key in keys[:-1]:
                        parent = parent[key]
                    parent[keys[-1]] = invalid
                    with patch.object(p, "check_file") as check_file, patch.object(p, "run") as run:
                        with self.assertRaises(p.Rejected) as rejected:
                            p.verify_inputs(malformed)
                        self.assertIn(".".join(keys), str(rejected.exception))
                        check_file.assert_not_called()
                        run.assert_not_called()

    def test_bundle_format_requires_integer_one(self):
        _, _, app = self.packaged()
        receipt_path = app / "Contents/Resources" / p.RECEIPT
        receipt = p.read_json(receipt_path)
        for invalid in (True, False, 1.0, "1", None, 0, 2):
            with self.subTest(format=invalid):
                receipt["format"] = invalid
                p.write_json(receipt_path, receipt)
                args = argparse.Namespace(app=str(app))
                with self.assertRaisesRegex(p.Rejected, "format must be integer 1"):
                    p.verify_bundle(args)
                with self.assertRaisesRegex(p.Rejected, "format must be integer 1"):
                    p.finish_bundle(args)

    def test_normal_relocation_and_signing_preserve_input_binding(self):
        _, _, app = self.packaged()
        p.verify_bundle(argparse.Namespace(app=str(app)))
        receipt = p.read_json(app / "Contents/Resources" / p.RECEIPT)
        for record in receipt["files"].values():
            self.assertEqual(record["before_sha256"], record["source"]["sha256"])
            self.assertNotEqual(record["before_sha256"], record["after_sha256"])

    def test_real_cli_rejects_complete_legacy_bundle_without_mutation(self):
        _, install, app = self.packaged()
        # Construction above exercises the retained integrity code under an
        # in-process fixture mock. No mock crosses this real CLI boundary.
        before = {str(path): path.read_bytes() for path in app.rglob("*") if path.is_file()}
        for args in (["verify-bundle", str(app)], ["finish-bundle", str(app)],
                     ["restore-bundle", "--app", str(app), "--install", str(install),
                      "--cairo-prefix", str(self.cairo)]):
            with self.subTest(command=args[0]):
                result = subprocess.run([sys.executable, "-B", str(HELPER), *args],
                                        text=True, capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 1)
                self.assertIn("production release closure is unavailable", result.stderr)
                self.assertEqual(result.stdout, "")
                after = {str(path): path.read_bytes() for path in app.rglob("*") if path.is_file()}
                self.assertEqual(before, after)

    def test_verification_after_relocation_without_source_prefixes(self):
        _, install, app = self.packaged()
        relocated = self.root / "DMG contents/Relocated.app"
        relocated.parent.mkdir()
        shutil.move(app, relocated)
        shutil.rmtree(self.deps)
        shutil.rmtree(install)
        shutil.rmtree(self.source)
        shutil.rmtree(self.root / "build")
        shutil.rmtree(self.evidence)
        p.verify_bundle(argparse.Namespace(app=str(relocated)))

    def test_post_signing_binary_tamper(self):
        _, _, app = self.packaged()
        binary = app / "Contents/Resources/lib/libgtk-4.1.dylib"
        binary.write_bytes(binary.read_bytes() + b"tamper")
        with self.assertRaisesRegex(p.Rejected, "post-signing binary changed"):
            p.verify_bundle(argparse.Namespace(app=str(app)))

    def test_packaging_tool_swap_does_not_finalize_receipt(self):
        _, install, app = self.app()
        p.restore_bundle(argparse.Namespace(app=str(app), install=str(install), cairo_prefix=str(self.cairo)))
        self.sign(app)
        write(tool_path(self.tools, "codesign"), "replacement signing tool")
        with self.assertRaisesRegex(p.Rejected, "packaging tool changed"):
            p.finish_bundle(argparse.Namespace(app=str(app)))
        self.assertEqual(p.read_json(app / "Contents/Resources" / p.RECEIPT)["phase"], "relocated")

    def test_missing_svg_loader_fails_even_if_removed_from_receipt(self):
        _, _, app = self.packaged()
        receipt_file = app / "Contents/Resources" / p.RECEIPT
        receipt = p.read_json(receipt_file)
        loader = "Contents/Resources/" + p.LOADER_DEST
        del receipt["files"][loader]
        (app / loader).unlink()
        p.write_json(receipt_file, receipt)
        with self.assertRaisesRegex(p.Rejected, "omits the tested libpixbufloader_svg.so"):
            p.verify_bundle(argparse.Namespace(app=str(app)))

    def test_missing_tiff_loader_fails_even_if_removed_from_receipt(self):
        _, _, app = self.packaged()
        receipt_file = app / "Contents/Resources" / p.RECEIPT
        receipt = p.read_json(receipt_file)
        loader = "Contents/Resources/" + p.loader_relative("libpixbufloader-tiff.so")
        self.assertIn(loader, receipt["files"])
        del receipt["files"][loader]
        (app / loader).unlink()
        p.write_json(receipt_file, receipt)
        with self.assertRaisesRegex(p.Rejected, "omits the tested libpixbufloader-tiff.so"):
            p.verify_bundle(argparse.Namespace(app=str(app)))

    def test_missing_svg_loader_input_rejected_before_capture(self):
        self.loader.unlink()
        with self.assertRaisesRegex(p.Rejected, r"exactly one libpixbufloader_svg\.so input"):
            self.capture()
        self.assertFalse(self.inputs.exists())

    def test_missing_tiff_loader_input_rejected_before_capture(self):
        self.tiff_loader.unlink()
        with self.assertRaisesRegex(p.Rejected, r"exactly one libpixbufloader-tiff\.so input"):
            self.capture()
        self.assertFalse(self.inputs.exists())

    def test_unrelated_extra_module_does_not_join_required_loaders(self):
        extra = macho(self.gtk / "lib/libpixbufloader-bmp.so", [self.glib], ident="")
        self.extra_loaders.append(extra)
        data = self.capture()
        self.assertNotIn(str(extra.resolve()), {r["file"]["realpath"] for r in data["libraries"]})
        self.assertEqual(sum(r["loader"] for r in data["libraries"]), 2)

    def test_receipt_cannot_substitute_untested_source(self):
        _, _, app = self.packaged()
        file = app / "Contents/Resources" / p.RECEIPT
        receipt = p.read_json(file)
        record = receipt["files"]["Contents/Resources/lib/libcairo.2.dylib"]
        record["source"]["sha256"] = record["before_sha256"] = "0" * 64
        p.write_json(file, receipt)
        with self.assertRaisesRegex(p.Rejected, "pre-rewrite library differs"):
            p.verify_bundle(argparse.Namespace(app=str(app)))

    def test_unknown_bundled_library_and_missing_library_fail_before_receipt(self):
        _, install, app = self.app()
        unexpected = macho(app / "Contents/Resources/lib/unknown.dylib")
        args = argparse.Namespace(app=str(app), install=str(install), cairo_prefix=str(self.cairo))
        with self.assertRaisesRegex(p.Rejected, "no unique tested input"):
            p.restore_bundle(args)
        self.assertFalse((app / "Contents/Resources" / p.RECEIPT).exists())
        unexpected.unlink()
        (app / "Contents/Resources/lib/libcairo-gobject.2.dylib").unlink()
        with self.assertRaisesRegex(p.Rejected, "omits a required tested library"):
            p.restore_bundle(args)

    def test_extra_packaged_schema_and_symlink_fail(self):
        _, _, app = self.packaged()
        directory = app / "Contents/Resources" / p.SCHEMA_DEST
        extra = write(directory / "extra.gschema.override", "injected")
        with self.assertRaisesRegex(p.Rejected, "schema file set differs"):
            p.verify_bundle(argparse.Namespace(app=str(app)))
        extra.unlink()
        schema = directory / "org.gtk.gtk4.Settings.Debug.gschema.xml"
        schema.unlink()
        symlink(schema, self.schema_source / schema.name)
        with self.assertRaisesRegex(p.Rejected, "regular copied file"):
            p.verify_bundle(argparse.Namespace(app=str(app)))

    def test_old_attestation_cli_fails_without_success_record(self):
        _, install = self.install()
        provenance = write(install / "VACARDS-BUILD-PROVENANCE.env", "format=1\n")
        gate = p.read_env(install / "VACARDS-RELEASE-GATE.env")
        gate["build_provenance_sha256"] = p.digest(provenance)
        write(install / "VACARDS-RELEASE-GATE.env", "".join(f"{k}={v}\n" for k, v in gate.items()))
        result = subprocess.run([sys.executable, "-B", str(HELPER), "verify-attestation", str(install)],
                                text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("production release closure is unavailable", result.stderr)
        self.assertIn("fresh integrated full gate", result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertFalse((install / p.RECEIPT).exists())


class LibrevengeVerifier(unittest.TestCase):
    """Run the real tracked platform verifier on synthetic prefixes.

    On Windows this is the Windows verifier the product selects
    (CMakeScripts/DefineDependsandFlags.cmake); on other hosts it is the macOS
    verifier. The verifier is the packaging contract for the paired patched
    librevenge; these cases reject stock/mislabelled/stale prefixes and accept a
    matching one without building librevenge.
    """

    KEYS = ("format", "dependency", "release", "source_identity", "archive_filename",
            "archive_sha256", "patch_filename", "patch_sha256", "capability_name",
            "capability_value", "capability_binary_marker", "library_relative_path",
            "library_sha256", "test_total", "test_crop", "test_text", "test_other",
            "make_check_total", "make_check_pass", "make_check_fail", "created_utc")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="vacards librevenge ñ ")
        self.addCleanup(self.temp.cleanup)
        self.prefix = Path(self.temp.name) / "librevenge prefix with spaces"
        (self.prefix / "lib/pkgconfig").mkdir(parents=True)
        self.library = self.prefix / "lib/librevenge-0.0.0.dylib"
        self.tracked = p.read_env(ROOT / "packaging/dependencies/librevenge-0.0.6/"
                                         "VACARDS-LIBREVENGE-BUNDLE.env")
        self.marker = self.tracked["capability_binary_marker"]
        self.write_library(marker=True)
        self.manifest = self.prefix / "VACARDS-LIBREVENGE.env"
        self.write_manifest()
        self.pc = self.prefix / "lib/pkgconfig/librevenge-0.0.pc"
        self.pc.write_text(f"prefix={self.prefix.resolve()}\n"
                           "Name: librevenge-0.0\nDescription: synthetic\n"
                           "Version: 0.0.6\nLibs:\n", encoding="utf-8", newline="\n")

    def write_library(self, marker=True):
        body = b"synthetic librevenge 0.0.6 "
        if marker:
            body += self.marker.encode("ascii")
        self.library.write_bytes(body + b" payload\n")

    def write_manifest(self, **overrides):
        values = {key: self.tracked[key] for key in self.KEYS if key in self.tracked}
        values.setdefault("format", "1")
        values.setdefault("created_utc", "2026-09-19T00:00:00Z")
        values.update(library_relative_path="lib/librevenge-0.0.0.dylib",
                      library_sha256=p.digest(self.library))
        values.update(overrides)
        write(self.manifest, "".join(f"{key}={values[key]}\n" for key in self.KEYS))
        return values

    def verify(self):
        if os.name == "nt":
            command = ["bash", str(ROOT / "packaging/windows/vacards/"
                                           "verify-vacards-librevenge.sh")]
            env = dict(os.environ, MSYSTEM="UCRT64")
        else:
            command = ["sh", str(ROOT / "packaging/macos/vacards/"
                                     "verify-vacards-librevenge.sh")]
            env = None
        return subprocess.run(command + [str(self.prefix)], env=env,
                              text=True, capture_output=True)

    def test_matching_prefix_accepted(self):
        result = self.verify()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Verified VACards librevenge 0.0.6", result.stdout)

    def test_missing_prefix_rejected(self):
        (self.prefix / "VACARDS-LIBREVENGE.env").unlink()
        result = self.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing", result.stderr)

    def test_stale_library_hash_rejected(self):
        self.write_library(marker=True)
        self.library.write_bytes(self.library.read_bytes() + b"drift")
        result = self.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("checksum changed", result.stderr)

    def test_stock_library_without_marker_rejected(self):
        self.write_library(marker=False)
        self.write_manifest()
        result = self.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("stock 0.0.6", result.stderr)

    def test_capability_name_mismatch_rejected(self):
        self.write_manifest(capability_name="vacards:other-capability")
        result = self.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("capability name differs", result.stderr)

    def test_capability_value_mismatch_rejected(self):
        self.write_manifest(capability_value="2")
        result = self.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("capability value differs", result.stderr)

    def test_marker_mismatch_against_tracked_manifest_rejected(self):
        self.write_manifest(capability_binary_marker="librevenge:other-marker")
        result = self.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("capability binary marker differs", result.stderr)

    def test_archive_hash_mismatch_rejected(self):
        self.write_manifest(archive_sha256="0" * 64)
        result = self.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("archive hash differs", result.stderr)

    def test_wrong_release_rejected(self):
        self.write_manifest(release="0.0.5")
        result = self.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("wrong librevenge release", result.stderr)

    def test_unsafe_relative_path_rejected(self):
        self.write_manifest(library_relative_path="../escape.dylib")
        result = self.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsafe library_relative_path", result.stderr)


class InternalDmgLibrevengeCore(unittest.TestCase):
    """Canonical packager maps a duplicate stock librevenge core onto the one
    runtime-observed, verified-prefix, manifest-matching patched core.

    Mirrors the existing Cairo-family canonicalization. Only the manifest's
    exact core basename is in scope; the auxiliary librevenge-stream/generators
    siblings and unrelated same-basename libraries keep the global
    native-filename collision guard.
    """

    CORE = "librevenge-0.0.0.dylib"

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="vacards dmg librevenge ñ ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.prefix = self.root / "paired librevenge prefix"
        self.core = self.prefix / "lib" / self.CORE
        self.core.parent.mkdir(parents=True)
        self.core.write_bytes(b"patched librevenge 0.0.6 core\n")
        self.manifest = {"library_relative_path": "lib/" + self.CORE,
                         "library_sha256": p.digest(self.core)}
        self.core_real = str(self.core.resolve())
        self.observed = {self.core_real}
        self.core_record = self.record(self.core, [self.CORE])
        self.stock = self.root / "homebrew/lib" / self.CORE
        self.stock.parent.mkdir(parents=True)
        self.stock.write_bytes(b"stock homebrew librevenge 0.0.6 core\n")
        self.stock_record = self.record(self.stock, [self.CORE])

    @staticmethod
    def record(path, aliases):
        return {"file": {"path": str(path), "realpath": str(path.resolve())},
                "aliases": list(aliases), "loader": False}

    def duplicate(self, real, candidate_records, observed=None):
        return dmg.librevenge_core_duplicate(
            real, candidate_records, self.observed if observed is None else observed,
            self.prefix, self.manifest)

    def test_duplicate_core_in_initial_graph_resolves_to_attested_core(self):
        records = [self.core_record, self.stock_record]
        self.assertTrue(self.duplicate(str(self.stock.resolve()), records))
        self.assertFalse(self.duplicate(self.core_real, records))

    def test_duplicate_core_in_extra_closure_resolves_to_attested_core(self):
        # The closure pass already holds the attested core and meets the stock
        # duplicate later; only the attested core remains.
        self.assertTrue(self.duplicate(str(self.stock.resolve()), [self.core_record]))
        self.assertFalse(self.duplicate(self.core_real, [self.core_record]))

    def test_stock_absolute_load_link_maps_to_patched_destination(self):
        records = {self.core_real: self.core_record}
        destination = self.root / "app/Contents/Resources/lib" / self.CORE
        images = {self.core_real: (self.core, destination)}
        aliases = dmg.bundle_aliases(records, images)
        self.assertEqual(aliases[self.CORE], {self.core_real})
        stock_link = "/opt/homebrew/opt/librevenge/lib/" + self.CORE
        self.assertEqual(
            dmg.resolve_bundled_candidate(stock_link, aliases,
                                          exact=str(self.stock.resolve())),
            {self.core_real})

    def test_absent_observed_core_rejected(self):
        with self.assertRaisesRegex(dmg.pp.Rejected,
                                    "no unique attested observed librevenge core"):
            self.duplicate(str(self.stock.resolve()), [self.stock_record], observed=set())

    def test_core_digest_drift_rejected(self):
        self.core.write_bytes(b"patched librevenge 0.0.6 core drifted\n")
        with self.assertRaisesRegex(dmg.pp.Rejected,
                                    "no unique attested observed librevenge core"):
            self.duplicate(str(self.stock.resolve()), [self.core_record, self.stock_record])

    def test_wrong_prefix_core_rejected(self):
        wrong = self.root / "other/lib" / self.CORE
        wrong.parent.mkdir(parents=True)
        wrong.write_bytes(self.core.read_bytes())
        wrong_record = self.record(wrong, [self.CORE])
        with self.assertRaisesRegex(dmg.pp.Rejected,
                                    "no unique attested observed librevenge core"):
            self.duplicate(str(self.stock.resolve()), [wrong_record, self.stock_record],
                           observed={str(wrong.resolve())})

    def test_ambiguous_observed_core_rejected(self):
        twin = self.root / "twin/lib" / self.CORE
        twin.parent.mkdir(parents=True)
        symlink(twin, self.core)
        twin_record = self.record(twin, [self.CORE])
        self.assertEqual(twin_record["file"]["realpath"], self.core_real)
        with self.assertRaisesRegex(dmg.pp.Rejected,
                                    "no unique attested observed librevenge core"):
            self.duplicate(str(self.stock.resolve()),
                           [self.core_record, twin_record, self.stock_record])

    def test_unsafe_manifest_core_path_rejected(self):
        with self.assertRaisesRegex(dmg.pp.Rejected, "unsafe librevenge core"):
            dmg.librevenge_core_target(self.prefix, {"library_relative_path": "../escape.dylib"})

    def test_auxiliary_and_unrelated_basenames_not_canonicalized(self):
        for name in ("librevenge-stream-0.0.0.dylib", "librevenge-generators-0.0.0.dylib",
                     self.CORE + ".backup", "libcdr-0.1.1.dylib"):
            other = self.root / "other/lib" / name
            other.parent.mkdir(parents=True, exist_ok=True)
            other.write_bytes(b"other dependency\n")
            self.assertFalse(
                self.duplicate(str(other.resolve()),
                               [self.core_record, self.record(other, [name])]),
                name)


class CairoBuilder(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="vacards cairo builder ñ ")
        self.addCleanup(self.temp.cleanup)
        self.shell = shutil.which("sh")
        if not self.shell:
            raise RuntimeError("missing Cairo builder test tool: sh")
        self.root = Path(self.temp.name)
        self.scripts = self.root / "source scripts"
        self.bin = self.root / "tools"
        self.output = self.root / "dependency output"
        self.log = self.root / "commands.jsonl"
        self.scripts.mkdir()
        for name in ("build-patched-cairo.sh", "verify-vacards-cairo-prefix.sh", "cairo-1.18.4-clip-all.patch"):
            shutil.copyfile(ROOT / "packaging/macos/vacards" / name, self.scripts / name)
        (self.scripts / "verify-vacards-cairo-prefix.sh").chmod(0o755)
        archive_hash = __import__("hashlib").sha256(b"synthetic source archive").hexdigest()
        dependency = write(self.scripts / "vacards-dependencies.sh",
                           '#!/bin/sh\ncase "$2" in\ncairo_release) echo 1.18.4;;\n'
                           f'cairo_release_sha256) echo {archive_hash};;\n'
                           'cairo_fix_commit) echo synthetic-fix;;\nmacos_deployment_target) echo 26.0;;\nesac\n')
        dependency.chmod(0o755)
        adapter = '''import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).stem
args = sys.argv[1:]
root = pathlib.Path(os.environ["SYNTHETIC_ROOT"])
# MSYS converts shell paths before invoking native Python. Record the native
# spelling, matching the paths asserted by the parent unittest process.
args = [str(pathlib.Path(arg)) if pathlib.Path(arg).is_absolute() else arg for arg in args]
with (root / "commands.jsonl").open("a") as stream:
    stream.write(json.dumps([name] + args) + "\\n")
if name == "curl":
    pathlib.Path(args[args.index("--output") + 1]).write_bytes(b"synthetic source archive")
elif name == "tar":
    (root / "dependency output/cairo-1.18.4").mkdir()
elif name == "patch":
    sys.stdin.read()
elif name == "meson" and args[0] == "setup":
    pathlib.Path(args[1]).mkdir()
elif name == "meson" and args[0] == "install":
    lib = root / "dependency output/install/lib"
    lib.mkdir(parents=True)
    (lib / "libcairo.2.dylib").write_bytes(b"synthetic Cairo")
    (lib / "libcairo-gobject.2.dylib").write_bytes(b"synthetic GObject")
elif name == "uname":
    print("Synthetic")
elif name == "pkg-config":
    print((root / "dependency output/install").as_posix())
'''
        for name in ("curl", "tar", "patch", "meson", "uname", "pkg-config"):
            program = write(self.bin / (name + ".py"), adapter)
            path = write(self.bin / name, "#!/bin/sh\nexec " +
                         shlex.quote(Path(sys.executable).as_posix()) + " -B " +
                         shlex.quote(program.as_posix()) + ' "$@"\n')
            path.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ.get("PATH", ""),
                        SYNTHETIC_ROOT=str(self.root), PYTHONUTF8="1")
        self.env.pop("VACARDS_DEPENDENCY_JOBS", None)

    def build(self):
        return subprocess.run([self.shell, (self.scripts / "build-patched-cairo.sh").as_posix(), self.output.as_posix()],
                              cwd=self.root, env=self.env, text=True, encoding="utf-8", capture_output=True)

    def test_default_jobs_and_actual_patch_hash(self):
        result = self.build()
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = [json.loads(x) for x in self.log.read_text().splitlines()]
        self.assertIn(["meson", "compile", "-C", str(self.output / "build"), "-j", "2"], commands)
        marker = (self.output / "install/VACARDS-CAIRO.txt").read_text()
        self.assertIn("Patch SHA-256: " + p.digest(self.scripts / "cairo-1.18.4-clip-all.patch"), marker)
        self.assertIn("GObject Library SHA-256: " + p.digest(self.output / "install/lib/libcairo-gobject.2.dylib"), marker)

    def test_explicit_jobs(self):
        self.env["VACARDS_DEPENDENCY_JOBS"] = "3"
        result = self.build()
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = [json.loads(x) for x in self.log.read_text().splitlines()]
        self.assertIn(["meson", "compile", "-C", str(self.output / "build"), "-j", "3"], commands)

    def test_invalid_jobs_fail_before_any_build_or_download(self):
        for jobs in ("", "0", "00", "01", "-1", "1.5", "two", "2;touch nope"):
            with self.subTest(jobs=jobs):
                self.env["VACARDS_DEPENDENCY_JOBS"] = jobs
                result = self.build()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("VACARDS_DEPENDENCY_JOBS must be a positive integer", result.stderr)
                self.assertFalse(self.output.exists())
                self.assertFalse(self.log.exists())

    def test_verifier_rejects_changed_patch_and_gobject(self):
        self.assertEqual(self.build().returncode, 0)
        script = self.scripts / "verify-vacards-cairo-prefix.sh"
        patch_file = self.scripts / "cairo-1.18.4-clip-all.patch"
        original = patch_file.read_bytes()
        patch_file.write_bytes(original + b"different patch same version")
        result = subprocess.run([self.shell, script.as_posix(), (self.output / "install").as_posix()], env=self.env,
                                cwd=self.root, text=True, encoding="utf-8", capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Cairo patch bytes changed", result.stderr)
        patch_file.write_bytes(original)
        write(self.output / "install/lib/libcairo-gobject.2.dylib", "different binary")
        result = subprocess.run([self.shell, script.as_posix(), (self.output / "install").as_posix()], env=self.env,
                                cwd=self.root, text=True, encoding="utf-8", capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Cairo GObject library checksum changed", result.stderr)


def _launcher_cache_coverage():
    """Register the dedicated BUG-003 launcher-cache test under this CTest name.

    The standalone script can also run directly; loading it here keeps the
    TIFF/SVG cache guard in the already-registered packaging suite without
    editing shared test CMake wiring.
    """
    path = Path(__file__).resolve().parents[1] / "vacards-tiff-loader-launcher-test.py"
    spec = importlib.util.spec_from_file_location("vacards_tiff_loader_launcher", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.LauncherCache


TiffLoaderLauncherCache = _launcher_cache_coverage()


if __name__ == "__main__":
    unittest.main()
