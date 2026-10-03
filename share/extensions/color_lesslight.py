#!/usr/bin/env python3
"""Reduce lightness"""

import inkex


class LessLight(inkex.ColorExtension):
    """Reduce the light of the color"""

    target_space = "hsl"

    def modify_color(self, name, color):
        color.lightness -= int(0.05 * 100)
        return color


if __name__ == "__main__":
    LessLight().run()
