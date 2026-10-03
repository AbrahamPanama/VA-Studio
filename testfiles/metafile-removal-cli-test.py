#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the shipped CLI, without ImageMagick or metafile delegates."""
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest

APP = str(Path(sys.argv.pop(1)).resolve())
PROFILE_WRITER = str(Path(sys.argv.pop(1)).resolve())
FIXTURES = Path(__file__).parent / "cli_tests" / "testcases"


class MetafileRemoval(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="vacards-metafile-")
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        extensions = self.work / "extensions"
        extensions.mkdir()
        self.env = dict(os.environ, INKSCAPE_PROFILE_DIR=str(self.work / "profile"),
                        INKSCAPE_EXTENSIONS_DIR=str(extensions),
                        INKSCAPE_APP_ID_TAG="metafileremoval")

    def run_app(self, *args):
        result = subprocess.run([APP, *map(str, args)], cwd=self.work,
                                env=self.env, capture_output=True, timeout=60)
        self.assertGreaterEqual(result.returncode, 0, result.stderr)
        return result

    def test_removed_exports_rejected_without_writing(self):
        for fmt in ("emf", "wmf"):
            for mode in ("type", "uppercase", "suffix", "extension", "forced-extension"):
                with self.subTest(format=fmt, mode=mode):
                    destination = self.work / ("result." + fmt)
                    options = {
                        "type": ["--export-type=" + fmt],
                        "uppercase": ["--export-type=" + fmt.upper()],
                        "suffix": [],
                        "extension": ["--export-extension=org.inkscape.output." + fmt],
                        "forced-extension": ["--export-type=" + fmt,
                                             "--export-extension=org.inkscape.output." + fmt],
                    }[mode]
                    result = self.run_app(FIXTURES / "shapes.svg", *options,
                                          "--export-filename=" + str(destination))
                    # Legacy CLI reports unknown types on stderr, sometimes
                    # with exit status 0. Require the diagnostic AND no file.
                    self.assertIn(b"Unknown export type: " + fmt.encode(), result.stderr)
                    self.assertFalse(destination.exists())
                    self.assertEqual(list(self.work.glob("result.*")), [])
                    destination.write_bytes(b"existing-user-file")
                    result = self.run_app(FIXTURES / "shapes.svg", *options,
                                          "--export-filename=" + str(destination))
                    self.assertIn(b"Unknown export type: " + fmt.encode(), result.stderr)
                    self.assertEqual(destination.read_bytes(), b"existing-user-file")
                    destination.unlink()

    def test_removed_extension_ids_without_suffix(self):
        for fmt in ("emf", "wmf"):
            result = self.run_app(FIXTURES / "shapes.svg",
                                  "--export-extension=org.inkscape.output." + fmt,
                                  "--export-filename=" + str(self.work / "result"))
            self.assertIn(b"--export-extension was not found", result.stderr)
            self.assertEqual(list(self.work.glob("result*")), [])

    def test_real_metafiles_cannot_be_opened(self):
        for fmt in ("emf", "wmf"):
            result = self.run_app(FIXTURES / ("shapes_expected." + fmt),
                                  "--export-type=svg",
                                  "--export-filename=" + str(self.work / "import.svg"))
            self.assertIn(b"failed", result.stderr.lower())
            self.assertFalse((self.work / "import.svg").exists())

    def test_remaining_exports_work(self):
        profile = self.work / "synthetic-output.icc"
        subprocess.run([PROFILE_WRITER, str(profile)], check=True, timeout=10)
        self.env["INKSCAPE_VACARDS_TIFF_ICC_PROFILE"] = str(profile)
        signatures = {"svg": b"<svg", "png": b"\x89PNG", "pdf": b"%PDF",
                      "ps": b"%!PS", "eps": b"%!PS", "tiff": b"II"}
        for fmt, signature in signatures.items():
            with self.subTest(format=fmt):
                destination = self.work / ("supported." + fmt)
                result = self.run_app(FIXTURES / "shapes.svg",
                                      "--export-type=" + fmt,
                                      "--export-filename=" + str(destination))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(destination.is_file(), result.stderr)
                data = destination.read_bytes()
                self.assertGreater(len(data), 16)
                self.assertIn(signature, data[:4096])

    def test_help_does_not_advertise_metafiles(self):
        result = self.run_app("--help")
        export_line = next(line for line in result.stdout.splitlines()
                           if b"--export-type=" in line)
        self.assertNotIn(b"emf", export_line)
        self.assertNotIn(b"wmf", export_line)

    def test_packaging_does_not_advertise_metafiles(self):
        root = Path(__file__).resolve().parents[1]
        with (root / "packaging/macos/res/inkscape.plist").open("rb") as stream:
            plist = plistlib.load(stream)
        for entry in plist["CFBundleDocumentTypes"]:
            extensions = {value.lower() for value in entry.get("CFBundleTypeExtensions", [])}
            self.assertFalse(extensions.intersection({"wmf", "emf", "wmz", "emz"}))
        desktop = (root / "share/CMakeLists.txt").read_text()
        self.assertNotIn('"image/x-emf"', desktop)
        self.assertNotIn('"image/x-wmf"', desktop)
        sources = (root / "src/extension/CMakeLists.txt").read_text()
        for implementation in ("emf-inout", "emf-print", "wmf-inout", "wmf-print",
                               "metafile-inout", "metafile-print", "text_reassemble"):
            self.assertNotIn("internal/" + implementation, sources)
        self.assertNotIn("Uemf::uemf", (root / "src/CMakeLists.txt").read_text())
        clipboard = (root / "src/ui/clipboard.cpp").read_text()
        self.assertNotIn("CF_ENHMETAFILE", clipboard)
        self.assertNotIn('"image/x-emf"', clipboard)
        self.assertIn("CF_DIB", clipboard)
        self.assertIn("CF_BITMAP", clipboard)


if __name__ == "__main__":
    unittest.main()
