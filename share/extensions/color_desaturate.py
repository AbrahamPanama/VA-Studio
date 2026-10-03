#!/usr/bin/env python3
"""Remove colors"""

import inkex


class Desaturate(inkex.ColorExtension):
    """Remove color but maintain intesity"""

    target_space = "rgb"

    def modify_color(self, name, color):
        lum = (
            max(color.red, color.green, color.blue)
            + min(color.red, color.green, color.blue)
        ) // 2
        return inkex.ColorRGB((int(round(lum)), int(round(lum)), int(round(lum))))


if __name__ == "__main__":
    Desaturate().run()
