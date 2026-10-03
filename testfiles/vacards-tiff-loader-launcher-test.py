#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Outcome-based BUG-003 packaging regression for the macOS GdkPixbuf cache.

Run with: python3 -B testfiles/vacards-tiff-loader-launcher-test.py [-v]

Builds throwaway app-shaped directories under one temporary root (never the
installed app, a packaged staging tree, or the original share). A synthetic
``gdk-pixbuf-query-loaders`` and ``inkscape-bin`` exercise the real launchers so
the tests observe the cache each launcher actually hands to the application:

* ``internal-inkscape-launcher.sh`` regenerates the cache from its explicit
  module list and must refuse to start when the TIFF module is absent.
* ``inkscape-launcher.sh`` relocates the frozen ``gdk-pixbuf-loaders.cache.in``
  and must expose both modules with no leftover ``@APP_RESOURCES@`` token.

No app build, signing, or package creation is performed.
"""

import os
from pathlib import Path
import importlib.util
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
VACARDS = ROOT / "packaging/macos/vacards"
MODULE_DIR = "lib/gdk-pixbuf-2.0/2.10.0/loaders"

# The two launcher scripts exercised by the *execution* cases below are POSIX
# ``sh`` programs run through ``/bin/sh``; native Windows Python cannot resolve
# that path. Applicability is explicit and per-test so the loader-placement and
# script-content cases still execute on Windows (where this module is embedded
# by vacards-packaging-inputs-test) instead of being dropped by a blanket skip.
# This mirrors testfiles/CMakeLists.txt, which registers the standalone
# vacards-tiff-loader-launcher-test only under ``if(APPLE)``.
LAUNCHER_EXECUTION_PLATFORM = sys.platform == "darwin"
requires_macos_launcher = unittest.skipUnless(
    LAUNCHER_EXECUTION_PLATFORM,
    "macOS app-bundle launcher execution requires a POSIX /bin/sh",
)

# Bundled-data permission checks inspect POSIX mode bits; Windows has no
# equivalent for the owner-only failure being guarded against. Only these
# permission cases are skipped, the launcher/loader cases above still run on
# Windows when testfiles/src/vacards-packaging-inputs-test.py embeds this class.
POSIX_PERMISSIONS = os.name == "posix"
requires_posix_permissions = unittest.skipUnless(
    POSIX_PERMISSIONS,
    "bundled-data permission checks require POSIX mode-bit semantics",
)


def load_internal_dmg():
    """Import the internal-test packager to exercise its loader placement."""
    path = VACARDS / "create-internal-test-dmg.py"
    spec = importlib.util.spec_from_file_location("vacards_create_internal_test_dmg", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def svg_record(real_name="libpixbufloader_svg.dylib", registered="libpixbufloader_svg.so"):
    return {"loader": True, "aliases": [real_name, registered],
            "file": {"realpath": "/opt/homebrew/Cellar/librsvg/2.62.3/lib/gdk-pixbuf-2.0/"
                               "2.10.0/loaders/" + real_name,
                     "path": "/opt/homebrew/lib/gdk-pixbuf-2.0/2.10.0/loaders/" + registered}}


def write(path, text, executable=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")
    if executable:
        path.chmod(0o755)
    return path


class LauncherCache(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="vacards-tiff-loader-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def make_app(self, launcher, modules):
        app = self.root / ("App-" + launcher)
        macos = app / "Contents/MacOS"
        resources = app / "Contents/Resources"
        macos.mkdir(parents=True)
        resources.mkdir()
        shutil.copyfile(VACARDS / launcher, macos / "inkscape")
        (macos / "inkscape").chmod(0o755)
        for module in modules:
            write(resources / MODULE_DIR / module, "synthetic gdk-pixbuf module\n")
        query = write(macos / "gdk-pixbuf-query-loaders", f"""#!{sys.executable}
import os, sys
for path in sys.argv[1:]:
    if not os.path.isfile(path):
        continue
    print('"' + path + '"')
    if "tiff" in os.path.basename(path):
        print('"tiff" 5 "gdk-pixbuf" "TIFF" "LGPL"')
        print('"image/tiff"')
        print('"tiff" "tif"')
    else:
        print('"svg" 5 "gdk-pixbuf" "SVG" "LGPL"')
        print('"image/svg+xml"')
        print('"svg" "svgz"')
""", executable=True)
        bin_stub = write(macos / "inkscape-bin", """#!/bin/sh
echo "ran=1" > "$TEST_RESULT"
echo "cache_file=$GDK_PIXBUF_MODULE_FILE" >> "$TEST_RESULT"
echo "module_dir=$GDK_PIXBUF_MODULEDIR" >> "$TEST_RESULT"
[ -n "${GDK_PIXBUF_MODULE_FILE:-}" ] && cp "$GDK_PIXBUF_MODULE_FILE" "$TEST_RESULT.cache"
""", executable=True)
        return app, macos, query, bin_stub

    def run_launcher(self, macos, extra_env=None):
        result_file = self.root / "result.txt"
        profile = self.root / "profile"
        env = dict(os.environ)
        env.update(TEST_RESULT=str(result_file), INKSCAPE_PROFILE_DIR=str(profile),
                   HOME=str(self.root))
        env.update(extra_env or {})
        result = subprocess.run(["/bin/sh", str(macos / "inkscape")],
                                env=env, text=True, capture_output=True, timeout=30)
        return result, result_file

    def make_public_app(self, icc_mode, manifest_mode, directory_mode=0o755):
        """Build a minimal app bundle whose ICC and public manifest are the
        only permission-sensitive payload, with directory modes made explicit
        so a restrictive umask cannot decide a case's outcome."""
        module = load_internal_dmg()
        app = self.root / "Public.app"
        resources = app / "Contents/Resources"
        icc = resources / "share/inkscape/color/icc/TheBest.icc"
        icc.parent.mkdir(parents=True, exist_ok=True)
        icc.write_bytes(b"synthetic ICC profile bytes\n")
        manifest = resources / "VACARDS-INTERNAL-TEST.json"
        manifest.write_text('{"channel": "internal-test"}\n', encoding="utf-8")
        for directory in (app, app / "Contents", resources, resources / "share",
                          resources / "share/inkscape", resources / "share/inkscape/color",
                          icc.parent):
            directory.chmod(directory_mode)
        icc.chmod(icc_mode)
        manifest.chmod(manifest_mode)
        return module, app, (icc, manifest)

    @requires_posix_permissions
    def test_public_copy_sets_0644_from_0700_source_under_tight_umask(self):
        module = load_internal_dmg()
        source = self.root / "TheBest.icc"
        source.write_bytes(b"synthetic ICC profile bytes\n")
        source.chmod(0o700)
        target = self.root / "installed/TheBest.icc"
        previous = os.umask(0o077)
        try:
            module.copy(source, target, mode=module.PUBLIC_MODE)
        finally:
            os.umask(previous)
        self.assertEqual(stat.S_IMODE(source.stat().st_mode), 0o700,
                         "packaging must not chmod the source ICC")
        self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o644)
        self.assertEqual(stat.S_IMODE(target.stat().st_mode) & 0o044, 0o044,
                         "installed non-owner accounts must be able to read the ICC")
        self.assertEqual(target.read_bytes(), source.read_bytes())

    @requires_posix_permissions
    def test_default_copy_keeps_executable_mode_and_adds_owner_write(self):
        module = load_internal_dmg()
        source = self.root / "helper"
        source.write_bytes(b"native helper bytes\n")
        source.chmod(0o555)
        target = self.root / "out/helper"
        module.copy(source, target)
        self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o755)
        self.assertEqual(stat.S_IMODE(source.stat().st_mode), 0o555,
                         "packaging must not chmod the source helper")

    @requires_posix_permissions
    def test_verifier_rejects_owner_only_icc_even_when_owner_can_read(self):
        module, app, (icc, manifest) = self.make_public_app(0o700, 0o644)
        self.assertTrue(os.access(icc, os.R_OK),
                        "owner can read the 0700 ICC; os.access is not the oracle")
        with self.assertRaises(module.pp.Rejected):
            module.verify_bundled_data_permissions(app, (icc, manifest))

    @requires_posix_permissions
    def test_verifier_rejects_owner_only_manifest_even_when_owner_can_read(self):
        module, app, (icc, manifest) = self.make_public_app(0o644, 0o600)
        self.assertTrue(os.access(manifest, os.R_OK),
                        "owner can read the 0600 manifest; os.access is not the oracle")
        with self.assertRaises(module.pp.Rejected):
            module.verify_bundled_data_permissions(app, (icc, manifest))

    @requires_posix_permissions
    def test_verifier_rejects_owner_only_resource_ancestor(self):
        module, app, (icc, manifest) = self.make_public_app(0o644, 0o644)
        icc.parent.chmod(0o700)
        with self.assertRaises(module.pp.Rejected):
            module.verify_bundled_data_permissions(app, (icc, manifest))

    @requires_posix_permissions
    def test_verifier_accepts_0644_public_data_with_0755_ancestors(self):
        module, app, (icc, manifest) = self.make_public_app(0o644, 0o644)
        module.verify_bundled_data_permissions(app, (icc, manifest))

    @requires_macos_launcher
    def test_internal_launcher_valid_cache_has_svg_and_tiff(self):
        app, macos, _, _ = self.make_app(
            "internal-inkscape-launcher.sh", ["libpixbufloader_svg.so", "libpixbufloader-tiff.so"])
        result, result_file = self.run_launcher(macos)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result_file.exists(), "launcher did not exec the application")
        cache = (self.root / "result.txt.cache").read_text()
        self.assertIn("libpixbufloader_svg.so", cache)
        self.assertIn("libpixbufloader-tiff.so", cache)
        self.assertIn("image/tiff", cache)
        self.assertIn("image/svg+xml", cache)

    @requires_macos_launcher
    def test_internal_launcher_missing_tiff_refuses_to_start(self):
        _, macos, _, _ = self.make_app(
            "internal-inkscape-launcher.sh", ["libpixbufloader_svg.so"])
        result, result_file = self.run_launcher(macos)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TIFF", result.stderr)
        self.assertFalse(result_file.exists(), "launcher started without registering TIFF")

    @requires_macos_launcher
    def test_internal_launcher_missing_svg_refuses_to_start(self):
        _, macos, _, _ = self.make_app(
            "internal-inkscape-launcher.sh", ["libpixbufloader-tiff.so"])
        result, result_file = self.run_launcher(macos)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SVG", result.stderr)
        self.assertFalse(result_file.exists(), "launcher started without registering SVG")

    @requires_macos_launcher
    def test_production_launcher_relocates_cache_with_both_loaders(self):
        app, macos, _, _ = self.make_app(
            "inkscape-launcher.sh", ["libpixbufloader_svg.so", "libpixbufloader-tiff.so"])
        resources = app / "Contents/Resources"
        write(resources / "gdk-pixbuf-loaders.cache.in",
              f'"@APP_RESOURCES@/{MODULE_DIR}/libpixbufloader_svg.so"\n'
              '"svg" 5 "gdk-pixbuf" "SVG" "LGPL"\n"image/svg+xml"\n"svg" "svgz"\n'
              f'"@APP_RESOURCES@/{MODULE_DIR}/libpixbufloader-tiff.so"\n'
              '"tiff" 5 "gdk-pixbuf" "TIFF" "LGPL"\n"image/tiff"\n"tiff" "tif"\n')
        result, result_file = self.run_launcher(macos)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result_file.exists(), "launcher did not exec the application")
        record = result_file.read_text()
        self.assertNotIn("@APP_RESOURCES@", record)
        self.assertIn(str(resources), record)
        self.assertIn("module_dir=" + str(resources / MODULE_DIR), record)
        cache = (self.root / "result.txt.cache").read_text()
        self.assertIn(f"{resources}/{MODULE_DIR}/libpixbufloader_svg.so", cache)
        self.assertIn(f"{resources}/{MODULE_DIR}/libpixbufloader-tiff.so", cache)
        self.assertIn("image/tiff", cache)

    def test_scripts_name_the_required_modules(self):
        internal = (VACARDS / "internal-inkscape-launcher.sh").read_text()
        verify = (VACARDS / "verify-vacards-app.sh").read_text()
        bundle = (VACARDS / "bundle-vacards-app.sh").read_text()
        for text in (internal, verify, bundle):
            self.assertIn("libpixbufloader-tiff.so", text)
            self.assertIn("libpixbufloader_svg.so", text)

    def test_internal_packager_declares_native_entry(self):
        # The app's normal entry must be the native inkscape-bin (real AppKit
        # identity). The nested shell entries are covered by the behavior tests
        # below rather than a source-substring oracle.
        source = (VACARDS / "create-internal-test-dmg.py").read_text()
        self.assertIn('CFBundleExecutable="inkscape-bin"', source)

    def test_sign_internal_app_entry_signs_scripts_then_outer_bundle(self):
        module = load_internal_dmg()
        app = self.root / "Signed.app"
        calls = []

        def recording_run(*args, **kwargs):
            calls.append(args)

        module.run = recording_run
        module.sign_internal_app_entry(app)
        macos = app / "Contents/MacOS"
        self.assertEqual(calls, [
            ("codesign", "--force", "--sign", "-", macos / "inkscape"),
            ("codesign", "--force", "--sign", "-", macos / "python3"),
            ("codesign", "--force", "--sign", "-", macos / "python3-bin"),
            ("codesign", "--force", "--sign", "-", macos / "gdk-pixbuf-query-loaders"),
            ("codesign", "--force", "--sign", "-", app),
        ])

    def test_sign_internal_app_entry_propagates_first_error(self):
        module = load_internal_dmg()
        app = self.root / "Signed-fail.app"
        calls = []

        class SigningFailed(Exception):
            pass

        def failing_run(*args, **kwargs):
            calls.append(args)
            raise SigningFailed("codesign failed")

        module.run = failing_run
        with self.assertRaises(SigningFailed):
            module.sign_internal_app_entry(app)
        macos = app / "Contents/MacOS"
        self.assertEqual(calls, [
            ("codesign", "--force", "--sign", "-", macos / "inkscape"),
        ])

    def test_internal_dmg_maps_dylib_backed_svg_to_registered_so(self):
        # r4 shipped libpixbufloader_svg.dylib because the packager named the
        # copy after the realpath; the launcher and cache require the registered
        # .so name.
        module = load_internal_dmg()
        resources = self.root / "App.app/Contents/Resources"
        destination = module.loader_destination(resources, svg_record())
        self.assertEqual(destination, resources / MODULE_DIR / "libpixbufloader_svg.so")

    def test_internal_dmg_rejects_loader_without_registered_alias(self):
        module = load_internal_dmg()
        record = {"loader": True, "aliases": ["libpixbufloader_svg.dylib"],
                  "file": {"realpath": "/opt/homebrew/Cellar/librsvg/2.62.3/lib/gdk-pixbuf-2.0/"
                                       "2.10.0/loaders/libpixbufloader_svg.dylib",
                           "path": "/opt/homebrew/lib/gdk-pixbuf-2.0/2.10.0/loaders/"
                                   "libpixbufloader_svg.dylib"}}
        with self.assertRaises(module.pp.Rejected):
            module.loader_destination(self.root, record)

    @requires_macos_launcher
    def test_internal_dmg_placement_registers_dylib_backed_svg(self):
        # Outcome regression: place the module exactly as the packager would,
        # then let the real launcher regenerate the cache.
        module = load_internal_dmg()
        app, macos, _, _ = self.make_app(
            "internal-inkscape-launcher.sh", ["libpixbufloader-tiff.so"])
        resources = app / "Contents/Resources"
        destination = module.loader_destination(resources, svg_record())
        destination.write_text("synthetic gdk-pixbuf module\n", encoding="utf-8")
        result, result_file = self.run_launcher(macos)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result_file.exists(), "launcher did not exec the application")
        cache = (self.root / "result.txt.cache").read_text()
        self.assertIn("libpixbufloader_svg.so", cache)
        self.assertIn("image/svg+xml", cache)


if __name__ == "__main__":
    unittest.main()
