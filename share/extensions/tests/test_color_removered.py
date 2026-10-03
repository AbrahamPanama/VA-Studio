# coding=utf-8
from color_removered import RemoveRed
from .test_inkex_extensions import ColorBaseCase


class ColorRemoveRedTest(ColorBaseCase):
    effect_class = RemoveRed
    color_tests = [
        ("none", "none"),
        ((0, 0, 0), "#000000"),
        ((255, 255, 255), "#00ffff"),
        ((192, 192, 192), "#00c0c0"),
        ((128, 128, 128), "#008080"),
        ((128, 0, 0), "#000000"),
        ((255, 0, 0), "#000000"),
        ((128, 128, 0), "#008000"),
        ((255, 255, 0), "#00ff00"),
        ((0, 128, 0), "#008000"),
        ((0, 255, 0), "#00ff00"),
        ((0, 128, 128), "#008080"),
        ((0, 255, 255), "#00ffff"),
        ((0, 0, 128), "#000080"),
        ((0, 0, 255), "#0000ff"),
        ((128, 0, 128), "#000080"),
        ((255, 0, 255), "#0000ff"),
    ]
