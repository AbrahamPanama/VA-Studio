# coding=utf-8
from color_removegreen import RemoveGreen
from .test_inkex_extensions import ColorBaseCase


class ColorRemoveGreenTest(ColorBaseCase):
    effect_class = RemoveGreen
    color_tests = [
        ("none", "none"),
        ((0, 0, 0), "#000000"),
        ((255, 255, 255), "#ff00ff"),
        ((192, 192, 192), "#c000c0"),
        ((128, 128, 128), "#800080"),
        ((128, 0, 0), "#800000"),
        ((255, 0, 0), "#ff0000"),
        ((128, 128, 0), "#800000"),
        ((255, 255, 0), "#ff0000"),
        ((0, 128, 0), "#000000"),
        ((0, 255, 0), "#000000"),
        ((0, 128, 128), "#000080"),
        ((0, 255, 255), "#0000ff"),
        ((0, 0, 128), "#000080"),
        ((0, 0, 255), "#0000ff"),
        ((128, 0, 128), "#800080"),
        ((255, 0, 255), "#ff00ff"),
    ]
