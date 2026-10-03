#!/usr/bin/env python3
"""Remove green color from selected objects"""

import inkex


class RemoveGreen(inkex.ColorExtension):
    """Remove green color from selected objects"""

    target_space = "rgb"

    def modify_color(self, name, color):
        color.green = 0
        return color


if __name__ == "__main__":
    RemoveGreen().run()
