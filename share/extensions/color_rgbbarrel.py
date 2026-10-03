#!/usr/bin/env python3
"""Rotate the colors in the selected elements"""

import inkex


class RgbBarrel(inkex.ColorExtension):
    """
    Cycle colors RGB -> BRG

    aka  Do a Barrel Roll!
    """

    target_space = "rgb"

    def modify_color(self, name, color):
        return inkex.ColorRGB((color.blue, color.red, color.green))


if __name__ == "__main__":
    RgbBarrel().run()
