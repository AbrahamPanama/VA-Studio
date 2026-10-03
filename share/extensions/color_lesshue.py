#!/usr/bin/env python3
"""Reduce hue"""

import inkex


class LessHue(inkex.ColorExtension):
    """Remove Hue from the color"""

    target_space = "hsl"

    def modify_color(self, name, color):
        color.hue -= int(0.05 * 360)
        return color


if __name__ == "__main__":
    LessHue().run()
