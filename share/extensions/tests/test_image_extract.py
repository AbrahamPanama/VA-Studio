# coding=utf-8

import os, sys

import pytest
from image_extract import ExtractImage
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase
from inkex.tester.mock import Capture, MockCommandMixin
import inkex


class ExtractImageBasicTest(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    """Comparison tests of ExtractImage"""

    stderr_protect = False
    effect_class = ExtractImage
    compare_file = "svg/images.svg"
    compare_file_extension = "png"
    comparisons = [
        ("--selectedonly=False",),
        ("--selectedonly=True", "--id=embeded_image01"),
    ]

    def test_all_comparisons(self):
        """Images are extracted to a file directory"""
        for args in self.comparisons:
            args += (f"--filepath={self.tempdir}/",)
            self.assertCompare(self.compare_file, None, args, "embeded_image01.png")


class ExtractImageComponentTests(TestCase, MockCommandMixin):
    """Test some functions of ExtractImage"""

    mocks = [(sys, "exit", lambda status: None)]  # type:ignore

    def test_extract_multiple(self):
        """test extraction in a multi-image file"""
        args = [
            self.data_file("svg", "images_multiple.svg"),
            f"--directory={self.tempdir}/",
            f"--output={self.tempdir}/out.svg",
            "--selectedonly=True",
            "--id=embedded_image01",
            "--id=embedded_image02",
            "--basename=image.jpg",  # test that bad file extensions are corrected
            "--linkextracted=false",
        ]

        ext = ExtractImage()

        def run_test(outexists):
            with Capture("stderr") as stderr:
                ext.run(args)
                # There should be two images saved to the tempdir
                self.assertListEqual(
                    sorted(os.listdir(self.tempdir)),
                    ["image_1.png", "image_2.png"] + (["out.svg"] if outexists else []),
                )
                self.assertEqual(len(stderr.getvalue().split("extracted")), 3)
                self.assertEqual(ext.errcount, 0)

        # the out file only exists if it was changed, in this case, no change should
        # be made
        run_test(False)
        for file in os.scandir(self.tempdir):
            os.remove(file.path)
        # We can do the same test again, because of linkextracted=False, the file is
        # unchaged when running it on the output file.
        args[-1] = "--linkextracted=true"
        run_test(True)

        # Now save all images again from the temp file, two images will raise an error
        # because they are already embedded

        # we also test that that relative target directories are interpreted relative to
        # the input file stored in DOCUMENT_PATH (and not the actual input file name)

        args[0] = f"{self.tempdir}/out.svg"
        args[3] = "--selectedonly=False"
        os.environ["DOCUMENT_PATH"] = os.path.abspath("images_multiple.svg")
        args[1] = "--directory=" + os.path.relpath(self.tempdir, "images_multiple.svg")

        with Capture("stderr") as stderr:
            ext.run(args)
            # There should be three images saved to the tempdir
            self.assertListEqual(
                sorted(os.listdir(self.tempdir)),
                ["image_1.png", "image_2.png", "image_3.png", "out.svg"],
            )
            self.assertEqual(len(stderr.getvalue().split("extracted")), 2)
            self.assertEqual(len(stderr.getvalue().split("Unable")), 4)
            self.assertEqual(ext.errcount, 3)

    def test_extract_single_contextmenu(self):
        """test extraction in a multi-image file, called from the extensions menu,
        but with only one selected image"""
        args = [
            self.data_file("svg", "images_multiple.svg"),
            f"--directory={self.tempdir}/",
            "--selectedonly=True",
            "--id=embedded_image01",
            "--basename=image",
        ]
        ext = ExtractImage()

        with Capture("stderr") as stderr:
            ext.run(args)

            self.assertListEqual(sorted(os.listdir(self.tempdir)), ["image.png"])

            self.assertEqual(len(stderr.getvalue().split("extracted")), 2)
            self.assertEqual(ext.errcount, 0)

    @pytest.mark.skipif(sys.platform == "win32", reason="all directories writeable")
    def test_extract_badpath(self):
        """Test writing to an unwriteable directory"""

        args = [
            self.data_file("svg", "images_multiple.svg"),
            "--directory=/proc",
            "--id=embedded_image01",
            "--basename=image",
        ]
        with Capture("stderr") as stderr:
            ext = ExtractImage()
            ext.run(args)

            self.assertIn("Unable to write to", stderr.getvalue())
            self.assertEqual(ext.errcount, 1)

        args = [
            self.data_file("svg", "images_multiple.svg"),
            "--directory=/proc/test",
            "--id=embedded_image01",
            "--basename=image",
        ]
        ext = ExtractImage()

        with Capture("stderr") as stderr:
            # The extension aborts
            ext.run(args)
            self.assertIn("Unable to create", stderr.getvalue())

    def test_extract_badfilename(self):
        """Test writing to an unwriteable directory"""

        if sys.platform == "win32":
            filename = "<"
        else:
            filename = "a\x00"  # null bytes are invalid
        args = [
            self.data_file("svg", "images_multiple.svg"),
            "--directory=" + self.tempdir,
            "--id=embedded_image01",
            "--basename=" + filename,
        ]
        with Capture("stderr") as stderr:
            ext = ExtractImage()

            if sys.version_info > (3, 8, 0) or filename == "<":
                ext.run(args)

                self.assertIn("Unable to write to", stderr.getvalue())
                self.assertEqual(ext.errcount, 1)
            else:
                with self.assertRaises(ValueError) as __:
                    ext.run(args)

    def test_extract_bad_data(self):
        """Extract bad data"""

        svg = inkex.load_svg(self.data_file("svg", "images_multiple.svg")).getroot()

        svg.getElementById("embedded_image01").set("xlink:href", "data:svg+xml;base64,")
        svg.getElementById("embedded_image02").set(
            "xlink:href", "data:image/abc;base32,"
        )

        temppath = os.path.join(self.tempdir, "temp.svg")

        with open(temppath, "wb") as f:
            f.write(svg.tostring())
        args = [
            self.data_file("svg", temppath),
            "--directory=" + self.tempdir,
            "--id=embedded_image01",
            "--id=embedded_image02",
            "--basename=badimage",
        ]
        ext = ExtractImage()
        with Capture("stderr") as stderr:
            ext.run(args)

            self.assertIn("Invalid image format", stderr.getvalue())
            self.assertIn("encoding", stderr.getvalue())
            self.assertEqual(ext.errcount, 2)
