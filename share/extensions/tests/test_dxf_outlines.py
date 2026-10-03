# coding=utf-8
from io import BytesIO
from dxf_outlines import DxfOutlines
from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase
from inkex.tester.filters import WindowsTextCompat
from inkex.elements._parser import load_svg

from inkex.utils import AbortExtension
from inkex.base import SvgOutputMixin
from inkex.elements import Rectangle, Circle


class DFXOutlineBasicTest(ComparisonMixin, InkscapeExtensionTestMixin, TestCase):
    effect_class = DxfOutlines
    comparisons = [
        (),
        ("--id=p1", "--id=r3"),
        ("--POLY=true",),
        ("--ROBO=true",),
    ]
    compare_filters = [WindowsTextCompat()]


class DXFOutlineTestPxUnit(ComparisonMixin, TestCase):
    """Test for https://gitlab.com/inkscape/extensions/-/issues/542"""

    effect_class = DxfOutlines
    compare_file = ["svg/units_pt.svg"]
    comparisons = [
        (),
    ]
    compare_filters = [WindowsTextCompat()]


class DXFOutlineTestNonANSI(ComparisonMixin, TestCase):
    """Test for issues:
    https://gitlab.com/inkscape/extensions/-/issues/593
    https://gitlab.com/inkscape/extensions/-/issues/586
    """

    effect_class = DxfOutlines
    compare_file = ["svg/non_ansi_characters.svg"]
    comparisons = [
        (),
    ]
    compare_filters = [WindowsTextCompat()]


def run_extension(document, *args) -> str:
    output = BytesIO()
    ext = DxfOutlines()
    ext.parse_arguments([*args])
    ext.svg = document.getroot()
    ext.document = document
    ext.effect()
    ext.save(output)
    output.seek(0)
    return output.read()


class CommentTest(TestCase):
    """Check that a svg with comments is exported correctly"""

    def test_comment(self):
        """Test that the comments are removed and do not raise an exception"""
        svg = load_svg('<svg xmlns="http://www.w3.org/2000/svg"><!-- comment --></svg>')
        out1 = run_extension(svg)

        svg = load_svg('<svg xmlns="http://www.w3.org/2000/svg"></svg>')
        out2 = run_extension(svg)

        self.assertEqual(out1, out2)


class DXFDeeplyNestedTest(TestCase):
    """Check that a deeply nested SVG raises an AbortExtension"""

    @staticmethod
    def create_deep_svg(amount):
        """Create a very deep svg and test getting ancestors"""
        svg = '<svg xmlns="http://www.w3.org/2000/svg">'
        for i in range(amount):
            svg += f'<g id="{i}">'
        svg = load_svg(svg + ("</g>" * amount) + "</svg>")
        return svg

    def test_deeply_nested(self):
        "Run test"
        with self.assertRaisesRegex(AbortExtension, "Deep Ungroup"):
            run_extension(self.create_deep_svg(1500))


class TestDxfUnits(TestCase):
    """Test ensuring that units work properly"""

    def test_mm(self):
        """Test that the documents created with/without scaling and base units are
        identical."""
        document = SvgOutputMixin.get_template(width=210, height=297, unit="mm")
        document.getroot().namedview.set("inkscape:document-units", "mm")
        document.getroot().add(Rectangle.new(200, 0, 10, 16))
        out1 = run_extension(document)

        document = SvgOutputMixin.get_template(width=210, height=297, unit="mm")
        document.getroot().add(Rectangle.new(200, 0, 10, 16))
        out2 = run_extension(document, "--unit_from_document=False", "--units=mm")

        self.assertEqual(out1, out2)
        # Now with scaling - should result in the same document
        document = SvgOutputMixin.get_template(width=210, height=297, unit="mm")
        document.getroot().set("viewBox", "0 0 105 148.5")
        document.getroot().add(Rectangle.new(100, 0, 5, 8))
        out3 = run_extension(document, "--unit_from_document=False", "--units=mm")

        self.assertEqual(out1, out3)


class TestFlattenBez(TestCase):
    """Test that beziers are flattened"""

    def test_mm(self):
        """Test when FLATTENBEZ is enabled, splines are not present in the output"""
        document = SvgOutputMixin.get_template(width=210, height=297, unit="mm")
        document.getroot().namedview.set("inkscape:document-units", "mm")
        document.getroot().add(Circle.new(center=(105, 25), radius=15))
        out = run_extension(document, "-F=True")
        # If -F was False/not set, there will be a SPLINE in the output
        self.assertFalse("SPLINE" in str(out))
