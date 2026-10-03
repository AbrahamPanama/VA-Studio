#!/usr/bin/env python3
"""Replace color extension"""

import inkex


class ReplaceColor(inkex.ColorExtension):
    """Replace color in SVG with another"""

    pass_rgba = True

    def add_arguments(self, pars):
        pars.add_argument("--tab")
        pars.add_argument(
            "-f",
            "--from_color",
            default=inkex.Color("#000000"),
            type=inkex.Color,
            help="Replace color",
        )
        pars.add_argument(
            "-t",
            "--to_color",
            default=inkex.Color("#ff0000"),
            type=inkex.Color,
            help="By color",
        )
        pars.add_argument(
            "-i",
            "--ignore_opacity",
            default=True,
            type=inkex.Boolean,
            help="Whether color should be replaced regardless of opacity match",
        )

    def modify_color(self, name, color):  # color is rgba
        from_color = self.options.from_color
        if from_color.alpha is None:
            from_color.alpha = 1.0
        if color.alpha is None:
            color.alpha = 1.0

        if from_color.to_rgb() == color.to_rgb() and (
            self.options.ignore_opacity or abs(from_color.alpha - color.alpha) < 0.01
        ):
            color = self.options.to_color
        if color.alpha == 1.0:
            color.alpha = None
        return color


if __name__ == "__main__":
    ReplaceColor().run()
