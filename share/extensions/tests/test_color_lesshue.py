# coding=utf-8
from color_lesshue import LessHue
from .test_inkex_extensions import ColorBaseCase


class ColorLessHueTest(ColorBaseCase):
    effect_class = LessHue
    color_tests = [
        ("none", "none"),
        ("hsl(0, 0, 0)", "hsl(342, 0, 0)"),
        ("hsl(255, 255, 255)", "hsl(237, 100, 100)"),
        ((0, 0, 0), "#000000"),
        ((255, 255, 255), "#ffffff"),
        ((192, 192, 192), "#bfbfbf"),
        ((128, 128, 128), "#808080"),
        ((128, 0, 0), "#800026"),
        ((255, 0, 0), "#ff004d"),
        ((128, 128, 0), "#805900"),
        ((255, 255, 0), "#ffb200"),
        ((0, 128, 0), "#268000"),
        ((0, 255, 0), "#4cff00"),
        ((0, 128, 128), "#008059"),
        ((0, 255, 255), "#00ffb3"),
        ((0, 0, 128), "#002680"),
        ((0, 0, 255), "#004cff"),
        ((128, 0, 128), "#590080"),
        ((255, 0, 255), "#b300ff"),
    ]
