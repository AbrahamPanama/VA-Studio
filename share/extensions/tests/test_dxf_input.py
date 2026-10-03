# coding=utf-8

from io import BytesIO
import re
from dxf_input import DxfInput

from inkex.tester import ComparisonMixin, TestCase, xmldiff
from inkex.tester.filters import Compare, CompareNumericFuzzy
from inkex.tester.mock import Capture


class DxfInputFiltersMixin:
    def _apply_compare_filters(self, data, is_saving=None):
        """Remove the full pathnames"""
        if is_saving is True:
            return data
        data = super()._apply_compare_filters(data)
        return data.replace((self.datadir() + "/").encode("utf-8"), b"")


class TestDxfInputBasic(ComparisonMixin, TestCase, DxfInputFiltersMixin):
    compare_file = [
        "io/test_r12.dxf",
        "io/test_r14.dxf",
        # Unit test for https://gitlab.com/inkscape/extensions/-/issues/355
        "io/dxf_with_arc.dxf",
        # test polylines
        "io/dxf_polylines.dxf",
        # File missing a BLOCKS session
        "io/no_block_section.dxf",
        # test placement of graphical objects from BLOCKS section
        "io/dxf_multiple_inserts.dxf",
        # test correct colors generated
        # currently BYLAYER and BYBLOCK colors in inserted block are wrong
        "io/color.dxf",
        "io/test_input_rotated_ellipse_r14.dxf",
        "io/test_one_blankline_at_the_end.dxf",
        "io/Testdata - 2018 - Binary.dxf",
        # Test for https://gitlab.com/inkscape/extensions/-/issues/605
        # File insert_mirror.dxf is public domain
        "io/insert_mirror.dxf",
    ]
    compare_filters = [CompareNumericFuzzy()]
    comparisons = [()]
    effect_class = DxfInput


class TestDxfInputEnc(ComparisonMixin, TestCase, DxfInputFiltersMixin):
    compare_file = [
        "io/Testdata - 2018 - ASCII.dxf",
    ]
    compare_filters = [CompareNumericFuzzy()]
    comparisons = [("--encoding=utf8",)]
    effect_class = DxfInput


class TestDxfInputBasicError(ComparisonMixin, TestCase, DxfInputFiltersMixin):
    TestCase.stderr_protect = False
    # sample uses POLYLINE,TEXT (R12), LWPOLYLINE,MTEXT (R13, R14)
    # however has warnings when handling points with a display mode
    compare_file = [
        "io/test2_r12.dxf",
        "io/test2_r13.dxf",
        "io/test2_r14.dxf",
        "io/test_extrude.dxf",
    ]
    compare_filters = [CompareNumericFuzzy()]
    comparisons = [()]
    effect_class = DxfInput


class TestDxfInputTextHeight(ComparisonMixin, TestCase, DxfInputFiltersMixin):
    compare_file = ["io/CADTextHeight.dxf"]
    compare_filters = [CompareNumericFuzzy()]
    comparisons = [(), ("--textscale=1.411",)]
    effect_class = DxfInput


class DXFBinaryFilter(Compare):
    """
    Perform a few replacements where binary and ASCII DXF differ for some reason
    """

    @staticmethod
    def filter(contents):
        contents = contents.replace(b"^ ", b"^").replace(
            b"Testdata - 2018 - ASCII", b"Testdata - 2018 - Binary"
        )
        # THis particular text is different in Binary and ASCII
        i1 = contents.index(b'<text x="1843.68')
        contents = contents[0:i1] + contents[contents.index(b"</text>", i1) + 7 :]
        return contents


class DxfBinaryTestType(type):
    def __init__(cls, name, bases, attrs):
        super().__init__(name, bases, attrs)
        if name == "ComparisonMixin":
            return  # don't execute on the base class
        for rev in cls.revs:

            def test_method(self):
                binary_out = BytesIO()
                DxfInput().run(
                    [cls.data_file(f"io/Testdata - {rev} - Binary.dxf")],
                    output=binary_out,
                )
                binary_out.seek(0)
                data_a = bytes(binary_out.read())
                for cfilter in self.compare_filters:
                    data_a = cfilter(data_a)

                ascii_out = BytesIO()
                DxfInput().run(
                    [
                        cls.data_file(f"io/Testdata - {rev} - ASCII.dxf"),
                        "--encoding",
                        "utf8",
                    ],
                    output=ascii_out,
                )
                ascii_out.seek(0)
                data_b = bytes(ascii_out.read())
                for cfilter in self.compare_filters:
                    data_b = cfilter(data_b)

                diff_xml, delta = xmldiff(data_a.decode(), data_b.decode())
                diff = "SVG Differences\n\n"
                self.maxDiff = 1000
                for x, (value_a, value_b) in enumerate(delta):
                    try:
                        # Take advantage of better text diff in testcase's own asserts.

                        self.assertEqual(value_a, value_b)
                    except AssertionError as err:
                        diff += f" {x}. {str(err)}\n"
                self.assertTrue(delta, diff)

            setattr(cls, f"test_binary_{rev}", test_method)


class TestDxfBinary(TestCase, DxfInputFiltersMixin, metaclass=DxfBinaryTestType):
    """We just check that we can read the binary files, and that the output is
    identical to the corresponding ASCII file.
    """

    revs = [2000, 2004, 2007, 2010, 2013, 2018]
    compare_filters = [CompareNumericFuzzy(), DXFBinaryFilter()]

    def _test_comparison(self, args, compare_file, addout=None):
        return
