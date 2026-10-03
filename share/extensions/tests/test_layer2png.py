"""
Test export slices of an image.
"""

from inkex.tester import ComparisonMixin, TestCase
from layer2png import ExportSlices


class Layer2PNGTest(ComparisonMixin, TestCase):
    effect_class = ExportSlices
    compare_file = "svg/slicer.svg"
    comparisons = []

    def test_get_layers(self):
        basic_svg = self.data_file("svg", "slicer.svg")
        args = [basic_svg, "--layer=slices"]
        self.effect.options = self.effect.arg_parser.parse_args(args)
        self.effect.options.input_file = basic_svg
        self.effect.load_raw()
        nodes = self.effect.get_layer_nodes("slices")
        self.assertEqual(len(nodes), 1)
        self.assertEqual(nodes[0].tag, "{http://www.w3.org/2000/svg}rect")

    def test_bad_slice_layer(self):
        basic_svg = self.data_file("svg", "slicer.svg")
        args = [basic_svg, "--layer=slices"]
        self.effect.options = self.effect.arg_parser.parse_args(args)
        self.effect.options.input_file = basic_svg
        self.effect.load_raw()
        nodes = self.effect.get_layer_nodes("badslices")
        self.assertEqual(nodes, None)

    def test_color(self):
        basic_svg = self.data_file("svg", "slicer.svg")
        args = [basic_svg, "--layer=slices"]
        self.effect.options = self.effect.arg_parser.parse_args(args)
        self.effect.options.input_file = basic_svg
        self.effect.load_raw()
        nodes = self.effect.get_layer_nodes("slices")
        color, kwargs = self.effect.get_color_and_command_kwargs(nodes[0])
        self.assertEqual(color, self.effect.GREEN)
        self.assertEqual(kwargs["export-id"], "slice1")
