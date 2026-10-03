#!/usr/bin/env python3
"""Add more hue"""

import inkex


class MoreHue(inkex.ColorExtension):
    """Add hue to any selected object"""

    target_space = "hsl"

    def modify_color(self, name, color):
        color.hue += int(0.035 * 360.0)
        return color


if __name__ == "__main__":
    MoreHue().run()
