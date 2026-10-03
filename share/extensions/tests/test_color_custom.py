# coding=utf-8

import inkex
from color_custom import Custom
from .test_inkex_extensions import ColorBaseCase


class ColorCustomTest(ColorBaseCase):
    effect_class = Custom
    color_tests = [
        # The default ranges are set to 0, and thus the color should not change.
        ("none", "none"),
        ((255, 255, 255), "#ffffff"),
        ((100, 0, 0), "#c80000", ["-r r*2"]),
        ((12, 34, 56), "#0c3822", ["-g b", "-b g"]),
        ((12, 34, 56), "#183822", ["-g b", "-b g", "-r r*2"]),
        ((0, 0, 0), "#100000", ["-s 255", "-r 16"]),
        ((0, 0, 0), "#0f0000", ["-s 1", "-r 0.0625"]),
        ((0, 0, 0), "#ff0000", ["-r 400"]),
        ((0, 0, 0), "#000000", ["-r -400"]),
        ("red", "#fe0000", ["-s 400"]),
    ]

    def test_evil_fails(self):
        """
        eval shouldn't allow for evil things to happen

        Here we try and check if a file exists but it could just as easily
        overwrite or delete the file

        """
        args = ["-r __import__('os').path.exists('__init__.py')", self.empty_svg]
        self.effect.run(args)

        with self.assertRaises(TypeError):
            self.effect.modify_color("fill", inkex.Color("black"))

    def test_invalid_operator(self):
        args = ["-r r % 100", self.empty_svg]
        self.effect.run(args)

        with self.assertRaises(KeyError):
            self.effect.modify_color("fill", inkex.Color("black"))

    def test_bad_syntax(self):
        args = ["-r r + 100)", self.empty_svg]
        self.effect.run(args)

        with self.assertRaises(SyntaxError):
            self.effect.modify_color("fill", inkex.Color("black"))
