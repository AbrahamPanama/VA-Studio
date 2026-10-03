#!/usr/bin/env python3
"""Increase satuation"""

import inkex


class MoreSaturation(inkex.ColorExtension):
    """Increase saturation of selected objects"""

    target_space = "hsl"

    def modify_color(self, name, color):
        color.saturation += int(0.05 * 100.0)
        return color


if __name__ == "__main__":
    MoreSaturation().run()
