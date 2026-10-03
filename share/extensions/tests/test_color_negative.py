# coding=utf-8
from color_negative import Negative
from .test_inkex_extensions import ColorBaseCase


class ColorNegativeTest(ColorBaseCase):
    effect_class = Negative
    color_tests = [
        ("none", "none"),
        ((0, 0, 0), "#ffffff"),
        ((255, 255, 255), "#000000"),
        ((192, 192, 192), "#3f3f3f"),
        ((128, 128, 128), "#7f7f7f"),
        ((128, 0, 0), "#7fffff"),
        ((255, 0, 0), "#00ffff"),
        ((128, 128, 0), "#7f7fff"),
        ((255, 255, 0), "#0000ff"),
        ((0, 128, 0), "#ff7fff"),
        ((0, 255, 0), "#ff00ff"),
        ((0, 128, 128), "#ff7f7f"),
        ((0, 255, 255), "#ff0000"),
        ((0, 0, 128), "#ffff7f"),
        ((0, 0, 255), "#ffff00"),
        ((128, 0, 128), "#7fff7f"),
        ((255, 0, 255), "#00ff00"),
    ]
