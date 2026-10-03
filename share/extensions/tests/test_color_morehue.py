# coding=utf-8

from color_morehue import MoreHue
from .test_inkex_extensions import ColorBaseCase


class ColorMoreHueTest(ColorBaseCase):
    effect_class = MoreHue
    color_tests = [
        ("none", "none"),
        ("hsl(0, 0, 0)", "hsl(12, 0, 0)"),
        ("hsl(360, 255, 255)", "hsl(12, 100, 100)"),
        ((0, 0, 0), "#000000"),
        ((255, 255, 255), "#ffffff"),
        ((192, 192, 192), "#bfbfbf"),
        ((128, 128, 128), "#808080"),
        ((128, 0, 0), "#801a00"),
        ((255, 0, 0), "#ff3300"),
        ((128, 128, 0), "#668000"),
        ((255, 255, 0), "#ccff00"),
        ((0, 128, 0), "#008019"),
        ((0, 255, 0), "#00ff33"),
        ((0, 128, 128), "#006680"),
        ((0, 255, 255), "#00ccff"),
        ((0, 0, 128), "#190080"),
        ((0, 0, 255), "#3300ff"),
        ((128, 0, 128), "#800066"),
        ((255, 0, 255), "#ff00cc"),
    ]
