"""
Full run-through test for svg2xaml. Only covers basic cases.
"""
from io import BytesIO

from inkex.tester.mock import Capture
from .context import XamlOutput
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy


class XamlOutputRunthroughTest(ComparisonMixin, TestCase):
    """Test basic configuration of XAML export. The other parameters
    are checked mostly in test_svg2xaml_adv.py"""

    effect_class = XamlOutput
    compare_file = "svg/shapes.svg"
    compare_filters = [CompareNumericFuzzy()]
    comparisons = [
        ("--mode=canvas", "--target=wpf", "--indent=4"),
        ("--mode=canvas", "--target=wpf", "--text-to-path=True"),
        ("--mode=lowlevel", "--target=wpf"),
        (
            "--mode=canvas",
            "--target=avaloniaui",
        ),
        (
            "--mode=lowlevel",
            "--target=avaloniaui",
        ),
    ]


class TestLayerWarning(TestCase):
    """Test that a warning is created when we run Layers as Resources with
    toplevel elements that are not layers"""

    def test_warning(self):
        with Capture("stderr") as stderr:
            exporter = XamlOutput()
            out = BytesIO()
            exporter.run(
                [
                    "--target=wpf",
                    "--mode=lowlevel",
                    "--layers-as-resources=true",
                    self.data_file("svg", "circle_defaults.svg"),
                ],
                output=out,
            )

            self.assertIn("root element", stderr.getvalue())
