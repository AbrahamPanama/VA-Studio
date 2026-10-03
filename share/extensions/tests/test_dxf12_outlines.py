# coding=utf-8
from dxf12_outlines import DxfTwelve
from inkex import AbortExtension
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import WindowsTextCompat
from inkex.elements._parser import load_svg
from io import BytesIO


def run_extension(document, *args) -> str:
    output = BytesIO()
    ext = DxfTwelve()
    ext.parse_arguments([*args])
    ext.svg = document.getroot()
    ext.document = document
    ext.effect()
    ext.save(output)
    output.seek(0)
    return output.read().decode("utf-8")


class TestDXF12OutlinesBasic(ComparisonMixin, TestCase):
    compare_file = [
        "svg/shapes.svg",
        "svg/preserved-transforms.svg",
        "svg/dxf_nested_transforms.svg",
        "svg/clips_and_masks.svg",
        "svg/scale_not_unity.svg",
        "svg/dxf12_points.svg",
    ]
    comparisons = [()]
    effect_class = DxfTwelve
    compare_filters = [WindowsTextCompat()]


class TestDXF12TooManyColors(TestCase):
    """
    Check that we can serialize an SVG with the maximum # of colors (249), but that 250 raises an
    AbortExtension
    """

    @staticmethod
    def create_many_colors(amount):
        """Create an svg with many colors to test serialization"""
        svg = '<svg xmlns="http://www.w3.org/2000/svg">'
        for i in range(amount):
            color = format(i % 0x1000000, "06x")
            svg += f'<rect x="0" y="0" width="2" height="2" style="stroke:#{color}" id="i" />'
        svg = load_svg(svg + "</svg>")
        return svg

    def test_maximum_colors(self):
        """Test that we can serialize with 249 colors"""
        run_extension(self.create_many_colors(249))

    def test_too_many_colors(self):
        """Test that we can't serialize with 250 colors"""
        with self.assertRaisesRegex(AbortExtension, "maximum of 249 distinct colors"):
            run_extension(self.create_many_colors(250))
