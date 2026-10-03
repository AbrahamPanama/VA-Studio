#!/usr/bin/env python3
"""Extension for removing the colour red from selected objects"""

import inkex


class RemoveRed(inkex.ColorExtension):
    """Remove red color from selected objects"""

    target_space = "rgb"

    def modify_color(self, name, color):
        color.red = 0
        return color


if __name__ == "__main__":
    RemoveRed().run()
