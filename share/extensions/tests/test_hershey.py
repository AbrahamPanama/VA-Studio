# coding=utf-8
# tests for the hershey-text extension (hershey.py and hershey.inx)
from lxml import etree

from inkex.tester import ComparisonMixin, InkscapeExtensionTestMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy, CompareOrderIndependentStyle

from hershey import Hershey


class HersheyComparisonMixin(ComparisonMixin):
    comparisons_cmpfile_dict = {}  # pairs of args and expected outputs
    effect_class = Hershey
    compare_filters = [CompareNumericFuzzy(), CompareOrderIndependentStyle()]


class TestHersheyBasic(InkscapeExtensionTestMixin, HersheyComparisonMixin, TestCase):
    compare_file = "svg/hershey_input.svg"  # a huge number of inputs
    comparisons_cmpfile_dict = {
        # default parameters:
        (): "hershey.out",
        # same as above, but explicit parameters. same output:
        (
            '--tab="render"',
            '--fontface="HersheySans1"',
            '--preserve="False"',
        ): "hershey.out",
    }


class TestHersheyTrivialInput(
    InkscapeExtensionTestMixin, HersheyComparisonMixin, TestCase
):
    compare_file = "svg/hershey_trivial_input.svg"
    comparisons_cmpfile_dict = {
        # loading a different font:
        ('--fontface="EMSAllure"',): "hershey_loadfont.out",
        # using the "other font" option. same output as above:
        ('--fontface="other"', '--otherfont="EMSAllure"'): "hershey_loadfont.out",
        # tests preserve text option
        ('--fontface="EMSOsmotron"', "--preserve=true"): "hershey_preservetext.out",
        # tests when just part of the input file is selected
        ("--id=A",): "hershey_partialselection.out",
    }


class TestHersheyTables(InkscapeExtensionTestMixin, HersheyComparisonMixin, TestCase):
    compare_file = "svg/default-inkscape-SVG.svg"
    comparisons_cmpfile_dict = {
        # generates a simple font table:
        (
            '--tab="utilities"',
            '--action="sample"',
            '--text="I am a quick brown fox"',
        ): "hershey_fonttable.out",
        # generates a simple font table, while testing UTF-8 input
        (
            '--tab="utilities"',
            '--action="sample"',
            '--text="Î âm å qù¡çk brõwñ fø×"',
        ): "hershey_encoding.out",
        # generates a glyph table in the font "EMSOsmotron"
        (
            '--tab="utilities"',
            '--action="table"',
            '--fontface="other"',
            '--otherfont="EMSOsmotron"',
        ): "hershey_glyphtable.out",
    }
