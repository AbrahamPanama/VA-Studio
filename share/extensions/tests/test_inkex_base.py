# coding=utf-8
"""Test base inkex module functionality"""

from __future__ import absolute_import, print_function, unicode_literals

import os
import sys

from io import BytesIO

from inkex import AbortExtension, SvgDocumentElement
from inkex.base import InkscapeExtension, SvgThroughMixin
from inkex.tester import TestCase
from inkex.tester.mock import Capture


class ModExtension(InkscapeExtension):
    """A non-svg extension that loads, saves and flipples"""

    def effect(self):
        self.document += b">flipple"

    def load(self, stream):
        return stream.read()

    def save(self, stream):
        stream.write(self.document)


class NoModSvgExtension(SvgThroughMixin, InkscapeExtension):
    """Test the loading and not-saving of non-modified svg files"""

    def effect(self):
        return True


class ModSvgExtension(SvgThroughMixin, InkscapeExtension):
    """Test the loading and saving of svg files"""

    def effect(self):
        self.svg.set("attr", "foo")


class InkscapeExtensionTest(TestCase):
    """Tests for Inkscape Extensions"""

    effect_class = InkscapeExtension

    def setUp(self):
        self.e = self.effect_class()

    def test_bare_bones(self):
        """What happens when we don't inherit"""
        with self.assertRaises(NotImplementedError):
            self.e.run([])
        with self.assertRaises(NotImplementedError):
            prevarg = sys.argv
            sys.argv = ["pytest"]
            try:
                self.e.run()
            finally:
                sys.argv = prevarg
        with self.assertRaises(NotImplementedError):
            self.e.effect()
        with self.assertRaises(NotImplementedError):
            self.e.load(sys.stdin)
        with self.assertRaises(NotImplementedError):
            self.e.save(sys.stdout)
        self.assertEqual(self.e.name, "InkscapeExtension")

    def test_compat(self):
        """Test a few old functions and how we handle them"""
        with self.assertRaises(AttributeError):
            self.assertEqual(self.e.OptionParser, None)
        with self.assertRaises(AttributeError):
            self.assertEqual(self.e.affect(), None)

    def test_arg_parser_defaults(self):
        """Test arguments for the base class are given defaults"""
        options = self.e.arg_parser.parse_args([])
        self.assertEqual(options.input_file, None)
        self.assertEqual(options.output, None)

    def test_arg_parser_passed(self):
        """Test arguments for the base class are parsed"""
        options = self.e.arg_parser.parse_args(["--output", "foo.txt", self.empty_svg])
        self.assertEqual(options.input_file, self.empty_svg)
        self.assertEqual(options.output, "foo.txt")

    def test_run_help(self):
        """Ensure we can run `--help` and output contains '--help'"""
        with Capture("stdout") as stdout:
            with self.assertRaises(SystemExit):
                self.e.run(["--help"])
            self.assertIn("--help", stdout.getvalue())

    def test_get_resource(self):
        """We can get a resource path, based on where the extension is located"""
        ext = ModExtension()
        self.assertRaises(AbortExtension, ext.get_resource, "sir-not-apearing.py")

        # Test relative filename, which fails with AbortExtension if not found.
        ret = ext.get_resource(__file__)
        # Test absolute filename, which we already have, so just feed it back.
        self.assertEqual(ext.get_resource(ret), ret)

    def test_svg_path(self):
        """Can get the svg file location"""
        output = os.path.join(self.tempdir, "output.tmp")
        ext = ModExtension()
        os.environ["DOCUMENT_PATH"] = self.empty_svg
        self.assertEqual(ext.svg_path(), os.path.join(self.datadir(), "svg"))
        self.assertIn(ext.absolute_href("/foo"), ["/foo", "C:\\foo"])
        self.assertEqual(
            ext.absolute_href("./foo"), os.path.join(self.datadir(), "svg", "foo")
        )
        self.assertEqual(
            ext.absolute_href("~/foo"), os.path.realpath(os.path.expanduser("~/foo"))
        )

    def test_svg_no_path(self):
        tmp_foo = os.path.realpath("/tmp/foo")
        os.environ["DOCUMENT_PATH"] = ""
        ext = ModExtension()
        # Default results in home dir
        self.assertEqual(
            ext.absolute_href("./foo"), os.path.realpath(os.path.expanduser("~/foo"))
        )
        # Or override the default
        self.assertEqual(ext.absolute_href("./foo", "/tmp/"), tmp_foo)
        # But we can ask for errors too, this one for "document not saved"
        self.assertRaises(AbortExtension, ext.absolute_href, "./foo", default=None)
        # This covers inkscape old versions
        del os.environ["DOCUMENT_PATH"]
        self.assertRaises(AbortExtension, ext.absolute_href, "./foo", default=None)


class TestArgumentDatatypes(TestCase):
    """Test special argument types for the dataparser"""

    def test_page_descriptor(self):
        """Test that page descriptions are parsed correctly"""

        def parsetest(string, length, comparison, startvalue=1):
            result = InkscapeExtension.arg_number_ranges()
            self.assertTupleEqual(
                result(string)(length, startvalue=startvalue), comparison
            )

        parsetest("1, 2,3", 10, (1, 2, 3))
        parsetest("1-3, 5, 10", 10, (1, 2, 3, 5, 10))
        parsetest("2, 4-, 6, 7-", 10, (2, 4, 5, 6, 7, 8, 9, 10))
        parsetest("2-5, 7-9, 10-", 12, (2, 3, 4, 5, 7, 8, 9, 10, 11, 12))
        parsetest("6, 7, 8", 3, tuple())
        parsetest("2, 216-218, 10", 300, (2, 10, 216, 217, 218))
        parsetest("-3,10-,5", 12, (1, 2, 3, 5, 10, 11, 12))
        parsetest("-5, 7-", 7, (3, 4, 5, 7), 3)
        parsetest("-", 7, (), 3)


class SvgInputOutputTest(TestCase):
    """Test SVG Input Mixin"""

    def test_input_mixin(self):
        """Test svg input gets loaded"""
        obj = NoModSvgExtension()
        obj.run([self.empty_svg])
        self.assertNotEqual(obj.document, None)
        self.assertNotEqual(obj.original_document, None)

    def test_no_output(self):
        """Test svg output isn't saved when not modified"""
        obj = NoModSvgExtension()
        filename = self.temp_file(suffix=".svg")
        obj.run(["--output", filename, self.empty_svg])
        self.assertEqual(type(obj.document).__name__, "_ElementTree")
        self.assertEqual(type(obj.svg).__name__, "SvgDocumentElement")
        self.assertFalse(os.path.isfile(filename))

    def test_svg_output(self):
        """Test svg output is saved"""
        obj = ModSvgExtension()
        filename = self.temp_file(suffix=".svg")
        obj.run(["--output", filename, self.empty_svg])
        self.assertTrue(os.path.isfile(filename))
        with open(filename, "r") as fhl:
            self.assertIn("<svg", fhl.read())

    def test_str_document(self):
        """Document is saved even if it's not bytes"""
        obj = ModSvgExtension()
        obj.document = b"foo"
        obj.save(BytesIO())
        obj.document = "foo"
        ret = BytesIO()
        obj.save(ret)
        self.assertEqual(ret.getvalue(), b"foo")


class ParserTests(TestCase):
    """Test special parsing cases"""

    def test_bad_namespace(self):
        """Ignore bad namespaces, https://gitlab.com/inkscape/extensions/-/issues/465"""
        svg = (
            '<?xml version="1.0" encoding="UTF-8" standalone="no"?>'
            '<svg xmlns:ns="&amp;#38;#38;ns_ai;"></svg>'
        )
        filename = self.temp_file(suffix=".svg")
        with open(filename, "w") as file:
            file.write(svg)
        with Capture("stderr") as stderr:
            obj = NoModSvgExtension()
            obj.run([filename])
            # Test that the extension runs completely, and the document gets loaded
            self.assertTrue(isinstance(obj.svg, SvgDocumentElement))
            # Test that the error is presented to the user
            self.assertIn("is not a valid URI", stderr.getvalue())
